/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
/* Exercise the actual RX, FIFO, serial, RPC, reassembly and inbox sources with
 * the existing bounded fake-register/DMA fixture. No hardware or elapsed-time
 * behavior is measured. */
#define main hosted_control_legacy_test_main
#include "test_hosted_control.c"
#undef main

#include "hosted_inbox.h"
#include "hosted_rx.h"

#define RX_TEST_GENERATION UINT32_C(0x6681)
#define RX_REPLACEMENT_GENERATION UINT32_C(0x6682)

struct rx_callback_context {
    struct fake_host *fake;
    struct p4_hosted_rx *rx;
    struct p4_hosted_rx replacement;
    unsigned replace_rx_on_fifo;
    unsigned replaced_rx;
    struct p4_hosted_rx rx_after;
    struct p4_hosted_fifo fifo_after;
    unsigned captured_after;
};

struct rx_fixture {
    struct fixture legacy;
    struct p4_hosted_rx rx;
    struct p4_hosted_rx_ops ops;
    struct rx_callback_context callback;
    struct p4_hosted_rx_result result;
};

static void rx_fixture_init(struct rx_fixture *fixture)
{
    memset(fixture, 0, sizeof *fixture);
    fixture_init(&fixture->legacy);
    CHECK(p4_hosted_fifo_start(&fixture->legacy.state, RX_TEST_GENERATION,
                               TEST_POOL_LIMIT));
    fixture->legacy.fake.generation = RX_TEST_GENERATION;
    fixture->callback.fake = &fixture->legacy.fake;
    fixture->callback.rx = &fixture->rx;
    CHECK(p4_hosted_rx_start(&fixture->rx, RX_TEST_GENERATION));
    fixture->ops.context = &fixture->callback;
    fixture->ops.session_ok = NULL;
    fixture->ops.budget_ok = NULL;
    fixture->ops.read_word = NULL;
    fixture->ops.write_word = NULL;
    fixture->ops.read_fifo = NULL;
    memset(&fixture->result, 0xc7, sizeof fixture->result);
}

static int rx_fake_session_ok(void *context, uint32_t generation)
{
    struct rx_callback_context *callback = context;
    return fake_session_ok(callback->fake, generation);
}

static int rx_fake_budget_ok(void *context)
{
    struct rx_callback_context *callback = context;
    return fake_budget_ok(callback->fake);
}

static int rx_fake_read_word(void *context, uint32_t address, uint32_t *value)
{
    struct rx_callback_context *callback = context;
    return fake_rx_read_word(callback->fake, address, value);
}

static int rx_fake_write_word(void *context, uint32_t address, uint32_t value)
{
    struct rx_callback_context *callback = context;
    return fake_write_word(callback->fake, address, value);
}

static int rx_fake_read_fifo(void *context,
                             const struct p4_hosted_fifo_plan *plan,
                             uint8_t *buffer)
{
    struct rx_callback_context *callback = context;
    int result = fake_read_fifo(callback->fake, plan, buffer);

    if (callback->replace_rx_on_fifo && !callback->replaced_rx) {
        memcpy(callback->rx, &callback->replacement,
               sizeof callback->replacement);
        callback->replaced_rx = 1u;
    }
    if (!callback->captured_after) {
        memcpy(&callback->rx_after, callback->rx, sizeof callback->rx_after);
        memcpy(&callback->fifo_after, callback->fake->state,
               sizeof callback->fifo_after);
        callback->captured_after = 1u;
    }
    return result;
}

static void rx_fixture_install_callbacks(struct rx_fixture *fixture)
{
    fixture->ops.context = &fixture->callback;
    fixture->ops.session_ok = rx_fake_session_ok;
    fixture->ops.budget_ok = rx_fake_budget_ok;
    fixture->ops.read_word = rx_fake_read_word;
    fixture->ops.write_word = rx_fake_write_word;
    fixture->ops.read_fifo = rx_fake_read_fifo;
}

static int rx_poll(struct rx_fixture *fixture, uint32_t generation,
                   unsigned checksum_enabled, uint8_t *buffer,
                   size_t capacity)
{
    return p4_hosted_rx_poll(&fixture->rx, &fixture->legacy.state,
        generation, &fixture->ops, checksum_enabled, buffer, capacity,
        &fixture->result);
}

static size_t append_rx_rpc(uint8_t *destination, size_t capacity, size_t *used,
                            enum hosted_serial_endpoint endpoint,
                            uint32_t type, uint32_t id, uint32_t uid,
                            const uint8_t *body, size_t body_size,
                            unsigned checksum_enabled)
{
    uint8_t frame[TEST_FRAME_CAPACITY];
    size_t frame_size = make_serial_rpc_frame(frame, sizeof frame, endpoint,
        type, id, uid, body, body_size, checksum_enabled,
        P4_HOSTED_SERIAL, 0u, 0u, 0u);
    if (frame_size == 0u || !append_bytes(destination, capacity, used,
                                           frame, frame_size))
        return 0u;
    return frame_size;
}

static void rx_add_batch(struct rx_fixture *fixture, const uint8_t *bytes,
                         size_t size)
{
    CHECK(add_batch(&fixture->legacy.fake, bytes, size));
}

static void rx_set_interrupts(struct rx_fixture *fixture,
                              const uint32_t *interrupts, unsigned count)
{
    set_raws(&fixture->legacy.fake, interrupts, count);
}

static void expect_rx_preflight_unchanged(struct rx_fixture *fixture,
    uint32_t generation, unsigned checksum_enabled, uint8_t *buffer,
    size_t capacity)
{
    struct p4_hosted_rx rx_before;
    struct p4_hosted_fifo fifo_before;
    struct p4_hosted_rx_result result_before;
    uint8_t buffer_before[TEST_CAPACITY];

    memcpy(&rx_before, &fixture->rx, sizeof rx_before);
    memcpy(&fifo_before, &fixture->legacy.state, sizeof fifo_before);
    memcpy(&result_before, &fixture->result, sizeof result_before);
    memcpy(buffer_before, fixture->legacy.buffer, sizeof buffer_before);
    CHECK(rx_poll(fixture, generation, checksum_enabled, buffer, capacity) == 0);
    CHECK(memcmp(&rx_before, &fixture->rx, sizeof rx_before) == 0);
    CHECK(memcmp(&fifo_before, &fixture->legacy.state, sizeof fifo_before) == 0);
    CHECK(memcmp(&result_before, &fixture->result,
                 sizeof result_before) == 0);
    CHECK(memcmp(buffer_before, fixture->legacy.buffer,
                 sizeof buffer_before) == 0);
    CHECK(fixture->legacy.fake.raw_reads == 0u);
    CHECK(fixture->legacy.fake.length_reads == 0u);
    CHECK(fixture->legacy.fake.rx_fifo_reads == 0u);
    CHECK(fixture->legacy.fake.write_word_calls == 0u);
}

static void test_rx_idle_poll_is_one_nonblocking_batch(void)
{
    struct rx_fixture fixture;
    struct p4_hosted_inbox inbox_before;
    static const uint32_t interrupt[] = { 0u };
    struct p4_hosted_inbox_item item, item_before;
    uint8_t buffer_before[TEST_CAPACITY];

    rx_fixture_init(&fixture);
    rx_fixture_install_callbacks(&fixture);
    rx_set_interrupts(&fixture, interrupt, 1u);
    memcpy(&inbox_before, &fixture.rx.inbox, sizeof inbox_before);
    memcpy(buffer_before, fixture.legacy.buffer, sizeof buffer_before);
    memset(&item, 0x5a, sizeof item);
    memcpy(&item_before, &item, sizeof item_before);

    CHECK(rx_poll(&fixture, RX_TEST_GENERATION, 0u,
        fixture.legacy.buffer, sizeof fixture.legacy.buffer) == 1);
    CHECK(fixture.result.error == P4_HOSTED_RX_OK);
    CHECK(fixture.result.batches == 0u && fixture.result.messages == 0u);
    CHECK(fixture.result.partial == 0u && fixture.result.logical_bytes == 0u);
    CHECK(fixture.legacy.fake.raw_reads == 1u);
    CHECK(fixture.legacy.fake.length_reads == 0u);
    CHECK(fixture.legacy.fake.rx_fifo_reads == 0u);
    CHECK(fixture.legacy.fake.tx_token_reads == 0u);
    CHECK(fixture.legacy.fake.tx_fifo_writes == 0u);
    CHECK(fixture.legacy.fake.yield_calls == 0u);
    CHECK(fixture.legacy.fake.write_word_calls == 0u);
    CHECK(memcmp(&inbox_before, &fixture.rx.inbox, sizeof inbox_before) == 0);
    CHECK(memcmp(buffer_before, fixture.legacy.buffer,
                 sizeof buffer_before) == 0);
    CHECK(p4_hosted_inbox_pop(&fixture.rx.inbox, RX_TEST_GENERATION,
                              &item) == 0);
    CHECK(memcmp(&item_before, &item, sizeof item_before) == 0);
    CHECK(fixture.legacy.state.rx_consumed == 0u);
    CHECK(fixture.legacy.state.tx_sent == 0u);
}

static void test_rx_owned_event_and_response_items(void)
{
    struct rx_fixture fixture;
    struct p4_hosted_inbox_item event, response;
    static const uint8_t event_body[] = { 0x00u, 0x7fu, 0xffu, 0x41u };
    static const uint8_t response_body[] = { 0x91u, 0x00u, 0x22u };
    static const uint32_t interrupt[] = { P4_HOSTED_OPEN_INT_PACKET };
    uint8_t batch[TEST_CAPACITY];
    size_t used = 0u;

    rx_fixture_init(&fixture);
    rx_fixture_install_callbacks(&fixture);
    CHECK(append_rx_rpc(batch, sizeof batch, &used,
        HOSTED_SERIAL_ENDPOINT_RPCEVT, P4_HOSTED_RPC_TYPE_EVENT, 775u, 0u,
        event_body, sizeof event_body, 0u) != 0u);
    CHECK(append_rx_rpc(batch, sizeof batch, &used,
        HOSTED_SERIAL_ENDPOINT_RPCRSP, P4_HOSTED_RPC_TYPE_RESPONSE, 513u,
        UINT32_C(0x10203040), response_body, sizeof response_body, 0u) != 0u);
    rx_add_batch(&fixture, batch, used);
    rx_set_interrupts(&fixture, interrupt, 1u);

    CHECK(rx_poll(&fixture, RX_TEST_GENERATION, 0u,
        fixture.legacy.buffer, sizeof fixture.legacy.buffer) == 1);
    CHECK(fixture.result.error == P4_HOSTED_RX_OK);
    CHECK(fixture.result.batches == 1u && fixture.result.messages == 2u);
    CHECK(fixture.result.partial == 0u);
    CHECK(fixture.result.logical_bytes == (uint32_t)used);
    CHECK(fixture.legacy.fake.raw_reads == 1u);
    CHECK(fixture.legacy.fake.length_reads == 1u);
    CHECK(fixture.legacy.fake.rx_fifo_reads == 1u);
    CHECK(fixture.legacy.fake.tx_token_reads == 0u);
    CHECK(fixture.legacy.fake.tx_fifo_writes == 0u);
    CHECK(fixture.legacy.fake.yield_calls == 0u);

    /* Prove inbox payload ownership is independent of the reusable DMA area. */
    memset(fixture.legacy.buffer, 0xa6, sizeof fixture.legacy.buffer);
    CHECK(p4_hosted_inbox_pop(&fixture.rx.inbox, RX_TEST_GENERATION,
                              &event) == 1);
    CHECK(event.msg_type == P4_HOSTED_RPC_TYPE_EVENT);
    CHECK(event.msg_id == 775u && event.uid == 0u);
    CHECK(event.payload_size == sizeof event_body);
    CHECK(memcmp(event.payload, event_body, sizeof event_body) == 0);
    CHECK(p4_hosted_inbox_pop(&fixture.rx.inbox, RX_TEST_GENERATION,
                              &response) == 1);
    CHECK(response.msg_type == P4_HOSTED_RPC_TYPE_RESPONSE);
    CHECK(response.msg_id == 513u && response.uid == UINT32_C(0x10203040));
    CHECK(response.payload_size == sizeof response_body);
    CHECK(memcmp(response.payload, response_body, sizeof response_body) == 0);
    CHECK(fixture.rx.inbox.count == 0u && fixture.rx.inbox.poisoned == 0u);
}

static void test_rx_fragment_survives_idle_poll(void)
{
    struct rx_fixture fixture;
    struct p4_hosted_inbox inbox_after_first;
    struct p4_hosted_inbox_item item;
    static const uint8_t body[] = {
        0x01u, 0x00u, 0x7fu, 0xffu, 0x31u, 0x00u, 0xa5u, 0x5au,
        0x62u, 0x73u, 0x84u, 0x95u, 0xa6u, 0xb7u, 0xc8u, 0xd9u
    };
    static const uint32_t interrupts[] = {
        P4_HOSTED_OPEN_INT_PACKET, 0u, P4_HOSTED_OPEN_INT_PACKET
    };
    uint8_t message[TEST_LARGE_MESSAGE_CAPACITY];
    uint8_t stream[TEST_CAPACITY];
    size_t message_size, stream_size, first_size;

    rx_fixture_init(&fixture);
    rx_fixture_install_callbacks(&fixture);
    message_size = make_serial_rpc_message(message, sizeof message,
        HOSTED_SERIAL_ENDPOINT_RPCEVT, P4_HOSTED_RPC_TYPE_EVENT, 775u, 0u,
        body, sizeof body);
    CHECK(message_size > 16u);
    stream_size = make_fragment_stream(stream, sizeof stream, message,
        message_size, 8u, UINT16_C(65534), 0u);
    first_size = 2u * (P4_HOSTED_HEADER_SIZE + 8u);
    CHECK(stream_size > first_size);
    rx_add_batch(&fixture, stream, first_size);
    rx_set_interrupts(&fixture, interrupts, 3u);

    CHECK(rx_poll(&fixture, RX_TEST_GENERATION, 0u,
        fixture.legacy.buffer, sizeof fixture.legacy.buffer) == 1);
    CHECK(fixture.result.batches == 1u && fixture.result.messages == 0u);
    CHECK(fixture.result.partial == 1u);
    CHECK(fixture.rx.assembly.fragments == 2u);
    CHECK(fixture.rx.inbox.count == 0u);
    memcpy(&inbox_after_first, &fixture.rx.inbox, sizeof inbox_after_first);

    CHECK(rx_poll(&fixture, RX_TEST_GENERATION, 0u,
        fixture.legacy.buffer, sizeof fixture.legacy.buffer) == 1);
    CHECK(fixture.result.error == P4_HOSTED_RX_OK);
    CHECK(fixture.result.batches == 0u && fixture.result.messages == 0u);
    CHECK(fixture.result.partial == 1u);
    CHECK(fixture.rx.assembly.fragments == 2u);
    CHECK(memcmp(&inbox_after_first, &fixture.rx.inbox,
                 sizeof inbox_after_first) == 0);
    CHECK(fixture.legacy.fake.rx_fifo_reads == 1u);
    CHECK(fixture.legacy.fake.yield_calls == 0u);
    CHECK(fixture.legacy.fake.tx_fifo_writes == 0u);

    rx_add_batch(&fixture, stream + first_size, stream_size - first_size);
    CHECK(rx_poll(&fixture, RX_TEST_GENERATION, 0u,
        fixture.legacy.buffer, sizeof fixture.legacy.buffer) == 1);
    CHECK(fixture.result.error == P4_HOSTED_RX_OK);
    CHECK(fixture.result.batches == 1u && fixture.result.messages == 1u);
    CHECK(fixture.result.partial == 0u);
    CHECK(fixture.rx.assembly.fragments == 0u);
    CHECK(fixture.rx.inbox.count == 1u);
    CHECK(p4_hosted_inbox_pop(&fixture.rx.inbox, RX_TEST_GENERATION,
                              &item) == 1);
    CHECK(item.msg_type == P4_HOSTED_RPC_TYPE_EVENT);
    CHECK(item.msg_id == 775u && item.uid == 0u);
    CHECK(item.payload_size == sizeof body);
    CHECK(memcmp(item.payload, body, sizeof body) == 0);
    CHECK(fixture.legacy.fake.rx_fifo_reads == 2u);
    CHECK(fixture.legacy.fake.tx_fifo_writes == 0u);
}

static void test_rx_queue_overflow_poison(void)
{
    struct rx_fixture fixture;
    static const uint8_t body[] = { 0x01u, 0x02u };
    static const uint32_t interrupt[] = { P4_HOSTED_OPEN_INT_PACKET };
    uint8_t batch[TEST_CAPACITY];
    size_t used = 0u;
    unsigned i;

    rx_fixture_init(&fixture);
    rx_fixture_install_callbacks(&fixture);
    for (i = 0; i <= P4_HOSTED_INBOX_CAPACITY; ++i)
        CHECK(append_rx_rpc(batch, sizeof batch, &used,
            HOSTED_SERIAL_ENDPOINT_RPCEVT, P4_HOSTED_RPC_TYPE_EVENT,
            769u + i, 0u, body, sizeof body, 0u) != 0u);
    rx_add_batch(&fixture, batch, used);
    rx_set_interrupts(&fixture, interrupt, 1u);

    CHECK(rx_poll(&fixture, RX_TEST_GENERATION, 0u,
        fixture.legacy.buffer, sizeof fixture.legacy.buffer) == 0);
    CHECK(fixture.result.error == P4_HOSTED_RX_QUEUE_FULL);
    CHECK(fixture.result.batches == 1u);
    CHECK(fixture.result.messages == P4_HOSTED_INBOX_CAPACITY);
    CHECK(fixture.legacy.state.rx_consumed == (uint32_t)used);
    CHECK(fixture.legacy.state.poisoned == 1u);
    CHECK(fixture.rx.poisoned == 1u);
    CHECK(fixture.rx.inbox.poisoned == 1u && fixture.rx.inbox.count == 0u);
    CHECK(fixture.legacy.fake.rx_fifo_reads == 1u);
    CHECK(fixture.legacy.fake.tx_fifo_writes == 0u);
}

static void run_rx_protocol_fault(unsigned checksum_enabled,
                                  unsigned corrupt_header)
{
    struct rx_fixture fixture;
    static const uint8_t body[] = { 0x11u, 0x22u, 0x33u };
    static const uint32_t interrupt[] = { P4_HOSTED_OPEN_INT_PACKET };
    uint8_t frame[TEST_FRAME_CAPACITY];
    size_t frame_size, used = 0u;

    rx_fixture_init(&fixture);
    rx_fixture_install_callbacks(&fixture);
    frame_size = append_rx_rpc(frame, sizeof frame, &used,
        HOSTED_SERIAL_ENDPOINT_RPCEVT, P4_HOSTED_RPC_TYPE_EVENT, 775u, 0u,
        body, sizeof body, checksum_enabled);
    CHECK(frame_size != 0u);
    if (corrupt_header)
        /* STA now carries opaque Ethernet. AP is still unsupported and
         * preserves this negative interface-header test without guessing
         * an Ethernet payload's contents. */
        frame[0] = (uint8_t)((frame[0] & 0xf0u) | P4_HOSTED_AP);
    else
        frame[frame_size - 1u] ^= 1u;
    rx_add_batch(&fixture, frame, frame_size);
    rx_set_interrupts(&fixture, interrupt, 1u);

    CHECK(rx_poll(&fixture, RX_TEST_GENERATION, checksum_enabled,
        fixture.legacy.buffer, sizeof fixture.legacy.buffer) == 0);
    CHECK(fixture.result.error == P4_HOSTED_RX_PROTOCOL);
    CHECK(fixture.legacy.state.poisoned == 1u);
    CHECK(fixture.rx.poisoned == 1u);
    CHECK(fixture.rx.inbox.poisoned == 1u);
    CHECK(fixture.rx.assembly.poisoned == 1u);
    CHECK(fixture.legacy.state.rx_consumed == (uint32_t)frame_size);
    CHECK(fixture.legacy.fake.rx_fifo_reads == 1u);
}

static void test_rx_malformed_and_checksum_poison(void)
{
    run_rx_protocol_fault(0u, 1u);
    run_rx_protocol_fault(1u, 0u);
}

static void test_rx_ack_fifo_and_counter_failures(void)
{
    struct rx_fixture fixture;
    static const uint8_t body[] = { 0x04u, 0x05u };
    static const uint32_t interrupt[] = { P4_HOSTED_OPEN_INT_PACKET };
    uint8_t frame[TEST_FRAME_CAPACITY];
    size_t frame_size, used;

    rx_fixture_init(&fixture);
    rx_fixture_install_callbacks(&fixture);
    used = 0u;
    frame_size = append_rx_rpc(frame, sizeof frame, &used,
        HOSTED_SERIAL_ENDPOINT_RPCEVT, P4_HOSTED_RPC_TYPE_EVENT, 775u, 0u,
        body, sizeof body, 0u);
    CHECK(frame_size != 0u);
    rx_add_batch(&fixture, frame, frame_size);
    rx_set_interrupts(&fixture, interrupt, 1u);
    fixture.legacy.fake.clear_return = 0;
    CHECK(rx_poll(&fixture, RX_TEST_GENERATION, 0u,
        fixture.legacy.buffer, sizeof fixture.legacy.buffer) == 0);
    CHECK(fixture.result.error == P4_HOSTED_RX_REGISTER);
    CHECK(fixture.legacy.fake.write_word_calls == 1u);
    CHECK(fixture.legacy.fake.length_reads == 0u);
    CHECK(fixture.legacy.state.poisoned == 1u && fixture.rx.poisoned == 1u);

    rx_fixture_init(&fixture);
    rx_fixture_install_callbacks(&fixture);
    used = 0u;
    frame_size = append_rx_rpc(frame, sizeof frame, &used,
        HOSTED_SERIAL_ENDPOINT_RPCEVT, P4_HOSTED_RPC_TYPE_EVENT, 775u, 0u,
        body, sizeof body, 0u);
    CHECK(frame_size != 0u);
    rx_add_batch(&fixture, frame, frame_size);
    rx_set_interrupts(&fixture, interrupt, 1u);
    fixture.legacy.fake.fifo_return = 0;
    CHECK(rx_poll(&fixture, RX_TEST_GENERATION, 0u,
        fixture.legacy.buffer, sizeof fixture.legacy.buffer) == 0);
    CHECK(fixture.result.error == P4_HOSTED_RX_TRANSFER);
    CHECK(fixture.legacy.fake.rx_fifo_reads == 1u);
    CHECK(fixture.legacy.state.poisoned == 1u &&
          fixture.legacy.state.busy == 0u);
    CHECK(fixture.rx.poisoned == 1u);

    rx_fixture_init(&fixture);
    rx_fixture_install_callbacks(&fixture);
    used = 0u;
    frame_size = append_rx_rpc(frame, sizeof frame, &used,
        HOSTED_SERIAL_ENDPOINT_RPCEVT, P4_HOSTED_RPC_TYPE_EVENT, 775u, 0u,
        body, sizeof body, 0u);
    CHECK(frame_size != 0u);
    rx_add_batch(&fixture, frame, frame_size);
    rx_set_interrupts(&fixture, interrupt, 1u);
    fixture.legacy.fake.counter_override_enabled = 1u;
    fixture.legacy.fake.counter_override = P4_HOSTED_FIFO_BUS_MAX + 1u;
    CHECK(rx_poll(&fixture, RX_TEST_GENERATION, 0u,
        fixture.legacy.buffer, sizeof fixture.legacy.buffer) == 0);
    CHECK(fixture.result.error == P4_HOSTED_RX_COUNTER);
    CHECK(fixture.legacy.fake.rx_fifo_reads == 0u);
    CHECK(fixture.legacy.state.poisoned == 1u && fixture.rx.poisoned == 1u);
}

static void test_rx_packet_ack_zero_delta_and_peer_fault(void)
{
    struct rx_fixture fixture;
    static const uint32_t packet[] = { P4_HOSTED_OPEN_INT_PACKET };
    static const uint32_t peer_fault[] = { P4_HOSTED_OPEN_INT_RX_UNDERFLOW };

    rx_fixture_init(&fixture);
    rx_fixture_install_callbacks(&fixture);
    rx_set_interrupts(&fixture, packet, 1u);
    fixture.legacy.fake.counter_override_enabled = 1u;
    fixture.legacy.fake.counter_override = 0u;
    CHECK(rx_poll(&fixture, RX_TEST_GENERATION, 0u,
        fixture.legacy.buffer, sizeof fixture.legacy.buffer) == 1);
    CHECK(fixture.result.error == P4_HOSTED_RX_OK);
    CHECK(fixture.result.batches == 0u && fixture.result.messages == 0u);
    CHECK(fixture.legacy.fake.write_word_calls == 1u);
    CHECK(fixture.legacy.fake.length_reads == 1u);
    CHECK(fixture.legacy.fake.rx_fifo_reads == 0u);
    CHECK(fixture.legacy.state.rx_consumed == 0u);
    CHECK(fixture.legacy.state.poisoned == 0u && fixture.rx.poisoned == 0u);

    rx_fixture_init(&fixture);
    rx_fixture_install_callbacks(&fixture);
    rx_set_interrupts(&fixture, peer_fault, 1u);
    CHECK(rx_poll(&fixture, RX_TEST_GENERATION, 0u,
        fixture.legacy.buffer, sizeof fixture.legacy.buffer) == 0);
    CHECK(fixture.result.error == P4_HOSTED_RX_PEER_FAULT);
    CHECK(fixture.legacy.state.poisoned == 1u && fixture.rx.poisoned == 1u);
    CHECK(fixture.rx.inbox.poisoned == 1u);
    CHECK(fixture.legacy.fake.write_word_calls == 0u);
    CHECK(fixture.legacy.fake.rx_fifo_reads == 0u);
}

static void test_rx_capacity_refusal_after_ack_releases_plan(void)
{
    struct rx_fixture fixture;
    uint8_t body[600];
    uint8_t frame[TEST_FRAME_CAPACITY];
    static const uint32_t interrupt[] = { P4_HOSTED_OPEN_INT_PACKET };
    size_t used = 0u, frame_size;
    size_t i;

    for (i = 0; i < sizeof body; ++i)
        body[i] = (uint8_t)(i * 19u + 7u);
    rx_fixture_init(&fixture);
    rx_fixture_install_callbacks(&fixture);
    frame_size = append_rx_rpc(frame, sizeof frame, &used,
        HOSTED_SERIAL_ENDPOINT_RPCEVT, P4_HOSTED_RPC_TYPE_EVENT, 775u, 0u,
        body, sizeof body, 0u);
    CHECK(frame_size > 512u);
    rx_add_batch(&fixture, frame, frame_size);
    rx_set_interrupts(&fixture, interrupt, 1u);

    CHECK(rx_poll(&fixture, RX_TEST_GENERATION, 0u,
        fixture.legacy.buffer, 512u) == 0);
    CHECK(fixture.result.error == P4_HOSTED_RX_CAPACITY);
    CHECK(fixture.legacy.fake.write_word_calls == 1u);
    CHECK(fixture.legacy.fake.rx_fifo_reads == 0u);
    CHECK(fixture.legacy.state.busy == 0u);
    CHECK(fixture.legacy.state.poisoned == 1u && fixture.rx.poisoned == 1u);
    CHECK(fixture.legacy.state.rx_consumed == 0u);
}

static void test_rx_partial_budget_expiry_poison(void)
{
    struct rx_fixture fixture;
    static const uint8_t body[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12 };
    static const uint32_t interrupt[] = {
        P4_HOSTED_OPEN_INT_PACKET, P4_HOSTED_OPEN_INT_PACKET
    };
    uint8_t message[TEST_LARGE_MESSAGE_CAPACITY];
    uint8_t stream[TEST_CAPACITY];
    size_t message_size, stream_size, first_size;

    rx_fixture_init(&fixture);
    rx_fixture_install_callbacks(&fixture);
    message_size = make_serial_rpc_message(message, sizeof message,
        HOSTED_SERIAL_ENDPOINT_RPCEVT, P4_HOSTED_RPC_TYPE_EVENT, 775u, 0u,
        body, sizeof body);
    stream_size = make_fragment_stream(stream, sizeof stream, message,
        message_size, 8u, 44u, 0u);
    first_size = P4_HOSTED_HEADER_SIZE + 8u;
    CHECK(stream_size > first_size);
    rx_add_batch(&fixture, stream, first_size);
    rx_set_interrupts(&fixture, interrupt, 2u);
    CHECK(rx_poll(&fixture, RX_TEST_GENERATION, 0u,
        fixture.legacy.buffer, sizeof fixture.legacy.buffer) == 1);
    CHECK(fixture.result.partial == 1u);
    CHECK(fixture.rx.assembly.fragments == 1u);

    rx_add_batch(&fixture, stream + first_size, stream_size - first_size);
    fixture.legacy.fake.expire_after_raw = 1u;
    CHECK(rx_poll(&fixture, RX_TEST_GENERATION, 0u,
        fixture.legacy.buffer, sizeof fixture.legacy.buffer) == 0);
    CHECK(fixture.result.error == P4_HOSTED_RX_BUDGET);
    CHECK(fixture.rx.poisoned == 1u && fixture.rx.assembly.poisoned == 1u);
    CHECK(fixture.rx.inbox.poisoned == 1u);
    CHECK(fixture.legacy.state.poisoned == 1u);
    CHECK(fixture.legacy.fake.rx_fifo_reads == 1u);
    CHECK(fixture.legacy.fake.tx_fifo_writes == 0u);
}

static void test_rx_counter_wrap(void)
{
    struct rx_fixture fixture;
    struct p4_hosted_inbox_item item;
    static const uint8_t body[] = { 0x61u, 0x62u, 0x63u };
    static const uint32_t interrupt[] = { P4_HOSTED_OPEN_INT_PACKET };
    uint8_t frame[TEST_FRAME_CAPACITY];
    size_t used = 0u, frame_size;
    uint32_t start;

    rx_fixture_init(&fixture);
    rx_fixture_install_callbacks(&fixture);
    frame_size = append_rx_rpc(frame, sizeof frame, &used,
        HOSTED_SERIAL_ENDPOINT_RPCEVT, P4_HOSTED_RPC_TYPE_EVENT, 775u, 0u,
        body, sizeof body, 0u);
    CHECK(frame_size != 0u);
    rx_add_batch(&fixture, frame, frame_size);
    rx_set_interrupts(&fixture, interrupt, 1u);
    start = P4_HOSTED_RX_MASK - 5u;
    fixture.legacy.state.rx_consumed = start;
    fixture.legacy.fake.rx_total = start;

    CHECK(rx_poll(&fixture, RX_TEST_GENERATION, 0u,
        fixture.legacy.buffer, sizeof fixture.legacy.buffer) == 1);
    CHECK(fixture.result.error == P4_HOSTED_RX_OK);
    CHECK(fixture.result.logical_bytes == frame_size);
    CHECK(fixture.legacy.state.rx_consumed ==
          ((start + (uint32_t)frame_size) & P4_HOSTED_RX_MASK));
    CHECK(fixture.rx.inbox.count == 1u);
    CHECK(p4_hosted_inbox_pop(&fixture.rx.inbox, RX_TEST_GENERATION,
                              &item) == 1);
    CHECK(item.msg_id == 775u && item.payload_size == sizeof body);
    CHECK(memcmp(item.payload, body, sizeof body) == 0);
}

static void test_rx_stale_preflight_and_alias_refusals(void)
{
    struct rx_fixture fixture;
    struct p4_hosted_inbox inbox_before;
    struct p4_hosted_rx_ops ops_before;

    rx_fixture_init(&fixture);
    rx_fixture_install_callbacks(&fixture);
    memcpy(&inbox_before, &fixture.rx.inbox, sizeof inbox_before);
    expect_rx_preflight_unchanged(&fixture, RX_TEST_GENERATION + 1u, 0u,
        fixture.legacy.buffer, sizeof fixture.legacy.buffer);
    expect_rx_preflight_unchanged(&fixture, RX_TEST_GENERATION, 0u,
        (uint8_t *)&fixture.rx, sizeof fixture.legacy.buffer);
    expect_rx_preflight_unchanged(&fixture, RX_TEST_GENERATION, 0u,
        fixture.legacy.buffer, 511u);
    expect_rx_preflight_unchanged(&fixture, RX_TEST_GENERATION, 2u,
        fixture.legacy.buffer, sizeof fixture.legacy.buffer);
    expect_rx_preflight_unchanged(&fixture, RX_TEST_GENERATION, 0u,
        (uint8_t *)(uintptr_t)(UINTPTR_MAX - 7u), 512u);
    ops_before = fixture.ops;
    fixture.ops.read_fifo = NULL;
    expect_rx_preflight_unchanged(&fixture, RX_TEST_GENERATION, 0u,
        fixture.legacy.buffer, sizeof fixture.legacy.buffer);
    CHECK(fixture.ops.read_fifo == NULL);
    fixture.ops = ops_before;
    CHECK(memcmp(&inbox_before, &fixture.rx.inbox, sizeof inbox_before) == 0);
    fixture.rx.inbox.generation = RX_REPLACEMENT_GENERATION;
    expect_rx_preflight_unchanged(&fixture, RX_TEST_GENERATION, 0u,
        fixture.legacy.buffer, sizeof fixture.legacy.buffer);
    fixture.rx.inbox.generation = RX_TEST_GENERATION;
    fixture.rx.inbox.poisoned = 1u;
    expect_rx_preflight_unchanged(&fixture, RX_TEST_GENERATION, 0u,
        fixture.legacy.buffer, sizeof fixture.legacy.buffer);
    fixture.rx.inbox.poisoned = 0u;
    fixture.rx.inbox.count = P4_HOSTED_INBOX_CAPACITY + 1u;
    expect_rx_preflight_unchanged(&fixture, RX_TEST_GENERATION, 0u,
        fixture.legacy.buffer, sizeof fixture.legacy.buffer);
    fixture.rx.inbox.count = 0u;
    fixture.rx.inbox.head = P4_HOSTED_INBOX_CAPACITY;
    expect_rx_preflight_unchanged(&fixture, RX_TEST_GENERATION, 0u,
        fixture.legacy.buffer, sizeof fixture.legacy.buffer);
    fixture.rx.inbox.head = 0u;
    fixture.rx.assembly.generation = RX_REPLACEMENT_GENERATION;
    expect_rx_preflight_unchanged(&fixture, RX_TEST_GENERATION, 0u,
        fixture.legacy.buffer, sizeof fixture.legacy.buffer);
}

static void test_rx_replacement_callback_preserves_new_epoch(void)
{
    struct rx_fixture fixture;
    static const uint8_t body[] = { 0x09u, 0x08u, 0x07u };
    static const uint32_t interrupt[] = { P4_HOSTED_OPEN_INT_PACKET };
    uint8_t frame[TEST_FRAME_CAPACITY];
    size_t used = 0u, frame_size;

    rx_fixture_init(&fixture);
    rx_fixture_install_callbacks(&fixture);
    frame_size = append_rx_rpc(frame, sizeof frame, &used,
        HOSTED_SERIAL_ENDPOINT_RPCEVT, P4_HOSTED_RPC_TYPE_EVENT, 775u, 0u,
        body, sizeof body, 0u);
    CHECK(frame_size != 0u);
    rx_add_batch(&fixture, frame, frame_size);
    rx_set_interrupts(&fixture, interrupt, 1u);
    fixture.callback.replace_rx_on_fifo = 1u;
    CHECK(p4_hosted_rx_start(&fixture.callback.replacement,
                             RX_REPLACEMENT_GENERATION));
    fixture.legacy.fake.replace_generation_after_fifo = 1u;
    fixture.legacy.fake.replacement_generation = RX_REPLACEMENT_GENERATION;

    CHECK(rx_poll(&fixture, RX_TEST_GENERATION, 0u,
        fixture.legacy.buffer, sizeof fixture.legacy.buffer) == 0);
    CHECK(fixture.result.error == P4_HOSTED_RX_SESSION);
    CHECK(fixture.callback.replaced_rx == 1u);
    CHECK(fixture.callback.captured_after == 1u);
    CHECK(memcmp(&fixture.rx, &fixture.callback.rx_after,
                 sizeof fixture.rx) == 0);
    CHECK(memcmp(&fixture.legacy.state, &fixture.callback.fifo_after,
                 sizeof fixture.legacy.state) == 0);
    CHECK(fixture.rx.generation == RX_REPLACEMENT_GENERATION);
    CHECK(fixture.rx.poisoned == 0u && fixture.rx.inbox.poisoned == 0u);
    CHECK(fixture.rx.inbox.generation == RX_REPLACEMENT_GENERATION);
    CHECK(fixture.legacy.state.generation == RX_REPLACEMENT_GENERATION);
    CHECK(fixture.legacy.state.poisoned == 0u);
    CHECK(fixture.legacy.state.rx_consumed == 0u);
    CHECK(fixture.legacy.fake.tx_fifo_writes == 0u);
}

static void test_rx_start_child_refusals_are_transactional(void)
{
    struct p4_hosted_rx state, before;

    memset(&state, 0, sizeof state);
    CHECK(p4_hosted_inbox_init(&state.inbox, RX_TEST_GENERATION));
    memcpy(&before, &state, sizeof before);
    CHECK(p4_hosted_rx_start(&state, RX_TEST_GENERATION) == 0);
    CHECK(memcmp(&before, &state, sizeof state) == 0);

    memset(&state, 0, sizeof state);
    CHECK(p4_hosted_reassembly_start(&state.assembly, RX_TEST_GENERATION));
    memcpy(&before, &state, sizeof before);
    CHECK(p4_hosted_rx_start(&state, RX_TEST_GENERATION) == 0);
    CHECK(memcmp(&before, &state, sizeof state) == 0);

    memset(&state, 0, sizeof state);
    state.inbox.head = P4_HOSTED_INBOX_CAPACITY;
    memcpy(&before, &state, sizeof before);
    CHECK(p4_hosted_rx_start(&state, RX_TEST_GENERATION) == 0);
    CHECK(memcmp(&before, &state, sizeof state) == 0);
}

int main(void)
{
    test_rx_idle_poll_is_one_nonblocking_batch();
    test_rx_owned_event_and_response_items();
    test_rx_fragment_survives_idle_poll();
    test_rx_queue_overflow_poison();
    test_rx_malformed_and_checksum_poison();
    test_rx_ack_fifo_and_counter_failures();
    test_rx_packet_ack_zero_delta_and_peer_fault();
    test_rx_capacity_refusal_after_ack_releases_plan();
    test_rx_partial_budget_expiry_poison();
    test_rx_counter_wrap();
    test_rx_stale_preflight_and_alias_refusals();
    test_rx_replacement_callback_preserves_new_epoch();
    test_rx_start_child_refusals_are_transactional();
    printf("hosted_rx: %lu checks passed\n", checks);
    return 0;
}
