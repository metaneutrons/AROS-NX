/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#include "layout.h"
#include <string.h>
static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1]<<8 |
        (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24;
}
static int all_ff(const uint8_t *p, size_t n)
{
    size_t i;
    for (i = 0; i < n; ++i) if (p[i] != 0xff) return 0;
    return 1;
}
int board_layout_parse(const uint8_t *table, size_t bytes, uint32_t flash_bytes,
                       struct board_layout *layout)
{
    struct board_layout parsed;
    struct board_partition part;
    size_t pos, i, j;
    unsigned terminated = 0;
    uintptr_t start = (uintptr_t)table, out = (uintptr_t)layout;
    if (!table || !layout || bytes < 64 || bytes > BOARD_LAYOUT_TABLE_MAX ||
        bytes % 32 || flash_bytes < 0x100000u || flash_bytes > 0x8000000u ||
        (flash_bytes & (flash_bytes - 1)) ||
        start > UINTPTR_MAX - bytes || out > UINTPTR_MAX - sizeof(*layout) ||
        (start < out + sizeof(*layout) && out < start + bytes)) return 0;
    memset(&parsed, 0, sizeof(parsed));
    parsed.flash_bytes = flash_bytes;
    for (pos = 0; pos < bytes; pos += 32) {
        const uint8_t *record = table + pos;
        if (record[0] == 0xff && record[1] == 0xff) {
            if (!all_ff(record, bytes - pos)) return 0;
            terminated = 1;
            break;
        }
        if (record[0] == 0xeb && record[1] == 0xeb) {
            if (!parsed.count || parsed.md5_record_present ||
                !all_ff(record + 2, 14)) return 0;
            parsed.md5_record_present = 1;
            /* MD5 is terminal; any records after it must be erased padding. */
            if (!all_ff(record + 32, bytes - pos - 32)) return 0;
            terminated = 1;
            break;
        }
        if (record[0] != 0xaa || record[1] != 0x50 ||
            parsed.count == BOARD_LAYOUT_MAX_PARTITIONS) return 0;
        memset(&part, 0, sizeof(part));
        part.type = record[2];
        part.subtype = record[3];
        part.offset = le32(record + 4);
        part.size = le32(record + 8);
        part.flags = le32(record + 28);
        if (!part.size || part.offset >= flash_bytes ||
            part.size > flash_bytes - part.offset ||
            part.offset % 4096 ||
            (part.type == 0 && (part.offset % 65536 || part.size % 4096))) return 0;
        for (i = 0; i < 16; ++i) {
            if (!record[12 + i]) break;
            if (record[12 + i] < 32 || record[12 + i] > 126) return 0;
            part.label[i] = (char)record[12 + i];
        }
        if (!part.label[0]) return 0;
        for (i = 0; i < parsed.count; ++i) {
            const struct board_partition *prior = &parsed.partitions[i];
            if (!strcmp(part.label, prior->label) ||
                (part.offset < prior->offset + prior->size &&
                 prior->offset < part.offset + part.size)) return 0;
        }
        j = parsed.count;
        while (j && parsed.partitions[j - 1].offset > part.offset) {
            parsed.partitions[j] = parsed.partitions[j - 1];
            --j;
        }
        parsed.partitions[j] = part;
        ++parsed.count;
    }
    if (!terminated || !parsed.count) return 0;
    *layout = parsed;
    return 1;
}
