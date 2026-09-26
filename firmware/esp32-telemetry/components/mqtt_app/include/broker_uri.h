/*
 * broker_uri — pure helper that normalizes the MQTT broker URI (M19).
 *
 * esp-mqtt requires a URI with an explicit scheme ("mqtt://host:port");
 * a bare "host:port" — as the mobile app may send it over BLE — fails
 * to parse inside esp_mqtt_client_init ("Error parse uri = ...").
 * Normalizing at the single consumer (mqtt_app_init) makes every broker
 * source (BLE / NVS / Kconfig) harmless: "host:port" is accepted and
 * silently completed with "mqtt://".
 *
 * Everything here is pure (no ESP-IDF calls, no logging) so the host
 * harness (/tmp/opencode/m19-harness/) compiles the .c verbatim —
 * same pattern as main/board_id.{c,h} (M18).
 */
#pragma once

#include <stddef.h>

#include "esp_err.h"

/*
 * Normalize the broker URI from in into out (NUL-terminated):
 *   - in containing "://"      -> copied verbatim (scheme kept as-is);
 *   - in without "://"         -> "mqtt://" prepended;
 *   - in NULL or empty         -> ESP_ERR_INVALID_ARG;
 *   - out too small            -> ESP_ERR_INVALID_SIZE, NOTHING written
 *                                 to out (a truncated URI would silently
 *                                 point at the wrong host);
 *   - otherwise                -> ESP_OK.
 * Verbatim copy needs strlen(in) + 1 <= out_len; prepending needs
 * strlen(in) + 7 + 1 <= out_len ("mqtt://" is 7 chars + NUL).
 */
esp_err_t mqtt_app_normalize_broker_uri(const char *in, char *out,
                                        size_t out_len);
