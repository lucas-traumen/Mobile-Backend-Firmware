/*
 * sht3x — SHT30/SHT31 driver over the new I2C master driver.
 */

#include <math.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "driver/i2c_master.h"

#include "sht3x.h"

static const char *TAG = "sht3x";

/* Single-shot, high repeatability, clock stretching disabled (datasheet 4.13). */
#define SHT3X_CMD_SINGLE_SHOT_HIGH_NO_STRETCH 0x2400

/* Max measurement duration for high repeatability is 15.5 ms. */
#define SHT3X_MEAS_DELAY_MS 16

/* I2C transaction timeout inside the driver. */
#define SHT3X_I2C_TIMEOUT_MS 100

/* Datasheet 4.13 conversion coefficients. */
#define SHT3X_TEMP_LSB      175.0f
#define SHT3X_TEMP_OFFSET   45.0f
#define SHT3X_RH_LSB        100.0f
#define SHT3X_RAW_MAX       65535.0f

/* Physical validation window (mirrors the backend Zod schema). */
#define SHT3X_TEMP_MIN (-40.0f)
#define SHT3X_TEMP_MAX 125.0f
#define SHT3X_RH_MIN   0.0f
#define SHT3X_RH_MAX   100.0f

#define SHT3X_DATA_LEN 6 /* temp_msb, temp_lsb, crc_t, rh_msb, rh_lsb, crc_rh */

struct sht3x_dev {
    i2c_master_dev_handle_t i2c_dev;
    uint8_t addr;
};

/* CRC-8/NRSC-5: poly 0x31 (x^8 + x^5 + x^4 + 1), init 0xFF, MSB first. */
static uint8_t sht3x_crc8(const uint8_t *data, size_t len)
{
    uint8_t crc = 0xFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++) {
            if (crc & 0x80) {
                crc = (uint8_t)((crc << 1) ^ 0x31);
            } else {
                crc = (uint8_t)(crc << 1);
            }
        }
    }
    return crc;
}

esp_err_t sht3x_init(i2c_master_bus_handle_t bus, uint8_t addr,
                     sht3x_handle_t *out_handle)
{
    if (bus == NULL || out_handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (addr != 0x44 && addr != 0x45) {
        ESP_LOGE(TAG, "invalid SHT3x address 0x%02x (expected 0x44 or 0x45)", addr);
        return ESP_ERR_INVALID_ARG;
    }

    struct sht3x_dev *dev = calloc(1, sizeof(*dev));
    if (dev == NULL) {
        return ESP_ERR_NO_MEM;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = 100 * 1000, /* 100 kHz standard mode */
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &dev_cfg, &dev->i2c_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_master_bus_add_device failed: %s", esp_err_to_name(err));
        free(dev);
        return err;
    }
    dev->addr = addr;
    *out_handle = dev;
    ESP_LOGI(TAG, "SHT3x attached at 0x%02x (100 kHz)", addr);
    return ESP_OK;
}

esp_err_t sht3x_read(sht3x_handle_t dev, float *out_temp_c, float *out_rh_pct)
{
    if (dev == NULL || out_temp_c == NULL || out_rh_pct == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const uint8_t cmd[2] = {
        (uint8_t)(SHT3X_CMD_SINGLE_SHOT_HIGH_NO_STRETCH >> 8),
        (uint8_t)(SHT3X_CMD_SINGLE_SHOT_HIGH_NO_STRETCH & 0xFF),
    };

    esp_err_t err = i2c_master_transmit(dev->i2c_dev, cmd, sizeof(cmd),
                                        SHT3X_I2C_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "command transmit failed: %s", esp_err_to_name(err));
        return err;
    }

    /* High repeatability measurement takes up to 15.5 ms. */
    vTaskDelay(pdMS_TO_TICKS(SHT3X_MEAS_DELAY_MS));

    uint8_t data[SHT3X_DATA_LEN];
    err = i2c_master_receive(dev->i2c_dev, data, sizeof(data),
                             SHT3X_I2C_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "data receive failed: %s", esp_err_to_name(err));
        return err;
    }

    const uint8_t crc_temp = sht3x_crc8(&data[0], 2);
    const uint8_t crc_rh = sht3x_crc8(&data[3], 2);
    if (crc_temp != data[2]) {
        ESP_LOGW(TAG, "temperature CRC mismatch (got 0x%02x, want 0x%02x)",
                 data[2], crc_temp);
        return ESP_ERR_INVALID_CRC;
    }
    if (crc_rh != data[5]) {
        ESP_LOGW(TAG, "humidity CRC mismatch (got 0x%02x, want 0x%02x)",
                 data[5], crc_rh);
        return ESP_ERR_INVALID_CRC;
    }

    const uint16_t raw_temp = (uint16_t)(((uint16_t)data[0] << 8) | data[1]);
    const uint16_t raw_rh = (uint16_t)(((uint16_t)data[3] << 8) | data[4]);

    const float temp_c = -SHT3X_TEMP_OFFSET +
                         SHT3X_TEMP_LSB * (float)raw_temp / SHT3X_RAW_MAX;
    const float rh_pct = SHT3X_RH_LSB * (float)raw_rh / SHT3X_RAW_MAX;

    /* Hard contract: never hand out NaN/Inf. */
    if (!isfinite(temp_c) || !isfinite(rh_pct)) {
        ESP_LOGW(TAG, "non-finite conversion (raw_t=%u raw_rh=%u)",
                 raw_temp, raw_rh);
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (temp_c < SHT3X_TEMP_MIN || temp_c > SHT3X_TEMP_MAX) {
        ESP_LOGW(TAG, "temperature %.2f C out of range", temp_c);
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (rh_pct < SHT3X_RH_MIN || rh_pct > SHT3X_RH_MAX) {
        ESP_LOGW(TAG, "humidity %.2f %% out of range", rh_pct);
        return ESP_ERR_INVALID_RESPONSE;
    }

    *out_temp_c = temp_c;
    *out_rh_pct = rh_pct;
    return ESP_OK;
}
