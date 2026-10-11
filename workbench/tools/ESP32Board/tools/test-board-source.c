/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#include <dos/dos.h>
#include <exec/memory.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "image.h"
#include "source_file.h"

int boardtool_check_c6(const char *path, const char *sha256,
    char *status, size_t capacity);
enum board_image_error board_c6_image_check_real(const uint8_t *image,
    size_t bytes, const char *trusted_sha256, struct board_image_info *info);

enum read_action {
    READ_NORMAL,
    READ_EARLY_EOF,
    READ_ERROR,
    READ_BAD_COUNT
};

struct fake_dos {
    const uint8_t *data;
    size_t data_size;
    size_t advertised_size;
    size_t offset;
    size_t read_chunk;
    size_t fail_read_call;
    size_t read_calls;
    size_t max_read_request;
    enum read_action read_action;
    LONG dir_entry_type;
    int open_succeeds;
    int fib_alloc_succeeds;
    int examine_succeeds;
    int image_alloc_succeeds;
    int close_succeeds;
    char *digest_to_mutate_on_open;
    int mutate_digest_on_open;
    int open_calls;
    int close_calls;
    int file_open;
    int fib_alloc_calls;
    int fib_free_calls;
    int active_fibs;
    int image_alloc_calls;
    int image_free_calls;
    int active_images;
    size_t allocated_image_size;
    int examine_calls;
    int hook_errors;
};

static struct fake_dos dos;
static unsigned long checks;
static unsigned long failures;
static unsigned long cases;
static const char *case_name = "startup";
static char current_fixture_digest[65];
static int verifier_calls;
static int verifier_saw_open_file;
static enum board_image_error verifier_result;
static const struct boardtool_c6_source *watched_owner;
static int owner_zero_when_freed;

#define CHECK(expression) do { \
    ++checks; \
    if (!(expression)) { \
        ++failures; \
        fprintf(stderr, "FAIL [%s] line %d: %s\n", case_name, __LINE__, \
            #expression); \
    } \
} while (0)

static void begin_case(const char *name)
{
    case_name = name;
    ++cases;
}

static int source_is_all_zero(const struct boardtool_c6_source *source)
{
    const uint8_t *bytes = (const uint8_t *)source;
    size_t i;
    for (i = 0; i < sizeof(*source); ++i)
        if (bytes[i] != 0) return 0;
    return 1;
}

static void fake_reset(const uint8_t *data, size_t data_size,
    size_t advertised_size)
{
    memset(&dos, 0, sizeof(dos));
    dos.data = data;
    dos.data_size = data_size;
    dos.advertised_size = advertised_size;
    dos.dir_entry_type = -1;
    dos.open_succeeds = 1;
    dos.fib_alloc_succeeds = 1;
    dos.examine_succeeds = 1;
    dos.image_alloc_succeeds = 1;
    dos.close_succeeds = 1;
    verifier_calls = 0;
    verifier_saw_open_file = 0;
    verifier_result = BOARD_IMAGE_ARGUMENT;
}

static void put_le16(uint8_t *bytes, uint16_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
}

static void put_le32(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
    bytes[2] = (uint8_t)(value >> 16);
    bytes[3] = (uint8_t)(value >> 24);
}

static void digest_to_hex(const uint8_t digest[32], char hex[65])
{
    static const char digits[] = "0123456789abcdef";
    size_t i;
    for (i = 0; i < 32; ++i) {
        hex[i * 2] = digits[digest[i] >> 4];
        hex[i * 2 + 1] = digits[digest[i] & 15];
    }
    hex[64] = '\0';
}

/* Build a real unsigned ESP image: one C6 app segment and an appended hash. */
static int make_fixture(uint8_t *image, size_t size, uint32_t secure_version,
    uint16_t min_revision, uint16_t max_revision, char trusted_sha256[65])
{
    size_t i, payload_end, hash_end;
    uint8_t checksum = 0xef, hash[32];

    if (!image || size < 336 || (size & 15) != 0 ||
        size > BOARD_C6_IMAGE_MAX) return 0;
    for (i = 0; i < size; ++i)
        image[i] = (uint8_t)((i * 29u + 7u) & 0xffu);

    image[0] = 0xe9;
    image[1] = 1;
    put_le16(image + 12, 13);
    put_le16(image + 15, min_revision);
    put_le16(image + 17, max_revision);
    image[23] = 1;

    put_le32(image + 24, 0x3f400020u);
    put_le32(image + 28, (uint32_t)(size - 80));
    put_le32(image + 32, 0xabcd5432u);
    put_le32(image + 36, secure_version);
    payload_end = size - 48;
    hash_end = size - 32;
    for (i = 32; i < payload_end; ++i) checksum ^= image[i];
    image[hash_end - 1] = checksum;
    if (!board_sha256(image, hash_end, hash)) return 0;
    memcpy(image + hash_end, hash, sizeof(hash));
    if (!board_sha256(image, size, hash)) return 0;
    digest_to_hex(hash, trusted_sha256);
    return 1;
}

static void expect_counts(int opens, int closes, int fib_allocs,
    int fib_frees, int image_allocs, int image_frees, size_t reads,
    int verifications, int active_images)
{
    CHECK(dos.open_calls == opens);
    CHECK(dos.close_calls == closes);
    CHECK(dos.fib_alloc_calls == fib_allocs);
    CHECK(dos.fib_free_calls == fib_frees);
    CHECK(dos.image_alloc_calls == image_allocs);
    CHECK(dos.image_free_calls == image_frees);
    CHECK(dos.read_calls == reads);
    CHECK(verifier_calls == verifications);
    CHECK(dos.file_open == 0);
    CHECK(dos.active_fibs == 0);
    CHECK(dos.active_images == active_images);
    CHECK(dos.hook_errors == 0);
}

BPTR Open(CONST_STRPTR name, LONG mode)
{
    ++dos.open_calls;
    if (strcmp(name, "firmware.bin") != 0 || mode != MODE_OLDFILE)
        ++dos.hook_errors;
    if (!dos.open_succeeds) return BNULL;
    dos.file_open = 1;
    if (dos.mutate_digest_on_open && dos.digest_to_mutate_on_open) {
        memset(dos.digest_to_mutate_on_open, '0', 64);
        dos.digest_to_mutate_on_open[64] = '\0';
    }
    return (BPTR)0xc6u;
}

struct FileInfoBlock *AllocDosObject(ULONG type, const void *tags)
{
    struct FileInfoBlock *fib;
    ++dos.fib_alloc_calls;
    if (type != DOS_FIB || tags != NULL) ++dos.hook_errors;
    if (!dos.fib_alloc_succeeds) return NULL;
    fib = (struct FileInfoBlock *)calloc(1, sizeof(*fib));
    if (fib) ++dos.active_fibs;
    return fib;
}

void FreeDosObject(ULONG type, void *object)
{
    ++dos.fib_free_calls;
    if (type != DOS_FIB || !object || dos.active_fibs != 1)
        ++dos.hook_errors;
    else
        --dos.active_fibs;
    free(object);
}

LONG ExamineFH(BPTR file, struct FileInfoBlock *fib)
{
    ++dos.examine_calls;
    if (file != (BPTR)0xc6u || !dos.file_open || !fib)
        ++dos.hook_errors;
    if (!dos.examine_succeeds) return 0;
    if (dos.advertised_size > INT32_MAX) {
        ++dos.hook_errors;
        return 0;
    }
    fib->fib_DirEntryType = dos.dir_entry_type;
    fib->fib_Size = (LONG)dos.advertised_size;
    return 1;
}

void *AllocVec(size_t size, uint32_t requirements)
{
    void *memory;
    ++dos.image_alloc_calls;
    dos.allocated_image_size = size;
    if (requirements != MEMF_PUBLIC) ++dos.hook_errors;
    if (!dos.image_alloc_succeeds) return NULL;
    memory = malloc(size);
    if (memory) ++dos.active_images;
    return memory;
}

void FreeVec(void *memory)
{
    ++dos.image_free_calls;
    if (watched_owner && watched_owner->image == NULL &&
        source_is_all_zero(watched_owner))
        owner_zero_when_freed = 1;
    if (!memory || dos.active_images != 1)
        ++dos.hook_errors;
    else
        --dos.active_images;
    free(memory);
}

LONG Read(BPTR file, void *buffer, LONG length)
{
    size_t requested, count;
    ++dos.read_calls;
    if (file != (BPTR)0xc6u || !dos.file_open || length < 0 || !buffer)
        ++dos.hook_errors;
    if (length < 0) return -1;
    requested = (size_t)length;
    if (requested > dos.max_read_request) dos.max_read_request = requested;
    if (dos.fail_read_call == dos.read_calls) {
        if (dos.read_action == READ_EARLY_EOF) return 0;
        if (dos.read_action == READ_ERROR) return -1;
        if (dos.read_action == READ_BAD_COUNT) return length + 1;
    }
    if (dos.offset >= dos.data_size) return 0;
    count = requested;
    if (dos.read_chunk && count > dos.read_chunk) count = dos.read_chunk;
    if (count > dos.data_size - dos.offset) count = dos.data_size - dos.offset;
    memcpy(buffer, dos.data + dos.offset, count);
    dos.offset += count;
    return (LONG)count;
}

LONG Close(BPTR file)
{
    ++dos.close_calls;
    if (file != (BPTR)0xc6u || !dos.file_open) ++dos.hook_errors;
    /* AmigaDOS closes the handle even when Close reports an error. */
    dos.file_open = 0;
    return dos.close_succeeds ? 1 : 0;
}

enum board_image_error board_c6_image_check(const uint8_t *image,
    size_t bytes, const char *trusted_sha256, struct board_image_info *info)
{
    ++verifier_calls;
    if (dos.file_open) ++verifier_saw_open_file;
    verifier_result = board_c6_image_check_real(image, bytes,
        trusted_sha256, info);
    return verifier_result;
}

static void test_sha_and_valid_short_reads(const uint8_t *small_image,
    size_t small_size, const char original_digest[65])
{
    static const uint8_t abc_sha256[32] = {
        0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea,
        0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
        0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c,
        0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad
    };
    uint8_t hash[32];
    struct boardtool_c6_source source;
    struct boardtool_c6_source before;
    struct boardtool_c6_source zero_source;
    char digest[65], status[160], second_status[96];
    char image_prefix[32];
    size_t opens_before, frees_before;

    begin_case("real SHA and valid short-read load");
    CHECK(board_sha256((const uint8_t *)"abc", 3, hash));
    CHECK(memcmp(hash, abc_sha256, sizeof(hash)) == 0);

    memset(&source, 0, sizeof(source));
    memcpy(digest, original_digest, sizeof(digest));
    memset(status, 0xa5, sizeof(status));
    fake_reset(small_image, small_size, small_size);
    dos.read_chunk = 53;
    dos.digest_to_mutate_on_open = digest;
    dos.mutate_digest_on_open = 1;

    CHECK(boardtool_c6_source_load("firmware.bin", digest, &source,
        status, sizeof(status)) == 1);
    CHECK(dos.open_calls == 1);
    CHECK(dos.close_calls == 1);
    CHECK(dos.file_open == 0);
    CHECK(dos.read_calls > 2);
    CHECK(dos.max_read_request <= 4096);
    CHECK(dos.fib_alloc_calls == 1 && dos.fib_free_calls == 1);
    CHECK(dos.image_alloc_calls == 1 && dos.image_free_calls == 0);
    CHECK(dos.active_images == 1);
    CHECK(dos.allocated_image_size == small_size);
    CHECK(verifier_calls == 1 && verifier_result == BOARD_IMAGE_OK);
    CHECK(verifier_saw_open_file == 0);
    CHECK(source.image != NULL && source.image != small_image);
    CHECK(source.size == small_size);
    CHECK(memcmp(source.image, small_image, small_size) == 0);
    CHECK(memcmp(source.expected_sha256, original_digest, 65) == 0);
    CHECK(strcmp(digest, original_digest) != 0);
    CHECK(source.info.bytes == small_size);
    CHECK(source.info.secure_version == 7);
    CHECK(source.info.min_revision == 2);
    CHECK(source.info.max_revision == 5);
    CHECK(strstr(status, "NOT verified; no write") != NULL);

    opens_before = (size_t)dos.open_calls;
    CHECK(boardtool_c6_source_recheck(&source) == 1);
    CHECK((size_t)dos.open_calls == opens_before);
    CHECK(verifier_saw_open_file == 0);

    before = source;
    memset(second_status, 0, sizeof(second_status));
    CHECK(boardtool_c6_source_load("firmware.bin", original_digest,
        &source, second_status, sizeof(second_status)) == 0);
    CHECK(memcmp(&source, &before, sizeof(source)) == 0);
    CHECK(strstr(second_status, "already owns") != NULL);
    CHECK((size_t)dos.open_calls == opens_before);

    before = source;
    memcpy(image_prefix, source.image, sizeof(image_prefix));
    CHECK(boardtool_c6_source_load("firmware.bin", original_digest,
        &source, (char *)source.image, sizeof(image_prefix)) == 0);
    CHECK(memcmp(&source, &before, sizeof(source)) == 0);
    CHECK(memcmp(source.image, image_prefix, sizeof(image_prefix)) == 0);
    CHECK((size_t)dos.open_calls == opens_before);

    boardtool_c6_source_dispose(&source);
    CHECK(dos.image_free_calls == 1 && dos.active_images == 0);

    /* The read-only CHECK-C6 adapter must load and dispose the same owner. */
    fake_reset(small_image, small_size, small_size);
    dos.read_chunk = 71;
    memset(status, 0, sizeof(status));
    CHECK(boardtool_check_c6("firmware.bin", original_digest,
        status, sizeof(status)) == 1);
    CHECK(verifier_calls == 1 && verifier_result == BOARD_IMAGE_OK);
    CHECK(verifier_saw_open_file == 0);
    CHECK(dos.open_calls == 1 && dos.close_calls == 1);
    CHECK(dos.fib_alloc_calls == 1 && dos.fib_free_calls == 1);
    CHECK(dos.image_alloc_calls == 1 && dos.image_free_calls == 1);
    CHECK(dos.active_images == 0);
    CHECK(dos.hook_errors == 0);

    /* The FreeVec hook observes the cleared owner before the sole free. */
    fake_reset(small_image, small_size, small_size);
    memcpy(digest, original_digest, sizeof(digest));
    memset(&source, 0, sizeof(source));
    CHECK(boardtool_c6_source_load("firmware.bin", digest, &source,
        status, sizeof(status)) == 1);
    CHECK(dos.active_images == 1);
    frees_before = (size_t)dos.image_free_calls;
    watched_owner = &source;
    owner_zero_when_freed = 0;
    boardtool_c6_source_dispose(&source);
    watched_owner = NULL;
    memset(&zero_source, 0, sizeof(zero_source));
    CHECK(memcmp(&source, &zero_source, sizeof(source)) == 0);
    CHECK(owner_zero_when_freed == 1);
    CHECK((size_t)dos.image_free_calls == frees_before + 1);
    CHECK(dos.active_images == 0);
    boardtool_c6_source_dispose(&source);
    CHECK(memcmp(&source, &zero_source, sizeof(source)) == 0);
    CHECK((size_t)dos.image_free_calls == frees_before + 1);

}

static void test_max_size_and_read_bound(void)
{
    const size_t size = BOARD_C6_IMAGE_MAX;
    uint8_t *image = (uint8_t *)malloc(size);
    struct boardtool_c6_source source;
    char digest[65], status[160];

    begin_case("maximum image and 4096-byte read bound");
    CHECK(image != NULL);
    if (!image) return;
    CHECK(make_fixture(image, size, 19, 3, 8, digest));
    memset(&source, 0, sizeof(source));
    fake_reset(image, size, size);
    dos.read_chunk = 3071;
    CHECK(boardtool_c6_source_load("firmware.bin", digest, &source,
        status, sizeof(status)) == 1);
    CHECK(source.size == size);
    CHECK(dos.allocated_image_size == size);
    CHECK(dos.max_read_request == 4096);
    CHECK(dos.read_calls > 300);
    CHECK(memcmp(source.image, image, size) == 0);
    CHECK(verifier_calls == 1 && verifier_result == BOARD_IMAGE_OK);
    CHECK(verifier_saw_open_file == 0);
    expect_counts(1, 1, 1, 1, 1, 0, dos.read_calls, 1, 1);
    boardtool_c6_source_dispose(&source);
    CHECK(dos.image_free_calls == 1 && dos.active_images == 0);
    free(image);
}

static void check_refusal(const char *name, const uint8_t *data,
    size_t actual_size, size_t advertised_size,
    int open_calls, int close_calls, int fib_allocs, int fib_frees,
    int image_allocs, int image_frees, size_t read_calls, int verify_calls,
    int active_images, int expect_status)
{
    struct boardtool_c6_source source, before;
    char status[160], digest[65];

    begin_case(name);
    memset(&source, 0, sizeof(source));
    memcpy(digest, current_fixture_digest, sizeof(digest));
    memset(status, 0x5a, sizeof(status));
    fake_reset(data, actual_size, advertised_size);
    if (strcmp(name, "open failure") == 0) {
        dos.open_succeeds = 0;
    } else if (strcmp(name, "FIB allocation failure") == 0) {
        dos.fib_alloc_succeeds = 0;
    } else if (strcmp(name, "Examine failure") == 0) {
        dos.examine_succeeds = 0;
    } else if (strcmp(name, "directory refusal") == 0) {
        dos.dir_entry_type = 1;
    } else if (strcmp(name, "image allocation failure") == 0) {
        dos.image_alloc_succeeds = 0;
    } else if (strcmp(name, "early EOF") == 0) {
        dos.read_chunk = 64;
        dos.fail_read_call = 3;
        dos.read_action = READ_EARLY_EOF;
    } else if (strcmp(name, "read error") == 0) {
        dos.read_chunk = 64;
        dos.fail_read_call = 2;
        dos.read_action = READ_ERROR;
    } else if (strcmp(name, "bad read count") == 0) {
        dos.fail_read_call = 1;
        dos.read_action = READ_BAD_COUNT;
    } else if (strcmp(name, "close failure") == 0) {
        dos.close_succeeds = 0;
    } else if (strcmp(name, "outer hash mismatch") == 0) {
        digest[0] = digest[0] == '0' ? '1' : '0';
    }
    before = source;
    CHECK(boardtool_c6_source_load("firmware.bin", digest, &source,
        status, sizeof(status)) == 0);
    CHECK(memcmp(&source, &before, sizeof(source)) == 0);
    if (expect_status) CHECK(status[0] != '\0');
    expect_counts(open_calls, close_calls, fib_allocs, fib_frees,
        image_allocs, image_frees, read_calls, verify_calls, active_images);
}

static void test_refusal_paths(const uint8_t *small_image, size_t small_size,
    const uint8_t *trailing_image)
{
    uint8_t *malformed = (uint8_t *)malloc(small_size);
    uint8_t digest_bytes[32];
    char saved_digest[65];

    CHECK(malformed != NULL);
    if (!malformed) return;

    check_refusal("open failure", small_image, small_size, small_size,
        1, 0, 0, 0, 0, 0, 0, 0, 0, 1);
    check_refusal("FIB allocation failure", small_image, small_size,
        small_size, 1, 1, 1, 0, 0, 0, 0, 0, 0, 1);
    check_refusal("Examine failure", small_image, small_size, small_size,
        1, 1, 1, 1, 0, 0, 0, 0, 0, 1);
    check_refusal("directory refusal", small_image, small_size, small_size,
        1, 1, 1, 1, 0, 0, 0, 0, 0, 1);
    check_refusal("too-small size", small_image, small_size, 287,
        1, 1, 1, 1, 0, 0, 0, 0, 0, 1);
    check_refusal("over-cap size", small_image, small_size,
        BOARD_C6_IMAGE_MAX + 1u, 1, 1, 1, 1, 0, 0, 0, 0, 0, 1);
    check_refusal("image allocation failure", small_image, small_size,
        small_size, 1, 1, 1, 1, 1, 0, 0, 0, 0, 1);
    check_refusal("early EOF", small_image, small_size, small_size,
        1, 1, 1, 1, 1, 1, 3, 0, 0, 1);
    check_refusal("read error", small_image, small_size, small_size,
        1, 1, 1, 1, 1, 1, 2, 0, 0, 1);
    check_refusal("bad read count", small_image, small_size, small_size,
        1, 1, 1, 1, 1, 1, 1, 0, 0, 1);
    check_refusal("trailing data", trailing_image, small_size + 1,
        small_size, 1, 1, 1, 1, 1, 1, 2, 0, 0, 1);
    check_refusal("close failure", small_image, small_size, small_size,
        1, 1, 1, 1, 1, 1, 2, 0, 0, 1);

    check_refusal("outer hash mismatch", small_image, small_size, small_size,
        1, 1, 1, 1, 1, 1, 2, 1, 0, 1);
    begin_case("real image checksum refusal");
    memcpy(malformed, small_image, small_size);
    malformed[small_size - 33] ^= 1;
    CHECK(board_sha256(malformed, small_size, digest_bytes));
    digest_to_hex(digest_bytes, saved_digest);
    memcpy(current_fixture_digest, saved_digest, sizeof(saved_digest));
    check_refusal("real image checksum refusal", malformed, small_size,
        small_size, 1, 1, 1, 1, 1, 1, 2, 1, 0, 1);
    CHECK(verifier_result == BOARD_IMAGE_CHECKSUM);
    free(malformed);
}

enum recheck_mutation {
    MUTATE_IMAGE,
    MUTATE_EXPECTED_HASH,
    MUTATE_INFO_BYTES,
    MUTATE_INFO_DIGEST,
    MUTATE_SECURE_VERSION,
    MUTATE_MIN_REVISION,
    MUTATE_MAX_REVISION,
    MUTATE_MIN_EFUSE_REVISION,
    MUTATE_MAX_EFUSE_REVISION,
    MUTATE_MMU_PAGE_SIZE
};

static void test_recheck_detects_changes(const uint8_t *image, size_t size,
    const char digest[65])
{
    static const struct {
        enum recheck_mutation mutation;
        const char *name;
    } mutations[] = {
        {MUTATE_IMAGE, "recheck changed image bytes"},
        {MUTATE_EXPECTED_HASH, "recheck changed trusted hash"},
        {MUTATE_INFO_BYTES, "recheck changed byte count"},
        {MUTATE_INFO_DIGEST, "recheck changed inspected digest"},
        {MUTATE_SECURE_VERSION, "recheck changed secure version"},
        {MUTATE_MIN_REVISION, "recheck changed minimum revision"},
        {MUTATE_MAX_REVISION, "recheck changed maximum revision"},
        {MUTATE_MIN_EFUSE_REVISION, "recheck changed minimum eFuse block revision"},
        {MUTATE_MAX_EFUSE_REVISION, "recheck changed maximum eFuse block revision"},
        {MUTATE_MMU_PAGE_SIZE, "recheck changed MMU page size"}
    };
    size_t i;

    for (i = 0; i < sizeof(mutations) / sizeof(mutations[0]); ++i) {
        struct boardtool_c6_source source;
        char local_digest[65], status[160];
        int opens_before;

        begin_case(mutations[i].name);
        memset(&source, 0, sizeof(source));
        memcpy(local_digest, digest, sizeof(local_digest));
        fake_reset(image, size, size);
        CHECK(boardtool_c6_source_load("firmware.bin", local_digest,
            &source, status, sizeof(status)) == 1);
        CHECK(source.image != NULL);
        if (!source.image) continue;
        switch (mutations[i].mutation) {
        case MUTATE_IMAGE:
            source.image[48] ^= 1;
            break;
        case MUTATE_EXPECTED_HASH:
            source.expected_sha256[0] =
                source.expected_sha256[0] == '0' ? '1' : '0';
            break;
        case MUTATE_INFO_BYTES:
            --source.info.bytes;
            break;
        case MUTATE_INFO_DIGEST:
            source.info.digest[0] ^= 1;
            break;
        case MUTATE_SECURE_VERSION:
            ++source.info.secure_version;
            break;
        case MUTATE_MIN_REVISION:
            ++source.info.min_revision;
            break;
        case MUTATE_MAX_REVISION:
            ++source.info.max_revision;
            break;
        case MUTATE_MIN_EFUSE_REVISION:
            ++source.info.min_efuse_revision;
            break;
        case MUTATE_MAX_EFUSE_REVISION:
            ++source.info.max_efuse_revision;
            break;
        case MUTATE_MMU_PAGE_SIZE:
            ++source.info.mmu_page_size;
            break;
        }
        opens_before = dos.open_calls;
        CHECK(boardtool_c6_source_recheck(&source) == 0);
        CHECK(dos.open_calls == opens_before);
        CHECK(verifier_saw_open_file == 0);
        boardtool_c6_source_dispose(&source);
        CHECK(dos.active_images == 0);
        CHECK(dos.image_free_calls == 1);
        CHECK(dos.hook_errors == 0);
    }
}

static void test_alias_and_argument_refusals(const uint8_t *image,
    size_t image_size, const char *digest)
{
    struct boardtool_c6_source source, before;
    char path[32] = "firmware.bin";
    char status[80];
    char status_before[80];
    char digest_copy[65];
    char empty_path[] = "";
    char bad_digest[65];

    begin_case("source/status/input alias refusals");
    memset(&source, 0, sizeof(source));
    memset(status, 0x6b, sizeof(status));
    memcpy(status_before, status, sizeof(status));
    memcpy(digest_copy, digest, sizeof(digest_copy));
    before = source;
    fake_reset(NULL, 0, 0);
    CHECK(boardtool_c6_source_load(path, digest, &source,
        (char *)&source, sizeof(source)) == 0);
    CHECK(memcmp(&source, &before, sizeof(source)) == 0);
    CHECK(dos.open_calls == 0 && dos.close_calls == 0);

    memset(&source, 0, sizeof(source));
    before = source;
    CHECK(boardtool_c6_source_load((const char *)&source, digest, &source,
        status, sizeof(status)) == 0);
    CHECK(memcmp(&source, &before, sizeof(source)) == 0);
    CHECK(memcmp(status, status_before, sizeof(status)) == 0);
    CHECK(dos.open_calls == 0);

    memset(&source, 0, sizeof(source));
    before = source;
    CHECK(boardtool_c6_source_load(path, (const char *)&source, &source,
        status, sizeof(status)) == 0);
    CHECK(memcmp(&source, &before, sizeof(source)) == 0);
    CHECK(memcmp(status, status_before, sizeof(status)) == 0);
    CHECK(dos.open_calls == 0);

    CHECK(boardtool_c6_source_load(path, digest, &source, path,
        sizeof(path)) == 0);
    CHECK(strcmp(path, "firmware.bin") == 0);
    CHECK(memcmp(digest_copy, digest, sizeof(digest_copy)) == 0);
    CHECK(boardtool_c6_source_load(path, digest_copy, &source, digest_copy,
        sizeof(digest_copy)) == 0);
    CHECK(memcmp(digest_copy, digest, sizeof(digest_copy)) == 0);
    CHECK(boardtool_c6_source_load(path, digest, &source, status, 0) == 0);
    CHECK(dos.open_calls == 0);

    begin_case("bad path and digest refusals");
    CHECK(boardtool_c6_source_load(empty_path, digest, &source,
        status, sizeof(status)) == 0);
    CHECK(dos.open_calls == 0);
    memcpy(bad_digest, digest, sizeof(bad_digest));
    bad_digest[3] = '\0';
    fake_reset(image, image_size, image_size);
    CHECK(boardtool_c6_source_load(path, bad_digest, &source,
        status, sizeof(status)) == 0);
    CHECK(dos.open_calls == 0 && dos.close_calls == 0);

    memset(bad_digest, 'g', 64);
    bad_digest[64] = '\0';
    fake_reset(image, image_size, image_size);
    CHECK(boardtool_c6_source_load(path, bad_digest, &source,
        status, sizeof(status)) == 0);
    CHECK(dos.open_calls == 1 && dos.close_calls == 1);
    CHECK(dos.fib_alloc_calls == 1 && dos.fib_free_calls == 1);
    CHECK(dos.image_alloc_calls == 1 && dos.image_free_calls == 1);
    CHECK(dos.read_calls == 2 && verifier_calls == 1);
    CHECK(verifier_result == BOARD_IMAGE_ARGUMENT);
    CHECK(dos.active_images == 0 && dos.hook_errors == 0);
    CHECK(dos.hook_errors == 0);
}

int main(void)
{
    const size_t small_size = 336;
    uint8_t *small_image = (uint8_t *)malloc(small_size + 1);
    char digest[65];

    if (!small_image) {
        fprintf(stderr, "fixture allocation failed\n");
        return 2;
    }
    if (!make_fixture(small_image, small_size, 7, 2, 5, digest)) {
        fprintf(stderr, "valid fixture generation failed\n");
        free(small_image);
        return 2;
    }
    small_image[small_size] = 0xa5;
    memcpy(current_fixture_digest, digest, sizeof(digest));

    test_sha_and_valid_short_reads(small_image, small_size, digest);
    test_max_size_and_read_bound();
    test_refusal_paths(small_image, small_size, small_image);
    memcpy(current_fixture_digest, digest, sizeof(digest));
    test_recheck_detects_changes(small_image, small_size, digest);
    test_alias_and_argument_refusals(small_image, small_size, digest);

    free(small_image);
    printf("RESULT cases=%lu checks=%lu failures=%lu\n", cases, checks,
        failures);
    return failures ? 1 : 0;
}
