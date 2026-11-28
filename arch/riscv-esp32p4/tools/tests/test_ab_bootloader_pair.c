#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if defined(AROS_AB_TEST_COMMONCRYPTO)
#include <CommonCrypto/CommonDigest.h>
#endif

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL (-1)
#define ESP_PARTITION_MAGIC 0x50aa
#define ESP_PARTITION_TABLE_OFFSET 0x20000U
#define ESP_PARTITION_TABLE_SIZE 0x1000U
#define ESP_PARTITION_TABLE_MAX_LEN 0x0c00U
#define ESP_PARTITION_TABLE_MAX_ENTRIES 96
#define PART_FLAG_READONLY (1U << 1)
#define CONFIG_ESPTOOLPY_FLASHSIZE_1MB 0
#define CONFIG_ESPTOOLPY_FLASHSIZE_2MB 0
#define CONFIG_ESPTOOLPY_FLASHSIZE_4MB 0
#define CONFIG_ESPTOOLPY_FLASHSIZE_8MB 0
#define CONFIG_ESPTOOLPY_FLASHSIZE_16MB 0
#define CONFIG_ESPTOOLPY_FLASHSIZE_32MB 0
#define CONFIG_ESPTOOLPY_FLASHSIZE_64MB 0
#define CONFIG_ESPTOOLPY_FLASHSIZE_128MB 0
#if defined(AROS_AB_TEST_FLASH_32MB)
#undef CONFIG_ESPTOOLPY_FLASHSIZE_32MB
#define CONFIG_ESPTOOLPY_FLASHSIZE_32MB 1
#else
#undef CONFIG_ESPTOOLPY_FLASHSIZE_16MB
#define CONFIG_ESPTOOLPY_FLASHSIZE_16MB 1
#endif

typedef struct {
    uint32_t offset;
    uint32_t size;
} esp_partition_pos_t;

typedef struct {
    uint16_t magic;
    uint8_t type;
    uint8_t subtype;
    esp_partition_pos_t pos;
    uint8_t label[16];
    uint32_t flags;
} esp_partition_info_t;

typedef struct {
    uint32_t image_len;
} esp_image_metadata_t;

typedef void *bootloader_sha256_handle_t;

const void *bootloader_mmap(uint32_t offset, uint32_t size);
void bootloader_munmap(const void *mapping);
esp_err_t bootloader_flash_read(size_t offset, void *dest, size_t size, bool allow_decrypt);
esp_err_t esp_partition_table_verify(const esp_partition_info_t *table, bool log_errors,
                                     int *num_partitions);
bootloader_sha256_handle_t bootloader_sha256_start(void);
void bootloader_sha256_data(bootloader_sha256_handle_t handle, const void *data, size_t size);
void bootloader_sha256_finish(bootloader_sha256_handle_t handle, uint8_t *digest);

/* IDF flash/table services are host stubs. macOS uses CommonCrypto for the
 * real digest; the portable non-macOS digest stub is expressly not SHA-256.
 */
#define AROS_AB_CHECK_TEST 1
#include "../../bootloader/aros_ab_check.h"

#define FLASH_BYTES 0x01000000U
#define KERNEL_A 0x00040000U
#define KERNEL_B 0x000c0000U
#define KERNEL_BYTES 0x00080000U
#define BSP_A 0x00140000U
#define BSP_B 0x00740000U
#define BSP_BYTES 0x00600000U
#define DEVELOP 0x00d40000U
#define DEVELOP_BYTES 0x002c0000U
#define CORE_BYTES 1031U
#define PKG_BYTES 19U

static uint8_t flash_bytes[FLASH_BYTES];
static int table_count;

#if defined(AROS_AB_TEST_COMMONCRYPTO)
static CC_SHA256_CTX sha_state;
#else
struct test_sha {
    uint32_t state[8];
    uint32_t length;
};

static struct test_sha sha_state;
#endif

const void *bootloader_mmap(uint32_t offset, uint32_t size)
{
    if (offset != ESP_PARTITION_TABLE_OFFSET || size != ESP_PARTITION_TABLE_MAX_LEN) {
        return NULL;
    }
    return flash_bytes + offset;
}

void bootloader_munmap(const void *mapping)
{
    (void)mapping;
}

esp_err_t bootloader_flash_read(size_t offset, void *dest, size_t size, bool allow_decrypt)
{
    (void)allow_decrypt;
    if (dest == NULL || (offset & 3U) != 0 || (size & 3U) != 0 ||
        offset > sizeof(flash_bytes) || size > sizeof(flash_bytes) - offset) {
        return ESP_FAIL;
    }
    memcpy(dest, flash_bytes + offset, size);
    return ESP_OK;
}

esp_err_t esp_partition_table_verify(const esp_partition_info_t *table, bool log_errors,
                                     int *num_partitions)
{
    (void)log_errors;
    if (table != (const esp_partition_info_t *)(flash_bytes + ESP_PARTITION_TABLE_OFFSET) ||
        num_partitions == NULL || table_count <= 0) {
        return ESP_FAIL;
    }
    *num_partitions = table_count;
    return ESP_OK;
}

#if defined(AROS_AB_TEST_COMMONCRYPTO)
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
bootloader_sha256_handle_t bootloader_sha256_start(void)
{
    return CC_SHA256_Init(&sha_state) == 1 ? &sha_state : NULL;
}

void bootloader_sha256_data(bootloader_sha256_handle_t handle, const void *data, size_t size)
{
    assert(handle == &sha_state && size <= UINT32_MAX);
    assert(CC_SHA256_Update(&sha_state, data, (CC_LONG)size) == 1);
}

void bootloader_sha256_finish(bootloader_sha256_handle_t handle, uint8_t *digest)
{
    assert(handle == &sha_state);
    if (digest == NULL) {
        memset(&sha_state, 0, sizeof(sha_state));
    } else {
        assert(CC_SHA256_Final(digest, &sha_state) == 1);
    }
}
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
#else
bootloader_sha256_handle_t bootloader_sha256_start(void)
{
    static const uint32_t seeds[8] = {
        0x243f6a88U, 0x85a308d3U, 0x13198a2eU, 0x03707344U,
        0xa4093822U, 0x299f31d0U, 0x082efa98U, 0xec4e6c89U
    };
    memcpy(sha_state.state, seeds, sizeof(seeds));
    sha_state.length = 0;
    return &sha_state;
}

void bootloader_sha256_data(bootloader_sha256_handle_t handle, const void *data, size_t size)
{
    const uint8_t *bytes = data;
    size_t i;
    assert(handle == &sha_state);
    for (i = 0; i < size; ++i) {
        unsigned lane = sha_state.length & 7U;
        uint32_t next = sha_state.state[(lane + 1U) & 7U];
        sha_state.state[lane] ^= (uint32_t)bytes[i] + 0x9e3779b9U +
                                 (next << 6) + (next >> 2);
        sha_state.state[lane] = sha_state.state[lane] * 0x85ebca6bU + 0xc2b2ae35U;
        ++sha_state.length;
    }
}

void bootloader_sha256_finish(bootloader_sha256_handle_t handle, uint8_t *digest)
{
    unsigned i;
    assert(handle == &sha_state);
    if (digest == NULL) {
        memset(&sha_state, 0, sizeof(sha_state));
        return;
    }
    for (i = 0; i < 8; ++i) {
        uint32_t value = sha_state.state[i] ^ (sha_state.length * (i + 1U));
        digest[4U * i] = (uint8_t)value;
        digest[4U * i + 1U] = (uint8_t)(value >> 8);
        digest[4U * i + 2U] = (uint8_t)(value >> 16);
        digest[4U * i + 3U] = (uint8_t)(value >> 24);
    }
}
#endif

static void digest_bytes(const void *bytes, size_t length, uint8_t digest[32])
{
    bootloader_sha256_handle_t handle = bootloader_sha256_start();
    bootloader_sha256_data(handle, bytes, length);
    bootloader_sha256_finish(handle, digest);
}

static void check_digest_backend(void)
{
#if defined(AROS_AB_TEST_COMMONCRYPTO)
    static const uint8_t expected[32] = {
        0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea,
        0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
        0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c,
        0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad
    };
    uint8_t actual[32];
    digest_bytes("abc", 3, actual);
    assert(memcmp(actual, expected, sizeof(expected)) == 0);
    puts("CommonCrypto SHA-256 known-answer check passed");
#else
    puts("portable test digest stub active (not SHA-256)");
#endif
}

static void set_label(esp_partition_info_t *entry, const char *label)
{
    size_t length = strlen(label);
    assert(length < sizeof(entry->label));
    memcpy(entry->label, label, length);
}

static void add_partition(unsigned index, const char *label, uint8_t type, uint8_t subtype,
                          uint32_t offset, uint32_t size)
{
    esp_partition_info_t *table = (esp_partition_info_t *)(flash_bytes + ESP_PARTITION_TABLE_OFFSET);
    esp_partition_info_t *entry = &table[index];
    memset(entry, 0, sizeof(*entry));
    entry->magic = ESP_PARTITION_MAGIC;
    entry->type = type;
    entry->subtype = subtype;
    entry->pos.offset = offset;
    entry->pos.size = size;
    set_label(entry, label);
}

static void build_table(bool include_d1001_storage)
{
    memset(flash_bytes + ESP_PARTITION_TABLE_OFFSET, 0xff, ESP_PARTITION_TABLE_MAX_LEN);
    add_partition(0, "KERNEL_A", 0x00, 0x10, KERNEL_A, KERNEL_BYTES);
    add_partition(1, "KERNEL_B", 0x00, 0x11, KERNEL_B, KERNEL_BYTES);
    add_partition(2, "BSP_A", 0x40, 0x00, BSP_A, BSP_BYTES);
    add_partition(3, "BSP_B", 0x40, 0x01, BSP_B, BSP_BYTES);
    add_partition(4, "DEVELOP", 0x41, 0x00, DEVELOP, DEVELOP_BYTES);
    if (include_d1001_storage) {
        add_partition(5, "storage", 0x01, 0x81, 0x01020000U, 0x00fe0000U);
        table_count = 6;
    } else {
        table_count = 5;
    }
}

static void make_manifest(unsigned slot)
{
    uint32_t kernel_offset = slot == 0 ? KERNEL_A : KERNEL_B;
    uint32_t bsp_offset = slot == 0 ? BSP_A : BSP_B;
    uint32_t manifest_words[AROS_AB_MANIFEST_BYTES / sizeof(uint32_t)] = {0};
    uint8_t *manifest = (uint8_t *)manifest_words;
    uint8_t *pkg = flash_bytes + bsp_offset + AROS_AB_MANIFEST_BYTES;
    unsigned i;

    for (i = 0; i < CORE_BYTES; ++i) {
        flash_bytes[kernel_offset + i] = (uint8_t)(i * 13U + slot * 29U);
    }
    memcpy(pkg, "PKG\x01", 4);
    pkg[4] = (uint8_t)(PKG_BYTES >> 24);
    pkg[5] = (uint8_t)(PKG_BYTES >> 16);
    pkg[6] = (uint8_t)(PKG_BYTES >> 8);
    pkg[7] = (uint8_t)PKG_BYTES;
    for (i = 8; i < PKG_BYTES; ++i) {
        pkg[i] = (uint8_t)(0xa7U + i + slot);
    }

    memcpy(manifest, "P4ABPKG\x01", 8);
    manifest[8] = (uint8_t)CORE_BYTES;
    manifest[9] = (uint8_t)(CORE_BYTES >> 8);
    manifest[10] = (uint8_t)(CORE_BYTES >> 16);
    manifest[11] = (uint8_t)(CORE_BYTES >> 24);
    manifest[12] = (uint8_t)PKG_BYTES;
    manifest[13] = (uint8_t)(PKG_BYTES >> 8);
    manifest[14] = (uint8_t)(PKG_BYTES >> 16);
    manifest[15] = (uint8_t)(PKG_BYTES >> 24);
    digest_bytes(flash_bytes + kernel_offset, CORE_BYTES, manifest + 16);
    digest_bytes(pkg, PKG_BYTES, manifest + 48);
    digest_bytes(manifest, AROS_AB_MANIFEST_HASHED, manifest + 80);
    memcpy(flash_bytes + bsp_offset, manifest, sizeof(manifest_words));
}

static uint8_t *manifest_for(unsigned slot)
{
    uint32_t bsp_offset = slot == 0 ? BSP_A : BSP_B;
    return flash_bytes + bsp_offset;
}

static void refresh_manifest_header(unsigned slot)
{
    uint8_t *manifest = manifest_for(slot);
    digest_bytes(manifest, AROS_AB_MANIFEST_HASHED, manifest + 80);
}

static bool candidate_is_valid(unsigned slot)
{
    esp_partition_pos_t partition = {
        slot == 0 ? KERNEL_A : KERNEL_B,
        KERNEL_BYTES
    };
    esp_image_metadata_t image = { CORE_BYTES };
    return bootloader_aros_ab_pair_valid(&partition, &image);
}

static void test_manifest_rejections(void)
{
    uint8_t *manifest = manifest_for(0);

    manifest[0] ^= 0x01U;
    assert(!candidate_is_valid(0));
    make_manifest(0);

    manifest[80] ^= 0x01U;
    assert(!candidate_is_valid(0));
    make_manifest(0);

    manifest[112] = 0x01U;
    assert(!candidate_is_valid(0));
    make_manifest(0);

    --manifest[8];
    refresh_manifest_header(0);
    assert(!candidate_is_valid(0));
    make_manifest(0);

    --manifest[12]; /* Manifest says 18 bytes; PKG header still says 19. */
    refresh_manifest_header(0);
    assert(!candidate_is_valid(0));
    make_manifest(0);
}

static void test_partition_rejections(void)
{
    esp_partition_info_t *table =
        (esp_partition_info_t *)(flash_bytes + ESP_PARTITION_TABLE_OFFSET);

    table[0].subtype = 0x11;
    assert(!candidate_is_valid(0));
    build_table(false);

    memcpy(table[1].label, table[0].label, sizeof(table[1].label));
    assert(!candidate_is_valid(0));
    build_table(false);

    table[4].pos.offset = BSP_A + 0x1000U;
    assert(!candidate_is_valid(0));
    build_table(false);

    memset(table[0].label, 0, sizeof(table[0].label));
    memcpy(table[0].label, "KERNEL_X", sizeof("KERNEL_X") - 1U);
    assert(!candidate_is_valid(0));
    build_table(false);
}

int main(void)
{
    memset(flash_bytes, 0xff, sizeof(flash_bytes));
    check_digest_backend();
    build_table(false);
    make_manifest(0);
    make_manifest(1);

    assert(candidate_is_valid(0));
    assert(candidate_is_valid(1));

    flash_bytes[KERNEL_A + CORE_BYTES - 1U] ^= 0x80U;
    assert(!candidate_is_valid(0));
    assert(candidate_is_valid(1)); /* OTA fallback candidate remains sound. */
    flash_bytes[KERNEL_A + CORE_BYTES - 1U] ^= 0x80U;

    test_manifest_rejections();
    test_partition_rejections();

    flash_bytes[BSP_B + AROS_AB_MANIFEST_BYTES + PKG_BYTES - 1U] ^= 0x01U;

    /* D1001 retains this 32-MiB-flash partition above the A/B 16-MiB
     * payload/MMU window. The identical extent exceeds the JC board capacity.
     */
    build_table(true);
#if defined(AROS_AB_TEST_FLASH_32MB)
    assert(candidate_is_valid(0));
#else
    assert(!candidate_is_valid(0));
#endif
    assert(!candidate_is_valid(1));
    flash_bytes[BSP_B + AROS_AB_MANIFEST_BYTES + PKG_BYTES - 1U] ^= 0x01U;

    puts("A/B bootloader pair gate host checks passed");
    return 0;
}
