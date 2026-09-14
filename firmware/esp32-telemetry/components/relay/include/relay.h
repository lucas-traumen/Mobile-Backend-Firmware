/*
 * relay — generic GPIO relay channel driver.
 *
 * Drives an Active High (configurable) relay board from plain GPIOs. The
 * channel table lives in relay.c: adding a channel means adding one row plus
 * a Kconfig entry, the API below never changes.
 *
 * No I2C, no Wi-Fi, no MQTT: this component only touches the GPIO driver.
 *
 * Boot contract: relay_init() drives every channel to OFF as early as
 * possible and pre-loads the output latch before enabling the pad, so a
 * relay cannot glitch ON between reset and the first firmware drive. External
 * pull-downs are still recommended on RTC pins (G32/G33) because pad pulls
 * are not active while the chip is held in reset.
 *
 * Thread-safety: relay_set() is not internally locked; call it from a single
 * task (e.g. the MQTT command handler) or add external locking when several
 * writers appear.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief One relay channel: static wiring plus its runtime state.
 *
 * @c id is the logical channel name understood by the command protocol
 * ("K1"/"K2"/"K3", extensible to "Kn"). @c gpio is the output pin and
 * @c active_high describes the board (true: HIGH = ON, false: LOW = ON).
 * @c state is the last commanded state and is updated by relay_init() and
 * relay_set().
 */
typedef struct {
    const char *id;     /**< Logical channel id, e.g. "K1". */
    int gpio;           /**< Output-capable GPIO driving the channel. */
    bool active_high;   /**< true: HIGH = ON; false: LOW = ON. */
    bool state;         /**< Runtime state: true = ON, false = OFF. */
} relay_info_t;

/**
 * @brief Initialise every channel as an output and force it OFF.
 *
 * Idempotent and safe to call once at startup, before Wi-Fi/MQTT. Loads the
 * OFF level into the output latch before enabling the output driver to avoid
 * a switching glitch. Continues over a failing pin, logging each error.
 *
 * @return ESP_OK if all channels configured; otherwise the first error seen.
 */
esp_err_t relay_init(void);

/**
 * @brief Set one channel by its logical id.
 *
 * @param[in] id  Channel id, e.g. "K1". Must not be NULL.
 * @param[in] on  true to energise, false to release.
 * @return
 *      - ESP_OK on success.
 *      - ESP_ERR_INVALID_ARG: NULL id, or no channel matches @p id.
 */
esp_err_t relay_set(const char *id, bool on);

/**
 * @brief Get the state of every channel.
 *
 * The returned pointer references the internal, read-only-until-next-call
 * table and stays valid for the lifetime of the program.
 *
 * @param[out] out_count Optional; receives the number of channels.
 * @return Pointer to the first channel of the internal table (never NULL).
 */
const relay_info_t *relay_get_all(size_t *out_count);

#ifdef __cplusplus
}
#endif
