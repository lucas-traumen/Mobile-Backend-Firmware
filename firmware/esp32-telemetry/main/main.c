/*
 * esp32-telemetry — composition root.
 *
 * SHT30/SHT31 (I2C) -> MQTT telemetry for the Smart Home backend.
 * See PLAN.md for the decisions behind this firmware.
 */

#include <stdio.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_random.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "hal/gpio_types.h"
#include "driver/i2c_master.h"

#include "sht3x.h"
#include "wifi_conn.h"
#include "mqtt_app.h"
#include "relay.h"

static const char *TAG = "main";

#if CONFIG_SENSOR_FAKE_MODE
/* Demo ranges: always finite and inside the schema limits
 * (T: -40..125 C, RH: 0..100 %). */
#define FAKE_T_MIN 24.0f
#define FAKE_T_MAX 26.0f
#define FAKE_RH_MIN 55.0f
#define FAKE_RH_MAX 65.0f

static void seed_fake_rng(void)
{
    srand(esp_random());
}

static void fake_read(float *out_temp_c, float *out_rh_pct)
{
    const float t_span = FAKE_T_MAX - FAKE_T_MIN;
    const float rh_span = FAKE_RH_MAX - FAKE_RH_MIN;
    *out_temp_c = FAKE_T_MIN + t_span * ((float)rand() / (float)RAND_MAX);
    *out_rh_pct = FAKE_RH_MIN + rh_span * ((float)rand() / (float)RAND_MAX);
}
#endif /* CONFIG_SENSOR_FAKE_MODE */

#if CONFIG_SHT3X_I2C_ADDR_0X45
#define SHT3X_I2C_DEVICE_ADDR 0x45
#else
#define SHT3X_I2C_DEVICE_ADDR 0x44
#endif

#define I2C_PORT_NUM   0
#define I2C_GLITCH_CNT 7

/* Bounded wait for Wi-Fi; after this, MQTT keeps retrying on its own. */
#define WIFI_WAIT_TIMEOUT_MS 60000

#define SENSOR_TASK_STACK_SIZE 4096
#define SENSOR_TASK_PRIORITY   5

#if !CONFIG_SENSOR_FAKE_MODE
static sht3x_handle_t s_sensor;
#endif

static void sensor_task(void *arg)
{
    const TickType_t period = pdMS_TO_TICKS(CONFIG_SENSOR_PERIOD_MS);
    TickType_t last_wake = xTaskGetTickCount();
    float temp_c;
    float rh_pct;
    esp_err_t err;

    for (;;) {
#if CONFIG_SENSOR_FAKE_MODE
        /* Demo mode: no SHT3x hardware required — never publish garbage
         * because fake_read() only yields finite in-range values. */
        fake_read(&temp_c, &rh_pct);
        ESP_LOGI(TAG, "FAKE mode: T=%.2f RH=%.2f", temp_c, rh_pct);
        err = ESP_OK;
#else
        err = sht3x_read(s_sensor, &temp_c, &rh_pct);
        if (err != ESP_OK) {
            /* Sensor errors are skipped — never publish garbage. */
            ESP_LOGW(TAG, "SHT3x read failed (%s) — skipping cycle",
                     esp_err_to_name(err));
        }
#endif /* CONFIG_SENSOR_FAKE_MODE */

        if (err == ESP_OK) {
            err = mqtt_app_publish_telemetry(temp_c, rh_pct);
            if (err != ESP_OK) {
                /* Drop only: the mqtt client retransmits QoS1 by itself. */
                ESP_LOGW(TAG, "telemetry dropped: %s", esp_err_to_name(err));
            } else {
                ESP_LOGI(TAG, "telemetry sent: T=%.2f C, RH=%.2f %%", temp_c, rh_pct);
            }
        }
        vTaskDelayUntil(&last_wake, period);
    }
}

void app_main(void)
{
    /* 0. Relays first: drive every channel OFF before anything else can fail
     *    or stall startup, so the coils are never left floating. */
    ESP_ERROR_CHECK(relay_init());

    /* 1. NVS (required by the Wi-Fi driver). */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* 2. Network stack + default event loop. */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

#if CONFIG_SENSOR_FAKE_MODE
    /* 3. Demo mode: no SHT3x hardware and no I2C bus needed.
     *    Seed the fake-data RNG once at startup. */
    seed_fake_rng();
#else
    /* 3. I2C master bus (100 kHz is set per-device in sht3x) + SHT3x. */
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_PORT_NUM,
        .sda_io_num = (gpio_num_t)CONFIG_SHT3X_I2C_SDA_GPIO,
        .scl_io_num = (gpio_num_t)CONFIG_SHT3X_I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = I2C_GLITCH_CNT,
        .flags = {
            .enable_internal_pullup = true, /* external pull-ups still recommended */
        },
    };
    i2c_master_bus_handle_t bus;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &bus));
    ESP_ERROR_CHECK(sht3x_init(bus, SHT3X_I2C_DEVICE_ADDR, &s_sensor));
#endif /* CONFIG_SENSOR_FAKE_MODE */

    /* 4. Wi-Fi (station, event-driven retry with backoff). */
    ESP_ERROR_CHECK(wifi_conn_init(CONFIG_WIFI_SSID, CONFIG_WIFI_PASSWORD));
    ESP_ERROR_CHECK(wifi_conn_start());

    /* 5. Wait for the network — bounded, never forever. */
    EventBits_t bits = xEventGroupWaitBits(wifi_conn_event_group(),
                                           WIFI_CONNECTED_BIT,
                                           pdFALSE, pdTRUE,
                                           pdMS_TO_TICKS(WIFI_WAIT_TIMEOUT_MS));
    if (!(bits & WIFI_CONNECTED_BIT)) {
        ESP_LOGW(TAG, "Wi-Fi not connected after %d s; "
                      "Wi-Fi and MQTT keep retrying in the background",
                 WIFI_WAIT_TIMEOUT_MS / 1000);
    }

    /* 6. MQTT telemetry client. */
    const mqtt_app_config_t mqtt_cfg = {
        .broker_uri = CONFIG_MQTT_BROKER_URI,
        .username = CONFIG_MQTT_USER,
        .password = CONFIG_MQTT_PASSWORD,
        .device_id = CONFIG_DEVICE_ID,
        .room_id = CONFIG_ROOM_ID,
        .outbox_limit = CONFIG_MQTT_OUTBOX_LIMIT,
    };
    ESP_ERROR_CHECK(mqtt_app_init(&mqtt_cfg));
    ESP_ERROR_CHECK(mqtt_app_start());

    /* 7. Telemetry task. */
    BaseType_t task_ok = xTaskCreate(sensor_task, "sensor_task",
                                     SENSOR_TASK_STACK_SIZE, NULL,
                                     SENSOR_TASK_PRIORITY, NULL);
    if (task_ok != pdPASS) {
        ESP_LOGE(TAG, "failed to create sensor task");
        ESP_ERROR_CHECK(ESP_ERR_NO_MEM);
    }

    ESP_LOGI(TAG, "started: device=%s room=%s broker=%s",
             CONFIG_DEVICE_ID, CONFIG_ROOM_ID, CONFIG_MQTT_BROKER_URI);
}
