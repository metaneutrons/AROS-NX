/* Actual public Obtain body plus actual registry/pins/ledger retirement. IRQ,
 * semaphore gate, scheduler transfer, and unrelated Exec effects are mocked. */
int p4_public_retirement_baseline_main(void);
#define main p4_public_retirement_baseline_main
#include "../../kernel/tests/p4-semaphore-operation-admission-test.c"
#undef main

#include <setjmp.h>

#define P4_SEMAPHORE_PUBLIC_TEST 1
#define SIGF_SINGLE UINT32_C(16)

enum retirement_injection
{
    RETIREMENT_AT_SET_SIGNAL = 1,
    RETIREMENT_AFTER_INCOMPLETE_COMPLETE = 2
};

static struct p4_semaphore_ledger retirement_ledger;
static struct p4_semaphore_ledger_operation retirement_operations[TEST_OP_CAPACITY];
static struct p4_semaphore_ledger_entry retirement_entries[TEST_ENTRY_CAPACITY];
static struct p4_task_registry_record retirement_records[2];
static struct SignalSemaphore retirement_semaphore;
static struct ExecBase retirement_sysbase;
static struct Task *retirement_tasks[2];
static struct Task *retirement_current_task;
static unsigned retirement_forbid_depth;
static unsigned retirement_permit_calls;
static unsigned retirement_wait_calls;
static unsigned retirement_clear_calls;
static unsigned retirement_query_calls;
static unsigned retirement_transfer_calls;
static unsigned retirement_injection_calls;
static uint32_t retirement_signal_bits;
static uint32_t retirement_current_hart;
static int retirement_query_result;
static int retirement_active_injection;
static int retirement_seen;
static int retirement_unexpected_return;
static int retirement_probe_entered;
static jmp_buf retirement_escape;

int p4_retirement_reserve(struct Task *, uint32_t,
    struct p4_task_wait_operation *, struct ExecBase *);
int p4_retirement_configure(struct Task *, uint32_t, uint32_t,
    struct SignalSemaphore *, uint32_t, struct ExecBase *);
int p4_retirement_close(struct Task *, uint32_t, struct ExecBase *);
int p4_retirement_acquire(struct Task *, uint32_t, uint32_t,
    struct SignalSemaphore *, struct ExecBase *);
int p4_retirement_complete(struct Task *, uint32_t, struct ExecBase *);
void Forbid(void);
void Permit(void);
uint32_t SetSignal(uint32_t, uint32_t);
uint32_t Wait(uint32_t);
void Exec_P4SemaphoreLock(struct SignalSemaphore *, struct ExecBase *);
void Exec_P4SemaphoreUnlock(struct SignalSemaphore *, struct ExecBase *);

#define krnP4SemaphoreOperationReserve p4_retirement_reserve
#define krnP4SemaphoreOperationConfigure p4_retirement_configure
#define krnP4SemaphoreOperationClose p4_retirement_close
#define krnP4SemaphoreOperationAcquire p4_retirement_acquire
#define krnP4SemaphoreOperationComplete p4_retirement_complete
#include "../p4_semaphore_public.c"
#undef krnP4SemaphoreOperationReserve
#undef krnP4SemaphoreOperationConfigure
#undef krnP4SemaphoreOperationClose
#undef krnP4SemaphoreOperationAcquire
#undef krnP4SemaphoreOperationComplete

static void retirement_inject_now(void)
{
    int result;
    CHECK(!mock_outer_semaphore_gate_held && !mock_gate_held);
    CHECK(retirement_current_task == retirement_tasks[1]);
    CHECK(mock_irq_depth == 0u);
    Disable();
    mock_task_lock_held = 1;
    result = krnP4TaskRecordRetireForService(retirement_current_task,
        &retirement_sysbase);
    mock_task_lock_held = 0;
    Enable();
    CHECK(result == 1);
    CHECK(retirement_records[1].service_pending == 1u);
    CHECK(retirement_records[1].lifetime.flags ==
          P4_TASK_LIFETIME_RETIRED);
    CHECK(retirement_records[1].lifetime.running_harts == 2u);
    ++retirement_injection_calls;
    retirement_seen = 1;
}

int krnP4SelfRemoveCurrentRetired(struct ExecBase *base)
{
    CHECK(base == &retirement_sysbase);
    CHECK(mock_irq_depth > 0u);
    CHECK(!mock_outer_semaphore_gate_held && !mock_gate_held);
    CHECK(!mock_task_lock_held);
    CHECK(retirement_current_task == retirement_tasks[1]);
    ++retirement_query_calls;
    retirement_query_result = krnP4TaskRecordServiceOwnerRunning(
        retirement_current_task, retirement_current_hart, base);
    retirement_probe_entered = 1;
    return retirement_query_result;
}

void Exec_P4RemoveSelf(struct Task *task, struct ExecBase *base)
{
    CHECK(task == retirement_current_task);
    CHECK(base == &retirement_sysbase);
    CHECK(mock_irq_depth > 0u);
    CHECK(!mock_outer_semaphore_gate_held && !mock_gate_held);
    CHECK(!mock_task_lock_held);
    CHECK(retirement_probe_entered && retirement_query_result == 1);
    CHECK(retirement_seen);
    CHECK(retirement_wait_calls == 0u);
    CHECK(retirement_permit_calls == 0u);
    CHECK(retirement_forbid_depth == 1u);
    ++retirement_transfer_calls;
    longjmp(retirement_escape, 1);
}

void Forbid(void)
{
    CHECK(!mock_outer_semaphore_gate_held && !mock_gate_held);
    ++retirement_forbid_depth;
}

void Permit(void)
{
    CHECK(!mock_outer_semaphore_gate_held && !mock_gate_held);
    CHECK(retirement_forbid_depth == 1u);
    ++retirement_permit_calls;
    --retirement_forbid_depth;
}

void Exec_P4SemaphoreLock(struct SignalSemaphore *sem, struct ExecBase *base)
{
    CHECK(base == &retirement_sysbase && sem == &retirement_semaphore);
    CHECK(retirement_forbid_depth == 1u);
    test_semaphore_gate_lock();
}

void Exec_P4SemaphoreUnlock(struct SignalSemaphore *sem, struct ExecBase *base)
{
    CHECK(base == &retirement_sysbase && sem == &retirement_semaphore);
    test_semaphore_gate_unlock();
}

uint32_t SetSignal(uint32_t value, uint32_t mask)
{
    uint32_t previous = retirement_signal_bits;
    CHECK(value == 0u && mask == SIGF_SINGLE);
    CHECK(!mock_outer_semaphore_gate_held && !mock_gate_held);
    CHECK(retirement_forbid_depth == 1u);
    ++retirement_clear_calls;
    retirement_signal_bits &= ~mask;
    if (retirement_active_injection == RETIREMENT_AT_SET_SIGNAL)
    {
        retirement_active_injection = 0;
        retirement_inject_now();
    }
    return previous;
}

uint32_t Wait(uint32_t mask)
{
    CHECK(mask == SIGF_SINGLE);
    CHECK(!mock_outer_semaphore_gate_held && !mock_gate_held);
    ++retirement_wait_calls;
    longjmp(retirement_escape, 2);
}

int p4_retirement_reserve(struct Task *task, uint32_t count,
    struct p4_task_wait_operation *out, struct ExecBase *base)
{
    return krnP4SemaphoreOperationReserve(task, count, out, base);
}

int p4_retirement_configure(struct Task *task, uint32_t generation,
    uint32_t ordinal, struct SignalSemaphore *sem, uint32_t shared,
    struct ExecBase *base)
{
    return krnP4SemaphoreOperationConfigure(task, generation, ordinal,
        sem, shared, base);
}

int p4_retirement_close(struct Task *task, uint32_t generation,
    struct ExecBase *base)
{
    return krnP4SemaphoreOperationClose(task, generation, base);
}

int p4_retirement_acquire(struct Task *task, uint32_t generation,
    uint32_t ordinal, struct SignalSemaphore *sem, struct ExecBase *base)
{
    return krnP4SemaphoreOperationAcquire(task, generation, ordinal,
        sem, base);
}

int p4_retirement_complete(struct Task *task, uint32_t generation,
    struct ExecBase *base)
{
    int result = krnP4SemaphoreOperationComplete(task, generation, base);
    if (retirement_active_injection ==
            RETIREMENT_AFTER_INCOMPLETE_COMPLETE && result == 0)
    {
        retirement_active_injection = 0;
        retirement_inject_now();
    }
    return result;
}

static void retirement_reset(void *page, int injection)
{
    unsigned slot;

    memset(&retirement_ledger, 0, sizeof(retirement_ledger));
    memset(retirement_operations, 0, sizeof(retirement_operations));
    memset(retirement_entries, 0, sizeof(retirement_entries));
    memset(retirement_records, 0, sizeof(retirement_records));
    memset(&retirement_semaphore, 0, sizeof(retirement_semaphore));
    memset(test_boot_requests, 0, sizeof(test_boot_requests));
    p4_task_registry_init(&p4_task_records);
    test_boot_ledger = &retirement_ledger;
    mock_irq_depth = mock_disable_calls = mock_enable_calls = 0u;
    mock_acquire_calls = mock_unlock_calls = mock_fatal_calls = 0u;
    mock_poke_calls = mock_retired_poke_calls = 0u;
    mock_gate_held = mock_outer_semaphore_gate_held = mock_task_lock_held = 0;
    mock_gate_address = &p4_task_pin_gate;
    CHECK(krnP4PublicSpinInit(&p4_task_pin_gate) == P4_PUBLIC_SPIN_OK);
    CHECK(p4_semaphore_ledger_init(&retirement_ledger,
        retirement_operations, TEST_OP_CAPACITY, retirement_entries,
        TEST_ENTRY_CAPACITY) == 1);
    p4_cleanup_worker = NULL;
    p4_cleanup_wake_mask = 0u;
    p4_cleanup_wake_pending = false;
    p4_cancel_retry.active = 0u;
    p4_cancel_retry.waiting = 0u;
    test_empty_semaphore(&retirement_semaphore);
    retirement_semaphore.ss_NestCount = 1;
    retirement_semaphore.ss_QueueCount = 0;
    retirement_tasks[0] = guarded_identity(page, 64u);
    retirement_tasks[1] = guarded_identity(page, 128u);
    retirement_current_task = retirement_tasks[1];
    retirement_current_hart = 1u;
    for (slot = 0u; slot < 2u; ++slot)
    {
        CHECK(krnP4TaskRecordPublish(retirement_tasks[slot],
            &retirement_records[slot], &retirement_sysbase) == 1);
        CHECK(krnP4TaskRecordEnterRunning(retirement_tasks[slot], slot,
            &retirement_sysbase) == 1);
    }
    retirement_semaphore.ss_Owner = retirement_tasks[0];
    retirement_forbid_depth = 0u;
    retirement_permit_calls = 0u;
    retirement_wait_calls = 0u;
    retirement_clear_calls = 0u;
    retirement_query_calls = 0u;
    retirement_transfer_calls = 0u;
    retirement_injection_calls = 0u;
    retirement_signal_bits = SIGF_SINGLE;
    retirement_query_result = 0;
    retirement_active_injection = injection;
    retirement_seen = 0;
    retirement_unexpected_return = 0;
    retirement_probe_entered = 0;
}

static void retirement_run_case(void *page, int injection)
{
    int jump_result;
    retirement_reset(page, injection);
    jump_result = setjmp(retirement_escape);
    if (jump_result == 0)
    {
        Exec_P4ObtainSemaphore(&retirement_semaphore, retirement_current_task,
            retirement_current_task, &retirement_sysbase);
        retirement_unexpected_return = 1;
        longjmp(retirement_escape, 3);
    }

    CHECK(jump_result == 1);
    CHECK(!retirement_unexpected_return);
    CHECK(retirement_seen);
    CHECK(retirement_injection_calls == 1u);
    CHECK(retirement_query_calls == 1u);
    CHECK(retirement_query_result == 1);
    CHECK(retirement_transfer_calls == 1u);
    CHECK(retirement_forbid_depth == 1u);
    CHECK(retirement_permit_calls == 0u);
    CHECK(retirement_wait_calls == 0u);
    CHECK(retirement_clear_calls == 1u);
    CHECK(retirement_records[1].lifetime.running_harts == 2u);
    CHECK(retirement_records[1].service_pending == 1u);
    CHECK(retirement_records[1].lifetime.notification_pins >= 1u);
    CHECK(!mock_outer_semaphore_gate_held && !mock_gate_held);
    CHECK(!mock_task_lock_held);
    CHECK(mock_irq_depth == 1u); /* no-return self-removal keeps MIE masked */
    CHECK(mock_disable_calls == mock_enable_calls + 1u);
    CHECK(mock_acquire_calls == mock_unlock_calls);
    CHECK(mock_fatal_calls == 0u);
    if (injection == RETIREMENT_AT_SET_SIGNAL)
    {
        CHECK(retirement_records[1].wait_scope.active == 1u);
        CHECK(retirement_records[1].wait_scope.cancel_requested == 1u);
        CHECK(retirement_records[1].wait_scope.queued_count == 0u);
    }
    else
    {
        CHECK(retirement_records[1].wait_scope.active == 1u);
        CHECK(retirement_records[1].wait_scope.cancel_requested == 1u);
        CHECK(retirement_records[1].wait_scope.queued_count == 1u);
        CHECK(retirement_semaphore.ss_QueueCount == 1);
        CHECK(retirement_semaphore.ss_NestCount == 1);
    }
}

int main(void)
{
    long page_size;
    void *page;
    unsigned baseline_checks;

    if (p4_public_retirement_baseline_main() != 0)
        return 1;
    baseline_checks = checks;
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

    retirement_run_case(page, RETIREMENT_AT_SET_SIGNAL);
    retirement_run_case(page, RETIREMENT_AFTER_INCOMPLETE_COMPLETE);

    CHECK(munmap(page, (size_t)page_size) == 0);
    if (failures != 0u)
        return 1;
    printf("P4 public retirement boundaries: %u checks passed; included baseline %u\n",
        checks - baseline_checks, baseline_checks);
    return 0;
}
