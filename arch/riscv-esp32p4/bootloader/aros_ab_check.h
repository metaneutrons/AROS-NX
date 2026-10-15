/*
 * Read-only A/B kernel + BSP consistency gate for the ESP-IDF bootloader.
 *
 * The digest fields detect accidental corruption and mismatched packages;
 * they are not publisher authentication. The ESP image verifier remains
 * responsible for the kernel image's normal IDF integrity/signature checks.
 */
#ifndef AROS_AB_CHECK_H
#define AROS_AB_CHECK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifndef AROS_AB_CHECK_TEST
#include "bootloader_flash_priv.h"
#include "bootloader_sha.h"
#include "esp_flash_partitions.h"
#include "esp_image_format.h"
#endif

#define AROS_AB_MAPPED_LIMIT       0x01000000U
#define AROS_AB_BSP_LIMIT          0x00600000U
#define AROS_AB_KERNEL_BYTES       0x00080000U
#define AROS_AB_DEVELOP_BYTES      0x002c0000U
#define AROS_AB_MANIFEST_BYTES     128U
#define AROS_AB_MANIFEST_HASHED    80U
#define AROS_AB_DIGEST_BYTES       32U

/* CONFIG_ESPTOOLPY_FLASHSIZE is a Kconfig string (for esptool), so derive
 * the byte capacity from its selected choice symbol for C range checks.
 */
#if CONFIG_ESPTOOLPY_FLASHSIZE_1MB
#define AROS_AB_CONFIG_FLASH_BYTES 0x00100000U
#elif CONFIG_ESPTOOLPY_FLASHSIZE_2MB
#define AROS_AB_CONFIG_FLASH_BYTES 0x00200000U
#elif CONFIG_ESPTOOLPY_FLASHSIZE_4MB
#define AROS_AB_CONFIG_FLASH_BYTES 0x00400000U
#elif CONFIG_ESPTOOLPY_FLASHSIZE_8MB
#define AROS_AB_CONFIG_FLASH_BYTES 0x00800000U
#elif CONFIG_ESPTOOLPY_FLASHSIZE_16MB
#define AROS_AB_CONFIG_FLASH_BYTES 0x01000000U
#elif CONFIG_ESPTOOLPY_FLASHSIZE_32MB
#define AROS_AB_CONFIG_FLASH_BYTES 0x02000000U
#elif CONFIG_ESPTOOLPY_FLASHSIZE_64MB
#define AROS_AB_CONFIG_FLASH_BYTES 0x04000000U
#elif CONFIG_ESPTOOLPY_FLASHSIZE_128MB
#define AROS_AB_CONFIG_FLASH_BYTES 0x08000000U
#else
#error "A/B bootloader gate requires a fixed ESPTOOLPY flash-size choice"
#endif

static inline uint32_t aros_ab_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline uint32_t aros_ab_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static inline bool aros_ab_label_is(const uint8_t label[16], const char *name)
{
    size_t i, n = strlen(name);
    if (n >= 16 || memcmp(label, name, n) != 0) {
        return false;
    }
    for (i = n; i < 16; ++i) {
        if (label[i] != 0) {
            return false;
        }
    }
    return true;
}

static inline bool aros_ab_hash_bytes(const void *bytes, size_t length,
                                      uint8_t digest[AROS_AB_DIGEST_BYTES])
{
    bootloader_sha256_handle_t handle;
    if (bytes == NULL || digest == NULL) {
        return false;
    }
    handle = bootloader_sha256_start();
    if (handle == NULL) {
        return false;
    }
    bootloader_sha256_data(handle, bytes, length);
    bootloader_sha256_finish(handle, digest);
    return true;
}

/* The bootloader flash-read API requires 4-byte aligned addresses, buffers,
 * and lengths. Only the final read may be rounded up; those extra bytes are
 * excluded from the digest and checked to remain inside the supplied extent.
 */
static inline bool aros_ab_hash_flash(uint32_t offset, uint32_t length,
                                      uint32_t available,
                                      uint8_t digest[AROS_AB_DIGEST_BYTES])
{
    uint32_t scratch[128];
    uint32_t done = 0;
    bootloader_sha256_handle_t handle;

    if (digest == NULL || length == 0 || length > available || (offset & 3U)) {
        return false;
    }
    handle = bootloader_sha256_start();
    if (handle == NULL) {
        return false;
    }
    while (done < length) {
        uint32_t remaining = length - done;
        uint32_t chunk = remaining < sizeof(scratch) ? remaining : sizeof(scratch);
        uint32_t read_size = (chunk + 3U) & ~3U;
        if (read_size > available - done ||
            bootloader_flash_read((size_t)offset + done, scratch, read_size, true) != ESP_OK) {
            bootloader_sha256_finish(handle, NULL);
            return false;
        }
        bootloader_sha256_data(handle, scratch, chunk);
        done += chunk;
    }
    bootloader_sha256_finish(handle, digest);
    return true;
}

static inline bool aros_ab_resolve_candidate(
    const esp_partition_info_t *table, int count,
    const esp_partition_pos_t *partition,
    esp_partition_pos_t kernel[2], esp_partition_pos_t bsp[2],
    uint32_t *candidate_slot)
{
    static const char *const labels[5] = {
        "KERNEL_A", "KERNEL_B", "BSP_A", "BSP_B", "DEVELOP"
    };
    static const uint8_t types[5] = { 0x00, 0x00, 0x40, 0x40, 0x41 };
    static const uint8_t subtypes[5] = { 0x10, 0x11, 0x00, 0x01, 0x00 };
    unsigned seen = 0, app_count = 0;
    size_t i, j;

    if (table == NULL || partition == NULL || kernel == NULL || bsp == NULL ||
        candidate_slot == NULL || count <= 0 || count > ESP_PARTITION_TABLE_MAX_ENTRIES) {
        return false;
    }

    for (i = 0; i < (size_t)count; ++i) {
        const esp_partition_info_t *entry = &table[i];
        uint32_t offset = entry->pos.offset;
        uint32_t size = entry->pos.size;
        uint32_t end;
        unsigned role;

        if (entry->magic != ESP_PARTITION_MAGIC || size == 0 ||
            offset < ESP_PARTITION_TABLE_OFFSET + ESP_PARTITION_TABLE_SIZE ||
            (offset & 0xfffU) != 0 || (size & 0xfffU) != 0 ||
            offset >= AROS_AB_CONFIG_FLASH_BYTES ||
            size > AROS_AB_CONFIG_FLASH_BYTES - offset ||
            (entry->flags & ~PART_FLAG_READONLY) != 0) {
            return false;
        }
        end = offset + size;

        for (j = 0; j < i; ++j) {
            uint32_t old_offset = table[j].pos.offset;
            uint32_t old_size = table[j].pos.size;
            uint32_t old_end = old_offset + old_size;
            if (memcmp(entry->label, table[j].label, sizeof(entry->label)) == 0 ||
                (offset < old_end && old_offset < end)) {
                return false;
            }
        }

        if (entry->type == 0x00) {
            ++app_count;
            if (app_count > 2 || entry->subtype < 0x10 || entry->subtype > 0x11) {
                return false;
            }
        }

        for (role = 0; role < 5; ++role) {
            if (!aros_ab_label_is(entry->label, labels[role])) {
                continue;
            }
            if ((seen & (1U << role)) != 0 || entry->type != types[role] ||
                entry->subtype != subtypes[role]) {
                return false;
            }
            seen |= 1U << role;
            if (role < 2) {
                if ((offset & 0xffffU) != 0 || size != AROS_AB_KERNEL_BYTES ||
                    offset >= AROS_AB_MAPPED_LIMIT ||
                    size > AROS_AB_MAPPED_LIMIT - offset) {
                    return false;
                }
                kernel[role].offset = offset;
                kernel[role].size = size;
            } else if (role < 4) {
                if (size != AROS_AB_BSP_LIMIT || offset >= AROS_AB_MAPPED_LIMIT ||
                    size > AROS_AB_MAPPED_LIMIT - offset) {
                    return false;
                }
                bsp[role - 2].offset = offset;
                bsp[role - 2].size = size;
            } else if (size != AROS_AB_DEVELOP_BYTES ||
                       offset >= AROS_AB_MAPPED_LIMIT ||
                       size > AROS_AB_MAPPED_LIMIT - offset) {
                return false;
            }
            break;
        }
    }
    if (seen != 0x1fU || app_count != 2) {
        return false;
    }

    if (partition->offset == kernel[0].offset && partition->size == kernel[0].size) {
        *candidate_slot = 0;
    } else if (partition->offset == kernel[1].offset && partition->size == kernel[1].size) {
        *candidate_slot = 1;
    } else {
        return false;
    }
    return true;
}

static inline bool bootloader_aros_ab_pair_valid(
    const esp_partition_pos_t *partition, const esp_image_metadata_t *image)
{
    esp_partition_pos_t kernel[2] = {{0, 0}, {0, 0}};
    esp_partition_pos_t bsp[2] = {{0, 0}, {0, 0}};
    uint32_t manifest_words[AROS_AB_MANIFEST_BYTES / sizeof(uint32_t)];
    uint32_t pkg_prefix[2];
    uint8_t calculated[AROS_AB_DIGEST_BYTES];
    const uint8_t *manifest = (const uint8_t *)manifest_words;
    const esp_partition_info_t *mapped;
    uint32_t package_size, core_size, candidate_slot;
    int count = 0;
    size_t i;

    if (partition == NULL || image == NULL || image->image_len == 0 ||
        (partition->offset & 3U) != 0 || partition->size == 0 ||
        image->image_len > partition->size ||
        partition->offset >= AROS_AB_MAPPED_LIMIT ||
        partition->size > AROS_AB_MAPPED_LIMIT - partition->offset) {
        return false;
    }

    /* The mmap is released before flash reads; the bootloader supports one
     * outstanding mapping and the hash reads must use the flash API instead.
     */
    mapped = (const esp_partition_info_t *)bootloader_mmap(
        ESP_PARTITION_TABLE_OFFSET, ESP_PARTITION_TABLE_MAX_LEN);
    if (mapped == NULL) {
        return false;
    }
    if (esp_partition_table_verify(mapped, true, &count) != ESP_OK ||
        !aros_ab_resolve_candidate(mapped, count, partition, kernel, bsp,
                                   &candidate_slot)) {
        bootloader_munmap(mapped);
        return false;
    }
    bootloader_munmap(mapped);

    if (bsp[candidate_slot].size <= AROS_AB_MANIFEST_BYTES) {
        return false;
    }

    /* Manifest: magic/version, core/pkg lengths, core digest, package digest,
     * SHA-256 of bytes 0..79, and zero-reserved tail. The core digest covers
     * the complete raw IDF image including its appended image hash.
     */
    if (bootloader_flash_read(bsp[candidate_slot].offset, manifest_words,
                              sizeof(manifest_words), true) != ESP_OK) {
        return false;
    }
    if (memcmp(manifest, "P4ABPKG\x01", 8) != 0) {
        return false;
    }
    core_size = aros_ab_le32(manifest + 8);
    package_size = aros_ab_le32(manifest + 12);
    if (core_size != image->image_len ||
        package_size < 8 ||
        package_size > bsp[candidate_slot].size - AROS_AB_MANIFEST_BYTES ||
        package_size > AROS_AB_BSP_LIMIT) {
        return false;
    }
    for (i = AROS_AB_MANIFEST_HASHED + AROS_AB_DIGEST_BYTES;
         i < AROS_AB_MANIFEST_BYTES; ++i) {
        if (manifest[i] != 0) {
            return false;
        }
    }
    if (!aros_ab_hash_bytes(manifest, AROS_AB_MANIFEST_HASHED, calculated) ||
        memcmp(calculated, manifest + AROS_AB_MANIFEST_HASHED,
               AROS_AB_DIGEST_BYTES) != 0) {
        return false;
    }

    if (!aros_ab_hash_flash(partition->offset, core_size, partition->size, calculated) ||
        memcmp(calculated, manifest + 16, AROS_AB_DIGEST_BYTES) != 0) {
        return false;
    }

    if (bootloader_flash_read((size_t)bsp[candidate_slot].offset +
                              AROS_AB_MANIFEST_BYTES, pkg_prefix,
                              sizeof(pkg_prefix), true) != ESP_OK ||
        memcmp(pkg_prefix, "PKG\x01", 4) != 0 ||
        aros_ab_be32((const uint8_t *)pkg_prefix + 4) != package_size) {
        return false;
    }
    if (!aros_ab_hash_flash(bsp[candidate_slot].offset + AROS_AB_MANIFEST_BYTES,
                            package_size,
                            bsp[candidate_slot].size - AROS_AB_MANIFEST_BYTES,
                            calculated) ||
        memcmp(calculated, manifest + 48, AROS_AB_DIGEST_BYTES) != 0) {
        return false;
    }
    return true;
}

#endif /* AROS_AB_CHECK_H */
