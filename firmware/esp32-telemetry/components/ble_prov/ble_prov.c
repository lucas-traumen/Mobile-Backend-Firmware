/*
 * ble_prov — implementation (M16a).
 *
 * GATT contract (mobile app side is the source of truth, byte-exact):
 *
 *   | Item          | UUID (textual)                       | Flags                     |
 *   |---------------|--------------------------------------|---------------------------|
 *   | Service       | e5f4a3b2-1c0d-4e2f-9a8b-7c6d5e4f3a01 | —                         |
 *   | WiFi SSID     | ...3a02 | WRITE, encrypted link required |
 *   | WiFi Password | ...3a03 | WRITE, encrypted link required |
 *   | Command       | ...3a04 | WRITE — ASCII "PROVISION"     |
 *   | Device Info   | ...3a05 | READ — JSON, 3 fixed fields   |
 *   | Status        | ...3a06 | NOTIFY — ASCII                |
 *   | Broker URI    | ...3a07 | WRITE                         |
 *   | MQTT User     | ...3a08 | WRITE                         |
 *   | MQTT Pass     | ...3a09 | WRITE, encrypted link required |
 *
 * Status strings: IDLE / CONNECTING / CONNECTED / FAILED:BAD_AUTH
 *                 / FAILED:NO_SSID / FAILED:TIMEOUT / FAILED:ERROR.
 *
 * Long writes: values larger than the negotiated ATT MTU arrive as an ATT
 * prepared-write sequence. NimBLE's ATT server accumulates the fragments per
 * handle (validating that offsets are contiguous and start at 0) and, on
 * Execute Write, hands the FULL reassembled value to the access callback in
 * one call (ble_att_svr_prep_extract -> ble_att_svr_write in NimBLE sources).
 * The write handlers below therefore only validate the total length of the
 * value they receive — no manual offset bookkeeping is needed.
 *
 * Threading: the GATT access + GAP callbacks run in the NimBLE host task;
 * ble_prov_report_result/stop run in caller tasks. s_lock (FreeRTOS mutex)
 * guards the shared state; it is NEVER held across nimble_port_* calls (the
 * host task has to be able to exit) and always taken BEFORE the host's own
 * locks (the host never calls back into us holding them — no lock cycle).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"

/* NimBLE (init sequence mirrors examples/bluetooth/nimble/bleprph, IDF v6). */
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_sm.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "ble_prov.h"

static const char *TAG = "ble_prov";

/*
 * 128-bit UUIDs of the provisioning contract.
 *
 * BLE_UUID128_INIT takes the raw value bytes least-significant first, i.e.
 * the REVERSE of the textual UUID. The textual form
 * "e5f4a3b2-1c0d-4e2f-9a8b-7c6d5e4f3aXX" therefore expands to
 * { 0xXX, 0x3a, 0x4f, 0x5e, 0x6d, 0x7c, 0x8b, 0x9a,
 *   0x2f, 0x4e, 0x0d, 0x1c, 0xb2, 0xa3, 0xf4, 0xe5 }.
 * The last 15 bytes are shared by the whole contract; only the first byte
 * (XX = 01..09) differs per item.
 */
#define UUID_CONTRACT_TAIL                                     \
    0x3a, 0x4f, 0x5e, 0x6d, 0x7c, 0x8b, 0x9a,                  \
    0x2f, 0x4e, 0x0d, 0x1c, 0xb2, 0xa3, 0xf4, 0xe5

static const ble_uuid128_t s_uuid_svc        = BLE_UUID128_INIT(0x01, UUID_CONTRACT_TAIL);
static const ble_uuid128_t s_uuid_ssid       = BLE_UUID128_INIT(0x02, UUID_CONTRACT_TAIL);
static const ble_uuid128_t s_uuid_wifi_pass  = BLE_UUID128_INIT(0x03, UUID_CONTRACT_TAIL);
static const ble_uuid128_t s_uuid_cmd        = BLE_UUID128_INIT(0x04, UUID_CONTRACT_TAIL);
static const ble_uuid128_t s_uuid_info       = BLE_UUID128_INIT(0x05, UUID_CONTRACT_TAIL);
static const ble_uuid128_t s_uuid_status     = BLE_UUID128_INIT(0x06, UUID_CONTRACT_TAIL);
static const ble_uuid128_t s_uuid_broker_uri = BLE_UUID128_INIT(0x07, UUID_CONTRACT_TAIL);
static const ble_uuid128_t s_uuid_mqtt_user  = BLE_UUID128_INIT(0x08, UUID_CONTRACT_TAIL);
static const ble_uuid128_t s_uuid_mqtt_pass  = BLE_UUID128_INIT(0x09, UUID_CONTRACT_TAIL);

/* Status payloads of the GATT contract (ASCII). */
static const char STATUS_IDLE[]            = "IDLE";
static const char STATUS_CONNECTING[]      = "CONNECTING";
static const char STATUS_CONNECTED[]       = "CONNECTED";
static const char STATUS_FAILED_NO_SSID[]  = "FAILED:NO_SSID";
static const char STATUS_FAILED_BAD_AUTH[] = "FAILED:BAD_AUTH";
static const char STATUS_FAILED_TIMEOUT[]  = "FAILED:TIMEOUT";
static const char STATUS_FAILED_ERROR[]    = "FAILED:ERROR";

/* The single command value accepted by the Command characteristic. */
static const char CMD_PROVISION[] = "PROVISION";

/* Advertised local name: "IoTBoard-" + boardId. 29 bytes is the largest
 * value that still fits a complete AD field into the 31-byte scan response. */
#define ADV_NAME_PREFIX "IoTBoard-"
#define ADV_NAME_MAX    29

/* Pending-config fields, in one place for the GATT writer + NVS round-trip. */
typedef enum {
    PENDING_SSID = 0,
    PENDING_WIFI_PASS,
    PENDING_BROKER_URI,
    PENDING_MQTT_USER,
    PENDING_MQTT_PASS,
    PENDING_COUNT,
} pending_idx_t;

#define FIELD_CAP(field)   sizeof(((ble_prov_wifi_cfg_t *)0)->field)
#define FIELD_OFFSET(field) offsetof(ble_prov_wifi_cfg_t, field)

static const size_t s_field_offset[PENDING_COUNT] = {
    [PENDING_SSID]       = FIELD_OFFSET(ssid),
    [PENDING_WIFI_PASS]  = FIELD_OFFSET(wifi_pass),
    [PENDING_BROKER_URI] = FIELD_OFFSET(broker_uri),
    [PENDING_MQTT_USER]  = FIELD_OFFSET(mqtt_user),
    [PENDING_MQTT_PASS]  = FIELD_OFFSET(mqtt_pass),
};
static const size_t s_field_cap[PENDING_COUNT] = {
    [PENDING_SSID]       = FIELD_CAP(ssid),
    [PENDING_WIFI_PASS]  = FIELD_CAP(wifi_pass),
    [PENDING_BROKER_URI] = FIELD_CAP(broker_uri),
    [PENDING_MQTT_USER]  = FIELD_CAP(mqtt_user),
    [PENDING_MQTT_PASS]  = FIELD_CAP(mqtt_pass),
};
static const char *const s_field_key[PENDING_COUNT] = {
    [PENDING_SSID]       = "ssid",
    [PENDING_WIFI_PASS]  = "wifi_pass",
    [PENDING_BROKER_URI] = "broker_uri",
    [PENDING_MQTT_USER]  = "mqtt_user",
    [PENDING_MQTT_PASS]  = "mqtt_pass",
};

/* Fixed board identity, copied from ble_prov_cfg_t at start. */
static char s_board_id[64];
static char s_board_type[64];
static char s_adv_name[ADV_NAME_MAX + 1];
static ble_prov_provision_cb_t s_on_provision;
static void *s_user_ctx;

/* Config received over GATT, committed by the PROVISION command. */
static ble_prov_wifi_cfg_t s_pending;
static bool s_pending_set[PENDING_COUNT];

/* Provision progress — rejects duplicate PROVISION commands. */
typedef enum {
    PROV_STATE_IDLE = 0,    /**< advertising, waiting for config         */
    PROV_STATE_CONNECTING,  /**< PROVISION accepted, waiting for result  */
    PROV_STATE_CONNECTED,   /**< result OK, stop timer armed             */
} prov_state_t;

static prov_state_t s_state;
static bool s_running;
static bool s_status_subscribed;
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_status_val_handle;
static uint8_t s_own_addr_type;

/* Guards s_pending / s_state / s_conn_handle between the NimBLE host task
 * and the upper layer's task (report_result, stop). */
static SemaphoreHandle_t s_lock;

/* One-shot timer: auto-stop 30 s after CONNECTED. Created once, reused. */
static esp_timer_handle_t s_stop_timer;

/*
 * NimBLE bond/CCC storage backend. Declared here (prototype only) exactly as
 * the IDF examples do — the header lives inside the stack's private store
 * config and is not on the public include path.
 */
void ble_store_config_init(void);

static int ble_prov_gap_event(struct ble_gap_event *event, void *arg);

/*
 * GATT service table. The access callback receives the chr id via .arg, so
 * the dispatcher works by id instead of UUID comparisons. Every write
 * characteristic is write-with-response (no WRITE_NO_RSP flag) so that long
 * values can use the ATT prepared-write protocol.
 */
typedef enum {
    CHR_SSID = 0,
    CHR_WIFI_PASS,
    CHR_CMD,
    CHR_INFO,
    CHR_STATUS,
    CHR_BROKER_URI,
    CHR_MQTT_USER,
    CHR_MQTT_PASS,
} chr_id_t;

static int gatt_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                          struct ble_gatt_access_ctxt *ctxt, void *arg);

static const struct ble_gatt_svc_def s_gatt_svcs[] = {
    {
        /*** Provisioning service (primary) ***/
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_uuid_svc.u,
        .characteristics = (struct ble_gatt_chr_def[])
        { {
              /* WiFi SSID — WRITE, encrypted link required. */
              .uuid = &s_uuid_ssid.u,
              .access_cb = gatt_access_cb,
              .arg = (void *)(intptr_t)CHR_SSID,
              .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_ENC,
          }, {
              /* WiFi Password — WRITE, encrypted link required. */
              .uuid = &s_uuid_wifi_pass.u,
              .access_cb = gatt_access_cb,
              .arg = (void *)(intptr_t)CHR_WIFI_PASS,
              .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_ENC,
          }, {
              /* Command — ASCII "PROVISION". */
              .uuid = &s_uuid_cmd.u,
              .access_cb = gatt_access_cb,
              .arg = (void *)(intptr_t)CHR_CMD,
              .flags = BLE_GATT_CHR_F_WRITE,
          }, {
              /* Device Info — fixed-shape JSON. */
              .uuid = &s_uuid_info.u,
              .access_cb = gatt_access_cb,
              .arg = (void *)(intptr_t)CHR_INFO,
              .flags = BLE_GATT_CHR_F_READ,
          }, {
              /* Status — NOTIFY; value handle kept for ble_gatts_notify_custom. */
              .uuid = &s_uuid_status.u,
              .access_cb = gatt_access_cb,
              .arg = (void *)(intptr_t)CHR_STATUS,
              .flags = BLE_GATT_CHR_F_NOTIFY,
              .val_handle = &s_status_val_handle,
          }, {
              /* Broker URI — WRITE. */
              .uuid = &s_uuid_broker_uri.u,
              .access_cb = gatt_access_cb,
              .arg = (void *)(intptr_t)CHR_BROKER_URI,
              .flags = BLE_GATT_CHR_F_WRITE,
          }, {
              /* MQTT User — WRITE. */
              .uuid = &s_uuid_mqtt_user.u,
              .access_cb = gatt_access_cb,
              .arg = (void *)(intptr_t)CHR_MQTT_USER,
              .flags = BLE_GATT_CHR_F_WRITE,
          }, {
              /* MQTT Pass — WRITE, encrypted link required. */
              .uuid = &s_uuid_mqtt_pass.u,
              .access_cb = gatt_access_cb,
              .arg = (void *)(intptr_t)CHR_MQTT_PASS,
              .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_ENC,
          }, {
              0, /* No more characteristics in this service. */
          } },
    },

    {
        0, /* No more services. */
    },
};

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

/*
 * Copy `src` into `dst` escaping the characters that would break the fixed
 * JSON shape of Device Info: '"' and '\' get backslash-escaped, raw control
 * bytes (illegal inside JSON strings) become '?'. Bytes >= 0x80 pass through
 * unchanged (the strings come from Kconfig; UTF-8 stays UTF-8). `dst_cap`
 * must be >= 2 * strlen(src) + 1 (worst case every char escapes to 2 bytes).
 */
static void json_escape_copy(char *dst, size_t dst_cap, const char *src)
{
    size_t o = 0;
    for (size_t i = 0; src[i] != '\0' && o + 2 < dst_cap; i++) {
        const unsigned char c = (unsigned char)src[i];
        if (c == '"' || c == '\\' || c < 0x20) {
            dst[o++] = '\\';
            dst[o++] = (c == '"') ? '"' : (c == '\\') ? '\\' : '?';
        } else {
            dst[o++] = (char)c;
        }
    }
    dst[o] = '\0';
}

/*
 * Build the Device Info payload: {"schemaVersion":1,"boardId":"..",
 * "boardType":".."} — exactly the 3 fields of the contract, built with
 * snprintf (no cJSON dependency in this component). Returns false when the
 * result would not fit (mis-sized Kconfig strings; cannot happen for the
 * 64-byte statics + the buffers below).
 */
static bool build_device_info_json(char *buf, size_t buf_size)
{
    char esc_id[2 * sizeof(s_board_id) + 1];
    char esc_type[2 * sizeof(s_board_type) + 1];

    json_escape_copy(esc_id, sizeof(esc_id), s_board_id);
    json_escape_copy(esc_type, sizeof(esc_type), s_board_type);

    const int n = snprintf(buf, buf_size,
                           "{\"schemaVersion\":1,\"boardId\":\"%s\","
                           "\"boardType\":\"%s\"}",
                           esc_id, esc_type);
    return n > 0 && (size_t)n < buf_size;
}

/*
 * Notify `status` (ASCII) on the Status characteristic.
 * ble_gatts_notify_custom consumes the mbuf regardless of outcome.
 * Without a subscribed central the notification is dropped — the app
 * re-learns the current state on its next CCCD subscription (we send it
 * there). Callers hold s_lock.
 */
static void notify_status_locked(const char *status)
{
    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE || !s_status_subscribed) {
        ESP_LOGI(TAG, "status %s: no subscribed central — dropped", status);
        return;
    }
    struct os_mbuf *om = ble_hs_mbuf_from_flat(status, strlen(status));
    if (om == NULL) {
        ESP_LOGE(TAG, "status %s: mbuf alloc failed", status);
        return;
    }
    const int rc = ble_gatts_notify_custom(s_conn_handle, s_status_val_handle, om);
    if (rc != 0) {
        ESP_LOGW(TAG, "status %s: notify failed rc=%d", status, rc);
    } else {
        ESP_LOGI(TAG, "status notified: %s", status);
    }
}

/* ------------------------------------------------------------------ */
/* Writes: length validation + pending buffer                          */
/* ------------------------------------------------------------------ */

/*
 * Validate and store one written value. NimBLE calls the access callback
 * once per write with the COMPLETE value (long writes are reassembled by
 * the ATT server before this runs — see file header).
 *
 * A length outside [min_len, max_len] (or an embedded NUL) is rejected with
 * the ATT "Invalid Attribute Value Length" error so the app sees the problem
 * immediately and nothing is stored — "log WARN + drop the value" from the
 * contract. A plain ACK-and-ignore would leave stale pending data behind and
 * only surface at PROVISION time.
 */
static int chr_write_field(pending_idx_t idx, size_t min_len, size_t max_len,
                           const char *name, struct os_mbuf *om)
{
    const uint16_t value_len = OS_MBUF_PKTLEN(om);
    if (value_len < min_len || value_len > max_len) {
        ESP_LOGW(TAG, "write %s: invalid length %u (allowed %u..%u) — rejected",
                 name, (unsigned)value_len, (unsigned)min_len, (unsigned)max_len);
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    /* Flatten to a staging buffer first: the pending buffer itself is only
     * touched under s_lock (report_result snapshots it from another task).
     * The broker URI is the largest field — stage up to its 128 bytes. */
    char staging[BLE_PROV_BROKER_URI_MAX_LEN + 1];
    uint16_t copied = 0;
    const int rc = ble_hs_mbuf_to_flat(om, staging,
                                       (uint16_t)(s_field_cap[idx] - 1), &copied);
    if (rc != 0) {
        ESP_LOGE(TAG, "write %s: mbuf flatten failed rc=%d", name, rc);
        return BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    if (memchr(staging, '\0', copied) != NULL) {
        ESP_LOGW(TAG, "write %s: embedded NUL byte — rejected", name);
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    staging[copied] = '\0';

    xSemaphoreTake(s_lock, portMAX_DELAY);
    memcpy((char *)&s_pending + s_field_offset[idx], staging, (size_t)copied + 1);
    s_pending_set[idx] = true;
    xSemaphoreGive(s_lock);

    ESP_LOGD(TAG, "write %s: %u bytes accepted", name, (unsigned)copied);
    return 0;
}

/*
 * Command characteristic: accept exactly "PROVISION", warn-and-ignore
 * anything else (ACKed — the contract defines no error payload for it).
 */
static int chr_write_command(struct os_mbuf *om)
{
    const size_t cmd_len = strlen(CMD_PROVISION);
    const uint16_t value_len = OS_MBUF_PKTLEN(om);
    char buf[sizeof(CMD_PROVISION)];  /* 9 chars + NUL, fixed — no VLA */

    if (value_len != cmd_len ||
        ble_hs_mbuf_to_flat(om, buf, (uint16_t)(sizeof(buf) - 1), NULL) != 0 ||
        memcmp(buf, CMD_PROVISION, cmd_len) != 0) {
        ESP_LOGW(TAG, "command: unknown payload (len=%u) — ignored",
                 (unsigned)value_len);
        return 0;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    const prov_state_t state = s_state;
    const bool have_ssid = s_pending_set[PENDING_SSID];
    xSemaphoreGive(s_lock);

    if (state != PROV_STATE_IDLE) {
        ESP_LOGW(TAG, "command PROVISION while state=%d — ignored", (int)state);
        return 0;
    }
    if (!have_ssid) {
        ESP_LOGW(TAG, "command PROVISION without SSID — FAILED:NO_SSID");
        xSemaphoreTake(s_lock, portMAX_DELAY);
        notify_status_locked(STATUS_FAILED_NO_SSID);
        xSemaphoreGive(s_lock);
        return 0;
    }

    /* Snapshot under the lock, run the callback outside it: the upper layer
     * may call back into this component (report_result) from its own task. */
    ble_prov_wifi_cfg_t snapshot;
    ble_prov_provision_cb_t cb;
    void *user_ctx;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    snapshot = s_pending;
    cb = s_on_provision;
    user_ctx = s_user_ctx;
    s_state = PROV_STATE_CONNECTING;
    xSemaphoreGive(s_lock);

    ESP_LOGI(TAG, "PROVISION accepted — handing config to the upper layer");
    xSemaphoreTake(s_lock, portMAX_DELAY);
    notify_status_locked(STATUS_CONNECTING);
    xSemaphoreGive(s_lock);

    if (cb != NULL) {
        cb(&snapshot, user_ctx);
    } else {
        ESP_LOGW(TAG, "no on_provision callback registered — config dropped");
    }
    return 0;
}

static int chr_read_device_info(struct ble_gatt_access_ctxt *ctxt)
{
    /* Worst case: 47 bytes of shape + 2 escaped 64-byte fields = ~303 bytes. */
    char json[512];
    if (!build_device_info_json(json, sizeof(json))) {
        ESP_LOGE(TAG, "device info JSON does not fit its buffer");
        return BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    const int rc = os_mbuf_append(ctxt->om, json, strlen(json));
    return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int gatt_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                          struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    const chr_id_t id = (chr_id_t)(intptr_t)arg;

    switch (ctxt->op) {
    case BLE_GATT_ACCESS_OP_READ_CHR:
        if (id == CHR_INFO) {
            return chr_read_device_info(ctxt);
        }
        break;

    case BLE_GATT_ACCESS_OP_WRITE_CHR:
        switch (id) {
        case CHR_SSID:
            return chr_write_field(PENDING_SSID, 1, BLE_PROV_SSID_MAX_LEN,
                                   "ssid", ctxt->om);
        case CHR_WIFI_PASS:
            return chr_write_field(PENDING_WIFI_PASS, 0,
                                   BLE_PROV_WIFI_PASS_MAX_LEN,
                                   "wifi_pass", ctxt->om);
        case CHR_CMD:
            return chr_write_command(ctxt->om);
        case CHR_BROKER_URI:
            return chr_write_field(PENDING_BROKER_URI,
                                   BLE_PROV_BROKER_URI_MIN_LEN,
                                   BLE_PROV_BROKER_URI_MAX_LEN,
                                   "broker_uri", ctxt->om);
        case CHR_MQTT_USER:
            return chr_write_field(PENDING_MQTT_USER, 1,
                                   BLE_PROV_MQTT_USER_MAX_LEN,
                                   "mqtt_user", ctxt->om);
        case CHR_MQTT_PASS:
            return chr_write_field(PENDING_MQTT_PASS, 0,
                                   BLE_PROV_MQTT_PASS_MAX_LEN,
                                   "mqtt_pass", ctxt->om);
        default:
            break;
        }
        break;

    default:
        break;
    }

    ESP_LOGW(TAG, "unexpected GATT access: op=%d chr=%d", (int)ctxt->op, (int)id);
    return BLE_ATT_ERR_UNLIKELY;
}

/* ------------------------------------------------------------------ */
/* GAP: advertising + connection events                                */
/* ------------------------------------------------------------------ */

/*
 * Advertisement payload (31-byte budget): Flags (3) + complete 128-bit
 * service UUID (18) = 21 bytes. The local name does not fit alongside it,
 * so it goes into the scan response (up to 29 bytes there).
 */
static void ble_prov_advertise(void)
{
    if (!s_running) {
        return;
    }

    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof(fields));
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids128 = &s_uuid_svc;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "error setting advertisement data; rc=%d", rc);
        return;
    }

    struct ble_hs_adv_fields rsp;
    memset(&rsp, 0, sizeof(rsp));
    rsp.name = (const uint8_t *)s_adv_name;
    rsp.name_len = strlen(s_adv_name);
    rsp.name_is_complete = 1;

    rc = ble_gap_adv_rsp_set_fields(&rsp);
    if (rc != 0) {
        ESP_LOGE(TAG, "error setting scan response; rc=%d", rc);
        return;
    }

    struct ble_gap_adv_params adv_params;
    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER,
                           &adv_params, ble_prov_gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "error enabling advertisement; rc=%d", rc);
        return;
    }
    ESP_LOGI(TAG, "advertising \"%s\" (service "
                  "e5f4a3b2-1c0d-4e2f-9a8b-7c6d5e4f3a01)", s_adv_name);
}

static int
ble_prov_gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    struct ble_gap_conn_desc desc;
    int rc;

    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_conn_handle = event->connect.conn_handle;
            xSemaphoreGive(s_lock);
            ESP_LOGI(TAG, "central connected (handle=%d)",
                     event->connect.conn_handle);
        } else {
            /* Connection attempt failed; resume advertising. */
            ESP_LOGW(TAG, "connection failed; status=%d — resuming advertising",
                     event->connect.status);
            ble_prov_advertise();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "disconnect; reason=%d — resuming advertising",
                 event->disconnect.reason);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        s_status_subscribed = false;
        xSemaphoreGive(s_lock);
        ble_prov_advertise();
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == s_status_val_handle) {
            const bool notify = event->subscribe.cur_notify &&
                                !event->subscribe.cur_indicate;
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_status_subscribed = notify;
            const prov_state_t state = s_state;
            xSemaphoreGive(s_lock);
            ESP_LOGI(TAG, "status CCCD: notify=%d", notify);
            if (notify) {
                /* Contract: send IDLE when the central subscribes before a
                 * PROVISION. CONNECTING is sent instead if an attempt is
                 * already running, so a re-subscribing app stays in sync. */
                xSemaphoreTake(s_lock, portMAX_DELAY);
                notify_status_locked(state == PROV_STATE_IDLE
                                     ? STATUS_IDLE : STATUS_CONNECTING);
                xSemaphoreGive(s_lock);
            }
        }
        return 0;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "mtu update: conn=%d mtu=%d",
                 event->mtu.conn_handle, event->mtu.value);
        return 0;

    case BLE_GAP_EVENT_ENC_CHANGE:
        rc = ble_gap_conn_find(event->enc_change.conn_handle, &desc);
        ESP_LOGI(TAG, "encryption change: status=%d encrypted=%d bonded=%d",
                 event->enc_change.status,
                 rc == 0 ? (int)desc.sec_state.encrypted : -1,
                 rc == 0 ? (int)desc.sec_state.bonded : -1);
        return 0;

    case BLE_GAP_EVENT_REPEAT_PAIRING:
        /* Peer re-pairs although we hold a bond (e.g. app reinstalled):
         * drop the old bond and let the host retry, as in the IDF example. */
        rc = ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc);
        if (rc == 0) {
            ble_store_util_delete_peer(&desc.peer_id_addr);
        }
        return BLE_GAP_REPEAT_PAIRING_RETRY;

    default:
        return 0;
    }
}

/* NimBLE host callbacks. */

static void
ble_prov_on_reset(int reason)
{
    ESP_LOGE(TAG, "host reset; reason=%d", reason);
}

static void
ble_prov_on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "no address available; rc=%d", rc);
        return;
    }
    rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "error determining address type; rc=%d", rc);
        return;
    }
    ble_prov_advertise();
}

static void
ble_prov_host_task(void *param)
{
    (void)param;
    ESP_LOGI(TAG, "NimBLE host task started");
    /* Returns only when nimble_port_stop() is executed. */
    nimble_port_run();
    nimble_port_freertos_deinit();
}

/* ------------------------------------------------------------------ */
/* NVS persistence (namespace "bleprov")                               */
/* ------------------------------------------------------------------ */

/* Write all five keys from `cfg`, then a single commit. Values are stored
 * as-is; the empty string is a legitimate value (open network / no auth). */
static esp_err_t persist_pending(const ble_prov_wifi_cfg_t *cfg)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(BLE_PROV_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open(%s) failed: %s", BLE_PROV_NVS_NAMESPACE,
                 esp_err_to_name(err));
        return err;
    }

    for (size_t i = 0; err == ESP_OK && i < PENDING_COUNT; i++) {
        const char *value = (const char *)cfg + s_field_offset[i];
        err = nvs_set_str(nvs, s_field_key[i], value);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "nvs_set_str(%s) failed: %s", s_field_key[i],
                     esp_err_to_name(err));
        }
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "nvs_commit failed: %s", esp_err_to_name(err));
        } else {
            ESP_LOGI(TAG, "provisioned credentials stored to NVS (%s)",
                     BLE_PROV_NVS_NAMESPACE);
        }
    }
    nvs_close(nvs);
    return err;
}

/* ------------------------------------------------------------------ */
/* Stop timer                                                          */
/* ------------------------------------------------------------------ */

static void stop_timer_cb(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "%d s elapsed after CONNECTED — stopping BLE provisioning",
             BLE_PROV_STOP_DELAY_S);
    ble_prov_stop();
}

/* Create the auto-stop timer once (reused across start/stop cycles; never
 * deleted from inside its own callback). */
static esp_err_t ensure_stop_timer(void)
{
    if (s_stop_timer != NULL) {
        return ESP_OK;
    }
    const esp_timer_create_args_t args = {
        .callback = stop_timer_cb,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "bleprov_stop",
        .skip_unhandled_events = false,
    };
    return esp_timer_create(&args, &s_stop_timer);
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

/*
 * Init chain (M16d): every step is numbered and logged, so a hardware boot
 * that dies inside this function shows exactly which step failed. Any
 * failure AFTER a stack resource was taken rolls back in reverse order
 * (goto cleanup) so a retry attempt never inherits half-initialized state:
 *
 *   1. one-shot auto-stop timer        (failure: nothing to roll back)
 *   2. nimble_port_init                (failure: cleans up internally —
 *                                       controller disable+deinit run
 *                                       inside IDF on every internal
 *                                       error path, so callers must NOT
 *                                       call nimble_port_deinit here)
 *   3. host callbacks + SMP policy     (cannot fail)
 *   4. services + device name          (failure: goto cleanup -> step 2 undo)
 *   5. bond store + host task          (cannot fail; success point)
 */
esp_err_t ble_prov_start(const ble_prov_cfg_t *cfg)
{
    esp_err_t err;
    int rc;
    bool nimble_inited = false;

    if (cfg == NULL || cfg->board_id == NULL || cfg->board_id[0] == '\0' ||
        cfg->board_type == NULL || cfg->board_type[0] == '\0') {
        ESP_LOGE(TAG, "invalid ble_prov_cfg: board_id/board_type are mandatory");
        return ESP_ERR_INVALID_ARG;
    }

    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) {
            ESP_LOGE(TAG, "mutex creation failed");
            return ESP_ERR_NO_MEM;
        }
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_running) {
        xSemaphoreGive(s_lock);
        ESP_LOGW(TAG, "ble_prov already running");
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreGive(s_lock);

    /* Copy identity + callback before touching the stack. */
    const size_t id_len_cfg = strlen(cfg->board_id);
    if (id_len_cfg >= sizeof(s_board_id)) {
        ESP_LOGE(TAG, "board_id too long (%u >= %u)",
                 (unsigned)id_len_cfg, (unsigned)sizeof(s_board_id));
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(s_board_id, cfg->board_id, id_len_cfg + 1);
    const size_t type_len = strlen(cfg->board_type);
    if (type_len >= sizeof(s_board_type)) {
        ESP_LOGE(TAG, "board_type too long (%u >= %u)",
                 (unsigned)type_len, (unsigned)sizeof(s_board_type));
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(s_board_type, cfg->board_type, type_len + 1);
    s_on_provision = cfg->on_provision;
    s_user_ctx = cfg->user_ctx;

    /* "IoTBoard-" + boardId, truncated to what a complete AD field allows. */
    size_t id_len = id_len_cfg;
    if (strlen(ADV_NAME_PREFIX) + id_len > ADV_NAME_MAX) {
        id_len = ADV_NAME_MAX - strlen(ADV_NAME_PREFIX);
        ESP_LOGW(TAG, "advertised name truncated to \"%s%.*s\" (%d-byte AD "
                      "limit)", ADV_NAME_PREFIX, (int)id_len, s_board_id,
                 ADV_NAME_MAX);
    }
    memcpy(s_adv_name, ADV_NAME_PREFIX, strlen(ADV_NAME_PREFIX));
    memcpy(s_adv_name + strlen(ADV_NAME_PREFIX), s_board_id, id_len);
    s_adv_name[strlen(ADV_NAME_PREFIX) + id_len] = '\0';

    memset(&s_pending, 0, sizeof(s_pending));
    memset(s_pending_set, 0, sizeof(s_pending_set));
    s_state = PROV_STATE_IDLE;

    /* --- step 1/5: one-shot auto-stop timer (reused across attempts) --- */
    err = ensure_stop_timer();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "step 1/5 (esp_timer_create) failed: %s (0x%x)",
                 esp_err_to_name(err), (unsigned)err);
        return err;
    }
    ESP_LOGI(TAG, "step 1/5 ok: auto-stop timer ready");

    /* --- step 2/5: controller + host (nimble_port_init) ---
     * Init sequence mirrors examples/bluetooth/nimble/bleprph (IDF v6):
     * nimble_port_init brings up controller + host; host callbacks and SMP
     * are configured; services are registered; then the host task starts.
     * Advertising begins from the sync callback once the controller is up. */
    err = nimble_port_init();
    if (err != ESP_OK) {
        /* nimble_port_init() disables + deinits the controller itself on
         * every internal failure path — do NOT call nimble_port_deinit()
         * here: it would run against a never-inited host. Retry attempts
         * start from a clean controller state. */
        ESP_LOGE(TAG, "step 2/5 (nimble_port_init) failed: %s (0x%x) — see "
                      "the BLE_INIT log lines above this one for the exact "
                      "sub-step (controller init / enable / host init)",
                 esp_err_to_name(err), (unsigned)err);
        return err;
    }
    nimble_inited = true;
    ESP_LOGI(TAG, "step 2/5 ok: nimble_port_init (controller enabled, host initialized)");

    /* --- step 3/5: host callbacks + SMP policy --- */
    ble_hs_cfg.reset_cb = ble_prov_on_reset;
    ble_hs_cfg.sync_cb = ble_prov_on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    /* Security: no-input/no-output -> Just Works pairing with bonding, LE
     * Secure Connections when the peer supports it. The first write to an
     * encrypted characteristic (SSID / WiFi Pass / MQTT Pass) triggers the
     * pairing procedure; bonds persist across reboots via
     * CONFIG_BT_NIMBLE_NVS_PERSIST. MITM is impossible without IO. */
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_sc_only = 0;  /* accept legacy pairing for old phones */
    ble_hs_cfg.sm_our_key_dist |= BLE_SM_PAIR_KEY_DIST_ENC;
    ble_hs_cfg.sm_their_key_dist |= BLE_SM_PAIR_KEY_DIST_ENC;
    ESP_LOGI(TAG, "step 3/5 ok: host callbacks + SMP policy configured");

    /* --- step 4/5: services ---
     * void-returning service constructors (NimBLE 2.x), then the
     * int-returning registration calls — same order as the IDF example's
     * gatt_svr_init(). */
    ble_svc_gap_init();
    ble_svc_gatt_init();

    rc = ble_gatts_count_cfg(s_gatt_svcs);
    if (rc == 0) {
        rc = ble_gatts_add_svcs(s_gatt_svcs);
    }
    if (rc == 0) {
        rc = ble_svc_gap_device_name_set(s_adv_name);
    }
    if (rc != 0) {
        ESP_LOGE(TAG, "step 4/5 (service registration) failed; rc=0x%04x "
                      "(NimBLE module 0x%02x, code 0x%02x)",
                 (unsigned)rc, (unsigned)(rc >> 8), (unsigned)(rc & 0xff));
        err = ESP_FAIL;
        goto cleanup;
    }
    ESP_LOGI(TAG, "step 4/5 ok: provisioning GATT services registered");

    /* --- step 5/5: bond/CCC store + host task --- */
    /* NimBLE bond/CCC store (persisted to NVS by the stack itself). */
    ble_store_config_init();

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_running = true;
    s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    s_status_subscribed = false;
    xSemaphoreGive(s_lock);

    nimble_port_freertos_init(ble_prov_host_task);
    ESP_LOGI(TAG, "step 5/5 ok: NimBLE host task started");
    ESP_LOGI(TAG, "BLE provisioning started: name=\"%s\" boardType=\"%s\"",
             s_adv_name, s_board_type);
    return ESP_OK;

cleanup:
    /* Reverse-order rollback. The host task was never started, so the
     * correct undo is a plain nimble_port_deinit() — nimble_port_stop()
     * would wait for a host that never ran. The auto-stop timer is kept:
     * created once, reused by the next attempt. */
    if (nimble_inited) {
        nimble_port_deinit();
        ESP_LOGW(TAG, "cleanup: NimBLE deinitialized after a failed start");
    }
    return err;
}

esp_err_t ble_prov_stop(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_running) {
        xSemaphoreGive(s_lock);
        return ESP_OK;  /* idempotent */
    }
    s_running = false;  /* blocks re-advertise / sync-callback re-entry */
    const uint16_t conn = s_conn_handle;
    xSemaphoreGive(s_lock);

    if (s_stop_timer != NULL) {
        esp_timer_stop(s_stop_timer);
    }

    if (conn != BLE_HS_CONN_HANDLE_NONE) {
        (void)ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
    }

    const int rc = nimble_port_stop();
    if (rc != 0) {
        ESP_LOGE(TAG, "nimble_port_stop failed; rc=%d — host left running", rc);
        return ESP_FAIL;
    }
    nimble_port_deinit();

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    s_status_subscribed = false;
    s_state = PROV_STATE_IDLE;
    memset(&s_pending, 0, sizeof(s_pending));
    memset(s_pending_set, 0, sizeof(s_pending_set));
    xSemaphoreGive(s_lock);

    ESP_LOGI(TAG, "BLE provisioning stopped (NimBLE deinitialized)");
    return ESP_OK;
}

bool ble_prov_is_running(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool running = s_running;
    xSemaphoreGive(s_lock);
    return running;
}

void ble_prov_report_result(ble_prov_result_t result)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_running || s_state != PROV_STATE_CONNECTING) {
        const bool running = s_running;
        const prov_state_t state = s_state;
        xSemaphoreGive(s_lock);
        ESP_LOGW(TAG, "report_result(%d) ignored: running=%d state=%d",
                 (int)result, (int)running, (int)state);
        return;
    }

    if (result == BLE_PROV_RESULT_OK) {
        /* Snapshot under the lock, write NVS outside it (the host task may
         * still mutate s_pending through new writes). */
        const ble_prov_wifi_cfg_t snapshot = s_pending;
        s_state = PROV_STATE_CONNECTED;
        notify_status_locked(STATUS_CONNECTED);
        xSemaphoreGive(s_lock);

        const esp_err_t err = persist_pending(&snapshot);
        if (err != ESP_OK) {
            /* Wi-Fi DID connect — stay CONNECTED for this session, but the
             * config will be gone after a reboot. Loud error on purpose. */
            ESP_LOGE(TAG, "credentials NOT persisted (%s) — board will lose "
                          "the config on reboot", esp_err_to_name(err));
        }

        if (esp_timer_start_once(s_stop_timer,
                                 (uint64_t)BLE_PROV_STOP_DELAY_S * 1000000ULL)
            != ESP_OK) {
            ESP_LOGE(TAG, "failed to arm the stop timer");
        }
        ESP_LOGI(TAG, "provisioning succeeded — stopping BLE in %d s",
                 BLE_PROV_STOP_DELAY_S);
        return;
    }

    const char *status = STATUS_FAILED_ERROR;
    if (result == BLE_PROV_FAIL_BAD_AUTH) {
        status = STATUS_FAILED_BAD_AUTH;
    } else if (result == BLE_PROV_FAIL_TIMEOUT) {
        status = STATUS_FAILED_TIMEOUT;
    }
    s_state = PROV_STATE_IDLE;  /* allow corrected writes + PROVISION retry */
    notify_status_locked(status);
    xSemaphoreGive(s_lock);
    ESP_LOGW(TAG, "provisioning attempt failed: %s", status);
}

esp_err_t ble_prov_load(ble_prov_wifi_cfg_t *out_cfg)
{
    if (out_cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out_cfg, 0, sizeof(*out_cfg));

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(BLE_PROV_NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            return ESP_ERR_NOT_FOUND;  /* nothing was ever provisioned */
        }
        ESP_LOGW(TAG, "nvs_open(%s) failed: %s", BLE_PROV_NVS_NAMESPACE,
                 esp_err_to_name(err));
        return err;
    }

    for (size_t i = 0; err == ESP_OK && i < PENDING_COUNT; i++) {
        char *dst = (char *)out_cfg + s_field_offset[i];
        size_t len = s_field_cap[i];
        err = nvs_get_str(nvs, s_field_key[i], dst, &len);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            err = ESP_ERR_NOT_FOUND;
        } else if (err != ESP_OK) {
            ESP_LOGW(TAG, "nvs_get_str(%s) failed: %s", s_field_key[i],
                     esp_err_to_name(err));
        }
    }
    nvs_close(nvs);
    return err;
}

esp_err_t ble_prov_erase(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(BLE_PROV_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            return ESP_OK;  /* nothing was ever stored */
        }
        ESP_LOGW(TAG, "nvs_open(%s) failed: %s", BLE_PROV_NVS_NAMESPACE,
                 esp_err_to_name(err));
        return err;
    }

    for (size_t i = 0; i < PENDING_COUNT; i++) {
        err = nvs_erase_key(nvs, s_field_key[i]);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            err = ESP_OK;  /* erasing all five is the contract; partial is fine */
        } else if (err != ESP_OK) {
            ESP_LOGW(TAG, "nvs_erase_key(%s) failed: %s", s_field_key[i],
                     esp_err_to_name(err));
        }
    }
    const esp_err_t commit_err = nvs_commit(nvs);
    nvs_close(nvs);

    ESP_LOGI(TAG, "provisioned credentials erased from NVS (%s)",
             BLE_PROV_NVS_NAMESPACE);
    return commit_err;
}
