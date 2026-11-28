/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
/* Host-model contract tests only. No hardware, timing, or firmware claim. */
#include "hosted_startup.h"
#include "hosted_rpc.h"
#include "hosted_fifo.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_GENERATION UINT32_C(0x360)
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
    MODEL_MALFORMED
};

struct model_fault {
    uint32_t request_id;
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
    unsigned callback_calls;
    unsigned deadline_epoch;
    struct model_fault faults[2];
    unsigned fault_count;
    unsigned contract_error;
};

struct fixture {
    struct p4_hosted_startup run;
    struct p4_hosted_fifo fifo;
    struct p4_hosted_open_ops rx;
    struct p4_hosted_tx_ops tx;
    struct p4_hosted_wifi_config config;
    uint8_t buffer[TEST_CAPACITY];
    struct model model;
};

static int callback(void *context)
{
    struct model *model = (struct model *)context;
    ++model->callback_calls;
    return 1;
}

static int open_read_byte(void *context, unsigned function, uint32_t address,
                          uint8_t *value)
{
    (void)function; (void)address; (void)value;
    return callback(context);
}

static int open_write_byte(void *context, unsigned function, uint32_t address,
                           uint8_t value)
{
    (void)function; (void)address; (void)value;
    return callback(context);
}

static int open_read_word(void *context, uint32_t address, uint32_t *value)
{
    (void)address; (void)value;
    return callback(context);
}

static int open_write_word(void *context, uint32_t address, uint32_t value)
{
    (void)address; (void)value;
    return callback(context);
}

static int open_read_fifo(void *context,
                          const struct p4_hosted_fifo_plan *plan,
                          uint8_t *buffer)
{
    (void)plan; (void)buffer;
    return callback(context);
}

static int tx_read_word(void *context, uint32_t address, uint32_t *value)
{
    (void)address; (void)value;
    return callback(context);
}

static int tx_write_fifo(void *context,
                         const struct p4_hosted_fifo_plan *plan,
                         const uint8_t *buffer,
                         struct p4_hosted_tx_completion *completion)
{
    (void)plan; (void)buffer; (void)completion;
    return callback(context);
}

static int tx_session(void *context, uint32_t generation)
{
    (void)generation;
    return callback(context);
}

static void yield_callback(void *context)
{
    (void)callback(context);
}

static void fixture_init(struct fixture *f)
{
    memset(f, 0, sizeof *f);
    f->model.generation = TEST_GENERATION;
    f->model.deadline_epoch = 1;
    CHECK(p4_hosted_fifo_start(&f->fifo, TEST_GENERATION, 20));
    f->rx.context = &f->model;
    f->rx.read_byte = open_read_byte;
    f->rx.write_byte = open_write_byte;
    f->rx.read_word = open_read_word;
    f->rx.write_word = open_write_word;
    f->rx.read_fifo = open_read_fifo;
    f->rx.budget_ok = callback;
    f->rx.yield = yield_callback;
    f->tx.context = &f->model;
    f->tx.read_word = tx_read_word;
    f->tx.write_fifo = tx_write_fifo;
    f->tx.session_ok = tx_session;
    f->tx.budget_ok = callback;
    f->tx.yield = yield_callback;
    memset(f->buffer, 0xa5, sizeof f->buffer);
}

static void set_schedule(struct model *m, const uint32_t *ids,
                         const uint32_t *uids, unsigned count)
{
    unsigned i;
    CHECK(count <= MAX_CALLS);
    m->expected_count = count;
    m->calls = 0;
    m->uid_mask = 0;
    m->contract_error = 0;
    for (i = 0; i < count; ++i) {
        m->expected_ids[i] = ids[i];
        m->expected_uids[i] = uids[i];
    }
}

static void add_fault(struct model *m, uint32_t id,
                      enum model_outcome outcome, unsigned write_called)
{
    CHECK(m->fault_count < 2u);
    m->faults[m->fault_count].request_id = id;
    m->faults[m->fault_count].outcome = outcome;
    m->faults[m->fault_count].write_called = write_called;
    ++m->fault_count;
}

static size_t make_status_body(uint8_t *body, enum model_outcome outcome)
{
    if (outcome == MODEL_MALFORMED) {
        body[0] = 0x08; body[1] = 0x00;
        body[2] = 0x08; body[3] = 0x00;
        return 4;
    }
    body[0] = 0x08;
    body[1] = outcome == MODEL_REMOTE ? 1u : 0u;
    return 2;
}

static size_t make_mac_body(uint8_t *body, enum model_outcome outcome)
{
    static const uint8_t address[6] = { 0x02, 0x11, 0x22, 0x33, 0x44, 0x55 };
    if (outcome == MODEL_MALFORMED) {
        body[0] = 0x10; body[1] = 0;
        body[2] = 0x10; body[3] = 0;
        return 4;
    }
    body[0] = 0x0a; body[1] = 6;
    memcpy(body + 2, address, sizeof address);
    body[8] = 0x10;
    body[9] = outcome == MODEL_REMOTE ? 1u : 0u;
    return 10;
}

static int request_payload_matches(const struct p4_hosted_rpc *rpc)
{
    static const uint8_t init_body[] = {
        0x0a, 0x2d,
        0x08, 0, 0x10, 0, 0x18, 0, 0x20, 0, 0x28, 0, 0x30, 0,
        0x38, 0, 0x40, 0, 0x48, 0, 0x50, 0, 0x58, 0, 0x60, 0,
        0x68, 0, 0x70, 0, 0x78, 0, 0x80, 0x01, 0,
        0x88, 0x01, 0, 0x90, 0x01, 0, 0x98, 0x01, 0,
        0xa0, 0x01, 0
    };
    static const uint8_t mode_body[] = { 0x08, 0x01 };
    const uint8_t *wanted = NULL;
    size_t wanted_size = 0;

    switch (rpc->msg_id) {
    case 278:
        wanted = init_body; wanted_size = sizeof init_body;
        break;
    case 260:
        wanted = mode_body; wanted_size = sizeof mode_body;
        break;
    case 257: case 279: case 280: case 281:
        wanted_size = 0;
        break;
    default:
        return 0;
    }
    return rpc->payload_size == wanted_size &&
        (wanted_size == 0 || memcmp(rpc->payload, wanted, wanted_size) == 0);
}

static int next_fault(struct model *m, uint32_t id, struct model_fault *fault)
{
    unsigned i;
    for (i = 0; i < m->fault_count; ++i) {
        if (m->faults[i].request_id == id) {
            *fault = m->faults[i];
            return 1;
        }
    }
    fault->request_id = id;
    fault->outcome = MODEL_OK;
    fault->write_called = 1;
    return 0;
}

static int record_request(struct model *m, const uint8_t *request,
                          size_t request_size, uint32_t id, uint32_t uid)
{
    struct p4_hosted_rpc parsed;
    unsigned index = m->calls;
    if (index >= m->expected_count) {
        m->contract_error = 1;
        return 0;
    }
    if (!p4_hosted_rpc_decode(request, request_size, &parsed) ||
        parsed.msg_type != P4_HOSTED_RPC_TYPE_REQUEST ||
        parsed.msg_id != id || parsed.uid != uid ||
        !request_payload_matches(&parsed) ||
        m->expected_ids[index] != id || m->expected_uids[index] != uid ||
        uid == 0 || uid > 6 || (m->uid_mask & (1u << uid)) != 0) {
        m->contract_error = 1;
        return 0;
    }
    m->uid_mask |= 1u << uid;
    ++m->calls;
    return 1;
}

static int model_exchange(struct p4_hosted_fifo *state, uint32_t generation,
    const struct p4_hosted_open_ops *rx, const struct p4_hosted_tx_ops *tx,
    const uint8_t *request, size_t request_size, uint8_t *buffer,
    size_t capacity, struct p4_hosted_control_result *result,
    int get_mac_call)
{
    struct model *m;
    struct p4_hosted_rpc parsed, response, decoded;
    struct model_fault fault;
    uint8_t response_wire[512], body[32];
    size_t response_size, body_size;
    uint32_t id, uid;

    if (!rx || !tx || rx->context != tx->context || !result ||
        !p4_hosted_rpc_decode(request, request_size, &parsed))
        return 0;
    m = (struct model *)rx->context;
    id = parsed.msg_id;
    uid = parsed.uid;
    if (generation != m->generation ||
        (get_mac_call && id != 257u) ||
        !record_request(m, request, request_size, id, uid)) {
        m->contract_error = 1;
        return 0;
    }
    /* Mirror hosted_control_exchange preflight: invalid/stale/busy/poisoned
     * ledgers refuse before touching the caller's result or any callback. */
    if (state->initialized != 1 || state->generation != generation ||
        state->busy || state->poisoned || state->pool_limit == 0 ||
        state->pool_limit > P4_HOSTED_TX_MASK)
        return 0;
    (void)next_fault(m, id, &fault);
    if (fault.outcome == MODEL_REFUSE) {
        result->error = P4_HOSTED_CONTROL_OK;
        result->tx.write_called = fault.write_called;
        return 0;
    }
    if (fault.outcome == MODEL_TRANSPORT) {
        result->error = P4_HOSTED_CONTROL_TIMEOUT;
        result->tx.write_called = fault.write_called;
        return 0;
    }
    result->tx.write_called = 1;
    body_size = get_mac_call ? make_mac_body(body, fault.outcome) :
                               make_status_body(body, fault.outcome);
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
        m->contract_error = 1;
        return 0;
    }
    if (decoded.payload_size != 0)
        memcpy(result->response_body, decoded.payload, decoded.payload_size);
    result->response_size = decoded.payload_size;
    result->response_id = decoded.msg_id;
    result->response_uid = uid;
    if (get_mac_call) {
        struct p4_hosted_rpc mac_rpc = decoded;
        mac_rpc.payload = result->response_body;
        if (!p4_hosted_mac_response(&mac_rpc, uid, &result->mac)) {
            result->error = P4_HOSTED_CONTROL_PROTOCOL;
            state->poisoned = 1;
            return 0;
        }
        if (result->mac.status != 0) {
            result->error = P4_HOSTED_CONTROL_REMOTE;
            return 0;
        }
        result->error = P4_HOSTED_CONTROL_OK;
        result->success = 1;
        return result->mac.valid_mac != 0;
    }
    if (buffer == NULL || capacity < TEST_CAPACITY) {
        m->contract_error = 1;
        return 0;
    }
    result->error = P4_HOSTED_CONTROL_OK;
    return 1;
}

int p4_hosted_control_exchange(struct p4_hosted_fifo *state,
    uint32_t generation, const struct p4_hosted_open_ops *rx,
    const struct p4_hosted_tx_ops *tx, unsigned checksum,
    const uint8_t *request, size_t request_size, uint8_t *buffer,
    size_t capacity, struct p4_hosted_control_result *result)
{
    (void)checksum;
    return model_exchange(state, generation, rx, tx, request, request_size,
                          buffer, capacity, result, 0);
}

int p4_hosted_control_get_mac(struct p4_hosted_fifo *state,
    uint32_t generation, const struct p4_hosted_open_ops *rx,
    const struct p4_hosted_tx_ops *tx, unsigned checksum, uint32_t uid,
    uint8_t *buffer, size_t capacity,
    struct p4_hosted_control_result *result)
{
    uint8_t request_bytes[64];
    size_t request_size = 0;
    (void)checksum;
    if (!p4_hosted_mac_request(uid, request_bytes, sizeof request_bytes,
                               &request_size))
        return 0;
    return model_exchange(state, generation, rx, tx, request_bytes,
                          request_size, buffer, capacity, result, 1);
}

static void assert_schedule(const struct fixture *f, unsigned calls,
                           unsigned uid_mask)
{
    CHECK(f->model.calls == calls);
    CHECK(f->model.expected_count == calls);
    CHECK(f->model.uid_mask == uid_mask);
    CHECK(f->model.contract_error == 0);
    CHECK(f->model.callback_calls == 0);
    CHECK(f->model.deadline_epoch == 1);
}

static void test_success_and_single_use(void)
{
    static const uint32_t ids[] = { 278, 260, 280, 257, 281, 279 };
    static const uint32_t uids[] = { 1, 2, 3, 4, 5, 6 };
    struct fixture f;
    struct p4_hosted_startup saved;
    struct p4_hosted_fifo saved_fifo;
    fixture_init(&f);
    set_schedule(&f.model, ids, uids, 6);
    CHECK(p4_hosted_startup_run(&f.run, &f.config, &f.fifo,
        TEST_GENERATION, &f.rx, &f.tx, 0, f.buffer, sizeof f.buffer));
    CHECK(f.run.entered && f.run.success && f.run.mac_attempted);
    CHECK(f.run.init.success && f.run.mode.success && f.run.start.success);
    CHECK(f.run.mac.mac.valid_mac == 1);
    CHECK(f.run.mac.mac.mac[0] == 0x02 && f.run.mac.mac.mac[5] == 0x55);
    CHECK(p4_hosted_startup_cleanup(&f.run, &f.fifo, TEST_GENERATION,
        &f.rx, &f.tx, 0, f.buffer, sizeof f.buffer));
    CHECK(f.run.cleanup_success && f.run.stop.success && f.run.deinit.success);
    assert_schedule(&f, 6, 0x7eu);
    saved = f.run;
    saved_fifo = f.fifo;
    CHECK(!p4_hosted_startup_run(&f.run, &f.config, &f.fifo,
        TEST_GENERATION, &f.rx, &f.tx, 0, f.buffer, sizeof f.buffer));
    CHECK(memcmp(&saved, &f.run, sizeof saved) == 0);
    CHECK(memcmp(&saved_fifo, &f.fifo, sizeof saved_fifo) == 0);
    CHECK(!p4_hosted_startup_cleanup(&f.run, &f.fifo, TEST_GENERATION,
        &f.rx, &f.tx, 0, f.buffer, sizeof f.buffer));
    CHECK(memcmp(&saved, &f.run, sizeof saved) == 0);
    assert_schedule(&f, 6, 0x7eu);
}

static void test_run_preflight_unchanged(void)
{
    struct fixture f;
    struct p4_hosted_startup saved_run;
    struct p4_hosted_fifo saved_fifo;
    uint8_t saved_buffer[TEST_CAPACITY];
    uintptr_t near_end = UINTPTR_MAX - 1u;
    fixture_init(&f);
    saved_run = f.run; saved_fifo = f.fifo;
    memcpy(saved_buffer, f.buffer, sizeof saved_buffer);
#define CHECK_RUN_REFUSAL(config_arg, fifo_arg, rx_arg, tx_arg, buffer_arg, cap_arg) do { \
    CHECK(!p4_hosted_startup_run(&f.run, (config_arg), (fifo_arg), \
        TEST_GENERATION, (rx_arg), (tx_arg), 0, (buffer_arg), (cap_arg))); \
    CHECK(memcmp(&saved_run, &f.run, sizeof saved_run) == 0); \
    CHECK(memcmp(&saved_fifo, &f.fifo, sizeof saved_fifo) == 0); \
    CHECK(memcmp(saved_buffer, f.buffer, sizeof saved_buffer) == 0); \
    CHECK(f.model.calls == 0 && f.model.callback_calls == 0); \
} while (0)
    CHECK_RUN_REFUSAL(NULL, &f.fifo, &f.rx, &f.tx, f.buffer, sizeof f.buffer);
    CHECK_RUN_REFUSAL(&f.config, &f.fifo, &f.rx, &f.tx, f.buffer, 511u);
    CHECK_RUN_REFUSAL(&f.config, &f.fifo, &f.rx, &f.tx, f.buffer, 4097u);
    CHECK_RUN_REFUSAL(&f.config, &f.fifo, &f.rx, &f.tx, f.buffer, SIZE_MAX);
    CHECK_RUN_REFUSAL(&f.config, &f.fifo, &f.rx, &f.tx,
                      (uint8_t *)(uintptr_t)near_end, TEST_CAPACITY);
    CHECK_RUN_REFUSAL(&f.config, &f.fifo, &f.rx, &f.tx,
                      (uint8_t *)(void *)&f.run, TEST_CAPACITY);
    CHECK_RUN_REFUSAL((const struct p4_hosted_wifi_config *)(void *)&f.run,
                      &f.fifo, &f.rx, &f.tx, f.buffer, sizeof f.buffer);
    CHECK_RUN_REFUSAL(&f.config, &f.fifo, &f.rx, &f.tx,
                      (uint8_t *)(void *)&f.config, TEST_CAPACITY);
    CHECK_RUN_REFUSAL(&f.config, (struct p4_hosted_fifo *)(uintptr_t)near_end,
                      &f.rx, &f.tx, f.buffer, sizeof f.buffer);
    CHECK_RUN_REFUSAL(&f.config, &f.fifo,
                      (const struct p4_hosted_open_ops *)(uintptr_t)near_end,
                      &f.tx, f.buffer, sizeof f.buffer);
    CHECK_RUN_REFUSAL(&f.config, &f.fifo, &f.rx,
                      (const struct p4_hosted_tx_ops *)(uintptr_t)near_end,
                      f.buffer, sizeof f.buffer);
    CHECK_RUN_REFUSAL(&f.config, &f.fifo, &f.rx, &f.tx,
                      f.buffer, SIZE_MAX);
    CHECK(!p4_hosted_startup_run((struct p4_hosted_startup *)(uintptr_t)near_end,
        &f.config, &f.fifo, TEST_GENERATION, &f.rx, &f.tx, 0,
        f.buffer, sizeof f.buffer));
    CHECK(memcmp(&saved_run, &f.run, sizeof saved_run) == 0);
    CHECK(memcmp(&saved_fifo, &f.fifo, sizeof saved_fifo) == 0);
    CHECK(memcmp(saved_buffer, f.buffer, sizeof saved_buffer) == 0);
    CHECK(f.model.calls == 0 && f.model.callback_calls == 0);
#undef CHECK_RUN_REFUSAL
}

static void test_ledger_preflight_refusal(void)
{
    static const uint32_t ids[] = { 278 };
    static const uint32_t uids[] = { 1 };
    struct fixture f;
    struct p4_hosted_fifo saved_fifo;
    unsigned mode;

    for (mode = 0; mode < 3u; ++mode) {
        fixture_init(&f);
        set_schedule(&f.model, ids, uids, 1);
        if (mode == 0)
            f.fifo.generation = TEST_GENERATION + 1u;
        else if (mode == 1)
            f.fifo.busy = 1;
        else
            f.fifo.poisoned = 1;
        saved_fifo = f.fifo;
        CHECK(!p4_hosted_startup_run(&f.run, &f.config, &f.fifo,
            TEST_GENERATION, &f.rx, &f.tx, 0, f.buffer, sizeof f.buffer));
        CHECK(f.run.entered && f.run.init.attempted &&
              f.run.init.error == P4_HOSTED_CONTROL_PREFLIGHT &&
              f.run.init.write_called == 0 && !f.run.mac_attempted);
        CHECK(f.run.scratch.error == P4_HOSTED_CONTROL_OK &&
              f.run.scratch.tx.write_called == 0 &&
              f.run.scratch.response_size == 0 &&
              f.run.scratch.response_id == 0 &&
              f.run.scratch.response_uid == 0);
        CHECK(memcmp(&saved_fifo, &f.fifo, sizeof saved_fifo) == 0);
        CHECK(f.model.uid_mask == 0x02u);
        CHECK(f.model.calls == 1 && f.model.callback_calls == 0);
        if (mode == 0) {
            struct p4_hosted_startup saved_run = f.run;
            CHECK(!p4_hosted_startup_cleanup(&f.run, &f.fifo,
                TEST_GENERATION, &f.rx, &f.tx, 0,
                f.buffer, sizeof f.buffer));
            CHECK(memcmp(&saved_run, &f.run, sizeof saved_run) == 0);
            CHECK(memcmp(&saved_fifo, &f.fifo, sizeof saved_fifo) == 0);
        } else {
            CHECK(p4_hosted_startup_cleanup(&f.run, &f.fifo,
                TEST_GENERATION, &f.rx, &f.tx, 0,
                f.buffer, sizeof f.buffer));
        }
        assert_schedule(&f, 1, 0x02u);
    }
}

static void prepare_failed_run(struct fixture *f, uint32_t failed_id,
    enum model_outcome outcome, unsigned write_called,
    const uint32_t *ids, const uint32_t *uids, unsigned call_count)
{
    fixture_init(f);
    set_schedule(&f->model, ids, uids, call_count);
    add_fault(&f->model, failed_id, outcome, write_called);
}

static void test_upstream_failures(void)
{
    static const uint32_t ids_init_nw[] = { 278 };
    static const uint32_t uids_init_nw[] = { 1 };
    static const uint32_t ids_init_written[] = { 278, 279 };
    static const uint32_t uids_init_written[] = { 1, 6 };
    static const uint32_t ids_mode[] = { 278, 260, 279 };
    static const uint32_t uids_mode[] = { 1, 2, 6 };
    static const uint32_t ids_start_nw[] = { 278, 260, 280, 279 };
    static const uint32_t uids_start_nw[] = { 1, 2, 3, 6 };
    static const uint32_t ids_start_written[] = { 278, 260, 280, 281, 279 };
    static const uint32_t uids_start_written[] = { 1, 2, 3, 5, 6 };
    struct fixture f;
    unsigned kind;

    for (kind = MODEL_REFUSE; kind <= MODEL_MALFORMED; ++kind) {
        const uint32_t *ids = kind == MODEL_REFUSE ? ids_init_nw : ids_init_written;
        const uint32_t *uids = kind == MODEL_REFUSE ? uids_init_nw : uids_init_written;
        unsigned count = kind == MODEL_REFUSE ? 1u : 2u;
        prepare_failed_run(&f, 278, (enum model_outcome)kind,
                           kind == MODEL_REFUSE ? 0u : 1u, ids, uids, count);
        CHECK(!p4_hosted_startup_run(&f.run, &f.config, &f.fifo,
            TEST_GENERATION, &f.rx, &f.tx, 0, f.buffer, sizeof f.buffer));
        CHECK(f.run.entered && !f.run.success && !f.run.mac_attempted);
        CHECK(f.run.init.attempted && !f.run.mode.attempted &&
              !f.run.start.attempted);
        CHECK(f.run.init.error == (kind == MODEL_REFUSE ?
              P4_HOSTED_CONTROL_PREFLIGHT : kind == MODEL_TRANSPORT ?
              P4_HOSTED_CONTROL_TIMEOUT : kind == MODEL_REMOTE ?
              P4_HOSTED_CONTROL_REMOTE : P4_HOSTED_CONTROL_PROTOCOL));
        if (kind == MODEL_MALFORMED) CHECK(f.fifo.poisoned);
        CHECK(p4_hosted_startup_cleanup(&f.run, &f.fifo, TEST_GENERATION,
            &f.rx, &f.tx, 0, f.buffer, sizeof f.buffer) !=
            (kind == MODEL_MALFORMED));
        /* The preceding call follows the exact write-called ledger: cleanup
         * either makes no request (no-write Init) or makes Deinit. */
        assert_schedule(&f, count, kind == MODEL_REFUSE ? 0x02u : 0x42u);
    }

    for (kind = MODEL_REFUSE; kind <= MODEL_MALFORMED; ++kind) {
        prepare_failed_run(&f, 260, (enum model_outcome)kind,
                           kind == MODEL_REFUSE ? 0u : 1u,
                           ids_mode, uids_mode, 3);
        CHECK(!p4_hosted_startup_run(&f.run, &f.config, &f.fifo,
            TEST_GENERATION, &f.rx, &f.tx, 0, f.buffer, sizeof f.buffer));
        CHECK(f.run.init.success && f.run.mode.attempted &&
              !f.run.start.attempted && !f.run.mac_attempted);
        CHECK(f.run.mode.error == (kind == MODEL_REFUSE ?
              P4_HOSTED_CONTROL_PREFLIGHT : kind == MODEL_TRANSPORT ?
              P4_HOSTED_CONTROL_TIMEOUT : kind == MODEL_REMOTE ?
              P4_HOSTED_CONTROL_REMOTE : P4_HOSTED_CONTROL_PROTOCOL));
        if (kind == MODEL_MALFORMED) CHECK(f.fifo.poisoned);
        CHECK(p4_hosted_startup_cleanup(&f.run, &f.fifo, TEST_GENERATION,
            &f.rx, &f.tx, 0, f.buffer, sizeof f.buffer) !=
            (kind == MODEL_MALFORMED));
        CHECK(f.run.cleanup_success != (kind == MODEL_MALFORMED));
        assert_schedule(&f, 3, 0x46u);
    }

    for (kind = MODEL_REFUSE; kind <= MODEL_MALFORMED; ++kind) {
        const uint32_t *ids = kind == MODEL_REFUSE ? ids_start_nw : ids_start_written;
        const uint32_t *uids = kind == MODEL_REFUSE ? uids_start_nw : uids_start_written;
        unsigned count = kind == MODEL_REFUSE ? 4u : 5u;
        prepare_failed_run(&f, 280, (enum model_outcome)kind,
                           kind == MODEL_REFUSE ? 0u : 1u, ids, uids, count);
        CHECK(!p4_hosted_startup_run(&f.run, &f.config, &f.fifo,
            TEST_GENERATION, &f.rx, &f.tx, 0, f.buffer, sizeof f.buffer));
        CHECK(f.run.init.success && f.run.mode.success && f.run.start.attempted &&
              !f.run.mac_attempted);
        CHECK(f.run.start.error == (kind == MODEL_REFUSE ?
              P4_HOSTED_CONTROL_PREFLIGHT : kind == MODEL_TRANSPORT ?
              P4_HOSTED_CONTROL_TIMEOUT : kind == MODEL_REMOTE ?
              P4_HOSTED_CONTROL_REMOTE : P4_HOSTED_CONTROL_PROTOCOL));
        if (kind == MODEL_MALFORMED) CHECK(f.fifo.poisoned);
        CHECK(p4_hosted_startup_cleanup(&f.run, &f.fifo, TEST_GENERATION,
            &f.rx, &f.tx, 0, f.buffer, sizeof f.buffer) !=
            (kind == MODEL_MALFORMED));
        CHECK(f.run.cleanup_success != (kind == MODEL_MALFORMED));
        assert_schedule(&f, count,
            kind == MODEL_REFUSE ? 0x4eu : 0x6eu);
    }
}

static void test_getmac_failures(void)
{
    static const uint32_t ids[] = { 278, 260, 280, 257, 281, 279 };
    static const uint32_t uids[] = { 1, 2, 3, 4, 5, 6 };
    struct fixture f;
    unsigned kind;
    for (kind = MODEL_REFUSE; kind <= MODEL_MALFORMED; ++kind) {
        fixture_init(&f);
        set_schedule(&f.model, ids, uids, 6);
        add_fault(&f.model, 257, (enum model_outcome)kind,
                  kind == MODEL_REFUSE ? 0u : 1u);
        CHECK(!p4_hosted_startup_run(&f.run, &f.config, &f.fifo,
            TEST_GENERATION, &f.rx, &f.tx, 0, f.buffer, sizeof f.buffer));
        CHECK(f.run.mac_attempted && !f.run.success);
        CHECK(f.run.init.success && f.run.mode.success && f.run.start.success);
        CHECK(f.run.mac.error == (kind == MODEL_REFUSE ?
              P4_HOSTED_CONTROL_PREFLIGHT : kind == MODEL_TRANSPORT ?
              P4_HOSTED_CONTROL_TIMEOUT : kind == MODEL_REMOTE ?
              P4_HOSTED_CONTROL_REMOTE : P4_HOSTED_CONTROL_PROTOCOL));
        if (kind == MODEL_MALFORMED) CHECK(f.fifo.poisoned);
        CHECK(p4_hosted_startup_cleanup(&f.run, &f.fifo, TEST_GENERATION,
            &f.rx, &f.tx, 0, f.buffer, sizeof f.buffer) !=
            (kind == MODEL_MALFORMED));
        CHECK(f.run.cleanup_success != (kind == MODEL_MALFORMED));
        assert_schedule(&f, 6, 0x7eu);
    }
}

static void test_cleanup_failures_and_bounded_deinit(void)
{
    static const uint32_t ids[] = { 278, 260, 280, 257, 281, 279 };
    static const uint32_t uids[] = { 1, 2, 3, 4, 5, 6 };
    struct fixture f;
    unsigned kind;
    for (kind = MODEL_REFUSE; kind <= MODEL_MALFORMED; ++kind) {
        fixture_init(&f);
        set_schedule(&f.model, ids, uids, 6);
        add_fault(&f.model, 281, (enum model_outcome)kind,
                  kind == MODEL_REFUSE ? 0u : 1u);
        CHECK(p4_hosted_startup_run(&f.run, &f.config, &f.fifo,
            TEST_GENERATION, &f.rx, &f.tx, 0, f.buffer, sizeof f.buffer));
        CHECK(!p4_hosted_startup_cleanup(&f.run, &f.fifo, TEST_GENERATION,
            &f.rx, &f.tx, 0, f.buffer, sizeof f.buffer));
        CHECK(f.run.stop.attempted && f.run.deinit.attempted);
        CHECK(!f.run.cleanup_success);
        if (kind == MODEL_REMOTE) {
            CHECK(f.run.stop.error == P4_HOSTED_CONTROL_REMOTE);
            CHECK(f.run.deinit.success); /* bounded Deinit follows Stop error */
        } else if (kind == MODEL_MALFORMED) {
            CHECK(f.run.stop.error == P4_HOSTED_CONTROL_PROTOCOL);
            CHECK(f.run.deinit.error == P4_HOSTED_CONTROL_PREFLIGHT);
            CHECK(f.fifo.poisoned);
        } else if (kind == MODEL_REFUSE) {
            CHECK(f.run.stop.error == P4_HOSTED_CONTROL_PREFLIGHT);
            CHECK(f.run.deinit.success);
        } else {
            CHECK(f.run.stop.error == P4_HOSTED_CONTROL_TIMEOUT);
            CHECK(f.run.deinit.success);
        }
        assert_schedule(&f, 6, 0x7eu);
    }

    fixture_init(&f);
    set_schedule(&f.model, ids, uids, 6);
    add_fault(&f.model, 279, MODEL_REMOTE, 1);
    CHECK(p4_hosted_startup_run(&f.run, &f.config, &f.fifo,
        TEST_GENERATION, &f.rx, &f.tx, 0, f.buffer, sizeof f.buffer));
    CHECK(!p4_hosted_startup_cleanup(&f.run, &f.fifo, TEST_GENERATION,
        &f.rx, &f.tx, 0, f.buffer, sizeof f.buffer));
    CHECK(f.run.stop.success && f.run.deinit.error == P4_HOSTED_CONTROL_REMOTE);
    assert_schedule(&f, 6, 0x7eu);

    for (kind = MODEL_REFUSE; kind <= MODEL_MALFORMED; ++kind) {
        fixture_init(&f);
        set_schedule(&f.model, ids, uids, 6);
        add_fault(&f.model, 279, (enum model_outcome)kind,
                  kind == MODEL_REFUSE ? 0u : 1u);
        CHECK(p4_hosted_startup_run(&f.run, &f.config, &f.fifo,
            TEST_GENERATION, &f.rx, &f.tx, 0, f.buffer, sizeof f.buffer));
        CHECK(!p4_hosted_startup_cleanup(&f.run, &f.fifo, TEST_GENERATION,
            &f.rx, &f.tx, 0, f.buffer, sizeof f.buffer));
        CHECK(f.run.stop.success && f.run.deinit.attempted);
        CHECK(f.run.deinit.error == (kind == MODEL_REFUSE ?
              P4_HOSTED_CONTROL_PREFLIGHT : kind == MODEL_TRANSPORT ?
              P4_HOSTED_CONTROL_TIMEOUT : kind == MODEL_REMOTE ?
              P4_HOSTED_CONTROL_REMOTE : P4_HOSTED_CONTROL_PROTOCOL));
        if (kind == MODEL_MALFORMED) CHECK(f.fifo.poisoned);
        assert_schedule(&f, 6, 0x7eu);
    }
}

static void test_cleanup_preflight_and_stale_replacement(void)
{
    static const uint32_t ids[] = { 278, 260 };
    static const uint32_t uids[] = { 1, 2 };
    struct fixture f;
    struct p4_hosted_startup saved_run;
    struct p4_hosted_fifo saved_fifo;
    uint8_t saved_buffer[TEST_CAPACITY];
    uintptr_t near_end = UINTPTR_MAX - 1u;

    fixture_init(&f);
    set_schedule(&f.model, ids, uids, 2);
    add_fault(&f.model, 260, MODEL_TRANSPORT, 0);
    CHECK(!p4_hosted_startup_run(&f.run, &f.config, &f.fifo,
        TEST_GENERATION, &f.rx, &f.tx, 0, f.buffer, sizeof f.buffer));
    saved_run = f.run; saved_fifo = f.fifo;
    memcpy(saved_buffer, f.buffer, sizeof saved_buffer);
#define CHECK_CLEANUP_REFUSAL(fifo_arg, rx_arg, tx_arg, buffer_arg, cap_arg) do { \
    CHECK(!p4_hosted_startup_cleanup(&f.run, (fifo_arg), TEST_GENERATION, \
        (rx_arg), (tx_arg), 0, (buffer_arg), (cap_arg))); \
    CHECK(memcmp(&saved_run, &f.run, sizeof saved_run) == 0); \
    CHECK(memcmp(&saved_fifo, &f.fifo, sizeof saved_fifo) == 0); \
    CHECK(memcmp(saved_buffer, f.buffer, sizeof saved_buffer) == 0); \
    CHECK(f.model.calls == 2 && f.model.callback_calls == 0); \
} while (0)
    CHECK_CLEANUP_REFUSAL(&f.fifo, &f.rx, &f.tx, f.buffer, 511u);
    CHECK_CLEANUP_REFUSAL(&f.fifo, &f.rx, &f.tx, f.buffer, SIZE_MAX);
    CHECK_CLEANUP_REFUSAL(&f.fifo,
        (const struct p4_hosted_open_ops *)(uintptr_t)near_end,
        &f.tx, f.buffer, sizeof f.buffer);
    CHECK_CLEANUP_REFUSAL(&f.fifo, &f.rx,
        (const struct p4_hosted_tx_ops *)(uintptr_t)near_end,
        f.buffer, sizeof f.buffer);
    CHECK_CLEANUP_REFUSAL(&f.fifo, &f.rx, &f.tx,
        (uint8_t *)(uintptr_t)near_end, sizeof f.buffer);
    CHECK_CLEANUP_REFUSAL(&f.fifo, &f.rx, &f.tx,
        (uint8_t *)(void *)&f.run, sizeof f.buffer);
#undef CHECK_CLEANUP_REFUSAL
    CHECK(!p4_hosted_startup_cleanup(&f.run, &f.fifo, TEST_GENERATION + 1u,
        &f.rx, &f.tx, 0, f.buffer, sizeof f.buffer));
    CHECK(memcmp(&saved_run, &f.run, sizeof saved_run) == 0);
    CHECK(memcmp(&saved_fifo, &f.fifo, sizeof saved_fifo) == 0);
    CHECK(memcmp(saved_buffer, f.buffer, sizeof saved_buffer) == 0);
    assert_schedule(&f, 2, 0x06u);

    fixture_init(&f);
    set_schedule(&f.model, ids, uids, 2);
    add_fault(&f.model, 260, MODEL_TRANSPORT, 0);
    CHECK(!p4_hosted_startup_run(&f.run, &f.config, &f.fifo,
        TEST_GENERATION, &f.rx, &f.tx, 0, f.buffer, sizeof f.buffer));
    saved_run = f.run;
    CHECK(p4_hosted_fifo_start(&f.fifo, TEST_GENERATION + 1u, 17));
    saved_fifo = f.fifo;
    memcpy(saved_buffer, f.buffer, sizeof saved_buffer);
    CHECK(!p4_hosted_startup_cleanup(&f.run, &f.fifo, TEST_GENERATION,
        &f.rx, &f.tx, 0, f.buffer, sizeof f.buffer));
    CHECK(memcmp(&saved_run, &f.run, sizeof saved_run) == 0);
    CHECK(memcmp(&saved_fifo, &f.fifo, sizeof saved_fifo) == 0);
    CHECK(memcmp(saved_buffer, f.buffer, sizeof saved_buffer) == 0);
    CHECK(f.model.calls == 2 && f.model.callback_calls == 0);
}

int main(void)
{
    test_success_and_single_use();
    test_run_preflight_unchanged();
    test_ledger_preflight_refusal();
    test_upstream_failures();
    test_getmac_failures();
    test_cleanup_failures_and_bounded_deinit();
    test_cleanup_preflight_and_stale_replacement();
    printf("hosted startup model: %lu checks passed\n", checks);
    return 0;
}
