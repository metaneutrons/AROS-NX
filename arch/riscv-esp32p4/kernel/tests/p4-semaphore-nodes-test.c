#define _DEFAULT_SOURCE 1
#define __AROSEXEC_SMP__ 1
#define __AROSPLATFORM_SMP__ 1
#define P4_SEMAPHORE_NODES_TEST 1

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

typedef int16_t WORD;

struct Task { uintptr_t opaque; };
struct MinNode { struct MinNode *mln_Succ; struct MinNode *mln_Pred; };
struct MinList
{
    struct MinNode *mlh_Head;
    struct MinNode *mlh_Tail;
    struct MinNode *mlh_TailPred;
};
struct Node
{
    struct Node *ln_Succ;
    struct Node *ln_Pred;
};
struct SemaphoreRequest
{
    struct MinNode sr_Link;
    struct Task *sr_Waiter;
};
struct SignalSemaphore
{
    struct Node ss_Link;
    WORD ss_NestCount;
    struct MinList ss_WaitQueue;
    struct SemaphoreRequest ss_MultipleLink;
    struct Task *ss_Owner;
    WORD ss_QueueCount;
};

#include "../p4_task_lifetime.c"
#include "../p4_semaphore_ledger.c"
#include "../p4_task_registry.c"
#include "../p4_semaphore_nodes.c"

#define TEST_TASKS 4u
#define TEST_OPS 8u
#define TEST_ENTRIES 16u
#define CHECK(condition) do { \
    ++checks; \
    if (!(condition)) { \
        fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); \
        exit(1); \
    } \
} while (0)

static unsigned checks;
static void *task_page;
static size_t task_page_size;

struct Fixture
{
    struct p4_task_registry registry;
    struct p4_task_registry_record records[TEST_TASKS];
    struct p4_semaphore_ledger ledger;
    struct p4_semaphore_ledger_operation operations[TEST_OPS];
    struct p4_semaphore_ledger_entry entries[TEST_ENTRIES];
    struct SemaphoreRequest requests[TEST_ENTRIES];
    struct Task *tasks[TEST_TASKS];
};

static struct Task *task_identity(unsigned index)
{
    return (struct Task *)((unsigned char *)task_page + (size_t)index * 32u);
}

static void empty_sem(struct SignalSemaphore *sem)
{
    struct MinNode *head = (struct MinNode *)(void *)&sem->ss_WaitQueue.mlh_Head;
    struct MinNode *tail = (struct MinNode *)(void *)&sem->ss_WaitQueue.mlh_Tail;
    memset(sem, 0, sizeof(*sem));
    sem->ss_WaitQueue.mlh_Head = tail;
    sem->ss_WaitQueue.mlh_Tail = NULL;
    sem->ss_WaitQueue.mlh_TailPred = head;
    sem->ss_QueueCount = -1;
}

static uint32_t queue_links(const struct SignalSemaphore *sem)
{
    const struct MinNode *tail =
        (const struct MinNode *)(const void *)&sem->ss_WaitQueue.mlh_Tail;
    const struct MinNode *cursor = sem->ss_WaitQueue.mlh_Head;
    uint32_t count = 0;
    while (cursor != tail && count < 40000u)
    {
        cursor = cursor->mln_Succ;
        ++count;
    }
    return cursor == tail ? count : UINT32_MAX;
}

static void fixture_init(struct Fixture *fixture)
{
    unsigned i;
    memset(fixture, 0, sizeof(*fixture));
    p4_task_registry_init(&fixture->registry);
    CHECK(p4_semaphore_ledger_init(&fixture->ledger, fixture->operations,
        TEST_OPS, fixture->entries, TEST_ENTRIES) == 1);
    for (i = 0; i < TEST_TASKS; ++i)
        fixture->tasks[i] = task_identity(i);
}

static void register_running(struct Fixture *fixture, unsigned task,
    uint32_t hart)
{
    CHECK(p4_task_registry_register_locked(&fixture->registry,
        &fixture->records[task], fixture->tasks[task]) == 1);
    CHECK(p4_task_registry_enter_running_locked(&fixture->registry,
        fixture->tasks[task], hart) == 1);
}

static uint32_t make_operation(struct Fixture *fixture, unsigned task,
    uint32_t count, struct SignalSemaphore *const semaphores[],
    const uint32_t shared[])
{
    struct p4_task_wait_operation wait;
    uint32_t i;
    CHECK(p4_task_registry_wait_reserve_operation_locked(&fixture->registry,
        &fixture->ledger, fixture->tasks[task], count, &wait) == 1);
    CHECK(wait.operation != NULL && wait.generation != 0);
    for (i = 0; i < count; ++i)
        CHECK(p4_semaphore_nodes_configure_locked(&fixture->registry,
            &fixture->ledger, fixture->requests, fixture->tasks[task],
            wait.generation, i, semaphores[i], shared[i]) == 1);
    CHECK(p4_task_registry_wait_operation_close_locked(&fixture->registry,
        &fixture->ledger, fixture->tasks[task], wait.generation) == 1);
    return wait.generation;
}

static void test_both_hart_immediate_acquire(void)
{
    unsigned hart;
    for (hart = 0; hart < 2; ++hart)
    {
        struct Fixture fixture;
        struct SignalSemaphore sem;
        struct SignalSemaphore *semaphores[1] = { &sem };
        const uint32_t shared[1] = { 0 };
        uint32_t generation;
        empty_sem(&sem);
        fixture_init(&fixture);
        register_running(&fixture, 0, hart);
        generation = make_operation(&fixture, 0, 1, semaphores, shared);
        CHECK(p4_semaphore_nodes_acquire_locked(&fixture.registry,
            &fixture.ledger, fixture.requests, fixture.tasks[0], generation,
            0, &sem) == 1);
        CHECK(sem.ss_NestCount == 1);
        CHECK(sem.ss_QueueCount == 0);
        CHECK(sem.ss_Owner == fixture.tasks[0]);
        CHECK(fixture.ledger.entries[0].state ==
            P4_SEMAPHORE_LEDGER_ENTRY_OWNED);
        CHECK(fixture.requests[0].sr_Waiter == fixture.tasks[0]);
        CHECK(p4_task_registry_wait_operation_complete_locked(
            &fixture.registry, &fixture.ledger, fixture.tasks[0],
            generation) == 1);
        CHECK(fixture.records[0].wait_scope.active == 0);
        CHECK(sem.ss_NestCount == 1 && sem.ss_Owner == fixture.tasks[0]);
    }
}

static void test_fragmented_absolute_slot_reuse(void)
{
    struct Fixture fixture;
    struct SignalSemaphore actual;
    struct SignalSemaphore *semaphores[2] = { &actual, &actual };
    const uint32_t shared[2] = { 0, 0 };
    struct p4_semaphore_ledger_operation *hole;
    struct p4_semaphore_ledger_operation *blocker;
    uint32_t generation;
    empty_sem(&actual);
    fixture_init(&fixture);
    register_running(&fixture, 0, 1);
    CHECK(p4_semaphore_ledger_reserve(&fixture.ledger, (void *)0x1000u,
        11, 1, &hole) == 1);
    CHECK(p4_semaphore_ledger_reserve(&fixture.ledger, (void *)0x2000u,
        12, 1, &blocker) == 1);
    CHECK(p4_semaphore_ledger_cancel(&fixture.ledger, hole) == 1);
    CHECK(p4_semaphore_ledger_reclaim(&fixture.ledger, hole) == 1);
    generation = make_operation(&fixture, 0, 2, semaphores, shared);
    CHECK(fixture.entries[0].semaphore == &actual);
    CHECK(fixture.entries[2].semaphore == &actual);
    CHECK(fixture.entries[0].request == &fixture.requests[0]);
    CHECK(fixture.entries[2].request == &fixture.requests[2]);
    CHECK(fixture.requests[1].sr_Link.mln_Succ == NULL);
    CHECK(fixture.requests[1].sr_Link.mln_Pred == NULL);
    CHECK(p4_semaphore_nodes_acquire_locked(&fixture.registry,
        &fixture.ledger, fixture.requests, fixture.tasks[0], generation,
        0, &actual) == 1);
    CHECK(p4_semaphore_nodes_acquire_locked(&fixture.registry,
        &fixture.ledger, fixture.requests, fixture.tasks[0], generation,
        1, &actual) == 1);
    CHECK(actual.ss_NestCount == 2 && actual.ss_QueueCount == 1);
    CHECK(actual.ss_Owner == fixture.tasks[0]);
    CHECK(fixture.ledger.entries[0].state == P4_SEMAPHORE_LEDGER_ENTRY_OWNED);
    CHECK(fixture.ledger.entries[2].state == P4_SEMAPHORE_LEDGER_ENTRY_OWNED);
    CHECK(p4_semaphore_nodes_acquire_locked(&fixture.registry,
        &fixture.ledger, fixture.requests, fixture.tasks[0], generation,
        2, &actual) == 0);
    (void)blocker;
}

static void test_recursive_and_shared_semantics(void)
{
    struct Fixture fixture;
    struct SignalSemaphore exclusive, shared_sem;
    struct SignalSemaphore *exclusive_entries[2] = { &exclusive, &exclusive };
    struct SignalSemaphore *shared_entries[1] = { &shared_sem };
    const uint32_t exclusive_modes[2] = { 0, 0 };
    const uint32_t shared_mode[1] = { 1 };
    uint32_t generation0, generation1;
    empty_sem(&exclusive);
    empty_sem(&shared_sem);
    fixture_init(&fixture);
    register_running(&fixture, 0, 0);
    register_running(&fixture, 1, 1);
    generation0 = make_operation(&fixture, 0, 2, exclusive_entries,
        exclusive_modes);
    CHECK(p4_semaphore_nodes_acquire_locked(&fixture.registry,
        &fixture.ledger, fixture.requests, fixture.tasks[0], generation0,
        0, &exclusive) == 1);
    CHECK(p4_semaphore_nodes_acquire_locked(&fixture.registry,
        &fixture.ledger, fixture.requests, fixture.tasks[0], generation0,
        1, &exclusive) == 1);
    CHECK(exclusive.ss_NestCount == 2 && exclusive.ss_QueueCount == 1);
    CHECK(exclusive.ss_Owner == fixture.tasks[0]);
    CHECK(p4_task_registry_wait_operation_complete_locked(
        &fixture.registry, &fixture.ledger, fixture.tasks[0], generation0) == 1);

    generation0 = make_operation(&fixture, 0, 1, shared_entries, shared_mode);
    generation1 = make_operation(&fixture, 1, 1, shared_entries, shared_mode);
    CHECK(p4_semaphore_nodes_acquire_locked(&fixture.registry,
        &fixture.ledger, fixture.requests, fixture.tasks[0], generation0,
        0, &shared_sem) == 1);
    CHECK(shared_sem.ss_NestCount == 1 && shared_sem.ss_QueueCount == 0);
    CHECK(shared_sem.ss_Owner == NULL);
    CHECK((((uintptr_t)fixture.requests[1].sr_Waiter) & 1u) == 1u);
    CHECK(p4_semaphore_nodes_acquire_locked(&fixture.registry,
        &fixture.ledger, fixture.requests, fixture.tasks[1], generation1,
        0, &shared_sem) == 1);
    CHECK(shared_sem.ss_NestCount == 2 && shared_sem.ss_QueueCount == 1);
    CHECK(shared_sem.ss_Owner == NULL);
    CHECK(p4_task_registry_wait_operation_complete_locked(
        &fixture.registry, &fixture.ledger, fixture.tasks[0], generation0) == 1);
    CHECK(p4_task_registry_wait_operation_complete_locked(
        &fixture.registry, &fixture.ledger, fixture.tasks[1], generation1) == 1);
}

static void test_fused_cancel_unlinks_and_rolls_back(void)
{
    unsigned hart;
    for (hart = 0; hart < 2; ++hart)
    {
        struct Fixture fixture;
        struct SignalSemaphore own, blocked;
        struct SignalSemaphore *owner_sem[1] = { &blocked };
        const uint32_t owner_mode[1] = { 0 };
        struct SignalSemaphore *mixed[2] = { &own, &blocked };
        const uint32_t modes[2] = { 0, 0 };
        struct p4_task_wait_cancel_snapshot snapshot;
        struct Fixture before;
        struct SignalSemaphore own_before;
        uint32_t owner_generation, generation;
        empty_sem(&own);
        empty_sem(&blocked);
        fixture_init(&fixture);
        register_running(&fixture, 0, 1u - hart);
        register_running(&fixture, 1, hart);
        owner_generation = make_operation(&fixture, 0, 1, owner_sem,
            owner_mode);
        CHECK(p4_semaphore_nodes_acquire_locked(&fixture.registry,
            &fixture.ledger, fixture.requests, fixture.tasks[0],
            owner_generation, 0, &blocked) == 1);
        CHECK(p4_task_registry_wait_operation_complete_locked(
            &fixture.registry, &fixture.ledger, fixture.tasks[0],
            owner_generation) == 1);

        generation = make_operation(&fixture, 1, 2, mixed, modes);
        CHECK(p4_semaphore_nodes_acquire_locked(&fixture.registry,
            &fixture.ledger, fixture.requests, fixture.tasks[1], generation,
            0, &own) == 1);
        CHECK(p4_semaphore_nodes_acquire_locked(&fixture.registry,
            &fixture.ledger, fixture.requests, fixture.tasks[1], generation,
            1, &blocked) == 2);
        CHECK(own.ss_NestCount == 1 && own.ss_QueueCount == 0);
        CHECK(own.ss_Owner == fixture.tasks[1]);
        CHECK(blocked.ss_NestCount == 1 && blocked.ss_QueueCount == 1);
        CHECK(queue_links(&blocked) == 1);
        CHECK(fixture.records[1].wait_scope.queued_count == 1);

        CHECK(p4_task_registry_retire_with_ledger_locked(&fixture.registry,
            &fixture.ledger, fixture.tasks[1], 0) == 1);
        CHECK(p4_task_registry_claim_next_wait_cancel_with_ledger_locked(
            &fixture.registry, &fixture.ledger, &snapshot) == 1);
        CHECK(snapshot.identity == fixture.tasks[1]);
        CHECK(snapshot.generation == generation);
        CHECK(snapshot.queued_count == 1);
        CHECK(p4_semaphore_nodes_withdraw_locked(&fixture.registry,
            &fixture.ledger, fixture.requests, fixture.tasks[1], generation,
            1, &blocked) == 1);
        CHECK(blocked.ss_QueueCount == 0 && blocked.ss_NestCount == 1);
        CHECK(queue_links(&blocked) == 0);
        CHECK(fixture.records[1].wait_scope.queued_count == 0);
        CHECK(fixture.requests[1].sr_Link.mln_Succ == NULL);
        CHECK(fixture.requests[1].sr_Link.mln_Pred == NULL);

        /* Withdrawal is safe while RUN remains pinned; ownership rollback is
         * deferred until the retired task's execution reference drains. */
        before = fixture;
        own_before = own;
        CHECK(p4_semaphore_nodes_rollback_locked(&fixture.registry,
            &fixture.ledger, fixture.requests, fixture.tasks[1], generation,
            0, &own) == 0);
        CHECK(memcmp(&before, &fixture, sizeof(fixture)) == 0);
        CHECK(memcmp(&own_before, &own, sizeof(own)) == 0);
        CHECK(p4_task_registry_leave_running_locked(&fixture.registry,
            fixture.tasks[1], hart) == 1);
        CHECK(p4_semaphore_nodes_rollback_locked(&fixture.registry,
            &fixture.ledger, fixture.requests, fixture.tasks[1], generation,
            0, &own) == 1);
        CHECK(own.ss_NestCount == 0 && own.ss_QueueCount == -1);
        CHECK(own.ss_Owner == NULL);
        CHECK(fixture.ledger.entries[1].state ==
            P4_SEMAPHORE_LEDGER_ENTRY_WITHDRAWN);
        CHECK(fixture.ledger.entries[0].state ==
            P4_SEMAPHORE_LEDGER_ENTRY_RELEASED);
        CHECK(p4_task_registry_wait_operation_finish_cancel_locked(
            &fixture.registry, &fixture.ledger, fixture.tasks[1],
            generation) == 1);
        CHECK(fixture.records[1].wait_scope.active == 0);
        CHECK(fixture.ledger.active_operations == 0);
        CHECK(p4_task_registry_claim_reclaim_locked(&fixture.registry,
            fixture.tasks[1]) == &fixture.records[1]);
        CHECK(fixture.registry.count == 1);
    }
}

static void test_no_running_refusal_and_harts(void)
{
    unsigned hart;
    for (hart = 0; hart < 2; ++hart)
    {
        struct Fixture fixture;
        struct SignalSemaphore sem;
        struct SignalSemaphore *semaphores[1] = { &sem };
        const uint32_t modes[1] = { 0 };
        struct Fixture before;
        struct SignalSemaphore sem_before;
        uint32_t generation;
        empty_sem(&sem);
        fixture_init(&fixture);
        register_running(&fixture, 0, hart);
        generation = make_operation(&fixture, 0, 1, semaphores, modes);
        CHECK(p4_task_registry_leave_running_locked(&fixture.registry,
            fixture.tasks[0], hart) == 1);
        before = fixture;
        sem_before = sem;
        CHECK(p4_semaphore_nodes_acquire_locked(&fixture.registry,
            &fixture.ledger, fixture.requests, fixture.tasks[0], generation,
            0, &sem) == 0);
        CHECK(memcmp(&before, &fixture, sizeof(fixture)) == 0);
        CHECK(memcmp(&sem_before, &sem, sizeof(sem)) == 0);
        CHECK(sem.ss_NestCount == 0 && sem.ss_QueueCount == -1);
    }

    {
        struct Fixture fixture;
        struct SignalSemaphore sem;
        struct p4_task_wait_operation wait;
        struct Fixture before;
        struct SignalSemaphore sem_before;
        empty_sem(&sem);
        fixture_init(&fixture);
        register_running(&fixture, 0, 1);
        CHECK(p4_task_registry_wait_reserve_operation_locked(&fixture.registry,
            &fixture.ledger, fixture.tasks[0], 1, &wait) == 1);
        CHECK(p4_task_registry_leave_running_locked(&fixture.registry,
            fixture.tasks[0], 1) == 1);
        before = fixture;
        sem_before = sem;
        CHECK(p4_semaphore_nodes_configure_locked(&fixture.registry,
            &fixture.ledger, fixture.requests, fixture.tasks[0],
            wait.generation, 0, &sem, 0) == 0);
        CHECK(memcmp(&before, &fixture, sizeof(fixture)) == 0);
        CHECK(memcmp(&sem_before, &sem, sizeof(sem)) == 0);
    }
}

static void test_corrupt_lists_and_aliases(void)
{
    unsigned kind;
    for (kind = 0; kind < 4; ++kind)
    {
        struct Fixture fixture;
        struct SignalSemaphore sem;
        struct SignalSemaphore sem_before;
        struct SignalSemaphore *semaphores[1] = { &sem };
        const uint32_t modes[1] = { 0 };
        struct Fixture before;
        uint32_t generation;
        int result;
        empty_sem(&sem);
        fixture_init(&fixture);
        register_running(&fixture, 0, 0);
        register_running(&fixture, 1, 1);
        generation = make_operation(&fixture, 0, 1, semaphores, modes);
        CHECK(p4_semaphore_nodes_acquire_locked(&fixture.registry,
            &fixture.ledger, fixture.requests, fixture.tasks[0], generation,
            0, &sem) == 1);
        CHECK(p4_task_registry_wait_operation_complete_locked(&fixture.registry,
            &fixture.ledger, fixture.tasks[0], generation) == 1);
        generation = make_operation(&fixture, 1, 1, semaphores, modes);
        CHECK(p4_semaphore_nodes_acquire_locked(&fixture.registry,
            &fixture.ledger, fixture.requests, fixture.tasks[1], generation,
            0, &sem) == 2);
        CHECK(p4_task_registry_wait_operation_complete_locked(&fixture.registry,
            &fixture.ledger, fixture.tasks[1], generation) == 0);
        register_running(&fixture, 2, 0);
        generation = make_operation(&fixture, 2, 1, semaphores, modes);
        switch (kind)
        {
            case 0:
                fixture.requests[0].sr_Link.mln_Pred = NULL;
                break;
            case 1:
                sem.ss_WaitQueue.mlh_TailPred =
                    (struct MinNode *)(void *)&sem.ss_WaitQueue.mlh_Head;
                break;
            case 2:
                sem.ss_QueueCount = 0;
                break;
            default:
                fixture.records[2].wait_scope.queued_count = 1;
                break;
        }
        before = fixture;
        sem_before = sem;
        result = p4_semaphore_nodes_acquire_locked(&fixture.registry,
            &fixture.ledger, fixture.requests, fixture.tasks[2], generation,
            0, &sem);
        if (result != -1)
            fprintf(stderr, "corruption kind %u returned %d\n", kind, result);
        CHECK(result == -1);
        CHECK(memcmp(&before, &fixture, sizeof(fixture)) == 0);
        CHECK(memcmp(&sem_before, &sem, sizeof(sem)) == 0);
    }

    {
        struct Fixture fixture;
        struct SignalSemaphore sem;
        struct SignalSemaphore *semaphores[1] = { &sem };
        const uint32_t modes[1] = { 0 };
        struct Fixture before;
        struct SignalSemaphore sem_before;
        uint32_t generation;
        empty_sem(&sem);
        fixture_init(&fixture);
        register_running(&fixture, 0, 0);
        generation = make_operation(&fixture, 0, 1, semaphores, modes);
        before = fixture;
        sem_before = sem;
        CHECK(p4_semaphore_nodes_configure_locked(&fixture.registry,
            &fixture.ledger,
            (struct SemaphoreRequest *)(void *)((unsigned char *)fixture.requests + 1u),
            fixture.tasks[0], generation, 0, &sem, 0) == -1);
        CHECK(memcmp(&before, &fixture, sizeof(fixture)) == 0);
        CHECK(p4_semaphore_nodes_configure_locked(&fixture.registry,
            &fixture.ledger, (struct SemaphoreRequest *)(void *)&fixture.ledger,
            fixture.tasks[0], generation, 0, &sem, 0) == -1);
        CHECK(memcmp(&before, &fixture, sizeof(fixture)) == 0);
        CHECK(p4_semaphore_nodes_configure_locked(&fixture.registry,
            &fixture.ledger, fixture.requests, fixture.tasks[0], generation,
            0, (struct SignalSemaphore *)(void *)&fixture.ledger, 0) == -1);
        CHECK(memcmp(&before, &fixture, sizeof(fixture)) == 0);
        CHECK(p4_semaphore_nodes_configure_locked(&fixture.registry,
            &fixture.ledger, fixture.requests, fixture.tasks[0], generation,
            0, (struct SignalSemaphore *)(void *)((unsigned char *)&sem + 1u), 0) == -1);
        CHECK(memcmp(&before, &fixture, sizeof(fixture)) == 0);
        CHECK(p4_semaphore_nodes_configure_locked(&fixture.registry,
            &fixture.ledger, fixture.requests, fixture.tasks[0], generation,
            0, &sem, 2) == -1);
        CHECK(memcmp(&before, &fixture, sizeof(fixture)) == 0);
        CHECK(p4_semaphore_nodes_configure_locked(&fixture.registry,
            &fixture.ledger,
            (struct SemaphoreRequest *)(void *)&fixture.registry,
            fixture.tasks[0], generation, 0, &sem, 0) == -1);
        CHECK(memcmp(&before, &fixture, sizeof(fixture)) == 0);
        CHECK(p4_semaphore_nodes_configure_locked(&fixture.registry,
            &fixture.ledger,
            (struct SemaphoreRequest *)(void *)fixture.entries,
            fixture.tasks[0], generation, 0, &sem, 0) == -1);
        CHECK(memcmp(&before, &fixture, sizeof(fixture)) == 0);
        CHECK(p4_semaphore_nodes_configure_locked(&fixture.registry,
            &fixture.ledger, fixture.requests, fixture.tasks[0], generation,
            0, (struct SignalSemaphore *)(void *)&fixture.registry, 0) == -1);
        CHECK(memcmp(&before, &fixture, sizeof(fixture)) == 0);
        CHECK(p4_semaphore_nodes_configure_locked(&fixture.registry,
            &fixture.ledger, fixture.requests, fixture.tasks[0], generation,
            0, (struct SignalSemaphore *)(void *)&fixture.requests[0], 0) == -1);
        CHECK(memcmp(&before, &fixture, sizeof(fixture)) == 0);
        CHECK(memcmp(&sem_before, &sem, sizeof(sem)) == 0);
    }
}

static void test_pending_owned_nest_mismatch(void)
{
    struct Fixture fixture;
    struct SignalSemaphore sem;
    struct SignalSemaphore *semaphores[2] = { &sem, &sem };
    const uint32_t modes[2] = { 0, 0 };
    struct Fixture before;
    struct SignalSemaphore sem_before;
    uint32_t generation;
    empty_sem(&sem);
    fixture_init(&fixture);
    register_running(&fixture, 0, 0);
    generation = make_operation(&fixture, 0, 2, semaphores, modes);
    CHECK(p4_semaphore_nodes_acquire_locked(&fixture.registry,
        &fixture.ledger, fixture.requests, fixture.tasks[0], generation,
        0, &sem) == 1);
    sem.ss_NestCount = 0;
    sem.ss_QueueCount = -1;
    sem.ss_Owner = NULL;
    before = fixture;
    sem_before = sem;
    CHECK(p4_semaphore_nodes_acquire_locked(&fixture.registry,
        &fixture.ledger, fixture.requests, fixture.tasks[0], generation,
        1, &sem) == -1);
    CHECK(memcmp(&before, &fixture, sizeof(fixture)) == 0);
    CHECK(memcmp(&sem_before, &sem, sizeof(sem)) == 0);
}

static void test_nest_overflow_refusal(void)
{
    struct Fixture fixture;
    struct SignalSemaphore sem;
    struct SignalSemaphore *semaphores[1] = { &sem };
    const uint32_t modes[1] = { 0 };
    struct Fixture before;
    struct SignalSemaphore sem_before;
    uint32_t generation;
    empty_sem(&sem);
    fixture_init(&fixture);
    register_running(&fixture, 0, 0);
    generation = make_operation(&fixture, 0, 1, semaphores, modes);
    sem.ss_NestCount = INT16_MAX;
    sem.ss_QueueCount = INT16_MAX - 1;
    sem.ss_Owner = fixture.tasks[0];
    before = fixture;
    sem_before = sem;
    CHECK(p4_semaphore_nodes_acquire_locked(&fixture.registry,
        &fixture.ledger, fixture.requests, fixture.tasks[0], generation,
        0, &sem) == -1);
    CHECK(memcmp(&before, &fixture, sizeof(fixture)) == 0);
    CHECK(memcmp(&sem_before, &sem, sizeof(sem)) == 0);
}

static void link_unknown_nodes(struct SignalSemaphore *sem,
    struct MinNode *nodes, uint32_t count)
{
    struct MinNode *head = (struct MinNode *)(void *)&sem->ss_WaitQueue.mlh_Head;
    struct MinNode *tail = (struct MinNode *)(void *)&sem->ss_WaitQueue.mlh_Tail;
    uint32_t i;
    if (count == 0)
    {
        sem->ss_WaitQueue.mlh_Head = tail;
        sem->ss_WaitQueue.mlh_TailPred = head;
        return;
    }
    sem->ss_WaitQueue.mlh_Head = nodes;
    sem->ss_WaitQueue.mlh_TailPred = &nodes[count - 1u];
    for (i = 0; i < count; ++i)
    {
        nodes[i].mln_Pred = i == 0 ? head : &nodes[i - 1u];
        nodes[i].mln_Succ = i + 1u == count ? tail : &nodes[i + 1u];
    }
}

static void test_queue_length_boundary(void)
{
    const uint32_t external_count = (uint32_t)INT16_MAX;
    struct Fixture fixture;
    struct SignalSemaphore sem;
    struct SignalSemaphore *semaphores[3] = { &sem, &sem, &sem };
    const uint32_t modes[3] = { 0, 0, 0 };
    struct MinNode *external;
    struct Fixture before;
    struct SignalSemaphore sem_before;
    struct p4_task_wait_cancel_snapshot cancel_snapshot;
    uint32_t generation[3];
    unsigned i;
    empty_sem(&sem);
    fixture_init(&fixture);
    for (i = 0; i < 3; ++i)
    {
        register_running(&fixture, i, i & 1u);
        generation[i] = make_operation(&fixture, i, 1, semaphores, modes);
    }
    external = calloc(external_count, sizeof(*external));
    CHECK(external != NULL);
    link_unknown_nodes(&sem, external, external_count);
    sem.ss_NestCount = 0;
    sem.ss_QueueCount = (WORD)(external_count - 1u);
    sem.ss_Owner = NULL;
    CHECK(queue_links(&sem) == external_count);

    CHECK(p4_semaphore_nodes_acquire_locked(&fixture.registry,
        &fixture.ledger, fixture.requests, fixture.tasks[0], generation[0],
        0, &sem) == 2);
    CHECK(queue_links(&sem) == (uint32_t)INT16_MAX + 1u);
    CHECK(sem.ss_QueueCount == INT16_MAX);
    CHECK(sem.ss_NestCount == 0 && sem.ss_Owner == NULL);
    CHECK(fixture.records[0].wait_scope.queued_count == 1);
    before = fixture;
    sem_before = sem;
    CHECK(p4_semaphore_nodes_acquire_locked(&fixture.registry,
        &fixture.ledger, fixture.requests, fixture.tasks[1], generation[1],
        0, &sem) == -1);
    CHECK(memcmp(&before, &fixture, sizeof(fixture)) == 0);
    CHECK(memcmp(&sem_before, &sem, sizeof(sem)) == 0);
    CHECK(sem.ss_QueueCount == INT16_MAX);
    CHECK(queue_links(&sem) == (uint32_t)INT16_MAX + 1u);
    CHECK(p4_task_registry_retire_with_ledger_locked(&fixture.registry,
        &fixture.ledger, fixture.tasks[0], 0) == 1);
    CHECK(p4_task_registry_claim_next_wait_cancel_with_ledger_locked(
        &fixture.registry, &fixture.ledger, &cancel_snapshot) == 1);
    CHECK(cancel_snapshot.identity == fixture.tasks[0]);
    CHECK(cancel_snapshot.queued_count == 1);
    CHECK(p4_semaphore_nodes_withdraw_locked(&fixture.registry,
        &fixture.ledger, fixture.requests, fixture.tasks[0], generation[0],
        0, &sem) == 1);
    CHECK(sem.ss_QueueCount == INT16_MAX - 1);
    CHECK(queue_links(&sem) == (uint32_t)INT16_MAX);
    CHECK(fixture.records[0].wait_scope.queued_count == 0);
    CHECK(fixture.requests[0].sr_Link.mln_Succ == NULL);
    CHECK(fixture.requests[0].sr_Link.mln_Pred == NULL);
    CHECK(p4_task_registry_leave_running_locked(&fixture.registry,
        fixture.tasks[0], 0) == 1);
    CHECK(p4_task_registry_wait_operation_finish_cancel_locked(
        &fixture.registry, &fixture.ledger, fixture.tasks[0],
        generation[0]) == 1);
    CHECK(p4_task_registry_claim_reclaim_locked(&fixture.registry,
        fixture.tasks[0]) == &fixture.records[0]);
    empty_sem(&sem);
    free(external);
}

int main(void)
{
    long page_size_long = sysconf(_SC_PAGESIZE);
    CHECK(page_size_long > 0);
    task_page_size = (size_t)page_size_long;
    task_page = mmap(NULL, task_page_size, PROT_NONE,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(task_page != MAP_FAILED);
    CHECK((uintptr_t)task_page % 32u == 0);
    test_both_hart_immediate_acquire();
    test_fragmented_absolute_slot_reuse();
    test_recursive_and_shared_semantics();
    test_fused_cancel_unlinks_and_rolls_back();
    test_no_running_refusal_and_harts();
    test_corrupt_lists_and_aliases();
    test_pending_owned_nest_mismatch();
    test_nest_overflow_refusal();
    test_queue_length_boundary();
    CHECK(munmap(task_page, task_page_size) == 0);
    printf("p4 semaphore real-node checks: %u\n", checks);
    return 0;
}
