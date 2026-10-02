/* Actual-source tests for the Configure/Acquire gate bindings. */
int p4_semaphore_operation_admission_baseline_main(void);
#define main p4_semaphore_operation_admission_baseline_main
#include "p4-semaphore-operation-admission-test.c"
#undef main

#include <signal.h>
#include <sys/resource.h>
#include <sys/wait.h>

#define BINDING_RECORD_CAPACITY 2u

static struct p4_semaphore_ledger binding_ledger;
static struct p4_semaphore_ledger_operation
    binding_operations[TEST_OP_CAPACITY];
static struct p4_semaphore_ledger_entry binding_entries[TEST_ENTRY_CAPACITY];
static struct p4_task_registry_record binding_records[BINDING_RECORD_CAPACITY];
static struct SignalSemaphore binding_semaphores[2];
static struct p4_task_wait_operation binding_operations_out[2];
static struct ExecBase binding_sysbase;
static struct Task *binding_tasks[2];

struct binding_image
{
    struct p4_task_registry registry;
    struct p4_task_registry_record records[BINDING_RECORD_CAPACITY];
    struct p4_semaphore_ledger ledger;
    struct p4_semaphore_ledger_operation operations[TEST_OP_CAPACITY];
    struct p4_semaphore_ledger_entry entries[TEST_ENTRY_CAPACITY];
    struct SemaphoreRequest requests[8];
    struct SignalSemaphore semaphores[2];
    struct p4_task_wait_operation operation_outputs[2];
    struct p4_semaphore_ledger *boot_ledger;
};

static void binding_image_take(struct binding_image *image)
{
    memset(image, 0, sizeof(*image));
    memcpy(&image->registry, &p4_task_records, sizeof(image->registry));
    memcpy(image->records, binding_records, sizeof(image->records));
    memcpy(&image->ledger, &binding_ledger, sizeof(image->ledger));
    memcpy(image->operations, binding_operations, sizeof(image->operations));
    memcpy(image->entries, binding_entries, sizeof(image->entries));
    memcpy(image->requests, test_boot_requests, sizeof(image->requests));
    memcpy(image->semaphores, binding_semaphores,
           sizeof(image->semaphores));
    memcpy(image->operation_outputs, binding_operations_out,
           sizeof(image->operation_outputs));
    image->boot_ledger = test_boot_ledger;
}

static int binding_image_matches(const struct binding_image *image)
{
    struct binding_image current;
    binding_image_take(&current);
    return memcmp(&current, image, sizeof(current)) == 0;
}

static void binding_empty_sem(struct SignalSemaphore *sem)
{
    struct MinNode *head =
        (struct MinNode *)(void *)&sem->ss_WaitQueue.mlh_Head;
    struct MinNode *tail =
        (struct MinNode *)(void *)&sem->ss_WaitQueue.mlh_Tail;
    memset(sem, 0, sizeof(*sem));
    sem->ss_WaitQueue.mlh_Head = tail;
    sem->ss_WaitQueue.mlh_Tail = NULL;
    sem->ss_WaitQueue.mlh_TailPred = head;
    sem->ss_QueueCount = -1;
}

static void binding_fixture_reset(void)
{
    memset(&binding_ledger, 0, sizeof(binding_ledger));
    memset(binding_operations, 0, sizeof(binding_operations));
    memset(binding_entries, 0, sizeof(binding_entries));
    memset(binding_records, 0, sizeof(binding_records));
    memset(binding_semaphores, 0, sizeof(binding_semaphores));
    memset(binding_operations_out, 0, sizeof(binding_operations_out));
    memset(&binding_sysbase, 0, sizeof(binding_sysbase));
    memset(test_boot_requests, 0, sizeof(test_boot_requests));
    p4_task_registry_init(&p4_task_records);
    test_boot_ledger = &binding_ledger;
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
    CHECK(p4_task_pins_prepared == 1);
    CHECK(krnP4PublicSpinInit(&p4_task_pin_gate) == P4_PUBLIC_SPIN_OK);
    CHECK(p4_semaphore_ledger_init(&binding_ledger, binding_operations,
        TEST_OP_CAPACITY, binding_entries, TEST_ENTRY_CAPACITY) == 1);
    binding_empty_sem(&binding_semaphores[0]);
    binding_empty_sem(&binding_semaphores[1]);
}

static struct Task *binding_identity(void *page, size_t offset)
{
    return (struct Task *)((unsigned char *)page + offset);
}

static void binding_register_task(unsigned int slot, uint32_t hart)
{
    int result;
    CHECK(slot < BINDING_RECORD_CAPACITY);
    result = krnP4TaskRecordPublish(binding_tasks[slot],
        &binding_records[slot], &binding_sysbase);
    CHECK(result == 1);
    result = krnP4TaskRecordEnterRunning(binding_tasks[slot], hart,
        &binding_sysbase);
    CHECK(result == 1);
}

static int binding_reserve(struct Task *task, unsigned int output,
                           uint32_t entries)
{
    return krnP4SemaphoreOperationReserve(task, entries,
        &binding_operations_out[output], &binding_sysbase);
}

static int binding_configure(struct Task *task, unsigned int output,
    uint32_t ordinal, struct SignalSemaphore *sem, uint32_t shared)
{
    return krnP4SemaphoreOperationConfigure(task,
        binding_operations_out[output].generation, ordinal, sem, shared,
        &binding_sysbase);
}

static void binding_prepare_one(void *page, int configure)
{
    int result;
    binding_fixture_reset();
    binding_tasks[0] = binding_identity(page, 64u);
    binding_tasks[1] = binding_identity(page, 128u);
    binding_register_task(0u, 0u);
    result = binding_reserve(binding_tasks[0], 0u, 1u);
    CHECK(result == 1);
    if (configure)
    {
        test_semaphore_gate_lock();
        CHECK(binding_configure(binding_tasks[0], 0u, 0u,
                                &binding_semaphores[0], 0u) == 1);
        test_semaphore_gate_unlock();
        CHECK(krnP4SemaphoreOperationClose(binding_tasks[0],
            binding_operations_out[0].generation, &binding_sysbase) == 1);
    }
}

static void test_actual_immediate_queued_and_clean_refusals(void *page)
{
    struct binding_image before;
    struct p4_semaphore_ledger_entry *entry0;
    struct p4_semaphore_ledger_entry *entry1;
    unsigned int acquire_before;
    unsigned int unlock_before;
    unsigned int poke_before;

    binding_fixture_reset();
    binding_tasks[0] = binding_identity(page, 192u);
    binding_tasks[1] = binding_identity(page, 256u);
    binding_register_task(0u, 0u);
    binding_register_task(1u, 1u);
    CHECK(binding_reserve(binding_tasks[0], 0u, 1u) == 1);
    CHECK(binding_reserve(binding_tasks[1], 1u, 1u) == 1);

    test_semaphore_gate_lock();
    CHECK(binding_configure(binding_tasks[0], 0u, 0u,
                            &binding_semaphores[0], 0u) == 1);
    /* A stale generation is a clean refusal and leaves the complete
     * registry/ledger/request/semaphore image unchanged. */
    binding_image_take(&before);
    CHECK(krnP4SemaphoreOperationConfigure(binding_tasks[1],
        binding_operations_out[1].generation + 1u, 0u,
        &binding_semaphores[0], 0u, &binding_sysbase) == 0);
    CHECK(binding_image_matches(&before));
    CHECK(binding_configure(binding_tasks[1], 1u, 0u,
                            &binding_semaphores[0], 0u) == 1);
    test_semaphore_gate_unlock();
    CHECK(krnP4SemaphoreOperationClose(binding_tasks[0],
        binding_operations_out[0].generation, &binding_sysbase) == 1);
    CHECK(krnP4SemaphoreOperationClose(binding_tasks[1],
        binding_operations_out[1].generation, &binding_sysbase) == 1);

    binding_image_take(&before);
    CHECK(krnP4SemaphoreOperationAcquire(binding_tasks[1],
        binding_operations_out[1].generation + 1u, 0u,
        &binding_semaphores[0], &binding_sysbase) == 0);
    CHECK(binding_image_matches(&before));
    CHECK(krnP4SemaphoreOperationAcquire(binding_tasks[1],
        binding_operations_out[1].generation, 1u,
        &binding_semaphores[0], &binding_sysbase) == 0);
    CHECK(binding_image_matches(&before));

    acquire_before = mock_acquire_calls;
    unlock_before = mock_unlock_calls;
    poke_before = mock_poke_calls;
    test_semaphore_gate_lock();
    CHECK(krnP4SemaphoreOperationAcquire(binding_tasks[0],
        binding_operations_out[0].generation, 0u, &binding_semaphores[0],
        &binding_sysbase) == 1);
    CHECK(mock_outer_semaphore_gate_held);
    CHECK(krnP4SemaphoreOperationAcquire(binding_tasks[1],
        binding_operations_out[1].generation, 0u, &binding_semaphores[0],
        &binding_sysbase) == 2);
    CHECK(mock_outer_semaphore_gate_held);
    test_semaphore_gate_unlock();

    entry0 = binding_operations_out[0].operation->first;
    entry1 = binding_operations_out[1].operation->first;
    CHECK(binding_semaphores[0].ss_Owner == binding_tasks[0]);
    CHECK(binding_semaphores[0].ss_NestCount == 1);
    CHECK(binding_semaphores[0].ss_QueueCount == 1);
    CHECK(entry0->state == P4_SEMAPHORE_LEDGER_ENTRY_OWNED);
    CHECK(entry1->state == P4_SEMAPHORE_LEDGER_ENTRY_QUEUED);
    CHECK(binding_records[1].wait_scope.queued_count == 1u);
    CHECK(test_boot_requests[1].sr_Link.mln_Pred != NULL);
    CHECK(test_boot_requests[1].sr_Link.mln_Succ != NULL);
    CHECK(mock_acquire_calls == acquire_before + 2u);
    CHECK(mock_unlock_calls == unlock_before + 2u);
    CHECK(mock_poke_calls == poke_before);
    CHECK(mock_irq_depth == 0u && mock_gate_held == 0);
    CHECK(mock_disable_calls == mock_enable_calls);
    CHECK(mock_acquire_calls == mock_unlock_calls);
    CHECK(mock_fatal_calls == 0u);
}

enum malformed_case
{
    MALFORMED_ZERO_CAPACITY,
    MALFORMED_ENTRY_CYCLE,
    MALFORMED_LEDGER_HEADER_ALIAS,
    MALFORMED_OPERATION_ARRAY_RECORD_ALIAS,
    MALFORMED_ENTRY_ARRAY_REQUEST_POOL_ALIAS,
    MALFORMED_OPERATION_ARRAY_WRAP,
    MALFORMED_SEMAPHORE_RECORD_ALIAS,
    MALFORMED_WRONG_CONFIGURED_SEMAPHORE,
    MALFORMED_QUEUE_TAIL,
    MALFORMED_CONFIG_SHARED_TAG
};

static void malformed_child_call(enum malformed_case which)
{
    int result;
    if (which == MALFORMED_ZERO_CAPACITY)
        binding_ledger.entry_capacity = 0u;
    else if (which == MALFORMED_ENTRY_CYCLE)
        binding_entries[0].operation_next = &binding_entries[0];
    else if (which == MALFORMED_LEDGER_HEADER_ALIAS)
        test_boot_ledger = (struct p4_semaphore_ledger *)(void *)&p4_task_records;
    else if (which == MALFORMED_OPERATION_ARRAY_RECORD_ALIAS)
        binding_ledger.operations =
            (struct p4_semaphore_ledger_operation *)(void *)&binding_records[0];
    else if (which == MALFORMED_ENTRY_ARRAY_REQUEST_POOL_ALIAS)
        binding_ledger.entries =
            (struct p4_semaphore_ledger_entry *)(void *)test_boot_requests;
    else if (which == MALFORMED_OPERATION_ARRAY_WRAP)
        binding_ledger.operations =
            (struct p4_semaphore_ledger_operation *)(uintptr_t)(UINTPTR_MAX - 7u);
    else if (which == MALFORMED_QUEUE_TAIL)
        binding_semaphores[0].ss_WaitQueue.mlh_TailPred = NULL;

    test_semaphore_gate_lock();
    if (which == MALFORMED_SEMAPHORE_RECORD_ALIAS)
        result = krnP4SemaphoreOperationAcquire(binding_tasks[0],
            binding_operations_out[0].generation, 0u,
            (struct SignalSemaphore *)(void *)&binding_records[0],
            &binding_sysbase);
    else if (which == MALFORMED_WRONG_CONFIGURED_SEMAPHORE)
        result = krnP4SemaphoreOperationAcquire(binding_tasks[0],
            binding_operations_out[0].generation, 0u,
            &binding_semaphores[1], &binding_sysbase);
    else if (which == MALFORMED_CONFIG_SHARED_TAG)
        result = binding_configure(binding_tasks[0], 0u, 0u,
            &binding_semaphores[0], 2u);
    else
        result = krnP4SemaphoreOperationAcquire(binding_tasks[0],
            binding_operations_out[0].generation, 0u,
            &binding_semaphores[0], &binding_sysbase);
    (void)result;
    _exit(0);
}

static void expect_malformed_fatal(void *page, enum malformed_case which)
{
    struct binding_image before;
    struct rlimit no_core = { 0u, 0u };
    pid_t child;
    int status = 0;
    int configure = which != MALFORMED_CONFIG_SHARED_TAG;

    binding_prepare_one(page, configure);
    binding_image_take(&before);
    fflush(NULL);
    child = fork();
    CHECK(child >= 0);
    if (child == 0)
    {
        (void)setrlimit(RLIMIT_CORE, &no_core);
        (void)close(STDERR_FILENO);
        malformed_child_call(which);
    }
    if (child > 0)
    {
        CHECK(waitpid(child, &status, 0) == child);
        CHECK(WIFSIGNALED(status));
        if (WIFSIGNALED(status))
            CHECK(WTERMSIG(status) == SIGABRT);
    }
    CHECK(binding_image_matches(&before));
    CHECK(mock_irq_depth == 0u && mock_gate_held == 0);
    CHECK(mock_outer_semaphore_gate_held == 0);
    CHECK(mock_fatal_calls == 0u);
    CHECK(mock_disable_calls == mock_enable_calls);
    CHECK(mock_acquire_calls == mock_unlock_calls);
}

static void test_malformed_fatal_children(void *page)
{
    enum malformed_case which;
    for (which = MALFORMED_ZERO_CAPACITY;
         which <= MALFORMED_CONFIG_SHARED_TAG;
         which = (enum malformed_case)((int)which + 1))
        expect_malformed_fatal(page, which);
}

int main(void)
{
    long page_size;
    void *page;
    unsigned int baseline_checks;

    if (p4_semaphore_operation_admission_baseline_main() != 0)
        return 1;
    baseline_checks = checks;
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

    test_actual_immediate_queued_and_clean_refusals(page);
    test_malformed_fatal_children(page);
    if (munmap(page, (size_t)page_size) != 0)
    {
        fprintf(stderr, "munmap failed: %d\n", errno);
        return 2;
    }
    if (failures != 0u)
    {
        fprintf(stderr, "P4 semaphore acquire wrappers: %u/%u new checks failed\n",
                failures, checks - baseline_checks);
        return 1;
    }
    printf("P4 semaphore acquire wrappers: %u checks passed; included baseline %u\n",
           checks - baseline_checks, baseline_checks);
    return 0;
}
