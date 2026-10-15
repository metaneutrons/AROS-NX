#include "journal.h"

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
    bool fail_erase;
    bool fail_write;
    bool corrupt_readback;
};

static void put32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

static bool flash_read(void *ctx, uint32_t offset, void *dst, size_t length)
{
    struct fake_flash *flash = ctx;
    ++flash->reads;
    if (flash->reads == flash->fail_read_call ||
        (size_t)offset > flash->size || length > flash->size - offset)
        return false;
    if (flash->corrupt_readback && flash->reads == 4 &&
        offset == HS_JOURNAL_OFFSET && length == HS_JOURNAL_SECTOR)
        flash->bytes[offset + 16] ^= 1;
    memcpy(dst, flash->bytes + offset, length);
    return true;
}

static bool flash_erase(void *ctx, uint32_t offset, size_t length)
{
    struct fake_flash *flash = ctx;
    ++flash->erases;
    if (flash->fail_erase || offset != HS_JOURNAL_OFFSET ||
        length != HS_JOURNAL_SECTOR ||
        (size_t)offset > flash->size || length > flash->size - offset)
        return false;
    memset(flash->bytes + offset, 0xff, length);
    return true;
}

static bool flash_write(void *ctx, uint32_t offset, const void *src,
                        size_t length)
{
    struct fake_flash *flash = ctx;
    ++flash->writes;
    if (offset != HS_JOURNAL_OFFSET || length != HS_JOURNAL_SECTOR ||
        (size_t)offset > flash->size || length > flash->size - offset)
        return false;
    if (flash->fail_write) {
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
    fixture->store.mac[0] = 0x80;
    fixture->store.mac[1] = 0xf1;
    fixture->store.mac[2] = 0xb2;
    fixture->store.mac[3] = 0xd3;
    fixture->store.mac[4] = 0x3b;
    fixture->store.mac[5] = 0xa6;
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

/* Build a structurally bounded ESP image with its last segment ending at pos. */
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

static void make_record(uint8_t *sector, const char *text, size_t length)
{
    memset(sector, 0xff, HS_JOURNAL_SECTOR);
    memcpy(sector, "HSJ1", 4);
    put32(sector + 4, 1);
    put32(sector + 8, (uint32_t)length);
    put32(sector + 12, hs_journal_crc((const uint8_t *)text, length));
    memcpy(sector + 16, text, length);
}

static int save(struct fixture *fixture, const char *text, size_t length,
                uint8_t *work)
{
    return hs_journal_save(&fixture->store, text, length, work,
                           HS_JOURNAL_SECTOR);
}

static bool test_crc_and_record_validation(void)
{
    static const uint8_t vector[] = "123456789";
    uint8_t sector[HS_JOURNAL_SECTOR];
    static const char saved[] = "diagnostic snapshot\n";

    CHECK(hs_journal_crc(vector, sizeof(vector) - 1) == 0xcbf43926u);
    make_record(sector, saved, sizeof(saved) - 1);
    CHECK(hs_journal_length(sector, sizeof(sector)) == sizeof(saved) - 1);
    CHECK(hs_journal_length(sector, sizeof(sector) - 1) == 0);
    sector[12] ^= 1;
    CHECK(hs_journal_length(sector, sizeof(sector)) == 0);
    make_record(sector, saved, sizeof(saved) - 1);
    sector[16 + sizeof(saved) - 1] = 0;
    CHECK(hs_journal_length(sector, sizeof(sector)) == 0);
    memset(sector, 0xff, sizeof(sector));
    CHECK(hs_journal_length(sector, sizeof(sector)) == 0);
    return true;
}

static bool test_write_then_preserve(void)
{
    struct fixture fixture;
    uint8_t work[HS_JOURNAL_SECTOR];
    static const char first[] = "first bounded capture\n";
    static const char newer[] = "newer filesystem snapshot\n";
    CHECK(fixture_init(&fixture));
    make_image(&fixture.flash, 64);
    CHECK(save(&fixture, first, sizeof(first) - 1, work) == HS_JOURNAL_WRITTEN);
    CHECK(fixture.flash.erases == 1 && fixture.flash.writes == 1);
    CHECK(hs_journal_length(fixture.flash.bytes + HS_JOURNAL_OFFSET,
                           HS_JOURNAL_SECTOR) == sizeof(first) - 1);
    CHECK(memcmp(fixture.flash.bytes + HS_JOURNAL_OFFSET + 16, first,
                 sizeof(first) - 1) == 0);

    fixture.store.debug_connected = true;
    CHECK(save(&fixture, newer, sizeof(newer) - 1, work) ==
          HS_JOURNAL_PRESERVED);
    CHECK(fixture.flash.erases == 1 && fixture.flash.writes == 1);
    CHECK(memcmp(work + 16, first, sizeof(first) - 1) == 0);
    fixture.store.debug_connected = false;
    CHECK(save(&fixture, newer, sizeof(newer) - 1, work) ==
          HS_JOURNAL_PRESERVED);
    CHECK(fixture.flash.erases == 1 && fixture.flash.writes == 1);
    CHECK(memcmp(work + 16, first, sizeof(first) - 1) == 0);
    fixture_destroy(&fixture);
    return true;
}

static bool test_valid_record_preserved_with_debug_attached(void)
{
    struct fixture fixture;
    uint8_t work[HS_JOURNAL_SECTOR];
    static const char prior[] = "prior run\n";
    static const char current[] = "current run\n";
    CHECK(fixture_init(&fixture));
    make_record(fixture.flash.bytes + HS_JOURNAL_OFFSET,
                prior, sizeof(prior) - 1);
    fixture.store.debug_connected = true;
    CHECK(save(&fixture, current, sizeof(current) - 1, work) ==
          HS_JOURNAL_PRESERVED);
    CHECK(fixture.flash.erases == 0 && fixture.flash.writes == 0);
    CHECK(memcmp(work + 16, prior, sizeof(prior) - 1) == 0);
    fixture_destroy(&fixture);
    return true;
}

static bool test_bad_or_partial_sector_never_overwritten(void)
{
    struct fixture fixture;
    uint8_t work[HS_JOURNAL_SECTOR];
    static const char text[] = "attempted capture\n";
    CHECK(fixture_init(&fixture));
    make_image(&fixture.flash, 64);
    memcpy(fixture.flash.bytes + HS_JOURNAL_OFFSET, "HSJ1", 4);
    put32(fixture.flash.bytes + HS_JOURNAL_OFFSET + 4, 1);
    put32(fixture.flash.bytes + HS_JOURNAL_OFFSET + 8, 200);
    CHECK(save(&fixture, text, sizeof(text) - 1, work) == HS_JOURNAL_REFUSED);
    CHECK(fixture.flash.erases == 0 && fixture.flash.writes == 0);
    fixture_destroy(&fixture);

    CHECK(fixture_init(&fixture));
    make_image(&fixture.flash, 64);
    fixture.flash.bytes[HS_JOURNAL_OFFSET + 31] = 0;
    CHECK(save(&fixture, text, sizeof(text) - 1, work) == HS_JOURNAL_REFUSED);
    CHECK(fixture.flash.erases == 0 && fixture.flash.writes == 0);
    fixture_destroy(&fixture);
    return true;
}

static bool test_board_and_text_gates(void)
{
    struct fixture fixture;
    uint8_t work[HS_JOURNAL_SECTOR];
    static const char text[] = "capture\n";
    uint8_t original_mac[6];
    CHECK(fixture_init(&fixture));
    make_image(&fixture.flash, 64);
    memcpy(original_mac, fixture.store.mac, sizeof(original_mac));

    fixture.store.mac[5] ^= 1;
    CHECK(save(&fixture, text, sizeof(text) - 1, work) == HS_JOURNAL_REFUSED);
    fixture.store.mac[5] ^= 1;
    fixture.store.address++;
    CHECK(save(&fixture, text, sizeof(text) - 1, work) == HS_JOURNAL_REFUSED);
    fixture.store.address--;
    fixture.store.size--;
    CHECK(save(&fixture, text, sizeof(text) - 1, work) == HS_JOURNAL_REFUSED);
    fixture.store.size++;
    fixture.store.encrypted = true;
    CHECK(save(&fixture, text, sizeof(text) - 1, work) == HS_JOURNAL_REFUSED);
    fixture.store.encrypted = false;
    fixture.store.debug_connected = true;
    CHECK(save(&fixture, text, sizeof(text) - 1, work) == HS_JOURNAL_REFUSED);
    fixture.store.debug_connected = false;
    fixture.store.preimage_crc ^= 1;
    CHECK(save(&fixture, text, sizeof(text) - 1, work) == HS_JOURNAL_REFUSED);
    CHECK(fixture.flash.erases == 0 && fixture.flash.writes == 0);
    CHECK(fixture.flash.reads == 2);
    memcpy(fixture.store.mac, original_mac, sizeof(original_mac));
    fixture_destroy(&fixture);
    return true;
}

static bool test_image_headers_rejected(void)
{
    static const char text[] = "capture\n";
    uint8_t work[HS_JOURNAL_SECTOR];
    for (unsigned kind = 0; kind < 5; ++kind) {
        struct fixture fixture;
        CHECK(fixture_init(&fixture));
        make_image(&fixture.flash, 64);
        uint8_t *header = fixture.flash.bytes;
        switch (kind) {
        case 0: header[0] = 0; break;
        case 1: header[1] = 0; break;
        case 2: header[1] = 17; break;
        case 3: header[12] = 19; break;
        case 4: header[23] = 0; break;
        }
        CHECK(save(&fixture, text, sizeof(text) - 1, work) ==
              HS_JOURNAL_REFUSED);
        CHECK(fixture.flash.erases == 0 && fixture.flash.writes == 0);
        fixture_destroy(&fixture);
    }
    return true;
}

static bool test_image_bounds_include_checksum_and_sha(void)
{
    struct fixture fixture;
    uint8_t work[HS_JOURNAL_SECTOR];
    static const char text[] = "capture\n";
    CHECK(fixture_init(&fixture));
    /* The complete padded/checksummed/SHA-appended image ends at the sector. */
    make_image(&fixture.flash, HS_JOURNAL_OFFSET - 44);
    CHECK(save(&fixture, text, sizeof(text) - 1, work) == HS_JOURNAL_WRITTEN);
    CHECK(fixture.flash.erases == 1 && fixture.flash.writes == 1);
    fixture_destroy(&fixture);

    CHECK(fixture_init(&fixture));
    /* Segment data ends earlier, but checksum padding + SHA would overlap. */
    make_image(&fixture.flash, HS_JOURNAL_OFFSET - 28);
    CHECK(save(&fixture, text, sizeof(text) - 1, work) == HS_JOURNAL_REFUSED);
    CHECK(fixture.flash.erases == 0 && fixture.flash.writes == 0);
    fixture_destroy(&fixture);
    return true;
}

static bool test_read_errors(void)
{
    static const char text[] = "capture\n";
    uint8_t work[HS_JOURNAL_SECTOR];
    for (unsigned call = 1; call <= 3; ++call) {
        struct fixture fixture;
        CHECK(fixture_init(&fixture));
        make_image(&fixture.flash, 64);
        fixture.flash.fail_read_call = call;
        CHECK(save(&fixture, text, sizeof(text) - 1, work) ==
              HS_JOURNAL_READ_ERROR);
        CHECK(fixture.flash.erases == 0 && fixture.flash.writes == 0);
        fixture_destroy(&fixture);
    }
    return true;
}

static bool test_uncertain_flash_operations_do_not_retry(void)
{
    static const char text[] = "capture\n";
    uint8_t work[HS_JOURNAL_SECTOR];
    struct fixture fixture;

    CHECK(fixture_init(&fixture));
    make_image(&fixture.flash, 64);
    fixture.flash.fail_erase = true;
    CHECK(save(&fixture, text, sizeof(text) - 1, work) ==
          HS_JOURNAL_WRITE_UNCERTAIN);
    CHECK(fixture.flash.erases == 1 && fixture.flash.writes == 0);
    fixture_destroy(&fixture);

    CHECK(fixture_init(&fixture));
    make_image(&fixture.flash, 64);
    fixture.flash.fail_write = true;
    CHECK(save(&fixture, text, sizeof(text) - 1, work) ==
          HS_JOURNAL_WRITE_UNCERTAIN);
    CHECK(fixture.flash.erases == 1 && fixture.flash.writes == 1);
    fixture_destroy(&fixture);

    CHECK(fixture_init(&fixture));
    make_image(&fixture.flash, 64);
    fixture.flash.fail_read_call = 4;
    CHECK(save(&fixture, text, sizeof(text) - 1, work) ==
          HS_JOURNAL_WRITE_UNCERTAIN);
    CHECK(fixture.flash.erases == 1 && fixture.flash.writes == 1);
    fixture_destroy(&fixture);

    CHECK(fixture_init(&fixture));
    make_image(&fixture.flash, 64);
    fixture.flash.corrupt_readback = true;
    CHECK(save(&fixture, text, sizeof(text) - 1, work) ==
          HS_JOURNAL_WRITE_UNCERTAIN);
    CHECK(fixture.flash.erases == 1 && fixture.flash.writes == 1);
    fixture_destroy(&fixture);
    return true;
}

static bool test_workspace_text_validation(void)
{
    struct fixture fixture;
    uint8_t work[HS_JOURNAL_SECTOR];
    char text[HS_JOURNAL_TEXT_MAX + 1] = {0};
    CHECK(fixture_init(&fixture));
    make_image(&fixture.flash, 64);

    CHECK(hs_journal_save(NULL, text, 1, work, sizeof(work)) ==
          HS_JOURNAL_REFUSED);
    CHECK(hs_journal_save(&fixture.store, NULL, 1, work, sizeof(work)) ==
          HS_JOURNAL_REFUSED);
    CHECK(hs_journal_save(&fixture.store, text, 1, NULL, sizeof(work)) ==
          HS_JOURNAL_REFUSED);
    CHECK(save(&fixture, text, 0, work) == HS_JOURNAL_REFUSED);
    CHECK(save(&fixture, text, HS_JOURNAL_TEXT_MAX + 1, work) ==
          HS_JOURNAL_REFUSED);
    CHECK(hs_journal_save(&fixture.store, text, 1, work,
                          HS_JOURNAL_SECTOR - 1) == HS_JOURNAL_REFUSED);
    CHECK(hs_journal_save(&fixture.store, text, 1, work,
                          HS_JOURNAL_SECTOR + 1) == HS_JOURNAL_REFUSED);
    CHECK(fixture.flash.reads == 0);

    struct hs_journal_store missing = fixture.store;
    missing.read = NULL;
    CHECK(hs_journal_save(&missing, text, 1, work, sizeof(work)) ==
          HS_JOURNAL_REFUSED);
    missing = fixture.store;
    missing.erase = NULL;
    CHECK(hs_journal_save(&missing, text, 1, work, sizeof(work)) ==
          HS_JOURNAL_REFUSED);
    missing = fixture.store;
    missing.write = NULL;
    CHECK(hs_journal_save(&missing, text, 1, work, sizeof(work)) ==
          HS_JOURNAL_REFUSED);

    uint8_t *arena = malloc(HS_JOURNAL_SECTOR + sizeof(text));
    CHECK(arena != NULL);
    memset(arena, 0x41, HS_JOURNAL_SECTOR + sizeof(text));
    CHECK(hs_journal_save(&fixture.store, (char *)arena + HS_JOURNAL_SECTOR,
                          sizeof(text) - 1, arena, HS_JOURNAL_SECTOR) ==
          HS_JOURNAL_WRITTEN);
    free(arena);
    fixture_destroy(&fixture);

    CHECK(fixture_init(&fixture));
    make_image(&fixture.flash, 64);
    uint8_t overlapping[HS_JOURNAL_SECTOR + 32];
    CHECK(hs_journal_save(&fixture.store, (char *)overlapping + 8, 16,
                          overlapping, HS_JOURNAL_SECTOR) == HS_JOURNAL_REFUSED);
    CHECK(hs_journal_save(&fixture.store, (char *)overlapping, 64,
                          overlapping + 32, HS_JOURNAL_SECTOR) ==
          HS_JOURNAL_REFUSED);
    CHECK(fixture.flash.reads == 0 && fixture.flash.erases == 0 &&
          fixture.flash.writes == 0);
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
        {"write and preserve", test_write_then_preserve},
        {"valid record with debug attached", test_valid_record_preserved_with_debug_attached},
        {"malformed and partial sector", test_bad_or_partial_sector_never_overwritten},
        {"board and text gates", test_board_and_text_gates},
        {"image header gates", test_image_headers_rejected},
        {"checksum/SHA image boundary", test_image_bounds_include_checksum_and_sha},
        {"read errors", test_read_errors},
        {"uncertain flash operations", test_uncertain_flash_operations_do_not_retry},
        {"workspace/text validation", test_workspace_text_validation},
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
    printf("%u journal tests passed\n", passed);
    return 0;
}
