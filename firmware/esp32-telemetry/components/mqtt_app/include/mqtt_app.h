/*
 * mqtt_app — telemetry publisher + relay command handler on top of esp-mqtt
 * (managed component).
 *
 * Topics (per PLAN.md):
 *   smarthome/{deviceId}/status       — LWT "offline" (retained, QoS 1) registered
 *                                       at connect; "online" published retained on
 *                                       every successful connect.
 *   smarthome/{deviceId}/telemetry    — {"schemaVersion":1,...} QoS 1, retained=no.
 *   smarthome/{deviceId}/relay/set    — subscribed; JSON command
 *                                       {"schemaVersion":1,"relay":"K1","state":"ON"}.
 *   smarthome/{deviceId}/relay/+/set  — subscribed; per-channel payload ON/OFF.
 *   smarthome/{deviceId}/relay/state  — published retained (QoS 1) after every
 *                                       accepted command and on every connect.
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
    const char *device_id;    /**< topic/client id (mandatory, non-empty)       */
    const char *room_id;      /**< goes into the payload (mandatory, non-empty) */
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
