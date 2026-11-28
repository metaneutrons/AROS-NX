/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
/* Host-model codec tests only. No hardware or firmware-compatibility claim. */
#include "hosted_epoch_reassembly.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_GENERATION UINT32_C(0x360)
#define TEST_SEQUENCE UINT16_C(0xfffe)

static unsigned long checks;

#define CHECK(expression) do { \
    ++checks; \
    if (!(expression)) { \
        fprintf(stderr, "check failed at %s:%d: %s\n", __FILE__, __LINE__, \
                #expression); \
        exit(1); \
    } \
} while (0)

static const uint8_t rpc_response_tlv[] = {
    0x01, 0x06, 0x00, 'R', 'P', 'C', 'R', 's', 'p',
    0x02, 0x06, 0x00, 0x08, 0x02, 0x10, 0x01, 0x18, 0x01
};
static const uint8_t rpc_event_tlv[] = {
    0x01, 0x06, 0x00, 'R', 'P', 'C', 'E', 'v', 't',
    0x02, 0x04, 0x00, 0x08, 0x03, 0x10, 0x01
};

struct encoded_frame {
    uint8_t bytes[P4_HOSTED_FRAME_MAX];
    size_t wire_size;
    struct p4_hosted_frame frame;
};

static int all_zero(const uint8_t *bytes, size_t size)
{
    size_t i;
    for (i = 0; i < size; ++i)
        if (bytes[i] != 0) return 0;
    return 1;
}

static void expect_wiped(const struct p4_hosted_reassembly *state)
{
    CHECK(state->used == 0);
    CHECK(state->fragments == 0);
    CHECK(state->complete == 0);
    CHECK(state->poisoned == 1);
    CHECK(all_zero(state->bytes, sizeof state->bytes));
}

static void make_frame(struct encoded_frame *encoded, const uint8_t *payload,
    size_t payload_size, uint16_t sequence, uint8_t flags,
    uint8_t interface_type, uint8_t interface_number, uint8_t throttle,
    uint8_t packet_type)
{
    struct p4_hosted_frame input;
    input.interface_type = interface_type;
    input.interface_number = interface_number;
    input.flags = flags;
    input.throttle = throttle;
    input.packet_type = packet_type;
    input.sequence = sequence;
    input.payload = payload;
    input.payload_size = payload_size;
    input.wire_size = 0;
    encoded->wire_size = 0;
    CHECK(p4_hosted_encode(encoded->bytes, sizeof encoded->bytes, 0,
                          &input, &encoded->wire_size));
    CHECK(p4_hosted_decode(encoded->bytes, encoded->wire_size, 0,
                           &encoded->frame));
    CHECK(encoded->frame.payload_size == payload_size);
    CHECK(encoded->frame.sequence == sequence);
}

static enum p4_hosted_reassembly_result feed_bytes(
    struct p4_hosted_reassembly *state, uint32_t generation,
    const uint8_t *payload, size_t payload_size, uint16_t sequence,
    uint8_t flags)
{
    struct encoded_frame encoded;
    enum p4_hosted_reassembly_result result;
    make_frame(&encoded, payload, payload_size, sequence, flags,
        P4_HOSTED_SERIAL, 0, 0, 0);
    result = p4_hosted_reassembly_feed(state, generation, &encoded.frame);
    memset(encoded.bytes, 0xa5, sizeof encoded.bytes);
    return result;
}

static void state_start(struct p4_hosted_reassembly *state,
                        uint32_t generation)
{
    CHECK(p4_hosted_reassembly_start(state, generation));
    CHECK(state->initialized == 1);
    CHECK(state->generation == generation);
}

static void check_view(const struct p4_hosted_reassembly *state,
    uint32_t generation, const uint8_t *wire, size_t wire_size,
    enum hosted_serial_endpoint endpoint, const uint8_t *protobuf,
    size_t protobuf_size)
{
    struct hosted_serial_message message;
    memset(&message, 0xcc, sizeof message);
    CHECK(p4_hosted_reassembly_view(state, generation, &message));
    CHECK(message.endpoint == endpoint);
    CHECK(message.protobuf == state->bytes + HOSTED_SERIAL_MESSAGE_OVERHEAD);
    CHECK(message.protobuf_size == protobuf_size);
    CHECK(message.wire_size == wire_size);
    CHECK(memcmp(state->bytes, wire, wire_size) == 0);
    CHECK(memcmp(message.protobuf, protobuf, protobuf_size) == 0);
}

/* Return one only when an exact incrementing uint16 sequence reconstructs
 * every byte. The source fragments are overwritten immediately after feed,
 * exercising ownership independent of receive/DMA storage. */
static int feed_chunks(struct p4_hosted_reassembly *state, uint32_t generation,
    const uint8_t *wire, size_t wire_size, const size_t *chunks,
    unsigned chunk_count, uint16_t first_sequence,
    enum hosted_serial_endpoint endpoint, const uint8_t *protobuf,
    size_t protobuf_size)
{
    size_t offset = 0;
    unsigned i;
    state_start(state, generation);
    for (i = 0; i < chunk_count; ++i) {
        struct encoded_frame encoded;
        uint8_t flags = i + 1u < chunk_count ? P4_HOSTED_MORE_FRAGMENT : 0;
        uint16_t sequence = (uint16_t)(first_sequence + i);
        enum p4_hosted_reassembly_result result;
        if (chunks[i] == 0 || chunks[i] > wire_size - offset) return 0;
        make_frame(&encoded, wire + offset, chunks[i], sequence, flags,
            P4_HOSTED_SERIAL, 0, 0, 0);
        result = p4_hosted_reassembly_feed(state, generation, &encoded.frame);
        memset(encoded.bytes, 0xd3, sizeof encoded.bytes);
        offset += chunks[i];
        if (i + 1u < chunk_count) {
            if (result != P4_HOSTED_REASSEMBLY_PARTIAL) return 0;
        } else if (result != P4_HOSTED_REASSEMBLY_COMPLETE) {
            return 0;
        }
    }
    if (offset != wire_size) return 0;
    check_view(state, generation, wire, wire_size, endpoint,
               protobuf, protobuf_size);
    return 1;
}

static void make_tlv(uint8_t *wire, const uint8_t endpoint[6],
                     const uint8_t *protobuf, size_t protobuf_size)
{
    wire[0] = 1; wire[1] = 6; wire[2] = 0;
    memcpy(wire + 3, endpoint, 6);
    wire[9] = 2;
    wire[10] = (uint8_t)protobuf_size;
    wire[11] = (uint8_t)(protobuf_size >> 8);
    memcpy(wire + HOSTED_SERIAL_MESSAGE_OVERHEAD, protobuf, protobuf_size);
}

static void test_literal_vectors_split_at_every_boundary(void)
{
    const uint8_t *vectors[2] = { rpc_response_tlv, rpc_event_tlv };
    const size_t sizes[2] = { sizeof rpc_response_tlv, sizeof rpc_event_tlv };
    const uint8_t *protobufs[2] = { rpc_response_tlv + 12, rpc_event_tlv + 12 };
    const size_t protobuf_sizes[2] = { 6, 4 };
    const enum hosted_serial_endpoint endpoints[2] = {
        HOSTED_SERIAL_ENDPOINT_RPCRSP, HOSTED_SERIAL_ENDPOINT_RPCEVT
    };
    struct p4_hosted_reassembly state = {0};
    uint32_t generation = 10;
    unsigned vector;
    for (vector = 0; vector < 2; ++vector) {
        size_t split;
        for (split = 1; split < sizes[vector]; ++split) {
            size_t chunks[2] = { split, sizes[vector] - split };
            CHECK(feed_chunks(&state, generation++, vectors[vector],
                sizes[vector], chunks, 2, TEST_SEQUENCE,
                endpoints[vector], protobufs[vector],
                protobuf_sizes[vector]));
            CHECK(state.complete == 1 && state.fragments == 2);
            CHECK(p4_hosted_reassembly_release(&state, state.generation));
            CHECK(state.used == 0 && state.fragments == 0 && !state.complete);
            CHECK(all_zero(state.bytes, sizeof state.bytes));
        }
    }
}

static void test_lifecycle_and_same_sequence_after_release(void)
{
    struct p4_hosted_reassembly state = {0}, saved;
    struct encoded_frame encoded;
    struct hosted_serial_message view, before_view;
    uint32_t generation = TEST_GENERATION;

    CHECK(p4_hosted_reassembly_start(&state, generation));
    make_frame(&encoded, rpc_response_tlv, 7, 44,
               P4_HOSTED_MORE_FRAGMENT, P4_HOSTED_SERIAL, 0, 0, 0);
    CHECK(p4_hosted_reassembly_feed(&state, generation, &encoded.frame) ==
          P4_HOSTED_REASSEMBLY_PARTIAL);
    saved = state;
    CHECK(!p4_hosted_reassembly_start(&state, generation));
    CHECK(memcmp(&saved, &state, sizeof state) == 0);
    memset(&view, 0x69, sizeof view);
    before_view = view;
    CHECK(!p4_hosted_reassembly_view(&state, generation, &view));
    CHECK(memcmp(&before_view, &view, sizeof view) == 0);
    CHECK(!p4_hosted_reassembly_release(&state, generation));
    CHECK(memcmp(&saved, &state, sizeof state) == 0);

    CHECK(p4_hosted_reassembly_start(&state, generation + 1u));
    CHECK(state.used == 0 && state.fragments == 0 && !state.complete &&
          !state.poisoned);
    CHECK(all_zero(state.bytes, sizeof state.bytes));
    generation += 1u;
    CHECK(feed_bytes(&state, generation, rpc_response_tlv,
        sizeof rpc_response_tlv, 44, 0) == P4_HOSTED_REASSEMBLY_COMPLETE);
    check_view(&state, generation, rpc_response_tlv, sizeof rpc_response_tlv,
        HOSTED_SERIAL_ENDPOINT_RPCRSP, rpc_response_tlv + 12, 6);

    saved = state;
    make_frame(&encoded, rpc_event_tlv, sizeof rpc_event_tlv, 0x1234, 0,
               P4_HOSTED_SERIAL, 0, 0, 0);
    CHECK(p4_hosted_reassembly_feed(&state, generation, &encoded.frame) ==
          P4_HOSTED_REASSEMBLY_REFUSED);
    CHECK(memcmp(&saved, &state, sizeof state) == 0);
    CHECK(p4_hosted_reassembly_release(&state, generation));
    CHECK(!p4_hosted_reassembly_view(&state, generation, &view));
    CHECK(!p4_hosted_reassembly_release(&state, generation));
    CHECK(feed_bytes(&state, generation, rpc_event_tlv,
        sizeof rpc_event_tlv, 44, 0) == P4_HOSTED_REASSEMBLY_COMPLETE);
    check_view(&state, generation, rpc_event_tlv, sizeof rpc_event_tlv,
        HOSTED_SERIAL_ENDPOINT_RPCEVT, rpc_event_tlv + 12, 4);
}

static void test_sequence_increments_and_rejections(void)
{
    struct p4_hosted_reassembly state = {0};
    uint32_t generation = 100;
    size_t chunks[2] = { 7, sizeof rpc_response_tlv - 7 };
    struct encoded_frame first, second;

    CHECK(feed_chunks(&state, generation++, rpc_response_tlv,
        sizeof rpc_response_tlv, chunks, 2, UINT16_C(0xffff),
        HOSTED_SERIAL_ENDPOINT_RPCRSP, rpc_response_tlv + 12, 6));
    CHECK(p4_hosted_reassembly_release(&state, state.generation));

    state_start(&state, generation);
    make_frame(&first, rpc_response_tlv, chunks[0], 20,
        P4_HOSTED_MORE_FRAGMENT, P4_HOSTED_SERIAL, 0, 0, 0);
    CHECK(p4_hosted_reassembly_feed(&state, generation, &first.frame) ==
          P4_HOSTED_REASSEMBLY_PARTIAL);
    make_frame(&second, rpc_response_tlv + chunks[0], chunks[1], 20, 0,
        P4_HOSTED_SERIAL, 0, 0, 0); /* duplicate, not next sequence */
    CHECK(p4_hosted_reassembly_feed(&state, generation, &second.frame) ==
          P4_HOSTED_REASSEMBLY_PROTOCOL);
    expect_wiped(&state);
    CHECK(p4_hosted_reassembly_feed(&state, generation, &second.frame) ==
          P4_HOSTED_REASSEMBLY_REFUSED);

    generation++;
    state_start(&state, generation);
    make_frame(&first, rpc_response_tlv, chunks[0], 30,
        P4_HOSTED_MORE_FRAGMENT, P4_HOSTED_SERIAL, 0, 0, 0);
    CHECK(p4_hosted_reassembly_feed(&state, generation, &first.frame) ==
          P4_HOSTED_REASSEMBLY_PARTIAL);
    make_frame(&second, rpc_response_tlv + chunks[0], chunks[1], 32, 0,
        P4_HOSTED_SERIAL, 0, 0, 0); /* skipped sequence */
    CHECK(p4_hosted_reassembly_feed(&state, generation, &second.frame) ==
          P4_HOSTED_REASSEMBLY_PROTOCOL);
    expect_wiped(&state);
}

static void test_arbitrary_chunks_and_fragment_bound(void)
{
    struct p4_hosted_reassembly state = {0};
    size_t chunks3[] = { 5, 1, sizeof rpc_response_tlv - 6 };
    size_t chunks16[16];
    uint8_t short_tlv[17];
    static const uint8_t endpoint[6] = { 'R', 'P', 'C', 'E', 'v', 't' };
    static const uint8_t short_proto[5] = { 0x08, 0x03, 0x10, 0x01, 0x18 };
    uint32_t generation = 200;
    unsigned i;

    CHECK(feed_chunks(&state, generation++, rpc_response_tlv,
        sizeof rpc_response_tlv, chunks3, 3, 77,
        HOSTED_SERIAL_ENDPOINT_RPCRSP, rpc_response_tlv + 12, 6));
    CHECK(state.fragments == 3);
    CHECK(p4_hosted_reassembly_release(&state, state.generation));

    for (i = 0; i < 15; ++i) chunks16[i] = 1;
    chunks16[15] = sizeof rpc_response_tlv - 15u;
    CHECK(feed_chunks(&state, generation++, rpc_response_tlv,
        sizeof rpc_response_tlv, chunks16, 16, UINT16_C(0xfff8),
        HOSTED_SERIAL_ENDPOINT_RPCRSP, rpc_response_tlv + 12, 6));
    CHECK(state.fragments == 16);
    CHECK(p4_hosted_reassembly_release(&state, state.generation));

    make_tlv(short_tlv, endpoint, short_proto, sizeof short_proto);
    state_start(&state, generation++);
    for (i = 0; i < 15; ++i) {
        enum p4_hosted_reassembly_result result = feed_bytes(&state,
            state.generation, short_tlv + i, 1,
            (uint16_t)(300u + i), P4_HOSTED_MORE_FRAGMENT);
        CHECK(result == P4_HOSTED_REASSEMBLY_PARTIAL);
    }
    /* The sixteenth MORE cannot be safely completed within the 16-fragment
     * policy; it is consumed and poisons instead of waiting for fragment 17. */
    CHECK(feed_bytes(&state, state.generation, short_tlv + 15, 1,
        (uint16_t)(300u + 15u), P4_HOSTED_MORE_FRAGMENT) ==
        P4_HOSTED_REASSEMBLY_PROTOCOL);
    expect_wiped(&state);
    {
        struct p4_hosted_reassembly poisoned = state;
        CHECK(feed_bytes(&state, state.generation, short_tlv + 16, 1,
            (uint16_t)(300u + 16u), 0) == P4_HOSTED_REASSEMBLY_REFUSED);
        CHECK(memcmp(&poisoned, &state, sizeof state) == 0);
    }
}

static void test_large_serial_limits_and_fragment_size(void)
{
    static const uint8_t response_endpoint[6] = { 'R', 'P', 'C', 'R', 's', 'p' };
    uint8_t protobuf[HOSTED_SERIAL_PROTOBUF_MAX + 1u];
    uint8_t wire[HOSTED_SERIAL_MESSAGE_OVERHEAD + sizeof protobuf];
    struct p4_hosted_reassembly state = {0};
    size_t max_chunks[] = { 1500, 1500, 1108 };
    uint32_t generation = 300;
    size_t i;

    for (i = 0; i < sizeof protobuf; ++i)
        protobuf[i] = (uint8_t)(i * 31u + 7u);
    make_tlv(wire, response_endpoint, protobuf, HOSTED_SERIAL_PROTOBUF_MAX);
    CHECK(sizeof rpc_response_tlv == HOSTED_SERIAL_MESSAGE_OVERHEAD + 6u);
    CHECK(feed_chunks(&state, generation++, wire,
        HOSTED_SERIAL_MESSAGE_OVERHEAD + HOSTED_SERIAL_PROTOBUF_MAX,
        max_chunks, 3, TEST_SEQUENCE, HOSTED_SERIAL_ENDPOINT_RPCRSP,
        protobuf, HOSTED_SERIAL_PROTOBUF_MAX));
    CHECK(state.used == HOSTED_SERIAL_MESSAGE_MAX && state.fragments == 3);
    CHECK(p4_hosted_reassembly_release(&state, state.generation));

    make_tlv(wire, response_endpoint, protobuf,
             HOSTED_SERIAL_PROTOBUF_MAX + 1u);
    state_start(&state, generation++);
    CHECK(feed_bytes(&state, state.generation, wire, 1500, 41,
                     P4_HOSTED_MORE_FRAGMENT) == P4_HOSTED_REASSEMBLY_PARTIAL);
    CHECK(feed_bytes(&state, state.generation, wire + 1500, 1500, 42,
                     P4_HOSTED_MORE_FRAGMENT) == P4_HOSTED_REASSEMBLY_PARTIAL);
    CHECK(feed_bytes(&state, state.generation, wire + 3000, 1109, 43, 0) ==
          P4_HOSTED_REASSEMBLY_PROTOCOL);
    expect_wiped(&state);

    state_start(&state, generation++);
    CHECK(feed_bytes(&state, state.generation, wire, 1500, 51,
                     P4_HOSTED_MORE_FRAGMENT) == P4_HOSTED_REASSEMBLY_PARTIAL);
    CHECK(feed_bytes(&state, state.generation, wire + 1500, 1500, 52,
                     P4_HOSTED_MORE_FRAGMENT) == P4_HOSTED_REASSEMBLY_PARTIAL);
    CHECK(feed_bytes(&state, state.generation, wire + 3000, 1108, 53,
                     P4_HOSTED_MORE_FRAGMENT) == P4_HOSTED_REASSEMBLY_PARTIAL);
    CHECK(state.used == HOSTED_SERIAL_MESSAGE_MAX);
    CHECK(feed_bytes(&state, state.generation, wire + 4108, 1, 54, 0) ==
          P4_HOSTED_REASSEMBLY_PROTOCOL);
    expect_wiped(&state);

    /* Frame sizes above pinned ETH_DATA_LEN are consumed protocol errors,
     * even though the outer 1536-byte Hosted frame could carry them. */
    {
        uint8_t payload[1501] = {0};
        struct encoded_frame encoded;
        state_start(&state, generation++);
        make_frame(&encoded, payload, sizeof payload, 8, 0,
            P4_HOSTED_SERIAL, 0, 0, 0);
        CHECK(p4_hosted_reassembly_feed(&state, state.generation,
            &encoded.frame) == P4_HOSTED_REASSEMBLY_PROTOCOL);
        expect_wiped(&state);
    }
}

static void test_protocol_fields_and_invalid_final_tlv(void)
{
    struct p4_hosted_reassembly state = {0};
    struct encoded_frame encoded;
    struct p4_hosted_frame bad;
    const uint8_t *payload = rpc_response_tlv;
    unsigned variant;
    uint32_t generation = 400;
    static const uint8_t invalid[] = { 0xff, 0x00, 0x02, 0x11 };

    for (variant = 0; variant < 5; ++variant) {
        state_start(&state, generation++);
        make_frame(&encoded, payload, sizeof rpc_response_tlv, 5, 0,
            P4_HOSTED_SERIAL, 0, 0, 0);
        bad = encoded.frame;
        if (variant == 0) bad.interface_type = P4_HOSTED_STA;
        if (variant == 1) bad.interface_number = 1;
        if (variant == 2) bad.throttle = 1;
        if (variant == 3) bad.packet_type = 1;
        if (variant == 4) bad.flags = 2;
        CHECK(p4_hosted_reassembly_feed(&state, state.generation, &bad) ==
              P4_HOSTED_REASSEMBLY_PROTOCOL);
        expect_wiped(&state);
    }

    state_start(&state, generation++);
    CHECK(feed_bytes(&state, state.generation, invalid, sizeof invalid, 9, 0) ==
          P4_HOSTED_REASSEMBLY_PROTOCOL);
    expect_wiped(&state);
}

static void test_preflight_refusals_preserve_all_inputs(void)
{
    struct p4_hosted_reassembly state = {0}, saved;
    struct encoded_frame encoded;
    struct p4_hosted_frame frame, original_frame;
    struct hosted_serial_message view, original_view;
    uintptr_t near_end = UINTPTR_MAX - 1u;
    uint32_t generation = 500;

    state_start(&state, generation);
    make_frame(&encoded, rpc_response_tlv, sizeof rpc_response_tlv,
        11, P4_HOSTED_MORE_FRAGMENT, P4_HOSTED_SERIAL, 0, 0, 0);
    frame = encoded.frame;
    saved = state;
    original_frame = frame;
#define CHECK_FEED_REFUSED(frame_arg, gen_arg) do { \
    struct p4_hosted_frame frame_before = frame; \
    CHECK(p4_hosted_reassembly_feed(&state, (gen_arg), (frame_arg)) == \
          P4_HOSTED_REASSEMBLY_REFUSED); \
    CHECK(memcmp(&saved, &state, sizeof state) == 0); \
    CHECK(memcmp(&frame_before, &frame, sizeof frame) == 0); \
    CHECK(memcmp(encoded.bytes + P4_HOSTED_HEADER_SIZE, \
                 rpc_response_tlv, sizeof rpc_response_tlv) == 0); \
} while (0)
    CHECK_FEED_REFUSED(&frame, generation + 1u); /* stale epoch */
    CHECK_FEED_REFUSED((const struct p4_hosted_frame *)(uintptr_t)near_end,
                       generation);
    CHECK_FEED_REFUSED((const struct p4_hosted_frame *)(void *)&state,
                       generation);

    frame = original_frame;
    frame.payload = state.bytes + 5;
    frame.payload_size = 2;
    CHECK_FEED_REFUSED(&frame, generation); /* state/payload alias */
    frame = original_frame;
    frame.payload = (const uint8_t *)(const void *)&frame;
    frame.payload_size = 1;
    CHECK_FEED_REFUSED(&frame, generation); /* metadata/payload alias */
    frame = original_frame;
    frame.payload = (const uint8_t *)(uintptr_t)near_end;
    frame.payload_size = 4;
    CHECK_FEED_REFUSED(&frame, generation); /* pointer range overflow */
    frame = original_frame;
    frame.payload = NULL;
    CHECK_FEED_REFUSED(&frame, generation);
    frame = original_frame;
    frame.payload_size = 0;
    CHECK_FEED_REFUSED(&frame, generation);
    frame = original_frame;
    frame.payload_size = SIZE_MAX;
    CHECK_FEED_REFUSED(&frame, generation);
    CHECK(p4_hosted_reassembly_feed(
        (struct p4_hosted_reassembly *)(uintptr_t)near_end,
        generation, &original_frame) == P4_HOSTED_REASSEMBLY_REFUSED);
#undef CHECK_FEED_REFUSED

    memset(&view, 0x73, sizeof view);
    original_view = view;
    CHECK(!p4_hosted_reassembly_view(&state, generation + 1u, &view));
    CHECK(memcmp(&view, &original_view, sizeof view) == 0);
    CHECK(!p4_hosted_reassembly_view(&state, generation,
        (struct hosted_serial_message *)(void *)state.bytes));
    CHECK(memcmp(&saved, &state, sizeof state) == 0);
    CHECK(!p4_hosted_reassembly_release(&state, generation + 1u));
    CHECK(memcmp(&saved, &state, sizeof state) == 0);
    CHECK(!p4_hosted_reassembly_abort(&state, generation + 1u));
    CHECK(memcmp(&saved, &state, sizeof state) == 0);
    CHECK(!p4_hosted_reassembly_start(&state, 0));
    CHECK(memcmp(&saved, &state, sizeof state) == 0);
    CHECK(!p4_hosted_reassembly_start(
        (struct p4_hosted_reassembly *)(uintptr_t)near_end, generation + 1u));
    CHECK(memcmp(&saved, &state, sizeof state) == 0);
    CHECK(!p4_hosted_reassembly_view(
        (const struct p4_hosted_reassembly *)(uintptr_t)near_end,
        generation, &view));
    CHECK(memcmp(&view, &original_view, sizeof view) == 0);
    CHECK(!p4_hosted_reassembly_release(
        (struct p4_hosted_reassembly *)(uintptr_t)near_end, generation));
    CHECK(!p4_hosted_reassembly_abort(
        (struct p4_hosted_reassembly *)(uintptr_t)near_end, generation));
}

static void test_forged_sizes_and_internal_metadata(void)
{
    struct p4_hosted_reassembly state = {0};
    struct encoded_frame encoded;
    uint8_t payload[P4_HOSTED_REASSEMBLY_FRAGMENT_MAX + 1u] = {0};
    uint32_t generation = 600;

    state_start(&state, generation++);
    make_frame(&encoded, payload, sizeof payload, 1, 0,
        P4_HOSTED_SERIAL, 0, 0, 0);
    CHECK(p4_hosted_reassembly_feed(&state, state.generation,
        &encoded.frame) == P4_HOSTED_REASSEMBLY_PROTOCOL);
    expect_wiped(&state);

    state_start(&state, generation++);
    state.used = sizeof state.bytes + 1u;
    CHECK(p4_hosted_reassembly_feed(&state, state.generation,
        &encoded.frame) == P4_HOSTED_REASSEMBLY_PROTOCOL);
    expect_wiped(&state);

    state_start(&state, generation++);
    state.fragments = P4_HOSTED_REASSEMBLY_MAX_FRAGMENTS;
    CHECK(p4_hosted_reassembly_feed(&state, state.generation,
        &encoded.frame) == P4_HOSTED_REASSEMBLY_PROTOCOL);
    expect_wiped(&state);
}

static void test_abort_and_new_epoch_after_poison(void)
{
    struct p4_hosted_reassembly state = {0}, saved;
    uint32_t generation = 700;

    state_start(&state, generation);
    CHECK(feed_bytes(&state, generation, rpc_response_tlv, 5,
        TEST_SEQUENCE, P4_HOSTED_MORE_FRAGMENT) == P4_HOSTED_REASSEMBLY_PARTIAL);
    saved = state;
    CHECK(!p4_hosted_reassembly_abort(&state, generation + 1u));
    CHECK(memcmp(&saved, &state, sizeof state) == 0);
    CHECK(p4_hosted_reassembly_abort(&state, generation));
    expect_wiped(&state);
    CHECK(p4_hosted_reassembly_feed(&state, generation, NULL) ==
          P4_HOSTED_REASSEMBLY_REFUSED);
    CHECK(!p4_hosted_reassembly_start(&state, generation));
    CHECK(p4_hosted_reassembly_start(&state, generation + 1u));
    CHECK(!state.poisoned && state.generation == generation + 1u);
    CHECK(all_zero(state.bytes, sizeof state.bytes));
}

int main(void)
{
    test_literal_vectors_split_at_every_boundary();
    test_lifecycle_and_same_sequence_after_release();
    test_sequence_increments_and_rejections();
    test_arbitrary_chunks_and_fragment_bound();
    test_large_serial_limits_and_fragment_size();
    test_protocol_fields_and_invalid_final_tlv();
    test_preflight_refusals_preserve_all_inputs();
    test_forged_sizes_and_internal_metadata();
    test_abort_and_new_epoch_after_poison();
    printf("hosted epoch reassembly model: %lu checks passed\n", checks);
    return 0;
}
