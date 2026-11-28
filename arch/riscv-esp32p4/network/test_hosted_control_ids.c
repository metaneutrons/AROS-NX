/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
/* Reuse the real-control fake transport fixture. No wall-clock or hardware
 * behavior is measured by this test. */
#define main hosted_control_legacy_test_main
#include "test_hosted_control.c"
#undef main

#include "hosted_request_ids.h"

#define IDS_TEST_GENERATION UINT32_C(0x5721)
#define IDS_REPLACEMENT_GENERATION UINT32_C(0x5722)
static const uint8_t ids_request_body[] = { 0x08u, 0x01u, 0x10u, 0x02u };

static void prepare_ids(struct p4_hosted_request_ids *ids,
                        uint32_t generation, uint32_t request_id,
                        uint32_t *uid)
{
    memset(ids, 0, sizeof *ids);
    CHECK(p4_hosted_request_ids_init(ids, generation));
    CHECK(p4_hosted_request_ids_reserve(ids, generation, request_id, uid));
}

static void fixture_init_for_ids(struct fixture *fixture)
{
    fixture_init(fixture);
    CHECK(p4_hosted_fifo_start(&fixture->state, IDS_TEST_GENERATION,
                               TEST_POOL_LIMIT));
    fixture->fake.generation = IDS_TEST_GENERATION;
}

static size_t make_ids_request(uint8_t *bytes, size_t capacity, uint32_t uid,
                               uint32_t request_id)
{
    return make_request(bytes, capacity, uid, request_id, ids_request_body,
                        sizeof ids_request_body);
}

static size_t add_ids_response(struct fixture *fixture, uint32_t uid,
                               uint32_t request_id)
{
    static const uint8_t body[] = { 0xdeu, 0xadu, 0x00u, 0xbeu };
    uint8_t frame[TEST_FRAME_CAPACITY];
    size_t size = make_serial_rpc_frame(frame, sizeof frame,
        HOSTED_SERIAL_ENDPOINT_RPCRSP, P4_HOSTED_RPC_TYPE_RESPONSE,
        request_id + 256u, uid, body, sizeof body, 0u,
        P4_HOSTED_SERIAL, 0u, 0u, 0u);
    CHECK(size != 0u);
    CHECK(add_batch(&fixture->fake, frame, size));
    return size;
}

static int call_ids_exchange(struct fixture *fixture,
                             struct p4_hosted_request_ids *ids,
                             uint32_t generation, const uint8_t *request,
                             size_t request_size, uint8_t *buffer,
                             struct p4_hosted_control_result *result)
{
    return p4_hosted_control_exchange_reserved(&fixture->state, ids,
        generation, &fixture->rx_ops, &fixture->tx_ops, 0u, request,
        request_size, buffer, TEST_CAPACITY, result);
}

static void expect_ids_preflight_unchanged(
    struct fixture *fixture, struct p4_hosted_request_ids *ids,
    uint32_t generation, const uint8_t *request, size_t request_size,
    uint8_t *buffer)
{
    struct p4_hosted_fifo state_before;
    struct p4_hosted_request_ids ids_before;
    struct p4_hosted_control_result result_before;
    uint8_t buffer_before[TEST_CAPACITY];

    memcpy(&state_before, &fixture->state, sizeof state_before);
    memcpy(&ids_before, ids, sizeof ids_before);
    memcpy(&result_before, &fixture->result, sizeof result_before);
    memcpy(buffer_before, fixture->buffer, sizeof buffer_before);
    CHECK(p4_hosted_control_exchange_reserved(&fixture->state, ids,
        generation, &fixture->rx_ops, &fixture->tx_ops, 0u, request,
        request_size, buffer, TEST_CAPACITY, &fixture->result) == 0);
    CHECK(memcmp(&state_before, &fixture->state, sizeof state_before) == 0);
    CHECK(memcmp(&ids_before, ids, sizeof ids_before) == 0);
    CHECK(memcmp(&result_before, &fixture->result,
                 sizeof result_before) == 0);
    CHECK(memcmp(buffer_before, fixture->buffer, sizeof buffer_before) == 0);
    no_callbacks(&fixture->fake);
}

static void test_ids_success_and_sequential_uid(void)
{
    struct fixture fixture;
    struct p4_hosted_request_ids ids;
    static const uint32_t raw[] = {
        P4_HOSTED_OPEN_INT_PACKET, P4_HOSTED_OPEN_INT_PACKET
    };
    uint8_t request_1[P4_HOSTED_CONTROL_MAX_RPC_SIZE];
    uint8_t request_2[P4_HOSTED_CONTROL_MAX_RPC_SIZE];
    uint32_t uid_1, uid_2;
    size_t request_size_1, request_size_2, response_size_1;

    fixture_init_for_ids(&fixture);
    prepare_ids(&ids, IDS_TEST_GENERATION, 257u, &uid_1);
    CHECK(uid_1 == 1u);
    request_size_1 = make_ids_request(request_1, sizeof request_1,
                                      uid_1, 257u);
    response_size_1 = add_ids_response(&fixture, uid_1, 257u);
    CHECK(request_size_1 != 0u);
    set_raws(&fixture.fake, raw, 2u);

    CHECK(call_ids_exchange(&fixture, &ids, IDS_TEST_GENERATION,
        request_1, request_size_1, fixture.buffer, &fixture.result) == 1);
    CHECK(fixture.result.success == 1u);
    CHECK(fixture.result.error == P4_HOSTED_CONTROL_OK);
    CHECK(fixture.result.matched == 1u && fixture.result.response_uid == uid_1);
    CHECK(fixture.result.response_id == 513u);
    CHECK(fixture.state.tx_sent == 1u);
    CHECK(fixture.state.rx_consumed == (uint32_t)response_size_1);
    CHECK(fixture.state.poisoned == 0u && fixture.state.busy == 0u);
    CHECK(ids.initialized == 1u && ids.retired == 0u);
    CHECK(ids.generation == IDS_TEST_GENERATION && ids.next_uid == 2u);
    CHECK(ids.active_uid == 0u && ids.active_request_id == 0u);
    verify_exchange_request(&fixture, request_1, request_size_1, uid_1,
        257u, ids_request_body, sizeof ids_request_body, 1u);

    CHECK(p4_hosted_request_ids_reserve(&ids, IDS_TEST_GENERATION,
                                        260u, &uid_2));
    CHECK(uid_2 == 2u);
    request_size_2 = make_ids_request(request_2, sizeof request_2,
                                      uid_2, 260u);
    CHECK(request_size_2 != 0u);
    CHECK(add_ids_response(&fixture, uid_2, 260u) != 0u);
    fixture.fake.token = UINT32_C(2) << 16;

    CHECK(call_ids_exchange(&fixture, &ids, IDS_TEST_GENERATION,
        request_2, request_size_2, fixture.buffer, &fixture.result) == 1);
    CHECK(fixture.result.success == 1u && fixture.result.response_uid == uid_2);
    CHECK(fixture.result.response_id == 516u);
    CHECK(fixture.state.tx_sent == 2u && fixture.state.poisoned == 0u);
    CHECK(ids.generation == IDS_TEST_GENERATION && ids.next_uid == 3u);
    CHECK(ids.active_uid == 0u && ids.active_request_id == 0u);
    verify_exchange_request(&fixture, request_2, request_size_2, uid_2,
        260u, ids_request_body, sizeof ids_request_body, 2u);
}

static void test_ids_wrapper_preflight_refusals(void)
{
    struct fixture fixture;
    struct p4_hosted_request_ids ids;
    uint8_t request[P4_HOSTED_CONTROL_MAX_RPC_SIZE];
    uint8_t mismatch[P4_HOSTED_CONTROL_MAX_RPC_SIZE];
    uint32_t uid;
    size_t request_size, mismatch_size;

    fixture_init_for_ids(&fixture);
    prepare_ids(&ids, IDS_TEST_GENERATION, 257u, &uid);
    request_size = make_ids_request(request, sizeof request, uid, 257u);
    CHECK(request_size != 0u);

    mismatch_size = make_ids_request(mismatch, sizeof mismatch, uid, 260u);
    CHECK(mismatch_size != 0u);
    expect_ids_preflight_unchanged(&fixture, &ids, IDS_TEST_GENERATION,
        mismatch, mismatch_size, fixture.buffer);

    mismatch_size = make_ids_request(mismatch, sizeof mismatch, uid + 1u, 257u);
    CHECK(mismatch_size != 0u);
    expect_ids_preflight_unchanged(&fixture, &ids, IDS_TEST_GENERATION,
        mismatch, mismatch_size, fixture.buffer);

    expect_ids_preflight_unchanged(&fixture, &ids,
        IDS_TEST_GENERATION + 1u, request, request_size, fixture.buffer);

    /* The wrapper must reject aliasing before it decodes the fake bytes that
     * happen to occupy the caller's live identity object. */
    expect_ids_preflight_unchanged(&fixture, &ids, IDS_TEST_GENERATION,
        (const uint8_t *)&ids, sizeof ids, fixture.buffer);

    expect_ids_preflight_unchanged(&fixture, &ids, IDS_TEST_GENERATION,
        (const uint8_t *)(uintptr_t)(UINTPTR_MAX - 3u), 8u, fixture.buffer);

    CHECK(p4_hosted_request_ids_finish(&ids, IDS_TEST_GENERATION,
                                      uid, 257u));
    expect_ids_preflight_unchanged(&fixture, &ids, IDS_TEST_GENERATION,
        request, request_size, fixture.buffer);
}

static void test_ids_accepted_lower_preflight_consumes_reservation(void)
{
    struct fixture fixture;
    struct p4_hosted_request_ids ids;
    struct p4_hosted_fifo state_before;
    struct p4_hosted_control_result result_before;
    uint8_t buffer_before[TEST_CAPACITY];
    uint8_t request[P4_HOSTED_CONTROL_MAX_RPC_SIZE];
    uint32_t uid;
    size_t request_size;

    fixture_init_for_ids(&fixture);
    prepare_ids(&ids, IDS_TEST_GENERATION, 257u, &uid);
    request_size = make_ids_request(request, sizeof request, uid, 257u);
    CHECK(request_size != 0u);
    /* Wrapper identity preflight accepts this call; the legacy exchange's
     * operational preflight rejects its missing callback before I/O. */
    fixture.tx_ops.yield = NULL;
    memcpy(&state_before, &fixture.state, sizeof state_before);
    memcpy(&result_before, &fixture.result, sizeof result_before);
    memcpy(buffer_before, fixture.buffer, sizeof buffer_before);

    CHECK(call_ids_exchange(&fixture, &ids, IDS_TEST_GENERATION,
        request, request_size, fixture.buffer, &fixture.result) == 0);
    CHECK(memcmp(&state_before, &fixture.state, sizeof state_before) == 0);
    CHECK(memcmp(&result_before, &fixture.result,
                 sizeof result_before) == 0);
    CHECK(memcmp(buffer_before, fixture.buffer, sizeof buffer_before) == 0);
    CHECK(ids.generation == IDS_TEST_GENERATION && ids.retired == 0u);
    CHECK(ids.next_uid == 2u && ids.active_uid == 0u);
    CHECK(ids.active_request_id == 0u);
    no_callbacks(&fixture.fake);
}

static void test_ids_timeout_consumes_uid_and_allows_next(void)
{
    struct fixture fixture;
    struct p4_hosted_request_ids ids;
    uint8_t request_1[P4_HOSTED_CONTROL_MAX_RPC_SIZE];
    uint8_t request_2[P4_HOSTED_CONTROL_MAX_RPC_SIZE];
    static const uint32_t no_raws[] = { 0u };
    uint32_t uid_1, uid_2;
    size_t request_size_1, request_size_2, response_size;

    fixture_init_for_ids(&fixture);
    prepare_ids(&ids, IDS_TEST_GENERATION, 279u, &uid_1);
    request_size_1 = make_ids_request(request_1, sizeof request_1,
                                      uid_1, 279u);
    CHECK(request_size_1 != 0u);
    set_raws(&fixture.fake, no_raws, 1u);
    CHECK(call_ids_exchange(&fixture, &ids, IDS_TEST_GENERATION,
        request_1, request_size_1, fixture.buffer, &fixture.result) == 0);
    CHECK(fixture.result.error == P4_HOSTED_CONTROL_TIMEOUT);
    CHECK(fixture.result.polls == P4_HOSTED_CONTROL_MAX_POLLS);
    CHECK(fixture.fake.tx_fifo_writes == 1u &&
          fixture.fake.tx_token_reads == 1u);
    CHECK(fixture.state.tx_sent == 1u && fixture.state.rx_consumed == 0u);
    CHECK(fixture.state.poisoned == 0u);
    CHECK(ids.generation == IDS_TEST_GENERATION && ids.retired == 0u);
    CHECK(ids.next_uid == 2u && ids.active_uid == 0u);
    CHECK(ids.active_request_id == 0u);

    CHECK(p4_hosted_request_ids_reserve(&ids, IDS_TEST_GENERATION,
                                        279u, &uid_2));
    CHECK(uid_2 == 2u);
    request_size_2 = make_ids_request(request_2, sizeof request_2,
                                      uid_2, 279u);
    CHECK(request_size_2 != 0u);
    response_size = add_ids_response(&fixture, uid_2, 279u);
    fixture.fake.raw_default = P4_HOSTED_OPEN_INT_PACKET;
    fixture.fake.token = UINT32_C(2) << 16;
    CHECK(call_ids_exchange(&fixture, &ids, IDS_TEST_GENERATION,
        request_2, request_size_2, fixture.buffer, &fixture.result) == 1);
    CHECK(fixture.result.response_uid == uid_2);
    CHECK(fixture.state.tx_sent == 2u);
    CHECK(fixture.state.rx_consumed == (uint32_t)response_size);
    CHECK(ids.next_uid == 3u && ids.active_uid == 0u);
}

static void test_ids_rx_failure_retires_only_old_epoch(void)
{
    struct fixture fixture;
    struct p4_hosted_request_ids ids;
    uint8_t request[P4_HOSTED_CONTROL_MAX_RPC_SIZE];
    uint32_t uid;
    size_t request_size;
    static const uint32_t raw[] = { P4_HOSTED_OPEN_INT_PACKET };

    fixture_init_for_ids(&fixture);
    prepare_ids(&ids, IDS_TEST_GENERATION, 257u, &uid);
    request_size = make_ids_request(request, sizeof request, uid, 257u);
    CHECK(request_size != 0u);
    CHECK(add_ids_response(&fixture, uid, 257u) != 0u);
    set_raws(&fixture.fake, raw, 1u);
    fixture.fake.fail_fifo_read_number = 1u;

    CHECK(call_ids_exchange(&fixture, &ids, IDS_TEST_GENERATION,
        request, request_size, fixture.buffer, &fixture.result) == 0);
    CHECK(fixture.result.error == P4_HOSTED_CONTROL_RX);
    CHECK(fixture.state.poisoned == 1u && fixture.state.busy == 0u);
    CHECK(fixture.state.tx_sent == 1u && fixture.state.rx_consumed == 0u);
    CHECK(ids.generation == IDS_TEST_GENERATION && ids.retired == 1u);
    CHECK(ids.next_uid == 2u && ids.active_uid == 0u);
    CHECK(ids.active_request_id == 0u);
    CHECK(p4_hosted_request_ids_init(&ids, IDS_REPLACEMENT_GENERATION));
    CHECK(ids.generation == IDS_REPLACEMENT_GENERATION);
    CHECK(ids.retired == 0u && ids.next_uid == 2u);
}

struct ids_callback_context {
    struct fake_host *fake;
    struct p4_hosted_request_ids *ids;
    uint32_t old_generation;
    uint32_t new_generation;
    unsigned replace_ids_on_fifo;
    unsigned replaced;
    struct p4_hosted_request_ids ids_after;
    struct p4_hosted_fifo fifo_after;
    unsigned captured_fifo;
};

static void replace_ids_from_old_callback(struct ids_callback_context *context)
{
    if (!context->replace_ids_on_fifo || context->replaced)
        return;
    CHECK(p4_hosted_request_ids_invalidate(context->ids,
                                           context->old_generation));
    CHECK(p4_hosted_request_ids_init(context->ids,
                                     context->new_generation));
    memcpy(&context->ids_after, context->ids, sizeof context->ids_after);
    context->replaced = 1u;
}

static int ids_proxy_rx_read_word(void *opaque, uint32_t address,
                                  uint32_t *value)
{
    struct ids_callback_context *context = opaque;
    return fake_rx_read_word(context->fake, address, value);
}

static int ids_proxy_tx_read_word(void *opaque, uint32_t address,
                                  uint32_t *value)
{
    struct ids_callback_context *context = opaque;
    return fake_tx_read_word(context->fake, address, value);
}

static int ids_proxy_write_word(void *opaque, uint32_t address, uint32_t value)
{
    struct ids_callback_context *context = opaque;
    return fake_write_word(context->fake, address, value);
}

static int ids_proxy_read_fifo(void *opaque,
                               const struct p4_hosted_fifo_plan *plan,
                               uint8_t *buffer)
{
    struct ids_callback_context *context = opaque;
    int result = fake_read_fifo(context->fake, plan, buffer);
    replace_ids_from_old_callback(context);
    if (context->captured_fifo == 0u) {
        memcpy(&context->fifo_after, context->fake->state,
               sizeof context->fifo_after);
        context->captured_fifo = 1u;
    }
    return result;
}

static int ids_proxy_write_fifo(void *opaque,
                                const struct p4_hosted_fifo_plan *plan,
                                const uint8_t *buffer,
                                struct p4_hosted_tx_completion *completion)
{
    struct ids_callback_context *context = opaque;
    return fake_write_fifo(context->fake, plan, buffer, completion);
}

static int ids_proxy_session_ok(void *opaque, uint32_t generation)
{
    struct ids_callback_context *context = opaque;
    return fake_session_ok(context->fake, generation);
}

static int ids_proxy_budget_ok(void *opaque)
{
    struct ids_callback_context *context = opaque;
    return fake_budget_ok(context->fake);
}

static void ids_proxy_yield(void *opaque)
{
    struct ids_callback_context *context = opaque;
    fake_yield(context->fake);
}

static void install_ids_callback_proxy(struct fixture *fixture,
                                       struct ids_callback_context *context,
                                       struct p4_hosted_request_ids *ids,
                                       unsigned replace_fifo_state)
{
    memset(context, 0, sizeof *context);
    context->fake = &fixture->fake;
    context->ids = ids;
    context->old_generation = IDS_TEST_GENERATION;
    context->new_generation = IDS_REPLACEMENT_GENERATION;
    context->replace_ids_on_fifo = 1u;
    if (replace_fifo_state) {
        fixture->fake.replace_generation_after_fifo = 1u;
        fixture->fake.replacement_generation = IDS_REPLACEMENT_GENERATION;
    }

    fixture->rx_ops.context = context;
    fixture->rx_ops.read_word = ids_proxy_rx_read_word;
    fixture->rx_ops.write_word = ids_proxy_write_word;
    fixture->rx_ops.read_fifo = ids_proxy_read_fifo;
    fixture->rx_ops.yield = ids_proxy_yield;
    fixture->tx_ops.context = context;
    fixture->tx_ops.read_word = ids_proxy_tx_read_word;
    fixture->tx_ops.write_fifo = ids_proxy_write_fifo;
    fixture->tx_ops.session_ok = ids_proxy_session_ok;
    fixture->tx_ops.budget_ok = ids_proxy_budget_ok;
    fixture->tx_ops.yield = ids_proxy_yield;
}

static void test_ids_replacement_while_fifo_remains_old(void)
{
    struct fixture fixture;
    struct p4_hosted_request_ids ids;
    struct ids_callback_context context;
    uint8_t request[P4_HOSTED_CONTROL_MAX_RPC_SIZE];
    uint32_t uid;
    size_t request_size, response_size;
    static const uint32_t raw[] = { P4_HOSTED_OPEN_INT_PACKET };

    fixture_init_for_ids(&fixture);
    prepare_ids(&ids, IDS_TEST_GENERATION, 257u, &uid);
    request_size = make_ids_request(request, sizeof request, uid, 257u);
    CHECK(request_size != 0u);
    response_size = add_ids_response(&fixture, uid, 257u);
    set_raws(&fixture.fake, raw, 1u);
    install_ids_callback_proxy(&fixture, &context, &ids, 0u);

    /* The callback replaces only the UID ledger; FIFO work completes and
     * produces a valid old response. The wrapper must still refuse success. */
    CHECK(p4_hosted_control_exchange_reserved(&fixture.state, &ids,
        IDS_TEST_GENERATION, &fixture.rx_ops, &fixture.tx_ops, 0u,
        request, request_size, fixture.buffer, sizeof fixture.buffer,
        &fixture.result) == 0);
    CHECK(context.replaced == 1u && context.captured_fifo == 1u);
    CHECK(memcmp(&context.ids_after, &ids, sizeof ids) == 0);
    CHECK(ids.generation == IDS_REPLACEMENT_GENERATION);
    CHECK(ids.retired == 0u && ids.next_uid == 2u);
    CHECK(ids.active_uid == 0u && ids.active_request_id == 0u);
    CHECK(fixture.state.generation == IDS_TEST_GENERATION);
    CHECK(fixture.state.tx_sent == 1u);
    CHECK(fixture.state.rx_consumed == (uint32_t)response_size);
    CHECK(fixture.state.poisoned == 0u && fixture.state.busy == 0u);
    CHECK(fixture.result.matched == 1u);
    CHECK(fixture.result.success == 0u);
    CHECK(fixture.result.error == P4_HOSTED_CONTROL_SESSION);
}

static void test_ids_replacement_preserves_replacement_fifo(void)
{
    struct fixture fixture;
    struct p4_hosted_request_ids ids;
    struct ids_callback_context context;
    uint8_t request[P4_HOSTED_CONTROL_MAX_RPC_SIZE];
    uint32_t uid;
    size_t request_size;
    static const uint32_t raw[] = { P4_HOSTED_OPEN_INT_PACKET };

    fixture_init_for_ids(&fixture);
    prepare_ids(&ids, IDS_TEST_GENERATION, 257u, &uid);
    request_size = make_ids_request(request, sizeof request, uid, 257u);
    CHECK(request_size != 0u);
    CHECK(add_ids_response(&fixture, uid, 257u) != 0u);
    set_raws(&fixture.fake, raw, 1u);
    install_ids_callback_proxy(&fixture, &context, &ids, 1u);

    CHECK(p4_hosted_control_exchange_reserved(&fixture.state, &ids,
        IDS_TEST_GENERATION, &fixture.rx_ops, &fixture.tx_ops, 0u,
        request, request_size, fixture.buffer, sizeof fixture.buffer,
        &fixture.result) == 0);
    CHECK(context.replaced == 1u && context.captured_fifo == 1u);
    CHECK(memcmp(&context.ids_after, &ids, sizeof ids) == 0);
    CHECK(memcmp(&context.fifo_after, &fixture.state,
                 sizeof fixture.state) == 0);
    CHECK(ids.generation == IDS_REPLACEMENT_GENERATION);
    CHECK(ids.retired == 0u && ids.next_uid == 2u);
    CHECK(ids.active_uid == 0u && ids.active_request_id == 0u);
    CHECK(fixture.state.generation == IDS_REPLACEMENT_GENERATION);
    CHECK(fixture.state.initialized == 1u && fixture.state.poisoned == 0u);
    CHECK(fixture.state.tx_sent == 0u && fixture.state.rx_consumed == 0u);
    CHECK(fixture.state.busy == 0u);
    CHECK(fixture.result.error == P4_HOSTED_CONTROL_SESSION);
    CHECK(fixture.result.success == 0u);
}

int main(void)
{
    test_ids_success_and_sequential_uid();
    test_ids_wrapper_preflight_refusals();
    test_ids_accepted_lower_preflight_consumes_reservation();
    test_ids_timeout_consumes_uid_and_allows_next();
    test_ids_rx_failure_retires_only_old_epoch();
    test_ids_replacement_while_fifo_remains_old();
    test_ids_replacement_preserves_replacement_fifo();
    printf("hosted_control_ids: %lu checks passed\n", checks);
    return 0;
}
