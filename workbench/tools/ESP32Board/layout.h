/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#ifndef ESP32BOARD_LAYOUT_H
#define ESP32BOARD_LAYOUT_H
#include <stddef.h>
#include <stdint.h>
#define BOARD_LAYOUT_MAX_PARTITIONS 32u
#define BOARD_LAYOUT_TABLE_MAX 4096u
struct board_partition {
    uint32_t offset, size, flags;
    uint8_t type, subtype;
    char label[17];
};
struct board_layout {
    uint32_t flash_bytes;
    size_t count;
    unsigned md5_record_present;
    struct board_partition partitions[BOARD_LAYOUT_MAX_PARTITIONS];
};
/* Imported structural preview only; input integrity is verified by the file
 * adapter against a supplied trusted SHA. MD5 marker is recognized, not
 * independently verified. No live capacity/active slot/write role inference.
 * Entries sorted by offset; output unchanged on failure, must be disjoint. */
int board_layout_parse(const uint8_t *table, size_t bytes, uint32_t flash_bytes,
                       struct board_layout *layout);
int boardtool_load_layout(const char *path, const char *sha256,
    uint32_t flash_bytes, struct board_layout *layout, char *status, size_t capacity);
#endif
