/* Actual-source test of the SMP Task-construction pin binding. */
#define main p4_operation_admission_fixture_main
#include "p4-semaphore-operation-admission-test.c"
#undef main

/* The admission fixture includes the real registry, lifetime, pin, semaphore,
 * cancellation and retry sources. These two leaves are dependencies of the
 * actual pin binding and are included here, as in the construction leaf test. */
#include "../../exec/p4_remove_bank.c"
#include "../../exec/p4_remove_completion.c"

#include <signal.h>
#include <sys/wait.h>

static unsigned int observed_resource_reservations;
static unsigned int observed_retry_events;
static unsigned int observed_pokes;
static int observed_poke_gate_held;
static unsigned int observed_poke_irq_depth;
static int trace_construction;

/* The production retry event is a read-only leaf. Instrumented host builds
 * count the two resource-reservation calls without replacing the implementation.
 */
void __cyg_profile_func_enter(void *function, void *caller)
    __attribute__((no_instrument_function));
void __cyg_profile_func_exit(void *function, void *caller)
    __attribute__((no_instrument_function));

void __cyg_profile_func_enter(void *function, void *caller)
{
    (void)caller;
    if (trace_construction)
    {
        if ((uintptr_t)function ==
            (uintptr_t)&p4_task_pins_reserve_resource_wake_locked)
            ++observed_resource_reservations;
        if ((uintptr_t)function ==
            (uintptr_t)&p4_semaphore_cancel_retry_event)
            ++observed_retry_events;
        if ((uintptr_t)function == (uintptr_t)&krnP4CleanupPoke)
        {
            ++observed_pokes;
            observed_poke_gate_held = mock_gate_held;
            observed_poke_irq_depth = mock_irq_depth;
        }
    }
}

void __cyg_profile_func_exit(void *function, void *caller)
{
    (void)function;
    (void)caller;
}

static struct p4_task_registry_record creator_record;
static struct p4_task_registry_record child_record;
static struct p4_task_registry_record second_creator_record;
static struct p4_task_registry_record second_child_record;
static struct p4_task_registry_record malformed_creator_record;
static struct p4_semaphore_ledger boot_ledger;
static struct p4_semaphore_ledger_operation boot_operations[2];
static struct p4_semaphore_ledger_entry boot_entries[4];
static struct ExecBase test_sysbase;

static int publish_identity(struct Task *task,
                            struct p4_task_registry_record *record)
{
    return krnP4TaskRecordPublish(task, record, &test_sysbase);
}

static int enter_identity(struct Task *task, uint32_t hart)
{
    return krnP4TaskRecordEnterRunning(task, (unsigned int)hart,
                                       &test_sysbase);
}

static int retire_identity(struct Task *task)
{
    return krnP4TaskRecordRetireForService(task, &test_sysbase);
}

static void test_publish_and_finish(void *page)
{
    struct Task *creator = guarded_identity(page, 64u);
    struct Task *child = guarded_identity(page, 128u);
    struct p4_task_registry_record creator_before;
    struct p4_task_registry_record child_before;
    uint32_t generation = UINT32_C(0xdeadbeef);
    unsigned int disables_before;
    unsigned int enables_before;
    unsigned int acquires_before;
    unsigned int unlocks_before;
    unsigned int pokes_before;
    unsigned int retries_before;
    unsigned int reservations_before;
    int result;

    CHECK(publish_identity(creator, &creator_record) == 1);
    CHECK(enter_identity(creator, 0u) == 1);
    CHECK(mock_irq_depth == 1u && !mock_gate_held);

    disables_before = mock_disable_calls;
    enables_before = mock_enable_calls;
    acquires_before = mock_acquire_calls;
    unlocks_before = mock_unlock_calls;
    CHECK(krnP4TaskConstructionPublish(child, &child_record, creator, 0u,
              &generation, &test_sysbase) == 1);
    CHECK(generation == 1u);
    CHECK(mock_disable_calls == disables_before + 1u);
    CHECK(mock_enable_calls == enables_before + 1u);
    CHECK(mock_acquire_calls == acquires_before + 1u);
    CHECK(mock_unlock_calls == unlocks_before + 1u);
    CHECK(mock_irq_depth == 1u && !mock_gate_held);
    CHECK(child_record.identity == child);
    CHECK(child_record.construction_creator == creator);
    CHECK(child_record.construction_generation == generation);
    CHECK(child_record.lifetime.notification_pins == 1u);
    CHECK(creator_record.construction_children == 1u);
    CHECK(creator_record.lifetime.notification_pins == 1u);
    CHECK(p4_task_records.count == 2u);

    /* Wrong hart, stale generation, and missing identity are ordinary
     * refusals and preserve every byte in the registry and records. */
    memcpy(&creator_before, &creator_record, sizeof(creator_before));
    memcpy(&child_before, &child_record, sizeof(child_before));
    CHECK(krnP4TaskConstructionFinish(child, creator, generation, 1u,
              &test_sysbase) == 0);
    CHECK(memcmp(&creator_record, &creator_before, sizeof(creator_before)) == 0);
    CHECK(memcmp(&child_record, &child_before, sizeof(child_before)) == 0);
    CHECK(krnP4TaskConstructionFinish(child, creator, generation + 1u, 0u,
              &test_sysbase) == 0);
    CHECK(memcmp(&creator_record, &creator_before, sizeof(creator_before)) == 0);
    CHECK(memcmp(&child_record, &child_before, sizeof(child_before)) == 0);
    CHECK(krnP4TaskConstructionFinish(guarded_identity(page, 192u), creator,
              generation, 0u, &test_sysbase) == 0);
    CHECK(memcmp(&creator_record, &creator_before, sizeof(creator_before)) == 0);
    CHECK(memcmp(&child_record, &child_before, sizeof(child_before)) == 0);
    CHECK(mock_irq_depth == 1u && !mock_gate_held);

    /* Retiring the target while its construction pin is outstanding cannot
     * queue it. Finish drops both pins atomically, makes only that target
     * service-ready, and pokes after the gate is released. */
    CHECK(retire_identity(child) == 1);
    CHECK(child_record.service_pending == 1u);
    CHECK(child_record.service_queued == 0u);
    CHECK(child_record.lifetime.notification_pins == 2u);
    CHECK(creator_record.construction_children == 1u);

    /* Keep retry interest live so instrumentation verifies both recipient
     * wake reservations execute even though the first one already requests a
     * poke. This leaf changes no Task state and is the production function. */
    CHECK(p4_semaphore_cancel_retry_begin(&p4_cancel_retry) == 1);
    pokes_before = mock_poke_calls;
    observed_pokes = 0u;
    observed_resource_reservations = 0u;
    observed_retry_events = 0u;
    reservations_before = observed_resource_reservations;
    retries_before = observed_retry_events;
    trace_construction = 1;
    result = krnP4TaskConstructionFinish(child, creator, generation, 0u,
                                         &test_sysbase);
    trace_construction = 0;
    CHECK(result == 1);
    CHECK(observed_resource_reservations - reservations_before == 2u);
    CHECK(observed_retry_events - retries_before == 2u);
    CHECK(observed_pokes == 1u);
    CHECK(mock_poke_calls == pokes_before + 1u);
    CHECK(observed_poke_gate_held == 0);
    CHECK(observed_poke_irq_depth == 1u);
    CHECK(!mock_gate_held && mock_irq_depth == 1u);
    CHECK(child_record.construction_creator == NULL);
    CHECK(child_record.lifetime.notification_pins == 1u);
    CHECK(child_record.service_queued == 1u);
    CHECK(creator_record.construction_children == 0u);
    CHECK(creator_record.lifetime.notification_pins == 0u);
    CHECK(krnP4TaskConstructionFinish(child, creator, generation, 0u,
              &test_sysbase) == 0);
    CHECK(mock_poke_calls == pokes_before + 1u);
}

static void test_retired_creator_refusal(void *page)
{
    struct Task *creator = guarded_identity(page, 256u);
    struct Task *child = guarded_identity(page, 320u);
    struct p4_task_registry_record creator_before;
    struct p4_task_registry_record child_before;
    uint32_t generation = 0u;
    unsigned int pokes_before;

    CHECK(publish_identity(creator, &second_creator_record) == 1);
    CHECK(enter_identity(creator, 1u) == 1);
    CHECK(krnP4TaskConstructionPublish(child, &second_child_record, creator,
              1u, &generation, &test_sysbase) == 1);
    CHECK(generation == 2u);
    /* Put the creator into the retired state under the real pin gate using
     * the registry transition directly. The fixture does not model the
     * caller's outer Task lock required by RetireForService's owner poke. */
    p4_task_pins_lock(&test_sysbase);
    CHECK(p4_task_registry_retire_with_ledger_locked(&p4_task_records,
              &boot_ledger, creator, 1u) == 1);
    p4_task_pins_unlock(&test_sysbase);
    krnP4TaskRecordLeaveRunning(creator, 1u, &test_sysbase);
    CHECK(creator_record.construction_children == 0u);
    CHECK(second_creator_record.construction_children == 1u);
    CHECK(second_creator_record.service_pending == 1u);
    CHECK(second_creator_record.service_queued == 0u);
    CHECK(second_creator_record.lifetime.notification_pins == 2u);
    memcpy(&creator_before, &second_creator_record, sizeof(creator_before));
    memcpy(&child_before, &second_child_record, sizeof(child_before));
    pokes_before = mock_poke_calls;
    CHECK(krnP4TaskConstructionFinish(child, creator, generation, 1u,
              &test_sysbase) == 0);
    CHECK(memcmp(&second_creator_record, &creator_before,
                 sizeof(creator_before)) == 0);
    CHECK(memcmp(&second_child_record, &child_before,
                 sizeof(child_before)) == 0);
    CHECK(mock_poke_calls == pokes_before);
    CHECK(!mock_gate_held && mock_irq_depth == 1u);
}

static struct p4_task_registry_record fatal_record;
static struct Task *fatal_identity;
static unsigned int fatal_unlock_baseline;
static unsigned int fatal_enable_baseline;

static void fatal_signal_check(int signal_number)
{
    if (signal_number == SIGABRT && mock_gate_held && mock_irq_depth == 2u &&
        mock_unlock_calls == fatal_unlock_baseline &&
        mock_enable_calls == fatal_enable_baseline)
        _exit(42);
    _exit(70);
}

static void test_malformed_refuses_under_gate(void *page)
{
    struct Task *creator = guarded_identity(page, 384u);
    struct p4_task_registry_record *creator_rec = &malformed_creator_record;
    uint32_t generation = 0u;
    pid_t child_pid;
    int status;

    CHECK(publish_identity(creator, creator_rec) == 1);
    CHECK(enter_identity(creator, 0u) == 1);
    fatal_identity = creator;
    fatal_unlock_baseline = mock_unlock_calls;
    fatal_enable_baseline = mock_enable_calls;
    child_pid = fork();
    CHECK(child_pid >= 0);
    if (child_pid == 0)
    {
        struct sigaction action;
        memset(&action, 0, sizeof(action));
        action.sa_handler = fatal_signal_check;
        sigemptyset(&action.sa_mask);
        (void)sigaction(SIGABRT, &action, NULL);
        /* identity==creator is structurally malformed. The wrapper must
         * fail closed while still masked and holding the canonical gate. */
        (void)krnP4TaskConstructionPublish(fatal_identity, &fatal_record,
            fatal_identity, 0u, &generation, &test_sysbase);
        _exit(71);
    }
    if (child_pid > 0)
    {
        CHECK(waitpid(child_pid, &status, 0) == child_pid);
        CHECK(WIFEXITED(status));
        if (WIFEXITED(status))
            CHECK(WEXITSTATUS(status) == 42);
    }
    CHECK(mock_unlock_calls == fatal_unlock_baseline);
    CHECK(mock_enable_calls == fatal_enable_baseline);
    CHECK(!mock_gate_held && mock_irq_depth == 1u);
}

int main(void)
{
    long page_size = sysconf(_SC_PAGESIZE);
    void *page;

    if (page_size <= 0)
    {
        fputs("could not get page size\n", stderr);
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
        perror("mmap PROT_NONE task identities");
        return EXIT_FAILURE;
    }

    memset(&boot_ledger, 0, sizeof(boot_ledger));
    memset(boot_operations, 0, sizeof(boot_operations));
    memset(boot_entries, 0, sizeof(boot_entries));
    CHECK(p4_semaphore_ledger_init(&boot_ledger, boot_operations, 2u,
              boot_entries, 4u) == 1);
    test_boot_ledger = &boot_ledger;
    mock_irq_depth = 1u; /* Boot Prepare is entered with local IRQs masked. */
    CHECK(krnP4TaskPinsPrepare() == 1);
    CHECK(mock_irq_depth == 1u && !mock_gate_held);

    test_publish_and_finish(page);
    test_retired_creator_refusal(page);
    test_malformed_refuses_under_gate(page);

    CHECK(mock_irq_depth == 1u && !mock_gate_held);
    CHECK(mock_fatal_calls == 0u);
    CHECK(mock_acquire_calls == mock_unlock_calls);
    CHECK(mock_disable_calls == mock_enable_calls);
    CHECK(failures == 0u);
    if (failures != 0u)
    {
        fprintf(stderr, "%u of %u construction-binding checks failed\n",
                failures, checks);
        return EXIT_FAILURE;
    }
    printf("P4 Task-construction binding: %u checks passed\n", checks);
    (void)munmap(page, (size_t)page_size);
    return EXIT_SUCCESS;
}
