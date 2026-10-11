/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#include "discovery_wire.h"
#include <string.h>

static int disjoint(const void *a, size_t na, const void *b, size_t nb)
{
    uintptr_t pa = (uintptr_t)a, pb = (uintptr_t)b;
    return a && b && na && nb && na <= UINTPTR_MAX - pa &&
        nb <= UINTPTR_MAX - pb && (pa + na <= pb || pb + nb <= pa);
}
static void put32(uint8_t *p, uint32_t value)
{
    unsigned i;
    for (i = 0; i < 4; ++i) p[i] = (uint8_t)(value >> (8u * i));
}
static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
        (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static int zero(const uint8_t *p, size_t n)
{
    size_t i;
    for (i = 0; i < n; ++i) if (p[i]) return 0;
    return 1;
}

int board_discovery_encode(const struct board_discovery_snapshot *snapshot,
    uint32_t nonce, uint8_t *bytes, size_t capacity, size_t *written)
{
    size_t n, i, length;
    if (!nonce || !snapshot || !written || !bytes ||
        !disjoint(snapshot, sizeof(*snapshot), bytes, capacity) ||
        !disjoint(snapshot, sizeof(*snapshot), written, sizeof(*written)) ||
        !disjoint(bytes, capacity, written, sizeof(*written)) ||
        !board_discovery_validate(snapshot)) return 0;
    n = BOARD_DISCOVERY_WIRE_HEADER + snapshot->layout.count * BOARD_DISCOVERY_WIRE_RECORD;
    if (capacity < n) return 0;
    /* All refusal checks precede the first store. */
    memset(bytes, 0, n);
    memcpy(bytes, "C6DI", 4);
    bytes[4] = BOARD_DISCOVERY_WIRE_VERSION;
    bytes[5] = BOARD_DISCOVERY_WIRE_HEADER;
    bytes[6] = BOARD_DISCOVERY_WIRE_RECORD;
    bytes[7] = (uint8_t)snapshot->layout.count;
    bytes[8] = snapshot->running; bytes[9] = snapshot->boot;
    bytes[10] = snapshot->update; bytes[11] = snapshot->running_state;
    bytes[12] = snapshot->security_known; bytes[13] = snapshot->security_enabled;
    bytes[14] = snapshot->target_known;
    put32(bytes + 16, snapshot->physical_bytes);
    put32(bytes + 20, snapshot->layout.flash_bytes);
    put32(bytes + 24, nonce);
    bytes[28] = (uint8_t)snapshot->chip_id;
    bytes[29] = (uint8_t)(snapshot->chip_id >> 8);
    bytes[30] = (uint8_t)snapshot->chip_revision;
    bytes[31] = (uint8_t)(snapshot->chip_revision >> 8);
    put32(bytes + 32, snapshot->secure_version);
    for (i = 0; i < snapshot->layout.count; ++i) {
        const struct board_partition *part = &snapshot->layout.partitions[i];
        uint8_t *record = bytes + BOARD_DISCOVERY_WIRE_HEADER + i * BOARD_DISCOVERY_WIRE_RECORD;
        put32(record, part->offset); put32(record + 4, part->size);
        put32(record + 8, part->flags);
        record[12] = part->type; record[13] = part->subtype;
        length = strlen(part->label); record[14] = (uint8_t)length;
        memcpy(record + 16, part->label, length);
    }
    *written = n;
    return 1;
}

int board_discovery_decode(const uint8_t *bytes, size_t size,
    uint32_t expected_nonce, struct board_discovery_snapshot *snapshot)
{
    struct board_discovery_snapshot parsed = {0};
    size_t i, count, length;
    if (!bytes || !snapshot || !expected_nonce ||
        size < BOARD_DISCOVERY_WIRE_HEADER || size > BOARD_DISCOVERY_WIRE_MAX ||
        !disjoint(bytes, size, snapshot, sizeof(*snapshot)) ||
        memcmp(bytes, "C6DI", 4) || bytes[4] != BOARD_DISCOVERY_WIRE_VERSION ||
        bytes[5] != BOARD_DISCOVERY_WIRE_HEADER || bytes[6] != BOARD_DISCOVERY_WIRE_RECORD ||
        bytes[15] || !zero(bytes + 36, 4) ||
        get32(bytes + 24) != expected_nonce) return 0;
    count = bytes[7];
    if (!count || count > BOARD_LAYOUT_MAX_PARTITIONS ||
        size != BOARD_DISCOVERY_WIRE_HEADER + count * BOARD_DISCOVERY_WIRE_RECORD) return 0;
    parsed.layout.count = count;
    parsed.running = bytes[8]; parsed.boot = bytes[9]; parsed.update = bytes[10];
    parsed.running_state = bytes[11];
    parsed.security_known = bytes[12]; parsed.security_enabled = bytes[13];
    parsed.target_known = bytes[14];
    parsed.chip_id = (uint16_t)((uint16_t)bytes[28] | (uint16_t)bytes[29] << 8);
    parsed.chip_revision = (uint16_t)((uint16_t)bytes[30] | (uint16_t)bytes[31] << 8);
    parsed.secure_version = get32(bytes + 32);
    parsed.physical_bytes = get32(bytes + 16);
    parsed.layout.flash_bytes = get32(bytes + 20);
    for (i = 0; i < count; ++i) {
        const uint8_t *record = bytes + BOARD_DISCOVERY_WIRE_HEADER + i * BOARD_DISCOVERY_WIRE_RECORD;
        struct board_partition *part = &parsed.layout.partitions[i];
        length = record[14];
        if (!length || length > 16u || record[15] ||
            !zero(record + 16 + length, 16u - length) ||
            memchr(record + 16, 0, length)) return 0;
        part->offset = get32(record); part->size = get32(record + 4);
        part->flags = get32(record + 8); part->type = record[12]; part->subtype = record[13];
        memcpy(part->label, record + 16, length);
    }
    if (!board_discovery_validate(&parsed)) return 0;
    *snapshot = parsed;
    return 1;
}
