/* Actual-source integration test for the protected semaphore-cancel consumer. */
#define _DEFAULT_SOURCE 1
#define __AROSEXEC_SMP__ 1
#define __AROSPLATFORM_SMP__ 1
#define P4_TASK_PINS_TEST 1
#define P4_PUBLIC_SPINLOCK_TEST 1
#define P4_SEMAPHORE_NODES_TEST 1

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

int p4_semaphore_operation_admission_baseline_main(void);
#define main p4_semaphore_operation_admission_baseline_main
#include "p4-semaphore-operation-admission-test.c"
#undef main

#include "../p4_semaphore_cancel.h"
#include "../p4_semaphore_cancel_retry.h"
#include "../p4_semaphore_boot.h"

void Exec_P4SemaphoreLock(struct SignalSemaphore *, struct ExecBase *);
void Exec_P4SemaphoreUnlock(struct SignalSemaphore *, struct ExecBase *);
void p4_signal_adapter_fail_closed(void) __attribute__((noreturn));

#define P4_SEMAPHORE_CANCEL_WORKER_TEST 1
#include "../../exec/p4_semaphore_cancel_worker.c"

#define CONSUMER_RECORD_CAPACITY 5u

static struct p4_task_registry_record consumer_records[CONSUMER_RECORD_CAPACITY];
static struct p4_semaphore_ledger consumer_ledger;
static struct p4_semaphore_ledger_operation consumer_operations[TEST_OP_CAPACITY];
static struct p4_semaphore_ledger_entry consumer_entries[TEST_ENTRY_CAPACITY];
static struct SignalSemaphore consumer_sem_a;
static struct SignalSemaphore consumer_sem_b;
static struct ExecBase consumer_sysbase;
static struct Task *consumer_worker;
static struct Task *consumer_owner;
static struct Task *consumer_queued_peer;
static struct Task *consumer_live_waiter;
static struct Task *consumer_other_owner;
static struct p4_task_wait_operation owner_operation;
static struct p4_task_wait_operation peer_operation;
static struct p4_task_wait_operation waiter_operation;
static struct p4_task_wait_operation other_owner_operation;
static unsigned int worker_sem_lock_calls;
static unsigned int worker_sem_unlock_calls;

struct consumer_fixture_image
{
    struct p4_task_registry registry;
    struct p4_task_registry_record records[CONSUMER_RECORD_CAPACITY];
    struct p4_semaphore_ledger ledger;
    struct p4_semaphore_ledger_operation operations[TEST_OP_CAPACITY];
    struct p4_semaphore_ledger_entry entries[TEST_ENTRY_CAPACITY];
    struct SemaphoreRequest requests[8];
    struct SignalSemaphore sem_a;
    struct SignalSemaphore sem_b;
    struct p4_semaphore_cancel_retry retry;
    struct Task *cleanup_worker;
    uint32_t cleanup_wake_mask;
    int cleanup_wake_pending;
};

static void consumer_snapshot_take(struct consumer_fixture_image *image)
{
    memset(image, 0, sizeof(*image));
    memcpy(&image->registry, &p4_task_records, sizeof(image->registry));
    memcpy(image->records, consumer_records, sizeof(image->records));
    memcpy(&image->ledger, &consumer_ledger, sizeof(image->ledger));
    memcpy(image->operations, consumer_operations, sizeof(image->operations));
    memcpy(image->entries, consumer_entries, sizeof(image->entries));
    memcpy(image->requests, test_boot_requests, sizeof(image->requests));
    memcpy(&image->sem_a, &consumer_sem_a, sizeof(image->sem_a));
    memcpy(&image->sem_b, &consumer_sem_b, sizeof(image->sem_b));
    memcpy(&image->retry, &p4_cancel_retry, sizeof(image->retry));
    image->cleanup_worker = p4_cleanup_worker;
    image->cleanup_wake_mask = p4_cleanup_wake_mask;
    image->cleanup_wake_pending = p4_cleanup_wake_pending;
}

static int consumer_snapshot_matches(
    const struct consumer_fixture_image *image)
{
    struct consumer_fixture_image current;
    consumer_snapshot_take(&current);
    return memcmp(&current, image, sizeof(current)) == 0;
}

static int consumer_fixture_init(void)
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
    mock_irq_depth = 0u;
    mock_gate_held = 0;
    mock_outer_semaphore_gate_held = 0;
    p4_cleanup_worker = NULL;
    p4_cleanup_wake_mask = 0u;
    p4_cleanup_wake_pending = false;
    p4_cancel_retry.active = 0u;
    p4_cancel_retry.waiting = 0u;
    worker_sem_lock_calls = 0u;
    worker_sem_unlock_calls = 0u;

    memset(consumer_records, 0, sizeof(consumer_records));
    memset(&consumer_ledger, 0, sizeof(consumer_ledger));
    memset(consumer_operations, 0, sizeof(consumer_operations));
    memset(consumer_entries, 0, sizeof(consumer_entries));
    memset(test_boot_requests, 0, sizeof(test_boot_requests));
    memset(&consumer_sem_a, 0, sizeof(consumer_sem_a));
    memset(&consumer_sem_b, 0, sizeof(consumer_sem_b));
    memset(&consumer_sysbase, 0, sizeof(consumer_sysbase));
    memset(&owner_operation, 0, sizeof(owner_operation));
    memset(&peer_operation, 0, sizeof(peer_operation));
    memset(&waiter_operation, 0, sizeof(waiter_operation));
    memset(&other_owner_operation, 0, sizeof(other_owner_operation));
    consumer_worker = NULL;
    consumer_owner = NULL;
    consumer_queued_peer = NULL;
    consumer_live_waiter = NULL;
    consumer_other_owner = NULL;

    test_boot_ledger = &consumer_ledger;
    if (p4_semaphore_ledger_init(&consumer_ledger, consumer_operations,
            TEST_OP_CAPACITY, consumer_entries, TEST_ENTRY_CAPACITY) != 1)
        return 0;
    test_empty_semaphore(&consumer_sem_a);
    test_empty_semaphore(&consumer_sem_b);
    return 1;
}

static void consumer_publish(unsigned int slot, struct Task *identity)
{
    CHECK(slot < CONSUMER_RECORD_CAPACITY);
    CHECK(krnP4TaskRecordPublish(identity, &consumer_records[slot],
                                  &consumer_sysbase) == 1);
}

void Exec_P4SemaphoreLock(struct SignalSemaphore *sem,
    struct ExecBase *SysBase)
{
    CHECK(sem != NULL && SysBase == &consumer_sysbase);
    CHECK(!mock_outer_semaphore_gate_held && !mock_gate_held);
    ++worker_sem_lock_calls;
    test_semaphore_gate_lock();
}

void Exec_P4SemaphoreUnlock(struct SignalSemaphore *sem,
    struct ExecBase *SysBase)
{
    CHECK(sem != NULL && SysBase == &consumer_sysbase);
    CHECK(mock_outer_semaphore_gate_held && !mock_gate_held);
    ++worker_sem_unlock_calls;
    test_semaphore_gate_unlock();
}

void p4_signal_adapter_fail_closed(void)
{
    fprintf(stderr, "actual cancellation worker failed closed\n");
    abort();
}

static void consumer_prepare_operation(struct Task *task,
    struct p4_task_wait_operation *operation,
    struct SignalSemaphore *const semaphores[], const uint32_t shared[],
    const int expected[], uint32_t count, unsigned int hart,
    int leave_running, int verify_stale)
{
    uint32_t i;

    CHECK(krnP4TaskRecordEnterRunning(task, hart, &consumer_sysbase) == 1);
    CHECK(krnP4SemaphoreOperationReserve(task, count, operation,
                                         &consumer_sysbase) == 1);
    for (i = 0; i < count; ++i)
    {
        int result;
        test_semaphore_gate_lock();
        result = krnP4SemaphoreOperationConfigure(task,
            operation->generation, i, semaphores[i], shared[i],
            &consumer_sysbase);
        CHECK(mock_outer_semaphore_gate_held && !mock_gate_held);
        test_semaphore_gate_unlock();
        CHECK(result == 1);
    }
    CHECK(krnP4SemaphoreOperationClose(task, operation->generation,
                                       &consumer_sysbase) == 1);

    if (verify_stale)
    {
        struct consumer_fixture_image before;
        int result;
        consumer_snapshot_take(&before);
        test_semaphore_gate_lock();
        result = krnP4SemaphoreOperationConfigure(task,
            operation->generation, 0u, semaphores[0], shared[0],
            &consumer_sysbase);
        test_semaphore_gate_unlock();
        CHECK(result == 0);
        CHECK(consumer_snapshot_matches(&before));
    }

    for (i = 0; i < count; ++i)
    {
        int result;
        test_semaphore_gate_lock();
        result = krnP4SemaphoreOperationAcquire(task,
            operation->generation, i, semaphores[i], &consumer_sysbase);
        test_semaphore_gate_unlock();
        CHECK(result == expected[i]);
        if (verify_stale && i == 0u)
        {
            struct consumer_fixture_image before;
            consumer_snapshot_take(&before);
            test_semaphore_gate_lock();
            result = krnP4SemaphoreOperationAcquire(task,
                operation->generation, i, semaphores[i], &consumer_sysbase);
            test_semaphore_gate_unlock();
            CHECK(result == 0);
            CHECK(consumer_snapshot_matches(&before));
        }
    }
    if (leave_running)
        krnP4TaskRecordLeaveRunning(task, hart, &consumer_sysbase);
}

static void consumer_setup_worker(void *page)
{
    consumer_worker = guarded_identity(page, 64u);
    consumer_publish(0u, consumer_worker);
    CHECK(krnP4TaskRecordEnterRunning(consumer_worker, 0u,
                                      &consumer_sysbase) == 1);
    CHECK(krnP4ProtectCleanupWorker(consumer_worker,
                                   &consumer_sysbase) == 1);
    CHECK(krnP4ArmCleanupWake(consumer_worker, UINT32_C(1) << 5,
                              &consumer_sysbase) == 1);
}

static void consumer_drain_wake(void)
{
    struct p4_cleanup_wake wake;
    memset(&wake, 0xa5, sizeof(wake));
    CHECK(krnP4TakeCleanupWake(&wake, &consumer_sysbase) == 1);
    CHECK(wake.worker == consumer_worker);
    CHECK(wake.mask == (UINT32_C(1) << 5));
    CHECK(!p4_cleanup_wake_pending);
}

static void consumer_retire_for_service(struct Task *identity)
{
    mock_task_lock_held = 1;
    CHECK(krnP4TaskRecordRetireForService(identity, &consumer_sysbase) == 1);
    mock_task_lock_held = 0;
}

static void consumer_seed_high_pin_count(struct Task *identity)
{
    enum p4_public_spinlock_result lock_result;
    struct p4_task_registry_record *record;
    Disable();
    lock_result = krnP4PublicSpinTryAcquire(&p4_task_pin_gate,
                                            SPINLOCK_MODE_WRITE);
    CHECK(lock_result == P4_PUBLIC_SPIN_OK);
    record = p4_task_registry_lookup_locked(&p4_task_records, identity);
    CHECK(record == &consumer_records[3]);
    if (record != NULL)
        record->lifetime.notification_pins = UINT32_MAX;
    CHECK(krnP4PublicSpinUnlock(&p4_task_pin_gate) == P4_PUBLIC_SPIN_OK);
    Enable();
}

static void test_actual_consumer_passes(void *page)
{
    struct SignalSemaphore *other_owner_semaphores[1] = { &consumer_sem_b };
    struct SignalSemaphore *owner_semaphores[2] = {
        &consumer_sem_a, &consumer_sem_b
    };
    struct SignalSemaphore *peer_semaphores[1] = { &consumer_sem_a };
    struct SignalSemaphore *waiter_semaphores[1] = { &consumer_sem_a };
    const uint32_t shared_one[1] = { 0u };
    const uint32_t owner_shared[2] = { 0u, 0u };
    const int immediate[1] = { 1 };
    const int owner_results[2] = { 1, 2 };
    const int queued[1] = { 2 };
    struct consumer_fixture_image before;
    struct p4_semaphore_notification notification;
    unsigned int poke_before;
    unsigned int retired_poke_before;
    int result;

    CHECK(consumer_fixture_init() == 1);
    consumer_setup_worker(page);

    /* A protected empty pass must begin/end retry interest without a poke. */
    poke_before = mock_poke_calls;
    CHECK(Exec_P4SemaphoreCancellationPass(consumer_worker,
              &consumer_sysbase) == 0);
    CHECK(p4_cancel_retry.active == 0u && p4_cancel_retry.waiting == 0u);
    CHECK(mock_poke_calls == poke_before && !p4_cleanup_wake_pending);

    consumer_owner = guarded_identity(page, 128u);
    consumer_queued_peer = guarded_identity(page, 192u);
    consumer_live_waiter = guarded_identity(page, 256u);
    consumer_other_owner = guarded_identity(page, 320u);
    consumer_publish(1u, consumer_owner);
    consumer_publish(2u, consumer_queued_peer);
    consumer_publish(3u, consumer_live_waiter);
    consumer_publish(4u, consumer_other_owner);

    /* D owns semB. A then owns semA and queues a second acquisition on semB;
     * it will remain RUN-held during pass 1. */
    consumer_prepare_operation(consumer_other_owner,
        &other_owner_operation, other_owner_semaphores, shared_one,
        immediate, 1u, 1u, 1, 1);
    consumer_prepare_operation(consumer_owner, &owner_operation,
        owner_semaphores, owner_shared, owner_results, 2u, 1u, 1, 0);
    consumer_prepare_operation(consumer_queued_peer, &peer_operation,
        peer_semaphores, shared_one, queued, 1u, 1u, 1, 0);
    consumer_prepare_operation(consumer_live_waiter, &waiter_operation,
        waiter_semaphores, shared_one, queued, 1u, 1u, 1, 0);

    CHECK(consumer_sem_a.ss_Owner == consumer_owner);
    CHECK(consumer_sem_a.ss_NestCount == 1 && consumer_sem_a.ss_QueueCount == 2);
    CHECK(consumer_sem_a.ss_WaitQueue.mlh_Head ==
          (struct MinNode *)(void *)peer_operation.operation->first->request);
    CHECK(consumer_sem_b.ss_Owner == consumer_other_owner);
    CHECK(consumer_sem_b.ss_NestCount == 1 && consumer_sem_b.ss_QueueCount == 1);

    /* Re-enter A's continuation to model cancellation racing its live RUN
     * owner after the multi-entry scope has published both requests. */
    CHECK(krnP4TaskRecordEnterRunning(consumer_owner, 1u,
                                      &consumer_sysbase) == 1);
    poke_before = mock_poke_calls;
    retired_poke_before = mock_retired_poke_calls;
    consumer_retire_for_service(consumer_owner);
    CHECK(mock_retired_poke_calls == retired_poke_before + 1u);
    consumer_retire_for_service(consumer_queued_peer);
    CHECK(mock_retired_poke_calls == retired_poke_before + 1u);
    CHECK(mock_poke_calls == poke_before + 2u);
    CHECK(consumer_records[1].wait_scope.cancel_requested == 1u);
    CHECK(consumer_records[1].lifetime.running_harts == 2u);
    CHECK(consumer_records[2].wait_scope.cancel_requested == 1u);
    CHECK(consumer_records[2].wait_scope.queued_count == 1u);
    CHECK(p4_cleanup_wake_pending);
    consumer_drain_wake();

    /* The actual consumer skips A's RUN-held OWNED unit, withdraws A's queued
     * peer request on semB, then withdraws B from semA and finishes B. */
    poke_before = mock_poke_calls;
    result = Exec_P4SemaphoreCancellationPass(consumer_worker,
                                               &consumer_sysbase);
    CHECK(result == 1);
    CHECK(p4_cancel_retry.active == 0u && p4_cancel_retry.waiting == 1u);
    CHECK(consumer_records[1].wait_scope.active == 1u);
    CHECK(consumer_records[1].wait_scope.cancel_claimed == 0u);
    CHECK(consumer_records[1].wait_scope.queued_count == 0u);
    CHECK(consumer_records[1].lifetime.running_harts == 2u);
    CHECK(owner_operation.operation->cancelled == 1u);
    CHECK(consumer_entries[2].state == P4_SEMAPHORE_LEDGER_ENTRY_WITHDRAWN);
    CHECK(consumer_records[2].wait_scope.active == 0u);
    CHECK(consumer_records[2].service_queued == 1u);
    CHECK(consumer_records[2].lifetime.notification_pins == 1u);
    CHECK(consumer_entries[3].state == P4_SEMAPHORE_LEDGER_ENTRY_FREE);
    CHECK(consumer_sem_a.ss_Owner == consumer_owner);
    CHECK(consumer_sem_a.ss_NestCount == 1 && consumer_sem_a.ss_QueueCount == 1);
    CHECK(consumer_sem_a.ss_WaitQueue.mlh_Head ==
          (struct MinNode *)(void *)waiter_operation.operation->first->request);
    CHECK(consumer_sem_b.ss_Owner == consumer_other_owner);
    CHECK(consumer_sem_b.ss_NestCount == 1 && consumer_sem_b.ss_QueueCount == 0);
    CHECK(mock_poke_calls == poke_before + 3u);
    CHECK(p4_cleanup_wake_pending);
    consumer_drain_wake();

    /* RUN drain makes A claimable, but the first rollback pass cannot reserve
     * C's final semaphore-notification pin. It must leave owner, queue, ledger
     * and all records unchanged, release the claim, and remain retryable. */
    poke_before = mock_poke_calls;
    krnP4TaskRecordLeaveRunning(consumer_owner, 1u, &consumer_sysbase);
    CHECK(consumer_records[1].lifetime.running_harts == 0u);
    CHECK(p4_cleanup_wake_pending);
    CHECK(mock_poke_calls == poke_before + 1u);
    consumer_drain_wake();
    CHECK(krnP4SignalTaskPin(consumer_live_waiter,
                             &consumer_sysbase) == 1);
    consumer_seed_high_pin_count(consumer_live_waiter);
    CHECK(consumer_records[3].lifetime.notification_pins == UINT32_MAX);
    consumer_snapshot_take(&before);
    poke_before = mock_poke_calls;
    result = Exec_P4SemaphoreCancellationPass(consumer_worker,
                                               &consumer_sysbase);
    CHECK(result == 0);
    CHECK(p4_cancel_retry.active == 0u && p4_cancel_retry.waiting == 1u);
    CHECK(consumer_snapshot_matches(&before));
    CHECK(mock_poke_calls == poke_before && !p4_cleanup_wake_pending);
    CHECK(consumer_sem_a.ss_Owner == consumer_owner);
    CHECK(consumer_sem_a.ss_NestCount == 1 && consumer_sem_a.ss_QueueCount == 1);
    CHECK(consumer_entries[1].state == P4_SEMAPHORE_LEDGER_ENTRY_OWNED);
    CHECK(consumer_records[1].wait_scope.cancel_claimed == 0u);

    /* One real ordinary pin drop is a cross-Task capacity event. Retry
     * interest retained by End(true) must wake the next actual pass. */
    poke_before = mock_poke_calls;
    krnP4SignalTaskUnpin(consumer_live_waiter, &consumer_sysbase);
    CHECK(consumer_records[3].lifetime.notification_pins == UINT32_MAX - 1u);
    CHECK(p4_cleanup_wake_pending);
    CHECK(mock_poke_calls == poke_before + 1u);
    consumer_drain_wake();

    /* The next actual pass atomically releases A and grants live waiter C;
     * cancellation finish queues A cleanup, and the cohort reserves exactly
     * one durable semaphore notification pin for C. */
    poke_before = mock_poke_calls;
    result = Exec_P4SemaphoreCancellationPass(consumer_worker,
                                               &consumer_sysbase);
    CHECK(result == 1);
    CHECK(p4_cancel_retry.active == 0u && p4_cancel_retry.waiting == 0u);
    CHECK(consumer_records[1].wait_scope.active == 0u);
    CHECK(consumer_records[1].service_queued == 1u);
    CHECK(consumer_records[1].lifetime.notification_pins == 1u);
    CHECK(consumer_sem_a.ss_Owner == consumer_live_waiter);
    CHECK(consumer_sem_a.ss_NestCount == 1 && consumer_sem_a.ss_QueueCount == 0);
    CHECK(consumer_sem_a.ss_WaitQueue.mlh_Head ==
          (struct MinNode *)(void *)&consumer_sem_a.ss_WaitQueue.mlh_Tail);
    CHECK(consumer_records[3].wait_scope.active == 1u);
    CHECK(consumer_records[3].wait_scope.queued_count == 0u);
    CHECK(consumer_records[3].semaphore_pending == 1u);
    CHECK(consumer_records[3].semaphore_inflight == 0u);
    CHECK(consumer_records[3].lifetime.notification_pins == UINT32_MAX);
    CHECK(consumer_records[3].semaphore_pending_generation != 0u);
    CHECK(mock_poke_calls == poke_before + 2u);
    CHECK(p4_cleanup_wake_pending);
    consumer_drain_wake();

    CHECK(krnP4SemaphoreOperationComplete(consumer_live_waiter,
              waiter_operation.generation, &consumer_sysbase) == 1);
    CHECK(consumer_records[3].lifetime.notification_pins == UINT32_MAX - 1u);
    CHECK(test_durable_release(&consumer_sem_a, &consumer_sysbase) == 1);
    CHECK(consumer_sem_a.ss_Owner == NULL && consumer_sem_a.ss_NestCount == 0);
    memset(&notification, 0, sizeof(notification));
    CHECK(krnP4SemaphoreNotificationClaim(consumer_worker, &notification,
                                          &consumer_sysbase) == 1);
    CHECK(notification.identity == consumer_live_waiter);
    CHECK(notification.count == 1u);
    CHECK(notification.generation ==
          consumer_records[3].semaphore_inflight_generation);
    CHECK(consumer_records[3].semaphore_pending == 0u);
    CHECK(consumer_records[3].semaphore_inflight == 1u);
    krnP4SemaphoreNotificationFinish(consumer_worker, &notification,
                                      &consumer_sysbase);
    CHECK(consumer_records[3].semaphore_inflight == 0u);
    CHECK(consumer_records[3].lifetime.notification_pins == UINT32_MAX - 2u);

    CHECK(krnP4SemaphoreOperationComplete(consumer_other_owner,
              other_owner_operation.generation, &consumer_sysbase) == 1);
    CHECK(test_durable_release(&consumer_sem_b, &consumer_sysbase) == 1);
    CHECK(consumer_sem_b.ss_Owner == NULL && consumer_sem_b.ss_NestCount == 0);
    CHECK(!p4_cleanup_wake_pending);
    CHECK(mock_outer_semaphore_gate_held == 0 && !mock_gate_held);
    CHECK(mock_disable_calls == mock_enable_calls);
    CHECK(mock_acquire_calls == mock_unlock_calls);
    CHECK(worker_sem_lock_calls == worker_sem_unlock_calls);
    CHECK(mock_fatal_calls == 0u);
    CHECK(mock_irq_depth == 0u);
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
        fprintf(stderr, "included O-A baseline failed: result=%d checks=%u\n",
                baseline_result, baseline_checks);
        return 2;
    }
    if (fflush(stdout) != 0)
        return 2;
    page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0)
    {
        fprintf(stderr, "sysconf(_SC_PAGESIZE) failed: %d\n", errno);
        return 2;
    }
#if defined(MAP_ANONYMOUS)
    page = mmap(NULL, (size_t)page_size, PROT_NONE,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
#else
    page = mmap(NULL, (size_t)page_size, PROT_NONE,
                MAP_PRIVATE | MAP_ANON, -1, 0);
#endif
    if (page == MAP_FAILED)
    {
        fprintf(stderr, "mmap(PROT_NONE) failed: %d\n", errno);
        return 2;
    }

    test_actual_consumer_passes(page);
    if (munmap(page, (size_t)page_size) != 0)
        return 2;
    if (failures != 0u)
    {
        fprintf(stderr, "P4 cancellation consumer binding: %u/%u failed\n",
                failures, checks - baseline_checks);
        return 1;
    }
    printf("P4 cancellation consumer binding: %u checks passed; "
           "included O-A baseline %u\n", checks - baseline_checks,
           baseline_checks);
    return 0;
}
