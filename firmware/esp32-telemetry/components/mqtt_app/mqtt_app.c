/*
 * mqtt_app — implementation (firmware v2, contract board-centric; M14b).
 */

#include <ctype.h>
#include <inttypes.h>
#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "sdkconfig.h"
#include "cJSON.h"
#include "mqtt_client.h"

#include "broker_uri.h"
#include "mqtt_app.h"
#include "relay.h"

static const char *TAG = "mqtt_app";

#define BROKER_URI_MAX 128
#define CRED_MAX       64
#define PREFIX_MAX     64
#define ID_MAX         64
#define DISPLAY_NAME_MAX 64

/*
 * "{prefix}/boards/" (8) + boardId + longest suffix. Fixed topics need at
 * most "/relays/+/set" (13) and "/descriptor" (11); dynamic per-channel
 * topics need "/relays/" (8) + channel + "/state" (6). 96 of slack covers
 * the worst case; fmt_bounded() refuses truncation on top of that.
 */
#define TOPIC_MAX (PREFIX_MAX + ID_MAX + 96)

/*
 * Board hardware description — template "A" (mirror of the bridge template
 * in src/bridge/boards-mapper.ts). This table is the single source of truth
 * for the descriptor AND the telemetry values: sensors[0] carries the
 * temperature sample and sensors[1] the humidity sample (see
 * mqtt_app_publish_telemetry). A board with different hardware declares a
 * different table (and CONFIG_BOARD_TYPE) here. The boardType string comes
 * from the BOARD_TYPE Kconfig option, shared with BLE Device Info (M16a).
 */

typedef struct {
    const char *channel; /**< topic segment + "values" key, e.g. "S1" */
    const char *field;   /**< Influx field name from the descriptor    */
    const char *unit;    /**< descriptor unit                          */
} sensor_desc_t;

static const sensor_desc_t s_sensors[] = {
    { .channel = "S1", .field = "temperature", .unit = "°C" },
    { .channel = "S2", .field = "humidity",    .unit = "%"  },
};

#define SENSOR_COUNT (sizeof(s_sensors) / sizeof(s_sensors[0]))

/* The positional temp->sensors[0] / rh->sensors[1] mapping needs exactly 2. */
_Static_assert(SENSOR_COUNT == 2, "template A expects exactly S1 + S2");

#define ONLINE_PAYLOAD  "online"
#define OFFLINE_PAYLOAD "offline"

static char s_broker_uri[BROKER_URI_MAX];
static char s_username[CRED_MAX];
static char s_password[CRED_MAX];
static char s_topic_prefix[PREFIX_MAX];
static char s_board_id[ID_MAX];
static char s_display_name[DISPLAY_NAME_MAX];
static char s_board_base[TOPIC_MAX];        /* "{prefix}/boards/{boardId}" */
static char s_descriptor_topic[TOPIC_MAX];
static char s_telemetry_topic[TOPIC_MAX];
static char s_status_topic[TOPIC_MAX];
static char s_relays_prefix[TOPIC_MAX];     /* "{base}/relays/" (trailing '/') */
static char s_relay_cmd_pattern[TOPIC_MAX]; /* "{base}/relays/+/set"           */

static esp_mqtt_client_handle_t s_client;
static bool s_connected;
static bool s_initialized;

/*
 * Event topics are NOT NUL-terminated: always compare with the length.
 */
static bool topic_starts_with(const char *topic, int topic_len, const char *prefix)
{
    const size_t prefix_len = strlen(prefix);
    return (size_t)topic_len >= prefix_len &&
           memcmp(topic, prefix, prefix_len) == 0;
}

/* Accept "ON"/"OFF" (case-insensitive), optionally surrounded by whitespace. */
static bool parse_on_off(const char *text, int len, bool *out_on)
{
    while (len > 0 && isspace((unsigned char)text[0])) {
        text++;
        len--;
    }
    while (len > 0 && isspace((unsigned char)text[len - 1])) {
        len--;
    }
    if (len == 2 && strncasecmp(text, "ON", 2) == 0) {
        *out_on = true;
        return true;
    }
    if (len == 3 && strncasecmp(text, "OFF", 3) == 0) {
        *out_on = false;
        return true;
    }
    return false;
}

/*
 * Firmware v1 published values with "%.2f"; keep the same 2-decimal precision
 * so the promoted float never prints double fuzz ("24.130000114440918" from
 * %1.15g) into the JSON payloads and retained states.
 */
static double round2(double v)
{
    const double sign = (v < 0.0) ? -1.0 : 1.0;
    return sign * floor(fabs(v) * 100.0 + 0.5) / 100.0;
}

/* Copy/format refusing truncation — a silently truncated topic would break
 * the contract, so overlong configuration is an init error instead. */
static esp_err_t fmt_bounded(char *dst, size_t dst_size, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(dst, dst_size, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= dst_size) {
        ESP_LOGE(TAG, "buffer too small (need %d, have %u)",
                 n, (unsigned)dst_size);
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

/*
 * One publish; -1 (failure) and -2 (outbox full) are logged and dropped —
 * no manual retry, the client retransmits unacknowledged QoS1 by itself.
 */
static esp_err_t publish_text(const char *topic, const char *payload,
                              int qos, bool retain)
{
    if (!s_initialized || s_client == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    const int len = (int)strlen(payload);
    const int msg_id = esp_mqtt_client_publish(s_client, topic, payload, len,
                                               qos, retain ? 1 : 0);
    if (msg_id == -2) {
        ESP_LOGW(TAG, "publish %s: outbox full (limit reached) — dropped",
                 topic);
        return ESP_ERR_NO_MEM;
    }
    if (msg_id < 0) {
        ESP_LOGW(TAG, "publish %s: failed (msg_id=%d) — dropped", topic, msg_id);
        return ESP_FAIL;
    }
    ESP_LOGD(TAG, "publish queued msg_id=%d topic=%s", msg_id, topic);
    return ESP_OK;
}

/*
 * Descriptor — the board's own hardware table, published retained on every
 * (re)connect so the backend registry and the app always hold the fresh one.
 */
static esp_err_t publish_descriptor(void)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = ESP_OK;
    if (cJSON_AddNumberToObject(root, "schemaVersion", 1) == NULL ||
        cJSON_AddStringToObject(root, "boardId", s_board_id) == NULL ||
        cJSON_AddStringToObject(root, "boardType", CONFIG_BOARD_TYPE) == NULL) {
        err = ESP_ERR_NO_MEM;
    }
    if (err == ESP_OK && s_display_name[0] != '\0' &&
        cJSON_AddStringToObject(root, "displayName", s_display_name) == NULL) {
        err = ESP_ERR_NO_MEM;
    }

    cJSON *sensors = NULL;
    cJSON *relays = NULL;
    if (err == ESP_OK) {
        sensors = cJSON_AddArrayToObject(root, "sensors");
        relays = cJSON_AddArrayToObject(root, "relays");
        if (sensors == NULL || relays == NULL) {
            err = ESP_ERR_NO_MEM;
        }
    }

    for (size_t i = 0; err == ESP_OK && i < SENSOR_COUNT; i++) {
        cJSON *item = cJSON_CreateObject();
        if (item == NULL ||
            cJSON_AddStringToObject(item, "channel", s_sensors[i].channel) == NULL ||
            cJSON_AddStringToObject(item, "field", s_sensors[i].field) == NULL ||
            cJSON_AddStringToObject(item, "unit", s_sensors[i].unit) == NULL ||
            !cJSON_AddItemToArray(sensors, item)) {
            /* Detached on any failure in the chain — safe to free. */
            cJSON_Delete(item);
            err = ESP_ERR_NO_MEM;
        }
    }

    size_t relay_count = 0;
    const relay_info_t *relay_channels = relay_get_all(&relay_count);
    for (size_t i = 0; err == ESP_OK && i < relay_count; i++) {
        cJSON *item = cJSON_CreateObject();
        if (item == NULL ||
            cJSON_AddStringToObject(item, "channel", relay_channels[i].id) == NULL ||
            !cJSON_AddItemToArray(relays, item)) {
            cJSON_Delete(item);
            err = ESP_ERR_NO_MEM;
        }
    }

    if (err == ESP_OK) {
        char *payload = cJSON_PrintUnformatted(root);
        if (payload == NULL) {
            err = ESP_ERR_NO_MEM;
        } else {
            /* QoS 1, retain = true (the registry reloads it on every start). */
            err = publish_text(s_descriptor_topic, payload, 1, true);
            cJSON_free(payload);
        }
    }
    cJSON_Delete(root);
    return err;
}

/* "{base}/sensors/{channel}/state" — plain number, retained (app realtime). */
static esp_err_t publish_sensor_state(const char *channel, const cJSON *num)
{
    char topic[TOPIC_MAX];
    if (fmt_bounded(topic, sizeof(topic), "%s/sensors/%s/state",
                    s_board_base, channel) != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }
    char *text = cJSON_PrintUnformatted(num);
    if (text == NULL) {
        return ESP_ERR_NO_MEM;
    }
    const esp_err_t err = publish_text(topic, text, 1, true);
    cJSON_free(text);
    return err;
}

/* "{base}/relays/{K}/state" — plain ON/OFF, retained (app draws UI from it). */
static esp_err_t relay_publish_state(const char *channel_id, bool on)
{
    char topic[TOPIC_MAX];
    if (fmt_bounded(topic, sizeof(topic), "%s/relays/%s/state",
                    s_board_base, channel_id) != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }
    return publish_text(topic, on ? "ON" : "OFF", 1, true);
}

/* Publish every channel from the relay table (on connect). */
static void relay_publish_all_states(void)
{
    size_t count = 0;
    const relay_info_t *channels = relay_get_all(&count);
    for (size_t i = 0; i < count; i++) {
        (void)relay_publish_state(channels[i].id, channels[i].state);
    }
}

/* Apply one already-validated command and echo the new state. */
static void relay_apply(const char *id, bool on)
{
    const esp_err_t err = relay_set(id, on);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "relay \"%s\": set failed (%s) — ignored",
                 id, esp_err_to_name(err));
        return;
    }
    (void)relay_publish_state(id, on);
}

/*
 * Plain per-channel command on {prefix}/boards/{boardId}/relays/{K}/set with
 * payload ON/OFF (trimmed). The channel is parsed from the topic segment;
 * unknown channels are rejected by relay_set(), bad payloads here — both
 * logged and dropped, never a crash and never a publish.
 */
static void relay_handle_command(const char *topic, int topic_len,
                                 const char *data, int data_len)
{
    const size_t prefix_len = strlen(s_relays_prefix);
    if (!topic_starts_with(topic, topic_len, s_relays_prefix)) {
        ESP_LOGW(TAG, "relay: unexpected topic — ignored");
        return;
    }

    const char *rest = topic + prefix_len;
    const int rest_len = topic_len - (int)prefix_len;
    const char *slash = memchr(rest, '/', (size_t)rest_len);
    if (slash == NULL) {
        ESP_LOGW(TAG, "relay: malformed per-channel topic — ignored");
        return;
    }

    const int id_len = (int)(slash - rest);
    const char *suffix = slash + 1;
    const int suffix_len = rest_len - id_len - 1;
    if (id_len <= 0 || id_len >= ID_MAX ||
        suffix_len != 3 || memcmp(suffix, "set", 3) != 0) {
        ESP_LOGW(TAG, "relay: malformed per-channel topic — ignored");
        return;
    }

    char id[ID_MAX];
    memcpy(id, rest, (size_t)id_len);
    id[id_len] = '\0';

    bool on = false;
    if (!parse_on_off(data, data_len, &on)) {
        ESP_LOGW(TAG, "relay %s: payload is not ON/OFF — ignored", id);
        return;
    }

    relay_apply(id, on);
}

static void relay_subscribe(void)
{
    const int msg_id = esp_mqtt_client_subscribe(s_client, s_relay_cmd_pattern, 1);
    if (msg_id < 0) {
        ESP_LOGW(TAG, "subscribe %s failed (msg_id=%d)",
                 s_relay_cmd_pattern, msg_id);
    }
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        s_connected = true;
        (void)publish_text(s_status_topic, ONLINE_PAYLOAD, 1, true);
        (void)publish_descriptor();
        relay_subscribe();
        relay_publish_all_states();
        ESP_LOGI(TAG, "connected to broker; published retained \"online\" + "
                      "descriptor + relay states");
        break;

    case MQTT_EVENT_DISCONNECTED:
        s_connected = false;
        /* Broker will publish the retained LWT "offline" on our behalf. */
        ESP_LOGW(TAG, "disconnected from broker (client keeps retrying)");
        break;

    case MQTT_EVENT_ERROR:
        if (event->error_handle != NULL) {
            ESP_LOGE(TAG, "MQTT error: type=0x%x connect_rc=0x%x",
                     event->error_handle->error_type,
                     event->error_handle->connect_return_code);
        } else {
            ESP_LOGE(TAG, "MQTT error (no details)");
        }
        break;

    case MQTT_EVENT_PUBLISHED:
        ESP_LOGD(TAG, "published msg_id=%d", event->msg_id);
        break;

    case MQTT_EVENT_DATA: {
        /*
         * The command subscription is handled in this same MQTT event task:
         * relay_set() is not internally locked, so commands must stay on a
         * single task (the sensor task never touches relays).
         */
        if (event->topic == NULL || event->data == NULL ||
            event->topic_len <= 0 || event->data_len <= 0) {
            ESP_LOGW(TAG, "ignoring empty MQTT data event");
            break;
        }
        /* Tiny command payloads are never fragmented; refuse in case. */
        if (event->data_len != event->total_data_len ||
            event->current_data_offset != 0) {
            ESP_LOGW(TAG, "ignoring fragmented relay command (len %d/%d at %d)",
                     event->data_len, event->total_data_len,
                     event->current_data_offset);
            break;
        }
        relay_handle_command(event->topic, event->topic_len,
                             event->data, event->data_len);
        break;
    }

    default:
        ESP_LOGD(TAG, "event id=%" PRIi32, event_id);
        break;
    }
}

esp_err_t mqtt_app_init(const mqtt_app_config_t *cfg)
{
    if (cfg == NULL || cfg->broker_uri == NULL || cfg->broker_uri[0] == '\0' ||
        cfg->topic_prefix == NULL || cfg->topic_prefix[0] == '\0' ||
        cfg->board_id == NULL || cfg->board_id[0] == '\0') {
        ESP_LOGE(TAG, "invalid mqtt_app_config: "
                      "broker_uri/topic_prefix/board_id are mandatory");
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = ESP_OK;
    if (err == ESP_OK) {
        /* M19: accept scheme-less "host:port" (e.g. what the mobile app
         * sends over BLE) by prepending "mqtt://" at the single consumer;
         * a full URI passes through verbatim. */
        err = mqtt_app_normalize_broker_uri(cfg->broker_uri, s_broker_uri,
                                            sizeof(s_broker_uri));
        if (err == ESP_OK && strstr(cfg->broker_uri, "://") == NULL) {
            ESP_LOGI(TAG, "broker URI had no scheme — normalized to %s",
                     s_broker_uri);
        }
    }
    if (err == ESP_OK) {
        err = fmt_bounded(s_username, sizeof(s_username), "%s",
                          cfg->username != NULL ? cfg->username : "");
    }
    if (err == ESP_OK) {
        err = fmt_bounded(s_password, sizeof(s_password), "%s",
                          cfg->password != NULL ? cfg->password : "");
    }
    if (err == ESP_OK) {
        err = fmt_bounded(s_topic_prefix, sizeof(s_topic_prefix), "%s",
                          cfg->topic_prefix);
    }
    if (err == ESP_OK) {
        err = fmt_bounded(s_board_id, sizeof(s_board_id), "%s", cfg->board_id);
    }
    if (err == ESP_OK) {
        err = fmt_bounded(s_display_name, sizeof(s_display_name), "%s",
                          cfg->display_name != NULL ? cfg->display_name : "");
    }

    /* Board topic tree: {prefix}/boards/{boardId}/... */
    if (err == ESP_OK) {
        err = fmt_bounded(s_board_base, sizeof(s_board_base), "%s/boards/%s",
                          s_topic_prefix, s_board_id);
    }
    if (err == ESP_OK) {
        err = fmt_bounded(s_descriptor_topic, sizeof(s_descriptor_topic),
                          "%s/descriptor", s_board_base);
    }
    if (err == ESP_OK) {
        err = fmt_bounded(s_telemetry_topic, sizeof(s_telemetry_topic),
                          "%s/telemetry", s_board_base);
    }
    if (err == ESP_OK) {
        err = fmt_bounded(s_status_topic, sizeof(s_status_topic),
                          "%s/status", s_board_base);
    }
    if (err == ESP_OK) {
        err = fmt_bounded(s_relays_prefix, sizeof(s_relays_prefix),
                          "%s/relays/", s_board_base);
    }
    if (err == ESP_OK) {
        err = fmt_bounded(s_relay_cmd_pattern, sizeof(s_relay_cmd_pattern),
                          "%s/relays/+/set", s_board_base);
    }
    if (err != ESP_OK) {
        return err;
    }

    const int outbox_limit =
        cfg->outbox_limit > 0 ? cfg->outbox_limit : CONFIG_MQTT_OUTBOX_LIMIT;

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker = {
            .address = {
                .uri = s_broker_uri,
            },
        },
        .credentials = {
            .client_id = s_board_id,
        },
        .session = {
            .last_will = {
                .topic = s_status_topic,
                .msg = OFFLINE_PAYLOAD,
                .msg_len = (int)strlen(OFFLINE_PAYLOAD),
                .qos = 1,
                .retain = 1,
            },
        },
        /* Cap the outbox so a downed broker cannot exhaust RAM. */
        .outbox = {
            .limit = (uint64_t)outbox_limit,
        },
    };
    /* Only attach credentials when configured (empty strings disable auth). */
    if (s_username[0] != '\0') {
        mqtt_cfg.credentials.username = s_username;
        if (s_password[0] != '\0') {
            mqtt_cfg.credentials.authentication.password = s_password;
        }
    }

    s_client = esp_mqtt_client_init(&mqtt_cfg);
    if (s_client == NULL) {
        ESP_LOGE(TAG, "esp_mqtt_client_init failed");
        return ESP_FAIL;
    }

    err = esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID,
                                         mqtt_event_handler, NULL);
    if (err != ESP_OK) {
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
        return err;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "mqtt_app initialized: board=%s prefix=%s outbox_limit=%d",
             s_board_id, s_topic_prefix, outbox_limit);
    ESP_LOGD(TAG, "broker=%s base=%s", s_broker_uri, s_board_base);
    ESP_LOGD(TAG, "topics: descriptor=%s telemetry=%s status=%s cmd=%s",
             s_descriptor_topic, s_telemetry_topic, s_status_topic,
             s_relay_cmd_pattern);
    return ESP_OK;
}

esp_err_t mqtt_app_start(void)
{
    if (!s_initialized || s_client == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_mqtt_client_start(s_client);
}

void mqtt_app_stop(void)
{
    if (s_client == NULL) {
        return;   /* never initialized / already stopped — safe no-op */
    }
    s_connected = false;
    /* IDF v6 esp-mqtt: stop before destroy is the correct order for a
     * client that has been started. A stop failure must not leak the
     * client — destroy regardless (the caller reboots right after). */
    (void)esp_mqtt_client_stop(s_client);
    (void)esp_mqtt_client_destroy(s_client);
    s_client = NULL;
    s_initialized = false;
    ESP_LOGW(TAG, "mqtt client stopped before NVS reboot");
}

bool mqtt_app_is_connected(void)
{
    /* Written by the MQTT event task (CONNECTED/DISCONNECTED); a plain
     * bool read is enough here — worst case the caller sees the state
     * one event late. */
    return s_connected;
}

esp_err_t mqtt_app_publish_telemetry(float temp_c, float rh_pct)
{
    if (!s_initialized || s_client == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!isfinite(temp_c) || !isfinite(rh_pct)) {
        ESP_LOGW(TAG, "drop telemetry: non-finite value (t=%f rh=%f)", temp_c, rh_pct);
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_connected) {
        ESP_LOGW(TAG, "drop telemetry: not connected (client buffers nothing here)");
        return ESP_ERR_INVALID_STATE;
    }

    const double values[SENSOR_COUNT] = { round2((double)temp_c),
                                          round2((double)rh_pct) };

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = ESP_OK;
    cJSON *obj = NULL;
    if (cJSON_AddNumberToObject(root, "schemaVersion", 2) == NULL ||
        cJSON_AddStringToObject(root, "boardId", s_board_id) == NULL ||
        (obj = cJSON_AddObjectToObject(root, "values")) == NULL) {
        err = ESP_ERR_NO_MEM;
    }
    for (size_t i = 0; err == ESP_OK && i < SENSOR_COUNT; i++) {
        if (cJSON_AddNumberToObject(obj, s_sensors[i].channel, values[i]) == NULL) {
            err = ESP_ERR_NO_MEM;
        }
    }

    if (err == ESP_OK) {
        char *payload = cJSON_PrintUnformatted(root);
        if (payload == NULL) {
            err = ESP_ERR_NO_MEM;
        } else {
            /* QoS 1, retain = false (ingest path — retained replay would
             * re-ingest stale points after a backend restart). */
            err = publish_text(s_telemetry_topic, payload, 1, false);
            cJSON_free(payload);
        }
    }
    cJSON_Delete(root);
    if (err != ESP_OK) {
        return err;
    }

    /* Per-channel retained state — same data, same cycle (app realtime). */
    for (size_t i = 0; i < SENSOR_COUNT && err == ESP_OK; i++) {
        cJSON *num = cJSON_CreateNumber(values[i]);
        if (num == NULL) {
            err = ESP_ERR_NO_MEM;
            break;
        }
        err = publish_sensor_state(s_sensors[i].channel, num);
        cJSON_Delete(num);
    }
    return err;
}
