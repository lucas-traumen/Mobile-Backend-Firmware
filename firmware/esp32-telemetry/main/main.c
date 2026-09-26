/*
 * esp32-telemetry — composition root.
 *
 * SHT30/SHT31 (I2C) -> MQTT telemetry for the Smart Home backend.
 * See PLAN.md for the decisions behind this firmware.
 *
 * Credential sources, in priority order (M16b):
 *   1. NVS "bleprov" namespace — credentials provisioned over BLE earlier.
 *      The board runs like a Kconfig-configured one, except (M22, refined
 *      M23) the stored credentials get one bounded boot check: no AP
 *      (disconnect reason 200/201 — fail-fast), no IP within 15 s, or no
 *      broker connection within a further 10 s erases "bleprov" and
 *      reboots into BLE provisioning (the MQTT client is stopped first).
 *      EXCEPTION — the AP rejected the credentials (reason 202 / BAD_AUTH
 *      as the last failure): "bleprov" is KEPT, no reboot — retries are
 *      suspended and the board parks until the BOOT button (GPIO0, 5 s)
 *      erases the provision. No endless background retry on this branch.
 *   2. Kconfig (CONFIG_WIFI_SSID non-empty) — the classic build-time flow
 *      (60 s wait, background retries, never self-erases).
 *   3. BLE provisioning — nothing stored and no Kconfig SSID: advertise the
 *      provisioning service and wait for the mobile app. Each PROVISION
 *      attempt is bounded by 30 s (app-side timeout, matched); on failure
 *      the board reports FAILED:* back over BLE and waits for the app to
 *      correct the values and resend.
 *
 * Nothing on the BLE path aborts (M16d): ble_prov_start is retried 3 times
 * (5 s apart) and, if it still fails, the board parks in safe mode — no
 * reboot, no boot loop (the relay coils must not chatter), with the BOOT
 * button still active as the recovery path.
 *
 * The BOOT button (GPIO0, active LOW) is a recovery path at runtime: holding
 * it for 5 s erases the provisioned credentials and reboots straight back
 * into BLE provisioning (or Kconfig mode, whichever applies).
 *
 * Board identity (M18): the boardId is CONFIG_DEVICE_ID when set, else
 * derived at boot from the Wi-Fi STA MAC (hex-8 of the last 4 bytes) —
 * see resolve_board_id(). One generic image serves both.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"

#include "sht3x.h"
#include "wifi_conn.h"
#include "mqtt_app.h"
#include "relay.h"
#include "ble_prov.h"
#include "board_id.h"

static const char *TAG = "main";

#if CONFIG_SENSOR_FAKE_MODE
/* Demo ranges: always finite and inside the schema limits
 * (T: -40..125 C, RH: 0..100 %). */
#define FAKE_T_MIN 24.0f
#define FAKE_T_MAX 26.0f
#define FAKE_RH_MIN 55.0f
#define FAKE_RH_MAX 65.0f

static void seed_fake_rng(void)
{
    srand(esp_random());
}

static void fake_read(float *out_temp_c, float *out_rh_pct)
{
    const float t_span = FAKE_T_MAX - FAKE_T_MIN;
    const float rh_span = FAKE_RH_MAX - FAKE_RH_MIN;
    *out_temp_c = FAKE_T_MIN + t_span * ((float)rand() / (float)RAND_MAX);
    *out_rh_pct = FAKE_RH_MIN + rh_span * ((float)rand() / (float)RAND_MAX);
}
#endif /* CONFIG_SENSOR_FAKE_MODE */

#if CONFIG_SHT3X_I2C_ADDR_0X45
#define SHT3X_I2C_DEVICE_ADDR 0x45
#else
#define SHT3X_I2C_DEVICE_ADDR 0x44
#endif

#define I2C_PORT_NUM   0
#define I2C_GLITCH_CNT 7

/* Bounded wait for Wi-Fi; after this, MQTT keeps retrying on its own
 * (Kconfig branch only — see below). */
#define WIFI_WAIT_TIMEOUT_MS 60000

/*
 * M22 — NVS-branch boot check: stored credentials get one bounded chance,
 * never a background retry limbo. Wi-Fi must produce an IP within
 * NVS_WIFI_CHECK_MS (fail-fast on 200/201, see the reason macros below),
 * then MQTT must connect within NVS_MQTT_CHECK_MS. Either check failing
 * erases "bleprov" and reboots into BLE provisioning (M23: the MQTT
 * client is stopped first) — EXCEPT a credentials rejection (reason 202 /
 * BAD_AUTH as the last failure): the provision is kept and the board
 * parks (see nvs_bad_auth_keep_nvs()).
 */
#define NVS_WIFI_CHECK_MS 15000
#define NVS_MQTT_CHECK_MS 10000
#define NVS_POLL_SLICE_MS 250

/*
 * STA disconnect reasons proving the stored SSID is gone from the air.
 * The IDF constants WIFI_REASON_BEACON_TIMEOUT (200) and
 * WIFI_REASON_NO_AP_FOUND (201) live in esp_wifi_types.h, which this
 * component cannot include: esp_wifi is a PRIVATE requirement of
 * wifi_conn, so its headers are not on main's include path. The values
 * are fixed by the 802.11 standard and mirror classify_disconnect_reason()
 * in components/wifi_conn/wifi_conn.c.
 */
#define NVS_REASON_BEACON_TIMEOUT 200 /* WIFI_REASON_BEACON_TIMEOUT */
#define NVS_REASON_NO_AP_FOUND    201 /* WIFI_REASON_NO_AP_FOUND    */

/* Provisioning connection attempts: the app gives up after 30 s, so the
 * board reports its result within the same bound. Waited for in slices so
 * a rejected-password failure (BAD_AUTH) is reported early instead of
 * burning the whole window on hopeless retries. */
#define PROV_WAIT_IP_TIMEOUT_MS 30000
#define PROV_WAIT_POLL_SLICE_MS 250

/* BLE provisioning start (M16d): bounded retries with backoff, then park.
 * A failed ble_prov_start must NEVER abort/reboot — the relay coils hang off
 * this board and a boot loop would chatter them indefinitely. */
#define BLE_START_MAX_ATTEMPTS  3
#define BLE_START_RETRY_DELAY_S 5

/* BOOT button (GPIO0) recovery: hold 5 s while running to erase the
 * provisioned credentials and reboot. GPIO0 is the boot-strap pin — only
 * its level WHILE THE APP RUNS is observed here; holding it across reset
 * still enters the ROM download mode as usual. */
#define BOOT_BUTTON_GPIO      GPIO_NUM_0
#define BOOT_HOLD_ERASE_MS    5000
#define BOOT_POLL_PERIOD_MS   10
#define BOOT_DEBOUNCE_SAMPLES 2   /* 2 identical consecutive reads = 20 ms */
#define BOOT_TASK_STACK_SIZE  3072
#define BOOT_TASK_PRIORITY    3

#define SENSOR_TASK_STACK_SIZE 4096
#define SENSOR_TASK_PRIORITY   5

/* Config received over BLE (on_provision) — read by main only after the
 * event bit is set; exactly one config is in flight at any time because
 * the ble_prov component ignores PROVISION while an attempt is running. */
static ble_prov_wifi_cfg_t s_prov_cfg;
static EventGroupHandle_t s_prov_events;
#define PROV_CFG_READY_BIT BIT0

/* Telemetry bring-up latch (M16d): set once mqtt_app_init() succeeded.
 * mqtt_app_init has no idempotency guard — calling it again after the client
 * exists would create a second client and leak the first — so once the latch
 * is up, start_telemetry() is a no-op returning ESP_OK. */
static bool s_telemetry_client_live;

/* Board identity (M18): resolved once at boot, before the credential
 * branches, so the NVS/Kconfig/BLE paths all use the same boardId (MQTT
 * config, BLE advertising name, board= log lines). Points either at the
 * CONFIG_DEVICE_ID string literal or at s_board_id_mac below (see
 * resolve_board_id()); both outlive every user of the pointer, and both
 * consumers (mqtt_app, ble_prov) copy the string into their own buffers
 * (M14b/M16a), so passing the static pointer is safe. */
static const char *s_board_id;

#if !CONFIG_SENSOR_FAKE_MODE
static sht3x_handle_t s_sensor;
#endif

static void sensor_task(void *arg)
{
    const TickType_t period = pdMS_TO_TICKS(CONFIG_SENSOR_PERIOD_MS);
    TickType_t last_wake = xTaskGetTickCount();
    float temp_c;
    float rh_pct;
    esp_err_t err;

    for (;;) {
#if CONFIG_SENSOR_FAKE_MODE
        /* Demo mode: no SHT3x hardware required — never publish garbage
         * because fake_read() only yields finite in-range values. */
        fake_read(&temp_c, &rh_pct);
        ESP_LOGI(TAG, "FAKE mode: T=%.2f RH=%.2f", temp_c, rh_pct);
        err = ESP_OK;
#else
        err = sht3x_read(s_sensor, &temp_c, &rh_pct);
        if (err != ESP_OK) {
            /* Sensor errors are skipped — never publish garbage. */
            ESP_LOGW(TAG, "SHT3x read failed (%s) — skipping cycle",
                     esp_err_to_name(err));
        }
#endif /* CONFIG_SENSOR_FAKE_MODE */

        if (err == ESP_OK) {
            err = mqtt_app_publish_telemetry(temp_c, rh_pct);
            if (err != ESP_OK) {
                /* Drop only: the mqtt client retransmits QoS1 by itself. */
                ESP_LOGW(TAG, "telemetry dropped: %s", esp_err_to_name(err));
            } else {
                ESP_LOGI(TAG, "telemetry sent: T=%.2f C, RH=%.2f %%", temp_c, rh_pct);
            }
        }
        vTaskDelayUntil(&last_wake, period);
    }
}

/* ------------------------------------------------------------------ */
/* BOOT button recovery                                                */
/* ------------------------------------------------------------------ */

/*
 * Watch GPIO0 (BOOT): holding it down for 5 s erases the provisioned
 * credentials and reboots. Debounced by requiring BOOT_DEBOUNCE_SAMPLES
 * identical consecutive reads before a level change is accepted (bounce
 * cannot fake a 20 ms stable level); the hold time is measured from the
 * confirmed press, so bounces at the press edge only shift the start.
 */
static void boot_button_task(void *arg)
{
    (void)arg;
    const gpio_config_t io_cfg = {
        .pin_bit_mask = 1ULL << BOOT_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,   /* BOOT is active LOW */
    };
    ESP_ERROR_CHECK(gpio_config(&io_cfg));

    bool pressed = false;   /* debounced level: true = held down */
    bool raw_prev = false;
    uint32_t stable_cnt = 0;
    TickType_t held_since = 0;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(BOOT_POLL_PERIOD_MS));
        const bool raw = gpio_get_level(BOOT_BUTTON_GPIO) == 0; /* LOW = press */

        if (raw == raw_prev) {
            if (raw != pressed && ++stable_cnt >= BOOT_DEBOUNCE_SAMPLES) {
                pressed = raw;   /* level stable — accept it */
                if (pressed) {
                    held_since = xTaskGetTickCount();
                    ESP_LOGI(TAG, "BOOT pressed — hold %d s to erase "
                                  "provisioning and reboot",
                             BOOT_HOLD_ERASE_MS / 1000);
                }
            }
        } else {
            raw_prev = raw;
            stable_cnt = 1;
        }

        if (pressed &&
            xTaskGetTickCount() - held_since >=
                pdMS_TO_TICKS(BOOT_HOLD_ERASE_MS)) {
            ESP_LOGW(TAG, "BOOT held %d s — erasing provisioned credentials "
                          "and rebooting", BOOT_HOLD_ERASE_MS / 1000);
            esp_err_t err = ble_prov_erase();
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "ble_prov_erase failed: %s",
                         esp_err_to_name(err));
            }
            esp_restart();
        }
    }
}

/* ------------------------------------------------------------------ */
/* BLE provisioning (M16b)                                             */
/* ------------------------------------------------------------------ */

static esp_err_t start_telemetry(const char *broker_uri, const char *mqtt_user,
                                 const char *mqtt_pass);

/*
 * on_provision runs in the NimBLE host task: copy the snapshot and wake
 * the main task, then return immediately — the ble_prov component notifies
 * CONNECTING right after this returns and blocking here stalls the host.
 */
static void on_provision_cb(const ble_prov_wifi_cfg_t *cfg, void *user_ctx)
{
    (void)user_ctx;
    s_prov_cfg = *cfg;
    xEventGroupSetBits(s_prov_events, PROV_CFG_READY_BIT);
}

static const char *fail_reason_name(wifi_conn_fail_reason_t reason)
{
    switch (reason) {
    case WIFI_CONN_FAIL_BAD_AUTH: return "BAD_AUTH";
    case WIFI_CONN_FAIL_TIMEOUT:  return "TIMEOUT";
    case WIFI_CONN_FAIL_ERROR:    return "ERROR";
    default:                      return "NONE";
    }
}

/* Map the wifi_conn classification onto the BLE provisioning result. */
static ble_prov_result_t prov_result_from_wifi(void)
{
    switch (wifi_conn_last_fail_reason()) {
    case WIFI_CONN_FAIL_BAD_AUTH: return BLE_PROV_FAIL_BAD_AUTH;
    case WIFI_CONN_FAIL_TIMEOUT:  return BLE_PROV_FAIL_TIMEOUT;
    default:                      return BLE_PROV_FAIL_ERROR;
    }
}

/*
 * Wait up to 30 s for an IP, polling in slices. Early exit once the driver
 * recorded BAD_AUTH for the current attempt (the classification is reset by
 * wifi_conn_apply_credentials, so anything observed here belongs to THIS
 * attempt) — the app then shows FAILED:BAD_AUTH within seconds.
 */
static bool wait_provisioning_ip(void)
{
    const TickType_t deadline = pdMS_TO_TICKS(PROV_WAIT_IP_TIMEOUT_MS);
    const TickType_t slice = pdMS_TO_TICKS(PROV_WAIT_POLL_SLICE_MS);
    EventGroupHandle_t eg = wifi_conn_event_group();
    TickType_t waited = 0;

    while (waited < deadline) {
        TickType_t this_slice = deadline - waited;
        if (this_slice > slice) {
            this_slice = slice;
        }
        const EventBits_t bits = xEventGroupWaitBits(eg, WIFI_CONNECTED_BIT,
                                                     pdFALSE, pdTRUE,
                                                     this_slice);
        if (bits & WIFI_CONNECTED_BIT) {
            return true;
        }
        waited += this_slice;
        if (wifi_conn_last_fail_reason() == WIFI_CONN_FAIL_BAD_AUTH) {
            break;
        }
    }
    return false;
}

/*
 * Boot-time diagnostics for the BLE-only path (M16d), logged right before
 * the first ble_prov_start attempt. When a start attempt dies, this block
 * plus the per-step logs inside ble_prov_start tell from the serial log
 * alone whether memory, the compile-time BT config or a specific NimBLE
 * init step is at fault.
 */
static void log_ble_boot_diagnostics(void)
{
    ESP_LOGI(TAG, "BLE diagnostics: free heap=%lu bytes, min ever free=%lu bytes",
             (unsigned long)esp_get_free_heap_size(),
             (unsigned long)esp_get_minimum_free_heap_size());

    uint8_t mac_base[6] = {0};
    uint8_t mac_sta[6] = {0};
    if (esp_efuse_mac_get_default(mac_base) == ESP_OK) {
        ESP_LOGI(TAG, "BLE diagnostics: eFuse base MAC "
                       "%02x:%02x:%02x:%02x:%02x:%02x",
                 mac_base[0], mac_base[1], mac_base[2],
                 mac_base[3], mac_base[4], mac_base[5]);
    }
    if (esp_read_mac(mac_sta, ESP_MAC_WIFI_STA) == ESP_OK) {
        ESP_LOGI(TAG, "BLE diagnostics: Wi-Fi STA MAC "
                       "%02x:%02x:%02x:%02x:%02x:%02x (boardId candidate, M18)",
                 mac_sta[0], mac_sta[1], mac_sta[2],
                 mac_sta[3], mac_sta[4], mac_sta[5]);
    }

#if defined(CONFIG_BT_ENABLED) && defined(CONFIG_BT_NIMBLE_ENABLED)
    ESP_LOGI(TAG, "BLE diagnostics: compile-time config OK — BT_ENABLED=y, "
                  "BT_NIMBLE_ENABLED=y (controller + NimBLE host linked in)");
#elif defined(CONFIG_BT_ENABLED)
    ESP_LOGW(TAG, "BLE diagnostics: BT_ENABLED=y but BT_NIMBLE_ENABLED is "
                  "NOT set — the NimBLE host is not in this image, "
                  "ble_prov_start cannot succeed; fix menuconfig");
#else
    ESP_LOGE(TAG, "BLE diagnostics: BT_ENABLED is NOT set — there is no BLE "
                  "controller in this image, ble_prov_start cannot succeed; "
                  "fix menuconfig");
#endif
}

/*
 * Terminal safe mode after the bounded start attempts failed (M16d): log
 * the recovery instructions and delete this task. Deliberately NOT
 * esp_restart() and NOT abort(): the board must stay up so the relay coils
 * are not reset in a loop, and the boot button task (independent of this
 * task) keeps watching GPIO0 — holding BOOT 5 s erases the credentials and
 * reboots into a fresh attempt once the configuration is fixed.
 */
static void park_provisioning(void)
{
    ESP_LOGE(TAG, "BLE provisioning could not be started after %d attempts "
                  "(see the ble_prov step logs above)", BLE_START_MAX_ATTEMPTS);
    ESP_LOGE(TAG, "SAFE MODE: the board stays up WITHOUT rebooting — no "
                  "provisioning, no telemetry, relays stay OFF");
    ESP_LOGE(TAG, "Recovery: check the log above for the failing init step; "
                  "fix the configuration; then hold the BOOT button for %d s "
                  "to erase credentials and reboot into a fresh attempt "
                  "(or re-flash the firmware)",
             BOOT_HOLD_ERASE_MS / 1000);
    vTaskDelete(NULL);   /* mirrors an early return from app_main */
}

/*
 * BLE-only boot path (M16d): start provisioning with bounded retries, then
 * loop until one attempt connects. Every failed attempt is reported to the
 * app (which may fix the values and resend PROVISION) and the loop goes
 * back to waiting — retry is unbounded by design.
 *
 * Everything on the app-data path is fail-soft: a Wi-Fi or MQTT failure is
 * reported as FAILED:* over BLE and the loop waits for corrected values —
 * nothing on this path may abort or reboot.
 */
static void run_ble_provisioning(void)
{
    s_prov_events = xEventGroupCreate();
    if (s_prov_events == NULL) {
        /* Parking is the only meaningful reaction; keep the board up. */
        ESP_LOGE(TAG, "failed to create provisioning event group (out of "
                      "memory)");
        park_provisioning();
    }

    const ble_prov_cfg_t prov_cfg = {
        .board_id = s_board_id,
        .board_type = CONFIG_BOARD_TYPE,
        .on_provision = on_provision_cb,
        .user_ctx = NULL,
    };

    log_ble_boot_diagnostics();

    /* Bounded start attempts: a transient failure (e.g. controller init
     * racing a brown-out) gets a second/third chance; a persistent one
     * parks the board instead of aborting into a boot loop (M16 bug). */
    esp_err_t err = ESP_FAIL;
    for (int attempt = 1; attempt <= BLE_START_MAX_ATTEMPTS; attempt++) {
        err = ble_prov_start(&prov_cfg);
        if (err == ESP_OK) {
            break;
        }
        ESP_LOGE(TAG, "ble_prov_start attempt %d/%d failed: %s (0x%x)",
                 attempt, BLE_START_MAX_ATTEMPTS,
                 esp_err_to_name(err), (unsigned)err);
        if (attempt < BLE_START_MAX_ATTEMPTS) {
            ESP_LOGW(TAG, "retrying ble_prov_start in %d s ...",
                     BLE_START_RETRY_DELAY_S);
            vTaskDelay(pdMS_TO_TICKS(BLE_START_RETRY_DELAY_S * 1000));
        }
    }
    if (err != ESP_OK) {
        park_provisioning();
    }

    ESP_LOGI(TAG, "BLE provisioning: advertising \"IoTBoard-%s\" "
                  "(boardType=%s) — waiting for the mobile app",
             s_board_id, CONFIG_BOARD_TYPE);

    bool wifi_inited = false;  /* wifi_conn_init() completed */
    bool wifi_dead = false;    /* init failed once — no re-init this boot
                                * (a partial wifi_conn_init must not be run
                                * twice: it would create a second STA netif
                                * and re-init the driver) */
    for (;;) {
        /* Blocks until the app pushes a config (no busy-wait). */
        (void)xEventGroupWaitBits(s_prov_events, PROV_CFG_READY_BIT,
                                  pdTRUE, pdFALSE, portMAX_DELAY);
        ESP_LOGI(TAG, "credentials received over BLE: ssid=\"%s\" broker=%s",
                 s_prov_cfg.ssid, s_prov_cfg.broker_uri);

        if (wifi_dead) {
            ESP_LOGE(TAG, "Wi-Fi bring-up is disabled after the earlier "
                          "init failure this boot — reporting FAILED:ERROR; "
                          "hold BOOT %d s to erase and reboot once fixed",
                     BOOT_HOLD_ERASE_MS / 1000);
            ble_prov_report_result(BLE_PROV_FAIL_ERROR);
            continue;
        }

        if (!wifi_inited) {
            /* First attempt: bring the station up with the received config. */
            err = wifi_conn_init(s_prov_cfg.ssid, s_prov_cfg.wifi_pass);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "wifi_conn_init failed: %s (0x%x) — disabling "
                              "Wi-Fi bring-up for this boot",
                         esp_err_to_name(err), (unsigned)err);
                wifi_dead = true;
                ble_prov_report_result(BLE_PROV_FAIL_ERROR);
                continue;
            }
            wifi_inited = true;
        } else {
            /* Later attempts: swap the credentials in a running station. */
            err = wifi_conn_apply_credentials(s_prov_cfg.ssid,
                                              s_prov_cfg.wifi_pass);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "wifi_conn_apply_credentials failed: %s (0x%x)",
                         esp_err_to_name(err), (unsigned)err);
                ble_prov_report_result(BLE_PROV_FAIL_ERROR);
                continue;
            }
        }
        /* Idempotent: no-op once the station runs (also starts it after an
         * apply-into-stopped-station). */
        err = wifi_conn_start();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "wifi_conn_start failed: %s (0x%x)",
                     esp_err_to_name(err), (unsigned)err);
            ble_prov_report_result(BLE_PROV_FAIL_ERROR);
            continue;
        }

        if (wait_provisioning_ip()) {
            ESP_LOGI(TAG, "provisioned Wi-Fi connected (credentials source: BLE)");
            /* Telemetry first, CONNECTED report second: a bad app-supplied
             * broker URI must reach the app as FAILED:ERROR so it can resend
             * corrected values, instead of provisioning a board that will
             * never publish. */
            err = start_telemetry(s_prov_cfg.broker_uri, s_prov_cfg.mqtt_user,
                                  s_prov_cfg.mqtt_pass);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "telemetry bring-up failed (%s) — reporting "
                              "FAILED:ERROR, waiting for corrected values",
                         esp_err_to_name(err));
                ble_prov_report_result(BLE_PROV_FAIL_ERROR);
                continue;
            }
            /* Persists to NVS, notifies CONNECTED, arms the 30 s auto-stop. */
            ble_prov_report_result(BLE_PROV_RESULT_OK);
            return;
        }

        const ble_prov_result_t result = prov_result_from_wifi();
        ESP_LOGW(TAG, "no IP within %d s (last fail: %s, reason=%d) — "
                      "reporting result %d, waiting for a new PROVISION",
                 PROV_WAIT_IP_TIMEOUT_MS,
                 fail_reason_name(wifi_conn_last_fail_reason()),
                 wifi_conn_last_fail_code(), (int)result);
        ble_prov_report_result(result);
        /* M20: stop the retry loop from churning the wrong credentials
         * while waiting for the app to resend PROVISION (log spam + BLE/Wi-Fi
         * coexistence) — the next wifi_conn_apply_credentials() resumes
         * connecting by itself. */
        (void)wifi_conn_suspend_retries();
    }
}

/* ------------------------------------------------------------------ */
/* Telemetry bring-up (MQTT + sensor task)                             */
/* ------------------------------------------------------------------ */

/*
 * Init + start the MQTT client with the firmware-v2 board-centric config —
 * no sensor task, no "started:" line yet. The M22 NVS branch needs this
 * seam: it verifies the broker is actually reachable BEFORE the sensor
 * task exists (the old flow created the task right after
 * mqtt_app_start(), producing "drop telemetry: not connected" spam while
 * the broker was still down).
 *
 * Sets s_telemetry_client_live once mqtt_app_init() succeeded — the client
 * then exists and mqtt_app_init must not run again this boot (no
 * idempotency guard there: a second call would leak the first client).
 * Failures BEFORE that point leave nothing behind, so a corrected
 * config can retry cleanly.
 */
static esp_err_t telemetry_init_and_start(const char *broker_uri,
                                          const char *mqtt_user,
                                          const char *mqtt_pass)
{
    /* Firmware v2 speaks the boards contract directly. */
    const mqtt_app_config_t mqtt_cfg = {
        .broker_uri = broker_uri,
        .username = mqtt_user,
        .password = mqtt_pass,
        .topic_prefix = CONFIG_MQTT_TOPIC_PREFIX,
        .board_id = s_board_id,
        .display_name = CONFIG_BOARD_DISPLAY_NAME,
        .outbox_limit = CONFIG_MQTT_OUTBOX_LIMIT,
    };

    esp_err_t err = mqtt_app_init(&mqtt_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mqtt_app_init failed: %s (0x%x) — a malformed broker "
                      "URI sent over BLE is the usual cause",
                 esp_err_to_name(err), (unsigned)err);
        return err;
    }
    s_telemetry_client_live = true;   /* the MQTT client exists from here on */

    err = mqtt_app_start();
    if (err != ESP_OK) {
        /* Practically unreachable; the client is kept to avoid a leak, so
         * bring-up cannot be retried this boot. */
        ESP_LOGE(TAG, "mqtt_app_start failed: %s (0x%x)",
                 esp_err_to_name(err), (unsigned)err);
        return err;
    }
    return ESP_OK;
}

/*
 * Create the sensor task + log the "started:" line. On the M22 NVS branch
 * this runs only after mqtt_app_is_connected() confirmed the broker; the
 * Kconfig/BLE branches reach it via start_telemetry() right after client
 * start (unchanged behavior there — their sensor task drops cycles until
 * the client connects).
 *
 * Returns esp_err_t so the BLE provisioning path can fail soft (report
 * FAILED:ERROR to the app instead of aborting). ESP_ERR_NO_MEM leaves the
 * client up (re-entering bring-up would leak it).
 */
static esp_err_t telemetry_start_sensor_task(const char *broker_uri)
{
    BaseType_t task_ok = xTaskCreate(sensor_task, "sensor_task",
                                     SENSOR_TASK_STACK_SIZE, NULL,
                                     SENSOR_TASK_PRIORITY, NULL);
    if (task_ok != pdPASS) {
        /* MQTT is already up: do not fail the provisioning contract over
         * this (re-entering bring-up would leak the client). No telemetry
         * values are published until reboot — loud error on purpose. */
        ESP_LOGE(TAG, "failed to create sensor task — MQTT is up but NO "
                      "measurements will be published until reboot");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "started: board=%s prefix=%s broker=%s",
             s_board_id, CONFIG_MQTT_TOPIC_PREFIX, broker_uri);
    return ESP_OK;
}

/*
 * Full bring-up for the Kconfig and BLE paths (behavior unchanged): those
 * branches tolerate a downed broker (the client reconnects in the
 * background), so init+start+sensor task happen back-to-back.
 *
 * Idempotency: the s_telemetry_client_live latch makes later calls
 * no-ops (ESP_OK) — the pre-M22 contract, kept verbatim.
 */
static esp_err_t start_telemetry(const char *broker_uri, const char *mqtt_user,
                                 const char *mqtt_pass)
{
    if (s_telemetry_client_live) {
        ESP_LOGW(TAG, "start_telemetry: already brought up — ignoring");
        return ESP_OK;
    }

    esp_err_t err = telemetry_init_and_start(broker_uri, mqtt_user, mqtt_pass);
    if (err != ESP_OK) {
        return err;
    }
    return telemetry_start_sensor_task(broker_uri);
}

/* ------------------------------------------------------------------ */
/* NVS stale-credential check (M22)                                    */
/* ------------------------------------------------------------------ */

/*
 * Bounded Wi-Fi wait for the NVS branch (the Kconfig branch keeps its
 * plain 60 s wait + endless background retries): poll WIFI_CONNECTED_BIT
 * in short slices and watch wifi_conn_last_fail_code(). Reasons 200
 * (beacon timeout) / 201 (no AP found) prove the stored SSID is not on
 * the air — fail immediately instead of burning the whole window while
 * the driver's retry backoff climbs (1/2/4/8/16/30 s). A wrong password
 * (202) and every other reason do NOT fail fast: the full window is
 * waited, then the absence of an IP decides — the caller then splits
 * BAD_AUTH (keep the provision, park the board) from everything else
 * (erase + reboot).
 */
static bool wait_nvs_wifi_connected(void)
{
    const TickType_t deadline = pdMS_TO_TICKS(NVS_WIFI_CHECK_MS);
    const TickType_t slice = pdMS_TO_TICKS(NVS_POLL_SLICE_MS);
    EventGroupHandle_t eg = wifi_conn_event_group();
    TickType_t waited = 0;

    while (waited < deadline) {
        TickType_t this_slice = deadline - waited;
        if (this_slice > slice) {
            this_slice = slice;
        }
        const EventBits_t bits = xEventGroupWaitBits(eg, WIFI_CONNECTED_BIT,
                                                     pdFALSE, pdTRUE,
                                                     this_slice);
        if (bits & WIFI_CONNECTED_BIT) {
            return true;
        }
        waited += this_slice;
        const int code = wifi_conn_last_fail_code();
        if (code == NVS_REASON_BEACON_TIMEOUT ||
            code == NVS_REASON_NO_AP_FOUND) {
            ESP_LOGW(TAG, "NVS check: stored SSID not on the air "
                          "(reason=%d) after %u ms — failing fast",
                     code, (unsigned)waited);
            return false;
        }
    }
    ESP_LOGW(TAG, "NVS check: no IP within %d s "
                  "(last disconnect reason=%d)",
             NVS_WIFI_CHECK_MS / 1000, wifi_conn_last_fail_code());
    return false;
}

/*
 * Bounded MQTT wait for the NVS branch: the client retries on its own,
 * but a broker that has not answered within the window marks the
 * provision stale — no endless retry limbo on this branch.
 */
static bool wait_nvs_mqtt_connected(void)
{
    const TickType_t deadline = pdMS_TO_TICKS(NVS_MQTT_CHECK_MS);
    const TickType_t slice = pdMS_TO_TICKS(NVS_POLL_SLICE_MS);
    TickType_t waited = 0;

    while (waited < deadline) {
        if (mqtt_app_is_connected()) {
            return true;
        }
        TickType_t this_slice = deadline - waited;
        if (this_slice > slice) {
            this_slice = slice;
        }
        vTaskDelay(this_slice);
        waited += this_slice;
    }
    return mqtt_app_is_connected();
}

/*
 * Terminal path for a stale provision: stop the Wi-Fi retry churn first
 * (the radio must not keep connecting against an AP/broker that just
 * failed the check), then close the MQTT client (M23 — it is live on the
 * broker-fail path; a safe no-op when it was never created, e.g. the
 * Wi-Fi-fail path), clear the bring-up latch, erase the bleprov namespace
 * and reboot. The next boot finds an empty NVS and — with CONFIG_WIFI_SSID
 * empty — enters BLE provisioning (branch 4c). The erase only ever runs
 * from a branch that KNOWS credentials existed (ble_prov_load == ESP_OK),
 * so there is no erase-reboot loop.
 */
static void nvs_stale_erase_reboot(void)
{
    (void)wifi_conn_suspend_retries();
    mqtt_app_stop();                  /* no-op unless the client exists */
    s_telemetry_client_live = false;
    const esp_err_t err = ble_prov_erase();
    if (err != ESP_OK) {
        /* Same pattern as the BOOT button path: log it, but reboot
         * anyway — wedging on a broken NVS write would be worse. */
        ESP_LOGE(TAG, "ble_prov_erase failed: %s", esp_err_to_name(err));
    }
    ESP_LOGW(TAG, "stale NVS — erased, BLE provisioning");
    esp_restart();
}

/*
 * BAD_AUTH terminal path (M23): the AP rejected the STORED credentials
 * (reason 202 or any other BAD_AUTH classification as the last failure).
 * Erasing and rebooting cannot fix a wrong password by itself and would
 * also discard a provision that may still be fine (a transient
 * router-side rejection), so the NVS is deliberately KEPT: suspend the
 * retry loop so the radio stops churning rejected credentials, then
 * return from app_main with the board parked — relays are already OFF,
 * the BOOT button task (created before this branch) keeps watching
 * GPIO0, and holding BOOT for BOOT_HOLD_ERASE_MS erases the provision
 * and reboots into BLE provisioning. No sensor task, no MQTT on this
 * path (mqtt_app_stop is deliberately NOT called — the client was never
 * created here).
 */
static void nvs_bad_auth_keep_nvs(void)
{
    (void)wifi_conn_suspend_retries();
    ESP_LOGW(TAG, "Wi-Fi credentials rejected — NVS kept, board parked; "
                  "hold the BOOT button (GPIO0) for %d s to erase and "
                  "re-enter BLE provisioning",
             BOOT_HOLD_ERASE_MS / 1000);
}

/* First non-empty string, for per-field NVS -> Kconfig fallback. */
static const char *first_non_empty(const char *a, const char *b)
{
    return (a != NULL && a[0] != '\0') ? a : b;
}

/*
 * Build-time Kconfig SSID check, marked noinline on purpose: CONFIG_WIFI_SSID
 * expands to a string literal, and at -O2 GCC would otherwise constant-fold
 * the branch in app_main(), dead-strip the whole BLE provisioning path
 * (ble_prov_start/report_result + NimBLE) out of the linked image and ship a
 * binary that can never provision. One firmware image must support all three
 * credential sources, so the branch decision stays a runtime call.
 */
static bool __attribute__((noinline)) kconfig_ssid_present(void)
{
    return CONFIG_WIFI_SSID[0] != '\0';
}

/* Static buffer for the MAC-derived boardId: 8 hex chars + NUL. */
static char s_board_id_mac[9];

/*
 * Runtime boardId resolution (M18), noinline on purpose — the same
 * literal-folding hazard kconfig_ssid_present() guards against above:
 * CONFIG_DEVICE_ID expands to a string literal, so at -O2 GCC would
 * constant-fold the empty check below and, depending on the literal,
 * dead-strip the whole MAC branch (or the verbatim-id return) out of the
 * image. One firmware image must serve both fixed-id and MAC-derived
 * boards, so the decision stays a runtime call.
 *
 *   - CONFIG_DEVICE_ID non-empty -> returned verbatim (back-compat).
 *   - CONFIG_DEVICE_ID empty     -> lowercase hex-8 of the LAST 4 bytes of
 *     the Wi-Fi STA MAC — the same MAC log_ble_boot_diagnostics() prints
 *     as the "boardId candidate" — e.g. 5c:01:3b:6b:af:6c -> "3b6baf6c".
 *     The eFuse MAC never changes, so no NVS copy is needed.
 *
 * *from_mac reports which source filled the id (for the boot log).
 */
static const char *__attribute__((noinline))
resolve_board_id(bool *from_mac)
{
    if (CONFIG_DEVICE_ID[0] != '\0') {
        *from_mac = false;
        return CONFIG_DEVICE_ID;
    }

    uint8_t mac[6] = {0};
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK ||
        board_id_from_mac(mac, s_board_id_mac, sizeof(s_board_id_mac)) == 0) {
        /* Unreachable in practice (the eFuse MAC always reads back and the
         * buffer fits hex-8). Fall back to the Kconfig string rather than
         * hand out an empty id — mqtt_app/ble_prov reject an empty board_id
         * in their config checks and fail soft, so nothing publishes an
         * invalid topic even then. */
        ESP_LOGE(TAG, "cannot derive boardId from MAC — falling back to "
                      "CONFIG_DEVICE_ID");
        *from_mac = false;
        return CONFIG_DEVICE_ID;
    }
    *from_mac = true;
    return s_board_id_mac;
}

void app_main(void)
{
    /* 0. Relays first: drive every channel OFF before anything else can fail
     *    or stall startup, so the coils are never left floating. */
    ESP_ERROR_CHECK(relay_init());

    /* 1. NVS (required by the Wi-Fi driver and the BLE provisioning store). */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* 2. Network stack + default event loop. */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

#if CONFIG_SENSOR_FAKE_MODE
    /* 3. Demo mode: no SHT3x hardware and no I2C bus needed.
     *    Seed the fake-data RNG once at startup. */
    seed_fake_rng();
#else
    /* 3. I2C master bus (100 kHz is set per-device in sht3x) + SHT3x. */
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_PORT_NUM,
        .sda_io_num = (gpio_num_t)CONFIG_SHT3X_I2C_SDA_GPIO,
        .scl_io_num = (gpio_num_t)CONFIG_SHT3X_I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = I2C_GLITCH_CNT,
        .flags = {
            .enable_internal_pullup = true, /* external pull-ups still recommended */
        },
    };
    i2c_master_bus_handle_t bus;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &bus));
    ESP_ERROR_CHECK(sht3x_init(bus, SHT3X_I2C_DEVICE_ADDR, &s_sensor));
#endif /* CONFIG_SENSOR_FAKE_MODE */

    /* 3.5. BOOT button recovery task — active whenever the app runs
     *      (only the runtime level of GPIO0 matters; its level across a
     *      reset is the ROM's download-mode strap, untouched here). */
    if (xTaskCreate(boot_button_task, "boot_btn",
                    BOOT_TASK_STACK_SIZE, NULL,
                    BOOT_TASK_PRIORITY, NULL) != pdPASS) {
        ESP_LOGE(TAG, "failed to create boot button task");
        ESP_ERROR_CHECK(ESP_ERR_NO_MEM);
    }

    /* 3.6. Board identity (M18): CONFIG_DEVICE_ID when set, otherwise
     *      hex-8 derived from the Wi-Fi STA MAC (last 4 bytes). Resolved
     *      once here, before the credential branches, so the NVS, Kconfig
     *      and BLE paths all publish the same boardId. */
    bool board_id_from_mac_src = false;
    s_board_id = resolve_board_id(&board_id_from_mac_src);
    ESP_LOGI(TAG, "boardId=\"%s\" (%s)", s_board_id,
             board_id_from_mac_src ? "from MAC" : "from Kconfig");

    /* 4. Credentials: NVS bleprov -> Kconfig -> BLE provisioning. */
    ble_prov_wifi_cfg_t stored;
    const esp_err_t have_stored = ble_prov_load(&stored);
    if (have_stored == ESP_OK) {
        /* 4a. Board already provisioned over BLE (M22, refined M23): one
         *     bounded check of the stored credentials — Wi-Fi must produce
         *     an IP within NVS_WIFI_CHECK_MS (fail-fast on reason 200/201),
         *     then MQTT must connect within NVS_MQTT_CHECK_MS. Either check
         *     failing means the provision is stale: stop the MQTT client,
         *     erase "bleprov" and reboot into BLE provisioning. EXCEPTION
         *     (M23): a credentials rejection (202 / BAD_AUTH) keeps the
         *     provision and parks the board (see nvs_bad_auth_keep_nvs).
         *     No background retry limbo here; BLE stays off while the
         *     check lasts. */
        ESP_LOGI(TAG, "credentials source: NVS (provisioned) ssid=\"%s\"",
                 stored.ssid);
        ESP_ERROR_CHECK(wifi_conn_init(stored.ssid, stored.wifi_pass));
        /* M22 (opt-in, M23): on THIS branch a missing AP (200/201) must
         * latch the retry loop OFF — the fail-fast below reads the stable
         * fail code and backing off cannot fix a gone AP. The Kconfig (4b)
         * and BLE (4c) branches keep the default: background retry for
         * every reason. */
        wifi_conn_stop_retries_when_ap_gone(true);
        ESP_ERROR_CHECK(wifi_conn_start());

        if (!wait_nvs_wifi_connected()) {
            if (wifi_conn_last_fail_reason() == WIFI_CONN_FAIL_BAD_AUTH) {
                /* M23: the AP rejected the STORED credentials — erasing and
                 * rebooting cannot fix a wrong password and would discard a
                 * provision the user can still recover deliberately. Park
                 * here: no erase, no reboot, no MQTT, no sensor task. */
                nvs_bad_auth_keep_nvs();
                return;
            }
            nvs_stale_erase_reboot();
        }

        /* 5. MQTT — per-field NVS value, falling back to Kconfig when
         *    empty. init+start only here; the sensor task is created
         *    after the broker check passes so no cycle is
         *    created-then-dropped ("drop telemetry: not connected"). */
        const char *broker = first_non_empty(stored.broker_uri,
                                             CONFIG_MQTT_BROKER_URI);
        const char *user = first_non_empty(stored.mqtt_user,
                                           CONFIG_MQTT_USER);
        const char *pass = first_non_empty(stored.mqtt_pass,
                                           CONFIG_MQTT_PASSWORD);
        esp_err_t tel = telemetry_init_and_start(broker, user, pass);
        if (tel != ESP_OK) {
            ESP_LOGW(TAG, "NVS check: MQTT bring-up failed (%s) — treating "
                          "the provision as stale", esp_err_to_name(tel));
            nvs_stale_erase_reboot();
        }
        if (!wait_nvs_mqtt_connected()) {
            ESP_LOGW(TAG, "NVS check: MQTT not connected within %d s",
                     NVS_MQTT_CHECK_MS / 1000);
            nvs_stale_erase_reboot();
        }
        ESP_ERROR_CHECK(telemetry_start_sensor_task(broker));
        return;
    }

    if (have_stored != ESP_ERR_NOT_FOUND) {
        ESP_LOGW(TAG, "ble_prov_load failed (%s) — treating as not provisioned",
                 esp_err_to_name(have_stored));
    }

    if (kconfig_ssid_present()) {
        /* 4b. Classic build-time credentials — flow unchanged. Note the
         *     DEFAULT opt-out here (no wifi_conn_stop_retries_when_ap_gone
         *     call): a gone AP (200/201) keeps the normal background retry,
         *     like every other reason (M23). */
        ESP_LOGI(TAG, "credentials source: Kconfig ssid=\"%s\"", CONFIG_WIFI_SSID);
        ESP_ERROR_CHECK(wifi_conn_init(CONFIG_WIFI_SSID, CONFIG_WIFI_PASSWORD));
        ESP_ERROR_CHECK(wifi_conn_start());

        EventBits_t bits = xEventGroupWaitBits(wifi_conn_event_group(),
                                               WIFI_CONNECTED_BIT,
                                               pdFALSE, pdTRUE,
                                               pdMS_TO_TICKS(WIFI_WAIT_TIMEOUT_MS));
        if (!(bits & WIFI_CONNECTED_BIT)) {
            ESP_LOGW(TAG, "Wi-Fi not connected after %d s; "
                          "Wi-Fi and MQTT keep retrying in the background",
                     WIFI_WAIT_TIMEOUT_MS / 1000);
        }

        ESP_ERROR_CHECK(start_telemetry(CONFIG_MQTT_BROKER_URI, CONFIG_MQTT_USER,
                                        CONFIG_MQTT_PASSWORD));
        return;
    }

    /* 4c. Nothing stored, nothing configured: BLE provisioning mode. The
     *     loop only returns once one attempt connects; MQTT and the sensor
     *     task start afterwards with the provisioned credentials. */
    ESP_LOGI(TAG, "no stored credentials and CONFIG_WIFI_SSID is empty — "
                  "BLE provisioning mode");
    run_ble_provisioning();
}
