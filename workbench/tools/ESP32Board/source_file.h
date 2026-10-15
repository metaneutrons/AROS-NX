/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#ifndef ESP32BOARD_SOURCE_FILE_H
#define ESP32BOARD_SOURCE_FILE_H
#include "image.h"

/* Tool-private owner, never a public radio ABI object. Initialize to zero.
 * Open once, close before publication, retain exact inspected bytes until
 * disposal. Caller keeps object/bytes immutable and exclusively owned through
 * recheck and synchronous submit. Concurrent mutation is unsupported.
 * Supplied digest is a trusted INPUT, not publisher authentication. A pass
 * supplies no target/layout/recovery/write authority. */
struct boardtool_c6_source {
    uint8_t *image;
    size_t size;
    struct board_image_info info;
    char expected_sha256[65];
};
/* Refusal leaves source unchanged. Empty source, inputs and status must be
 * disjoint. Positive short reads accumulate; EOF/error/trailing data refuse.
 * No DOS lock implies immutable file contents: verify the frozen bytes. */
int boardtool_c6_source_load(const char *path, const char *sha256,
    struct boardtool_c6_source *source, char *status, size_t capacity);
/* Recheck held bytes against original digest/metadata without reopening or
 * changing the object. Required before future submit; not manifest, installed
 * peer or recovery verification. */
int boardtool_c6_source_recheck(const struct boardtool_c6_source *source);
/* Only after synchronous submit has copied/refused bytes, or read-only use.
 * Never dispose while another task uses the source. Empty is idempotent. */
void boardtool_c6_source_dispose(struct boardtool_c6_source *source);
#endif
