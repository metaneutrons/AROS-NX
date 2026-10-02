/* Actual-source tests for protected operation-cancellation snapshots. */
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

/* Reuse the real node/registry/ledger/pin fixture and its checked lock mocks. */
int p4_semaphore_operation_admission_baseline_main(void);
#define main p4_semaphore_operation_admission_baseline_main
#include "p4-semaphore-operation-admission-test.c"
#undef main

#define CANCEL_RECORDS 5u
#define CANCEL_OPERATIONS 8u
#define CANCEL_ENTRIES 8u

static struct p4_semaphore_ledger cancel_ledger;
static struct p4_semaphore_ledger_operation cancel_operations[CANCEL_OPERATIONS];
static struct p4_semaphore_ledger_entry cancel_entries[CANCEL_ENTRIES];
static struct p4_task_registry_record cancel_records[CANCEL_RECORDS];
static struct SignalSemaphore cancel_sem_a;
static struct SignalSemaphore cancel_sem_b;
static struct ExecBase cancel_sysbase;
static struct Task *cancel_worker;
static struct Task *cancel_legacy;
static struct Task *cancel_a;
static struct Task *cancel_b;
static struct Task *cancel_c;
static struct p4_task_registry_record *cancel_legacy_record;
static struct p4_task_registry_record *cancel_a_record;
static struct p4_task_registry_record *cancel_b_record;
static struct p4_task_registry_record *cancel_c_record;
static struct p4_task_wait_operation cancel_a_operation;
static struct p4_task_wait_operation cancel_b_operation;
static struct p4_task_wait_operation cancel_c_operation;

struct cancel_state_image
{
    struct p4_task_registry registry;
    struct p4_task_registry_record records[CANCEL_RECORDS];
    struct p4_semaphore_ledger ledger;
    struct p4_semaphore_ledger_operation operations[CANCEL_OPERATIONS];
    struct p4_semaphore_ledger_entry entries[CANCEL_ENTRIES];
    struct SemaphoreRequest requests[8];
    struct SignalSemaphore sem_a;
    struct SignalSemaphore sem_b;
};

static void cancel_state_capture(struct cancel_state_image *image)
{
    memcpy(&image->registry, &p4_task_records, sizeof(image->registry));
    memcpy(image->records, cancel_records, sizeof(image->records));
    memcpy(&image->ledger, &cancel_ledger, sizeof(image->ledger));
    memcpy(image->operations, cancel_operations, sizeof(image->operations));
    memcpy(image->entries, cancel_entries, sizeof(image->entries));
    memcpy(image->requests, test_boot_requests, sizeof(image->requests));
    memcpy(&image->sem_a, &cancel_sem_a, sizeof(image->sem_a));
    memcpy(&image->sem_b, &cancel_sem_b, sizeof(image->sem_b));
}

static int cancel_state_matches(const struct cancel_state_image *image)
{
    struct cancel_state_image current;
    cancel_state_capture(&current);
    return memcmp(&current, image, sizeof(current)) == 0;
}

static void cancel_publish_running(unsigned int slot, struct Task *identity,
                                   unsigned int hart)
{
    CHECK(slot < CANCEL_RECORDS);
    CHECK(krnP4TaskRecordPublish(identity, &cancel_records[slot],
                                  &cancel_sysbase) == 1);
    CHECK(krnP4TaskRecordEnterRunning(identity, hart, &cancel_sysbase) == 1);
}

static void cancel_retire_for_service(struct Task *identity)
{
    mock_task_lock_held = 1;
    CHECK(krnP4TaskRecordRetireForService(identity, &cancel_sysbase) == 1);
    mock_task_lock_held = 0;
}

static void cancel_build_operation(struct Task *identity,
    struct p4_task_wait_operation *operation,
    struct SignalSemaphore *const semaphores[], const uint32_t shared[],
    const int expected[], uint32_t count)
{
    CHECK(krnP4SemaphoreOperationReserve(identity, count, operation,
                                          &cancel_sysbase) == 1);
    test_configure_operation(identity, operation, semaphores, shared, count,
                             &cancel_sysbase);
    test_acquire_operation(identity, operation, semaphores, expected, count);
}

static void cancel_drain_wake(void)
{
    struct p4_cleanup_wake wake;
    if (p4_cleanup_wake_pending)
    {
        memset(&wake, 0xa5, sizeof(wake));
        CHECK(krnP4TakeCleanupWake(&wake, &cancel_sysbase) == 1);
        CHECK(wake.worker == cancel_worker);
        CHECK(wake.mask == (UINT32_C(1) << 5));
    }
}

static void cancel_setup(void *page)
{
    struct p4_task_wait_cancel_snapshot legacy_snapshot;
    uint32_t legacy_generation = 0;
    const struct SignalSemaphore *legacy_sem;

    cancel_worker = guarded_identity(page, 256u);
    cancel_legacy = guarded_identity(page, 64u);
    cancel_a = guarded_identity(page, 128u);
    cancel_b = guarded_identity(page, 192u);
    cancel_c = guarded_identity(page, 320u);
    memset(cancel_records, 0, sizeof(cancel_records));
    memset(cancel_operations, 0, sizeof(cancel_operations));
    memset(cancel_entries, 0, sizeof(cancel_entries));
    memset(test_boot_requests, 0, sizeof(test_boot_requests));
    memset(&cancel_sysbase, 0, sizeof(cancel_sysbase));
    memset(&cancel_sem_a, 0, sizeof(cancel_sem_a));
    memset(&cancel_sem_b, 0, sizeof(cancel_sem_b));
    memset(&cancel_a_operation, 0, sizeof(cancel_a_operation));
    memset(&cancel_b_operation, 0, sizeof(cancel_b_operation));
    memset(&cancel_c_operation, 0, sizeof(cancel_c_operation));
    test_empty_semaphore(&cancel_sem_a);
    test_empty_semaphore(&cancel_sem_b);

    test_boot_ledger = &cancel_ledger;
    CHECK(p4_task_pins_prepared == 1);

    cancel_publish_running(0u, cancel_worker, 0u);
    CHECK(krnP4ProtectCleanupWorker(cancel_worker, &cancel_sysbase) == 1);
    CHECK(krnP4ArmCleanupWake(cancel_worker, UINT32_C(1) << 5,
                              &cancel_sysbase) == 1);

    /* Keep a lower-address, sticky cancelled legacy scope visible to prove
     * operation-only address scans neither claim nor wrap through it. */
    cancel_publish_running(1u, cancel_legacy, 1u);
    legacy_sem = &cancel_sem_a;
    CHECK(krnP4SemaphoreWaitBegin(cancel_legacy, P4_TASK_WAIT_SINGLE,
              legacy_sem, &test_boot_requests[7], NULL,
              &legacy_generation, &cancel_sysbase) == 1);
    CHECK(krnP4SemaphoreWaitClose(cancel_legacy, legacy_generation,
                                  &cancel_sysbase) == 1);
    krnP4TaskRecordLeaveRunning(cancel_legacy, 1u, &cancel_sysbase);
    CHECK(krnP4TaskRecordRetire(cancel_legacy, &cancel_sysbase) == 1);
    memset(&legacy_snapshot, 0xa5, sizeof(legacy_snapshot));
    CHECK(krnP4SemaphoreWaitClaimCancel(&legacy_snapshot,
              &cancel_sysbase) == 1);
    CHECK(legacy_snapshot.identity == cancel_legacy);
    CHECK(legacy_snapshot.kind == P4_TASK_WAIT_SINGLE);
    CHECK(krnP4SemaphoreWaitReleaseCancel(cancel_legacy, legacy_generation,
              &cancel_sysbase) == 1);
    cancel_legacy_record = &cancel_records[1];
    CHECK(cancel_legacy_record->wait_scope.cancel_requested == 1u);
    CHECK(cancel_legacy_record->wait_scope.cancel_claimed == 0u);
    cancel_drain_wake();

    cancel_publish_running(2u, cancel_a, 1u);
    {
        struct SignalSemaphore *semaphores[2] = {
            &cancel_sem_a, &cancel_sem_b
        };
        const uint32_t shared[2] = { 0u, 0u };
        const int immediate[2] = { 1, 1 };
        cancel_build_operation(cancel_a, &cancel_a_operation, semaphores,
                               shared, immediate, 2u);
    }
    cancel_a_record = &cancel_records[2];
    cancel_retire_for_service(cancel_a);
    krnP4TaskRecordLeaveRunning(cancel_a, 1u, &cancel_sysbase);
    CHECK(cancel_a_record->lifetime.running_harts == 0u);

    cancel_publish_running(3u, cancel_b, 1u);
    {
        struct SignalSemaphore *semaphores[1] = { &cancel_sem_b };
        const uint32_t shared[1] = { 0u };
        const int queued[1] = { 2 };
        cancel_build_operation(cancel_b, &cancel_b_operation, semaphores,
                               shared, queued, 1u);
    }
    cancel_b_record = &cancel_records[3];
    cancel_retire_for_service(cancel_b); /* Preserve hart1 RUN through claim. */

    cancel_publish_running(4u, cancel_c, 1u);
    {
        struct SignalSemaphore *semaphores[1] = { &cancel_sem_b };
        const uint32_t shared[1] = { 0u };
        const int queued[1] = { 2 };
        cancel_build_operation(cancel_c, &cancel_c_operation, semaphores,
                               shared, queued, 1u);
    }
    cancel_c_record = &cancel_records[4];
    cancel_retire_for_service(cancel_c);
    CHECK(cancel_sem_b.ss_NestCount == 1);
    CHECK(cancel_sem_b.ss_QueueCount == 2);
    CHECK(cancel_b_record->wait_scope.queued_count == 1u);
    CHECK(cancel_c_record->wait_scope.queued_count == 1u);
    cancel_drain_wake();
}

static int cancel_leaf_claim(uintptr_t after,
    struct p4_task_wait_cancel_snapshot *snapshot)
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
    result = p4_task_registry_claim_wait_operation_cancel_after_with_ledger_locked(
        &p4_task_records, &cancel_ledger, after, snapshot);
    CHECK(krnP4PublicSpinUnlock(&p4_task_pin_gate) == P4_PUBLIC_SPIN_OK);
    Enable();
    return result;
}

enum malformed_kind
{
    MALFORMED_LINK,
    MALFORMED_OWNER,
    MALFORMED_GENERATION,
    MALFORMED_CANCELLED,
    MALFORMED_CLOSED,
    MALFORMED_QUEUE_COUNT
};

static void cancel_test_malformed(enum malformed_kind kind)
{
    struct p4_task_wait_cancel_snapshot output, before_output;
    struct cancel_state_image before_state;
    struct p4_semaphore_ledger_operation *operation = cancel_b_operation.operation;
    struct p4_task_wait_scope scope_before = cancel_b_record->wait_scope;
    struct p4_semaphore_ledger_operation operation_before = *operation;
    int result;

    switch (kind)
    {
        case MALFORMED_LINK:
            cancel_b_record->wait_scope.list_index = cancel_a_operation.operation;
            break;
        case MALFORMED_OWNER:
            operation->owner = cancel_a;
            break;
        case MALFORMED_GENERATION:
            ++operation->generation;
            break;
        case MALFORMED_CANCELLED:
            operation->cancelled = 0u;
            break;
        case MALFORMED_CLOSED:
            operation->construction_closed = 0u;
            break;
        case MALFORMED_QUEUE_COUNT:
            /* Preserve RUN-independent eligibility while making the
             * registry/ledger queued-count relation inconsistent. */
            cancel_b_record->wait_scope.queued_count = 2u;
            break;
        default:
            CHECK(0);
            return;
    }
    memset(&output, 0xa5, sizeof(output));
    memcpy(&before_output, &output, sizeof(output));
    cancel_state_capture(&before_state);
    result = cancel_leaf_claim((uintptr_t)cancel_a, &output);
    CHECK(result == -1);
    CHECK(memcmp(&output, &before_output, sizeof(output)) == 0);
    CHECK(cancel_state_matches(&before_state));

    cancel_b_record->wait_scope = scope_before;
    *operation = operation_before;
}

static void cancel_test_cross_domain_refusals(void)
{
    cancel_test_malformed(MALFORMED_LINK);
    cancel_test_malformed(MALFORMED_OWNER);
    cancel_test_malformed(MALFORMED_GENERATION);
    cancel_test_malformed(MALFORMED_CANCELLED);
    cancel_test_malformed(MALFORMED_CLOSED);
    cancel_test_malformed(MALFORMED_QUEUE_COUNT);
}

static void cancel_expect_fatal_child(int describe, struct Task *target,
    uint32_t generation, void *bad_output)
{
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
        if (describe)
            (void)krnP4SemaphoreCancelDescribe(cancel_worker, target,
                generation, 0u,
                (struct p4_semaphore_cancel_entry_snapshot *)bad_output,
                &cancel_sysbase);
        else
            (void)krnP4SemaphoreCancelClaimAfter(cancel_worker, 0u,
                (struct p4_task_wait_cancel_snapshot *)bad_output,
                &cancel_sysbase);
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

static void cancel_test_alias_refusals(void)
{
    void *aliases[4];
    uint32_t i;
    aliases[0] = (void *)&test_boot_requests[0];
    aliases[1] = (void *)&cancel_ledger.entries[0];
    aliases[2] = (void *)&p4_task_records;
    for (i = 0; i < 3u; ++i)
    {
        cancel_expect_fatal_child(0, cancel_a,
            cancel_a_operation.generation, aliases[i]);
        cancel_expect_fatal_child(1, cancel_a,
            cancel_a_operation.generation, aliases[i]);
    }
    /* Describe also rejects snapshots overlapping the separately-owned
     * semaphore object whose pointer it would otherwise publish. */
    cancel_expect_fatal_child(1, cancel_b,
        cancel_b_operation.generation, &cancel_sem_b);
}

static void cancel_test_snapshots_and_release(void)
{
    struct p4_task_wait_cancel_snapshot snapshot;
    struct p4_semaphore_cancel_entry_snapshot entry, before_entry;
    unsigned int poke_before;
    int result;

    memset(&snapshot, 0xa5, sizeof(snapshot));
    CHECK(krnP4SemaphoreCancelClaimAfter(cancel_worker, 0u, &snapshot,
                                         &cancel_sysbase) == 1);
    CHECK(snapshot.identity == cancel_a);
    CHECK(snapshot.generation == cancel_a_operation.generation);
    CHECK(snapshot.kind == P4_TASK_WAIT_OPERATION);
    CHECK(snapshot.queued_count == 0u && snapshot.construction_closed == 1u);
    CHECK(snapshot.cancel_requested == 1u);
    CHECK(snapshot.semaphore_index == NULL && snapshot.request_index == NULL &&
          snapshot.list_index == NULL);
    CHECK(cancel_a_record->wait_scope.cancel_claimed == 1u);

    memset(&entry, 0xa5, sizeof(entry));
    CHECK(krnP4SemaphoreCancelDescribe(cancel_worker, cancel_a,
              cancel_a_operation.generation, 0u, &entry,
              &cancel_sysbase) == 1);
    CHECK(entry.semaphore == &cancel_sem_a);
    CHECK(entry.entry_count == 2u);
    CHECK(entry.state == P4_SEMAPHORE_LEDGER_ENTRY_OWNED);
    CHECK(entry.committed == 0u && entry.running_harts == 0u);
    CHECK(krnP4SemaphoreCancelDescribe(cancel_worker, cancel_a,
              cancel_a_operation.generation, 1u, &entry,
              &cancel_sysbase) == 1);
    CHECK(entry.semaphore == &cancel_sem_b);
    CHECK(entry.state == P4_SEMAPHORE_LEDGER_ENTRY_OWNED);
    CHECK(entry.entry_count == 2u && entry.committed == 0u);

    memset(&entry, 0x6d, sizeof(entry));
    memcpy(&before_entry, &entry, sizeof(entry));
    CHECK(krnP4SemaphoreCancelDescribe(cancel_worker, cancel_a,
              cancel_a_operation.generation + 1u, 0u, &entry,
              &cancel_sysbase) == 0);
    CHECK(memcmp(&entry, &before_entry, sizeof(entry)) == 0);
    CHECK(krnP4SemaphoreCancelDescribe(cancel_worker, cancel_a,
              cancel_a_operation.generation, 2u, &entry,
              &cancel_sysbase) == 0);
    CHECK(memcmp(&entry, &before_entry, sizeof(entry)) == 0);

    poke_before = mock_poke_calls;
    CHECK(krnP4SemaphoreCancelReleaseClaim(cancel_worker, cancel_a,
              cancel_a_operation.generation + 1u, 0u,
              &cancel_sysbase) == 0);
    CHECK(cancel_a_record->wait_scope.cancel_claimed == 1u);
    CHECK(mock_poke_calls == poke_before && !p4_cleanup_wake_pending);
    CHECK(krnP4SemaphoreCancelReleaseClaim(cancel_worker, cancel_a,
              cancel_a_operation.generation, 0u, &cancel_sysbase) == 1);
    CHECK(cancel_a_record->wait_scope.cancel_claimed == 0u);
    CHECK(mock_poke_calls == poke_before && !p4_cleanup_wake_pending);

    /* B is now the first eligible operation after A. Exercise malformed
     * cross-domain snapshots before publishing B's exclusive claim. */
    cancel_test_cross_domain_refusals();

    /* The next address-ordered claim is B even though registry insertion
     * order is C, B, A, legacy, worker. B is live on hart1 while claimed. */
    memset(&snapshot, 0xa5, sizeof(snapshot));
    CHECK(krnP4SemaphoreCancelClaimAfter(cancel_worker, (uintptr_t)cancel_a,
              &snapshot, &cancel_sysbase) == 1);
    CHECK(snapshot.identity == cancel_b);
    CHECK(snapshot.generation == cancel_b_operation.generation);
    CHECK(snapshot.semaphore_index == NULL && snapshot.request_index == NULL &&
          snapshot.list_index == NULL);
    CHECK(snapshot.queued_count == 1u && snapshot.construction_closed == 1u);

    memset(&entry, 0xa5, sizeof(entry));
    CHECK(krnP4SemaphoreCancelDescribe(cancel_worker, cancel_b,
              cancel_b_operation.generation, 0u, &entry,
              &cancel_sysbase) == 1);
    CHECK(entry.semaphore == &cancel_sem_b);
    CHECK(entry.entry_count == 1u);
    CHECK(entry.state == P4_SEMAPHORE_LEDGER_ENTRY_QUEUED);
    CHECK(entry.committed == 0u && entry.running_harts == 2u);

    cancel_test_alias_refusals();

    /* RUN leave while claimed is intentionally quiet; release observes that
     * the captured RUN=2 drained and latches/pokes only after gate unlock. */
    poke_before = mock_poke_calls;
    krnP4TaskRecordLeaveRunning(cancel_b, 1u, &cancel_sysbase);
    CHECK(cancel_b_record->lifetime.running_harts == 0u);
    CHECK(mock_poke_calls == poke_before && !p4_cleanup_wake_pending);
    CHECK(krnP4SemaphoreCancelReleaseClaim(cancel_worker, cancel_b,
              cancel_b_operation.generation, 2u, &cancel_sysbase) == 1);
    CHECK(cancel_b_record->wait_scope.cancel_claimed == 0u);
    CHECK(mock_poke_calls == poke_before + 1u && p4_cleanup_wake_pending);
    cancel_drain_wake();

    /* C is the next address after B. It remains RUN=2 through claim release,
     * so release is quiet; a later RUN leave reserves the durable wake. */
    memset(&snapshot, 0xa5, sizeof(snapshot));
    CHECK(krnP4SemaphoreCancelClaimAfter(cancel_worker, (uintptr_t)cancel_b,
              &snapshot, &cancel_sysbase) == 1);
    CHECK(snapshot.identity == cancel_c);
    CHECK(snapshot.generation == cancel_c_operation.generation);
    CHECK(snapshot.queued_count == 1u && snapshot.construction_closed == 1u);
    poke_before = mock_poke_calls;
    CHECK(krnP4SemaphoreCancelReleaseClaim(cancel_worker, cancel_c,
              cancel_c_operation.generation, 2u, &cancel_sysbase) == 1);
    CHECK(cancel_c_record->lifetime.running_harts == 2u);
    CHECK(mock_poke_calls == poke_before && !p4_cleanup_wake_pending);
    krnP4TaskRecordLeaveRunning(cancel_c, 1u, &cancel_sysbase);
    CHECK(cancel_c_record->lifetime.running_harts == 0u);
    CHECK(mock_poke_calls == poke_before + 1u && p4_cleanup_wake_pending);
    cancel_drain_wake();

    /* Only the lower-address legacy scope remains eligible. Operation-only
     * cursor selection skips it and does not wrap after C. */
    memset(&snapshot, 0xa5, sizeof(snapshot));
    result = krnP4SemaphoreCancelClaimAfter(cancel_worker,
        (uintptr_t)cancel_c, &snapshot, &cancel_sysbase);
    CHECK(result == 0);
    CHECK(snapshot.identity == NULL && snapshot.generation == 0u &&
          snapshot.kind == 0u && snapshot.queued_count == 0u &&
          snapshot.construction_closed == 0u &&
          snapshot.cancel_requested == 0u &&
          snapshot.semaphore_index == NULL && snapshot.request_index == NULL &&
          snapshot.list_index == NULL);
    CHECK(cancel_legacy_record->wait_scope.cancel_requested == 1u);
    CHECK(cancel_legacy_record->wait_scope.cancel_claimed == 0u);
    CHECK(cancel_b_record->wait_scope.cancel_claimed == 0u);
    CHECK(cancel_c_record->wait_scope.cancel_claimed == 0u);
    CHECK(!mock_gate_held && !mock_outer_semaphore_gate_held);
    CHECK(mock_irq_depth == 0u);
    CHECK(mock_disable_calls == mock_enable_calls);
    CHECK(mock_acquire_calls == mock_unlock_calls);
}

int main(void)
{
    long page_size = sysconf(_SC_PAGESIZE);
    void *page;
    unsigned int baseline_checks;
    int baseline_result;

    baseline_result = p4_semaphore_operation_admission_baseline_main();
    baseline_checks = checks;
    if (baseline_result != 0 || baseline_checks != 379u)
    {
        fprintf(stderr,
            "included operation-admission baseline failed: result=%d checks=%u\n",
            baseline_result, baseline_checks);
        return 2;
    }
    if (fflush(stdout) != 0)
        return 2;

    /* The included baseline leaves its fixture quiescent. Reinitialize only
     * the private, test-owned bindings and storage before this fixture. */
    if (mock_irq_depth != 0u || mock_gate_held ||
        mock_outer_semaphore_gate_held || p4_task_pins_prepared != 1)
        return 2;
    p4_task_registry_init(&p4_task_records);
    memset(&p4_task_pin_gate, 0, sizeof(p4_task_pin_gate));
    CHECK(krnP4PublicSpinInit(&p4_task_pin_gate) == P4_PUBLIC_SPIN_OK);
    memset(cancel_records, 0, sizeof(cancel_records));
    memset(cancel_operations, 0, sizeof(cancel_operations));
    memset(cancel_entries, 0, sizeof(cancel_entries));
    memset(&cancel_ledger, 0, sizeof(cancel_ledger));
    memset(test_boot_requests, 0, sizeof(test_boot_requests));
    memset(&cancel_sem_a, 0, sizeof(cancel_sem_a));
    memset(&cancel_sem_b, 0, sizeof(cancel_sem_b));
    p4_cleanup_worker = NULL;
    p4_cleanup_wake_mask = 0u;
    p4_cleanup_wake_pending = false;
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
    CHECK(p4_semaphore_ledger_init(&cancel_ledger, cancel_operations,
              CANCEL_OPERATIONS, cancel_entries, CANCEL_ENTRIES) == 1);
    test_boot_ledger = &cancel_ledger;

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

    cancel_setup(page);
    cancel_test_snapshots_and_release();

    if (munmap(page, (size_t)page_size) != 0)
        return 2;
    if (failures != 0u)
    {
        fprintf(stderr, "P4 semaphore cancellation snapshot binding: %u/%u failed\n",
                failures, checks);
        return 1;
    }
    printf("P4 semaphore cancellation snapshot binding: %u checks passed; "
           "included baseline %u\n", checks - baseline_checks - 2u,
           baseline_checks);
    return 0;
}
