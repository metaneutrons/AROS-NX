/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#include "hosted_wire.h"

#define TEST_CAPACITY (P4_HOSTED_FRAME_MAX + 32u)

static unsigned long checks;

#define CHECK(expression) do { \
    ++checks; \
    if (!(expression)) { \
        fprintf(stderr, "line %u: %s\n", (unsigned int)__LINE__, #expression); \
        exit(1); \
    } \
} while (0)

static uint16_t test_get16(const uint8_t *p)
{
    return (uint16_t)((unsigned)p[0] | ((unsigned)p[1] << 8));
}

static void test_put16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

/* Deliberately separate from the codec's implementation. */
static uint16_t test_checksum(const uint8_t *bytes, size_t length)
{
    uint32_t sum = 0;
    size_t i;

    for (i = 0; i < length; ++i)
        if (i != 6 && i != 7)
            sum += bytes[i];
    return (uint16_t)sum;
}

static void set_header(uint8_t *bytes, uint8_t type, uint8_t number,
                       uint8_t flags, uint16_t payload_size, uint16_t offset,
                       uint16_t sequence, uint8_t throttle,
                       uint8_t packet_type)
{
    bytes[0] = (uint8_t)((number << 4) | type);
    bytes[1] = flags;
    test_put16(bytes + 2, payload_size);
    test_put16(bytes + 4, offset);
    test_put16(bytes + 6, 0);
    test_put16(bytes + 8, sequence);
    bytes[10] = throttle;
    bytes[11] = packet_type;
}

static void make_wire(uint8_t *bytes, size_t offset, size_t payload_size,
                      uint8_t type, uint8_t number, uint8_t flags,
                      uint16_t sequence, uint8_t throttle,
                      uint8_t packet_type, const uint8_t *payload,
                      unsigned checksum_enabled)
{
    size_t total = offset + payload_size;
    size_t i;

    memset(bytes, 0x6d, total);
    set_header(bytes, type, number, flags, (uint16_t)payload_size,
               (uint16_t)offset, sequence, throttle, packet_type);
    for (i = 0; i < payload_size; ++i)
        bytes[offset + i] = payload ? payload[i] : (uint8_t)(i * 29u + 7u);
    if (checksum_enabled)
        test_put16(bytes + 6, test_checksum(bytes, total));
}

static void expect_decode_refusal(const uint8_t *bytes, size_t available,
                                  unsigned checksum_enabled)
{
    struct p4_hosted_frame frame;
    uint8_t before[sizeof frame];

    memset(&frame, 0xa5, sizeof frame);
    memcpy(before, &frame, sizeof frame);
    CHECK(!p4_hosted_decode(bytes, available, checksum_enabled, &frame));
    CHECK(memcmp(&frame, before, sizeof frame) == 0);
}

static void check_decoded_frame(const uint8_t *bytes, size_t available,
                                unsigned checksum_enabled)
{
    struct p4_hosted_frame frame;
    uint8_t before[sizeof frame];
    size_t payload_offset;

    memset(&frame, 0x5a, sizeof frame);
    memcpy(before, &frame, sizeof frame);
    if (!p4_hosted_decode(bytes, available, checksum_enabled, &frame)) {
        CHECK(memcmp(&frame, before, sizeof frame) == 0);
        return;
    }
    CHECK(frame.interface_type >= P4_HOSTED_STA);
    CHECK(frame.interface_type <= P4_HOSTED_ETH);
    CHECK(frame.interface_number <= 15);
    CHECK(frame.flags <= P4_HOSTED_MORE_FRAGMENT);
    CHECK(frame.throttle <= 2);
    CHECK(frame.payload_size != 0);
    CHECK(frame.wire_size <= P4_HOSTED_FRAME_MAX);
    CHECK(frame.wire_size <= available);
    CHECK(frame.payload >= bytes);
    CHECK(frame.payload <= bytes + available);
    payload_offset = (size_t)(frame.payload - bytes);
    CHECK(frame.payload_size <= available - payload_offset);
    CHECK(payload_offset + frame.payload_size == frame.wire_size);
    if (checksum_enabled == 1)
        CHECK(test_checksum(bytes, frame.wire_size) == test_get16(bytes + 6));
}

static uint32_t random_state = UINT32_C(0x72a4c19d);

static uint32_t next_random(void)
{
    random_state = random_state * UINT32_C(1664525) + UINT32_C(1013904223);
    return random_state;
}

static void test_known_vector(void)
{
    static const uint8_t wire[] = {
        0x21, 0x01, 0x03, 0x00, 0x0c, 0x00, 0x83, 0x01,
        0x34, 0x12, 0x02, 0xaa, 0x10, 0x20, 0x30
    };
    struct p4_hosted_frame frame;
    uint8_t encoded[TEST_CAPACITY];
    uint8_t damaged[sizeof wire];
    size_t written = 0;

    CHECK(test_checksum(wire, sizeof wire) == UINT16_C(0x0183));
    CHECK(p4_hosted_decode(wire, sizeof wire, 1, &frame));
    CHECK(frame.interface_type == P4_HOSTED_STA);
    CHECK(frame.interface_number == 2);
    CHECK(frame.flags == P4_HOSTED_MORE_FRAGMENT);
    CHECK(frame.throttle == 2);
    CHECK(frame.packet_type == 0xaa);
    CHECK(frame.sequence == UINT16_C(0x1234));
    CHECK(frame.payload == wire + P4_HOSTED_HEADER_SIZE);
    CHECK(frame.payload_size == 3);
    CHECK(frame.wire_size == sizeof wire);
    CHECK(memcmp(frame.payload, "\x10\x20\x30", 3) == 0);

    memset(encoded, 0xcc, sizeof encoded);
    CHECK(p4_hosted_encode(encoded, sizeof encoded, 1, &frame, &written));
    CHECK(written == sizeof wire);
    CHECK(memcmp(encoded, wire, sizeof wire) == 0);
    CHECK(encoded[written] == 0xcc);

    memcpy(damaged, wire, sizeof wire);
    damaged[6] ^= 1;
    expect_decode_refusal(damaged, sizeof damaged, 1);
    CHECK(p4_hosted_decode(damaged, sizeof damaged, 0, &frame));
    CHECK(frame.wire_size == sizeof wire);

    memcpy(damaged, wire, sizeof wire);
    damaged[13] ^= 0x40;
    expect_decode_refusal(damaged, sizeof damaged, 1);
    CHECK(p4_hosted_decode(damaged, sizeof damaged, 0, &frame));
    CHECK(frame.payload[1] == (uint8_t)(wire[13] ^ 0x40));
}

static void test_interface_combinations(void)
{
    uint8_t payload[1] = { 0x7b };
    uint8_t bytes[TEST_CAPACITY];
    unsigned type, number, flags, throttle;
    unsigned combination = 0;

    for (type = P4_HOSTED_STA; type <= P4_HOSTED_ETH; ++type)
        for (number = 0; number <= 15; ++number)
            for (flags = 0; flags <= P4_HOSTED_MORE_FRAGMENT; ++flags)
                for (throttle = 0; throttle <= 2; ++throttle) {
                    struct p4_hosted_frame input, output;
                    size_t written = SIZE_MAX;
                    unsigned checksum_enabled = combination & 1u;

                    memset(&input, 0, sizeof input);
                    input.interface_type = (uint8_t)type;
                    input.interface_number = (uint8_t)number;
                    input.flags = (uint8_t)flags;
                    input.throttle = (uint8_t)throttle;
                    input.packet_type = (uint8_t)(combination * 17u);
                    input.sequence = (uint16_t)(combination * 257u);
                    input.payload = payload;
                    input.payload_size = sizeof payload;
                    input.wire_size = 12345; /* Encoder always emits offset 12. */
                    memset(bytes, 0xd2, sizeof bytes);
                    CHECK(p4_hosted_encode(bytes, sizeof bytes,
                                           checksum_enabled, &input, &written));
                    CHECK(written == P4_HOSTED_HEADER_SIZE + sizeof payload);
                    CHECK(bytes[0] == (uint8_t)((number << 4) | type));
                    CHECK(test_get16(bytes + 4) == P4_HOSTED_HEADER_SIZE);
                    CHECK(bytes[written] == 0xd2);
                    CHECK(p4_hosted_decode(bytes, written, checksum_enabled,
                                           &output));
                    CHECK(output.interface_type == type);
                    CHECK(output.interface_number == number);
                    CHECK(output.flags == flags);
                    CHECK(output.throttle == throttle);
                    CHECK(output.packet_type == input.packet_type);
                    CHECK(output.sequence == input.sequence);
                    CHECK(output.payload_size == sizeof payload);
                    CHECK(output.wire_size == written);
                    CHECK(output.payload[0] == payload[0]);
                    ++combination;
                }
    CHECK(combination == 7u * 16u * 2u * 3u);
}

static void test_all_payload_lengths(void)
{
    uint8_t payload[P4_HOSTED_FRAME_MAX];
    uint8_t bytes[TEST_CAPACITY];
    size_t length, i;

    for (length = 1; length <= P4_HOSTED_FRAME_MAX - P4_HOSTED_HEADER_SIZE;
         ++length) {
        struct p4_hosted_frame input;

        for (i = 0; i < length; ++i)
            payload[i] = (uint8_t)(i * 31u + length * 7u);
        memset(&input, 0, sizeof input);
        input.interface_type = P4_HOSTED_ETH;
        input.interface_number = 15;
        input.flags = (uint8_t)(length & P4_HOSTED_MORE_FRAGMENT);
        input.throttle = (uint8_t)(length % 3u);
        input.packet_type = (uint8_t)length;
        input.sequence = (uint16_t)(length * 19u);
        input.payload = payload;
        input.payload_size = length;

        {
            unsigned checksum_enabled;
            for (checksum_enabled = 0; checksum_enabled <= 1;
                 ++checksum_enabled) {
                struct p4_hosted_frame output;
                size_t written = 0;

                memset(bytes, 0xd3, sizeof bytes);
                CHECK(p4_hosted_encode(bytes, sizeof bytes, checksum_enabled,
                                       &input, &written));
                CHECK(written == P4_HOSTED_HEADER_SIZE + length);
                CHECK(test_get16(bytes + 2) == length);
                CHECK(test_get16(bytes + 4) == P4_HOSTED_HEADER_SIZE);
                CHECK(memcmp(bytes + P4_HOSTED_HEADER_SIZE, payload, length) == 0);
                CHECK(bytes[written] == 0xd3);
                if (checksum_enabled)
                    CHECK(test_checksum(bytes, written) == test_get16(bytes + 6));
                memset(&output, 0, sizeof output);
                CHECK(p4_hosted_decode(bytes, written, checksum_enabled,
                                       &output));
                CHECK(output.interface_type == input.interface_type);
                CHECK(output.interface_number == input.interface_number);
                CHECK(output.flags == input.flags);
                CHECK(output.throttle == input.throttle);
                CHECK(output.packet_type == input.packet_type);
                CHECK(output.sequence == input.sequence);
                CHECK(output.payload_size == length);
                CHECK(output.wire_size == written);
                CHECK(output.payload == bytes + P4_HOSTED_HEADER_SIZE);
                CHECK(memcmp(output.payload, payload, length) == 0);
                expect_decode_refusal(bytes, written - 1, checksum_enabled);
            }
        }
    }
}

static void test_truncation_padding_and_offsets(void)
{
    static const uint8_t payload[] = { 0x91, 0x27, 0x63 };
    uint8_t bytes[TEST_CAPACITY];
    struct p4_hosted_frame frame;
    size_t i;

    make_wire(bytes, P4_HOSTED_HEADER_SIZE, sizeof payload, P4_HOSTED_AP,
              15, 0, UINT16_C(0xfedc), 1, 0x42, payload, 1);
    for (i = 0; i < P4_HOSTED_HEADER_SIZE + sizeof payload; ++i)
        expect_decode_refusal(bytes, i, 1);
    CHECK(p4_hosted_decode(bytes, P4_HOSTED_HEADER_SIZE + sizeof payload,
                           1, &frame));
    CHECK(frame.wire_size == P4_HOSTED_HEADER_SIZE + sizeof payload);

    /* Extra transport bytes are ignored and are not included in the checksum. */
    memset(bytes + frame.wire_size, 0xe9, 16);
    CHECK(p4_hosted_decode(bytes, frame.wire_size + 16, 1, &frame));
    CHECK(frame.wire_size == P4_HOSTED_HEADER_SIZE + sizeof payload);
    bytes[frame.wire_size + 7] ^= 0xff;
    CHECK(p4_hosted_decode(bytes, frame.wire_size + 16, 1, &frame));

    make_wire(bytes, 13, 2, P4_HOSTED_PRIVATE, 3, 1,
              UINT16_C(0x8081), 2, 0xf1, payload, 1);
    CHECK(p4_hosted_decode(bytes, 15, 1, &frame));
    CHECK(frame.payload == bytes + 13);
    CHECK(frame.payload_size == 2);
    CHECK(frame.wire_size == 15);
    CHECK(frame.payload[0] == payload[0] && frame.payload[1] == payload[1]);

    make_wire(bytes, P4_HOSTED_FRAME_MAX - 1u, 1, P4_HOSTED_TEST, 0,
              P4_HOSTED_MORE_FRAGMENT, 0, 0, 0, payload, 1);
    CHECK(p4_hosted_decode(bytes, P4_HOSTED_FRAME_MAX, 1, &frame));
    CHECK(frame.payload == bytes + P4_HOSTED_FRAME_MAX - 1u);
    CHECK(frame.wire_size == P4_HOSTED_FRAME_MAX);

    make_wire(bytes, P4_HOSTED_FRAME_MAX, 1, P4_HOSTED_STA, 0, 0,
              0, 0, 0, payload, 0);
    expect_decode_refusal(bytes, P4_HOSTED_FRAME_MAX + 1u, 0);
}

static void test_malformed_headers_and_limits(void)
{
    static const uint8_t payload[] = { 0x44 };
    uint8_t bytes[TEST_CAPACITY];
    struct p4_hosted_frame frame;
    unsigned value;

    make_wire(bytes, P4_HOSTED_HEADER_SIZE, sizeof payload, P4_HOSTED_STA,
              0, 0, 9, 0, 0, payload, 0);
    for (value = 0; value < 16; ++value) {
        if (value == P4_HOSTED_STA ||
            (value >= P4_HOSTED_AP && value <= P4_HOSTED_ETH))
            continue;
        bytes[0] = (uint8_t)((bytes[0] & 0xf0u) | value);
        expect_decode_refusal(bytes, P4_HOSTED_HEADER_SIZE + sizeof payload, 0);
    }
    for (value = 0; value <= UINT8_MAX; ++value) {
        if (value <= P4_HOSTED_MORE_FRAGMENT)
            continue;
        bytes[0] = P4_HOSTED_STA;
        bytes[1] = (uint8_t)value;
        expect_decode_refusal(bytes, P4_HOSTED_HEADER_SIZE + sizeof payload, 0);
    }
    for (value = 0; value <= UINT8_MAX; ++value) {
        if (value <= 2)
            continue;
        bytes[1] = 0;
        bytes[10] = (uint8_t)value;
        expect_decode_refusal(bytes, P4_HOSTED_HEADER_SIZE + sizeof payload, 0);
    }

    /* Zero length, header overlap, and values beyond the 1536-byte frame cap. */
    set_header(bytes, P4_HOSTED_STA, 0, 0, 0, P4_HOSTED_HEADER_SIZE, 0, 0, 0);
    expect_decode_refusal(bytes, P4_HOSTED_HEADER_SIZE, 0);
    set_header(bytes, P4_HOSTED_STA, 0, 0, 1, P4_HOSTED_HEADER_SIZE - 1u,
               0, 0, 0);
    expect_decode_refusal(bytes, P4_HOSTED_FRAME_MAX, 0);
    set_header(bytes, P4_HOSTED_STA, 0, 0, 1, P4_HOSTED_FRAME_MAX,
               0, 0, 0);
    expect_decode_refusal(bytes, P4_HOSTED_FRAME_MAX, 0);
    set_header(bytes, P4_HOSTED_STA, 0, 0, UINT16_MAX,
               P4_HOSTED_HEADER_SIZE, 0, 0, 0);
    expect_decode_refusal(bytes, P4_HOSTED_FRAME_MAX, 0);
    set_header(bytes, P4_HOSTED_STA, 0, 0, UINT16_MAX, UINT16_MAX,
               0, 0, 0);
    expect_decode_refusal(bytes, P4_HOSTED_FRAME_MAX, 0);
    set_header(bytes, P4_HOSTED_STA, 0, 0, 2, UINT16_C(0xfff0),
               0, 0, 0);
    expect_decode_refusal(bytes, P4_HOSTED_FRAME_MAX, 0);

    /* The largest valid one-byte offset plus payload ends exactly at 1536. */
    make_wire(bytes, P4_HOSTED_FRAME_MAX - 1u, 1, P4_HOSTED_ETH, 15,
              0, 0, 0, 0, payload, 0);
    CHECK(p4_hosted_decode(bytes, P4_HOSTED_FRAME_MAX, 0, &frame));
    CHECK(frame.wire_size == P4_HOSTED_FRAME_MAX);
    CHECK(frame.payload == bytes + P4_HOSTED_FRAME_MAX - 1u);
    /* checksum selectors outside the negotiated boolean domain are refused. */
    expect_decode_refusal(bytes, P4_HOSTED_FRAME_MAX, 2);
    expect_decode_refusal(bytes, P4_HOSTED_FRAME_MAX, UINT_MAX);
}

static void expect_encode_refusal(const struct p4_hosted_frame *frame,
                                  size_t capacity, unsigned checksum_enabled,
                                  int null_bytes, int null_written)
{
    uint8_t bytes[TEST_CAPACITY];
    uint8_t before[TEST_CAPACITY];
    size_t written = SIZE_MAX - 17u;
    size_t before_written = written;
    uint8_t *output = null_bytes ? NULL : bytes;
    size_t *written_out = null_written ? NULL : &written;

    memset(bytes, 0xb6, sizeof bytes);
    memcpy(before, bytes, sizeof bytes);
    CHECK(!p4_hosted_encode(output, capacity, checksum_enabled, frame,
                            written_out));
    CHECK(memcmp(bytes, before, sizeof bytes) == 0);
    if (!null_written)
        CHECK(written == before_written);
}

static void test_encode_refusals(void)
{
    uint8_t payload[P4_HOSTED_FRAME_MAX];
    struct p4_hosted_frame frame;
    unsigned value;

    memset(payload, 0x8c, sizeof payload);
    memset(&frame, 0, sizeof frame);
    frame.interface_type = P4_HOSTED_STA;
    frame.payload = payload;
    frame.payload_size = 1;
    for (value = 0; value <= UINT8_MAX; ++value) {
        if (value >= P4_HOSTED_STA && value <= P4_HOSTED_ETH)
            continue;
        frame.interface_type = (uint8_t)value;
        expect_encode_refusal(&frame, TEST_CAPACITY, 0, 0, 0);
    }
    frame.interface_type = P4_HOSTED_STA;
    for (value = 16; value <= UINT8_MAX; ++value) {
        frame.interface_number = (uint8_t)value;
        expect_encode_refusal(&frame, TEST_CAPACITY, 0, 0, 0);
    }
    frame.interface_number = 0;
    for (value = 2; value <= UINT8_MAX; ++value) {
        frame.flags = (uint8_t)value;
        expect_encode_refusal(&frame, TEST_CAPACITY, 0, 0, 0);
    }
    frame.flags = 0;
    for (value = 3; value <= UINT8_MAX; ++value) {
        frame.throttle = (uint8_t)value;
        expect_encode_refusal(&frame, TEST_CAPACITY, 0, 0, 0);
    }

    frame.throttle = 0;
    frame.payload_size = 0;
    expect_encode_refusal(&frame, TEST_CAPACITY, 0, 0, 0);
    frame.payload_size = P4_HOSTED_FRAME_MAX - P4_HOSTED_HEADER_SIZE + 1u;
    expect_encode_refusal(&frame, TEST_CAPACITY, 0, 0, 0);
    frame.payload_size = SIZE_MAX;
    expect_encode_refusal(&frame, TEST_CAPACITY, 0, 0, 0);
    frame.payload_size = 1;
    frame.payload = NULL;
    expect_encode_refusal(&frame, TEST_CAPACITY, 0, 0, 0);
    frame.payload = payload;
    expect_encode_refusal(&frame, TEST_CAPACITY, 2, 0, 0);
    expect_encode_refusal(&frame, TEST_CAPACITY, UINT_MAX, 0, 0);
    for (value = 0; value < P4_HOSTED_HEADER_SIZE + 1u; ++value)
        expect_encode_refusal(&frame, value, 0, 0, 0);
    expect_encode_refusal(NULL, TEST_CAPACITY, 0, 0, 0);
    expect_encode_refusal(&frame, TEST_CAPACITY, 0, 1, 0);
    expect_encode_refusal(&frame, TEST_CAPACITY, 0, 0, 1);
}

static void test_overlapping_payloads(void)
{
    static const size_t source_offsets[] = { 3, 20 };
    uint8_t bytes[TEST_CAPACITY];
    uint8_t expected[48];
    size_t case_index;

    for (case_index = 0;
         case_index < sizeof source_offsets / sizeof source_offsets[0];
         ++case_index) {
        struct p4_hosted_frame frame;
        size_t source_offset = source_offsets[case_index];
        size_t written = 0;
        size_t i;

        for (i = 0; i < sizeof bytes; ++i)
            bytes[i] = (uint8_t)(i * 13u + 5u);
        memcpy(expected, bytes + source_offset, sizeof expected);
        memset(&frame, 0, sizeof frame);
        frame.interface_type = P4_HOSTED_HCI;
        frame.interface_number = 7;
        frame.flags = P4_HOSTED_MORE_FRAGMENT;
        frame.throttle = 2;
        frame.packet_type = 0xfe;
        frame.sequence = UINT16_C(0xcafe);
        frame.payload = bytes + source_offset;
        frame.payload_size = sizeof expected;
        CHECK(p4_hosted_encode(bytes, sizeof bytes, 1, &frame, &written));
        CHECK(written == P4_HOSTED_HEADER_SIZE + sizeof expected);
        CHECK(memcmp(bytes + P4_HOSTED_HEADER_SIZE, expected,
                     sizeof expected) == 0);
        CHECK(test_checksum(bytes, written) == test_get16(bytes + 6));
    }
}

static void test_bounded_deterministic_fuzz(void)
{
    uint8_t bytes[TEST_CAPACITY];
    size_t iteration;

    /* Arbitrary bytes with varied lengths, including short and padded spans. */
    for (iteration = 0; iteration < 12000; ++iteration) {
        size_t available = next_random() % (TEST_CAPACITY + 1u);
        size_t i;
        unsigned checksum_enabled = (unsigned)(next_random() & 1u);

        for (i = 0; i < available; ++i)
            bytes[i] = (uint8_t)next_random();
        if (available >= P4_HOSTED_HEADER_SIZE && iteration % 5u == 0) {
            /* Keep fields plausible often enough to explore deeper bounds. */
            bytes[0] = (uint8_t)((next_random() & 0xf0u) |
                (1u + next_random() % P4_HOSTED_ETH));
            bytes[1] = (uint8_t)(next_random() & 1u);
            bytes[10] = (uint8_t)(next_random() % 3u);
        }
        check_decoded_frame(bytes, available, checksum_enabled);
    }

    /* Valid random frames, then deterministic single-bit mutations. */
    for (iteration = 0; iteration < 3000; ++iteration) {
        size_t available = P4_HOSTED_HEADER_SIZE + 1u +
            next_random() % (P4_HOSTED_FRAME_MAX - P4_HOSTED_HEADER_SIZE);
        size_t offset = P4_HOSTED_HEADER_SIZE +
            next_random() % (available - P4_HOSTED_HEADER_SIZE);
        size_t max_payload = P4_HOSTED_FRAME_MAX - offset;
        size_t available_payload = available - offset;
        size_t payload_size = 1u + next_random() %
            (max_payload < available_payload ? max_payload : available_payload);
        unsigned checksum_enabled = (unsigned)(next_random() & 1u);
        size_t i;

        for (i = 0; i < available; ++i)
            bytes[i] = (uint8_t)next_random();
        set_header(bytes,
                   (uint8_t)(1u + next_random() % P4_HOSTED_ETH),
                   (uint8_t)(next_random() & 15u),
                   (uint8_t)(next_random() & 1u),
                   (uint16_t)payload_size, (uint16_t)offset,
                   (uint16_t)next_random(), (uint8_t)(next_random() % 3u),
                   (uint8_t)next_random());
        if (checksum_enabled)
            test_put16(bytes + 6, test_checksum(bytes, offset + payload_size));
        check_decoded_frame(bytes, available, checksum_enabled);
        bytes[next_random() % (offset + payload_size)] ^=
            (uint8_t)(1u << (next_random() & 7u));
        check_decoded_frame(bytes, available, checksum_enabled);
    }
}

static void test_nulls_and_invalid_options(void)
{
    static const uint8_t payload[] = { 0x01 };
    uint8_t bytes[TEST_CAPACITY];
    struct p4_hosted_frame frame;
    size_t written;

    make_wire(bytes, P4_HOSTED_HEADER_SIZE, sizeof payload, P4_HOSTED_STA,
              0, 0, 0, 0, 0, payload, 0);
    expect_decode_refusal(NULL, sizeof bytes, 0);
    CHECK(!p4_hosted_decode(bytes, sizeof bytes, 0, NULL));
    expect_decode_refusal(bytes, P4_HOSTED_HEADER_SIZE - 1u, 0);
    expect_decode_refusal(bytes, sizeof bytes, 2);
    expect_decode_refusal(bytes, sizeof bytes, UINT_MAX);

    memset(&frame, 0, sizeof frame);
    frame.interface_type = P4_HOSTED_STA;
    frame.payload = payload;
    frame.payload_size = sizeof payload;
    written = 17;
    CHECK(!p4_hosted_encode(NULL, sizeof bytes, 0, &frame, &written));
    CHECK(written == 17);
    CHECK(!p4_hosted_encode(bytes, sizeof bytes, 0, &frame, NULL));
}

int main(void)
{
    test_known_vector();
    test_interface_combinations();
    test_all_payload_lengths();
    test_truncation_padding_and_offsets();
    test_malformed_headers_and_limits();
    test_encode_refusals();
    test_overlapping_payloads();
    test_nulls_and_invalid_options();
    test_bounded_deterministic_fuzz();
    printf("PASS: %lu hosted-wire checks\n", checks);
    return 0;
}
