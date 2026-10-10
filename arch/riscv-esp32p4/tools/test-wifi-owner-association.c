/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
/* Host qualification for the source-extracted native association workflow.
 * RPC bytes/codecs, request-ID ledger, AP decoder, link observer, and owner
 * retirement are production code. The exchange, controller, clock and Exec
 * scheduling boundary are deterministic host fakes; this is not radio or
 * hardware qualification. */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EXEC_TYPES_H
#define EXEC_LIBRARIES_H
#define EXEC_SEMAPHORES_H
#define P4_BOARD_WIFI_SDIO 1
#define P4_WIFI_HOSTED_OWNER 1
#define P4_WIFI_HOSTED_REPLY 1
#define P4_WIFI_SDIO_RESET_QUERY 0
typedef void *APTR;
typedef uint32_t ULONG;
typedef int32_t LONG;
typedef uint8_t UBYTE;
typedef uintptr_t IPTR;
typedef int BOOL;
#define TRUE 1
#define FALSE 0
#define SIGBREAKF_CTRL_C UINT32_C(0x1000)
struct ExecBase { unsigned unused; };
struct Task { unsigned marker; };
struct Library { unsigned unused; };
struct SignalSemaphore { struct Task *ss_Owner; unsigned depth; };

#include "runtime_probe.h"
#include "host.h"
#include "command.h"
#include "hosted_association.h"
#include "hosted_link.h"
#include "hosted_mac.h"
#include "hosted_owner.h"
#include "hosted_scan.h"
#include "hosted_tx.h"
#include "hosted_wifi.h"

#define TEST_GENERATION UINT32_C(0x1278a901)
#define TEST_PEER_CLOCK UINT32_C(400000)
#define TEST_POOL 32u
#define TEST_SSID "p4-assoc"
#define TEST_PASSWORD "safe-pass-123"

static unsigned long checks;
#define CHECK(expression) do { \
    ++checks; \
    if (!(expression)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                #expression); \
        exit(1); \
    } \
} while (0)

static struct Task owner_task = {1u};
static struct Task *current_task = &owner_task;
static struct P4WifiBase *wifi_resource_base;
static const struct p4_hosted_wifi_config wifi_reference_profile = {0};
static ULONG fake_now;
static ULONG fake_signals;
static unsigned delay_calls;
static unsigned set_signal_calls;
static unsigned semaphore_depth_errors;
static unsigned watched_secret_wipes;
static uint8_t watched_secret[P4_HOSTED_ASSOCIATION_PASSWORD_SIZE];
static size_t watched_secret_size;

static struct Task *FindTask(void *name)
{
    (void)name;
    return current_task;
}

static void Delay(ULONG ticks)
{
    ++delay_calls;
    fake_now += ticks * 20000u;
}

static ULONG SetSignal(ULONG signals, ULONG mask)
{
    ULONG previous = fake_signals;
    ++set_signal_calls;
    fake_signals = (fake_signals & ~mask) | (signals & mask);
    return previous;
}

static void CopyMem(const void *source, void *destination, ULONG size)
{
    memmove(destination, source, (size_t)size);
}

static void p4host_zero(void *memory, size_t size)
{
    volatile uint8_t *bytes = (volatile uint8_t *)memory;
    size_t i;
    if (watched_secret_size && size >= watched_secret_size) {
        for (i = 0; i <= size - watched_secret_size; ++i) {
            if (memcmp((const uint8_t *)memory + i, watched_secret,
                       watched_secret_size) == 0) {
                ++watched_secret_wipes;
                break;
            }
        }
    }
    for (i = 0; i < size; ++i)
        bytes[i] = 0u;
}

static int bytes_zero(const void *memory, size_t size)
{
    const uint8_t *bytes = memory;
    size_t i;
    for (i = 0u; i < size; ++i)
        if (bytes[i] != 0u)
            return 0;
    return 1;
}

static void ObtainSemaphore(struct SignalSemaphore *sem)
{
    CHECK(sem != NULL);
    if (sem->depth && sem->ss_Owner != current_task) {
        ++semaphore_depth_errors;
        CHECK(FALSE);
    }
    ++sem->depth;
    sem->ss_Owner = current_task;
}

static void ReleaseSemaphore(struct SignalSemaphore *sem)
{
    CHECK(sem != NULL);
    CHECK(sem->depth != 0u);
    CHECK(sem->ss_Owner == current_task);
    if (--sem->depth == 0u)
        sem->ss_Owner = NULL;
}

/* Generated from runtime_probe.c with exact signature and uniqueness checks. */
#include "wifi_owner_association_extract.inc"

struct fixture {
    struct P4WifiBase base;
    struct P4SDMMCHost host;
    struct wifi_probe_context context;
    struct p4_hosted_owner owner;
    struct wifi_owner_control control;
    struct p4_hosted_fifo fifo;
    struct wifi_device_runtime runtime;
    unsigned exchange_calls;
    unsigned acquire_calls;
    unsigned release_calls;
    unsigned budget_calls;
    unsigned cancel_calls;
    unsigned cancel_called_owned;
    unsigned request_count[600];
    unsigned submitted_count[600];
    unsigned no_response_count;
    unsigned malformed_request_count;
    unsigned secret_seen_in_rpc;
    ULONG last_budget;
    ULONG max_timeout;
    uint32_t fail_id;
    unsigned fail_occurrence;
    int32_t fail_status;
    unsigned initial_event_once;
    unsigned repeat_initial_event;
    unsigned postreply_disconnect_once;
    unsigned repeat_final_event;
    unsigned cancel_after_config;
    unsigned deadline_during_config;
    unsigned break_after_initial_disconnect;
    unsigned generation_change_final;
    unsigned cancel_final_snapshot;
    unsigned cancel_on_final_release;
    uint32_t malformed_response_request_id;
    uint32_t timeout_request_id;
    unsigned timeout_occurrence;
    enum {
        AP_VALID = 0,
        AP_WRONG_SSID,
        AP_WRONG_BSSID,
        AP_WRONG_AUTHMODE,
        AP_WRONG_PAIRWISE,
        AP_WRONG_GROUP,
        AP_WRONG_CHANNEL,
        AP_ZERO_BSSID,
        AP_MULTICAST_BSSID,
        AP_CHANNEL_ZERO,
        AP_CHANNEL_FIFTEEN
    } ap_fault;
    unsigned ap_info_calls;
    unsigned final_ap_calls;
    BOOL cancel_requested;
    BOOL is_protected;
};

static struct fixture f;

static void check_confirmed_ap_cleared(void)
{
    CHECK(bytes_zero(&f.base.confirmed_ap, sizeof(f.base.confirmed_ap)));
}

static void check_confirmed_ap_preserved(void)
{
    const uint8_t *bytes = (const uint8_t *)&f.base.confirmed_ap;
    size_t index;
    for (index = 0u; index < sizeof(f.base.confirmed_ap); ++index)
        CHECK(bytes[index] == 0xa5u);
}

static void check_confirmed_ap_matches(ULONG authmode)
{
    static const uint8_t bssid[6] =
        {0x02u, 0x11u, 0x22u, 0x33u, 0x44u, 0x55u};
    struct p4_wifi_link_info expected;
    memset(&expected, 0, sizeof(expected));
    memcpy(expected.ssid, TEST_SSID, sizeof(TEST_SSID) - 1u);
    expected.ssid_length = sizeof(TEST_SSID) - 1u;
    memcpy(expected.bssid, bssid, sizeof(bssid));
    expected.channel = 6u;
    expected.authmode = authmode;
    CHECK(memcmp(&f.base.confirmed_ap, &expected, sizeof(expected)) == 0);
}

static ULONG fake_now_us(void)
{
    return fake_now;
}

static BOOL fake_host_acquire(struct P4SDMMCHost *host, ULONG wait_us)
{
    ++f.acquire_calls;
    CHECK(host == &f.host);
    CHECK(wait_us > 0u && wait_us <= P4_HOSTED_OWNER_MAX_ACQUIRE_US);
    if (host->lock.ss_Owner || f.host.dma_faulted)
        return FALSE;
    host->lock.ss_Owner = current_task;
    host->lock.depth = 1u;
    return TRUE;
}

static void fake_host_release(struct P4SDMMCHost *host)
{
    ++f.release_calls;
    CHECK(host == &f.host);
    CHECK(host->lock.ss_Owner == current_task);
    CHECK(host->lock.depth == 1u);
    host->lock.ss_Owner = NULL;
    host->lock.depth = 0u;
    if (f.cancel_on_final_release) {
        f.cancel_on_final_release = 0u;
        f.cancel_requested = TRUE;
    }
}

static BOOL fake_host_set_budget(struct P4SDMMCHost *host, ULONG usec)
{
    ++f.budget_calls;
    f.last_budget = usec;
    CHECK(host == &f.host);
    CHECK(host->lock.ss_Owner == current_task);
    CHECK(usec > 0u && usec <= P4_HOSTED_OWNER_MAX_SLICE_US);
    return TRUE;
}

static BOOL fake_cancelled(APTR opaque)
{
    struct fixture *state = opaque;
    ++state->cancel_calls;
    if (state->host.lock.ss_Owner == current_task) {
        ++state->cancel_called_owned;
        CHECK(FALSE);
    }
    return state->cancel_requested ? TRUE : FALSE;
}

struct wire_buffer {
    uint8_t bytes[512];
    size_t size;
    int ok;
};

static void wire_init(struct wire_buffer *buffer)
{
    memset(buffer, 0, sizeof(*buffer));
    buffer->ok = 1;
}

static void wire_byte(struct wire_buffer *buffer, uint8_t byte)
{
    if (!buffer->ok || buffer->size == sizeof(buffer->bytes)) {
        buffer->ok = 0;
        return;
    }
    buffer->bytes[buffer->size++] = byte;
}

static void wire_varint(struct wire_buffer *buffer, uint64_t value)
{
    while (value >= 0x80u) {
        wire_byte(buffer, (uint8_t)((value & 0x7fu) | 0x80u));
        value >>= 7;
    }
    wire_byte(buffer, (uint8_t)value);
}

static void wire_key(struct wire_buffer *buffer, uint32_t field,
                     unsigned wire_type)
{
    wire_varint(buffer, ((uint64_t)field << 3) | wire_type);
}

static void wire_uint_field(struct wire_buffer *buffer, uint32_t field,
                            uint32_t value)
{
    wire_key(buffer, field, 0u);
    wire_varint(buffer, value);
}

static void wire_bytes_field(struct wire_buffer *buffer, uint32_t field,
                             const uint8_t *bytes, size_t size)
{
    wire_key(buffer, field, 2u);
    wire_varint(buffer, size);
    if (size > sizeof(buffer->bytes) - buffer->size ||
        (size && !bytes)) {
        buffer->ok = 0;
        return;
    }
    if (size) {
        memcpy(buffer->bytes + buffer->size, bytes, size);
        buffer->size += size;
    }
}

static int make_down_event(struct p4_hosted_inbox_item *item)
{
    struct wire_buffer nested, wrapper;
    uint8_t ssid[P4_HOSTED_ASSOCIATION_SSID_SIZE] = {0};
    uint8_t bssid[P4_HOSTED_ASSOCIATION_BSSID_SIZE] =
        {0x02u, 0x10u, 0x20u, 0x30u, 0x40u, 0x50u};
    static const uint8_t hint[] = "delayed-event";
    memcpy(ssid, hint, sizeof(hint) - 1u);
    wire_init(&nested);
    wire_bytes_field(&nested, 1u, ssid, sizeof(ssid));
    wire_uint_field(&nested, 2u, (uint32_t)(sizeof(hint) - 1u));
    wire_bytes_field(&nested, 3u, bssid, sizeof(bssid));
    wire_uint_field(&nested, 4u, 8u);
    wire_init(&wrapper);
    wire_uint_field(&wrapper, 1u, 0u);
    wire_bytes_field(&wrapper, 2u, nested.bytes, nested.size);
    if (!nested.ok || !wrapper.ok || wrapper.size > sizeof(item->payload))
        return 0;
    memset(item, 0, sizeof(*item));
    item->msg_type = P4_HOSTED_RPC_TYPE_EVENT;
    item->msg_id = 776u;
    item->uid = 0u;
    item->payload_size = wrapper.size;
    memcpy(item->payload, wrapper.bytes, wrapper.size);
    return 1;
}

static int make_connected_event(struct p4_hosted_inbox_item *item)
{
    struct wire_buffer nested, wrapper;
    uint8_t ssid[P4_HOSTED_ASSOCIATION_SSID_SIZE] = {0};
    static const uint8_t bssid[6] = {0x02u, 0x11u, 0x22u,
                                     0x33u, 0x44u, 0x55u};
    memcpy(ssid, TEST_SSID, sizeof(TEST_SSID) - 1u);
    wire_init(&nested);
    wire_bytes_field(&nested, 1u, ssid, sizeof(ssid));
    wire_uint_field(&nested, 2u, sizeof(TEST_SSID) - 1u);
    wire_bytes_field(&nested, 3u, bssid, sizeof(bssid));
    wire_uint_field(&nested, 4u, 6u);
    wire_uint_field(&nested, 5u, 0u);
    wire_init(&wrapper);
    wire_uint_field(&wrapper, 1u, 0u);
    wire_bytes_field(&wrapper, 2u, nested.bytes, nested.size);
    if (!nested.ok || !wrapper.ok || wrapper.size > sizeof(item->payload))
        return 0;
    memset(item, 0, sizeof(*item));
    item->msg_type = P4_HOSTED_RPC_TYPE_EVENT;
    item->msg_id = 775u;
    item->uid = 0u;
    item->payload_size = wrapper.size;
    memcpy(item->payload, wrapper.bytes, wrapper.size);
    return 1;
}

static void build_ap_body(struct fixture *state, int32_t status,
                          struct wire_buffer *body)
{
    struct wire_buffer record;
    uint8_t bssid[6] = {0x02u, 0x11u, 0x22u, 0x33u, 0x44u, 0x55u};
    uint8_t ssid[32] = {0};
    uint32_t channel = 6u;
    int32_t authmode = state->is_protected ? 3 : 0;
    int32_t pairwise = state->is_protected ? 4 : 0;
    int32_t group = pairwise;
    size_t ssid_length = sizeof(TEST_SSID) - 1u;
    memcpy(ssid, TEST_SSID, ssid_length);
    switch (state->ap_fault) {
    case AP_WRONG_SSID: ssid[0] ^= 0x20u; break;
    case AP_WRONG_BSSID: bssid[5] ^= 0x01u; break;
    case AP_WRONG_AUTHMODE: authmode = state->is_protected ? 0 : 3; break;
    case AP_WRONG_PAIRWISE: pairwise ^= 1; break;
    case AP_WRONG_GROUP: group ^= 1; break;
    case AP_WRONG_CHANNEL: channel = 7u; break;
    case AP_ZERO_BSSID: memset(bssid, 0, sizeof(bssid)); break;
    case AP_MULTICAST_BSSID: bssid[0] = 0x03u; break;
    case AP_CHANNEL_ZERO: channel = 0u; break;
    case AP_CHANNEL_FIFTEEN: channel = 15u; break;
    default: break;
    }
    wire_init(&record);
    wire_bytes_field(&record, 1u, bssid, sizeof(bssid));
    wire_bytes_field(&record, 2u, ssid, ssid_length);
    wire_uint_field(&record, 3u, channel);
    wire_uint_field(&record, 6u, (uint32_t)authmode);
    wire_uint_field(&record, 7u, (uint32_t)pairwise);
    wire_uint_field(&record, 8u, (uint32_t)group);
    wire_init(body);
    wire_uint_field(body, 1u, (uint32_t)status);
    if (status == 0)
        wire_bytes_field(body, 2u, record.bytes, record.size);
    CHECK(record.ok && body->ok);
}

static int publish_response(struct fixture *state,
    struct p4_hosted_owner *owner, const struct p4_hosted_rpc *request,
    int32_t status)
{
    struct p4_hosted_rpc response, decoded;
    struct wire_buffer ap_body;
    uint8_t status_body[512];
    uint8_t response_wire[512];
    size_t payload_size = 0u, response_size = 0u;
    if (request->msg_id == 294u) {
        build_ap_body(state, status, &ap_body);
        memcpy(status_body, ap_body.bytes, ap_body.size);
        payload_size = ap_body.size;
    } else {
        struct wire_buffer body;
        wire_init(&body);
        wire_uint_field(&body, 1u, (uint32_t)status);
        CHECK(body.ok && body.size <= sizeof(status_body));
        memcpy(status_body, body.bytes, body.size);
        payload_size = body.size;
    }
    response.msg_type = P4_HOSTED_RPC_TYPE_RESPONSE;
    response.msg_id = request->msg_id + 256u;
    response.uid = request->uid +
        (state->malformed_response_request_id == request->msg_id ? 1u : 0u);
    response.payload = status_body;
    response.payload_size = payload_size;
    if (!p4_hosted_rpc_encode(response_wire, sizeof(response_wire),
                              &response, &response_size) ||
        !p4_hosted_rpc_decode(response_wire, response_size, &decoded) ||
        decoded.payload_size > sizeof(owner->reply.payload))
        return 0;
    memset(&owner->reply, 0, sizeof(owner->reply));
    owner->reply.msg_type = decoded.msg_type;
    owner->reply.msg_id = decoded.msg_id;
    owner->reply.uid = decoded.uid;
    owner->reply.payload_size = decoded.payload_size;
    memcpy(owner->reply.payload, decoded.payload, decoded.payload_size);
    return 1;
}

/* Fake one bounded owner transaction. The real request encoder/UID ledger,
 * RPC envelope codec, status/AP decoder and link event callback remain under
 * test. This does not model owner RX polling/reassembly or controller timing. */
int p4_hosted_owner_exchange(struct p4_hosted_owner *owner,
    struct p4_hosted_fifo *fifo, const struct p4_hosted_owner_ops *ops,
    uint32_t generation, unsigned checksum_enabled,
    const uint8_t *request_bytes, size_t request_size, uint32_t timeout_us,
    struct p4_hosted_owner_result *result)
{
    struct fixture *state = &f;
    struct p4_hosted_rpc request;
    struct p4_hosted_inbox_item event_item;
    struct wifi_probe_context *context = ops ? ops->context : NULL;
    ULONG wait_us, slice_us;
    int32_t status = 0;
    int stop;
    unsigned occurrence;
    if (!owner || !fifo || !ops || !result || !request_bytes || !context)
        return 0;
    memset(result, 0, sizeof(*result));
    ++state->exchange_calls;
    if (timeout_us > state->max_timeout)
        state->max_timeout = timeout_us;
    CHECK(timeout_us > 0u && timeout_us <= 8000000u);
    if (!p4_hosted_rpc_decode(request_bytes, request_size, &request) ||
        request.msg_type != P4_HOSTED_RPC_TYPE_REQUEST ||
        request.uid != owner->ids.active_uid ||
        request.msg_id != owner->ids.active_request_id ||
        request.uid == 0u || generation != context->generation ||
        fifo->generation != generation || owner->generation != generation ||
        checksum_enabled != 0u || request.msg_id >= 600u) {
        ++state->malformed_request_count;
        result->error = P4_HOSTED_OWNER_PREFLIGHT;
        return 0;
    }
    occurrence = ++state->request_count[request.msg_id];
    if (request.msg_id == 284u && state->is_protected) {
        const uint8_t *needle = watched_secret;
        size_t i;
        for (i = 0u; i + watched_secret_size <= request_size; ++i)
            if (memcmp(request_bytes + i, needle, watched_secret_size) == 0) {
                ++state->secret_seen_in_rpc;
                break;
            }
    }

    /* This is the owner lifetime preflight before touching the controller. */
    if (wifi_owner_stopping(context)) {
        ++state->no_response_count;
        result->error = P4_HOSTED_OWNER_STOPPED;
        return 0; /* No TX; wrapper must retire the reservation. */
    }
    wait_us = timeout_us < P4_HOSTED_OWNER_MAX_ACQUIRE_US
        ? timeout_us : P4_HOSTED_OWNER_MAX_ACQUIRE_US;
    if (!wait_us || !wifi_owner_acquire(context, wait_us)) {
        result->error = P4_HOSTED_OWNER_ACQUIRE;
        return 0;
    }
    slice_us = timeout_us < P4_HOSTED_OWNER_MAX_SLICE_US
        ? timeout_us : P4_HOSTED_OWNER_MAX_SLICE_US;

    if (request.msg_id == 284u && state->deadline_during_config) {
        unsigned cancel_calls_before = state->cancel_calls;
        fake_now = context->association_started + 8000000u;
        stop = wifi_owner_stopping(context);
        CHECK(stop == 1);
        CHECK(state->cancel_calls == cancel_calls_before);
        wifi_owner_release(context);
        result->error = P4_HOSTED_OWNER_TIMEOUT;
        result->submitted = 0u;
        ++state->no_response_count;
        return 0;
    }
    if (!wifi_owner_budget(context, slice_us)) {
        wifi_owner_release(context);
        result->error = P4_HOSTED_OWNER_BUDGET;
        return 0;
    }
    if (request.msg_id == 284u && state->cancel_after_config) {
        unsigned cancel_calls_before = state->cancel_calls;
        state->cancel_requested = TRUE;
        stop = wifi_owner_stopping(context); /* Still owned: hook deferred. */
        CHECK(stop == 0);
        CHECK(state->cancel_calls == cancel_calls_before);
    } else {
        stop = wifi_owner_stopping(context);
        if (stop) {
            wifi_owner_release(context);
            result->error = P4_HOSTED_OWNER_STOPPED;
            result->submitted = 0u;
            ++state->no_response_count;
            return 0;
        }
    }
    wifi_owner_release(context);

    if (request.msg_id == state->timeout_request_id &&
        (!state->timeout_occurrence || occurrence == state->timeout_occurrence)) {
        result->error = P4_HOSTED_OWNER_TIMEOUT;
        result->submitted = 1u;
        ++state->submitted_count[request.msg_id];
        ++state->no_response_count;
        (void)p4_hosted_owner_cancel(owner, fifo, generation);
        return 0;
    }

    if (request.msg_id == 294u) {
        ++state->ap_info_calls;
        if (state->request_count[282u] == 0u) {
            if (state->initial_event_once && state->ap_info_calls == 1u) {
                CHECK(make_down_event(&event_item));
                CHECK(wifi_owner_event(context, &event_item) == 1);
            } else if (state->repeat_initial_event) {
                CHECK(make_down_event(&event_item));
                CHECK(wifi_owner_event(context, &event_item) == 1);
            }
            status = 0x300f;
        } else {
            ++state->final_ap_calls;
            if (state->postreply_disconnect_once &&
                state->final_ap_calls == 1u) {
                CHECK(make_down_event(&event_item));
                CHECK(wifi_owner_event(context, &event_item) == 1);
                status = 0;
            } else if (state->repeat_final_event) {
                CHECK(make_down_event(&event_item));
                CHECK(wifi_owner_event(context, &event_item) == 1);
                status = 0;
            } else if (state->postreply_disconnect_once) {
                status = 5; /* Fresh query supersedes the stale AP success. */
            } else {
                status = 0;
            }
            if (state->generation_change_final && state->final_ap_calls == 1u)
                ++context->generation;
        }
    }
    if (request.msg_id == state->fail_id &&
               (!state->fail_occurrence || occurrence == state->fail_occurrence)) {
        status = state->fail_status;
    }

    if (request.msg_id == 283u && state->break_after_initial_disconnect &&
        occurrence == 1u)
        fake_signals |= SIGBREAKF_CTRL_C;
    if (!publish_response(state, owner, &request, status)) {
        result->error = P4_HOSTED_OWNER_PROTOCOL;
        result->submitted = 1u;
        (void)p4_hosted_owner_cancel(owner, fifo, generation);
        return 0;
    }
    result->error = P4_HOSTED_OWNER_OK;
    result->submitted = 1u;
    result->matched = 1u;
    ++state->submitted_count[request.msg_id];
    result->uid = request.uid;
    result->request_id = request.msg_id;
    result->elapsed_us = 1000u;
    if (!p4_hosted_request_ids_finish(&owner->ids, generation,
                                      request.uid, request.msg_id)) {
        result->error = P4_HOSTED_OWNER_SESSION;
        (void)p4_hosted_owner_cancel(owner, fifo, generation);
        return 0;
    }
    fake_now += 1000u;
    if (state->cancel_final_snapshot && request.msg_id == 294u &&
        state->request_count[282u] != 0u &&
        state->final_ap_calls == 1u && status == 0)
        state->cancel_on_final_release = 1u;
    return 1;
}

static void fixture_init(void)
{
    memset(&f, 0, sizeof(f));
    fake_now = 100000u;
    fake_signals = 0u;
    delay_calls = 0u;
    set_signal_calls = 0u;
    semaphore_depth_errors = 0u;
    watched_secret_wipes = 0u;
    watched_secret_size = 0u;
    memset(watched_secret, 0, sizeof(watched_secret));
    f.host.generation = TEST_GENERATION;
    f.host.now_us = fake_now_us;
    f.host.slot_clock[0] = TEST_PEER_CLOCK;
    f.host.acquire = fake_host_acquire;
    f.host.release = fake_host_release;
    f.host.set_budget = fake_host_set_budget;
    f.context.host = &f.host;
    f.context.generation = TEST_GENERATION;
    f.context.peer_clock = TEST_PEER_CLOCK;
    f.base.ready = TRUE;
    f.base.association_enabled = TRUE;
    f.base.associated = TRUE;
    memset(&f.base.confirmed_ap, 0xa5, sizeof(f.base.confirmed_ap));
    f.base.owner_task = current_task;
    f.base.client_lock.ss_Owner = current_task;
    f.base.client_lock.depth = 1u;
    f.base.runtime = &f.runtime;
    f.runtime.context = &f.context;
    f.runtime.owner = &f.owner;
    f.runtime.control = &f.control;
    f.runtime.fifo = &f.fifo;
    f.runtime.servicing = TRUE;
    f.control.ops.context = &f.context;
    f.control.ops.dispatch_event = wifi_owner_event;
    wifi_resource_base = &f.base;
    CHECK(p4_hosted_link_init(&f.context.link_observation, TEST_GENERATION));
    CHECK(p4_hosted_rx_start(&f.owner.rx, TEST_GENERATION));
    CHECK(p4_hosted_fifo_start(&f.fifo, TEST_GENERATION, TEST_POOL));
    CHECK(p4_hosted_owner_ids_init(&f.owner, TEST_GENERATION, fake_now));
}

static void target_init(struct p4_wifi_association_target *target,
                        unsigned protected_network)
{
    static const uint8_t bssid[6] = {0x02u, 0x11u, 0x22u, 0x33u, 0x44u, 0x55u};
    memset(target, 0, sizeof(*target));
    memcpy(target->ssid, TEST_SSID, sizeof(TEST_SSID) - 1u);
    target->ssid_length = sizeof(TEST_SSID) - 1u;
    memcpy(target->bssid, bssid, sizeof(bssid));
    target->bssid_set = TRUE;
    target->channel = 6u;
    target->authmode = protected_network ? 3u : 0u;
    if (protected_network) {
        memcpy(target->password, TEST_PASSWORD, sizeof(TEST_PASSWORD) - 1u);
        target->password_length = sizeof(TEST_PASSWORD) - 1u;
        memcpy(watched_secret, target->password, target->password_length);
        watched_secret_size = target->password_length;
        f.is_protected = TRUE;
    }
}

static BOOL invoke(struct p4_wifi_association_target *target)
{
    struct p4_wifi_association_cancel cancel;
    cancel.context = &f;
    cancel.cancelled = fake_cancelled;
    return wifi_device_associate(&f.base, target, &cancel);
}

static void check_restored(BOOL expected_associated)
{
    CHECK(f.context.association_active == FALSE);
    CHECK(f.context.association_started == 0u);
    CHECK(f.context.association_cancel == NULL);
    CHECK(f.runtime.scanning == FALSE);
    CHECK(f.runtime.servicing == TRUE);
    CHECK(f.base.associated == expected_associated);
    CHECK(f.base.client_lock.ss_Owner == current_task);
    CHECK(f.base.client_lock.depth == 1u);
    CHECK(f.host.lock.ss_Owner == NULL);
    CHECK(f.host.lock.depth == 0u);
    CHECK(semaphore_depth_errors == 0u);
}

static void check_secret_wiped(void)
{
    CHECK(watched_secret_wipes != 0u);
    CHECK(bytes_zero(&f.control.sta_config, sizeof(f.control.sta_config)));
    CHECK(bytes_zero(f.control.request, sizeof(f.control.request)));
    CHECK(bytes_zero(&f.control.ap_info, sizeof(f.control.ap_info)));
}

static void test_success_open_and_protected(void)
{
    struct p4_wifi_association_target target, original;
    struct p4_hosted_inbox_item event_item;
    fixture_init();
    target_init(&target, 0u);
    original = target;
    CHECK(invoke(&target) == TRUE);
    CHECK(f.request_count[283u] == 1u);
    CHECK(f.request_count[294u] == 2u);
    CHECK(f.request_count[284u] == 1u);
    CHECK(f.request_count[282u] == 1u);
    CHECK(f.malformed_request_count == 0u);
    CHECK(f.acquire_calls == f.release_calls);
    CHECK(f.budget_calls != 0u);
    CHECK(f.max_timeout <= 8000000u);
    CHECK(memcmp(&target, &original, sizeof(target)) == 0);
    CHECK(f.base.associated == FALSE);
    check_confirmed_ap_matches(0u);
    CHECK(f.acquire_calls == f.release_calls);
    CHECK(f.host.lock.ss_Owner == NULL && f.host.lock.depth == 0u);
    CHECK(make_connected_event(&event_item));
    CHECK(wifi_owner_event(&f.context, &event_item) == 1);
    CHECK(f.context.link_observation.kind == P4_HOSTED_LINK_UP_HINT);
    CHECK(f.base.associated == FALSE);
    check_confirmed_ap_cleared();
    CHECK(bytes_zero(&f.control.sta_config, sizeof(f.control.sta_config)));
    CHECK(bytes_zero(f.control.request, sizeof(f.control.request)));
    CHECK(bytes_zero(&f.control.ap_info, sizeof(f.control.ap_info)));
    check_restored(FALSE);

    fixture_init();
    target_init(&target, 1u);
    original = target;
    CHECK(invoke(&target) == TRUE);
    CHECK(f.request_count[284u] == 1u && f.secret_seen_in_rpc == 1u);
    CHECK(memcmp(&target, &original, sizeof(target)) == 0);
    CHECK(f.base.associated == FALSE);
    check_confirmed_ap_matches(3u);
    CHECK(f.acquire_calls == f.release_calls);
    CHECK(f.host.lock.ss_Owner == NULL && f.host.lock.depth == 0u);
    check_secret_wiped();
    check_restored(FALSE);
}

static void test_final_cancellation_refuses_snapshot(void)
{
    struct p4_wifi_association_target target;
    fixture_init();
    target_init(&target, 0u);
    f.cancel_final_snapshot = 1u;
    CHECK(invoke(&target) == FALSE);
    CHECK(f.final_ap_calls == 1u);
    CHECK(f.cancel_requested == TRUE);
    CHECK(f.cancel_calls != 0u);
    CHECK(f.base.associated == FALSE);
    check_confirmed_ap_cleared();
    CHECK(f.acquire_calls == f.release_calls);
    CHECK(f.host.lock.ss_Owner == NULL && f.host.lock.depth == 0u);
    check_restored(FALSE);
}

static void test_default_gate_and_validation_refuse_without_rpc(void)
{
    struct p4_wifi_association_target target, original;
    unsigned invalid_case;
    fixture_init();
    target_init(&target, 0u);
    f.base.association_enabled = FALSE;
    CHECK(invoke(&target) == FALSE);
    CHECK(f.exchange_calls == 0u);
    check_restored(TRUE);
    check_confirmed_ap_preserved();

    for (invalid_case = 0u; invalid_case < 7u; ++invalid_case) {
        fixture_init();
        target_init(&target, 1u);
        switch (invalid_case) {
        case 0: target.ssid_length = 0u; break;
        case 1: target.ssid_length = 33u; break;
        case 2: target.channel = 15u; break;
        case 3: target.authmode = 1u; break;
        case 4: target.password_length = 7u; break;
        case 5: target.bssid[0] = 0x03u; break;
        default: target.password[0] = '\n'; break;
        }
        original = target;
        CHECK(invoke(&target) == FALSE);
        CHECK(f.exchange_calls == 0u);
        CHECK(memcmp(&target, &original, sizeof(target)) == 0);
        check_restored(TRUE);
        check_confirmed_ap_preserved();
    }
    CHECK(!wifi_association_target_valid(NULL));
}

static void test_remote_failures_and_absence_requirements(void)
{
    struct p4_wifi_association_target target;
    unsigned scenario;
    for (scenario = 0u; scenario < 5u; ++scenario) {
        fixture_init();
        target_init(&target, 1u);
        if (scenario == 0u) {
            f.fail_id = 283u; f.fail_occurrence = 1u; f.fail_status = 7;
        } else if (scenario == 1u) {
            f.fail_id = 284u; f.fail_occurrence = 1u; f.fail_status = 7;
        } else if (scenario == 2u) {
            f.fail_id = 282u; f.fail_occurrence = 1u; f.fail_status = 7;
        } else if (scenario == 3u) {
            f.fail_id = 294u; f.fail_occurrence = 1u; f.fail_status = 5;
        } else {
            f.fail_id = 294u; f.fail_occurrence = 1u; f.fail_status = 0;
        }
        CHECK(invoke(&target) == FALSE);
        CHECK(f.request_count[282u] == (scenario == 2u ? 1u : 0u));
        CHECK(f.request_count[284u] == (scenario == 1u || scenario == 2u ? 1u : 0u));
        if (scenario == 0u) CHECK(f.request_count[283u] == 2u);
        if (scenario >= 3u) CHECK(f.request_count[294u] == 1u);
        check_secret_wiped();
        check_confirmed_ap_cleared();
        check_restored(FALSE);
    }
}

static void test_ap_identity_cipher_and_channel_validation(void)
{
    struct p4_wifi_association_target target;
    unsigned fault;
    for (fault = AP_WRONG_SSID; fault <= AP_CHANNEL_FIFTEEN; ++fault) {
        fixture_init();
        target_init(&target, 1u);
        f.ap_fault = (int)fault;
        if (fault == AP_ZERO_BSSID || fault == AP_MULTICAST_BSSID)
            target.bssid_set = FALSE; /* Malformed AP is invalid even unrestricted. */
        if (fault == AP_CHANNEL_ZERO || fault == AP_CHANNEL_FIFTEEN)
            target.channel = 0u; /* AP channel itself must still be valid. */
        CHECK(invoke(&target) == FALSE);
        CHECK(f.request_count[284u] == 1u && f.request_count[282u] == 1u);
        CHECK(f.final_ap_calls == 1u);
        check_secret_wiped();
        check_confirmed_ap_cleared();
        check_restored(FALSE);
    }
}

static void test_initial_and_final_event_races(void)
{
    struct p4_wifi_association_target target;
    fixture_init();
    target_init(&target, 0u);
    f.initial_event_once = 1u;
    CHECK(invoke(&target) == TRUE);
    CHECK(f.request_count[294u] == 3u); /* Retry absence query, then final AP. */
    CHECK(f.request_count[284u] == 1u);
    CHECK(f.base.associated == FALSE);
    check_confirmed_ap_matches(0u);
    CHECK(f.acquire_calls == f.release_calls);
    CHECK(f.host.lock.ss_Owner == NULL && f.host.lock.depth == 0u);
    check_restored(FALSE);

    fixture_init();
    target_init(&target, 0u);
    f.postreply_disconnect_once = 1u;
    CHECK(invoke(&target) == FALSE);
    CHECK(f.final_ap_calls == 2u); /* Stale success discarded; fresh query fails. */
    CHECK(f.request_count[284u] == 1u && f.request_count[282u] == 1u);
    check_confirmed_ap_cleared();
    check_restored(FALSE);
}

static void test_bounded_repeated_event_races(void)
{
    struct p4_wifi_association_target target;
    fixture_init();
    target_init(&target, 0u);
    f.repeat_initial_event = 1u;
    CHECK(invoke(&target) == FALSE);
    CHECK(f.request_count[294u] <= 500u);
    CHECK(f.request_count[284u] == 0u);
    CHECK(fake_now - 100000u >= 8000000u);
    check_restored(FALSE);

    fixture_init();
    target_init(&target, 0u);
    f.repeat_final_event = 1u;
    CHECK(invoke(&target) == FALSE);
    CHECK(f.final_ap_calls <= 500u);
    CHECK(f.request_count[284u] == 1u && f.request_count[282u] == 1u);
    CHECK(fake_now - 100000u >= 8000000u);
    check_restored(FALSE);
}

static void test_cancel_break_timeout_and_generation_retirement(void)
{
    struct p4_wifi_association_target target;
    fixture_init();
    target_init(&target, 1u);
    f.cancel_requested = TRUE;
    CHECK(invoke(&target) == FALSE);
    CHECK(f.exchange_calls == 0u);
    CHECK(f.cancel_calls != 0u && f.cancel_called_owned == 0u);
    check_restored(TRUE);
    check_confirmed_ap_preserved();

    fixture_init();
    target_init(&target, 1u);
    f.cancel_after_config = 1u;
    CHECK(invoke(&target) == FALSE);
    CHECK(f.request_count[284u] == 1u && f.submitted_count[282u] == 0u);
    CHECK(f.cancel_called_owned == 0u);
    check_secret_wiped();
    check_confirmed_ap_cleared();
    check_restored(FALSE);

    fixture_init();
    target_init(&target, 1u);
    f.break_after_initial_disconnect = 1u;
    CHECK(invoke(&target) == FALSE);
    CHECK(f.request_count[283u] == 1u && f.submitted_count[294u] == 0u);
    CHECK(f.submitted_count[284u] == 0u);
    CHECK(set_signal_calls != 0u);
    check_secret_wiped();
    check_confirmed_ap_cleared();
    check_restored(FALSE);

    fixture_init();
    target_init(&target, 1u);
    f.deadline_during_config = 1u;
    CHECK(invoke(&target) == FALSE);
    CHECK(f.request_count[284u] == 1u && f.submitted_count[282u] == 0u);
    CHECK(f.cancel_called_owned == 0u);
    CHECK(fake_now - 100000u >= 8000000u);
    check_secret_wiped();
    check_confirmed_ap_cleared();
    check_restored(FALSE);

    fixture_init();
    target_init(&target, 1u);
    f.generation_change_final = 1u;
    CHECK(invoke(&target) == FALSE);
    CHECK(f.final_ap_calls == 1u);
    CHECK(f.base.associated == FALSE);
    CHECK(f.owner.poisoned == 1u || f.owner.ids.retired == 1u);
    check_secret_wiped();
    check_confirmed_ap_cleared();
    check_restored(FALSE);
}

static void test_transport_timeout_and_malformed_correlation(void)
{
    struct p4_wifi_association_target target;
    unsigned request_id;
    for (request_id = 282u; request_id <= 284u; ++request_id) {
        if (request_id == 283u) continue;
        fixture_init();
        target_init(&target, 1u);
        f.malformed_response_request_id = request_id;
        CHECK(invoke(&target) == FALSE);
        CHECK(f.owner.poisoned == 1u);
        if (request_id == 284u) {
            CHECK(f.request_count[284u] == 1u && f.request_count[282u] == 0u);
        } else {
            CHECK(f.request_count[284u] == 1u && f.request_count[282u] == 1u);
            CHECK(f.request_count[294u] == 1u);
        }
        check_secret_wiped();
        check_confirmed_ap_cleared();
        check_restored(FALSE);
    }

    fixture_init();
    target_init(&target, 1u);
    f.malformed_response_request_id = 283u;
    CHECK(invoke(&target) == FALSE);
    CHECK(f.request_count[283u] == 1u && f.owner.poisoned == 1u);
    check_secret_wiped();
    check_confirmed_ap_cleared();
    check_restored(FALSE);

    fixture_init();
    target_init(&target, 1u);
    f.timeout_request_id = 282u;
    f.timeout_occurrence = 1u;
    CHECK(invoke(&target) == FALSE);
    CHECK(f.request_count[282u] == 1u && f.submitted_count[282u] == 1u);
    CHECK(f.owner.poisoned == 1u);
    check_secret_wiped();
    check_confirmed_ap_cleared();
    check_restored(FALSE);
}

int main(void)
{
    test_success_open_and_protected();
    test_final_cancellation_refuses_snapshot();
    test_default_gate_and_validation_refuse_without_rpc();
    test_remote_failures_and_absence_requirements();
    test_ap_identity_cipher_and_channel_validation();
    test_initial_and_final_event_races();
    test_bounded_repeated_event_races();
    test_cancel_break_timeout_and_generation_retirement();
    test_transport_timeout_and_malformed_correlation();
    if (f.acquire_calls != f.release_calls || f.cancel_called_owned ||
        f.malformed_request_count || semaphore_depth_errors) {
        fprintf(stderr, "association fixture invariant failed\n");
        return 1;
    }
    printf("wifi owner association source-extracted checks: %lu\n", checks);
    return 0;
}
