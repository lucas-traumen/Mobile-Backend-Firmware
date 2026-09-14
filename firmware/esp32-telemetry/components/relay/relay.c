/*
 * relay — generic GPIO relay channel driver.
 *
 * The channel table is the single source of truth. To add a relay:
 *   1. add a CONFIG_RELAY_K<n>_GPIO entry in main/Kconfig.projbuild,
 *   2. add one row to s_channels below.
 * The API and the MQTT command protocol stay unchanged.
 */

#include "relay.h"

#include <stddef.h>
#include <string.h>

#include "esp_log.h"
#include "driver/gpio.h"

static const char *TAG = "relay";

#define RELAY_ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

/*
 * A bool Kconfig symbol is defined as 1 when "y" and left undefined when "n",
 * so normalise it into a real boolean usable in initialisers.
 */
#if defined(CONFIG_RELAY_ACTIVE_HIGH) && CONFIG_RELAY_ACTIVE_HIGH
#define RELAY_ACTIVE_HIGH true
#else
#define RELAY_ACTIVE_HIGH false
#endif

/* Driven level for a logical state, honouring the board polarity. */
static inline uint32_t level_for(bool on, bool active_high)
{
    return (on == active_high) ? 1U : 0U;
}

/*
 * Channel table. Adding a channel = one row here (+ its Kconfig default).
 * `state` is the runtime part mutated by relay_init()/relay_set().
 */
static relay_info_t s_channels[] = {
    { .id = "K1", .gpio = CONFIG_RELAY_K1_GPIO, .active_high = RELAY_ACTIVE_HIGH, .state = false },
    { .id = "K2", .gpio = CONFIG_RELAY_K2_GPIO, .active_high = RELAY_ACTIVE_HIGH, .state = false },
    { .id = "K3", .gpio = CONFIG_RELAY_K3_GPIO, .active_high = RELAY_ACTIVE_HIGH, .state = false },
};

static relay_info_t *find_channel(const char *id)
{
    if (id == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < RELAY_ARRAY_SIZE(s_channels); i++) {
        if (strcmp(s_channels[i].id, id) == 0) {
            return &s_channels[i];
        }
    }
    return NULL;
}

esp_err_t relay_init(void)
{
    esp_err_t first_err = ESP_OK;
    int configured = 0;

    for (size_t i = 0; i < RELAY_ARRAY_SIZE(s_channels); i++) {
        relay_info_t *ch = &s_channels[i];
        const gpio_num_t pin = (gpio_num_t)ch->gpio;

        /*
         * 1. Pre-load the OFF level into the output latch BEFORE enabling the
         *    pad. The latch holds the level the instant the pin becomes an
         *    output, so the relay cannot pulse ON during init.
         */
        esp_err_t err = gpio_set_level(pin, level_for(false, ch->active_high));
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "%s: preset level on GPIO%d failed (%s)",
                     ch->id, ch->gpio, esp_err_to_name(err));
            if (first_err == ESP_OK) {
                first_err = err;
            }
            continue;
        }

        /* 2. Enable the pin as an output, no pulls, no interrupts. */
        const gpio_config_t cfg = {
            .pin_bit_mask = 1ULL << ch->gpio,
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        err = gpio_config(&cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "%s: GPIO%d config failed (%s)",
                     ch->id, ch->gpio, esp_err_to_name(err));
            if (first_err == ESP_OK) {
                first_err = err;
            }
            continue;
        }

        ch->state = false;
        configured++;

        /* 3. Re-assert OFF now that the pad is driven (belt and braces). */
        const esp_err_t reassert = gpio_set_level(pin, level_for(false, ch->active_high));
        if (reassert != ESP_OK && first_err == ESP_OK) {
            first_err = reassert;
        }
    }

    if (first_err == ESP_OK) {
        ESP_LOGI(TAG, "initialised %d channel(s), all OFF", configured);
    } else {
        ESP_LOGW(TAG, "initialised with errors: %d/%u channel(s) configured",
                 configured, (unsigned)RELAY_ARRAY_SIZE(s_channels));
    }
    return first_err;
}

esp_err_t relay_set(const char *id, bool on)
{
    if (id == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    relay_info_t *ch = find_channel(id);
    if (ch == NULL) {
        ESP_LOGW(TAG, "unknown relay id '%s'", id);
        return ESP_ERR_INVALID_ARG;
    }

    const esp_err_t err = gpio_set_level((gpio_num_t)ch->gpio,
                                         level_for(on, ch->active_high));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s: set GPIO%d failed (%s)",
                 ch->id, ch->gpio, esp_err_to_name(err));
        return err;
    }

    ch->state = on;
    ESP_LOGI(TAG, "%s -> %s", ch->id, on ? "ON" : "OFF");
    return ESP_OK;
}

const relay_info_t *relay_get_all(size_t *out_count)
{
    if (out_count != NULL) {
        *out_count = RELAY_ARRAY_SIZE(s_channels);
    }
    return s_channels;
}
