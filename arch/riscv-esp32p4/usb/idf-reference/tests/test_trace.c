#include "trace.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_APP_SIZE 0x800000u
#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: check failed: %s\n", \
                __FILE__, __LINE__, #condition); \
        return false; \
    } \
} while (0)

struct fake_flash {
    uint8_t *bytes;
    size_t size;
    unsigned reads;
    unsigned erases;
    unsigned writes;
    unsigned fail_read_call;
    unsigned corrupt_read_call;
    unsigned fail_write_call;
    bool partial_write;
    bool fail_erase;
    bool unexpected;
};

static void put32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

static bool valid_read_range(uint32_t offset, size_t length)
{
    if (offset == HS_JOURNAL_OFFSET && length == HS_JOURNAL_SECTOR)
        return true;
    if (offset == 0 && length == 24)
        return true;
    if (offset >= 24 && offset < HS_JOURNAL_OFFSET && length == 8 &&
        (offset - 24) % 8 == 0)
        return true;
    return offset >= HS_JOURNAL_OFFSET &&
           offset < HS_JOURNAL_OFFSET + HS_JOURNAL_SECTOR &&
           (offset - HS_JOURNAL_OFFSET) % HS_TRACE_RECORD == 0 &&
           length == HS_TRACE_RECORD;
}

static bool flash_read(void *ctx, uint32_t offset, void *dst, size_t length)
{
    struct fake_flash *flash = ctx;
    ++flash->reads;
    if (!valid_read_range(offset, length) || (size_t)offset > flash->size ||
        length > flash->size - offset) {
        flash->unexpected = true;
        return false;
    }
    if (flash->reads == flash->fail_read_call)
        return false;
    memcpy(dst, flash->bytes + offset, length);
    if (flash->reads == flash->corrupt_read_call)
        ((uint8_t *)dst)[0] ^= 1;
    return true;
}

static bool flash_erase(void *ctx, uint32_t offset, size_t length)
{
    struct fake_flash *flash = ctx;
    ++flash->erases;
    if (offset != HS_JOURNAL_OFFSET || length != HS_JOURNAL_SECTOR ||
        (size_t)offset > flash->size || length > flash->size - offset) {
        flash->unexpected = true;
        return false;
    }
    if (flash->fail_erase)
        return false;
    memset(flash->bytes + offset, 0xff, length);
    return true;
}

static bool flash_write(void *ctx, uint32_t offset, const void *src,
                        size_t length)
{
    struct fake_flash *flash = ctx;
    ++flash->writes;
    if (offset < HS_JOURNAL_OFFSET ||
        offset >= HS_JOURNAL_OFFSET + HS_JOURNAL_SECTOR ||
        (offset - HS_JOURNAL_OFFSET) % HS_TRACE_RECORD != 0 ||
        length != HS_TRACE_RECORD || (size_t)offset > flash->size ||
        length > flash->size - offset) {
        flash->unexpected = true;
        return false;
    }
    if (flash->writes == flash->fail_write_call) {
        if (flash->partial_write)
            memcpy(flash->bytes + offset, src, length / 2);
        return false;
    }
    memcpy(flash->bytes + offset, src, length);
    return true;
}

struct fixture {
    struct fake_flash flash;
    struct hs_journal_store store;
};

static bool fixture_init(struct fixture *fixture)
{
    memset(fixture, 0, sizeof(*fixture));
    fixture->flash.size = TEST_APP_SIZE;
    fixture->flash.bytes = malloc(fixture->flash.size);
    if (!fixture->flash.bytes)
        return false;
    memset(fixture->flash.bytes, 0xff, fixture->flash.size);
    fixture->store.ctx = &fixture->flash;
    fixture->store.address = 0x20000u;
    fixture->store.size = TEST_APP_SIZE;
    fixture->store.preimage_crc = hs_journal_crc(
        fixture->flash.bytes + HS_JOURNAL_OFFSET, HS_JOURNAL_SECTOR);
    static const uint8_t mac[6] = {0x80, 0xf1, 0xb2, 0xd3, 0x3b, 0xa6};
    memcpy(fixture->store.mac, mac, sizeof(mac));
    fixture->store.read = flash_read;
    fixture->store.erase = flash_erase;
    fixture->store.write = flash_write;
    return true;
}

static void fixture_destroy(struct fixture *fixture)
{
    free(fixture->flash.bytes);
    fixture->flash.bytes = NULL;
}

/* Build one structurally bounded unsigned ESP image whose segment ends at pos. */
static void make_image(struct fake_flash *flash, uint32_t pos)
{
    uint8_t *header = flash->bytes;
    uint8_t *segment = header + 24;
    uint32_t body_length = pos - 32;
    memset(header, 0, 24);
    header[0] = 0xe9;
    header[1] = 1;
    header[12] = 18;
    header[13] = 0;
    header[23] = 1;
    memset(segment, 0, 8);
    put32(segment + 4, body_length);
    memset(segment + 8, 0x5a, body_length);
}

static void make_trace_record(uint8_t *record, unsigned sequence,
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

static void make_old_journal(uint8_t *sector, const char *text, size_t length)
{
    memset(sector, 0xff, HS_JOURNAL_SECTOR);
    memcpy(sector, "HSJ1", 4);
    put32(sector + 4, 1);
    put32(sector + 8, (uint32_t)length);
    put32(sector + 12, hs_journal_crc((const uint8_t *)text, length));
    memcpy(sector + 16, text, length);
}

static int begin_trace(struct fixture *fixture, struct hs_trace_state *state,
                       const char *text, size_t length, uint8_t *work)
{
    return hs_trace_begin(&fixture->store, state, text, length, work,
                          HS_JOURNAL_SECTOR);
}

static int append_trace(struct fixture *fixture, struct hs_trace_state *state,
                        const char *text, size_t length, uint8_t *work)
{
    return hs_trace_append(&fixture->store, state, text, length, work,
                           HS_JOURNAL_SECTOR);
}

static bool test_crc_and_record_validation(void)
{
    static const uint8_t vector[] = "123456789";
    static const char message[] = "bounded trace";
    uint8_t record[HS_TRACE_RECORD];

    CHECK(hs_journal_crc(vector, sizeof(vector) - 1) == 0xcbf43926u);
    make_trace_record(record, 7, message, sizeof(message) - 1);
    CHECK(hs_trace_record_length(record, sizeof(record), 7) ==
          sizeof(message) - 1);
    CHECK(hs_trace_record_length(record, sizeof(record) - 1, 7) == 0);
    CHECK(hs_trace_record_length(record, sizeof(record), 8) == 0);
    CHECK(hs_trace_record_length(NULL, sizeof(record), 7) == 0);

    record[12] = 1;
    CHECK(hs_trace_record_length(record, sizeof(record), 7) == 0);
    make_trace_record(record, 7, message, sizeof(message) - 1);
    record[16 + sizeof(message) - 1] = 0;
    put32(record + 252, hs_journal_crc(record, 252));
    CHECK(hs_trace_record_length(record, sizeof(record), 7) == 0);
    make_trace_record(record, 7, message, sizeof(message) - 1);
    record[20] ^= 1;
    CHECK(hs_trace_record_length(record, sizeof(record), 7) == 0);
    make_trace_record(record, 7, message, sizeof(message) - 1);
    put32(record + 8, HS_TRACE_TEXT_MAX + 1);
    put32(record + 252, hs_journal_crc(record, 252));
    CHECK(hs_trace_record_length(record, sizeof(record), 7) == 0);
    CHECK(strcmp(hs_trace_result_name(HS_TRACE_WRITTEN), "WRITTEN") == 0);
    CHECK(strcmp(hs_trace_result_name(-999), "UNKNOWN") == 0);
    return true;
}

static bool test_begin_append_and_debug_connection(void)
{
    struct fixture fixture;
    struct hs_trace_state state = {99, true};
    uint8_t work[HS_JOURNAL_SECTOR];
    char first[HS_TRACE_TEXT_MAX];
    static const char second[] = "second trace event";
    memset(first, 'A', sizeof(first));
    CHECK(fixture_init(&fixture));
    make_image(&fixture.flash, 64);
    fixture.store.debug_connected = true;

    CHECK(begin_trace(&fixture, &state, first, sizeof(first), work) ==
          HS_TRACE_WRITTEN);
    CHECK(state.active && state.next == 1);
    CHECK(fixture.flash.erases == 1 && fixture.flash.writes == 1);
    CHECK(hs_trace_record_length(fixture.flash.bytes + HS_JOURNAL_OFFSET,
                                HS_TRACE_RECORD, 0) == sizeof(first));
    CHECK(memcmp(fixture.flash.bytes + HS_JOURNAL_OFFSET + 16, first,
                 sizeof(first)) == 0);
    CHECK(fixture.flash.bytes[HS_JOURNAL_OFFSET + 12] == 0);

    CHECK(append_trace(&fixture, &state, second, sizeof(second) - 1, work) ==
          HS_TRACE_WRITTEN);
    CHECK(state.active && state.next == 2);
    CHECK(fixture.flash.erases == 1 && fixture.flash.writes == 2);
    CHECK(hs_trace_record_length(fixture.flash.bytes + HS_JOURNAL_OFFSET +
                                 HS_TRACE_RECORD, HS_TRACE_RECORD, 1) ==
          sizeof(second) - 1);
    CHECK(memcmp(fixture.flash.bytes + HS_JOURNAL_OFFSET + HS_TRACE_RECORD +
                 16, second, sizeof(second) - 1) == 0);
    CHECK(!fixture.flash.unexpected);
    fixture_destroy(&fixture);
    return true;
}

static bool test_valid_new_and_old_records_are_preserved(void)
{
    static const char prior[] = "prior trace";
    static const char current[] = "current run";
    struct hs_trace_state state;
    uint8_t work[HS_JOURNAL_SECTOR];
    struct fixture fixture;

    CHECK(fixture_init(&fixture));
    make_image(&fixture.flash, 64);
    memset(fixture.flash.bytes + HS_JOURNAL_OFFSET, 0xff,
           HS_JOURNAL_SECTOR);
    make_trace_record(fixture.flash.bytes + HS_JOURNAL_OFFSET, 0,
                      prior, sizeof(prior) - 1);
    fixture.store.debug_connected = true;
    CHECK(begin_trace(&fixture, &state, current, sizeof(current) - 1, work) ==
          HS_TRACE_PRESERVED);
    CHECK(!state.active && state.next == 0);
    CHECK(fixture.flash.erases == 0 && fixture.flash.writes == 0);
    CHECK(hs_trace_record_length(work, HS_TRACE_RECORD, 0) ==
          sizeof(prior) - 1);
    fixture_destroy(&fixture);

    CHECK(fixture_init(&fixture));
    make_image(&fixture.flash, 64);
    make_old_journal(fixture.flash.bytes + HS_JOURNAL_OFFSET,
                     prior, sizeof(prior) - 1);
    CHECK(begin_trace(&fixture, &state, current, sizeof(current) - 1, work) ==
          HS_TRACE_PRESERVED);
    CHECK(!state.active && state.next == 0);
    CHECK(fixture.flash.erases == 0 && fixture.flash.writes == 0);
    CHECK(hs_journal_length(work, HS_JOURNAL_SECTOR) == sizeof(prior) - 1);
    fixture_destroy(&fixture);
    return true;
}

static bool test_unknown_or_torn_sector_is_never_erased(void)
{
    struct fixture fixture;
    struct hs_trace_state state = {3, true};
    uint8_t work[HS_JOURNAL_SECTOR];
    static const char text[] = "capture";

    CHECK(fixture_init(&fixture));
    make_image(&fixture.flash, 64);
    memcpy(fixture.flash.bytes + HS_JOURNAL_OFFSET, "HST2", 4);
    put32(fixture.flash.bytes + HS_JOURNAL_OFFSET + 4, 0);
    put32(fixture.flash.bytes + HS_JOURNAL_OFFSET + 8, 100);
    CHECK(begin_trace(&fixture, &state, text, sizeof(text) - 1, work) ==
          HS_TRACE_PREIMAGE);
    CHECK(!state.active && state.next == 0);
    CHECK(fixture.flash.erases == 0 && fixture.flash.writes == 0);
    CHECK(begin_trace(&fixture, &state, text, sizeof(text) - 1, work) ==
          HS_TRACE_PREIMAGE);
    CHECK(fixture.flash.erases == 0 && fixture.flash.writes == 0);
    CHECK(!fixture.flash.unexpected);
    fixture_destroy(&fixture);
    return true;
}

static bool test_argument_board_and_encryption_gates(void)
{
    struct fixture fixture;
    struct hs_trace_state state = {7, true};
    uint8_t work[HS_JOURNAL_SECTOR];
    uint8_t arena[HS_JOURNAL_SECTOR + 32];
    static const char text[] = "capture";
    CHECK(fixture_init(&fixture));
    make_image(&fixture.flash, 64);

    CHECK(hs_trace_begin(NULL, &state, text, sizeof(text) - 1, work,
                         sizeof(work)) == HS_TRACE_ARG);
    CHECK(!state.active && state.next == 0);
    CHECK(hs_trace_begin(&fixture.store, NULL, text, sizeof(text) - 1, work,
                         sizeof(work)) == HS_TRACE_ARG);
    CHECK(hs_trace_begin(&fixture.store, &state, NULL, 1, work,
                         sizeof(work)) == HS_TRACE_ARG);
    CHECK(hs_trace_begin(&fixture.store, &state, text, 0, work,
                         sizeof(work)) == HS_TRACE_ARG);
    CHECK(hs_trace_begin(&fixture.store, &state, text, HS_TRACE_TEXT_MAX + 1,
                         work, sizeof(work)) == HS_TRACE_ARG);
    CHECK(hs_trace_begin(&fixture.store, &state, text, sizeof(text) - 1, NULL,
                         sizeof(work)) == HS_TRACE_ARG);
    CHECK(hs_trace_begin(&fixture.store, &state, text, sizeof(text) - 1, work,
                         sizeof(work) - 1) == HS_TRACE_ARG);
    CHECK(hs_trace_begin(&fixture.store, &state, text, sizeof(text) - 1, work,
                         sizeof(work) + 1) == HS_TRACE_ARG);

    struct hs_journal_store missing = fixture.store;
    missing.read = NULL;
    CHECK(hs_trace_begin(&missing, &state, text, sizeof(text) - 1, work,
                         sizeof(work)) == HS_TRACE_ARG);
    missing = fixture.store;
    missing.erase = NULL;
    CHECK(hs_trace_begin(&missing, &state, text, sizeof(text) - 1, work,
                         sizeof(work)) == HS_TRACE_ARG);
    missing = fixture.store;
    missing.write = NULL;
    CHECK(hs_trace_begin(&missing, &state, text, sizeof(text) - 1, work,
                         sizeof(work)) == HS_TRACE_ARG);

    CHECK(hs_trace_begin(&fixture.store, &state, (char *)arena + 8, 8,
                         arena, HS_JOURNAL_SECTOR) == HS_TRACE_ARG);
    CHECK(hs_trace_begin(&fixture.store, &state, (char *)arena, 64,
                         arena + 32, HS_JOURNAL_SECTOR) == HS_TRACE_ARG);
    CHECK(fixture.flash.reads == 0);

    fixture.store.mac[5] ^= 1;
    CHECK(begin_trace(&fixture, &state, text, sizeof(text) - 1, work) ==
          HS_TRACE_BOARD);
    fixture.store.mac[5] ^= 1;
    ++fixture.store.address;
    CHECK(begin_trace(&fixture, &state, text, sizeof(text) - 1, work) ==
          HS_TRACE_BOARD);
    --fixture.store.address;
    --fixture.store.size;
    CHECK(begin_trace(&fixture, &state, text, sizeof(text) - 1, work) ==
          HS_TRACE_BOARD);
    ++fixture.store.size;
    fixture.store.encrypted = true;
    CHECK(begin_trace(&fixture, &state, text, sizeof(text) - 1, work) ==
          HS_TRACE_ENCRYPTED);
    CHECK(fixture.flash.reads == 0 && fixture.flash.erases == 0 &&
          fixture.flash.writes == 0);
    fixture_destroy(&fixture);
    return true;
}

static bool test_image_header_and_bounds_gates(void)
{
    static const char text[] = "capture";
    uint8_t work[HS_JOURNAL_SECTOR];
    for (unsigned kind = 0; kind < 6; ++kind) {
        struct fixture fixture;
        struct hs_trace_state state;
        CHECK(fixture_init(&fixture));
        make_image(&fixture.flash, 64);
        switch (kind) {
        case 0: fixture.flash.bytes[0] = 0; break;
        case 1: fixture.flash.bytes[1] = 0; break;
        case 2: fixture.flash.bytes[1] = 17; break;
        case 3: fixture.flash.bytes[12] = 19; break;
        case 4: fixture.flash.bytes[13] = 1; break;
        case 5: fixture.flash.bytes[23] = 0; break;
        }
        CHECK(begin_trace(&fixture, &state, text, sizeof(text) - 1, work) ==
              HS_TRACE_IMAGE);
        CHECK(fixture.flash.erases == 0 && fixture.flash.writes == 0);
        CHECK(!fixture.flash.unexpected);
        fixture_destroy(&fixture);
    }

    struct fixture fixture;
    struct hs_trace_state state;
    CHECK(fixture_init(&fixture));
    make_image(&fixture.flash, HS_JOURNAL_OFFSET - 44);
    CHECK(begin_trace(&fixture, &state, text, sizeof(text) - 1, work) ==
          HS_TRACE_WRITTEN);
    CHECK(fixture.flash.erases == 1 && fixture.flash.writes == 1);
    fixture_destroy(&fixture);

    CHECK(fixture_init(&fixture));
    make_image(&fixture.flash, HS_JOURNAL_OFFSET - 28);
    CHECK(begin_trace(&fixture, &state, text, sizeof(text) - 1, work) ==
          HS_TRACE_IMAGE);
    CHECK(fixture.flash.erases == 0 && fixture.flash.writes == 0);
    fixture_destroy(&fixture);

    CHECK(fixture_init(&fixture));
    make_image(&fixture.flash, 64);
    put32(fixture.flash.bytes + 28, HS_JOURNAL_OFFSET);
    CHECK(begin_trace(&fixture, &state, text, sizeof(text) - 1, work) ==
          HS_TRACE_IMAGE);
    CHECK(fixture.flash.erases == 0 && fixture.flash.writes == 0);
    fixture_destroy(&fixture);
    return true;
}

static bool test_begin_read_failures_and_uncertain_operations(void)
{
    static const char text[] = "capture";
    uint8_t work[HS_JOURNAL_SECTOR];
    for (unsigned call = 1; call <= 3; ++call) {
        struct fixture fixture;
        struct hs_trace_state state;
        CHECK(fixture_init(&fixture));
        make_image(&fixture.flash, 64);
        fixture.flash.fail_read_call = call;
        CHECK(begin_trace(&fixture, &state, text, sizeof(text) - 1, work) ==
              HS_TRACE_READ);
        CHECK(fixture.flash.erases == 0 && fixture.flash.writes == 0);
        fixture_destroy(&fixture);
    }

    struct fixture fixture;
    struct hs_trace_state state;
    CHECK(fixture_init(&fixture));
    make_image(&fixture.flash, 64);
    fixture.flash.fail_erase = true;
    CHECK(begin_trace(&fixture, &state, text, sizeof(text) - 1, work) ==
          HS_TRACE_UNCERTAIN);
    CHECK(!state.active && state.next == 0);
    CHECK(fixture.flash.erases == 1 && fixture.flash.writes == 0);
    fixture_destroy(&fixture);

    CHECK(fixture_init(&fixture));
    make_image(&fixture.flash, 64);
    fixture.flash.fail_write_call = 1;
    fixture.flash.partial_write = true;
    CHECK(begin_trace(&fixture, &state, text, sizeof(text) - 1, work) ==
          HS_TRACE_UNCERTAIN);
    CHECK(!state.active && fixture.flash.erases == 1 &&
          fixture.flash.writes == 1);
    CHECK(append_trace(&fixture, &state, text, sizeof(text) - 1, work) ==
          HS_TRACE_INACTIVE);
    CHECK(fixture.flash.erases == 1 && fixture.flash.writes == 1);
    CHECK(begin_trace(&fixture, &state, text, sizeof(text) - 1, work) ==
          HS_TRACE_PREIMAGE);
    CHECK(fixture.flash.erases == 1 && fixture.flash.writes == 1);
    fixture_destroy(&fixture);

    CHECK(fixture_init(&fixture));
    make_image(&fixture.flash, 64);
    fixture.flash.fail_read_call = 4;
    CHECK(begin_trace(&fixture, &state, text, sizeof(text) - 1, work) ==
          HS_TRACE_UNCERTAIN);
    CHECK(fixture.flash.erases == 1 && fixture.flash.writes == 1);
    fixture_destroy(&fixture);

    CHECK(fixture_init(&fixture));
    make_image(&fixture.flash, 64);
    fixture.flash.corrupt_read_call = 4;
    CHECK(begin_trace(&fixture, &state, text, sizeof(text) - 1, work) ==
          HS_TRACE_UNCERTAIN);
    CHECK(fixture.flash.erases == 1 && fixture.flash.writes == 1);
    CHECK(!fixture.flash.unexpected);
    fixture_destroy(&fixture);
    return true;
}

static bool test_append_preimage_and_read_failures_disable_state(void)
{
    static const char text[] = "event";
    struct fixture fixture;
    struct hs_trace_state state;
    uint8_t work[HS_JOURNAL_SECTOR];

    CHECK(fixture_init(&fixture));
    make_image(&fixture.flash, 64);
    CHECK(begin_trace(&fixture, &state, text, sizeof(text) - 1, work) ==
          HS_TRACE_WRITTEN);
    fixture.flash.bytes[HS_JOURNAL_OFFSET + HS_TRACE_RECORD] = 0;
    CHECK(append_trace(&fixture, &state, text, sizeof(text) - 1, work) ==
          HS_TRACE_PREIMAGE);
    CHECK(!state.active && fixture.flash.writes == 1);
    unsigned reads = fixture.flash.reads;
    CHECK(append_trace(&fixture, &state, text, sizeof(text) - 1, work) ==
          HS_TRACE_INACTIVE);
    CHECK(fixture.flash.reads == reads && fixture.flash.writes == 1);
    fixture_destroy(&fixture);

    CHECK(fixture_init(&fixture));
    make_image(&fixture.flash, 64);
    CHECK(begin_trace(&fixture, &state, text, sizeof(text) - 1, work) ==
          HS_TRACE_WRITTEN);
    fixture.flash.fail_read_call = 5;
    CHECK(append_trace(&fixture, &state, text, sizeof(text) - 1, work) ==
          HS_TRACE_READ);
    CHECK(!state.active && fixture.flash.writes == 1);
    reads = fixture.flash.reads;
    CHECK(append_trace(&fixture, &state, text, sizeof(text) - 1, work) ==
          HS_TRACE_INACTIVE);
    CHECK(fixture.flash.reads == reads && fixture.flash.writes == 1);
    fixture_destroy(&fixture);
    return true;
}

static bool test_append_partial_write_and_readback_failures_do_not_retry(void)
{
    static const char text[] = "event";
    struct fixture fixture;
    struct hs_trace_state state;
    uint8_t work[HS_JOURNAL_SECTOR];

    CHECK(fixture_init(&fixture));
    make_image(&fixture.flash, 64);
    CHECK(begin_trace(&fixture, &state, text, sizeof(text) - 1, work) ==
          HS_TRACE_WRITTEN);
    fixture.flash.fail_write_call = 2;
    fixture.flash.partial_write = true;
    CHECK(append_trace(&fixture, &state, text, sizeof(text) - 1, work) ==
          HS_TRACE_UNCERTAIN);
    CHECK(!state.active && fixture.flash.writes == 2);
    unsigned reads = fixture.flash.reads;
    CHECK(append_trace(&fixture, &state, text, sizeof(text) - 1, work) ==
          HS_TRACE_INACTIVE);
    CHECK(fixture.flash.reads == reads && fixture.flash.writes == 2);
    fixture_destroy(&fixture);

    CHECK(fixture_init(&fixture));
    make_image(&fixture.flash, 64);
    CHECK(begin_trace(&fixture, &state, text, sizeof(text) - 1, work) ==
          HS_TRACE_WRITTEN);
    fixture.flash.fail_read_call = 6;
    CHECK(append_trace(&fixture, &state, text, sizeof(text) - 1, work) ==
          HS_TRACE_UNCERTAIN);
    CHECK(!state.active && fixture.flash.writes == 2);
    fixture_destroy(&fixture);

    CHECK(fixture_init(&fixture));
    make_image(&fixture.flash, 64);
    CHECK(begin_trace(&fixture, &state, text, sizeof(text) - 1, work) ==
          HS_TRACE_WRITTEN);
    fixture.flash.corrupt_read_call = 6;
    CHECK(append_trace(&fixture, &state, text, sizeof(text) - 1, work) ==
          HS_TRACE_UNCERTAIN);
    CHECK(!state.active && fixture.flash.writes == 2);
    CHECK(!fixture.flash.unexpected);
    fixture_destroy(&fixture);
    return true;
}

static bool test_full_capacity_and_last_valid_prefix(void)
{
    static const char text[] = "event";
    struct fixture fixture;
    struct hs_trace_state state;
    uint8_t work[HS_JOURNAL_SECTOR];
    CHECK(fixture_init(&fixture));
    make_image(&fixture.flash, 64);
    CHECK(begin_trace(&fixture, &state, text, sizeof(text) - 1, work) ==
          HS_TRACE_WRITTEN);
    for (unsigned sequence = 1; sequence < HS_TRACE_SLOTS; ++sequence) {
        CHECK(append_trace(&fixture, &state, text, sizeof(text) - 1, work) ==
              HS_TRACE_WRITTEN);
        CHECK(state.active && state.next == sequence + 1);
    }
    CHECK(fixture.flash.erases == 1 && fixture.flash.writes == HS_TRACE_SLOTS);
    for (unsigned sequence = 0; sequence < HS_TRACE_SLOTS; ++sequence) {
        const uint8_t *record = fixture.flash.bytes + HS_JOURNAL_OFFSET +
                                sequence * HS_TRACE_RECORD;
        CHECK(hs_trace_record_length(record, HS_TRACE_RECORD, sequence) ==
              sizeof(text) - 1);
    }
    unsigned reads = fixture.flash.reads;
    CHECK(append_trace(&fixture, &state, text, sizeof(text) - 1, work) ==
          HS_TRACE_FULL);
    CHECK(fixture.flash.reads == reads && fixture.flash.writes == HS_TRACE_SLOTS);
    CHECK(!fixture.flash.unexpected);
    fixture_destroy(&fixture);

    CHECK(fixture_init(&fixture));
    make_image(&fixture.flash, 64);
    CHECK(begin_trace(&fixture, &state, text, sizeof(text) - 1, work) ==
          HS_TRACE_WRITTEN);
    for (unsigned sequence = 1; sequence < 4; ++sequence)
        CHECK(append_trace(&fixture, &state, text, sizeof(text) - 1, work) ==
              HS_TRACE_WRITTEN);
    fixture.flash.fail_write_call = 5;
    fixture.flash.partial_write = true;
    CHECK(append_trace(&fixture, &state, text, sizeof(text) - 1, work) ==
          HS_TRACE_UNCERTAIN);
    CHECK(!state.active && fixture.flash.writes == 5);
    for (unsigned sequence = 0; sequence < 4; ++sequence) {
        const uint8_t *record = fixture.flash.bytes + HS_JOURNAL_OFFSET +
                                sequence * HS_TRACE_RECORD;
        CHECK(hs_trace_record_length(record, HS_TRACE_RECORD, sequence) ==
              sizeof(text) - 1);
    }
    CHECK(hs_trace_record_length(fixture.flash.bytes + HS_JOURNAL_OFFSET +
                                 4 * HS_TRACE_RECORD,
                                 HS_TRACE_RECORD, 4) == 0);
    CHECK(fixture.flash.bytes[HS_JOURNAL_OFFSET + 5 * HS_TRACE_RECORD] ==
          0xff);
    CHECK(fixture.flash.erases == 1 && !fixture.flash.unexpected);
    fixture_destroy(&fixture);
    return true;
}

static bool test_append_argument_and_guard_failures_disable_state(void)
{
    static const char text[] = "event";
    struct fixture fixture;
    struct hs_trace_state state;
    uint8_t work[HS_JOURNAL_SECTOR];
    CHECK(fixture_init(&fixture));
    make_image(&fixture.flash, 64);
    CHECK(begin_trace(&fixture, &state, text, sizeof(text) - 1, work) ==
          HS_TRACE_WRITTEN);
    CHECK(hs_trace_append(&fixture.store, &state, text, 0, work,
                          sizeof(work)) == HS_TRACE_ARG);
    CHECK(!state.active && fixture.flash.writes == 1);

    CHECK(begin_trace(&fixture, &state, text, sizeof(text) - 1, work) ==
          HS_TRACE_PRESERVED);
    CHECK(!state.active);
    CHECK(append_trace(&fixture, &state, text, sizeof(text) - 1, work) ==
          HS_TRACE_INACTIVE);
    fixture_destroy(&fixture);
    return true;
}

int main(void)
{
    static const struct {
        const char *name;
        bool (*run)(void);
    } tests[] = {
        {"CRC and record validation", test_crc_and_record_validation},
        {"begin, append, and debug connection", test_begin_append_and_debug_connection},
        {"new and old records preserved", test_valid_new_and_old_records_are_preserved},
        {"unknown or torn sector", test_unknown_or_torn_sector_is_never_erased},
        {"argument, board, and encryption gates", test_argument_board_and_encryption_gates},
        {"image header and bounds gates", test_image_header_and_bounds_gates},
        {"begin read and uncertain operations", test_begin_read_failures_and_uncertain_operations},
        {"append preimage and read failures", test_append_preimage_and_read_failures_disable_state},
        {"append partial write and readback failures", test_append_partial_write_and_readback_failures_do_not_retry},
        {"full capacity and last valid prefix", test_full_capacity_and_last_valid_prefix},
        {"append argument and guard failures", test_append_argument_and_guard_failures_disable_state},
    };
    unsigned passed = 0;
    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); ++i) {
        if (!tests[i].run()) {
            fprintf(stderr, "FAIL: %s\n", tests[i].name);
            return 1;
        }
        ++passed;
        printf("PASS: %s\n", tests[i].name);
    }
    printf("%u trace tests passed\n", passed);
    return 0;
}
