/*
 * board_id.c — M18 MAC-derived board identity (implementation).
 *
 * Deliberately free of ESP-IDF calls so the host harness
 * (/tmp/opencode/m16-harness/test_board_id.c) can include this file
 * verbatim; the MAC is fetched by the caller (main.c resolve_board_id).
 */
#include "board_id.h"

#define BOARD_ID_LEN 8   /* 4 bytes -> 8 lowercase hex chars */

size_t board_id_from_mac(const uint8_t mac[6], char *out, size_t out_size)
{
    if (mac == NULL || out == NULL || out_size < BOARD_ID_LEN + 1) {
        return 0;   /* guard: nothing written */
    }

    static const char hex_digits[] = "0123456789abcdef";
    for (size_t i = 0; i < BOARD_ID_LEN / 2; i++) {
        const uint8_t b = mac[2 + i];   /* last 4 bytes: indexes 2..5 */
        out[i * 2] = hex_digits[b >> 4];
        out[i * 2 + 1] = hex_digits[b & 0x0f];
    }
    out[BOARD_ID_LEN] = '\0';
    return BOARD_ID_LEN;
}
