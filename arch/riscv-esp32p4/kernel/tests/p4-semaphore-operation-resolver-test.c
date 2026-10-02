/* Actual-source regression for checked Task-scope/ledger resolution. */
#define _DEFAULT_SOURCE 1
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#define main p4_semaphore_operation_admission_baseline_main
#include "p4-semaphore-operation-admission-test.c"
#undef main

enum resolver_corruption
{
    CORRUPT_OPERATION_IDENTITY,
    CORRUPT_OWNER,
    CORRUPT_GENERATION,
    CORRUPT_CLOSED,
    CORRUPT_CANCELLED
};

static void make_direct_operation(struct direct_fixture *fixture, void *page,
    size_t task_offset, struct Task **task_out,
    struct p4_task_registry_record **record_out,
    struct p4_semaphore_ledger_operation **operation_out,
    uint32_t *generation_out)
{
    struct p4_task_wait_operation handle = { 0, 0 };
    struct p4_task_registry_record *record;
    struct Task *task = guarded_identity(page, task_offset);

    fixture_init(fixture, TEST_OP_CAPACITY, TEST_ENTRY_CAPACITY);
    record = fixture_add_task(fixture, 0u, task, 0u);
    CHECK(record != NULL);
    CHECK(reserve_direct(fixture, task, 1u, &handle) == 1);
    CHECK(handle.operation != NULL && handle.generation != 0u);
    *task_out = task;
    *record_out = record;
    *operation_out = handle.operation;
    *generation_out = handle.generation;
}

static void check_valid_domains(struct direct_fixture *fixture)
{
    CHECK(p4_task_registry_find_locked(&fixture->registry, NULL, NULL, NULL));
    CHECK(p4_semaphore_ledger_validate(&fixture->ledger));
}

static void test_resolver_success_stale_and_missing(void *page)
{
    struct direct_fixture fixture;
    struct p4_task_registry_record *record;
    struct p4_semaphore_ledger_operation *operation;
    struct p4_task_registry_record *out_record;
    struct p4_semaphore_ledger_operation *out_operation;
    struct p4_task_registry_record *record_sentinel =
        (struct p4_task_registry_record *)(uintptr_t)0x1000u;
    struct p4_semaphore_ledger_operation *operation_sentinel =
        (struct p4_semaphore_ledger_operation *)(uintptr_t)0x2000u;
    struct Task *task;
    struct byte_snapshot before;
    uint32_t generation;
    int result;

    make_direct_operation(&fixture, page, 64u, &task, &record, &operation,
                          &generation);
    check_valid_domains(&fixture);
    snapshot_fixture(&fixture, &before);
    out_record = record_sentinel;
    out_operation = operation_sentinel;
    result = p4_task_registry_operation_resolve_locked(&fixture.registry,
        &fixture.ledger, task, generation, &out_record, &out_operation);
    CHECK(result == 1);
    CHECK(out_record == record && out_operation == operation);
    CHECK(fixture_matches_snapshot(&fixture, &before));

    out_record = record_sentinel;
    out_operation = operation_sentinel;
    result = p4_task_registry_operation_resolve_locked(&fixture.registry,
        &fixture.ledger, task, generation + 1u, &out_record, &out_operation);
    CHECK(result == 0);
    CHECK(out_record == record_sentinel && out_operation == operation_sentinel);
    CHECK(fixture_matches_snapshot(&fixture, &before));

    out_record = record_sentinel;
    out_operation = operation_sentinel;
    result = p4_task_registry_operation_resolve_locked(&fixture.registry,
        &fixture.ledger, guarded_identity(page, 128u), generation,
        &out_record, &out_operation);
    CHECK(result == 0);
    CHECK(out_record == record_sentinel && out_operation == operation_sentinel);
    CHECK(fixture_matches_snapshot(&fixture, &before));
}

static void corrupt_operation(struct direct_fixture *fixture,
    struct p4_task_registry_record *record,
    struct p4_semaphore_ledger_operation *operation,
    struct Task *task, void *page, enum resolver_corruption corruption)
{
    switch (corruption)
    {
        case CORRUPT_OPERATION_IDENTITY:
            record->wait_scope.list_index = &fixture->operations[1];
            break;
        case CORRUPT_OWNER:
            operation->owner = guarded_identity(page, 256u);
            break;
        case CORRUPT_GENERATION:
            ++operation->generation;
            break;
        case CORRUPT_CLOSED:
            record->wait_scope.construction_closed = 1u;
            break;
        case CORRUPT_CANCELLED:
            CHECK(p4_semaphore_ledger_cancel(&fixture->ledger, operation) == 1);
            break;
        default:
            CHECK(0);
            break;
    }
    (void)task;
}

static void test_resolver_cross_link_corruption(void *page)
{
    unsigned int corruption;

    for (corruption = (unsigned int)CORRUPT_OPERATION_IDENTITY;
         corruption <= (unsigned int)CORRUPT_CANCELLED; ++corruption)
    {
        struct direct_fixture fixture;
        struct p4_task_registry_record *record;
        struct p4_semaphore_ledger_operation *operation;
        struct p4_task_registry_record *out_record;
        struct p4_semaphore_ledger_operation *out_operation;
        struct p4_task_registry_record *record_sentinel =
            (struct p4_task_registry_record *)(uintptr_t)0x3000u;
        struct p4_semaphore_ledger_operation *operation_sentinel =
            (struct p4_semaphore_ledger_operation *)(uintptr_t)0x4000u;
        struct Task *task;
        struct byte_snapshot before;
        uint32_t generation;
        int result;

        make_direct_operation(&fixture, page,
            512u + (size_t)corruption * 64u, &task, &record, &operation,
            &generation);
        corrupt_operation(&fixture, record, operation, task, page,
                          (enum resolver_corruption)corruption);
        /* Each side remains structurally valid on its own; only the opaque
         * Task-scope-to-ledger association is inconsistent. */
        check_valid_domains(&fixture);
        snapshot_fixture(&fixture, &before);
        out_record = record_sentinel;
        out_operation = operation_sentinel;
        result = p4_task_registry_operation_resolve_locked(&fixture.registry,
            &fixture.ledger, task, generation, &out_record, &out_operation);
        CHECK(result == -1);
        CHECK(out_record == record_sentinel &&
              out_operation == operation_sentinel);
        CHECK(fixture_matches_snapshot(&fixture, &before));
    }
}

static void test_resolver_output_alias_refusal(void *page)
{
    struct direct_fixture fixture;
    struct p4_task_registry_record *record;
    struct p4_semaphore_ledger_operation *operation;
    struct p4_task_registry_record *out_record;
    struct p4_semaphore_ledger_operation *out_operation;
    struct Task *task;
    struct byte_snapshot before;
    uint32_t generation;
    int result;

    make_direct_operation(&fixture, page, 1024u, &task, &record, &operation,
                          &generation);
    (void)record;
    (void)operation;
    snapshot_fixture(&fixture, &before);
    out_operation = (struct p4_semaphore_ledger_operation *)(uintptr_t)0x5000u;
    result = p4_task_registry_operation_resolve_locked(&fixture.registry,
        &fixture.ledger, task, generation,
        (struct p4_task_registry_record **)(void *)&fixture.registry.head,
        &out_operation);
    CHECK(result == -1);
    CHECK(out_operation ==
          (struct p4_semaphore_ledger_operation *)(uintptr_t)0x5000u);
    CHECK(fixture_matches_snapshot(&fixture, &before));

    out_record = (struct p4_task_registry_record *)(uintptr_t)0x6000u;
    result = p4_task_registry_operation_resolve_locked(&fixture.registry,
        &fixture.ledger, task, generation, &out_record,
        (struct p4_semaphore_ledger_operation **)(void *)&fixture.operations[2]);
    CHECK(result == -1);
    CHECK(out_record == (struct p4_task_registry_record *)(uintptr_t)0x6000u);
    CHECK(fixture_matches_snapshot(&fixture, &before));
}

static void test_lifecycle_callers_propagate_corruption(void *page)
{
    struct direct_fixture fixture;
    struct p4_task_registry_record *record;
    struct p4_semaphore_ledger_operation *operation;
    struct Task *task;
    struct byte_snapshot before;
    uint32_t generation;

    make_direct_operation(&fixture, page, 1152u, &task, &record, &operation,
                          &generation);
    operation->owner = guarded_identity(page, 1280u);
    check_valid_domains(&fixture);
    snapshot_fixture(&fixture, &before);

    CHECK(p4_task_registry_wait_operation_set_entry_locked(&fixture.registry,
        &fixture.ledger, task, generation, 0u,
        (const void *)(uintptr_t)0x8000u,
        (const void *)(uintptr_t)0x9000u) == -1);
    CHECK(fixture_matches_snapshot(&fixture, &before));
    CHECK(p4_task_registry_wait_operation_close_locked(&fixture.registry,
        &fixture.ledger, task, generation) == -1);
    CHECK(fixture_matches_snapshot(&fixture, &before));
    CHECK(p4_task_registry_wait_operation_complete_locked(&fixture.registry,
        &fixture.ledger, task, generation) == -1);
    CHECK(fixture_matches_snapshot(&fixture, &before));
    CHECK(p4_task_registry_wait_operation_finish_cancel_locked(
        &fixture.registry, &fixture.ledger, task, generation) == -1);
    CHECK(fixture_matches_snapshot(&fixture, &before));
    CHECK(p4_task_registry_retire_with_ledger_locked(&fixture.registry,
        &fixture.ledger, task, 0u) == -1);
    CHECK(fixture_matches_snapshot(&fixture, &before));
}

static void test_cancel_consumer_propagates_corruption(void *page)
{
    struct direct_fixture fixture;
    struct p4_task_registry_record *record;
    struct p4_semaphore_ledger_operation *operation;
    struct p4_task_wait_cancel_snapshot output;
    struct p4_task_wait_cancel_snapshot output_before;
    struct Task *task;
    struct SignalSemaphore sem;
    struct SemaphoreRequest request;
    struct byte_snapshot before;
    uint32_t generation;

    make_direct_operation(&fixture, page, 1408u, &task, &record, &operation,
                          &generation);
    test_empty_semaphore(&sem);
    memset(&request, 0, sizeof(request));
    request.sr_Waiter = task;
    CHECK(p4_semaphore_ledger_set_entry(&fixture.ledger, operation, 0u,
        &sem, &request) == 1);
    CHECK(p4_semaphore_ledger_close(&fixture.ledger, operation) == 1);
    CHECK(p4_semaphore_ledger_queue(&fixture.ledger, operation, 0u) == 1);
    record->wait_scope.construction_closed = 1u;
    record->wait_scope.queued_count = 1u;
    CHECK(p4_task_registry_retire_with_ledger_locked(&fixture.registry,
        &fixture.ledger, task, 0u) == 1);
    CHECK(record->wait_scope.cancel_requested == 1u &&
          record->lifetime.running_harts == 1u);
    operation->owner = guarded_identity(page, 1472u);
    check_valid_domains(&fixture);
    memset(&output, 0xa5, sizeof(output));
    output_before = output;
    snapshot_fixture(&fixture, &before);

    CHECK(p4_task_registry_claim_wait_operation_cancel_after_with_ledger_locked(
        &fixture.registry, &fixture.ledger, 0u, &output) == -1);
    CHECK(memcmp(&output, &output_before, sizeof(output)) == 0);
    CHECK(fixture_matches_snapshot(&fixture, &before));
}

enum wrapper_fatal_case
{
    WRAPPER_FATAL_CONFIGURE,
    WRAPPER_FATAL_ACQUIRE
};

static int expect_wrapper_abort(enum wrapper_fatal_case which,
    struct Task *task, uint32_t generation, struct SignalSemaphore *sem,
    struct ExecBase *SysBase)
{
    struct rlimit no_core = { 0u, 0u };
    pid_t child;
    int status;

    if (fflush(NULL) != 0)
        return 0;
    child = fork();
    if (child < 0)
        return 0;
    if (child == 0)
    {
        (void)setrlimit(RLIMIT_CORE, &no_core);
        (void)close(STDERR_FILENO);
        if (which == WRAPPER_FATAL_CONFIGURE)
        {
            test_semaphore_gate_lock();
            (void)krnP4SemaphoreOperationConfigure(task, generation, 0u,
                sem, 0u, SysBase);
        }
        else
        {
            test_semaphore_gate_lock();
            (void)krnP4SemaphoreOperationAcquire(task, generation, 0u,
                sem, SysBase);
        }
        _exit(91);
    }
    if (waitpid(child, &status, 0) != child)
        return 0;
    return WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT;
}

static void test_public_wrapper_fatal_translation(void *page)
{
    static struct p4_task_registry_record wrapper_record;
    struct p4_task_wait_operation handle = { 0, 0 };
    struct p4_task_registry registry_before;
    struct p4_semaphore_ledger ledger_before;
    struct p4_semaphore_ledger_operation operations_before[TEST_OP_CAPACITY];
    struct p4_semaphore_ledger_entry entries_before[TEST_ENTRY_CAPACITY];
    struct SignalSemaphore sem;
    struct ExecBase sysbase = { 0 };
    struct Task *task = guarded_identity(page, 1536u);
    struct p4_semaphore_ledger_operation *operation;
    int result;

    CHECK(p4_task_pins_prepared == 1);
    memset(&wrapper_record, 0, sizeof(wrapper_record));
    CHECK(krnP4TaskRecordPublish(task, &wrapper_record, &sysbase) == 1);
    CHECK(krnP4TaskRecordEnterRunning(task, 0u, &sysbase) == 1);
    CHECK(krnP4SemaphoreOperationReserve(task, 1u, &handle, &sysbase) == 1);
    CHECK(handle.operation != NULL && handle.generation != 0u);
    operation = handle.operation;
    test_empty_semaphore(&sem);

    operation->owner = guarded_identity(page, 1600u);
    memcpy(&registry_before, &p4_task_records, sizeof(registry_before));
    memcpy(&ledger_before, test_boot_ledger, sizeof(ledger_before));
    memcpy(operations_before, test_boot_ledger->operations,
           sizeof(operations_before));
    memcpy(entries_before, test_boot_ledger->entries, sizeof(entries_before));
    CHECK(expect_wrapper_abort(WRAPPER_FATAL_CONFIGURE, task,
        handle.generation, &sem, &sysbase));
    CHECK(memcmp(&registry_before, &p4_task_records,
                 sizeof(registry_before)) == 0);
    CHECK(memcmp(&ledger_before, test_boot_ledger, sizeof(ledger_before)) == 0);
    CHECK(memcmp(operations_before, test_boot_ledger->operations,
                 sizeof(operations_before)) == 0);
    CHECK(memcmp(entries_before, test_boot_ledger->entries,
                 sizeof(entries_before)) == 0);
    operation->owner = task;

    test_semaphore_gate_lock();
    result = krnP4SemaphoreOperationConfigure(task, handle.generation, 0u,
        &sem, 0u, &sysbase);
    CHECK(result == 1);
    test_semaphore_gate_unlock();
    CHECK(krnP4SemaphoreOperationClose(task, handle.generation, &sysbase) == 1);

    operation->owner = guarded_identity(page, 1664u);
    memcpy(&registry_before, &p4_task_records, sizeof(registry_before));
    memcpy(&ledger_before, test_boot_ledger, sizeof(ledger_before));
    memcpy(operations_before, test_boot_ledger->operations,
           sizeof(operations_before));
    memcpy(entries_before, test_boot_ledger->entries, sizeof(entries_before));
    CHECK(expect_wrapper_abort(WRAPPER_FATAL_ACQUIRE, task,
        handle.generation, &sem, &sysbase));
    CHECK(memcmp(&registry_before, &p4_task_records,
                 sizeof(registry_before)) == 0);
    CHECK(memcmp(&ledger_before, test_boot_ledger, sizeof(ledger_before)) == 0);
    CHECK(memcmp(operations_before, test_boot_ledger->operations,
                 sizeof(operations_before)) == 0);
    CHECK(memcmp(entries_before, test_boot_ledger->entries,
                 sizeof(entries_before)) == 0);
    operation->owner = task;

    test_semaphore_gate_lock();
    CHECK(krnP4SemaphoreOperationAcquire(task, handle.generation, 0u,
        &sem, &sysbase) == 1);
    test_semaphore_gate_unlock();
    CHECK(krnP4SemaphoreOperationComplete(task, handle.generation,
        &sysbase) == 1);
    krnP4TaskRecordLeaveRunning(task, 0u, &sysbase);
    CHECK(krnP4TaskRecordRetire(task, &sysbase) == 1);
    CHECK(krnP4TaskRecordClaimReclaim(task, &sysbase) == &wrapper_record);
}

int main(void)
{
    long page_size;
    void *page;
    unsigned int baseline_checks;
    int baseline_result;

    alarm(20u);
    baseline_result = p4_semaphore_operation_admission_baseline_main();
    if (baseline_result != 0)
        return baseline_result;
    baseline_checks = checks;
    page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0)
    {
        fprintf(stderr, "sysconf(_SC_PAGESIZE) failed\n");
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
        fprintf(stderr, "mmap(PROT_NONE) failed\n");
        return 2;
    }

    test_resolver_success_stale_and_missing(page);
    test_resolver_cross_link_corruption(page);
    test_resolver_output_alias_refusal(page);
    test_lifecycle_callers_propagate_corruption(page);
    test_cancel_consumer_propagates_corruption(page);
    test_public_wrapper_fatal_translation(page);

    if (munmap(page, (size_t)page_size) != 0)
        return 2;
    if (failures != 0u)
    {
        fprintf(stderr, "P4 semaphore resolver: %u/%u total checks failed\n",
                failures, checks);
        return 1;
    }
    printf("P4 semaphore resolver: admission baseline %u + %u new checks passed\n",
           baseline_checks, checks - baseline_checks);
    return 0;
}
