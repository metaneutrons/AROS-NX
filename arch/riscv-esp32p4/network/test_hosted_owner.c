/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
/* Exercise the integrated shared-host owner against the real pending, RX,
 * FIFO, RPC, serial and Hosted codecs. All timing/acquisition callbacks are
 * deterministic host fakes; this is not a hardware or timing acceptance test. */
#define main hosted_control_legacy_test_main
#include "test_hosted_control.c"
#undef main

#include "hosted_owner.h"
#include "hosted_request_ids.h"

#define OWNER_TEST_GENERATION UINT32_C(0x7191)
#define OWNER_NEXT_GENERATION UINT32_C(0x7192)
#define OWNER_TEST_REQUEST_ID 257u
#define OWNER_TIMEOUT_US 250u

struct owner_fixture {
    struct p4_hosted_owner owner;
    struct p4_hosted_fifo fifo;
    struct p4_hosted_owner_ops owner_ops;
    struct p4_hosted_rx_ops rx_ops;
    struct p4_hosted_tx_ops tx_ops;
    struct p4_hosted_owner_result result;
    struct fake_host fake;
    uint32_t generation;
    uint32_t now;
    uint32_t slice_start;
    uint32_t slice_us;
    uint32_t wait_step_us;
    uint32_t slices[P4_HOSTED_OWNER_MAX_POLLS];
    size_t slice_count;
    unsigned acquired;
    unsigned acquire_attempts;
    unsigned acquire_successes;
    unsigned releases;
    unsigned waits;
    unsigned busy_remaining;
    unsigned always_busy;
    unsigned budget_fail;
    unsigned stopping;
    unsigned event_calls;
    unsigned frame_calls;
    unsigned dispatch_under_lock;
    unsigned acquire_attempts_at_dispatch;
    uint32_t dispatch_advance_us;
    uint32_t rx_advance_us;
    struct p4_hosted_inbox_item last_event;
    uint8_t request[P4_HOSTED_PENDING_MAX_REQUEST_SIZE];
    size_t request_size;
};

static void owner_check_acquired(const struct owner_fixture *fixture)
{
    CHECK(fixture->acquired == 1u);
}

static int owner_acquire(void *context, uint32_t remaining_us)
{
    struct owner_fixture *fixture = context;

    CHECK(fixture->acquired == 0u);
    CHECK(remaining_us != 0u &&
          remaining_us <= P4_HOSTED_OWNER_MAX_ACQUIRE_US);
    ++fixture->acquire_attempts;
    if (fixture->always_busy || fixture->busy_remaining != 0u) {
        if (fixture->busy_remaining != 0u)
            --fixture->busy_remaining;
        return 0;
    }
    fixture->acquired = 1u;
    ++fixture->acquire_successes;
    return 1;
}

static void owner_release(void *context)
{
    struct owner_fixture *fixture = context;
    owner_check_acquired(fixture);
    fixture->acquired = 0u;
    ++fixture->releases;
}

static uint32_t owner_now(void *context)
{
    const struct owner_fixture *fixture = context;
    return fixture->now;
}

static void owner_wait(void *context)
{
    struct owner_fixture *fixture = context;
    CHECK(fixture->acquired == 0u);
    ++fixture->waits;
    fixture->now += fixture->wait_step_us;
}

static int owner_set_budget(void *context, uint32_t slice_us)
{
    struct owner_fixture *fixture = context;
    owner_check_acquired(fixture);
    CHECK(slice_us != 0u && slice_us <= P4_HOSTED_OWNER_MAX_SLICE_US);
    CHECK(fixture->slice_count < sizeof fixture->slices /
                                      sizeof fixture->slices[0]);
    fixture->slice_start = fixture->now;
    fixture->slice_us = slice_us;
    fixture->slices[fixture->slice_count++] = slice_us;
    return fixture->budget_fail ? 0 : 1;
}

static int owner_stopping(void *context)
{
    const struct owner_fixture *fixture = context;
    return fixture->stopping ? 1 : 0;
}

static int owner_dispatch_event(void *context,
                                const struct p4_hosted_inbox_item *item)
{
    struct owner_fixture *fixture = context;
    CHECK(item != NULL);
    if (fixture->acquired != 0u)
        fixture->dispatch_under_lock = 1u;
    ++fixture->event_calls;
    fixture->acquire_attempts_at_dispatch = fixture->acquire_attempts;
    fixture->last_event = *item;
    fixture->now += fixture->dispatch_advance_us;
    return fixture->acquired == 0u ? 1 : 0;
}

static int owner_dispatch_frame(void *context, const uint8_t *bytes,
                                size_t size)
{
    struct owner_fixture *fixture = context;
    CHECK(bytes != NULL && size >= P4_HOSTED_ETHERNET_HEADER_SIZE);
    if (fixture->acquired != 0u)
        fixture->dispatch_under_lock = 1u;
    ++fixture->frame_calls;
    return fixture->acquired == 0u ? 1 : 0;
}

static int owner_session_ok(void *context, uint32_t generation)
{
    struct owner_fixture *fixture = context;
    owner_check_acquired(fixture);
    return generation == fixture->generation &&
           fixture->fake.session_value == 1;
}

static int owner_budget_ok(void *context)
{
    struct owner_fixture *fixture = context;
    owner_check_acquired(fixture);
    if (fixture->budget_fail || fixture->slice_us == 0u)
        return 0;
    return (uint32_t)(fixture->now - fixture->slice_start) <
           fixture->slice_us;
}

static int owner_rx_read_word(void *context, uint32_t address,
                              uint32_t *value)
{
    struct owner_fixture *fixture = context;
    int result;
    owner_check_acquired(fixture);
    result = fake_rx_read_word(&fixture->fake, address, value);
    fixture->now += fixture->rx_advance_us;
    return result;
}

static int owner_rx_write_word(void *context, uint32_t address,
                               uint32_t value)
{
    struct owner_fixture *fixture = context;
    int result;
    owner_check_acquired(fixture);
    result = fake_write_word(&fixture->fake, address, value);
    fixture->now += fixture->rx_advance_us;
    return result;
}

static int owner_rx_read_fifo(void *context,
    const struct p4_hosted_fifo_plan *plan, uint8_t *buffer)
{
    struct owner_fixture *fixture = context;
    int result;
    owner_check_acquired(fixture);
    result = fake_read_fifo(&fixture->fake, plan, buffer);
    fixture->now += fixture->rx_advance_us;
    return result;
}

static int owner_tx_read_word(void *context, uint32_t address,
                              uint32_t *value)
{
    struct owner_fixture *fixture = context;
    owner_check_acquired(fixture);
    return fake_tx_read_word(&fixture->fake, address, value);
}

static int owner_tx_write_fifo(void *context,
    const struct p4_hosted_fifo_plan *plan, const uint8_t *buffer,
    struct p4_hosted_tx_completion *completion)
{
    struct owner_fixture *fixture = context;
    owner_check_acquired(fixture);
    return fake_write_fifo(&fixture->fake, plan, buffer, completion);
}

static void owner_tx_yield(void *context)
{
    struct owner_fixture *fixture = context;
    owner_check_acquired(fixture);
    fake_yield(&fixture->fake);
}

static void owner_fixture_prepare(struct owner_fixture *fixture,
                                  uint32_t generation, uint32_t now)
{
    memset(fixture, 0, sizeof *fixture);
    fixture->generation = generation;
    fixture->now = now;
    fixture->wait_step_us = 10u;
    CHECK(p4_hosted_fifo_start(&fixture->fifo, generation, TEST_POOL_LIMIT));

    fixture->fake.state = &fixture->fifo;
    fixture->fake.generation = generation;
    fixture->fake.session_value = 1;
    fixture->fake.budget_value = 1;
    fixture->fake.token = UINT32_C(1) << 16;
    fixture->fake.raw_return = 1;
    fixture->fake.length_return = 1;
    fixture->fake.clear_return = 1;
    fixture->fake.fifo_return = 1;
    fixture->fake.token_return = 1;
    fixture->fake.tx_fifo_return = 1;

    CHECK(p4_hosted_rx_start(&fixture->owner.rx, generation));
    fixture->rx_ops.context = fixture;
    fixture->rx_ops.session_ok = owner_session_ok;
    fixture->rx_ops.budget_ok = owner_budget_ok;
    fixture->rx_ops.read_word = owner_rx_read_word;
    fixture->rx_ops.write_word = owner_rx_write_word;
    fixture->rx_ops.read_fifo = owner_rx_read_fifo;

    fixture->tx_ops.context = fixture;
    fixture->tx_ops.read_word = owner_tx_read_word;
    fixture->tx_ops.write_fifo = owner_tx_write_fifo;
    fixture->tx_ops.session_ok = owner_session_ok;
    fixture->tx_ops.budget_ok = owner_budget_ok;
    fixture->tx_ops.yield = owner_tx_yield;

    fixture->owner_ops.context = fixture;
    fixture->owner_ops.acquire = owner_acquire;
    fixture->owner_ops.release = owner_release;
    fixture->owner_ops.now_us = owner_now;
    fixture->owner_ops.wait = owner_wait;
    fixture->owner_ops.set_budget = owner_set_budget;
    fixture->owner_ops.stopping = owner_stopping;
    fixture->owner_ops.dispatch_event = owner_dispatch_event;
    fixture->owner_ops.dispatch_frame = owner_dispatch_frame;
    fixture->owner_ops.rx = &fixture->rx_ops;
    fixture->owner_ops.tx = &fixture->tx_ops;
}

static void owner_fixture_init(struct owner_fixture *fixture,
                               uint32_t generation, uint32_t now)
{
    owner_fixture_prepare(fixture, generation, now);
    CHECK(p4_hosted_owner_ids_init(&fixture->owner, generation, now));
}

static void owner_reserve_empty_request(struct owner_fixture *fixture,
                                        uint32_t request_id)
{
    struct p4_hosted_rpc rpc = {0};
    uint32_t uid = 0u;

    CHECK(p4_hosted_request_ids_reserve(&fixture->owner.ids,
        fixture->generation, request_id, &uid));
    rpc.msg_type = P4_HOSTED_RPC_TYPE_REQUEST;
    rpc.msg_id = request_id;
    rpc.uid = uid;
    CHECK(p4_hosted_rpc_encode(fixture->request, sizeof fixture->request,
                               &rpc, &fixture->request_size));
}

static size_t append_owner_rpc(uint8_t *destination, size_t capacity,
                               size_t *used,
                               enum hosted_serial_endpoint endpoint,
                               uint32_t type, uint32_t id, uint32_t uid,
                               const uint8_t *body, size_t body_size)
{
    uint8_t frame[TEST_FRAME_CAPACITY];
    size_t frame_size = make_serial_rpc_frame(frame, sizeof frame, endpoint,
        type, id, uid, body, body_size, 0u, P4_HOSTED_SERIAL, 0u, 0u, 0u);
    if (frame_size == 0u || !append_bytes(destination, capacity, used,
                                           frame, frame_size))
        return 0u;
    return frame_size;
}

static void owner_verify_tx_request(const struct owner_fixture *fixture,
                                    uint32_t uid, uint32_t request_id)
{
    struct p4_hosted_frame frame;
    struct hosted_serial_message serial;
    struct p4_hosted_rpc rpc;

    CHECK(fixture->fake.tx_fifo_writes == 1u);
    CHECK(p4_hosted_decode(fixture->fake.tx_capture,
        fixture->fake.tx_capture_size, 0u, &frame));
    CHECK(frame.interface_type == P4_HOSTED_SERIAL &&
          frame.interface_number == 0u && frame.flags == 0u &&
          frame.sequence == 0u && frame.packet_type == 0u &&
          frame.throttle == 0u);
    CHECK(hosted_serial_decode(frame.payload, frame.payload_size, &serial));
    CHECK(serial.endpoint == HOSTED_SERIAL_ENDPOINT_RPCRSP);
    CHECK(p4_hosted_rpc_decode(serial.protobuf, serial.protobuf_size, &rpc));
    CHECK(rpc.msg_type == P4_HOSTED_RPC_TYPE_REQUEST &&
          rpc.msg_id == request_id && rpc.uid == uid);
}

static void test_exchange_dispatches_after_release_across_clock_wrap(void)
{
    static const uint8_t event_body[] = { 0x12u, 0x34u };
    struct owner_fixture fixture;
    uint8_t batch[TEST_CAPACITY];
    uint32_t raw[] = { 0u, P4_HOSTED_OPEN_INT_PACKET };
    uint32_t uid;
    size_t used = 0u;

    owner_fixture_init(&fixture, OWNER_TEST_GENERATION, UINT32_MAX - 25u);
    fixture.busy_remaining = 1u;
    fixture.wait_step_us = 30u;
    owner_reserve_empty_request(&fixture, OWNER_TEST_REQUEST_ID);
    uid = fixture.owner.ids.active_uid;
    CHECK(append_owner_rpc(batch, sizeof batch, &used,
        HOSTED_SERIAL_ENDPOINT_RPCEVT, P4_HOSTED_RPC_TYPE_EVENT, 769u, 0u,
        event_body, sizeof event_body) != 0u);
    CHECK(append_owner_rpc(batch, sizeof batch, &used,
        HOSTED_SERIAL_ENDPOINT_RPCRSP, P4_HOSTED_RPC_TYPE_RESPONSE,
        OWNER_TEST_REQUEST_ID + 256u, uid, NULL, 0u) != 0u);
    CHECK(add_batch(&fixture.fake, batch, used));
    set_raws(&fixture.fake, raw, 2u);

    CHECK(p4_hosted_owner_exchange(&fixture.owner, &fixture.fifo,
        &fixture.owner_ops, fixture.generation, 0u, fixture.request,
        fixture.request_size, OWNER_TIMEOUT_US, &fixture.result));
    CHECK(fixture.result.error == P4_HOSTED_OWNER_OK &&
          fixture.result.matched == 1u);
    CHECK(fixture.result.error == P4_HOSTED_OWNER_OK &&
          fixture.result.submitted == 1u && fixture.result.matched == 1u);
    CHECK(fixture.result.uid == uid &&
          fixture.result.request_id == OWNER_TEST_REQUEST_ID);
    CHECK(fixture.result.events == 1u && fixture.event_calls == 1u &&
          fixture.dispatch_under_lock == 0u);
    CHECK(fixture.last_event.msg_type == P4_HOSTED_RPC_TYPE_EVENT &&
          fixture.last_event.msg_id == 769u &&
          fixture.last_event.uid == 0u &&
          fixture.last_event.payload_size == sizeof event_body &&
          memcmp(fixture.last_event.payload, event_body,
                 sizeof event_body) == 0);
    CHECK(fixture.owner.reply.msg_type == P4_HOSTED_RPC_TYPE_RESPONSE &&
          fixture.owner.reply.msg_id == OWNER_TEST_REQUEST_ID + 256u &&
          fixture.owner.reply.uid == uid);
    CHECK(fixture.now < 100u && fixture.result.elapsed_us < OWNER_TIMEOUT_US);
    CHECK(fixture.acquire_attempts >= 4u && fixture.acquire_successes ==
          fixture.releases && fixture.waits >= 2u && fixture.acquired == 0u);
    CHECK(fixture.slice_count >= 2u && fixture.slices[0] <= OWNER_TIMEOUT_US);
    CHECK(fixture.fake.tx_fifo_writes == 1u && fixture.fake.rx_fifo_reads == 1u);
    CHECK(fixture.fifo.tx_sent == 1u && fixture.fifo.rx_consumed != 0u &&
          fixture.fifo.poisoned == 0u);
    owner_verify_tx_request(&fixture, uid, OWNER_TEST_REQUEST_ID);
}

static void test_exchange_deadline_is_not_renewed_and_uid_is_consumed(void)
{
    struct owner_fixture fixture;
    uint32_t previous = UINT32_MAX;
    size_t index;

    owner_fixture_init(&fixture, OWNER_TEST_GENERATION, 100u);
    fixture.wait_step_us = 100u;
    owner_reserve_empty_request(&fixture, OWNER_TEST_REQUEST_ID);
    CHECK(!p4_hosted_owner_exchange(&fixture.owner, &fixture.fifo,
        &fixture.owner_ops, fixture.generation, 0u, fixture.request,
        fixture.request_size, 500u, &fixture.result));
    CHECK(fixture.result.error == P4_HOSTED_OWNER_TIMEOUT);
    CHECK(fixture.result.submitted == 1u && fixture.result.matched == 0u);
    CHECK(fixture.result.elapsed_us == 500u);
    CHECK(fixture.fake.tx_fifo_writes == 1u && fixture.fake.rx_fifo_reads == 0u);
    CHECK(fixture.owner.ids.next_uid == 2u &&
          fixture.owner.ids.active_uid == 0u &&
          fixture.owner.ids.retired == 1u);
    CHECK(fixture.owner.pending_active == 0u && fixture.owner.poisoned == 1u);
    CHECK(fixture.fifo.poisoned == 1u && fixture.acquired == 0u);
    CHECK(fixture.slice_count >= 2u && fixture.slices[0] <= 500u);
    for (index = 0u; index < fixture.slice_count; ++index) {
        CHECK(fixture.slices[index] <= 500u);
        CHECK(fixture.slices[index] <= previous);
        previous = fixture.slices[index];
    }
    CHECK(fixture.waits > 0u && fixture.acquire_successes == fixture.releases);
    owner_verify_tx_request(&fixture, 1u, OWNER_TEST_REQUEST_ID);
}

static void test_preserved_matching_response_is_unmatched_until_tx(void)
{
    struct owner_fixture fixture;
    struct p4_hosted_rpc early = {0};
    uint8_t response[TEST_FRAME_CAPACITY];
    uint32_t raw[] = { P4_HOSTED_OPEN_INT_PACKET };
    size_t response_size;
    uint32_t uid;

    owner_fixture_init(&fixture, OWNER_TEST_GENERATION, 100u);
    owner_reserve_empty_request(&fixture, OWNER_TEST_REQUEST_ID);
    uid = fixture.owner.ids.active_uid;

    /* Model an exact response already copied from the preserving OPEN suffix.
     * It must be drained but cannot complete a request not yet sent. */
    early.msg_type = P4_HOSTED_RPC_TYPE_RESPONSE;
    early.msg_id = OWNER_TEST_REQUEST_ID + 256u;
    early.uid = uid;
    CHECK(p4_hosted_inbox_push(&fixture.owner.rx.inbox,
        fixture.generation, &early));
    response_size = make_serial_rpc_frame(response, sizeof response,
        HOSTED_SERIAL_ENDPOINT_RPCRSP, P4_HOSTED_RPC_TYPE_RESPONSE,
        OWNER_TEST_REQUEST_ID + 256u, uid, NULL, 0u, 0u,
        P4_HOSTED_SERIAL, 0u, 0u, 0u);
    CHECK(response_size != 0u);
    CHECK(add_batch(&fixture.fake, response, response_size));
    set_raws(&fixture.fake, raw, 1u);

    CHECK(p4_hosted_owner_exchange(&fixture.owner, &fixture.fifo,
        &fixture.owner_ops, fixture.generation, 0u, fixture.request,
        fixture.request_size, 500u, &fixture.result));
    CHECK(fixture.result.error == P4_HOSTED_OWNER_OK &&
          fixture.result.submitted == 1u && fixture.result.matched == 1u &&
          fixture.result.unmatched == 1u);
    CHECK(fixture.fake.tx_fifo_writes == 1u &&
          fixture.owner.reply.uid == uid &&
          fixture.owner.reply.msg_id == OWNER_TEST_REQUEST_ID + 256u);
    owner_verify_tx_request(&fixture, uid, OWNER_TEST_REQUEST_ID);
}

static void test_poll_callback_past_deadline_cannot_match(void)
{
    struct owner_fixture fixture;
    uint8_t response[TEST_FRAME_CAPACITY];
    uint32_t raw[] = { P4_HOSTED_OPEN_INT_PACKET };
    size_t response_size;

    owner_fixture_init(&fixture, OWNER_TEST_GENERATION, 100u);
    owner_reserve_empty_request(&fixture, OWNER_TEST_REQUEST_ID);
    response_size = make_serial_rpc_frame(response, sizeof response,
        HOSTED_SERIAL_ENDPOINT_RPCRSP, P4_HOSTED_RPC_TYPE_RESPONSE,
        OWNER_TEST_REQUEST_ID + 256u, 1u, NULL, 0u, 0u,
        P4_HOSTED_SERIAL, 0u, 0u, 0u);
    CHECK(response_size != 0u);
    CHECK(add_batch(&fixture.fake, response, response_size));
    set_raws(&fixture.fake, raw, 1u);
    fixture.rx_advance_us = 101u;

    CHECK(!p4_hosted_owner_exchange(&fixture.owner, &fixture.fifo,
        &fixture.owner_ops, fixture.generation, 0u, fixture.request,
        fixture.request_size, 100u, &fixture.result));
    CHECK(fixture.result.error != P4_HOSTED_OWNER_OK &&
          fixture.result.matched == 0u);
    CHECK(fixture.fake.tx_fifo_writes == 1u && fixture.acquired == 0u);
    CHECK(fixture.owner.ids.retired == 1u &&
          fixture.owner.ids.next_uid == 2u && fixture.fifo.poisoned == 1u);
}

static void test_dispatch_past_deadline_cannot_return_match_or_idle_success(void)
{
    static const uint8_t event_body[] = { 0x41u };
    struct owner_fixture fixture;
    uint8_t batch[TEST_CAPACITY];
    uint32_t raw[] = { P4_HOSTED_OPEN_INT_PACKET };
    size_t used = 0u;

    owner_fixture_init(&fixture, OWNER_TEST_GENERATION, 200u);
    owner_reserve_empty_request(&fixture, OWNER_TEST_REQUEST_ID);
    CHECK(append_owner_rpc(batch, sizeof batch, &used,
        HOSTED_SERIAL_ENDPOINT_RPCRSP, P4_HOSTED_RPC_TYPE_RESPONSE,
        OWNER_TEST_REQUEST_ID + 256u, 1u, NULL, 0u) != 0u);
    CHECK(append_owner_rpc(batch, sizeof batch, &used,
        HOSTED_SERIAL_ENDPOINT_RPCEVT, P4_HOSTED_RPC_TYPE_EVENT, 769u, 0u,
        event_body, sizeof event_body) != 0u);
    CHECK(add_batch(&fixture.fake, batch, used));
    set_raws(&fixture.fake, raw, 1u);
    fixture.dispatch_advance_us = 101u;

    CHECK(!p4_hosted_owner_exchange(&fixture.owner, &fixture.fifo,
        &fixture.owner_ops, fixture.generation, 0u, fixture.request,
        fixture.request_size, 100u, &fixture.result));
    CHECK(fixture.result.error == P4_HOSTED_OWNER_TIMEOUT &&
          fixture.event_calls == 1u && fixture.dispatch_under_lock == 0u);
    CHECK(fixture.owner.ids.retired == 1u &&
          fixture.owner.ids.next_uid == 2u && fixture.owner.poisoned == 1u);
    CHECK(memcmp(&fixture.owner.reply,
          &(const struct p4_hosted_inbox_item){0},
          sizeof fixture.owner.reply) == 0);

    owner_fixture_init(&fixture, OWNER_TEST_GENERATION, 300u);
    used = 0u;
    CHECK(append_owner_rpc(batch, sizeof batch, &used,
        HOSTED_SERIAL_ENDPOINT_RPCEVT, P4_HOSTED_RPC_TYPE_EVENT, 770u, 0u,
        event_body, sizeof event_body) != 0u);
    CHECK(add_batch(&fixture.fake, batch, used));
    set_raws(&fixture.fake, raw, 1u);
    fixture.dispatch_advance_us = 101u;
    CHECK(!p4_hosted_owner_step(&fixture.owner, &fixture.fifo,
        &fixture.owner_ops, fixture.generation, 0u, 100u,
        &fixture.result));
    CHECK(fixture.result.error == P4_HOSTED_OWNER_TIMEOUT &&
          fixture.result.events == 1u && fixture.dispatch_under_lock == 0u);
    CHECK(fixture.owner.poisoned == 1u && fixture.fifo.poisoned == 1u);
}

static void test_new_partial_after_completion_gets_new_deadline(void)
{
    static const uint8_t body_a[] = { 0x01u, 0x02u };
    static const uint8_t body_b[] = { 0x03u, 0x04u };
    struct owner_fixture fixture;
    uint8_t batch_a[TEST_CAPACITY], batch_b[TEST_CAPACITY];
    uint8_t message_a[TEST_LARGE_MESSAGE_CAPACITY];
    uint8_t message_b[TEST_LARGE_MESSAGE_CAPACITY];
    uint32_t raw[] = { P4_HOSTED_OPEN_INT_PACKET,
                       P4_HOSTED_OPEN_INT_PACKET, 0u };
    size_t used_a = 0u, used_b = 0u, size_a, size_b, split_a, split_b;
    uint32_t old_started, new_started;

    owner_fixture_init(&fixture, OWNER_TEST_GENERATION, 1000u);
    size_a = make_serial_rpc_message(message_a, sizeof message_a,
        HOSTED_SERIAL_ENDPOINT_RPCEVT, P4_HOSTED_RPC_TYPE_EVENT, 771u, 0u,
        body_a, sizeof body_a);
    size_b = make_serial_rpc_message(message_b, sizeof message_b,
        HOSTED_SERIAL_ENDPOINT_RPCEVT, P4_HOSTED_RPC_TYPE_EVENT, 772u, 0u,
        body_b, sizeof body_b);
    CHECK(size_a > 2u && size_b > 2u);
    split_a = size_a / 2u;
    split_b = size_b / 2u;
    CHECK(append_fragment(batch_a, sizeof batch_a, &used_a, message_a,
        split_a, 0u, 1u, 0u));
    CHECK(append_fragment(batch_b, sizeof batch_b, &used_b,
        message_a + split_a, size_a - split_a, 1u, 0u, 0u));
    CHECK(append_fragment(batch_b, sizeof batch_b, &used_b, message_b,
        split_b, 2u, 1u, 0u));
    CHECK(add_batch(&fixture.fake, batch_a, used_a));
    CHECK(add_batch(&fixture.fake, batch_b, used_b));
    set_raws(&fixture.fake, raw, 3u);

    CHECK(p4_hosted_owner_step(&fixture.owner, &fixture.fifo,
        &fixture.owner_ops, fixture.generation, 0u, 1000u,
        &fixture.result));
    CHECK(fixture.owner.partial_active == 1u &&
          fixture.owner.rx.assembly.fragments == 1u);
    old_started = fixture.owner.partial_started_us;
    fixture.now = old_started + P4_HOSTED_OWNER_PARTIAL_TIMEOUT_US - 100u;

    CHECK(p4_hosted_owner_step(&fixture.owner, &fixture.fifo,
        &fixture.owner_ops, fixture.generation, 0u, 1000u,
        &fixture.result));
    CHECK(fixture.result.events == 1u && fixture.owner.partial_active == 1u &&
          fixture.owner.rx.assembly.fragments == 1u);
    new_started = fixture.owner.partial_started_us;
    CHECK(new_started != old_started && new_started == fixture.now);

    fixture.now = new_started + P4_HOSTED_OWNER_PARTIAL_TIMEOUT_US - 1u;
    CHECK(p4_hosted_owner_step(&fixture.owner, &fixture.fifo,
        &fixture.owner_ops, fixture.generation, 0u, 1000u,
        &fixture.result));
    CHECK(fixture.owner.partial_active == 1u && fixture.fifo.poisoned == 0u);
    fixture.now = new_started + P4_HOSTED_OWNER_PARTIAL_TIMEOUT_US;
    CHECK(!p4_hosted_owner_step(&fixture.owner, &fixture.fifo,
        &fixture.owner_ops, fixture.generation, 0u, 1000u,
        &fixture.result));
    CHECK(fixture.result.error == P4_HOSTED_OWNER_TIMEOUT &&
          fixture.owner.poisoned == 1u && fixture.fifo.poisoned == 1u);
}

static void test_open_partial_age_is_preserved(void)
{
    static const uint8_t body[] = { 0x31u, 0x32u, 0x33u, 0x34u };
    struct owner_fixture fixture;
    struct p4_hosted_frame decoded;
    uint8_t message[TEST_LARGE_MESSAGE_CAPACITY];
    uint8_t frame_bytes[TEST_FRAME_CAPACITY];
    size_t message_size, frame_size, split;
    uint32_t open_started = 12345u;

    owner_fixture_prepare(&fixture, OWNER_TEST_GENERATION,
        open_started + P4_HOSTED_OWNER_PARTIAL_TIMEOUT_US);
    message_size = make_serial_rpc_message(message, sizeof message,
        HOSTED_SERIAL_ENDPOINT_RPCEVT, P4_HOSTED_RPC_TYPE_EVENT, 773u, 0u,
        body, sizeof body);
    CHECK(message_size > 2u);
    split = message_size / 2u;
    frame_size = make_fragment_frame(frame_bytes, sizeof frame_bytes,
        message, split, 0u, 1u, 0u);
    CHECK(frame_size != 0u && p4_hosted_decode(frame_bytes, frame_size, 0u,
                                                &decoded));
    CHECK(p4_hosted_reassembly_feed(&fixture.owner.rx.assembly,
        fixture.generation, &decoded) == P4_HOSTED_REASSEMBLY_PARTIAL);
    CHECK(fixture.owner.rx.assembly.fragments == 1u);

    CHECK(p4_hosted_owner_ids_init(&fixture.owner, fixture.generation,
                                    open_started));
    CHECK(fixture.owner.partial_active == 1u &&
          fixture.owner.partial_started_us == open_started);
    CHECK(!p4_hosted_owner_step(&fixture.owner, &fixture.fifo,
        &fixture.owner_ops, fixture.generation, 0u, 1000u,
        &fixture.result));
    CHECK(fixture.result.error == P4_HOSTED_OWNER_TIMEOUT &&
          fixture.owner.poisoned == 1u && fixture.owner.rx.poisoned == 1u &&
          fixture.fifo.poisoned == 1u && fixture.owner.ids.retired == 1u);
    CHECK(fixture.acquire_attempts == 0u && fixture.releases == 0u &&
          fixture.fake.tx_fifo_writes == 0u &&
          fixture.fake.rx_fifo_reads == 0u);
}

static void test_invalid_fifo_direction_refuses_without_mutation(void)
{
    struct owner_fixture fixture;
    struct p4_hosted_owner owner_before;
    struct p4_hosted_fifo fifo_before;
    struct p4_hosted_owner_result result_before;

    owner_fixture_init(&fixture, OWNER_TEST_GENERATION, 700u);
    fixture.fifo.pending_write = 2u;
    memset(&fixture.result, 0x6du, sizeof fixture.result);
    owner_before = fixture.owner;
    fifo_before = fixture.fifo;
    result_before = fixture.result;
    CHECK(!p4_hosted_owner_step(&fixture.owner, &fixture.fifo,
        &fixture.owner_ops, fixture.generation, 0u, 1000u,
        &fixture.result));
    CHECK(memcmp(&fixture.owner, &owner_before, sizeof owner_before) == 0 &&
          memcmp(&fixture.fifo, &fifo_before, sizeof fifo_before) == 0 &&
          memcmp(&fixture.result, &result_before, sizeof result_before) == 0);
    CHECK(fixture.acquire_attempts == 0u && fixture.waits == 0u &&
          fixture.releases == 0u);
}

static void test_busy_host_timeout_uses_one_deadline(void)
{
    struct owner_fixture fixture;

    owner_fixture_init(&fixture, OWNER_TEST_GENERATION, 0xffffff00u);
    fixture.always_busy = 1u;
    fixture.wait_step_us = 100u;
    owner_reserve_empty_request(&fixture, OWNER_TEST_REQUEST_ID);
    CHECK(!p4_hosted_owner_exchange(&fixture.owner, &fixture.fifo,
        &fixture.owner_ops, fixture.generation, 0u, fixture.request,
        fixture.request_size, 500u, &fixture.result));
    CHECK(fixture.result.error == P4_HOSTED_OWNER_TIMEOUT &&
          fixture.result.submitted == 0u && fixture.result.elapsed_us == 500u);
    CHECK(fixture.acquire_attempts == 5u && fixture.waits == 5u &&
          fixture.acquire_successes == 0u && fixture.releases == 0u);
    CHECK(fixture.slice_count == 0u && fixture.fake.tx_fifo_writes == 0u);
    CHECK(fixture.owner.ids.next_uid == 2u &&
          fixture.owner.ids.retired == 1u && fixture.owner.poisoned == 1u);
}

static void test_stopped_clock_uses_poll_cap(void)
{
    struct owner_fixture fixture;

    owner_fixture_init(&fixture, OWNER_TEST_GENERATION, 0u);
    fixture.wait_step_us = 0u;
    owner_reserve_empty_request(&fixture, OWNER_TEST_REQUEST_ID);
    CHECK(!p4_hosted_owner_exchange(&fixture.owner, &fixture.fifo,
        &fixture.owner_ops, fixture.generation, 0u, fixture.request,
        fixture.request_size, P4_HOSTED_OWNER_MAX_EXCHANGE_US,
        &fixture.result));
    CHECK(fixture.result.error == P4_HOSTED_OWNER_TIMEOUT &&
          fixture.result.elapsed_us == 0u);
    CHECK(fixture.acquire_attempts <= P4_HOSTED_OWNER_MAX_POLLS &&
          fixture.acquire_successes <= P4_HOSTED_OWNER_MAX_POLLS &&
          fixture.fake.raw_reads < P4_HOSTED_OWNER_MAX_POLLS);
    CHECK(fixture.fake.tx_fifo_writes == 1u &&
          fixture.owner.ids.next_uid == 2u &&
          fixture.owner.ids.retired == 1u);
}

static void test_set_budget_refusal_releases_and_retires(void)
{
    struct owner_fixture fixture;

    owner_fixture_init(&fixture, OWNER_TEST_GENERATION, 50u);
    owner_reserve_empty_request(&fixture, OWNER_TEST_REQUEST_ID);
    fixture.budget_fail = 1u;
    CHECK(!p4_hosted_owner_exchange(&fixture.owner, &fixture.fifo,
        &fixture.owner_ops, fixture.generation, 0u, fixture.request,
        fixture.request_size, 500u, &fixture.result));
    CHECK(fixture.result.error == P4_HOSTED_OWNER_BUDGET);
    CHECK(fixture.acquire_successes == 1u && fixture.releases == 1u &&
          fixture.acquired == 0u);
    CHECK(fixture.fake.tx_token_reads == 0u &&
          fixture.fake.tx_fifo_writes == 0u &&
          fixture.owner.ids.retired == 1u && fixture.owner.ids.next_uid == 2u);
}

static void test_ambiguous_tx_and_rx_fail_closed(void)
{
    struct owner_fixture fixture;
    uint8_t response[TEST_FRAME_CAPACITY];
    uint32_t raw[] = { P4_HOSTED_OPEN_INT_PACKET };
    size_t response_size;

    owner_fixture_init(&fixture, OWNER_TEST_GENERATION, 100u);
    owner_reserve_empty_request(&fixture, OWNER_TEST_REQUEST_ID);
    fixture.fake.tx_fifo_return = 0;
    CHECK(!p4_hosted_owner_exchange(&fixture.owner, &fixture.fifo,
        &fixture.owner_ops, fixture.generation, 0u, fixture.request,
        fixture.request_size, 1000u, &fixture.result));
    CHECK(fixture.result.submitted == 0u && fixture.fake.tx_fifo_writes == 1u &&
          fixture.fake.rx_fifo_reads == 0u && fixture.fifo.poisoned == 1u &&
          fixture.owner.ids.retired == 1u);

    owner_fixture_init(&fixture, OWNER_TEST_GENERATION, 200u);
    owner_reserve_empty_request(&fixture, OWNER_TEST_REQUEST_ID);
    response_size = make_serial_rpc_frame(response, sizeof response,
        HOSTED_SERIAL_ENDPOINT_RPCRSP, P4_HOSTED_RPC_TYPE_RESPONSE,
        OWNER_TEST_REQUEST_ID + 256u, 1u, NULL, 0u, 0u,
        P4_HOSTED_SERIAL, 0u, 0u, 0u);
    CHECK(response_size != 0u && add_batch(&fixture.fake, response,
                                           response_size));
    set_raws(&fixture.fake, raw, 1u);
    fixture.fake.fifo_return = 0;
    CHECK(!p4_hosted_owner_exchange(&fixture.owner, &fixture.fifo,
        &fixture.owner_ops, fixture.generation, 0u, fixture.request,
        fixture.request_size, 1000u, &fixture.result));
    CHECK(fixture.result.error == P4_HOSTED_OWNER_RX &&
          fixture.result.matched == 0u && fixture.fake.tx_fifo_writes == 1u &&
          fixture.fake.rx_fifo_reads == 1u && fixture.fifo.poisoned == 1u &&
          fixture.owner.ids.retired == 1u);
}

static void test_stale_generation_is_transactional(void)
{
    struct owner_fixture fixture;
    struct p4_hosted_owner owner_before;
    struct p4_hosted_fifo fifo_before;
    struct p4_hosted_owner_result result_before;

    owner_fixture_init(&fixture, OWNER_TEST_GENERATION, 100u);
    memset(&fixture.result, 0x6du, sizeof fixture.result);
    owner_before = fixture.owner;
    fifo_before = fixture.fifo;
    result_before = fixture.result;
    CHECK(!p4_hosted_owner_step(&fixture.owner, &fixture.fifo,
        &fixture.owner_ops, OWNER_NEXT_GENERATION, 0u, 1000u,
        &fixture.result));
    CHECK(memcmp(&fixture.owner, &owner_before, sizeof owner_before) == 0 &&
          memcmp(&fixture.fifo, &fifo_before, sizeof fifo_before) == 0 &&
          memcmp(&fixture.result, &result_before, sizeof result_before) == 0);
    CHECK(fixture.acquire_attempts == 0u && fixture.waits == 0u);
}

static void test_missing_frame_dispatcher_retires_outside_host(void)
{
    uint8_t ethernet[14] = {
        0x02u, 0x00u, 0x00u, 0x00u, 0x00u, 0x01u,
        0x02u, 0x00u, 0x00u, 0x00u, 0x00u, 0x02u,
        0x08u, 0x00u
    };
    struct owner_fixture fixture;
    uint8_t batch[TEST_FRAME_CAPACITY];
    uint32_t raw[] = { P4_HOSTED_OPEN_INT_PACKET };
    size_t frame_size;

    owner_fixture_init(&fixture, OWNER_TEST_GENERATION, 100u);
    frame_size = make_frame(batch, sizeof batch, P4_HOSTED_STA, 0u, 0u, 0u,
        0u, ethernet, sizeof ethernet);
    CHECK(frame_size != 0u && add_batch(&fixture.fake, batch, frame_size));
    set_raws(&fixture.fake, raw, 1u);
    fixture.owner_ops.dispatch_frame = NULL;
    CHECK(!p4_hosted_owner_step(&fixture.owner, &fixture.fifo,
        &fixture.owner_ops, fixture.generation, 0u, 1000u,
        &fixture.result));
    CHECK(fixture.result.error == P4_HOSTED_OWNER_DISPATCH &&
          fixture.frame_calls == 0u && fixture.dispatch_under_lock == 0u);
    CHECK(fixture.acquired == 0u && fixture.releases == 1u &&
          fixture.owner.poisoned == 1u && fixture.fifo.poisoned == 1u);
}

static void test_step_drains_owned_event_without_tx_or_host_lock(void)
{
    static const uint8_t body[] = { 0xaau };
    struct owner_fixture fixture;
    uint8_t batch[TEST_CAPACITY];
    uint32_t raw[] = { P4_HOSTED_OPEN_INT_PACKET };
    size_t used = 0u;

    owner_fixture_init(&fixture, OWNER_TEST_GENERATION, 1000u);
    CHECK(append_owner_rpc(batch, sizeof batch, &used,
        HOSTED_SERIAL_ENDPOINT_RPCEVT, P4_HOSTED_RPC_TYPE_EVENT, 770u, 0u,
        body, sizeof body) != 0u);
    CHECK(add_batch(&fixture.fake, batch, used));
    set_raws(&fixture.fake, raw, 1u);

    CHECK(p4_hosted_owner_step(&fixture.owner, &fixture.fifo,
        &fixture.owner_ops, fixture.generation, 0u, 1000u,
        &fixture.result));
    CHECK(fixture.result.error == P4_HOSTED_OWNER_OK &&
          fixture.result.events == 1u && fixture.result.batches == 1u &&
          fixture.result.idle == 0u);
    CHECK(fixture.event_calls == 1u && fixture.dispatch_under_lock == 0u);
    CHECK(fixture.fake.tx_token_reads == 0u &&
          fixture.fake.tx_fifo_writes == 0u && fixture.fifo.tx_sent == 0u);
    CHECK(fixture.fifo.rx_consumed == (uint32_t)used &&
          fixture.fifo.poisoned == 0u);
    CHECK(fixture.acquire_successes == 1u && fixture.releases == 1u &&
          fixture.acquired == 0u);

    CHECK(p4_hosted_owner_step(&fixture.owner, &fixture.fifo,
        &fixture.owner_ops, fixture.generation, 0u, 1000u,
        &fixture.result));
    CHECK(fixture.result.error == P4_HOSTED_OWNER_OK &&
          fixture.result.idle == 1u && fixture.result.events == 0u);
    CHECK(fixture.fake.tx_fifo_writes == 0u && fixture.fifo.tx_sent == 0u);
}

static void test_ids_init_preserves_open_handoff_event(void)
{
    struct owner_fixture fixture;
    struct p4_hosted_rpc rpc = {0};
    static const uint8_t payload[] = { 0x5au };

    owner_fixture_prepare(&fixture, OWNER_TEST_GENERATION, 0x1234u);
    rpc.msg_type = P4_HOSTED_RPC_TYPE_EVENT;
    rpc.msg_id = 769u;
    rpc.uid = 0u;
    rpc.payload = payload;
    rpc.payload_size = sizeof payload;
    CHECK(p4_hosted_inbox_push(&fixture.owner.rx.inbox,
        fixture.generation, &rpc));

    /* IDs binding must not reset the already-consumed OPEN suffix. */
    CHECK(p4_hosted_owner_ids_init(&fixture.owner, fixture.generation,
                                    fixture.now));
    CHECK(fixture.owner.rx.inbox.count == 1u &&
          fixture.owner.rx.inbox.items[fixture.owner.rx.inbox.head].uid == 0u);

    CHECK(p4_hosted_owner_step(&fixture.owner, &fixture.fifo,
        &fixture.owner_ops, fixture.generation, 0u, 1000u,
        &fixture.result));
    CHECK(fixture.result.events == 1u && fixture.event_calls == 1u);
    CHECK(fixture.acquire_attempts_at_dispatch == 0u &&
          fixture.dispatch_under_lock == 0u);
    CHECK(fixture.dispatch_under_lock == 0u &&
          fixture.last_event.msg_id == 769u);
}

static void test_cancel_closes_uid_without_bus_or_reuse(void)
{
    struct owner_fixture fixture;
    uint32_t uid = 0u;
    uint32_t next_uid = 0u;

    owner_fixture_init(&fixture, OWNER_TEST_GENERATION, 500u);
    CHECK(p4_hosted_request_ids_reserve(&fixture.owner.ids,
        fixture.generation, OWNER_TEST_REQUEST_ID, &uid));
    CHECK(uid == 1u);
    memset(fixture.owner.request, 0xa5, sizeof fixture.owner.request);
    memset(fixture.owner.wire, 0xa5, sizeof fixture.owner.wire);
    memset(&fixture.owner.reply, 0xa5, sizeof fixture.owner.reply);
    CHECK(p4_hosted_owner_cancel(&fixture.owner, &fixture.fifo,
                                  fixture.generation));
    CHECK(fixture.acquire_attempts == 0u && fixture.acquire_successes == 0u &&
          fixture.releases == 0u && fixture.fake.rx_fifo_reads == 0u &&
          fixture.fake.tx_fifo_writes == 0u);
    CHECK(fixture.owner.poisoned == 1u && fixture.owner.rx.poisoned == 1u &&
          fixture.fifo.poisoned == 1u);
    CHECK(fixture.owner.ids.retired == 1u &&
          fixture.owner.ids.active_uid == 0u &&
          fixture.owner.ids.next_uid == 2u);
    CHECK(memcmp(fixture.owner.request,
          (const uint8_t[P4_HOSTED_PENDING_MAX_REQUEST_SIZE]){0},
          sizeof fixture.owner.request) == 0);
    CHECK(memcmp(fixture.owner.wire,
          (const uint8_t[P4_HOSTED_PENDING_BUFFER_MAX]){0},
          sizeof fixture.owner.wire) == 0);
    CHECK(memcmp(&fixture.owner.reply,
          &(const struct p4_hosted_inbox_item){0},
          sizeof fixture.owner.reply) == 0);

    CHECK(p4_hosted_request_ids_init(&fixture.owner.ids,
                                     OWNER_NEXT_GENERATION));
    CHECK(p4_hosted_request_ids_reserve(&fixture.owner.ids,
        OWNER_NEXT_GENERATION, OWNER_TEST_REQUEST_ID, &next_uid));
    CHECK(next_uid == 2u);
}

int main(void)
{
    test_exchange_dispatches_after_release_across_clock_wrap();
    test_exchange_deadline_is_not_renewed_and_uid_is_consumed();
    test_preserved_matching_response_is_unmatched_until_tx();
    test_poll_callback_past_deadline_cannot_match();
    test_dispatch_past_deadline_cannot_return_match_or_idle_success();
    test_new_partial_after_completion_gets_new_deadline();
    test_open_partial_age_is_preserved();
    test_invalid_fifo_direction_refuses_without_mutation();
    test_busy_host_timeout_uses_one_deadline();
    test_stopped_clock_uses_poll_cap();
    test_set_budget_refusal_releases_and_retires();
    test_ambiguous_tx_and_rx_fail_closed();
    test_stale_generation_is_transactional();
    test_missing_frame_dispatcher_retires_outside_host();
    test_step_drains_owned_event_without_tx_or_host_lock();
    test_ids_init_preserves_open_handoff_event();
    test_cancel_closes_uid_without_bus_or_reuse();
    printf("hosted shared-host owner: %lu checks passed\n", checks);
    return 0;
}
