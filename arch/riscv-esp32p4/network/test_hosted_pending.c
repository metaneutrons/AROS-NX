/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
/* Host-only model for one asynchronous Hosted TX submission and its later
 * response correlation. No RX callback, hardware or elapsed-time claim. */
#include "hosted_association.h"
#include "hosted_pending.h"
#include "hosted_rpc.h"
#include "hosted_serial.h"
#include "hosted_wire.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_GENERATION UINT32_C(0x5bd1)
#define REPLACEMENT_GENERATION UINT32_C(0x5bd2)
#define TEST_POOL_LIMIT 20u
#define TEST_REQUEST_ID 282u
#define TEST_AP_INFO_REQUEST_ID 294u
#define TEST_CAPACITY P4_HOSTED_PENDING_BUFFER_MAX
#define TEST_FRAME_CAPACITY P4_HOSTED_FRAME_MAX

static unsigned long checks;

#define CHECK(expression) do { \
    ++checks; \
    if (!(expression)) { \
        fprintf(stderr, "check failed at %s:%d: %s\n", __FILE__, __LINE__, \
                #expression); \
        exit(1); \
    } \
} while (0)

struct fake_tx {
    struct p4_hosted_fifo *fifo;
    struct p4_hosted_request_ids *ids;
    uint32_t generation;
    uint32_t token;
    int token_result;
    int write_result;
    int session_result;
    int budget_result;
    unsigned budget_fail_at;
    unsigned token_reads;
    unsigned other_reads;
    unsigned write_calls;
    unsigned session_calls;
    unsigned budget_calls;
    unsigned yield_calls;
    unsigned bad_address;
    unsigned replace_epoch_on_write;
    unsigned replace_ids_on_write;
    struct p4_hosted_fifo replacement_fifo;
    struct p4_hosted_request_ids replacement_ids;
    struct p4_hosted_tx_completion completion_override;
    unsigned completion_overridden;
    struct p4_hosted_fifo_plan plan;
    uint8_t captured[TEST_FRAME_CAPACITY];
    size_t captured_size;
    unsigned captured_padding_zero;
};

struct fixture {
    struct p4_hosted_fifo fifo;
    struct p4_hosted_request_ids ids;
    struct p4_hosted_tx_ops ops;
    struct fake_tx fake;
    uint8_t request[P4_HOSTED_PENDING_MAX_REQUEST_SIZE];
    size_t request_size;
    uint8_t txbuffer[TEST_CAPACITY];
    struct p4_hosted_pending_result result;
};

static size_t encode_request(uint32_t request_id, uint32_t uid,
                             uint8_t *bytes, size_t capacity)
{
    size_t written = 0u;
    struct p4_hosted_rpc rpc = {0};

    if (request_id == 282u)
        return p4_hosted_association_connect_request(uid, bytes, capacity,
                                                      &written) ? written : 0u;
    if (request_id == 283u)
        return p4_hosted_association_disconnect_request(uid, bytes, capacity,
                                                         &written) ? written : 0u;
    rpc.msg_type = P4_HOSTED_RPC_TYPE_REQUEST;
    rpc.msg_id = request_id;
    rpc.uid = uid;
    if (!p4_hosted_rpc_encode(bytes, capacity, &rpc, &written))
        return 0u;
    return written;
}

static int fake_read_word(void *context, uint32_t address, uint32_t *value)
{
    struct fake_tx *fake = context;
    if (address != P4_HOSTED_TX_TOKEN_ADDRESS || !value) {
        ++fake->other_reads;
        fake->bad_address = 1u;
        return 0;
    }
    ++fake->token_reads;
    *value = fake->token;
    return fake->token_result;
}

static int fake_write_fifo(void *context,
    const struct p4_hosted_fifo_plan *plan, const uint8_t *buffer,
    struct p4_hosted_tx_completion *completion)
{
    struct fake_tx *fake = context;
    uint32_t index;

    ++fake->write_calls;
    if (!plan || !buffer || !completion ||
        plan->logical_bytes > sizeof fake->captured ||
        plan->bus_bytes < plan->logical_bytes ||
        plan->bus_bytes > TEST_CAPACITY) {
        fake->bad_address = 1u;
        return 0;
    }
    fake->plan = *plan;
    fake->captured_size = plan->logical_bytes;
    memcpy(fake->captured, buffer, fake->captured_size);
    fake->captured_padding_zero = 1u;
    for (index = plan->logical_bytes; index < plan->bus_bytes; ++index)
        if (buffer[index] != 0u)
            fake->captured_padding_zero = 0u;

    if (fake->completion_overridden)
        *completion = fake->completion_override;
    else {
        completion->started = 1u;
        completion->success = 1u;
        completion->completed_bytes = plan->bus_bytes;
    }

    if (fake->replace_epoch_on_write) {
        *fake->fifo = fake->replacement_fifo;
        *fake->ids = fake->replacement_ids;
        fake->generation = REPLACEMENT_GENERATION;
    }
    else if (fake->replace_ids_on_write)
        *fake->ids = fake->replacement_ids;
    return fake->write_result;
}

static int fake_session_ok(void *context, uint32_t generation)
{
    struct fake_tx *fake = context;
    ++fake->session_calls;
    return fake->session_result == 1 && fake->generation == generation;
}

static int fake_budget_ok(void *context)
{
    struct fake_tx *fake = context;
    ++fake->budget_calls;
    if (fake->budget_fail_at != 0u &&
        fake->budget_calls >= fake->budget_fail_at)
        return 0;
    return fake->budget_result;
}

static void fake_yield(void *context)
{
    struct fake_tx *fake = context;
    ++fake->yield_calls;
}

static void fixture_init(struct fixture *fixture, uint32_t request_id,
                         unsigned checksum_enabled)
{
    uint32_t uid = 0u;

    memset(fixture, 0, sizeof *fixture);
    memset(fixture->txbuffer, 0xa5, sizeof fixture->txbuffer);
    memset(&fixture->result, 0xc7, sizeof fixture->result);
    CHECK(p4_hosted_fifo_start(&fixture->fifo, TEST_GENERATION,
                               TEST_POOL_LIMIT));
    CHECK(p4_hosted_request_ids_init(&fixture->ids, TEST_GENERATION));
    CHECK(p4_hosted_request_ids_reserve(&fixture->ids, TEST_GENERATION,
                                        request_id, &uid));
    fixture->request_size = encode_request(request_id, uid,
        fixture->request, sizeof fixture->request);
    CHECK(fixture->request_size != 0u);

    fixture->fake.fifo = &fixture->fifo;
    fixture->fake.ids = &fixture->ids;
    fixture->fake.generation = TEST_GENERATION;
    fixture->fake.token = UINT32_C(1) << 16;
    fixture->fake.token_result = 1;
    fixture->fake.write_result = 1;
    fixture->fake.session_result = 1;
    fixture->fake.budget_result = 1;
    fixture->ops.context = &fixture->fake;
    fixture->ops.read_word = fake_read_word;
    fixture->ops.write_fifo = fake_write_fifo;
    fixture->ops.session_ok = fake_session_ok;
    fixture->ops.budget_ok = fake_budget_ok;
    fixture->ops.yield = fake_yield;
    (void)checksum_enabled;
}

static uint32_t fixture_uid(const struct fixture *fixture)
{
    return fixture->ids.active_uid;
}

static int submit(struct fixture *fixture, uint32_t generation,
                  const struct p4_hosted_tx_ops *ops,
                  unsigned checksum_enabled, const uint8_t *request,
                  size_t request_size, uint8_t *buffer, size_t capacity)
{
    return p4_hosted_pending_submit(&fixture->fifo, &fixture->ids,
        generation, ops, checksum_enabled, request, request_size, buffer,
        capacity, &fixture->result);
}

static void verify_request_frame(const struct fake_tx *fake,
                                 uint32_t uid, uint32_t request_id,
                                 unsigned checksum_enabled)
{
    struct p4_hosted_frame frame;
    struct hosted_serial_message serial;
    struct p4_hosted_rpc rpc;

    CHECK(fake->captured_size == fake->plan.logical_bytes);
    CHECK(p4_hosted_decode(fake->captured, fake->captured_size,
                           checksum_enabled, &frame));
    CHECK(frame.interface_type == P4_HOSTED_SERIAL &&
          frame.interface_number == 0u && frame.flags == 0u &&
          frame.packet_type == 0u && frame.throttle == 0u &&
          frame.sequence == 0u);
    CHECK(hosted_serial_decode(frame.payload, frame.payload_size, &serial));
    CHECK(serial.endpoint == HOSTED_SERIAL_ENDPOINT_RPCRSP);
    CHECK(p4_hosted_rpc_decode(serial.protobuf, serial.protobuf_size, &rpc));
    CHECK(rpc.msg_type == P4_HOSTED_RPC_TYPE_REQUEST &&
          rpc.msg_id == request_id && rpc.uid == uid &&
          rpc.payload_size == 0u);
}

static void test_submit_encodes_one_frame_and_leaves_identity_pending(
    unsigned checksum_enabled)
{
    struct fixture fixture;
    uint32_t uid;

    fixture_init(&fixture, TEST_REQUEST_ID, checksum_enabled);
    uid = fixture_uid(&fixture);
    CHECK(uid == 1u && fixture.ids.active_request_id == TEST_REQUEST_ID);
    CHECK(submit(&fixture, TEST_GENERATION, &fixture.ops, checksum_enabled,
        fixture.request, fixture.request_size, fixture.txbuffer,
        sizeof fixture.txbuffer));
    CHECK(fixture.result.error == P4_HOSTED_PENDING_ERROR_NONE);
    CHECK(fixture.result.submitted == 1u && fixture.result.uid == uid &&
          fixture.result.request_id == TEST_REQUEST_ID);
    CHECK(fixture.result.request_size == fixture.request_size &&
          fixture.result.serial_size > fixture.request_size &&
          fixture.result.wire_size == fixture.result.tx.plan.logical_bytes);
    CHECK(fixture.ids.generation == TEST_GENERATION &&
          fixture.ids.active_uid == uid &&
          fixture.ids.active_request_id == TEST_REQUEST_ID &&
          fixture.ids.next_uid == 2u && fixture.ids.retired == 0u);
    CHECK(fixture.fifo.tx_sent == 1u && fixture.fifo.rx_consumed == 0u &&
          fixture.fifo.pool_limit == TEST_POOL_LIMIT &&
          !fixture.fifo.busy && !fixture.fifo.poisoned);
    CHECK(fixture.fake.token_reads == 1u && fixture.fake.other_reads == 0u &&
          fixture.fake.write_calls == 1u && fixture.fake.yield_calls == 0u &&
          !fixture.fake.bad_address && fixture.fake.captured_padding_zero);
    verify_request_frame(&fixture.fake, uid, TEST_REQUEST_ID,
                         checksum_enabled);
}

static void init_response_item(struct p4_hosted_inbox_item *item,
                               uint32_t request_id, uint32_t uid,
                               const uint8_t *payload, size_t payload_size)
{
    memset(item, 0, sizeof *item);
    item->msg_type = P4_HOSTED_RPC_TYPE_RESPONSE;
    item->msg_id = request_id + 256u;
    item->uid = uid;
    item->payload_size = payload_size;
    if (payload_size != 0u)
        memcpy(item->payload, payload, payload_size);
}

static void test_events_unmatched_and_remote_error_only_correlate(void)
{
    /* Protobuf int32(-7): field 1, sign-extended negative varint. */
    static const uint8_t error_body[] = {
        0x08u, 0xf9u, 0xffu, 0xffu, 0xffu, 0xffu,
        0xffu, 0xffu, 0xffu, 0xffu, 0x01u
    };
    struct fixture fixture;
    struct p4_hosted_request_ids ids_before;
    struct p4_hosted_inbox_item item;
    struct p4_hosted_rpc rpc = {0};
    int32_t remote_status = 0;
    uint32_t uid;

    fixture_init(&fixture, TEST_REQUEST_ID, 0u);
    uid = fixture_uid(&fixture);
    CHECK(submit(&fixture, TEST_GENERATION, &fixture.ops, 0u,
        fixture.request, fixture.request_size, fixture.txbuffer,
        sizeof fixture.txbuffer));
    ids_before = fixture.ids;

    memset(&item, 0, sizeof item);
    item.msg_type = P4_HOSTED_RPC_TYPE_EVENT;
    item.msg_id = 775u;
    item.uid = 0u;
    CHECK(p4_hosted_pending_correlate(&fixture.ids, TEST_GENERATION,
        &item) == P4_HOSTED_PENDING_CORRELATION_EVENT);
    CHECK(memcmp(&ids_before, &fixture.ids, sizeof ids_before) == 0);

    init_response_item(&item, TEST_REQUEST_ID, uid + 1u,
                       error_body, sizeof error_body);
    CHECK(p4_hosted_pending_correlate(&fixture.ids, TEST_GENERATION,
        &item) == P4_HOSTED_PENDING_CORRELATION_UNMATCHED);
    CHECK(memcmp(&ids_before, &fixture.ids, sizeof ids_before) == 0);

    init_response_item(&item, TEST_REQUEST_ID + 1u, uid,
                       error_body, sizeof error_body);
    CHECK(p4_hosted_pending_correlate(&fixture.ids, TEST_GENERATION,
        &item) == P4_HOSTED_PENDING_CORRELATION_UNMATCHED);
    CHECK(memcmp(&ids_before, &fixture.ids, sizeof ids_before) == 0);

    rpc.msg_type = P4_HOSTED_RPC_TYPE_RESPONSE;
    rpc.msg_id = TEST_REQUEST_ID + 256u;
    rpc.uid = uid;
    rpc.payload = error_body;
    rpc.payload_size = sizeof error_body;
    CHECK(p4_hosted_association_status_response(&rpc, TEST_REQUEST_ID, uid,
                                                &remote_status));
    CHECK(remote_status == -7);

    init_response_item(&item, TEST_REQUEST_ID, uid,
                       error_body, sizeof error_body);
    CHECK(p4_hosted_pending_correlate(&fixture.ids, TEST_GENERATION,
        &item) == P4_HOSTED_PENDING_CORRELATION_MATCHED);
    CHECK(fixture.ids.active_uid == 0u &&
          fixture.ids.active_request_id == 0u &&
          fixture.ids.next_uid == 2u && fixture.ids.retired == 0u);
    /* Correlation is not acceptance of this negative remote status. */
    CHECK(remote_status < 0);
}

static void test_ap_info_request_uses_response_550_and_exact_uid(void)
{
    static const uint8_t not_connected_status[] = { 0x08u, 0x8fu, 0x60u };
    struct fixture fixture;
    struct p4_hosted_request_ids ids_before;
    struct p4_hosted_inbox_item item;
    uint32_t uid, next_uid;

    fixture_init(&fixture, TEST_AP_INFO_REQUEST_ID, 0u);
    uid = fixture_uid(&fixture);
    CHECK(uid == 1u &&
          fixture.ids.active_request_id == TEST_AP_INFO_REQUEST_ID);
    CHECK(submit(&fixture, TEST_GENERATION, &fixture.ops, 0u,
        fixture.request, fixture.request_size, fixture.txbuffer,
        sizeof fixture.txbuffer));
    CHECK(fixture.result.error == P4_HOSTED_PENDING_ERROR_NONE &&
          fixture.result.submitted == 1u && fixture.result.uid == uid &&
          fixture.result.request_id == TEST_AP_INFO_REQUEST_ID);
    verify_request_frame(&fixture.fake, uid, TEST_AP_INFO_REQUEST_ID, 0u);
    ids_before = fixture.ids;

    memset(&item, 0, sizeof item);
    item.msg_type = P4_HOSTED_RPC_TYPE_EVENT;
    item.msg_id = 775u;
    item.uid = 0u;
    CHECK(p4_hosted_pending_correlate(&fixture.ids, TEST_GENERATION,
        &item) == P4_HOSTED_PENDING_CORRELATION_EVENT);
    CHECK(memcmp(&ids_before, &fixture.ids, sizeof ids_before) == 0);

    init_response_item(&item, TEST_AP_INFO_REQUEST_ID, uid + 1u,
                       not_connected_status, sizeof not_connected_status);
    CHECK(item.msg_id == 550u &&
          p4_hosted_pending_correlate(&fixture.ids, TEST_GENERATION,
              &item) == P4_HOSTED_PENDING_CORRELATION_UNMATCHED);
    CHECK(memcmp(&ids_before, &fixture.ids, sizeof ids_before) == 0);

    init_response_item(&item, TEST_AP_INFO_REQUEST_ID + 1u, uid,
                       not_connected_status, sizeof not_connected_status);
    CHECK(item.msg_id == 551u &&
          p4_hosted_pending_correlate(&fixture.ids, TEST_GENERATION,
              &item) == P4_HOSTED_PENDING_CORRELATION_UNMATCHED);
    CHECK(memcmp(&ids_before, &fixture.ids, sizeof ids_before) == 0);

    init_response_item(&item, TEST_AP_INFO_REQUEST_ID, uid,
                       not_connected_status, sizeof not_connected_status);
    CHECK(item.msg_id == 550u &&
          p4_hosted_pending_correlate(&fixture.ids, TEST_GENERATION,
              &item) == P4_HOSTED_PENDING_CORRELATION_MATCHED);
    CHECK(fixture.ids.active_uid == 0u &&
          fixture.ids.active_request_id == 0u &&
          fixture.ids.next_uid == 2u && fixture.ids.retired == 0u);

    CHECK(p4_hosted_request_ids_reserve(&fixture.ids, TEST_GENERATION,
        TEST_AP_INFO_REQUEST_ID, &next_uid));
    CHECK(next_uid == 2u);
    ids_before = fixture.ids;
    init_response_item(&item, TEST_AP_INFO_REQUEST_ID, uid,
                       not_connected_status, sizeof not_connected_status);
    CHECK(p4_hosted_pending_correlate(&fixture.ids, TEST_GENERATION,
        &item) == P4_HOSTED_PENDING_CORRELATION_UNMATCHED);
    CHECK(memcmp(&ids_before, &fixture.ids, sizeof ids_before) == 0);

    /* A caller timeout closes the second query without reusing its UID. */
    CHECK(p4_hosted_request_ids_finish(&fixture.ids, TEST_GENERATION,
        next_uid, TEST_AP_INFO_REQUEST_ID));
    CHECK(fixture.ids.next_uid == 3u && fixture.ids.active_uid == 0u);
    CHECK(p4_hosted_request_ids_reserve(&fixture.ids, TEST_GENERATION,
        TEST_AP_INFO_REQUEST_ID, &next_uid));
    CHECK(next_uid == 3u);
}

static void test_cancel_consumes_uid_and_old_reply_is_unmatched(void)
{
    struct fixture fixture;
    struct p4_hosted_inbox_item old_response;
    struct p4_hosted_request_ids ids_before;
    uint32_t first_uid, second_uid;

    fixture_init(&fixture, TEST_REQUEST_ID, 0u);
    first_uid = fixture_uid(&fixture);
    CHECK(submit(&fixture, TEST_GENERATION, &fixture.ops, 0u,
        fixture.request, fixture.request_size, fixture.txbuffer,
        sizeof fixture.txbuffer));
    /* Caller timeout/cancellation closes only the active reservation. */
    CHECK(p4_hosted_request_ids_finish(&fixture.ids, TEST_GENERATION,
        first_uid, TEST_REQUEST_ID));
    CHECK(fixture.ids.next_uid == 2u && fixture.ids.active_uid == 0u);
    CHECK(p4_hosted_request_ids_reserve(&fixture.ids, TEST_GENERATION,
        TEST_REQUEST_ID, &second_uid));
    CHECK(second_uid == 2u);
    ids_before = fixture.ids;
    init_response_item(&old_response, TEST_REQUEST_ID, first_uid, NULL, 0u);
    CHECK(p4_hosted_pending_correlate(&fixture.ids, TEST_GENERATION,
        &old_response) == P4_HOSTED_PENDING_CORRELATION_UNMATCHED);
    CHECK(memcmp(&ids_before, &fixture.ids, sizeof ids_before) == 0);
    CHECK(fixture.ids.active_uid == second_uid &&
          fixture.ids.active_request_id == TEST_REQUEST_ID);
}

static void expect_preflight_unchanged(struct fixture *fixture,
    struct p4_hosted_fifo *fifo, struct p4_hosted_request_ids *ids,
    uint32_t generation, const struct p4_hosted_tx_ops *ops,
    unsigned checksum_enabled, const uint8_t *request, size_t request_size,
    uint8_t *buffer, size_t capacity)
{
    struct p4_hosted_fifo fifo_before;
    struct p4_hosted_request_ids ids_before;
    struct p4_hosted_pending_result result_before;
    unsigned token_reads = fixture->fake.token_reads;
    unsigned other_reads = fixture->fake.other_reads;
    unsigned writes = fixture->fake.write_calls;
    unsigned sessions = fixture->fake.session_calls;
    unsigned budgets = fixture->fake.budget_calls;
    unsigned yields = fixture->fake.yield_calls;
    uint8_t buffer_before[P4_HOSTED_PENDING_BUFFER_MAX + 1u];
    int accepted;

    CHECK(capacity <= sizeof buffer_before);
    memcpy(&fifo_before, fifo, sizeof fifo_before);
    memcpy(&ids_before, ids, sizeof ids_before);
    memcpy(&result_before, &fixture->result, sizeof result_before);
    memcpy(buffer_before, buffer, capacity);
    accepted = p4_hosted_pending_submit(fifo, ids, generation, ops,
        checksum_enabled, request, request_size, buffer, capacity,
        &fixture->result);
    CHECK(!accepted);
    CHECK(memcmp(&fifo_before, fifo, sizeof fifo_before) == 0);
    CHECK(memcmp(&ids_before, ids, sizeof ids_before) == 0);
    CHECK(memcmp(&result_before, &fixture->result,
                 sizeof result_before) == 0);
    CHECK(memcmp(buffer_before, buffer, capacity) == 0);
    CHECK(fixture->fake.token_reads == token_reads &&
          fixture->fake.other_reads == other_reads &&
          fixture->fake.write_calls == writes &&
          fixture->fake.session_calls == sessions &&
          fixture->fake.budget_calls == budgets &&
          fixture->fake.yield_calls == yields);
}

static void test_submit_preflight_refusals_are_transactional(void)
{
    struct fixture fixture;
    uint8_t mismatch[P4_HOSTED_PENDING_MAX_REQUEST_SIZE];
    uint8_t oversized[P4_HOSTED_PENDING_MAX_REQUEST_SIZE + 1u];
    uint8_t extended_buffer[P4_HOSTED_PENDING_BUFFER_MAX + 1u];
    struct p4_hosted_request_ids corrupt_ids;
    struct p4_hosted_fifo corrupt_fifo;
    struct p4_hosted_tx_ops missing_ops;
    size_t mismatch_size;

    fixture_init(&fixture, TEST_REQUEST_ID, 0u);
    mismatch_size = encode_request(TEST_REQUEST_ID, fixture_uid(&fixture) + 1u,
                                   mismatch, sizeof mismatch);
    CHECK(mismatch_size != 0u);
    expect_preflight_unchanged(&fixture, &fixture.fifo, &fixture.ids,
        TEST_GENERATION, &fixture.ops, 0u, mismatch, mismatch_size,
        fixture.txbuffer, sizeof fixture.txbuffer);

    mismatch_size = encode_request(TEST_REQUEST_ID + 1u,
        fixture_uid(&fixture), mismatch, sizeof mismatch);
    CHECK(mismatch_size != 0u);
    expect_preflight_unchanged(&fixture, &fixture.fifo, &fixture.ids,
        TEST_GENERATION, &fixture.ops, 0u, mismatch, mismatch_size,
        fixture.txbuffer, sizeof fixture.txbuffer);

    expect_preflight_unchanged(&fixture, &fixture.fifo, &fixture.ids,
        TEST_GENERATION + 1u, &fixture.ops, 0u, fixture.request,
        fixture.request_size, fixture.txbuffer, sizeof fixture.txbuffer);
    expect_preflight_unchanged(&fixture, &fixture.fifo, &fixture.ids,
        TEST_GENERATION, &fixture.ops, 2u, fixture.request,
        fixture.request_size, fixture.txbuffer, sizeof fixture.txbuffer);
    expect_preflight_unchanged(&fixture, &fixture.fifo, &fixture.ids,
        TEST_GENERATION, &fixture.ops, 0u, fixture.request, 0u,
        fixture.txbuffer, sizeof fixture.txbuffer);
    memset(mismatch, 0, sizeof mismatch);
    expect_preflight_unchanged(&fixture, &fixture.fifo, &fixture.ids,
        TEST_GENERATION, &fixture.ops, 0u, mismatch, sizeof mismatch,
        fixture.txbuffer, sizeof fixture.txbuffer);
    memset(oversized, 0x7a, sizeof oversized);
    expect_preflight_unchanged(&fixture, &fixture.fifo, &fixture.ids,
        TEST_GENERATION, &fixture.ops, 0u, oversized, sizeof oversized,
        fixture.txbuffer, sizeof fixture.txbuffer);
    expect_preflight_unchanged(&fixture, &fixture.fifo, &fixture.ids,
        TEST_GENERATION, &fixture.ops, 0u, fixture.request,
        fixture.request_size, fixture.txbuffer,
        P4_HOSTED_PENDING_BUFFER_MIN - 1u);
    expect_preflight_unchanged(&fixture, &fixture.fifo, &fixture.ids,
        TEST_GENERATION, &fixture.ops, 0u, fixture.request,
        fixture.request_size, extended_buffer, sizeof extended_buffer);

    /* Unsupported request 285 is well-formed and reservable by the generic
     * ID ledger, but is deliberately outside the transport allowlist. */
    {
        struct fixture unsupported;
        fixture_init(&unsupported, 285u, 0u);
        expect_preflight_unchanged(&unsupported, &unsupported.fifo,
            &unsupported.ids, TEST_GENERATION, &unsupported.ops, 0u,
            unsupported.request, unsupported.request_size,
            unsupported.txbuffer, sizeof unsupported.txbuffer);
    }

    corrupt_ids = fixture.ids;
    corrupt_ids.retired = 2u;
    expect_preflight_unchanged(&fixture, &fixture.fifo, &corrupt_ids,
        TEST_GENERATION, &fixture.ops, 0u, fixture.request,
        fixture.request_size, fixture.txbuffer, sizeof fixture.txbuffer);
    corrupt_fifo = fixture.fifo;
    corrupt_fifo.pool_limit = 0u;
    expect_preflight_unchanged(&fixture, &corrupt_fifo, &fixture.ids,
        TEST_GENERATION, &fixture.ops, 0u, fixture.request,
        fixture.request_size, fixture.txbuffer, sizeof fixture.txbuffer);
    missing_ops = fixture.ops;
    missing_ops.write_fifo = NULL;
    expect_preflight_unchanged(&fixture, &fixture.fifo, &fixture.ids,
        TEST_GENERATION, &missing_ops, 0u, fixture.request,
        fixture.request_size, fixture.txbuffer, sizeof fixture.txbuffer);

    expect_preflight_unchanged(&fixture, &fixture.fifo, &fixture.ids,
        TEST_GENERATION, &fixture.ops, 0u,
        (const uint8_t *)(uintptr_t)(UINTPTR_MAX - 3u), 8u,
        fixture.txbuffer, sizeof fixture.txbuffer);
    expect_preflight_unchanged(&fixture, &fixture.fifo, &fixture.ids,
        TEST_GENERATION, &fixture.ops, 0u,
        (const uint8_t *)(const void *)&fixture.fifo,
        sizeof fixture.fifo, fixture.txbuffer, sizeof fixture.txbuffer);
}

union pending_alias_slab {
    struct p4_hosted_fifo fifo;
    struct p4_hosted_request_ids ids;
    struct p4_hosted_tx_ops ops;
    struct p4_hosted_pending_result result;
    uint8_t bytes[P4_HOSTED_PENDING_BUFFER_MAX];
};

static void expect_pairwise_alias_refusal(struct fixture *fixture,
                                          unsigned first, unsigned second)
{
    struct p4_hosted_fifo fifo_before;
    struct p4_hosted_request_ids ids_before;
    struct p4_hosted_tx_ops ops_before;
    struct p4_hosted_pending_result result_before;
    uint8_t request_before[P4_HOSTED_PENDING_MAX_REQUEST_SIZE];
    uint8_t txbuffer_before[TEST_CAPACITY];
    uint8_t slab_before[P4_HOSTED_PENDING_BUFFER_MAX];
    union pending_alias_slab slab;
    void *objects[6];
    unsigned token_reads = fixture->fake.token_reads;
    unsigned other_reads = fixture->fake.other_reads;
    unsigned writes = fixture->fake.write_calls;

    CHECK(first < second && second < 6u);
    objects[0] = &fixture->fifo;
    objects[1] = &fixture->ids;
    objects[2] = &fixture->ops;
    objects[3] = fixture->request;
    objects[4] = fixture->txbuffer;
    objects[5] = &fixture->result;
    memset(&slab, 0x6du, sizeof slab);
    objects[first] = slab.bytes;
    objects[second] = slab.bytes;

    memcpy(&fifo_before, &fixture->fifo, sizeof fifo_before);
    memcpy(&ids_before, &fixture->ids, sizeof ids_before);
    memcpy(&ops_before, &fixture->ops, sizeof ops_before);
    memcpy(&result_before, &fixture->result, sizeof result_before);
    memcpy(request_before, fixture->request, sizeof request_before);
    memcpy(txbuffer_before, fixture->txbuffer, sizeof txbuffer_before);
    memcpy(slab_before, slab.bytes, sizeof slab_before);

    CHECK(!p4_hosted_pending_submit(
        (struct p4_hosted_fifo *)objects[0],
        (struct p4_hosted_request_ids *)objects[1], TEST_GENERATION,
        (const struct p4_hosted_tx_ops *)objects[2], 0u,
        (const uint8_t *)objects[3], fixture->request_size,
        (uint8_t *)objects[4], TEST_CAPACITY,
        (struct p4_hosted_pending_result *)objects[5]));

    CHECK(memcmp(&fifo_before, &fixture->fifo, sizeof fifo_before) == 0);
    CHECK(memcmp(&ids_before, &fixture->ids, sizeof ids_before) == 0);
    CHECK(memcmp(&ops_before, &fixture->ops, sizeof ops_before) == 0);
    CHECK(memcmp(&result_before, &fixture->result, sizeof result_before) == 0);
    CHECK(memcmp(request_before, fixture->request, sizeof request_before) == 0);
    CHECK(memcmp(txbuffer_before, fixture->txbuffer,
                 sizeof txbuffer_before) == 0);
    CHECK(memcmp(slab_before, slab.bytes, sizeof slab_before) == 0);
    CHECK(fixture->fake.token_reads == token_reads &&
          fixture->fake.other_reads == other_reads &&
          fixture->fake.write_calls == writes);
}

static void test_every_pending_object_pair_alias_refuses(void)
{
    struct fixture fixture;
    unsigned first, second;

    fixture_init(&fixture, TEST_REQUEST_ID, 0u);
    for (first = 0u; first < 6u; ++first)
        for (second = first + 1u; second < 6u; ++second)
            expect_pairwise_alias_refusal(&fixture, first, second);
}

static void test_failed_submission_consumes_or_retires_as_needed(void)
{
    struct fixture fixture;
    uint32_t uid;

    fixture_init(&fixture, TEST_REQUEST_ID, 0u);
    uid = fixture_uid(&fixture);
    fixture.fake.token_result = 0;
    CHECK(!submit(&fixture, TEST_GENERATION, &fixture.ops, 0u,
        fixture.request, fixture.request_size, fixture.txbuffer,
        sizeof fixture.txbuffer));
    CHECK(fixture.result.error == P4_HOSTED_PENDING_ERROR_TX);
    CHECK(fixture.result.tx.error == P4_HOSTED_TX_ERROR_TOKEN_READ);
    CHECK(fixture.result.submitted == 0u && fixture.fake.token_reads == 1u &&
          fixture.fake.write_calls == 0u);
    CHECK(!fixture.fifo.poisoned && fixture.ids.retired == 0u);
    CHECK(fixture.ids.next_uid == uid + 1u && fixture.ids.active_uid == 0u &&
          fixture.ids.active_request_id == 0u);

    fixture_init(&fixture, TEST_REQUEST_ID, 0u);
    uid = fixture_uid(&fixture);
    fixture.fake.budget_result = 0;
    CHECK(!submit(&fixture, TEST_GENERATION, &fixture.ops, 0u,
        fixture.request, fixture.request_size, fixture.txbuffer,
        sizeof fixture.txbuffer));
    CHECK(fixture.result.error == P4_HOSTED_PENDING_ERROR_TX &&
          fixture.result.tx.error == P4_HOSTED_TX_ERROR_BUDGET_EXPIRED);
    CHECK(fixture.fake.budget_calls == 1u && fixture.fake.token_reads == 0u &&
          fixture.fake.write_calls == 0u && !fixture.fifo.poisoned);
    CHECK(fixture.ids.next_uid == uid + 1u && fixture.ids.active_uid == 0u &&
          fixture.ids.retired == 0u);

    fixture_init(&fixture, TEST_REQUEST_ID, 0u);
    fixture.fake.session_result = 0;
    CHECK(!submit(&fixture, TEST_GENERATION, &fixture.ops, 0u,
        fixture.request, fixture.request_size, fixture.txbuffer,
        sizeof fixture.txbuffer));
    CHECK(fixture.result.error == P4_HOSTED_PENDING_ERROR_SESSION &&
          fixture.result.tx.error == P4_HOSTED_TX_ERROR_SESSION_LOST);
    CHECK(fixture.fake.token_reads == 0u && fixture.fake.write_calls == 0u &&
          fixture.fifo.poisoned && fixture.ids.retired == 1u &&
          fixture.ids.next_uid == 2u);

    fixture_init(&fixture, TEST_REQUEST_ID, 0u);
    uid = fixture_uid(&fixture);
    fixture.fake.write_result = 0;
    fixture.fake.completion_overridden = 1u;
    fixture.fake.completion_override.started = 1u;
    fixture.fake.completion_override.success = 0u;
    fixture.fake.completion_override.completed_bytes = 0u;
    CHECK(!submit(&fixture, TEST_GENERATION, &fixture.ops, 0u,
        fixture.request, fixture.request_size, fixture.txbuffer,
        sizeof fixture.txbuffer));
    CHECK(fixture.result.error == P4_HOSTED_PENDING_ERROR_SESSION);
    CHECK(fixture.result.tx.error == P4_HOSTED_TX_ERROR_WRITE_AMBIGUOUS);
    CHECK(fixture.fake.write_calls == 1u && fixture.fifo.poisoned &&
          !fixture.fifo.busy);
    CHECK(fixture.ids.retired == 1u && fixture.ids.active_uid == 0u &&
          fixture.ids.active_request_id == 0u &&
          fixture.ids.next_uid == uid + 1u);

    fixture_init(&fixture, TEST_REQUEST_ID, 0u);
    uid = fixture_uid(&fixture);
    /* TX checks the supplied aggregate deadline before/after the token read,
     * once more before writing, and once after the authoritative callback. */
    fixture.fake.budget_fail_at = 4u;
    CHECK(!submit(&fixture, TEST_GENERATION, &fixture.ops, 0u,
        fixture.request, fixture.request_size, fixture.txbuffer,
        sizeof fixture.txbuffer));
    CHECK(fixture.result.error == P4_HOSTED_PENDING_ERROR_SESSION &&
          fixture.result.tx.error == P4_HOSTED_TX_ERROR_BUDGET_EXPIRED);
    CHECK(fixture.fake.budget_calls == 4u && fixture.fake.write_calls == 1u &&
          fixture.result.tx.write_called == 1u && fixture.fifo.poisoned &&
          fixture.fifo.tx_sent == 0u);
    CHECK(fixture.ids.retired == 1u && fixture.ids.next_uid == uid + 1u &&
          fixture.ids.active_uid == 0u);
}

static void test_replacement_epoch_is_never_mutated_by_stale_tx(void)
{
    struct fixture fixture;
    struct p4_hosted_fifo replacement_fifo, fifo_before;
    struct p4_hosted_request_ids replacement_ids, ids_before;
    uint32_t replacement_uid = 0u;

    fixture_init(&fixture, TEST_REQUEST_ID, 0u);
    memset(&replacement_fifo, 0, sizeof replacement_fifo);
    memset(&replacement_ids, 0, sizeof replacement_ids);
    CHECK(p4_hosted_fifo_start(&replacement_fifo, REPLACEMENT_GENERATION,
                               TEST_POOL_LIMIT));
    replacement_fifo.tx_sent = 7u;
    CHECK(p4_hosted_request_ids_init(&replacement_ids,
                                     REPLACEMENT_GENERATION));
    CHECK(p4_hosted_request_ids_reserve(&replacement_ids,
        REPLACEMENT_GENERATION, 283u, &replacement_uid));
    CHECK(replacement_uid == 1u);
    fixture.fake.replacement_fifo = replacement_fifo;
    fixture.fake.replacement_ids = replacement_ids;
    fixture.fake.replace_epoch_on_write = 1u;

    CHECK(!submit(&fixture, TEST_GENERATION, &fixture.ops, 0u,
        fixture.request, fixture.request_size, fixture.txbuffer,
        sizeof fixture.txbuffer));
    fifo_before = replacement_fifo;
    ids_before = replacement_ids;
    CHECK(memcmp(&fifo_before, &fixture.fifo, sizeof fifo_before) == 0);
    CHECK(memcmp(&ids_before, &fixture.ids, sizeof ids_before) == 0);
    CHECK(fixture.result.error == P4_HOSTED_PENDING_ERROR_SESSION &&
          fixture.result.submitted == 0u);
    CHECK(fixture.fake.write_calls == 1u && fixture.fake.other_reads == 0u);
}

static void test_replacement_identity_is_not_completed_after_write(void)
{
    struct fixture fixture;
    struct p4_hosted_request_ids replacement_ids, ids_before;
    uint32_t replacement_uid = 0u;

    fixture_init(&fixture, TEST_REQUEST_ID, 0u);
    memset(&replacement_ids, 0, sizeof replacement_ids);
    CHECK(p4_hosted_request_ids_init(&replacement_ids,
                                     REPLACEMENT_GENERATION));
    CHECK(p4_hosted_request_ids_reserve(&replacement_ids,
        REPLACEMENT_GENERATION, 283u, &replacement_uid));
    CHECK(replacement_uid == 1u);
    fixture.fake.replacement_ids = replacement_ids;
    fixture.fake.replace_ids_on_write = 1u;
    ids_before = replacement_ids;

    CHECK(!submit(&fixture, TEST_GENERATION, &fixture.ops, 0u,
        fixture.request, fixture.request_size, fixture.txbuffer,
        sizeof fixture.txbuffer));
    CHECK(fixture.result.error == P4_HOSTED_PENDING_ERROR_SESSION &&
          fixture.result.submitted == 1u && fixture.result.tx.success == 1u);
    CHECK(fixture.fifo.tx_sent == 1u && !fixture.fifo.poisoned &&
          !fixture.fifo.busy);
    CHECK(memcmp(&ids_before, &fixture.ids, sizeof ids_before) == 0);
    CHECK(fixture.fake.write_calls == 1u && fixture.fake.other_reads == 0u);
}

static void test_correlator_refuses_bad_metadata_without_mutation(void)
{
    struct fixture fixture;
    struct p4_hosted_request_ids ids_before;
    struct p4_hosted_inbox_item item, item_before;

    fixture_init(&fixture, TEST_REQUEST_ID, 0u);
    CHECK(submit(&fixture, TEST_GENERATION, &fixture.ops, 0u,
        fixture.request, fixture.request_size, fixture.txbuffer,
        sizeof fixture.txbuffer));
    ids_before = fixture.ids;

    memset(&item, 0, sizeof item);
    item.msg_type = P4_HOSTED_RPC_TYPE_REQUEST;
    item.msg_id = TEST_REQUEST_ID;
    item.uid = fixture.ids.active_uid;
    item_before = item;
    CHECK(p4_hosted_pending_correlate(&fixture.ids, TEST_GENERATION,
        &item) == P4_HOSTED_PENDING_CORRELATION_REFUSED);
    CHECK(memcmp(&ids_before, &fixture.ids, sizeof ids_before) == 0);
    CHECK(memcmp(&item_before, &item, sizeof item_before) == 0);

    item.msg_type = P4_HOSTED_RPC_TYPE_RESPONSE;
    item.msg_id = 606u;
    item.uid = fixture.ids.active_uid;
    CHECK(p4_hosted_pending_correlate(&fixture.ids, TEST_GENERATION,
        &item) == P4_HOSTED_PENDING_CORRELATION_REFUSED);
    CHECK(memcmp(&ids_before, &fixture.ids, sizeof ids_before) == 0);

    item.msg_id = TEST_REQUEST_ID + 256u;
    item.uid = 0u;
    CHECK(p4_hosted_pending_correlate(&fixture.ids, TEST_GENERATION,
        &item) == P4_HOSTED_PENDING_CORRELATION_REFUSED);
    CHECK(memcmp(&ids_before, &fixture.ids, sizeof ids_before) == 0);

    item.uid = fixture.ids.active_uid;
    item.payload_size = P4_HOSTED_RPC_MAX_SIZE + 1u;
    CHECK(p4_hosted_pending_correlate(&fixture.ids, TEST_GENERATION,
        &item) == P4_HOSTED_PENDING_CORRELATION_REFUSED);
    CHECK(memcmp(&ids_before, &fixture.ids, sizeof ids_before) == 0);

    CHECK(p4_hosted_pending_correlate(&fixture.ids, TEST_GENERATION + 1u,
        &item) == P4_HOSTED_PENDING_CORRELATION_REFUSED);
    CHECK(memcmp(&ids_before, &fixture.ids, sizeof ids_before) == 0);

    CHECK(p4_hosted_pending_correlate(&fixture.ids, TEST_GENERATION,
        NULL) == P4_HOSTED_PENDING_CORRELATION_REFUSED);
    CHECK(memcmp(&ids_before, &fixture.ids, sizeof ids_before) == 0);
}

static void test_max_uid_response_finishes_but_never_reuses_uid(void)
{
    struct p4_hosted_request_ids ids;
    struct p4_hosted_inbox_item item;
    uint32_t uid = 0u, next_uid = 0u;

    memset(&ids, 0, sizeof ids);
    CHECK(p4_hosted_request_ids_init(&ids, TEST_GENERATION));
    ids.next_uid = UINT32_MAX;
    CHECK(p4_hosted_request_ids_reserve(&ids, TEST_GENERATION,
        TEST_REQUEST_ID, &uid));
    CHECK(uid == UINT32_MAX && ids.exhausted == 1u &&
          ids.next_uid == UINT32_MAX);
    init_response_item(&item, TEST_REQUEST_ID, uid, NULL, 0u);
    CHECK(p4_hosted_pending_correlate(&ids, TEST_GENERATION, &item) ==
          P4_HOSTED_PENDING_CORRELATION_MATCHED);
    CHECK(ids.active_uid == 0u && ids.active_request_id == 0u &&
          ids.exhausted == 1u && ids.next_uid == UINT32_MAX);
    CHECK(!p4_hosted_request_ids_reserve(&ids, TEST_GENERATION,
        TEST_REQUEST_ID, &next_uid));
    CHECK(next_uid == 0u && ids.exhausted == 1u);
}

int main(void)
{
    test_submit_encodes_one_frame_and_leaves_identity_pending(0u);
    test_submit_encodes_one_frame_and_leaves_identity_pending(1u);
    test_events_unmatched_and_remote_error_only_correlate();
    test_ap_info_request_uses_response_550_and_exact_uid();
    test_cancel_consumes_uid_and_old_reply_is_unmatched();
    test_submit_preflight_refusals_are_transactional();
    test_every_pending_object_pair_alias_refuses();
    test_failed_submission_consumes_or_retires_as_needed();
    test_replacement_epoch_is_never_mutated_by_stale_tx();
    test_replacement_identity_is_not_completed_after_write();
    test_correlator_refuses_bad_metadata_without_mutation();
    test_max_uid_response_finishes_but_never_reuses_uid();
    printf("hosted pending TX/correlation: %lu checks passed\n", checks);
    return 0;
}
