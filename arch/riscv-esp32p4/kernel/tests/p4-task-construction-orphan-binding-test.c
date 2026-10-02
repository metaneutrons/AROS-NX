/* Actual-source host coverage for the protected orphan-construction wrapper. */
#define main p4_operation_admission_fixture_main
#include "p4-semaphore-operation-admission-test.c"
#undef main

#include <signal.h>
#include <sys/wait.h>

/* The admission fixture includes the actual registry/lifetime/pin and
 * semaphore leaves. These actual dependencies are linked by the pin source. */
#include "../../exec/p4_remove_bank.c"
#include "../../exec/p4_remove_completion.c"

static unsigned int observed_resource_reservations;
static unsigned int observed_retry_events;
static unsigned int observed_pokes;
static int observed_poke_gate_held;
static unsigned int observed_poke_irq_depth;
static int trace_construction;
static struct ExecBase test_sysbase;
static struct p4_semaphore_ledger boot_ledger;
static struct p4_semaphore_ledger_operation boot_operations[TEST_OP_CAPACITY];
static struct p4_semaphore_ledger_entry boot_entries[TEST_ENTRY_CAPACITY];

void __cyg_profile_func_enter(void *function, void *caller)
    __attribute__((no_instrument_function));
void __cyg_profile_func_exit(void *function, void *caller)
    __attribute__((no_instrument_function));

void __cyg_profile_func_enter(void *function, void *caller)
{
    (void)caller;
    if (!trace_construction)
        return;
    if ((uintptr_t)function ==
        (uintptr_t)&p4_task_pins_reserve_resource_wake_locked)
        ++observed_resource_reservations;
    if ((uintptr_t)function == (uintptr_t)&p4_semaphore_cancel_retry_event)
        ++observed_retry_events;
    if ((uintptr_t)function == (uintptr_t)&krnP4CleanupPoke)
    {
        ++observed_pokes;
        observed_poke_gate_held = mock_gate_held;
        observed_poke_irq_depth = mock_irq_depth;
    }
}

void __cyg_profile_func_exit(void *function, void *caller)
{
    (void)function;
    (void)caller;
}

static struct p4_task_registry_record orphan_worker_record;
static struct p4_task_registry_record orphan_creator_record;
static struct p4_task_registry_record orphan_target_record;
static struct p4_task_registry_record pin_creator_record;
static struct p4_task_registry_record pin_target_record;
static struct Task *fatal_call_worker;
static struct Task *fatal_call_target;
static struct Task *fatal_call_creator;
static uint32_t fatal_call_generation;
static unsigned int fatal_call_unlocks;
static unsigned int fatal_call_enables;
static unsigned int fatal_call_acquires;
static unsigned int fatal_call_pokes;
static unsigned int fatal_call_reservations;
static unsigned int fatal_call_retry_events;

static void orphan_fatal_signal(int signal_number)
    __attribute__((no_instrument_function));

static void orphan_fatal_signal(int signal_number)
{
    if (signal_number == SIGABRT && mock_gate_held && mock_irq_depth == 2u &&
        mock_unlock_calls == fatal_call_unlocks &&
        mock_enable_calls == fatal_call_enables &&
        mock_acquire_calls == fatal_call_acquires + 1u &&
        mock_poke_calls == fatal_call_pokes &&
        observed_resource_reservations == fatal_call_reservations &&
        observed_retry_events == fatal_call_retry_events)
        _exit(42);
    _exit(70);
}

static void prepare_orphan_fixture(void *page)
{
    struct Task *worker = guarded_identity(page, 64u);
    struct ExecBase *base = &test_sysbase;

    memset(&boot_ledger, 0, sizeof(boot_ledger));
    memset(boot_operations, 0, sizeof(boot_operations));
    memset(boot_entries, 0, sizeof(boot_entries));
    CHECK(p4_semaphore_ledger_init(&boot_ledger, boot_operations, 2u,
              boot_entries, 4u) == 1);
    test_boot_ledger = &boot_ledger;
    mock_irq_depth = 1u; /* PinsPrepare's boot contract is IRQs disabled. */
    CHECK(krnP4TaskPinsPrepare() == 1);
    CHECK(krnP4TaskRecordPublish(worker, &orphan_worker_record, base) == 1);
    CHECK(krnP4TaskRecordEnterRunning(worker, 0u, base) == 1);
    CHECK(krnP4ProtectCleanupWorker(worker, base) == 1);
    CHECK(krnP4ArmCleanupWake(worker, UINT32_C(0x20), base) == 1);
    CHECK(mock_irq_depth == 1u && !mock_gate_held);
    CHECK(mock_poke_calls == 0u);
}

static void orphan_fatal_child(int which)
{
    struct sigaction action;
    struct Task *worker = fatal_call_worker;

    memset(&action, 0, sizeof(action));
    action.sa_handler = orphan_fatal_signal;
    sigemptyset(&action.sa_mask);
    (void)sigaction(SIGABRT, &action, NULL);
    (void)freopen("/dev/null", "w", stderr);
    if (which == 0)
        (void)krnP4TaskConstructionOrphanFinish(
            worker, fatal_call_target, fatal_call_creator,
            fatal_call_generation, &test_sysbase);
    else if (which == 1)
        (void)krnP4TaskConstructionOrphanFinish(
            worker, fatal_call_target, fatal_call_creator,
            fatal_call_generation, &test_sysbase);
    else if (which == 2)
        (void)krnP4TaskConstructionOrphanFinish(
            worker, fatal_call_creator, fatal_call_creator,
            fatal_call_generation, &test_sysbase);
    _exit(71);
}

static void expect_orphan_fatal(int which)
{
    pid_t child;
    int status = 0;

    fatal_call_unlocks = mock_unlock_calls;
    fatal_call_enables = mock_enable_calls;
    fatal_call_acquires = mock_acquire_calls;
    fatal_call_pokes = mock_poke_calls;
    fatal_call_reservations = observed_resource_reservations;
    fatal_call_retry_events = observed_retry_events;
    child = fork();
    CHECK(child >= 0);
    if (child == 0)
        orphan_fatal_child(which);
    if (child > 0)
    {
        CHECK(waitpid(child, &status, 0) == child);
        CHECK(WIFEXITED(status));
        if (WIFEXITED(status))
            CHECK(WEXITSTATUS(status) == 42);
    }
}

static void test_ready_pair_success(void *page)
{
    struct Task *worker = guarded_identity(page, 64u);
    struct Task *creator = guarded_identity(page, 128u);
    struct Task *target = guarded_identity(page, 192u);
    struct p4_task_registry_record creator_before;
    struct p4_task_registry_record target_before;
    struct p4_cleanup_wake wake;
    uint32_t generation = 0u;
    unsigned int poke_before;

    CHECK(krnP4TaskRecordPublish(creator, &orphan_creator_record,
              &test_sysbase) == 1);
    CHECK(krnP4TaskRecordEnterRunning(creator, 1u, &test_sysbase) == 1);
    CHECK(krnP4TaskConstructionPublish(target, &orphan_target_record,
              creator, 1u, &generation, &test_sysbase) == 1);

    /* A still-live pair is a non-mutating refusal. */
    memcpy(&creator_before, &orphan_creator_record, sizeof(creator_before));
    memcpy(&target_before, &orphan_target_record, sizeof(target_before));
    poke_before = mock_poke_calls;
    observed_resource_reservations = 0u;
    observed_retry_events = 0u;
    CHECK(krnP4TaskConstructionOrphanFinish(worker, target, creator,
              generation, &test_sysbase) == 0);
    CHECK(memcmp(&creator_before, &orphan_creator_record,
                 sizeof(creator_before)) == 0);
    CHECK(memcmp(&target_before, &orphan_target_record,
                 sizeof(target_before)) == 0);
    CHECK(mock_poke_calls == poke_before);
    CHECK(observed_resource_reservations == 0u);
    CHECK(observed_retry_events == 0u);

    /* Worker authentication rejects wrong identity, unarmed worker, absent
     * RUN owner, and malformed target/creator alias before releasing the gate. */
    fatal_call_worker = guarded_identity(page, 256u);
    fatal_call_target = target;
    fatal_call_creator = creator;
    fatal_call_generation = generation;
    expect_orphan_fatal(0);

    fatal_call_worker = worker;
    p4_cleanup_wake_mask = 0u;
    expect_orphan_fatal(1);
    p4_cleanup_wake_mask = UINT32_C(0x20);

    krnP4TaskRecordLeaveRunning(worker, 0u, &test_sysbase);
    expect_orphan_fatal(1);
    CHECK(krnP4TaskRecordEnterRunning(worker, 0u, &test_sysbase) == 1);

    expect_orphan_fatal(2);
    CHECK(mock_irq_depth == 1u && !mock_gate_held);

    /* Drain both targets and their RUN ownership; the helper's TaskRes/queue
     * cancellation and final target-access prerequisites remain caller-owned. */
    p4_task_pins_lock(&test_sysbase);
    CHECK(p4_task_registry_retire_with_ledger_locked(&p4_task_records,
              &boot_ledger, target, 1u) == 1);
    CHECK(p4_task_registry_retire_with_ledger_locked(&p4_task_records,
              &boot_ledger, creator, 1u) == 1);
    p4_task_pins_unlock(&test_sysbase);
    /* Both identities are retired, but creator RUN still protects its live
     * context. Orphan release must remain a byte-for-byte refusal until that
     * owner has switched away. */
    memcpy(&creator_before, &orphan_creator_record, sizeof(creator_before));
    memcpy(&target_before, &orphan_target_record, sizeof(target_before));
    poke_before = mock_poke_calls;
    observed_resource_reservations = 0u;
    observed_retry_events = 0u;
    CHECK(krnP4TaskConstructionOrphanFinish(worker, target, creator,
              generation, &test_sysbase) == 0);
    CHECK(memcmp(&creator_before, &orphan_creator_record,
                 sizeof(creator_before)) == 0);
    CHECK(memcmp(&target_before, &orphan_target_record,
                 sizeof(target_before)) == 0);
    CHECK(mock_poke_calls == poke_before);
    CHECK(observed_resource_reservations == 0u);
    CHECK(observed_retry_events == 0u);
    krnP4TaskRecordLeaveRunning(creator, 1u, &test_sysbase);
    CHECK(orphan_target_record.service_pending == 1u);
    CHECK(orphan_creator_record.service_pending == 1u);
    CHECK(orphan_target_record.service_queued == 0u);
    CHECK(orphan_creator_record.service_queued == 0u);
    CHECK(orphan_target_record.lifetime.running_harts == 0u);
    CHECK(orphan_creator_record.lifetime.running_harts == 0u);

    memcpy(&creator_before, &orphan_creator_record, sizeof(creator_before));
    memcpy(&target_before, &orphan_target_record, sizeof(target_before));
    poke_before = mock_poke_calls;
    CHECK(krnP4TaskConstructionOrphanFinish(worker, target, creator,
              generation + 1u, &test_sysbase) == 0);
    CHECK(memcmp(&creator_before, &orphan_creator_record,
                 sizeof(creator_before)) == 0);
    CHECK(memcmp(&target_before, &orphan_target_record,
                 sizeof(target_before)) == 0);
    CHECK(mock_poke_calls == poke_before);

    observed_resource_reservations = 0u;
    observed_retry_events = 0u;
    observed_pokes = 0u;
    trace_construction = 1;
    CHECK(krnP4TaskConstructionOrphanFinish(worker, target, creator,
              generation, &test_sysbase) == 1);
    trace_construction = 0;
    CHECK(observed_resource_reservations == 2u);
    CHECK(observed_retry_events == 2u);
    CHECK(observed_pokes == 1u);
    CHECK(observed_poke_gate_held == 0);
    CHECK(observed_poke_irq_depth == 1u);
    CHECK(mock_poke_calls == poke_before + 1u);
    CHECK(orphan_target_record.construction_creator == NULL);
    CHECK(orphan_creator_record.construction_children == 0u);
    CHECK(orphan_target_record.lifetime.notification_pins == 1u);
    CHECK(orphan_creator_record.lifetime.notification_pins == 1u);
    CHECK(orphan_target_record.service_queued == 1u);
    CHECK(orphan_creator_record.service_queued == 1u);
    CHECK(krnP4TakeCleanupWake(&wake, &test_sysbase) == 1);
    CHECK(wake.worker == worker && wake.mask == UINT32_C(0x20));

    /* Duplicate/stale finish is refused and cannot reserve another wake. */
    poke_before = mock_poke_calls;
    CHECK(krnP4TaskConstructionOrphanFinish(worker, target, creator,
              generation, &test_sysbase) == 0);
    CHECK(mock_poke_calls == poke_before);
}

static void test_ordinary_pins_defer_readiness(void *page)
{
    struct Task *worker = guarded_identity(page, 64u);
    struct Task *creator = guarded_identity(page, 320u);
    struct Task *target = guarded_identity(page, 384u);
    struct p4_cleanup_wake wake;
    uint32_t generation = 0u;
    unsigned int poke_before;

    CHECK(krnP4TaskRecordPublish(creator, &pin_creator_record,
              &test_sysbase) == 1);
    CHECK(krnP4TaskRecordEnterRunning(creator, 1u, &test_sysbase) == 1);
    CHECK(krnP4TaskConstructionPublish(target, &pin_target_record, creator,
              1u, &generation, &test_sysbase) == 1);
    CHECK(krnP4SignalTaskPin(target, &test_sysbase) == 1);
    CHECK(krnP4SignalTaskPin(creator, &test_sysbase) == 1);
    krnP4SemaphoreCancelRetryBegin(worker, &test_sysbase);
    CHECK(p4_cancel_retry.active == 1u && p4_cancel_retry.waiting == 1u);

    p4_task_pins_lock(&test_sysbase);
    CHECK(p4_task_registry_retire_with_ledger_locked(&p4_task_records,
              &boot_ledger, target, 1u) == 1);
    CHECK(p4_task_registry_retire_with_ledger_locked(&p4_task_records,
              &boot_ledger, creator, 1u) == 1);
    p4_task_pins_unlock(&test_sysbase);
    krnP4TaskRecordLeaveRunning(creator, 1u, &test_sysbase);

    observed_resource_reservations = 0u;
    observed_retry_events = 0u;
    observed_pokes = 0u;
    poke_before = mock_poke_calls;
    trace_construction = 1;
    CHECK(krnP4TaskConstructionOrphanFinish(worker, target, creator,
              generation, &test_sysbase) == 1);
    trace_construction = 0;
    CHECK(observed_resource_reservations == 2u);
    CHECK(observed_retry_events == 2u);
    CHECK(observed_pokes == 1u);
    CHECK(mock_poke_calls == poke_before + 1u);
    CHECK(pin_target_record.lifetime.notification_pins == 2u);
    CHECK(pin_creator_record.lifetime.notification_pins == 2u);
    CHECK(pin_target_record.service_queued == 0u);
    CHECK(pin_creator_record.service_queued == 0u);
    CHECK(krnP4TakeCleanupWake(&wake, &test_sysbase) == 1);

    /* The last ordinary references make each retained service reference
     * independently ready and preserve a fresh cleanup wake. */
    poke_before = mock_poke_calls;
    krnP4SignalTaskUnpin(target, &test_sysbase);
    CHECK(pin_target_record.service_queued == 1u);
    CHECK(pin_creator_record.service_queued == 0u);
    CHECK(mock_poke_calls == poke_before + 1u);
    krnP4SignalTaskUnpin(creator, &test_sysbase);
    CHECK(pin_creator_record.service_queued == 1u);
    CHECK(mock_poke_calls == poke_before + 2u);
    CHECK(krnP4TakeCleanupWake(&wake, &test_sysbase) == 1);
    CHECK(wake.worker == worker && wake.mask == UINT32_C(0x20));
    krnP4SemaphoreCancelRetryEnd(worker, 1u, &test_sysbase);
    CHECK(p4_cancel_retry.active == 0u && p4_cancel_retry.waiting == 1u);
}

int main(void)
{
    long page_size = sysconf(_SC_PAGESIZE);
    void *page;

    if (page_size <= 0)
    {
        fputs("could not read page size\n", stderr);
        return EXIT_FAILURE;
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
        perror("mmap PROT_NONE identities");
        return EXIT_FAILURE;
    }

    prepare_orphan_fixture(page);
    test_ready_pair_success(page);
    test_ordinary_pins_defer_readiness(page);
    CHECK(mock_irq_depth == 1u && !mock_gate_held);
    CHECK(mock_fatal_calls == 0u);
    CHECK(mock_acquire_calls == mock_unlock_calls);
    CHECK(mock_disable_calls == mock_enable_calls);
    CHECK(failures == 0u);
    if (failures != 0u)
    {
        fprintf(stderr, "%u of %u orphan-binding checks failed\n",
                failures, checks);
        return EXIT_FAILURE;
    }
    printf("P4 orphan construction binding: %u checks passed\n", checks);
    (void)munmap(page, (size_t)page_size);
    return EXIT_SUCCESS;
}
