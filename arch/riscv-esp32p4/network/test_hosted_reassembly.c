/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hosted_reassembly.h"

#define TEST_WIRE_CAPACITY (HOSTED_REASSEMBLY_BUFFER_SIZE + 1u)

static unsigned long checks;

#define CHECK(expression) do { \
    ++checks; \
    if (!(expression)) { \
        fprintf(stderr, "line %u: %s\n", (unsigned int)__LINE__, #expression); \
        exit(1); \
    } \
} while (0)

static void put_le16(uint8_t *bytes, size_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
}

static size_t make_tlv(uint8_t *bytes, size_t capacity,
                       size_t protobuf_size)
{
    size_t wire_size = HOSTED_SERIAL_MESSAGE_OVERHEAD + protobuf_size;
    size_t i;

    CHECK(protobuf_size != 0);
    CHECK(protobuf_size <= HOSTED_SERIAL_PROTOBUF_MAX);
    CHECK(capacity >= wire_size);
    bytes[0] = 1u;
    put_le16(bytes + 1u, HOSTED_SERIAL_ENDPOINT_SIZE);
    memcpy(bytes + 3u, "RPCRsp", HOSTED_SERIAL_ENDPOINT_SIZE);
    bytes[9] = 2u;
    put_le16(bytes + 10u, protobuf_size);
    for (i = 0; i < protobuf_size; ++i)
        bytes[HOSTED_SERIAL_MESSAGE_OVERHEAD + i] = (uint8_t)(i * 73u + 19u);
    return wire_size;
}

static size_t make_oversized_tlv(uint8_t *bytes, size_t capacity,
                                 size_t protobuf_size)
{
    size_t wire_size = HOSTED_SERIAL_MESSAGE_OVERHEAD + protobuf_size;
    size_t i;

    CHECK(capacity >= wire_size);
    bytes[0] = 1u;
    put_le16(bytes + 1u, HOSTED_SERIAL_ENDPOINT_SIZE);
    memcpy(bytes + 3u, "RPCRsp", HOSTED_SERIAL_ENDPOINT_SIZE);
    bytes[9] = 2u;
    put_le16(bytes + 10u, protobuf_size);
    for (i = 0; i < protobuf_size; ++i)
        bytes[HOSTED_SERIAL_MESSAGE_OVERHEAD + i] = (uint8_t)(i * 37u + 11u);
    return wire_size;
}

static struct p4_hosted_frame make_frame(const uint8_t *payload,
                                         size_t payload_size,
                                         uint16_t sequence,
                                         unsigned more)
{
    struct p4_hosted_frame frame;

    memset(&frame, 0, sizeof frame);
    frame.interface_type = P4_HOSTED_SERIAL;
    frame.interface_number = 0u;
    frame.flags = more ? P4_HOSTED_MORE_FRAGMENT : 0u;
    frame.throttle = 2u;
    frame.packet_type = 0xffu;
    frame.sequence = sequence;
    frame.payload = payload;
    frame.payload_size = payload_size;
    frame.wire_size = P4_HOSTED_HEADER_SIZE + payload_size;
    return frame;
}

static enum hosted_reassembly_result feed_bytes(
    struct hosted_reassembly_state *state, const uint8_t *bytes,
    size_t offset, size_t size, uint16_t sequence, unsigned more,
    uint32_t now_ms, struct hosted_serial_message *output)
{
    struct p4_hosted_frame frame = make_frame(bytes + offset, size,
                                              sequence, more);
    return hosted_reassembly_feed(state, &frame, now_ms, output);
}

static void expect_output_unchanged(
    const struct hosted_serial_message *output,
    const struct hosted_serial_message *before)
{
    CHECK(memcmp(output, before, sizeof *output) == 0);
}

static void expect_rejected(struct hosted_reassembly_state *state,
                            const struct p4_hosted_frame *frame,
                            uint32_t now_ms,
                            struct hosted_serial_message *output)
{
    struct hosted_serial_message before;

    memcpy(&before, output, sizeof before);
    CHECK(hosted_reassembly_feed(state, frame, now_ms, output) ==
          HOSTED_REASSEMBLY_REJECTED);
    expect_output_unchanged(output, &before);
}

static void expect_complete(struct hosted_reassembly_state *state,
                            const uint8_t *bytes, size_t wire_size,
                            uint16_t sequence, uint32_t now_ms,
                            struct hosted_serial_message *output)
{
    CHECK(feed_bytes(state, bytes, 0u, wire_size, sequence, 0u,
                     now_ms, output) == HOSTED_REASSEMBLY_COMPLETE);
    CHECK(output->endpoint == HOSTED_SERIAL_ENDPOINT_RPCRSP);
    CHECK(output->wire_size == wire_size);
    CHECK(output->protobuf == state->bytes + HOSTED_SERIAL_MESSAGE_OVERHEAD);
    CHECK(output->protobuf_size == wire_size - HOSTED_SERIAL_MESSAGE_OVERHEAD);
    CHECK(memcmp(output->protobuf,
                 bytes + HOSTED_SERIAL_MESSAGE_OVERHEAD,
                 output->protobuf_size) == 0);
    CHECK(state->active == 0u);
}

static void test_init_and_single_frame(void)
{
    struct hosted_reassembly_state state, before_state;
    struct hosted_serial_message output;
    uint8_t bytes[HOSTED_REASSEMBLY_BUFFER_SIZE];
    size_t wire_size;

    memset(&state, 0xa7, sizeof state);
    memcpy(&before_state, &state, sizeof state);
    CHECK(!hosted_reassembly_init(&state, 0u));
    CHECK(memcmp(&state, &before_state, sizeof state) == 0);
    CHECK(!hosted_reassembly_init(&state, UINT32_C(0x80000000)));
    CHECK(memcmp(&state, &before_state, sizeof state) == 0);
    CHECK(hosted_reassembly_init(&state, 100u));
    CHECK(state.timeout_ms == 100u);
    CHECK(state.active == 0u);

    wire_size = make_tlv(bytes, sizeof bytes, 7u);
    bytes[HOSTED_SERIAL_MESSAGE_OVERHEAD] = 0u;
    bytes[HOSTED_SERIAL_MESSAGE_OVERHEAD + 1u] = 0xffu;
    bytes[HOSTED_SERIAL_MESSAGE_OVERHEAD + 2u] = 0x80u;
    memset(&output, 0xa5, sizeof output);
    expect_complete(&state, bytes, wire_size, UINT16_MAX, 10u, &output);
    CHECK(output.protobuf[0] == 0u);
    CHECK(output.protobuf[1] == 0xffu);
    CHECK(output.protobuf[2] == 0x80u);

    /* Idle frames have no inferred sequence baseline. */
    expect_complete(&state, bytes, wire_size, 0u, 11u, &output);
}

static void test_fragmented_wrap_and_interleaving(void)
{
    struct hosted_reassembly_state state;
    struct hosted_serial_message output, before;
    struct p4_hosted_frame frame;
    uint8_t bytes[80], other_payload[3] = { 9u, 8u, 7u };
    size_t wire_size, offset;

    CHECK(hosted_reassembly_init(&state, 100u));
    wire_size = make_tlv(bytes, sizeof bytes, 23u);
    memset(&output, 0x5c, sizeof output);
    memcpy(&before, &output, sizeof before);
    CHECK(feed_bytes(&state, bytes, 0u, 5u, UINT16_C(0xfffe), 1u,
                     100u, &output) == HOSTED_REASSEMBLY_PENDING);
    expect_output_unchanged(&output, &before);
    CHECK(state.active == 1u);
    CHECK(state.expected_sequence == UINT16_C(0xffff));
    CHECK(state.first_tick == 100u);

    frame = make_frame(other_payload, sizeof other_payload, 400u, 0u);
    frame.interface_type = P4_HOSTED_ETH;
    CHECK(hosted_reassembly_feed(&state, &frame, 101u, &output) ==
          HOSTED_REASSEMBLY_IGNORED);
    expect_output_unchanged(&output, &before);
    CHECK(state.active == 1u);
    CHECK(state.expected_sequence == UINT16_C(0xffff));

    CHECK(feed_bytes(&state, bytes, 5u, 7u, UINT16_C(0xffff), 1u,
                     102u, &output) == HOSTED_REASSEMBLY_PENDING);
    expect_output_unchanged(&output, &before);
    CHECK(state.expected_sequence == 0u);
    offset = 12u;
    CHECK(feed_bytes(&state, bytes, offset, wire_size - offset, 0u, 0u,
                     103u, &output) == HOSTED_REASSEMBLY_COMPLETE);
    CHECK(output.endpoint == HOSTED_SERIAL_ENDPOINT_RPCRSP);
    CHECK(output.protobuf_size == 23u);
    CHECK(memcmp(output.protobuf,
                 bytes + HOSTED_SERIAL_MESSAGE_OVERHEAD, 23u) == 0);
    CHECK(state.active == 0u);
}

static void test_wire_decode_reassemble_event(void)
{
    struct hosted_reassembly_state state;
    struct hosted_serial_message input, output, before;
    struct p4_hosted_frame encoded, decoded;
    uint8_t protobuf[1788], envelope[1800], packet[P4_HOSTED_FRAME_MAX];
    size_t envelope_size, packet_size, i;

    for (i = 0; i < sizeof protobuf; ++i)
        protobuf[i] = (uint8_t)(i * 41u);
    input.endpoint = HOSTED_SERIAL_ENDPOINT_RPCEVT;
    input.protobuf = protobuf;
    input.protobuf_size = sizeof protobuf;
    input.wire_size = 0u;
    CHECK(hosted_serial_encode(envelope, sizeof envelope, &input,
                               &envelope_size));
    CHECK(envelope_size == sizeof envelope);
    CHECK(hosted_reassembly_init(&state, 100u));
    memset(&output, 0x4b, sizeof output);
    memcpy(&before, &output, sizeof before);

    encoded = make_frame(envelope, 1500u, UINT16_C(0xfffe), 1u);
    encoded.throttle = 1u;
    CHECK(p4_hosted_encode(packet, sizeof packet, 1u, &encoded,
                           &packet_size));
    CHECK(p4_hosted_decode(packet, packet_size, 1u, &decoded));
    CHECK(decoded.interface_type == P4_HOSTED_SERIAL);
    CHECK(decoded.interface_number == 0u);
    CHECK(decoded.sequence == UINT16_C(0xfffe));
    CHECK(hosted_reassembly_feed(&state, &decoded, 10u, &output) ==
          HOSTED_REASSEMBLY_PENDING);
    expect_output_unchanged(&output, &before);

    encoded = make_frame(envelope + 1500u, envelope_size - 1500u,
                         UINT16_C(0xffff), 0u);
    encoded.throttle = 2u;
    CHECK(p4_hosted_encode(packet, sizeof packet, 1u, &encoded,
                           &packet_size));
    CHECK(p4_hosted_decode(packet, packet_size, 1u, &decoded));
    CHECK(decoded.sequence == UINT16_C(0xffff));
    CHECK(hosted_reassembly_feed(&state, &decoded, 11u, &output) ==
          HOSTED_REASSEMBLY_COMPLETE);
    CHECK(output.endpoint == HOSTED_SERIAL_ENDPOINT_RPCEVT);
    CHECK(output.protobuf_size == sizeof protobuf);
    CHECK(memcmp(output.protobuf, protobuf, sizeof protobuf) == 0);
}

static void test_sequence_refusal_and_recovery(void)
{
    struct hosted_reassembly_state state;
    struct hosted_serial_message output, before;
    struct p4_hosted_frame frame;
    uint8_t bytes[64];
    size_t wire_size;

    CHECK(hosted_reassembly_init(&state, 100u));
    wire_size = make_tlv(bytes, sizeof bytes, 16u);
    memset(&output, 0x39, sizeof output);
    memcpy(&before, &output, sizeof before);

    CHECK(feed_bytes(&state, bytes, 0u, 4u, 20u, 1u, 1u, &output) ==
          HOSTED_REASSEMBLY_PENDING);
    frame = make_frame(bytes + 4u, 4u, 20u, 1u); /* duplicate */
    expect_rejected(&state, &frame, 2u, &output);
    CHECK(state.active == 0u);
    expect_complete(&state, bytes, wire_size, 500u, 3u, &output);
    memcpy(&before, &output, sizeof before);

    CHECK(feed_bytes(&state, bytes, 0u, 4u, 30u, 1u, 4u, &output) ==
          HOSTED_REASSEMBLY_PENDING);
    frame = make_frame(bytes + 4u, 4u, 32u, 1u); /* gap */
    expect_rejected(&state, &frame, 5u, &output);
    CHECK(state.active == 0u);

    /* A late reordered fragment cannot be appended to the discarded prefix. */
    frame = make_frame(bytes + 4u, wire_size - 4u, 31u, 0u);
    expect_rejected(&state, &frame, 6u, &output);
    expect_output_unchanged(&output, &before);
    expect_complete(&state, bytes, wire_size, 7u, 7u, &output);
}

static void test_invalid_fragments_discard_partial(void)
{
    struct hosted_reassembly_state state;
    struct hosted_serial_message output, before;
    struct p4_hosted_frame frame;
    uint8_t bytes[64], large_payload[HOSTED_REASSEMBLY_FRAGMENT_SIZE + 1u];
    size_t wire_size;

    CHECK(hosted_reassembly_init(&state, 100u));
    wire_size = make_tlv(bytes, sizeof bytes, 20u);
    memset(&output, 0x82, sizeof output);
    memcpy(&before, &output, sizeof before);

    CHECK(feed_bytes(&state, bytes, 0u, 4u, 1u, 1u, 1u, &output) ==
          HOSTED_REASSEMBLY_PENDING);
    frame = make_frame(bytes + 4u, 4u, 2u, 1u);
    frame.flags = 2u;
    expect_rejected(&state, &frame, 2u, &output);
    CHECK(state.active == 0u);

    CHECK(feed_bytes(&state, bytes, 0u, 4u, 9u, 1u, 3u, &output) ==
          HOSTED_REASSEMBLY_PENDING);
    memset(large_payload, 0x3d, sizeof large_payload);
    frame = make_frame(large_payload, sizeof large_payload, 10u, 0u);
    expect_rejected(&state, &frame, 4u, &output);
    CHECK(state.active == 0u);

    frame = make_frame(bytes, 0u, 11u, 0u);
    expect_rejected(&state, &frame, 5u, &output);
    CHECK(state.active == 0u);
    expect_output_unchanged(&output, &before);
    expect_complete(&state, bytes, wire_size, 77u, 6u, &output);
}

static void test_interface_number_ignored(void)
{
    struct hosted_reassembly_state state;
    struct hosted_serial_message output, before;
    struct p4_hosted_frame frame;
    uint8_t bytes[64];
    size_t wire_size;

    CHECK(hosted_reassembly_init(&state, 100u));
    wire_size = make_tlv(bytes, sizeof bytes, 15u);
    memset(&output, 0x42, sizeof output);
    memcpy(&before, &output, sizeof before);

    CHECK(feed_bytes(&state, bytes, 0u, 5u, 3u, 1u, 1u, &output) ==
          HOSTED_REASSEMBLY_PENDING);
    frame = make_frame(bytes, wire_size, 50u, 0u);
    frame.interface_type = P4_HOSTED_SERIAL;
    frame.interface_number = 1u;
    CHECK(hosted_reassembly_feed(&state, &frame, 500u, &output) ==
          HOSTED_REASSEMBLY_IGNORED);
    expect_output_unchanged(&output, &before);
    CHECK(state.active == 1u);
    CHECK(state.length == 5u);
    CHECK(state.expected_sequence == 4u);
    CHECK(feed_bytes(&state, bytes, 5u, wire_size - 5u, 4u, 0u,
                     500u, &output) == HOSTED_REASSEMBLY_REJECTED);
    expect_output_unchanged(&output, &before);
    CHECK(state.active == 0u);
    expect_complete(&state, bytes, wire_size, 51u, 501u, &output);
}

static void test_fragment_and_message_bounds(void)
{
    struct hosted_reassembly_state state;
    struct hosted_serial_message output, before;
    uint8_t maximum[HOSTED_REASSEMBLY_BUFFER_SIZE];
    uint8_t too_large[TEST_WIRE_CAPACITY];
    uint8_t one_fragment[HOSTED_REASSEMBLY_FRAGMENT_SIZE];
    size_t wire_size, too_large_size;

    CHECK(hosted_reassembly_init(&state, 100u));
    wire_size = make_tlv(maximum, sizeof maximum,
                         HOSTED_SERIAL_PROTOBUF_MAX);
    memset(&output, 0x71, sizeof output);
    memcpy(&before, &output, sizeof before);
    CHECK(wire_size == HOSTED_REASSEMBLY_BUFFER_SIZE);
    CHECK(feed_bytes(&state, maximum, 0u, 1500u, 8u, 1u, 1u,
                     &output) == HOSTED_REASSEMBLY_PENDING);
    CHECK(feed_bytes(&state, maximum, 1500u, 1500u, 9u, 1u, 2u,
                     &output) == HOSTED_REASSEMBLY_PENDING);
    CHECK(feed_bytes(&state, maximum, 3000u, wire_size - 3000u, 10u, 0u,
                     3u, &output) == HOSTED_REASSEMBLY_COMPLETE);
    CHECK(output.protobuf_size == HOSTED_SERIAL_PROTOBUF_MAX);
    CHECK(memcmp(output.protobuf,
                 maximum + HOSTED_SERIAL_MESSAGE_OVERHEAD,
                 HOSTED_SERIAL_PROTOBUF_MAX) == 0);
    memcpy(&before, &output, sizeof before);

    too_large_size = make_oversized_tlv(too_large, sizeof too_large,
                                        HOSTED_SERIAL_PROTOBUF_MAX + 1u);
    CHECK(too_large_size == HOSTED_REASSEMBLY_BUFFER_SIZE + 1u);
    CHECK(feed_bytes(&state, too_large, 0u, 1500u, 11u, 1u, 4u,
                     &output) == HOSTED_REASSEMBLY_PENDING);
    CHECK(feed_bytes(&state, too_large, 1500u, 1500u, 12u, 1u, 5u,
                     &output) == HOSTED_REASSEMBLY_PENDING);
    CHECK(feed_bytes(&state, too_large, 3000u, 1109u, 13u, 0u, 6u,
                     &output) == HOSTED_REASSEMBLY_REJECTED);
    expect_output_unchanged(&output, &before);
    CHECK(state.active == 0u);

    wire_size = make_tlv(one_fragment, sizeof one_fragment, 1488u);
    CHECK(wire_size == sizeof one_fragment);
    expect_complete(&state, one_fragment, wire_size, 1000u, 7u, &output);
}

static void test_fragment_count_limit(void)
{
    struct hosted_reassembly_state state;
    struct hosted_serial_message output, before;
    uint8_t at_limit[16], over_limit[17];
    size_t wire_size, i;

    CHECK(hosted_reassembly_init(&state, 100u));
    wire_size = make_tlv(at_limit, sizeof at_limit, 4u);
    CHECK(wire_size == sizeof at_limit);
    memset(&output, 0x66, sizeof output);
    memcpy(&before, &output, sizeof before);
    for (i = 0; i < sizeof at_limit; ++i) {
        enum hosted_reassembly_result result = feed_bytes(
            &state, at_limit, i, 1u, (uint16_t)(100u + i),
            i + 1u < sizeof at_limit, (uint32_t)i, &output);
        if (i + 1u < sizeof at_limit) {
            CHECK(result == HOSTED_REASSEMBLY_PENDING);
            expect_output_unchanged(&output, &before);
        } else {
            CHECK(result == HOSTED_REASSEMBLY_COMPLETE);
        }
    }
    CHECK(output.wire_size == sizeof at_limit);
    memcpy(&before, &output, sizeof before);

    wire_size = make_tlv(over_limit, sizeof over_limit, 5u);
    CHECK(wire_size == sizeof over_limit);
    for (i = 0; i < HOSTED_REASSEMBLY_FRAGMENT_LIMIT - 1u; ++i)
        CHECK(feed_bytes(&state, over_limit, i, 1u, (uint16_t)(200u + i),
                         1u, (uint32_t)(20u + i), &output) ==
              HOSTED_REASSEMBLY_PENDING);
    CHECK(feed_bytes(&state, over_limit,
                     HOSTED_REASSEMBLY_FRAGMENT_LIMIT - 1u, 1u,
                     (uint16_t)(200u + HOSTED_REASSEMBLY_FRAGMENT_LIMIT - 1u),
                     1u, 40u, &output) == HOSTED_REASSEMBLY_REJECTED);
    expect_output_unchanged(&output, &before);
    CHECK(state.active == 0u);
}

static void test_timeout_and_tick_wrap(void)
{
    struct hosted_reassembly_state state;
    struct hosted_serial_message output, before;
    uint8_t prefix[32], complete[32];
    size_t prefix_size, complete_size;

    CHECK(hosted_reassembly_init(&state, 10u));
    prefix_size = make_tlv(prefix, sizeof prefix, 5u);
    complete_size = make_tlv(complete, sizeof complete, 1u);
    (void)prefix_size;
    memset(&output, 0xb4, sizeof output);
    memcpy(&before, &output, sizeof before);

    CHECK(feed_bytes(&state, prefix, 0u, 3u, 10u, 1u, 100u, &output) ==
          HOSTED_REASSEMBLY_PENDING);
    /* This would be a valid standalone envelope if retried as a new message.
     * At the exact deadline it is refused as the expired message's tail. */
    CHECK(feed_bytes(&state, complete, 0u, complete_size, 11u, 0u,
                     110u, &output) == HOSTED_REASSEMBLY_REJECTED);
    expect_output_unchanged(&output, &before);
    CHECK(state.active == 0u);
    expect_complete(&state, complete, complete_size, 12u, 111u, &output);
    memcpy(&before, &output, sizeof before);

    CHECK(hosted_reassembly_init(&state, 10u));
    CHECK(feed_bytes(&state, prefix, 0u, 4u, UINT16_MAX, 1u,
                     UINT32_MAX - 5u, &output) == HOSTED_REASSEMBLY_PENDING);
    CHECK(feed_bytes(&state, prefix, 4u, prefix_size - 4u, 0u, 0u, 4u,
                     &output) == HOSTED_REASSEMBLY_REJECTED);
    expect_output_unchanged(&output, &before);
    CHECK(state.active == 0u);

    CHECK(hosted_reassembly_init(&state, 20u));
    CHECK(feed_bytes(&state, prefix, 0u, 4u, UINT16_MAX, 1u,
                     UINT32_MAX - 5u, &output) == HOSTED_REASSEMBLY_PENDING);
    CHECK(feed_bytes(&state, prefix, 4u, prefix_size - 4u, 0u, 0u, 4u,
                     &output) == HOSTED_REASSEMBLY_COMPLETE);
    CHECK(output.protobuf_size == 5u);
    memcpy(&before, &output, sizeof before);

    /* Progress on the middle fragment does not extend the message deadline. */
    CHECK(hosted_reassembly_init(&state, 10u));
    CHECK(feed_bytes(&state, prefix, 0u, 4u, 10u, 1u, 100u, &output) ==
          HOSTED_REASSEMBLY_PENDING);
    CHECK(feed_bytes(&state, prefix, 4u, 4u, 11u, 1u, 109u, &output) ==
          HOSTED_REASSEMBLY_PENDING);
    CHECK(state.first_tick == 100u);
    CHECK(feed_bytes(&state, prefix, 8u, prefix_size - 8u, 12u, 0u,
                     110u, &output) == HOSTED_REASSEMBLY_REJECTED);
    expect_output_unchanged(&output, &before);
    CHECK(state.active == 0u);
}

static void test_bad_final_tlvs(void)
{
    struct hosted_reassembly_state state;
    struct hosted_serial_message output, before;
    uint8_t bytes[64], damaged[64];
    size_t wire_size;

    CHECK(hosted_reassembly_init(&state, 100u));
    wire_size = make_tlv(bytes, sizeof bytes, 4u);
    memset(&output, 0xc3, sizeof output);
    memcpy(&before, &output, sizeof before);

    memcpy(damaged, bytes, wire_size);
    damaged[3] = 'X';
    expect_rejected(&state, &(struct p4_hosted_frame){
        .interface_type = P4_HOSTED_SERIAL,
        .interface_number = 0u,
        .flags = 0u,
        .sequence = 1u,
        .payload = damaged,
        .payload_size = wire_size
    }, 1u, &output);

    /* Declared protobuf length exceeds the available bytes. */
    expect_rejected(&state, &(struct p4_hosted_frame){
        .interface_type = P4_HOSTED_SERIAL,
        .interface_number = 0u,
        .flags = 0u,
        .sequence = 2u,
        .payload = bytes,
        .payload_size = wire_size - 1u
    }, 2u, &output);

    /* The same bad envelope is rejected after transport fragmentation. */
    CHECK(feed_bytes(&state, damaged, 0u, 5u, 3u, 1u, 3u, &output) ==
          HOSTED_REASSEMBLY_PENDING);
    CHECK(feed_bytes(&state, damaged, 5u, wire_size - 5u, 4u, 0u,
                     4u, &output) == HOSTED_REASSEMBLY_REJECTED);
    expect_output_unchanged(&output, &before);
    CHECK(state.active == 0u);

    memcpy(damaged, bytes, wire_size);
    damaged[wire_size] = 0u;
    expect_rejected(&state, &(struct p4_hosted_frame){
        .interface_type = P4_HOSTED_SERIAL,
        .interface_number = 0u,
        .flags = 0u,
        .sequence = 5u,
        .payload = damaged,
        .payload_size = wire_size + 1u
    }, 5u, &output);
}

static void test_alias_refusal(void)
{
    struct hosted_reassembly_state state, before_state;
    struct hosted_serial_message output, before_output;
    struct p4_hosted_frame frame;
    uint8_t bytes[32];
    size_t wire_size;

    CHECK(hosted_reassembly_init(&state, 100u));
    wire_size = make_tlv(bytes, sizeof bytes, 4u);
    memset(&output, 0x2a, sizeof output);
    frame = make_frame(bytes, wire_size, 1u, 0u);

    memset(state.bytes, 0x95, sizeof state.bytes);
    memcpy(&before_state, &state, sizeof state);
    memcpy(&before_output, &output, sizeof output);
    frame.payload = state.bytes;
    frame.payload_size = 1u;
    expect_rejected(&state, &frame, 1u, &output);
    CHECK(memcmp(&state, &before_state, sizeof state) == 0);
    expect_output_unchanged(&output, &before_output);

    frame = make_frame((const uint8_t *)&output, 1u, 1u, 0u);
    memcpy(&before_state, &state, sizeof state);
    memcpy(&before_output, &output, sizeof output);
    expect_rejected(&state, &frame, 1u, &output);
    CHECK(memcmp(&state, &before_state, sizeof state) == 0);
    expect_output_unchanged(&output, &before_output);

    frame = make_frame(bytes, wire_size, 1u, 0u);
    frame.payload = (const uint8_t *)&frame;
    frame.payload_size = 1u;
    memcpy(&before_state, &state, sizeof state);
    expect_rejected(&state, &frame, 1u, &output);
    CHECK(memcmp(&state, &before_state, sizeof state) == 0);

    /* Object overlaps must be refused before reading the aliased metadata. */
    {
        union state_frame_alias {
            struct hosted_reassembly_state state;
            struct p4_hosted_frame frame;
        } alias;
        uint8_t saved[sizeof alias];

        memset(&alias, 0x73, sizeof alias);
        memcpy(saved, &alias, sizeof saved);
        expect_rejected(&alias.state, &alias.frame, 1u, &output);
        CHECK(memcmp(&alias, saved, sizeof saved) == 0);
    }
    {
        union frame_output_alias {
            struct p4_hosted_frame frame;
            struct hosted_serial_message output;
        } alias;
        uint8_t saved[sizeof alias];

        memset(&alias, 0x48, sizeof alias);
        memcpy(saved, &alias, sizeof saved);
        memcpy(&before_state, &state, sizeof state);
        CHECK(hosted_reassembly_feed(&state, &alias.frame, 1u,
                                     &alias.output) == HOSTED_REASSEMBLY_REJECTED);
        CHECK(memcmp(&alias, saved, sizeof saved) == 0);
        CHECK(memcmp(&state, &before_state, sizeof state) == 0);
    }

    /* The state/output object overlap is refused before either is touched. */
    {
        union state_output_alias {
            struct hosted_reassembly_state state;
            struct hosted_serial_message output;
            uint8_t bytes[HOSTED_REASSEMBLY_BUFFER_SIZE +
                          sizeof(struct hosted_serial_message)];
        } alias;
        uint8_t saved[sizeof alias.bytes];

        memset(&alias, 0x6e, sizeof alias);
        memcpy(saved, alias.bytes, sizeof saved);
        CHECK(hosted_reassembly_feed(&alias.state, &frame, 2u,
                                     &alias.output) ==
              HOSTED_REASSEMBLY_REJECTED);
        CHECK(memcmp(alias.bytes, saved, sizeof saved) == 0);

        /* A missing frame must not clear state when output aliases it. */
        CHECK(hosted_reassembly_feed(&alias.state, NULL, 2u,
                                     &alias.output) ==
              HOSTED_REASSEMBLY_REJECTED);
        CHECK(memcmp(alias.bytes, saved, sizeof saved) == 0);
    }
}

static void test_reset_invalidates_partial(void)
{
    struct hosted_reassembly_state state;
    struct hosted_serial_message output, before;
    uint8_t bytes[32];
    size_t wire_size;

    CHECK(hosted_reassembly_init(&state, 100u));
    wire_size = make_tlv(bytes, sizeof bytes, 3u);
    memset(&output, 0x11, sizeof output);
    memcpy(&before, &output, sizeof before);
    CHECK(feed_bytes(&state, bytes, 0u, 4u, 1u, 1u, 1u, &output) ==
          HOSTED_REASSEMBLY_PENDING);
    hosted_reassembly_reset(&state);
    CHECK(state.active == 0u);
    CHECK(state.timeout_ms == 100u);
    expect_output_unchanged(&output, &before);
    expect_complete(&state, bytes, wire_size, 600u, 2u, &output);
}

int main(void)
{
    test_init_and_single_frame();
    test_fragmented_wrap_and_interleaving();
    test_wire_decode_reassemble_event();
    test_sequence_refusal_and_recovery();
    test_invalid_fragments_discard_partial();
    test_interface_number_ignored();
    test_fragment_and_message_bounds();
    test_fragment_count_limit();
    test_timeout_and_tick_wrap();
    test_bad_final_tlvs();
    test_alias_refusal();
    test_reset_invalidates_partial();
    printf("PASS hosted reassembly (%lu checks)\n", checks);
    return 0;
}
