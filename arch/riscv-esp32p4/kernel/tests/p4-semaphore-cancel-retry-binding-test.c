/* Actual-source tests for protected cancellation retry-interest bindings. */
#define _DEFAULT_SOURCE 1
#define __AROSEXEC_SMP__ 1
#define __AROSPLATFORM_SMP__ 1
#define P4_TASK_PINS_TEST 1
#define P4_PUBLIC_SPINLOCK_TEST 1
#define P4_SEMAPHORE_NODES_TEST 1

#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

int p4_semaphore_operation_admission_baseline_main(void);
#define main p4_semaphore_operation_admission_baseline_main
#include "p4-semaphore-operation-admission-test.c"
#undef main

#include "../p4_semaphore_cancel_retry.h"

#define RETRY_RECORD_CAPACITY 6u
#define RETRY_OPERATION_CAPACITY 8u
#define RETRY_ENTRY_CAPACITY 8u

static struct p4_semaphore_ledger retry_ledger;
static struct p4_semaphore_ledger_operation retry_operations[RETRY_OPERATION_CAPACITY];
static struct p4_semaphore_ledger_entry retry_entries[RETRY_ENTRY_CAPACITY];
static struct p4_task_registry_record retry_records[RETRY_RECORD_CAPACITY];
static struct SignalSemaphore retry_sem_a;
static struct SignalSemaphore retry_sem_b;
static struct ExecBase retry_sysbase;
static struct Task *retry_worker;
static struct Task *retry_owner;
static struct Task *retry_target;
static struct Task *retry_unrelated;
static struct Task *retry_waiter;
static struct p4_task_wait_operation retry_owner_operation;
static struct p4_task_wait_operation retry_target_operation;
static struct p4_task_wait_operation retry_waiter_operation;

struct retry_fixture_image
{
    struct p4_task_registry registry;
    struct p4_task_registry_record records[RETRY_RECORD_CAPACITY];
    struct p4_semaphore_ledger ledger;
    struct p4_semaphore_ledger_operation operations[RETRY_OPERATION_CAPACITY];
    struct p4_semaphore_ledger_entry entries[RETRY_ENTRY_CAPACITY];
    struct SemaphoreRequest requests[8];
    struct SignalSemaphore sem_a;
    struct SignalSemaphore sem_b;
};

static int retry_fixture_reset(void)
{
    if (p4_task_pins_prepared != 1 || mock_irq_depth != 0u ||
        mock_gate_held || mock_outer_semaphore_gate_held)
        return 0;

    p4_task_registry_init(&p4_task_records);
    if (krnP4PublicSpinInit(&p4_task_pin_gate) != P4_PUBLIC_SPIN_OK)
        return 0;
    mock_gate_address = &p4_task_pin_gate;
    mock_disable_calls = 0u;
    mock_enable_calls = 0u;
    mock_acquire_calls = 0u;
    mock_unlock_calls = 0u;
    mock_fatal_calls = 0u;
    mock_poke_calls = 0u;
    mock_retired_poke_calls = 0u;
    mock_task_lock_held = 0;
    p4_cleanup_worker = NULL;
    p4_cleanup_wake_mask = 0u;
    p4_cleanup_wake_pending = false;
    p4_cancel_retry.active = 0u;
    p4_cancel_retry.waiting = 0u;
    memset(retry_records, 0, sizeof(retry_records));
    memset(retry_operations, 0, sizeof(retry_operations));
    memset(retry_entries, 0, sizeof(retry_entries));
    memset(test_boot_requests, 0, sizeof(test_boot_requests));
    memset(&retry_ledger, 0, sizeof(retry_ledger));
    memset(&retry_sem_a, 0, sizeof(retry_sem_a));
    memset(&retry_sem_b, 0, sizeof(retry_sem_b));
    memset(&retry_sysbase, 0, sizeof(retry_sysbase));
    memset(&retry_owner_operation, 0, sizeof(retry_owner_operation));
    memset(&retry_target_operation, 0, sizeof(retry_target_operation));
    memset(&retry_waiter_operation, 0, sizeof(retry_waiter_operation));
    retry_worker = NULL;
    retry_owner = NULL;
    retry_target = NULL;
    retry_unrelated = NULL;
    retry_waiter = NULL;
    test_boot_ledger = &retry_ledger;
    if (p4_semaphore_ledger_init(&retry_ledger, retry_operations,
            RETRY_OPERATION_CAPACITY, retry_entries,
            RETRY_ENTRY_CAPACITY) != 1)
        return 0;
    test_empty_semaphore(&retry_sem_a);
    test_empty_semaphore(&retry_sem_b);
    return 1;
}

static struct Task *retry_identity(void *page, size_t offset)
{
    return guarded_identity(page, offset);
}

static void retry_publish_live(unsigned int slot, struct Task *identity,
                               unsigned int hart)
{
    CHECK(slot < RETRY_RECORD_CAPACITY);
    CHECK(krnP4TaskRecordPublish(identity, &retry_records[slot],
                                  &retry_sysbase) == 1);
    CHECK(krnP4TaskRecordEnterRunning(identity, hart, &retry_sysbase) == 1);
}

static void retry_setup_worker(void *page)
{
    retry_worker = retry_identity(page, 768u);
    retry_publish_live(0u, retry_worker, 0u);
    CHECK(krnP4ProtectCleanupWorker(retry_worker, &retry_sysbase) == 1);
    CHECK(krnP4ArmCleanupWake(retry_worker, UINT32_C(1) << 5,
                              &retry_sysbase) == 1);
}

static void retry_retire_for_service(struct Task *task)
{
    mock_task_lock_held = 1;
    CHECK(krnP4TaskRecordRetireForService(task, &retry_sysbase) == 1);
    mock_task_lock_held = 0;
}

static void retry_drain_wake(void)
{
    struct p4_cleanup_wake wake;
    memset(&wake, 0xa5, sizeof(wake));
    CHECK(krnP4TakeCleanupWake(&wake, &retry_sysbase) == 1);
    CHECK(wake.worker == retry_worker);
    CHECK(wake.mask == (UINT32_C(1) << 5));
    CHECK(!p4_cleanup_wake_pending);
}

static void retry_build_operation(struct Task *task,
    struct p4_task_wait_operation *operation,
    struct SignalSemaphore *const semaphores[], const uint32_t shared[],
    const int expected[], uint32_t count)
{
    CHECK(krnP4SemaphoreOperationReserve(task, count, operation,
                                          &retry_sysbase) == 1);
    test_configure_operation(task, operation, semaphores, shared, count,
                             &retry_sysbase);
    test_acquire_operation(task, operation, semaphores, expected, count);
}

static void retry_snapshot_take(struct retry_fixture_image *image)
{
    memset(image, 0, sizeof(*image));
    memcpy(&image->registry, &p4_task_records, sizeof(image->registry));
    memcpy(image->records, retry_records, sizeof(image->records));
    memcpy(&image->ledger, &retry_ledger, sizeof(image->ledger));
    memcpy(image->operations, retry_operations, sizeof(image->operations));
    memcpy(image->entries, retry_entries, sizeof(image->entries));
    memcpy(image->requests, test_boot_requests, sizeof(image->requests));
    memcpy(&image->sem_a, &retry_sem_a, sizeof(image->sem_a));
    memcpy(&image->sem_b, &retry_sem_b, sizeof(image->sem_b));
}

static int retry_snapshot_matches(const struct retry_fixture_image *image)
{
    struct retry_fixture_image current;
    retry_snapshot_take(&current);
    return memcmp(&current, image, sizeof(current)) == 0;
}

static int retry_cancel_ready(struct Task *task)
{
    enum p4_public_spinlock_result lock_result;
    int result;
    Disable();
    lock_result = krnP4PublicSpinTryAcquire(&p4_task_pin_gate,
                                            SPINLOCK_MODE_WRITE);
    if (lock_result != P4_PUBLIC_SPIN_OK)
    {
        ++failures;
        Enable();
        return -99;
    }
    result = p4_task_registry_wait_cancel_ready_locked(&p4_task_records, task);
    CHECK(krnP4PublicSpinUnlock(&p4_task_pin_gate) == P4_PUBLIC_SPIN_OK);
    Enable();
    return result;
}

static void test_no_interest_drain_is_quiet(void *page)
{
    unsigned int poke_before;
    struct Task *unrelated = retry_identity(page, 64u);

    CHECK(retry_fixture_reset() == 1);
    retry_setup_worker(page);
    CHECK(krnP4TaskRecordPublishPinned(unrelated, &retry_records[1],
                                       &retry_sysbase) == 1);
    CHECK(retry_records[1].lifetime.notification_pins == 1u);
    CHECK(p4_semaphore_cancel_retry_event(&p4_cancel_retry) == 0);
    poke_before = mock_poke_calls;
    krnP4SignalTaskUnpin(unrelated, &retry_sysbase);
    CHECK(retry_records[1].lifetime.notification_pins == 0u);
    CHECK(mock_poke_calls == poke_before);
    CHECK(!p4_cleanup_wake_pending);
    CHECK(p4_semaphore_cancel_retry_event(&p4_cancel_retry) == 0);
}

static void test_interest_before_refusal_and_cross_target_wake(void *page)
{
    struct p4_task_wait_cancel_snapshot claim;
    struct retry_fixture_image before;
    struct SignalSemaphore *owner_semaphores[1] = { &retry_sem_b };
    struct SignalSemaphore *target_semaphores[2] = {
        &retry_sem_a, &retry_sem_b
    };
    const uint32_t owner_shared[1] = { 0u };
    const uint32_t target_shared[2] = { 0u, 0u };
    const int owner_result[1] = { 1 };
    const int target_result[2] = { 1, 2 };
    unsigned int poke_before;
    int result;

    CHECK(retry_fixture_reset() == 1);
    retry_setup_worker(page);
    retry_owner = retry_identity(page, 64u);
    retry_target = retry_identity(page, 128u);
    retry_unrelated = retry_identity(page, 192u);
    retry_publish_live(1u, retry_owner, 1u);
    retry_build_operation(retry_owner, &retry_owner_operation,
        owner_semaphores, owner_shared, owner_result, 1u);
    CHECK(krnP4SemaphoreOperationComplete(retry_owner,
              retry_owner_operation.generation, &retry_sysbase) == 1);
    krnP4TaskRecordLeaveRunning(retry_owner, 1u, &retry_sysbase);

    retry_publish_live(2u, retry_target, 1u);
    retry_build_operation(retry_target, &retry_target_operation,
        target_semaphores, target_shared, target_result, 2u);
    retry_retire_for_service(retry_target);
    CHECK(retry_records[2].wait_scope.cancel_requested == 1u);
    CHECK(retry_records[2].wait_scope.queued_count == 1u);
    CHECK(retry_records[2].lifetime.running_harts == 2u);
    retry_drain_wake();

    memset(&claim, 0, sizeof(claim));
    CHECK(krnP4SemaphoreCancelClaimAfter(retry_worker, 0u, &claim,
                                         &retry_sysbase) == 1);
    CHECK(claim.identity == retry_target);
    CHECK(retry_records[2].wait_scope.cancel_claimed == 1u);
    CHECK(retry_cancel_ready(retry_target) == 0);

    CHECK(krnP4TaskRecordPublishPinned(retry_unrelated,
              &retry_records[3], &retry_sysbase) == 1);
    CHECK(p4_semaphore_cancel_retry_event(&p4_cancel_retry) == 0);
    poke_before = mock_poke_calls;
    krnP4SemaphoreCancelRetryBegin(retry_worker, &retry_sysbase);
    CHECK(p4_semaphore_cancel_retry_event(&p4_cancel_retry) == 1);
    CHECK(p4_cancel_retry.active == 1u && p4_cancel_retry.waiting == 1u);
    CHECK(mock_poke_calls == poke_before);
    CHECK(!p4_cleanup_wake_pending);

    retry_snapshot_take(&before);
    test_semaphore_gate_lock();
    result = krnP4SemaphoreCancelRollbackDurable(retry_worker, retry_target,
        retry_target_operation.generation, 0u, &retry_sem_a, &retry_sysbase);
    CHECK(mock_outer_semaphore_gate_held && !mock_gate_held);
    test_semaphore_gate_unlock();
    CHECK(result == 0); /* Target still owns a live hart1 RUN continuation. */
    CHECK(retry_snapshot_matches(&before));
    CHECK(retry_records[2].wait_scope.cancel_claimed == 1u);
    CHECK(retry_records[2].lifetime.running_harts == 2u);
    CHECK(retry_cancel_ready(retry_target) == 0);
    CHECK(mock_poke_calls == poke_before);
    CHECK(!p4_cleanup_wake_pending);

    /* RUN leave is not a generic retry event. The claim and active scope keep
     * same-record cancellation/service readiness unavailable. */
    krnP4TaskRecordLeaveRunning(retry_target, 1u, &retry_sysbase);
    CHECK(retry_records[2].lifetime.running_harts == 0u);
    CHECK(retry_cancel_ready(retry_target) == 0);
    CHECK(mock_poke_calls == poke_before);
    CHECK(!p4_cleanup_wake_pending);

    /* The unrelated successful pin drain is the cross-Task resource event.
     * It must wake while target cancellation remains claimed and unready. */
    krnP4SignalTaskUnpin(retry_unrelated, &retry_sysbase);
    CHECK(retry_records[3].lifetime.notification_pins == 0u);
    CHECK(retry_records[2].wait_scope.cancel_claimed == 1u);
    CHECK(retry_cancel_ready(retry_target) == 0);
    CHECK(mock_poke_calls == poke_before + 1u);
    CHECK(p4_cleanup_wake_pending);
    CHECK(p4_semaphore_cancel_retry_event(&p4_cancel_retry) == 1);

    /* A producer event precedes End(false); ending the pass cannot erase the
     * independently reserved durable wake and does not self-poke. */
    krnP4SemaphoreCancelRetryEnd(retry_worker, 0u, &retry_sysbase);
    CHECK(p4_cancel_retry.active == 0u && p4_cancel_retry.waiting == 0u);
    CHECK(p4_semaphore_cancel_retry_event(&p4_cancel_retry) == 0);
    CHECK(p4_cleanup_wake_pending);
    CHECK(mock_poke_calls == poke_before + 1u);
    retry_drain_wake();
}

static void test_end_true_and_operation_complete(void *page)
{
    struct SignalSemaphore *semaphores[1] = { &retry_sem_a };
    const uint32_t shared[1] = { 0u };
    const int immediate[1] = { 1 };
    unsigned int poke_before;

    CHECK(retry_fixture_reset() == 1);
    retry_setup_worker(page);
    retry_target = retry_identity(page, 64u);
    retry_publish_live(1u, retry_target, 1u);
    retry_build_operation(retry_target, &retry_target_operation,
        semaphores, shared, immediate, 1u);

    poke_before = mock_poke_calls;
    krnP4SemaphoreCancelRetryBegin(retry_worker, &retry_sysbase);
    krnP4SemaphoreCancelRetryEnd(retry_worker, 1u, &retry_sysbase);
    CHECK(p4_cancel_retry.active == 0u && p4_cancel_retry.waiting == 1u);
    CHECK(p4_semaphore_cancel_retry_event(&p4_cancel_retry) == 1);
    CHECK(mock_poke_calls == poke_before && !p4_cleanup_wake_pending);

    CHECK(krnP4SemaphoreOperationComplete(retry_target,
              retry_target_operation.generation, &retry_sysbase) == 1);
    CHECK(retry_records[1].wait_scope.active == 0u);
    CHECK(p4_cleanup_wake_pending);
    CHECK(mock_poke_calls == poke_before + 1u);
    retry_drain_wake();

    /* A second active pass sees a stale completion. Failed finish changes no
     * wake state, even though retry interest remains registered. */
    poke_before = mock_poke_calls;
    krnP4SemaphoreCancelRetryBegin(retry_worker, &retry_sysbase);
    CHECK(krnP4SemaphoreOperationComplete(retry_target,
              retry_target_operation.generation, &retry_sysbase) == 0);
    CHECK(mock_poke_calls == poke_before && !p4_cleanup_wake_pending);
    krnP4SemaphoreCancelRetryEnd(retry_worker, 0u, &retry_sysbase);
    CHECK(mock_poke_calls == poke_before && !p4_cleanup_wake_pending);
    CHECK(p4_semaphore_cancel_retry_event(&p4_cancel_retry) == 0);
    krnP4TaskRecordLeaveRunning(retry_target, 1u, &retry_sysbase);
    CHECK(mock_poke_calls == poke_before && !p4_cleanup_wake_pending);
}

static void test_operation_finish_cancel_event(void *page)
{
    struct SignalSemaphore *semaphores[1] = { &retry_sem_a };
    const uint32_t shared[1] = { 0u };
    struct p4_task_wait_cancel_snapshot claim;
    unsigned int poke_before;

    CHECK(retry_fixture_reset() == 1);
    retry_setup_worker(page);
    retry_target = retry_identity(page, 64u);
    retry_publish_live(1u, retry_target, 1u);
    CHECK(krnP4SemaphoreOperationReserve(retry_target,
              1u, &retry_target_operation, &retry_sysbase) == 1);
    test_configure_operation(retry_target, &retry_target_operation,
        semaphores, shared, 1u, &retry_sysbase);
    retry_retire_for_service(retry_target);
    krnP4TaskRecordLeaveRunning(retry_target, 1u, &retry_sysbase);
    retry_drain_wake();

    memset(&claim, 0xa5, sizeof(claim));
    CHECK(krnP4SemaphoreCancelClaimAfter(retry_worker, 0u, &claim,
                                         &retry_sysbase) == 1);
    CHECK(claim.identity == retry_target);
    CHECK(retry_records[1].lifetime.running_harts == 0u);
    CHECK(retry_records[1].service_pending == 1u);
    CHECK(retry_records[1].wait_scope.cancel_claimed == 1u);

    poke_before = mock_poke_calls;
    krnP4SemaphoreCancelRetryBegin(retry_worker, &retry_sysbase);
    CHECK(krnP4SemaphoreOperationFinishCancel(retry_target,
              retry_target_operation.generation, &retry_sysbase) == 1);
    CHECK(retry_records[1].wait_scope.active == 0u);
    CHECK(retry_records[1].service_queued == 1u);
    CHECK(retry_records[1].lifetime.notification_pins == 1u);
    CHECK(p4_cleanup_wake_pending);
    CHECK(mock_poke_calls == poke_before + 1u);
    CHECK(p4_semaphore_cancel_retry_event(&p4_cancel_retry) == 1);
    retry_drain_wake();

    poke_before = mock_poke_calls;
    CHECK(krnP4SemaphoreOperationFinishCancel(retry_target,
              retry_target_operation.generation, &retry_sysbase) == 0);
    CHECK(mock_poke_calls == poke_before && !p4_cleanup_wake_pending);
    krnP4SemaphoreCancelRetryEnd(retry_worker, 0u, &retry_sysbase);
    CHECK(mock_poke_calls == poke_before && !p4_cleanup_wake_pending);
}

static void test_legacy_wait_finish_event(void *page)
{
    struct p4_task_wait_cancel_snapshot claim;
    uint32_t generation = 0;
    unsigned int poke_before;

    CHECK(retry_fixture_reset() == 1);
    retry_setup_worker(page);
    retry_target = retry_identity(page, 64u);
    retry_publish_live(1u, retry_target, 1u);
    CHECK(krnP4SemaphoreWaitBegin(retry_target, P4_TASK_WAIT_SINGLE,
              &retry_sem_a, &test_boot_requests[7], NULL,
              &generation, &retry_sysbase) == 1);
    CHECK(krnP4SemaphoreWaitClose(retry_target, generation,
                                  &retry_sysbase) == 1);
    krnP4TaskRecordLeaveRunning(retry_target, 1u, &retry_sysbase);
    retry_retire_for_service(retry_target);
    retry_drain_wake();

    memset(&claim, 0xa5, sizeof(claim));
    CHECK(krnP4SemaphoreWaitClaimCancel(&claim, &retry_sysbase) == 1);
    CHECK(claim.identity == retry_target);
    CHECK(claim.kind == P4_TASK_WAIT_SINGLE);
    CHECK(retry_records[1].service_pending == 1u);

    poke_before = mock_poke_calls;
    krnP4SemaphoreCancelRetryBegin(retry_worker, &retry_sysbase);
    CHECK(krnP4SemaphoreWaitFinishCancel(retry_target, generation,
                                         &retry_sysbase) == 1);
    CHECK(retry_records[1].wait_scope.active == 0u);
    CHECK(retry_records[1].service_queued == 1u);
    CHECK(retry_records[1].lifetime.notification_pins == 1u);
    CHECK(p4_cleanup_wake_pending);
    CHECK(mock_poke_calls == poke_before + 1u);
    CHECK(p4_semaphore_cancel_retry_event(&p4_cancel_retry) == 1);
    retry_drain_wake();

    poke_before = mock_poke_calls;
    CHECK(krnP4SemaphoreWaitFinishCancel(retry_target, generation,
                                         &retry_sysbase) == 0);
    CHECK(mock_poke_calls == poke_before && !p4_cleanup_wake_pending);
    krnP4SemaphoreCancelRetryEnd(retry_worker, 0u, &retry_sysbase);
    CHECK(mock_poke_calls == poke_before && !p4_cleanup_wake_pending);
}

static void test_notification_finish_combines_events(void *page)
{
    struct p4_semaphore_notification notification;
    struct SignalSemaphore *owner_semaphores[1] = { &retry_sem_a };
    struct SignalSemaphore *waiter_semaphores[1] = { &retry_sem_a };
    const uint32_t shared[1] = { 0u };
    const int owner_result[1] = { 1 };
    const int queued_result[1] = { 2 };
    unsigned int poke_before;

    CHECK(retry_fixture_reset() == 1);
    retry_setup_worker(page);
    retry_owner = retry_identity(page, 64u);
    retry_waiter = retry_identity(page, 128u);
    retry_publish_live(1u, retry_owner, 1u);
    retry_build_operation(retry_owner, &retry_owner_operation,
        owner_semaphores, shared, owner_result, 1u);
    CHECK(krnP4SemaphoreOperationComplete(retry_owner,
              retry_owner_operation.generation, &retry_sysbase) == 1);
    krnP4TaskRecordLeaveRunning(retry_owner, 1u, &retry_sysbase);

    retry_publish_live(2u, retry_waiter, 1u);
    retry_build_operation(retry_waiter, &retry_waiter_operation,
        waiter_semaphores, shared, queued_result, 1u);
    CHECK(retry_sem_a.ss_QueueCount == 1);
    CHECK(retry_records[2].wait_scope.queued_count == 1u);

    /* Real release grants the waiter and reserves its durable notification. */
    CHECK(test_durable_release(&retry_sem_a, &retry_sysbase) == 2);
    CHECK(retry_sem_a.ss_Owner == retry_waiter);
    CHECK(retry_records[2].semaphore_pending == 1u);
    CHECK(p4_cleanup_wake_pending);
    krnP4CleanupPoke(&retry_sysbase); /* Caller poke follows semaphore unlock. */
    retry_drain_wake();

    memset(&notification, 0, sizeof(notification));
    CHECK(krnP4SemaphoreNotificationClaim(retry_worker, &notification,
                                          &retry_sysbase) == 1);
    CHECK(notification.identity == retry_waiter);
    CHECK(notification.count == 1u && notification.generation != 0u);
    CHECK(retry_records[2].semaphore_pending == 0u);
    CHECK(retry_records[2].semaphore_inflight == 1u);

    /* Scope completion and release drop the request/ledger and semaphore
     * ownership while the independent in-flight notification pin remains. */
    poke_before = mock_poke_calls;
    CHECK(krnP4SemaphoreOperationComplete(retry_waiter,
              retry_waiter_operation.generation, &retry_sysbase) == 1);
    CHECK(retry_records[2].wait_scope.active == 0u);
    CHECK(retry_records[2].lifetime.notification_pins == 1u);
    CHECK(mock_poke_calls == poke_before && !p4_cleanup_wake_pending);
    CHECK(test_durable_release(&retry_sem_a, &retry_sysbase) == 1);
    CHECK(retry_sem_a.ss_NestCount == 0);
    CHECK(retry_sem_a.ss_Owner == NULL);

    retry_retire_for_service(retry_waiter);
    CHECK(retry_records[2].service_pending == 1u);
    CHECK(retry_records[2].lifetime.notification_pins == 2u);
    krnP4TaskRecordLeaveRunning(retry_waiter, 1u, &retry_sysbase);
    CHECK(retry_records[2].lifetime.running_harts == 0u);
    CHECK(!p4_cleanup_wake_pending);
    CHECK(mock_poke_calls == poke_before);
    CHECK(mock_retired_poke_calls != 0u); /* Separate retired-owner IPI. */

    krnP4SemaphoreCancelRetryBegin(retry_worker, &retry_sysbase);
    CHECK(p4_semaphore_cancel_retry_event(&p4_cancel_retry) == 1);
    poke_before = mock_poke_calls;
    krnP4SemaphoreNotificationFinish(retry_worker, &notification,
                                      &retry_sysbase);
    CHECK(retry_records[2].semaphore_inflight == 0u);
    CHECK(retry_records[2].lifetime.notification_pins == 1u);
    CHECK(retry_records[2].service_queued == 1u);
    CHECK(p4_cleanup_wake_pending);
    CHECK(p4_semaphore_cancel_retry_event(&p4_cancel_retry) == 1);
    CHECK(mock_poke_calls == poke_before + 1u);
    retry_drain_wake();
    poke_before = mock_poke_calls;
    krnP4SemaphoreCancelRetryEnd(retry_worker, 0u, &retry_sysbase);
    CHECK(mock_poke_calls == poke_before && !p4_cleanup_wake_pending);
}

enum retry_fatal_case
{
    RETRY_BEGIN_UNARMED,
    RETRY_BEGIN_NONCANONICAL,
    RETRY_BEGIN_STOPPED,
    RETRY_BEGIN_NESTED,
    RETRY_END_UNARMED,
    RETRY_END_NONCANONICAL,
    RETRY_END_STOPPED,
    RETRY_END_INACTIVE,
    RETRY_END_BAD_BLOCKED
};

static void retry_fatal_child(enum retry_fatal_case which, void *page)
{
    struct Task *unknown = retry_identity(page, 1024u);
    pid_t child;
    int status = 0;
    struct rlimit no_core = { 0u, 0u };

    fflush(NULL);
    child = fork();
    CHECK(child >= 0);
    if (child == 0)
    {
        (void)setrlimit(RLIMIT_CORE, &no_core);
        (void)close(STDERR_FILENO);
        switch (which)
        {
            case RETRY_BEGIN_UNARMED:
                p4_cleanup_wake_mask = 0u;
                krnP4SemaphoreCancelRetryBegin(retry_worker, &retry_sysbase);
                break;
            case RETRY_BEGIN_NONCANONICAL:
                krnP4SemaphoreCancelRetryBegin(unknown, &retry_sysbase);
                break;
            case RETRY_BEGIN_STOPPED:
                krnP4TaskRecordLeaveRunning(retry_worker, 0u, &retry_sysbase);
                krnP4SemaphoreCancelRetryBegin(retry_worker, &retry_sysbase);
                break;
            case RETRY_BEGIN_NESTED:
                krnP4SemaphoreCancelRetryBegin(retry_worker, &retry_sysbase);
                krnP4SemaphoreCancelRetryBegin(retry_worker, &retry_sysbase);
                break;
            case RETRY_END_UNARMED:
                p4_cleanup_wake_mask = 0u;
                krnP4SemaphoreCancelRetryEnd(retry_worker, 0u,
                                              &retry_sysbase);
                break;
            case RETRY_END_NONCANONICAL:
                krnP4SemaphoreCancelRetryEnd(unknown, 0u, &retry_sysbase);
                break;
            case RETRY_END_STOPPED:
                krnP4TaskRecordLeaveRunning(retry_worker, 0u, &retry_sysbase);
                krnP4SemaphoreCancelRetryEnd(retry_worker, 0u,
                                              &retry_sysbase);
                break;
            case RETRY_END_INACTIVE:
                krnP4SemaphoreCancelRetryEnd(retry_worker, 0u,
                                              &retry_sysbase);
                break;
            case RETRY_END_BAD_BLOCKED:
                krnP4SemaphoreCancelRetryBegin(retry_worker, &retry_sysbase);
                krnP4SemaphoreCancelRetryEnd(retry_worker, 2u,
                                              &retry_sysbase);
                break;
            default:
                _exit(3);
        }
        _exit(0);
    }
    if (child > 0)
    {
        CHECK(waitpid(child, &status, 0) == child);
        CHECK(WIFSIGNALED(status));
        if (WIFSIGNALED(status))
            CHECK(WTERMSIG(status) == SIGABRT);
    }
}

static void test_retry_binding_fail_closed(void *page)
{
    enum retry_fatal_case which;
    CHECK(retry_fixture_reset() == 1);
    retry_setup_worker(page);
    for (which = RETRY_BEGIN_UNARMED; which <= RETRY_END_BAD_BLOCKED;
         which = (enum retry_fatal_case)((int)which + 1))
        retry_fatal_child(which, page);
    CHECK(mock_irq_depth == 0u && !mock_gate_held);
    CHECK(mock_disable_calls == mock_enable_calls);
    CHECK(mock_acquire_calls == mock_unlock_calls);
    CHECK(mock_fatal_calls == 0u);
}

int main(void)
{
    long page_size;
    void *page;
    unsigned int baseline_checks;
    int baseline_result;

    baseline_result = p4_semaphore_operation_admission_baseline_main();
    baseline_checks = checks;
    if (baseline_result != 0 || baseline_checks != 379u)
    {
        fprintf(stderr,
            "included O-A baseline failed: result=%d checks=%u\n",
            baseline_result, baseline_checks);
        return 2;
    }
    if (fflush(stdout) != 0)
        return 2;

    if (retry_fixture_reset() != 1)
    {
        fprintf(stderr, "could not reset quiescent O-A fixture\n");
        return 2;
    }
    page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0)
        return 2;
#if defined(MAP_ANONYMOUS)
    page = mmap(NULL, (size_t)page_size, PROT_NONE,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
#else
    page = mmap(NULL, (size_t)page_size, PROT_NONE,
                MAP_PRIVATE | MAP_ANON, -1, 0);
#endif
    if (page == MAP_FAILED)
        return 2;

    test_no_interest_drain_is_quiet(page);
    test_interest_before_refusal_and_cross_target_wake(page);
    test_end_true_and_operation_complete(page);
    test_operation_finish_cancel_event(page);
    test_legacy_wait_finish_event(page);
    test_notification_finish_combines_events(page);
    test_retry_binding_fail_closed(page);

    if (munmap(page, (size_t)page_size) != 0)
        return 2;
    if (failures != 0u)
    {
        fprintf(stderr, "P4 semaphore cancel retry binding: %u/%u failed\n",
                failures, checks - baseline_checks);
        return 1;
    }
    if (mock_irq_depth != 0u || mock_gate_held || mock_outer_semaphore_gate_held ||
        mock_disable_calls != mock_enable_calls ||
        mock_acquire_calls != mock_unlock_calls || mock_fatal_calls != 0u)
    {
        fprintf(stderr, "P4 semaphore cancel retry binding: fixture not quiescent\n");
        return 1;
    }
    printf("P4 semaphore cancel retry binding: %u checks passed; "
           "included O-A baseline %u\n", checks - baseline_checks,
           baseline_checks);
    return 0;
}
