/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#ifndef ESP32BOARD_IMAGE_H
#define ESP32BOARD_IMAGE_H
#include <stddef.h>
#include <stdint.h>

/* Local memory/format ceiling, NOT the installed OTA partition capacity. */
#define BOARD_C6_IMAGE_MAX (1536u * 1024u)
enum board_image_error {
    BOARD_IMAGE_OK, BOARD_IMAGE_ARGUMENT, BOARD_IMAGE_HASH,
    BOARD_IMAGE_FORMAT, BOARD_IMAGE_CHIP, BOARD_IMAGE_APP,
    BOARD_IMAGE_CHECKSUM, BOARD_IMAGE_APPENDED_HASH
};
struct board_image_info {
    size_t bytes;
    uint32_t secure_version;
    uint16_t min_revision, max_revision;
    uint8_t digest[32];
};
/* Corruption/format preflight only. No installed identity, partition capacity,
 * protocol compatibility, signature authenticity or recovery claim. Image
 * must be immutable and disjoint from info for the duration of this call.
 * info is unchanged on refusal. Only unsigned C6 app images with an appended
 * simple hash are supported; signed/encrypted/merged images are refused. */
enum board_image_error board_c6_image_check(const uint8_t *image, size_t bytes,
    const char *trusted_sha256, struct board_image_info *info);
int board_sha256(const uint8_t *data, size_t size, uint8_t digest[32]);
#endif
