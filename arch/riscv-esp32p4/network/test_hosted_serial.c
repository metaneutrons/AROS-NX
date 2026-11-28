/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hosted_serial.h"

#define TEST_CAPACITY HOSTED_SERIAL_MESSAGE_MAX

static unsigned long checks;

#define CHECK(expression) do { \
    ++checks; \
    if (!(expression)) { \
        fprintf(stderr, "line %u: %s\n", (unsigned int)__LINE__, #expression); \
        exit(1); \
    } \
} while (0)

static uint16_t test_get_le16(const uint8_t *bytes)
{
    return (uint16_t)((unsigned)bytes[0] | ((unsigned)bytes[1] << 8));
}

static void test_put_le16(uint8_t *bytes, size_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
}

static void expect_decode_refusal(const uint8_t *bytes, size_t available)
{
    struct hosted_serial_message message;
    uint8_t before[sizeof message];

    memset(&message, 0xa5, sizeof message);
    memcpy(before, &message, sizeof message);
    CHECK(!hosted_serial_decode(bytes, available, &message));
    CHECK(memcmp(&message, before, sizeof message) == 0);
}

static void expect_encode_refusal(uint8_t *bytes, size_t capacity,
                                  const struct hosted_serial_message *message,
                                  size_t *written)
{
    uint8_t before[TEST_CAPACITY];
    size_t written_before = *written;

    memcpy(before, bytes, sizeof before);
    CHECK(!hosted_serial_encode(bytes, capacity, message, written));
    CHECK(memcmp(bytes, before, sizeof before) == 0);
    CHECK(*written == written_before);
}

static void test_known_vector(void)
{
    /* Independently stated wire vector: endpoint RPCRsp, protobuf field 1
     * containing the single byte 'x'. */
    static const uint8_t wire[] = {
        0x01, 0x06, 0x00, 'R', 'P', 'C', 'R', 's', 'p',
        0x02, 0x03, 0x00, 0x0a, 0x01, 0x78
    };
    struct hosted_serial_message message;
    uint8_t encoded[sizeof wire + 1u];
    size_t written = 0;

    CHECK(hosted_serial_decode(wire, sizeof wire, &message));
    CHECK(message.endpoint == HOSTED_SERIAL_ENDPOINT_RPCRSP);
    CHECK(message.protobuf == wire + HOSTED_SERIAL_MESSAGE_OVERHEAD);
    CHECK(message.protobuf_size == 3u);
    CHECK(message.wire_size == sizeof wire);
    CHECK(memcmp(message.protobuf, "\x0a\x01x", 3u) == 0);

    memset(encoded, 0xcc, sizeof encoded);
    CHECK(hosted_serial_encode(encoded + 1u, sizeof wire, &message, &written));
    CHECK(written == sizeof wire);
    CHECK(memcmp(encoded + 1u, wire, sizeof wire) == 0);
    CHECK(encoded[0] == 0xcc);
}

static void make_wire(uint8_t *bytes, size_t protobuf_size,
                      enum hosted_serial_endpoint endpoint)
{
    static const uint8_t response[] = { 'R', 'P', 'C', 'R', 's', 'p' };
    static const uint8_t event[] = { 'R', 'P', 'C', 'E', 'v', 't' };
    size_t i;

    bytes[0] = 1u;
    test_put_le16(bytes + 1u, HOSTED_SERIAL_ENDPOINT_SIZE);
    memcpy(bytes + 3u, endpoint == HOSTED_SERIAL_ENDPOINT_RPCRSP ?
           response : event, HOSTED_SERIAL_ENDPOINT_SIZE);
    bytes[9] = 2u;
    test_put_le16(bytes + 10u, protobuf_size);
    for (i = 0; i < protobuf_size; ++i)
        bytes[HOSTED_SERIAL_MESSAGE_OVERHEAD + i] = (uint8_t)(i * 31u + 7u);
}

static void test_endpoints_and_boundaries(void)
{
    uint8_t protobuf[HOSTED_SERIAL_PROTOBUF_MAX];
    uint8_t wire[TEST_CAPACITY];
    struct hosted_serial_message input, decoded;
    size_t written, i;

    for (i = 0; i < sizeof protobuf; ++i)
        protobuf[i] = (uint8_t)(i * 29u + 3u);

    input.endpoint = HOSTED_SERIAL_ENDPOINT_RPCRSP;
    input.protobuf = protobuf;
    input.protobuf_size = 1u;
    input.wire_size = 0u;
    memset(wire, 0xd1, sizeof wire);
    written = 0;
    CHECK(hosted_serial_encode(wire, sizeof wire, &input, &written));
    CHECK(written == HOSTED_SERIAL_MESSAGE_OVERHEAD + 1u);
    CHECK(test_get_le16(wire + 1u) == HOSTED_SERIAL_ENDPOINT_SIZE);
    CHECK(test_get_le16(wire + 10u) == 1u);
    CHECK(wire[written] == 0xd1);
    CHECK(hosted_serial_decode(wire, written, &decoded));
    CHECK(decoded.endpoint == HOSTED_SERIAL_ENDPOINT_RPCRSP);
    CHECK(decoded.protobuf_size == 1u);
    CHECK(decoded.protobuf[0] == protobuf[0]);

    input.endpoint = HOSTED_SERIAL_ENDPOINT_RPCEVT;
    input.protobuf_size = HOSTED_SERIAL_PROTOBUF_MAX;
    written = 0;
    CHECK(hosted_serial_encode(wire, sizeof wire, &input, &written));
    CHECK(written == HOSTED_SERIAL_MESSAGE_MAX);
    CHECK(hosted_serial_decode(wire, written, &decoded));
    CHECK(decoded.endpoint == HOSTED_SERIAL_ENDPOINT_RPCEVT);
    CHECK(decoded.protobuf_size == HOSTED_SERIAL_PROTOBUF_MAX);
    CHECK(memcmp(decoded.protobuf, protobuf, sizeof protobuf) == 0);

    input.protobuf_size = HOSTED_SERIAL_PROTOBUF_MAX + 1u;
    memset(wire, 0x6b, sizeof wire);
    written = 0x12345678u;
    expect_encode_refusal(wire, sizeof wire, &input, &written);

    input.protobuf_size = 1u;
    input.endpoint = (enum hosted_serial_endpoint)99;
    expect_encode_refusal(wire, sizeof wire, &input, &written);

    make_wire(wire, 1u, HOSTED_SERIAL_ENDPOINT_RPCEVT);
    CHECK(hosted_serial_decode(wire, HOSTED_SERIAL_MESSAGE_OVERHEAD + 1u,
                              &decoded));
    CHECK(decoded.endpoint == HOSTED_SERIAL_ENDPOINT_RPCEVT);
}

static void test_malformed_wire(void)
{
    uint8_t wire[TEST_CAPACITY];
    uint8_t damaged[TEST_CAPACITY];
    const size_t valid_size = HOSTED_SERIAL_MESSAGE_OVERHEAD + 3u;

    make_wire(wire, 3u, HOSTED_SERIAL_ENDPOINT_RPCRSP);
    for (size_t length = 0; length < valid_size; ++length)
        expect_decode_refusal(wire, length);

    memcpy(damaged, wire, valid_size);
    damaged[0] = 0u;
    expect_decode_refusal(damaged, valid_size);
    memcpy(damaged, wire, valid_size);
    damaged[0] = 3u;
    expect_decode_refusal(damaged, valid_size);
    memcpy(damaged, wire, valid_size);
    test_put_le16(damaged + 1u, 0u);
    expect_decode_refusal(damaged, valid_size);
    memcpy(damaged, wire, valid_size);
    test_put_le16(damaged + 1u, HOSTED_SERIAL_ENDPOINT_SIZE - 1u);
    expect_decode_refusal(damaged, valid_size);
    memcpy(damaged, wire, valid_size);
    test_put_le16(damaged + 1u, HOSTED_SERIAL_ENDPOINT_SIZE + 1u);
    expect_decode_refusal(damaged, valid_size);
    memcpy(damaged, wire, valid_size);
    memcpy(damaged + 3u, "Unknown", HOSTED_SERIAL_ENDPOINT_SIZE);
    expect_decode_refusal(damaged, valid_size);
    memcpy(damaged, wire, valid_size);
    damaged[9] = 0u;
    expect_decode_refusal(damaged, valid_size);
    memcpy(damaged, wire, valid_size);
    damaged[9] = 1u;
    expect_decode_refusal(damaged, valid_size);
    memcpy(damaged, wire, valid_size);
    damaged[9] = 3u;
    expect_decode_refusal(damaged, valid_size);
    memcpy(damaged, wire, valid_size);
    test_put_le16(damaged + 10u, 0u);
    expect_decode_refusal(damaged, valid_size);
    memcpy(damaged, wire, valid_size);
    test_put_le16(damaged + 10u, HOSTED_SERIAL_PROTOBUF_MAX + 1u);
    expect_decode_refusal(damaged, valid_size);
    memcpy(damaged, wire, valid_size);
    test_put_le16(damaged + 10u, 2u);
    expect_decode_refusal(damaged, valid_size);
    memcpy(damaged, wire, valid_size);
    test_put_le16(damaged + 10u, 4u);
    expect_decode_refusal(damaged, valid_size);

    /* Any extra byte, including zero, is trailing data rather than padding. */
    memcpy(damaged, wire, valid_size);
    damaged[valid_size] = 0u;
    expect_decode_refusal(damaged, valid_size + 1u);

    /* A declared maximum-sized protobuf with missing final byte is truncated. */
    make_wire(damaged, HOSTED_SERIAL_PROTOBUF_MAX, HOSTED_SERIAL_ENDPOINT_RPCRSP);
    expect_decode_refusal(damaged, HOSTED_SERIAL_MESSAGE_MAX - 1u);
}

static void test_output_atomicity_and_aliasing(void)
{
    uint8_t wire[TEST_CAPACITY];
    uint8_t protobuf[8] = { 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u };
    struct hosted_serial_message message, decoded;
    size_t written;

    memset(wire, 0x4e, sizeof wire);
    message.endpoint = HOSTED_SERIAL_ENDPOINT_RPCRSP;
    message.protobuf = protobuf;
    message.protobuf_size = sizeof protobuf;
    message.wire_size = 0u;
    written = 0x76543210u;
    message.protobuf = (const uint8_t *)&message;
    message.protobuf_size = 1u;
    expect_encode_refusal(wire, sizeof wire, &message, &written);
    message.protobuf = protobuf;
    message.protobuf_size = sizeof protobuf;
    expect_encode_refusal(wire, HOSTED_SERIAL_MESSAGE_OVERHEAD + sizeof protobuf - 1u,
                          &message, &written);

    message.protobuf_size = 0u;
    expect_encode_refusal(wire, sizeof wire, &message, &written);
    message.protobuf = NULL;
    message.protobuf_size = sizeof protobuf;
    expect_encode_refusal(wire, sizeof wire, &message, &written);
    message.protobuf = protobuf;
    message.protobuf_size = sizeof protobuf;

    /* Aliased input/output data and output metadata are refused before any
     * byte or output count changes. */
    message.protobuf = wire;
    message.protobuf_size = 1u;
    expect_encode_refusal(wire, sizeof wire, &message, &written);
    message.protobuf = protobuf;
    {
        union output_alias {
            size_t written;
            uint8_t bytes[TEST_CAPACITY];
        } alias;

        memset(&alias, 0x4e, sizeof alias);
        alias.written = 0x76543210u;
        expect_encode_refusal(alias.bytes, sizeof alias.bytes, &message,
                              &alias.written);
    }

    make_wire(wire, 3u, HOSTED_SERIAL_ENDPOINT_RPCRSP);
    CHECK(hosted_serial_decode(wire, HOSTED_SERIAL_MESSAGE_OVERHEAD + 3u,
                               &decoded));
    /* A misaligned input is valid; all integer fields are byte-decoded. */
    memmove(wire + 1u, wire, HOSTED_SERIAL_MESSAGE_OVERHEAD + 3u);
    CHECK(hosted_serial_decode(wire + 1u, HOSTED_SERIAL_MESSAGE_OVERHEAD + 3u,
                               &decoded));
    CHECK(decoded.endpoint == HOSTED_SERIAL_ENDPOINT_RPCRSP);

    /* Decoder output cannot overlap input, preserving both on refusal. */
    {
        union overlap_storage {
            struct hosted_serial_message message;
            uint8_t bytes[sizeof(struct hosted_serial_message) + 32u];
        } overlap;
        uint8_t before[sizeof overlap];

        memset(&overlap, 0x93, sizeof overlap);
        memcpy(before, &overlap, sizeof overlap);
        CHECK(!hosted_serial_decode(overlap.bytes, sizeof overlap.bytes,
                                    &overlap.message));
        CHECK(memcmp(&overlap, before, sizeof overlap) == 0);
    }
}

int main(void)
{
    test_known_vector();
    test_endpoints_and_boundaries();
    test_malformed_wire();
    test_output_atomicity_and_aliasing();
    printf("hosted_serial: %lu checks passed\n", checks);
    return 0;
}
