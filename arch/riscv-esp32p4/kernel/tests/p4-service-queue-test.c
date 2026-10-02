/* Host fixture for durable P4 Task-service readiness and claim-next. */
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define P4_TASK_PINS_TEST 1
#define P4_PUBLIC_SPINLOCK_TEST 1

#include "../p4_public_spinlock.h"
#include "../p4_task_registry.h"

static unsigned int checks;
static unsigned int failures;

#define CHECK(condition)                                                     \
    do                                                                       \
    {                                                                        \
        ++checks;                                                            \
        if (!(condition))                                                    \
        {                                                                    \
            ++failures;                                                     \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,        \
                    #condition);                                            \
        }                                                                    \
    } while (0)

static int mock_mie = 1;
static unsigned int mock_disable_depth;
static unsigned int mock_enable_calls;
static unsigned int mock_gate_acquires;
static unsigned int mock_gate_unlocks;
static unsigned int mock_locked_registry_calls;
static unsigned int mock_unpin_calls;
static unsigned int mock_leave_running_calls;
static unsigned int mock_ready_reservations;
static unsigned int mock_claim_next_calls;
static int mock_gate_held;
static void (*mock_enable_callback)(void);

static jmp_buf mock_fatal_jump;
static int mock_fatal_armed;
static unsigned int mock_fatal_calls;
static enum p4_public_spinlock_result mock_fatal_reason;
static int mock_fatal_mie_before;
static int mock_fatal_mie_after;

void Disable(void);
void Enable(void);

#include "../p4_task_registry.c"
#include "../p4_task_lifetime.c"

static void check_registry_gate(void)
{
    ++mock_locked_registry_calls;
    CHECK(mock_mie == 0);
    CHECK(mock_disable_depth != 0u);
    CHECK(mock_gate_held);
}

static int pins_test_unpin(
    struct p4_task_registry *registry, struct Task *identity)
{
    check_registry_gate();
    ++mock_unpin_calls;
    return p4_task_registry_unpin_locked(registry, identity);
}

static int pins_test_leave_running(
    struct p4_task_registry *registry, struct Task *identity, uint32_t hart)
{
    check_registry_gate();
    ++mock_leave_running_calls;
    return p4_task_registry_leave_running_locked(registry, identity, hart);
}

static struct Task *pins_test_take_service_ready(
    struct p4_task_registry *registry, struct Task *identity)
{
    check_registry_gate();
    ++mock_ready_reservations;
    return p4_task_registry_take_service_ready_locked(registry, identity);
}

static int pins_test_claim_next_service(
    struct p4_task_registry *registry,
    struct p4_task_registry_record **out_record)
{
    check_registry_gate();
    ++mock_claim_next_calls;
    return p4_task_registry_claim_next_service_locked(registry, out_record);
}

#define p4_task_registry_unpin_locked pins_test_unpin
#define p4_task_registry_leave_running_locked pins_test_leave_running
#define p4_task_registry_take_service_ready_locked pins_test_take_service_ready
#define p4_task_registry_claim_next_service_locked pins_test_claim_next_service
#include "../p4_task_pins.c"
#undef p4_task_registry_unpin_locked
#undef p4_task_registry_leave_running_locked
#undef p4_task_registry_take_service_ready_locked
#undef p4_task_registry_claim_next_service_locked

/* Publisher transport is outside this registry fixture; verify only the
 * copied endpoint and leaf-gate release contract, not physical delivery. */
void krnP4CleanupPoke(struct ExecBase *base)
{
    (void)base;
    CHECK(!mock_gate_held);
}

void krnP4RetiredOwnerPoke(uint32_t destination, struct ExecBase *base)
{
    (void)base;
    CHECK(!mock_gate_held);
    CHECK(destination < 2u);
}

void Disable(void)
{
    ++mock_disable_depth;
    mock_mie = 0;
}

void Enable(void)
{
    void (*callback)(void);

    ++mock_enable_calls;
    CHECK(!mock_gate_held);
    if (mock_disable_depth == 0u)
    {
        CHECK(0 && "unbalanced Enable");
        return;
    }

    --mock_disable_depth;
    if (mock_disable_depth == 0u)
    {
        mock_mie = 1;
        callback = mock_enable_callback;
        mock_enable_callback = NULL;
        if (callback != NULL)
            callback();
    }
}

enum p4_public_spinlock_result krnP4PublicSpinInit(spinlock_t *lock)
{
    CHECK(mock_mie == 0);
    CHECK(!mock_gate_held);
    lock->lock = 0;
    lock->s_Owner = 0;
    return P4_PUBLIC_SPIN_OK;
}

enum p4_public_spinlock_result krnP4PublicSpinTryAcquire(
    spinlock_t *lock, uint32_t mode)
{
    CHECK(mock_mie == 0);
    CHECK(!mock_gate_held);
    CHECK(mode == SPINLOCK_MODE_WRITE);
    ++mock_gate_acquires;
    mock_gate_held = 1;
    lock->lock = mode;
    return P4_PUBLIC_SPIN_OK;
}

enum p4_public_spinlock_result krnP4PublicSpinUnlock(spinlock_t *lock)
{
    CHECK(mock_mie == 0);
    CHECK(mock_gate_held);
    ++mock_gate_unlocks;
    mock_gate_held = 0;
    lock->lock = 0;
    lock->s_Owner = 0;
    return P4_PUBLIC_SPIN_OK;
}

void krnP4PublicSpinFatal(enum p4_public_spinlock_result reason)
{
    ++mock_fatal_calls;
    mock_fatal_reason = reason;
    mock_fatal_mie_before = mock_mie;
    mock_mie = 0;
    mock_fatal_mie_after = mock_mie;
    if (mock_fatal_armed)
        longjmp(mock_fatal_jump, 1);
    fputs("unexpected P4 public-spin fatal\n", stderr);
    abort();
}

static void mock_reset_after_fatal(void)
{
    p4_task_pin_gate.lock = 0;
    p4_task_pin_gate.s_Owner = 0;
    mock_gate_held = 0;
    mock_disable_depth = 0;
    mock_mie = 1;
}

static void expect_bad_state_fatal(void (*operation)(void))
{
    unsigned int prior = mock_fatal_calls;
    int caught;

    mock_fatal_armed = 1;
    caught = setjmp(mock_fatal_jump);
    if (caught == 0)
    {
        operation();
        mock_fatal_armed = 0;
        CHECK(0 && "malformed registry did not fail closed");
        return;
    }

    mock_fatal_armed = 0;
    CHECK(mock_fatal_calls == prior + 1u);
    CHECK(mock_fatal_reason == P4_PUBLIC_SPIN_BAD_STATE);
    CHECK(mock_fatal_mie_before == 0);
    CHECK(mock_fatal_mie_after == 0);
}

static struct p4_task_registry_record *fatal_claimed_record;

static void operation_claim_next_malformed(void)
{
    fatal_claimed_record = krnP4TaskRecordClaimNextService(NULL);
}

static struct p4_task_registry_record *ready_expected_record;
static struct p4_task_registry_record *ready_callback_claimed;
static unsigned int ready_callback_calls;

static void claim_durable_ready_before_enable(void)
{
    ++ready_callback_calls;
    CHECK(mock_mie == 1);
    CHECK(!mock_gate_held);
    CHECK(ready_expected_record != NULL);
    CHECK(ready_expected_record->service_pending == 1u);
    CHECK(ready_expected_record->service_queued == 1u);
    CHECK(ready_expected_record->lifetime.notification_pins == 1u);
    CHECK(ready_expected_record->lifetime.running_harts == 0u);
    CHECK(ready_callback_claimed == NULL);
    ready_callback_claimed = krnP4TaskRecordClaimNextService(NULL);
}

static int test_prepare_and_empty_claim(void)
{
    struct p4_task_registry_record record = {0};
    struct Task *identity = (struct Task *)(uintptr_t)0x1000u;
    unsigned int prior_claims;

    CHECK(mock_mie == 1 && mock_disable_depth == 0u);
    Disable();
    CHECK(krnP4TaskPinsPrepare() == 1);
    Enable();
    CHECK(mock_mie == 1 && !mock_gate_held);

    prior_claims = mock_claim_next_calls;
    CHECK(krnP4TaskRecordClaimNextService(NULL) == NULL);
    CHECK(mock_claim_next_calls == prior_claims + 1u);

    CHECK(krnP4TaskRecordPublish(identity, &record, NULL) == 1);
    prior_claims = mock_claim_next_calls;
    CHECK(krnP4TaskRecordClaimNextService(NULL) == NULL);
    CHECK(mock_claim_next_calls == prior_claims + 1u);
    CHECK(record.identity == identity && record.next == NULL);
    CHECK(record.service_pending == 0u && record.service_queued == 0u);
    CHECK(krnP4TaskRecordRetire(identity, NULL) == 1);
    CHECK(krnP4TaskRecordClaimReclaim(identity, NULL) == &record);
    CHECK(mock_mie == 1 && mock_disable_depth == 0u && !mock_gate_held);
    return 1;
}

static int prepare_retired_running_record(
    struct p4_task_registry_record *record,
    struct Task *identity,
    unsigned int hart)
{
    memset(record, 0, sizeof(*record));
    CHECK(krnP4TaskRecordPublishPinned(identity, record, NULL) == 1);
    CHECK(krnP4TaskRecordEnterRunning(identity, hart, NULL) == 1);
    CHECK(krnP4SignalTaskPin(identity, NULL) == 1);
    CHECK(krnP4TaskRecordRetireForService(identity, NULL) == 1);
    CHECK(record->service_pending == 1u && record->service_queued == 0u);
    CHECK(record->lifetime.notification_pins == 3u);
    CHECK(record->lifetime.running_harts == (UINT32_C(1) << hart));
    return 1;
}

static int finish_with_ready_callback(
    struct p4_task_registry_record *record,
    void (*last_release)(struct Task *identity, unsigned int hart),
    struct Task *identity,
    unsigned int hart)
{
    unsigned int prior_enables = mock_enable_calls;

    ready_expected_record = record;
    ready_callback_claimed = NULL;
    ready_callback_calls = 0u;
    mock_enable_callback = claim_durable_ready_before_enable;
    last_release(identity, hart);
    mock_enable_callback = NULL;
    ready_expected_record = NULL;

    CHECK(ready_callback_calls == 1u);
    CHECK(mock_enable_calls == prior_enables + 2u);
    CHECK(ready_callback_claimed == record);
    CHECK(record->next == NULL);
    CHECK(record->service_pending == 0u && record->service_queued == 0u);
    CHECK(record->lifetime.notification_pins == 0u);
    CHECK(record->lifetime.running_harts == 0u);
    CHECK((record->lifetime.flags & P4_TASK_LIFETIME_CLAIMED) != 0u);
    CHECK(krnP4TaskRecordClaimNextService(NULL) == NULL);
    return 1;
}

static void last_unpin(struct Task *identity, unsigned int hart)
{
    (void)hart;
    krnP4SignalTaskUnpin(identity, NULL);
}

static void last_leave_running(struct Task *identity, unsigned int hart)
{
    krnP4TaskRecordLeaveRunning(identity, hart, NULL);
}

static int test_both_harts_and_release_orders(void)
{
    unsigned int hart;
    unsigned int order;

    for (hart = 0; hart < 2u; ++hart)
    {
        for (order = 0; order < 2u; ++order)
        {
            struct p4_task_registry_record record = {0};
            struct Task *identity =
                (struct Task *)(uintptr_t)(0x2000u + hart * 0x100u + order * 0x10u);
            unsigned int prior_ready;

            CHECK(prepare_retired_running_record(&record, identity, hart));
            prior_ready = mock_ready_reservations;
            if (order == 0u)
            {
                /* Last ordinary pins drain first; the final RUN release must
                 * reserve readiness before the enable callback can claim. */
                krnP4SignalTaskUnpin(identity, NULL);
                CHECK(record.service_queued == 0u);
                CHECK(record.lifetime.notification_pins == 2u);
                krnP4SignalTaskUnpin(identity, NULL);
                CHECK(record.service_queued == 0u);
                CHECK(record.lifetime.notification_pins == 1u);
                CHECK(record.lifetime.running_harts != 0u);
                CHECK(finish_with_ready_callback(
                    &record, last_leave_running, identity, hart));
            }
            else
            {
                /* RUN drains first; the last ordinary unpin must reserve. */
                krnP4TaskRecordLeaveRunning(identity, hart, NULL);
                CHECK(record.service_queued == 0u);
                CHECK(record.lifetime.notification_pins == 3u);
                krnP4SignalTaskUnpin(identity, NULL);
                CHECK(record.service_queued == 0u);
                CHECK(record.lifetime.notification_pins == 2u);
                CHECK(finish_with_ready_callback(
                    &record, last_unpin, identity, hart));
            }
            CHECK(mock_ready_reservations == prior_ready + 3u);
            CHECK(mock_mie == 1 && mock_disable_depth == 0u && !mock_gate_held);
        }
    }
    return 1;
}

static int test_multiple_records_exactly_once(void)
{
    struct p4_task_registry_record first = {0};
    struct p4_task_registry_record second = {0};
    struct Task *first_identity = (struct Task *)(uintptr_t)0x3000u;
    struct Task *second_identity = (struct Task *)(uintptr_t)0x4000u;

    CHECK(krnP4TaskRecordPublishPinned(first_identity, &first, NULL) == 1);
    CHECK(krnP4TaskRecordRetireForService(first_identity, NULL) == 1);
    krnP4SignalTaskUnpin(first_identity, NULL);
    CHECK(first.service_queued == 1u);

    CHECK(krnP4TaskRecordPublishPinned(second_identity, &second, NULL) == 1);
    CHECK(krnP4TaskRecordRetireForService(second_identity, NULL) == 1);
    krnP4SignalTaskUnpin(second_identity, NULL);
    CHECK(second.service_queued == 1u);
    CHECK(p4_task_records.head == &second);

    CHECK(krnP4TaskRecordClaimNextService(NULL) == &second);
    CHECK(krnP4TaskRecordClaimNextService(NULL) == &first);
    CHECK(krnP4TaskRecordClaimNextService(NULL) == NULL);
    CHECK(second.next == NULL && first.next == NULL);
    CHECK(mock_mie == 1 && mock_disable_depth == 0u && !mock_gate_held);
    return 1;
}

static int test_claim_next_outputs_and_corruption(void)
{
    struct p4_task_registry empty = {0};
    struct p4_task_registry registry = {0};
    struct p4_task_registry_record candidate = {0};
    struct p4_task_registry_record tail = {0};
    struct p4_task_registry_record *out;
    struct Task *candidate_identity = (struct Task *)(uintptr_t)0x5000u;
    struct Task *tail_identity = (struct Task *)(uintptr_t)0x6000u;
    struct p4_task_registry_record *head_before;
    uint32_t count_before;

    out = (struct p4_task_registry_record *)(uintptr_t)1u;
    CHECK(p4_task_registry_claim_next_service_locked(&empty, &out) == 0);
    CHECK(out == NULL);
    CHECK(p4_task_registry_claim_next_service_locked(&empty, NULL) == -1);
    out = (struct p4_task_registry_record *)(uintptr_t)1u;
    CHECK(p4_task_registry_claim_next_service_locked(NULL, &out) == -1);
    CHECK(out == NULL);

    CHECK(p4_task_registry_register_locked(&registry, &tail, tail_identity) == 1);
    CHECK(p4_task_registry_register_locked(
        &registry, &candidate, candidate_identity) == 1);
    CHECK(p4_task_registry_retire_for_service_locked(
        &registry, candidate_identity) == 1);
    CHECK(p4_task_registry_take_service_ready_locked(
        &registry, candidate_identity) == candidate_identity);
    CHECK(registry.head == &candidate && candidate.next == &tail);
    head_before = registry.head;
    count_before = registry.count;

    /* A malformed later entry must be found by whole-registry validation
     * before the valid head reservation can be consumed. */
    tail.service_queued = 2u;
    out = (struct p4_task_registry_record *)(uintptr_t)1u;
    CHECK(p4_task_registry_claim_next_service_locked(&registry, &out) == -1);
    CHECK(out == NULL);
    CHECK(registry.head == head_before && registry.count == count_before);
    CHECK(candidate.service_pending == 1u && candidate.service_queued == 1u);
    CHECK(candidate.lifetime.notification_pins == 1u);
    tail.service_queued = 0u;

    /* Count overflow is refused without consuming a queued head. */
    registry.count = P4_TASK_REGISTRY_LIMIT + 1u;
    out = (struct p4_task_registry_record *)(uintptr_t)1u;
    CHECK(p4_task_registry_claim_next_service_locked(&registry, &out) == -1);
    CHECK(out == NULL);
    CHECK(registry.head == head_before);
    CHECK(candidate.service_pending == 1u && candidate.service_queued == 1u);
    registry.count = count_before;

    /* A cycle within the claimed count fails in bounded validation. */
    tail.next = &candidate;
    out = (struct p4_task_registry_record *)(uintptr_t)1u;
    CHECK(p4_task_registry_claim_next_service_locked(&registry, &out) == -1);
    CHECK(out == NULL);
    CHECK(registry.head == head_before && registry.count == count_before);
    CHECK(candidate.service_queued == 1u);
    tail.next = NULL;

    /* Selected identity duplication is rejected by the existing queued-claim
     * path, with the ready reservation and links left untouched. */
    tail.identity = candidate_identity;
    out = (struct p4_task_registry_record *)(uintptr_t)1u;
    CHECK(p4_task_registry_claim_next_service_locked(&registry, &out) == -1);
    CHECK(out == NULL);
    CHECK(registry.head == head_before && registry.count == count_before);
    CHECK(candidate.service_queued == 1u && candidate.service_pending == 1u);
    CHECK(candidate.next == &tail && tail.next == NULL);
    tail.identity = tail_identity;

    out = NULL;
    CHECK(p4_task_registry_claim_next_service_locked(&registry, &out) == 1);
    CHECK(out == &candidate);
    CHECK(registry.head == &tail && registry.count == 1u);
    CHECK(candidate.next == NULL && candidate.service_pending == 0u);
    CHECK(candidate.service_queued == 0u);
    CHECK(candidate.lifetime.notification_pins == 0u);
    CHECK((candidate.lifetime.flags & P4_TASK_LIFETIME_CLAIMED) != 0u);
    return 1;
}

static int test_binding_fails_closed_on_corruption(void)
{
    struct p4_task_registry_record record = {0};
    struct Task *identity = (struct Task *)(uintptr_t)0x7000u;
    unsigned int prior_enable_calls;
    unsigned int prior_claim_calls;

    CHECK(krnP4TaskRecordPublishPinned(identity, &record, NULL) == 1);
    CHECK(krnP4TaskRecordRetireForService(identity, NULL) == 1);
    krnP4SignalTaskUnpin(identity, NULL);
    CHECK(record.service_queued == 1u);
    record.service_queued = 2u;
    fatal_claimed_record = NULL;
    prior_enable_calls = mock_enable_calls;
    prior_claim_calls = mock_claim_next_calls;
    expect_bad_state_fatal(operation_claim_next_malformed);
    CHECK(fatal_claimed_record == NULL);
    CHECK(mock_claim_next_calls == prior_claim_calls + 1u);
    CHECK(mock_enable_calls == prior_enable_calls);
    CHECK(mock_gate_held && mock_disable_depth == 1u && mock_mie == 0);
    CHECK(record.identity == identity && record.service_pending == 1u);
    CHECK(record.service_queued == 2u);
    mock_reset_after_fatal();

    record.service_queued = 1u;
    CHECK(krnP4TaskRecordClaimNextService(NULL) == &record);
    CHECK(record.next == NULL && record.service_pending == 0u);
    CHECK(record.service_queued == 0u);
    CHECK(mock_mie == 1 && mock_disable_depth == 0u && !mock_gate_held);
    return 1;
}

int main(void)
{
    (void)test_prepare_and_empty_claim();
    (void)test_both_harts_and_release_orders();
    (void)test_multiple_records_exactly_once();
    (void)test_claim_next_outputs_and_corruption();
    (void)test_binding_fails_closed_on_corruption();

    CHECK(mock_locked_registry_calls == mock_unpin_calls +
          mock_leave_running_calls + mock_ready_reservations +
          mock_claim_next_calls);
    CHECK(mock_gate_acquires == mock_gate_unlocks + mock_fatal_calls);
    CHECK(mock_mie == 1 && mock_disable_depth == 0u && !mock_gate_held);

    if (failures != 0u)
    {
        fprintf(stderr, "%u of %u checks failed\n", failures, checks);
        return EXIT_FAILURE;
    }

    printf("P4 service-queue checks: %u passed\n", checks);
    return EXIT_SUCCESS;
}
