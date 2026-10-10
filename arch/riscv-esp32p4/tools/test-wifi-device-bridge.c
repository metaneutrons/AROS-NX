/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
/* Host model for the resource-to-device bridge. The bridge callbacks and
 * SDMMC owner are small mocks; the tested bridge functions are extracted
 * verbatim by run-wifi-device-bridge-tests.sh from runtime_probe.c. */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Minimal Exec type surface needed by the real runtime_probe.h and host.h. */
#define EXEC_TYPES_H
#define EXEC_LIBRARIES_H
#define EXEC_SEMAPHORES_H
typedef void *APTR;
typedef uint32_t ULONG;
typedef int32_t LONG;
typedef uint8_t UBYTE;
typedef uintptr_t IPTR;
typedef int BOOL;
#define TRUE 1
#define FALSE 0
struct ExecBase { unsigned unused; };
struct Task { unsigned id; };
struct Library { unsigned unused; };
struct SignalSemaphore { struct Task *ss_Owner; unsigned depth; };

#include "runtime_probe.h"
#include "host.h"
#include "hosted_mac.h"
#include "hosted_discovery.h"
#include "hosted_association.h"
#include "hosted_owner.h"
#include "hosted_tx.h"
#include "hosted_wire.h"
#include "memory.h"

#define TEST_GENERATION 23u
#define TEST_POOL 20u
#define TEST_FRAME_SIZE 60u

static unsigned long checks;
#define CHECK(expression) do { \
    ++checks; \
    if (!(expression)) { \
        fprintf(stderr, "check failed at %s:%d: %s\n", __FILE__, __LINE__, \
                #expression); \
        exit(1); \
    } \
} while (0)

static struct Task task_owner = {1u};
static struct Task *current_task = &task_owner;
static struct P4WifiBase *wifi_resource_base;
static unsigned semaphore_fail_next;
static unsigned host_held;
static unsigned host_refuse_acquire;
static unsigned host_refuse_budget;
static unsigned host_acquire_calls, host_release_calls, host_budget_calls;
static ULONG host_wait_seen, host_budget_seen;
static ULONG fake_now_us;

struct wifi_probe_context {
    struct P4SDMMCHost *host;
    ULONG generation;
    ULONG peer_clock;
    ULONG scan_started;
    BOOL scan_active;
    ULONG association_started;
    BOOL association_active;
    BOOL association_exchanging;
    const struct p4_wifi_association_cancel *association_cancel;
};

static struct Task *FindTask(void *name)
{
    (void)name;
    return current_task;
}

static ULONG mock_now_us(void)
{
    return fake_now_us;
}

static BOOL AttemptSemaphore(struct SignalSemaphore *sem)
{
    if (!sem || semaphore_fail_next || sem->depth != 0u) {
        semaphore_fail_next = 0u;
        return FALSE;
    }
    sem->depth = 1u;
    sem->ss_Owner = current_task;
    return TRUE;
}

static void ObtainSemaphore(struct SignalSemaphore *sem)
{
    CHECK(sem != NULL);
    CHECK(sem->depth == 0u);
    sem->depth = 1u;
    sem->ss_Owner = current_task;
}

static void ReleaseSemaphore(struct SignalSemaphore *sem)
{
    CHECK(sem != NULL);
    CHECK(sem->depth == 1u);
    CHECK(sem->ss_Owner == current_task);
    sem->depth = 0u;
    sem->ss_Owner = NULL;
}

/* Include the actual attach/detach, owner TX/session adapters, frame
 * dispatcher and the actual owner-stop publication statements. */
#include "wifi_device_bridge_extract.inc"

struct tx_mock {
    struct P4SDMMCHost *host;
    ULONG generation;
    unsigned read_calls, write_calls, session_calls, budget_calls, yield_calls;
    unsigned write_mode;
    unsigned decoded_ok;
    unsigned checksum;
    ULONG token;
    size_t expected_size;
    UBYTE expected_frame[P4_HOSTED_ETHERNET_FRAME_MAX];
};

static BOOL mock_host_acquire(struct P4SDMMCHost *host, ULONG wait_us)
{
    ++host_acquire_calls;
    host_wait_seen = wait_us;
    if (!host || host_held || host_refuse_acquire || wait_us == 0u)
        return FALSE;
    host_held = 1u;
    host->lock.ss_Owner = current_task;
    host->lock.depth = 1u;
    return TRUE;
}

static void mock_host_release(struct P4SDMMCHost *host)
{
    CHECK(host != NULL);
    CHECK(host_held == 1u);
    CHECK(host->lock.ss_Owner == current_task);
    ++host_release_calls;
    host_held = 0u;
    host->lock.ss_Owner = NULL;
    host->lock.depth = 0u;
}

static BOOL mock_host_set_budget(struct P4SDMMCHost *host, ULONG usec)
{
    CHECK(host != NULL);
    ++host_budget_calls;
    host_budget_seen = usec;
    return !host_refuse_budget && host_held == 1u &&
        host->lock.ss_Owner == current_task && usec != 0u;
}

static int mock_tx_read_word(void *opaque, uint32_t address, uint32_t *value)
{
    struct tx_mock *mock = opaque;
    ++mock->read_calls;
    if (!host_held || address != P4_HOSTED_TX_TOKEN_ADDRESS || !value)
        return 0;
    *value = mock->token;
    return 1;
}

static int mock_tx_write_fifo(void *opaque,
    const struct p4_hosted_fifo_plan *plan, const uint8_t *buffer,
    struct p4_hosted_tx_completion *completion)
{
    struct tx_mock *mock = opaque;
    struct p4_hosted_frame decoded;
    ++mock->write_calls;
    if (!host_held || !plan || !buffer || !completion)
        return 0;
    mock->decoded_ok = p4_hosted_decode(buffer, plan->logical_bytes, mock->checksum,
                                         &decoded) &&
        decoded.interface_type == P4_HOSTED_STA &&
        decoded.interface_number == 0u && decoded.flags == 0u &&
        decoded.throttle == 0u && decoded.packet_type == 0u &&
        decoded.sequence == 0u && decoded.payload_size == mock->expected_size &&
        memcmp(decoded.payload, mock->expected_frame,
               mock->expected_size) == 0;
    if (mock->write_mode == 0u) {
        completion->started = 1u;
        completion->success = 1u;
        completion->completed_bytes = plan->bus_bytes;
        return 1;
    }
    /* A single ambiguous started transfer is terminal; never simulate retry. */
    completion->started = 1u;
    completion->success = 0u;
    completion->completed_bytes = 0u;
    return 1;
}

static int mock_tx_session(void *opaque, uint32_t generation)
{
    struct tx_mock *mock = opaque;
    ++mock->session_calls;
    return generation == mock->generation && host_held &&
        mock->host->lock.ss_Owner == current_task;
}

static int mock_tx_budget(void *opaque)
{
    struct tx_mock *mock = opaque;
    ++mock->budget_calls;
    return host_held && mock->host->lock.ss_Owner == current_task;
}

static void mock_tx_yield(void *opaque)
{
    struct tx_mock *mock = opaque;
    ++mock->yield_calls;
}

struct fixture {
    struct P4WifiBase base;
    struct P4SDMMCHost host;
    struct wifi_probe_context context;
    struct p4_hosted_owner owner;
    struct wifi_owner_control control;
    struct p4_hosted_fifo fifo;
    struct p4_hosted_tx_ops tx_ops;
    struct wifi_device_runtime runtime;
    struct tx_mock tx;
    UBYTE frame[TEST_FRAME_SIZE];
};

static BOOL mock_disconnect(struct P4WifiBase *base)
{
    (void)base;
    return FALSE;
}

static void reset_counters(void)
{
    semaphore_fail_next = 0u;
    host_held = 0u;
    host_refuse_acquire = 0u;
    host_refuse_budget = 0u;
    host_acquire_calls = 0u;
    host_release_calls = 0u;
    host_budget_calls = 0u;
    host_wait_seen = 0u;
    host_budget_seen = 0u;
    fake_now_us = 1000u;
}

static void fixture_init(struct fixture *f)
{
    size_t i;
    memset(f, 0, sizeof(*f));
    reset_counters();
    f->host.generation = TEST_GENERATION;
    f->host.now_us = mock_now_us;
    f->host.slot_clock[0] = 400000u;
    f->host.acquire = mock_host_acquire;
    f->host.release = mock_host_release;
    f->host.set_budget = mock_host_set_budget;
    f->context.host = &f->host;
    f->context.generation = TEST_GENERATION;
    f->context.peer_clock = 400000u;
    f->tx.host = &f->host;
    f->tx.generation = TEST_GENERATION;
    f->tx.token = UINT32_C(1) << 16;
    f->tx.expected_size = sizeof(f->frame);
    for (i = 0u; i < sizeof(f->frame); ++i)
        f->frame[i] = (UBYTE)(i * 13u + 7u);
    f->frame[12] = 0x08u;
    f->frame[13] = 0x00u;
    memcpy(f->tx.expected_frame, f->frame, sizeof(f->frame));
    CHECK(p4_hosted_fifo_start(&f->fifo, TEST_GENERATION, TEST_POOL));
    f->tx_ops.context = &f->tx;
    f->tx_ops.read_word = mock_tx_read_word;
    f->tx_ops.write_fifo = mock_tx_write_fifo;
    f->tx_ops.session_ok = mock_tx_session;
    f->tx_ops.budget_ok = mock_tx_budget;
    f->tx_ops.yield = mock_tx_yield;
    f->runtime.context = &f->context;
    f->runtime.owner = &f->owner;
    f->runtime.control = &f->control;
    f->runtime.fifo = &f->fifo;
    f->runtime.tx = &f->tx_ops;
    f->runtime.checksum = 0u;
    f->runtime.servicing = TRUE;
    f->base.ready = TRUE;
    f->base.associated = TRUE;
    f->base.disconnect = mock_disconnect;
    f->base.runtime = &f->runtime;
    f->base.owner_task = current_task;
    memset(f->owner.wire, 0xa5, sizeof(f->owner.wire));
}

static BOOL send_fixture(struct fixture *f)
{
    return wifi_device_send(&f->base, f->frame, sizeof(f->frame));
}

static int wire_is_zero(const uint8_t *wire, size_t size)
{
    size_t i;
    for (i = 0u; i < size; ++i)
        if (wire[i] != 0u)
            return 0;
    return 1;
}

static unsigned ready_calls, stopped_calls, frame_calls;
static unsigned callback_saw_client_lock, callback_saw_host_lock;
static unsigned callback_stop_state_ok;
static UBYTE copied_frame[TEST_FRAME_SIZE];
static size_t copied_size;
static int frame_callback_result = 1;

static void client_service(APTR client, struct P4WifiBase *base)
{
    (void)client; (void)base;
}

static BOOL client_frame(APTR client, const UBYTE *bytes, ULONG length)
{
    (void)client;
    ++frame_calls;
    callback_saw_client_lock = wifi_resource_base->client_lock.depth == 1u &&
        wifi_resource_base->client_lock.ss_Owner == current_task;
    callback_saw_host_lock = host_held;
    copied_size = length <= sizeof(copied_frame) ? (size_t)length : 0u;
    if (copied_size)
        memcpy(copied_frame, bytes, copied_size);
    return frame_callback_result ? TRUE : FALSE;
}

static void client_ready(APTR client, const UBYTE *mac)
{
    (void)client; (void)mac;
    ++ready_calls;
    callback_saw_client_lock = wifi_resource_base->client_lock.depth == 1u &&
        wifi_resource_base->client_lock.ss_Owner == current_task;
}

static void client_stopped(APTR client)
{
    (void)client;
    ++stopped_calls;
    callback_stop_state_ok = !wifi_resource_base->ready &&
        wifi_resource_base->runtime == NULL &&
        wifi_resource_base->owner_task == NULL &&
        wifi_resource_base->client != NULL &&
        wifi_resource_base->callbacks != NULL &&
        wifi_resource_base->client_lock.depth == 1u &&
        wifi_resource_base->client_lock.ss_Owner == current_task;
}

static const struct p4_wifi_device_callbacks client_callbacks = {
    client_service, client_frame, client_ready, client_stopped
};

static void test_registration_and_detach(void)
{
    struct fixture f;
    int client_a, client_b;
    struct p4_wifi_device_callbacks incomplete = client_callbacks;
    fixture_init(&f);
    wifi_resource_base = &f.base;
    f.base.ready = FALSE;
    CHECK(!wifi_device_attach(&f.base, &client_a, &client_callbacks));
    CHECK(f.base.client == NULL && f.base.callbacks == NULL);
    CHECK(ready_calls == 0u);

    f.base.ready = TRUE;
    incomplete.frame = NULL;
    CHECK(!wifi_device_attach(&f.base, &client_a, &incomplete));
    CHECK(f.base.client == NULL);
    semaphore_fail_next = 1u;
    CHECK(!wifi_device_attach(&f.base, &client_a, &client_callbacks));
    CHECK(f.base.client == NULL && f.base.client_lock.depth == 0u);

    f.base.maintenance_state = P4_WIFI_MAINT_ACTIVE;
    CHECK(!wifi_device_attach(&f.base, &client_a, &client_callbacks));
    CHECK(f.base.client == NULL && f.base.callbacks == NULL);
    CHECK(f.base.client_lock.depth == 0u);
    f.base.maintenance_state = P4_WIFI_MAINT_IDLE;

    CHECK(wifi_device_attach(&f.base, &client_a, &client_callbacks));
    CHECK(f.base.client == &client_a && f.base.callbacks == &client_callbacks);
    CHECK(ready_calls == 1u && callback_saw_client_lock == 1u);
    CHECK(f.base.client_lock.depth == 0u);
    CHECK(!wifi_device_attach(&f.base, &client_b, &client_callbacks));
    wifi_device_detach(&f.base, &client_b);
    CHECK(f.base.client == &client_a && f.base.callbacks == &client_callbacks);

    wifi_device_detach(&f.base, &client_a);
    CHECK(f.base.client == NULL && f.base.callbacks == NULL);
    CHECK(f.base.ready && f.base.runtime == &f.runtime &&
          f.base.owner_task == current_task);
    CHECK(wifi_device_attach(&f.base, &client_b, &client_callbacks));
    CHECK(ready_calls == 2u && f.base.client == &client_b);
}

static void test_frame_dispatch_copy_and_lock_boundary(void)
{
    struct fixture f;
    UBYTE original[TEST_FRAME_SIZE];
    unsigned i;
    fixture_init(&f);
    wifi_resource_base = &f.base;
    CHECK(wifi_device_attach(&f.base, &f, &client_callbacks));
    memcpy(original, f.frame, sizeof(original));
    host_held = 0u;
    callback_saw_client_lock = 0u;
    callback_saw_host_lock = 1u;
    copied_size = 0u;
    frame_callback_result = 1;
    CHECK(wifi_owner_frame(NULL, original, sizeof(original)) == 1);
    CHECK(frame_calls == 1u && copied_size == sizeof(original));
    CHECK(callback_saw_client_lock == 1u && callback_saw_host_lock == 0u);
    for (i = 0u; i < sizeof(original); ++i)
        original[i] ^= 0xffu;
    CHECK(memcmp(copied_frame, f.frame, sizeof(copied_frame)) == 0);
    frame_callback_result = 0;
    CHECK(wifi_owner_frame(NULL, f.frame, sizeof(f.frame)) == 0);
    CHECK(frame_calls == 2u);
    f.base.ready = FALSE;
    CHECK(wifi_owner_frame(NULL, f.frame, sizeof(f.frame)) == 1);
    CHECK(frame_calls == 2u);
    CHECK(f.base.client_lock.depth == 0u);
}

static void test_send_preflight_refusals(void)
{
    struct fixture f;
    struct Task other_task = {2u};
    UBYTE short_frame[13] = {0};
    UBYTE long_frame[1519] = {0};
    fixture_init(&f);
    CHECK(!wifi_device_send(NULL, f.frame, sizeof(f.frame)));
    f.base.ready = FALSE;
    CHECK(!send_fixture(&f));
    f.base.ready = TRUE;
    f.base.runtime = NULL;
    CHECK(!send_fixture(&f));
    f.base.runtime = &f.runtime;
    f.base.owner_task = NULL;
    CHECK(!send_fixture(&f));
    f.base.owner_task = current_task;
    CHECK(f.runtime.servicing);
    f.runtime.servicing = FALSE;
    CHECK(!send_fixture(&f));
    CHECK(host_acquire_calls == 0u && wire_is_zero(f.owner.wire,
                                                  sizeof(f.owner.wire)) == 0);
    f.runtime.servicing = TRUE;
    f.base.associated = FALSE;
    CHECK(!send_fixture(&f));
    CHECK(host_acquire_calls == 0u && f.tx.read_calls == 0u &&
          f.tx.write_calls == 0u && wire_is_zero(f.owner.wire,
                                                  sizeof(f.owner.wire)) == 0);
    f.base.associated = TRUE;
    current_task = &other_task;
    CHECK(!send_fixture(&f));
    current_task = &task_owner;
    CHECK(host_acquire_calls == 0u);
    CHECK(!wifi_device_send(&f.base, short_frame, sizeof(short_frame)));
    CHECK(!wifi_device_send(&f.base, long_frame, sizeof(long_frame)));
    CHECK(!wifi_device_send(&f.base, NULL, sizeof(f.frame)));
    CHECK(host_acquire_calls == 0u);
    f.owner.pending_active = 1u;
    CHECK(!send_fixture(&f));
    f.owner.pending_active = 0u;
    f.owner.poisoned = 1u;
    CHECK(!send_fixture(&f));
    f.owner.poisoned = 0u;
    f.fifo.poisoned = 1u;
    CHECK(!send_fixture(&f));
    CHECK(host_acquire_calls == 0u && host_release_calls == 0u);
}

static void test_send_one_sta_transfer_and_wipe(void)
{
    struct fixture f;
    fixture_init(&f);
    CHECK(send_fixture(&f));
    CHECK(host_acquire_calls == 1u && host_wait_seen == 20000u);
    CHECK(host_budget_calls == 1u && host_budget_seen == 250000u);
    CHECK(host_release_calls == 1u && host_held == 0u);
    CHECK(f.tx.read_calls == 1u && f.tx.write_calls == 1u);
    CHECK(f.tx.decoded_ok == 1u && f.tx.yield_calls == 0u);
    CHECK(f.fifo.tx_sent == 1u && f.fifo.busy == 0u && !f.fifo.poisoned);
    CHECK(f.control.frame_tx.success == 1u);
    CHECK(wire_is_zero(f.owner.wire, sizeof(f.owner.wire)));
}

static void test_send_failure_is_single_attempt_and_wiped(void)
{
    struct fixture f;
    fixture_init(&f);
    f.owner.initialized = 1u;
    f.owner.generation = TEST_GENERATION;
    f.tx.write_mode = 1u;
    CHECK(!send_fixture(&f));
    CHECK(host_acquire_calls == 1u && host_release_calls == 1u);
    CHECK(f.tx.read_calls == 1u && f.tx.write_calls == 1u);
    CHECK(f.fifo.poisoned == 1u && f.fifo.tx_sent == 0u);
    CHECK(f.owner.poisoned == 1u);
    CHECK(wire_is_zero(f.owner.wire, sizeof(f.owner.wire)));
}

static void test_send_frame_bounds_and_checksums(void)
{
    static const size_t lengths[] = {14u, P4_HOSTED_ETHERNET_FRAME_MAX};
    struct fixture f;
    UBYTE bytes[P4_HOSTED_ETHERNET_FRAME_MAX];
    size_t index, variant;
    unsigned checksum;
    for (index = 0; index < sizeof(bytes); ++index)
        bytes[index] = (UBYTE)(index * 19u + 3u);
    for (checksum = 0; checksum <= 1; ++checksum) {
        for (variant = 0; variant < sizeof(lengths) / sizeof(lengths[0]); ++variant) {
            fixture_init(&f);
            f.runtime.checksum = checksum;
            f.tx.checksum = checksum;
            f.tx.expected_size = lengths[variant];
            memcpy(f.tx.expected_frame, bytes, lengths[variant]);
            CHECK(wifi_device_send(&f.base, bytes, (ULONG)lengths[variant]));
            CHECK(f.tx.decoded_ok == 1u && f.tx.write_calls == 1u);
            CHECK(f.control.frame_tx.plan.logical_bytes == lengths[variant] +
                    P4_HOSTED_HEADER_SIZE);
            CHECK(f.control.frame_tx.plan.bus_bytes ==
                    ((lengths[variant] + P4_HOSTED_HEADER_SIZE + 511u) & ~511u));
            CHECK(f.fifo.tx_sent == 1u && !f.fifo.poisoned);
            CHECK(host_acquire_calls == 1u && host_release_calls == 1u);
            CHECK(wire_is_zero(f.owner.wire, sizeof(f.owner.wire)));
        }
    }
}

static void test_budget_and_acquire_failure_cleanup(void)
{
    struct fixture f;
    fixture_init(&f);
    host_refuse_acquire = 1u;
    CHECK(!send_fixture(&f));
    CHECK(host_acquire_calls == 1u && host_release_calls == 0u);
    CHECK(wire_is_zero(f.owner.wire, sizeof(f.owner.wire)));

    fixture_init(&f);
    /* Budget refusal after acquisition releases exactly once, without TX. */
    host_refuse_budget = 1u;
    CHECK(!send_fixture(&f));
    CHECK(host_acquire_calls == 1u && host_release_calls == 1u);
    CHECK(f.tx.read_calls == 0u && f.tx.write_calls == 0u);
    CHECK(wire_is_zero(f.owner.wire, sizeof(f.owner.wire)));

    fixture_init(&f);
    /* A generation/session mismatch makes the real owner budget adapter
     * refuse before token access, while still releasing the acquired host. */
    f.context.generation = TEST_GENERATION + 1u;
    CHECK(!send_fixture(&f));
    CHECK(host_acquire_calls == 1u && host_release_calls == 1u);
    CHECK(host_budget_calls == 0u && f.tx.read_calls == 0u);
    CHECK(wire_is_zero(f.owner.wire, sizeof(f.owner.wire)));
}

static void test_association_budget_uses_remaining_deadline(void)
{
    struct fixture f;
    fixture_init(&f);
    f.context.association_active = TRUE;
    f.context.association_started = 0u;
    fake_now_us = 7999990u;
    CHECK(mock_host_acquire(&f.host, 20000u));
    CHECK(wifi_owner_budget(&f.context, 250000u));
    CHECK(host_budget_seen == 10u);
    mock_host_release(&f.host);

    reset_counters();
    fake_now_us = 8000000u;
    CHECK(mock_host_acquire(&f.host, 20000u));
    CHECK(!wifi_owner_budget(&f.context, 250000u));
    CHECK(host_budget_calls == 0u);
    mock_host_release(&f.host);
    CHECK(host_held == 0u);
}

static void test_stopped_unpublishes_before_callback(void)
{
    struct fixture f;
    fixture_init(&f);
    wifi_resource_base = &f.base;
    CHECK(wifi_device_attach(&f.base, &f, &client_callbacks));
    stopped_calls = 0u;
    callback_stop_state_ok = 0u;
    f.control.discovery_state = P4_WIFI_DISCOVERY_READY;
    f.control.discovery_nonce = 19u;
    f.control.discovery_token = 3u;
    memset(&f.control.discovery_result, 0xa5, sizeof(f.control.discovery_result));
    memset(&f.control.discovery_decoded, 0xa5, sizeof(f.control.discovery_decoded));
    memset(f.control.discovery_response, 0xa5, sizeof(f.control.discovery_response));
    wifi_test_owner_stop_publish(&f.base);
    CHECK(f.control.discovery_state == P4_WIFI_DISCOVERY_FAILED);
    CHECK(f.control.discovery_nonce == 19u && f.control.discovery_token == 0u);
    {
        struct p4_wifi_discovery_result empty_result = {0};
        struct p4_hosted_discovery_result empty_decoded = {0};
        unsigned i;
        CHECK(memcmp(&f.control.discovery_result, &empty_result,
                     sizeof(empty_result)) == 0);
        CHECK(memcmp(&f.control.discovery_decoded, &empty_decoded,
                     sizeof(empty_decoded)) == 0);
        for (i = 0; i < sizeof(f.control.discovery_response); ++i)
            CHECK(f.control.discovery_response[i] == 0u);
    }
    CHECK(stopped_calls == 1u && callback_stop_state_ok == 1u);
    CHECK(f.base.client == &f && f.base.callbacks == &client_callbacks);
    CHECK(!wifi_device_send(&f.base, f.frame, sizeof(f.frame)));
    CHECK(host_acquire_calls == 0u && host_held == 0u);
    CHECK(f.base.client_lock.depth == 0u);
    wifi_device_detach(&f.base, &f);
    CHECK(f.base.client == NULL && !f.base.ready && f.base.runtime == NULL);
}

static void test_terminal_fence_survives_owner_stop(void)
{
    ULONG mode, state;
    for (mode = P4_WIFI_MAINT_FENCE_INCOMPLETE;
         mode <= P4_WIFI_MAINT_FENCE_WAIT_REBOOT; ++mode) {
        for (state = P4_WIFI_MAINT_FENCING;
             state <= P4_WIFI_MAINT_FENCED; ++state) {
            struct fixture f;
            fixture_init(&f);
            wifi_resource_base = &f.base;
            CHECK(wifi_device_attach(&f.base, &f, &client_callbacks));
            f.base.maintenance_fence_mode = mode;
            f.base.maintenance_state = state;
            wifi_test_owner_stop_publish(&f.base);
            CHECK(f.base.maintenance_fence_mode == mode);
            CHECK(f.base.maintenance_state == (state == P4_WIFI_MAINT_FENCED
                ? P4_WIFI_MAINT_FENCED : P4_WIFI_MAINT_FAILED));
            CHECK(!f.base.ready && !f.base.associated);
            CHECK(!f.base.runtime && !f.base.owner_task);
            CHECK(!wifi_device_send(&f.base, f.frame, sizeof(f.frame)));
            CHECK(!host_acquire_calls && !host_held);
        }
    }
}

int main(void)
{
    test_registration_and_detach();
    test_frame_dispatch_copy_and_lock_boundary();
    test_send_preflight_refusals();
    test_send_one_sta_transfer_and_wipe();
    test_send_failure_is_single_attempt_and_wiped();
    test_send_frame_bounds_and_checksums();
    test_budget_and_acquire_failure_cleanup();
    test_association_budget_uses_remaining_deadline();
    test_stopped_unpublishes_before_callback();
    test_terminal_fence_survives_owner_stop();
    printf("wifi device bridge: %lu checks passed (source-extracted callbacks, mocked Exec/host)\n",
           checks);
    return 0;
}
