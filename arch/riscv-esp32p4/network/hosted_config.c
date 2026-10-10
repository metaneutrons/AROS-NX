/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#include "hosted_config.h"

static int ranges_overlap_or_overflow(const void *first, size_t first_size,
                                     const void *second, size_t second_size)
{
    uintptr_t first_address, second_address;

    if (first_size == 0 || second_size == 0)
        return 0;

    first_address = (uintptr_t)first;
    second_address = (uintptr_t)second;
    if ((uintmax_t)first_size >
            (uintmax_t)(UINTPTR_MAX - first_address) ||
        (uintmax_t)second_size >
            (uintmax_t)(UINTPTR_MAX - second_address))
        return 1;

    if (first_address <= second_address)
        return second_address - first_address < first_size;
    return first_address - second_address < second_size;
}

int p4_hosted_config_encode(const struct p4_hosted_init *init,
                            uint8_t *bytes, size_t capacity, size_t *written)
{
    static const uint8_t payload[P4_HOSTED_CONFIG_PAYLOAD_SIZE] = {
        0x22, 0x0f,
        0x44, 0x01, 0x00,
        0x45, 0x01, P4_HOSTED_INIT_CHIP_ESP32C6,
        0x46, 0x01, 0x00,
        0x47, 0x01, 0x00,
        0x48, 0x01, 0x00
    };
    const uint8_t required_fields = (uint8_t)(
        P4_HOSTED_INIT_HAS_CAPABILITIES | P4_HOSTED_INIT_HAS_CHIP_ID);
    const uint8_t allowed_fields = (uint8_t)(
        P4_HOSTED_INIT_HAS_CAPABILITIES | P4_HOSTED_INIT_HAS_CHIP_ID |
        P4_HOSTED_INIT_HAS_TEST_RAW_TP |
        P4_HOSTED_INIT_HAS_RX_QUEUE_SIZE |
        P4_HOSTED_INIT_HAS_TX_QUEUE_SIZE |
        P4_HOSTED_INIT_HAS_EXTENDED_CAPABILITIES);
    struct p4_hosted_frame frame;
    size_t encoded_size;
    unsigned checksum_enabled;

    if (!init || !bytes || !written)
        return 0;

    /* Check all caller-owned ranges before the first possible output write. */
    if (ranges_overlap_or_overflow(init, sizeof(*init), bytes, capacity) ||
        ranges_overlap_or_overflow(init, sizeof(*init), written,
                                   sizeof(*written)) ||
        ranges_overlap_or_overflow(bytes, capacity, written,
                                   sizeof(*written)))
        return 0;

    if ((init->present & required_fields) != required_fields ||
        (init->present & (uint8_t)~allowed_fields) != 0 ||
        init->chip_id != P4_HOSTED_INIT_CHIP_ESP32C6 ||
        (init->capabilities & P4_HOSTED_INIT_CAP_WLAN_SDIO) == 0 ||
        ((init->present & P4_HOSTED_INIT_HAS_TEST_RAW_TP) != 0 &&
         init->test_raw_tp != 0) ||
        capacity < P4_HOSTED_CONFIG_FRAME_SIZE)
        return 0;

    checksum_enabled =
        (init->capabilities & P4_HOSTED_CONFIG_CAP_CHECKSUM) != 0;
    frame.interface_type = P4_HOSTED_PRIVATE;
    frame.interface_number = 0;
    frame.flags = 0;
    frame.throttle = 0;
    frame.packet_type = 0;
    frame.sequence = 0;
    frame.payload = payload;
    frame.payload_size = sizeof(payload);
    frame.wire_size = 0;

    if (!p4_hosted_encode(bytes, capacity, checksum_enabled, &frame,
                          &encoded_size))
        return 0;

    *written = encoded_size;
    return 1;
}
