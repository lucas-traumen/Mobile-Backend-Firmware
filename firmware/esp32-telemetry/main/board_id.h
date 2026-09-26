/*
 * board_id — pure helpers for the M18 MAC-derived board identity.
 *
 * The boardId is the lowercase hex-8 form of the LAST 4 bytes of the
 * Wi-Fi STA MAC (factory-burned eFuse MAC), e.g.
 *
 *   5c:01:3b:6b:af:6c -> "3b6baf6c"
 *
 * The MAC never changes, so the id is stable per board across reboots
 * and reflashes and needs no NVS storage. The MAC is read by the caller
 * (main.c); everything here is pure and host-testable.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

/*
 * Derive the hex-8 boardId from a 6-byte MAC. Pure: no ESP-IDF calls.
 * Writes 8 lowercase hex chars + NUL into out. Returns the length
 * written (always 8), or 0 — writing NOTHING — when the arguments are
 * unusable (NULL) or out_size is smaller than 9 (8 hex chars + NUL).
 */
size_t board_id_from_mac(const uint8_t mac[6], char *out, size_t out_size);
