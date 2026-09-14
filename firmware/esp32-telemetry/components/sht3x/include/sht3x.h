/*
 * sht3x — SHT30/SHT31 I2C temperature/humidity sensor driver.
 *
 * Uses the new I2C master driver (driver/i2c_master.h); the legacy
 * driver/i2c.h is EOL in ESP-IDF v6 and must not be used.
 *
 * Readout mode: single-shot, high repeatability, no clock stretching
 * (command 0x2400). CRC-8 (poly 0x31, init 0xFF) is verified separately
 * for temperature and humidity.
 *
 * Contract: sht3x_read() NEVER returns NaN/Inf in its out parameters —
 * every failure is reported through the returned esp_err_t.
 */
#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Opaque SHT3x device handle. */
typedef struct sht3x_dev *sht3x_handle_t;

/**
 * @brief Attach an SHT3x device to an existing I2C master bus.
 *
 * The bus itself is created by the caller (i2c_new_master_bus) so that
 * other devices can share it.
 *
 * @param[in]  bus        I2C master bus handle.
 * @param[in]  addr       7-bit device address (0x44 or 0x45).
 * @param[out] out_handle Receives the device handle.
 * @return ESP_OK on success; ESP_ERR_INVALID_ARG on bad arguments.
 */
esp_err_t sht3x_init(i2c_master_bus_handle_t bus, uint8_t addr,
                     sht3x_handle_t *out_handle);

/**
 * @brief Trigger a single-shot measurement and read the result.
 *
 * Blocks for the measurement duration (>= 15 ms, high repeatability).
 * CRC is checked for both words; values outside the physical range
 * (-40..125 °C, 0..100 %RH) or non-finite conversions are rejected.
 *
 * @param[in]  dev       Device handle from sht3x_init().
 * @param[out] out_temp_c Temperature in °C.
 * @param[out] out_rh_pct Relative humidity in %.
 * @return
 *      - ESP_OK on success.
 *      - ESP_ERR_INVALID_ARG: NULL handle/out pointers.
 *      - ESP_ERR_INVALID_CRC: CRC mismatch (temperature or humidity).
 *      - ESP_ERR_INVALID_RESPONSE: value outside the physical range.
 *      - driver errors (timeout/NACK) are passed through.
 */
esp_err_t sht3x_read(sht3x_handle_t dev, float *out_temp_c, float *out_rh_pct);

#ifdef __cplusplus
}
#endif
