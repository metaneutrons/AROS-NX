/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#include "hosted_init.h"

static int supported_chip(uint8_t chip_id)
{
    switch (chip_id) {
    case P4_HOSTED_INIT_CHIP_ESP32:
    case P4_HOSTED_INIT_CHIP_ESP32S2:
    case P4_HOSTED_INIT_CHIP_ESP32C3:
    case P4_HOSTED_INIT_CHIP_ESP32S3:
    case P4_HOSTED_INIT_CHIP_ESP32C2:
    case P4_HOSTED_INIT_CHIP_ESP32C6:
    case P4_HOSTED_INIT_CHIP_ESP32C5:
        return 1;
    default:
        return 0;
    }
}

static uint32_t read_le32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] |
        ((uint32_t)bytes[1] << 8) |
        ((uint32_t)bytes[2] << 16) |
        ((uint32_t)bytes[3] << 24);
}

int p4_hosted_init_parse(const uint8_t *event, size_t event_size,
                         struct p4_hosted_init *result)
{
    struct p4_hosted_init parsed = { 0 };
    size_t offset, remaining;
    uint8_t declared_size;

    if (!event || !result ||
        event_size < P4_HOSTED_INIT_EVENT_HEADER_SIZE ||
        event_size > P4_HOSTED_INIT_MAX_EVENT_SIZE ||
        event[0] != P4_HOSTED_INIT_EVENT_TYPE)
        return 0;

    declared_size = event[1];
    if (declared_size > P4_HOSTED_INIT_MAX_TLV_SIZE ||
        (size_t)declared_size != event_size - P4_HOSTED_INIT_EVENT_HEADER_SIZE)
        return 0;

    offset = P4_HOSTED_INIT_EVENT_HEADER_SIZE;
    remaining = declared_size;
    while (remaining != 0) {
        const uint8_t *value;
        uint8_t tag, tag_size, field;
        size_t item_size;

        if (remaining < 2u)
            return 0;
        tag = event[offset];
        tag_size = event[offset + 1u];
        if ((size_t)tag_size > remaining - 2u)
            return 0;
        value = event + offset + 2u;
        item_size = 2u + (size_t)tag_size;
        field = 0;

        switch (tag) {
        case P4_HOSTED_INIT_TAG_CAPABILITIES:
            field = P4_HOSTED_INIT_HAS_CAPABILITIES;
            if (tag_size != 1u)
                return 0;
            parsed.capabilities = value[0];
            break;
        case P4_HOSTED_INIT_TAG_CHIP_ID:
            field = P4_HOSTED_INIT_HAS_CHIP_ID;
            if (tag_size != 1u)
                return 0;
            parsed.chip_id = value[0];
            break;
        case P4_HOSTED_INIT_TAG_TEST_RAW_TP:
            field = P4_HOSTED_INIT_HAS_TEST_RAW_TP;
            if (tag_size != 1u)
                return 0;
            parsed.test_raw_tp = value[0];
            break;
        case P4_HOSTED_INIT_TAG_RX_QUEUE_SIZE:
            field = P4_HOSTED_INIT_HAS_RX_QUEUE_SIZE;
            if (tag_size != 1u)
                return 0;
            parsed.rx_queue_size = value[0];
            break;
        case P4_HOSTED_INIT_TAG_TX_QUEUE_SIZE:
            field = P4_HOSTED_INIT_HAS_TX_QUEUE_SIZE;
            if (tag_size != 1u)
                return 0;
            parsed.tx_queue_size = value[0];
            break;
        case P4_HOSTED_INIT_TAG_EXTENDED_CAPABILITIES:
            field = P4_HOSTED_INIT_HAS_EXTENDED_CAPABILITIES;
            if (tag_size != 4u)
                return 0;
            parsed.extended_capabilities = read_le32(value);
            break;
        case P4_HOSTED_INIT_TAG_BOARD_DISCOVERY:
            field = P4_HOSTED_INIT_HAS_BOARD_DISCOVERY;
            if (tag_size != 1u)
                return 0;
            parsed.board_discovery_version = value[0];
            break;
        default:
            /* The pinned host skips unknown tags by their bounded TLV size. */
            break;
        }

        if (field != 0) {
            if ((parsed.present & field) != 0)
                return 0;
            parsed.present = (uint8_t)(parsed.present | field);
        }
        offset += item_size;
        remaining -= item_size;
    }

    if ((parsed.present & (P4_HOSTED_INIT_HAS_CAPABILITIES |
                           P4_HOSTED_INIT_HAS_CHIP_ID)) !=
        (P4_HOSTED_INIT_HAS_CAPABILITIES | P4_HOSTED_INIT_HAS_CHIP_ID) ||
        !supported_chip(parsed.chip_id) ||
        (parsed.capabilities & P4_HOSTED_INIT_CAP_WLAN_SDIO) == 0)
        return 0;

    *result = parsed;
    return 1;
}

int p4_hosted_init_has_board_discovery(const struct p4_hosted_init *init)
{
    const uint8_t required = P4_HOSTED_INIT_HAS_BOARD_DISCOVERY |
        P4_HOSTED_INIT_HAS_CAPABILITIES | P4_HOSTED_INIT_HAS_CHIP_ID;

    return init && (init->present & required) == required &&
        init->board_discovery_version ==
            P4_HOSTED_INIT_BOARD_DISCOVERY_VERSION_CURRENT &&
        init->chip_id == P4_HOSTED_INIT_CHIP_ESP32C6 &&
        (init->capabilities & P4_HOSTED_INIT_CAP_WLAN_SDIO) != 0;
}
