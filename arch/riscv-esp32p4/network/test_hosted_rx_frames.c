/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
/* Exercise Ethernet routing through the real bounded RX/FIFO/codec path. */
#define main hosted_control_reference_test_main
#include "test_hosted_control.c"
#undef main

#include "hosted_frames.h"
#include "hosted_rx.h"

#define FRAMES_RX_GENERATION UINT32_C(0x7391)
#define FRAMES_RX_REPLACEMENT UINT32_C(0x7392)

struct frames_rx_fixture {
    struct fixture transport;
    struct p4_hosted_rx rx;
    struct p4_hosted_rx_ops ops;
    struct p4_hosted_rx_result result;
};

static void frames_rx_fixture_init(struct frames_rx_fixture *fixture)
{
    memset(fixture, 0, sizeof *fixture);
    fixture_init(&fixture->transport);
    CHECK(p4_hosted_fifo_start(&fixture->transport.state,
                               FRAMES_RX_GENERATION, TEST_POOL_LIMIT));
    fixture->transport.fake.generation = FRAMES_RX_GENERATION;
    fixture->transport.fake.state = &fixture->transport.state;
    CHECK(p4_hosted_rx_start(&fixture->rx, FRAMES_RX_GENERATION));
    fixture->ops.context = &fixture->transport.fake;
    fixture->ops.session_ok = fake_session_ok;
    fixture->ops.budget_ok = fake_budget_ok;
    fixture->ops.read_word = fake_rx_read_word;
    fixture->ops.write_word = fake_write_word;
    fixture->ops.read_fifo = fake_read_fifo;
    memset(&fixture->result, 0xc7, sizeof fixture->result);
}

static int frames_rx_poll(struct frames_rx_fixture *fixture,
                          uint32_t generation, unsigned checksum_enabled,
                          uint8_t *buffer, size_t capacity)
{
    return p4_hosted_rx_poll(&fixture->rx, &fixture->transport.state,
        generation, &fixture->ops, checksum_enabled, buffer, capacity,
        &fixture->result);
}

static size_t append_frame(uint8_t *destination, size_t capacity, size_t *used,
                           unsigned type, unsigned number, unsigned flags,
                           unsigned packet_type, uint16_t sequence,
                           unsigned throttle, unsigned checksum_enabled,
                           const uint8_t *payload, size_t payload_size)
{
    struct p4_hosted_frame frame;
    size_t written = 0u;

    if (*used > capacity)
        return 0u;
    frame.interface_type = (uint8_t)type;
    frame.interface_number = (uint8_t)number;
    frame.flags = (uint8_t)flags;
    frame.throttle = (uint8_t)throttle;
    frame.packet_type = (uint8_t)packet_type;
    frame.sequence = sequence;
    frame.payload = payload;
    frame.payload_size = payload_size;
    frame.wire_size = 0u;
    if (!p4_hosted_encode(destination + *used, capacity - *used,
                          checksum_enabled, &frame, &written))
        return 0u;
    *used += written;
    return written;
}

static size_t append_sta(uint8_t *destination, size_t capacity, size_t *used,
                         const uint8_t *ethernet, size_t size,
                         unsigned number, unsigned flags,
                         uint16_t sequence, unsigned packet_type,
                         unsigned throttle, unsigned checksum_enabled)
{
    return append_frame(destination, capacity, used, P4_HOSTED_STA, number,
        flags, packet_type, sequence, throttle, checksum_enabled,
        ethernet, size);
}

static size_t append_serial_event(uint8_t *destination, size_t capacity,
    size_t *used, uint32_t event_id, const uint8_t *body, size_t body_size,
    unsigned checksum_enabled)
{
    uint8_t frame[TEST_FRAME_CAPACITY];
    size_t frame_size = make_serial_rpc_frame(frame, sizeof frame,
        HOSTED_SERIAL_ENDPOINT_RPCEVT, P4_HOSTED_RPC_TYPE_EVENT,
        event_id, 0u, body, body_size, checksum_enabled,
        P4_HOSTED_SERIAL, 0u, 0u, 0u);

    if (!frame_size || !append_bytes(destination, capacity, used,
                                     frame, frame_size))
        return 0u;
    return frame_size;
}

static void add_rx_batch(struct frames_rx_fixture *fixture,
                         const uint8_t *bytes, size_t size)
{
    CHECK(add_batch(&fixture->transport.fake, bytes, size));
}

static void set_rx_packet(struct frames_rx_fixture *fixture)
{
    static const uint32_t interrupt[] = { P4_HOSTED_OPEN_INT_PACKET };
    set_raws(&fixture->transport.fake, interrupt, 1u);
}

static void check_frame_slot_zero(const struct p4_hosted_owned_frame *item)
{
    const uint8_t *bytes = (const uint8_t *)item;
    size_t index;

    for (index = 0u; index < sizeof(*item); ++index) {
        if (bytes[index] != 0u) {
            CHECK(0);
            return;
        }
    }
    CHECK(1);
}

static void check_frames_empty_and_zero(const struct p4_hosted_frames *frames)
{
    size_t index;

    CHECK(frames->count == 0u && frames->head == 0u);
    for (index = 0u; index < P4_HOSTED_FRAME_QUEUE_CAPACITY; ++index)
        check_frame_slot_zero(&frames->items[index]);
}

static void test_rx_owns_minimum_and_maximum_sta_frames(void)
{
    struct frames_rx_fixture fixture;
    uint8_t minimum[P4_HOSTED_ETHERNET_HEADER_SIZE];
    uint8_t maximum[P4_HOSTED_ETHERNET_FRAME_MAX];
    uint8_t batch[TEST_CAPACITY];
    uint8_t output[P4_HOSTED_ETHERNET_FRAME_MAX];
    size_t used = 0u, output_size = 0u;
    unsigned raw_reads, length_reads, fifo_reads, token_reads, writes;
    size_t index;

    for (index = 0u; index < sizeof minimum; ++index)
        minimum[index] = (uint8_t)(0x21u + index * 3u);
    minimum[12] = 0x08u;
    minimum[13] = 0x00u;
    for (index = 0u; index < sizeof maximum; ++index)
        maximum[index] = (uint8_t)(0x75u + index * 11u);
    maximum[12] = 0x86u;
    maximum[13] = 0xddu;

    frames_rx_fixture_init(&fixture);
    CHECK(append_sta(batch, sizeof batch, &used, minimum, sizeof minimum,
        0u, 0u, 0u, 0u, 0u, 0u) != 0u);
    CHECK(append_sta(batch, sizeof batch, &used, maximum, sizeof maximum,
        0u, 0u, 0u, 0u, 0u, 0u) != 0u);
    add_rx_batch(&fixture, batch, used);
    set_rx_packet(&fixture);

    CHECK(frames_rx_poll(&fixture, FRAMES_RX_GENERATION, 0u,
        fixture.transport.buffer, sizeof fixture.transport.buffer) == 1);
    CHECK(fixture.result.error == P4_HOSTED_RX_OK &&
          fixture.result.batches == 1u && fixture.result.messages == 0u &&
          fixture.result.frames == 2u && fixture.result.partial == 0u &&
          fixture.result.logical_bytes == (uint32_t)used);
    CHECK(fixture.transport.fake.raw_reads == 1u &&
          fixture.transport.fake.length_reads == 1u &&
          fixture.transport.fake.rx_fifo_reads == 1u);
    CHECK(fixture.transport.fake.tx_token_reads == 0u &&
          fixture.transport.fake.tx_fifo_writes == 0u &&
          fixture.transport.fake.yield_calls == 0u);
    CHECK(fixture.transport.state.rx_consumed == (uint32_t)used &&
          fixture.transport.state.tx_sent == 0u);
    CHECK(fixture.rx.frames.count == 2u &&
          fixture.rx.frames.generation == FRAMES_RX_GENERATION &&
          !fixture.rx.frames.poisoned);

    /* Reuse/overwrite both caller and DMA storage before dequeuing. */
    memset(minimum, 0xa1, sizeof minimum);
    memset(maximum, 0xa2, sizeof maximum);
    memset(fixture.transport.buffer, 0xb3,
           sizeof fixture.transport.buffer);
    raw_reads = fixture.transport.fake.raw_reads;
    length_reads = fixture.transport.fake.length_reads;
    fifo_reads = fixture.transport.fake.rx_fifo_reads;
    token_reads = fixture.transport.fake.tx_token_reads;
    writes = fixture.transport.fake.tx_fifo_writes;

    CHECK(p4_hosted_frames_pop(&fixture.rx.frames, FRAMES_RX_GENERATION,
        output, sizeof output, &output_size));
    CHECK(output_size == sizeof minimum &&
          memcmp(output, (const uint8_t[]){
              0x21u, 0x24u, 0x27u, 0x2au, 0x2du, 0x30u, 0x33u,
              0x36u, 0x39u, 0x3cu, 0x3fu, 0x42u, 0x08u, 0x00u
          }, sizeof minimum) == 0);
    CHECK(p4_hosted_frames_pop(&fixture.rx.frames, FRAMES_RX_GENERATION,
        output, sizeof output, &output_size));
    CHECK(output_size == sizeof maximum && output[12] == 0x86u &&
          output[13] == 0xddu && output[0] == 0x75u &&
          output[sizeof maximum - 1u] ==
              (uint8_t)(0x75u + (uint8_t)((sizeof maximum - 1u) * 11u)));
    CHECK(fixture.rx.frames.count == 0u && !fixture.rx.frames.poisoned);
    CHECK(fixture.transport.fake.raw_reads == raw_reads &&
          fixture.transport.fake.length_reads == length_reads &&
          fixture.transport.fake.rx_fifo_reads == fifo_reads &&
          fixture.transport.fake.tx_token_reads == token_reads &&
          fixture.transport.fake.tx_fifo_writes == writes);
}

static void run_bad_sta_frame(unsigned number, unsigned flags,
                              uint16_t sequence, unsigned packet_type,
                              unsigned throttle, size_t payload_size,
                              unsigned mutate_unknown_type)
{
    struct frames_rx_fixture fixture;
    uint8_t ethernet[P4_HOSTED_ETHERNET_FRAME_MAX + 1u];
    uint8_t batch[TEST_FRAME_CAPACITY];
    size_t used = 0u, frame_size;
    size_t index;

    for (index = 0u; index < sizeof ethernet; ++index)
        ethernet[index] = (uint8_t)(index * 7u + 9u);
    frames_rx_fixture_init(&fixture);
    frame_size = append_sta(batch, sizeof batch, &used, ethernet,
        payload_size, number, flags, sequence, packet_type, throttle, 0u);
    CHECK(frame_size != 0u);
    if (mutate_unknown_type)
        batch[0] = (uint8_t)((batch[0] & 0xf0u) | 8u);
    add_rx_batch(&fixture, batch, used);
    set_rx_packet(&fixture);

    CHECK(frames_rx_poll(&fixture, FRAMES_RX_GENERATION, 0u,
        fixture.transport.buffer, sizeof fixture.transport.buffer) == 0);
    CHECK(fixture.result.error == P4_HOSTED_RX_PROTOCOL);
    CHECK(fixture.transport.fake.rx_fifo_reads == 1u &&
          fixture.transport.state.rx_consumed == (uint32_t)used);
    CHECK(fixture.transport.state.poisoned == 1u && fixture.rx.poisoned == 1u &&
          fixture.rx.inbox.poisoned == 1u &&
          fixture.rx.frames.poisoned == 1u &&
          fixture.rx.assembly.poisoned == 1u);
    check_frames_empty_and_zero(&fixture.rx.frames);
    CHECK(fixture.rx.inbox.count == 0u);
}

static void test_rx_sta_contract_and_fail_closed_profiles(void)
{
    uint8_t payload[P4_HOSTED_ETHERNET_FRAME_MAX + 1u];
    struct frames_rx_fixture fixture;
    uint8_t batch[TEST_FRAME_CAPACITY];
    size_t used = 0u;
    size_t index;

    for (index = 0u; index < sizeof payload; ++index)
        payload[index] = (uint8_t)(index ^ 0x5au);

    /* The only accepted route is the pinned peer's complete STA/0 envelope:
     * flags, sequence, packet type, and throttle/reserved field are zero. */
    run_bad_sta_frame(1u, 0u, 0u, 0u, 0u,
        P4_HOSTED_ETHERNET_HEADER_SIZE, 0u);
    run_bad_sta_frame(0u, P4_HOSTED_MORE_FRAGMENT, 0u, 0u, 0u,
        P4_HOSTED_ETHERNET_HEADER_SIZE, 0u);
    run_bad_sta_frame(0u, 0u, 1u, 0u, 0u,
        P4_HOSTED_ETHERNET_HEADER_SIZE, 0u);
    run_bad_sta_frame(0u, 0u, 0u, 1u, 0u,
        P4_HOSTED_ETHERNET_HEADER_SIZE, 0u);
    run_bad_sta_frame(0u, 0u, 0u, 0u, 1u,
        P4_HOSTED_ETHERNET_HEADER_SIZE, 0u);
    run_bad_sta_frame(0u, 0u, 0u, 0u, 0u,
        P4_HOSTED_ETHERNET_HEADER_SIZE - 1u, 0u);
    run_bad_sta_frame(0u, 0u, 0u, 0u, 0u,
        P4_HOSTED_ETHERNET_FRAME_MAX + 1u, 0u);
    run_bad_sta_frame(0u, 0u, 0u, 0u, 0u,
        P4_HOSTED_ETHERNET_HEADER_SIZE, 1u);

    frames_rx_fixture_init(&fixture);
    CHECK(append_frame(batch, sizeof batch, &used, P4_HOSTED_AP, 0u,
        0u, 0u, 0u, 0u, 0u, payload,
        P4_HOSTED_ETHERNET_HEADER_SIZE) != 0u);
    add_rx_batch(&fixture, batch, used);
    set_rx_packet(&fixture);
    CHECK(frames_rx_poll(&fixture, FRAMES_RX_GENERATION, 0u,
        fixture.transport.buffer, sizeof fixture.transport.buffer) == 0);
    CHECK(fixture.result.error == P4_HOSTED_RX_PROTOCOL);
    CHECK(fixture.rx.poisoned && fixture.rx.frames.poisoned &&
          fixture.transport.state.poisoned);
    check_frames_empty_and_zero(&fixture.rx.frames);

    frames_rx_fixture_init(&fixture);
    used = 0u;
    CHECK(append_frame(batch, sizeof batch, &used, P4_HOSTED_ETH, 0u,
        0u, 0u, 0u, 0u, 0u, payload,
        P4_HOSTED_ETHERNET_HEADER_SIZE) != 0u);
    add_rx_batch(&fixture, batch, used);
    set_rx_packet(&fixture);
    CHECK(frames_rx_poll(&fixture, FRAMES_RX_GENERATION, 0u,
        fixture.transport.buffer, sizeof fixture.transport.buffer) == 0);
    CHECK(fixture.result.error == P4_HOSTED_RX_PROTOCOL);
    CHECK(fixture.rx.poisoned && fixture.rx.frames.poisoned &&
          fixture.transport.state.poisoned);
    check_frames_empty_and_zero(&fixture.rx.frames);
}

static void test_rx_sta_payload_containing_rpc_bytes_stays_opaque(void)
{
    struct frames_rx_fixture fixture;
    uint8_t ethernet[P4_HOSTED_ETHERNET_FRAME_MAX];
    uint8_t rpc_body[] = { 0x18u, 0x00u, 0x7fu, 0x81u };
    uint8_t serial_message[128];
    uint8_t batch[TEST_FRAME_CAPACITY];
    uint8_t output[P4_HOSTED_ETHERNET_FRAME_MAX];
    size_t serial_size, ethernet_size, used = 0u, output_size = 0u;

    serial_size = make_serial_rpc_message(serial_message,
        sizeof serial_message, HOSTED_SERIAL_ENDPOINT_RPCEVT,
        P4_HOSTED_RPC_TYPE_EVENT, 775u, 0u, rpc_body, sizeof rpc_body);
    CHECK(serial_size != 0u && serial_size + 14u <= sizeof ethernet);
    memset(ethernet, 0, 14u);
    ethernet[12] = 0x08u;
    ethernet[13] = 0x00u;
    memcpy(ethernet + 14u, serial_message, serial_size);
    ethernet_size = 14u + serial_size;

    frames_rx_fixture_init(&fixture);
    CHECK(append_sta(batch, sizeof batch, &used, ethernet, ethernet_size,
        0u, 0u, 0u, 0u, 0u, 0u) != 0u);
    add_rx_batch(&fixture, batch, used);
    set_rx_packet(&fixture);
    CHECK(frames_rx_poll(&fixture, FRAMES_RX_GENERATION, 0u,
        fixture.transport.buffer, sizeof fixture.transport.buffer) == 1);
    CHECK(fixture.result.error == P4_HOSTED_RX_OK &&
          fixture.result.frames == 1u && fixture.result.messages == 0u &&
          fixture.rx.frames.count == 1u && fixture.rx.inbox.count == 0u);
    memset(fixture.transport.buffer, 0xcc, sizeof fixture.transport.buffer);
    memset(ethernet, 0xdd, ethernet_size);
    CHECK(p4_hosted_frames_pop(&fixture.rx.frames, FRAMES_RX_GENERATION,
        output, sizeof output, &output_size));
    CHECK(output_size == ethernet_size &&
          memcmp(output + 14u, serial_message, serial_size) == 0);
}

static void test_rx_mixed_serial_and_ethernet_batch_order(void)
{
    struct frames_rx_fixture fixture;
    struct p4_hosted_inbox_item event, response;
    uint8_t batch[TEST_CAPACITY];
    uint8_t ethernet_a[P4_HOSTED_ETHERNET_HEADER_SIZE];
    uint8_t ethernet_b[P4_HOSTED_ETHERNET_HEADER_SIZE];
    uint8_t body_a[] = { 0x11u, 0x22u, 0x33u };
    uint8_t body_b[] = { 0x44u, 0x55u };
    size_t used = 0u, output_size = 0u;
    uint8_t output[P4_HOSTED_ETHERNET_FRAME_MAX];

    memset(ethernet_a, 0x31, sizeof ethernet_a);
    memset(ethernet_b, 0x72, sizeof ethernet_b);
    ethernet_a[12] = 0x08u; ethernet_a[13] = 0x06u;
    ethernet_b[12] = 0x08u; ethernet_b[13] = 0x00u;
    frames_rx_fixture_init(&fixture);
    CHECK(append_serial_event(batch, sizeof batch, &used, 775u,
        body_a, sizeof body_a, 0u) != 0u);
    CHECK(append_sta(batch, sizeof batch, &used, ethernet_a,
        sizeof ethernet_a, 0u, 0u, 0u, 0u, 0u, 0u) != 0u);
    {
        size_t response_size = make_serial_rpc_frame(batch + used,
            sizeof batch - used,
        HOSTED_SERIAL_ENDPOINT_RPCRSP, P4_HOSTED_RPC_TYPE_RESPONSE,
        513u, 9u, body_b, sizeof body_b, 0u,
            P4_HOSTED_SERIAL, 0u, 0u, 0u);
        CHECK(response_size != 0u);
        used += response_size;
    }
    CHECK(append_sta(batch, sizeof batch, &used, ethernet_b,
        sizeof ethernet_b, 0u, 0u, 0u, 0u, 0u, 0u) != 0u);
    add_rx_batch(&fixture, batch, used);
    set_rx_packet(&fixture);

    CHECK(frames_rx_poll(&fixture, FRAMES_RX_GENERATION, 0u,
        fixture.transport.buffer, sizeof fixture.transport.buffer) == 1);
    CHECK(fixture.result.error == P4_HOSTED_RX_OK &&
          fixture.result.messages == 2u && fixture.result.frames == 2u &&
          fixture.rx.inbox.count == 2u && fixture.rx.frames.count == 2u);
    CHECK(p4_hosted_inbox_pop(&fixture.rx.inbox, FRAMES_RX_GENERATION,
                              &event));
    CHECK(event.msg_id == 775u && event.uid == 0u &&
          event.payload_size == sizeof body_a &&
          memcmp(event.payload, body_a, sizeof body_a) == 0);
    CHECK(p4_hosted_inbox_pop(&fixture.rx.inbox, FRAMES_RX_GENERATION,
                              &response));
    CHECK(response.msg_id == 513u && response.uid == 9u &&
          response.msg_type == P4_HOSTED_RPC_TYPE_RESPONSE &&
          response.payload_size == sizeof body_b &&
          memcmp(response.payload, body_b, sizeof body_b) == 0);
    CHECK(p4_hosted_frames_pop(&fixture.rx.frames, FRAMES_RX_GENERATION,
        output, sizeof output, &output_size));
    CHECK(output_size == sizeof ethernet_a &&
          memcmp(output, ethernet_a, sizeof ethernet_a) == 0);
    CHECK(p4_hosted_frames_pop(&fixture.rx.frames, FRAMES_RX_GENERATION,
        output, sizeof output, &output_size));
    CHECK(output_size == sizeof ethernet_b &&
          memcmp(output, ethernet_b, sizeof ethernet_b) == 0);
    CHECK(fixture.transport.fake.tx_token_reads == 0u &&
          fixture.transport.fake.tx_fifo_writes == 0u &&
          fixture.transport.fake.yield_calls == 0u);
}

static void test_rx_serial_partial_survives_interleaved_sta_frame(void)
{
    struct frames_rx_fixture fixture;
    struct p4_hosted_inbox_item item;
    uint8_t message[TEST_LARGE_MESSAGE_CAPACITY];
    uint8_t first[TEST_FRAME_CAPACITY];
    uint8_t last[TEST_FRAME_CAPACITY];
    uint8_t batch[TEST_CAPACITY];
    uint8_t ethernet[P4_HOSTED_ETHERNET_HEADER_SIZE];
    uint8_t ethernet_out[P4_HOSTED_ETHERNET_FRAME_MAX];
    uint8_t body[] = { 0x01u, 0x00u, 0x7fu, 0xffu, 0x44u, 0x55u };
    size_t message_size, first_size, last_size, used = 0u, output_size = 0u;

    memset(ethernet, 0x96, sizeof ethernet);
    ethernet[12] = 0x86u; ethernet[13] = 0xddu;
    frames_rx_fixture_init(&fixture);
    message_size = make_serial_rpc_message(message, sizeof message,
        HOSTED_SERIAL_ENDPOINT_RPCEVT, P4_HOSTED_RPC_TYPE_EVENT,
        776u, 0u, body, sizeof body);
    CHECK(message_size > 7u);
    first_size = make_fragment_frame(first, sizeof first, message, 7u,
        UINT16_MAX, 1u, 0u);
    last_size = make_fragment_frame(last, sizeof last, message + 7u,
        message_size - 7u, 0u, 0u, 0u);
    CHECK(first_size && last_size);
    CHECK(append_bytes(batch, sizeof batch, &used, first, first_size));
    CHECK(append_sta(batch, sizeof batch, &used, ethernet,
        sizeof ethernet, 0u, 0u, 0u, 0u, 0u, 0u) != 0u);
    CHECK(append_bytes(batch, sizeof batch, &used, last, last_size));
    add_rx_batch(&fixture, batch, used);
    set_rx_packet(&fixture);

    CHECK(frames_rx_poll(&fixture, FRAMES_RX_GENERATION, 0u,
        fixture.transport.buffer, sizeof fixture.transport.buffer) == 1);
    CHECK(fixture.result.error == P4_HOSTED_RX_OK &&
          fixture.result.messages == 1u && fixture.result.frames == 1u &&
          fixture.result.partial == 0u && fixture.rx.assembly.fragments == 0u);
    CHECK(p4_hosted_inbox_pop(&fixture.rx.inbox, FRAMES_RX_GENERATION,
                              &item));
    CHECK(item.msg_id == 776u && item.payload_size == sizeof body &&
          memcmp(item.payload, body, sizeof body) == 0);
    CHECK(p4_hosted_frames_pop(&fixture.rx.frames, FRAMES_RX_GENERATION,
        ethernet_out, sizeof ethernet_out, &output_size));
    CHECK(output_size == sizeof ethernet &&
          memcmp(ethernet_out, ethernet, sizeof ethernet) == 0);
}

static void test_frame_queue_overflow_is_terminal(void)
{
    struct frames_rx_fixture fixture;
    uint8_t ethernet[P4_HOSTED_ETHERNET_HEADER_SIZE];
    uint8_t batch[TEST_CAPACITY];
    size_t used = 0u;
    unsigned index;

    frames_rx_fixture_init(&fixture);
    for (index = 0u; index <= P4_HOSTED_FRAME_QUEUE_CAPACITY; ++index) {
        memset(ethernet, (int)(0x30u + index), sizeof ethernet);
        ethernet[12] = 0x08u; ethernet[13] = 0x00u;
        CHECK(append_sta(batch, sizeof batch, &used, ethernet,
            sizeof ethernet, 0u, 0u, 0u, 0u, 0u, 0u) != 0u);
    }
    add_rx_batch(&fixture, batch, used);
    set_rx_packet(&fixture);

    CHECK(frames_rx_poll(&fixture, FRAMES_RX_GENERATION, 0u,
        fixture.transport.buffer, sizeof fixture.transport.buffer) == 0);
    CHECK(fixture.result.error == P4_HOSTED_RX_FRAME_QUEUE_FULL);
    CHECK(fixture.result.batches == 1u &&
          fixture.result.frames == P4_HOSTED_FRAME_QUEUE_CAPACITY);
    CHECK(fixture.transport.state.rx_consumed == (uint32_t)used);
    CHECK(fixture.transport.state.poisoned && fixture.rx.poisoned &&
          fixture.rx.frames.poisoned && fixture.rx.inbox.poisoned &&
          fixture.rx.assembly.poisoned);
    check_frames_empty_and_zero(&fixture.rx.frames);
    CHECK(fixture.rx.inbox.count == 0u);
    CHECK(fixture.transport.fake.rx_fifo_reads == 1u &&
          fixture.transport.fake.tx_fifo_writes == 0u);
}

static void test_malformed_second_frame_wipes_all_owned_queues(void)
{
    struct frames_rx_fixture fixture;
    struct p4_hosted_inbox_item item;
    struct p4_hosted_rpc seeded_rpc;
    uint8_t old_payload[] = { 0x42u, 0x43u };
    uint8_t ethernet[P4_HOSTED_ETHERNET_HEADER_SIZE];
    uint8_t batch[TEST_CAPACITY];
    size_t used = 0u;

    memset(ethernet, 0x8au, sizeof ethernet);
    ethernet[12] = 0x08u; ethernet[13] = 0x00u;
    frames_rx_fixture_init(&fixture);
    memset(&seeded_rpc, 0, sizeof seeded_rpc);
    seeded_rpc.msg_type = P4_HOSTED_RPC_TYPE_EVENT;
    seeded_rpc.msg_id = 775u;
    seeded_rpc.uid = 0u;
    seeded_rpc.payload = old_payload;
    seeded_rpc.payload_size = sizeof old_payload;
    CHECK(p4_hosted_inbox_push(&fixture.rx.inbox, FRAMES_RX_GENERATION,
                               &seeded_rpc));

    CHECK(append_sta(batch, sizeof batch, &used, ethernet,
        sizeof ethernet, 0u, 0u, 0u, 0u, 0u, 0u) != 0u);
    CHECK(append_sta(batch, sizeof batch, &used, ethernet,
        sizeof ethernet, 1u, 0u, 0u, 0u, 0u, 0u) != 0u);
    add_rx_batch(&fixture, batch, used);
    set_rx_packet(&fixture);

    CHECK(frames_rx_poll(&fixture, FRAMES_RX_GENERATION, 0u,
        fixture.transport.buffer, sizeof fixture.transport.buffer) == 0);
    CHECK(fixture.result.error == P4_HOSTED_RX_PROTOCOL &&
          fixture.result.frames == 1u);
    CHECK(fixture.transport.state.rx_consumed == (uint32_t)used);
    CHECK(fixture.transport.state.poisoned && fixture.rx.poisoned &&
          fixture.rx.frames.poisoned && fixture.rx.inbox.poisoned &&
          fixture.rx.assembly.poisoned);
    check_frames_empty_and_zero(&fixture.rx.frames);
    CHECK(fixture.rx.inbox.count == 0u);
    for (size_t index = 0u; index < P4_HOSTED_INBOX_CAPACITY; ++index) {
        const uint8_t *bytes = (const uint8_t *)&fixture.rx.inbox.items[index];
        size_t byte;
        for (byte = 0u; byte < sizeof fixture.rx.inbox.items[index]; ++byte)
            if (bytes[byte] != 0u) CHECK(0);
        CHECK(1);
    }
    memset(&item, 0x56, sizeof item);
    CHECK(p4_hosted_inbox_pop(&fixture.rx.inbox, FRAMES_RX_GENERATION,
                              &item) == 0);
    CHECK(fixture.transport.fake.tx_token_reads == 0u &&
          fixture.transport.fake.tx_fifo_writes == 0u);
}

static void test_rx_frame_queue_corruption_and_stale_preflight(void)
{
    struct frames_rx_fixture fixture;
    struct p4_hosted_rx rx_before;
    struct p4_hosted_fifo fifo_before;
    struct p4_hosted_rx_result result_before;
    uint8_t buffer_before[TEST_CAPACITY];

    frames_rx_fixture_init(&fixture);
    fixture.rx.frames.head = P4_HOSTED_FRAME_QUEUE_CAPACITY;
    memcpy(&rx_before, &fixture.rx, sizeof rx_before);
    memcpy(&fifo_before, &fixture.transport.state, sizeof fifo_before);
    memcpy(&result_before, &fixture.result, sizeof result_before);
    memcpy(buffer_before, fixture.transport.buffer, sizeof buffer_before);
    CHECK(!frames_rx_poll(&fixture, FRAMES_RX_GENERATION, 0u,
        fixture.transport.buffer, sizeof fixture.transport.buffer));
    CHECK(memcmp(&rx_before, &fixture.rx, sizeof rx_before) == 0);
    CHECK(memcmp(&fifo_before, &fixture.transport.state,
                 sizeof fifo_before) == 0);
    CHECK(memcmp(&result_before, &fixture.result, sizeof result_before) == 0);
    CHECK(memcmp(buffer_before, fixture.transport.buffer,
                 sizeof buffer_before) == 0);
    CHECK(fixture.transport.fake.raw_reads == 0u &&
          fixture.transport.fake.length_reads == 0u &&
          fixture.transport.fake.rx_fifo_reads == 0u &&
          fixture.transport.fake.write_word_calls == 0u);

    fixture.rx.frames.head = 0u;
    CHECK(!frames_rx_poll(&fixture, FRAMES_RX_REPLACEMENT, 0u,
        fixture.transport.buffer, sizeof fixture.transport.buffer));
    CHECK(fixture.transport.fake.raw_reads == 0u &&
          fixture.transport.fake.rx_fifo_reads == 0u);

    CHECK(!p4_hosted_rx_poll(&fixture.rx, &fixture.transport.state,
        FRAMES_RX_GENERATION, &fixture.ops, 0u,
        (uint8_t *)(void *)&fixture.rx.frames, sizeof fixture.transport.buffer,
        &fixture.result));
    CHECK(fixture.transport.fake.raw_reads == 0u &&
          fixture.transport.fake.rx_fifo_reads == 0u);
}

static void test_rx_start_rejects_corrupt_frames_child_transactionally(void)
{
    struct p4_hosted_rx state, before;

    memset(&state, 0, sizeof state);
    state.frames.head = P4_HOSTED_FRAME_QUEUE_CAPACITY;
    memcpy(&before, &state, sizeof before);
    CHECK(!p4_hosted_rx_start(&state, FRAMES_RX_GENERATION));
    CHECK(memcmp(&before, &state, sizeof state) == 0);

    memset(&state, 0, sizeof state);
    state.frames.count = P4_HOSTED_FRAME_QUEUE_CAPACITY + 1u;
    memcpy(&before, &state, sizeof before);
    CHECK(!p4_hosted_rx_start(&state, FRAMES_RX_GENERATION));
    CHECK(memcmp(&before, &state, sizeof state) == 0);
}

int main(void)
{
    test_rx_owns_minimum_and_maximum_sta_frames();
    test_rx_sta_contract_and_fail_closed_profiles();
    test_rx_sta_payload_containing_rpc_bytes_stays_opaque();
    test_rx_mixed_serial_and_ethernet_batch_order();
    test_rx_serial_partial_survives_interleaved_sta_frame();
    test_frame_queue_overflow_is_terminal();
    test_malformed_second_frame_wipes_all_owned_queues();
    test_rx_frame_queue_corruption_and_stale_preflight();
    test_rx_start_rejects_corrupt_frames_child_transactionally();
    printf("hosted RX Ethernet routing: %lu checks passed\n", checks);
    return 0;
}
