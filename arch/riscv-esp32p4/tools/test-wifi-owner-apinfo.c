/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#include "../network/hosted_link.h"
#include "../network/hosted_events.h"
#include "../network/hosted_mac.h"
#include "../network/hosted_owner.h"
#include "../network/hosted_association.h"
#include "../network/hosted_scan.h"
#include "../network/hosted_tx.h"
#include "../network/hosted_wifi.h"

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

typedef uint32_t ULONG;
typedef int32_t LONG;
typedef uint8_t UBYTE;
typedef int BOOL;
#define FALSE 0
#define TRUE 1

#define P4_WIFI_LINK_UNKNOWN 0u
#define P4_WIFI_LINK_UP 1u
#define P4_WIFI_LINK_DOWN 2u
#include "wifi_owner_apinfo_types.inc"

struct Task { unsigned marker; };
struct SignalSemaphore { struct Task *ss_Owner; unsigned depth; };
struct P4WifiBase {
    BOOL associated;
    BOOL association_enabled;
    struct p4_wifi_link_info confirmed_ap;
    ULONG link_state;
    struct p4_wifi_link_info observed_ap;
    struct SignalSemaphore client_lock;
};

static struct Task test_task;
static struct P4WifiBase test_resource;
static struct P4WifiBase *wifi_resource_base = &test_resource;

static struct Task *FindTask(void *name)
{
    (void)name;
    return &test_task;
}

static void ObtainSemaphore(struct SignalSemaphore *sem)
{
    if (sem->depth == 0u)
        sem->ss_Owner = FindTask(NULL);
    else if (sem->ss_Owner != FindTask(NULL))
        abort();
    ++sem->depth;
}

static void ReleaseSemaphore(struct SignalSemaphore *sem)
{
    if (sem->depth == 0u || sem->ss_Owner != FindTask(NULL))
        abort();
    if (--sem->depth == 0u)
        sem->ss_Owner = NULL;
}

struct fake_host {
    ULONG (*now_us)(void);
    ULONG generation;
    ULONG slot_clock[1];
    struct SignalSemaphore lock;
    BOOL dma_faulted;
    BOOL (*acquire)(struct fake_host *, ULONG);
    void (*release)(struct fake_host *);
};

struct wifi_probe_context {
    BOOL owner_started;
    BOOL owner_init_seen;
    BOOL association_active;
    struct fake_host *host;
    ULONG generation;
    ULONG peer_clock;
    BOOL association_exchanging;
    ULONG owner_events;
    ULONG owner_last_event;
    struct p4_hosted_link_observation link_observation;
    struct p4_hosted_sta_ap_info link_snapshot;
    ULONG link_query_revision;
    ULONG link_query_started;
    BOOL link_snapshot_matches_hint;
    ULONG accepted_profile;
    ULONG accepted_generation;
    ULONG accepted_revision;
    struct p4_wifi_link_info accepted_ap;
};

struct p4_wifi_scan_result {
    struct p4_hosted_scan_records aps;
};

struct wifi_owner_control {
    struct p4_hosted_owner_ops ops;
    struct p4_hosted_rx_ops rx_ops;
    struct p4_hosted_owner_result result;
    struct p4_hosted_mac mac;
    struct p4_hosted_tx_result frame_tx;
    struct p4_hosted_scan_count scan_count;
    struct p4_hosted_sta_ap_info ap_info;
    struct p4_hosted_sta_config sta_config;
    struct p4_wifi_scan_result scan_result;
    uint16_t scan_capacity;
    uint8_t request[256];
    uint32_t uid;
    int32_t status;
    unsigned init_attempted, start_attempted, startup_ok, cleanup_ok;
#if defined(P4_WIFI_ASSOC_DIAGNOSTICS) && P4_WIFI_ASSOC_DIAGNOSTICS
    int association_reply_valid;
#endif
};

static const struct p4_hosted_wifi_config wifi_reference_profile = {0};

static void CopyMem(const void *source, void *destination, ULONG size)
{
    memmove(destination, source, (size_t)size);
}

static void p4host_zero(void *memory, size_t size)
{
    volatile uint8_t *bytes = (volatile uint8_t *)memory;
    size_t index;
    for (index = 0u; index < size; ++index)
        bytes[index] = 0u;
}

static int wifi_owner_event(void *opaque,
                            const struct p4_hosted_inbox_item *item);

static unsigned int checks;
static unsigned int failures;

#define CHECK(expression) do {                                               \
    ++checks;                                                                \
    if (!(expression)) {                                                     \
        ++failures;                                                          \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,             \
                #expression);                                                \
    }                                                                        \
} while (0)

#define DIAGNOSTIC_LOG_CAPACITY 16384u
static char diagnostic_log[DIAGNOSTIC_LOG_CAPACITY];
static size_t diagnostic_log_size;
static unsigned int diagnostic_bad_formats;

static void diagnostic_reset(void)
{
    diagnostic_log_size = 0u;
    diagnostic_bad_formats = 0u;
    diagnostic_log[0] = '\0';
}

/* The extracted production code may emit only these numeric records. Keep
 * the formats exact so a future accidental SSID/BSSID/credential addition
 * fails the fixture instead of silently reaching its capture buffer. */
void bug(const char *format, ...)
{
    static const char event_format[] =
        "[WiFiEvent] id=%u g=%u rev=%u k=%u st=%ld reason=%u\n";
    static const char hint_format[] = "[WiFiHint] id=%u g=%u st=%ld\n";
    static const char link_format[] =
        "[WiFiLink] g=%u u=%u st=%ld rb=%u ra=%u ev=%u lastid=%u k=%u reason=%u\n";
    static const char rpc_start_format[] =
        "[WiFiRPC] p=1 id=%u u=%u us=%u\n";
    static const char rpc_finish_format[] =
        "[WiFiRPC] p=2 id=%u u=%u us=%u sub=%u v=0 st=-1\n";
    static const char rpc_status_format[] =
        "[WiFiRPC] p=2 id=%u u=%u us=%u sub=%u v=1 st=%ld\n";
    va_list arguments;
    int written;

    if (strcmp(format, event_format) != 0 &&
        strcmp(format, hint_format) != 0 &&
        strcmp(format, link_format) != 0 &&
        strcmp(format, rpc_start_format) != 0 &&
        strcmp(format, rpc_finish_format) != 0 &&
        strcmp(format, rpc_status_format) != 0) {
        ++diagnostic_bad_formats;
        return;
    }
    if (diagnostic_log_size >= sizeof diagnostic_log) {
        ++diagnostic_bad_formats;
        return;
    }
    va_start(arguments, format);
    written = vsnprintf(diagnostic_log + diagnostic_log_size,
                        sizeof diagnostic_log - diagnostic_log_size,
                        format, arguments);
    va_end(arguments);
    if (written < 0 || (size_t)written >=
                       sizeof diagnostic_log - diagnostic_log_size) {
        ++diagnostic_bad_formats;
        return;
    }
    diagnostic_log_size += (size_t)written;
}

static void check_diagnostics(const char *expected)
{
    CHECK(diagnostic_bad_formats == 0u);
#if defined(P4_WIFI_ASSOC_DIAGNOSTICS) && P4_WIFI_ASSOC_DIAGNOSTICS
    CHECK(strcmp(diagnostic_log, expected) == 0);
#else
    (void)expected;
    CHECK(diagnostic_log[0] == '\0');
#endif
}

static void check_no_ap_identity_output(void)
{
    CHECK(strstr(diagnostic_log, "S3CR3T_SSID") == NULL);
    CHECK(strstr(diagnostic_log, "BSSID1") == NULL);
    CHECK(strstr(diagnostic_log, "PW_SECRET") == NULL);
}

struct wire_buffer {
    uint8_t bytes[512];
    size_t size;
    int ok;
};

static void wire_init(struct wire_buffer *buffer)
{
    memset(buffer, 0, sizeof *buffer);
    buffer->ok = 1;
}

static void wire_byte(struct wire_buffer *buffer, uint8_t value)
{
    if (!buffer->ok || buffer->size == sizeof buffer->bytes) {
        buffer->ok = 0;
        return;
    }
    buffer->bytes[buffer->size++] = value;
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
                     unsigned int wire_type)
{
    wire_varint(buffer, ((uint64_t)field << 3) | wire_type);
}

static void wire_uint_field(struct wire_buffer *buffer, uint32_t field,
                            uint32_t value)
{
    wire_key(buffer, field, 0u);
    wire_varint(buffer, value);
}

static void wire_int32_field(struct wire_buffer *buffer, uint32_t field,
                             int32_t value)
{
    wire_key(buffer, field, 0u);
    wire_varint(buffer, (uint64_t)(int64_t)value);
}

static void wire_bytes_field(struct wire_buffer *buffer, uint32_t field,
                             const uint8_t *bytes, size_t size)
{
    wire_key(buffer, field, 2u);
    wire_varint(buffer, (uint64_t)size);
    if (size > sizeof buffer->bytes - buffer->size ||
        (size != 0u && bytes == NULL)) {
        buffer->ok = 0;
        return;
    }
    if (size != 0u) {
        memcpy(buffer->bytes + buffer->size, bytes, size);
        buffer->size += size;
    }
}

static int bytes_are_zero(const void *memory, size_t size)
{
    const uint8_t *bytes = memory;
    size_t index;
    for (index = 0u; index < size; ++index)
        if (bytes[index] != 0u)
            return 0;
    return 1;
}

static int bytes_match(const void *memory, size_t size, uint8_t value)
{
    const uint8_t *bytes = memory;
    size_t index;
    for (index = 0u; index < size; ++index)
        if (bytes[index] != value)
            return 0;
    return 1;
}

enum fake_exchange_mode {
    FAKE_AP_SUCCESS,
    FAKE_REMOTE_300F,
    FAKE_REMOTE_OTHER,
    FAKE_MALFORMED_AP,
    FAKE_WRONG_UID,
    FAKE_FAIL_UNSUBMITTED,
    FAKE_TIMEOUT_SUBMITTED,
    FAKE_LATE_SUCCESS,
    FAKE_INTERLEAVE_DOWN,
    FAKE_INTERLEAVE_UP,
    FAKE_INTERLEAVE_SAME_UP,
    FAKE_INTERLEAVE_GENERATION,
    FAKE_INTERLEAVE_HOST_EPOCH,
    FAKE_INTERLEAVE_BEACON,
    FAKE_INTERLEAVE_CHANNEL,
    FAKE_MALFORMED_STATUS,
    FAKE_WRONG_ASSOC_RESPONSE_ID
};

struct ap_fixture {
    struct fake_host host;
    struct wifi_probe_context context;
    struct p4_hosted_owner owner;
    struct p4_hosted_fifo fifo;
    struct wifi_owner_control control;
    ULONG now;
    ULONG advance_us_after_exchange;
    ULONG request_timeout;
    ULONG last_request_uid;
    uint32_t last_request_id;
    uint32_t response_id;
    uint32_t response_uid;
    size_t last_request_payload_size;
    int32_t response_status;
    enum fake_exchange_mode mode;
    uint8_t ap_ssid[P4_HOSTED_SCAN_SSID_MAX_SIZE];
    size_t ap_ssid_size;
    uint8_t ap_bssid[P4_HOSTED_SCAN_BSSID_SIZE];
    uint32_t ap_channel;
    int32_t ap_authmode;
    unsigned ap_fault;
    unsigned int exchange_calls;
    unsigned int request_valid;
    unsigned int response_encoded;
    unsigned int event_dispatches;
};

static struct ap_fixture fixture;
static struct ap_fixture *clock_fixture;
static unsigned host_acquire_calls;
static unsigned host_release_calls;

static void expect_link_unknown(void)
{
    CHECK(test_resource.associated == FALSE);
    CHECK(test_resource.link_state == P4_WIFI_LINK_UNKNOWN);
    CHECK(bytes_are_zero(&test_resource.observed_ap,
                         sizeof test_resource.observed_ap));
}

static void expect_observed_ap(void)
{
    ULONG expected_authmode = fixture.context.accepted_profile ?
        fixture.context.accepted_ap.authmode : (ULONG)fixture.ap_authmode;
    CHECK(test_resource.associated == FALSE);
    CHECK(test_resource.link_state == P4_WIFI_LINK_UP);
    CHECK(test_resource.observed_ap.ssid_length == fixture.ap_ssid_size);
    CHECK(memcmp(test_resource.observed_ap.ssid, fixture.ap_ssid,
                 fixture.ap_ssid_size) == 0);
    CHECK(memcmp(test_resource.observed_ap.bssid, fixture.ap_bssid, 6u) == 0);
    CHECK(test_resource.observed_ap.channel == fixture.ap_channel);
    CHECK(test_resource.observed_ap.authmode == expected_authmode);
}

static ULONG fake_now_us(void)
{
    return clock_fixture->now;
}

static BOOL fake_host_acquire(struct fake_host *host, ULONG wait_us)
{
    ++host_acquire_calls;
    if (!host || !wait_us || host->lock.depth || host->dma_faulted)
        return FALSE;
    host->lock.depth = 1u;
    host->lock.ss_Owner = FindTask(NULL);
    return TRUE;
}

static void fake_host_release(struct fake_host *host)
{
    ++host_release_calls;
    if (!host || host->lock.depth != 1u ||
        host->lock.ss_Owner != FindTask(NULL))
        abort();
    host->lock.depth = 0u;
    host->lock.ss_Owner = NULL;
}

static void fill_pattern(uint8_t *bytes, size_t size, uint8_t seed)
{
    size_t index;
    for (index = 0u; index < size; ++index)
        bytes[index] = (uint8_t)(seed + (uint8_t)(index * 23u));
}

static int make_link_event_item(struct p4_hosted_inbox_item *item,
                                uint32_t event_id, uint8_t seed)
{
    struct wire_buffer data, wrapper;
    uint8_t ssid[P4_HOSTED_ASSOCIATION_SSID_SIZE];
    uint8_t bssid[P4_HOSTED_ASSOCIATION_BSSID_SIZE];
    fill_pattern(ssid, sizeof ssid, seed);
    fill_pattern(bssid, sizeof bssid, (uint8_t)(seed + 17u));
    ssid[1] = 0u;
    ssid[31] = 0xffu;

    wire_init(&data);
    wire_bytes_field(&data, 1u, ssid, sizeof ssid);
    wire_uint_field(&data, 2u, sizeof ssid);
    wire_bytes_field(&data, 3u, bssid, sizeof bssid);
    if (event_id == 775u) {
        wire_uint_field(&data, 4u, 11u);
        wire_uint_field(&data, 5u, 3u);
        wire_uint_field(&data, 6u, 7u);
    } else {
        wire_uint_field(&data, 4u, 8u);
        wire_uint_field(&data, 5u, 1u);
    }
    wire_init(&wrapper);
    wire_uint_field(&wrapper, 1u, 0u);
    wire_bytes_field(&wrapper, 2u, data.bytes, data.size);
    if (!data.ok || !wrapper.ok || wrapper.size > sizeof item->payload)
        return 0;
    memset(item, 0, sizeof *item);
    item->msg_type = P4_HOSTED_RPC_TYPE_EVENT;
    item->msg_id = event_id;
    item->uid = 0u;
    item->payload_size = wrapper.size;
    memcpy(item->payload, wrapper.bytes, wrapper.size);
    return 1;
}

static int make_current_ap_event_item(struct p4_hosted_inbox_item *item)
{
    struct wire_buffer data, wrapper;
    uint8_t ssid[P4_HOSTED_ASSOCIATION_SSID_SIZE] = { 0 };
    ULONG event_authmode = fixture.context.accepted_profile ?
        fixture.context.accepted_ap.authmode : (ULONG)fixture.ap_authmode;

    if (fixture.ap_ssid_size > sizeof ssid)
        return 0;
    memcpy(ssid, fixture.ap_ssid, fixture.ap_ssid_size);
    wire_init(&data);
    wire_bytes_field(&data, 1u, ssid, sizeof ssid);
    wire_uint_field(&data, 2u, (uint32_t)fixture.ap_ssid_size);
    wire_bytes_field(&data, 3u, fixture.ap_bssid,
                     sizeof fixture.ap_bssid);
    wire_uint_field(&data, 4u, fixture.ap_channel);
    wire_uint_field(&data, 5u, (uint32_t)event_authmode);
    wire_uint_field(&data, 6u, 1u);
    wire_init(&wrapper);
    wire_uint_field(&wrapper, 1u, 0u);
    wire_bytes_field(&wrapper, 2u, data.bytes, data.size);
    if (!data.ok || !wrapper.ok || wrapper.size > sizeof item->payload)
        return 0;
    memset(item, 0, sizeof *item);
    item->msg_type = P4_HOSTED_RPC_TYPE_EVENT;
    item->msg_id = 775u;
    item->uid = 0u;
    item->payload_size = wrapper.size;
    memcpy(item->payload, wrapper.bytes, wrapper.size);
    return 1;
}

static int make_ap_response_payload(struct ap_fixture *state,
                                    uint8_t *payload, size_t capacity,
                                    size_t *payload_size)
{
    struct wire_buffer record, body;
    uint8_t bssid[P4_HOSTED_SCAN_BSSID_SIZE];
    uint32_t channel = state->ap_channel;
    int32_t authmode = state->ap_authmode;
    size_t ssid_size = state->ap_ssid_size;
    int include_record = state->mode != FAKE_MALFORMED_AP &&
                         state->response_status == 0;

    memcpy(bssid, state->ap_bssid, sizeof bssid);
    if (state->ap_fault == 1u)
        memset(bssid, 0, sizeof bssid);
    else if (state->ap_fault == 2u)
        channel = 15u;
    else if (state->ap_fault == 3u)
        authmode = 4;
    else if (state->ap_fault == 5u)
        ssid_size = 0u;
    wire_init(&record);
    if (include_record) {
        wire_bytes_field(&record, 1u, bssid, sizeof bssid);
        wire_bytes_field(&record, 2u, state->ap_ssid,
                         ssid_size);
        wire_uint_field(&record, 3u, channel);
        wire_uint_field(&record, 6u, (uint32_t)authmode);
        wire_uint_field(&record, 7u, state->ap_fault == 4u ? 2u :
                        (authmode == 0 ? 0u : 4u));
        wire_uint_field(&record, 8u, state->ap_fault == 6u ? 2u :
                        (authmode == 0 ? 0u : 4u));
    }
    wire_init(&body);
    wire_uint_field(&body, 1u, (uint32_t)state->response_status);
    if (include_record)
        wire_bytes_field(&body, 2u, record.bytes, record.size);
    if (!record.ok || !body.ok || body.size > capacity ||
        (body.size != 0u && payload == NULL) || payload_size == NULL)
        return 0;
    memcpy(payload, body.bytes, body.size);
    *payload_size = body.size;
    return 1;
}

/* Fake transport/exchange boundary; request and response bytes still pass
 * through the production RPC, request-ID, AP-info and link codecs. In-flight
 * events invoke the extracted callback directly. This does not model the
 * owner-step dispatcher, controller locks, wire timing, or a physical radio. */
int p4_hosted_owner_exchange(struct p4_hosted_owner *owner,
    struct p4_hosted_fifo *fifo, const struct p4_hosted_owner_ops *ops,
    uint32_t generation, unsigned checksum_enabled,
    const uint8_t *request, size_t request_size, uint32_t timeout_us,
    struct p4_hosted_owner_result *result)
{
    struct ap_fixture *state = ops ? ops->context : NULL;
    struct p4_hosted_rpc request_rpc, response_rpc, decoded_response;
    struct p4_hosted_inbox_item event_item;
    struct wire_buffer status_body;
    uint8_t response_payload[256], response_wire[512];
    size_t response_payload_size = 0u, response_wire_size = 0u;
    uint32_t request_id;
    int request_ok;

    if (!state || !owner || !fifo || !request || !result)
        return 0;
    ++state->exchange_calls;
    state->request_timeout = timeout_us;
    request_ok = p4_hosted_rpc_decode(request, request_size, &request_rpc) &&
        request_rpc.msg_type == P4_HOSTED_RPC_TYPE_REQUEST &&
        (request_rpc.msg_id == 282u || request_rpc.msg_id == 283u ||
         request_rpc.msg_id == 284u || request_rpc.msg_id == 294u) &&
        (request_rpc.msg_id == 284u ? request_rpc.payload_size != 0u :
                                      request_rpc.payload_size == 0u) &&
        request_rpc.uid == owner->ids.active_uid &&
        owner->ids.active_request_id == request_rpc.msg_id &&
        generation == state->context.generation && checksum_enabled == 0u;
    state->request_valid = request_ok ? 1u : 0u;
    if (!request_ok) {
        result->error = P4_HOSTED_OWNER_PREFLIGHT;
        return 0;
    }
    request_id = request_rpc.msg_id;
    state->last_request_id = request_rpc.msg_id;
    state->last_request_uid = request_rpc.uid;
    state->last_request_payload_size = request_rpc.payload_size;

    if (state->mode == FAKE_FAIL_UNSUBMITTED) {
        result->error = P4_HOSTED_OWNER_PREFLIGHT;
        result->submitted = 0u;
        return 0;
    }
    if (state->mode == FAKE_TIMEOUT_SUBMITTED) {
        result->error = P4_HOSTED_OWNER_TIMEOUT;
        result->submitted = 1u;
        (void)p4_hosted_owner_cancel(owner, fifo, generation);
        return 0;
    }

    if (request_id == 282u || request_id == 283u || request_id == 284u) {
        wire_init(&status_body);
        if (state->mode == FAKE_MALFORMED_STATUS)
            wire_bytes_field(&status_body, 1u, NULL, 0u);
        else
            wire_int32_field(&status_body, 1u, state->response_status);
        if (!status_body.ok) {
            result->error = P4_HOSTED_OWNER_PREFLIGHT;
            result->submitted = 0u;
            return 0;
        }
        response_rpc.msg_type = P4_HOSTED_RPC_TYPE_RESPONSE;
        response_rpc.msg_id = request_id + 256u;
        if (state->mode == FAKE_WRONG_ASSOC_RESPONSE_ID)
            ++response_rpc.msg_id;
        response_rpc.uid = state->mode == FAKE_WRONG_UID
            ? request_rpc.uid + 1u : request_rpc.uid;
        response_rpc.payload = status_body.bytes;
        response_rpc.payload_size = status_body.size;
        if (!p4_hosted_rpc_encode(response_wire, sizeof response_wire,
                                  &response_rpc, &response_wire_size) ||
            !p4_hosted_rpc_decode(response_wire, response_wire_size,
                                  &decoded_response))
            return 0;
        state->response_encoded = 1u;
        state->response_id = decoded_response.msg_id;
        state->response_uid = decoded_response.uid;
        memset(&owner->reply, 0, sizeof owner->reply);
        owner->reply.msg_type = decoded_response.msg_type;
        owner->reply.msg_id = decoded_response.msg_id;
        owner->reply.uid = decoded_response.uid;
        owner->reply.payload_size = decoded_response.payload_size;
        if (decoded_response.payload_size != 0u)
            memcpy(owner->reply.payload, decoded_response.payload,
                   decoded_response.payload_size);
        result->error = P4_HOSTED_OWNER_OK;
        result->submitted = 1u;
        result->matched = 1u;
        result->uid = request_rpc.uid;
        result->request_id = request_id;
        if (!p4_hosted_request_ids_finish(&owner->ids, generation,
                                          request_rpc.uid, request_id))
            return 0;
        state->now += state->advance_us_after_exchange;
        return 1;
    }

    if (state->mode == FAKE_INTERLEAVE_DOWN ||
        state->mode == FAKE_INTERLEAVE_UP ||
        state->mode == FAKE_INTERLEAVE_SAME_UP) {
        uint32_t event_id = state->mode == FAKE_INTERLEAVE_DOWN ? 776u : 775u;
        int event_ok = state->mode == FAKE_INTERLEAVE_SAME_UP ?
            make_current_ap_event_item(&event_item) :
            make_link_event_item(&event_item, event_id,
                state->mode == FAKE_INTERLEAVE_DOWN ? 0x71u : 0x91u);
        if (!event_ok ||
            wifi_owner_event(&state->context, &event_item) != 1)
            return 0;
        ++state->event_dispatches;
    } else if (state->mode == FAKE_INTERLEAVE_BEACON ||
               state->mode == FAKE_INTERLEAVE_CHANNEL) {
        memset(&event_item, 0, sizeof event_item);
        event_item.msg_type = P4_HOSTED_RPC_TYPE_EVENT;
        event_item.msg_id = 773u;
        event_item.payload[0] = 0x10u;
        event_item.payload[1] = state->mode == FAKE_INTERLEAVE_BEACON ? 21u : 43u;
        event_item.payload_size = 2u;
        if (wifi_owner_event(&state->context, &event_item) != 1)
            return 0;
        ++state->event_dispatches;
    } else if (state->mode == FAKE_INTERLEAVE_GENERATION) {
        ++state->context.generation;
    } else if (state->mode == FAKE_INTERLEAVE_HOST_EPOCH) {
        ++state->host.generation;
    }

    if (state->mode == FAKE_REMOTE_300F)
        state->response_status = 0x300f;
    else if (state->mode == FAKE_REMOTE_OTHER)
        state->response_status = 5;
    else if (state->mode == FAKE_MALFORMED_AP)
        state->response_status = 0;
    else
        state->response_status = 0;

    if (!make_ap_response_payload(state, response_payload,
                                  sizeof response_payload,
                                  &response_payload_size))
        return 0;
    response_rpc.msg_type = P4_HOSTED_RPC_TYPE_RESPONSE;
    response_rpc.msg_id = state->response_id != 0u ? state->response_id : 550u;
    response_rpc.uid = state->mode == FAKE_WRONG_UID
        ? request_rpc.uid + 1u : request_rpc.uid;
    response_rpc.payload = response_payload;
    response_rpc.payload_size = response_payload_size;
    if (state->mode == FAKE_WRONG_UID)
        response_rpc.msg_id = 550u;
    if (!p4_hosted_rpc_encode(response_wire, sizeof response_wire,
                              &response_rpc, &response_wire_size) ||
        !p4_hosted_rpc_decode(response_wire, response_wire_size,
                              &decoded_response))
        return 0;
    state->response_encoded = 1u;
    state->response_id = decoded_response.msg_id;
    state->response_uid = decoded_response.uid;
    memset(&owner->reply, 0, sizeof owner->reply);
    owner->reply.msg_type = decoded_response.msg_type;
    owner->reply.msg_id = decoded_response.msg_id;
    owner->reply.uid = decoded_response.uid;
    owner->reply.payload_size = decoded_response.payload_size;
    if (decoded_response.payload_size != 0u)
        memcpy(owner->reply.payload, decoded_response.payload,
               decoded_response.payload_size);

    if (state->mode == FAKE_LATE_SUCCESS)
        state->now += 1000000u;
    result->error = P4_HOSTED_OWNER_OK;
    result->submitted = 1u;
    result->matched = 1u;
    result->uid = request_rpc.uid;
    result->request_id = request_id;
    if (!p4_hosted_request_ids_finish(&owner->ids, generation,
                                      request_rpc.uid, request_id))
        return 0;
    return 1;
}

/* Generated from production runtime_probe.c with exact signature checks. */
#include "wifi_owner_apinfo_extract.inc"

static void fixture_init(void)
{
    memset(&fixture, 0, sizeof fixture);
    memset(&test_resource, 0, sizeof test_resource);
    test_resource.associated = TRUE;
    test_resource.association_enabled = TRUE;
    test_resource.link_state = P4_WIFI_LINK_UP;
    memset(&test_resource.observed_ap, 0xa5,
           sizeof test_resource.observed_ap);
    memset(&test_resource.confirmed_ap, 0xa5,
           sizeof test_resource.confirmed_ap);
    host_acquire_calls = 0u;
    host_release_calls = 0u;
    wifi_resource_base = &test_resource;
    clock_fixture = &fixture;
    fixture.host.now_us = fake_now_us;
    fixture.context.host = &fixture.host;
    fixture.context.generation = UINT32_C(0x12345670);
    fixture.context.peer_clock = UINT32_C(400000);
    fixture.now = 100u;
    fixture.mode = FAKE_AP_SUCCESS;
    fixture.ap_ssid_size = 12u;
    fixture.ap_channel = 11u;
    fixture.ap_authmode = 3;
    fill_pattern(fixture.ap_ssid, sizeof fixture.ap_ssid, 0x21u);
    fixture.ap_ssid[1] = 0u;
    fixture.ap_ssid[10] = 0xffu;
    fill_pattern(fixture.ap_bssid, sizeof fixture.ap_bssid, 0x81u);
    fixture.ap_bssid[0] = 0x02u;
    fixture.host.generation = fixture.context.generation;
    fixture.host.slot_clock[0] = fixture.context.peer_clock;
    fixture.host.now_us = fake_now_us;
    fixture.host.acquire = fake_host_acquire;
    fixture.host.release = fake_host_release;
    CHECK(p4_hosted_link_init(&fixture.context.link_observation,
                              fixture.context.generation));
    CHECK(p4_hosted_rx_start(&fixture.owner.rx,
                             fixture.context.generation));
    CHECK(p4_hosted_fifo_start(&fixture.fifo,
                               fixture.context.generation, 32u));
    CHECK(p4_hosted_owner_ids_init(&fixture.owner,
        fixture.context.generation, fixture.now));
    fixture.control.ops.context = &fixture;
}

static void prepare_hint(const uint8_t *ssid, size_t ssid_size,
                         const uint8_t *bssid, uint32_t channel,
                         int32_t authmode)
{
    struct p4_hosted_link_observation *hint =
        &fixture.context.link_observation;
    hint->revision = 1u;
    hint->kind = P4_HOSTED_LINK_UP_HINT;
    hint->status = 0;
    memcpy(hint->ssid, ssid, P4_HOSTED_ASSOCIATION_SSID_SIZE);
    hint->ssid_length = (uint32_t)ssid_size;
    memcpy(hint->bssid, bssid, P4_HOSTED_ASSOCIATION_BSSID_SIZE);
    hint->channel = channel;
    hint->authmode = authmode;
    hint->reason = 0u;
}

static void prepare_typed_acceptance(ULONG profile,
                                     int32_t event_authmode)
{
    uint8_t hint_ssid[P4_HOSTED_ASSOCIATION_SSID_SIZE] = { 0 };
    ULONG accepted_authmode;

    memcpy(hint_ssid, fixture.ap_ssid, fixture.ap_ssid_size);
    prepare_hint(hint_ssid, fixture.ap_ssid_size, fixture.ap_bssid,
                 fixture.ap_channel, event_authmode);
    fixture.context.accepted_profile = profile;
    fixture.context.accepted_generation = fixture.context.generation;
    fixture.context.accepted_revision =
        fixture.context.link_observation.revision;
    if (profile == P4_WIFI_FULLMAC_OPEN)
        accepted_authmode = 0u;
    else if (profile == P4_WIFI_FULLMAC_WPA2_CCMP)
        accepted_authmode = 3u;
    else if (profile == P4_WIFI_FULLMAC_SAE_CCMP_PMF)
        accepted_authmode = 6u;
    else
        accepted_authmode = (ULONG)fixture.ap_authmode;
    p4host_zero(&fixture.context.accepted_ap,
                sizeof fixture.context.accepted_ap);
    memcpy(fixture.context.accepted_ap.ssid, fixture.ap_ssid,
           fixture.ap_ssid_size);
    fixture.context.accepted_ap.ssid_length =
        (ULONG)fixture.ap_ssid_size;
    memcpy(fixture.context.accepted_ap.bssid, fixture.ap_bssid,
           sizeof fixture.ap_bssid);
    fixture.context.accepted_ap.channel = fixture.ap_channel;
    fixture.context.accepted_ap.authmode = accepted_authmode;
}

static int query(void)
{
    return wifi_owner_refresh_link_snapshot(&fixture.context,
        &fixture.owner, &fixture.control, &fixture.fifo, 0u);
}

static int request_owner_rpc(uint32_t id)
{
    return wifi_owner_request(&fixture.context, &fixture.owner,
        &fixture.control, &fixture.fifo, 0u, id, 1000000u);
}

static void prepare_secret_hint(void)
{
    static const uint8_t secret_ssid[] = "S3CR3T_SSID";
    uint8_t hint_ssid[P4_HOSTED_ASSOCIATION_SSID_SIZE] = { 0 };

    memset(fixture.ap_ssid, 0, sizeof fixture.ap_ssid);
    memcpy(fixture.ap_ssid, secret_ssid, sizeof secret_ssid - 1u);
    fixture.ap_ssid_size = sizeof secret_ssid - 1u;
    memcpy(fixture.ap_bssid, "BSSID1", P4_HOSTED_SCAN_BSSID_SIZE);
    memcpy(hint_ssid, fixture.ap_ssid, fixture.ap_ssid_size);
    prepare_hint(hint_ssid, fixture.ap_ssid_size, fixture.ap_bssid,
                 fixture.ap_channel, fixture.ap_authmode);
}

static void expect_match(void)
{
    CHECK(fixture.context.link_snapshot_matches_hint == TRUE);
    CHECK(fixture.context.link_snapshot.status == 0);
    CHECK(fixture.context.link_snapshot.ap.ssid_size ==
          fixture.context.link_observation.ssid_length);
    CHECK(memcmp(fixture.context.link_snapshot.ap.ssid,
                 fixture.context.link_observation.ssid,
                 fixture.context.link_observation.ssid_length) == 0);
    CHECK(memcmp(fixture.context.link_snapshot.ap.bssid,
                 fixture.context.link_observation.bssid,
                 P4_HOSTED_ASSOCIATION_BSSID_SIZE) == 0);
    CHECK(fixture.context.link_snapshot.ap.primary ==
          fixture.context.link_observation.channel);
    if (fixture.context.accepted_profile == P4_WIFI_FULLMAC_SAE_CCMP_PMF)
        CHECK(fixture.context.link_snapshot.ap.authmode == 6 ||
              fixture.context.link_snapshot.ap.authmode == 7);
    else
        CHECK(fixture.context.link_snapshot.ap.authmode ==
              fixture.context.link_observation.authmode);
    expect_observed_ap();
}

static void expect_typed_owner_retired(void)
{
    CHECK(fixture.owner.poisoned == 1u);
    CHECK(fixture.fifo.poisoned == 1u);
    CHECK(fixture.owner.ids.retired == 1u);
    CHECK(fixture.owner.ids.active_uid == 0u);
    CHECK(fixture.host.lock.depth == 0u);
    CHECK(host_acquire_calls == host_release_calls);
    expect_link_unknown();
}

static void prepare_typed_failure(ULONG profile, int32_t event_authmode)
{
    prepare_typed_acceptance(profile, event_authmode);
    memset(&fixture.context.link_snapshot, 0xa5,
           sizeof fixture.context.link_snapshot);
    fixture.context.link_snapshot_matches_hint = TRUE;
    memset(&fixture.control.ap_info, 0x5a, sizeof fixture.control.ap_info);
}

static void expect_typed_failure(void)
{
    CHECK(!query());
    CHECK(fixture.exchange_calls == 1u);
    CHECK(fixture.context.link_snapshot_matches_hint == FALSE);
    CHECK(fixture.context.link_snapshot.status == 0);
    CHECK(bytes_are_zero(&fixture.control.ap_info,
                         sizeof fixture.control.ap_info));
    expect_typed_owner_retired();
}

static void run_typed_success(ULONG profile, int32_t ap_authmode,
                              int32_t event_authmode)
{
    fixture_init();
    fixture.ap_authmode = ap_authmode;
    prepare_typed_acceptance(profile, event_authmode);
    CHECK(fixture.context.accepted_generation == fixture.context.generation);
    CHECK(fixture.context.accepted_revision ==
          fixture.context.link_observation.revision);
    CHECK(query());
    CHECK(fixture.exchange_calls == 1u);
    expect_match();

    /* A repeated lookup reuses only this accepted revision's fresh snapshot. */
    CHECK(query());
    CHECK(fixture.exchange_calls == 1u);
    expect_match();

    fixture.now += 1000000u;
    CHECK(query());
    CHECK(fixture.exchange_calls == 2u);
    expect_match();
}

static void test_typed_profile_apinfo_acceptance(void)
{
    run_typed_success(P4_WIFI_FULLMAC_OPEN, 0, 0);
    run_typed_success(P4_WIFI_FULLMAC_WPA2_CCMP, 3, 3);
    run_typed_success(P4_WIFI_FULLMAC_SAE_CCMP_PMF, 6, 6);
    /* IDF scan authmode 7 is accepted for a successful pure-SAE profile,
     * while the published credential-free identity remains authmode 6. */
    run_typed_success(P4_WIFI_FULLMAC_SAE_CCMP_PMF, 7, 6);
}

static void test_typed_profile_rejections(void)
{
    fixture_init();
    prepare_typed_failure(3u, 3);
    expect_typed_failure(); /* unsupported profile */

    fixture_init();
    prepare_typed_failure(P4_WIFI_FULLMAC_WPA2_CCMP, 3);
    fixture.context.accepted_generation = 0u;
    expect_typed_failure();

    fixture_init();
    prepare_typed_failure(P4_WIFI_FULLMAC_WPA2_CCMP, 3);
    fixture.context.accepted_generation--;
    expect_typed_failure();

    fixture_init();
    prepare_typed_failure(P4_WIFI_FULLMAC_WPA2_CCMP, 3);
    fixture.context.link_observation.generation = 0u;
    CHECK(query()); /* The event-generation race gate remains conservative. */
    CHECK(fixture.exchange_calls == 1u);
    CHECK(fixture.context.link_snapshot_matches_hint == FALSE);
    CHECK(!fixture.owner.poisoned && !fixture.fifo.poisoned);
    expect_link_unknown();

    fixture_init();
    prepare_typed_failure(P4_WIFI_FULLMAC_WPA2_CCMP, 3);
    fixture.context.accepted_revision = 0u;
    expect_typed_failure();

    fixture_init();
    prepare_typed_failure(P4_WIFI_FULLMAC_WPA2_CCMP, 3);
    ++fixture.context.link_observation.revision;
    expect_typed_failure();

    fixture_init();
    prepare_typed_failure(P4_WIFI_FULLMAC_WPA2_CCMP, 3);
    fixture.context.accepted_ap.ssid[2] ^= 0x80u;
    expect_typed_failure();

    fixture_init();
    prepare_typed_failure(P4_WIFI_FULLMAC_WPA2_CCMP, 3);
    fixture.context.accepted_ap.bssid[5] ^= 1u;
    expect_typed_failure();

    fixture_init();
    prepare_typed_failure(P4_WIFI_FULLMAC_WPA2_CCMP, 3);
    ++fixture.context.accepted_ap.channel;
    expect_typed_failure();

    fixture_init();
    prepare_typed_failure(P4_WIFI_FULLMAC_WPA2_CCMP, 3);
    fixture.context.link_observation.ssid[2] ^= 0x80u;
    expect_typed_failure();

    fixture_init();
    prepare_typed_failure(P4_WIFI_FULLMAC_WPA2_CCMP, 3);
    fixture.context.link_observation.bssid[5] ^= 1u;
    expect_typed_failure();

    fixture_init();
    prepare_typed_failure(P4_WIFI_FULLMAC_WPA2_CCMP, 3);
    fixture.context.link_observation.channel++;
    expect_typed_failure();

    fixture_init();
    prepare_typed_failure(P4_WIFI_FULLMAC_WPA2_CCMP, 0);
    expect_typed_failure(); /* event authmode contradicts accepted WPA2 */

    fixture_init();
    prepare_typed_failure(P4_WIFI_FULLMAC_WPA2_CCMP, 3);
    fixture.ap_authmode = 0;
    expect_typed_failure(); /* peer reports open rather than WPA2 */

    fixture_init();
    prepare_typed_failure(P4_WIFI_FULLMAC_WPA2_CCMP, 3);
    fixture.ap_fault = 4u;
    expect_typed_failure(); /* wrong pairwise cipher */

    fixture_init();
    prepare_typed_failure(P4_WIFI_FULLMAC_WPA2_CCMP, 3);
    fixture.ap_fault = 6u;
    expect_typed_failure(); /* wrong group cipher */

    fixture_init();
    fixture.ap_authmode = 6;
    prepare_typed_failure(0u, 6);
    expect_typed_failure(); /* AP-only SAE is not an accepted SAE profile */
}

static void test_typed_acceptance_event_interleaving(void)
{
    fixture_init();
    prepare_typed_acceptance(P4_WIFI_FULLMAC_WPA2_CCMP, 3);
    fixture.mode = FAKE_INTERLEAVE_SAME_UP;

    /* The same AP's later event still makes the in-flight query stale. */
    CHECK(query());
    CHECK(fixture.exchange_calls == 1u);
    CHECK(fixture.event_dispatches == 1u);
    CHECK(fixture.context.link_observation.revision == 2u);
    CHECK(fixture.context.link_snapshot_matches_hint == FALSE);
    CHECK(bytes_are_zero(&fixture.context.link_snapshot,
                         sizeof fixture.context.link_snapshot));
    CHECK(bytes_are_zero(&fixture.control.ap_info,
                         sizeof fixture.control.ap_info));
    expect_link_unknown();

    /* The old acceptance cannot authorize the new event revision. */
    fixture.mode = FAKE_AP_SUCCESS;
    CHECK(!query());
    CHECK(fixture.exchange_calls == 2u);
    CHECK(fixture.context.link_snapshot_matches_hint == FALSE);
    CHECK(fixture.context.link_snapshot.status == 0);
    CHECK(bytes_are_zero(&fixture.control.ap_info,
                         sizeof fixture.control.ap_info));
    expect_typed_owner_retired();
}

static void test_exact_request_response_and_dedup(void)
{
    uint8_t hint_ssid[P4_HOSTED_ASSOCIATION_SSID_SIZE] = {0};
    uint8_t hint_bssid[P4_HOSTED_ASSOCIATION_BSSID_SIZE];
    fixture_init();
    memcpy(hint_ssid, fixture.ap_ssid, fixture.ap_ssid_size);
    memcpy(hint_bssid, fixture.ap_bssid, sizeof hint_bssid);
    prepare_hint(hint_ssid, fixture.ap_ssid_size, hint_bssid, 11u, 3);
    memset(&fixture.context.link_snapshot, 0xa5,
           sizeof fixture.context.link_snapshot);
    fixture.context.link_snapshot_matches_hint = TRUE;
    memset(&fixture.control.ap_info, 0x5a, sizeof fixture.control.ap_info);

    CHECK(query());
    expect_match();
    CHECK(fixture.exchange_calls == 1u);
    CHECK(fixture.request_valid == 1u);
    CHECK(fixture.last_request_id == 294u);
    CHECK(fixture.response_id == 550u);
    CHECK(fixture.last_request_uid == 1u);
    CHECK(fixture.response_uid == fixture.last_request_uid);
    CHECK(fixture.owner.ids.next_uid == 2u);
    CHECK(fixture.request_timeout == 1000000u);
    CHECK(fixture.owner.ids.active_uid == 0u);
    CHECK(fixture.control.ap_info.status == 0);
    CHECK(bytes_are_zero(&fixture.control.ap_info,
                         sizeof fixture.control.ap_info));
    CHECK(!fixture.owner.poisoned && !fixture.fifo.poisoned);
    CHECK(host_acquire_calls == host_release_calls);
    CHECK(fixture.host.lock.depth == 0u);
    expect_observed_ap();
    CHECK(bytes_match(&test_resource.confirmed_ap,
                      sizeof test_resource.confirmed_ap, 0xa5u));

    CHECK(query());
    expect_match();
    CHECK(fixture.exchange_calls == 1u);
    fixture.now += 999999u;
    CHECK(query());
    CHECK(fixture.exchange_calls == 1u);
    fixture.now += 1u;
    CHECK(query());
    CHECK(fixture.exchange_calls == 2u);
    expect_observed_ap();
}

static void test_remote_statuses_down_or_quarantine(void)
{
    uint8_t hint_ssid[P4_HOSTED_ASSOCIATION_SSID_SIZE] = {0};
    uint8_t hint_bssid[P4_HOSTED_ASSOCIATION_BSSID_SIZE];
    enum fake_exchange_mode modes[] = { FAKE_REMOTE_300F, FAKE_REMOTE_OTHER };
    int32_t statuses[] = { 0x300f, 5 };
    size_t index;

    for (index = 0u; index < sizeof modes / sizeof modes[0]; ++index) {
        fixture_init();
        memcpy(hint_ssid, fixture.ap_ssid, fixture.ap_ssid_size);
        memcpy(hint_bssid, fixture.ap_bssid, sizeof hint_bssid);
        prepare_hint(hint_ssid, fixture.ap_ssid_size, hint_bssid, 11u, 3);
        fixture.mode = modes[index];
        memset(&fixture.context.link_snapshot, 0xa5,
               sizeof fixture.context.link_snapshot);
        fixture.context.link_snapshot_matches_hint = TRUE;
        if (statuses[index] == 0x300f) {
            CHECK(query());
        } else {
            CHECK(!query());
        }
        CHECK(fixture.context.link_snapshot.status == statuses[index]);
        CHECK(fixture.context.link_snapshot_matches_hint == FALSE);
        CHECK(bytes_are_zero(&fixture.context.link_snapshot.ap,
                             sizeof fixture.context.link_snapshot.ap));
        CHECK(bytes_are_zero(&fixture.control.ap_info,
                             sizeof fixture.control.ap_info));
        if (statuses[index] == 0x300f) {
            CHECK(!fixture.owner.poisoned && !fixture.fifo.poisoned);
            CHECK(fixture.owner.ids.retired == 0u);
        } else {
            CHECK(fixture.owner.poisoned && fixture.fifo.poisoned);
            CHECK(fixture.owner.ids.retired == 1u);
        }
        CHECK(fixture.owner.ids.active_uid == 0u);
        if (statuses[index] == 0x300f) {
            CHECK(test_resource.associated == FALSE);
            CHECK(test_resource.link_state == P4_WIFI_LINK_DOWN);
            CHECK(bytes_are_zero(&test_resource.observed_ap,
                                 sizeof test_resource.observed_ap));
        } else {
            expect_link_unknown();
        }
    }
}

static void test_unsubmitted_and_poisoned_timeout(void)
{
    uint8_t hint_ssid[P4_HOSTED_ASSOCIATION_SSID_SIZE] = {0};
    uint8_t hint_bssid[P4_HOSTED_ASSOCIATION_BSSID_SIZE];

    fixture_init();
    memcpy(hint_ssid, fixture.ap_ssid, fixture.ap_ssid_size);
    memcpy(hint_bssid, fixture.ap_bssid, sizeof hint_bssid);
    prepare_hint(hint_ssid, fixture.ap_ssid_size, hint_bssid, 11u, 3);
    memset(&fixture.context.link_snapshot, 0x69,
           sizeof fixture.context.link_snapshot);
    fixture.context.link_snapshot_matches_hint = TRUE;
    memset(&fixture.control.ap_info, 0x96, sizeof fixture.control.ap_info);
    fixture.mode = FAKE_FAIL_UNSUBMITTED;
    CHECK(!query());
    CHECK(fixture.exchange_calls == 1u);
    CHECK(fixture.owner.poisoned == 0u && fixture.fifo.poisoned == 0u);
    CHECK(fixture.owner.ids.retired == 0u);
    CHECK(fixture.owner.ids.active_uid == 0u);
    CHECK(fixture.owner.ids.next_uid == 2u);
    CHECK(bytes_are_zero(&fixture.context.link_snapshot,
                         sizeof fixture.context.link_snapshot));
    CHECK(fixture.context.link_snapshot_matches_hint == FALSE);
    CHECK(bytes_are_zero(&fixture.control.ap_info,
                         sizeof fixture.control.ap_info));
    expect_link_unknown();

    fixture_init();
    memcpy(hint_ssid, fixture.ap_ssid, fixture.ap_ssid_size);
    memcpy(hint_bssid, fixture.ap_bssid, sizeof hint_bssid);
    prepare_hint(hint_ssid, fixture.ap_ssid_size, hint_bssid, 11u, 3);
    fixture.mode = FAKE_TIMEOUT_SUBMITTED;
    CHECK(!query());
    CHECK(fixture.owner.poisoned == 1u && fixture.fifo.poisoned == 1u);
    CHECK(fixture.owner.ids.retired == 1u);
    CHECK(fixture.owner.ids.active_uid == 0u);
    CHECK(fixture.context.link_snapshot_matches_hint == FALSE);
    CHECK(bytes_are_zero(&fixture.context.link_snapshot,
                         sizeof fixture.context.link_snapshot));
    CHECK(bytes_are_zero(&fixture.control.ap_info,
                         sizeof fixture.control.ap_info));
    expect_link_unknown();
}

static void test_wifi_rpc_diagnostics(void)
{
    fixture_init();
    fixture.response_status = 0;
    fixture.advance_us_after_exchange = 17u;
    diagnostic_reset();
    CHECK(request_owner_rpc(282u));
    CHECK(fixture.request_valid == 1u);
    CHECK(fixture.last_request_id == 282u);
    CHECK(fixture.response_id == 538u);
    CHECK(fixture.last_request_uid == 1u);
    CHECK(fixture.response_uid == 1u);
    CHECK(fixture.last_request_payload_size == 0u);
    CHECK(fixture.control.status == 0);
    CHECK(fixture.now == 117u);
#if defined(P4_WIFI_ASSOC_DIAGNOSTICS) && P4_WIFI_ASSOC_DIAGNOSTICS
    CHECK(fixture.control.association_reply_valid == 1);
#endif
    CHECK(!fixture.owner.poisoned && !fixture.fifo.poisoned);
    check_diagnostics(
        "[WiFiRPC] p=1 id=282 u=1 us=100\n"
        "[WiFiRPC] p=2 id=282 u=1 us=117 sub=1 v=1 st=0\n");
    check_no_ap_identity_output();

    fixture.advance_us_after_exchange = 0u;
    fixture.response_status = -5;
    CHECK(!request_owner_rpc(283u));
    CHECK(fixture.last_request_id == 283u);
    CHECK(fixture.response_id == 539u);
    CHECK(fixture.last_request_uid == 2u);
    CHECK(fixture.response_uid == 2u);
    CHECK(fixture.control.status == -5);
#if defined(P4_WIFI_ASSOC_DIAGNOSTICS) && P4_WIFI_ASSOC_DIAGNOSTICS
    CHECK(fixture.control.association_reply_valid == 1);
#endif
    CHECK(!fixture.owner.poisoned && !fixture.fifo.poisoned);

    fixture.mode = FAKE_MALFORMED_STATUS;
    CHECK(!request_owner_rpc(282u));
    CHECK(fixture.last_request_id == 282u);
    CHECK(fixture.last_request_uid == 3u);
    CHECK(fixture.response_id == 538u);
    CHECK(fixture.response_uid == 3u);
    CHECK(fixture.control.status == -5);
#if defined(P4_WIFI_ASSOC_DIAGNOSTICS) && P4_WIFI_ASSOC_DIAGNOSTICS
    CHECK(fixture.control.association_reply_valid == 0);
#endif
    CHECK(fixture.owner.poisoned == 1u && fixture.fifo.poisoned == 1u);
    CHECK(fixture.owner.ids.retired == 1u);
    check_diagnostics(
        "[WiFiRPC] p=1 id=282 u=1 us=100\n"
        "[WiFiRPC] p=2 id=282 u=1 us=117 sub=1 v=1 st=0\n"
        "[WiFiRPC] p=1 id=283 u=2 us=117\n"
        "[WiFiRPC] p=2 id=283 u=2 us=117 sub=1 v=1 st=-5\n"
        "[WiFiRPC] p=1 id=282 u=3 us=117\n"
        "[WiFiRPC] p=2 id=282 u=3 us=117 sub=1 v=0 st=-1\n");
    check_no_ap_identity_output();
}

static void test_wifi_rpc_failures_and_suppression(void)
{
    static const enum fake_exchange_mode invalid_modes[] = {
        FAKE_MALFORMED_STATUS,
        FAKE_WRONG_ASSOC_RESPONSE_ID,
        FAKE_WRONG_UID
    };
    size_t index;

    fixture_init();
    fixture.mode = FAKE_FAIL_UNSUBMITTED;
    diagnostic_reset();
    CHECK(!request_owner_rpc(282u));
    CHECK(fixture.last_request_id == 282u);
    CHECK(fixture.last_request_uid == 1u);
    CHECK(fixture.control.result.submitted == 0u);
#if defined(P4_WIFI_ASSOC_DIAGNOSTICS) && P4_WIFI_ASSOC_DIAGNOSTICS
    CHECK(fixture.control.association_reply_valid == 0);
#endif
    CHECK(!fixture.owner.poisoned && !fixture.fifo.poisoned);
    CHECK(fixture.owner.ids.active_uid == 0u);
    check_diagnostics(
        "[WiFiRPC] p=1 id=282 u=1 us=100\n"
        "[WiFiRPC] p=2 id=282 u=1 us=100 sub=0 v=0 st=-1\n");
    check_no_ap_identity_output();

    fixture_init();
    fixture.mode = FAKE_TIMEOUT_SUBMITTED;
    diagnostic_reset();
    CHECK(!request_owner_rpc(283u));
    CHECK(fixture.last_request_id == 283u);
    CHECK(fixture.last_request_uid == 1u);
    CHECK(fixture.control.result.submitted == 1u);
#if defined(P4_WIFI_ASSOC_DIAGNOSTICS) && P4_WIFI_ASSOC_DIAGNOSTICS
    CHECK(fixture.control.association_reply_valid == 0);
#endif
    CHECK(fixture.owner.poisoned == 1u && fixture.fifo.poisoned == 1u);
    CHECK(fixture.owner.ids.retired == 1u);
    check_diagnostics(
        "[WiFiRPC] p=1 id=283 u=1 us=100\n"
        "[WiFiRPC] p=2 id=283 u=1 us=100 sub=1 v=0 st=-1\n");
    check_no_ap_identity_output();

    for (index = 0u;
         index < sizeof invalid_modes / sizeof invalid_modes[0]; ++index) {
        fixture_init();
        fixture.mode = invalid_modes[index];
        fixture.control.status = 77;
        diagnostic_reset();
        CHECK(!request_owner_rpc(282u));
        CHECK(fixture.response_encoded == 1u);
        CHECK(fixture.last_request_id == 282u);
        CHECK(fixture.last_request_uid == 1u);
        CHECK(fixture.response_id ==
              (invalid_modes[index] == FAKE_WRONG_ASSOC_RESPONSE_ID ?
               539u : 538u));
        CHECK(fixture.response_uid ==
              (invalid_modes[index] == FAKE_WRONG_UID ? 2u : 1u));
        CHECK(fixture.control.status == 77);
#if defined(P4_WIFI_ASSOC_DIAGNOSTICS) && P4_WIFI_ASSOC_DIAGNOSTICS
        CHECK(fixture.control.association_reply_valid == 0);
#endif
        CHECK(fixture.owner.poisoned == 1u && fixture.fifo.poisoned == 1u);
        CHECK(fixture.owner.ids.retired == 1u);
        check_diagnostics(
            "[WiFiRPC] p=1 id=282 u=1 us=100\n"
            "[WiFiRPC] p=2 id=282 u=1 us=100 sub=1 v=0 st=-1\n");
        check_no_ap_identity_output();
    }

    {
        static const uint8_t secret_ssid[] = "S3CR3T_SSID";
        static const uint8_t secret_password[] = "PW_SECRET";
        static const uint8_t secret_bssid[] = "BSSID1";

        fixture_init();
        fixture.control.sta_config.ssid = secret_ssid;
        fixture.control.sta_config.ssid_length = sizeof secret_ssid - 1u;
        fixture.control.sta_config.password = secret_password;
        fixture.control.sta_config.password_length =
            sizeof secret_password - 1u;
        fixture.control.sta_config.bssid_set = 1u;
        fixture.control.sta_config.bssid = secret_bssid;
        fixture.control.sta_config.bssid_length = sizeof secret_bssid - 1u;
        diagnostic_reset();
        CHECK(request_owner_rpc(284u));
        CHECK(fixture.request_valid == 1u);
        CHECK(fixture.last_request_id == 284u);
        CHECK(fixture.response_id == 540u);
        CHECK(fixture.last_request_uid == 1u);
        CHECK(fixture.response_uid == 1u);
        CHECK(fixture.last_request_payload_size != 0u);
#if defined(P4_WIFI_ASSOC_DIAGNOSTICS) && P4_WIFI_ASSOC_DIAGNOSTICS
        CHECK(fixture.control.association_reply_valid == 1);
#endif
        check_diagnostics("");
        check_no_ap_identity_output();
    }
}

static void test_numeric_diagnostics_preserve_query_decisions(void)
{
    fixture_init();
    prepare_secret_hint();
    diagnostic_reset();
    CHECK(query());
    expect_match();
    check_diagnostics(
        "[WiFiLink] g=305419888 u=1 st=0 rb=1 ra=1 ev=0 lastid=0 k=1 reason=0\n");
    check_no_ap_identity_output();

    fixture_init();
    prepare_secret_hint();
    fixture.mode = FAKE_INTERLEAVE_DOWN;
    diagnostic_reset();
    CHECK(query());
    CHECK(fixture.event_dispatches == 1u);
    CHECK(fixture.context.link_observation.revision == 2u);
    CHECK(fixture.context.link_observation.kind == P4_HOSTED_LINK_DOWN);
    CHECK(fixture.context.link_observation.reason == 8u);
    CHECK(fixture.context.link_snapshot_matches_hint == FALSE);
    CHECK(bytes_are_zero(&fixture.control.ap_info,
                         sizeof fixture.control.ap_info));
    expect_link_unknown();
    CHECK(!fixture.owner.poisoned && !fixture.fifo.poisoned);
    check_diagnostics(
        "[WiFiEvent] id=776 g=305419888 rev=2 k=2 st=0 reason=8\n"
        "[WiFiLink] g=305419888 u=1 st=0 rb=1 ra=2 ev=1 lastid=776 k=2 reason=8\n");
    check_no_ap_identity_output();

    fixture_init();
    prepare_secret_hint();
    fixture.mode = FAKE_REMOTE_300F;
    diagnostic_reset();
    CHECK(query());
    CHECK(test_resource.associated == FALSE);
    CHECK(test_resource.link_state == P4_WIFI_LINK_DOWN);
    CHECK(bytes_are_zero(&test_resource.observed_ap,
                         sizeof test_resource.observed_ap));
    CHECK(!fixture.owner.poisoned && !fixture.fifo.poisoned);
    check_diagnostics(
        "[WiFiLink] g=305419888 u=1 st=12303 rb=1 ra=1 ev=0 lastid=0 k=1 reason=0\n");
    check_no_ap_identity_output();

    fixture_init();
    prepare_secret_hint();
    fixture.mode = FAKE_REMOTE_OTHER;
    diagnostic_reset();
    CHECK(!query());
    check_diagnostics(
        "[WiFiLink] g=305419888 u=1 st=5 rb=1 ra=1 ev=0 lastid=0 k=1 reason=0\n");
    CHECK(fixture.owner.poisoned == 1u && fixture.fifo.poisoned == 1u);
    CHECK(fixture.owner.ids.retired == 1u);
    expect_link_unknown();
    check_no_ap_identity_output();

    fixture_init();
    prepare_secret_hint();
    fixture.mode = FAKE_WRONG_UID;
    diagnostic_reset();
    CHECK(!query());
    check_diagnostics("");
    CHECK(fixture.owner.poisoned == 1u && fixture.fifo.poisoned == 1u);
    CHECK(fixture.owner.ids.retired == 1u);
    expect_link_unknown();
    check_no_ap_identity_output();
}

static void test_malformed_and_wrong_uid_retire(void)
{
    uint8_t hint_ssid[P4_HOSTED_ASSOCIATION_SSID_SIZE] = {0};
    uint8_t hint_bssid[P4_HOSTED_ASSOCIATION_BSSID_SIZE];
    enum fake_exchange_mode modes[] = { FAKE_MALFORMED_AP, FAKE_WRONG_UID };
    size_t index;

    for (index = 0u; index < sizeof modes / sizeof modes[0]; ++index) {
        fixture_init();
        memcpy(hint_ssid, fixture.ap_ssid, fixture.ap_ssid_size);
        memcpy(hint_bssid, fixture.ap_bssid, sizeof hint_bssid);
        prepare_hint(hint_ssid, fixture.ap_ssid_size, hint_bssid, 11u, 3);
        fixture.mode = modes[index];
        memset(&fixture.context.link_snapshot, 0x3c,
               sizeof fixture.context.link_snapshot);
        fixture.context.link_snapshot_matches_hint = TRUE;
        CHECK(!query());
        CHECK(fixture.response_encoded == 1u);
        CHECK(fixture.owner.poisoned == 1u && fixture.fifo.poisoned == 1u);
        CHECK(fixture.owner.ids.retired == 1u);
        CHECK(fixture.owner.ids.active_uid == 0u);
        CHECK(fixture.context.link_snapshot_matches_hint == FALSE);
        CHECK(bytes_are_zero(&fixture.context.link_snapshot,
                             sizeof fixture.context.link_snapshot));
        CHECK(bytes_are_zero(&fixture.control.ap_info,
                             sizeof fixture.control.ap_info));
        expect_link_unknown();
    }
}

static void run_mismatch_case(unsigned int mismatch)
{
    uint8_t hint_ssid[P4_HOSTED_ASSOCIATION_SSID_SIZE] = {0};
    uint8_t hint_bssid[P4_HOSTED_ASSOCIATION_BSSID_SIZE];
    fixture_init();
    memcpy(hint_ssid, fixture.ap_ssid, fixture.ap_ssid_size);
    memcpy(hint_bssid, fixture.ap_bssid, sizeof hint_bssid);
    prepare_hint(hint_ssid, fixture.ap_ssid_size, hint_bssid, 11u, 3);
    if (mismatch == 0u) {
        fixture.ap_ssid[2] ^= 0x80u;
    } else if (mismatch == 1u) {
        fixture.ap_ssid_size--;
    } else if (mismatch == 2u) {
        fixture.ap_bssid[0] ^= 0x80u;
    } else if (mismatch == 3u) {
        fixture.ap_channel++;
    } else {
        fixture.ap_authmode = 0;
    }
    CHECK(query());
    CHECK(fixture.context.link_snapshot_matches_hint == FALSE);
    CHECK(!fixture.owner.poisoned && !fixture.fifo.poisoned);
    CHECK(fixture.owner.ids.active_uid == 0u);
    CHECK(bytes_are_zero(&fixture.control.ap_info,
                         sizeof fixture.control.ap_info));
    expect_observed_ap();
}

static void test_invalid_correlated_ap_retires_owner(void)
{
    uint8_t hint_ssid[P4_HOSTED_ASSOCIATION_SSID_SIZE] = {0};
    uint8_t hint_bssid[P4_HOSTED_ASSOCIATION_BSSID_SIZE];
    unsigned fault;
    for (fault = 1u; fault <= 5u; ++fault) {
        fixture_init();
        memcpy(hint_ssid, fixture.ap_ssid, fixture.ap_ssid_size);
        memcpy(hint_bssid, fixture.ap_bssid, sizeof hint_bssid);
        prepare_hint(hint_ssid, fixture.ap_ssid_size, hint_bssid, 11u, 3);
        fixture.ap_fault = fault;
        CHECK(!query());
        CHECK(fixture.owner.poisoned == 1u && fixture.fifo.poisoned == 1u);
        CHECK(fixture.owner.ids.retired == 1u);
        expect_link_unknown();
    }
}

static void test_mismatch_fields_are_diagnostic_only(void)
{
    unsigned int mismatch;
    for (mismatch = 0u; mismatch < 5u; ++mismatch)
        run_mismatch_case(mismatch);
}

static void test_uid0_hints_and_interleaved_events(void)
{
    uint8_t hint_ssid[P4_HOSTED_ASSOCIATION_SSID_SIZE] = {0};
    uint8_t hint_bssid[P4_HOSTED_ASSOCIATION_BSSID_SIZE];
    struct p4_hosted_inbox_item event_item;
    enum fake_exchange_mode modes[] = {
        FAKE_INTERLEAVE_DOWN, FAKE_INTERLEAVE_UP
    };
    size_t index;

    fixture_init();
    memset(&fixture.control.ap_info, 0x5a, sizeof fixture.control.ap_info);
    CHECK(make_link_event_item(&event_item, 776u, 0x41u));
    CHECK(wifi_owner_event(&fixture.context, &event_item));
    CHECK(fixture.context.link_observation.kind == P4_HOSTED_LINK_DOWN);
    expect_link_unknown(); /* UID0 disconnect hint is not correlated DOWN. */
    CHECK(bytes_are_zero(&test_resource.confirmed_ap,
                         sizeof test_resource.confirmed_ap));
    CHECK(query());
    CHECK(fixture.exchange_calls == 1u);
    /* A contradictory AP snapshot cannot reopen a disconnected link. */
    expect_link_unknown();
    CHECK(fixture.context.link_snapshot_matches_hint == FALSE);
    CHECK(!fixture.owner.poisoned && !fixture.fifo.poisoned);

    fixture_init();
    CHECK(make_link_event_item(&event_item, 775u, 0x61u));
    CHECK(wifi_owner_event(&fixture.context, &event_item));
    CHECK(fixture.context.link_observation.kind == P4_HOSTED_LINK_UP_HINT);
    expect_link_unknown(); /* UID0 connect hint is also only UNKNOWN. */
    fixture.mode = FAKE_REMOTE_300F;
    CHECK(query());
    CHECK(fixture.exchange_calls == 1u);
    CHECK(test_resource.link_state == P4_WIFI_LINK_DOWN);
    CHECK(bytes_are_zero(&test_resource.observed_ap,
                         sizeof test_resource.observed_ap));

    for (index = 0u; index < sizeof modes / sizeof modes[0]; ++index) {
        fixture_init();
        memcpy(hint_ssid, fixture.ap_ssid, fixture.ap_ssid_size);
        memcpy(hint_bssid, fixture.ap_bssid, sizeof hint_bssid);
        prepare_hint(hint_ssid, fixture.ap_ssid_size, hint_bssid, 11u, 3);
        fixture.mode = modes[index];
        memset(&fixture.context.link_snapshot, 0xa7,
               sizeof fixture.context.link_snapshot);
        fixture.context.link_snapshot_matches_hint = TRUE;
        CHECK(query());
        CHECK(fixture.event_dispatches == 1u);
        CHECK(fixture.context.link_observation.revision == 2u);
        if (modes[index] == FAKE_INTERLEAVE_DOWN)
            CHECK(fixture.context.link_observation.kind == P4_HOSTED_LINK_DOWN);
        else
            CHECK(fixture.context.link_observation.kind == P4_HOSTED_LINK_UP_HINT);
        CHECK(fixture.context.link_snapshot_matches_hint == FALSE);
        CHECK(bytes_are_zero(&fixture.context.link_snapshot,
                             sizeof fixture.context.link_snapshot));
        CHECK(bytes_are_zero(&fixture.control.ap_info,
                             sizeof fixture.control.ap_info));
        expect_link_unknown(); /* A raced response cannot overwrite the hint. */
    }
}

static void test_generation_change_and_deadline_wrap(void)
{
    uint8_t hint_ssid[P4_HOSTED_ASSOCIATION_SSID_SIZE] = {0};
    uint8_t hint_bssid[P4_HOSTED_ASSOCIATION_BSSID_SIZE];

    fixture_init();
    fixture.now = 5u;
    CHECK(wifi_owner_remaining(&fixture.context, UINT32_MAX - 10u, 100u) == 84u);
    CHECK(wifi_owner_remaining(&fixture.context, fixture.now - 100u, 100u) == 0u);

    fixture_init();
    fixture.now = UINT32_MAX - 25u;
    memcpy(hint_ssid, fixture.ap_ssid, fixture.ap_ssid_size);
    memcpy(hint_bssid, fixture.ap_bssid, sizeof hint_bssid);
    prepare_hint(hint_ssid, fixture.ap_ssid_size, hint_bssid, 11u, 3);
    fixture.mode = FAKE_LATE_SUCCESS;
    CHECK(!query());
    CHECK(fixture.owner.poisoned == 1u && fixture.fifo.poisoned == 1u);
    CHECK(fixture.owner.ids.retired == 1u);
    CHECK(fixture.context.link_snapshot_matches_hint == FALSE);
    CHECK(bytes_are_zero(&fixture.context.link_snapshot,
                         sizeof fixture.context.link_snapshot));
    CHECK(bytes_are_zero(&fixture.control.ap_info,
                         sizeof fixture.control.ap_info));
    expect_link_unknown();

    fixture_init();
    memcpy(hint_ssid, fixture.ap_ssid, fixture.ap_ssid_size);
    memcpy(hint_bssid, fixture.ap_bssid, sizeof hint_bssid);
    prepare_hint(hint_ssid, fixture.ap_ssid_size, hint_bssid, 11u, 3);
    fixture.mode = FAKE_INTERLEAVE_GENERATION;
    CHECK(query());
    CHECK(fixture.context.generation == UINT32_C(0x12345671));
    CHECK(fixture.context.link_snapshot_matches_hint == FALSE);
    CHECK(bytes_are_zero(&fixture.context.link_snapshot,
                         sizeof fixture.context.link_snapshot));
    CHECK(bytes_are_zero(&fixture.control.ap_info,
                         sizeof fixture.control.ap_info));
    expect_link_unknown();

    fixture_init();
    memcpy(hint_ssid, fixture.ap_ssid, fixture.ap_ssid_size);
    memcpy(hint_bssid, fixture.ap_bssid, sizeof hint_bssid);
    prepare_hint(hint_ssid, fixture.ap_ssid_size, hint_bssid, 11u, 3);
    fixture.mode = FAKE_INTERLEAVE_HOST_EPOCH;
    CHECK(!query());
    CHECK(fixture.context.generation == UINT32_C(0x12345670));
    CHECK(fixture.host.generation == UINT32_C(0x12345671));
    CHECK(fixture.owner.poisoned == 1u && fixture.fifo.poisoned == 1u);
    CHECK(fixture.owner.ids.retired == 1u);
    expect_link_unknown();

}

static void test_station_transition_cannot_reopen_from_snapshot(void)
{
    static const uint32_t kinds[] = {
        P4_HOSTED_LINK_STATION_START, P4_HOSTED_LINK_STATION_STOP,
        P4_HOSTED_LINK_DOWN
    };
    size_t i;
    for (i = 0; i < sizeof(kinds) / sizeof(kinds[0]); ++i) {
        fixture_init();
        fixture.context.link_observation.revision = 1u;
        fixture.context.link_observation.kind = kinds[i];
        CHECK(query());
        CHECK(fixture.exchange_calls == 1u);
        CHECK(fixture.context.link_snapshot_matches_hint == FALSE);
        expect_link_unknown();
        CHECK(!fixture.owner.poisoned && !fixture.fifo.poisoned);
        CHECK(bytes_are_zero(&fixture.control.ap_info,
                             sizeof(fixture.control.ap_info)));
        /* A later AP-only query still cannot authenticate this transition. */
        fixture.now += 1000001u;
        CHECK(query());
        CHECK(fixture.exchange_calls == 2u);
        expect_link_unknown();
        CHECK(fixture.context.link_snapshot_matches_hint == FALSE);
        CHECK(!fixture.owner.poisoned && !fixture.fifo.poisoned);
    }
}

static void test_recheck_during_query_cannot_publish_stale_snapshot(void)
{
    const enum fake_exchange_mode modes[] = {
        FAKE_INTERLEAVE_BEACON, FAKE_INTERLEAVE_CHANNEL
    };
    unsigned i;
    for (i = 0; i < sizeof modes / sizeof modes[0]; ++i) {
        fixture_init();
        diagnostic_reset();
        fixture.ap_authmode = 7;
        prepare_typed_acceptance(P4_WIFI_FULLMAC_SAE_CCMP_PMF, 6);
        fixture.mode = modes[i];
        CHECK(query());
        expect_link_unknown();
        CHECK(!fixture.context.link_snapshot_matches_hint);
        CHECK(fixture.context.link_query_revision !=
              fixture.context.link_observation.revision);
        CHECK(fixture.event_dispatches == 1u);
        CHECK(!fixture.owner.poisoned && !fixture.fifo.poisoned);
        if (modes[i] == FAKE_INTERLEAVE_BEACON)
            check_diagnostics(
                "[WiFiHint] id=21 g=305419888 st=0\n"
                "[WiFiLink] g=305419888 u=1 st=0 rb=1 ra=1 ev=1 lastid=773 k=1 reason=0\n");
        else
            check_diagnostics(
                "[WiFiHint] id=43 g=305419888 st=0\n"
                "[WiFiLink] g=305419888 u=1 st=0 rb=1 ra=1 ev=1 lastid=773 k=1 reason=0\n");
        check_no_ap_identity_output();
        fixture.mode = FAKE_AP_SUCCESS;
        CHECK(query());
        CHECK(fixture.exchange_calls == 2u);
        expect_match();
    }
}

int main(void)
{
    test_exact_request_response_and_dedup();
    test_wifi_rpc_diagnostics();
    test_wifi_rpc_failures_and_suppression();
    test_numeric_diagnostics_preserve_query_decisions();
    test_remote_statuses_down_or_quarantine();
    test_unsubmitted_and_poisoned_timeout();
    test_malformed_and_wrong_uid_retire();
    test_mismatch_fields_are_diagnostic_only();
    test_invalid_correlated_ap_retires_owner();
    test_uid0_hints_and_interleaved_events();
    test_generation_change_and_deadline_wrap();
    test_station_transition_cannot_reopen_from_snapshot();
    test_typed_profile_apinfo_acceptance();
    test_typed_profile_rejections();
    test_typed_acceptance_event_interleaving();
    test_recheck_during_query_cannot_publish_stale_snapshot();
    if (failures != 0u) {
        fprintf(stderr, "wifi-owner-apinfo: %u checks, %u failures\n",
                checks, failures);
        return 1;
    }
    printf("wifi-owner-apinfo: %u checks, 0 failures\n", checks);
    return 0;
}
