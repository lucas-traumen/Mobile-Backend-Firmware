/*
 * wifi_conn — Wi-Fi station connection helper.
 *
 * Event-driven (esp_event): STA_START triggers the first connect;
 * STA_DISCONNECTED schedules a retry with exponential backoff
 * (1 s, 2 s, 4 s, 8 s, 16 s, then capped at 30 s) via a one-shot esp_timer
 * so nothing ever blocks the caller. IP_EVENT_STA_GOT_IP sets
 * WIFI_CONNECTED_BIT in the module event group so other tasks can wait
 * (with a timeout) for connectivity.
 *
 * M22: a disconnect with reason 200 (beacon timeout) or 201 (no AP found)
 * stops the retry loop instead of backing off (same latch as
 * wifi_conn_suspend_retries — a missing AP cannot be fixed by waiting) —
 * but ONLY after the caller opted in with
 * wifi_conn_stop_retries_when_ap_gone(true). The default after
 * wifi_conn_init() is the opposite: 200/201 keep the normal backoff retry
 * like every other reason (the Kconfig and BLE provisioning branches rely
 * on that; the M22 NVS boot branch is the only opt-in).
 *
 * The normal mode (credentials fixed at init) retries forever with the same
 * backoff — existing boards keep this behavior. For BLE provisioning
 * (M16b) the module additionally remembers why the last attempt failed
 * (wifi_conn_last_fail_reason) and accepts new credentials at runtime
 * (wifi_conn_apply_credentials) so the app can fix e.g. a wrong password
 * and reconnect without a reboot.
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Set once the station has an IP address; cleared on disconnect. */
#define WIFI_CONNECTED_BIT BIT0

/**
 * Three-class classification of the last STA disconnect reason, used by
 * BLE provisioning to map a failed attempt onto the GATT contract's
 * FAILED:* status strings. See classify_disconnect_reason() in wifi_conn.c
 * for the full reason-code mapping.
 */
typedef enum {
    WIFI_CONN_FAIL_NONE = 0, /**< nothing recorded since init / last apply */
    WIFI_CONN_FAIL_BAD_AUTH, /**< AP rejected the credentials (wrong
                               *   password, auth / 4-way handshake failure) */
    WIFI_CONN_FAIL_TIMEOUT,  /**< AP not reachable in time (no AP found,
                               *   beacon/connection failures) */
    WIFI_CONN_FAIL_ERROR,    /**< any other disconnect reason */
} wifi_conn_fail_reason_t;

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

/**
 * @brief Replace the station credentials at runtime (BLE provisioning).
 *
 * Stores the new SSID/password and, when the station is already started,
 * tears the current association/attempt down FIRST (M20: esp_wifi_set_config
 * is rejected with ESP_ERR_WIFI_STATE while the station is still connecting,
 * so the teardown must precede the reconfiguration), waits bounded for the
 * driver to go idle, then pushes the new credentials in
 * (esp_wifi_set_config — no deinit/re-init) and reconnects right away with
 * them (esp_wifi_connect). The failure record and the retry backoff are
 * reset, so wifi_conn_last_fail_reason() only reports failures of the NEW
 * attempt; the STA_DISCONNECTED caused by the teardown itself is suppressed
 * (not classified, not retried).
 *
 * Also the ONLY way out of a wifi_conn_suspend_retries() latch: the next
 * PROVISION clears the latch and drives its own reconnect.
 *
 * When the station is initialized but not started yet, the credentials are
 * just stored; wifi_conn_start() applies them.
 *
 * Safe to call between provisioning attempts; not safe to call
 * concurrently with itself (single caller — the main task).
 *
 * @param[in] ssid     New SSID (must be non-empty).
 * @param[in] password New password; may be empty for open networks.
 * @return ESP_OK on success; ESP_ERR_INVALID_ARG if ssid is NULL/empty;
 *         ESP_ERR_INVALID_STATE when wifi_conn_init() has not run; other
 *         esp_err_t values propagate esp_wifi errors.
 */
esp_err_t wifi_conn_apply_credentials(const char *ssid, const char *password);

/**
 * @brief Stop the background retry loop (M20, BLE provisioning).
 *
 * One-way latch for the "board reported FAILED:* and now waits for the app
 * to resend PROVISION" state: the pending retry timer is stopped, any live
 * association/attempt is dropped, and STA_DISCONNECTED events are no longer
 * classified, counted or retried (the connected bit is still cleared), so
 * wrong credentials cannot churn the radio — log spam and Wi-Fi/BLE
 * coexistence interference — while BLE advertising keeps running.
 *
 * There is no resume API on purpose: wifi_conn_apply_credentials() (the
 * next PROVISION) is the only path that clears the latch and reconnects.
 *
 * @return ESP_OK; ESP_ERR_INVALID_STATE when wifi_conn_init() has not run.
 */
esp_err_t wifi_conn_suspend_retries(void);

/**
 * @brief Opt in/out to stopping the retry loop when the AP is provably
 *        gone (M22 — disconnect reason 200 beacon timeout / 201 no AP
 *        found).
 *
 * Default after wifi_conn_init(): disabled — a 200/201 disconnect keeps
 * the normal 1..30 s background retry like every other reason. When
 * enabled, such a disconnect latches the retry loop OFF (the same one-way
 * latch wifi_conn_suspend_retries() uses; cleared only by
 * wifi_conn_apply_credentials()) and stops the pending retry timer, so a
 * missing AP cannot churn the radio and the recorded fail code stays
 * stable for the caller's boot check.
 *
 * Only the M22 NVS boot branch enables this (before wifi_conn_start());
 * the Kconfig and BLE provisioning branches keep the default so they keep
 * retrying in the background and the BLE flow can still classify and
 * recover from failures as before.
 *
 * @param[in] enable true to stop the retry loop on reason 200/201.
 */
void wifi_conn_stop_retries_when_ap_gone(bool enable);

/**
 * @brief Classification of the last recorded STA disconnect.
 *
 * Cleared by wifi_conn_init()/wifi_conn_apply_credentials(); set from the
 * WIFI_EVENT_STA_DISCONNECTED handler for every disconnect afterwards.
 */
wifi_conn_fail_reason_t wifi_conn_last_fail_reason(void);

/**
 * @brief Raw disconnect reason code (wifi_err_reason_t) behind
 *        wifi_conn_last_fail_reason(); 0 when nothing was recorded. For
 *        diagnostics/logging only.
 */
int wifi_conn_last_fail_code(void);

/** @brief Event group carrying WIFI_CONNECTED_BIT (NULL before init). */
EventGroupHandle_t wifi_conn_event_group(void);

#ifdef __cplusplus
}
#endif
