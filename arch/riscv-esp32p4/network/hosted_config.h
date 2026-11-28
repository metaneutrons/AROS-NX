#ifndef P4_HOSTED_CONFIG_H
#define P4_HOSTED_CONFIG_H

/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */

#include <stddef.h>
#include <stdint.h>

#include "hosted_init.h"
#include "hosted_wire.h"

#define P4_HOSTED_CONFIG_PAYLOAD_SIZE 17u
#define P4_HOSTED_CONFIG_FRAME_SIZE \
    (P4_HOSTED_HEADER_SIZE + P4_HOSTED_CONFIG_PAYLOAD_SIZE)
#define P4_HOSTED_CONFIG_CAP_CHECKSUM 0x80u

/* Encode the pinned ESP-Hosted C6 SDIO startup host-configuration frame.
 * The input must be a parsed INIT for C6 with WLAN-over-SDIO capability;
 * a present raw-throughput field must be zero. Checksum is selected from
 * INIT capability bit 7. Queue fields do not select TX credits or a pool.
 *
 * Success writes the canonical 12-byte-header frame and stores its logical
 * size in *written. No SDIO block padding, allocation, I/O, or negotiation
 * proof is performed. Refusal leaves bytes and *written unchanged. The
 * nonempty init, output-capacity, and written ranges must be pairwise disjoint.
 */
int p4_hosted_config_encode(const struct p4_hosted_init *init,
                            uint8_t *bytes, size_t capacity, size_t *written);

#endif
