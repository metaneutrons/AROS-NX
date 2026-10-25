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

static int overlaps(const void *a, size_t an, const void *b, size_t bn)
{
    uintptr_t av = (uintptr_t)a, bv = (uintptr_t)b;
    if (!a || !b || !an || !bn) return 0;
    return av <= bv ? bv - av < an : av - bv < bn;
}

static int reject(struct p4_hosted_init_diagnostic *diagnostic,
    enum p4_hosted_init_error error, size_t offset, uint8_t tag,
    uint8_t tag_size, uint8_t present)
{
    if (diagnostic) {
        struct p4_hosted_init_diagnostic detail = { 0 };
        detail.error = error;
        detail.offset = offset;
        detail.tag = tag;
        detail.tag_size = tag_size;
        detail.present = present;
        detail.declared_tlv_bytes = diagnostic->declared_tlv_bytes;
        *diagnostic = detail;
    }
    return 0;
}

int p4_hosted_init_parse_diagnostic(const uint8_t *event, size_t event_size,
    struct p4_hosted_init *result, struct p4_hosted_init_diagnostic *diagnostic)
{
    struct p4_hosted_init parsed = { 0 };
    size_t offset, remaining;
    uint8_t declared_size;

    if (diagnostic && (overlaps(diagnostic, sizeof(*diagnostic), event, event_size) ||
        overlaps(diagnostic, sizeof(*diagnostic), result, sizeof(*result))))
        return 0;
    if (diagnostic) {
        struct p4_hosted_init_diagnostic detail = { 0 };
        *diagnostic = detail;
    }
    if (!event || !result)
        return reject(diagnostic, P4_HOSTED_INIT_ERROR_ARGUMENT, 0, 0, 0, 0);
    if (event_size < P4_HOSTED_INIT_EVENT_HEADER_SIZE ||
        event_size > P4_HOSTED_INIT_MAX_EVENT_SIZE)
        return reject(diagnostic, P4_HOSTED_INIT_ERROR_EVENT_SIZE, 0, 0, 0, 0);
    if (event[0] != P4_HOSTED_INIT_EVENT_TYPE)
        return reject(diagnostic, P4_HOSTED_INIT_ERROR_EVENT_TYPE, 0, 0, 0, 0);

    declared_size = event[1];
    if (diagnostic) diagnostic->declared_tlv_bytes = declared_size;
    if (declared_size > P4_HOSTED_INIT_MAX_TLV_SIZE ||
        (size_t)declared_size != event_size - P4_HOSTED_INIT_EVENT_HEADER_SIZE)
        return reject(diagnostic, P4_HOSTED_INIT_ERROR_DECLARED_SIZE, 1, 0, 0, 0);

    offset = P4_HOSTED_INIT_EVENT_HEADER_SIZE;
    remaining = declared_size;
    while (remaining != 0) {
        const uint8_t *value;
        uint8_t tag, tag_size, field;
        size_t item_size;

        if (remaining < 2u)
            return reject(diagnostic, P4_HOSTED_INIT_ERROR_TLV_HEADER,
                offset, 0, 0, parsed.present);
        tag = event[offset];
        tag_size = event[offset + 1u];
        if ((size_t)tag_size > remaining - 2u)
            return reject(diagnostic, P4_HOSTED_INIT_ERROR_TLV_SIZE,
                offset, tag, tag_size, parsed.present);
        value = event + offset + 2u;
        item_size = 2u + (size_t)tag_size;
        field = 0;

#define FIELD_SIZE(expected) do { if (tag_size != (expected)) \
    return reject(diagnostic, P4_HOSTED_INIT_ERROR_FIELD_SIZE, \
        offset, tag, tag_size, parsed.present); } while (0)

        switch (tag) {
        case P4_HOSTED_INIT_TAG_CAPABILITIES:
            field = P4_HOSTED_INIT_HAS_CAPABILITIES;
            FIELD_SIZE(1u);
            parsed.capabilities = value[0];
            break;
        case P4_HOSTED_INIT_TAG_CHIP_ID:
            field = P4_HOSTED_INIT_HAS_CHIP_ID;
            FIELD_SIZE(1u);
            parsed.chip_id = value[0];
            break;
        case P4_HOSTED_INIT_TAG_TEST_RAW_TP:
            field = P4_HOSTED_INIT_HAS_TEST_RAW_TP;
            FIELD_SIZE(1u);
            parsed.test_raw_tp = value[0];
            break;
        case P4_HOSTED_INIT_TAG_RX_QUEUE_SIZE:
            field = P4_HOSTED_INIT_HAS_RX_QUEUE_SIZE;
            FIELD_SIZE(1u);
            parsed.rx_queue_size = value[0];
            break;
        case P4_HOSTED_INIT_TAG_TX_QUEUE_SIZE:
            field = P4_HOSTED_INIT_HAS_TX_QUEUE_SIZE;
            FIELD_SIZE(1u);
            parsed.tx_queue_size = value[0];
            break;
        case P4_HOSTED_INIT_TAG_EXTENDED_CAPABILITIES:
            field = P4_HOSTED_INIT_HAS_EXTENDED_CAPABILITIES;
            FIELD_SIZE(4u);
            parsed.extended_capabilities = read_le32(value);
            break;
        case P4_HOSTED_INIT_TAG_BOARD_DISCOVERY:
            field = P4_HOSTED_INIT_HAS_BOARD_DISCOVERY;
            FIELD_SIZE(1u);
            parsed.board_discovery_version = value[0];
            break;
        default:
            /* The pinned host skips unknown tags by their bounded TLV size. */
            break;
        }
#undef FIELD_SIZE

        if (field != 0) {
            if ((parsed.present & field) != 0)
                return reject(diagnostic, P4_HOSTED_INIT_ERROR_DUPLICATE,
                    offset, tag, tag_size, parsed.present);
            parsed.present = (uint8_t)(parsed.present | field);
        }
        offset += item_size;
        remaining -= item_size;
    }

    if ((parsed.present & (P4_HOSTED_INIT_HAS_CAPABILITIES |
                           P4_HOSTED_INIT_HAS_CHIP_ID)) !=
        (P4_HOSTED_INIT_HAS_CAPABILITIES | P4_HOSTED_INIT_HAS_CHIP_ID))
        return reject(diagnostic, P4_HOSTED_INIT_ERROR_REQUIRED_FIELDS,
            offset, 0, 0, parsed.present);
    if (!supported_chip(parsed.chip_id))
        return reject(diagnostic, P4_HOSTED_INIT_ERROR_CHIP,
            offset, P4_HOSTED_INIT_TAG_CHIP_ID, 1, parsed.present);
    if ((parsed.capabilities & P4_HOSTED_INIT_CAP_WLAN_SDIO) == 0)
        return reject(diagnostic, P4_HOSTED_INIT_ERROR_CAPABILITIES,
            offset, P4_HOSTED_INIT_TAG_CAPABILITIES, 1, parsed.present);

    *result = parsed;
    if (diagnostic) {
        struct p4_hosted_init_diagnostic detail = { 0 };
        *diagnostic = detail;
    }
    return 1;
}

int p4_hosted_init_parse(const uint8_t *event, size_t event_size,
                         struct p4_hosted_init *result)
{
    return p4_hosted_init_parse_diagnostic(event, event_size, result, NULL);
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
