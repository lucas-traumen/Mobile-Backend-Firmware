/*
 * wifi_conn — implementation.
 */

#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "wifi_conn.h"

static const char *TAG = "wifi_conn";

/* Retry backoff in ms; last entry is the cap (all later retries use it). */
static const uint32_t s_retry_backoff_ms[] = {1000, 2000, 4000, 8000, 16000, 30000};
#define RETRY_BACKOFF_COUNT (sizeof(s_retry_backoff_ms) / sizeof(s_retry_backoff_ms[0]))

#define SSID_MAX_LEN 33
#define PASS_MAX_LEN 65

static EventGroupHandle_t s_event_group;
static esp_timer_handle_t s_retry_timer;
static char s_ssid[SSID_MAX_LEN];
static char s_pass[PASS_MAX_LEN];
static bool s_started;

/* State shared between the event handler (esp_event task) and the retry
 * timer callback (esp_timer task); guarded by a spinlock — short sections. */
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_has_ip;
static bool s_retry_scheduled;
static uint32_t s_retry_idx;

/* Last disconnect, read by the main task for BLE provisioning (M16b). */
static wifi_conn_fail_reason_t s_fail_reason = WIFI_CONN_FAIL_NONE;
static int s_fail_code;

/* M20 — apply / retry-suspension state, guarded by s_lock like the rest
 * (volatile semantics via the critical section, same as the flags above):
 *
 *   s_apply_in_progress  wifi_conn_apply_credentials() is mid-teardown (it
 *                        called esp_wifi_disconnect() itself); the handler
 *                        must treat the matching STA_DISCONNECTED as the
 *                        LOCAL abort, not as a verdict on any credentials.
 *   s_apply_settled      that abort's STA_DISCONNECTED has been seen.
 *   s_retries_suspended  one-way latch set by wifi_conn_suspend_retries()
 *                        (after a FAILED report, while waiting for the app
 *                        to resend PROVISION); cleared only by
 *                        wifi_conn_apply_credentials().
 */
static bool s_apply_in_progress;
static bool s_apply_settled;
static bool s_retries_suspended;

/* M22 (opt-in, M23): when true, a disconnect with reason 200/201 (AP
 * provably gone) latches s_retries_suspended instead of backing off.
 * Default false — the Kconfig and BLE provisioning branches keep the
 * normal background retry for every reason; only the M22 NVS boot branch
 * enables this via wifi_conn_stop_retries_when_ap_gone(). */
static bool s_stop_retries_when_ap_gone;

/* M20 tuning: bounded settle wait for the abort event + set_config
 * micro-retry against a residual connect-state race. */
#define APPLY_SETTLE_TIMEOUT_MS   2000
#define APPLY_SETTLE_SLICE_MS     20
#define SET_CONFIG_MICRO_RETRIES  5
#define SET_CONFIG_RETRY_DELAY_MS 100

/*
 * Map an esp_wifi disconnect reason code (esp_wifi_types.h,
 * wifi_err_reason_t) onto the three provisioning failure classes:
 *
 *   BAD_AUTH — the AP rejected the credentials themselves: authentication
 *              or 4-way handshake failures that a corrected password fixes:
 *                2 AUTH_EXPIRE, 14 MIC_FAILURE, 15 4WAY_HANDSHAKE_TIMEOUT,
 *                17 IE_IN_4WAY_DIFFERS, 23 802_1X_AUTH_FAILED,
 *                202 AUTH_FAIL, 204 HANDSHAKE_TIMEOUT
 *
 *   TIMEOUT  — the AP was not reachable in time (a corrected password does
 *              not help; SSID/range/AP state is the problem):
 *               39 TIMEOUT, 200 BEACON_TIMEOUT, 201 NO_AP_FOUND,
 *               203 ASSOC_FAIL, 205 CONNECTION_FAIL,
 *               208 ASSOC_COMEBACK_TIME_TOO_LONG, 209 SA_QUERY_TIMEOUT,
 *               210 NO_AP_FOUND_W_COMPATIBLE_SECURITY,
 *               211 NO_AP_FOUND_IN_AUTHMODE_THRESHOLD,
 *               212 NO_AP_FOUND_IN_RSSI_THRESHOLD
 *
 *   ERROR    — everything else: protocol state errors, AP policy decisions,
 *              and the local deauth reasons (3 AUTH_LEAVE / 8 ASSOC_LEAVE)
 *              the driver emits for our own disconnect inside
 *              wifi_conn_apply_credentials(). Those are always overwritten
 *              by the following attempt's real result before the caller
 *              looks at the classification.
 */
static wifi_conn_fail_reason_t classify_disconnect_reason(uint8_t reason)
{
    switch (reason) {
    /* Credentials rejected by the AP — wrong password / auth failure. */
    case WIFI_REASON_AUTH_EXPIRE:              /* 2   */
    case WIFI_REASON_MIC_FAILURE:              /* 14  */
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:   /* 15  */
    case WIFI_REASON_IE_IN_4WAY_DIFFERS:       /* 17  */
    case WIFI_REASON_802_1X_AUTH_FAILED:       /* 23  */
    case WIFI_REASON_AUTH_FAIL:                /* 202 */
    case WIFI_REASON_HANDSHAKE_TIMEOUT:        /* 204 */
        return WIFI_CONN_FAIL_BAD_AUTH;

    /* AP not reachable in time. */
    case WIFI_REASON_TIMEOUT:                  /* 39  */
    case WIFI_REASON_BEACON_TIMEOUT:           /* 200 */
    case WIFI_REASON_NO_AP_FOUND:              /* 201 */
    case WIFI_REASON_ASSOC_FAIL:               /* 203 */
    case WIFI_REASON_CONNECTION_FAIL:          /* 205 */
    case WIFI_REASON_ASSOC_COMEBACK_TIME_TOO_LONG: /* 208 */
    case WIFI_REASON_SA_QUERY_TIMEOUT:         /* 209 */
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:  /* 210 */
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:  /* 211 */
    case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:      /* 212 */
        return WIFI_CONN_FAIL_TIMEOUT;

    default:
        return WIFI_CONN_FAIL_ERROR;
    }
}

static void schedule_retry(uint32_t delay_ms)
{
    esp_err_t err = esp_timer_start_once(s_retry_timer, (uint64_t)delay_ms * 1000);
    if (err != ESP_OK) {
        /* Should not happen; retry immediately rather than stall forever. */
        ESP_LOGE(TAG, "esp_timer_start_once failed (%s), connecting now",
                 esp_err_to_name(err));
        esp_wifi_connect();
    }
}

static void retry_timer_cb(void *arg)
{
    (void)arg;
    bool connect;

    portENTER_CRITICAL(&s_lock);
    s_retry_scheduled = false;
    /* M20: no reconnect while an apply is tearing the driver down or while
     * retries are suspended — esp_timer_stop() cannot cancel a callback
     * that the esp_timer task has already dequeued, so this is the
     * belt-and-braces on top of the timer stop. */
    connect = !s_has_ip && !s_apply_in_progress && !s_retries_suspended;
    portEXIT_CRITICAL(&s_lock);

    if (connect) {
        esp_wifi_connect();
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    (void)arg;
    if (base == WIFI_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_STA_START:
            portENTER_CRITICAL(&s_lock);
            s_retry_idx = 0;
            s_retry_scheduled = false;
            portEXIT_CRITICAL(&s_lock);
            ESP_LOGI(TAG, "STA started, connecting to \"%s\"", s_ssid);
            esp_wifi_connect();
            break;

        case WIFI_EVENT_STA_DISCONNECTED: {
            const wifi_event_sta_disconnected_t *disc = event_data;
            bool apply_abort;
            bool suspended;
            bool stop_on_ap_gone;

            portENTER_CRITICAL(&s_lock);
            s_has_ip = false;
            apply_abort = s_apply_in_progress;
            if (apply_abort) {
                /* M20: this disconnect is the LOCAL abort our own
                 * esp_wifi_disconnect() inside wifi_conn_apply_credentials()
                 * caused — not a verdict on any credentials. Only mark the
                 * settle wait; never record it as a failure, never count it
                 * into the backoff and never schedule a retry. */
                s_apply_settled = true;
            }
            suspended = s_retries_suspended;
            stop_on_ap_gone = s_stop_retries_when_ap_gone;
            portEXIT_CRITICAL(&s_lock);

            /* Every disconnect clears the connected bit — suppressed ones
             * included: the radio really is not connected. */
            if (s_event_group != NULL) {
                xEventGroupClearBits(s_event_group, WIFI_CONNECTED_BIT);
            }

            if (apply_abort) {
                ESP_LOGW(TAG, "disconnect (reason=%d) suppressed "
                              "(apply in progress)", (int)disc->reason);
                break;
            }
            if (suspended) {
                /* M20: retries are suspended after a FAILED:* report —
                 * wait quietly for the next PROVISION instead of churning
                 * the wrong credentials against the AP. */
                ESP_LOGW(TAG, "disconnect (reason=%d) suppressed "
                              "(retries suspended)", (int)disc->reason);
                break;
            }

            portENTER_CRITICAL(&s_lock);
            s_fail_code = (int)disc->reason;
            s_fail_reason = classify_disconnect_reason(disc->reason);
            portEXIT_CRITICAL(&s_lock);

            if (stop_on_ap_gone &&
                (disc->reason == WIFI_REASON_BEACON_TIMEOUT ||
                 disc->reason == WIFI_REASON_NO_AP_FOUND)) {
                /* M22 (opt-in via wifi_conn_stop_retries_when_ap_gone,
                 * M23): the SSID is provably not on the air (beacon
                 * timeout / no AP found) — backing off 1..30 s cannot
                 * fix a missing AP. Latch the retry loop OFF right here
                 * (same latch wifi_conn_suspend_retries() uses; cleared
                 * only by wifi_conn_apply_credentials()) and stop the
                 * pending timer, so no later timer fire can swallow the
                 * next 201 behind a 30 s gap: the fail code recorded
                 * above stays stable and the boot check (main, M22 NVS
                 * branch) reads it within its poll slice. Without the
                 * opt-in these reasons fall through to the normal
                 * backoff below, like every other reason. */
                portENTER_CRITICAL(&s_lock);
                s_retries_suspended = true;
                s_retry_scheduled = false;
                portEXIT_CRITICAL(&s_lock);
                if (s_retry_timer != NULL) {
                    (void)esp_timer_stop(s_retry_timer);
                }
                ESP_LOGW(TAG, "disconnected (reason=%d)", (int)disc->reason);
                ESP_LOGW(TAG, "AP gone (beacon timeout / no AP found) — "
                              "Wi-Fi retries stopped (M22), fail code "
                              "left for the boot check");
                break;
            }

            portENTER_CRITICAL(&s_lock);
            bool already_scheduled = s_retry_scheduled;
            s_retry_scheduled = true;
            uint32_t idx = s_retry_idx;
            if (s_retry_idx < RETRY_BACKOFF_COUNT - 1) {
                s_retry_idx++;
            }
            portEXIT_CRITICAL(&s_lock);

            ESP_LOGW(TAG, "disconnected (reason=%d); retrying in %" PRIu32 " ms (attempt %" PRIu32 ")",
                     (int)disc->reason, s_retry_backoff_ms[idx], idx + 1);
            if (!already_scheduled && s_retry_timer != NULL) {
                schedule_retry(s_retry_backoff_ms[idx]);
            }
            break;
        }

        default:
            break;
        }
    } else if (base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        portENTER_CRITICAL(&s_lock);
        s_has_ip = true;
        s_retry_idx = 0;
        s_retry_scheduled = false; /* pending timer fire will be a no-op */
        portEXIT_CRITICAL(&s_lock);

        if (s_event_group != NULL) {
            xEventGroupSetBits(s_event_group, WIFI_CONNECTED_BIT);
        }
        ESP_LOGI(TAG, "got IP: " IPSTR, IP2STR(&event->ip_info.ip));
    }
}

esp_err_t wifi_conn_init(const char *ssid, const char *password)
{
    if (ssid == NULL || ssid[0] == '\0') {
        ESP_LOGE(TAG, "CONFIG_WIFI_SSID is empty — configure it via menuconfig");
        return ESP_ERR_INVALID_ARG;
    }

    snprintf(s_ssid, sizeof(s_ssid), "%s", ssid);
    snprintf(s_pass, sizeof(s_pass), "%s", password != NULL ? password : "");

    if (s_event_group == NULL) {
        s_event_group = xEventGroupCreate();
        if (s_event_group == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    /* Returns the default STA netif object; NULL on failure. */
    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
    if (sta_netif == NULL) {
        ESP_LOGE(TAG, "esp_netif_create_default_wifi_sta failed");
        return ESP_FAIL;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_timer_create(&(esp_timer_create_args_t){
        .callback = retry_timer_cb,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "wifi_retry",
    }, &s_retry_timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_timer_create failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                              wifi_event_handler, NULL, NULL);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                              wifi_event_handler, NULL, NULL);
    if (err != ESP_OK) {
        return err;
    }

    ESP_LOGI(TAG, "wifi_conn initialized (ssid=\"%s\")", s_ssid);
    return ESP_OK;
}

esp_err_t wifi_conn_start(void)
{
    if (s_event_group == NULL || s_retry_timer == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_started) {
        return ESP_OK;
    }

    wifi_config_t wifi_config = { 0 };
    strlcpy((char *)wifi_config.sta.ssid, s_ssid, sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, s_pass, sizeof(wifi_config.sta.password));
    /* Accept any auth mode the AP offers; empty password = open network. */
    wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;

    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_wifi_start();
    if (err != ESP_OK) {
        return err;
    }
    s_started = true;
    return ESP_OK;
}

esp_err_t wifi_conn_apply_credentials(const char *ssid, const char *password)
{
    if (ssid == NULL || ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_event_group == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    snprintf(s_ssid, sizeof(s_ssid), "%s", ssid);
    snprintf(s_pass, sizeof(s_pass), "%s", password != NULL ? password : "");

    /* Fresh classification + backoff for the new attempt. Also resume from
     * a suspended state: a new PROVISION is the only path that re-enables
     * connecting after wifi_conn_suspend_retries() (no separate resume API
     * — this is the single resume path in the BLE provisioning flow). */
    portENTER_CRITICAL(&s_lock);
    s_retry_idx = 0;
    s_retry_scheduled = false;
    s_fail_reason = WIFI_CONN_FAIL_NONE;
    s_fail_code = 0;
    s_retries_suspended = false;
    s_apply_in_progress = true;
    s_apply_settled = false;
    portEXIT_CRITICAL(&s_lock);

    if (!s_started) {
        /* Initialized but not started: no driver activity can be in flight
         * (nothing was ever connected nor connecting) and wifi_conn_start()
         * applies the stored credentials — so the apply guard is dropped
         * again right here and the settle flag is left cleared; the code
         * below never runs in this branch. */
        portENTER_CRITICAL(&s_lock);
        s_apply_in_progress = false;
        s_apply_settled = false;
        portEXIT_CRITICAL(&s_lock);
        ESP_LOGI(TAG, "credentials updated (not started): ssid=\"%s\"", s_ssid);
        return ESP_OK;
    }

    /* Cancel any pending retry so it cannot fire mid-teardown
     * (esp_timer_stop is a no-op when nothing is pending). */
    (void)esp_timer_stop(s_retry_timer);

    /*
     * M20 fix — teardown BEFORE reconfiguring. esp_wifi_set_config() is
     * rejected with ESP_ERR_WIFI_STATE while the station is still
     * connecting ("sta is connecting, cannot set config"), so the previous
     * order (set_config first, disconnect afterwards — built on the wrong
     * assumption that set_config stays allowed while started) failed
     * on-target whenever the background retry loop still had an attempt in
     * flight when the app re-sent PROVISION. New order:
     *
     *   1. esp_wifi_disconnect() aborts the in-flight attempt/association:
     *      ESP_OK -> something was in flight; the resulting
     *                STA_DISCONNECTED is the abort event — the handler
     *                suppresses it (s_apply_in_progress) and only marks
     *                s_apply_settled, so classification and backoff stay
     *                untouched by it.
     *      error  -> the driver is already idle; nothing to wait for.
     *   2. Bounded poll (20 ms slices, 2 s cap) for s_apply_settled.
     *      Bounded on purpose: the event can be lost in a mode-transition
     *      race, and apply must never hang the provisioning loop.
     *   3. esp_wifi_set_config() with the new credentials — the driver is
     *      idle now, so "sta is connecting" cannot happen anymore; a
     *      residual ESP_ERR_WIFI_STATE gets up to 5 micro-retries, 100 ms
     *      apart. Still failing -> drop the guard, return the error (the
     *      caller reports FAILED:ERROR — fail-soft as before).
     *   4. esp_wifi_connect() reconnects right away with the new
     *      credentials. If the NEW association then fails, the handler
     *      runs normally (real classification, real backoff) and the
     *      main task's wait_provisioning_ip() early-exits on BAD_AUTH as
     *      before. wifi_conn_start() on the caller side is a no-op once
     *      started.
     */
    esp_err_t derr = esp_wifi_disconnect();
    if (derr == ESP_OK) {
        for (uint32_t waited_ms = 0; waited_ms < APPLY_SETTLE_TIMEOUT_MS;
             waited_ms += APPLY_SETTLE_SLICE_MS) {
            bool settled;
            portENTER_CRITICAL(&s_lock);
            settled = s_apply_settled;
            portEXIT_CRITICAL(&s_lock);
            if (settled) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(APPLY_SETTLE_SLICE_MS));
        }
    }

    wifi_config_t wifi_config = { 0 };
    strlcpy((char *)wifi_config.sta.ssid, s_ssid, sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, s_pass, sizeof(wifi_config.sta.password));
    /* Accept any auth mode the AP offers; empty password = open network. */
    wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;

    esp_err_t err = ESP_ERR_WIFI_STATE;
    for (int attempt = 1; attempt <= SET_CONFIG_MICRO_RETRIES; attempt++) {
        err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
        if (err != ESP_ERR_WIFI_STATE) {
            break;
        }
        if (attempt < SET_CONFIG_MICRO_RETRIES) {
            /* Residual connect-state race: let the driver settle briefly,
             * then push the config again. */
            vTaskDelay(pdMS_TO_TICKS(SET_CONFIG_RETRY_DELAY_MS));
        }
    }
    if (err != ESP_OK) {
        portENTER_CRITICAL(&s_lock);
        s_apply_in_progress = false;
        portEXIT_CRITICAL(&s_lock);
        ESP_LOGE(TAG, "esp_wifi_set_config failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Guard dropped before the reconnect: from here on every
     * STA_DISCONNECTED is a real result of the NEW attempt. */
    portENTER_CRITICAL(&s_lock);
    s_apply_in_progress = false;
    portEXIT_CRITICAL(&s_lock);

    esp_err_t cerr = esp_wifi_connect();
    if (cerr != ESP_OK) {
        /* Best effort: if the reconnect cannot be issued, the station sits
         * idle and wait_provisioning_ip() reports FAILED:ERROR after its
         * 30 s bound — the fail-soft contract still holds. Logged loudly so
         * the serial log explains the slow failure. */
        ESP_LOGE(TAG, "esp_wifi_connect after apply failed: %s",
                 esp_err_to_name(cerr));
    }

    ESP_LOGI(TAG, "credentials re-applied: ssid=\"%s\"", s_ssid);
    return ESP_OK;
}

esp_err_t wifi_conn_suspend_retries(void)
{
    if (s_event_group == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    /* One-way latch: once set, STA_DISCONNECTED events stop being
     * classified/counted/scheduled (handler) and the retry timer callback
     * stops reconnecting. Cleared ONLY by wifi_conn_apply_credentials() —
     * the next PROVISION drives its own reconnect, so no resume API. */
    portENTER_CRITICAL(&s_lock);
    s_retries_suspended = true;
    s_retry_scheduled = false;
    portEXIT_CRITICAL(&s_lock);

    /* Stop the pending retry timer and drop any live association/attempt:
     * the driver-level abort also fires STA_DISCONNECTED, which the
     * handler suppresses (suspended) — no failure is recorded and nothing
     * re-schedules. (esp_timer_stop is a no-op when nothing is pending.) */
    (void)esp_timer_stop(s_retry_timer);
    (void)esp_wifi_disconnect();

    ESP_LOGW(TAG, "retries suspended until the next apply_credentials");
    return ESP_OK;
}

void wifi_conn_stop_retries_when_ap_gone(bool enable)
{
    portENTER_CRITICAL(&s_lock);
    s_stop_retries_when_ap_gone = enable;
    portEXIT_CRITICAL(&s_lock);
    ESP_LOGI(TAG, "stop-retries-when-AP-gone (200/201) %s",
             enable ? "enabled" : "disabled (default)");
}

wifi_conn_fail_reason_t wifi_conn_last_fail_reason(void)
{
    portENTER_CRITICAL(&s_lock);
    const wifi_conn_fail_reason_t reason = s_fail_reason;
    portEXIT_CRITICAL(&s_lock);
    return reason;
}

int wifi_conn_last_fail_code(void)
{
    portENTER_CRITICAL(&s_lock);
    const int code = s_fail_code;
    portEXIT_CRITICAL(&s_lock);
    return code;
}

EventGroupHandle_t wifi_conn_event_group(void)
{
    return s_event_group;
}
