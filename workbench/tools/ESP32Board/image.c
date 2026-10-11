/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#include "image.h"
#include <string.h>

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1]<<8 |
        (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24;
}
static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (uint16_t)p[1]<<8);
}
static int hex(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
enum board_image_error board_c6_image_check(const uint8_t *image, size_t bytes,
    const char *trusted_sha256, struct board_image_info *info)
{
    struct board_image_info checked;
    uint8_t expected[32], simple[32], sum = 0xef;
    size_t pos = 24, end, n, i, segment;
    int a, b;
    uintptr_t start, output;
    if (!image || !info || !trusted_sha256 || bytes < 288 ||
        bytes > BOARD_C6_IMAGE_MAX || strlen(trusted_sha256) != 64)
        return BOARD_IMAGE_ARGUMENT;
    start = (uintptr_t)image;
    output = (uintptr_t)info;
    if (start > UINTPTR_MAX - bytes || output > UINTPTR_MAX - sizeof(*info) ||
        (start < output + sizeof(*info) && output < start + bytes))
        return BOARD_IMAGE_ARGUMENT;
    for (i = 0; i < 32; ++i) {
        a = hex(trusted_sha256[i * 2]);
        b = hex(trusted_sha256[i * 2 + 1]);
        if (a < 0 || b < 0) return BOARD_IMAGE_ARGUMENT;
        expected[i] = (uint8_t)(a * 16 + b);
    }
    memset(&checked, 0, sizeof(checked));
    if (!board_sha256(image, bytes, checked.digest) ||
        memcmp(expected, checked.digest, 32)) return BOARD_IMAGE_HASH;
    /* IDF esp_image_header_t: 24 bytes; C6 chip_id=13 at offset12.
     * Application descriptor starts in the first segment at offset32. */
    if (image[0] != 0xe9 || !image[1] || image[1] > 16 || image[23] != 1)
        return BOARD_IMAGE_FORMAT;
    if (le16(image + 12) != 13) return BOARD_IMAGE_CHIP;
    for (segment = 0; segment < image[1]; ++segment) {
        if (bytes - pos < 8) return BOARD_IMAGE_FORMAT;
        n = le32(image + pos + 4);
        pos += 8;
        if (n > bytes - pos) return BOARD_IMAGE_FORMAT;
        if (segment == 0 && (n < 256 || le32(image + pos) != 0xabcd5432u))
            return BOARD_IMAGE_APP;
        for (i = 0; i < n; ++i) sum ^= image[pos + i];
        pos += n;
    }
    /* Checksum occupies the last byte of the next 16-byte block. */
    end = (pos + 16) & ~(size_t)15;
    if (end > bytes || bytes - end != 32) return BOARD_IMAGE_FORMAT;
    if (image[end - 1] != sum) return BOARD_IMAGE_CHECKSUM;
    if (!board_sha256(image, end, simple) || memcmp(simple, image + end, 32))
        return BOARD_IMAGE_APPENDED_HASH;
    checked.bytes = bytes;
    checked.secure_version = le32(image + 36);
    checked.min_revision = le16(image + 15);
    checked.max_revision = le16(image + 17);
    /* Pinned IDF esp_app_desc_t offsets within the first segment's
     * descriptor: min/max efuse block revisions at 176/178, then the
     * MMU page-size log2 at 180. The first segment begins at image offset 32. */
    checked.min_efuse_revision = le16(image + 32 + 176);
    checked.max_efuse_revision = le16(image + 32 + 178);
    checked.mmu_page_size = image[32 + 180];
    *info = checked;
    return BOARD_IMAGE_OK;
}
