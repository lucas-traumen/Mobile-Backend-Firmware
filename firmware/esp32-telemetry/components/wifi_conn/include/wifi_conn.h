/*
 * wifi_conn — Wi-Fi station connection helper.
 *
 * Event-driven (esp_event): STA_START triggers the first connect;
 * STA_DISCONNECTED schedules a retry with exponential backoff
 * (1 s, 2 s, 4 s, 8 s, 16 s, then capped at 30 s) via a one-shot esp_timer
 * so nothing ever blocks the caller. IP_EVENT_STA_GOT_IP sets
 * WIFI_CONNECTED_BIT in the module event group so other tasks can wait
 * (with a timeout) for connectivity.
 */
#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Set once the station has an IP address; cleared on disconnect. */
#define WIFI_CONNECTED_BIT BIT0

/**
 * @brief Prepare the Wi-Fi driver and register event handlers.
 *
 * Does not start the station; call wifi_conn_start() afterwards.
 *
 * @param[in] ssid     SSID (must be non-empty).
 * @param[in] password Password (WPA/WPA2); may be empty for open networks.
 * @return ESP_OK on success; ESP_ERR_INVALID_ARG if ssid is NULL/empty.
 */
esp_err_t wifi_conn_init(const char *ssid, const char *password);

/** @brief Put the radio into station mode and start connecting. */
esp_err_t wifi_conn_start(void);

/** @brief Event group carrying WIFI_CONNECTED_BIT (NULL before init). */
EventGroupHandle_t wifi_conn_event_group(void);

#ifdef __cplusplus
}
#endif
