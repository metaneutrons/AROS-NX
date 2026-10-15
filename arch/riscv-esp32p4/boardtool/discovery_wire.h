/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#ifndef P4_BOARD_DISCOVERY_WIRE_H
#define P4_BOARD_DISCOVERY_WIRE_H
#include "discovery.h"

/* Version2 byte payload for the negotiated Hosted extension. Not a raw
 * partition table. The enclosing RPC must correlate UID and owner/peer epoch.
 * Nonzero nonce is echoed to prevent accidental cross-request publication;
 * it is not authentication or a substitute for epoch correlation. */
#define BOARD_DISCOVERY_WIRE_VERSION 2u
#define BOARD_DISCOVERY_WIRE_HEADER 40u
#define BOARD_DISCOVERY_WIRE_RECORD 32u
#define BOARD_DISCOVERY_WIRE_MAX (40u + 32u * BOARD_LAYOUT_MAX_PARTITIONS)

/* Exact canonical length; little-endian integers; zero reserved/padding bytes.
 * Encoded inventory is structurally validated. Input/output/written ranges
 * must be disjoint. Refusal leaves output bytes and written unchanged. */
int board_discovery_encode(const struct board_discovery_snapshot *snapshot,
    uint32_t nonce, uint8_t *bytes, size_t capacity, size_t *written);
/* Nonzero expected nonce must match; caller separately verifies negotiated
 * capability, RPC response ID/UID and live epoch. Refusal preserves output.
 * Output must not overlap the input. No writes or update authorization. */
int board_discovery_decode(const uint8_t *bytes, size_t size,
    uint32_t expected_nonce, struct board_discovery_snapshot *snapshot);
#endif
