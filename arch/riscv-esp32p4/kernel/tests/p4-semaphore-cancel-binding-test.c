/* Actual-source exercise of protected semaphore-cancellation bindings. */
#define _DEFAULT_SOURCE 1

int p4_semaphore_operation_admission_baseline_main(void);
#define main p4_semaphore_operation_admission_baseline_main
#include "p4-semaphore-operation-admission-test.c"
#undef main

#include <signal.h>
#include <sys/resource.h>
#include <sys/wait.h>

int main(void);

enum cancel_binding_rejection
{
    CANCEL_BINDING_UNARMED = 1,
    CANCEL_BINDING_NONCANONICAL = 2,
    CANCEL_BINDING_STOPPED = 3
};

static struct p4_task_registry_record cancel_records[TEST_RECORD_CAPACITY];
static struct SignalSemaphore cancel_sem_a;
static struct SignalSemaphore cancel_sem_b;

struct cancel_binding_snapshot
{
    struct p4_task_registry registry;
    struct p4_task_registry_record records[TEST_RECORD_CAPACITY];
    struct p4_semaphore_ledger ledger;
    struct p4_semaphore_ledger_operation operations[TEST_OP_CAPACITY];
    struct p4_semaphore_ledger_entry entries[TEST_ENTRY_CAPACITY];
    struct SemaphoreRequest requests[8];
    struct SignalSemaphore sem_a;
    struct SignalSemaphore sem_b;
    bool cleanup_wake_pending;
    unsigned int poke_calls;
};

static int cancel_binding_reset(struct ExecBase *SysBase)
{
    struct p4_semaphore_ledger_operation *operations;
    struct p4_semaphore_ledger_entry *entries;
    uint32_t operation_capacity, entry_capacity;

    (void)SysBase;
    if (test_boot_ledger == NULL || p4_task_pins_prepared != 1 ||
        mock_irq_depth != 0u || mock_gate_held ||
        mock_outer_semaphore_gate_held)
    {
        fprintf(stderr,
            "reset precondition: ledger=%p prepared=%d irq=%u gate=%d outer=%d\n",
            (void *)test_boot_ledger, p4_task_pins_prepared, mock_irq_depth,
            mock_gate_held, mock_outer_semaphore_gate_held);
        return 0;
    }

    operations = test_boot_ledger->operations;
    entries = test_boot_ledger->entries;
    operation_capacity = test_boot_ledger->operation_capacity;
    entry_capacity = test_boot_ledger->entry_capacity;
    if (operations == NULL || entries == NULL ||
        operation_capacity != TEST_OP_CAPACITY ||
        entry_capacity != TEST_ENTRY_CAPACITY)
    {
        fprintf(stderr,
            "reset storage: operations=%p entries=%p capacities=%u/%u\n",
            (void *)operations, (void *)entries, operation_capacity,
            entry_capacity);
        return 0;
    }

    p4_task_registry_init(&p4_task_records);
    memset(cancel_records, 0, sizeof(cancel_records));
    memset(test_boot_ledger, 0, sizeof(*test_boot_ledger));
    memset(operations, 0,
           (size_t)operation_capacity * sizeof(*operations));
    memset(entries, 0, (size_t)entry_capacity * sizeof(*entries));
    memset(test_boot_requests, 0, sizeof(test_boot_requests));
    memset(&cancel_sem_a, 0, sizeof(cancel_sem_a));
    memset(&cancel_sem_b, 0, sizeof(cancel_sem_b));
    p4_cleanup_worker = NULL;
    p4_cleanup_wake_mask = 0u;
    p4_cleanup_wake_pending = false;
    mock_irq_depth = 0u;
    mock_disable_calls = 0u;
    mock_enable_calls = 0u;
    mock_acquire_calls = 0u;
    mock_unlock_calls = 0u;
    mock_fatal_calls = 0u;
    mock_poke_calls = 0u;
    mock_retired_poke_calls = 0u;
    mock_gate_held = 0;
    mock_outer_semaphore_gate_held = 0;
    mock_task_lock_held = 0;
    mock_gate_address = &p4_task_pin_gate;

    if (krnP4PublicSpinInit(&p4_task_pin_gate) != P4_PUBLIC_SPIN_OK)
        return 0;
    if (p4_semaphore_ledger_init(test_boot_ledger, operations,
            operation_capacity, entries, entry_capacity) != 1)
    {
        fprintf(stderr, "reset ledger initialization failed\n");
        return 0;
    }
    return 1;
}

static void cancel_binding_snapshot_take(
    struct cancel_binding_snapshot *snapshot)
{
    memset(snapshot, 0, sizeof(*snapshot));
    memcpy(&snapshot->registry, &p4_task_records,
           sizeof(snapshot->registry));
    memcpy(snapshot->records, cancel_records, sizeof(snapshot->records));
    memcpy(&snapshot->ledger, test_boot_ledger, sizeof(snapshot->ledger));
    memcpy(snapshot->operations, test_boot_ledger->operations,
           sizeof(snapshot->operations));
    memcpy(snapshot->entries, test_boot_ledger->entries,
           sizeof(snapshot->entries));
    memcpy(snapshot->requests, test_boot_requests,
           sizeof(snapshot->requests));
    memcpy(&snapshot->sem_a, &cancel_sem_a, sizeof(snapshot->sem_a));
    memcpy(&snapshot->sem_b, &cancel_sem_b, sizeof(snapshot->sem_b));
    snapshot->cleanup_wake_pending = p4_cleanup_wake_pending;
    snapshot->poke_calls = mock_poke_calls;
}

static int cancel_binding_snapshot_matches(
    const struct cancel_binding_snapshot *snapshot)
{
    struct cancel_binding_snapshot current;
    cancel_binding_snapshot_take(&current);
    return memcmp(snapshot, &current, sizeof(current)) == 0;
}

static void cancel_binding_publish(struct Task *task_a,
    struct Task *task_b, struct Task *task_c, struct Task *worker,
    struct ExecBase *SysBase)
{
    /* Registry insertion is at the head: the legacy claim wrapper sees A,
     * then B, while A's exclusive claim remains held. */
    CHECK(krnP4TaskRecordPublish(worker, &cancel_records[3], SysBase) == 1);
    CHECK(krnP4TaskRecordPublish(task_c, &cancel_records[2], SysBase) == 1);
    CHECK(krnP4TaskRecordPublish(task_b, &cancel_records[1], SysBase) == 1);
    CHECK(krnP4TaskRecordPublish(task_a, &cancel_records[0], SysBase) == 1);
}

static void cancel_binding_prepare_operation(struct Task *task,
    struct p4_task_wait_operation *operation,
    struct SignalSemaphore *const semaphores[], const uint32_t shared[],
    uint32_t count, const int expected[], unsigned int hart,
    struct ExecBase *SysBase)
{
    CHECK(krnP4TaskRecordEnterRunning(task, hart, SysBase) == 1);
    CHECK(krnP4SemaphoreOperationReserve(task, count, operation,
              SysBase) == 1);
    test_configure_operation(task, operation, semaphores, shared, count,
                             SysBase);
    test_acquire_operation(task, operation, semaphores, expected, count);
    krnP4TaskRecordLeaveRunning(task, hart, SysBase);
}

static void cancel_binding_retire_for_service(struct Task *task,
    struct ExecBase *SysBase)
{
    mock_task_lock_held = 1;
    CHECK(krnP4TaskRecordRetireForService(task, SysBase) == 1);
    mock_task_lock_held = 0;
}

static void cancel_binding_drain_wake(struct Task *worker,
    struct ExecBase *SysBase)
{
    struct p4_cleanup_wake wake;
    memset(&wake, 0xa5, sizeof(wake));
    CHECK(krnP4TakeCleanupWake(&wake, SysBase) == 1);
    CHECK(wake.worker == worker);
    CHECK(wake.mask == (UINT32_C(1) << 5));
    CHECK(!p4_cleanup_wake_pending);
}

static void test_cancelled_owner_handoff(void *page,
    struct ExecBase *SysBase)
{
    struct p4_task_wait_operation operation_a = { 0, 0 };
    struct p4_task_wait_operation operation_b = { 0, 0 };
    struct p4_task_wait_operation operation_c = { 0, 0 };
    struct p4_task_wait_cancel_snapshot cancel_a;
    struct p4_task_wait_cancel_snapshot cancel_b;
    struct p4_cleanup_wake wake;
    struct SignalSemaphore *sem_a[1] = { &cancel_sem_a };
    struct SignalSemaphore *sem_b[1] = { &cancel_sem_a };
    struct SignalSemaphore *sem_c[1] = { &cancel_sem_a };
    const uint32_t shared[1] = { 0u };
    const int owner[1] = { 1 };
    const int queued[1] = { 2 };
    struct Task *task_a = guarded_identity(page, 64u);
    struct Task *task_b = guarded_identity(page, 128u);
    struct Task *task_c = guarded_identity(page, 192u);
    struct Task *worker = guarded_identity(page, 256u);
    unsigned int poke_before;
    int result;

    test_empty_semaphore(&cancel_sem_a);
    test_empty_semaphore(&cancel_sem_b);
    memset(&cancel_a, 0, sizeof(cancel_a));
    memset(&cancel_b, 0, sizeof(cancel_b));
    cancel_binding_publish(task_a, task_b, task_c, worker, SysBase);

    CHECK(krnP4TaskRecordEnterRunning(worker, 0u, SysBase) == 1);
    CHECK(krnP4ProtectCleanupWorker(worker, SysBase) == 1);
    CHECK(krnP4ArmCleanupWake(worker, UINT32_C(1) << 5, SysBase) == 1);

    /* A owns the semaphore; B and C become real ledger-backed queue nodes. */
    cancel_binding_prepare_operation(task_a, &operation_a, sem_a, shared,
        1u, owner, 1u, SysBase);
    cancel_binding_prepare_operation(task_b, &operation_b, sem_b, shared,
        1u, queued, 1u, SysBase);
    cancel_binding_prepare_operation(task_c, &operation_c, sem_c, shared,
        1u, queued, 1u, SysBase);
    CHECK(cancel_sem_a.ss_Owner == task_a);
    CHECK(cancel_sem_a.ss_NestCount == 1);
    CHECK(cancel_sem_a.ss_QueueCount == 2);
    CHECK(cancel_sem_a.ss_WaitQueue.mlh_Head ==
          (struct MinNode *)(void *)operation_b.operation->first->request);
    CHECK(cancel_records[2].wait_scope.queued_count == 1u);

    cancel_binding_retire_for_service(task_a, SysBase);
    cancel_binding_retire_for_service(task_b, SysBase);
    CHECK(cancel_records[0].wait_scope.cancel_requested == 1u);
    CHECK(cancel_records[1].wait_scope.cancel_requested == 1u);
    CHECK(p4_cleanup_wake_pending);
    cancel_binding_drain_wake(worker, SysBase);

    /* Keep A's cancellation claim while taking B's distinct claim. */
    CHECK(krnP4SemaphoreWaitClaimCancel(&cancel_a, SysBase) == 1);
    CHECK(cancel_a.identity == task_a);
    CHECK(cancel_a.generation == operation_a.generation);
    CHECK(krnP4SemaphoreWaitClaimCancel(&cancel_b, SysBase) == 1);
    CHECK(cancel_b.identity == task_b);
    CHECK(cancel_b.generation == operation_b.generation);
    CHECK(cancel_records[0].wait_scope.cancel_claimed == 1u);
    CHECK(cancel_records[1].wait_scope.cancel_claimed == 1u);

    poke_before = mock_poke_calls;
    test_semaphore_gate_lock();
    result = krnP4SemaphoreCancelWithdraw(worker, task_b,
        operation_b.generation, 0u, &cancel_sem_a, SysBase);
    CHECK(mock_outer_semaphore_gate_held && !mock_gate_held);
    CHECK(p4_cleanup_wake_pending);
    CHECK(mock_poke_calls == poke_before);
    test_semaphore_gate_unlock();
    CHECK(result == 1);
    CHECK(cancel_records[1].wait_scope.queued_count == 0u);
    CHECK(cancel_sem_a.ss_QueueCount == 1);
    CHECK(cancel_sem_a.ss_WaitQueue.mlh_Head ==
          (struct MinNode *)(void *)operation_c.operation->first->request);
    CHECK(mock_poke_calls == poke_before);
    krnP4CleanupPoke(SysBase); /* Consumer poke only after outer unlock. */
    CHECK(mock_poke_calls == poke_before + 1u);
    cancel_binding_drain_wake(worker, SysBase);

    poke_before = mock_poke_calls;
    CHECK(krnP4SemaphoreOperationFinishCancel(task_b,
              operation_b.generation, SysBase) == 1);
    CHECK(cancel_records[1].wait_scope.active == 0u);
    CHECK(cancel_records[1].service_queued == 1u);
    CHECK(p4_cleanup_wake_pending);
    CHECK(mock_poke_calls == poke_before + 1u);
    cancel_binding_drain_wake(worker, SysBase);

    /* A's last unit is now rolled back and handed durably to live C. */
    poke_before = mock_poke_calls;
    test_semaphore_gate_lock();
    result = krnP4SemaphoreCancelRollbackDurable(worker, task_a,
        operation_a.generation, 0u, &cancel_sem_a, SysBase);
    CHECK(mock_outer_semaphore_gate_held && !mock_gate_held);
    CHECK(p4_cleanup_wake_pending);
    CHECK(mock_poke_calls == poke_before);
    test_semaphore_gate_unlock();
    CHECK(result == 2); /* One rolled-back unit plus one notification. */
    CHECK(cancel_sem_a.ss_Owner == task_c);
    CHECK(cancel_sem_a.ss_NestCount == 1);
    CHECK(cancel_sem_a.ss_QueueCount == 0);
    CHECK(cancel_sem_a.ss_WaitQueue.mlh_Head ==
          (struct MinNode *)(void *)&cancel_sem_a.ss_WaitQueue.mlh_Tail);
    CHECK(cancel_records[0].wait_scope.active == 1u);
    CHECK(cancel_records[2].semaphore_pending == 1u);
    CHECK(cancel_records[2].semaphore_pending_generation != 0u);
    CHECK(cancel_records[2].lifetime.notification_pins == 2u);
    CHECK(mock_poke_calls == poke_before);
    krnP4CleanupPoke(SysBase);
    CHECK(mock_poke_calls == poke_before + 1u);
    cancel_binding_drain_wake(worker, SysBase);

    /* Completing C's scope leaves its cohort reservation as the sole pin. */
    CHECK(krnP4SemaphoreOperationComplete(task_c,
              operation_c.generation, SysBase) == 1);
    CHECK(cancel_records[2].lifetime.notification_pins == 1u);
    CHECK(cancel_records[2].semaphore_pending == 1u);

    poke_before = mock_poke_calls;
    CHECK(krnP4SemaphoreOperationFinishCancel(task_a,
              operation_a.generation, SysBase) == 1);
    CHECK(cancel_records[0].wait_scope.active == 0u);
    CHECK(cancel_records[0].service_pending == 1u);
    CHECK(cancel_records[0].service_queued == 1u);
    CHECK(cancel_records[2].semaphore_pending == 1u);
    CHECK(cancel_records[2].lifetime.notification_pins == 1u);
    CHECK(p4_cleanup_wake_pending);
    CHECK(mock_poke_calls == poke_before + 1u);
    memset(&wake, 0xa5, sizeof(wake));
    CHECK(krnP4TakeCleanupWake(&wake, SysBase) == 1);
    CHECK(wake.worker == worker && wake.mask == (UINT32_C(1) << 5));
    CHECK(!p4_cleanup_wake_pending);
}

static void test_run_held_rollback_refusal(void *page,
    struct ExecBase *SysBase)
{
    struct p4_task_wait_operation operation_a = { 0, 0 };
    struct p4_task_wait_operation operation_b = { 0, 0 };
    struct p4_task_wait_cancel_snapshot cancel_a;
    struct SignalSemaphore *owner_sem[1] = { &cancel_sem_b };
    struct SignalSemaphore *target_sems[2] = {
        &cancel_sem_a, &cancel_sem_b
    };
    const uint32_t shared_owner[1] = { 0u };
    const uint32_t shared_target[2] = { 0u, 0u };
    const int owner_result[1] = { 1 };
    const int target_result[2] = { 1, 2 };
    struct Task *task_a = guarded_identity(page, 384u);
    struct Task *task_b = guarded_identity(page, 448u);
    struct Task *task_c = guarded_identity(page, 512u);
    struct Task *worker = guarded_identity(page, 576u);
    struct cancel_binding_snapshot before;
    int result;

    test_empty_semaphore(&cancel_sem_a);
    test_empty_semaphore(&cancel_sem_b);
    memset(&cancel_a, 0, sizeof(cancel_a));
    cancel_binding_publish(task_a, task_b, task_c, worker, SysBase);
    CHECK(krnP4TaskRecordEnterRunning(worker, 0u, SysBase) == 1);
    CHECK(krnP4ProtectCleanupWorker(worker, SysBase) == 1);
    CHECK(krnP4ArmCleanupWake(worker, UINT32_C(1) << 5, SysBase) == 1);

    /* B completes ownership of sem_b; A keeps RUN while holding sem_a and
     * queued on sem_b, making rollback's target preflight refuse unchanged. */
    cancel_binding_prepare_operation(task_b, &operation_b, owner_sem,
        shared_owner, 1u, owner_result, 1u, SysBase);
    CHECK(krnP4SemaphoreOperationComplete(task_b,
              operation_b.generation, SysBase) == 1);
    CHECK(krnP4TaskRecordEnterRunning(task_a, 1u, SysBase) == 1);
    CHECK(krnP4SemaphoreOperationReserve(task_a, 2u, &operation_a,
              SysBase) == 1);
    test_configure_operation(task_a, &operation_a, target_sems,
        shared_target, 2u, SysBase);
    test_acquire_operation(task_a, &operation_a, target_sems,
        target_result, 2u);
    CHECK(cancel_records[0].lifetime.running_harts != 0u);
    mock_task_lock_held = 1;
    CHECK(krnP4TaskRecordRetire(task_a, SysBase) == 1);
    mock_task_lock_held = 0;
    CHECK(p4_cleanup_wake_pending);
    cancel_binding_drain_wake(worker, SysBase);
    CHECK(krnP4SemaphoreWaitClaimCancel(&cancel_a, SysBase) == 1);
    CHECK(cancel_a.identity == task_a);

    cancel_binding_snapshot_take(&before);
    test_semaphore_gate_lock();
    result = krnP4SemaphoreCancelRollbackDurable(worker, task_a,
        operation_a.generation, 0u, &cancel_sem_a, SysBase);
    CHECK(mock_outer_semaphore_gate_held && !mock_gate_held);
    test_semaphore_gate_unlock();
    CHECK(result == 0);
    CHECK(cancel_binding_snapshot_matches(&before));
    CHECK(mock_poke_calls == before.poke_calls);
    CHECK(!p4_cleanup_wake_pending);
}

static void test_worker_binding_rejections(void *page,
    struct ExecBase *SysBase)
{
    struct rlimit no_core = { 0, 0 };
    struct Task *task_a = guarded_identity(page, 64u);
    struct Task *worker = guarded_identity(page, 256u);
    struct Task *unknown = guarded_identity(page, 704u);
    int kind;

    (void)setrlimit(RLIMIT_CORE, &no_core);
    for (kind = CANCEL_BINDING_UNARMED;
         kind <= CANCEL_BINDING_STOPPED; ++kind)
    {
        pid_t child = fork();
        int status = 0;
        if (child == 0)
        {
            struct Task *caller = worker;
            (void)freopen("/dev/null", "w", stderr);
            if (kind == CANCEL_BINDING_UNARMED)
                p4_cleanup_wake_mask = 0u;
            else if (kind == CANCEL_BINDING_NONCANONICAL)
                caller = unknown;
            else
                krnP4TaskRecordLeaveRunning(worker, 0u, SysBase);
            test_semaphore_gate_lock();
            (void)krnP4SemaphoreCancelWithdraw(caller, task_a, 1u, 0u,
                &cancel_sem_a, SysBase);
            _exit(0);
        }
        CHECK(child > 0);
        if (child > 0)
        {
            CHECK(waitpid(child, &status, 0) == child);
            CHECK(WIFSIGNALED(status));
            CHECK(WTERMSIG(status) == SIGABRT);
        }
    }
}

int main(void)
{
    long page_size;
    void *page;
    struct ExecBase SysBase = { 0 };
    unsigned int baseline_checks;
    int baseline_result;

    baseline_result = p4_semaphore_operation_admission_baseline_main();
    baseline_checks = checks;
    if (baseline_result != 0)
        return baseline_result;
    if (fflush(stdout) != 0)
    {
        fprintf(stderr, "baseline stdout flush failed\n");
        return 2;
    }

    if (cancel_binding_reset(&SysBase) != 1)
    {
        fprintf(stderr, "could not reset quiescent included-source fixture\n");
        return 2;
    }
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

    test_cancelled_owner_handoff(page, &SysBase);
    test_worker_binding_rejections(page, &SysBase);
    if (munmap(page, (size_t)page_size) != 0)
    {
        fprintf(stderr, "munmap failed: %d\n", errno);
        return 2;
    }

    if (cancel_binding_reset(&SysBase) != 1)
    {
        fprintf(stderr, "could not reset fixture before RUN-held refusal\n");
        return 2;
    }
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
    test_run_held_rollback_refusal(page, &SysBase);
    if (munmap(page, (size_t)page_size) != 0)
    {
        fprintf(stderr, "munmap failed: %d\n", errno);
        return 2;
    }

    if (failures != 0u)
    {
        fprintf(stderr,
            "P4 semaphore cancel bindings: %u/%u checks failed\n",
            failures, checks);
        return 1;
    }
    if (mock_irq_depth != 0u || mock_gate_held ||
        mock_outer_semaphore_gate_held ||
        mock_disable_calls != mock_enable_calls ||
        mock_acquire_calls != mock_unlock_calls || mock_fatal_calls != 0u)
    {
        fprintf(stderr, "P4 semaphore cancel bindings: fixture not quiescent\n");
        return 1;
    }
    printf("P4 semaphore cancel bindings: %u checks passed; included baseline %u\n",
        checks - baseline_checks, baseline_checks);
    return 0;
}
