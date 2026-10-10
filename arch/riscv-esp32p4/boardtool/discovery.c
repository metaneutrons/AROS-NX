/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#include "discovery.h"
#include <string.h>

static int capacity(uint32_t size)
{
    return size >= 0x100000u && size <= 0x8000000u && !(size & (size - 1u));
}

static int role(const struct board_layout *layout, unsigned index, int update)
{
    const struct board_partition *part;
    if (index == BOARD_DISCOVERY_UNKNOWN_ROLE) return 1;
    if (index >= layout->count) return 0;
    part = &layout->partitions[index];
    return part->type == 0 &&
        (!update || (part->subtype >= 0x10u && part->subtype <= 0x1fu));
}

int board_discovery_validate(const struct board_discovery_snapshot *snapshot)
{
    const struct board_layout *layout;
    size_t i, j, length;
    if (!snapshot || !capacity(snapshot->physical_bytes)) return 0;
    layout = &snapshot->layout;
    if (!capacity(layout->flash_bytes) ||
        layout->flash_bytes > snapshot->physical_bytes ||
        !layout->count || layout->count > BOARD_LAYOUT_MAX_PARTITIONS ||
        layout->md5_record_present || snapshot->running_state > 6u ||
        (snapshot->security_known & ~BOARD_DISCOVERY_SECURITY_ALL) ||
        (snapshot->security_enabled & ~snapshot->security_known)) return 0;
    for (i = 0; i < layout->count; ++i) {
        const struct board_partition *part = &layout->partitions[i];
        if ((part->flags & ~(BOARD_DISCOVERY_PART_ENCRYPTED | BOARD_DISCOVERY_PART_READONLY)) ||
            !part->size || part->offset >= layout->flash_bytes ||
            part->size > layout->flash_bytes - part->offset ||
            part->offset % 4096u ||
            (part->type == 0 && (part->offset % 65536u || part->size % 4096u)))
            return 0;
        for (length = 0; length < sizeof(part->label); ++length) {
            unsigned char ch = (unsigned char)part->label[length];
            if (!ch) break;
            if (ch < 32u || ch > 126u) return 0;
        }
        if (!length || length == sizeof(part->label) || length > 16u) return 0;
        if (i && layout->partitions[i - 1].offset +
                     layout->partitions[i - 1].size > part->offset) return 0;
        for (j = 0; j < i; ++j)
            if (!strcmp(layout->partitions[j].label, part->label)) return 0;
    }
    if (!role(layout, snapshot->running, 0) ||
        !role(layout, snapshot->boot, 0) || !role(layout, snapshot->update, 1))
        return 0;
    if (snapshot->running == BOARD_DISCOVERY_UNKNOWN_ROLE &&
        snapshot->running_state != 0u) return 0;
    /* A one-OTA-slot registry can make IDF's next-update API return the
     * running entry. Preserve that detected fact for display; a write gate
     * must reject it, but inventory validity is not update permission. */
    return 1;
}
