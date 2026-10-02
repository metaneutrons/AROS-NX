/* Actual public bodies + real registry/arena/node bindings. IRQ/AMO,
 * semaphore gate, Forbid and wake delivery remain deterministic host mocks. */
int p4_public_binding_baseline_main(void);
#define main p4_public_binding_baseline_main
#include "../../kernel/tests/p4-semaphore-operation-admission-test.c"
#undef main

#define SIGF_SINGLE UINT32_C(16)
static struct p4_semaphore_ledger binding_ledger;
static struct p4_semaphore_ledger_operation binding_operations[TEST_OP_CAPACITY];
static struct p4_semaphore_ledger_entry binding_entries[TEST_ENTRY_CAPACITY];
static struct p4_task_registry_record binding_records[2];
static struct SignalSemaphore binding_semaphores[1];
static struct ExecBase binding_sysbase;
static struct Task *binding_tasks[2];
static unsigned public_forbid_depth;
static unsigned public_wait_calls;
static unsigned public_clear_calls;
static uint32_t public_signals;
static int public_release_at_clear;

void Forbid(void);
void Permit(void);
uint32_t SetSignal(uint32_t, uint32_t);
uint32_t Wait(uint32_t);
void Exec_P4SemaphoreLock(struct SignalSemaphore *, struct ExecBase *);
void Exec_P4SemaphoreUnlock(struct SignalSemaphore *, struct ExecBase *);

#define P4_SEMAPHORE_PUBLIC_TEST 1
#include "../p4_semaphore_public.c"

int krnP4SelfRemoveCurrentRetired(struct ExecBase *base)
{
    CHECK(base == &binding_sysbase && mock_irq_depth > 0u);
    CHECK(!mock_outer_semaphore_gate_held && !mock_gate_held);
    return 0;
}

void Exec_P4RemoveSelf(struct Task *task, struct ExecBase *base)
{
    (void)task;
    (void)base;
    abort(); /* Retirement injection belongs to the dedicated fixture. */
}

void Forbid(void)
{
    CHECK(!mock_outer_semaphore_gate_held && !mock_gate_held);
    ++public_forbid_depth;
}

void Permit(void)
{
    CHECK(!mock_outer_semaphore_gate_held && !mock_gate_held);
    CHECK(public_forbid_depth > 0u);
    --public_forbid_depth;
}

void Exec_P4SemaphoreLock(struct SignalSemaphore *sem, struct ExecBase *base)
{
    CHECK(base == &binding_sysbase);
    CHECK(sem == &binding_semaphores[0]);
    CHECK(public_forbid_depth >= 4u);
    test_semaphore_gate_lock();
}

void Exec_P4SemaphoreUnlock(struct SignalSemaphore *sem, struct ExecBase *base)
{
    CHECK(base == &binding_sysbase && sem == &binding_semaphores[0]);
    test_semaphore_gate_unlock();
}

uint32_t SetSignal(uint32_t value, uint32_t mask)
{
    uint32_t previous = public_signals;
    CHECK(!mock_outer_semaphore_gate_held && !mock_gate_held);
    CHECK(value == 0u && mask == SIGF_SINGLE);
    CHECK(public_forbid_depth == 4u);
    ++public_clear_calls;
    public_signals = (public_signals & ~mask) | (value & mask);
    if (public_release_at_clear)
    {
        public_release_at_clear = 0;
        Exec_P4ReleaseSemaphore(&binding_semaphores[0], &binding_sysbase);
    }
    return previous;
}

uint32_t Wait(uint32_t mask)
{
    CHECK(mask == SIGF_SINGLE);
    CHECK(!mock_outer_semaphore_gate_held && !mock_gate_held);
    CHECK(public_forbid_depth == 4u);
    CHECK(public_clear_calls == 1u);
    ++public_wait_calls;
    /* First wake is deliberately unrelated: exact completion must refuse.
     * Second wake models another continuation's actual durable Release. */
    if (public_wait_calls == 2u)
        Exec_P4ReleaseSemaphore(&binding_semaphores[0], &binding_sysbase);
    CHECK(public_wait_calls <= 2u);
    return mask;
}

static void public_reset(void *page)
{
    struct SignalSemaphore *sem = &binding_semaphores[0];
    unsigned slot;
    memset(&binding_ledger, 0, sizeof(binding_ledger));
    memset(binding_operations, 0, sizeof(binding_operations));
    memset(binding_entries, 0, sizeof(binding_entries));
    memset(binding_records, 0, sizeof(binding_records));
    memset(binding_semaphores, 0, sizeof(binding_semaphores));
    memset(test_boot_requests, 0, sizeof(test_boot_requests));
    p4_task_registry_init(&p4_task_records);
    test_boot_ledger = &binding_ledger;
    mock_irq_depth = mock_disable_calls = mock_enable_calls = 0u;
    mock_acquire_calls = mock_unlock_calls = mock_fatal_calls = 0u;
    mock_poke_calls = mock_retired_poke_calls = 0u;
    mock_gate_held = mock_outer_semaphore_gate_held = mock_task_lock_held = 0;
    mock_gate_address = &p4_task_pin_gate;
    CHECK(krnP4PublicSpinInit(&p4_task_pin_gate) == P4_PUBLIC_SPIN_OK);
    CHECK(p4_semaphore_ledger_init(&binding_ledger, binding_operations,
        TEST_OP_CAPACITY, binding_entries, TEST_ENTRY_CAPACITY) == 1);
    sem->ss_WaitQueue.mlh_Head =
        (struct MinNode *)(void *)&sem->ss_WaitQueue.mlh_Tail;
    sem->ss_WaitQueue.mlh_TailPred =
        (struct MinNode *)(void *)&sem->ss_WaitQueue.mlh_Head;
    sem->ss_QueueCount = -1;
    binding_tasks[0] = guarded_identity(page, 64u);
    binding_tasks[1] = guarded_identity(page, 128u);
    for (slot = 0u; slot < 2u; ++slot)
    {
        CHECK(krnP4TaskRecordPublish(binding_tasks[slot], &binding_records[slot],
            &binding_sysbase) == 1);
        CHECK(krnP4TaskRecordEnterRunning(binding_tasks[slot], slot,
            &binding_sysbase) == 1);
    }
    public_forbid_depth = 3u;
    public_wait_calls = 0u;
    public_clear_calls = 0u;
    public_signals = SIGF_SINGLE;
    public_release_at_clear = 0;
}

static void public_balanced(void)
{
    CHECK(public_forbid_depth == 3u);
    CHECK(mock_irq_depth == 0u);
    CHECK(!mock_gate_held && !mock_outer_semaphore_gate_held);
    CHECK(mock_disable_calls == mock_enable_calls);
    CHECK(mock_acquire_calls == mock_unlock_calls);
    CHECK(mock_fatal_calls == 0u);
}

static void public_notification_finish(void)
{
    struct p4_semaphore_notification notification;
    p4_task_pins_lock(&binding_sysbase);
    CHECK(p4_task_registry_claim_next_semaphore_notification_locked(
        &p4_task_records, &notification) == 1);
    CHECK(notification.identity == binding_tasks[1]);
    CHECK(notification.count == 1u && notification.generation != 0u);
    CHECK(p4_task_registry_finish_semaphore_notification_locked(
        &p4_task_records, &notification) == 1);
    p4_task_pins_unlock(&binding_sysbase);
    CHECK(binding_records[1].lifetime.notification_pins == 0u);
    CHECK(binding_records[1].semaphore_pending == 0u);
    CHECK(binding_records[1].semaphore_inflight == 0u);
}

static void public_fast_paths(void *page)
{
    public_reset(page);
    Exec_P4ObtainSemaphore(&binding_semaphores[0], binding_tasks[0],
        binding_tasks[0], &binding_sysbase);
    CHECK(binding_semaphores[0].ss_Owner == binding_tasks[0]);
    CHECK(binding_semaphores[0].ss_NestCount == 1);
    Exec_P4ObtainSemaphore(&binding_semaphores[0], binding_tasks[0],
        binding_tasks[0], &binding_sysbase);
    Exec_P4ObtainSemaphore(&binding_semaphores[0], binding_tasks[0],
        NULL, &binding_sysbase);
    CHECK(binding_semaphores[0].ss_Owner == binding_tasks[0]);
    CHECK(binding_semaphores[0].ss_NestCount == 3);
    CHECK(binding_semaphores[0].ss_QueueCount == 2);
    CHECK(public_wait_calls == 0u && public_clear_calls == 0u);
    CHECK(binding_records[0].wait_scope.active == 0u);
    Exec_P4ReleaseSemaphore(&binding_semaphores[0], &binding_sysbase);
    Exec_P4ReleaseSemaphore(&binding_semaphores[0], &binding_sysbase);
    Exec_P4ReleaseSemaphore(&binding_semaphores[0], &binding_sysbase);
    CHECK(binding_semaphores[0].ss_NestCount == 0);
    CHECK(binding_semaphores[0].ss_QueueCount == -1);
    CHECK(binding_semaphores[0].ss_Owner == NULL);
    CHECK(mock_poke_calls == 0u);
    public_balanced();

    public_reset(page);
    Exec_P4ObtainSemaphore(&binding_semaphores[0], binding_tasks[0], NULL,
        &binding_sysbase);
    Exec_P4ObtainSemaphore(&binding_semaphores[0], binding_tasks[1], NULL,
        &binding_sysbase);
    CHECK(binding_semaphores[0].ss_Owner == NULL);
    CHECK(binding_semaphores[0].ss_NestCount == 2);
    CHECK(binding_records[0].wait_scope.active == 0u);
    CHECK(binding_records[1].wait_scope.active == 0u);
    Exec_P4ReleaseSemaphore(&binding_semaphores[0], &binding_sysbase);
    Exec_P4ReleaseSemaphore(&binding_semaphores[0], &binding_sysbase);
    public_balanced();
}

static void public_slow_paths(void *page, int shared, int release_at_clear)
{
    public_reset(page);
    Exec_P4ObtainSemaphore(&binding_semaphores[0], binding_tasks[0],
        binding_tasks[0], &binding_sysbase);
    public_release_at_clear = release_at_clear;
    Exec_P4ObtainSemaphore(&binding_semaphores[0], binding_tasks[1],
        shared ? NULL : binding_tasks[1], &binding_sysbase);
    CHECK(binding_semaphores[0].ss_NestCount == 1);
    CHECK(binding_semaphores[0].ss_QueueCount == 0);
    CHECK(binding_semaphores[0].ss_Owner ==
        (shared ? NULL : binding_tasks[1]));
    CHECK(public_clear_calls == 1u);
    CHECK(public_signals == 0u);
    CHECK(public_wait_calls == (release_at_clear ? 0u : 2u));
    CHECK(binding_records[1].wait_scope.active == 0u);
    CHECK(binding_operations[0].active == 0u);
    CHECK(binding_ledger.active_entries == 0u);
    if (!release_at_clear)
    {
        CHECK(mock_poke_calls == 1u);
        CHECK(binding_records[1].lifetime.notification_pins == 1u);
        public_notification_finish();
    }
    else
        CHECK(binding_records[1].lifetime.notification_pins == 0u);
    Exec_P4ReleaseSemaphore(&binding_semaphores[0], &binding_sysbase);
    CHECK(binding_semaphores[0].ss_NestCount == 0);
    CHECK(binding_semaphores[0].ss_QueueCount == -1);
    public_balanced();
}

int main(void)
{
    long page_size;
    void *page;
    unsigned baseline_checks;
    if (p4_public_binding_baseline_main() != 0)
        return 1;
    baseline_checks = checks;
    page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0)
        return 2;
    page = mmap(NULL, (size_t)page_size, PROT_NONE,
                MAP_PRIVATE | MAP_ANON, -1, 0);
    if (page == MAP_FAILED)
        return 2;
    public_fast_paths(page);
    public_slow_paths(page, 0, 0);
    public_slow_paths(page, 1, 0);
    public_slow_paths(page, 0, 1);
    public_slow_paths(page, 1, 1);
    CHECK(munmap(page, (size_t)page_size) == 0);
    if (failures != 0u)
        return 1;
    printf("P4 public semaphore bindings: %u checks passed; included baseline %u\n",
        checks - baseline_checks, baseline_checks);
    return 0;
}
