/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
/* Exercise the extracted production request wrapper against the real Hosted
 * owner. No controller or hardware behavior is claimed. */
#include "hosted_owner.h"
#include "hosted_mac.h"
#include "hosted_scan.h"
#include "hosted_wifi.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint32_t ULONG;

struct wifi_probe_context {
    uint32_t generation;
};

struct p4_wifi_scan_result {
    struct p4_hosted_scan_records aps;
};

struct wifi_owner_control {
    struct p4_hosted_owner_ops ops;
    struct p4_hosted_rx_ops rx_ops;
    struct p4_hosted_owner_result result;
    struct p4_hosted_mac mac;
    struct p4_hosted_scan_count scan_count;
    struct p4_wifi_scan_result scan_result;
    uint16_t scan_capacity;
    uint8_t request[256];
    uint32_t uid;
    int32_t status;
    unsigned init_attempted, start_attempted;
};

static const struct p4_hosted_wifi_config wifi_reference_profile = {0};
static unsigned test_checks;
static unsigned callback_calls;

#define TEST_GENERATION UINT32_C(0x1a2b3c4d)

#define CHECK(expression) do { \
    ++test_checks; \
    if (!(expression)) { \
        fprintf(stderr, "check failed at %s:%d: %s\n", __FILE__, __LINE__, \
                #expression); \
        exit(1); \
    } \
} while (0)

static void p4host_zero(void *memory, size_t size)
{
    volatile uint8_t *bytes = (volatile uint8_t *)memory;
    size_t index;
    for (index = 0u; index < size; ++index)
        bytes[index] = 0u;
}

static int fake_acquire(void *context, uint32_t remaining_us)
{
    (void)context;
    (void)remaining_us;
    ++callback_calls;
    return 1;
}

static void fake_release(void *context)
{
    (void)context;
    ++callback_calls;
}

static uint32_t fake_now(void *context)
{
    (void)context;
    ++callback_calls;
    return 1u;
}

static void fake_wait(void *context)
{
    (void)context;
    ++callback_calls;
}

static int fake_set_budget(void *context, uint32_t slice_us)
{
    (void)context;
    (void)slice_us;
    ++callback_calls;
    return 1;
}

static int fake_stopping(void *context)
{
    (void)context;
    ++callback_calls;
    return 0;
}

static int fake_dispatch_event(void *context,
                               const struct p4_hosted_inbox_item *item)
{
    (void)context;
    (void)item;
    ++callback_calls;
    return 1;
}

static int fake_dispatch_frame(void *context, const uint8_t *bytes,
                               size_t size)
{
    (void)context;
    (void)bytes;
    (void)size;
    ++callback_calls;
    return 1;
}

static int fake_rx_session(void *context, uint32_t generation)
{
    (void)context;
    (void)generation;
    ++callback_calls;
    return 1;
}

static int fake_rx_budget(void *context)
{
    (void)context;
    ++callback_calls;
    return 1;
}

static int fake_rx_read_word(void *context, uint32_t address, uint32_t *word)
{
    (void)context;
    (void)address;
    (void)word;
    ++callback_calls;
    return 1;
}

static int fake_rx_write_word(void *context, uint32_t address, uint32_t word)
{
    (void)context;
    (void)address;
    (void)word;
    ++callback_calls;
    return 1;
}

static int fake_rx_read_fifo(void *context,
    const struct p4_hosted_fifo_plan *plan, uint8_t *buffer)
{
    (void)context;
    (void)plan;
    (void)buffer;
    ++callback_calls;
    return 1;
}

static int fake_tx_read_word(void *context, uint32_t address, uint32_t *word)
{
    (void)context;
    (void)address;
    (void)word;
    ++callback_calls;
    return 1;
}

static int fake_tx_write_fifo(void *context,
    const struct p4_hosted_fifo_plan *plan, const uint8_t *buffer,
    struct p4_hosted_tx_completion *completion)
{
    (void)context;
    (void)plan;
    (void)buffer;
    (void)completion;
    ++callback_calls;
    return 1;
}

static void fake_tx_yield(void *context)
{
    (void)context;
    ++callback_calls;
}

#include "wifi_owner_reservation_extract.inc"

struct fixture {
    struct wifi_probe_context context;
    struct p4_hosted_owner owner;
    struct p4_hosted_fifo fifo;
    struct wifi_owner_control control;
    struct p4_hosted_tx_ops tx;
};

static struct fixture fixture;

static void fixture_init(void)
{
    p4host_zero(&fixture, sizeof(fixture));
    fixture.context.generation = TEST_GENERATION;
    CHECK(p4_hosted_rx_start(&fixture.owner.rx, TEST_GENERATION));
    CHECK(p4_hosted_fifo_start(&fixture.fifo, TEST_GENERATION, 20u));
    CHECK(p4_hosted_owner_ids_init(&fixture.owner, TEST_GENERATION, 0u));

    fixture.control.ops.context = &fixture;
    fixture.control.ops.acquire = fake_acquire;
    fixture.control.ops.release = fake_release;
    fixture.control.ops.now_us = fake_now;
    fixture.control.ops.wait = fake_wait;
    fixture.control.ops.set_budget = fake_set_budget;
    fixture.control.ops.stopping = fake_stopping;
    fixture.control.ops.dispatch_event = fake_dispatch_event;
    fixture.control.ops.dispatch_frame = fake_dispatch_frame;
    fixture.control.ops.rx = &fixture.control.rx_ops;
    fixture.control.ops.tx = &fixture.tx;

    fixture.control.rx_ops.context = &fixture;
    fixture.control.rx_ops.session_ok = fake_rx_session;
    fixture.control.rx_ops.budget_ok = fake_rx_budget;
    fixture.control.rx_ops.read_word = fake_rx_read_word;
    fixture.control.rx_ops.write_word = fake_rx_write_word;
    fixture.control.rx_ops.read_fifo = fake_rx_read_fifo;

    fixture.tx.context = &fixture;
    fixture.tx.read_word = fake_tx_read_word;
    fixture.tx.write_fifo = fake_tx_write_fifo;
    fixture.tx.session_ok = fake_rx_session;
    fixture.tx.budget_ok = fake_rx_budget;
    fixture.tx.yield = fake_tx_yield;
}

static void test_real_outer_preflight_refusal_settles(void)
{
    uint32_t next_uid = 0u;

    fixture_init();
    /* This makes the real p4_hosted_owner_exchange outer preflight refuse at
     * ops_valid(), before any host or TX callback. It is not an exchange mock. */
    fixture.control.ops.wait = NULL;
    memset(&fixture.control.result, 0xa5, sizeof(fixture.control.result));
    callback_calls = 0u;

    CHECK(!wifi_owner_request(&fixture.context, &fixture.owner,
        &fixture.control, &fixture.fifo, 0u, 286u, 100000u));
    CHECK(fixture.control.result.submitted == 0u);
    CHECK(fixture.control.result.error == P4_HOSTED_OWNER_OK);
    CHECK(callback_calls == 0u);
    CHECK(fixture.owner.poisoned == 0u && fixture.fifo.poisoned == 0u);
    CHECK(fixture.owner.ids.active_uid == 0u);
    CHECK(fixture.owner.ids.active_request_id == 0u);
    CHECK(fixture.owner.ids.next_uid == 2u);
    CHECK(p4_hosted_request_ids_reserve(&fixture.owner.ids,
        TEST_GENERATION, 288u, &next_uid));
    CHECK(next_uid == 2u);
    CHECK(p4_hosted_request_ids_finish(&fixture.owner.ids,
        TEST_GENERATION, next_uid, 288u));
}

static void reserve_fixture_identity(uint32_t *uid)
{
    fixture_init();
    CHECK(p4_hosted_request_ids_reserve(&fixture.owner.ids,
        TEST_GENERATION, 286u, uid));
}

static void test_guard_refusals_preserve_reservation(void)
{
    uint32_t uid;

    reserve_fixture_identity(&uid);
    CHECK(!wifi_owner_finish_unsubmitted_reservation(&fixture.owner,
        &fixture.fifo, TEST_GENERATION, uid, 286u, 1u));
    CHECK(fixture.owner.ids.active_uid == uid);

    reserve_fixture_identity(&uid);
    fixture.owner.poisoned = 1u;
    CHECK(!wifi_owner_finish_unsubmitted_reservation(&fixture.owner,
        &fixture.fifo, TEST_GENERATION, uid, 286u, 0u));
    CHECK(fixture.owner.ids.active_uid == uid);

    reserve_fixture_identity(&uid);
    CHECK(!wifi_owner_finish_unsubmitted_reservation(&fixture.owner,
        &fixture.fifo, TEST_GENERATION + 1u, uid, 286u, 0u));
    CHECK(fixture.owner.ids.active_uid == uid);

    reserve_fixture_identity(&uid);
    fixture.fifo.poisoned = 1u;
    CHECK(!wifi_owner_finish_unsubmitted_reservation(&fixture.owner,
        &fixture.fifo, TEST_GENERATION, uid, 286u, 0u));
    CHECK(fixture.owner.ids.active_uid == uid);

    reserve_fixture_identity(&uid);
    CHECK(!wifi_owner_finish_unsubmitted_reservation(&fixture.owner,
        &fixture.fifo, TEST_GENERATION, uid + 1u, 286u, 0u));
    CHECK(fixture.owner.ids.active_uid == uid);

    reserve_fixture_identity(&uid);
    CHECK(!wifi_owner_finish_unsubmitted_reservation(&fixture.owner,
        &fixture.fifo, TEST_GENERATION, uid, 288u, 0u));
    CHECK(fixture.owner.ids.active_uid == uid);

    reserve_fixture_identity(&uid);
    fixture.owner.pending_active = 1u;
    fixture.owner.pending_uid = uid;
    fixture.owner.pending_request_id = 286u;
    CHECK(!wifi_owner_finish_unsubmitted_reservation(&fixture.owner,
        &fixture.fifo, TEST_GENERATION, uid, 286u, 0u));
    CHECK(fixture.owner.ids.active_uid == uid);

    reserve_fixture_identity(&uid);
    fixture.fifo.busy = 1u;
    CHECK(!wifi_owner_finish_unsubmitted_reservation(&fixture.owner,
        &fixture.fifo, TEST_GENERATION, uid, 286u, 0u));
    CHECK(fixture.owner.ids.active_uid == uid);
}

int main(void)
{
    test_real_outer_preflight_refusal_settles();
    test_guard_refusals_preserve_reservation();
    printf("owner reservation checks: %u\n", test_checks);
    return 0;
}
