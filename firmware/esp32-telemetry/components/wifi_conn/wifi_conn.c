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
    bool connect;

    portENTER_CRITICAL(&s_lock);
    s_retry_scheduled = false;
    connect = !s_has_ip;
    portEXIT_CRITICAL(&s_lock);

    if (connect) {
        esp_wifi_connect();
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
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
            portENTER_CRITICAL(&s_lock);
            s_has_ip = false;
            bool already_scheduled = s_retry_scheduled;
            s_retry_scheduled = true;
            uint32_t idx = s_retry_idx;
            if (s_retry_idx < RETRY_BACKOFF_COUNT - 1) {
                s_retry_idx++;
            }
            portEXIT_CRITICAL(&s_lock);

            if (s_event_group != NULL) {
                xEventGroupClearBits(s_event_group, WIFI_CONNECTED_BIT);
            }
            ESP_LOGW(TAG, "disconnected; retrying in %" PRIu32 " ms (attempt %" PRIu32 ")",
                     s_retry_backoff_ms[idx], idx + 1);
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

EventGroupHandle_t wifi_conn_event_group(void)
{
    return s_event_group;
}
