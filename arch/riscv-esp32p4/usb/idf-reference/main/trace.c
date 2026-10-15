/* Bounded development trace in the backed-up app scratch sector. */
#include "trace.h"

#include <string.h>

static const uint8_t expected_mac[6] = {
    0x80, 0xf1, 0xb2, 0xd3, 0x3b, 0xa6
};

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void put32(uint8_t *p, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i)
        p[i] = (uint8_t)(value >> (i * 8));
}

size_t hs_trace_record_length(const uint8_t *record, size_t bytes,
                              unsigned expected_sequence)
{
    if (!record || bytes != HS_TRACE_RECORD ||
        memcmp(record, "HST2", 4) != 0 ||
        get32(record + 4) != expected_sequence)
        return 0;

    uint32_t length = get32(record + 8);
    if (!length || length > HS_TRACE_TEXT_MAX || get32(record + 12) != 0 ||
        get32(record + 252) != hs_journal_crc(record, 252))
        return 0;

    for (size_t i = 16 + length; i < 252; ++i)
        if (record[i] != 0xff)
            return 0;
    return length;
}

static bool ranges_overlap(const void *first, size_t first_size,
                           const void *second, size_t second_size)
{
    uintptr_t a = (uintptr_t)first;
    uintptr_t b = (uintptr_t)second;
    if (a <= b)
        return b - a < first_size;
    return a - b < second_size;
}

static int validate_arguments(const struct hs_journal_store *store,
                              const char *text, size_t length,
                              uint8_t *work, size_t work_size)
{
    if (!store || !text || !work || !length ||
        length > HS_TRACE_TEXT_MAX || work_size != HS_JOURNAL_SECTOR ||
        !store->read || !store->erase || !store->write ||
        ranges_overlap(text, length, work, work_size))
        return HS_TRACE_ARG;

    if (store->address != 0x20000u || store->size != 0x800000u ||
        memcmp(store->mac, expected_mac, sizeof(expected_mac)) != 0)
        return HS_TRACE_BOARD;
    if (store->encrypted)
        return HS_TRACE_ENCRYPTED;
    return 0;
}

static int parse_image(const struct hs_journal_store *store)
{
    uint8_t header[24];
    uint8_t segment[8];
    if (!store->read(store->ctx, 0, header, sizeof(header)))
        return HS_TRACE_READ;
    if (header[0] != 0xe9 || !header[1] || header[1] > 16 ||
        header[12] != 18 || header[13] != 0 || header[23] != 1)
        return HS_TRACE_IMAGE;

    uint32_t position = 24;
    for (unsigned i = 0; i < header[1]; ++i) {
        if (position > HS_JOURNAL_OFFSET - sizeof(segment))
            return HS_TRACE_IMAGE;
        if (!store->read(store->ctx, position, segment, sizeof(segment)))
            return HS_TRACE_READ;

        uint32_t length = get32(segment + 4);
        if (length > HS_JOURNAL_OFFSET - position - sizeof(segment))
            return HS_TRACE_IMAGE;
        position += (uint32_t)sizeof(segment) + length;
    }

    /* Keep checksum padding, checksum byte and appended SHA256 below the
     * reserved sector, matching the journal's Espressif image bound. */
    if (((position + 16u) & ~15u) + 32u > HS_JOURNAL_OFFSET)
        return HS_TRACE_IMAGE;
    return 0;
}

static void make_record(uint8_t record[HS_TRACE_RECORD], unsigned sequence,
                        const char *text, size_t length)
{
    memset(record, 0xff, HS_TRACE_RECORD);
    memcpy(record, "HST2", 4);
    put32(record + 4, sequence);
    put32(record + 8, (uint32_t)length);
    put32(record + 12, 0);
    memcpy(record + 16, text, length);
    put32(record + 252, hs_journal_crc(record, 252));
}

int hs_trace_begin(const struct hs_journal_store *store,
                   struct hs_trace_state *state, const char *text,
                   size_t length, uint8_t *work, size_t work_size)
{
    if (state) {
        state->next = 0;
        state->active = false;
    }
    if (!state)
        return HS_TRACE_ARG;

    int result = validate_arguments(store, text, length, work, work_size);
    if (result)
        return result;

    if (!store->read(store->ctx, HS_JOURNAL_OFFSET, work,
                     HS_JOURNAL_SECTOR))
        return HS_TRACE_READ;

    if (hs_trace_record_length(work, HS_TRACE_RECORD, 0) ||
        hs_journal_length(work, HS_JOURNAL_SECTOR))
        return HS_TRACE_PRESERVED;

    /* The retained CRC is a development preimage guard, not authentication. */
    if (hs_journal_crc(work, HS_JOURNAL_SECTOR) != store->preimage_crc)
        return HS_TRACE_PREIMAGE;

    result = parse_image(store);
    if (result)
        return result;

    uint8_t initial[HS_TRACE_RECORD];
    make_record(initial, 0, text, length);
    if (!store->erase(store->ctx, HS_JOURNAL_OFFSET, HS_JOURNAL_SECTOR) ||
        !store->write(store->ctx, HS_JOURNAL_OFFSET, initial,
                      sizeof(initial)))
        return HS_TRACE_UNCERTAIN;

    if (!store->read(store->ctx, HS_JOURNAL_OFFSET, work,
                     sizeof(initial)))
        return HS_TRACE_UNCERTAIN;
    if (memcmp(work, initial, sizeof(initial)) != 0 ||
        hs_trace_record_length(work, sizeof(initial), 0) != length)
        return HS_TRACE_UNCERTAIN;

    state->next = 1;
    state->active = true;
    return HS_TRACE_WRITTEN;
}

int hs_trace_append(const struct hs_journal_store *store,
                    struct hs_trace_state *state, const char *text,
                    size_t length, uint8_t *work, size_t work_size)
{
    if (!state)
        return HS_TRACE_ARG;
    if (!state->active)
        return HS_TRACE_INACTIVE;
    if (state->next == HS_TRACE_SLOTS) {
        state->active = false;
        return HS_TRACE_FULL;
    }
    if (state->next == 0 || state->next > HS_TRACE_SLOTS) {
        state->active = false;
        return HS_TRACE_INACTIVE;
    }

    int result = validate_arguments(store, text, length, work, work_size);
    if (result) {
        state->active = false;
        return result;
    }

    uint32_t offset = HS_JOURNAL_OFFSET +
                      state->next * HS_TRACE_RECORD;
    if (!store->read(store->ctx, offset, work, HS_TRACE_RECORD)) {
        state->active = false;
        return HS_TRACE_READ;
    }
    for (size_t i = 0; i < HS_TRACE_RECORD; ++i) {
        if (work[i] != 0xff) {
            state->active = false;
            return HS_TRACE_PREIMAGE;
        }
    }

    uint8_t record[HS_TRACE_RECORD];
    make_record(record, state->next, text, length);
    if (!store->write(store->ctx, offset, record, sizeof(record))) {
        state->active = false;
        return HS_TRACE_UNCERTAIN;
    }
    if (!store->read(store->ctx, offset, work, sizeof(record))) {
        state->active = false;
        return HS_TRACE_UNCERTAIN;
    }
    if (memcmp(work, record, sizeof(record)) != 0 ||
        hs_trace_record_length(work, sizeof(record), state->next) != length) {
        state->active = false;
        return HS_TRACE_UNCERTAIN;
    }

    ++state->next;
    return HS_TRACE_WRITTEN;
}

const char *hs_trace_result_name(int result)
{
    switch (result) {
    case HS_TRACE_WRITTEN: return "WRITTEN";
    case HS_TRACE_PRESERVED: return "PRESERVED";
    case HS_TRACE_ARG: return "ARG";
    case HS_TRACE_BOARD: return "BOARD";
    case HS_TRACE_ENCRYPTED: return "ENCRYPTED";
    case HS_TRACE_PREIMAGE: return "PREIMAGE";
    case HS_TRACE_IMAGE: return "IMAGE";
    case HS_TRACE_READ: return "READ";
    case HS_TRACE_UNCERTAIN: return "UNCERTAIN";
    case HS_TRACE_FULL: return "FULL";
    case HS_TRACE_INACTIVE: return "INACTIVE";
    default: return "UNKNOWN";
    }
}
