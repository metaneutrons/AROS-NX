/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
/* Host model for the source-extracted production maintenance handshake.
 * The correlated WiFiStop/WiFiStart transport call alone is mocked. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint32_t ULONG;
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

struct p4_hosted_owner_ids {
    ULONG active_uid;
};

struct p4_hosted_owner {
    unsigned poisoned;
    ULONG generation;
    unsigned pending_active;
    struct p4_hosted_owner_ids ids;
    unsigned partial_active;
};

struct p4_hosted_fifo {
    unsigned poisoned;
    ULONG generation;
    unsigned busy;
};

struct wifi_owner_control {
    unsigned unused;
};

struct P4SDMMCHost {
    ULONG (*now_us)(void);
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
    unsigned checksum;
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
    return TRUE;
}

static void ReleaseSemaphore(struct SignalSemaphore *semaphore)
{
    semaphore->held = 0u;
}

static void p4host_zero(void *memory, size_t size)
{
    memset(memory, 0, size);
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

#ifdef WIFI_MAINTENANCE_MUTANT
#include "wifi_maintenance_mutant.inc"
#else
#include "wifi_maintenance_extract.inc"
#endif

static void reset_fixture(void)
{
    memset(&base, 0, sizeof(base));
    memset(&host, 0, sizeof(host));
    memset(&context, 0, sizeof(context));
    memset(&owner, 0, sizeof(owner));
    memset(&fifo, 0, sizeof(fifo));
    memset(&control, 0, sizeof(control));
    memset(&runtime, 0, sizeof(runtime));
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
    owner.generation = TEST_GENERATION;
    fifo.generation = TEST_GENERATION;
    runtime.context = &context;
    runtime.owner = &owner;
    runtime.control = &control;
    runtime.fifo = &fifo;
    runtime.checksum = 1u;
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
    CHECK(wifi_maintenance_service(&base, &runtime) == 1);
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
        CHECK(wifi_maintenance_service(&base, &runtime) == 2);
        CHECK(request_calls == 0u);
        CHECK(stopped_calls == 0u);
        CHECK(base.maintenance_state == P4_WIFI_MAINT_REQUESTED);
        CHECK(base.associated == TRUE);
    }
}

static void test_stop_ack_admission_and_active_silence(void)
{
    prepare_service(P4_WIFI_MAINT_REQUESTED);
    CHECK(wifi_maintenance_service(&base, &runtime) == 2);
    CHECK(request_calls == 1u);
    CHECK(request_ids[0] == 281u);
    CHECK(request_budgets[0] == 8000000u);
    CHECK(base.maintenance_state == P4_WIFI_MAINT_ACTIVE);
    CHECK(base.associated == FALSE);
    CHECK(base.link_state == P4_WIFI_LINK_UNKNOWN);
    CHECK(base.confirmed_ap.ssid[0] == 0u);
    CHECK(base.observed_ap.ssid[0] == 0u);
    CHECK(stopped_calls == 1u);

    CHECK(wifi_maintenance_service(&base, &runtime) == 2);
    CHECK(request_calls == 1u);
    CHECK(stopped_calls == 1u);

    prepare_service(P4_WIFI_MAINT_REQUESTED);
    request_succeeds = FALSE;
    CHECK(wifi_maintenance_service(&base, &runtime) == 0);
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
    CHECK(wifi_maintenance_service(&base, &runtime) == 1);
    CHECK(request_calls == 1u);
    CHECK(request_ids[0] == 280u);
    CHECK(request_budgets[0] == 8000000u);
    CHECK(base.maintenance_state == P4_WIFI_MAINT_RELEASED);
    CHECK(base.associated == FALSE);
    CHECK(ready_calls == 1u);

    prepare_service(P4_WIFI_MAINT_RELEASING);
    request_succeeds = FALSE;
    CHECK(wifi_maintenance_service(&base, &runtime) == 0);
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
    CHECK(wifi_maintenance_service(&base, &runtime) == 0);
    CHECK(request_calls == 0u);
    CHECK(base.maintenance_state == P4_WIFI_MAINT_FAILED);
    CHECK(base.associated == FALSE);

    prepare_service(P4_WIFI_MAINT_REQUESTED);
    request_advance_us = TEST_LEASE_US;
    CHECK(wifi_maintenance_service(&base, &runtime) == 0);
    CHECK(request_calls == 1u);
    CHECK(base.maintenance_state == P4_WIFI_MAINT_FAILED);
    CHECK(base.associated == FALSE);

    prepare_service(P4_WIFI_MAINT_RELEASING);
    request_advance_us = TEST_LEASE_US;
    CHECK(wifi_maintenance_service(&base, &runtime) == 0);
    CHECK(request_calls == 1u && request_ids[0] == 280u);
    CHECK(base.maintenance_state == P4_WIFI_MAINT_FAILED);
    CHECK(base.associated == FALSE);
    CHECK(ready_calls == 0u);

    prepare_service(P4_WIFI_MAINT_REQUESTED);
    owner.poisoned = 1u;
    CHECK(wifi_maintenance_service(&base, &runtime) == 0);
    CHECK(request_calls == 0u);
    CHECK(base.maintenance_state == P4_WIFI_MAINT_FAILED);

    prepare_service(P4_WIFI_MAINT_REQUESTED);
    fifo.generation += 1u;
    CHECK(wifi_maintenance_service(&base, &runtime) == 0);
    CHECK(request_calls == 0u);
    CHECK(base.maintenance_state == P4_WIFI_MAINT_FAILED);
}

int main(void)
{
    test_normal_poll_gate();
    test_request_identity_and_lock_contention();
    test_pending_release_and_poll();
    test_service_drain_fences();
    test_stop_ack_admission_and_active_silence();
    test_release_start_ack_and_offline_state();
    test_poll_and_release_expiry_and_wrap();
    test_service_deadline_failures();
    printf("wifi maintenance source tests: %u checks passed\n", test_checks);
    return 0;
}
