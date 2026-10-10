/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
/* Host model for source-extracted production maintenance/discovery helpers.
 * Exec, RPC/codec, UID reservation, private owner exchange and host ownership
 * boundaries are modeled; separate suites exercise the actual codecs/owner. */
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
};

struct P4WifiBase {
    BOOL ready;
    BOOL associated;
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
static ULONG discovery_exchange_timeout;
static ULONG discovery_acquire_wait;
static ULONG discovery_wire_uid;
static ULONG discovery_wire_nonce;
static ULONG discovery_wire_request_id;
static ULONG discovery_wire_generation;
static ULONG discovery_wire_checksum;

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
}

static const struct p4_wifi_device_callbacks callbacks = {
    mock_ready,
    mock_stopped
};

static struct P4WifiBase base;
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
    if (slot < sizeof(request_ids) / sizeof(request_ids[0])) {
        request_ids[slot] = id;
        request_budgets[slot] = remaining;
    }
    ++request_calls;
    fake_now += request_advance_us;
    return request_succeeds ? 1 : 0;
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
    ++discovery_reserve_calls;
    CHECK(ids == &owner.ids);
    CHECK(generation == TEST_GENERATION);
    CHECK(request_id == P4_HOSTED_DISCOVERY_REQUEST_ID);
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
    ++discovery_unsubmitted_calls;
    CHECK(request_owner == &owner);
    CHECK(request_fifo == &fifo);
    CHECK(generation == context.generation);
    CHECK(request_id == P4_HOSTED_DISCOVERY_REQUEST_ID);
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
    ++discovery_release_calls;
    host.lock.ss_Owner = NULL;
}

static int wifi_tx_session(void *opaque, uint32_t generation)
{
    CHECK(opaque == &context);
    return discovery_tx_session_succeeds && generation == context.generation &&
        host.generation == generation && host.lock.ss_Owner == current_task;
}

static int wifi_owner_stopping(void *opaque)
{
    CHECK(opaque == &context);
    CHECK(host.lock.ss_Owner == current_task);
    ++discovery_stopping_calls;
    return discovery_stopping ? 1 : 0;
}

#ifdef WIFI_MAINTENANCE_MUTANT
#include "wifi_maintenance_mutant.inc"
#else
#include "wifi_maintenance_extract.inc"
#endif

static void reset_discovery_mocks(void)
{
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
    discovery_exchange_timeout = 0u;
    discovery_acquire_wait = 0u;
    discovery_wire_uid = 0u;
    discovery_wire_nonce = 0u;
    discovery_wire_request_id = 0u;
    discovery_wire_generation = 0u;
    discovery_wire_checksum = 0u;
    init.discovery_supported = 1u;
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
    base.owner_task = &owner_task;
    base.runtime = &runtime;
    base.client = &client_cookie;
    base.callbacks = &callbacks;
    base.maintenance_token = TEST_TOKEN;
    base.maintenance_task = &requester_task;
    base.maintenance_started = fake_now;
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

static void test_normal_poll_gate(void)
{
    CHECK(wifi_maintenance_normal_poll(P4_WIFI_MAINT_IDLE) == TRUE);
    CHECK(wifi_maintenance_normal_poll(P4_WIFI_MAINT_RELEASED) == TRUE);
    CHECK(wifi_maintenance_normal_poll(P4_WIFI_MAINT_REQUESTED) == TRUE);
    CHECK(wifi_maintenance_normal_poll(P4_WIFI_MAINT_ACTIVE) == FALSE);
    CHECK(wifi_maintenance_normal_poll(P4_WIFI_MAINT_RELEASING) == FALSE);
    CHECK(wifi_maintenance_normal_poll(P4_WIFI_MAINT_FAILED) == FALSE);
    CHECK(wifi_maintenance_normal_poll(P4_WIFI_MAINT_BUSY) == FALSE);
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
    test_normal_poll_gate();
    test_request_identity_and_lock_contention();
    test_discovery_queue_admission_and_nonce();
    test_discovery_poll_status_copy_and_output_guard();
    test_discovery_service_success_unsupported_and_remote_error();
    test_discovery_service_deadline_and_failure_fences();
    test_discovery_poll_retires_expired_or_changed_session();
    test_discovery_correlations_and_cleanup();
    test_pending_release_and_poll();
    test_service_drain_fences();
    test_stop_ack_admission_and_active_silence();
    test_release_start_ack_and_offline_state();
    test_poll_and_release_expiry_and_wrap();
    test_service_deadline_failures();
    printf("wifi maintenance source tests: %u checks passed\n", test_checks);
    return 0;
}
