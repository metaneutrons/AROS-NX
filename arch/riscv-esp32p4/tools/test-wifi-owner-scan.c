/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
/* Source-extracted private scan-path tests. Hosted/Exec/clock/SDIO boundaries
 * are deterministic host fakes; passing does not qualify hardware timing. */
#include "hosted_owner.h"
#include "hosted_request_ids.h"
#include "hosted_scan.h"
#include "hosted_wifi.h"
#include "hosted_mac.h"
#include "hosted_link.h"
#include "hosted_tx.h"
#include "hosted_wire.h"

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint8_t UBYTE;
typedef uint32_t ULONG;
typedef int BOOL;
typedef void *APTR;
typedef uintptr_t IPTR;

#define TRUE 1
#define FALSE 0
#define SIGBREAKF_CTRL_C 1u
#define P4_HOSTED_OWNER_MAX_ACQUIRE_US UINT32_C(20000)
#define P4_HOSTED_OWNER_MAX_SLICE_US UINT32_C(250000)
#define P4_WIFI_HOSTED_OWNER 1
#define P4_WIFI_SDIO_RESET_QUERY 0

struct Task { unsigned marker; };
struct Library { uint8_t opaque[64]; };
struct ExecBase { unsigned marker; };
struct SignalSemaphore { struct Task *ss_Owner; };
struct P4HostCommandResult { unsigned marker; };

struct P4SDMMCHost {
    uint32_t generation;
    struct SignalSemaphore lock;
    uint32_t slot_clock[1];
    unsigned dma_faulted;
    uint32_t (*now_us)(void);
    BOOL (*acquire)(struct P4SDMMCHost *, ULONG);
    BOOL (*set_budget)(struct P4SDMMCHost *, ULONG);
    void (*release)(struct P4SDMMCHost *);
    ULONG last_budget;
};

static struct Task test_task;
static uint32_t test_clock_us;
static unsigned test_budget_ok = 1u;
static unsigned test_checks;

static struct Task *FindTask(const char *name)
{
    (void)name;
    return &test_task;
}

static ULONG SetSignal(ULONG new_signals, ULONG signal_mask)
{
    (void)new_signals;
    (void)signal_mask;
    return 0u;
}

static void Delay(ULONG ticks)
{
    test_clock_us += ticks * UINT32_C(20000);
}

static void CopyMem(const void *source, void *destination, ULONG size)
{
    (void)memmove(destination, source, size);
}

static void p4host_zero(void *buffer, size_t size)
{
    volatile uint8_t *bytes = (volatile uint8_t *)buffer;
    size_t index;
    for (index = 0; index < size; ++index)
        bytes[index] = 0u;
}

static int p4host_budget_ok(struct P4SDMMCHost *host)
{
    (void)host;
    return test_budget_ok != 0u;
}

struct P4WifiBase;
struct wifi_probe_context;
static int wifi_owner_acquire(void *, uint32_t);
static int wifi_owner_budget(void *, uint32_t);
static void wifi_owner_release(void *);
static const struct p4_hosted_wifi_config wifi_reference_profile;

#include "wifi_owner_scan_extract.inc"

static const struct p4_hosted_wifi_config wifi_reference_profile = {0};

#define TEST_GENERATION UINT32_C(0x76543210)
#define TEST_RESPONSE_CAPACITY 4096u
#define TEST_MAX_REQUESTS 16u
#define TEST_FRAME_SIZE 14u

enum scan_fault {
    SCAN_FAULT_NONE = 0,
    SCAN_FAULT_START_REMOTE,
    SCAN_FAULT_COUNT_REMOTE,
    SCAN_FAULT_RECORDS_REMOTE,
    SCAN_FAULT_START_MALFORMED,
    SCAN_FAULT_COUNT_MALFORMED,
    SCAN_FAULT_RECORDS_MALFORMED,
    SCAN_FAULT_RECORDS_SHORT,
    SCAN_FAULT_COUNT_TOO_LARGE,
    SCAN_FAULT_WRONG_RESPONSE_UID,
    SCAN_FAULT_START_UNSUBMITTED,
    SCAN_FAULT_COUNT_POISONED,
    SCAN_FAULT_STOP_REMOTE
};

struct scan_fixture;

struct scan_model {
    struct scan_fixture *fixture;
    enum scan_fault fault;
    uint16_t ap_count;
    uint16_t records_number;
    uint16_t records_actual;
    uint32_t cost_us[TEST_MAX_REQUESTS];
    uint32_t preempt_us[TEST_MAX_REQUESTS];
    uint32_t advance_after_reply_us[TEST_MAX_REQUESTS];
    uint32_t ids[TEST_MAX_REQUESTS];
    uint32_t uids[TEST_MAX_REQUESTS];
    uint32_t timeouts[TEST_MAX_REQUESTS];
    uint32_t budgets[TEST_MAX_REQUESTS];
    unsigned submitted[TEST_MAX_REQUESTS];
    unsigned calls;
    unsigned protocol_errors;
    unsigned cancel_calls;
    unsigned exchange_count;
    unsigned events_dispatched;
    unsigned reentry_calls;
    unsigned reentry_send_ok;
    unsigned reentry_scan_ok;
    unsigned acquisition_calls;
    unsigned release_calls;
    unsigned host_budget_calls;
    unsigned nested_tx_calls;
    unsigned fail_budget;
    unsigned current_call;
    unsigned stop_remote;
};

struct scan_fixture {
    struct wifi_probe_context context;
    struct P4SDMMCHost host;
    struct p4_hosted_owner owner;
    struct wifi_owner_control control;
    struct p4_hosted_fifo fifo;
    struct p4_hosted_tx_ops tx;
    struct wifi_device_runtime runtime;
    struct P4WifiBase base;
    struct p4_wifi_scan_result output;
    struct p4_wifi_scan_result nested_output;
    struct scan_model model;
};

static struct scan_fixture fixture;
static const struct p4_wifi_device_callbacks test_callbacks;

#define CHECK(expression) do { \
    ++test_checks; \
    if (!(expression)) { \
        fprintf(stderr, "check failed at %s:%d: %s\n", __FILE__, __LINE__, \
                #expression); \
        exit(1); \
    } \
} while (0)

static uint32_t fake_now_us(void)
{
    return test_clock_us;
}

static BOOL fake_host_acquire(struct P4SDMMCHost *host, ULONG wait_us)
{
    CHECK(wait_us != 0u && wait_us <= P4_HOSTED_OWNER_MAX_ACQUIRE_US);
    CHECK(host->lock.ss_Owner == NULL);
    host->lock.ss_Owner = FindTask(NULL);
    ++fixture.model.acquisition_calls;
    return TRUE;
}

static BOOL fake_host_budget(struct P4SDMMCHost *host, ULONG usec)
{
    CHECK(host->lock.ss_Owner == FindTask(NULL));
    CHECK(usec > 0u && usec <= P4_HOSTED_OWNER_MAX_SLICE_US);
    ++fixture.model.host_budget_calls;
    host->last_budget = usec;
    if (fixture.model.budgets[fixture.model.current_call] == 0u ||
        usec < fixture.model.budgets[fixture.model.current_call])
        fixture.model.budgets[fixture.model.current_call] = usec;
    return fixture.model.fail_budget ? FALSE : TRUE;
}

static void fake_host_release(struct P4SDMMCHost *host)
{
    CHECK(host->lock.ss_Owner == FindTask(NULL));
    host->lock.ss_Owner = NULL;
    ++fixture.model.release_calls;
}

static void scan_service_callback(APTR client, struct P4WifiBase *base)
{
    uint8_t frame[TEST_FRAME_SIZE] = {0};
    struct scan_fixture *current = client;
    unsigned acquisition_count;
    CHECK(current == &fixture);
    ++current->model.reentry_calls;
    acquisition_count = current->model.acquisition_calls;
    if (base->send_frame(base, frame, sizeof frame))
        ++current->model.reentry_send_ok;
    if (base->scan(base, &current->nested_output))
        ++current->model.reentry_scan_ok;
    CHECK(current->model.acquisition_calls == acquisition_count);
}

static BOOL scan_frame_callback(APTR client, const UBYTE *frame, ULONG size)
{
    (void)client;
    (void)frame;
    (void)size;
    return TRUE;
}

static void scan_ready_callback(APTR client, const UBYTE *mac)
{
    (void)client;
    (void)mac;
}

static void scan_stopped_callback(APTR client)
{
    (void)client;
}

static const struct p4_wifi_device_callbacks test_callbacks = {
    scan_service_callback,
    scan_frame_callback,
    scan_ready_callback,
    scan_stopped_callback
};

static size_t write_varint(uint8_t *bytes, size_t offset, uint64_t value)
{
    while (value >= UINT64_C(0x80)) {
        bytes[offset++] = (uint8_t)((value & UINT64_C(0x7f)) | UINT64_C(0x80));
        value >>= 7;
    }
    bytes[offset++] = (uint8_t)value;
    return offset;
}

static size_t add_varint(uint8_t *bytes, size_t offset, uint32_t field,
                         uint64_t value)
{
    offset = write_varint(bytes, offset, ((uint64_t)field << 3) | 0u);
    return write_varint(bytes, offset, value);
}

static size_t add_bytes(uint8_t *bytes, size_t offset, uint32_t field,
                        const uint8_t *value, size_t size)
{
    offset = write_varint(bytes, offset, ((uint64_t)field << 3) | 2u);
    offset = write_varint(bytes, offset, size);
    if (size != 0u)
        memcpy(bytes + offset, value, size);
    return offset + size;
}

static size_t make_status_body(uint8_t *body, int32_t status, unsigned malformed)
{
    size_t offset = 0u;
    if (malformed) {
        offset = add_varint(body, offset, 1u, 0u);
        return add_varint(body, offset, 1u, 0u);
    }
    if (status != 0)
        offset = add_varint(body, offset, 1u, (uint32_t)status);
    return offset;
}

static size_t make_count_body(uint8_t *body, int32_t status,
                              uint32_t count, unsigned malformed)
{
    size_t offset = 0u;
    if (malformed) {
        offset = add_varint(body, offset, 2u, count);
        return add_varint(body, offset, 2u, count);
    }
    if (status != 0)
        offset = add_varint(body, offset, 1u, (uint32_t)status);
    return add_varint(body, offset, 2u, count);
}

static size_t make_records_body(uint8_t *body, int32_t status,
                                uint16_t number, uint16_t actual,
                                unsigned malformed)
{
    uint8_t record[24], bssid[6];
    size_t offset = 0u, record_size;
    uint16_t index;
    if (malformed) {
        offset = add_varint(body, offset, 1u, 0u);
        return add_varint(body, offset, 1u, 0u);
    }
    if (status != 0)
        offset = add_varint(body, offset, 1u, (uint32_t)status);
    offset = add_varint(body, offset, 2u, number);
    for (index = 0u; index < actual; ++index) {
        bssid[0] = 0x02u;
        bssid[1] = 0x10u;
        bssid[2] = 0x20u;
        bssid[3] = 0x30u;
        bssid[4] = (uint8_t)(index >> 8);
        bssid[5] = (uint8_t)index;
        record_size = add_bytes(record, 0u, 1u, bssid, sizeof bssid);
        offset = add_bytes(body, offset, 3u, record, record_size);
    }
    return offset;
}

static int expect_request(const struct p4_hosted_rpc *rpc,
                          uint32_t expected_id, uint32_t expected_uid)
{
    static const uint8_t start_body[] = {0x10u, 0x01u, 0x18u, 0x00u};
    uint8_t records_body[2];
    const uint8_t *expected = NULL;
    size_t expected_size = 0u;
    if (rpc->msg_type != P4_HOSTED_RPC_TYPE_REQUEST ||
        rpc->msg_id != expected_id || rpc->uid != expected_uid)
        return 0;
    if (expected_id == 286u) {
        expected = start_body;
        expected_size = sizeof start_body;
    } else if (expected_id == 289u) {
        if (rpc->payload_size != 2u || rpc->payload[0] != 0x08u ||
            rpc->payload[1] == 0u || rpc->payload[1] > 8u)
            return 0;
        records_body[0] = 0x08u;
        records_body[1] = rpc->payload[1];
        expected = records_body;
        expected_size = sizeof records_body;
    } else if (expected_id == 287u || expected_id == 288u) {
        expected_size = 0u;
    } else {
        return 0;
    }
    return rpc->payload_size == expected_size &&
        (expected_size == 0u || memcmp(rpc->payload, expected, expected_size) == 0);
}

static int should_fail_for(enum scan_fault fault, uint32_t id)
{
    switch (fault) {
    case SCAN_FAULT_START_REMOTE:
    case SCAN_FAULT_START_MALFORMED:
    case SCAN_FAULT_START_UNSUBMITTED:
        return id == 286u;
    case SCAN_FAULT_COUNT_REMOTE:
    case SCAN_FAULT_COUNT_MALFORMED:
    case SCAN_FAULT_COUNT_TOO_LARGE:
    case SCAN_FAULT_WRONG_RESPONSE_UID:
    case SCAN_FAULT_COUNT_POISONED:
        return id == 288u;
    case SCAN_FAULT_RECORDS_REMOTE:
    case SCAN_FAULT_RECORDS_MALFORMED:
    case SCAN_FAULT_RECORDS_SHORT:
        return id == 289u;
    case SCAN_FAULT_STOP_REMOTE:
        return id == 287u;
    default:
        return 0;
    }
}

static int fake_spend_exchange(struct p4_hosted_owner *owner,
    const struct p4_hosted_owner_ops *ops, uint32_t timeout_us,
    uint32_t extra_preempt_us, uint32_t cost_us, unsigned call_index,
    unsigned *submitted)
{
    uint32_t remaining_cost = cost_us;
    uint32_t spend_started = test_clock_us;
    CHECK(owner == &fixture.owner);
    *submitted = 0u;
    fixture.model.current_call = call_index;
    test_clock_us += extra_preempt_us;
    if (ops->stopping(ops->context))
        return 0;

    /* Model the owner's bounded acquired slices using the production adapter
     * callbacks. This does not run the SDIO motor or RX implementation. */
    do {
        uint32_t remaining_deadline = wifi_owner_remaining(&fixture.context,
            fixture.context.scan_started, 8000000UL);
        uint32_t elapsed_call = test_clock_us - spend_started;
        uint32_t remaining_call = elapsed_call < timeout_us
            ? timeout_us - elapsed_call : 0u;
        ULONG requested = remaining_cost > P4_HOSTED_OWNER_MAX_SLICE_US
            ? P4_HOSTED_OWNER_MAX_SLICE_US : remaining_cost;
        if (remaining_call < requested)
            requested = remaining_call;
        if (remaining_deadline < requested)
            requested = remaining_deadline;
        if (requested == 0u || ops->stopping(ops->context))
            return 0;
        if (!ops->acquire(ops->context, P4_HOSTED_OWNER_MAX_ACQUIRE_US))
            return 0;
        if (!ops->set_budget(ops->context, requested)) {
            ops->release(ops->context);
            return 0;
        }
        *submitted = 1u;
        if (remaining_cost <= requested) {
            test_clock_us += remaining_cost;
            remaining_cost = 0u;
        } else {
            test_clock_us += requested;
            remaining_cost -= requested;
        }
        ops->release(ops->context);
        if (remaining_cost != 0u && ops->stopping(ops->context))
            return 0;
    } while (remaining_cost != 0u);
    return !ops->stopping(ops->context);
}

/* Deliberate fake for the host-owner exchange boundary. Requests and replies
 * still pass the real protobuf RPC, scan codecs, and persistent UID ledger. */
int p4_hosted_owner_exchange(struct p4_hosted_owner *owner,
    struct p4_hosted_fifo *fifo, const struct p4_hosted_owner_ops *ops,
    uint32_t generation, unsigned checksum_enabled,
    const uint8_t *request, size_t request_size, uint32_t timeout_us,
    struct p4_hosted_owner_result *result)
{
    struct p4_hosted_rpc parsed, response, decoded;
    uint8_t body[TEST_RESPONSE_CAPACITY], wire[TEST_RESPONSE_CAPACITY];
    size_t body_size = 0u, wire_size = 0u;
    uint32_t request_id, uid, cost, preempt;
    unsigned call_index;
    int32_t remote_status = 0;
    unsigned malformed = 0u;
    (void)checksum_enabled;
    CHECK(owner == &fixture.owner && fifo == &fixture.fifo);
    CHECK(ops == &fixture.control.ops && result == &fixture.control.result);
    CHECK(generation == fixture.context.generation);
    CHECK(timeout_us > 0u && timeout_us <= 8000000u);
    CHECK(p4_hosted_rpc_decode(request, request_size, &parsed));
    CHECK(parsed.msg_type == P4_HOSTED_RPC_TYPE_REQUEST);
    request_id = parsed.msg_id;
    uid = parsed.uid;
    CHECK(uid != 0u && uid == owner->ids.active_uid);
    CHECK(request_id == owner->ids.active_request_id);
    CHECK(expect_request(&parsed, request_id, uid));
    if (fixture.model.calls >= TEST_MAX_REQUESTS)
        return 0;
    call_index = fixture.model.calls++;
    fixture.model.ids[call_index] = request_id;
    fixture.model.uids[call_index] = uid;
    fixture.model.timeouts[call_index] = timeout_us;
    ++fixture.model.exchange_count;

    /* Service callbacks are the owner path's reentry opportunity. The
     * extracted scan/send functions must refuse both while scanning. */
    fixture.base.callbacks->service(fixture.base.client, &fixture.base);

    if (fixture.model.fault == SCAN_FAULT_START_UNSUBMITTED &&
        request_id == 286u) {
        result->uid = uid;
        result->request_id = request_id;
        result->submitted = 0u;
        result->error = P4_HOSTED_OWNER_TX;
        (void)p4_hosted_owner_cancel(owner, fifo, generation);
        return 0;
    }

    preempt = fixture.model.preempt_us[call_index];
    cost = fixture.model.cost_us[call_index];
    if (!fake_spend_exchange(owner, ops, timeout_us, preempt, cost,
                             call_index, &fixture.model.submitted[call_index])) {
        result->submitted = fixture.model.submitted[call_index];
        result->uid = uid;
        result->request_id = request_id;
        result->error = P4_HOSTED_OWNER_TIMEOUT;
        (void)p4_hosted_owner_cancel(owner, fifo, generation);
        return 0;
    }

    result->submitted = 1u;
    result->uid = uid;
    result->request_id = request_id;
    result->error = P4_HOSTED_OWNER_OK;
    if (should_fail_for(fixture.model.fault, request_id)) {
        switch (fixture.model.fault) {
        case SCAN_FAULT_START_REMOTE:
        case SCAN_FAULT_STOP_REMOTE:
        case SCAN_FAULT_COUNT_REMOTE:
        case SCAN_FAULT_RECORDS_REMOTE:
            remote_status = 7;
            break;
        case SCAN_FAULT_START_MALFORMED:
        case SCAN_FAULT_COUNT_MALFORMED:
        case SCAN_FAULT_RECORDS_MALFORMED:
            malformed = 1u;
            break;
        case SCAN_FAULT_COUNT_TOO_LARGE:
            break;
        case SCAN_FAULT_RECORDS_SHORT:
            break;
        case SCAN_FAULT_WRONG_RESPONSE_UID:
            break;
        case SCAN_FAULT_COUNT_POISONED:
            result->error = P4_HOSTED_OWNER_RX;
            (void)p4_hosted_owner_cancel(owner, fifo, generation);
            return 0;
        default:
            break;
        }
    }
    if (request_id == 287u && fixture.model.stop_remote)
        remote_status = 7;

    if (request_id == 286u || request_id == 287u)
        body_size = make_status_body(body, remote_status,
            malformed && request_id == 286u);
    else if (request_id == 288u) {
        uint32_t count = fixture.model.ap_count;
        if (fixture.model.fault == SCAN_FAULT_COUNT_TOO_LARGE &&
            request_id == 288u)
            count = 65536u;
        body_size = make_count_body(body, remote_status, count,
            malformed && request_id == 288u);
    } else {
        uint16_t number = fixture.model.records_number;
        uint16_t actual = fixture.model.records_actual;
        if (fixture.model.fault == SCAN_FAULT_RECORDS_SHORT &&
            request_id == 289u) {
            number = (uint16_t)(fixture.control.scan_capacity - 1u);
            actual = number;
        }
        body_size = make_records_body(body, remote_status, number, actual,
            malformed && request_id == 289u);
    }

    response.msg_type = P4_HOSTED_RPC_TYPE_RESPONSE;
    response.msg_id = request_id + 256u;
    response.uid = uid;
    if (fixture.model.fault == SCAN_FAULT_WRONG_RESPONSE_UID &&
        request_id == 288u)
        ++response.uid;
    response.payload = body;
    response.payload_size = body_size;
    CHECK(p4_hosted_rpc_encode(wire, sizeof wire, &response, &wire_size));
    CHECK(p4_hosted_rpc_decode(wire, wire_size, &decoded));
    CHECK(decoded.msg_type == P4_HOSTED_RPC_TYPE_RESPONSE);
    p4host_zero(&owner->reply, sizeof owner->reply);
    owner->reply.msg_type = decoded.msg_type;
    owner->reply.msg_id = decoded.msg_id;
    owner->reply.uid = decoded.uid;
    owner->reply.payload_size = decoded.payload_size;
    if (decoded.payload_size != 0u)
        memcpy(owner->reply.payload, decoded.payload, decoded.payload_size);
    CHECK(p4_hosted_request_ids_finish(&owner->ids, generation, uid,
                                       request_id));
    /* Lets tests model time consumed after a matched envelope but while the
     * caller validates its typed body; not a simulated physical delay. */
    test_clock_us += fixture.model.advance_after_reply_us[call_index];
    p4host_zero(body, sizeof body);
    p4host_zero(wire, sizeof wire);
    return 1;
}

int p4_hosted_owner_cancel(struct p4_hosted_owner *owner,
    struct p4_hosted_fifo *fifo, uint32_t generation)
{
    if (!owner || !fifo || owner->initialized != 1u ||
        owner->generation != generation || generation == 0u)
        return 0;
    ++fixture.model.cancel_calls;
    owner->poisoned = 1u;
    owner->pending_active = 0u;
    owner->partial_active = 0u;
    owner->pending_uid = 0u;
    owner->pending_request_id = 0u;
    if (owner->ids.generation == generation &&
        owner->ids.retired == 0u)
        (void)p4_hosted_request_ids_invalidate(&owner->ids, generation);
    if (fifo->generation == generation)
        fifo->poisoned = 1u;
    p4host_zero(&owner->reply, sizeof owner->reply);
    p4host_zero(owner->request, sizeof owner->request);
    p4host_zero(owner->wire, sizeof owner->wire);
    p4host_zero(owner->frame, sizeof owner->frame);
    return 1;
}

static void fixture_reset(void)
{
    static const uint32_t startup_ids[] = {278u, 260u, 280u, 257u};
    uint32_t startup_uid;
    unsigned index;
    memset(&fixture, 0, sizeof fixture);
    test_clock_us = 100000u;
    test_budget_ok = 1u;
    fixture.host.generation = TEST_GENERATION;
    fixture.host.slot_clock[0] = 400000u;
    fixture.host.now_us = fake_now_us;
    fixture.host.acquire = fake_host_acquire;
    fixture.host.set_budget = fake_host_budget;
    fixture.host.release = fake_host_release;
    fixture.context.host = &fixture.host;
    fixture.context.generation = TEST_GENERATION;
    fixture.context.peer_clock = 400000u;
    fixture.owner.generation = TEST_GENERATION;
    fixture.owner.initialized = 1u;
    fixture.owner.rx.initialized = 1u;
    fixture.owner.rx.generation = TEST_GENERATION;
    CHECK(p4_hosted_request_ids_init(&fixture.owner.ids, TEST_GENERATION));
    /* The private scan is only published after native Init/STA/Start/GetMAC.
     * Preserve those four already-consumed identities in this epoch. */
    for (index = 0; index < sizeof startup_ids / sizeof startup_ids[0]; ++index) {
        CHECK(p4_hosted_request_ids_reserve(&fixture.owner.ids,
            TEST_GENERATION, startup_ids[index], &startup_uid));
        CHECK(startup_uid == index + 1u);
        CHECK(p4_hosted_request_ids_finish(&fixture.owner.ids,
            TEST_GENERATION, startup_uid, startup_ids[index]));
    }
    CHECK(p4_hosted_fifo_start(&fixture.fifo, TEST_GENERATION, 20u));
    fixture.control.ops.context = &fixture.context;
    fixture.control.ops.acquire = wifi_owner_acquire;
    fixture.control.ops.release = wifi_owner_release;
    fixture.control.ops.now_us = wifi_owner_now;
    fixture.control.ops.wait = wifi_yield;
    fixture.control.ops.set_budget = wifi_owner_budget;
    fixture.control.ops.stopping = wifi_owner_stopping;
    fixture.runtime.context = &fixture.context;
    fixture.runtime.owner = &fixture.owner;
    fixture.runtime.control = &fixture.control;
    fixture.runtime.fifo = &fixture.fifo;
    fixture.runtime.tx = &fixture.tx;
    fixture.runtime.checksum = 0u;
    fixture.runtime.servicing = TRUE;
    fixture.base.ready = TRUE;
    fixture.base.owner_task = FindTask(NULL);
    fixture.base.runtime = &fixture.runtime;
    fixture.base.client = &fixture;
    fixture.base.callbacks = &test_callbacks;
    fixture.base.send_frame = wifi_device_send;
    fixture.base.scan = wifi_device_scan;
    fixture.base.client_lock.ss_Owner = FindTask(NULL);
    fixture.model.fixture = &fixture;
    fixture.model.ap_count = 3u;
    fixture.model.records_number = 3u;
    fixture.model.records_actual = 3u;
    fixture.model.cost_us[0] = 100000u;
    fixture.model.cost_us[1] = 100000u;
    fixture.model.cost_us[2] = 100000u;
    fixture.model.cost_us[3] = 100000u;
    fixture.model.cost_us[4] = 100000u;
    fixture.control.ops.dispatch_event = NULL;
    fixture.control.ops.dispatch_frame = NULL;
    memset(&fixture.output, 0xa5, sizeof fixture.output);
}

static void check_request_log(const uint32_t *ids, unsigned count,
                              uint32_t first_uid)
{
    unsigned index;
    CHECK(fixture.model.calls == count);
    for (index = 0; index < count; ++index) {
        CHECK(fixture.model.ids[index] == ids[index]);
        CHECK(fixture.model.uids[index] == first_uid + index);
    }
}

static void check_private_scratch_wiped(void)
{
    const uint8_t *request = fixture.control.request;
    size_t index;
    for (index = 0; index < sizeof fixture.control.request; ++index)
        CHECK(request[index] == 0u);
    CHECK(fixture.control.scan_capacity == 0u);
    for (index = 0; index < sizeof fixture.control.scan_count; ++index)
        CHECK(((const uint8_t *)&fixture.control.scan_count)[index] == 0u);
    for (index = 0; index < sizeof fixture.control.scan_result; ++index)
        CHECK(((const uint8_t *)&fixture.control.scan_result)[index] == 0u);
    CHECK(fixture.context.scan_active == FALSE);
    CHECK(fixture.context.scan_started == 0u);
    CHECK(fixture.runtime.scanning == FALSE);
    CHECK(fixture.runtime.servicing == TRUE);
}

static void check_failed_output_unchanged(void)
{
    const uint8_t *bytes = (const uint8_t *)&fixture.output;
    size_t index;
    for (index = 0; index < sizeof fixture.output; ++index)
        CHECK(bytes[index] == 0xa5u);
}

static void test_success_repeat_and_owned_results(void)
{
    static const uint32_t first_ids[] = {286u, 288u, 289u};
    static const uint32_t second_ids[] = {286u, 288u, 289u, 286u, 288u};
    unsigned i;
    fixture_reset();
    fixture.model.ap_count = 3u;
    fixture.model.records_number = 3u;
    fixture.model.records_actual = 3u;
    CHECK(wifi_device_scan(&fixture.base, &fixture.output) == TRUE);
    check_request_log(first_ids, 3u, 5u);
    CHECK(fixture.output.total_count == 3u);
    CHECK(fixture.output.aps.record_count == 3u);
    CHECK(fixture.output.aps.records[0].bssid[0] == 0x02u);
    CHECK(fixture.output.aps.records[2].bssid[5] == 2u);
    CHECK(fixture.output.truncated == FALSE);
    CHECK(fixture.output.empty_records_workaround == FALSE);
    CHECK(fixture.model.reentry_calls == 3u);
    CHECK(fixture.model.reentry_send_ok == 0u);
    CHECK(fixture.model.reentry_scan_ok == 0u);
    CHECK(fixture.model.nested_tx_calls == 0u);
    CHECK(fixture.model.cancel_calls == 0u);
    check_private_scratch_wiped();

    /* Output owns decoded bytes; later calls do not borrow/overwrite it. */
    for (i = 0; i < 3u; ++i)
        fixture.model.cost_us[i] = 100000u;
    fixture.model.ap_count = 0u;
    CHECK(wifi_device_scan(&fixture.base, &fixture.nested_output) == TRUE);
    check_request_log(second_ids, 5u, 5u);
    CHECK(fixture.model.uids[3] == 8u && fixture.model.uids[4] == 9u);
    CHECK(fixture.nested_output.total_count == 0u);
    CHECK(fixture.nested_output.empty_records_workaround == TRUE);
    CHECK(fixture.nested_output.aps.record_count == 0u);
    CHECK(fixture.output.total_count == 3u);
    CHECK(fixture.output.aps.record_count == 3u);
    CHECK(fixture.owner.ids.next_uid == 10u);
    CHECK(fixture.owner.ids.active_uid == 0u);
    check_private_scratch_wiped();
}

static void test_empty_and_truncated_count(void)
{
    static const uint32_t empty_ids[] = {286u, 288u};
    static const uint32_t capped_ids[] = {286u, 288u, 289u};
    fixture_reset();
    fixture.model.ap_count = 0u;
    CHECK(wifi_device_scan(&fixture.base, &fixture.output) == TRUE);
    check_request_log(empty_ids, 2u, 5u);
    CHECK(fixture.output.empty_records_workaround == TRUE);
    CHECK(fixture.output.total_count == 0u);
    CHECK(fixture.output.aps.record_count == 0u);
    CHECK(fixture.output.aps.number == 0u);
    CHECK(fixture.model.ids[1] == 288u);
    CHECK(fixture.owner.ids.next_uid == 7u);
    check_private_scratch_wiped();

    fixture_reset();
    fixture.model.ap_count = 13u;
    fixture.model.records_number = 8u;
    fixture.model.records_actual = 8u;
    CHECK(wifi_device_scan(&fixture.base, &fixture.output) == TRUE);
    check_request_log(capped_ids, 3u, 5u);
    CHECK(fixture.output.total_count == 13u);
    CHECK(fixture.output.truncated == TRUE);
    CHECK(fixture.output.aps.number == 8u);
    CHECK(fixture.output.aps.record_count == 8u);
    CHECK(fixture.model.ids[2] == 289u);
    CHECK(fixture.model.uids[2] == 7u);
    CHECK(fixture.owner.ids.next_uid == 8u);
    check_private_scratch_wiped();
}

static void run_failure_case(enum scan_fault fault,
                             const uint32_t *ids, unsigned count,
                             unsigned expect_poison, unsigned expect_cancel)
{
    fixture_reset();
    fixture.model.fault = fault;
    if (fault == SCAN_FAULT_RECORDS_SHORT) {
        fixture.model.ap_count = 4u;
        fixture.model.records_number = 4u;
        fixture.model.records_actual = 4u;
    }
    CHECK(wifi_device_scan(&fixture.base, &fixture.output) == FALSE);
    check_failed_output_unchanged();
    check_request_log(ids, count, 5u);
    CHECK((fixture.owner.poisoned != 0u) == (expect_poison != 0u));
    CHECK((fixture.model.cancel_calls != 0u) == (expect_cancel != 0u));
    if (fault == SCAN_FAULT_START_UNSUBMITTED)
        CHECK(fixture.control.result.submitted == 0u);
    check_private_scratch_wiped();
}

static void test_remote_errors_and_malformed_replies(void)
{
    static const uint32_t start_remote[] = {286u, 287u};
    static const uint32_t count_remote[] = {286u, 288u, 287u};
    static const uint32_t records_remote[] = {286u, 288u, 289u, 287u};
    static const uint32_t start_bad[] = {286u};
    static const uint32_t count_bad[] = {286u, 288u};
    static const uint32_t records_bad[] = {286u, 288u, 289u};
    static const uint32_t no_write[] = {286u};
    static const uint32_t poisoned_count[] = {286u, 288u};

    run_failure_case(SCAN_FAULT_START_REMOTE, start_remote, 2u, 0u, 0u);
    run_failure_case(SCAN_FAULT_COUNT_REMOTE, count_remote, 3u, 0u, 0u);
    run_failure_case(SCAN_FAULT_RECORDS_REMOTE, records_remote, 4u, 0u, 0u);
    run_failure_case(SCAN_FAULT_START_MALFORMED, start_bad, 1u, 1u, 1u);
    run_failure_case(SCAN_FAULT_COUNT_MALFORMED, count_bad, 2u, 1u, 1u);
    run_failure_case(SCAN_FAULT_RECORDS_MALFORMED, records_bad, 3u, 1u, 1u);
    run_failure_case(SCAN_FAULT_RECORDS_SHORT, records_bad, 3u, 1u, 1u);
    run_failure_case(SCAN_FAULT_COUNT_TOO_LARGE, count_bad, 2u, 1u, 1u);
    run_failure_case(SCAN_FAULT_WRONG_RESPONSE_UID, count_bad, 2u, 1u, 1u);
    run_failure_case(SCAN_FAULT_START_UNSUBMITTED, no_write, 1u, 1u, 1u);
    run_failure_case(SCAN_FAULT_COUNT_POISONED, poisoned_count, 2u, 1u, 1u);

}

static void test_cleanup_error_and_deadline(void)
{
    static const uint32_t stop_failure_ids[] = {286u, 288u, 287u};
    static const uint32_t deadline_ids[] = {286u, 288u};
    static const uint32_t late_decode_ids[] = {286u, 288u};
    fixture_reset();
    fixture.model.fault = SCAN_FAULT_COUNT_REMOTE;
    fixture.model.stop_remote = 1u;
    fixture.model.cost_us[0] = 100000u;
    fixture.model.cost_us[1] = 100000u;
    fixture.model.cost_us[2] = 100000u;
    /* The stop response is separately selected by forcing its wire status. */
    CHECK(wifi_device_scan(&fixture.base, &fixture.output) == FALSE);
    check_request_log(stop_failure_ids, 3u, 5u);
    CHECK(fixture.owner.poisoned != 0u);
    CHECK(fixture.model.cancel_calls == 1u);
    CHECK(fixture.model.submitted[0] == 1u);
    CHECK(fixture.model.submitted[1] == 1u);
    CHECK(fixture.model.ids[2] == 287u);
    CHECK(fixture.model.timeouts[2] == 7800000u);
    check_failed_output_unchanged();

    fixture_reset();
    fixture.model.ap_count = 3u;
    fixture.model.records_number = 3u;
    fixture.model.records_actual = 3u;
    fixture.model.cost_us[0] = 7000000u;
    fixture.model.preempt_us[1] = 1100000u;
    fixture.model.cost_us[1] = 100000u;
    CHECK(wifi_device_scan(&fixture.base, &fixture.output) == FALSE);
    check_request_log(deadline_ids, 2u, 5u);
    CHECK(fixture.model.timeouts[0] == 8000000u);
    CHECK(fixture.model.timeouts[1] == 1000000u);
    CHECK(fixture.model.budgets[0] <= 250000u);
    CHECK(fixture.model.budgets[1] == 0u);
    CHECK(fixture.model.submitted[1] == 0u);
    CHECK(fixture.owner.poisoned != 0u);
    CHECK(fixture.model.cancel_calls == 1u);
    check_failed_output_unchanged();
    check_private_scratch_wiped();

    fixture_reset();
    fixture.model.ap_count = 0u;
    fixture.model.advance_after_reply_us[1] = 7800000u;
    CHECK(wifi_device_scan(&fixture.base, &fixture.output) == FALSE);
    check_request_log(late_decode_ids, 2u, 5u);
    CHECK(fixture.model.timeouts[0] == 8000000u);
    CHECK(fixture.model.timeouts[1] == 7900000u);
    CHECK(fixture.owner.poisoned != 0u);
    CHECK(fixture.model.cancel_calls == 1u);
    check_failed_output_unchanged();
    check_private_scratch_wiped();
}

static void test_late_response_and_slice_deadline_helpers(void)
{
    fixture_reset();
    fixture.context.scan_active = TRUE;
    fixture.context.scan_started = test_clock_us;
    fixture.host.lock.ss_Owner = FindTask(NULL);
    test_clock_us += 7950000u;
    CHECK(wifi_owner_stopping(&fixture.context) == FALSE);
    CHECK(wifi_budget(&fixture.context) == TRUE);
    CHECK(wifi_owner_budget(&fixture.context, 250000u) == TRUE);
    CHECK(fixture.model.host_budget_calls == 1u);
    CHECK(fixture.host.last_budget == 50000u);
    test_clock_us += 50000u;
    CHECK(wifi_owner_stopping(&fixture.context) == TRUE);
    CHECK(wifi_budget(&fixture.context) == FALSE);
    CHECK(wifi_owner_budget(&fixture.context, 250000u) == FALSE);
    CHECK(fixture.model.host_budget_calls == 1u);
    fixture.host.lock.ss_Owner = NULL;
    CHECK(wifi_owner_stopping(&fixture.context) == TRUE);
}

static void check_refusal_unchanged(void)
{
    uint8_t before[sizeof fixture.output];
    uint32_t next_uid = fixture.owner.ids.next_uid;
    uint32_t active_uid = fixture.owner.ids.active_uid;
    uint32_t active_id = fixture.owner.ids.active_request_id;
    uint32_t owner_generation = fixture.owner.generation;
    uint32_t fifo_generation = fixture.fifo.generation;
    unsigned owner_poisoned = fixture.owner.poisoned;
    unsigned fifo_poisoned = fixture.fifo.poisoned;
    unsigned pending = fixture.owner.pending_active;
    unsigned servicing = (unsigned)fixture.runtime.servicing;
    unsigned scanning = (unsigned)fixture.runtime.scanning;
    unsigned scan_active = (unsigned)fixture.context.scan_active;
    ULONG scan_started = fixture.context.scan_started;
    memcpy(before, &fixture.output, sizeof before);
    CHECK(wifi_device_scan(&fixture.base, &fixture.output) == FALSE);
    CHECK(memcmp(before, &fixture.output, sizeof before) == 0);
    CHECK(fixture.model.calls == 0u);
    CHECK(fixture.model.cancel_calls == 0u);
    CHECK(fixture.owner.ids.next_uid == next_uid);
    CHECK(fixture.owner.ids.active_uid == active_uid);
    CHECK(fixture.owner.ids.active_request_id == active_id);
    CHECK(fixture.owner.generation == owner_generation);
    CHECK(fixture.fifo.generation == fifo_generation);
    CHECK(fixture.owner.poisoned == owner_poisoned);
    CHECK(fixture.fifo.poisoned == fifo_poisoned);
    CHECK(fixture.owner.pending_active == pending);
    CHECK((unsigned)fixture.runtime.servicing == servicing);
    CHECK((unsigned)fixture.runtime.scanning == scanning);
    CHECK((unsigned)fixture.context.scan_active == scan_active);
    CHECK(fixture.context.scan_started == scan_started);
}

static void test_preflight_refusals_and_aliases(void)
{
    struct wifi_device_runtime saved_runtime;
    struct wifi_probe_context saved_context;
    struct p4_hosted_owner saved_owner;
    struct wifi_owner_control saved_control;
    struct p4_hosted_fifo saved_fifo;
    struct P4SDMMCHost saved_host;
    struct p4_hosted_tx_ops saved_tx;
    struct p4_wifi_device_callbacks saved_callbacks;

    fixture_reset();
    fixture.base.owner_task = (struct Task *)(uintptr_t)0x1234u;
    check_refusal_unchanged();

    fixture_reset();
    fixture.runtime.servicing = FALSE;
    check_refusal_unchanged();

    fixture_reset();
    fixture.runtime.scanning = TRUE;
    check_refusal_unchanged();

    fixture_reset();
    fixture.context.scan_active = TRUE;
    check_refusal_unchanged();

    fixture_reset();
    fixture.owner.pending_active = 1u;
    check_refusal_unchanged();

    fixture_reset();
    fixture.owner.poisoned = 1u;
    check_refusal_unchanged();

    fixture_reset();
    fixture.fifo.poisoned = 1u;
    check_refusal_unchanged();

    fixture_reset();
    fixture.owner.generation++;
    check_refusal_unchanged();

    fixture_reset();
    fixture.fifo.generation++;
    check_refusal_unchanged();

    fixture_reset();
    fixture.base.client_lock.ss_Owner = NULL;
    check_refusal_unchanged();

    fixture_reset();
    fixture.host.lock.ss_Owner = FindTask(NULL);
    check_refusal_unchanged();

    fixture_reset();
    CHECK(p4_hosted_request_ids_reserve(&fixture.owner.ids,
        TEST_GENERATION, 286u, &fixture.control.uid));
    check_refusal_unchanged();

    fixture_reset();
    fixture.runtime.control = NULL;
    check_refusal_unchanged();

    fixture_reset();
    fixture.runtime.fifo = NULL;
    check_refusal_unchanged();

    fixture_reset();
    fixture.runtime.owner = NULL;
    check_refusal_unchanged();

    fixture_reset();
    fixture.runtime.context = NULL;
    check_refusal_unchanged();

    fixture_reset();
    fixture.context.host = NULL;
    check_refusal_unchanged();

    fixture_reset();
    CHECK(wifi_device_scan(&fixture.base, NULL) == FALSE);
    CHECK(fixture.model.calls == 0u);
    CHECK(fixture.owner.ids.next_uid == 5u);
    CHECK(fixture.runtime.servicing == TRUE);
    CHECK(fixture.context.scan_active == FALSE);

    fixture_reset();
    memset(&saved_runtime, 0, sizeof saved_runtime);
    memcpy(&saved_runtime, &fixture.runtime, sizeof saved_runtime);
    CHECK(wifi_device_scan(&fixture.base,
        (struct p4_wifi_scan_result *)&fixture.runtime) == FALSE);
    CHECK(memcmp(&saved_runtime, &fixture.runtime, sizeof saved_runtime) == 0);
    CHECK(fixture.model.calls == 0u);

    fixture_reset();
    memcpy(&saved_context, &fixture.context, sizeof saved_context);
    CHECK(wifi_device_scan(&fixture.base,
        (struct p4_wifi_scan_result *)&fixture.context) == FALSE);
    CHECK(memcmp(&saved_context, &fixture.context, sizeof saved_context) == 0);
    CHECK(fixture.model.calls == 0u);

    fixture_reset();
    memcpy(&saved_owner, &fixture.owner, sizeof saved_owner);
    CHECK(wifi_device_scan(&fixture.base,
        (struct p4_wifi_scan_result *)&fixture.owner) == FALSE);
    CHECK(memcmp(&saved_owner, &fixture.owner, sizeof saved_owner) == 0);
    CHECK(fixture.model.calls == 0u);

    fixture_reset();
    memcpy(&saved_control, &fixture.control, sizeof saved_control);
    CHECK(wifi_device_scan(&fixture.base,
        (struct p4_wifi_scan_result *)&fixture.control) == FALSE);
    CHECK(memcmp(&saved_control, &fixture.control, sizeof saved_control) == 0);
    CHECK(fixture.model.calls == 0u);

    fixture_reset();
    memcpy(&saved_fifo, &fixture.fifo, sizeof saved_fifo);
    CHECK(wifi_device_scan(&fixture.base,
        (struct p4_wifi_scan_result *)&fixture.fifo) == FALSE);
    CHECK(memcmp(&saved_fifo, &fixture.fifo, sizeof saved_fifo) == 0);
    CHECK(fixture.model.calls == 0u);

    fixture_reset();
    memcpy(&saved_host, &fixture.host, sizeof saved_host);
    CHECK(wifi_device_scan(&fixture.base,
        (struct p4_wifi_scan_result *)&fixture.host) == FALSE);
    CHECK(memcmp(&saved_host, &fixture.host, sizeof saved_host) == 0);
    CHECK(fixture.model.calls == 0u);

    fixture_reset();
    memcpy(&saved_tx, &fixture.tx, sizeof saved_tx);
    CHECK(wifi_device_scan(&fixture.base,
        (struct p4_wifi_scan_result *)&fixture.tx) == FALSE);
    CHECK(memcmp(&saved_tx, &fixture.tx, sizeof saved_tx) == 0);
    CHECK(fixture.model.calls == 0u);

    fixture_reset();
    memcpy(&saved_callbacks, &test_callbacks, sizeof saved_callbacks);
    CHECK(wifi_device_scan(&fixture.base,
        (struct p4_wifi_scan_result *)&test_callbacks) == FALSE);
    CHECK(memcmp(&saved_callbacks, &test_callbacks, sizeof saved_callbacks) == 0);
    CHECK(fixture.model.calls == 0u);
}

int main(void)
{
    test_success_repeat_and_owned_results();
    test_empty_and_truncated_count();
    test_remote_errors_and_malformed_replies();
    test_cleanup_error_and_deadline();
    test_late_response_and_slice_deadline_helpers();
    test_preflight_refusals_and_aliases();
    printf("owner scan source-extracted checks: %u\n", test_checks);
    return 0;
}
