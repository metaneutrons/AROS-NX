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
#include <stdarg.h>
#ifndef P4_WIFI_ASSOC_DIAGNOSTICS
#define P4_WIFI_ASSOC_DIAGNOSTICS 1
#endif
#ifndef P4_WIFI_DISCONNECT_EVENT_FENCE
#define P4_WIFI_DISCONNECT_EVENT_FENCE 0
#endif

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
#include "hosted_discovery.h"
#include "hosted_link.h"
#include "hosted_events.h"
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
#define TEST_SAE_PASSWORD "synthetic-sae-test-password"

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
static unsigned diagnostic_count, diagnostic_stage, diagnostic_rpc, diagnostic_ok;
static LONG diagnostic_status;
static LONG diagnostic_rpc_status, diagnostic_event_status, diagnostic_hint_status;
static unsigned diagnostic_valid;
static unsigned diagnostic_rpc_count, diagnostic_event_count;
static unsigned diagnostic_hint_count;
static unsigned diagnostic_prelude_count, diagnostic_prelude_first;
static unsigned diagnostic_prelude_generation, diagnostic_prelude_revision;

/* Consume only the numeric observer's exact formats. No printf of borrowed
 * payloads is allowed through this host boundary. */
static void bug(const char *format, ...)
{
    va_list args;
    if (!strcmp(format, "[WiFiJoin] s=0 r=0 st=-1 ok=0\n") ||
        !strcmp(format, "[WiFiJoin] s=1 r=0 st=-1 ok=0\n")) {
        ++diagnostic_count;
        diagnostic_stage = format[13] == '0' ? 0u : 1u;
        diagnostic_rpc = diagnostic_ok = 0;
        diagnostic_status = -1;
        return;
    }
    if (!strcmp(format, "[WiFiRPC] p=1 id=%u u=%u us=%u\n")) {
        unsigned id, uid, usec;
        va_start(args, format);
        id = va_arg(args, unsigned);
        uid = va_arg(args, unsigned);
        usec = va_arg(args, unsigned);
        va_end(args);
        CHECK(id == 282u || id == 283u);
        CHECK(uid != 0u);
        (void)usec;
        ++diagnostic_rpc_count;
        return;
    }
    if (!strcmp(format,
            "[WiFiRPC] p=2 id=%u u=%u us=%u sub=%u v=0 st=-1\n")) {
        unsigned id, uid, usec, submitted;
        va_start(args, format);
        id = va_arg(args, unsigned);
        uid = va_arg(args, unsigned);
        usec = va_arg(args, unsigned);
        submitted = va_arg(args, unsigned);
        va_end(args);
        CHECK(id == 282u || id == 283u);
        CHECK(uid != 0u && submitted <= 1u);
        (void)usec;
        ++diagnostic_rpc_count;
        return;
    }
    if (!strcmp(format,
            "[WiFiRPC] p=2 id=%u u=%u us=%u sub=%u v=1 st=%ld\n")) {
        unsigned id, uid, usec, submitted;
        LONG status;
        va_start(args, format);
        id = va_arg(args, unsigned);
        uid = va_arg(args, unsigned);
        usec = va_arg(args, unsigned);
        submitted = va_arg(args, unsigned);
        status = va_arg(args, LONG);
        va_end(args);
        CHECK(id == 282u || id == 283u);
        CHECK(uid != 0u && submitted == 1u);
        CHECK(status >= INT32_MIN && status <= INT32_MAX);
        (void)usec;
        diagnostic_rpc_status = status;
        ++diagnostic_rpc_count;
        return;
    }
    if (!strcmp(format,
            "[WiFiEvent] id=%u g=%u rev=%u k=%u st=%ld reason=%u\n")) {
        unsigned id, generation, revision, kind, reason;
        LONG status;
        va_start(args, format);
        id = va_arg(args, unsigned);
        generation = va_arg(args, unsigned);
        revision = va_arg(args, unsigned);
        kind = va_arg(args, unsigned);
        status = va_arg(args, LONG);
        reason = va_arg(args, unsigned);
        va_end(args);
        CHECK((id == 773u || id == 775u || id == 776u) && generation != 0u);
        CHECK(revision != 0u && kind <= P4_HOSTED_LINK_STATION_STOP);
        CHECK(status >= INT32_MIN && status <= INT32_MAX);
        (void)reason;
        diagnostic_event_status = status;
        ++diagnostic_event_count;
        return;
    }
    if (!strcmp(format, "[WiFiHint] id=%u g=%u st=%ld\n")) {
        unsigned id, generation;
        LONG status;
        va_start(args, format);
        id = va_arg(args, unsigned);
        generation = va_arg(args, unsigned);
        status = va_arg(args, LONG);
        va_end(args);
        CHECK(generation != 0u);
        CHECK(status >= INT32_MIN && status <= INT32_MAX);
        (void)id;
        diagnostic_hint_status = status;
        ++diagnostic_hint_count;
        return;
    }
    if (!strcmp(format, "[WiFiPrelude] first=%u g=%u rev=%u\n")) {
        unsigned first, generation, revision;
        va_start(args, format);
        first = va_arg(args, unsigned);
        generation = va_arg(args, unsigned);
        revision = va_arg(args, unsigned);
        va_end(args);
        CHECK(first <= 1u && generation != 0u);
        diagnostic_prelude_first = first;
        diagnostic_prelude_generation = generation;
        diagnostic_prelude_revision = revision;
        ++diagnostic_prelude_count;
        return;
    }
    CHECK(!strcmp(format, "[WiFiJoin] s=%u r=%u st=%ld v=%u ok=%u\n"));
    ++diagnostic_count;
    va_start(args, format);
    diagnostic_stage = va_arg(args, unsigned);
    diagnostic_rpc = va_arg(args, unsigned);
    diagnostic_status = va_arg(args, LONG);
    diagnostic_valid = va_arg(args, unsigned);
    diagnostic_ok = va_arg(args, unsigned);
    va_end(args);
}
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
    uint32_t request_sequence[1024];
    int32_t request_status_sequence[1024];
    unsigned request_sequence_count;
    ULONG first_disconnect_timeout;
    ULONG cleanup_disconnect_timeout;
    unsigned no_response_count;
    unsigned malformed_request_count;
    unsigned secret_seen_in_rpc;
    const UBYTE *caller_password;
    unsigned inspect_sta_config;
    unsigned expected_pmf_capable;
    unsigned expected_pmf_required;
    int32_t expected_sae_pwe;
    int32_t expected_authmode;
    unsigned config_policy_checks;
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
    uint32_t cancel_after_response_id;
    unsigned cancel_after_response_occurrence;
    unsigned expect_disconnect_final_fence;
    unsigned disconnect_intermediate_unknown_checks;
    unsigned disconnect_final_fence_checks;
    int32_t last_ap_info_status;
    unsigned deadline_during_config;
    unsigned break_after_initial_disconnect;
    unsigned generation_change_final;
    unsigned generation_change_first_prelude;
    unsigned cancel_final_snapshot;
    unsigned cancel_on_final_release;
    unsigned event_during_config;
    unsigned event_on_connect;
    unsigned duplicate_event_on_connect;
    unsigned disconnect_event_after_connect;
    unsigned event_on_first_final_ap;
    unsigned event_on_commit_acquire;
    unsigned commit_event_injections;
    unsigned event_injections;
    int32_t attempt_authmode;
    int32_t ap_authmode_override;
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
    enum {
        EVENT_EXACT = 0,
        EVENT_WRONG_SSID,
        EVENT_WRONG_BSSID,
        EVENT_WRONG_CHANNEL,
        EVENT_AUTHMODE_3,
        EVENT_AUTHMODE_7
    } event_fault;
    enum {
        PREFLIGHT_NONE = 0,
        PREFLIGHT_DOWN_SUCCESS,
        PREFLIGHT_DOWN_FAILED_STATUS,
        PREFLIGHT_DOWN_WRONG_GENERATION,
        PREFLIGHT_STATION_START,
        PREFLIGHT_STATION_STOP,
        PREFLIGHT_UP_HINT,
        PREFLIGHT_DOWN_THEN_START,
        PREFLIGHT_DOWN_THEN_STOP,
        PREFLIGHT_DOWN_THEN_UP
    } preflight_event;
    unsigned preflight_event_occurrence;
    unsigned preflight_event_injections;
    unsigned config_after_preflight_down;
    unsigned config_without_required_preflight_down;
    uint32_t disconnect_baseline_revision;
    int32_t initial_disconnect_status;
    unsigned ap_info_calls;
    unsigned final_ap_calls;
    BOOL cancel_requested;
    BOOL is_protected;
    BOOL is_sae;
};

static struct fixture f;

static void check_request_order(const uint32_t *expected, unsigned count)
{
    unsigned index;
    CHECK(expected != NULL);
    if (f.request_sequence_count != count) {
        fprintf(stderr, "request order count got=%u expected=%u:",
                f.request_sequence_count, count);
        for (index = 0u; index < f.request_sequence_count; ++index)
            fprintf(stderr, " %u", f.request_sequence[index]);
        fputc('\n', stderr);
    }
    CHECK(f.request_sequence_count == count);
    for (index = 0u; index < count; ++index)
        CHECK(f.request_sequence[index] == expected[index]);
}

static int inject_connected_event(struct fixture *state,
                                  struct wifi_probe_context *context);
static int inject_disconnected_event(struct fixture *state,
                                     struct wifi_probe_context *context);

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
    CHECK(memcmp(&f.base.observed_ap, &expected, sizeof(expected)) == 0);
    CHECK(f.base.link_state == P4_WIFI_LINK_UP);
}

static void check_typed_acceptance(ULONG profile)
{
    CHECK(f.context.accepted_profile == profile);
    CHECK(f.context.accepted_generation == TEST_GENERATION);
    CHECK(f.context.accepted_revision != 0u);
    CHECK(f.context.accepted_revision == f.context.link_observation.revision);
    CHECK(memcmp(&f.context.accepted_ap, &f.base.confirmed_ap,
                 sizeof(f.context.accepted_ap)) == 0);
}

static void check_acceptance_cleared(void)
{
    CHECK(f.context.accepted_profile == 0u);
    CHECK(f.context.accepted_generation == 0u);
    CHECK(f.context.accepted_revision == 0u);
    CHECK(bytes_zero(&f.context.accepted_ap, sizeof(f.context.accepted_ap)));
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
    if (f.expect_disconnect_final_fence && f.request_count[294u] == 2u &&
        f.last_ap_info_status == 0x300f) {
        CHECK(f.base.link_state == P4_WIFI_LINK_UNKNOWN);
        CHECK(f.base.associated == FALSE);
        CHECK(f.context.generation == TEST_GENERATION);
        CHECK(f.context.link_observation.generation == TEST_GENERATION);
        CHECK(f.context.link_observation.revision == 0u);
        CHECK(f.host.generation == TEST_GENERATION);
        CHECK(f.owner.poisoned == 0u && f.fifo.poisoned == 0u);
        ++f.disconnect_final_fence_checks;
    }
    if (host->lock.ss_Owner || f.host.dma_faulted)
        return FALSE;
    host->lock.ss_Owner = current_task;
    host->lock.depth = 1u;
    if (f.event_on_commit_acquire && f.request_count[282u] != 0u &&
        f.final_ap_calls == 1u) {
        f.event_on_commit_acquire = 0u;
        ++f.commit_event_injections;
        CHECK(inject_disconnected_event(&f, &f.context));
    }
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

static int make_down_event_values(struct p4_hosted_inbox_item *item,
                                 int32_t status, uint32_t reason)
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
    wire_uint_field(&nested, 4u, reason);
    wire_init(&wrapper);
    wire_uint_field(&wrapper, 1u, (uint32_t)status);
    if (status == 0)
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

static int make_down_event(struct p4_hosted_inbox_item *item)
{
    return make_down_event_values(item, 0, 0u);
}

static int make_wifi_lifecycle_event(struct p4_hosted_inbox_item *item,
                                     int32_t event_id)
{
    struct wire_buffer body;
    wire_init(&body);
    wire_uint_field(&body, 1u, 0u);
    wire_uint_field(&body, 2u, (uint32_t)event_id);
    if (!body.ok || body.size > sizeof(item->payload))
        return 0;
    memset(item, 0, sizeof(*item));
    item->msg_type = P4_HOSTED_RPC_TYPE_EVENT;
    item->msg_id = 773u;
    item->uid = 0u;
    item->payload_size = body.size;
    memcpy(item->payload, body.bytes, body.size);
    return 1;
}

static int make_connected_event_values(struct p4_hosted_inbox_item *item,
    const uint8_t *ssid, uint32_t ssid_length, const uint8_t *bssid,
    uint32_t channel, int32_t authmode)
{
    struct wire_buffer nested, wrapper;
    if (!item || !ssid || !bssid ||
        ssid_length > P4_HOSTED_ASSOCIATION_SSID_SIZE) return 0;
    wire_init(&nested);
    wire_bytes_field(&nested, 1u, ssid,
                     P4_HOSTED_ASSOCIATION_SSID_SIZE);
    wire_uint_field(&nested, 2u, ssid_length);
    wire_bytes_field(&nested, 3u, bssid,
                     P4_HOSTED_ASSOCIATION_BSSID_SIZE);
    wire_uint_field(&nested, 4u, channel);
    wire_uint_field(&nested, 5u, (uint32_t)authmode);
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

static int make_connected_event(struct p4_hosted_inbox_item *item)
{
    uint8_t ssid[P4_HOSTED_ASSOCIATION_SSID_SIZE] = {0};
    static const uint8_t bssid[P4_HOSTED_ASSOCIATION_BSSID_SIZE] =
        {0x02u, 0x11u, 0x22u, 0x33u, 0x44u, 0x55u};
    memcpy(ssid, TEST_SSID, sizeof(TEST_SSID) - 1u);
    return make_connected_event_values(item, ssid,
        (uint32_t)(sizeof(TEST_SSID) - 1u), bssid, 6u, 0);
}

static int inject_connected_event(struct fixture *state,
                                  struct wifi_probe_context *context)
{
    struct p4_hosted_inbox_item item;
    uint8_t ssid[P4_HOSTED_ASSOCIATION_SSID_SIZE] = {0};
    uint8_t bssid[P4_HOSTED_ASSOCIATION_BSSID_SIZE] =
        {0x02u, 0x11u, 0x22u, 0x33u, 0x44u, 0x55u};
    uint32_t channel = 6u;
    int32_t authmode = state->attempt_authmode;
    memcpy(ssid, TEST_SSID, sizeof(TEST_SSID) - 1u);
    switch (state->event_fault) {
    case EVENT_WRONG_SSID: ssid[0] ^= 0x20u; break;
    case EVENT_WRONG_BSSID: bssid[5] ^= 1u; break;
    case EVENT_WRONG_CHANNEL: channel = 7u; break;
    case EVENT_AUTHMODE_3: authmode = 3; break;
    case EVENT_AUTHMODE_7: authmode = 7; break;
    default: break;
    }
    if (!make_connected_event_values(&item, ssid,
            (uint32_t)(sizeof(TEST_SSID) - 1u), bssid, channel, authmode) ||
        wifi_owner_event(context, &item) != 1)
        return 0;
    ++state->event_injections;
    return 1;
}

static int inject_disconnected_event(struct fixture *state,
                                     struct wifi_probe_context *context)
{
    struct p4_hosted_inbox_item item;
    if (!make_down_event(&item) || wifi_owner_event(context, &item) != 1)
        return 0;
    ++state->event_injections;
    return 1;
}

static int inject_preflight_event(struct fixture *state,
                                  struct wifi_probe_context *context)
{
    struct p4_hosted_inbox_item item;
    int ok = 0;
    int overwrite = state->preflight_event == PREFLIGHT_DOWN_THEN_START ||
                    state->preflight_event == PREFLIGHT_DOWN_THEN_STOP ||
                    state->preflight_event == PREFLIGHT_DOWN_THEN_UP;

    if (overwrite) {
        ok = make_down_event_values(&item, 0, UINT32_C(0x76543210));
        if (!ok || wifi_owner_event(context, &item) != 1)
            return 0;
        ++state->preflight_event_injections;
        if (state->preflight_event == PREFLIGHT_DOWN_THEN_UP)
            ok = make_connected_event(&item);
        else
            ok = make_wifi_lifecycle_event(&item,
                state->preflight_event == PREFLIGHT_DOWN_THEN_START ?
                    P4_HOSTED_WIFI_EVENT_STA_START :
                    P4_HOSTED_WIFI_EVENT_STA_STOP);
        if (!ok || wifi_owner_event(context, &item) != 1)
            return 0;
        ++state->preflight_event_injections;
        return 1;
    }

    switch (state->preflight_event) {
    case PREFLIGHT_DOWN_SUCCESS:
    case PREFLIGHT_DOWN_WRONG_GENERATION:
        ok = make_down_event_values(&item, 0, UINT32_C(0x76543210));
        break;
    case PREFLIGHT_DOWN_FAILED_STATUS:
        ok = make_down_event_values(&item, 7, 0u);
        break;
    case PREFLIGHT_STATION_START:
        ok = make_wifi_lifecycle_event(&item,
                    P4_HOSTED_WIFI_EVENT_STA_START);
        break;
    case PREFLIGHT_STATION_STOP:
        ok = make_wifi_lifecycle_event(&item,
                    P4_HOSTED_WIFI_EVENT_STA_STOP);
        break;
    case PREFLIGHT_UP_HINT:
        ok = make_connected_event(&item);
        break;
    case PREFLIGHT_NONE:
    default:
        return 0;
    }
    if (!ok || wifi_owner_event(context, &item) != 1)
        return 0;
    ++state->preflight_event_injections;
    if (state->preflight_event == PREFLIGHT_DOWN_WRONG_GENERATION)
        context->link_observation.generation = TEST_GENERATION + 1u;
    return 1;
}

static void build_ap_body(struct fixture *state, int32_t status,
                          struct wire_buffer *body)
{
    struct wire_buffer record;
    uint8_t bssid[6] = {0x02u, 0x11u, 0x22u, 0x33u, 0x44u, 0x55u};
    uint8_t ssid[32] = {0};
    uint32_t channel = 6u;
    int32_t authmode = state->is_sae ? 6 : (state->is_protected ? 3 : 0);
    int32_t pairwise = state->is_protected ? 4 : 0;
    int32_t group = pairwise;
    size_t ssid_length = sizeof(TEST_SSID) - 1u;
    memcpy(ssid, TEST_SSID, ssid_length);
    if (state->ap_authmode_override)
        authmode = state->ap_authmode_override;
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
    unsigned occurrence, sequence_index;
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
    CHECK(state->request_sequence_count <
          sizeof(state->request_sequence) / sizeof(state->request_sequence[0]));
    sequence_index = state->request_sequence_count++;
    state->request_sequence[sequence_index] = request.msg_id;
    state->request_status_sequence[sequence_index] = -1;
    if (request.msg_id == 283u) {
        if (occurrence == 1u)
            state->first_disconnect_timeout = timeout_us;
        else if (occurrence == 2u)
            state->cleanup_disconnect_timeout = timeout_us;
    }
    if (request.msg_id == 284u && state->is_protected) {
        const uint8_t *needle = watched_secret;
        size_t i;
        for (i = 0u; i + watched_secret_size <= request_size; ++i)
            if (memcmp(request_bytes + i, needle, watched_secret_size) == 0) {
                ++state->secret_seen_in_rpc;
                break;
            }
    }
    if (request.msg_id == 284u && P4_WIFI_DISCONNECT_EVENT_FENCE &&
        state->initial_disconnect_status == 0) {
        const struct p4_hosted_link_observation *observation =
            &context->link_observation;
        if (observation->generation == context->generation &&
            observation->revision > state->disconnect_baseline_revision &&
            observation->kind == P4_HOSTED_LINK_DOWN &&
            observation->status == 0) {
            ++state->config_after_preflight_down;
        } else {
            ++state->config_without_required_preflight_down;
        }
    }
    if (request.msg_id == 284u && state->inspect_sta_config) {
        const struct p4_hosted_sta_config *config =
            &state->control.sta_config;
        CHECK(config->threshold_authmode == state->expected_authmode);
        CHECK(config->pmf_capable == state->expected_pmf_capable);
        CHECK(config->pmf_required == state->expected_pmf_required);
        CHECK(config->sae_pwe_h2e == state->expected_sae_pwe);
        CHECK(config->password_length == watched_secret_size);
        CHECK(config->password != NULL);
        CHECK(config->password != state->caller_password);
        if (config->password_length) {
            CHECK(memcmp(config->password, watched_secret,
                         config->password_length) == 0);
        }
        ++state->config_policy_checks;
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
            } else if (context->association_cancel &&
                       state->initial_disconnect_status == 0 &&
                       state->preflight_event != PREFLIGHT_NONE &&
                       state->ap_info_calls == state->preflight_event_occurrence) {
                CHECK(inject_preflight_event(state, context));
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
    if (request.msg_id == 294u && state->request_count[282u] == 0u &&
        state->generation_change_first_prelude &&
        state->ap_info_calls == 1u) {
        state->generation_change_first_prelude = 0u;
        ++context->generation;
    }
    if (request.msg_id == 283u && occurrence == 1u)
        state->initial_disconnect_status = status;
    if (request.msg_id == 294u) {
        state->last_ap_info_status = status;
        if (state->expect_disconnect_final_fence && occurrence == 1u) {
            CHECK(status == 0);
            CHECK(state->base.link_state == P4_WIFI_LINK_UNKNOWN);
            CHECK(state->base.associated == FALSE);
            CHECK(bytes_zero(&state->base.confirmed_ap,
                             sizeof(state->base.confirmed_ap)));
            CHECK(bytes_zero(&state->base.observed_ap,
                             sizeof(state->base.observed_ap)));
            ++state->disconnect_intermediate_unknown_checks;
        }
    }

    /* UID-zero link events are injected while their RPC is drained. The
     * caller must fence observations locally; this is not attempt-ID proof. */
    if (request.msg_id == 284u && state->event_during_config) {
        state->event_during_config = 0u;
        CHECK(inject_connected_event(state, context));
    }
    if (request.msg_id == 282u && state->event_on_connect) {
        state->event_on_connect = 0u;
        CHECK(inject_connected_event(state, context));
        if (state->duplicate_event_on_connect) {
            state->duplicate_event_on_connect = 0u;
            CHECK(inject_connected_event(state, context));
        }
    }
    if (request.msg_id == 282u && state->disconnect_event_after_connect) {
        state->disconnect_event_after_connect = 0u;
        CHECK(inject_disconnected_event(state, context));
    }
    if (request.msg_id == 294u && state->request_count[282u] != 0u &&
        state->final_ap_calls == 1u && state->event_on_first_final_ap) {
        state->event_on_first_final_ap = 0u;
        CHECK(inject_connected_event(state, context));
    }

    if (request.msg_id == 283u && state->break_after_initial_disconnect &&
        occurrence == 1u)
        fake_signals |= SIGBREAKF_CTRL_C;
    state->request_status_sequence[sequence_index] = status;
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
    if (request.msg_id == state->cancel_after_response_id &&
        (!state->cancel_after_response_occurrence ||
         occurrence == state->cancel_after_response_occurrence))
        state->cancel_requested = TRUE;
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
#if P4_WIFI_DISCONNECT_EVENT_FENCE
    f.preflight_event = PREFLIGHT_DOWN_SUCCESS;
    f.preflight_event_occurrence = 1u;
#endif
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
    f.base.link_state = P4_WIFI_LINK_UP;
    memset(&f.base.confirmed_ap, 0xa5, sizeof(f.base.confirmed_ap));
    memset(&f.base.observed_ap, 0xa5, sizeof(f.base.observed_ap));
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

static void target_init_typed_open(struct p4_wifi_association_target *target)
{
    target_init(target, 0u);
    target->fullmac_profile = P4_WIFI_FULLMAC_OPEN;
    target->diagnostic_risk_accepted = 1u;
}

static void target_init_typed_wpa2(struct p4_wifi_association_target *target,
                                   unsigned password_length)
{
    unsigned i;
    target_init(target, 1u);
    target->fullmac_profile = P4_WIFI_FULLMAC_WPA2_CCMP;
    target->diagnostic_risk_accepted = 1u;
    target->password_length = password_length;
    if (password_length == 64u) {
        for (i = 0u; i < password_length; ++i)
            target->password[i] = (UBYTE)((i & 1u) ? 'b' : 'a');
    } else {
        for (i = 0u; i < password_length; ++i)
            target->password[i] = 'p';
    }
    memcpy(watched_secret, target->password, password_length);
    watched_secret_size = password_length;
}

static void target_init_typed_sae(struct p4_wifi_association_target *target,
                                  unsigned pwe, unsigned password_length,
                                  unsigned diagnostic_risk_accepted)
{
    unsigned i;
    target_init(target, 0u);
    target->authmode = 6u;
    target->fullmac_profile = P4_WIFI_FULLMAC_SAE_CCMP_PMF;
    target->pmf_required = 1u;
    target->sae_pwe_h2e = pwe;
    target->diagnostic_risk_accepted = diagnostic_risk_accepted;
    target->password_length = password_length;
    for (i = 0u; i < password_length; ++i)
        target->password[i] = (UBYTE)TEST_SAE_PASSWORD[i %
            (sizeof(TEST_SAE_PASSWORD) - 1u)];
    memcpy(watched_secret, target->password, password_length);
    watched_secret_size = password_length;
    f.is_protected = TRUE;
    f.is_sae = TRUE;
}

static BOOL invoke(struct p4_wifi_association_target *target)
{
    struct p4_wifi_association_cancel cancel;
    cancel.context = &f;
    cancel.cancelled = fake_cancelled;
    f.caller_password = target ? target->password : NULL;
    f.attempt_authmode = target ? (int32_t)target->authmode : -1;
    f.disconnect_baseline_revision = f.context.link_observation.revision;
    return wifi_device_associate(&f.base, target, &cancel);
}

static BOOL invoke_disconnect(void)
{
    return wifi_device_disconnect(&f.base);
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

static void check_open_association_buffers_cleared(void)
{
    CHECK(bytes_zero(&f.control.sta_config, sizeof(f.control.sta_config)));
    CHECK(bytes_zero(f.control.request, sizeof(f.control.request)));
    CHECK(bytes_zero(&f.control.ap_info, sizeof(f.control.ap_info)));
}

static void check_disconnected_down(void)
{
    check_acceptance_cleared();
    CHECK(f.base.associated == FALSE);
    CHECK(f.base.link_state == P4_WIFI_LINK_DOWN);
    CHECK(bytes_zero(&f.base.confirmed_ap, sizeof(f.base.confirmed_ap)));
    CHECK(bytes_zero(&f.base.observed_ap, sizeof(f.base.observed_ap)));
    CHECK(f.context.association_active == FALSE);
    CHECK(f.context.association_started == 0u);
    CHECK(f.runtime.scanning == FALSE && f.runtime.servicing == TRUE);
    CHECK(f.host.lock.ss_Owner == NULL && f.host.lock.depth == 0u);
    CHECK(f.base.client_lock.ss_Owner == current_task);
    CHECK(f.base.client_lock.depth == 1u);
}

static void check_disconnect_failure_unknown(void)
{
    check_acceptance_cleared();
    CHECK(f.base.associated == FALSE);
    CHECK(f.base.link_state == P4_WIFI_LINK_UNKNOWN);
    CHECK(bytes_zero(&f.base.confirmed_ap, sizeof(f.base.confirmed_ap)));
    CHECK(bytes_zero(&f.base.observed_ap, sizeof(f.base.observed_ap)));
    CHECK(f.context.association_active == FALSE);
    CHECK(f.context.association_started == 0u);
    CHECK(f.runtime.scanning == FALSE && f.runtime.servicing == TRUE);
    CHECK(f.host.lock.ss_Owner == NULL && f.host.lock.depth == 0u);
    CHECK(f.base.client_lock.ss_Owner == current_task);
    CHECK(f.base.client_lock.depth == 1u);
}

static void check_disconnect_refused_unchanged(void)
{
    CHECK(f.exchange_calls == 0u);
    CHECK(f.base.associated == TRUE);
    CHECK(f.base.link_state == P4_WIFI_LINK_UP);
    CHECK(f.host.lock.ss_Owner == NULL && f.host.lock.depth == 0u);
    {
        const uint8_t *bytes = (const uint8_t *)&f.base.confirmed_ap;
        size_t index;
        for (index = 0u; index < sizeof(f.base.confirmed_ap); ++index)
            CHECK(bytes[index] == 0xa5u);
        bytes = (const uint8_t *)&f.base.observed_ap;
        for (index = 0u; index < sizeof(f.base.observed_ap); ++index)
            CHECK(bytes[index] == 0xa5u);
    }
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
    CHECK(f.request_count[294u] == 2u + P4_WIFI_DISCONNECT_EVENT_FENCE);
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
    CHECK(f.base.link_state == P4_WIFI_LINK_UNKNOWN);
    CHECK(bytes_zero(&f.base.observed_ap, sizeof(f.base.observed_ap)));
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

static void test_fullmac_typed_profile_association(void)
{
    struct p4_wifi_association_target target, original;
    unsigned pwe;

    fixture_init();
    target_init_typed_open(&target);
    f.inspect_sta_config = 1u;
    f.expected_authmode = 0;
    f.expected_pmf_capable = 0u;
    f.expected_pmf_required = 0u;
    f.expected_sae_pwe = 0;
    f.event_on_connect = 1u;
    original = target;
    CHECK(wifi_association_target_valid(&target));
    CHECK(invoke(&target) == TRUE);
    CHECK(f.config_policy_checks == 1u);
    CHECK(f.request_count[284u] == 1u && f.request_count[282u] == 1u);
    CHECK(f.base.associated == FALSE);
    CHECK(memcmp(&target, &original, sizeof(target)) == 0);
    check_confirmed_ap_matches(0u);
    CHECK(bytes_zero(&f.control.sta_config, sizeof(f.control.sta_config)));
    check_typed_acceptance(P4_WIFI_FULLMAC_OPEN);
    check_restored(FALSE);

    fixture_init();
    target_init_typed_wpa2(&target, 64u);
    f.inspect_sta_config = 1u;
    f.expected_authmode = 3;
    f.expected_pmf_capable = 0u;
    f.expected_pmf_required = 0u;
    f.expected_sae_pwe = 0;
    f.event_on_connect = 1u;
    original = target;
    CHECK(wifi_association_target_valid(&target));
    CHECK(invoke(&target) == TRUE);
    CHECK(f.config_policy_checks == 1u);
    CHECK(f.request_count[284u] == 1u && f.request_count[282u] == 1u);
    CHECK(f.base.associated == FALSE);
    CHECK(f.secret_seen_in_rpc == 1u);
    CHECK(memcmp(&target, &original, sizeof(target)) == 0);
    check_confirmed_ap_matches(3u);
    check_secret_wiped();
    check_restored(FALSE);

    check_typed_acceptance(P4_WIFI_FULLMAC_WPA2_CCMP);
    for (pwe = 1u; pwe <= 3u; ++pwe) {
        fixture_init();
        target_init_typed_sae(&target, pwe, 63u, 1u);
        f.inspect_sta_config = 1u;
        f.expected_authmode = 6;
        f.expected_pmf_capable = 1u;
        f.expected_pmf_required = 1u;
        f.expected_sae_pwe = (int32_t)pwe;
        f.event_on_connect = 1u;
        if (pwe == 3u)
            f.ap_authmode_override = 7; /* Transition BSS, SAE event remains6. */
        original = target;
        CHECK(wifi_association_target_valid(&target));
        CHECK(invoke(&target) == TRUE);
        CHECK(f.config_policy_checks == 1u);
        CHECK(f.request_count[284u] == 1u && f.request_count[282u] == 1u);
        CHECK(f.base.associated == FALSE);
        CHECK(f.secret_seen_in_rpc == 1u);
        CHECK(memcmp(&target, &original, sizeof(target)) == 0);
        check_confirmed_ap_matches(6u);
        check_typed_acceptance(P4_WIFI_FULLMAC_SAE_CCMP_PMF);
        check_secret_wiped();
        check_restored(FALSE);
    }
}

static void init_sae_ap_info(struct p4_hosted_sta_ap_info *ap,
                             int32_t ap_authmode)
{
    static const uint8_t bssid[P4_HOSTED_ASSOCIATION_BSSID_SIZE] =
        {0x02u, 0x11u, 0x22u, 0x33u, 0x44u, 0x55u};
    memset(ap, 0, sizeof(*ap));
    ap->ap.ssid_size = (uint32_t)(sizeof(TEST_SSID) - 1u);
    memcpy(ap->ap.ssid, TEST_SSID, sizeof(TEST_SSID) - 1u);
    memcpy(ap->ap.bssid, bssid, sizeof(bssid));
    ap->ap.primary = 6u;
    ap->ap.authmode = ap_authmode;
    ap->ap.pairwise_cipher = 4;
    ap->ap.group_cipher = 4;
}

static void test_fullmac_event_matcher_strict_identity(void)
{
    struct p4_wifi_association_target target;
    struct p4_hosted_sta_ap_info ap;
    struct p4_hosted_link_observation event, baseline;

    fixture_init();
    target_init_typed_sae(&target, 2u, 16u, 1u);
    f.attempt_authmode = 6;
    CHECK(inject_connected_event(&f, &f.context));
    baseline = f.context.link_observation;
    init_sae_ap_info(&ap, 7); /* BSS transition mode; event reports SAE6. */
    event = baseline;
    CHECK(wifi_association_event_matches(&target, &ap, &event,
                                         TEST_GENERATION, 0u));

#define CHECK_EVENT_REJECT(mutation) do { \
        event = baseline; \
        ap = (struct p4_hosted_sta_ap_info){0}; \
        init_sae_ap_info(&ap, 7); \
        mutation; \
        CHECK(!wifi_association_event_matches(&target, &ap, &event, \
                                               TEST_GENERATION, 0u)); \
    } while (0)
    CHECK_EVENT_REJECT(event.generation = TEST_GENERATION + 1u);
    CHECK_EVENT_REJECT(event.revision = 0u);
    CHECK_EVENT_REJECT(event.kind = P4_HOSTED_LINK_DOWN);
    CHECK_EVENT_REJECT(event.status = 1);
    CHECK_EVENT_REJECT(event.authmode = 3);
    CHECK_EVENT_REJECT(event.authmode = 7);
    CHECK_EVENT_REJECT(event.ssid[0] ^= 0x20u);
    CHECK_EVENT_REJECT(event.ssid_length--);
    CHECK_EVENT_REJECT(event.bssid[5] ^= 1u);
    CHECK_EVENT_REJECT(event.channel = 7u);
    CHECK_EVENT_REJECT(ap.ap.ssid[0] ^= 0x20u);
    CHECK_EVENT_REJECT(ap.ap.bssid[5] ^= 1u);
    CHECK_EVENT_REJECT(ap.ap.primary = 7u);
    event = baseline;
    init_sae_ap_info(&ap, 7);
    ap.status = 1;
    CHECK(!wifi_association_event_matches(&target, &ap, &event,
                                           TEST_GENERATION, 0u));
    init_sae_ap_info(&ap, 7);
    ap.ap.ssid_size--;
    CHECK(!wifi_association_event_matches(&target, &ap, &event,
                                           TEST_GENERATION, 0u));
    init_sae_ap_info(&ap, 7);
    CHECK(!wifi_association_event_matches(&target, &ap, &event,
                                           TEST_GENERATION, baseline.revision));
    target.authmode = 3u;
    CHECK(!wifi_association_event_matches(&target, &ap, &event,
                                           TEST_GENERATION, 0u));
    target.authmode = 6u;
    CHECK(!wifi_association_event_matches(NULL, &ap, &event,
                                           TEST_GENERATION, 0u));
    CHECK(!wifi_association_event_matches(&target, NULL, &event,
                                           TEST_GENERATION, 0u));
    CHECK(!wifi_association_event_matches(&target, &ap, NULL,
                                           TEST_GENERATION, 0u));
    CHECK(!wifi_association_event_matches(&target, &ap, &event, 0u, 0u));
    target.fullmac_profile = 0u;
    CHECK(!wifi_association_event_matches(&target, &ap, &event,
                                           TEST_GENERATION, 0u));
#undef CHECK_EVENT_REJECT
}

static void check_sae_association_failed(void)
{
    check_acceptance_cleared();
    CHECK(f.base.associated == FALSE);
    CHECK(f.base.link_state == P4_WIFI_LINK_UNKNOWN);
    check_confirmed_ap_cleared();
    CHECK(bytes_zero(&f.base.observed_ap, sizeof(f.base.observed_ap)));
    check_secret_wiped();
    check_restored(FALSE);
}

static void test_fullmac_event_required_and_config_baseline(void)
{
    struct p4_wifi_association_target target;
    unsigned scenario;
    for (scenario = 0u; scenario < 2u; ++scenario) {
        fixture_init();
        target_init_typed_sae(&target, 2u, 16u, 1u);
        /* An abandoned new attempt must not retain an older acceptance. */
        f.context.accepted_profile = P4_WIFI_FULLMAC_WPA2_CCMP;
        f.context.accepted_generation = TEST_GENERATION;
        f.context.accepted_revision = 9u;
        memset(&f.context.accepted_ap, 0x6a, sizeof(f.context.accepted_ap));
        if (scenario)
            f.event_during_config = 1u;
        CHECK(invoke(&target) == FALSE);
        CHECK(f.request_count[282u] == 1u);
        CHECK(f.final_ap_calls > 1u);
        CHECK(f.event_injections == scenario);
        CHECK(fake_now - 100000u >= 8000000u);
        if (scenario) {
            CHECK(f.context.link_observation.revision ==
                  1u + P4_WIFI_DISCONNECT_EVENT_FENCE);
            CHECK(f.context.link_observation.kind == P4_HOSTED_LINK_UP_HINT);
        } else {
            CHECK(f.context.link_observation.revision ==
                  P4_WIFI_DISCONNECT_EVENT_FENCE);
        }
        check_sae_association_failed();
    }
}

static void test_fullmac_event_identity_failure_matrix(void)
{
    struct p4_wifi_association_target target;
    int fault;
    for (fault = EVENT_WRONG_SSID; fault <= EVENT_AUTHMODE_7; ++fault) {
        fixture_init();
        target_init_typed_sae(&target, 2u, 16u, 1u);
        f.event_fault = (int)fault;
        f.event_on_connect = 1u;
        CHECK(invoke(&target) == FALSE);
        CHECK(f.request_count[282u] == 1u);
        CHECK(f.final_ap_calls == 1u);
        CHECK(f.event_injections == 1u);
        CHECK(fake_now - 100000u < 8000000u);
        check_sae_association_failed();
    }

    fixture_init();
    target_init_typed_sae(&target, 2u, 16u, 1u);
    f.event_on_connect = 1u;
    f.disconnect_event_after_connect = 1u;
    CHECK(invoke(&target) == FALSE);
    CHECK(f.final_ap_calls == 1u && f.event_injections == 2u);
    CHECK(f.context.link_observation.kind == P4_HOSTED_LINK_DOWN);
    check_sae_association_failed();

    fixture_init();
    target_init_typed_sae(&target, 2u, 16u, 1u);
    f.ap_authmode_override = 3; /* SAE profile cannot accept a WPA2 BSS. */
    f.event_on_connect = 1u;
    CHECK(invoke(&target) == FALSE);
    CHECK(f.final_ap_calls == 1u && f.event_injections == 1u);
    check_sae_association_failed();
}

static void test_fullmac_delayed_duplicate_and_snapshot_races(void)
{
    struct p4_wifi_association_target target;

    fixture_init();
    target_init_typed_sae(&target, 2u, 16u, 1u);
    f.event_on_first_final_ap = 1u;
    CHECK(invoke(&target) == TRUE);
    CHECK(f.final_ap_calls == 2u); /* Retry after the event advances revision. */
    CHECK(f.event_injections == 1u);
    CHECK(f.context.link_observation.revision ==
          1u + P4_WIFI_DISCONNECT_EVENT_FENCE);
    CHECK(f.base.associated == FALSE);
    check_confirmed_ap_matches(6u);
    check_secret_wiped();
    check_restored(FALSE);

    fixture_init();
    target_init_typed_sae(&target, 2u, 16u, 1u);
    f.event_on_connect = 1u;
    f.duplicate_event_on_connect = 1u;
    CHECK(invoke(&target) == TRUE);
    CHECK(f.final_ap_calls == 1u);
    CHECK(f.event_injections == 2u);
    CHECK(f.context.link_observation.revision ==
          2u + P4_WIFI_DISCONNECT_EVENT_FENCE);
    CHECK(f.base.associated == FALSE);
    check_confirmed_ap_matches(6u);
    check_secret_wiped();
    check_restored(FALSE);

    fixture_init();
    target_init_typed_sae(&target, 2u, 16u, 1u);
    f.event_on_connect = 1u;
    f.event_on_commit_acquire = 1u;
    CHECK(invoke(&target) == FALSE);
    CHECK(f.final_ap_calls == 1u);
    CHECK(f.commit_event_injections == 1u);
    CHECK(f.context.link_observation.kind == P4_HOSTED_LINK_DOWN);
    check_sae_association_failed();
}

static void test_fullmac_typed_cancellation_and_late_ack(void)
{
    struct p4_wifi_association_target target;

    fixture_init();
    target_init_typed_sae(&target, 2u, 16u, 1u);
    f.cancel_after_response_id = 284u;
    f.cancel_after_response_occurrence = 1u;
    CHECK(invoke(&target) == FALSE);
    CHECK(f.request_count[284u] == 1u);
    CHECK(f.request_count[282u] == 0u);
    CHECK(f.final_ap_calls == 0u);
    check_sae_association_failed();

    fixture_init();
    target_init_typed_sae(&target, 2u, 16u, 1u);
    f.event_on_connect = 1u;
    f.cancel_after_response_id = 282u;
    f.cancel_after_response_occurrence = 1u;
    CHECK(invoke(&target) == FALSE); /* ACK is not a qualified link event. */
    CHECK(f.request_count[282u] == 1u);
    CHECK(f.final_ap_calls == 0u);
    CHECK(f.event_injections == 1u);
    check_sae_association_failed();
}

static void test_fullmac_validation_and_pre_io_refusal(void)
{
    struct p4_wifi_association_target target;
    unsigned invalid_case;
    unsigned pwe;

    target_init_typed_open(&target);
    CHECK(wifi_association_target_valid(&target));

    target_init_typed_wpa2(&target, 8u);
    CHECK(wifi_association_target_valid(&target));
    target_init_typed_wpa2(&target, 63u);
    CHECK(wifi_association_target_valid(&target));
    target_init_typed_wpa2(&target, 64u);
    CHECK(wifi_association_target_valid(&target));

    for (pwe = 1u; pwe <= 3u; ++pwe) {
        target_init_typed_sae(&target, pwe, 1u, pwe & 1u);
        CHECK(wifi_association_target_valid(&target));
        target_init_typed_sae(&target, pwe, 63u, (pwe + 1u) & 1u);
        CHECK(wifi_association_target_valid(&target));
    }

    target_init(&target, 0u);
    CHECK(wifi_association_target_valid(&target));
    target_init(&target, 1u);
    CHECK(wifi_association_target_valid(&target));

    for (invalid_case = 0u; invalid_case < 4u; ++invalid_case) {
        fixture_init();
        target_init(&target, invalid_case == 1u);
        if (invalid_case == 0u) target.pmf_required = 1u;
        if (invalid_case == 1u) target.sae_pwe_h2e = 1u;
        if (invalid_case == 2u) target.diagnostic_risk_accepted = 1u;
        if (invalid_case == 3u) {
            target.pmf_required = 1u;
            target.sae_pwe_h2e = 1u;
        }
        CHECK(!wifi_association_target_valid(&target));
        CHECK(invoke(&target) == FALSE);
        CHECK(f.exchange_calls == 0u);
        check_restored(TRUE);
        check_confirmed_ap_preserved();
    }

    for (invalid_case = 0u; invalid_case < 12u; ++invalid_case) {
        fixture_init();
        target_init_typed_sae(&target, 2u, 16u, 1u);
        switch (invalid_case) {
        case 0u: target.fullmac_profile = 3u; break;
        case 1u: target.fullmac_profile = P4_WIFI_FULLMAC_OPEN; break;
        case 2u: target.authmode = 3u; break;
        case 3u: target.pmf_required = 0u; break;
        case 4u: target.pmf_required = 2u; break;
        case 5u: target.sae_pwe_h2e = 0u; break;
        case 6u: target.sae_pwe_h2e = 4u; break;
        case 7u: target.password_length = 0u; break;
        case 8u: target.password_length = 64u; break;
        case 9u: target.password[3] = 0u; break;
        case 10u: target.diagnostic_risk_accepted = 2u; break;
        default: target.fullmac_profile = P4_WIFI_FULLMAC_WPA2_CCMP; break;
        }
        CHECK(!wifi_association_target_valid(&target));
        CHECK(invoke(&target) == FALSE);
        CHECK(f.exchange_calls == 0u);
        check_restored(TRUE);
        check_confirmed_ap_preserved();
    }

    fixture_init();
    target_init_typed_sae(&target, 2u, 16u, 0u);
    CHECK(wifi_association_target_valid(&target));
    CHECK(invoke(&target) == FALSE); /* Typed risk exception is per attempt. */
    CHECK(f.exchange_calls == 0u);
    check_restored(TRUE);
    check_confirmed_ap_preserved();

    fixture_init();
    target_init_typed_wpa2(&target, 64u);
    target.password[63] = 'g';
    CHECK(!wifi_association_target_valid(&target));
    CHECK(invoke(&target) == FALSE);
    CHECK(f.exchange_calls == 0u);
    check_restored(TRUE);
    check_confirmed_ap_preserved();
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

    fixture_init();
    target_init(&target, 0u);
    original = target;
    f.base.maintenance_state = P4_WIFI_MAINT_ACTIVE;
    CHECK(invoke(&target) == FALSE);
    CHECK(f.exchange_calls == 0u);
    CHECK(memcmp(&target, &original, sizeof(target)) == 0);
    CHECK(f.base.maintenance_state == P4_WIFI_MAINT_ACTIVE);
    CHECK(f.base.link_state == P4_WIFI_LINK_UP);
    check_restored(TRUE);
    check_confirmed_ap_preserved();
    {
        const uint8_t *bytes = (const uint8_t *)&f.base.observed_ap;
        size_t index;
        for (index = 0u; index < sizeof(f.base.observed_ap); ++index)
            CHECK(bytes[index] == 0xa5u);
    }

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
    for (scenario = 0u; scenario < 4u; ++scenario) {
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
            /* Preserve this negative RPC-status case under the fence matrix;
             * a synthetic leave event must not mask the failed response. */
            f.preflight_event = PREFLIGHT_NONE;
        }
        CHECK(invoke(&target) == FALSE);
        CHECK(f.request_count[282u] == (scenario == 2u ? 1u : 0u));
        CHECK(f.request_count[284u] == (scenario == 1u || scenario == 2u ? 1u : 0u));
        if (scenario == 0u) {
            CHECK(f.request_count[283u] == 2u);
            CHECK(f.request_count[294u] == 0u);
            CHECK(f.request_count[284u] == 0u && f.request_count[282u] == 0u);
            CHECK(f.owner.poisoned == 0u && f.fifo.poisoned == 0u);
        }
        if (scenario >= 3u) CHECK(f.request_count[294u] == 1u);
        check_secret_wiped();
        check_confirmed_ap_cleared();
        check_restored(FALSE);
    }
}

static void test_association_initial_not_connected_and_delayed_absence(void)
{
    static const uint32_t not_connected_order[] =
        {283u, 294u, 284u, 282u, 294u};
    static const uint32_t delayed_absence_order[] =
        {283u, 294u, 294u, 284u, 282u, 294u};
    struct p4_wifi_association_target target;

    fixture_init();
    target_init(&target, 1u);
    f.fail_id = 283u;
    f.fail_occurrence = 1u;
    f.fail_status = 0x300f; /* ESP_ERR_WIFI_NOT_CONNECT */
    CHECK(invoke(&target) == TRUE);
    check_request_order(not_connected_order,
                        sizeof(not_connected_order) /
                            sizeof(not_connected_order[0]));
    CHECK(f.request_status_sequence[0] == 0x300f);
    CHECK(f.request_status_sequence[1] == 0x300f); /* Correlated absence. */
    CHECK(f.request_status_sequence[2] == 0u);
    CHECK(f.request_status_sequence[3] == 0u);
    CHECK(f.request_status_sequence[4] == 0u);
    CHECK(f.request_count[284u] == 1u && f.request_count[282u] == 1u);
    CHECK(f.owner.poisoned == 0u && f.fifo.poisoned == 0u);
    check_secret_wiped();
    check_confirmed_ap_matches(3u);
    check_restored(FALSE);

    fixture_init();
    target_init(&target, 1u);
    f.fail_id = 294u;
    f.fail_occurrence = 1u;
    f.fail_status = 0; /* Still attached; later query confirms absence. */
    CHECK(invoke(&target) == TRUE);
    check_request_order(delayed_absence_order,
                        sizeof(delayed_absence_order) /
                            sizeof(delayed_absence_order[0]));
    CHECK(f.request_status_sequence[1] == 0);
    CHECK(f.request_status_sequence[2] == 0x300f);
    CHECK(f.request_status_sequence[3] == 0u);
    CHECK(f.request_status_sequence[4] == 0u);
    CHECK(f.request_status_sequence[5] == 0u);
    CHECK(f.request_count[284u] == 1u && f.request_count[282u] == 1u);
    CHECK(f.owner.poisoned == 0u && f.fifo.poisoned == 0u);
    check_secret_wiped();
    check_confirmed_ap_matches(3u);
    check_restored(FALSE);
}

static void test_association_initial_attached_wait_is_bounded(void)
{
    struct p4_wifi_association_target target;
    fixture_init();
    target_init(&target, 1u);
    f.fail_id = 294u;
    f.fail_status = 0; /* The old association never disappears. */
    CHECK(invoke(&target) == FALSE);
    CHECK(f.request_count[283u] == 1u);
    CHECK(f.request_count[294u] > 1u && f.request_count[294u] <= 500u);
    CHECK(fake_now - 100000u >= 8000000u);
    CHECK(f.request_count[284u] == 0u && f.request_count[282u] == 0u);
    CHECK(f.submitted_count[284u] == 0u && f.submitted_count[282u] == 0u);
    CHECK(f.base.link_state == P4_WIFI_LINK_UNKNOWN);
    CHECK(f.base.associated == FALSE);
    check_secret_wiped();
    check_acceptance_cleared();
    check_confirmed_ap_cleared();
    check_restored(FALSE);
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

static void check_cancelled_association_cleanup(unsigned expected_ap_queries)
{
    ULONG expected_elapsed = (2u + expected_ap_queries) * 1000u;
    ULONG expected_budget_delta = (1u + expected_ap_queries) * 1000u;
    CHECK(f.request_count[283u] == 2u);
    CHECK(f.submitted_count[283u] == 2u);
    CHECK(f.request_count[294u] == expected_ap_queries);
    CHECK(f.request_count[284u] == 0u && f.request_count[282u] == 0u);
    CHECK(f.submitted_count[284u] == 0u && f.submitted_count[282u] == 0u);
    CHECK(f.owner.poisoned == 0u && f.fifo.poisoned == 0u);
    CHECK(f.owner.ids.retired == 0u && f.owner.ids.active_uid == 0u);
    CHECK(f.first_disconnect_timeout == 8000000u);
    CHECK(f.cleanup_disconnect_timeout < f.first_disconnect_timeout);
    CHECK(f.first_disconnect_timeout - f.cleanup_disconnect_timeout ==
          expected_budget_delta);
    CHECK(fake_now - 100000u == expected_elapsed);
    CHECK(f.acquire_calls == f.release_calls);
    CHECK(f.cancel_called_owned == 0u);
    CHECK(f.base.link_state == P4_WIFI_LINK_UNKNOWN);
    CHECK(bytes_zero(&f.base.confirmed_ap, sizeof(f.base.confirmed_ap)));
    CHECK(bytes_zero(&f.base.observed_ap, sizeof(f.base.observed_ap)));
    check_secret_wiped();
    check_restored(FALSE);
}

#if P4_WIFI_DISCONNECT_EVENT_FENCE
static void check_preflight_fence_bounded_failure(ULONG started)
{
    ULONG elapsed = fake_now - started;
    CHECK(f.request_count[283u] == 1u);
    CHECK(f.request_count[294u] > 1u && f.request_count[294u] <= 500u);
    CHECK(f.request_count[284u] == 0u && f.request_count[282u] == 0u);
    CHECK(f.submitted_count[284u] == 0u && f.submitted_count[282u] == 0u);
    CHECK(f.config_without_required_preflight_down == 0u);
    CHECK(elapsed >= 8000000u && elapsed < 8100000u);
    CHECK(f.base.associated == FALSE);
    CHECK(f.base.link_state == P4_WIFI_LINK_UNKNOWN);
    check_acceptance_cleared();
    check_confirmed_ap_cleared();
    CHECK(bytes_zero(&f.base.observed_ap, sizeof(f.base.observed_ap)));
    check_secret_wiped();
    check_restored(FALSE);
}

static void test_disconnect_event_fence_delayed_down(void)
{
    static const uint32_t expected_order[] =
        {283u, 294u, 294u, 294u, 284u, 282u, 294u};
    struct p4_wifi_association_target target;

    fixture_init();
    target_init(&target, 1u);
    f.preflight_event = PREFLIGHT_DOWN_SUCCESS;
    f.preflight_event_occurrence = 2u;
    CHECK(invoke(&target) == TRUE);
    check_request_order(expected_order,
                        sizeof(expected_order) / sizeof(expected_order[0]));
    CHECK(f.initial_disconnect_status == 0);
    CHECK(f.preflight_event_injections == 1u);
    CHECK(f.context.link_observation.kind == P4_HOSTED_LINK_DOWN);
    CHECK(f.context.link_observation.status == 0);
    CHECK(f.context.link_observation.reason == UINT32_C(0x76543210));
    CHECK(f.config_after_preflight_down == 1u);
    CHECK(f.config_without_required_preflight_down == 0u);
    CHECK(f.request_count[284u] == 1u && f.request_count[282u] == 1u);
    check_secret_wiped();
    check_confirmed_ap_matches(3u);
    check_restored(FALSE);
}

static void test_disconnect_event_fence_missing_stale_and_invalid_events(void)
{
    static const unsigned invalid_events[] = {
        PREFLIGHT_DOWN_FAILED_STATUS,
        PREFLIGHT_DOWN_WRONG_GENERATION,
        PREFLIGHT_STATION_START,
        PREFLIGHT_STATION_STOP,
        PREFLIGHT_UP_HINT,
        PREFLIGHT_DOWN_THEN_START,
        PREFLIGHT_DOWN_THEN_STOP,
        PREFLIGHT_DOWN_THEN_UP
    };
    struct p4_wifi_association_target target;
    unsigned index;
    ULONG started;

    fixture_init();
    target_init(&target, 1u);
    f.preflight_event = PREFLIGHT_NONE;
    started = fake_now;
    CHECK(invoke(&target) == FALSE);
    CHECK(f.preflight_event_injections == 0u);
    check_preflight_fence_bounded_failure(started);

    fixture_init();
    target_init(&target, 1u);
    f.preflight_event = PREFLIGHT_NONE;
    CHECK(inject_disconnected_event(&f, &f.context));
    CHECK(f.context.link_observation.revision == 1u);
    started = fake_now;
    CHECK(invoke(&target) == FALSE);
    CHECK(f.disconnect_baseline_revision == 1u);
    CHECK(f.context.link_observation.revision == 1u);
    CHECK(f.context.link_observation.kind == P4_HOSTED_LINK_DOWN);
    CHECK(f.event_injections == 1u);
    check_preflight_fence_bounded_failure(started);

    for (index = 0u;
         index < sizeof(invalid_events) / sizeof(invalid_events[0]);
         ++index) {
        fixture_init();
        target_init(&target, 1u);
        f.preflight_event = (int)invalid_events[index];
        f.preflight_event_occurrence = 1u;
        started = fake_now;
        CHECK(invoke(&target) == FALSE);
        if (invalid_events[index] == PREFLIGHT_DOWN_THEN_START ||
            invalid_events[index] == PREFLIGHT_DOWN_THEN_STOP ||
            invalid_events[index] == PREFLIGHT_DOWN_THEN_UP) {
            CHECK(f.preflight_event_injections == 2u);
            CHECK(f.context.link_observation.revision == 2u);
            if (invalid_events[index] == PREFLIGHT_DOWN_THEN_START)
                CHECK(f.context.link_observation.kind ==
                      P4_HOSTED_LINK_STATION_START);
            else if (invalid_events[index] == PREFLIGHT_DOWN_THEN_STOP)
                CHECK(f.context.link_observation.kind ==
                      P4_HOSTED_LINK_STATION_STOP);
            else
                CHECK(f.context.link_observation.kind ==
                      P4_HOSTED_LINK_UP_HINT);
        } else {
            CHECK(f.preflight_event_injections == 1u);
        }
        CHECK(f.config_after_preflight_down == 0u);
        check_preflight_fence_bounded_failure(started);
    }
}

static void test_disconnect_event_fence_not_connected_reply_and_malformed_reply(void)
{
    struct p4_wifi_association_target target;

    fixture_init();
    target_init(&target, 1u);
    f.preflight_event = PREFLIGHT_NONE;
    f.fail_id = 283u;
    f.fail_occurrence = 1u;
    f.fail_status = 0x300f;
    CHECK(invoke(&target) == TRUE);
    CHECK(f.initial_disconnect_status == 0x300f);
    CHECK(f.preflight_event_injections == 0u);
    CHECK(f.request_count[284u] == 1u && f.request_count[282u] == 1u);
    CHECK(f.config_without_required_preflight_down == 0u);
    check_confirmed_ap_matches(3u);
    check_secret_wiped();
    check_restored(FALSE);

    fixture_init();
    target_init(&target, 1u);
    f.preflight_event = PREFLIGHT_NONE;
    f.fail_id = 283u;
    f.fail_occurrence = 1u;
    f.fail_status = 0x300f;
    f.malformed_response_request_id = 283u;
    CHECK(invoke(&target) == FALSE);
    CHECK(f.request_count[283u] == 1u && f.request_count[294u] == 0u);
    CHECK(f.request_count[284u] == 0u && f.request_count[282u] == 0u);
    CHECK(f.owner.poisoned == 1u && f.fifo.poisoned == 1u);
    check_confirmed_ap_cleared();
    check_secret_wiped();
    check_restored(FALSE);
}

static void test_disconnect_event_fence_cancel_and_shared_budget_wrap(void)
{
    struct p4_wifi_association_target target;
    ULONG started;

    fixture_init();
    target_init(&target, 1u);
    f.preflight_event = PREFLIGHT_NONE;
    f.cancel_after_response_id = 294u;
    f.cancel_after_response_occurrence = 1u;
    CHECK(invoke(&target) == FALSE);
    CHECK(f.cancel_requested == TRUE && f.cancel_calls != 0u);
    CHECK(f.request_count[284u] == 0u && f.request_count[282u] == 0u);
    check_cancelled_association_cleanup(1u);

    fixture_init();
    target_init(&target, 1u);
    f.preflight_event = PREFLIGHT_DOWN_SUCCESS;
    f.preflight_event_occurrence = 381u;
    fake_now = UINT32_MAX - 100000u;
    started = fake_now;
    CHECK(invoke(&target) == FALSE);
    CHECK(f.preflight_event_injections == 1u);
    CHECK(f.context.link_observation.kind == P4_HOSTED_LINK_DOWN);
    CHECK(f.request_count[294u] == 381u);
    CHECK(f.request_count[284u] == 0u && f.request_count[282u] == 0u);
    CHECK(fake_now - started >= 8000000u &&
          fake_now - started < 8100000u);
    CHECK(f.config_without_required_preflight_down == 0u);
    CHECK(f.base.link_state == P4_WIFI_LINK_UNKNOWN);
    CHECK(f.base.associated == FALSE);
    check_secret_wiped();
    check_confirmed_ap_cleared();
    check_restored(FALSE);
}
#endif

static void test_cancel_between_initial_rpc_steps(void)
{
    struct p4_wifi_association_target target;

    fixture_init();
    target_init(&target, 1u);
    f.cancel_after_response_id = 283u;
    f.cancel_after_response_occurrence = 1u;
    CHECK(invoke(&target) == FALSE);
    CHECK(f.cancel_requested == TRUE && f.cancel_calls != 0u);
    check_cancelled_association_cleanup(0u);

    fixture_init();
    target_init(&target, 1u);
    f.cancel_after_response_id = 294u;
    f.cancel_after_response_occurrence = 1u;
    CHECK(invoke(&target) == FALSE);
    CHECK(f.cancel_requested == TRUE && f.cancel_calls != 0u);
    CHECK(f.last_ap_info_status == 0x300f);
    check_cancelled_association_cleanup(1u);

    fixture_init();
    target_init(&target, 1u);
    f.fail_id = 294u;
    f.fail_occurrence = 1u;
    f.fail_status = 0; /* Cancellation while the old association is present. */
    f.cancel_after_response_id = 294u;
    f.cancel_after_response_occurrence = 1u;
    CHECK(invoke(&target) == FALSE);
    CHECK(f.cancel_requested == TRUE && f.cancel_calls != 0u);
    CHECK(f.last_ap_info_status == 0);
    CHECK(f.request_count[284u] == 0u && f.request_count[282u] == 0u);
    {
        static const uint32_t expected_order[] = {283u, 294u, 283u};
        check_request_order(expected_order,
                            sizeof(expected_order) / sizeof(expected_order[0]));
    }
    check_cancelled_association_cleanup(1u);
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
    CHECK(f.request_count[284u] == 1u && f.submitted_count[284u] == 1u);
    CHECK(f.submitted_count[282u] == 0u);
    CHECK(f.request_count[283u] >= 2u); /* Initial and healthy cleanup leave. */
    CHECK(f.owner.poisoned == 0u && f.fifo.poisoned == 0u);
    CHECK(f.owner.ids.retired == 0u && f.owner.ids.active_uid == 0u);
    CHECK(f.cancel_called_owned == 0u);
    check_secret_wiped();
    check_confirmed_ap_cleared();
    check_restored(FALSE);

    fixture_init();
    target_init(&target, 1u);
    f.timeout_request_id = 284u;
    f.timeout_occurrence = 1u;
    CHECK(invoke(&target) == FALSE);
    CHECK(f.request_count[284u] == 1u && f.submitted_count[284u] == 1u);
    CHECK(f.request_count[282u] == 0u);
    CHECK(f.request_count[283u] == 1u); /* No cleanup over an ambiguous session. */
    CHECK(f.owner.poisoned == 1u && f.fifo.poisoned == 1u);
    CHECK(f.owner.ids.retired == 1u && f.owner.ids.active_uid == 0u);
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
            CHECK(f.request_count[294u] ==
                  1u + P4_WIFI_DISCONNECT_EVENT_FENCE);
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
    CHECK(f.request_count[294u] == 0u);
    CHECK(f.request_count[284u] == 0u && f.request_count[282u] == 0u);
    check_secret_wiped();
    check_confirmed_ap_cleared();
    check_restored(FALSE);

    fixture_init();
    target_init(&target, 1u);
    f.timeout_request_id = 283u;
    f.timeout_occurrence = 1u;
    CHECK(invoke(&target) == FALSE);
    CHECK(f.request_count[283u] == 1u && f.submitted_count[283u] == 1u);
    CHECK(f.request_count[294u] == 0u);
    CHECK(f.request_count[284u] == 0u && f.request_count[282u] == 0u);
    CHECK(f.owner.poisoned == 1u && f.fifo.poisoned == 1u);
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

static void test_disconnect_requires_healthy_service_gate(void)
{
    fixture_init();
    f.base.association_enabled = FALSE;
    CHECK(invoke_disconnect() == FALSE);
    check_disconnect_refused_unchanged();

    fixture_init();
    f.runtime.servicing = FALSE;
    CHECK(invoke_disconnect() == FALSE);
    check_disconnect_refused_unchanged();

    fixture_init();
    f.runtime.scanning = TRUE;
    CHECK(invoke_disconnect() == FALSE);
    CHECK(f.exchange_calls == 0u);
    CHECK(f.base.associated == TRUE && f.base.link_state == P4_WIFI_LINK_UP);

    fixture_init();
    f.owner.poisoned = 1u;
    CHECK(invoke_disconnect() == FALSE);
    check_disconnect_refused_unchanged();

    fixture_init();
    f.context.association_active = TRUE;
    CHECK(invoke_disconnect() == FALSE);
    check_disconnect_refused_unchanged();

    fixture_init();
    f.base.client_lock.ss_Owner = NULL;
    f.base.client_lock.depth = 0u;
    CHECK(invoke_disconnect() == FALSE);
    check_disconnect_refused_unchanged();
}

static void test_disconnect_uses_correlated_not_connected(void)
{
    fixture_init();
    f.context.accepted_profile = P4_WIFI_FULLMAC_SAE_CCMP_PMF;
    f.context.accepted_generation = TEST_GENERATION;
    f.context.accepted_revision = 9u;
    memset(&f.context.accepted_ap, 0x6a, sizeof(f.context.accepted_ap));
    f.fail_id = 283u;
    f.fail_status = 0x300f;
    CHECK(invoke_disconnect() == TRUE);
    CHECK(f.request_count[283u] == 1u);
    CHECK(f.request_count[294u] == 1u);
    CHECK(f.request_count[284u] == 0u && f.request_count[282u] == 0u);
    CHECK(f.control.status == 0x300f);
    CHECK(f.owner.poisoned == 0u && f.fifo.poisoned == 0u);
    CHECK(f.acquire_calls == f.release_calls);
    CHECK(f.max_timeout <= 8000000u);
    check_disconnected_down();
}

static void test_disconnect_waits_through_attached_snapshot(void)
{
    fixture_init();
    f.expect_disconnect_final_fence = 1u;
    f.fail_id = 294u;
    f.fail_occurrence = 1u;
    f.fail_status = 0; /* First query still sees an attached station. */
    CHECK(invoke_disconnect() == TRUE);
    CHECK(f.request_count[283u] == 1u);
    CHECK(f.request_count[294u] == 2u);
    CHECK(f.disconnect_intermediate_unknown_checks == 1u);
    CHECK(f.disconnect_final_fence_checks == 1u);
    CHECK(f.last_ap_info_status == 0x300f);
    CHECK(f.context.generation == TEST_GENERATION);
    CHECK(f.context.link_observation.generation == TEST_GENERATION);
    CHECK(f.context.link_observation.revision == 0u);
    CHECK(f.base.link_state == P4_WIFI_LINK_DOWN);
    CHECK(f.base.associated == FALSE);
    CHECK(bytes_zero(&f.base.confirmed_ap, sizeof(f.base.confirmed_ap)));
    CHECK(bytes_zero(&f.base.observed_ap, sizeof(f.base.observed_ap)));
    CHECK(f.owner.poisoned == 0u && f.fifo.poisoned == 0u);
    CHECK(f.owner.ids.retired == 0u && f.owner.ids.active_uid == 0u);
    CHECK(f.acquire_calls == f.release_calls);
    CHECK(f.host.lock.ss_Owner == NULL && f.host.lock.depth == 0u);
    CHECK(f.max_timeout <= 8000000u);
    check_disconnected_down();
}

static void test_disconnect_failures_never_infer_down(void)
{
    fixture_init();
    f.fail_id = 283u;
    f.fail_status = 5;
    CHECK(invoke_disconnect() == FALSE);
    CHECK(f.request_count[283u] == 1u && f.request_count[294u] == 0u);
    CHECK(f.owner.poisoned == 1u && f.fifo.poisoned == 1u);
    check_disconnect_failure_unknown();

    fixture_init();
    f.fail_id = 294u;
    f.fail_status = 5;
    CHECK(invoke_disconnect() == FALSE);
    CHECK(f.request_count[283u] == 1u && f.request_count[294u] == 1u);
    CHECK(f.owner.poisoned == 1u && f.fifo.poisoned == 1u);
    check_disconnect_failure_unknown();

    fixture_init();
    f.timeout_request_id = 294u;
    CHECK(invoke_disconnect() == FALSE);
    CHECK(f.request_count[283u] == 1u && f.request_count[294u] == 1u);
    CHECK(f.owner.poisoned == 1u && f.fifo.poisoned == 1u);
    check_disconnect_failure_unknown();

    fixture_init();
    f.fail_id = 294u;
    f.fail_status = 0; /* Still present throughout the aggregate deadline. */
    CHECK(invoke_disconnect() == FALSE);
    CHECK(f.request_count[283u] == 1u);
    CHECK(f.request_count[294u] <= 500u);
    CHECK(fake_now - 100000u >= 8000000u);
    CHECK(f.owner.poisoned == 1u && f.fifo.poisoned == 1u);
    check_disconnect_failure_unknown();
}

static void test_disconnect_revision_race(void)
{
    fixture_init();
    f.initial_event_once = 1u;
    CHECK(invoke_disconnect() == TRUE);
    CHECK(f.request_count[294u] == 2u);
    CHECK(f.context.link_observation.revision == 1u);
    CHECK(f.base.link_state == P4_WIFI_LINK_DOWN);
    CHECK(f.base.associated == FALSE);
    CHECK(bytes_zero(&f.base.observed_ap, sizeof(f.base.observed_ap)));
    CHECK(f.owner.poisoned == 0u && f.fifo.poisoned == 0u);
    CHECK(f.acquire_calls == f.release_calls);
    CHECK(f.runtime.servicing == TRUE && f.runtime.scanning == FALSE);

    fixture_init();
    f.repeat_initial_event = 1u;
    CHECK(invoke_disconnect() == FALSE);
    CHECK(f.request_count[294u] <= 500u);
    CHECK(fake_now - 100000u >= 8000000u);
    CHECK(f.base.link_state == P4_WIFI_LINK_UNKNOWN);
    CHECK(f.base.associated == FALSE);
    CHECK(bytes_zero(&f.base.observed_ap, sizeof(f.base.observed_ap)));
    CHECK(f.owner.poisoned == 1u && f.fifo.poisoned == 1u);
    CHECK(f.acquire_calls == f.release_calls);
}

static void test_unqualified_peer_refuses_typed_without_io(void)
{
    struct p4_wifi_association_target target, original;
    unsigned profile;
    for (profile = 0; profile < 3; ++profile) {
        fixture_init();
        if (profile == 0) target_init_typed_open(&target);
        else if (profile == 1) target_init_typed_wpa2(&target, 64u);
        else target_init_typed_sae(&target, 3u, 16u, 1u);
        original = target;
        CHECK(wifi_association_target_valid(&target));
        CHECK(invoke(&target) == FALSE);
        CHECK(f.acquire_calls == 0u && f.release_calls == 0u);
        CHECK(f.request_count[283u] == 0u && f.request_count[294u] == 0u &&
              f.request_count[284u] == 0u && f.request_count[282u] == 0u);
        CHECK(memcmp(&target, &original, sizeof(target)) == 0);
    }
}

static void seed_initial_join_observation(unsigned kind, ULONG generation,
                                          ULONG revision, LONG status,
                                          BOOL owner_started)
{
    fixture_init();
    f.context.owner_started = owner_started;
    f.context.link_observation.generation = generation;
    f.context.link_observation.revision = revision;
    f.context.link_observation.kind = kind;
    f.context.link_observation.status = status;
    f.context.first_join_pending = TRUE;
    f.context.first_join_revision = 99u;
}

static void check_initial_join_not_armed(unsigned kind, ULONG generation,
                                         ULONG revision, LONG status,
                                         BOOL owner_started,
                                         BOOL fresh_reset)
{
    seed_initial_join_observation(kind, generation, revision, status,
                                  owner_started);
    wifi_owner_initial_join_start(&f.context, fresh_reset);
    CHECK(f.context.first_join_pending == FALSE);
    CHECK(f.context.first_join_revision == 0u);
}

static void test_initial_join_start_helper_predicate(void)
{
    seed_initial_join_observation(P4_HOSTED_LINK_STATION_START,
        TEST_GENERATION, 11u, 0, TRUE);
    wifi_owner_initial_join_start(&f.context, TRUE);
    CHECK(f.context.first_join_pending == TRUE);
    CHECK(f.context.first_join_revision == 11u);

    seed_initial_join_observation(P4_HOSTED_LINK_NONE,
        TEST_GENERATION, 0u, 0, TRUE);
    wifi_owner_initial_join_start(&f.context, TRUE);
    CHECK(f.context.first_join_pending == TRUE);
    CHECK(f.context.first_join_revision == 1u);

    seed_initial_join_observation(P4_HOSTED_LINK_STATION_STOP,
        TEST_GENERATION, 12u, 0, TRUE);
    wifi_owner_initial_join_start(&f.context, TRUE);
    CHECK(f.context.first_join_pending == TRUE);
    CHECK(f.context.first_join_revision == 13u);

    check_initial_join_not_armed(P4_HOSTED_LINK_STATION_START,
        TEST_GENERATION, 1u, 0, TRUE, FALSE);
    check_initial_join_not_armed(P4_HOSTED_LINK_STATION_START,
        TEST_GENERATION, 1u, 0, (BOOL)2, TRUE);
    check_initial_join_not_armed(P4_HOSTED_LINK_STATION_START,
        TEST_GENERATION, 1u, 0, FALSE, TRUE);
    check_initial_join_not_armed(P4_HOSTED_LINK_STATION_START,
        TEST_GENERATION + 1u, 1u, 0, TRUE, TRUE);
    check_initial_join_not_armed(P4_HOSTED_LINK_STATION_START,
        TEST_GENERATION, 1u, -5, TRUE, TRUE);
    check_initial_join_not_armed(P4_HOSTED_LINK_STATION_START,
        TEST_GENERATION, 0u, 0, TRUE, TRUE);
    check_initial_join_not_armed(P4_HOSTED_LINK_STATION_START,
        TEST_GENERATION, UINT32_MAX, 0, TRUE, TRUE);
    check_initial_join_not_armed(P4_HOSTED_LINK_STATION_STOP,
        TEST_GENERATION, 0u, 0, TRUE, TRUE);
    check_initial_join_not_armed(P4_HOSTED_LINK_STATION_STOP,
        TEST_GENERATION, UINT32_MAX, 0, TRUE, TRUE);
    check_initial_join_not_armed(P4_HOSTED_LINK_NONE,
        TEST_GENERATION, 1u, 0, TRUE, TRUE);
    check_initial_join_not_armed(P4_HOSTED_LINK_UP_HINT,
        TEST_GENERATION, 1u, 0, TRUE, TRUE);
    check_initial_join_not_armed(P4_HOSTED_LINK_DOWN,
        TEST_GENERATION, 1u, 0, TRUE, TRUE);
}

static void prepare_first_idle_join_candidate(void)
{
    struct p4_hosted_inbox_item item;
    f.context.owner_started = TRUE;
    f.control.startup_ok = 1u;
    CHECK(make_wifi_lifecycle_event(&item, P4_HOSTED_WIFI_EVENT_STA_START));
    CHECK(wifi_owner_event(&f.context, &item) == 1);
    wifi_owner_initial_join_start(&f.context, TRUE);
    CHECK(f.context.first_join_pending == TRUE);
}

static void test_numeric_observer_parsers(void)
{
    diagnostic_count = 0u;
    diagnostic_rpc_count = 0u;
    diagnostic_event_count = 0u;
    diagnostic_hint_count = 0u;
    diagnostic_rpc_status = 0;
    diagnostic_event_status = 0;
    diagnostic_hint_status = 0;
    diagnostic_prelude_count = 0u;
    diagnostic_prelude_first = 0u;
    diagnostic_prelude_generation = 0u;
    diagnostic_prelude_revision = 0u;
    bug("[WiFiJoin] s=0 r=0 st=-1 ok=0\n");
    bug("[WiFiJoin] s=%u r=%u st=%ld v=%u ok=%u\n",
        3u, 283u, (LONG)0, 1u, 0u);
    bug("[WiFiRPC] p=1 id=%u u=%u us=%u\n", 283u, 9u, 100u);
    bug("[WiFiRPC] p=2 id=%u u=%u us=%u sub=%u v=0 st=-1\n",
        283u, 9u, 100u, 1u);
    bug("[WiFiRPC] p=2 id=%u u=%u us=%u sub=%u v=1 st=%ld\n",
        283u, 9u, 100u, 1u, (LONG)-5);
    bug("[WiFiEvent] id=%u g=%u rev=%u k=%u st=%ld reason=%u\n",
        776u, TEST_GENERATION, 1u, P4_HOSTED_LINK_DOWN,
        (LONG)-5, UINT32_C(0x76543210));
    bug("[WiFiHint] id=%u g=%u st=%ld\n",
        P4_HOSTED_WIFI_EVENT_STA_START, TEST_GENERATION, (LONG)-5);
    bug("[WiFiPrelude] first=%u g=%u rev=%u\n",
        1u, TEST_GENERATION, 7u);
    CHECK(diagnostic_count == 2u);
    CHECK(diagnostic_rpc_count == 3u);
    CHECK(diagnostic_event_count == 1u);
    CHECK(diagnostic_hint_count == 1u);
    CHECK(diagnostic_rpc_status == (LONG)-5);
    CHECK(diagnostic_event_status == (LONG)-5);
    CHECK(diagnostic_hint_status == (LONG)-5);
    CHECK(diagnostic_prelude_count == 1u);
    CHECK(diagnostic_prelude_first == 1u);
    CHECK(diagnostic_prelude_generation == TEST_GENERATION);
    CHECK(diagnostic_prelude_revision == 7u);
}

static void test_first_idle_join_success_and_one_shot(void)
{
    static const uint32_t first_order[] = {294u, 294u, 284u, 282u, 294u};
    static const uint32_t late_start_order[] =
        {294u, 294u, 294u, 284u, 282u, 294u};
    struct p4_wifi_association_target target;
    unsigned sequence_start;
    fixture_init();
    target_init(&target, 0u);
    f.preflight_event = PREFLIGHT_NONE;
    prepare_first_idle_join_candidate();
    diagnostic_prelude_count = 0u;
    CHECK(invoke(&target) == TRUE);
    check_request_order(first_order,
                        sizeof(first_order) / sizeof(first_order[0]));
    CHECK(f.request_count[283u] == 0u && f.request_count[294u] == 3u);
    CHECK(f.request_count[284u] == 1u && f.request_count[282u] == 1u);
    CHECK(f.context.first_join_pending == FALSE);
#if P4_WIFI_ASSOC_DIAGNOSTICS
    CHECK(diagnostic_prelude_count == 1u);
    CHECK(diagnostic_prelude_first == 1u);
    CHECK(diagnostic_prelude_generation == TEST_GENERATION);
    CHECK(diagnostic_prelude_revision == 1u);
#endif
    check_open_association_buffers_cleared();
    check_confirmed_ap_matches(0u);
    check_restored(FALSE);

    memset(f.request_count, 0, sizeof(f.request_count));
    f.request_sequence_count = 0u;
    f.ap_info_calls = 0u;
    f.final_ap_calls = 0u;
    f.initial_disconnect_status = 0;
    f.preflight_event = PREFLIGHT_DOWN_SUCCESS;
    f.preflight_event_occurrence = 1u;
    sequence_start = f.request_sequence_count;
    diagnostic_prelude_count = 0u;
    CHECK(invoke(&target) == TRUE);
    CHECK(f.request_sequence[sequence_start] == 283u);
    CHECK(f.request_count[283u] == 1u);
    CHECK(f.context.first_join_pending == FALSE);
#if P4_WIFI_ASSOC_DIAGNOSTICS
    CHECK(diagnostic_prelude_count == 1u);
    CHECK(diagnostic_prelude_first == 0u);
#endif
    check_open_association_buffers_cleared();
    check_confirmed_ap_matches(0u);
    check_restored(FALSE);

    fixture_init();
    target_init(&target, 0u);
    f.context.owner_started = TRUE;
    f.control.startup_ok = 1u;
    f.preflight_event = PREFLIGHT_STATION_START;
    f.preflight_event_occurrence = 1u;
    wifi_owner_initial_join_start(&f.context, TRUE);
    CHECK(f.context.first_join_pending == TRUE &&
          f.context.first_join_revision == 1u);
    CHECK(invoke(&target) == TRUE);
    check_request_order(late_start_order,
        sizeof(late_start_order) / sizeof(late_start_order[0]));
    CHECK(f.preflight_event_injections == 1u && f.request_count[283u] == 0u);
    CHECK(f.context.link_observation.kind == P4_HOSTED_LINK_STATION_START);
    CHECK(f.context.first_join_pending == FALSE);
    check_open_association_buffers_cleared();
    check_confirmed_ap_matches(0u);
    check_restored(FALSE);

    fixture_init();
    target_init(&target, 0u);
    f.preflight_event = PREFLIGHT_NONE;
    f.context.owner_started = TRUE;
    f.control.startup_ok = 1u;
    wifi_owner_initial_join_start(&f.context, TRUE);
    CHECK(f.context.first_join_pending == TRUE &&
          f.context.first_join_revision == 1u);
    sequence_start = f.request_sequence_count;
    {
        ULONG started = fake_now;
        CHECK(invoke(&target) == FALSE);
        CHECK(f.request_count[283u] == 0u && f.request_count[284u] == 0u &&
              f.request_count[282u] == 0u);
        CHECK(f.request_count[294u] > 1u && f.request_count[294u] < 500u);
        CHECK(fake_now - started >= 8000000u &&
              fake_now - started < 8100000u);
        CHECK(f.context.first_join_pending == FALSE);
    }
    CHECK(f.request_sequence[sequence_start] == 294u);
    check_open_association_buffers_cleared();
    check_confirmed_ap_cleared();
    check_restored(FALSE);
}

static void test_first_idle_join_fallbacks_to_disconnect(void)
{
    enum {
        FALLBACK_NO_FRESH_RESET = 0,
        FALLBACK_STARTUP_NOT_OK,
        FALLBACK_OWNER_NOT_STARTED,
        FALLBACK_WRONG_LINK_KIND,
        FALLBACK_REVISION_CHANGED,
        FALLBACK_GENERATION_CHANGED,
        FALLBACK_CASE_COUNT
    };
    struct p4_wifi_association_target target;
    unsigned scenario;
    for (scenario = 0u; scenario < FALLBACK_CASE_COUNT; ++scenario) {
#if P4_WIFI_DISCONNECT_EVENT_FENCE
        ULONG started;
#endif
        BOOL result;
        fixture_init();
        target_init(&target, 1u);
        prepare_first_idle_join_candidate();
        switch (scenario) {
        case FALLBACK_NO_FRESH_RESET:
            wifi_owner_initial_join_start(&f.context, FALSE);
            break;
        case FALLBACK_STARTUP_NOT_OK:
            f.control.startup_ok = 0u;
            break;
        case FALLBACK_OWNER_NOT_STARTED:
            f.context.owner_started = FALSE;
            break;
        case FALLBACK_WRONG_LINK_KIND:
            f.context.link_observation.kind = P4_HOSTED_LINK_UP_HINT;
            break;
        case FALLBACK_REVISION_CHANGED:
            ++f.context.link_observation.revision;
            break;
        default:
            f.context.link_observation.generation = TEST_GENERATION + 1u;
            f.preflight_event = PREFLIGHT_NONE;
            break;
        }
#if P4_WIFI_DISCONNECT_EVENT_FENCE
        started = fake_now;
#endif
        result = invoke(&target);
        CHECK(f.request_count[283u] >= 1u);
        CHECK(f.request_sequence[0] == 283u);
        CHECK(f.context.first_join_pending == FALSE);
        if (scenario == FALLBACK_GENERATION_CHANGED) {
            CHECK(result == FALSE);
#if P4_WIFI_DISCONNECT_EVENT_FENCE
            CHECK(f.request_count[284u] == 0u && f.request_count[282u] == 0u);
            check_preflight_fence_bounded_failure(started);
#else
            CHECK(f.request_count[284u] == 1u && f.request_count[282u] == 1u);
            check_secret_wiped();
            check_confirmed_ap_cleared();
            check_restored(FALSE);
#endif
        } else {
            CHECK(result == TRUE);
            CHECK(f.request_count[283u] == 1u);
            CHECK(f.request_count[284u] == 1u && f.request_count[282u] == 1u);
            check_secret_wiped();
            check_confirmed_ap_matches(3u);
            check_restored(FALSE);
        }
    }
}

static void test_first_idle_join_prequery_failures(void)
{
    enum {
        PREQUERY_TIMEOUT = 0,
        PREQUERY_MALFORMED,
        PREQUERY_CANCEL,
        PREQUERY_EPOCH_CHANGE,
        PREQUERY_REMOTE_ERROR,
        PREQUERY_CASE_COUNT
    };
    struct p4_wifi_association_target target;
    unsigned scenario;
    for (scenario = 0u; scenario < PREQUERY_CASE_COUNT; ++scenario) {
        fixture_init();
        target_init(&target, 0u);
        f.preflight_event = PREFLIGHT_NONE;
        prepare_first_idle_join_candidate();
        if (scenario == PREQUERY_TIMEOUT) {
            f.timeout_request_id = 294u;
            f.timeout_occurrence = 1u;
        } else if (scenario == PREQUERY_MALFORMED) {
            f.malformed_response_request_id = 294u;
        } else if (scenario == PREQUERY_CANCEL) {
            f.cancel_after_response_id = 294u;
            f.cancel_after_response_occurrence = 1u;
        } else if (scenario == PREQUERY_REMOTE_ERROR) {
            f.fail_id = 294u;
            f.fail_occurrence = 1u;
            f.fail_status = 5;
        } else {
            f.generation_change_first_prelude = 1u;
        }
        CHECK(invoke(&target) == FALSE);
        CHECK(f.request_count[294u] == 1u);
        CHECK(f.request_count[283u] == 0u);
        CHECK(f.request_count[284u] == 0u && f.request_count[282u] == 0u);
        CHECK(f.context.first_join_pending == FALSE);
        CHECK(f.cancel_called_owned == 0u);
        if (scenario == PREQUERY_CANCEL)
            CHECK(f.cancel_requested == TRUE && f.cancel_calls != 0u);
        if (scenario == PREQUERY_EPOCH_CHANGE)
            CHECK(f.context.generation == TEST_GENERATION + 1u);
        check_open_association_buffers_cleared();
        check_confirmed_ap_cleared();
        check_restored(FALSE);
    }
}

static void test_first_idle_join_contradiction_and_config_cleanup(void)
{
    static const uint32_t contradictory_order[] =
        {294u, 283u, 294u, 294u, 284u, 282u, 294u};
    static const uint32_t config_failure_order[] = {294u, 294u, 284u, 283u};
    struct p4_wifi_association_target target;

    fixture_init();
    target_init(&target, 0u);
    f.preflight_event = PREFLIGHT_DOWN_SUCCESS;
    f.preflight_event_occurrence = 2u;
    f.fail_id = 294u;
    f.fail_occurrence = 1u;
    f.fail_status = 0; /* Valid but contradictory attached-AP snapshot. */
    prepare_first_idle_join_candidate();
    CHECK(invoke(&target) == TRUE);
    check_request_order(contradictory_order,
        sizeof(contradictory_order) / sizeof(contradictory_order[0]));
    CHECK(f.context.first_join_pending == FALSE);
    check_open_association_buffers_cleared();
    check_confirmed_ap_matches(0u);
    check_restored(FALSE);

    fixture_init();
    target_init(&target, 0u);
    f.preflight_event = PREFLIGHT_NONE;
    prepare_first_idle_join_candidate();
    f.fail_id = 284u;
    f.fail_occurrence = 1u;
    f.fail_status = 5;
    CHECK(invoke(&target) == FALSE);
    check_request_order(config_failure_order,
        sizeof(config_failure_order) / sizeof(config_failure_order[0]));
    CHECK(f.request_count[282u] == 0u);
    CHECK(f.context.first_join_pending == FALSE);
    check_open_association_buffers_cleared();
    check_confirmed_ap_cleared();
    check_restored(FALSE);
}

static void test_first_idle_join_owned_secret_cleanup(void)
{
    struct p4_wifi_association_target target;
    unsigned failure;
    for (failure = 0; failure < 2; ++failure) {
        fixture_init();
        target_init(&target, 1u);
        f.preflight_event = PREFLIGHT_NONE;
        prepare_first_idle_join_candidate();
        if (failure) {
            f.fail_id = 294u;
            f.fail_occurrence = 1u;
            f.fail_status = 5;
        }
        CHECK(invoke(&target) == (failure ? FALSE : TRUE));
        CHECK(f.request_count[283u] == 0u);
        CHECK(f.context.first_join_pending == FALSE);
        check_secret_wiped();
        check_restored(FALSE);
    }
}

static void test_explicit_disconnect_consumes_first_join_candidate(void)
{
    fixture_init();
    f.preflight_event = PREFLIGHT_NONE;
    prepare_first_idle_join_candidate();
    CHECK(invoke_disconnect() == TRUE);
    CHECK(f.request_count[283u] == 1u && f.request_count[294u] == 1u);
    CHECK(f.context.first_join_pending == FALSE);
    check_disconnected_down();
}

static void test_numeric_join_diagnostics(void)
{
#if P4_WIFI_ASSOC_DIAGNOSTICS
    static const unsigned ids[] = {283, 284, 282, 294, 294};
    static const unsigned stages[] = {2, 4, 5, 3, 6};
    struct p4_wifi_association_target target;
    BOOL result;
    unsigned i;
    for (i = 0; i < sizeof(ids) / sizeof(ids[0]); ++i) {
        fixture_init();
        target_init(&target, 0u);
        f.fail_id = ids[i];
        f.fail_occurrence = i == 4 ?
            (P4_WIFI_DISCONNECT_EVENT_FENCE ? 3u : 2u) : 1u;
        f.fail_status = 7;
#if P4_WIFI_DISCONNECT_EVENT_FENCE
        if (ids[i] == 294u && f.fail_occurrence == 1u)
            f.preflight_event = PREFLIGHT_NONE;
#endif
        diagnostic_count = 0;
        result = invoke(&target);
        CHECK(result == FALSE);
        CHECK(diagnostic_count == 1);
        CHECK(diagnostic_stage == stages[i]);
        CHECK(diagnostic_rpc == ids[i]);
        CHECK(diagnostic_status == 7);
        CHECK(diagnostic_valid == 1);
        CHECK(diagnostic_ok == 0);
    }
    fixture_init();
    target_init(&target, 0u);
    diagnostic_count = 0;
    CHECK(invoke(&target) == TRUE);
    CHECK(diagnostic_count == 1 && diagnostic_stage == 8);
    CHECK(diagnostic_rpc == 294 && diagnostic_status == 0 && diagnostic_ok == 1);
    CHECK(diagnostic_valid == 1);
    /* CONFIG success must not become CONNECT's status after transport loss. */
    fixture_init();
    target_init(&target, 0u);
    f.timeout_request_id = 282u;
    diagnostic_count = 0;
    CHECK(invoke(&target) == FALSE);
    CHECK(diagnostic_count == 1 && diagnostic_stage == 5);
    CHECK(diagnostic_valid == 0 && diagnostic_status == -1 && diagnostic_ok == 0);
#endif
}

int main(void)
{
    test_numeric_observer_parsers();
    test_numeric_join_diagnostics();
    test_initial_join_start_helper_predicate();
    test_first_idle_join_success_and_one_shot();
    test_first_idle_join_fallbacks_to_disconnect();
    test_first_idle_join_prequery_failures();
    test_first_idle_join_contradiction_and_config_cleanup();
    test_first_idle_join_owned_secret_cleanup();
    test_explicit_disconnect_consumes_first_join_candidate();
#if P4_WIFI_DISCONNECT_EVENT_FENCE
    test_disconnect_event_fence_delayed_down();
    test_disconnect_event_fence_missing_stale_and_invalid_events();
    test_disconnect_event_fence_not_connected_reply_and_malformed_reply();
    test_disconnect_event_fence_cancel_and_shared_budget_wrap();
#endif
    if (!P4_WIFI_FULLMAC_PEER_CONTRACT_VERIFIED) {
        test_unqualified_peer_refuses_typed_without_io();
        printf("wifi owner unqualified-peer refusal checks: %lu\n", checks);
        return 0;
    }
    test_disconnect_requires_healthy_service_gate();
    test_disconnect_uses_correlated_not_connected();
    test_disconnect_waits_through_attached_snapshot();
    test_disconnect_failures_never_infer_down();
    test_disconnect_revision_race();
    test_success_open_and_protected();
    test_fullmac_typed_profile_association();
    test_fullmac_event_matcher_strict_identity();
    test_fullmac_event_required_and_config_baseline();
    test_fullmac_event_identity_failure_matrix();
    test_fullmac_delayed_duplicate_and_snapshot_races();
    test_fullmac_typed_cancellation_and_late_ack();
    test_fullmac_validation_and_pre_io_refusal();
    test_final_cancellation_refuses_snapshot();
    test_default_gate_and_validation_refuse_without_rpc();
    test_remote_failures_and_absence_requirements();
    test_association_initial_not_connected_and_delayed_absence();
    test_association_initial_attached_wait_is_bounded();
    test_ap_identity_cipher_and_channel_validation();
    test_initial_and_final_event_races();
    test_bounded_repeated_event_races();
    test_cancel_between_initial_rpc_steps();
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
