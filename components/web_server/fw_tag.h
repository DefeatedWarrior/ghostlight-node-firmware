/*
 * Board check for firmware uploads.
 *
 * Every image carries a marker naming the board it was built for:
 * "\x01GLBOARD\x02" + BOARD_ID + "\x03" (web_server.c). An upload is scanned
 * for it as it streams in, so another board's firmware, which would run with
 * the wrong pins, is refused before it is installed. The control bytes make
 * an accidental match practically impossible, and the marker exists once
 * per image: the scanner is handed the running image's own marker as the
 * pattern instead of a second copy of it.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FW_TAG_ID_MAX 32

typedef struct {
    const char *prefix;      /* "\x01GLBOARD\x02" */
    size_t prefix_len;
    size_t matched;          /* prefix bytes matched so far */
    size_t id_len;
    char id[FW_TAG_ID_MAX];  /* NUL-terminated once found */
    bool found;
} fw_tag_scan_t;

void fw_tag_scan_init(fw_tag_scan_t *t, const char *prefix, size_t prefix_len);

/** Feed the next @p n bytes of the image. True once the board ID is found. */
bool fw_tag_scan(fw_tag_scan_t *t, const uint8_t *data, size_t n);
