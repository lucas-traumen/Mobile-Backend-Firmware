/*
 * mqtt_app — implementation.
 */

#include <ctype.h>
#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "sdkconfig.h"
#include "cJSON.h"
#include "mqtt_client.h"

#include "mqtt_app.h"
#include "relay.h"

static const char *TAG = "mqtt_app";

#define BROKER_URI_MAX 128
#define CRED_MAX       64
#define ID_MAX         64

/* "smarthome/" (10) + id + longest suffix "/relay/+/set" (12) + NUL */
#define TOPIC_MAX (ID_MAX + 22)
/* Worst-case payload with 64-byte ids stays well below this. */
#define PAYLOAD_MAX 256

#define STATUS_TOPIC_FMT    "smarthome/%s/status"
#define TELEMETRY_TOPIC_FMT "smarthome/%s/telemetry"
#define ONLINE_PAYLOAD      "online"
#define OFFLINE_PAYLOAD     "offline"

/*
 * Relay topics, all sharing one prefix so extra channels never change the
 * protocol:
 *   smarthome/{deviceId}/relay/set          JSON command  (subscribed)
 *   smarthome/{deviceId}/relay/+/set        per-channel    (subscribed)
 *   smarthome/{deviceId}/relay/state        retained state (published)
 *
 * Topic matching is anchored on the prefix "smarthome/{deviceId}/relay/";
 * the JSON topic is matched EXACTLY before the per-channel pattern because
 * ".../relay/set" also starts with the prefix and ends in "/set". The two
 * subscriptions do not overlap: the broker's "+"/"#" wildcards match whole
 * path segments, so "smarthome/{id}/relay/+/set" requires exactly one
 * non-empty segment between "relay" and "set" and therefore never matches
 * ".../relay/set" (which has no segment in that position).
 */
#define RELAY_PREFIX_FMT      "smarthome/%s/relay/"
#define RELAY_SET_TOPIC_FMT   "smarthome/%s/relay/set"
#define RELAY_CMD_TOPIC_FMT   "smarthome/%s/relay/+/set"
#define RELAY_STATE_TOPIC_FMT "smarthome/%s/relay/state"

/* Per-channel payloads are "ON"/"OFF" after trimming; anything longer is bad. */
#define RELAY_CMD_MAX 8

static char s_broker_uri[BROKER_URI_MAX];
static char s_username[CRED_MAX];
static char s_password[CRED_MAX];
static char s_device_id[ID_MAX];
static char s_room_id[ID_MAX];
static char s_status_topic[TOPIC_MAX];
static char s_telemetry_topic[TOPIC_MAX];
static char s_relay_prefix[TOPIC_MAX];
static char s_relay_set_topic[TOPIC_MAX];
static char s_relay_state_topic[TOPIC_MAX];
static char s_relay_cmd_pattern[TOPIC_MAX];

static esp_mqtt_client_handle_t s_client;
static bool s_connected;
static bool s_initialized;

/* Event topics are NOT NUL-terminated: always compare with the length. */
static bool topic_equals(const char *topic, int topic_len, const char *expected)
{
    return (size_t)topic_len == strlen(expected) &&
           memcmp(topic, expected, (size_t)topic_len) == 0;
}

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
 * Build the retained state payload from relay_get_all(): the channel ids and
 * count come from the relay table, so adding K4..Kn needs no change here.
 */
static esp_err_t relay_publish_state(void)
{
    if (!s_initialized || s_client == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    size_t count = 0;
    const relay_info_t *channels = relay_get_all(&count);

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (cJSON_AddNumberToObject(root, "schemaVersion", 1) == NULL) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    for (size_t i = 0; i < count; i++) {
        if (cJSON_AddStringToObject(root, channels[i].id,
                                    channels[i].state ? "ON" : "OFF") == NULL) {
            cJSON_Delete(root);
            return ESP_ERR_NO_MEM;
        }
    }

    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (payload == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /* QoS 1, retain = true (last known state survives a broker restart). */
    const int len = (int)strlen(payload);
    const int msg_id = esp_mqtt_client_publish(s_client, s_relay_state_topic,
                                               payload, len, 1, 1);
    cJSON_free(payload);
    if (msg_id == -2) {
        ESP_LOGW(TAG, "relay state: outbox full (limit reached)");
        return ESP_ERR_NO_MEM;
    }
    if (msg_id < 0) {
        ESP_LOGW(TAG, "relay state: publish failed (msg_id=%d)", msg_id);
        return ESP_FAIL;
    }
    ESP_LOGD(TAG, "relay state queued msg_id=%d", msg_id);
    return ESP_OK;
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
    (void)relay_publish_state();
}

/*
 * JSON command on smarthome/{deviceId}/relay/set:
 *   {"schemaVersion":1,"relay":"K1","state":"ON"}
 * Unknown/extra fields are ignored; a missing or non-1 schemaVersion, a
 * missing relay/state, an unknown channel or a bad state is logged and
 * dropped — never a crash and never a publish.
 */
static void relay_handle_json(const char *data, int data_len)
{
    cJSON *root = cJSON_ParseWithLength(data, (size_t)data_len);
    if (root == NULL) {
        ESP_LOGW(TAG, "relay/set: invalid JSON — ignored");
        return;
    }

    const cJSON *version = cJSON_GetObjectItemCaseSensitive(root, "schemaVersion");
    const cJSON *relay = cJSON_GetObjectItemCaseSensitive(root, "relay");
    const cJSON *state = cJSON_GetObjectItemCaseSensitive(root, "state");

    if (!cJSON_IsNumber(version) || version->valueint != 1) {
        ESP_LOGW(TAG, "relay/set: unsupported schemaVersion — ignored");
        cJSON_Delete(root);
        return;
    }
    if (!cJSON_IsString(relay) || relay->valuestring == NULL ||
        !cJSON_IsString(state) || state->valuestring == NULL) {
        ESP_LOGW(TAG, "relay/set: missing/non-string relay or state — ignored");
        cJSON_Delete(root);
        return;
    }

    bool on = false;
    if (!parse_on_off(state->valuestring, (int)strlen(state->valuestring), &on)) {
        ESP_LOGW(TAG, "relay/set: state \"%s\" is not ON/OFF — ignored",
                 state->valuestring);
        cJSON_Delete(root);
        return;
    }

    relay_apply(relay->valuestring, on);
    cJSON_Delete(root);
}

/*
 * Plain per-channel command on smarthome/{deviceId}/relay/{K}/set with
 * payload ON/OFF, kept for quick mosquitto_pub tests. Routed to the same
 * relay_apply() as the JSON form.
 */
static void relay_handle_plain(const char *topic, int topic_len,
                               const char *data, int data_len)
{
    const size_t prefix_len = strlen(s_relay_prefix);
    if (!topic_starts_with(topic, topic_len, s_relay_prefix)) {
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
    const int json_id = esp_mqtt_client_subscribe(s_client, s_relay_set_topic, 1);
    if (json_id < 0) {
        ESP_LOGW(TAG, "subscribe %s failed (msg_id=%d)", s_relay_set_topic, json_id);
    }
    const int plain_id =
        esp_mqtt_client_subscribe(s_client, s_relay_cmd_pattern, 1);
    if (plain_id < 0) {
        ESP_LOGW(TAG, "subscribe %s failed (msg_id=%d)",
                 s_relay_cmd_pattern, plain_id);
    }
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        s_connected = true;
        esp_mqtt_client_publish(s_client, s_status_topic, ONLINE_PAYLOAD,
                                (int)strlen(ONLINE_PAYLOAD), 1, 1);
        relay_subscribe();
        (void)relay_publish_state();
        ESP_LOGI(TAG, "connected to broker; published retained \"online\" + relay state");
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
         * Both command subscriptions are handled in this same MQTT event
         * task: relay_set() is not internally locked, so commands must stay
         * on a single task (the sensor task never touches relays).
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
        if (topic_equals(event->topic, event->topic_len, s_relay_set_topic)) {
            relay_handle_json(event->data, event->data_len);
        } else {
            relay_handle_plain(event->topic, event->topic_len,
                               event->data, event->data_len);
        }
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
        cfg->device_id == NULL || cfg->device_id[0] == '\0' ||
        cfg->room_id == NULL || cfg->room_id[0] == '\0') {
        ESP_LOGE(TAG, "invalid mqtt_app_config: broker_uri/device_id/room_id are mandatory");
        return ESP_ERR_INVALID_ARG;
    }

    snprintf(s_broker_uri, sizeof(s_broker_uri), "%s", cfg->broker_uri);
    snprintf(s_username, sizeof(s_username), "%s",
             cfg->username != NULL ? cfg->username : "");
    snprintf(s_password, sizeof(s_password), "%s",
             cfg->password != NULL ? cfg->password : "");
    snprintf(s_device_id, sizeof(s_device_id), "%s", cfg->device_id);
    snprintf(s_room_id, sizeof(s_room_id), "%s", cfg->room_id);
    snprintf(s_status_topic, sizeof(s_status_topic), STATUS_TOPIC_FMT, s_device_id);
    snprintf(s_telemetry_topic, sizeof(s_telemetry_topic), TELEMETRY_TOPIC_FMT,
             s_device_id);

    /* Relay topics: shared prefix + JSON set + per-channel wildcard + state. */
    snprintf(s_relay_prefix, sizeof(s_relay_prefix), RELAY_PREFIX_FMT, s_device_id);
    snprintf(s_relay_set_topic, sizeof(s_relay_set_topic), RELAY_SET_TOPIC_FMT,
             s_device_id);
    snprintf(s_relay_cmd_pattern, sizeof(s_relay_cmd_pattern), RELAY_CMD_TOPIC_FMT,
             s_device_id);
    snprintf(s_relay_state_topic, sizeof(s_relay_state_topic),
             RELAY_STATE_TOPIC_FMT, s_device_id);

    const int outbox_limit =
        cfg->outbox_limit > 0 ? cfg->outbox_limit : CONFIG_MQTT_OUTBOX_LIMIT;

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker = {
            .address = {
                .uri = s_broker_uri,
            },
        },
        .credentials = {
            .client_id = s_device_id,
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

    esp_err_t err = esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID,
                                                   mqtt_event_handler, NULL);
    if (err != ESP_OK) {
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
        return err;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "mqtt_app initialized: device=%s room=%s outbox_limit=%d",
             s_device_id, s_room_id, outbox_limit);
    ESP_LOGD(TAG, "broker=%s status_topic=%s telemetry_topic=%s",
             s_broker_uri, s_status_topic, s_telemetry_topic);
    ESP_LOGD(TAG, "relay topics: set=%s cmd=%s state=%s",
             s_relay_set_topic, s_relay_cmd_pattern, s_relay_state_topic);
    return ESP_OK;
}

esp_err_t mqtt_app_start(void)
{
    if (!s_initialized || s_client == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_mqtt_client_start(s_client);
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

    char payload[PAYLOAD_MAX];
    int len = snprintf(payload, sizeof(payload),
                       "{\"schemaVersion\":1,"
                       "\"deviceId\":\"%s\","
                       "\"roomId\":\"%s\","
                       "\"temperature\":%.2f,"
                       "\"humidity\":%.2f}",
                       s_device_id, s_room_id, temp_c, rh_pct);
    if (len < 0 || len >= (int)sizeof(payload)) {
        ESP_LOGW(TAG, "drop telemetry: payload build failed/truncated");
        return ESP_ERR_INVALID_ARG;
    }

    /* QoS 1, retain=false; -1 = failure, -2 = outbox full. */
    int msg_id = esp_mqtt_client_publish(s_client, s_telemetry_topic, payload,
                                         len, 1, 0);
    if (msg_id == -2) {
        ESP_LOGW(TAG, "drop telemetry: outbox full (limit reached)");
        return ESP_ERR_NO_MEM;
    }
    if (msg_id < 0) {
        ESP_LOGW(TAG, "drop telemetry: publish failed (msg_id=%d)", msg_id);
        return ESP_FAIL;
    }
    ESP_LOGD(TAG, "telemetry queued msg_id=%d: %s", msg_id, payload);
    return ESP_OK;
}
