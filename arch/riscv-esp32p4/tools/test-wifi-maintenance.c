/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
/* Host model for source-extracted production maintenance/discovery/fence and
 * private OTA owner entry. Exec, OTA codecs/transport, UID reservation and
 * host ownership are modeled; actual codecs/owner have separate suites. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint32_t ULONG;
typedef int32_t LONG;
typedef int BOOL;
typedef unsigned char UBYTE;
typedef void *APTR;

#define TRUE 1
#define FALSE 0

#include "owner_ota.h"
#include "wifi_maintenance_constants.inc"

struct Task {
    unsigned identity;
};

struct SignalSemaphore {
    unsigned held;
    struct Task *ss_Owner;
};

struct p4_wifi_link_info {
    UBYTE ssid[33];
    ULONG ssid_length;
    UBYTE bssid[6];
    ULONG channel;
    ULONG authmode;
};

struct p4_wifi_device_callbacks {
    void (*ready)(APTR, const UBYTE *);
    void (*stopped)(APTR);
};

struct p4_wifi_discovery_result {
    ULONG token, generation, uid, nonce;
    LONG remote_status;
    ULONG inventory_size;
    UBYTE inventory[P4_WIFI_DISCOVERY_MAX];
};

struct p4_hosted_init {
    unsigned discovery_supported;
    uint8_t chip_id;
    uint8_t capabilities;
    uint8_t present;
};

struct board_discovery_snapshot {
    unsigned marker;
};

struct p4_hosted_discovery_result {
    int32_t status;
    struct board_discovery_snapshot snapshot;
};

struct p4_hosted_request_ids {
    ULONG generation;
    ULONG active_uid;
    ULONG active_request_id;
    ULONG next_uid;
};

struct p4_hosted_owner_result {
    unsigned submitted;
};

struct p4_hosted_owner_ops {
    unsigned unused;
};

struct p4_hosted_tx_ops {
    unsigned unused;
};

struct p4_hosted_inbox_item {
    uint32_t msg_type;
    uint32_t msg_id;
    uint32_t uid;
    const uint8_t *payload;
    size_t payload_size;
};

struct p4_hosted_rpc {
    uint32_t msg_type;
    uint32_t msg_id;
    uint32_t uid;
    const uint8_t *payload;
    size_t payload_size;
};

struct p4_hosted_owner {
    unsigned poisoned;
    ULONG generation;
    unsigned pending_active;
    struct p4_hosted_request_ids ids;
    unsigned partial_active;
    struct p4_hosted_inbox_item reply;
};

struct p4_hosted_fifo {
    unsigned poisoned;
    ULONG generation;
    unsigned busy;
};

struct wifi_owner_control {
    struct p4_hosted_owner_result result;
    struct p4_hosted_owner_ops ops;
    ULONG discovery_state, discovery_nonce, discovery_token;
    ULONG ota_token, ota_generation, ota_image_size;
    ULONG ota_acknowledged_bytes;
    unsigned ota_begin_acknowledged, ota_closed;
    struct p4_wifi_discovery_result discovery_result;
    struct p4_hosted_discovery_result discovery_decoded;
    uint8_t request[256];
    uint8_t discovery_response[P4_HOSTED_RPC_MAX_SIZE];
};

struct P4SDMMCHost {
    ULONG (*now_us)(void);
    ULONG generation;
    BOOL dma_faulted;
    struct SignalSemaphore lock;
};

struct wifi_probe_context {
    struct P4SDMMCHost *host;
    ULONG generation;
};

struct wifi_device_runtime {
    struct wifi_probe_context *context;
    struct p4_hosted_owner *owner;
    struct wifi_owner_control *control;
    struct p4_hosted_fifo *fifo;
    const struct p4_hosted_tx_ops *tx;
    unsigned checksum;
    const struct p4_hosted_init *init;
    unsigned scanning;
};

struct P4WifiBase {
    BOOL ready;
    BOOL associated;
    BOOL association_enabled;
    struct p4_wifi_link_info confirmed_ap;
    ULONG link_state;
    struct p4_wifi_link_info observed_ap;
    struct SignalSemaphore client_lock;
    APTR client;
    const struct p4_wifi_device_callbacks *callbacks;
    UBYTE mac[6];
    struct Task *owner_task;
    struct wifi_device_runtime *runtime;
    struct Task *maintenance_task;
    ULONG maintenance_token;
    ULONG maintenance_state;
    ULONG maintenance_started;
    ULONG maintenance_fence_mode;
};

static struct Task owner_task = {1u};
static struct Task requester_task = {2u};
static struct Task other_task = {3u};
static struct Task *current_task = &requester_task;
static ULONG fake_now;
static unsigned now_calls;
static unsigned request_calls;
static uint32_t request_ids[4];
static ULONG request_budgets[4];
static BOOL request_succeeds = TRUE;
static ULONG request_advance_us;
static unsigned stopped_calls;
static unsigned ready_calls;
static ULONG ready_advance_us;
static unsigned test_checks;
static BOOL discovery_exchange_succeeds = TRUE;
static BOOL discovery_request_succeeds = TRUE;
static BOOL discovery_rpc_encode_succeeds = TRUE;
static BOOL discovery_response_succeeds = TRUE;
static BOOL discovery_inventory_succeeds = TRUE;
static BOOL discovery_acquire_succeeds = TRUE;
static BOOL discovery_tx_session_succeeds = TRUE;
static BOOL discovery_stopping;
static BOOL discovery_change_host_generation_on_acquire;
static unsigned discovery_supported_calls;
static unsigned discovery_reserve_calls;
static unsigned discovery_request_calls;
static unsigned discovery_exchange_calls;
static unsigned discovery_rpc_encode_calls;
static unsigned discovery_response_calls;
static unsigned discovery_inventory_calls;
static unsigned discovery_acquire_calls;
static unsigned discovery_release_calls;
static unsigned tx_session_calls;
static unsigned discovery_cancel_calls;
static unsigned discovery_unsubmitted_calls;
static unsigned discovery_stopping_calls;
static unsigned discovery_exchange_submitted;
static unsigned discovery_reply_id_delta;
static uint32_t discovery_reply_uid_delta;
static uint32_t discovery_reply_nonce_delta;
static LONG discovery_remote_status;
static ULONG discovery_request_advance_us;
static ULONG discovery_exchange_advance_us;
static ULONG discovery_acquire_advance_us;
static ULONG discovery_release_advance_us;
static ULONG discovery_exchange_timeout;
static ULONG discovery_acquire_wait;
static BOOL finish_stopping_probe;
static unsigned finish_stopping_unlocked_calls;
static unsigned finish_stopping_locked_calls;
static unsigned finish_no_begin_clear_observations;
static unsigned finish_fence_release_observations;
static ULONG discovery_wire_uid;
static ULONG discovery_wire_nonce;
static ULONG discovery_wire_request_id;
static ULONG discovery_wire_generation;
static ULONG discovery_wire_checksum;

struct ota_exchange_observation {
    uint32_t request_id, uid, generation, timeout_us, response_type;
    uint32_t response_id, response_uid;
    size_t request_size, data_size;
    uint8_t data[P4_BOARD_UPDATE_CHUNK_SIZE];
};

static BOOL ota_reserve_succeeds = TRUE;
static BOOL ota_encode_succeeds = TRUE;
static BOOL ota_exchange_succeeds = TRUE;
static BOOL ota_status_succeeds = TRUE;
static unsigned ota_reserve_calls;
static unsigned ota_encode_calls;
static unsigned ota_exchange_calls;
static unsigned ota_status_calls;
static unsigned ota_unsubmitted_calls;
static unsigned ota_exchange_submitted;
static unsigned ota_response_type_delta;
static unsigned ota_response_id_delta;
static uint32_t ota_response_uid_delta;
static ULONG ota_encode_advance_us;
static ULONG ota_exchange_advance_us;
static LONG ota_remote_status;
static uint32_t ota_encoded_request_id;
static uint32_t ota_encoded_uid;
static uint8_t ota_status_payload[1];
static struct ota_exchange_observation ota_observations[16];
static unsigned ota_observation_count;

#define TEST_LEASE_US ((ULONG)P4_WIFI_MAINT_LEASE_US)
#define TEST_GENERATION UINT32_C(0x23a1)
#define TEST_TOKEN UINT32_C(17)

#define CHECK(expression) do { \
    ++test_checks; \
    if (!(expression)) { \
        fprintf(stderr, "check failed at %s:%d: %s\n", __FILE__, __LINE__, \
                #expression); \
        exit(1); \
    } \
} while (0)

static ULONG fake_now_us(void)
{
    ++now_calls;
    return fake_now;
}

static struct Task *FindTask(APTR ignored)
{
    (void)ignored;
    return current_task;
}

static BOOL AttemptSemaphore(struct SignalSemaphore *semaphore)
{
    if (semaphore->held)
        return FALSE;
    semaphore->held = 1u;
    semaphore->ss_Owner = current_task;
    return TRUE;
}

static void ReleaseSemaphore(struct SignalSemaphore *semaphore)
{
    semaphore->held = 0u;
    semaphore->ss_Owner = NULL;
}

static void p4host_zero(void *memory, size_t size)
{
    memset(memory, 0, size);
}

static BOOL bytes_are_zero(const void *memory, size_t size);

static void CopyMem(const void *source, void *destination, ULONG size)
{
    memmove(destination, source, size);
}

static void mock_stopped(APTR client)
{
    CHECK(client != NULL);
    ++stopped_calls;
}

static void mock_ready(APTR client, const UBYTE *mac)
{
    CHECK(client != NULL);
    CHECK(mac != NULL);
    ++ready_calls;
    fake_now += ready_advance_us;
}

static const struct p4_wifi_device_callbacks callbacks = {
    mock_ready,
    mock_stopped
};

static struct P4WifiBase base;
/* Models the registered resource instance required by production entrypoints. */
static struct P4WifiBase *wifi_resource_base;
static struct P4SDMMCHost host;
static struct wifi_probe_context context;
static struct p4_hosted_owner owner;
static struct p4_hosted_fifo fifo;
static struct wifi_owner_control control;
static struct wifi_device_runtime runtime;
static struct p4_hosted_tx_ops tx_ops;
static struct p4_hosted_init init;
static const UBYTE discovery_payload[] = {0x5au, 0xa5u};
static int client_cookie;

/* This is the only mocked production dependency. A successful result stands
 * for a correlated command response; IDs and lease budgets remain observable. */
static int wifi_owner_request(struct wifi_probe_context *request_context,
    struct p4_hosted_owner *request_owner,
    struct wifi_owner_control *request_control,
    struct p4_hosted_fifo *request_fifo, unsigned checksum, uint32_t id,
    ULONG remaining)
{
    unsigned slot = request_calls;
    CHECK(request_context == &context);
    CHECK(request_owner == &owner);
    CHECK(request_control == &control);
    CHECK(request_fifo == &fifo);
    CHECK(checksum == runtime.checksum);
    if (finish_stopping_probe && id == 280u) {
        CHECK(base.maintenance_state == P4_WIFI_MAINT_RELEASING);
        CHECK(control.discovery_state == P4_WIFI_DISCOVERY_IDLE);
        CHECK(control.discovery_token == 0u);
        CHECK(bytes_are_zero(&control.discovery_result,
                             sizeof(control.discovery_result)));
        CHECK(bytes_are_zero(&control.discovery_decoded,
                             sizeof(control.discovery_decoded)));
        CHECK(bytes_are_zero(control.discovery_response,
                             sizeof(control.discovery_response)));
        ++finish_no_begin_clear_observations;
    }
    if (slot < sizeof(request_ids) / sizeof(request_ids[0])) {
        request_ids[slot] = id;
        request_budgets[slot] = remaining;
    }
    ++request_calls;
    fake_now += request_advance_us;
    return request_succeeds ? 1 : 0;
}

static void reset_ota_mocks(void)
{
    ota_reserve_succeeds = TRUE;
    ota_encode_succeeds = TRUE;
    ota_exchange_succeeds = TRUE;
    ota_status_succeeds = TRUE;
    ota_reserve_calls = 0u;
    ota_encode_calls = 0u;
    ota_exchange_calls = 0u;
    ota_status_calls = 0u;
    ota_unsubmitted_calls = 0u;
    ota_exchange_submitted = 0u;
    ota_response_type_delta = 0u;
    ota_response_id_delta = 0u;
    ota_response_uid_delta = 0u;
    ota_encode_advance_us = 0u;
    ota_exchange_advance_us = 0u;
    ota_remote_status = 0;
    ota_encoded_request_id = 0u;
    ota_encoded_uid = 0u;
    memset(ota_status_payload, 0, sizeof(ota_status_payload));
    memset(ota_observations, 0, sizeof(ota_observations));
    ota_observation_count = 0u;
}

static int ota_encode_request(uint32_t request_id, uint32_t uid,
    const uint8_t *data, size_t data_size, uint8_t *bytes, size_t capacity,
    size_t *written)
{
    ++ota_encode_calls;
    CHECK(bytes == control.request);
    CHECK(capacity == sizeof(control.request));
    CHECK(written != NULL);
    CHECK(uid != 0u && uid == owner.ids.active_uid);
    CHECK(request_id >= 272u && request_id <= 274u);
    if (request_id == 273u)
        CHECK(data != NULL && data_size != 0u &&
              data_size <= P4_HOSTED_OTA_MAX_WRITE_SIZE);
    else
        CHECK(data == NULL && data_size == 0u);
    ota_encoded_request_id = request_id;
    ota_encoded_uid = uid;
    fake_now += ota_encode_advance_us;
    if (!ota_encode_succeeds || capacity < sizeof(request_id) + sizeof(uid) +
                                      data_size)
        return 0;
    memcpy(bytes, &request_id, sizeof(request_id));
    memcpy(bytes + sizeof(request_id), &uid, sizeof(uid));
    if (data_size) memcpy(bytes + sizeof(request_id) + sizeof(uid), data,
                          data_size);
    *written = sizeof(request_id) + sizeof(uid) + data_size;
    return 1;
}

static int p4_hosted_ota_begin_request(uint32_t uid, uint8_t *bytes,
    size_t capacity, size_t *written)
{
    return ota_encode_request(272u, uid, NULL, 0u, bytes, capacity, written);
}

static int p4_hosted_ota_write_request(uint32_t uid, const uint8_t *data,
    size_t data_size, uint8_t *bytes, size_t capacity, size_t *written)
{
    return ota_encode_request(273u, uid, data, data_size, bytes, capacity,
                              written);
}

static int p4_hosted_ota_end_request(uint32_t uid, uint8_t *bytes,
    size_t capacity, size_t *written)
{
    return ota_encode_request(274u, uid, NULL, 0u, bytes, capacity, written);
}

static int p4_hosted_owner_exchange_ota(struct p4_hosted_owner *request_owner,
    struct p4_hosted_fifo *request_fifo,
    const struct p4_hosted_owner_ops *ops, uint32_t generation,
    unsigned checksum, const uint8_t *request, size_t request_size,
    uint32_t timeout_us, struct p4_hosted_owner_result *result)
{
    struct ota_exchange_observation *observation;
    uint32_t encoded_id, encoded_uid;
    ++ota_exchange_calls;
    CHECK(ota_observation_count <
          sizeof(ota_observations) / sizeof(ota_observations[0]));
    observation = &ota_observations[ota_observation_count++];
    CHECK(request_owner == &owner);
    CHECK(request_fifo == &fifo);
    CHECK(ops == &control.ops);
    CHECK(generation == context.generation);
    CHECK(checksum == runtime.checksum);
    CHECK(request == control.request);
    CHECK(result == &control.result);
    CHECK(host.lock.ss_Owner != current_task);
    CHECK(request_size >= sizeof(encoded_id) + sizeof(encoded_uid));
    memcpy(&encoded_id, request, sizeof(encoded_id));
    memcpy(&encoded_uid, request + sizeof(encoded_id), sizeof(encoded_uid));
    CHECK(encoded_id == ota_encoded_request_id);
    CHECK(encoded_uid == ota_encoded_uid);
    CHECK(owner.ids.active_uid == encoded_uid);
    CHECK(owner.ids.active_request_id == encoded_id);
    CHECK(timeout_us != 0u && timeout_us <= P4_HOSTED_OWNER_MAX_EXCHANGE_US);
    observation->request_id = encoded_id;
    observation->uid = encoded_uid;
    observation->generation = generation;
    observation->timeout_us = timeout_us;
    observation->request_size = request_size;
    observation->data_size = request_size - sizeof(encoded_id) -
        sizeof(encoded_uid);
    CHECK(observation->data_size <= sizeof(observation->data));
    if (observation->data_size)
        memcpy(observation->data, request + sizeof(encoded_id) +
               sizeof(encoded_uid), observation->data_size);
    result->submitted = ota_exchange_submitted;
    fake_now += ota_exchange_advance_us;
    if (!ota_exchange_succeeds) {
        if (ota_exchange_submitted) {
            owner.ids.active_uid = 0u;
            owner.ids.active_request_id = 0u;
            owner.poisoned = 1u;
            fifo.poisoned = 1u;
        }
        return 0;
    }
    observation->response_id = encoded_id + 256u + ota_response_id_delta;
    observation->response_uid = encoded_uid + ota_response_uid_delta;
    observation->response_type = 2u + ota_response_type_delta;
    owner.reply.msg_type = observation->response_type;
    owner.reply.msg_id = observation->response_id;
    owner.reply.uid = observation->response_uid;
    owner.reply.payload = ota_status_payload;
    owner.reply.payload_size = sizeof(ota_status_payload);
    owner.ids.active_uid = 0u;
    owner.ids.active_request_id = 0u;
    return 1;
}

static int p4_hosted_ota_status_response(const struct p4_hosted_rpc *rpc,
    uint32_t expected_request_id, uint32_t expected_uid, int32_t *status)
{
    struct ota_exchange_observation *observation;
    ++ota_status_calls;
    CHECK(ota_observation_count != 0u);
    observation = &ota_observations[ota_observation_count - 1u];
    CHECK(rpc != NULL && status != NULL);
    CHECK(expected_request_id == observation->request_id);
    CHECK(expected_uid == observation->uid);
    CHECK(rpc->msg_type == observation->response_type);
    CHECK(rpc->msg_id == observation->response_id);
    CHECK(rpc->uid == observation->response_uid);
    CHECK(rpc->payload == ota_status_payload);
    CHECK(rpc->payload_size == sizeof(ota_status_payload));
    if (!ota_status_succeeds || rpc->msg_type != 2u ||
        rpc->msg_id != expected_request_id + 256u ||
        rpc->uid != expected_uid)
        return 0;
    *status = ota_remote_status;
    return 1;
}

/* ABI8 discovery model boundary: the production runtime admission, job state,
 * response publication and polling helpers are source-extracted below. The
 * private owner exchange, UID ledger, protobuf envelopes and canonical board
 * encoder are mocked here; their actual codec/owner suites run separately. */
static int p4_hosted_init_has_board_discovery(const struct p4_hosted_init *value)
{
    ++discovery_supported_calls;
    CHECK(value == &init);
    return value->discovery_supported != 0u;
}

static int p4_hosted_request_ids_reserve(struct p4_hosted_request_ids *ids,
    uint32_t generation, uint32_t request_id, uint32_t *uid)
{
    CHECK(ids == &owner.ids);
    CHECK(request_id == P4_HOSTED_DISCOVERY_REQUEST_ID ||
          (request_id >= 272u && request_id <= 274u));
    if (request_id == P4_HOSTED_DISCOVERY_REQUEST_ID)
        ++discovery_reserve_calls;
    else {
        ++ota_reserve_calls;
        if (!ota_reserve_succeeds) return 0;
    }
    if (generation != TEST_GENERATION) return 0;
    if (ids->generation != generation || ids->active_uid || !uid ||
        !ids->next_uid)
        return 0;
    ids->active_uid = ids->next_uid++;
    ids->active_request_id = request_id;
    *uid = ids->active_uid;
    return 1;
}

static int p4_hosted_discovery_request(const struct p4_hosted_init *value,
    uint32_t uid, uint32_t nonce, uint8_t *bytes, size_t capacity,
    size_t *written)
{
    size_t i;
    ++discovery_request_calls;
    CHECK(value == runtime.init);
    CHECK(value == &init);
    CHECK(uid != 0u && uid == owner.ids.active_uid);
    CHECK(nonce == control.discovery_nonce);
    CHECK(bytes == control.request);
    CHECK(capacity >= 13u);
    CHECK(written != NULL);
    if (!discovery_request_succeeds) return 0;
    bytes[0] = 0x35u;
    for (i = 0; i < 4u; ++i) {
        bytes[1u + i] = (uint8_t)(uid >> (i * 8u));
        bytes[5u + i] = (uint8_t)(nonce >> (i * 8u));
    }
    for (i = 0; i < 4u; ++i)
        bytes[9u + i] = (uint8_t)(context.generation >> (i * 8u));
    *written = 13u;
    fake_now += discovery_request_advance_us;
    return 1;
}

static int p4_hosted_owner_exchange_board_discovery(
    struct p4_hosted_owner *request_owner, struct p4_hosted_fifo *request_fifo,
    const struct p4_hosted_owner_ops *ops, uint32_t generation,
    unsigned checksum, const uint8_t *request, size_t request_size,
    uint32_t timeout_us, struct p4_hosted_owner_result *result,
    const struct p4_hosted_init *value, uint32_t nonce)
{
    size_t i;
    uint32_t request_uid = 0, request_nonce = 0, request_generation = 0;
    ++discovery_exchange_calls;
    CHECK(request_owner == &owner);
    CHECK(request_fifo == &fifo);
    CHECK(ops == &control.ops);
    CHECK(generation == context.generation);
    CHECK(checksum == runtime.checksum);
    CHECK(request == control.request);
    CHECK(request_size == 13u);
    CHECK(result == &control.result);
    CHECK(value == runtime.init);
    CHECK(value == &init);
    CHECK(nonce == control.discovery_nonce);
    CHECK(timeout_us > 0u && timeout_us <= 8000000u);
    CHECK(request[0] == 0x35u);
    for (i = 0; i < 4u; ++i) {
        request_uid |= (uint32_t)request[1u + i] << (i * 8u);
        request_nonce |= (uint32_t)request[5u + i] << (i * 8u);
        request_generation |= (uint32_t)request[9u + i] << (i * 8u);
    }
    CHECK(request_uid == owner.ids.active_uid);
    CHECK(request_nonce == nonce);
    CHECK(request_generation == generation);
    discovery_exchange_timeout = timeout_us;
    discovery_wire_uid = request_uid + discovery_reply_uid_delta;
    discovery_wire_nonce = request_nonce + discovery_reply_nonce_delta;
    discovery_wire_request_id = 606u + discovery_reply_id_delta;
    discovery_wire_generation = generation;
    discovery_wire_checksum = checksum;
    result->submitted = discovery_exchange_submitted;
    fake_now += discovery_exchange_advance_us;
    if (!discovery_exchange_succeeds) return 0;
    owner.reply.msg_type = 2u;
    owner.reply.msg_id = discovery_wire_request_id;
    owner.reply.uid = discovery_wire_uid;
    owner.reply.payload = discovery_payload;
    owner.reply.payload_size = sizeof(discovery_payload);
    owner.ids.active_uid = 0u;
    owner.ids.active_request_id = 0u;
    return 1;
}

static int p4_hosted_rpc_encode_board_discovery(uint8_t *bytes,
    size_t capacity, const struct p4_hosted_rpc *rpc, size_t *written)
{
    ++discovery_rpc_encode_calls;
    CHECK(bytes == control.discovery_response);
    CHECK(capacity == sizeof(control.discovery_response));
    CHECK(rpc != NULL && rpc->payload == discovery_payload);
    CHECK(rpc->payload_size == sizeof(discovery_payload));
    if (!discovery_rpc_encode_succeeds) return 0;
    bytes[0] = (uint8_t)rpc->msg_type;
    bytes[1] = (uint8_t)rpc->msg_id;
    bytes[2] = (uint8_t)rpc->uid;
    *written = 3u;
    discovery_wire_uid = rpc->uid;
    discovery_wire_request_id = rpc->msg_id;
    return 1;
}

static int p4_hosted_discovery_response(const uint8_t *bytes, size_t size,
    uint32_t uid, uint32_t nonce, struct p4_hosted_discovery_result *result)
{
    ++discovery_response_calls;
    CHECK(bytes == control.discovery_response);
    CHECK(size == 3u);
    CHECK(result == &control.discovery_decoded);
    if (!discovery_response_succeeds || bytes[0] != 2u || bytes[1] != 94u ||
        bytes[2] != (uint8_t)uid || discovery_wire_uid != uid ||
        discovery_wire_nonce != nonce || discovery_wire_request_id != 606u)
        return 0;
    result->status = discovery_remote_status;
    result->snapshot.marker = (unsigned)(nonce ^ 0x5a5a5a5au);
    return 1;
}

static int board_discovery_encode(const struct board_discovery_snapshot *snapshot,
    uint32_t nonce, uint8_t *bytes, size_t capacity, size_t *written)
{
    size_t i;
    ++discovery_inventory_calls;
    CHECK(snapshot == &control.discovery_decoded.snapshot);
    CHECK(nonce == control.discovery_nonce);
    CHECK(bytes == control.discovery_result.inventory);
    CHECK(capacity == sizeof(control.discovery_result.inventory));
    if (!discovery_inventory_succeeds) return 0;
    for (i = 0; i < 48u; ++i)
        bytes[i] = (uint8_t)(snapshot->marker + (unsigned)i);
    *written = 48u;
    return 1;
}

static int wifi_owner_finish_unsubmitted_reservation(
    struct p4_hosted_owner *request_owner,
    struct p4_hosted_fifo *request_fifo, uint32_t generation,
    uint32_t uid, uint32_t request_id, unsigned submitted)
{
    CHECK(request_owner == &owner);
    CHECK(request_fifo == &fifo);
    CHECK(generation == context.generation);
    if (request_id == P4_HOSTED_DISCOVERY_REQUEST_ID)
        ++discovery_unsubmitted_calls;
    else {
        CHECK(request_id >= 272u && request_id <= 274u);
        ++ota_unsubmitted_calls;
    }
    if (submitted || owner.ids.generation != generation ||
        owner.ids.active_uid != uid || owner.ids.active_request_id != request_id)
        return 0;
    owner.ids.active_uid = 0u;
    owner.ids.active_request_id = 0u;
    return 1;
}

static int p4_hosted_owner_cancel(struct p4_hosted_owner *request_owner,
    struct p4_hosted_fifo *request_fifo, uint32_t generation)
{
    ++discovery_cancel_calls;
    CHECK(request_owner == &owner);
    CHECK(request_fifo == &fifo);
    CHECK(generation == context.generation);
    owner.ids.active_uid = 0u;
    owner.ids.active_request_id = 0u;
    owner.poisoned = 1u;
    fifo.poisoned = 1u;
    return 1;
}

static ULONG wifi_owner_now(void *opaque)
{
    CHECK(opaque == &context);
    return fake_now;
}

static int wifi_owner_acquire(void *opaque, uint32_t wait_us)
{
    CHECK(opaque == &context);
    CHECK(current_task == &owner_task);
    CHECK(wait_us != 0u && wait_us <= P4_HOSTED_OWNER_MAX_ACQUIRE_US);
    CHECK(host.lock.ss_Owner == NULL);
    ++discovery_acquire_calls;
    discovery_acquire_wait = wait_us;
    fake_now += discovery_acquire_advance_us;
    if (!discovery_acquire_succeeds) return 0;
    host.lock.ss_Owner = current_task;
    if (discovery_change_host_generation_on_acquire)
        ++host.generation;
    return 1;
}

static void wifi_owner_release(void *opaque)
{
    CHECK(opaque == &context);
    CHECK(host.lock.ss_Owner == current_task);
    if (finish_stopping_probe &&
        base.maintenance_state == P4_WIFI_MAINT_FENCED) {
        CHECK(base.ready == FALSE);
        ++finish_fence_release_observations;
    }
    ++discovery_release_calls;
    host.lock.ss_Owner = NULL;
    fake_now += discovery_release_advance_us;
}

static int wifi_tx_session(void *opaque, uint32_t generation)
{
    ++tx_session_calls;
    CHECK(opaque == &context);
    return discovery_tx_session_succeeds && generation == context.generation &&
        host.generation == generation && host.lock.ss_Owner == current_task;
}

static int wifi_owner_stopping(void *opaque)
{
    CHECK(opaque == &context);
    if (finish_stopping_probe) {
        if (host.lock.ss_Owner == current_task)
            ++finish_stopping_locked_calls;
        else
            ++finish_stopping_unlocked_calls;
    } else {
        CHECK(host.lock.ss_Owner == current_task);
    }
    ++discovery_stopping_calls;
    return discovery_stopping ? 1 : 0;
}

#ifdef WIFI_MAINTENANCE_CLEANUP_MUTANT
#include "wifi_maintenance_cleanup_mutant.inc"
#elif defined(WIFI_MAINTENANCE_MUTANT)
#include "wifi_maintenance_mutant.inc"
#elif defined(WIFI_OTA_PREBEGIN_MUTANT)
#include "wifi_maintenance_ota_mutant.inc"
#elif defined(WIFI_OTA_FINISH_CAST_MUTANT)
#include "wifi_maintenance_finish_mutant.inc"
#else
#include "wifi_maintenance_extract.inc"
#endif

static void reset_discovery_mocks(void)
{
    reset_ota_mocks();
    discovery_exchange_succeeds = TRUE;
    discovery_request_succeeds = TRUE;
    discovery_rpc_encode_succeeds = TRUE;
    discovery_response_succeeds = TRUE;
    discovery_inventory_succeeds = TRUE;
    discovery_acquire_succeeds = TRUE;
    discovery_tx_session_succeeds = TRUE;
    discovery_stopping = FALSE;
    discovery_change_host_generation_on_acquire = FALSE;
    discovery_supported_calls = 0u;
    discovery_reserve_calls = 0u;
    discovery_request_calls = 0u;
    discovery_exchange_calls = 0u;
    discovery_rpc_encode_calls = 0u;
    discovery_response_calls = 0u;
    discovery_inventory_calls = 0u;
    discovery_acquire_calls = 0u;
    discovery_release_calls = 0u;
    tx_session_calls = 0u;
    discovery_cancel_calls = 0u;
    discovery_unsubmitted_calls = 0u;
    discovery_stopping_calls = 0u;
    discovery_exchange_submitted = 0u;
    discovery_reply_id_delta = 0u;
    discovery_reply_uid_delta = 0u;
    discovery_reply_nonce_delta = 0u;
    discovery_remote_status = 0;
    discovery_request_advance_us = 0u;
    discovery_exchange_advance_us = 0u;
    discovery_acquire_advance_us = 0u;
    discovery_release_advance_us = 0u;
    discovery_exchange_timeout = 0u;
    discovery_acquire_wait = 0u;
    finish_stopping_probe = FALSE;
    finish_stopping_unlocked_calls = 0u;
    finish_stopping_locked_calls = 0u;
    finish_no_begin_clear_observations = 0u;
    finish_fence_release_observations = 0u;
    discovery_wire_uid = 0u;
    discovery_wire_nonce = 0u;
    discovery_wire_request_id = 0u;
    discovery_wire_generation = 0u;
    discovery_wire_checksum = 0u;
    init.discovery_supported = 1u;
    init.chip_id = P4_HOSTED_INIT_CHIP_ESP32C6;
    init.capabilities = P4_HOSTED_INIT_CAP_WLAN_SDIO;
    init.present = P4_HOSTED_INIT_HAS_CHIP_ID |
        P4_HOSTED_INIT_HAS_CAPABILITIES;
}

static int service_step(void)
{
    struct Task *saved_task = current_task;
    struct Task *saved_lock_owner = base.client_lock.ss_Owner;
    unsigned saved_lock_held = base.client_lock.held;
    int result;
    current_task = &owner_task;
    base.client_lock.held = 1u;
    base.client_lock.ss_Owner = &owner_task;
    result = wifi_maintenance_service(&base, &runtime);
    base.client_lock.held = saved_lock_held;
    base.client_lock.ss_Owner = saved_lock_owner;
    current_task = saved_task;
    return result;
}

static void reset_fixture(void)
{
    memset(&base, 0, sizeof(base));
    wifi_resource_base = &base;
    memset(&host, 0, sizeof(host));
    memset(&context, 0, sizeof(context));
    memset(&owner, 0, sizeof(owner));
    memset(&fifo, 0, sizeof(fifo));
    memset(&control, 0, sizeof(control));
    memset(&runtime, 0, sizeof(runtime));
    reset_discovery_mocks();
    current_task = &requester_task;
    fake_now = 1000u;
    now_calls = 0u;
    request_calls = 0u;
    memset(request_ids, 0, sizeof(request_ids));
    memset(request_budgets, 0, sizeof(request_budgets));
    request_succeeds = TRUE;
    request_advance_us = 0u;
    stopped_calls = 0u;
    ready_calls = 0u;
    ready_advance_us = 0u;

    host.now_us = fake_now_us;
    context.host = &host;
    context.generation = TEST_GENERATION;
    host.generation = TEST_GENERATION;
    owner.generation = TEST_GENERATION;
    owner.ids.generation = TEST_GENERATION;
    owner.ids.next_uid = 100u;
    fifo.generation = TEST_GENERATION;
    runtime.context = &context;
    runtime.owner = &owner;
    runtime.control = &control;
    runtime.fifo = &fifo;
    runtime.tx = &tx_ops;
    runtime.checksum = 1u;
    runtime.init = &init;
    base.ready = TRUE;
    base.association_enabled = TRUE;
    base.owner_task = &owner_task;
    base.runtime = &runtime;
    base.client = &client_cookie;
    base.callbacks = &callbacks;
    base.maintenance_token = TEST_TOKEN;
    base.maintenance_task = &requester_task;
    base.maintenance_started = fake_now;
}

static const struct p4_board_update_gates valid_ota_gates = {
    1u, 1u, 1u, 1u
};

static void prepare_ota_active(void)
{
    reset_fixture();
    current_task = &owner_task;
    base.client_lock.held = 1u;
    base.client_lock.ss_Owner = current_task;
    base.maintenance_state = P4_WIFI_MAINT_ACTIVE;
    base.maintenance_task = &requester_task;
    base.maintenance_started = fake_now;
    base.associated = TRUE;
}

static void prepare_finish_active(void)
{
    prepare_ota_active();
    finish_stopping_probe = TRUE;
}

static int call_finish(enum p4_board_update_finish_mode mode,
    uint32_t timeout_us, uint32_t token, uint32_t generation)
{
    return p4_wifi_owner_ota_finish(&base, token, generation, mode,
                                    timeout_us);
}

static enum p4_board_update_call_result call_ota(uint32_t request_id,
    const uint8_t *data, size_t data_size, uint32_t image_size,
    const struct p4_board_update_gates *gates, uint32_t timeout_us,
    int32_t *remote_status)
{
    return p4_wifi_owner_ota_exchange(&base, TEST_TOKEN, TEST_GENERATION,
        request_id, data, data_size, image_size, gates, timeout_us,
        remote_status);
}

static void expect_ota_preflight_refusal(uint32_t request_id,
    const uint8_t *data, size_t data_size, uint32_t image_size,
    const struct p4_board_update_gates *gates, uint32_t timeout_us,
    uint32_t token, uint32_t generation)
{
    struct P4WifiBase base_before;
    struct P4SDMMCHost host_before;
    struct wifi_probe_context context_before;
    struct p4_hosted_owner owner_before;
    struct p4_hosted_fifo fifo_before;
    struct wifi_owner_control control_before;
    struct wifi_device_runtime runtime_before;
    const unsigned reserve_before = ota_reserve_calls;
    const unsigned encode_before = ota_encode_calls;
    const unsigned exchange_before = ota_exchange_calls;
    const unsigned status_before = ota_status_calls;
    const unsigned unsubmitted_before = ota_unsubmitted_calls;
    const unsigned observed_before = ota_observation_count;
    int32_t status = INT32_C(0x13572468);
    memcpy(&base_before, &base, sizeof(base_before));
    memcpy(&host_before, &host, sizeof(host_before));
    memcpy(&context_before, &context, sizeof(context_before));
    memcpy(&owner_before, &owner, sizeof(owner_before));
    memcpy(&fifo_before, &fifo, sizeof(fifo_before));
    memcpy(&control_before, &control, sizeof(control_before));
    memcpy(&runtime_before, &runtime, sizeof(runtime_before));
    CHECK(p4_wifi_owner_ota_exchange(&base, token, generation, request_id,
        data, data_size, image_size, gates, timeout_us, &status) ==
        P4_BOARD_UPDATE_CALL_AMBIGUOUS);
    CHECK(status == INT32_C(0x13572468));
    CHECK(memcmp(&base_before, &base, sizeof(base_before)) == 0);
    CHECK(memcmp(&host_before, &host, sizeof(host_before)) == 0);
    CHECK(memcmp(&context_before, &context, sizeof(context_before)) == 0);
    CHECK(memcmp(&owner_before, &owner, sizeof(owner_before)) == 0);
    CHECK(memcmp(&fifo_before, &fifo, sizeof(fifo_before)) == 0);
    CHECK(memcmp(&control_before, &control, sizeof(control_before)) == 0);
    CHECK(memcmp(&runtime_before, &runtime, sizeof(runtime_before)) == 0);
    CHECK(ota_reserve_calls == reserve_before);
    CHECK(ota_encode_calls == encode_before);
    CHECK(ota_exchange_calls == exchange_before);
    CHECK(ota_status_calls == status_before);
    CHECK(ota_unsubmitted_calls == unsubmitted_before);
    CHECK(ota_observation_count == observed_before);
}

static void expect_default_ota_refusal(uint32_t request_id,
    const uint8_t *data, size_t data_size, uint32_t image_size,
    const struct p4_board_update_gates *gates, uint32_t timeout_us)
{
    expect_ota_preflight_refusal(request_id, data, data_size, image_size,
        gates, timeout_us, TEST_TOKEN, TEST_GENERATION);
}

static void expect_finish_preflight_refusal(
    enum p4_board_update_finish_mode mode, uint32_t timeout_us,
    uint32_t token, uint32_t generation)
{
    struct P4WifiBase base_before;
    struct P4SDMMCHost host_before;
    struct wifi_probe_context context_before;
    struct p4_hosted_owner owner_before;
    struct p4_hosted_fifo fifo_before;
    struct wifi_owner_control control_before;
    struct wifi_device_runtime runtime_before;
    const ULONG time_before = fake_now;
    const unsigned now_before = now_calls;
    const unsigned requests_before = request_calls;
    const unsigned acquires_before = discovery_acquire_calls;
    const unsigned releases_before = discovery_release_calls;
    const unsigned ready_before = ready_calls;
    const unsigned stops_before = discovery_stopping_calls;
    const unsigned unlocked_stops_before = finish_stopping_unlocked_calls;
    const unsigned locked_stops_before = finish_stopping_locked_calls;

    memcpy(&base_before, &base, sizeof(base_before));
    memcpy(&host_before, &host, sizeof(host_before));
    memcpy(&context_before, &context, sizeof(context_before));
    memcpy(&owner_before, &owner, sizeof(owner_before));
    memcpy(&fifo_before, &fifo, sizeof(fifo_before));
    memcpy(&control_before, &control, sizeof(control_before));
    memcpy(&runtime_before, &runtime, sizeof(runtime_before));
    CHECK(call_finish(mode, timeout_us, token, generation) == -1);
    CHECK(memcmp(&base_before, &base, sizeof(base_before)) == 0);
    CHECK(memcmp(&host_before, &host, sizeof(host_before)) == 0);
    CHECK(memcmp(&context_before, &context, sizeof(context_before)) == 0);
    CHECK(memcmp(&owner_before, &owner, sizeof(owner_before)) == 0);
    CHECK(memcmp(&fifo_before, &fifo, sizeof(fifo_before)) == 0);
    CHECK(memcmp(&control_before, &control, sizeof(control_before)) == 0);
    CHECK(memcmp(&runtime_before, &runtime, sizeof(runtime_before)) == 0);
    CHECK(fake_now == time_before && now_calls == now_before);
    CHECK(request_calls == requests_before);
    CHECK(discovery_acquire_calls == acquires_before);
    CHECK(discovery_release_calls == releases_before);
    CHECK(ready_calls == ready_before);
    CHECK(discovery_stopping_calls == stops_before);
    CHECK(finish_stopping_unlocked_calls == unlocked_stops_before);
    CHECK(finish_stopping_locked_calls == locked_stops_before);
}

static void test_ota_span_helpers(void)
{
    uint8_t wrapped[1] = {0u};
    uintptr_t near_end = UINTPTR_MAX - 1u;
    prepare_ota_active();
    CHECK(wifi_ota_span_valid(&base, &runtime, &valid_ota_gates,
                              sizeof(valid_ota_gates)));
    CHECK(!wifi_ota_span_valid(&base, &runtime, &control, sizeof(control)));
    CHECK(!wifi_ota_span_valid(&base, &runtime,
        (const void *)near_end, 4u));
    CHECK(wifi_ota_spans_disjoint(&valid_ota_gates,
        sizeof(valid_ota_gates), wrapped, sizeof(wrapped)));
    CHECK(!wifi_ota_spans_disjoint(&valid_ota_gates,
        sizeof(valid_ota_gates), &valid_ota_gates.manifest_accepted, 1u));
    CHECK(!wifi_ota_spans_disjoint((const void *)near_end, 4u,
        wrapped, sizeof(wrapped)));
}

static void test_ota_admission_refusals(void)
{
    uint8_t image[P4_HOSTED_OTA_MAX_WRITE_SIZE + 1u];
    struct p4_board_update_gates gates;
    unsigned gate;
    size_t index;
    memset(image, 0x6d, sizeof(image));

    prepare_ota_active();
    expect_default_ota_refusal(272u, NULL, 0u, 0u, &valid_ota_gates, 1000u);
    prepare_ota_active();
    expect_default_ota_refusal(272u, NULL, 0u,
        P4_BOARD_UPDATE_MAX_IMAGE_SIZE + 1u, &valid_ota_gates, 1000u);
    prepare_ota_active();
    expect_default_ota_refusal(271u, NULL, 0u, 1u, &valid_ota_gates, 1000u);
    prepare_ota_active();
    expect_default_ota_refusal(275u, NULL, 0u, 1u, &valid_ota_gates, 1000u);
    prepare_ota_active();
    expect_default_ota_refusal(272u, NULL, 0u, 1u, &valid_ota_gates, 0u);
    prepare_ota_active();
    expect_default_ota_refusal(272u, NULL, 0u, 1u, &valid_ota_gates,
        P4_HOSTED_OWNER_MAX_EXCHANGE_US + 1u);
    prepare_ota_active();
    expect_ota_preflight_refusal(272u, NULL, 0u, 1u, &valid_ota_gates,
        1000u, 0u, TEST_GENERATION);
    prepare_ota_active();
    expect_ota_preflight_refusal(272u, NULL, 0u, 1u, &valid_ota_gates,
        1000u, TEST_TOKEN + 1u, TEST_GENERATION);
    prepare_ota_active();
    expect_ota_preflight_refusal(272u, NULL, 0u, 1u, &valid_ota_gates,
        1000u, TEST_TOKEN, 0u);
    prepare_ota_active();
    expect_ota_preflight_refusal(272u, NULL, 0u, 1u, &valid_ota_gates,
        1000u, TEST_TOKEN, TEST_GENERATION + 1u);

    prepare_ota_active();
    current_task = &requester_task;
    expect_default_ota_refusal(272u, NULL, 0u, 1u, &valid_ota_gates, 1000u);
    prepare_ota_active();
    base.client_lock.ss_Owner = &requester_task;
    expect_default_ota_refusal(272u, NULL, 0u, 1u, &valid_ota_gates, 1000u);
    prepare_ota_active();
    base.owner_task = &other_task;
    expect_default_ota_refusal(272u, NULL, 0u, 1u, &valid_ota_gates, 1000u);
    prepare_ota_active();
    base.association_enabled = FALSE;
    expect_default_ota_refusal(272u, NULL, 0u, 1u, &valid_ota_gates, 1000u);
    prepare_ota_active();
    base.maintenance_state = P4_WIFI_MAINT_REQUESTED;
    expect_default_ota_refusal(272u, NULL, 0u, 1u, &valid_ota_gates, 1000u);
    prepare_ota_active();
    base.maintenance_task = NULL;
    expect_default_ota_refusal(272u, NULL, 0u, 1u, &valid_ota_gates, 1000u);
    prepare_ota_active();
    base.maintenance_task = &owner_task;
    expect_default_ota_refusal(272u, NULL, 0u, 1u, &valid_ota_gates, 1000u);
    prepare_ota_active();
    fake_now += TEST_LEASE_US;
    expect_default_ota_refusal(272u, NULL, 0u, 1u, &valid_ota_gates, 1000u);

    prepare_ota_active();
    runtime.scanning = 1u;
    expect_default_ota_refusal(272u, NULL, 0u, 1u, &valid_ota_gates, 1000u);
    prepare_ota_active();
    owner.pending_active = 1u;
    expect_default_ota_refusal(272u, NULL, 0u, 1u, &valid_ota_gates, 1000u);
    prepare_ota_active();
    owner.partial_active = 1u;
    expect_default_ota_refusal(272u, NULL, 0u, 1u, &valid_ota_gates, 1000u);
    prepare_ota_active();
    owner.ids.active_uid = 101u;
    expect_default_ota_refusal(272u, NULL, 0u, 1u, &valid_ota_gates, 1000u);
    prepare_ota_active();
    fifo.busy = 1u;
    expect_default_ota_refusal(272u, NULL, 0u, 1u, &valid_ota_gates, 1000u);
    prepare_ota_active();
    control.discovery_state = P4_WIFI_DISCOVERY_REQUESTED;
    expect_default_ota_refusal(272u, NULL, 0u, 1u, &valid_ota_gates, 1000u);
    prepare_ota_active();
    control.ota_closed = 1u;
    expect_default_ota_refusal(272u, NULL, 0u, 1u, &valid_ota_gates, 1000u);
    prepare_ota_active();
    host.lock.ss_Owner = current_task;
    expect_default_ota_refusal(272u, NULL, 0u, 1u, &valid_ota_gates, 1000u);
    prepare_ota_active();
    init.chip_id = 0u;
    expect_default_ota_refusal(272u, NULL, 0u, 1u, &valid_ota_gates, 1000u);
    prepare_ota_active();
    init.present &= (uint8_t)~P4_HOSTED_INIT_HAS_CHIP_ID;
    expect_default_ota_refusal(272u, NULL, 0u, 1u, &valid_ota_gates, 1000u);
    prepare_ota_active();
    init.present &= (uint8_t)~P4_HOSTED_INIT_HAS_CAPABILITIES;
    expect_default_ota_refusal(272u, NULL, 0u, 1u, &valid_ota_gates, 1000u);
    prepare_ota_active();
    init.capabilities = 0u;
    expect_default_ota_refusal(272u, NULL, 0u, 1u, &valid_ota_gates, 1000u);

    for (gate = 0u; gate < 4u; ++gate) {
        prepare_ota_active();
        gates = valid_ota_gates;
        switch (gate) {
        case 0u: gates.manifest_accepted = 0u; break;
        case 1u: gates.inspection_passed = 0u; break;
        case 2u: gates.recovery_verified = 0u; break;
        default: gates.target_compatible = 0u; break;
        }
        expect_default_ota_refusal(272u, NULL, 0u, 1u, &gates, 1000u);
    }

    prepare_ota_active();
    control.ota_generation = TEST_GENERATION;
    expect_default_ota_refusal(272u, NULL, 0u, 1u, &valid_ota_gates, 1000u);
    prepare_ota_active();
    control.ota_image_size = 1u;
    expect_default_ota_refusal(272u, NULL, 0u, 1u, &valid_ota_gates, 1000u);

    for (index = 0u; index < 2u; ++index) {
        prepare_ota_active();
        if (index == 0u) owner.generation += 1u;
        else fifo.generation += 1u;
        expect_default_ota_refusal(272u, NULL, 0u, 1u, &valid_ota_gates,
                                   1000u);
    }
    prepare_ota_active();
    host.dma_faulted = TRUE;
    expect_default_ota_refusal(272u, NULL, 0u, 1u, &valid_ota_gates, 1000u);
    prepare_ota_active();
    owner.poisoned = 1u;
    expect_default_ota_refusal(272u, NULL, 0u, 1u, &valid_ota_gates, 1000u);
    prepare_ota_active();
    fifo.poisoned = 1u;
    expect_default_ota_refusal(272u, NULL, 0u, 1u, &valid_ota_gates, 1000u);
    prepare_ota_active();
    base.ready = FALSE;
    expect_default_ota_refusal(272u, NULL, 0u, 1u, &valid_ota_gates, 1000u);
    prepare_ota_active();
    host.now_us = NULL;
    expect_default_ota_refusal(272u, NULL, 0u, 1u, &valid_ota_gates, 1000u);

    prepare_ota_active();
    expect_default_ota_refusal(273u, image, 1u, 1u, &valid_ota_gates, 1000u);
    prepare_ota_active();
    expect_default_ota_refusal(274u, NULL, 0u, 1u, &valid_ota_gates, 1000u);
}

static void test_ota_success_order_and_acknowledged_bytes(void)
{
    uint8_t image[500];
    unsigned index;
    int32_t status = -71;
    memset(image, 0, sizeof(image));
    for (index = 0u; index < sizeof(image); ++index)
        image[index] = (uint8_t)(index * 19u + 7u);
    prepare_ota_active();

    CHECK(call_ota(273u, image, 1u, sizeof(image), &valid_ota_gates,
        1000000u, &status) == P4_BOARD_UPDATE_CALL_AMBIGUOUS);
    CHECK(status == -71);
    CHECK(ota_reserve_calls == 0u && ota_exchange_calls == 0u);

    CHECK(call_ota(272u, NULL, 0u, sizeof(image), &valid_ota_gates,
        1000000u, &status) == P4_BOARD_UPDATE_CALL_ACKNOWLEDGED);
    CHECK(status == 0);
    CHECK(base.maintenance_state == P4_WIFI_MAINT_ACTIVE);
    CHECK(base.maintenance_fence_mode == P4_WIFI_MAINT_FENCE_INCOMPLETE);
    CHECK(base.associated == FALSE);
    CHECK(control.ota_token == TEST_TOKEN);
    CHECK(control.ota_generation == TEST_GENERATION);
    CHECK(control.ota_image_size == sizeof(image));
    CHECK(control.ota_begin_acknowledged == 1u);
    CHECK(control.ota_acknowledged_bytes == 0u);
    CHECK(!control.ota_closed);
    CHECK(ota_observations[0].request_id == 272u);
    CHECK(ota_observations[0].response_id == 528u);
    CHECK(ota_observations[0].uid == 100u);
    CHECK(ota_observations[0].response_uid == 100u);
    CHECK(ota_observations[0].generation == TEST_GENERATION);
    CHECK(ota_observations[0].timeout_us == 1000000u);
    CHECK(ota_observations[0].data_size == 0u);

    expect_default_ota_refusal(272u, NULL, 0u, sizeof(image),
        &valid_ota_gates, 1000000u);
    expect_default_ota_refusal(273u, image, 0u, sizeof(image),
        &valid_ota_gates, 1000000u);
    expect_default_ota_refusal(273u, image, sizeof(image), sizeof(image),
        &valid_ota_gates, 1000000u);
    expect_default_ota_refusal(273u, image, P4_HOSTED_OTA_MAX_WRITE_SIZE + 1u,
        sizeof(image), &valid_ota_gates, 1000000u);
    expect_default_ota_refusal(274u, NULL, 0u, sizeof(image),
        &valid_ota_gates, 1000000u);
    expect_ota_preflight_refusal(273u, image, 1u, sizeof(image),
        &valid_ota_gates, 1000000u, TEST_TOKEN + 1u, TEST_GENERATION);
    expect_ota_preflight_refusal(273u, image, 1u, sizeof(image),
        &valid_ota_gates, 1000000u, TEST_TOKEN, TEST_GENERATION + 1u);

    CHECK(call_ota(273u, image, 238u, sizeof(image), &valid_ota_gates,
        1000000u, &status) == P4_BOARD_UPDATE_CALL_ACKNOWLEDGED);
    CHECK(control.ota_acknowledged_bytes == 238u);
    CHECK(memcmp(ota_observations[1].data, image, 238u) == 0);
    CHECK(ota_observations[1].request_id == 273u);
    CHECK(ota_observations[1].response_id == 529u);
    CHECK(ota_observations[1].data_size == 238u);

    CHECK(call_ota(273u, image + 238u, 238u, sizeof(image),
        &valid_ota_gates, 1000000u, &status) ==
        P4_BOARD_UPDATE_CALL_ACKNOWLEDGED);
    CHECK(control.ota_acknowledged_bytes == 476u);
    CHECK(memcmp(ota_observations[2].data, image + 238u, 238u) == 0);

    expect_default_ota_refusal(273u, image + 476u, 25u, sizeof(image),
        &valid_ota_gates, 1000000u);
    expect_default_ota_refusal(274u, NULL, 0u, sizeof(image),
        &valid_ota_gates, 1000000u);

    CHECK(call_ota(273u, image + 476u, 24u, sizeof(image),
        &valid_ota_gates, 1000000u, &status) ==
        P4_BOARD_UPDATE_CALL_ACKNOWLEDGED);
    CHECK(control.ota_acknowledged_bytes == sizeof(image));
    CHECK(ota_observations[3].request_id == 273u);
    CHECK(ota_observations[3].response_id == 529u);
    CHECK(ota_observations[3].data_size == 24u);
    CHECK(memcmp(ota_observations[3].data, image + 476u, 24u) == 0);

    CHECK(call_ota(274u, NULL, 0u, sizeof(image), &valid_ota_gates,
        1000000u, &status) == P4_BOARD_UPDATE_CALL_ACKNOWLEDGED);
    CHECK(status == 0);
    CHECK(base.maintenance_fence_mode == P4_WIFI_MAINT_FENCE_WAIT_REBOOT);
    CHECK(control.ota_closed == 1u);
    CHECK(ota_observations[4].request_id == 274u);
    CHECK(ota_observations[4].response_id == 530u);
    CHECK(ota_reserve_calls == 5u);
    CHECK(ota_encode_calls == 5u);
    CHECK(ota_exchange_calls == 5u);
    CHECK(ota_status_calls == 5u);
    CHECK(ota_unsubmitted_calls == 0u);
    for (index = 1u; index < ota_observation_count; ++index)
        CHECK(ota_observations[index].uid != ota_observations[index - 1u].uid);

    expect_default_ota_refusal(273u, image, 1u, sizeof(image),
        &valid_ota_gates, 1000000u);
    expect_default_ota_refusal(274u, NULL, 0u, sizeof(image),
        &valid_ota_gates, 1000000u);
}

static void test_ota_status_and_buffer_alias_refusals(void)
{
    uint8_t byte = 0xa5u;
    int32_t status = -91;
    union {
        struct p4_board_update_gates gates;
        int32_t status;
    } aliased;
    struct P4WifiBase base_before;
    struct wifi_owner_control control_before;
    struct p4_hosted_owner owner_before;
    unsigned exchanges_before;

    prepare_ota_active();
    memcpy(&base_before, &base, sizeof(base_before));
    memcpy(&control_before, &control, sizeof(control_before));
    memcpy(&owner_before, &owner, sizeof(owner_before));
    CHECK(p4_wifi_owner_ota_exchange(&base, TEST_TOKEN, TEST_GENERATION,
        272u, NULL, 0u, 1u, &valid_ota_gates, 1000u, NULL) ==
        P4_BOARD_UPDATE_CALL_AMBIGUOUS);
    CHECK(memcmp(&base_before, &base, sizeof(base_before)) == 0);
    CHECK(memcmp(&control_before, &control, sizeof(control_before)) == 0);
    CHECK(memcmp(&owner_before, &owner, sizeof(owner_before)) == 0);
    CHECK(ota_reserve_calls == 0u && ota_exchange_calls == 0u);

    CHECK(p4_wifi_owner_ota_exchange(&base, TEST_TOKEN, TEST_GENERATION,
        272u, NULL, 0u, 1u,
        (const struct p4_board_update_gates *)(const void *)&control,
        1000u, &status) ==
        P4_BOARD_UPDATE_CALL_AMBIGUOUS);
    CHECK(status == -91);
    CHECK(memcmp(&base_before, &base, sizeof(base_before)) == 0);
    CHECK(memcmp(&control_before, &control, sizeof(control_before)) == 0);
    CHECK(memcmp(&owner_before, &owner, sizeof(owner_before)) == 0);

    CHECK(p4_wifi_owner_ota_exchange(&base, TEST_TOKEN, TEST_GENERATION,
        272u, NULL, 0u, 1u, &valid_ota_gates, 1000u,
        (int32_t *)(void *)&base.maintenance_token) ==
        P4_BOARD_UPDATE_CALL_AMBIGUOUS);
    CHECK(memcmp(&base_before, &base, sizeof(base_before)) == 0);

    aliased.gates = valid_ota_gates;
    CHECK(p4_wifi_owner_ota_exchange(&base, TEST_TOKEN, TEST_GENERATION,
        272u, NULL, 0u, 1u, &aliased.gates, 1000u, &aliased.status) ==
        P4_BOARD_UPDATE_CALL_AMBIGUOUS);
    CHECK(ota_reserve_calls == 0u && ota_exchange_calls == 0u);

    CHECK(p4_wifi_owner_ota_exchange(&base, TEST_TOKEN, TEST_GENERATION,
        272u, NULL, 0u, 1u, &valid_ota_gates, 1000u,
        (int32_t *)(void *)((uint8_t *)&status + 1u)) ==
        P4_BOARD_UPDATE_CALL_AMBIGUOUS);
    CHECK(status == -91);

    CHECK(call_ota(272u, NULL, 0u, 2u, &valid_ota_gates, 1000u,
        &status) == P4_BOARD_UPDATE_CALL_ACKNOWLEDGED);
    status = -91;
    exchanges_before = ota_exchange_calls;
    CHECK(call_ota(273u, (const uint8_t *)&status, 1u, 2u,
        &valid_ota_gates, 1000u, &status) ==
        P4_BOARD_UPDATE_CALL_AMBIGUOUS);
    CHECK(status == -91);
    CHECK(control.ota_acknowledged_bytes == 0u);
    CHECK(ota_exchange_calls == exchanges_before);
    exchanges_before = ota_exchange_calls;
    CHECK(call_ota(273u, &byte, 1u, 2u, &valid_ota_gates, 1000u,
        &status) == P4_BOARD_UPDATE_CALL_ACKNOWLEDGED);
    CHECK(control.ota_acknowledged_bytes == 1u);
    expect_default_ota_refusal(273u, (const uint8_t *)&valid_ota_gates, 1u,
        2u, &valid_ota_gates, 1000u);
    expect_default_ota_refusal(273u, control.request, 1u, 2u,
        &valid_ota_gates, 1000u);
    expect_default_ota_refusal(273u,
        (const uint8_t *)(uintptr_t)(UINTPTR_MAX - 1u), 4u, 2u,
        &valid_ota_gates, 1000u);
    CHECK(ota_exchange_calls == exchanges_before + 1u);
}

static void test_ota_genuine_resource_instance_refusal(void)
{
    struct P4WifiBase lookalike;
    struct P4WifiBase base_before;
    struct P4WifiBase lookalike_before;
    struct p4_hosted_owner owner_before;
    struct wifi_owner_control control_before;
    struct wifi_device_runtime runtime_before;
    int32_t status = INT32_C(0x24681357);
    unsigned reserve_before;
    unsigned encode_before;
    unsigned exchange_before;
    unsigned status_before;
    unsigned unsubmitted_before;
    unsigned observations_before;

    prepare_ota_active();
    CHECK(wifi_resource_base == &base);
    lookalike = base;
    memcpy(&base_before, &base, sizeof(base_before));
    memcpy(&lookalike_before, &lookalike, sizeof(lookalike_before));
    memcpy(&owner_before, &owner, sizeof(owner_before));
    memcpy(&control_before, &control, sizeof(control_before));
    memcpy(&runtime_before, &runtime, sizeof(runtime_before));
    reserve_before = ota_reserve_calls;
    encode_before = ota_encode_calls;
    exchange_before = ota_exchange_calls;
    status_before = ota_status_calls;
    unsubmitted_before = ota_unsubmitted_calls;
    observations_before = ota_observation_count;

    CHECK(p4_wifi_owner_ota_exchange(&lookalike, TEST_TOKEN,
        TEST_GENERATION, 272u, NULL, 0u, 1u, &valid_ota_gates, 1000u,
        &status) == P4_BOARD_UPDATE_CALL_AMBIGUOUS);
    CHECK(status == INT32_C(0x24681357));
    CHECK(memcmp(&base_before, &base, sizeof(base_before)) == 0);
    CHECK(memcmp(&lookalike_before, &lookalike,
        sizeof(lookalike_before)) == 0);
    CHECK(memcmp(&owner_before, &owner, sizeof(owner_before)) == 0);
    CHECK(memcmp(&control_before, &control, sizeof(control_before)) == 0);
    CHECK(memcmp(&runtime_before, &runtime, sizeof(runtime_before)) == 0);
    CHECK(ota_reserve_calls == reserve_before);
    CHECK(ota_encode_calls == encode_before);
    CHECK(ota_exchange_calls == exchange_before);
    CHECK(ota_status_calls == status_before);
    CHECK(ota_unsubmitted_calls == unsubmitted_before);
    CHECK(ota_observation_count == observations_before);
}

static void test_ota_begin_failures_are_sticky_and_closed(void)
{
    unsigned failure;
    for (failure = 0u; failure < 9u; ++failure) {
        int32_t status = -321;
        unsigned expected_status_calls = 0u;
        prepare_ota_active();
        if (failure == 0u)
            ota_reserve_succeeds = FALSE;
        else if (failure == 1u)
            ota_encode_succeeds = FALSE;
        else if (failure == 2u)
            ota_encode_advance_us = 1000u;
        else if (failure == 3u)
            ota_exchange_succeeds = FALSE;
        else if (failure == 4u) {
            ota_exchange_succeeds = FALSE;
            ota_exchange_submitted = 1u;
        } else if (failure == 5u)
            ota_response_id_delta = 1u;
        else if (failure == 6u)
            ota_response_uid_delta = 1u;
        else if (failure == 7u)
            ota_response_type_delta = 1u;
        else
            ota_status_succeeds = FALSE;

        if (failure == 2u) {
            CHECK(call_ota(272u, NULL, 0u, 1u, &valid_ota_gates, 1000u,
                &status) == P4_BOARD_UPDATE_CALL_AMBIGUOUS);
        } else {
            CHECK(call_ota(272u, NULL, 0u, 1u, &valid_ota_gates, 10000u,
                &status) == P4_BOARD_UPDATE_CALL_AMBIGUOUS);
        }
        CHECK(status == -321);
        CHECK(base.maintenance_fence_mode == P4_WIFI_MAINT_FENCE_INCOMPLETE);
        CHECK(base.associated == FALSE);
        CHECK(control.ota_token == TEST_TOKEN);
        CHECK(control.ota_generation == TEST_GENERATION);
        CHECK(control.ota_image_size == 1u);
        CHECK(control.ota_closed == 1u);
        CHECK(control.ota_begin_acknowledged == 0u);
        CHECK(control.ota_acknowledged_bytes == 0u);
        if (failure == 0u) {
            CHECK(ota_reserve_calls == 1u);
            CHECK(ota_encode_calls == 0u && ota_exchange_calls == 0u);
            CHECK(ota_unsubmitted_calls == 0u);
        } else if (failure == 1u || failure == 2u) {
            CHECK(ota_reserve_calls == 1u && ota_encode_calls == 1u);
            CHECK(ota_exchange_calls == 0u && ota_status_calls == 0u);
            CHECK(ota_unsubmitted_calls == 1u);
        } else if (failure == 3u || failure == 4u) {
            CHECK(ota_reserve_calls == 1u && ota_encode_calls == 1u);
            CHECK(ota_exchange_calls == 1u && ota_status_calls == 0u);
            CHECK(ota_unsubmitted_calls == 1u);
            CHECK(owner.poisoned == (failure == 4u));
            CHECK(fifo.poisoned == (failure == 4u));
        } else {
            expected_status_calls = 1u;
            CHECK(ota_reserve_calls == 1u && ota_encode_calls == 1u);
            CHECK(ota_exchange_calls == 1u);
            CHECK(ota_status_calls == expected_status_calls);
            CHECK(ota_unsubmitted_calls == 0u);
            CHECK(ota_observations[0].request_id == 272u);
            CHECK(ota_observations[0].response_uid != 0u);
        }

        {
            unsigned exchanges = ota_exchange_calls;
            unsigned reservations = ota_reserve_calls;
            CHECK(call_ota(272u, NULL, 0u, 1u, &valid_ota_gates,
                10000u, &status) == P4_BOARD_UPDATE_CALL_AMBIGUOUS);
            CHECK(status == -321);
            CHECK(ota_exchange_calls == exchanges);
            CHECK(ota_reserve_calls == reservations);
        }
    }
}

static void test_ota_remote_rejection_and_end_commit_unknown(void)
{
    uint8_t image = 0x7cu;
    int32_t status = -445;
    unsigned exchanges;

    prepare_ota_active();
    ota_remote_status = -9;
    CHECK(call_ota(272u, NULL, 0u, 1u, &valid_ota_gates, 10000u,
        &status) == P4_BOARD_UPDATE_CALL_REJECTED);
    CHECK(status == -9);
    CHECK(base.maintenance_fence_mode == P4_WIFI_MAINT_FENCE_INCOMPLETE);
    CHECK(control.ota_closed == 1u && !control.ota_begin_acknowledged);
    exchanges = ota_exchange_calls;
    CHECK(call_ota(272u, NULL, 0u, 1u, &valid_ota_gates, 10000u,
        &status) == P4_BOARD_UPDATE_CALL_AMBIGUOUS);
    CHECK(ota_exchange_calls == exchanges);

    prepare_ota_active();
    CHECK(call_ota(272u, NULL, 0u, 1u, &valid_ota_gates, 10000u,
        &status) == P4_BOARD_UPDATE_CALL_ACKNOWLEDGED);
    ota_remote_status = -10;
    CHECK(call_ota(273u, &image, 1u, 1u, &valid_ota_gates, 10000u,
        &status) == P4_BOARD_UPDATE_CALL_REJECTED);
    CHECK(status == -10);
    CHECK(control.ota_acknowledged_bytes == 0u);
    CHECK(base.maintenance_fence_mode == P4_WIFI_MAINT_FENCE_INCOMPLETE);
    CHECK(control.ota_closed == 1u);
    exchanges = ota_exchange_calls;
    CHECK(call_ota(274u, NULL, 0u, 1u, &valid_ota_gates, 10000u,
        &status) == P4_BOARD_UPDATE_CALL_AMBIGUOUS);
    CHECK(ota_exchange_calls == exchanges);

    prepare_ota_active();
    CHECK(call_ota(272u, NULL, 0u, 1u, &valid_ota_gates, 10000u,
        &status) == P4_BOARD_UPDATE_CALL_ACKNOWLEDGED);
    CHECK(call_ota(273u, &image, 1u, 1u, &valid_ota_gates, 10000u,
        &status) == P4_BOARD_UPDATE_CALL_ACKNOWLEDGED);
    ota_remote_status = -11;
    CHECK(call_ota(274u, NULL, 0u, 1u, &valid_ota_gates, 10000u,
        &status) == P4_BOARD_UPDATE_CALL_REJECTED);
    CHECK(status == -11);
    CHECK(base.maintenance_fence_mode == P4_WIFI_MAINT_FENCE_COMMIT_UNKNOWN);
    CHECK(control.ota_closed == 1u);
    exchanges = ota_exchange_calls;
    CHECK(call_ota(274u, NULL, 0u, 1u, &valid_ota_gates, 10000u,
        &status) == P4_BOARD_UPDATE_CALL_AMBIGUOUS);
    CHECK(ota_exchange_calls == exchanges);

    prepare_ota_active();
    CHECK(call_ota(272u, NULL, 0u, 1u, &valid_ota_gates, 10000u,
        &status) == P4_BOARD_UPDATE_CALL_ACKNOWLEDGED);
    CHECK(call_ota(273u, &image, 1u, 1u, &valid_ota_gates, 10000u,
        &status) == P4_BOARD_UPDATE_CALL_ACKNOWLEDGED);
    ota_encode_succeeds = FALSE;
    status = -445;
    exchanges = ota_exchange_calls;
    CHECK(call_ota(274u, NULL, 0u, 1u, &valid_ota_gates, 10000u,
        &status) == P4_BOARD_UPDATE_CALL_AMBIGUOUS);
    CHECK(status == -445);
    CHECK(base.maintenance_fence_mode == P4_WIFI_MAINT_FENCE_COMMIT_UNKNOWN);
    CHECK(control.ota_closed == 1u);
    CHECK(ota_exchange_calls == exchanges);
}

static void test_ota_deadline_caps_and_late_ack(void)
{
    uint8_t image = 0x2au;
    int32_t status = -6;
    unsigned exchanges;

    prepare_ota_active();
    fake_now = base.maintenance_started + TEST_LEASE_US - 500u;
    CHECK(call_ota(272u, NULL, 0u, 1u, &valid_ota_gates, 1000u,
        &status) == P4_BOARD_UPDATE_CALL_ACKNOWLEDGED);
    CHECK(ota_observations[0].timeout_us == 500u);

    prepare_ota_active();
    CHECK(call_ota(272u, NULL, 0u, 1u, &valid_ota_gates, 100u,
        &status) == P4_BOARD_UPDATE_CALL_ACKNOWLEDGED);
    CHECK(call_ota(273u, &image, 1u, 1u, &valid_ota_gates, 100u,
        &status) == P4_BOARD_UPDATE_CALL_ACKNOWLEDGED);
    ota_exchange_advance_us = 101u;
    CHECK(call_ota(274u, NULL, 0u, 1u, &valid_ota_gates, 100u,
        &status) == P4_BOARD_UPDATE_CALL_ACKNOWLEDGED);
    CHECK(status == 0);
    CHECK(ota_observations[2].timeout_us == 100u);
    CHECK(base.maintenance_fence_mode == P4_WIFI_MAINT_FENCE_WAIT_REBOOT);
    CHECK(control.ota_closed == 1u);
    exchanges = ota_exchange_calls;
    status = -6;
    CHECK(call_ota(273u, &image, 1u, 1u, &valid_ota_gates, 100u,
        &status) == P4_BOARD_UPDATE_CALL_AMBIGUOUS);
    CHECK(status == -6);
    CHECK(ota_exchange_calls == exchanges);
}

struct finish_mode_case {
    enum p4_board_update_finish_mode engine_mode;
    ULONG native_mode;
};

static void test_ota_finish_mode_mapping_and_success(void)
{
    static const struct finish_mode_case cases[] = {
        {P4_BOARD_UPDATE_FINISH_NO_BEGIN, 0u},
        {P4_BOARD_UPDATE_FINISH_INCOMPLETE,
         P4_WIFI_MAINT_FENCE_INCOMPLETE},
        {P4_BOARD_UPDATE_FINISH_COMMIT_UNKNOWN,
         P4_WIFI_MAINT_FENCE_COMMIT_UNKNOWN},
        {P4_BOARD_UPDATE_FINISH_WAIT_REBOOT,
         P4_WIFI_MAINT_FENCE_WAIT_REBOOT}
    };
    unsigned index;
    for (index = 0u; index < sizeof(cases) / sizeof(cases[0]); ++index) {
        unsigned requests;
        unsigned acquires;
        unsigned releases;
        prepare_finish_active();
        if (cases[index].native_mode >= P4_WIFI_MAINT_FENCE_COMMIT_UNKNOWN) {
            base.maintenance_fence_mode = cases[index].native_mode;
            control.ota_token = TEST_TOKEN;
            control.ota_generation = TEST_GENERATION;
            control.ota_image_size = 19u;
            control.ota_begin_acknowledged = 1u;
            control.ota_acknowledged_bytes = 19u;
        }
        CHECK(base.maintenance_fence_mode ==
              (index == 0u ? 0u : cases[index].native_mode ==
                  P4_WIFI_MAINT_FENCE_INCOMPLETE ? 0u :
                  cases[index].native_mode));
        if (cases[index].native_mode == 0u) {
            control.discovery_state = P4_WIFI_DISCOVERY_READY;
            control.discovery_token = TEST_TOKEN;
            memset(&control.discovery_result, 0xa5,
                   sizeof(control.discovery_result));
            memset(&control.discovery_decoded, 0x5a,
                   sizeof(control.discovery_decoded));
            memset(control.discovery_response, 0xc3,
                   sizeof(control.discovery_response));
        }

        CHECK(call_finish(cases[index].engine_mode,
            cases[index].native_mode == 0u ? 9000000u : 100000u,
            TEST_TOKEN, TEST_GENERATION) == 0);
        CHECK(base.maintenance_fence_mode == cases[index].native_mode);
        CHECK(control.discovery_state == (cases[index].native_mode == 0u
            ? P4_WIFI_DISCOVERY_IDLE : P4_WIFI_DISCOVERY_FAILED));
        CHECK(control.discovery_token == 0u);
        CHECK(bytes_are_zero(&control.discovery_result,
                             sizeof(control.discovery_result)));
        CHECK(bytes_are_zero(&control.discovery_decoded,
                             sizeof(control.discovery_decoded)));
        CHECK(bytes_are_zero(control.discovery_response,
                             sizeof(control.discovery_response)));
        if (cases[index].native_mode == 0u) {
            CHECK(base.maintenance_state == P4_WIFI_MAINT_RELEASED);
            CHECK(base.ready == TRUE);
            CHECK(ready_calls == 1u);
            CHECK(request_calls == 1u && request_ids[0] == 280u);
            CHECK(request_budgets[0] == 8000000u);
            CHECK(finish_no_begin_clear_observations == 1u);
            CHECK(discovery_acquire_calls == 0u);
            CHECK(discovery_release_calls == 0u);
            CHECK(finish_fence_release_observations == 0u);
            CHECK(finish_stopping_unlocked_calls == 2u);
            CHECK(finish_stopping_locked_calls == 0u);
        } else {
            CHECK(base.maintenance_state == P4_WIFI_MAINT_FENCED);
            CHECK(base.ready == FALSE);
            CHECK(base.associated == FALSE);
            CHECK(control.ota_closed == 1u);
            CHECK(ready_calls == 0u);
            CHECK(request_calls == 0u);
            CHECK(discovery_acquire_calls == 1u);
            CHECK(discovery_acquire_wait == P4_HOSTED_OWNER_MAX_ACQUIRE_US);
            CHECK(discovery_release_calls == 1u);
            CHECK(finish_fence_release_observations == 1u);
            CHECK(host.lock.ss_Owner == NULL);
            CHECK(finish_stopping_unlocked_calls == 1u);
            CHECK(finish_stopping_locked_calls == 1u);
            CHECK(finish_no_begin_clear_observations == 0u);
        }

        requests = request_calls;
        acquires = discovery_acquire_calls;
        releases = discovery_release_calls;
        expect_finish_preflight_refusal(cases[index].engine_mode,
            100000u, TEST_TOKEN, TEST_GENERATION);
        CHECK(request_calls == requests);
        CHECK(discovery_acquire_calls == acquires);
        CHECK(discovery_release_calls == releases);
        CHECK(ready_calls == (cases[index].native_mode == 0u ? 1u : 0u));
    }
}

static void test_ota_finish_preflight_refusals(void)
{
    unsigned dirty_field;
    prepare_finish_active();
    expect_finish_preflight_refusal(P4_BOARD_UPDATE_FINISH_NONE,
        1000u, TEST_TOKEN, TEST_GENERATION);
    expect_finish_preflight_refusal(
        (enum p4_board_update_finish_mode)99, 1000u,
        TEST_TOKEN, TEST_GENERATION);
    expect_finish_preflight_refusal(P4_BOARD_UPDATE_FINISH_NO_BEGIN,
        0u, TEST_TOKEN, TEST_GENERATION);
    expect_finish_preflight_refusal(P4_BOARD_UPDATE_FINISH_NO_BEGIN,
        P4_HOSTED_OWNER_MAX_EXCHANGE_US + 1u,
        TEST_TOKEN, TEST_GENERATION);
    expect_finish_preflight_refusal(P4_BOARD_UPDATE_FINISH_NO_BEGIN,
        1000u, 0u, TEST_GENERATION);
    expect_finish_preflight_refusal(P4_BOARD_UPDATE_FINISH_NO_BEGIN,
        1000u, TEST_TOKEN + 1u, TEST_GENERATION);
    expect_finish_preflight_refusal(P4_BOARD_UPDATE_FINISH_NO_BEGIN,
        1000u, TEST_TOKEN, 0u);
    expect_finish_preflight_refusal(P4_BOARD_UPDATE_FINISH_NO_BEGIN,
        1000u, TEST_TOKEN, TEST_GENERATION + 1u);

    current_task = &requester_task;
    expect_finish_preflight_refusal(P4_BOARD_UPDATE_FINISH_NO_BEGIN,
        1000u, TEST_TOKEN, TEST_GENERATION);
    current_task = &owner_task;

    base.client_lock.held = 0u;
    base.client_lock.ss_Owner = NULL;
    expect_finish_preflight_refusal(P4_BOARD_UPDATE_FINISH_NO_BEGIN,
        1000u, TEST_TOKEN, TEST_GENERATION);
    base.client_lock.held = 1u;
    base.client_lock.ss_Owner = &requester_task;
    expect_finish_preflight_refusal(P4_BOARD_UPDATE_FINISH_NO_BEGIN,
        1000u, TEST_TOKEN, TEST_GENERATION);
    base.client_lock.ss_Owner = &owner_task;

    host.lock.ss_Owner = &owner_task;
    expect_finish_preflight_refusal(P4_BOARD_UPDATE_FINISH_NO_BEGIN,
        1000u, TEST_TOKEN, TEST_GENERATION);
    host.lock.ss_Owner = NULL;

    base.maintenance_task = NULL;
    expect_finish_preflight_refusal(P4_BOARD_UPDATE_FINISH_NO_BEGIN,
        1000u, TEST_TOKEN, TEST_GENERATION);
    base.maintenance_task = &owner_task;
    expect_finish_preflight_refusal(P4_BOARD_UPDATE_FINISH_NO_BEGIN,
        1000u, TEST_TOKEN, TEST_GENERATION);
    base.maintenance_task = &requester_task;
    base.maintenance_state = P4_WIFI_MAINT_RELEASED;
    expect_finish_preflight_refusal(P4_BOARD_UPDATE_FINISH_NO_BEGIN,
        1000u, TEST_TOKEN, TEST_GENERATION);

    prepare_finish_active();
    CHECK(p4_wifi_owner_ota_finish(NULL, TEST_TOKEN, TEST_GENERATION,
        P4_BOARD_UPDATE_FINISH_NO_BEGIN, 1000u) == -1);
    CHECK(request_calls == 0u && discovery_acquire_calls == 0u);
    {
        struct P4WifiBase lookalike = base;
        struct P4WifiBase before;
        memcpy(&before, &lookalike, sizeof(before));
        CHECK(p4_wifi_owner_ota_finish(&lookalike, TEST_TOKEN,
            TEST_GENERATION, P4_BOARD_UPDATE_FINISH_NO_BEGIN, 1000u) == -1);
        CHECK(memcmp(&before, &lookalike, sizeof(before)) == 0);
        CHECK(request_calls == 0u && discovery_acquire_calls == 0u);
    }

    for (dirty_field = 0u; dirty_field < 7u; ++dirty_field) {
        prepare_finish_active();
        switch (dirty_field) {
        case 0: control.ota_token = TEST_TOKEN; break;
        case 1: control.ota_generation = TEST_GENERATION; break;
        case 2: control.ota_image_size = 1u; break;
        case 3: control.ota_begin_acknowledged = 1u; break;
        case 4: control.ota_acknowledged_bytes = 1u; break;
        case 5: control.ota_closed = 1u; break;
        default:
            base.maintenance_fence_mode = P4_WIFI_MAINT_FENCE_INCOMPLETE;
            break;
        }
        expect_finish_preflight_refusal(P4_BOARD_UPDATE_FINISH_NO_BEGIN,
            1000u, TEST_TOKEN, TEST_GENERATION);
    }

    prepare_finish_active();
    expect_finish_preflight_refusal(P4_BOARD_UPDATE_FINISH_COMMIT_UNKNOWN,
        1000u, TEST_TOKEN, TEST_GENERATION);
    expect_finish_preflight_refusal(P4_BOARD_UPDATE_FINISH_WAIT_REBOOT,
        1000u, TEST_TOKEN, TEST_GENERATION);
    prepare_finish_active();
    base.maintenance_fence_mode = P4_WIFI_MAINT_FENCE_INCOMPLETE;
    control.ota_closed = 1u;
    expect_finish_preflight_refusal(P4_BOARD_UPDATE_FINISH_NO_BEGIN,
        1000u, TEST_TOKEN, TEST_GENERATION);
    expect_finish_preflight_refusal(P4_BOARD_UPDATE_FINISH_COMMIT_UNKNOWN,
        1000u, TEST_TOKEN, TEST_GENERATION);
    expect_finish_preflight_refusal(P4_BOARD_UPDATE_FINISH_WAIT_REBOOT,
        1000u, TEST_TOKEN, TEST_GENERATION);
    prepare_finish_active();
    base.maintenance_fence_mode = P4_WIFI_MAINT_FENCE_COMMIT_UNKNOWN;
    control.ota_token = TEST_TOKEN;
    control.ota_generation = TEST_GENERATION;
    control.ota_image_size = 3u;
    control.ota_begin_acknowledged = 1u;
    control.ota_acknowledged_bytes = 3u;
    expect_finish_preflight_refusal(P4_BOARD_UPDATE_FINISH_WAIT_REBOOT,
        1000u, TEST_TOKEN, TEST_GENERATION);
    expect_finish_preflight_refusal(P4_BOARD_UPDATE_FINISH_INCOMPLETE,
        1000u, TEST_TOKEN, TEST_GENERATION);
}

static void test_ota_finish_completed_identity_refusals(void)
{
    unsigned invalid_field;
    for (invalid_field = 0u; invalid_field < 4u; ++invalid_field) {
        prepare_finish_active();
        switch (invalid_field) {
        case 0: control.ota_generation = TEST_GENERATION; break;
        case 1: control.ota_image_size = 1u; break;
        case 2: control.ota_begin_acknowledged = 1u; break;
        default: control.ota_acknowledged_bytes = 1u; break;
        }
        expect_finish_preflight_refusal(P4_BOARD_UPDATE_FINISH_INCOMPLETE,
            1000u, TEST_TOKEN, TEST_GENERATION);
    }

    prepare_finish_active();
    control.ota_token = TEST_TOKEN + 1u;
    control.ota_generation = TEST_GENERATION;
    control.ota_image_size = 1u;
    control.ota_begin_acknowledged = 1u;
    expect_finish_preflight_refusal(P4_BOARD_UPDATE_FINISH_INCOMPLETE,
        1000u, TEST_TOKEN, TEST_GENERATION);

    for (invalid_field = 0u; invalid_field < 8u; ++invalid_field) {
        prepare_finish_active();
        base.maintenance_fence_mode = P4_WIFI_MAINT_FENCE_COMMIT_UNKNOWN;
        control.ota_token = TEST_TOKEN;
        control.ota_generation = TEST_GENERATION;
        control.ota_image_size = 4u;
        control.ota_begin_acknowledged = 1u;
        control.ota_acknowledged_bytes = 4u;
        switch (invalid_field) {
        case 0: control.ota_token = 0u; break;
        case 1: control.ota_token = TEST_TOKEN + 1u; break;
        case 2: control.ota_generation = TEST_GENERATION + 1u; break;
        case 3: control.ota_image_size = 0u; break;
        case 4:
            control.ota_image_size = P4_BOARD_UPDATE_MAX_IMAGE_SIZE + 1u;
            break;
        case 5: control.ota_acknowledged_bytes = 5u; break;
        case 6: control.ota_begin_acknowledged = 0u; break;
        default: control.ota_acknowledged_bytes = 3u; break;
        }
        expect_finish_preflight_refusal(P4_BOARD_UPDATE_FINISH_COMMIT_UNKNOWN,
            1000u, TEST_TOKEN, TEST_GENERATION);
    }
}

static void check_begun_finish_failed(void)
{
    CHECK(base.maintenance_state == P4_WIFI_MAINT_FAILED);
    CHECK(base.maintenance_fence_mode == P4_WIFI_MAINT_FENCE_INCOMPLETE);
    CHECK(control.ota_closed == 1u);
    CHECK(base.associated == FALSE);
    CHECK(control.discovery_state == P4_WIFI_DISCOVERY_FAILED);
    CHECK(request_calls == 0u);
    CHECK(ready_calls == 0u);
}

static void test_ota_finish_begun_revalidation_failures(void)
{
    unsigned condition;
    for (condition = 0u; condition < 9u; ++condition) {
        prepare_finish_active();
        switch (condition) {
        case 0: owner.poisoned = 1u; break;
        case 1: fifo.poisoned = 1u; break;
        case 2: owner.pending_active = 1u; break;
        case 3: owner.partial_active = 1u; break;
        case 4: owner.ids.active_uid = 77u; break;
        case 5: fifo.busy = 1u; break;
        case 6: runtime.scanning = 1u; break;
        case 7: control.discovery_state = P4_WIFI_DISCOVERY_REQUESTED; break;
        default: host.dma_faulted = TRUE; break;
        }
        if (condition == 7u) {
            /* Begun finalization discards queued read-only discovery first. */
            CHECK(call_finish(P4_BOARD_UPDATE_FINISH_INCOMPLETE, 10000u,
                TEST_TOKEN, TEST_GENERATION) == 0);
            CHECK(base.maintenance_state == P4_WIFI_MAINT_FENCED);
            CHECK(base.maintenance_fence_mode ==
                  P4_WIFI_MAINT_FENCE_INCOMPLETE);
            CHECK(control.ota_closed == 1u);
            CHECK(control.discovery_state == P4_WIFI_DISCOVERY_FAILED);
            CHECK(request_calls == 0u);
            CHECK(discovery_acquire_calls == 1u);
            CHECK(discovery_release_calls == 1u);
            CHECK(finish_fence_release_observations == 1u);
            continue;
        }
        CHECK(call_finish(P4_BOARD_UPDATE_FINISH_INCOMPLETE, 10000u,
            TEST_TOKEN, TEST_GENERATION) == -1);
        check_begun_finish_failed();
        CHECK(discovery_acquire_calls == 0u);
        CHECK(discovery_release_calls == 0u);
        CHECK(discovery_stopping_calls == 0u);
        CHECK(host.lock.ss_Owner == NULL);
        expect_finish_preflight_refusal(P4_BOARD_UPDATE_FINISH_INCOMPLETE,
            10000u, TEST_TOKEN, TEST_GENERATION);
    }
}

static void test_ota_finish_acquire_release_and_lease_failures(void)
{
    prepare_finish_active();
    discovery_acquire_succeeds = FALSE;
    CHECK(call_finish(P4_BOARD_UPDATE_FINISH_INCOMPLETE, 100000u,
        TEST_TOKEN, TEST_GENERATION) == -1);
    check_begun_finish_failed();
    CHECK(discovery_acquire_calls == 1u);
    CHECK(discovery_release_calls == 0u);
    CHECK(host.lock.ss_Owner == NULL);
    CHECK(finish_stopping_unlocked_calls == 1u);

    prepare_finish_active();
    discovery_stopping = TRUE;
    CHECK(call_finish(P4_BOARD_UPDATE_FINISH_INCOMPLETE, 100000u,
        TEST_TOKEN, TEST_GENERATION) == -1);
    check_begun_finish_failed();
    CHECK(discovery_acquire_calls == 0u);
    CHECK(discovery_release_calls == 0u);
    CHECK(finish_stopping_unlocked_calls == 1u);

    prepare_finish_active();
    fake_now = base.maintenance_started + TEST_LEASE_US;
    CHECK(call_finish(P4_BOARD_UPDATE_FINISH_INCOMPLETE, 100000u,
        TEST_TOKEN, TEST_GENERATION) == -1);
    check_begun_finish_failed();
    CHECK(discovery_acquire_calls == 0u);
    CHECK(discovery_release_calls == 0u);
    CHECK(finish_stopping_unlocked_calls == 0u);

    prepare_finish_active();
    discovery_acquire_advance_us = 101u;
    CHECK(call_finish(P4_BOARD_UPDATE_FINISH_INCOMPLETE, 100u,
        TEST_TOKEN, TEST_GENERATION) == -1);
    check_begun_finish_failed();
    CHECK(discovery_acquire_calls == 1u);
    CHECK(discovery_acquire_wait == 100u);
    CHECK(discovery_release_calls == 1u);
    CHECK(finish_fence_release_observations == 0u);
    CHECK(base.ready == TRUE);
    CHECK(host.lock.ss_Owner == NULL);

    prepare_finish_active();
    discovery_release_advance_us = 101u;
    CHECK(call_finish(P4_BOARD_UPDATE_FINISH_INCOMPLETE, 100u,
        TEST_TOKEN, TEST_GENERATION) == -1);
    check_begun_finish_failed();
    CHECK(discovery_acquire_calls == 1u);
    CHECK(discovery_acquire_wait == 100u);
    CHECK(discovery_release_calls == 1u);
    CHECK(finish_fence_release_observations == 1u);
    CHECK(base.ready == FALSE);
    CHECK(host.lock.ss_Owner == NULL);
    expect_finish_preflight_refusal(P4_BOARD_UPDATE_FINISH_INCOMPLETE,
        100u, TEST_TOKEN, TEST_GENERATION);

    prepare_finish_active();
    fake_now = base.maintenance_started + TEST_LEASE_US - 50u;
    discovery_acquire_advance_us = 51u;
    CHECK(call_finish(P4_BOARD_UPDATE_FINISH_INCOMPLETE, 1000u,
        TEST_TOKEN, TEST_GENERATION) == -1);
    check_begun_finish_failed();
    CHECK(discovery_acquire_calls == 1u);
    CHECK(discovery_acquire_wait == 50u);
    CHECK(discovery_release_calls == 1u);
    CHECK(finish_fence_release_observations == 0u);
    CHECK(base.ready == TRUE);
    CHECK(host.lock.ss_Owner == NULL);

    prepare_finish_active();
    CHECK(call_finish(P4_BOARD_UPDATE_FINISH_INCOMPLETE, 1234u,
        TEST_TOKEN, TEST_GENERATION) == 0);
    CHECK(discovery_acquire_calls == 1u);
    CHECK(discovery_acquire_wait == 1234u);
    CHECK(discovery_release_calls == 1u);
    CHECK(base.maintenance_state == P4_WIFI_MAINT_FENCED);
    CHECK(base.ready == FALSE);
}

static void test_ota_finish_no_begin_deadlines_and_late_ready(void)
{
    prepare_finish_active();
    CHECK(call_finish(P4_BOARD_UPDATE_FINISH_NO_BEGIN, 3000000u,
        TEST_TOKEN, TEST_GENERATION) == 0);
    CHECK(request_calls == 1u && request_ids[0] == 280u);
    CHECK(request_budgets[0] == 3000000u);

    prepare_finish_active();
    fake_now = base.maintenance_started + TEST_LEASE_US - 2000000u;
    CHECK(call_finish(P4_BOARD_UPDATE_FINISH_NO_BEGIN, 3000000u,
        TEST_TOKEN, TEST_GENERATION) == 0);
    CHECK(request_calls == 1u && request_ids[0] == 280u);
    CHECK(request_budgets[0] == 2000000u);

    prepare_finish_active();
    fake_now = base.maintenance_started + TEST_LEASE_US - 500u;
    CHECK(call_finish(P4_BOARD_UPDATE_FINISH_NO_BEGIN, 1000u,
        TEST_TOKEN, TEST_GENERATION) == 0);
    CHECK(request_calls == 1u && request_ids[0] == 280u);
    CHECK(request_budgets[0] == 500u);

    prepare_finish_active();
    ready_advance_us = 101u;
    CHECK(call_finish(P4_BOARD_UPDATE_FINISH_NO_BEGIN, 100u,
        TEST_TOKEN, TEST_GENERATION) == -1);
    CHECK(base.maintenance_state == P4_WIFI_MAINT_FAILED);
    CHECK(base.maintenance_fence_mode == 0u);
    CHECK(base.associated == FALSE);
    CHECK(control.ota_closed == 0u);
    CHECK(ready_calls == 1u);
    CHECK(request_calls == 1u && request_ids[0] == 280u);
    CHECK(request_budgets[0] == 100u);
    CHECK(discovery_acquire_calls == 0u && discovery_release_calls == 0u);
}

static void test_ota_finish_generation_change_during_acquire(void)
{
    prepare_finish_active();
    discovery_change_host_generation_on_acquire = TRUE;
    CHECK(call_finish(P4_BOARD_UPDATE_FINISH_INCOMPLETE, 100000u,
        TEST_TOKEN, TEST_GENERATION) == -1);
    check_begun_finish_failed();
    CHECK(host.generation == TEST_GENERATION + 1u);
    CHECK(discovery_acquire_calls == 1u);
    CHECK(discovery_acquire_wait == P4_HOSTED_OWNER_MAX_ACQUIRE_US);
    CHECK(tx_session_calls == 1u);
    CHECK(discovery_release_calls == 1u);
    CHECK(finish_fence_release_observations == 0u);
    CHECK(discovery_stopping_calls == 1u);
    CHECK(host.lock.ss_Owner == NULL);
    CHECK(base.ready == TRUE);
    CHECK(request_calls == 0u);
    expect_finish_preflight_refusal(P4_BOARD_UPDATE_FINISH_INCOMPLETE,
        100000u, TEST_TOKEN, TEST_GENERATION);
}

static void test_ota_finish_no_begin_request_failure(void)
{
    prepare_finish_active();
    request_succeeds = FALSE;
    CHECK(call_finish(P4_BOARD_UPDATE_FINISH_NO_BEGIN, 9000000u,
        TEST_TOKEN, TEST_GENERATION) == -1);
    CHECK(base.maintenance_state == P4_WIFI_MAINT_FAILED);
    CHECK(base.maintenance_fence_mode == 0u);
    CHECK(base.associated == FALSE);
    CHECK(base.ready == TRUE);
    CHECK(control.ota_closed == 0u);
    CHECK(control.discovery_state == P4_WIFI_DISCOVERY_FAILED);
    CHECK(request_calls == 1u && request_ids[0] == 280u);
    CHECK(request_budgets[0] == 8000000u);
    CHECK(finish_no_begin_clear_observations == 1u);
    CHECK(ready_calls == 0u);
    CHECK(discovery_acquire_calls == 0u);
    CHECK(discovery_release_calls == 0u);
    CHECK(host.lock.ss_Owner == NULL);
    expect_finish_preflight_refusal(P4_BOARD_UPDATE_FINISH_NO_BEGIN,
        9000000u, TEST_TOKEN, TEST_GENERATION);
    CHECK(request_calls == 1u && ready_calls == 0u);
}

static ULONG begin_request(void)
{
    ULONG token = wifi_maintenance_request(&base);
    CHECK(token != 0u);
    CHECK(token == TEST_TOKEN + 1u);
    CHECK(base.maintenance_state == P4_WIFI_MAINT_REQUESTED);
    CHECK(base.maintenance_task == current_task);
    CHECK(base.maintenance_started == fake_now);
    CHECK(base.client_lock.held == 0u);
    return token;
}

static void prepare_service(ULONG state)
{
    reset_fixture();
    base.maintenance_state = state;
    base.maintenance_started = fake_now;
    base.associated = TRUE;
    base.link_state = P4_WIFI_LINK_UP;
    memset(&base.confirmed_ap, 0xa5, sizeof(base.confirmed_ap));
    memset(&base.observed_ap, 0x5a, sizeof(base.observed_ap));
}

static void prepare_discovery_active(void)
{
    prepare_service(P4_WIFI_MAINT_REQUESTED);
    CHECK(service_step() == 2);
    CHECK(base.maintenance_state == P4_WIFI_MAINT_ACTIVE);
    request_calls = 0u;
    reset_discovery_mocks();
}

static void queue_discovery(void)
{
    CHECK(wifi_maintenance_discover(&base, TEST_TOKEN));
    CHECK(control.discovery_state == P4_WIFI_DISCOVERY_REQUESTED);
    CHECK(control.discovery_nonce == 1u);
    CHECK(control.discovery_token == TEST_TOKEN);
}

static void fill_ready_result(LONG remote_status)
{
    size_t i;
    memset(&control.discovery_result, 0, sizeof(control.discovery_result));
    control.discovery_result.token = TEST_TOKEN;
    control.discovery_result.generation = TEST_GENERATION;
    control.discovery_result.uid = 100u;
    control.discovery_result.nonce = control.discovery_nonce;
    control.discovery_result.remote_status = remote_status;
    control.discovery_result.inventory_size = remote_status ? 0u : 48u;
    for (i = 0; i < control.discovery_result.inventory_size; ++i)
        control.discovery_result.inventory[i] = (UBYTE)(0x40u + i);
    control.discovery_token = TEST_TOKEN;
    control.discovery_state = P4_WIFI_DISCOVERY_READY;
}

static BOOL bytes_are_zero(const void *memory, size_t size)
{
    const UBYTE *bytes = memory;
    size_t i;
    for (i = 0; i < size; ++i)
        if (bytes[i] != 0u) return FALSE;
    return TRUE;
}

static void check_discovery_storage_cleared(ULONG expected_state,
                                            ULONG expected_nonce)
{
    CHECK(control.discovery_state == expected_state);
    CHECK(control.discovery_nonce == expected_nonce);
    CHECK(control.discovery_token == 0u);
    CHECK(bytes_are_zero(&control.discovery_result,
                         sizeof(control.discovery_result)));
    CHECK(bytes_are_zero(&control.discovery_decoded,
                         sizeof(control.discovery_decoded)));
    CHECK(bytes_are_zero(control.discovery_response,
                         sizeof(control.discovery_response)));
}

static void fill_output(struct p4_wifi_discovery_result *output, UBYTE value)
{
    memset(output, value, sizeof(*output));
}

static void test_discovery_queue_admission_and_nonce(void)
{
    ULONG nonce;
    struct p4_wifi_discovery_result saved_result;

    prepare_discovery_active();
    CHECK(!wifi_maintenance_discover(NULL, TEST_TOKEN));
    CHECK(!wifi_maintenance_discover(&base, 0u));
    CHECK(!wifi_maintenance_discover(&base, TEST_TOKEN + 1u));
    CHECK(control.discovery_state == P4_WIFI_DISCOVERY_IDLE);
    CHECK(control.discovery_nonce == 0u);

    current_task = &other_task;
    CHECK(!wifi_maintenance_discover(&base, TEST_TOKEN));
    current_task = &requester_task;
    base.maintenance_state = P4_WIFI_MAINT_REQUESTED;
    CHECK(!wifi_maintenance_discover(&base, TEST_TOKEN));
    base.maintenance_state = P4_WIFI_MAINT_ACTIVE;

    current_task = &owner_task;
    CHECK(!wifi_maintenance_discover(&base, TEST_TOKEN));
    current_task = &requester_task;

    fake_now = base.maintenance_started + TEST_LEASE_US;
    CHECK(!wifi_maintenance_discover(&base, TEST_TOKEN));
    CHECK(control.discovery_state == P4_WIFI_DISCOVERY_IDLE);
    CHECK(control.discovery_nonce == 0u);
    fake_now = base.maintenance_started;

    queue_discovery();
    memset(&control.discovery_result, 0x6b, sizeof(control.discovery_result));
    memcpy(&saved_result, &control.discovery_result, sizeof(saved_result));
    nonce = control.discovery_nonce;
    CHECK(!wifi_maintenance_discover(&base, TEST_TOKEN));
    CHECK(control.discovery_state == P4_WIFI_DISCOVERY_REQUESTED);
    CHECK(control.discovery_nonce == nonce);
    CHECK(control.discovery_token == TEST_TOKEN);
    CHECK(memcmp(&control.discovery_result, &saved_result,
                 sizeof(saved_result)) == 0);

    fill_ready_result(0);
    CHECK(wifi_maintenance_discover(&base, TEST_TOKEN));
    CHECK(control.discovery_state == P4_WIFI_DISCOVERY_REQUESTED);
    CHECK(control.discovery_nonce == nonce + 1u);
    CHECK(control.discovery_token == TEST_TOKEN);
    CHECK(bytes_are_zero(&control.discovery_result,
                         sizeof(control.discovery_result)));

    control.discovery_nonce = UINT32_MAX;
    control.discovery_state = P4_WIFI_DISCOVERY_READY;
    control.discovery_token = TEST_TOKEN;
    memset(&control.discovery_result, 0x3c, sizeof(control.discovery_result));
    memcpy(&saved_result, &control.discovery_result, sizeof(saved_result));
    CHECK(!wifi_maintenance_discover(&base, TEST_TOKEN));
    CHECK(control.discovery_nonce == UINT32_MAX);
    CHECK(control.discovery_state == P4_WIFI_DISCOVERY_READY);
    CHECK(control.discovery_token == TEST_TOKEN);
    CHECK(memcmp(&control.discovery_result, &saved_result,
                 sizeof(saved_result)) == 0);
}

static void test_discovery_poll_status_copy_and_output_guard(void)
{
    struct p4_wifi_discovery_result output, expected;
    struct p4_wifi_discovery_result *overflowing;
    ULONG status;

    prepare_discovery_active();
    fill_output(&output, 0xa7u);
    memcpy(&expected, &output, sizeof(expected));
    CHECK(wifi_maintenance_discovery_poll(&base, TEST_TOKEN, &output) ==
          P4_WIFI_DISCOVERY_IDLE);
    CHECK(memcmp(&output, &expected, sizeof(output)) == 0);

    queue_discovery();
    CHECK(wifi_maintenance_discovery_poll(&base, TEST_TOKEN, &output) ==
          P4_WIFI_DISCOVERY_REQUESTED);
    CHECK(memcmp(&output, &expected, sizeof(output)) == 0);
    CHECK(wifi_maintenance_discovery_poll(NULL, TEST_TOKEN, &output) ==
          P4_WIFI_DISCOVERY_FAILED);
    CHECK(wifi_maintenance_discovery_poll(&base, 0u, &output) ==
          P4_WIFI_DISCOVERY_FAILED);

    current_task = &other_task;
    CHECK(wifi_maintenance_discovery_poll(&base, TEST_TOKEN, &output) ==
          P4_WIFI_DISCOVERY_FAILED);
    current_task = &requester_task;
    CHECK(wifi_maintenance_discovery_poll(&base, TEST_TOKEN + 1u, &output) ==
          P4_WIFI_DISCOVERY_FAILED);
    CHECK(memcmp(&output, &expected, sizeof(output)) == 0);

    base.client_lock.held = 1u;
    CHECK(wifi_maintenance_discovery_poll(&base, TEST_TOKEN, &output) ==
          P4_WIFI_DISCOVERY_BUSY);
    CHECK(memcmp(&output, &expected, sizeof(output)) == 0);
    base.client_lock.held = 0u;

    fill_ready_result(-9);
    status = wifi_maintenance_discovery_poll(&base, TEST_TOKEN, &output);
    CHECK(status == P4_WIFI_DISCOVERY_READY);
    CHECK(memcmp(&output, &control.discovery_result, sizeof(output)) == 0);
    CHECK(output.remote_status == -9);

    /* Every published identity/bound field is checked before copying. */
    fill_ready_result(0);
    control.discovery_result.token++;
    fill_output(&output, 0x61u);
    memcpy(&expected, &output, sizeof(expected));
    CHECK(wifi_maintenance_discovery_poll(&base, TEST_TOKEN, &output) ==
          P4_WIFI_DISCOVERY_FAILED);
    CHECK(memcmp(&output, &expected, sizeof(output)) == 0);
    fill_ready_result(0);
    ++control.discovery_result.generation;
    CHECK(wifi_maintenance_discovery_poll(&base, TEST_TOKEN, &output) ==
          P4_WIFI_DISCOVERY_FAILED);
    CHECK(memcmp(&output, &expected, sizeof(output)) == 0);
    fill_ready_result(0);
    control.discovery_result.uid = 0u;
    CHECK(wifi_maintenance_discovery_poll(&base, TEST_TOKEN, &output) ==
          P4_WIFI_DISCOVERY_FAILED);
    CHECK(memcmp(&output, &expected, sizeof(output)) == 0);
    fill_ready_result(0);
    control.discovery_result.nonce = 0u;
    CHECK(wifi_maintenance_discovery_poll(&base, TEST_TOKEN, &output) ==
          P4_WIFI_DISCOVERY_FAILED);
    CHECK(memcmp(&output, &expected, sizeof(output)) == 0);
    fill_ready_result(0);
    ++control.discovery_result.nonce;
    CHECK(wifi_maintenance_discovery_poll(&base, TEST_TOKEN, &output) ==
          P4_WIFI_DISCOVERY_FAILED);
    CHECK(memcmp(&output, &expected, sizeof(output)) == 0);
    fill_ready_result(0);
    control.discovery_result.inventory_size = P4_WIFI_DISCOVERY_MAX + 1u;
    CHECK(wifi_maintenance_discovery_poll(&base, TEST_TOKEN, &output) ==
          P4_WIFI_DISCOVERY_FAILED);
    CHECK(memcmp(&output, &expected, sizeof(output)) == 0);
    fill_ready_result(0);
    control.discovery_token++;
    CHECK(wifi_maintenance_discovery_poll(&base, TEST_TOKEN, &output) ==
          P4_WIFI_DISCOVERY_FAILED);
    CHECK(memcmp(&output, &expected, sizeof(output)) == 0);

    fill_ready_result(0);
    control.discovery_result.inventory_size = P4_WIFI_DISCOVERY_MAX;
    CHECK(wifi_maintenance_discovery_poll(&base, TEST_TOKEN, &output) ==
          P4_WIFI_DISCOVERY_READY);
    CHECK(output.inventory_size == P4_WIFI_DISCOVERY_MAX);

    overflowing = (struct p4_wifi_discovery_result *)(uintptr_t)
        (UINTPTR_MAX - 8u);
    CHECK(!wifi_discovery_output_valid(&base, &runtime, NULL));
    CHECK(!wifi_discovery_output_valid(&base, &runtime, overflowing));
    CHECK(!wifi_discovery_output_valid(&base, &runtime,
        (struct p4_wifi_discovery_result *)(void *)&base));
    CHECK(!wifi_discovery_output_valid(&base, &runtime,
        (struct p4_wifi_discovery_result *)(void *)&runtime));
    CHECK(!wifi_discovery_output_valid(&base, &runtime,
        (struct p4_wifi_discovery_result *)(void *)&context));
    CHECK(!wifi_discovery_output_valid(&base, &runtime,
        (struct p4_wifi_discovery_result *)(void *)&owner));
    CHECK(!wifi_discovery_output_valid(&base, &runtime,
        (struct p4_wifi_discovery_result *)(void *)&control));
    CHECK(!wifi_discovery_output_valid(&base, &runtime,
        (struct p4_wifi_discovery_result *)(void *)&fifo));
    CHECK(!wifi_discovery_output_valid(&base, &runtime,
        (struct p4_wifi_discovery_result *)(void *)&host));
    CHECK(!wifi_discovery_output_valid(&base, &runtime,
        (struct p4_wifi_discovery_result *)(void *)&tx_ops));
    CHECK(!wifi_discovery_output_valid(&base, &runtime,
        (struct p4_wifi_discovery_result *)(void *)&init));
    CHECK(!wifi_discovery_output_valid(&base, &runtime,
        (struct p4_wifi_discovery_result *)(void *)&callbacks));

    fill_output(&output, 0x19u);
    memcpy(&expected, &output, sizeof(expected));
    CHECK(wifi_maintenance_discovery_poll(&base, TEST_TOKEN,
        (struct p4_wifi_discovery_result *)(void *)&control) ==
        P4_WIFI_DISCOVERY_FAILED);
    CHECK(memcmp(&output, &expected, sizeof(output)) == 0);
}

static void test_sticky_fence_blocks_discovery_poll(void)
{
    struct p4_wifi_discovery_result output, expected_output, expected_result;
    struct p4_wifi_discovery_result *invalid_output =
        (struct p4_wifi_discovery_result *)(uintptr_t)(UINTPTR_MAX - 8u);
    ULONG mode;

    for (mode = P4_WIFI_MAINT_FENCE_INCOMPLETE;
         mode <= P4_WIFI_MAINT_FENCE_WAIT_REBOOT; ++mode) {
        prepare_discovery_active();
        queue_discovery();
        fill_ready_result(0);
        CHECK(control.discovery_state == P4_WIFI_DISCOVERY_READY);
        memcpy(&expected_result, &control.discovery_result,
               sizeof(expected_result));
        base.maintenance_fence_mode = mode;

        fill_output(&output, 0xa1u);
        memcpy(&expected_output, &output, sizeof(expected_output));
        CHECK(wifi_maintenance_discovery_poll(&base, TEST_TOKEN, &output) ==
              P4_WIFI_DISCOVERY_FAILED);
        CHECK(memcmp(&output, &expected_output, sizeof(output)) == 0);
        CHECK(wifi_maintenance_discovery_poll(&base, TEST_TOKEN + 1u,
                                               &output) ==
              P4_WIFI_DISCOVERY_FAILED);
        CHECK(memcmp(&output, &expected_output, sizeof(output)) == 0);
        CHECK(wifi_maintenance_discovery_poll(&base, TEST_TOKEN, NULL) ==
              P4_WIFI_DISCOVERY_FAILED);
        CHECK(wifi_maintenance_discovery_poll(&base, TEST_TOKEN,
                                               invalid_output) ==
              P4_WIFI_DISCOVERY_FAILED);
        CHECK(memcmp(&output, &expected_output, sizeof(output)) == 0);

        CHECK(base.maintenance_state == P4_WIFI_MAINT_ACTIVE);
        CHECK(base.maintenance_fence_mode == mode);
        CHECK(control.discovery_state == P4_WIFI_DISCOVERY_READY);
        CHECK(memcmp(&control.discovery_result, &expected_result,
                     sizeof(expected_result)) == 0);
        CHECK(request_calls == 0u);
        CHECK(discovery_supported_calls == 0u);
        CHECK(discovery_reserve_calls == 0u);
        CHECK(discovery_request_calls == 0u);
        CHECK(discovery_exchange_calls == 0u);
        CHECK(discovery_rpc_encode_calls == 0u);
        CHECK(discovery_response_calls == 0u);
        CHECK(discovery_inventory_calls == 0u);
        CHECK(discovery_acquire_calls == 0u);
        CHECK(discovery_release_calls == 0u);
        CHECK(discovery_cancel_calls == 0u);
    }
}

static void test_discovery_service_success_unsupported_and_remote_error(void)
{
    struct p4_wifi_discovery_result output, untouched;
    size_t i;

    prepare_discovery_active();
    queue_discovery();
    memset(control.request, 0x91, sizeof(control.request));
    CHECK(service_step() == 2);
    CHECK(base.maintenance_state == P4_WIFI_MAINT_ACTIVE);
    CHECK(control.discovery_state == P4_WIFI_DISCOVERY_READY);
    CHECK(control.discovery_token == TEST_TOKEN);
    CHECK(control.discovery_result.token == TEST_TOKEN);
    CHECK(control.discovery_result.generation == TEST_GENERATION);
    CHECK(control.discovery_result.uid == 100u);
    CHECK(control.discovery_result.nonce == 1u);
    CHECK(control.discovery_result.remote_status == 0);
    CHECK(control.discovery_result.inventory_size == 48u);
    for (i = 0; i < control.discovery_result.inventory_size; ++i)
        CHECK(control.discovery_result.inventory[i] ==
              (UBYTE)((1u ^ 0x5a5a5a5au) + (unsigned)i));
    CHECK(discovery_supported_calls == 1u);
    CHECK(discovery_reserve_calls == 1u);
    CHECK(discovery_request_calls == 1u);
    CHECK(discovery_exchange_calls == 1u);
    CHECK(discovery_rpc_encode_calls == 1u);
    CHECK(discovery_response_calls == 1u);
    CHECK(discovery_inventory_calls == 1u);
    CHECK(discovery_acquire_calls == 1u);
    CHECK(discovery_release_calls == 1u);
    CHECK(discovery_cancel_calls == 0u);
    CHECK(bytes_are_zero(control.request, sizeof(control.request)));
    CHECK(bytes_are_zero(&control.discovery_decoded,
                         sizeof(control.discovery_decoded)));
    CHECK(bytes_are_zero(control.discovery_response,
                         sizeof(control.discovery_response)));
    fill_output(&output, 0u);
    CHECK(wifi_maintenance_discovery_poll(&base, TEST_TOKEN, &output) ==
          P4_WIFI_DISCOVERY_READY);
    CHECK(output.token == TEST_TOKEN && output.generation == TEST_GENERATION);
    CHECK(output.uid == 100u && output.nonce == 1u);
    CHECK(output.inventory_size == 48u);

    prepare_discovery_active();
    queue_discovery();
    init.discovery_supported = 0u;
    CHECK(service_step() == 2);
    CHECK(control.discovery_state == P4_WIFI_DISCOVERY_UNSUPPORTED);
    CHECK(discovery_supported_calls == 1u);
    CHECK(discovery_reserve_calls == 0u);
    CHECK(discovery_request_calls == 0u);
    CHECK(discovery_exchange_calls == 0u);
    CHECK(discovery_rpc_encode_calls == 0u);
    CHECK(discovery_acquire_calls == 0u);
    fill_output(&output, 0x84u);
    memcpy(&untouched, &output, sizeof(output));
    CHECK(wifi_maintenance_discovery_poll(&base, TEST_TOKEN, &output) ==
          P4_WIFI_DISCOVERY_UNSUPPORTED);
    CHECK(memcmp(&output, &untouched, sizeof(output)) == 0);

    prepare_discovery_active();
    queue_discovery();
    discovery_remote_status = -23;
    CHECK(service_step() == 2);
    CHECK(control.discovery_state == P4_WIFI_DISCOVERY_READY);
    CHECK(control.discovery_result.remote_status == -23);
    CHECK(control.discovery_result.inventory_size == 0u);
    CHECK(bytes_are_zero(control.discovery_result.inventory,
                         sizeof(control.discovery_result.inventory)));
    CHECK(discovery_inventory_calls == 0u);
    CHECK(discovery_cancel_calls == 0u);
    fill_output(&output, 0u);
    CHECK(wifi_maintenance_discovery_poll(&base, TEST_TOKEN, &output) ==
          P4_WIFI_DISCOVERY_READY);
    CHECK(output.remote_status == -23 && output.inventory_size == 0u);
}

static void test_discovery_service_deadline_and_failure_fences(void)
{
    ULONG nonce;

    prepare_discovery_active();
    queue_discovery();
    discovery_request_advance_us = 1250000u;
    CHECK(service_step() == 2);
    CHECK(discovery_exchange_timeout == 6750000u);
    CHECK(discovery_acquire_wait == P4_HOSTED_OWNER_MAX_ACQUIRE_US);

    prepare_discovery_active();
    queue_discovery();
    nonce = control.discovery_nonce;
    discovery_request_advance_us = 8000000u;
    CHECK(service_step() == 0);
    CHECK(discovery_exchange_calls == 0u);
    CHECK(base.maintenance_state == P4_WIFI_MAINT_FAILED);
    check_discovery_storage_cleared(P4_WIFI_DISCOVERY_FAILED, nonce);

    prepare_discovery_active();
    queue_discovery();
    fake_now = 30000000u;
    base.maintenance_started = 5000000u;
    discovery_request_advance_us = 1000000u;
    CHECK(service_step() == 2);
    CHECK(discovery_exchange_timeout == 4000000u);
    CHECK(control.discovery_state == P4_WIFI_DISCOVERY_READY);

    prepare_discovery_active();
    queue_discovery();
    nonce = control.discovery_nonce;
    discovery_acquire_succeeds = FALSE;
    CHECK(service_step() == 0);
    CHECK(base.maintenance_state == P4_WIFI_MAINT_FAILED);
    CHECK(discovery_cancel_calls == 1u);
    check_discovery_storage_cleared(P4_WIFI_DISCOVERY_FAILED, nonce);

    prepare_discovery_active();
    queue_discovery();
    nonce = control.discovery_nonce;
    discovery_change_host_generation_on_acquire = TRUE;
    CHECK(service_step() == 0);
    CHECK(base.maintenance_state == P4_WIFI_MAINT_FAILED);
    CHECK(discovery_release_calls == 1u);
    CHECK(discovery_cancel_calls == 1u);
    check_discovery_storage_cleared(P4_WIFI_DISCOVERY_FAILED, nonce);

    prepare_discovery_active();
    queue_discovery();
    nonce = control.discovery_nonce;
    discovery_acquire_advance_us = 8000000u;
    CHECK(service_step() == 0);
    CHECK(base.maintenance_state == P4_WIFI_MAINT_FAILED);
    CHECK(discovery_release_calls == 1u);
    check_discovery_storage_cleared(P4_WIFI_DISCOVERY_FAILED, nonce);

    prepare_discovery_active();
    queue_discovery();
    nonce = control.discovery_nonce;
    base.maintenance_started = fake_now - (TEST_LEASE_US - 500000u);
    discovery_acquire_advance_us = 500001u;
    CHECK(service_step() == 0);
    CHECK(base.maintenance_state == P4_WIFI_MAINT_FAILED);
    CHECK(discovery_release_calls == 1u);
    check_discovery_storage_cleared(P4_WIFI_DISCOVERY_FAILED, nonce);

    prepare_discovery_active();
    queue_discovery();
    nonce = control.discovery_nonce;
    host.lock.ss_Owner = &owner_task;
    CHECK(service_step() == 0);
    CHECK(discovery_supported_calls == 0u);
    CHECK(discovery_reserve_calls == 0u);
    CHECK(discovery_exchange_calls == 0u);
    CHECK(discovery_acquire_calls == 0u);
    CHECK(discovery_cancel_calls == 1u);
    check_discovery_storage_cleared(P4_WIFI_DISCOVERY_FAILED, nonce);
    host.lock.ss_Owner = NULL;
}

static void test_discovery_poll_retires_expired_or_changed_session(void)
{
    struct p4_wifi_discovery_result output, untouched;
    unsigned change;
    for (change = 0; change < 2u; ++change) {
        prepare_discovery_active();
        queue_discovery();
        fill_ready_result(0);
        fill_output(&output, 0x7cu);
        memcpy(&untouched, &output, sizeof(output));
        if (change) ++host.generation;
        else fake_now = base.maintenance_started + TEST_LEASE_US;
        CHECK(wifi_maintenance_discovery_poll(&base, TEST_TOKEN, &output) ==
              P4_WIFI_DISCOVERY_FAILED);
        CHECK(base.maintenance_state == P4_WIFI_MAINT_FAILED);
        CHECK(memcmp(&output, &untouched, sizeof(output)) == 0);
        check_discovery_storage_cleared(P4_WIFI_DISCOVERY_FAILED, 1u);
    }
}

static void test_discovery_correlations_and_cleanup(void)
{
    ULONG nonce;
    unsigned corruption;

    for (corruption = 0u; corruption < 3u; ++corruption) {
        prepare_discovery_active();
        queue_discovery();
        nonce = control.discovery_nonce;
        if (corruption == 0u) discovery_reply_id_delta = 1u;
        else if (corruption == 1u) discovery_reply_uid_delta = 1u;
        else discovery_reply_nonce_delta = 1u;
        CHECK(service_step() == 0);
        CHECK(base.maintenance_state == P4_WIFI_MAINT_FAILED);
        CHECK(discovery_cancel_calls == 1u);
        CHECK(discovery_inventory_calls == 0u);
        check_discovery_storage_cleared(P4_WIFI_DISCOVERY_FAILED, nonce);
        CHECK(bytes_are_zero(control.request, sizeof(control.request)));
    }

    prepare_discovery_active();
    queue_discovery();
    nonce = control.discovery_nonce;
    fill_ready_result(0);
    memset(&control.discovery_decoded, 0x4d, sizeof(control.discovery_decoded));
    memset(control.discovery_response, 0x4e, sizeof(control.discovery_response));
    CHECK(wifi_maintenance_release(&base, TEST_TOKEN));
    CHECK(base.maintenance_state == P4_WIFI_MAINT_RELEASING);
    check_discovery_storage_cleared(P4_WIFI_DISCOVERY_IDLE, nonce);
    CHECK(service_step() == 1);
    CHECK(base.maintenance_state == P4_WIFI_MAINT_RELEASED);
    CHECK(wifi_maintenance_request(&base) == TEST_TOKEN + 1u);
    CHECK(base.maintenance_state == P4_WIFI_MAINT_REQUESTED);
    CHECK(control.discovery_nonce == nonce);
    CHECK(control.discovery_state == P4_WIFI_DISCOVERY_IDLE);
    CHECK(bytes_are_zero(&control.discovery_result,
                         sizeof(control.discovery_result)));

    /* The lifecycle's teardown call is source-guarded by the runner. Exercise
     * the same extracted clearing primitive and verify nonce retention. */
    memset(&control.discovery_result, 0x31, sizeof(control.discovery_result));
    memset(&control.discovery_decoded, 0x32, sizeof(control.discovery_decoded));
    memset(control.discovery_response, 0x33, sizeof(control.discovery_response));
    control.discovery_token = TEST_TOKEN + 1u;
    wifi_maintenance_discovery_clear(&runtime, P4_WIFI_DISCOVERY_FAILED);
    check_discovery_storage_cleared(P4_WIFI_DISCOVERY_FAILED, nonce);
}

static void test_request_identity_and_lock_contention(void)
{
    ULONG token;

    reset_fixture();
    CHECK(wifi_maintenance_request(NULL) == 0u);
    CHECK(wifi_maintenance_request(&base) == TEST_TOKEN + 1u);
    CHECK(base.maintenance_state == P4_WIFI_MAINT_REQUESTED);
    CHECK(base.maintenance_started == 1000u);

    reset_fixture();
    base.client_lock.held = 1u;
    CHECK(wifi_maintenance_request(&base) == 0u);
    CHECK(wifi_maintenance_poll(&base, TEST_TOKEN) == P4_WIFI_MAINT_BUSY);
    CHECK(!wifi_maintenance_release(&base, TEST_TOKEN));
    CHECK(base.client_lock.held == 1u);
    base.client_lock.held = 0u;

    reset_fixture();
    base.ready = FALSE;
    CHECK(wifi_maintenance_request(&base) == 0u);
    reset_fixture();
    base.runtime = NULL;
    CHECK(wifi_maintenance_request(&base) == 0u);
    reset_fixture();
    runtime.context = NULL;
    CHECK(wifi_maintenance_request(&base) == 0u);
    reset_fixture();
    context.host = NULL;
    CHECK(wifi_maintenance_request(&base) == 0u);
    reset_fixture();
    base.owner_task = NULL;
    CHECK(wifi_maintenance_request(&base) == 0u);
    reset_fixture();
    base.owner_task = current_task;
    CHECK(wifi_maintenance_request(&base) == 0u);
    reset_fixture();
    base.maintenance_state = P4_WIFI_MAINT_ACTIVE;
    CHECK(wifi_maintenance_request(&base) == 0u);
    reset_fixture();
    base.maintenance_state = P4_WIFI_MAINT_IDLE;
    base.maintenance_token = UINT32_MAX;
    CHECK(wifi_maintenance_request(&base) == 0u);
    CHECK(base.maintenance_token == UINT32_MAX);

    reset_fixture();
    token = begin_request();
    CHECK(wifi_maintenance_poll(&base, 0u) == P4_WIFI_MAINT_FAILED);
    CHECK(wifi_maintenance_poll(NULL, token) == P4_WIFI_MAINT_FAILED);
    CHECK(wifi_maintenance_poll(&base, token + 1u) == P4_WIFI_MAINT_FAILED);
    current_task = &other_task;
    CHECK(wifi_maintenance_poll(&base, token) == P4_WIFI_MAINT_FAILED);
    CHECK(!wifi_maintenance_release(&base, token));
    CHECK(base.maintenance_state == P4_WIFI_MAINT_REQUESTED);
}

static void check_fence_request_refused_atomic(ULONG token, ULONG mode)
{
    struct P4WifiBase base_before;
    struct wifi_owner_control control_before;
    struct p4_hosted_owner owner_before;
    struct p4_hosted_fifo fifo_before;
    struct P4SDMMCHost host_before;
    unsigned request_before = request_calls;
    unsigned acquire_before = discovery_acquire_calls;
    unsigned release_before = discovery_release_calls;
    unsigned tx_before = tx_session_calls;

    memcpy(&base_before, &base, sizeof(base_before));
    memcpy(&control_before, &control, sizeof(control_before));
    memcpy(&owner_before, &owner, sizeof(owner_before));
    memcpy(&fifo_before, &fifo, sizeof(fifo_before));
    memcpy(&host_before, &host, sizeof(host_before));

    CHECK(!wifi_maintenance_fence(&base, token, mode));
    CHECK(memcmp(&base_before, &base, sizeof(base_before)) == 0);
    CHECK(memcmp(&control_before, &control, sizeof(control_before)) == 0);
    CHECK(memcmp(&owner_before, &owner, sizeof(owner_before)) == 0);
    CHECK(memcmp(&fifo_before, &fifo, sizeof(fifo_before)) == 0);
    CHECK(memcmp(&host_before, &host, sizeof(host_before)) == 0);
    CHECK(request_calls == request_before);
    CHECK(discovery_acquire_calls == acquire_before);
    CHECK(discovery_release_calls == release_before);
    CHECK(tx_session_calls == tx_before);
}

static void queue_terminal_fence(ULONG mode)
{
    prepare_discovery_active();
    CHECK(wifi_maintenance_cleanup_allowed(&base));
    CHECK(wifi_maintenance_fence(&base, TEST_TOKEN, mode));
}

static void test_terminal_fence_admission(void)
{
    static const ULONG inactive_states[] = {
        P4_WIFI_MAINT_IDLE, P4_WIFI_MAINT_REQUESTED,
        P4_WIFI_MAINT_RELEASING, P4_WIFI_MAINT_FAILED,
        P4_WIFI_MAINT_RELEASED, P4_WIFI_MAINT_BUSY,
        P4_WIFI_MAINT_FENCING, P4_WIFI_MAINT_FENCED
    };
    ULONG mode;
    size_t index;
    unsigned stopped_before, ready_before;

    CHECK(!wifi_maintenance_fence(NULL, TEST_TOKEN,
                                  P4_WIFI_MAINT_FENCE_INCOMPLETE));

    for (mode = P4_WIFI_MAINT_FENCE_INCOMPLETE;
         mode <= P4_WIFI_MAINT_FENCE_WAIT_REBOOT; ++mode) {
        prepare_discovery_active();
        stopped_before = stopped_calls;
        ready_before = ready_calls;
        CHECK(wifi_maintenance_cleanup_allowed(&base));
        CHECK(wifi_maintenance_fence(&base, TEST_TOKEN, mode));
        CHECK(base.maintenance_fence_mode == mode);
        CHECK(base.maintenance_state == P4_WIFI_MAINT_FENCING);
        CHECK(base.ready == TRUE);
        CHECK(base.associated == FALSE);
        CHECK(!wifi_maintenance_cleanup_allowed(&base));
        CHECK(wifi_maintenance_poll(&base, TEST_TOKEN) ==
              P4_WIFI_MAINT_FENCING);
        CHECK(discovery_acquire_calls == 0u);
        CHECK(tx_session_calls == 0u);
        CHECK(request_calls == 0u);
        CHECK(stopped_calls == stopped_before);
        CHECK(ready_calls == ready_before);
        check_discovery_storage_cleared(P4_WIFI_DISCOVERY_FAILED, 0u);
    }

    prepare_discovery_active();
    check_fence_request_refused_atomic(TEST_TOKEN, 0u);
    check_fence_request_refused_atomic(TEST_TOKEN,
                                       P4_WIFI_MAINT_FENCE_WAIT_REBOOT + 1u);
    check_fence_request_refused_atomic(0u, P4_WIFI_MAINT_FENCE_INCOMPLETE);
    check_fence_request_refused_atomic(TEST_TOKEN + 1u,
                                       P4_WIFI_MAINT_FENCE_INCOMPLETE);

    current_task = &other_task;
    check_fence_request_refused_atomic(TEST_TOKEN,
                                       P4_WIFI_MAINT_FENCE_INCOMPLETE);
    current_task = &requester_task;

    base.maintenance_task = &other_task;
    check_fence_request_refused_atomic(TEST_TOKEN,
                                       P4_WIFI_MAINT_FENCE_INCOMPLETE);
    base.maintenance_task = &requester_task;

    base.maintenance_task = &owner_task;
    current_task = &owner_task;
    check_fence_request_refused_atomic(TEST_TOKEN,
                                       P4_WIFI_MAINT_FENCE_INCOMPLETE);
    current_task = &requester_task;
    base.maintenance_task = &requester_task;

    base.client_lock.held = 1u;
    check_fence_request_refused_atomic(TEST_TOKEN,
                                       P4_WIFI_MAINT_FENCE_INCOMPLETE);
    base.client_lock.held = 0u;

    for (index = 0u;
         index < sizeof(inactive_states) / sizeof(inactive_states[0]); ++index) {
        prepare_discovery_active();
        base.maintenance_state = inactive_states[index];
        check_fence_request_refused_atomic(TEST_TOKEN,
                                           P4_WIFI_MAINT_FENCE_INCOMPLETE);
    }

    prepare_discovery_active();
    fake_now = base.maintenance_started + TEST_LEASE_US;
    check_fence_request_refused_atomic(TEST_TOKEN,
                                       P4_WIFI_MAINT_FENCE_INCOMPLETE);

    prepare_discovery_active();
    base.maintenance_fence_mode = P4_WIFI_MAINT_FENCE_INCOMPLETE;
    check_fence_request_refused_atomic(TEST_TOKEN,
                                       P4_WIFI_MAINT_FENCE_COMMIT_UNKNOWN);

    prepare_discovery_active();
    owner.pending_active = 1u;
    check_fence_request_refused_atomic(TEST_TOKEN,
                                       P4_WIFI_MAINT_FENCE_INCOMPLETE);

    prepare_discovery_active();
    owner.partial_active = 1u;
    check_fence_request_refused_atomic(TEST_TOKEN,
                                       P4_WIFI_MAINT_FENCE_INCOMPLETE);

    prepare_discovery_active();
    owner.ids.active_uid = 9u;
    owner.ids.active_request_id = 281u;
    check_fence_request_refused_atomic(TEST_TOKEN,
                                       P4_WIFI_MAINT_FENCE_INCOMPLETE);

    prepare_discovery_active();
    fifo.busy = 1u;
    check_fence_request_refused_atomic(TEST_TOKEN,
                                       P4_WIFI_MAINT_FENCE_INCOMPLETE);

    prepare_discovery_active();
    queue_discovery();
    check_fence_request_refused_atomic(TEST_TOKEN,
                                       P4_WIFI_MAINT_FENCE_INCOMPLETE);
}

static void test_prearmed_terminal_fence_admission(void)
{
    ULONG armed_mode, requested_mode;

    for (armed_mode = P4_WIFI_MAINT_FENCE_INCOMPLETE;
         armed_mode <= P4_WIFI_MAINT_FENCE_WAIT_REBOOT; ++armed_mode) {
        prepare_discovery_active();
        base.maintenance_fence_mode = armed_mode;
        CHECK(!wifi_maintenance_cleanup_allowed(&base));
        CHECK(wifi_maintenance_fence(&base, TEST_TOKEN, armed_mode));
        CHECK(base.maintenance_fence_mode == armed_mode);
        CHECK(base.maintenance_state == P4_WIFI_MAINT_FENCING);
        CHECK(base.ready == TRUE);
        CHECK(base.associated == FALSE);
        CHECK(!wifi_maintenance_cleanup_allowed(&base));
        CHECK(request_calls == 0u);
        CHECK(discovery_acquire_calls == 0u);
        CHECK(tx_session_calls == 0u);

        /* Once this pre-armed disposition enters FENCING, even an identical
         * second request is refused. While still ACTIVE, only this exact
         * armed disposition may advance the state. */
        check_fence_request_refused_atomic(TEST_TOKEN, armed_mode);

        prepare_discovery_active();
        base.maintenance_fence_mode = armed_mode;
        check_fence_request_refused_atomic(TEST_TOKEN, 0u);
        for (requested_mode = P4_WIFI_MAINT_FENCE_INCOMPLETE;
             requested_mode <= P4_WIFI_MAINT_FENCE_WAIT_REBOOT;
             ++requested_mode) {
            if (requested_mode != armed_mode)
                check_fence_request_refused_atomic(TEST_TOKEN,
                                                   requested_mode);
        }
    }
}

static void test_sticky_fence_blocks_discovery(void)
{
    struct P4WifiBase base_before;
    struct wifi_owner_control control_before;
    struct p4_hosted_owner owner_before;
    struct p4_hosted_fifo fifo_before;
    struct P4SDMMCHost host_before;
    ULONG nonce;
    struct Task *saved_task;
    struct Task *saved_lock_owner;
    unsigned saved_lock_held;

    prepare_discovery_active();
    base.maintenance_fence_mode = P4_WIFI_MAINT_FENCE_INCOMPLETE;
    memcpy(&base_before, &base, sizeof(base_before));
    memcpy(&control_before, &control, sizeof(control_before));
    memcpy(&owner_before, &owner, sizeof(owner_before));
    memcpy(&fifo_before, &fifo, sizeof(fifo_before));
    memcpy(&host_before, &host, sizeof(host_before));
    CHECK(!wifi_maintenance_discover(&base, TEST_TOKEN));
    CHECK(memcmp(&base_before, &base, sizeof(base_before)) == 0);
    CHECK(memcmp(&control_before, &control, sizeof(control_before)) == 0);
    CHECK(memcmp(&owner_before, &owner, sizeof(owner_before)) == 0);
    CHECK(memcmp(&fifo_before, &fifo, sizeof(fifo_before)) == 0);
    CHECK(memcmp(&host_before, &host, sizeof(host_before)) == 0);
    CHECK(discovery_supported_calls == 0u);
    CHECK(discovery_reserve_calls == 0u);
    CHECK(discovery_request_calls == 0u);
    CHECK(discovery_exchange_calls == 0u);
    CHECK(discovery_rpc_encode_calls == 0u);
    CHECK(discovery_response_calls == 0u);
    CHECK(discovery_inventory_calls == 0u);
    CHECK(discovery_acquire_calls == 0u);
    CHECK(discovery_release_calls == 0u);
    CHECK(discovery_cancel_calls == 0u);

    prepare_discovery_active();
    queue_discovery();
    nonce = control.discovery_nonce;
    base.maintenance_fence_mode = P4_WIFI_MAINT_FENCE_COMMIT_UNKNOWN;
    CHECK(service_step() == 2);
    CHECK(base.maintenance_state == P4_WIFI_MAINT_ACTIVE);
    CHECK(base.maintenance_fence_mode ==
          P4_WIFI_MAINT_FENCE_COMMIT_UNKNOWN);
    check_discovery_storage_cleared(P4_WIFI_DISCOVERY_FAILED, nonce);
    CHECK(request_calls == 0u);
    CHECK(discovery_supported_calls == 0u);
    CHECK(discovery_reserve_calls == 0u);
    CHECK(discovery_request_calls == 0u);
    CHECK(discovery_exchange_calls == 0u);
    CHECK(discovery_rpc_encode_calls == 0u);
    CHECK(discovery_response_calls == 0u);
    CHECK(discovery_inventory_calls == 0u);
    CHECK(discovery_acquire_calls == 0u);
    CHECK(discovery_release_calls == 0u);
    CHECK(discovery_cancel_calls == 0u);

    prepare_discovery_active();
    queue_discovery();
    nonce = control.discovery_nonce;
    base.maintenance_fence_mode = P4_WIFI_MAINT_FENCE_WAIT_REBOOT;
    saved_task = current_task;
    saved_lock_owner = base.client_lock.ss_Owner;
    saved_lock_held = base.client_lock.held;
    current_task = &owner_task;
    base.client_lock.held = 1u;
    base.client_lock.ss_Owner = &owner_task;
    CHECK(wifi_maintenance_discovery_service(&base, &runtime) == 0);
    base.client_lock.held = saved_lock_held;
    base.client_lock.ss_Owner = saved_lock_owner;
    current_task = saved_task;
    CHECK(base.maintenance_state == P4_WIFI_MAINT_ACTIVE);
    CHECK(base.maintenance_fence_mode == P4_WIFI_MAINT_FENCE_WAIT_REBOOT);
    check_discovery_storage_cleared(P4_WIFI_DISCOVERY_FAILED, nonce);
    CHECK(discovery_cancel_calls == 1u);
    CHECK(discovery_supported_calls == 0u);
    CHECK(discovery_reserve_calls == 0u);
    CHECK(discovery_request_calls == 0u);
    CHECK(discovery_exchange_calls == 0u);
    CHECK(discovery_rpc_encode_calls == 0u);
    CHECK(discovery_response_calls == 0u);
    CHECK(discovery_inventory_calls == 0u);
    CHECK(discovery_acquire_calls == 0u);
    CHECK(discovery_release_calls == 0u);
}

static void check_failed_terminal_fence(ULONG mode, unsigned expected_tx,
                                        unsigned expected_release,
                                        unsigned expected_stopping)
{
    CHECK(service_step() == 0);
    CHECK(base.maintenance_state == P4_WIFI_MAINT_FAILED);
    CHECK(base.maintenance_fence_mode == mode);
    CHECK(!wifi_maintenance_cleanup_allowed(&base));
    CHECK(base.associated == FALSE);
    CHECK(request_calls == 0u);
    CHECK(tx_session_calls == expected_tx);
    CHECK(discovery_release_calls == expected_release);
    CHECK(discovery_stopping_calls == expected_stopping);
    CHECK(host.lock.ss_Owner == NULL);
    CHECK(wifi_maintenance_poll(&base, TEST_TOKEN) == P4_WIFI_MAINT_FAILED);
    CHECK(!wifi_maintenance_release(&base, TEST_TOKEN));
    CHECK(base.maintenance_state == P4_WIFI_MAINT_FAILED);
    CHECK(base.maintenance_fence_mode == mode);
    CHECK(request_calls == 0u);
}

static void test_terminal_fence_owner_confirmation_and_failures(void)
{
    ULONG mode;
    unsigned stopped_before, ready_before;

    for (mode = P4_WIFI_MAINT_FENCE_INCOMPLETE;
         mode <= P4_WIFI_MAINT_FENCE_WAIT_REBOOT; ++mode) {
        queue_terminal_fence(mode);
        stopped_before = stopped_calls;
        ready_before = ready_calls;
        CHECK(base.maintenance_state == P4_WIFI_MAINT_FENCING);
        CHECK(base.ready == TRUE);
        CHECK(discovery_acquire_calls == 0u);
        CHECK(tx_session_calls == 0u);
        CHECK(request_calls == 0u);

        CHECK(service_step() == 2);
        CHECK(base.maintenance_state == P4_WIFI_MAINT_FENCED);
        CHECK(base.maintenance_fence_mode == mode);
        CHECK(base.ready == FALSE);
        CHECK(base.associated == FALSE);
        CHECK(discovery_acquire_calls == 1u);
        CHECK(discovery_acquire_wait == P4_HOSTED_OWNER_MAX_ACQUIRE_US);
        CHECK(tx_session_calls == 1u);
        CHECK(discovery_stopping_calls == 1u);
        CHECK(discovery_release_calls == 1u);
        CHECK(host.lock.ss_Owner == NULL);
        CHECK(wifi_maintenance_poll(&base, TEST_TOKEN) == P4_WIFI_MAINT_FENCED);
        CHECK(!wifi_maintenance_normal_poll(base.maintenance_state));
        CHECK(!wifi_maintenance_cleanup_allowed(&base));
        CHECK(request_calls == 0u);
        CHECK(stopped_calls == stopped_before);
        CHECK(ready_calls == ready_before);
    }

    queue_terminal_fence(P4_WIFI_MAINT_FENCE_INCOMPLETE);
    discovery_acquire_succeeds = FALSE;
    check_failed_terminal_fence(P4_WIFI_MAINT_FENCE_INCOMPLETE, 0u, 0u, 0u);
    CHECK(discovery_acquire_calls == 1u);

    queue_terminal_fence(P4_WIFI_MAINT_FENCE_COMMIT_UNKNOWN);
    discovery_change_host_generation_on_acquire = TRUE;
    check_failed_terminal_fence(P4_WIFI_MAINT_FENCE_COMMIT_UNKNOWN, 1u, 1u, 0u);

    queue_terminal_fence(P4_WIFI_MAINT_FENCE_WAIT_REBOOT);
    discovery_tx_session_succeeds = FALSE;
    check_failed_terminal_fence(P4_WIFI_MAINT_FENCE_WAIT_REBOOT, 1u, 1u, 0u);

    queue_terminal_fence(P4_WIFI_MAINT_FENCE_INCOMPLETE);
    discovery_stopping = TRUE;
    check_failed_terminal_fence(P4_WIFI_MAINT_FENCE_INCOMPLETE, 1u, 1u, 1u);

    queue_terminal_fence(P4_WIFI_MAINT_FENCE_COMMIT_UNKNOWN);
    discovery_acquire_advance_us = TEST_LEASE_US;
    check_failed_terminal_fence(P4_WIFI_MAINT_FENCE_COMMIT_UNKNOWN, 1u, 1u, 0u);

    /* The owner wait is capped by the original lease, not just the global
     * acquire maximum. Expire exactly while acquiring to force a safe failure. */
    queue_terminal_fence(P4_WIFI_MAINT_FENCE_WAIT_REBOOT);
    fake_now = 100000000u;
    base.maintenance_started = fake_now - (TEST_LEASE_US - 1000u);
    discovery_acquire_advance_us = 1000u;
    check_failed_terminal_fence(P4_WIFI_MAINT_FENCE_WAIT_REBOOT, 1u, 1u, 0u);
    CHECK(discovery_acquire_wait == 1000u);
}

static void test_terminal_fence_sticky_after_expiry(void)
{
    ULONG mode = P4_WIFI_MAINT_FENCE_WAIT_REBOOT;
    unsigned acquire_calls, release_calls, stopping_calls;

    queue_terminal_fence(mode);
    CHECK(service_step() == 2);
    CHECK(base.maintenance_state == P4_WIFI_MAINT_FENCED);
    acquire_calls = discovery_acquire_calls;
    release_calls = discovery_release_calls;
    stopping_calls = discovery_stopping_calls;

    fake_now = base.maintenance_started + TEST_LEASE_US;
    CHECK(wifi_maintenance_poll(&base, TEST_TOKEN) == P4_WIFI_MAINT_FENCED);
    CHECK(base.maintenance_state == P4_WIFI_MAINT_FENCED);
    CHECK(base.maintenance_fence_mode == mode);
    CHECK(service_step() == 2);
    CHECK(discovery_acquire_calls == acquire_calls);
    CHECK(discovery_release_calls == release_calls);
    CHECK(discovery_stopping_calls == stopping_calls);
    CHECK(request_calls == 0u);
    CHECK(!wifi_maintenance_normal_poll(base.maintenance_state));
    CHECK(!wifi_maintenance_cleanup_allowed(&base));
    CHECK(!wifi_maintenance_release(&base, TEST_TOKEN));
    CHECK(wifi_maintenance_request(&base) == 0u);
    CHECK(base.maintenance_state == P4_WIFI_MAINT_FENCED);
    CHECK(base.maintenance_fence_mode == mode);
    CHECK(request_calls == 0u);

    queue_terminal_fence(P4_WIFI_MAINT_FENCE_INCOMPLETE);
    fake_now = base.maintenance_started + TEST_LEASE_US;
    CHECK(wifi_maintenance_poll(&base, TEST_TOKEN) == P4_WIFI_MAINT_FAILED);
    CHECK(base.maintenance_state == P4_WIFI_MAINT_FAILED);
    CHECK(base.maintenance_fence_mode == P4_WIFI_MAINT_FENCE_INCOMPLETE);
    CHECK(!wifi_maintenance_cleanup_allowed(&base));
    CHECK(service_step() == 0);
    CHECK(base.maintenance_state == P4_WIFI_MAINT_FAILED);
    CHECK(base.maintenance_fence_mode == P4_WIFI_MAINT_FENCE_INCOMPLETE);
    CHECK(!wifi_maintenance_release(&base, TEST_TOKEN));
    CHECK(request_calls == 0u);
    CHECK(discovery_acquire_calls == 0u);
}

static void test_normal_poll_gate(void)
{
    CHECK(wifi_maintenance_normal_poll(P4_WIFI_MAINT_IDLE) == TRUE);
    CHECK(wifi_maintenance_normal_poll(P4_WIFI_MAINT_RELEASED) == TRUE);
    CHECK(wifi_maintenance_normal_poll(P4_WIFI_MAINT_REQUESTED) == TRUE);
    CHECK(wifi_maintenance_normal_poll(P4_WIFI_MAINT_ACTIVE) == FALSE);
    CHECK(wifi_maintenance_normal_poll(P4_WIFI_MAINT_RELEASING) == FALSE);
    CHECK(wifi_maintenance_normal_poll(P4_WIFI_MAINT_FAILED) == FALSE);
    CHECK(wifi_maintenance_normal_poll(P4_WIFI_MAINT_BUSY) == FALSE);
    CHECK(wifi_maintenance_normal_poll(P4_WIFI_MAINT_FENCING) == FALSE);
    CHECK(wifi_maintenance_normal_poll(P4_WIFI_MAINT_FENCED) == FALSE);
    CHECK(wifi_maintenance_normal_poll((ULONG)123456u) == FALSE);
}

static void test_pending_release_and_poll(void)
{
    ULONG token;

    reset_fixture();
    token = begin_request();
    CHECK(wifi_maintenance_poll(&base, token) == P4_WIFI_MAINT_REQUESTED);
    CHECK(wifi_maintenance_release(&base, token));
    CHECK(base.maintenance_state == P4_WIFI_MAINT_RELEASED);
    CHECK(wifi_maintenance_poll(&base, token) == P4_WIFI_MAINT_RELEASED);
    CHECK(service_step() == 1);
    CHECK(request_calls == 0u);
    CHECK(stopped_calls == 0u);
    CHECK(ready_calls == 0u);
}

static void test_service_drain_fences(void)
{
    unsigned fence_case;

    for (fence_case = 0u; fence_case < 4u; ++fence_case) {
        prepare_service(P4_WIFI_MAINT_REQUESTED);
        if (fence_case == 0u)
            owner.pending_active = 1u;
        else if (fence_case == 1u)
            owner.ids.active_uid = 9u;
        else if (fence_case == 2u)
            owner.partial_active = 1u;
        else
            fifo.busy = 1u;
        CHECK(service_step() == 2);
        CHECK(request_calls == 0u);
        CHECK(stopped_calls == 0u);
        CHECK(base.maintenance_state == P4_WIFI_MAINT_REQUESTED);
        CHECK(base.associated == TRUE);
    }
}

static void test_stop_ack_admission_and_active_silence(void)
{
    prepare_service(P4_WIFI_MAINT_REQUESTED);
    CHECK(service_step() == 2);
    CHECK(request_calls == 1u);
    CHECK(request_ids[0] == 281u);
    CHECK(request_budgets[0] == 8000000u);
    CHECK(base.maintenance_state == P4_WIFI_MAINT_ACTIVE);
    CHECK(base.associated == FALSE);
    CHECK(base.link_state == P4_WIFI_LINK_UNKNOWN);
    CHECK(base.confirmed_ap.ssid[0] == 0u);
    CHECK(base.observed_ap.ssid[0] == 0u);
    CHECK(stopped_calls == 1u);

    CHECK(service_step() == 2);
    CHECK(request_calls == 1u);
    CHECK(stopped_calls == 1u);

    prepare_service(P4_WIFI_MAINT_REQUESTED);
    request_succeeds = FALSE;
    CHECK(service_step() == 0);
    CHECK(request_calls == 1u && request_ids[0] == 281u);
    CHECK(base.maintenance_state == P4_WIFI_MAINT_FAILED);
    CHECK(base.associated == FALSE);
}

static void test_release_start_ack_and_offline_state(void)
{
    ULONG token;

    prepare_service(P4_WIFI_MAINT_ACTIVE);
    base.associated = FALSE; /* STOP was acknowledged before ACTIVE. */
    base.maintenance_token = TEST_TOKEN + 1u;
    base.maintenance_task = &requester_task;
    token = base.maintenance_token;
    CHECK(wifi_maintenance_release(&base, token));
    CHECK(base.maintenance_state == P4_WIFI_MAINT_RELEASING);
    CHECK(service_step() == 1);
    CHECK(request_calls == 1u);
    CHECK(request_ids[0] == 280u);
    CHECK(request_budgets[0] == 8000000u);
    CHECK(base.maintenance_state == P4_WIFI_MAINT_RELEASED);
    CHECK(base.associated == FALSE);
    CHECK(ready_calls == 1u);

    prepare_service(P4_WIFI_MAINT_RELEASING);
    request_succeeds = FALSE;
    CHECK(service_step() == 0);
    CHECK(request_calls == 1u && request_ids[0] == 280u);
    CHECK(base.maintenance_state == P4_WIFI_MAINT_FAILED);
    CHECK(base.associated == FALSE);
    CHECK(ready_calls == 0u);
}

static void test_poll_and_release_expiry_and_wrap(void)
{
    ULONG token;
    ULONG remaining;

    reset_fixture();
    token = begin_request();
    fake_now = base.maintenance_started + TEST_LEASE_US;
    base.associated = TRUE;
    CHECK(wifi_maintenance_poll(&base, token) == P4_WIFI_MAINT_FAILED);
    CHECK(base.maintenance_state == P4_WIFI_MAINT_FAILED);
    CHECK(base.associated == FALSE);

    reset_fixture();
    token = begin_request();
    base.maintenance_state = P4_WIFI_MAINT_ACTIVE;
    base.associated = TRUE;
    fake_now = base.maintenance_started + TEST_LEASE_US;
    CHECK(!wifi_maintenance_release(&base, token));
    CHECK(base.maintenance_state == P4_WIFI_MAINT_FAILED);
    CHECK(base.associated == FALSE);

    reset_fixture();
    token = begin_request();
    base.runtime = NULL;
    base.associated = TRUE;
    CHECK(!wifi_maintenance_release(&base, token));
    CHECK(base.maintenance_state == P4_WIFI_MAINT_FAILED);
    CHECK(base.associated == FALSE);

    reset_fixture();
    fake_now = UINT32_MAX - 100u;
    token = wifi_maintenance_request(&base);
    CHECK(token == TEST_TOKEN + 1u);
    fake_now = 50u;
    remaining = wifi_owner_remaining(&context, base.maintenance_started,
                                    TEST_LEASE_US);
    CHECK(remaining == TEST_LEASE_US - 151u);
    CHECK(wifi_maintenance_poll(&base, token) == P4_WIFI_MAINT_REQUESTED);
}

static void test_service_deadline_failures(void)
{
    prepare_service(P4_WIFI_MAINT_REQUESTED);
    fake_now = base.maintenance_started + TEST_LEASE_US;
    CHECK(service_step() == 0);
    CHECK(request_calls == 0u);
    CHECK(base.maintenance_state == P4_WIFI_MAINT_FAILED);
    CHECK(base.associated == FALSE);

    prepare_service(P4_WIFI_MAINT_REQUESTED);
    request_advance_us = TEST_LEASE_US;
    CHECK(service_step() == 0);
    CHECK(request_calls == 1u);
    CHECK(base.maintenance_state == P4_WIFI_MAINT_FAILED);
    CHECK(base.associated == FALSE);

    prepare_service(P4_WIFI_MAINT_RELEASING);
    request_advance_us = TEST_LEASE_US;
    CHECK(service_step() == 0);
    CHECK(request_calls == 1u && request_ids[0] == 280u);
    CHECK(base.maintenance_state == P4_WIFI_MAINT_FAILED);
    CHECK(base.associated == FALSE);
    CHECK(ready_calls == 0u);

    prepare_service(P4_WIFI_MAINT_REQUESTED);
    owner.poisoned = 1u;
    CHECK(service_step() == 0);
    CHECK(request_calls == 0u);
    CHECK(base.maintenance_state == P4_WIFI_MAINT_FAILED);

    prepare_service(P4_WIFI_MAINT_REQUESTED);
    fifo.generation += 1u;
    CHECK(service_step() == 0);
    CHECK(request_calls == 0u);
    CHECK(base.maintenance_state == P4_WIFI_MAINT_FAILED);
}

int main(void)
{
    test_ota_span_helpers();
    test_ota_admission_refusals();
    test_ota_success_order_and_acknowledged_bytes();
    test_ota_status_and_buffer_alias_refusals();
    test_ota_genuine_resource_instance_refusal();
    test_ota_begin_failures_are_sticky_and_closed();
    test_ota_remote_rejection_and_end_commit_unknown();
    test_ota_deadline_caps_and_late_ack();
    test_ota_finish_mode_mapping_and_success();
    test_ota_finish_preflight_refusals();
    test_ota_finish_completed_identity_refusals();
    test_ota_finish_begun_revalidation_failures();
    test_ota_finish_acquire_release_and_lease_failures();
    test_ota_finish_no_begin_deadlines_and_late_ready();
    test_ota_finish_generation_change_during_acquire();
    test_ota_finish_no_begin_request_failure();
    test_normal_poll_gate();
    test_request_identity_and_lock_contention();
    test_discovery_queue_admission_and_nonce();
    test_discovery_poll_status_copy_and_output_guard();
    test_sticky_fence_blocks_discovery_poll();
    test_discovery_service_success_unsupported_and_remote_error();
    test_discovery_service_deadline_and_failure_fences();
    test_discovery_poll_retires_expired_or_changed_session();
    test_discovery_correlations_and_cleanup();
    test_pending_release_and_poll();
    test_service_drain_fences();
    test_stop_ack_admission_and_active_silence();
    test_release_start_ack_and_offline_state();
    test_terminal_fence_admission();
    test_prearmed_terminal_fence_admission();
    test_sticky_fence_blocks_discovery();
    test_terminal_fence_owner_confirmation_and_failures();
    test_terminal_fence_sticky_after_expiry();
    test_poll_and_release_expiry_and_wrap();
    test_service_deadline_failures();
    printf("wifi maintenance source tests: %u checks passed\n", test_checks);
    return 0;
}
