/*
 * broker_uri.c — M19 broker URI normalization (implementation).
 *
 * Pure string helper, free of ESP-IDF calls except the esp_err_t type,
 * so the host harness (/tmp/opencode/m19-harness/) can compile this
 * file verbatim against a stub esp_err.h. No logging here — the caller
 * (mqtt_app_init) decides what to report.
 */
#include "broker_uri.h"

#include <stdbool.h>
#include <string.h>

esp_err_t mqtt_app_normalize_broker_uri(const char *in, char *out,
                                        size_t out_len)
{
    if (in == NULL || in[0] == '\0' || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t in_len = strlen(in);
    const bool has_scheme = strstr(in, "://") != NULL;

    /* Refuse BEFORE touching out: a silently truncated URI would point
     * at the wrong host, which is worse than a clean init failure.
     * Verbatim copy needs in_len + NUL; prepending adds "mqtt://" (7). */
    const size_t need = in_len + (has_scheme ? 1u : 7u + 1u);
    if (need > out_len) {
        return ESP_ERR_INVALID_SIZE;
    }

    if (has_scheme) {
        memcpy(out, in, in_len + 1);
    } else {
        memcpy(out, "mqtt://", 7);
        memcpy(out + 7, in, in_len + 1);
    }
    return ESP_OK;
}
