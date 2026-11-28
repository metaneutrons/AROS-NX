/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
/* Host-model contract tests only. No hardware, timing, or firmware claim. */
#include "hosted_scan_sequence.h"
#include "hosted_rpc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_GENERATION UINT32_C(0x520)
#define TEST_CAPACITY 512u
#define MAX_CALLS 8u

static unsigned long checks;

#define CHECK(expression) do { \
    ++checks; \
    if (!(expression)) { \
        fprintf(stderr, "check failed at %s:%d: %s\n", __FILE__, __LINE__, \
                #expression); \
        exit(1); \
    } \
} while (0)

enum model_outcome {
    MODEL_OK,
    MODEL_REFUSE,
    MODEL_TRANSPORT,
    MODEL_REMOTE,
    MODEL_MALFORMED,
    MODEL_EMPTY_RECORDS,
    MODEL_SHORT_RECORDS
};

struct model_fault {
    unsigned call_number;
    enum model_outcome outcome;
    unsigned write_called;
};

struct model {
    uint32_t generation;
    uint32_t expected_ids[MAX_CALLS];
    uint32_t expected_uids[MAX_CALLS];
    unsigned expected_count;
    unsigned calls;
    unsigned uid_mask;
    unsigned contract_error;
    unsigned deadline_epoch;
    unsigned fault_count;
    struct model_fault faults[2];
    unsigned replace_after_call;
    uint16_t ap_count;
    uint16_t records_count;
};

struct fixture {
    struct p4_hosted_scan_sequence run;
    struct p4_hosted_startup startup;
    struct p4_hosted_fifo fifo;
    struct p4_hosted_open_ops rx;
    struct p4_hosted_tx_ops tx;
    uint8_t buffer[TEST_CAPACITY];
    struct model model;
};

static void set_schedule(struct model *m, const uint32_t *ids,
                         const uint32_t *uids, unsigned count)
{
    unsigned i;
    CHECK(count <= MAX_CALLS);
    m->expected_count = count;
    m->calls = 0;
    m->uid_mask = 0;
    m->contract_error = 0;
    m->fault_count = 0;
    m->replace_after_call = 0;
    for (i = 0; i < count; ++i) {
        m->expected_ids[i] = ids[i];
        m->expected_uids[i] = uids[i];
    }
}

static void set_fault(struct model *m, unsigned call_number,
                      enum model_outcome outcome, unsigned write_called)
{
    CHECK(m->fault_count < sizeof m->faults / sizeof m->faults[0]);
    m->faults[m->fault_count].call_number = call_number;
    m->faults[m->fault_count].outcome = outcome;
    m->faults[m->fault_count].write_called = write_called;
    ++m->fault_count;
}

static enum model_outcome outcome_for(const struct model *m,
                                     unsigned call_number,
                                     unsigned *write_called)
{
    unsigned i;
    *write_called = 1u;
    for (i = 0; i < m->fault_count; ++i) {
        if (m->faults[i].call_number == call_number) {
            *write_called = m->faults[i].write_called;
            return m->faults[i].outcome;
        }
    }
    return MODEL_OK;
}

static void fixture_init(struct fixture *f)
{
    memset(f, 0, sizeof *f);
    f->model.generation = TEST_GENERATION;
    f->model.deadline_epoch = 1u;
    f->model.ap_count = 3u;
    f->model.records_count = 3u;

    /* Model the state of a successful, still-live startup epoch. */
    f->startup.generation = TEST_GENERATION;
    f->startup.entered = 1u;
    f->startup.success = 1u;
    f->startup.init.success = 1u;
    f->startup.mode.success = 1u;
    f->startup.start.success = 1u;
    f->startup.mac_attempted = 1u;
    f->startup.mac.success = 1u;
    f->startup.mac.mac.valid_mac = 1u;

    f->fifo.generation = TEST_GENERATION;
    f->fifo.pool_limit = 20u;
    f->fifo.initialized = 1u;
    f->rx.context = &f->model;
    f->tx.context = &f->model;
    memset(f->buffer, 0xa5, sizeof f->buffer);
}

static void expect_schedule(const struct fixture *f, unsigned calls,
                            unsigned uid_mask)
{
    CHECK(f->model.calls == calls);
    CHECK(f->model.uid_mask == uid_mask);
    CHECK(f->model.contract_error == 0u);
    CHECK(f->model.deadline_epoch == 1u);
    CHECK((f->model.uid_mask & 0x7eu) == 0u);
}

static int check_request(const struct model *m, const uint8_t *request,
                         size_t request_size, uint32_t id, uint32_t uid,
                         unsigned call_index)
{
    struct p4_hosted_rpc rpc;
    const uint8_t *expected = NULL;
    size_t expected_size = 0u;
    uint8_t records_body[2];

    if (!p4_hosted_rpc_decode(request, request_size, &rpc) ||
        rpc.msg_type != P4_HOSTED_RPC_TYPE_REQUEST || rpc.msg_id != id ||
        rpc.uid != uid)
        return 0;

    switch (id) {
    case 286u: {
        static const uint8_t blocking_start[] = {0x10u, 0x01u, 0x18u, 0x00u};
        expected = blocking_start;
        expected_size = sizeof blocking_start;
        break;
    }
    case 287u:
    case 288u:
        expected_size = 0u;
        break;
    case 289u:
        if (call_index >= m->expected_count ||
            m->expected_ids[call_index] != id ||
            m->expected_uids[call_index] != uid)
            return 0;
        records_body[0] = 0x08u;
        records_body[1] = m->ap_count < P4_HOSTED_SCAN_MAX_RECORDS ?
            (uint8_t)m->ap_count : (uint8_t)P4_HOSTED_SCAN_MAX_RECORDS;
        expected = records_body;
        expected_size = sizeof records_body;
        break;
    default:
        return 0;
    }
    return rpc.payload_size == expected_size &&
        (expected_size == 0u ||
         memcmp(rpc.payload, expected, expected_size) == 0);
}

static size_t make_status_body(uint8_t *body, enum model_outcome outcome)
{
    if (outcome == MODEL_MALFORMED) {
        body[0] = 0x08u; body[1] = 0x00u;
        body[2] = 0x08u; body[3] = 0x00u;
        return 4u;
    }
    body[0] = 0x08u;
    body[1] = outcome == MODEL_REMOTE ? 1u : 0u;
    return 2u;
}

static size_t make_num_body(uint8_t *body, enum model_outcome outcome,
                            uint16_t count)
{
    if (outcome == MODEL_MALFORMED) {
        body[0] = 0x10u; body[1] = (uint8_t)count;
        body[2] = 0x10u; body[3] = (uint8_t)count;
        return 4u;
    }
    body[0] = 0x08u;
    body[1] = outcome == MODEL_REMOTE ? 1u : 0u;
    body[2] = 0x10u;
    body[3] = (uint8_t)count;
    return 4u;
}

static size_t make_records_body(uint8_t *body, enum model_outcome outcome,
                                uint16_t requested, uint16_t available)
{
    uint16_t number = available;
    uint16_t i;
    size_t offset = 0u;

    if (outcome == MODEL_EMPTY_RECORDS) {
        body[offset++] = 0x08u; body[offset++] = 0x00u;
        body[offset++] = 0x10u; body[offset++] = 0x00u;
        return offset;
    }
    if (outcome == MODEL_REMOTE) {
        body[offset++] = 0x08u; body[offset++] = 0x01u;
        body[offset++] = 0x10u; body[offset++] = 0x00u;
        return offset;
    }
    if (outcome == MODEL_SHORT_RECORDS && number != 0u)
        --number;
    if (outcome == MODEL_MALFORMED) {
        body[offset++] = 0x08u; body[offset++] = 0x00u;
        body[offset++] = 0x08u; body[offset++] = 0x00u;
        return offset;
    }

    body[offset++] = 0x08u; body[offset++] = 0x00u;
    body[offset++] = 0x10u; body[offset++] = (uint8_t)number;
    for (i = 0u; i < number && i < requested; ++i) {
        uint8_t seed = (uint8_t)(0x20u + i * 8u);
        body[offset++] = 0x1au;
        body[offset++] = 0x08u;
        body[offset++] = 0x0au;
        body[offset++] = 0x06u;
        body[offset++] = seed;
        body[offset++] = (uint8_t)(seed + 1u);
        body[offset++] = (uint8_t)(seed + 2u);
        body[offset++] = (uint8_t)(seed + 3u);
        body[offset++] = (uint8_t)(seed + 4u);
        body[offset++] = (uint8_t)(seed + 5u);
    }
    return offset;
}

int p4_hosted_control_exchange(struct p4_hosted_fifo *state,
    uint32_t generation, const struct p4_hosted_open_ops *rx,
    const struct p4_hosted_tx_ops *tx, unsigned checksum_enabled,
    const uint8_t *request, size_t request_size, uint8_t *buffer,
    size_t capacity, struct p4_hosted_control_result *result)
{
    struct model *m;
    struct p4_hosted_rpc parsed, response, decoded;
    enum model_outcome outcome;
    uint8_t response_wire[512], body[128];
    size_t response_size, body_size;
    unsigned call_index, write_called;
    uint32_t id, uid;

    (void)checksum_enabled;
    if (!state || !rx || !tx || !result || !request || !buffer ||
        capacity < TEST_CAPACITY || rx->context != tx->context ||
        !p4_hosted_rpc_decode(request, request_size, &parsed))
        return 0;
    m = (struct model *)rx->context;
    if (!m || generation != m->generation) return 0;
    id = parsed.msg_id;
    uid = parsed.uid;
    call_index = m->calls;
    if (call_index >= m->expected_count ||
        id != m->expected_ids[call_index] ||
        uid != m->expected_uids[call_index] ||
        !check_request(m, request, request_size, id, uid, call_index)) {
        m->contract_error = 1u;
        return 0;
    }
    ++m->calls;
    if (uid < 1u || uid > 31u || (m->uid_mask & (1u << uid)) != 0u) {
        m->contract_error = 1u;
        return 0;
    }
    m->uid_mask |= 1u << uid;

    /* Model the production control helper's preflight refusal. */
    if (state->initialized != 1u || state->generation != generation ||
        state->busy || state->poisoned || !state->pool_limit ||
        state->pool_limit > P4_HOSTED_TX_MASK) {
        result->error = state->generation == generation ?
            P4_HOSTED_CONTROL_PREFLIGHT : P4_HOSTED_CONTROL_SESSION;
        return 0;
    }

    outcome = outcome_for(m, m->calls, &write_called);
    if (outcome == MODEL_REFUSE || outcome == MODEL_TRANSPORT) {
        result->error = outcome == MODEL_TRANSPORT ?
            P4_HOSTED_CONTROL_TIMEOUT : P4_HOSTED_CONTROL_OK;
        result->tx.write_called = write_called;
        return 0;
    }

    result->tx.write_called = 1u;
    result->error = P4_HOSTED_CONTROL_OK;
    if (id == 286u || id == 287u) {
        body_size = make_status_body(body, outcome);
    } else if (id == 288u) {
        body_size = make_num_body(body, outcome, m->ap_count);
    } else if (id == 289u) {
        body_size = make_records_body(body, outcome,
            m->ap_count < P4_HOSTED_SCAN_MAX_RECORDS ? m->ap_count :
                P4_HOSTED_SCAN_MAX_RECORDS,
            m->records_count);
    } else {
        m->contract_error = 1u;
        return 0;
    }

    response.msg_type = P4_HOSTED_RPC_TYPE_RESPONSE;
    response.msg_id = id + 256u;
    response.uid = uid;
    response.payload = body;
    response.payload_size = body_size;
    if (!p4_hosted_rpc_encode(response_wire, sizeof response_wire,
                              &response, &response_size) ||
        !p4_hosted_rpc_decode(response_wire, response_size, &decoded) ||
        decoded.msg_type != P4_HOSTED_RPC_TYPE_RESPONSE ||
        decoded.msg_id != response.msg_id || decoded.uid != uid ||
        decoded.payload_size > sizeof result->response_body ||
        decoded.payload_size > capacity) {
        m->contract_error = 1u;
        return 0;
    }
    if (decoded.payload_size != 0u)
        memcpy(result->response_body, decoded.payload, decoded.payload_size);
    result->response_size = decoded.payload_size;
    result->response_id = decoded.msg_id;
    result->response_uid = decoded.uid;
    result->matched = 1u;
    result->success = 1u;
    ++result->batches;

    if (m->replace_after_call == m->calls)
        state->generation = generation + 1u;
    return 1;
}

static void run_success_case(uint16_t ap_count, uint16_t records_count,
                             unsigned expected_records)
{
    uint32_t ids[] = {286u, 288u, 289u};
    uint32_t uids[] = {7u, 8u, 9u};
    struct fixture f;
    fixture_init(&f);
    f.model.ap_count = ap_count;
    f.model.records_count = records_count;
    if (ap_count == 0u) {
        set_schedule(&f.model, ids, uids, 2u);
        ids[1] = 288u; uids[1] = 8u;
    } else {
        set_schedule(&f.model, ids, uids, 3u);
    }

    CHECK(p4_hosted_scan_sequence_run(&f.run, &f.startup, &f.fifo,
        TEST_GENERATION, &f.rx, &f.tx, 0u, f.buffer, sizeof f.buffer));
    CHECK(f.run.entered == 1u && f.run.success == 1u);
    CHECK(f.run.start.success == 1u && f.run.start.status_valid == 1u &&
          f.run.start.status == 0);
    CHECK(f.run.count.success == 1u && f.run.count.status_valid == 1u &&
          f.run.ap_count.number == ap_count);
    if (ap_count == 0u) {
        CHECK(f.run.skipped_empty_records == 1u);
        CHECK(f.run.records.attempted == 0u);
        CHECK(f.run.aps.record_count == 0u);
        expect_schedule(&f, 2u, (1u << 7) | (1u << 8));
    } else {
        unsigned i;
        CHECK(f.run.skipped_empty_records == 0u);
        CHECK(f.run.records.success == 1u && f.run.records.status_valid == 1u);
        CHECK(f.run.aps.record_count == expected_records);
        CHECK(f.run.aps.number == expected_records);
        CHECK(f.run.records.attempted == 1u);
        for (i = 0u; i < expected_records; ++i)
            CHECK(f.run.aps.records[i].bssid[0] == (uint8_t)(0x20u + i * 8u));
        expect_schedule(&f, 3u, (1u << 7) | (1u << 8) | (1u << 9));
        /* Parsed AP records are owned, not borrowed from the exchange scratch. */
        memset(f.run.reply.response_body, 0, sizeof f.run.reply.response_body);
        CHECK(f.run.aps.records[0].bssid[0] == 0x20u);
    }

    /* A completed scan does not send ScanStop; caller proceeds to WiFiStop. */
    CHECK(p4_hosted_scan_sequence_cleanup(&f.run, &f.startup, &f.fifo,
        TEST_GENERATION, &f.rx, &f.tx, 0u, f.buffer, sizeof f.buffer));
    CHECK(f.run.cleanup_entered == 1u && f.run.cleanup_success == 1u);
    CHECK(f.run.stop.attempted == 0u);
    expect_schedule(&f, ap_count == 0u ? 2u : 3u,
        ap_count == 0u ? ((1u << 7) | (1u << 8)) :
                         ((1u << 7) | (1u << 8) | (1u << 9)));
}

static void test_success_and_owned_records(void)
{
    run_success_case(0u, 0u, 0u);
    run_success_case(3u, 3u, 3u);
    /* GetApRecords requests only the first eight when the peer reports more. */
    run_success_case(13u, 8u, 8u);
}

static void test_failure_case(unsigned fault_call, enum model_outcome outcome,
                              unsigned write_called, uint16_t ap_count,
                              uint16_t records_count, unsigned expect_stop,
                              int cleanup_ok)
{
    static const uint32_t stage_ids[] = {286u, 288u, 289u};
    static const uint32_t stage_uids[] = {7u, 8u, 9u};
    uint32_t ids[MAX_CALLS], uids[MAX_CALLS];
    struct fixture f;
    unsigned expected_calls = fault_call + (expect_stop ? 1u : 0u);
    unsigned i;
    int run_result;
    fixture_init(&f);
    f.model.ap_count = ap_count;
    f.model.records_count = records_count;
    CHECK(fault_call >= 1u && fault_call <= 3u);
    for (i = 0u; i < fault_call; ++i) {
        ids[i] = stage_ids[i];
        uids[i] = stage_uids[i];
    }
    if (expect_stop) {
        ids[fault_call] = 287u;
        uids[fault_call] = 10u;
    }
    set_schedule(&f.model, ids, uids, expected_calls);
    set_fault(&f.model, fault_call, outcome, write_called);

    run_result = p4_hosted_scan_sequence_run(&f.run, &f.startup, &f.fifo,
        TEST_GENERATION, &f.rx, &f.tx, 0u, f.buffer, sizeof f.buffer);
    if (run_result)
        fprintf(stderr, "unexpected scan success at fault call %u, outcome %u\n",
                fault_call, (unsigned)outcome);
    CHECK(!run_result);
    CHECK(f.run.entered == 1u && f.run.success == 0u);
    CHECK(!f.run.start.success || fault_call != 1u);

    if (expect_stop) {
        CHECK(p4_hosted_scan_sequence_cleanup(&f.run, &f.startup, &f.fifo,
            TEST_GENERATION, &f.rx, &f.tx, 0u, f.buffer, sizeof f.buffer) ==
              cleanup_ok);
        CHECK(f.run.stop.attempted == 1u);
        CHECK(f.run.stop.write_called == (cleanup_ok ? 1u : 0u));
        CHECK(f.run.cleanup_success == (unsigned)cleanup_ok);
        expect_schedule(&f, expected_calls, f.model.uid_mask);
    } else {
        CHECK(p4_hosted_scan_sequence_cleanup(&f.run, &f.startup, &f.fifo,
            TEST_GENERATION, &f.rx, &f.tx, 0u, f.buffer, sizeof f.buffer));
        CHECK(f.run.stop.attempted == 0u && f.run.cleanup_success == 1u);
        expect_schedule(&f, f.model.calls, f.model.uid_mask);
    }
}

static void test_failures_and_cleanup(void)
{
    /* Each operational stage preserves its failure and stops once if start
     * reached the write callback. */
    test_failure_case(1u, MODEL_REMOTE, 1u, 3u, 3u, 1u, 1);
    test_failure_case(2u, MODEL_REMOTE, 1u, 3u, 3u, 1u, 1);
    test_failure_case(3u, MODEL_REMOTE, 1u, 3u, 3u, 1u, 1);

    /* An uncertain written start still gets one UID10 stop attempt. */
    test_failure_case(1u, MODEL_TRANSPORT, 1u, 3u, 3u, 1u, 1);
    test_failure_case(2u, MODEL_TRANSPORT, 1u, 3u, 3u, 1u, 1);
    test_failure_case(3u, MODEL_TRANSPORT, 1u, 3u, 3u, 1u, 1);
    /* A refusal before write does not cause a spurious ScanStop. */
    test_failure_case(1u, MODEL_REFUSE, 0u, 3u, 3u, 0u, 1);
    test_failure_case(2u, MODEL_REFUSE, 0u, 3u, 3u, 1u, 1);
    test_failure_case(3u, MODEL_REFUSE, 0u, 3u, 3u, 1u, 1);
    test_failure_case(1u, MODEL_MALFORMED, 1u, 3u, 3u, 1u, 0);
    test_failure_case(2u, MODEL_MALFORMED, 1u, 3u, 3u, 1u, 0);

    /* Malformed, empty, and short positive-count records poison the matching
     * transport. Cleanup is still single-use, but its exchange is refused. */
    test_failure_case(3u, MODEL_MALFORMED, 1u, 3u, 3u, 1u, 0);
    test_failure_case(3u, MODEL_EMPTY_RECORDS, 1u, 3u, 3u, 1u, 0);
    test_failure_case(3u, MODEL_SHORT_RECORDS, 1u, 3u, 3u, 1u, 0);

    /* A failed cleanup status remains failure and is never retried. */
    {
        uint32_t ids[] = {286u, 288u, 287u};
        uint32_t uids[] = {7u, 8u, 10u};
        struct fixture f;
        uint8_t before[sizeof f.run];
        fixture_init(&f);
        f.model.ap_count = 0u;
        set_schedule(&f.model, ids, uids, 3u);
        set_fault(&f.model, 2u, MODEL_REMOTE, 1u); /* count fails */
        set_fault(&f.model, 3u, MODEL_REMOTE, 1u); /* stop fails */
        CHECK(!p4_hosted_scan_sequence_run(&f.run, &f.startup, &f.fifo,
            TEST_GENERATION, &f.rx, &f.tx, 0u, f.buffer, sizeof f.buffer));
        CHECK(!p4_hosted_scan_sequence_cleanup(&f.run, &f.startup, &f.fifo,
            TEST_GENERATION, &f.rx, &f.tx, 0u, f.buffer, sizeof f.buffer));
        CHECK(f.run.stop.status_valid == 1u && f.run.stop.status == 1);
        CHECK(f.run.stop.error == P4_HOSTED_CONTROL_REMOTE);
        memcpy(before, &f.run, sizeof before);
        CHECK(!p4_hosted_scan_sequence_cleanup(&f.run, &f.startup, &f.fifo,
            TEST_GENERATION, &f.rx, &f.tx, 0u, f.buffer, sizeof f.buffer));
        CHECK(memcmp(before, &f.run, sizeof before) == 0);
        expect_schedule(&f, 3u, (1u << 7) | (1u << 8) | (1u << 10));
    }
}

static void assert_run_refusal(struct fixture *f,
    const struct p4_hosted_startup *startup, struct p4_hosted_fifo *fifo,
    const struct p4_hosted_open_ops *rx, const struct p4_hosted_tx_ops *tx,
    uint32_t generation, uint8_t *buffer, size_t capacity)
{
    uint8_t run_before[sizeof f->run];
    uint8_t fifo_before[sizeof f->fifo];
    uint8_t buffer_before[TEST_CAPACITY];
    memcpy(run_before, &f->run, sizeof run_before);
    memcpy(fifo_before, &f->fifo, sizeof fifo_before);
    memcpy(buffer_before, f->buffer, sizeof buffer_before);
    CHECK(!p4_hosted_scan_sequence_run(&f->run, startup, fifo, generation,
        rx, tx, 0u, buffer, capacity));
    CHECK(memcmp(run_before, &f->run, sizeof run_before) == 0);
    CHECK(memcmp(fifo_before, &f->fifo, sizeof fifo_before) == 0);
    CHECK(memcmp(buffer_before, f->buffer, sizeof buffer_before) == 0);
    CHECK(f->model.calls == 0u);
}

static void test_run_preflight_refusals(void)
{
    struct fixture f;
    struct p4_hosted_startup bad_startup;
    uintptr_t near_end = UINTPTR_MAX - 1u;
    uint8_t run_before[sizeof f.run];
    uint8_t fifo_before[sizeof f.fifo];
    uint8_t buffer_before[TEST_CAPACITY];

    fixture_init(&f);
    memcpy(run_before, &f.run, sizeof run_before);
    memcpy(fifo_before, &f.fifo, sizeof fifo_before);
    memcpy(buffer_before, f.buffer, sizeof buffer_before);
    CHECK(!p4_hosted_scan_sequence_run(NULL, &f.startup, &f.fifo,
        TEST_GENERATION, &f.rx, &f.tx, 0u, f.buffer, sizeof f.buffer));
    CHECK(memcmp(run_before, &f.run, sizeof run_before) == 0);
    CHECK(memcmp(fifo_before, &f.fifo, sizeof fifo_before) == 0);
    CHECK(memcmp(buffer_before, f.buffer, sizeof buffer_before) == 0);
    assert_run_refusal(&f, NULL, &f.fifo, &f.rx, &f.tx, TEST_GENERATION,
                       f.buffer, sizeof f.buffer);
    assert_run_refusal(&f, &f.startup, NULL, &f.rx, &f.tx, TEST_GENERATION,
                       f.buffer, sizeof f.buffer);
    assert_run_refusal(&f, &f.startup, &f.fifo, NULL, &f.tx, TEST_GENERATION,
                       f.buffer, sizeof f.buffer);
    assert_run_refusal(&f, &f.startup, &f.fifo, &f.rx, NULL, TEST_GENERATION,
                       f.buffer, sizeof f.buffer);
    assert_run_refusal(&f, &f.startup, &f.fifo, &f.rx, &f.tx, 0u,
                       f.buffer, sizeof f.buffer);
    assert_run_refusal(&f, &f.startup, &f.fifo, &f.rx, &f.tx,
                       TEST_GENERATION + 1u, f.buffer, sizeof f.buffer);
    assert_run_refusal(&f, &f.startup, &f.fifo, &f.rx, &f.tx,
                       TEST_GENERATION, NULL, sizeof f.buffer);
    assert_run_refusal(&f, &f.startup, &f.fifo, &f.rx, &f.tx,
                       TEST_GENERATION, f.buffer, 511u);
    assert_run_refusal(&f, &f.startup, &f.fifo, &f.rx, &f.tx,
                       TEST_GENERATION, f.buffer, 4097u);
    assert_run_refusal(&f, &f.startup, &f.fifo, &f.rx, &f.tx,
                       TEST_GENERATION, f.buffer, SIZE_MAX);
    assert_run_refusal(&f, &f.startup, &f.fifo, &f.rx, &f.tx,
                       TEST_GENERATION, (uint8_t *)(uintptr_t)near_end,
                       TEST_CAPACITY);
    assert_run_refusal(&f, (const struct p4_hosted_startup *)(uintptr_t)near_end,
                       &f.fifo, &f.rx, &f.tx, TEST_GENERATION,
                       f.buffer, TEST_CAPACITY);
    assert_run_refusal(&f, &f.startup,
                       (struct p4_hosted_fifo *)(uintptr_t)near_end,
                       &f.rx, &f.tx, TEST_GENERATION,
                       f.buffer, TEST_CAPACITY);
    assert_run_refusal(&f, &f.startup, &f.fifo, &f.rx, &f.tx,
                       TEST_GENERATION, (uint8_t *)(void *)&f.run,
                       TEST_CAPACITY);
    assert_run_refusal(&f, &f.startup,
                       (struct p4_hosted_fifo *)(void *)&f.startup,
                       &f.rx, &f.tx, TEST_GENERATION,
                       f.buffer, TEST_CAPACITY);
    assert_run_refusal(&f, &f.startup, &f.fifo, &f.rx,
                       (const struct p4_hosted_tx_ops *)(void *)&f.rx,
                       TEST_GENERATION, f.buffer, TEST_CAPACITY);
    assert_run_refusal(&f, (const struct p4_hosted_startup *)(void *)&f.run,
                       &f.fifo, &f.rx, &f.tx, TEST_GENERATION,
                       f.buffer, TEST_CAPACITY);
    assert_run_refusal(&f, &f.startup,
                       (struct p4_hosted_fifo *)(void *)&f.run,
                       &f.rx, &f.tx, TEST_GENERATION,
                       f.buffer, TEST_CAPACITY);
    assert_run_refusal(&f, &f.startup, &f.fifo,
                       (const struct p4_hosted_open_ops *)(void *)&f.run,
                       &f.tx, TEST_GENERATION, f.buffer, TEST_CAPACITY);
    assert_run_refusal(&f, &f.startup, &f.fifo, &f.rx,
                       (const struct p4_hosted_tx_ops *)(void *)&f.run,
                       TEST_GENERATION, f.buffer, TEST_CAPACITY);

    memcpy(&bad_startup, &f.startup, sizeof bad_startup);
    bad_startup.success = 0u;
    assert_run_refusal(&f, &bad_startup, &f.fifo, &f.rx, &f.tx,
                       TEST_GENERATION, f.buffer, TEST_CAPACITY);
    memcpy(&bad_startup, &f.startup, sizeof bad_startup);
    bad_startup.entered = 0u;
    assert_run_refusal(&f, &bad_startup, &f.fifo, &f.rx, &f.tx,
                       TEST_GENERATION, f.buffer, TEST_CAPACITY);
    memcpy(&bad_startup, &f.startup, sizeof bad_startup);
    bad_startup.generation = TEST_GENERATION + 1u;
    assert_run_refusal(&f, &bad_startup, &f.fifo, &f.rx, &f.tx,
                       TEST_GENERATION, f.buffer, TEST_CAPACITY);
    memcpy(&bad_startup, &f.startup, sizeof bad_startup);
    bad_startup.cleanup_entered = 1u;
    assert_run_refusal(&f, &bad_startup, &f.fifo, &f.rx, &f.tx,
                       TEST_GENERATION, f.buffer, TEST_CAPACITY);
    f.fifo.generation = TEST_GENERATION + 1u;
    assert_run_refusal(&f, &f.startup, &f.fifo, &f.rx, &f.tx,
                       TEST_GENERATION, f.buffer, TEST_CAPACITY);
}

static void assert_cleanup_refusal(struct fixture *f,
    const struct p4_hosted_startup *startup, struct p4_hosted_fifo *fifo,
    const struct p4_hosted_open_ops *rx, const struct p4_hosted_tx_ops *tx,
    uint32_t generation, uint8_t *buffer, size_t capacity)
{
    uint8_t run_before[sizeof f->run];
    uint8_t fifo_before[sizeof f->fifo];
    uint8_t buffer_before[TEST_CAPACITY];
    memcpy(run_before, &f->run, sizeof run_before);
    memcpy(fifo_before, &f->fifo, sizeof fifo_before);
    memcpy(buffer_before, f->buffer, sizeof buffer_before);
    CHECK(!p4_hosted_scan_sequence_cleanup(&f->run, startup, fifo,
        generation, rx, tx, 0u, buffer, capacity));
    CHECK(memcmp(run_before, &f->run, sizeof run_before) == 0);
    CHECK(memcmp(fifo_before, &f->fifo, sizeof fifo_before) == 0);
    CHECK(memcmp(buffer_before, f->buffer, sizeof buffer_before) == 0);
}

static void test_cleanup_preflight_refusals(void)
{
    uint32_t ids[] = {286u};
    uint32_t uids[] = {7u};
    struct fixture f;
    uintptr_t near_end = UINTPTR_MAX - 1u;
    uint8_t run_before[sizeof f.run];
    uint8_t fifo_before[sizeof f.fifo];
    uint8_t buffer_before[sizeof f.buffer];

    fixture_init(&f);
    memcpy(run_before, &f.run, sizeof run_before);
    memcpy(fifo_before, &f.fifo, sizeof fifo_before);
    memcpy(buffer_before, f.buffer, sizeof buffer_before);
    CHECK(!p4_hosted_scan_sequence_cleanup(&f.run, &f.startup, &f.fifo,
        TEST_GENERATION, &f.rx, &f.tx, 0u, f.buffer, sizeof f.buffer));
    CHECK(memcmp(run_before, &f.run, sizeof run_before) == 0);
    CHECK(memcmp(fifo_before, &f.fifo, sizeof fifo_before) == 0);
    CHECK(memcmp(buffer_before, f.buffer, sizeof buffer_before) == 0);
    CHECK(!p4_hosted_scan_sequence_cleanup(NULL, &f.startup, &f.fifo,
        TEST_GENERATION, &f.rx, &f.tx, 0u, f.buffer, sizeof f.buffer));
    set_schedule(&f.model, ids, uids, 1u);
    set_fault(&f.model, 1u, MODEL_TRANSPORT, 1u);
    CHECK(!p4_hosted_scan_sequence_run(&f.run, &f.startup, &f.fifo,
        TEST_GENERATION, &f.rx, &f.tx, 0u, f.buffer, sizeof f.buffer));
    CHECK(f.run.start.write_called == 1u && f.run.entered == 1u);

    assert_cleanup_refusal(&f, NULL, &f.fifo, &f.rx, &f.tx, TEST_GENERATION,
                           f.buffer, sizeof f.buffer);
    assert_cleanup_refusal(&f, &f.startup, NULL, &f.rx, &f.tx,
                           TEST_GENERATION, f.buffer, sizeof f.buffer);
    assert_cleanup_refusal(&f, &f.startup, &f.fifo, NULL, &f.tx,
                           TEST_GENERATION, f.buffer, sizeof f.buffer);
    assert_cleanup_refusal(&f, &f.startup, &f.fifo, &f.rx, NULL,
                           TEST_GENERATION, f.buffer, sizeof f.buffer);
    assert_cleanup_refusal(&f, &f.startup, &f.fifo, &f.rx, &f.tx, 0u,
                           f.buffer, sizeof f.buffer);
    assert_cleanup_refusal(&f, &f.startup, &f.fifo, &f.rx, &f.tx,
                           TEST_GENERATION + 1u, f.buffer, sizeof f.buffer);
    assert_cleanup_refusal(&f, &f.startup, &f.fifo, &f.rx, &f.tx,
                           TEST_GENERATION, NULL, sizeof f.buffer);
    assert_cleanup_refusal(&f, &f.startup, &f.fifo, &f.rx, &f.tx,
                           TEST_GENERATION, f.buffer, 511u);
    assert_cleanup_refusal(&f, &f.startup, &f.fifo, &f.rx, &f.tx,
                           TEST_GENERATION, f.buffer, SIZE_MAX);
    assert_cleanup_refusal(&f, &f.startup, &f.fifo, &f.rx, &f.tx,
                           TEST_GENERATION,
                           (uint8_t *)(uintptr_t)near_end, TEST_CAPACITY);
    assert_cleanup_refusal(&f, (const struct p4_hosted_startup *)(uintptr_t)near_end,
                           &f.fifo, &f.rx, &f.tx, TEST_GENERATION,
                           f.buffer, TEST_CAPACITY);
    assert_cleanup_refusal(&f, &f.startup,
                           (struct p4_hosted_fifo *)(uintptr_t)near_end,
                           &f.rx, &f.tx, TEST_GENERATION,
                           f.buffer, TEST_CAPACITY);
    assert_cleanup_refusal(&f, &f.startup, &f.fifo, &f.rx, &f.tx,
                           TEST_GENERATION, (uint8_t *)(void *)&f.run,
                           TEST_CAPACITY);
    assert_cleanup_refusal(&f, (const struct p4_hosted_startup *)(void *)&f.run,
                           &f.fifo, &f.rx, &f.tx, TEST_GENERATION,
                           f.buffer, TEST_CAPACITY);
    assert_cleanup_refusal(&f, &f.startup,
                           (struct p4_hosted_fifo *)(void *)&f.run,
                           &f.rx, &f.tx, TEST_GENERATION,
                           f.buffer, TEST_CAPACITY);

    f.startup.cleanup_entered = 1u;
    assert_cleanup_refusal(&f, &f.startup, &f.fifo, &f.rx, &f.tx,
                           TEST_GENERATION, f.buffer, TEST_CAPACITY);
    CHECK(f.model.calls == 1u);
    CHECK(f.model.uid_mask == (1u << 7));
    CHECK(f.model.contract_error == 0u);
}

static void test_generation_replacement_preserved(void)
{
    uint32_t ids[] = {286u, 288u};
    uint32_t uids[] = {7u, 8u};
    struct fixture f;
    uint8_t replacement_before[sizeof f.fifo];
    fixture_init(&f);
    set_schedule(&f.model, ids, uids, 2u);
    f.model.replace_after_call = 1u;

    CHECK(!p4_hosted_scan_sequence_run(&f.run, &f.startup, &f.fifo,
        TEST_GENERATION, &f.rx, &f.tx, 0u, f.buffer, sizeof f.buffer));
    CHECK(f.run.start.write_called == 1u);
    CHECK(f.run.count.error == P4_HOSTED_CONTROL_SESSION);
    CHECK(f.fifo.generation == TEST_GENERATION + 1u);
    CHECK(f.fifo.poisoned == 0u);
    memcpy(replacement_before, &f.fifo, sizeof replacement_before);
    CHECK(!p4_hosted_scan_sequence_cleanup(&f.run, &f.startup, &f.fifo,
        TEST_GENERATION, &f.rx, &f.tx, 0u, f.buffer, sizeof f.buffer));
    CHECK(memcmp(replacement_before, &f.fifo, sizeof replacement_before) == 0);
    expect_schedule(&f, 2u, (1u << 7) | (1u << 8));
}

static void test_startup_and_single_use_guards(void)
{
    struct fixture f;
    uint8_t run_before[sizeof f.run];
    uint8_t fifo_before[sizeof f.fifo];
    uint8_t buffer_before[sizeof f.buffer];
    fixture_init(&f);
    f.startup.success = 0u;
    memcpy(run_before, &f.run, sizeof run_before);
    memcpy(fifo_before, &f.fifo, sizeof fifo_before);
    memcpy(buffer_before, f.buffer, sizeof buffer_before);
    CHECK(!p4_hosted_scan_sequence_run(&f.run, &f.startup, &f.fifo,
        TEST_GENERATION, &f.rx, &f.tx, 0u, f.buffer, sizeof f.buffer));
    CHECK(memcmp(run_before, &f.run, sizeof run_before) == 0);
    CHECK(memcmp(fifo_before, &f.fifo, sizeof fifo_before) == 0);
    CHECK(memcmp(buffer_before, f.buffer, sizeof buffer_before) == 0);

    fixture_init(&f);
    f.model.ap_count = 0u;
    {
        uint32_t ids[] = {286u, 288u};
        uint32_t uids[] = {7u, 8u};
        uint8_t saved_run[sizeof f.run];
        set_schedule(&f.model, ids, uids, 2u);
        CHECK(p4_hosted_scan_sequence_run(&f.run, &f.startup, &f.fifo,
            TEST_GENERATION, &f.rx, &f.tx, 0u, f.buffer, sizeof f.buffer));
        memcpy(saved_run, &f.run, sizeof saved_run);
        memcpy(fifo_before, &f.fifo, sizeof fifo_before);
        CHECK(!p4_hosted_scan_sequence_run(&f.run, &f.startup, &f.fifo,
            TEST_GENERATION, &f.rx, &f.tx, 0u, f.buffer, sizeof f.buffer));
        CHECK(memcmp(saved_run, &f.run, sizeof saved_run) == 0);
        CHECK(memcmp(fifo_before, &f.fifo, sizeof fifo_before) == 0);
        CHECK(p4_hosted_scan_sequence_cleanup(&f.run, &f.startup, &f.fifo,
            TEST_GENERATION, &f.rx, &f.tx, 0u, f.buffer, sizeof f.buffer));
        memcpy(saved_run, &f.run, sizeof saved_run);
        CHECK(!p4_hosted_scan_sequence_cleanup(&f.run, &f.startup, &f.fifo,
            TEST_GENERATION, &f.rx, &f.tx, 0u, f.buffer, sizeof f.buffer));
        CHECK(memcmp(saved_run, &f.run, sizeof saved_run) == 0);
        expect_schedule(&f, 2u, (1u << 7) | (1u << 8));
    }
}

int main(void)
{
    test_success_and_owned_records();
    test_failures_and_cleanup();
    test_run_preflight_refusals();
    test_cleanup_preflight_refusals();
    test_generation_replacement_preserved();
    test_startup_and_single_use_guards();
    printf("hosted_scan_sequence: %lu checks\n", checks);
    return 0;
}
