/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hosted_config.h"

#define TEST_BUFFER_SIZE 64u

static unsigned long checks;

#define CHECK(expression) do { \
    ++checks; \
    if (!(expression)) { \
        fprintf(stderr, "line %u: %s\n", (unsigned int)__LINE__, #expression); \
        exit(1); \
    } \
} while (0)

static const uint8_t c6_init_event[] = {
    0x22, 0x06,
    0x11, 0x01, 0x0d,
    0x12, 0x01, 0x0d
};

static const uint8_t c6_config_without_checksum[] = {
    0x05, 0x00, 0x11, 0x00, 0x0c, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x22, 0x0f,
    0x44, 0x01, 0x00,
    0x45, 0x01, 0x0d,
    0x46, 0x01, 0x00,
    0x47, 0x01, 0x00,
    0x48, 0x01, 0x00
};

static const uint8_t c6_config_with_checksum[] = {
    0x05, 0x00, 0x11, 0x00, 0x0c, 0x00, 0xc3, 0x01,
    0x00, 0x00, 0x00, 0x00,
    0x22, 0x0f,
    0x44, 0x01, 0x00,
    0x45, 0x01, 0x0d,
    0x46, 0x01, 0x00,
    0x47, 0x01, 0x00,
    0x48, 0x01, 0x00
};

static uint16_t test_checksum(const uint8_t *bytes, size_t length)
{
    uint32_t sum = 0;
    size_t index;

    for (index = 0; index < length; ++index)
        if (index != 6u && index != 7u)
            sum += bytes[index];
    return (uint16_t)sum;
}

static uint16_t test_get16(const uint8_t *bytes)
{
    return (uint16_t)((unsigned)bytes[0] | ((unsigned)bytes[1] << 8));
}

static void make_init(uint8_t capabilities, struct p4_hosted_init *init)
{
    uint8_t event[sizeof(c6_init_event)];

    memcpy(event, c6_init_event, sizeof(event));
    event[4] = capabilities;
    CHECK(p4_hosted_init_parse(event, sizeof(event), init));
}

static void check_decode(const uint8_t *wire, unsigned checksum_enabled)
{
    struct p4_hosted_frame frame;

    CHECK(p4_hosted_decode(wire, P4_HOSTED_CONFIG_FRAME_SIZE,
                           checksum_enabled, &frame));
    CHECK(frame.interface_type == P4_HOSTED_PRIVATE);
    CHECK(frame.interface_number == 0);
    CHECK(frame.flags == 0);
    CHECK(frame.throttle == 0);
    CHECK(frame.sequence == 0);
    CHECK(frame.packet_type == 0);
    CHECK(frame.payload_size == P4_HOSTED_CONFIG_PAYLOAD_SIZE);
    CHECK(frame.wire_size == P4_HOSTED_CONFIG_FRAME_SIZE);
    CHECK(frame.payload == wire + P4_HOSTED_HEADER_SIZE);
    CHECK(frame.payload[0] == 0x22 && frame.payload[1] == 0x0f);
    CHECK(frame.payload[2] == 0x44 && frame.payload[5] == 0x45);
    CHECK(frame.payload[8] == 0x46 && frame.payload[11] == 0x47);
    CHECK(frame.payload[14] == 0x48);
    if (checksum_enabled)
        CHECK(test_checksum(wire, P4_HOSTED_CONFIG_FRAME_SIZE) ==
              test_get16(wire + 6));
}

static void check_encode_matches(const struct p4_hosted_init *init,
                                 const uint8_t *expected)
{
    uint8_t output[TEST_BUFFER_SIZE];
    size_t written = (size_t)0x9876u;
    unsigned checksum_enabled =
        (init->capabilities & P4_HOSTED_CONFIG_CAP_CHECKSUM) != 0;

    memset(output, 0xa6, sizeof(output));
    CHECK(p4_hosted_config_encode(init, output, sizeof(output), &written));
    CHECK(written == P4_HOSTED_CONFIG_FRAME_SIZE);
    CHECK(memcmp(output, expected, P4_HOSTED_CONFIG_FRAME_SIZE) == 0);
    CHECK(output[P4_HOSTED_CONFIG_FRAME_SIZE] == 0xa6);
    CHECK(output[P4_HOSTED_CONFIG_FRAME_SIZE + 1u] == 0xa6);
    check_decode(output, checksum_enabled);
}

static void test_independent_vectors_and_decode_back(void)
{
    struct p4_hosted_init init;

    CHECK(sizeof(c6_config_without_checksum) == P4_HOSTED_CONFIG_FRAME_SIZE);
    CHECK(sizeof(c6_config_with_checksum) == P4_HOSTED_CONFIG_FRAME_SIZE);
    CHECK(test_checksum(c6_config_without_checksum,
                        sizeof(c6_config_without_checksum)) == 0x01c3u);
    CHECK(test_checksum(c6_config_with_checksum,
                        sizeof(c6_config_with_checksum)) == 0x01c3u);
    CHECK(test_get16(c6_config_with_checksum + 6) == 0x01c3u);

    make_init(0x0d, &init);
    check_encode_matches(&init, c6_config_without_checksum);

    make_init(0x8d, &init);
    check_encode_matches(&init, c6_config_with_checksum);
}

static void expect_refusal_unchanged(const struct p4_hosted_init *init,
                                     size_t capacity)
{
    uint8_t output[TEST_BUFFER_SIZE];
    uint8_t before[TEST_BUFFER_SIZE];
    size_t written = (size_t)0x76543210u;

    memset(output, 0x5c, sizeof(output));
    memcpy(before, output, sizeof(output));
    CHECK(!p4_hosted_config_encode(init, output, capacity, &written));
    CHECK(memcmp(output, before, sizeof(output)) == 0);
    CHECK(written == (size_t)0x76543210u);
}

static void test_capacities_and_nulls(void)
{
    struct p4_hosted_init init;
    uint8_t output[TEST_BUFFER_SIZE];
    size_t written;
    size_t capacity;

    make_init(0x0d, &init);
    for (capacity = 0; capacity < P4_HOSTED_CONFIG_FRAME_SIZE; ++capacity)
        expect_refusal_unchanged(&init, capacity);

    memset(output, 0x9a, sizeof(output));
    written = (size_t)0x76543210u;
    CHECK(!p4_hosted_config_encode(NULL, output, sizeof(output), &written));
    CHECK(output[0] == 0x9a && written == (size_t)0x76543210u);

    CHECK(!p4_hosted_config_encode(&init, NULL, sizeof(output), &written));
    CHECK(output[0] == 0x9a && written == (size_t)0x76543210u);

    CHECK(!p4_hosted_config_encode(&init, output, sizeof(output), NULL));
    CHECK(output[0] == 0x9a);

    written = (size_t)0x76543210u;
    CHECK(p4_hosted_config_encode(&init, output,
                                  P4_HOSTED_CONFIG_FRAME_SIZE, &written));
    CHECK(written == P4_HOSTED_CONFIG_FRAME_SIZE);
}

static void test_mutated_init_profiles(void)
{
    struct p4_hosted_init init, candidate;

    make_init(0x0d, &init);
    candidate = init;
    candidate.chip_id = P4_HOSTED_INIT_CHIP_ESP32C3;
    expect_refusal_unchanged(&candidate, TEST_BUFFER_SIZE);

    candidate = init;
    candidate.capabilities = 0x8c;
    expect_refusal_unchanged(&candidate, TEST_BUFFER_SIZE);

    candidate = init;
    candidate.present = (uint8_t)(candidate.present &
                                  (uint8_t)~P4_HOSTED_INIT_HAS_CHIP_ID);
    expect_refusal_unchanged(&candidate, TEST_BUFFER_SIZE);

    candidate = init;
    candidate.present = (uint8_t)(candidate.present &
                                  (uint8_t)~P4_HOSTED_INIT_HAS_CAPABILITIES);
    expect_refusal_unchanged(&candidate, TEST_BUFFER_SIZE);

    candidate = init;
    candidate.present = (uint8_t)(candidate.present | 0x40u);
    expect_refusal_unchanged(&candidate, TEST_BUFFER_SIZE);

    candidate = init;
    candidate.present = (uint8_t)(candidate.present |
                                  P4_HOSTED_INIT_HAS_TEST_RAW_TP);
    candidate.test_raw_tp = 1;
    expect_refusal_unchanged(&candidate, TEST_BUFFER_SIZE);

    candidate.test_raw_tp = 0;
    check_encode_matches(&candidate, c6_config_without_checksum);

    /* Queue metadata and extended capabilities do not tune the frame. */
    candidate = init;
    candidate.present = (uint8_t)(candidate.present |
        P4_HOSTED_INIT_HAS_RX_QUEUE_SIZE | P4_HOSTED_INIT_HAS_TX_QUEUE_SIZE |
        P4_HOSTED_INIT_HAS_EXTENDED_CAPABILITIES);
    candidate.rx_queue_size = 0xff;
    candidate.tx_queue_size = 0;
    candidate.extended_capabilities = UINT32_MAX;
    check_encode_matches(&candidate, c6_config_without_checksum);

    /* The checksum bit alone selects the checksum field. */
    candidate = init;
    candidate.capabilities = (uint8_t)(0x0d |
                                       P4_HOSTED_CONFIG_CAP_CHECKSUM);
    check_encode_matches(&candidate, c6_config_with_checksum);

    candidate.capabilities = 0x8c;
    expect_refusal_unchanged(&candidate, TEST_BUFFER_SIZE);
}

static void test_received_nonzero_raw_throughput(void)
{
    static const uint8_t raw_throughput_init[] = {
        0x22, 0x09,
        0x11, 0x01, 0x0d,
        0x12, 0x01, 0x0d,
        0x13, 0x01, 0x01
    };
    struct p4_hosted_init init;

    CHECK(p4_hosted_init_parse(raw_throughput_init,
                               sizeof(raw_throughput_init), &init));
    CHECK((init.present & P4_HOSTED_INIT_HAS_TEST_RAW_TP) != 0);
    CHECK(init.test_raw_tp == 1);
    expect_refusal_unchanged(&init, TEST_BUFFER_SIZE);
}

static void test_metadata_output_and_written_aliases(void)
{
    union init_output {
        struct p4_hosted_init init;
        uint8_t bytes[TEST_BUFFER_SIZE];
    } init_output_alias;
    union init_written {
        struct p4_hosted_init init;
        size_t written;
        uint8_t bytes[TEST_BUFFER_SIZE];
    } init_written_alias;
    union output_written {
        size_t written;
        uint8_t bytes[TEST_BUFFER_SIZE];
    } output_written_alias;
    struct p4_hosted_init separate_init;
    uint8_t output[TEST_BUFFER_SIZE];
    uint8_t before[TEST_BUFFER_SIZE];
    uint8_t output_before[TEST_BUFFER_SIZE];
    size_t written;

    CHECK(p4_hosted_init_parse(c6_init_event, sizeof(c6_init_event),
                               &init_output_alias.init));
    memcpy(before, init_output_alias.bytes, sizeof(before));
    written = (size_t)0x13579bdfu;
    CHECK(!p4_hosted_config_encode(&init_output_alias.init,
                                   init_output_alias.bytes,
                                   sizeof(init_output_alias.bytes), &written));
    CHECK(memcmp(init_output_alias.bytes, before, sizeof(before)) == 0);
    CHECK(written == (size_t)0x13579bdfu);

    CHECK(p4_hosted_init_parse(c6_init_event, sizeof(c6_init_event),
                               &init_written_alias.init));
    memcpy(before, init_written_alias.bytes, sizeof(before));
    memset(output, 0x6d, sizeof(output));
    memcpy(output_before, output, sizeof(output));
    CHECK(!p4_hosted_config_encode(&init_written_alias.init, output,
                                   sizeof(output),
                                   &init_written_alias.written));
    CHECK(memcmp(init_written_alias.bytes, before, sizeof(before)) == 0);
    CHECK(memcmp(output, output_before, sizeof(output)) == 0);

    memset(output_written_alias.bytes, 0xb3,
           sizeof(output_written_alias.bytes));
    memcpy(before, output_written_alias.bytes, sizeof(before));
    make_init(0x0d, &separate_init);
    CHECK(!p4_hosted_config_encode(&separate_init,
                                   output_written_alias.bytes,
                                   sizeof(output_written_alias.bytes),
                                   &output_written_alias.written));
    CHECK(memcmp(output_written_alias.bytes, before, sizeof(before)) == 0);
}

static void test_overflowing_capacity(void)
{
    struct p4_hosted_init init;
    uint8_t output[TEST_BUFFER_SIZE];
    uint8_t before[TEST_BUFFER_SIZE];
    size_t written = (size_t)0x2468ace0u;

    make_init(0x0d, &init);
    memset(output, 0x87, sizeof(output));
    memcpy(before, output, sizeof(before));
    CHECK(!p4_hosted_config_encode(&init, output, SIZE_MAX, &written));
    CHECK(memcmp(output, before, sizeof(output)) == 0);
    CHECK(written == (size_t)0x2468ace0u);
}

static uint32_t random_state = UINT32_C(0x6c8e9cf5);

static uint32_t next_random(void)
{
    random_state = random_state * UINT32_C(1664525) + UINT32_C(1013904223);
    return random_state;
}

static int profile_should_encode(const struct p4_hosted_init *init,
                                 size_t capacity)
{
    const uint8_t required = (uint8_t)(
        P4_HOSTED_INIT_HAS_CAPABILITIES | P4_HOSTED_INIT_HAS_CHIP_ID);
    const uint8_t known = (uint8_t)(
        P4_HOSTED_INIT_HAS_CAPABILITIES | P4_HOSTED_INIT_HAS_CHIP_ID |
        P4_HOSTED_INIT_HAS_TEST_RAW_TP |
        P4_HOSTED_INIT_HAS_RX_QUEUE_SIZE |
        P4_HOSTED_INIT_HAS_TX_QUEUE_SIZE |
        P4_HOSTED_INIT_HAS_EXTENDED_CAPABILITIES);

    return (init->present & required) == required &&
        (init->present & (uint8_t)~known) == 0 &&
        init->chip_id == P4_HOSTED_INIT_CHIP_ESP32C6 &&
        (init->capabilities & P4_HOSTED_INIT_CAP_WLAN_SDIO) != 0 &&
        ((init->present & P4_HOSTED_INIT_HAS_TEST_RAW_TP) == 0 ||
         init->test_raw_tp == 0) &&
        capacity >= P4_HOSTED_CONFIG_FRAME_SIZE;
}

static void test_fuzz_mutated_init_profiles(void)
{
    unsigned iteration;

    for (iteration = 0; iteration < 40000u; ++iteration) {
        struct p4_hosted_init init;
        uint8_t output[TEST_BUFFER_SIZE];
        uint8_t before[TEST_BUFFER_SIZE];
        const uint8_t *expected;
        size_t capacity = (size_t)(next_random() % (TEST_BUFFER_SIZE + 1u));
        size_t written = (size_t)next_random();
        size_t previous_written = written;
        int expected_result, actual_result;
        size_t index;

        memset(&init, 0, sizeof(init));
        init.chip_id = (uint8_t)next_random();
        init.capabilities = (uint8_t)next_random();
        init.test_raw_tp = (uint8_t)next_random();
        init.rx_queue_size = (uint8_t)next_random();
        init.tx_queue_size = (uint8_t)next_random();
        init.extended_capabilities = next_random();
        init.present = (uint8_t)next_random();
        for (index = 0; index < sizeof(output); ++index)
            output[index] = (uint8_t)next_random();
        memcpy(before, output, sizeof(before));

        expected_result = profile_should_encode(&init, capacity);
        actual_result = p4_hosted_config_encode(&init, output, capacity,
                                                 &written);
        CHECK(actual_result == expected_result);
        if (!actual_result) {
            CHECK(memcmp(output, before, sizeof(output)) == 0);
            CHECK(written == previous_written);
            continue;
        }

        expected = (init.capabilities & P4_HOSTED_CONFIG_CAP_CHECKSUM) != 0 ?
            c6_config_with_checksum : c6_config_without_checksum;
        CHECK(written == P4_HOSTED_CONFIG_FRAME_SIZE);
        CHECK(memcmp(output, expected, P4_HOSTED_CONFIG_FRAME_SIZE) == 0);
        CHECK(memcmp(output + P4_HOSTED_CONFIG_FRAME_SIZE,
                     before + P4_HOSTED_CONFIG_FRAME_SIZE,
                     sizeof(output) - P4_HOSTED_CONFIG_FRAME_SIZE) == 0);
        check_decode(output,
                     (init.capabilities & P4_HOSTED_CONFIG_CAP_CHECKSUM) != 0);
    }
}

int main(void)
{
    test_independent_vectors_and_decode_back();
    test_capacities_and_nulls();
    test_mutated_init_profiles();
    test_received_nonzero_raw_throughput();
    test_metadata_output_and_written_aliases();
    test_overflowing_capacity();
    test_fuzz_mutated_init_profiles();
    printf("hosted-config: %lu checks\n", checks);
    return 0;
}
