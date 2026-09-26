/*
 * ble_prov — BLE provisioning over NimBLE (M16a).
 *
 * Boards that have no stored Wi-Fi credentials yet start BLE provisioning:
 * the component advertises as `IoTBoard-{boardId}`, a mobile app scans the
 * board's QR code, connects over GATT and pushes the Wi-Fi + MQTT broker
 * configuration. The GATT contract (UUIDs, flags, status strings) is fixed
 * by the mobile app — every UUID and payload here must match it byte for
 * byte. See the UUID comment block in ble_prov.c for the source table.
 *
 * Division of labour with the upper layer (main.c, wired up in M16b):
 *   - This component owns the BLE stack (NimBLE init/deinit, advertising,
 *     GATT, pairing/bonding) and the `bleprov` NVS namespace.
 *   - It does NOT touch esp_wifi/esp_mqtt: on a valid `PROVISION` command it
 *     snapshots the pending config and calls the on_provision callback; the
 *     upper layer connects Wi-Fi + MQTT and reports the outcome back through
 *     ble_prov_report_result().
 *   - Credentials are persisted to NVS ONLY on a successful connect
 *     (BLE_PROV_RESULT_OK). ble_prov_load() reads them back so the upper
 *     layer can decide "board already has Wi-Fi -> do not start provisioning".
 *   - After a success report the component notifies CONNECTED, waits 30 s and
 *     then shuts NimBLE down (frees RAM for the Wi-Fi + MQTT workload).
 *
 * Requirements on the caller:
 *   - NVS must be initialized (nvs_flash_init) before ble_prov_start().
 *   - BLE and Wi-Fi coexist on the same radio (CONFIG_ESP_COEX_SW_COEXIST_ENABLE).
 *     Start provisioning before (or while) Wi-Fi is down; the coexistence
 *     scheduler arbitrates the shared antenna.
 */
#ifndef BLE_PROV_H
#define BLE_PROV_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Value length limits of the GATT contract, in bytes (att_write values are
 * counted as raw bytes; embedded NULs are rejected).
 */
#define BLE_PROV_SSID_MAX_LEN        32  /**< Wi-Fi SSID, 1..32 bytes        */
#define BLE_PROV_WIFI_PASS_MAX_LEN   63  /**< Wi-Fi pass, 0..63 ("" = open)  */
#define BLE_PROV_BROKER_URI_MIN_LEN   4  /**< broker URI, e.g. mqtt://host   */
#define BLE_PROV_BROKER_URI_MAX_LEN 128  /**< broker URI upper bound         */
#define BLE_PROV_MQTT_USER_MAX_LEN   64  /**< MQTT username, 1..64 bytes     */
#define BLE_PROV_MQTT_PASS_MAX_LEN   64  /**< MQTT password, 0..64 bytes     */

/** NVS namespace holding the provisioned credentials. */
#define BLE_PROV_NVS_NAMESPACE "bleprov"

/** Seconds between the CONNECTED report and the automatic ble_prov_stop(). */
#define BLE_PROV_STOP_DELAY_S 30

/**
 * One provisioned configuration (Wi-Fi + broker), used both for the RAM
 * pending buffer filled over GATT and for the NVS round-trip.
 */
typedef struct {
    char ssid[BLE_PROV_SSID_MAX_LEN + 1];
    char wifi_pass[BLE_PROV_WIFI_PASS_MAX_LEN + 1];  /**< "" = open network */
    char broker_uri[BLE_PROV_BROKER_URI_MAX_LEN + 1];
    char mqtt_user[BLE_PROV_MQTT_USER_MAX_LEN + 1];
    char mqtt_pass[BLE_PROV_MQTT_PASS_MAX_LEN + 1];
} ble_prov_wifi_cfg_t;

/**
 * Provision outcome reported by the upper layer after it tried to connect.
 * The component maps the failure values to the `FAILED:*` status strings of
 * the GATT contract and notifies them (ASCII) on the Status characteristic.
 */
typedef enum {
    BLE_PROV_RESULT_OK = 0,  /**< Wi-Fi connected (got IP): persist NVS,
                              *   notify CONNECTED, arm the stop timer.      */
    BLE_PROV_FAIL_BAD_AUTH,  /**< wrong password / auth reject (FAILED:BAD_AUTH) */
    BLE_PROV_FAIL_TIMEOUT,   /**< AP not reachable in time    (FAILED:TIMEOUT) */
    BLE_PROV_FAIL_ERROR,     /**< any other failure           (FAILED:ERROR)   */
} ble_prov_result_t;

/**
 * Called from the NimBLE host task when a valid `PROVISION` command arrives
 * and the pending config contains an SSID.
 *
 * The component notifies `CONNECTING` right after the callback returns, so
 * the implementation should kick off the Wi-Fi connection (wifi_conn) and
 * return quickly; blocking here stalls the BLE host task.
 *
 * @param cfg      Snapshot of the received configuration (valid until return).
 * @param user_ctx The user_ctx passed to ble_prov_start().
 */
typedef void (*ble_prov_provision_cb_t)(const ble_prov_wifi_cfg_t *cfg,
                                        void *user_ctx);

/** Arguments for ble_prov_start(). */
typedef struct {
    /** Board id (CONFIG_DEVICE_ID): advertised as `IoTBoard-{boardId}` and
     *  reported in the Device Info JSON. Mandatory, non-empty. */
    const char *board_id;
    /** Board type (CONFIG_BOARD_TYPE): reported in the Device Info JSON.
     *  Mandatory, non-empty. */
    const char *board_type;
    /** Provision request callback (mandatory in practice — without it the
     *  received config is acknowledged but nobody connects). */
    ble_prov_provision_cb_t on_provision;
    /** Opaque context handed back to on_provision. */
    void *user_ctx;
} ble_prov_cfg_t;

/**
 * Bring up NimBLE and start advertising the provisioning service.
 *
 * Sequence follows the ESP-IDF v6 NimBLE peripheral example: nimble_port_init,
 * host callbacks + SMP config (Just Works pairing, bonding, Secure
 * Connections), service registration, device name, then the host task.
 * Advertising starts from the host sync callback.
 *
 * Failure handling (M16d): the init chain is stepped and logged ("step n/5
 * ok" lines). A failure after NimBLE was initialized rolls the stack back in
 * reverse order (goto cleanup), so the caller may retry ble_prov_start()
 * after a delay without inheriting half-initialized state.
 *
 * @param cfg Start arguments (copied — caller may release afterwards).
 *
 * @return ESP_OK on success; ESP_ERR_INVALID_ARG for bad arguments;
 *         ESP_ERR_INVALID_STATE when already running; other esp_err_t
 *         values propagate NimBLE/NVS failures.
 */
esp_err_t ble_prov_start(const ble_prov_cfg_t *cfg);

/**
 * Stop advertising, terminate an active connection and shut NimBLE down
 * (nimble_port_stop + nimble_port_deinit), freeing its RAM.
 * Idempotent: returns ESP_OK when provisioning is not running.
 *
 * @return ESP_OK on success (or when already stopped); ESP_FAIL when the
 *         host could not be stopped (its task kept running).
 */
esp_err_t ble_prov_stop(void);

/** @return true when provisioning is currently running (started, not stopped). */
bool ble_prov_is_running(void);

/**
 * Read the provisioned credentials stored by ble_prov_report_result() on a
 * successful connect. The upper layer uses this at boot to decide whether
 * provisioning must start at all (has config -> do not start BLE).
 *
 * @param out_cfg Destination, fully zeroed before filling.
 *
 * @return ESP_OK when all five keys exist in NVS; ESP_ERR_NOT_FOUND when
 *         the namespace or any key is missing; ESP_ERR_INVALID_ARG when
 *         out_cfg is NULL; other esp_err_t values propagate NVS errors.
 */
esp_err_t ble_prov_load(ble_prov_wifi_cfg_t *out_cfg);

/**
 * Erase the provisioned credentials (all five keys of the `bleprov`
 * namespace). Used for BOOT-button recovery: erase, then reboot straight
 * into provisioning. Missing keys are not an error.
 *
 * @return ESP_OK on success (or nothing was stored); other esp_err_t
 *         values propagate NVS errors.
 */
esp_err_t ble_prov_erase(void);

/**
 * Report the outcome of the provisioning connection attempt (called by the
 * upper layer from its own task — never from the NimBLE host task).
 *
 * - BLE_PROV_RESULT_OK: persist the pending config to NVS, notify
 *   CONNECTED and arm the 30 s auto-stop timer.
 * - BLE_PROV_FAIL_*: notify the matching FAILED:* string and go back to
 *   accepting writes + PROVISION (the app may fix values and retry);
 *   advertising keeps running, nothing is persisted.
 *
 * No-op (with a WARN log) when provisioning is not running.
 *
 * @param result Outcome of the connection attempt.
 */
void ble_prov_report_result(ble_prov_result_t result);

#ifdef __cplusplus
}
#endif

#endif /* BLE_PROV_H */
