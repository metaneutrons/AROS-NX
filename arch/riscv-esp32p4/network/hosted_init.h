#ifndef P4_HOSTED_INIT_H
#define P4_HOSTED_INIT_H

/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */

#include <stddef.h>
#include <stdint.h>

#define P4_HOSTED_INIT_EVENT_TYPE 0x22u
#define P4_HOSTED_INIT_MAX_TLV_SIZE 64u
#define P4_HOSTED_INIT_EVENT_HEADER_SIZE 2u
#define P4_HOSTED_INIT_MAX_EVENT_SIZE \
    (P4_HOSTED_INIT_EVENT_HEADER_SIZE + P4_HOSTED_INIT_MAX_TLV_SIZE)

#define P4_HOSTED_INIT_TAG_CAPABILITIES 0x11u
#define P4_HOSTED_INIT_TAG_CHIP_ID 0x12u
#define P4_HOSTED_INIT_TAG_TEST_RAW_TP 0x13u
#define P4_HOSTED_INIT_TAG_RX_QUEUE_SIZE 0x14u
#define P4_HOSTED_INIT_TAG_TX_QUEUE_SIZE 0x15u
#define P4_HOSTED_INIT_TAG_EXTENDED_CAPABILITIES 0x16u
#define P4_HOSTED_INIT_TAG_BOARD_DISCOVERY 0x17u

#define P4_HOSTED_INIT_BOARD_DISCOVERY_VERSION_1 1u

#define P4_HOSTED_INIT_CHIP_ESP32 0x00u
#define P4_HOSTED_INIT_CHIP_ESP32S2 0x02u
#define P4_HOSTED_INIT_CHIP_ESP32C3 0x05u
#define P4_HOSTED_INIT_CHIP_ESP32S3 0x09u
#define P4_HOSTED_INIT_CHIP_ESP32C2 0x0cu
#define P4_HOSTED_INIT_CHIP_ESP32C6 0x0du
#define P4_HOSTED_INIT_CHIP_ESP32C5 0x17u

#define P4_HOSTED_INIT_CAP_WLAN_SDIO 0x01u

enum p4_hosted_init_present {
    P4_HOSTED_INIT_HAS_CAPABILITIES = 1u << 0,
    P4_HOSTED_INIT_HAS_CHIP_ID = 1u << 1,
    P4_HOSTED_INIT_HAS_TEST_RAW_TP = 1u << 2,
    P4_HOSTED_INIT_HAS_RX_QUEUE_SIZE = 1u << 3,
    P4_HOSTED_INIT_HAS_TX_QUEUE_SIZE = 1u << 4,
    P4_HOSTED_INIT_HAS_EXTENDED_CAPABILITIES = 1u << 5,
    P4_HOSTED_INIT_HAS_BOARD_DISCOVERY = 1u << 6
};

/* Parsed values only; this is not a wire-layout structure. */
struct p4_hosted_init {
    uint8_t chip_id;
    uint8_t capabilities;
    uint8_t test_raw_tp;
    uint8_t rx_queue_size;
    uint8_t tx_queue_size;
    uint32_t extended_capabilities;
    uint8_t board_discovery_version;
    uint8_t present;
};

/* Parse a complete private INIT event payload after frame decoding.
 * The payload is [event type, TLV byte count, TLVs...]. Known singleton
 * fields must have their protocol-defined lengths and occur once. The board
 * discovery version is retained as reported, including unsupported values.
 * Unknown TLVs are skipped when they fit inside the 64-byte reference limit.
 * Requires a supported ESP chip and WLAN-over-SDIO capability. Returns 1 on
 * success; on refusal, *result is unchanged. No wire structs or allocation. */
int p4_hosted_init_parse(const uint8_t *event, size_t event_size,
                         struct p4_hosted_init *result);

/* True only for version 1 discovery on an ESP32-C6 with valid WLAN-over-SDIO
 * capability facts. Unsupported versions remain available in the parsed
 * inventory but do not enable discovery. */
int p4_hosted_init_has_board_discovery(const struct p4_hosted_init *init);

#endif
