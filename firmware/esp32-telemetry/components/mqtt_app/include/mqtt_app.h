/*
 * mqtt_app — firmware v2 publisher + relay command handler on top of esp-mqtt
 * (managed component), speaking the board-centric contract directly
 * (M14b — PLAN.md; descriptor shape chốt M13 + displayName M14).
 *
 * All topics live under the board base "{prefix}/boards/{boardId}" where
 * prefix = CONFIG MQTT_TOPIC_PREFIX and boardId = the configured device id:
 *
 *   {prefix}/boards/{boardId}/descriptor       — board hardware description
 *                                                 (retained, published on every
 *                                                 connect incl. reconnects)
 *   {prefix}/boards/{boardId}/telemetry        — {"schemaVersion":2,...} per
 *                                                 channel values, QoS 1,
 *                                                 retained = no
 *   {prefix}/boards/{boardId}/sensors/{S}/state — plain number (retained),
 *                                                 same data as telemetry,
 *                                                 published in the same cycle
 *   {prefix}/boards/{boardId}/status           — plain "online"/"offline"
 *                                                 (retained, QoS 1); the LWT
 *                                                 is registered on this topic
 *   {prefix}/boards/{boardId}/relays/+/set     — subscribed; payload ON/OFF
 *                                                 (trimmed), channel parsed
 *                                                 from the topic segment
 *   {prefix}/boards/{boardId}/relays/{K}/state — plain ON/OFF (retained)
 *                                                 after every accepted
 *                                                 command + all channels on
 *                                                 every connect
 *
 * Firmware v1 topics (smarthome/{deviceId}/telemetry|status|relay/...) are
 * gone: a v2 board speaks the boards contract only.
 *
 * All relay handling runs inside the MQTT event task: relay_set() is not
 * internally locked, so commands must not be dispatched from another task.
 *
 * Failure policy: esp_mqtt_client_publish() returning -1 (failure) or -2
 * (outbox full) is logged and dropped — no manual retry, the client
 * retransmits unacknowledged QoS1 messages by itself. Malformed relay
 * commands are logged and dropped without publishing.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Configuration supplied by the caller (strings are copied by mqtt_app_init). */
typedef struct {
    const char *broker_uri;   /**< e.g. "mqtt://192.168.1.100:1883" (mandatory) */
    const char *username;     /**< NULL or empty disables auth                  */
    const char *password;     /**< NULL or empty disables auth                  */
    const char *topic_prefix; /**< first segment of board topics (mandatory)    */
    const char *board_id;     /**< boardId in topics + client id (mandatory)    */
    const char *display_name; /**< NULL/empty -> no "displayName" in descriptor */
    int outbox_limit;         /**< outbox cap in bytes; <=0 -> CONFIG default   */
} mqtt_app_config_t;

/**
 * @brief Create and configure the MQTT client (does not connect).
 * @return ESP_OK, ESP_ERR_INVALID_ARG (missing mandatory fields) or driver error.
 */
esp_err_t mqtt_app_init(const mqtt_app_config_t *cfg);

/** @brief Start the client; connection attempts happen in the background. */
esp_err_t mqtt_app_start(void);

/**
 * @brief Stop and destroy the MQTT client (M23).
 *
 * Stops a started client, then destroys it and resets the module state
 * (client handle, connected flag, initialized flag) so a later
 * mqtt_app_init() can run cleanly. Used by the M22 NVS boot check right
 * before the erase-and-reboot on the broker-fail path, so the client is
 * never killed by the reboot while still holding its socket and task.
 *
 * Safe no-op when the client was never created (or already stopped) —
 * callers may invoke it unconditionally. Logs a single "stopped" line;
 * never the credentials or broker URI.
 */
void mqtt_app_stop(void);

/**
 * @brief Whether the client currently holds a broker connection.
 *
 * Set by MQTT_EVENT_CONNECTED, cleared by MQTT_EVENT_DISCONNECTED
 * (event task); readers may see at most one event of staleness. Used by
 * the M22 NVS boot check to bound how long a downed broker may keep the
 * board from provisioning.
 *
 * @return true while MQTT_EVENT_CONNECTED is the last connection event.
 */
bool mqtt_app_is_connected(void);

/**
 * @brief Publish one telemetry sample.
 *
 * Silently refused unless the client is currently connected.
 *
 * @param[in] temp_c Temperature in °C (must be finite, -40..125).
 * @param[in] rh_pct Relative humidity in % (must be finite, 0..100).
 * @return
 *      - ESP_OK — accepted by the client.
 *      - ESP_ERR_INVALID_STATE — not initialized or not connected (dropped).
 *      - ESP_ERR_INVALID_ARG — non-finite values (dropped).
 *      - ESP_ERR_NO_MEM — outbox full (dropped).
 *      - ESP_FAIL — publish failed (dropped).
 */
esp_err_t mqtt_app_publish_telemetry(float temp_c, float rh_pct);

#ifdef __cplusplus
}
#endif
