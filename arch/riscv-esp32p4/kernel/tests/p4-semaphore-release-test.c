#define main p4_semaphore_nodes_baseline_main
#include "p4-semaphore-nodes-test.c"
#undef main

static uint32_t queue_group(struct Fixture *fixture,
    struct SignalSemaphore *sem, unsigned task, uint32_t count,
    const uint32_t modes[])
{
    struct SignalSemaphore *semaphores[TEST_ENTRIES];
    uint32_t i, generation;
    for (i = 0; i < count; ++i)
        semaphores[i] = sem;
    register_running(fixture, task, 1);
    generation = make_operation(fixture, task, count, semaphores, modes);
    for (i = 0; i < count; ++i)
        CHECK(p4_semaphore_nodes_acquire_locked(&fixture->registry,
            &fixture->ledger, fixture->requests, fixture->tasks[task],
            generation, i, sem) == 2);
    CHECK(p4_task_registry_leave_running_locked(&fixture->registry,
        fixture->tasks[task], 1) == 1);
    return generation;
}

static uint32_t start_holder(struct Fixture *fixture,
    struct SignalSemaphore *sem)
{
    struct SignalSemaphore *semaphores[1] = { sem };
    const uint32_t shared[1] = { 0 };
    uint32_t generation;
    register_running(fixture, 0, 0);
    generation = make_operation(fixture, 0, 1, semaphores, shared);
    CHECK(p4_semaphore_nodes_acquire_locked(&fixture->registry,
        &fixture->ledger, fixture->requests, fixture->tasks[0], generation,
        0, sem) == 1);
    CHECK(p4_task_registry_wait_operation_complete_locked(&fixture->registry,
        &fixture->ledger, fixture->tasks[0], generation) == 1);
    CHECK(sem->ss_NestCount == 1 && sem->ss_QueueCount == 0);
    CHECK(sem->ss_Owner == fixture->tasks[0]);
    return generation;
}

static uint32_t acquire_completed_again(struct Fixture *fixture,
    struct SignalSemaphore *sem, unsigned task)
{
    struct SignalSemaphore *semaphores[1] = { sem };
    const uint32_t shared[1] = { 0 };
    uint32_t generation = make_operation(fixture, task, 1, semaphores, shared);
    CHECK(p4_semaphore_nodes_acquire_locked(&fixture->registry,
        &fixture->ledger, fixture->requests, fixture->tasks[task], generation,
        0, sem) == 1);
    CHECK(p4_task_registry_wait_operation_complete_locked(&fixture->registry,
        &fixture->ledger, fixture->tasks[task], generation) == 1);
    return generation;
}

static void test_completed_nonlast_and_empty_last_release(void)
{
    struct Fixture fixture;
    struct SignalSemaphore sem;
    struct Task *targets[2] = { task_identity(7), task_identity(7) };
    struct Task *first_target = targets[0];
    struct Task *second_target = targets[1];
    empty_sem(&sem);
    fixture_init(&fixture);
    (void)start_holder(&fixture, &sem);
    (void)acquire_completed_again(&fixture, &sem, 0);
    CHECK(sem.ss_NestCount == 2 && sem.ss_QueueCount == 1);
    CHECK(sem.ss_Owner == fixture.tasks[0]);

    /* A completed/recursive unit can be released without any notification
     * storage because another actual unit remains. */
    CHECK(p4_semaphore_nodes_release_locked(&fixture.registry,
        &fixture.ledger, fixture.requests, &sem, NULL, 0) == 1);
    CHECK(sem.ss_NestCount == 1 && sem.ss_QueueCount == 0);
    CHECK(sem.ss_Owner == fixture.tasks[0]);

    /* The final empty-queue release still requires valid target storage, but
     * does not write any target slot. */
    CHECK(p4_semaphore_nodes_release_locked(&fixture.registry,
        &fixture.ledger, fixture.requests, &sem, targets, 2) == 1);
    CHECK(sem.ss_NestCount == 0 && sem.ss_QueueCount == -1);
    CHECK(sem.ss_Owner == NULL);
    CHECK(targets[0] == first_target && targets[1] == second_target);
}

static void test_completed_then_pending_release_restart(void)
{
    struct Fixture fixture;
    struct SignalSemaphore sem;
    struct SignalSemaphore *semaphores[1] = { &sem };
    const uint32_t shared[1] = { 0 };
    struct Task *targets[2] = { task_identity(7), task_identity(7) };
    struct Task *target_sentinel = targets[0];
    uint32_t generation;

    empty_sem(&sem);
    fixture_init(&fixture);
    (void)start_holder(&fixture, &sem); /* One completed unit. */
    generation = make_operation(&fixture, 0, 1, semaphores, shared);
    CHECK(p4_semaphore_nodes_acquire_locked(&fixture.registry,
        &fixture.ledger, fixture.requests, fixture.tasks[0], generation,
        0, &sem) == 1);
    CHECK(sem.ss_NestCount == 2 && sem.ss_QueueCount == 1);
    CHECK(fixture.ledger.entries[0].state ==
        P4_SEMAPHORE_LEDGER_ENTRY_OWNED);

    /* Anonymous releases consume an indistinguishable completed unit first. */
    CHECK(p4_semaphore_nodes_release_locked(&fixture.registry,
        &fixture.ledger, fixture.requests, &sem, NULL, 0) == 1);
    CHECK(sem.ss_NestCount == 1 && sem.ss_QueueCount == 0);
    CHECK(fixture.ledger.entries[0].state ==
        P4_SEMAPHORE_LEDGER_ENTRY_OWNED);

    /* With all remaining nest represented by an uncommitted OWNED entry, the
     * actual release marks it RELEASED and reaches zero without a handoff. */
    CHECK(p4_semaphore_nodes_release_locked(&fixture.registry,
        &fixture.ledger, fixture.requests, &sem, targets, 2) == 1);
    CHECK(sem.ss_NestCount == 0 && sem.ss_QueueCount == -1);
    CHECK(sem.ss_Owner == NULL);
    CHECK(fixture.ledger.entries[0].state ==
        P4_SEMAPHORE_LEDGER_ENTRY_RELEASED);
    CHECK(targets[0] == target_sentinel && targets[1] == task_identity(7));
    CHECK(p4_task_registry_wait_operation_complete_locked(&fixture.registry,
        &fixture.ledger, fixture.tasks[0], generation) == 0);
    CHECK(fixture.records[0].wait_scope.active == 1);

    CHECK(p4_semaphore_ledger_restart_released(&fixture.ledger,
        &fixture.operations[0], 0) == 1);
    CHECK(fixture.ledger.entries[0].state ==
        P4_SEMAPHORE_LEDGER_ENTRY_UNVISITED);
    CHECK(p4_semaphore_nodes_acquire_locked(&fixture.registry,
        &fixture.ledger, fixture.requests, fixture.tasks[0], generation,
        0, &sem) == 1);
    CHECK(sem.ss_NestCount == 1 && sem.ss_QueueCount == 0);
    CHECK(sem.ss_Owner == fixture.tasks[0]);
    CHECK(fixture.ledger.entries[0].state ==
        P4_SEMAPHORE_LEDGER_ENTRY_OWNED);
    CHECK(p4_task_registry_wait_operation_complete_locked(&fixture.registry,
        &fixture.ledger, fixture.tasks[0], generation) == 1);
    CHECK(sem.ss_NestCount == 1 && sem.ss_Owner == fixture.tasks[0]);
}

static void test_actual_shared_cohort_release(void)
{
    struct Fixture fixture;
    struct SignalSemaphore sem;
    const uint32_t shared[1] = { 1 };
    const uint32_t exclusive[1] = { 0 };
    struct Task *targets[4] = {
        task_identity(7), task_identity(7), task_identity(7), task_identity(7)
    };
    WORD initial_queue_count;
    uint32_t generation[4];

    empty_sem(&sem);
    fixture_init(&fixture);
    (void)start_holder(&fixture, &sem);
    generation[1] = queue_group(&fixture, &sem, 1, 1, shared);
    generation[2] = queue_group(&fixture, &sem, 2, 1, exclusive);
    generation[3] = queue_group(&fixture, &sem, 3, 1, shared);
    initial_queue_count = sem.ss_QueueCount;
    CHECK(sem.ss_NestCount == 1 && queue_links(&sem) == 3);

    /* Release itself performs the last-unit count transition and the entire
     * interspersed S1,E,S2 shared cohort handoff. */
    CHECK(p4_semaphore_nodes_release_locked(&fixture.registry,
        &fixture.ledger, fixture.requests, &sem, targets, 4) == 3);
    CHECK(targets[0] == fixture.tasks[1] && targets[1] == fixture.tasks[3]);
    CHECK(targets[2] == task_identity(7) && targets[3] == task_identity(7));
    CHECK(sem.ss_NestCount == 2 && sem.ss_Owner == NULL);
    CHECK(sem.ss_QueueCount == (WORD)(initial_queue_count - 1));
    CHECK(queue_links(&sem) == 1);
    CHECK(sem.ss_WaitQueue.mlh_Head == &fixture.requests[1].sr_Link);
    CHECK(fixture.ledger.entries[0].state ==
        P4_SEMAPHORE_LEDGER_ENTRY_OWNED);
    CHECK(fixture.ledger.entries[1].state ==
        P4_SEMAPHORE_LEDGER_ENTRY_QUEUED);
    CHECK(fixture.ledger.entries[2].state ==
        P4_SEMAPHORE_LEDGER_ENTRY_OWNED);
    CHECK(fixture.records[1].wait_scope.queued_count == 0);
    CHECK(fixture.records[2].wait_scope.queued_count == 1);
    CHECK(fixture.records[3].wait_scope.queued_count == 0);
    CHECK(fixture.records[1].lifetime.notification_pins == 2);
    CHECK(fixture.records[3].lifetime.notification_pins == 2);

    CHECK(p4_task_registry_wait_operation_complete_locked(&fixture.registry,
        &fixture.ledger, fixture.tasks[1], generation[1]) == 1);
    CHECK(p4_task_registry_wait_operation_complete_locked(&fixture.registry,
        &fixture.ledger, fixture.tasks[3], generation[3]) == 1);
    CHECK(p4_task_registry_unpin_locked(&fixture.registry, targets[0]) == 1);
    CHECK(p4_task_registry_unpin_locked(&fixture.registry, targets[1]) == 1);
    CHECK(fixture.records[1].lifetime.notification_pins == 0);
    CHECK(fixture.records[3].lifetime.notification_pins == 0);
}

static void test_actual_exclusive_head_release(void)
{
    struct Fixture fixture;
    struct SignalSemaphore sem;
    const uint32_t exclusive[1] = { 0 };
    const uint32_t shared[1] = { 1 };
    struct Task *targets[3] = { task_identity(7), task_identity(7), task_identity(7) };
    uint32_t generation;

    empty_sem(&sem);
    fixture_init(&fixture);
    (void)start_holder(&fixture, &sem);
    generation = queue_group(&fixture, &sem, 1, 1, exclusive);
    (void)queue_group(&fixture, &sem, 2, 1, shared);
    CHECK(p4_semaphore_nodes_release_locked(&fixture.registry,
        &fixture.ledger, fixture.requests, &sem, targets, 3) == 2);
    CHECK(targets[0] == fixture.tasks[1]);
    CHECK(targets[1] == task_identity(7) && targets[2] == task_identity(7));
    CHECK(sem.ss_NestCount == 1 && sem.ss_Owner == fixture.tasks[1]);
    CHECK(sem.ss_QueueCount == 1 && queue_links(&sem) == 1);
    CHECK(sem.ss_WaitQueue.mlh_Head == &fixture.requests[1].sr_Link);
    CHECK(fixture.records[1].wait_scope.queued_count == 0);
    CHECK(fixture.records[1].lifetime.notification_pins == 2);
    CHECK(p4_task_registry_wait_operation_complete_locked(&fixture.registry,
        &fixture.ledger, fixture.tasks[1], generation) == 1);
    CHECK(p4_task_registry_unpin_locked(&fixture.registry, targets[0]) == 1);
    CHECK(fixture.records[1].lifetime.notification_pins == 0);
}

static void setup_two_shared_waiters(struct Fixture *fixture,
    struct SignalSemaphore *sem)
{
    const uint32_t shared[1] = { 1 };
    empty_sem(sem);
    fixture_init(fixture);
    (void)start_holder(fixture, sem);
    (void)queue_group(fixture, sem, 1, 1, shared);
    (void)queue_group(fixture, sem, 2, 1, shared);
    CHECK(sem->ss_NestCount == 1 && queue_links(sem) == 2);
}

static void check_release_refusal_unchanged(struct Fixture *fixture,
    struct SignalSemaphore *sem, struct Task **targets, uint32_t capacity,
    int expected)
{
    struct Fixture before = *fixture;
    struct SignalSemaphore sem_before = *sem;
    CHECK(p4_semaphore_nodes_release_locked(&fixture->registry,
        &fixture->ledger, fixture->requests, sem, targets, capacity) == expected);
    CHECK(memcmp(fixture, &before, sizeof(before)) == 0);
    CHECK(memcmp(sem, &sem_before, sizeof(sem_before)) == 0);
}

static void test_last_release_refusals_are_atomic(void)
{
    struct Fixture fixture;
    struct SignalSemaphore sem;
    struct Task *targets[4] = {
        task_identity(7), task_identity(7), task_identity(7), task_identity(7)
    };
    struct Task *sentinel = targets[0];
    void *aliases[5];
    unsigned i;

    setup_two_shared_waiters(&fixture, &sem);
    check_release_refusal_unchanged(&fixture, &sem, targets, 1, 0);
    CHECK(targets[0] == sentinel && targets[1] == sentinel &&
        targets[2] == sentinel && targets[3] == sentinel);

    aliases[0] = &fixture.registry;
    aliases[1] = &fixture.ledger;
    aliases[2] = fixture.operations;
    aliases[3] = fixture.entries;
    aliases[4] = fixture.requests;
    for (i = 0; i < 5; ++i)
        check_release_refusal_unchanged(&fixture, &sem,
            (struct Task **)aliases[i], 2, -1);

    /* One prospective notification pin per shared grant; overflow refuses
     * before the old holder is released or queue nodes are detached. */
    fixture.records[1].lifetime.notification_pins = UINT32_MAX;
    check_release_refusal_unchanged(&fixture, &sem, targets, 4, 0);
    CHECK(targets[0] == sentinel && targets[1] == sentinel &&
        targets[2] == sentinel && targets[3] == sentinel);

    setup_two_shared_waiters(&fixture, &sem);
    CHECK(p4_task_registry_retire_with_ledger_locked(&fixture.registry,
        &fixture.ledger, fixture.tasks[1], 0) == 1);
    check_release_refusal_unchanged(&fixture, &sem, targets, 4, 0);
    CHECK(targets[0] == sentinel && targets[1] == sentinel &&
        targets[2] == sentinel && targets[3] == sentinel);
}

static void test_unknown_external_node_refuses_release_without_overlay_read(void)
{
    struct Fixture fixture;
    struct SignalSemaphore sem;
    const uint32_t shared[1] = { 1 };
    struct Task *targets[2] = { task_identity(7), task_identity(7) };
    struct Task *sentinel = targets[0];
    struct Fixture before;
    struct SignalSemaphore sem_before;
    long page_size_long = sysconf(_SC_PAGESIZE);
    size_t page_size;
    unsigned char *pair;
    struct MinNode *external, *head, *tail;

    CHECK(page_size_long > 0);
    page_size = (size_t)page_size_long;
    pair = mmap(NULL, page_size * 2u, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(pair != MAP_FAILED);
    CHECK(mprotect(pair + page_size, page_size, PROT_NONE) == 0);
    external = (struct MinNode *)(void *)(pair + page_size - sizeof(*external));

    empty_sem(&sem);
    fixture_init(&fixture);
    (void)start_holder(&fixture, &sem);
    (void)queue_group(&fixture, &sem, 1, 1, shared);
    head = (struct MinNode *)(void *)&sem.ss_WaitQueue.mlh_Head;
    tail = (struct MinNode *)(void *)&sem.ss_WaitQueue.mlh_Tail;
    external->mln_Pred = head;
    external->mln_Succ = sem.ss_WaitQueue.mlh_Head;
    sem.ss_WaitQueue.mlh_Head->mln_Pred = external;
    sem.ss_WaitQueue.mlh_Head = external;
    ++sem.ss_QueueCount;
    CHECK(sem.ss_NestCount == 1 && queue_links(&sem) == 2);
    CHECK(sem.ss_WaitQueue.mlh_TailPred != tail);
    before = fixture;
    sem_before = sem;
    CHECK(p4_semaphore_nodes_release_locked(&fixture.registry,
        &fixture.ledger, fixture.requests, &sem, targets, 2) == -1);
    CHECK(memcmp(&before, &fixture, sizeof(before)) == 0);
    CHECK(memcmp(&sem_before, &sem, sizeof(sem)) == 0);
    CHECK(targets[0] == sentinel && targets[1] == task_identity(7));
    CHECK(munmap(pair, page_size * 2u) == 0);
}

static uint32_t acquire_owned_group(struct Fixture *fixture,
    struct SignalSemaphore *sem, unsigned task, uint32_t count)
{
    struct SignalSemaphore *semaphores[TEST_ENTRIES];
    uint32_t modes[TEST_ENTRIES];
    uint32_t i, generation;
    for (i = 0; i < count; ++i)
    {
        semaphores[i] = sem;
        modes[i] = 0;
    }
    register_running(fixture, task, task & 1u);
    generation = make_operation(fixture, task, count, semaphores, modes);
    for (i = 0; i < count; ++i)
        CHECK(p4_semaphore_nodes_acquire_locked(&fixture->registry,
            &fixture->ledger, fixture->requests, fixture->tasks[task],
            generation, i, sem) == 1);
    return generation;
}

static void test_cancelled_released_owned_unit_finishes_without_rollback(void)
{
    struct Fixture fixture;
    struct SignalSemaphore owned_sem, queued_sem;
    struct SignalSemaphore *semaphores[2] = { &owned_sem, &queued_sem };
    const uint32_t modes[2] = { 0, 0 };
    struct p4_task_wait_operation wait;
    struct p4_task_wait_cancel_snapshot snapshot;
    struct Task *targets[2] = { task_identity(7), task_identity(7) };
    struct Fixture before;
    struct SignalSemaphore owned_before, queued_before;
    uint32_t generation;

    empty_sem(&owned_sem);
    empty_sem(&queued_sem);
    fixture_init(&fixture);
    (void)start_holder(&fixture, &queued_sem);
    register_running(&fixture, 1, 1);
    CHECK(p4_task_registry_wait_reserve_operation_locked(&fixture.registry,
        &fixture.ledger, fixture.tasks[1], 2, &wait) == 1);
    generation = wait.generation;
    CHECK(p4_semaphore_nodes_configure_locked(&fixture.registry,
        &fixture.ledger, fixture.requests, fixture.tasks[1], generation,
        0, semaphores[0], modes[0]) == 1);
    CHECK(p4_semaphore_nodes_configure_locked(&fixture.registry,
        &fixture.ledger, fixture.requests, fixture.tasks[1], generation,
        1, semaphores[1], modes[1]) == 1);
    CHECK(p4_task_registry_wait_operation_close_locked(&fixture.registry,
        &fixture.ledger, fixture.tasks[1], generation) == 1);
    CHECK(p4_semaphore_nodes_acquire_locked(&fixture.registry,
        &fixture.ledger, fixture.requests, fixture.tasks[1], generation,
        0, &owned_sem) == 1);
    CHECK(p4_semaphore_nodes_acquire_locked(&fixture.registry,
        &fixture.ledger, fixture.requests, fixture.tasks[1], generation,
        1, &queued_sem) == 2);
    CHECK(fixture.ledger.entries[0].state ==
        P4_SEMAPHORE_LEDGER_ENTRY_OWNED);
    CHECK(fixture.ledger.entries[1].state ==
        P4_SEMAPHORE_LEDGER_ENTRY_QUEUED);
    CHECK(fixture.records[1].wait_scope.queued_count == 1);

    CHECK(p4_task_registry_retire_with_ledger_locked(&fixture.registry,
        &fixture.ledger, fixture.tasks[1], 0) == 1);
    CHECK(p4_task_registry_claim_next_wait_cancel_with_ledger_locked(
        &fixture.registry, &fixture.ledger, &snapshot) == 1);
    CHECK(snapshot.identity == fixture.tasks[1]);

    /* A different legal Release consumes the now-retired operation's owned
     * unit. Its ledger state becomes RELEASED before cleanup finishes. */
    CHECK(p4_semaphore_nodes_release_locked(&fixture.registry,
        &fixture.ledger, fixture.requests, &owned_sem, targets, 2) == 1);
    CHECK(owned_sem.ss_NestCount == 0 && owned_sem.ss_QueueCount == -1);
    CHECK(owned_sem.ss_Owner == NULL);
    CHECK(fixture.ledger.entries[0].state ==
        P4_SEMAPHORE_LEDGER_ENTRY_RELEASED);
    CHECK(targets[0] == task_identity(7) && targets[1] == task_identity(7));

    CHECK(p4_semaphore_nodes_withdraw_locked(&fixture.registry,
        &fixture.ledger, fixture.requests, fixture.tasks[1], generation,
        1, &queued_sem) == 1);
    CHECK(queued_sem.ss_NestCount == 1 && queued_sem.ss_QueueCount == 0);
    CHECK(queue_links(&queued_sem) == 0);
    CHECK(fixture.records[1].wait_scope.queued_count == 0);

    /* A released ledger entry is not rolled back a second time. FinishCancel
     * must wait for RUN drain, then reclaim this RELEASED+WITHDRAWN operation. */
    before = fixture;
    owned_before = owned_sem;
    queued_before = queued_sem;
    CHECK(p4_task_registry_wait_operation_finish_cancel_locked(
        &fixture.registry, &fixture.ledger, fixture.tasks[1], generation) == 0);
    CHECK(memcmp(&fixture, &before, sizeof(fixture)) == 0);
    CHECK(memcmp(&owned_sem, &owned_before, sizeof(owned_sem)) == 0);
    CHECK(memcmp(&queued_sem, &queued_before, sizeof(queued_sem)) == 0);

    CHECK(p4_task_registry_leave_running_locked(&fixture.registry,
        fixture.tasks[1], 1) == 1);
    before = fixture;
    owned_before = owned_sem;
    CHECK(p4_semaphore_nodes_rollback_locked(&fixture.registry,
        &fixture.ledger, fixture.requests, fixture.tasks[1], generation,
        0, &owned_sem) == 0);
    CHECK(memcmp(&fixture, &before, sizeof(fixture)) == 0);
    CHECK(memcmp(&owned_sem, &owned_before, sizeof(owned_sem)) == 0);

    CHECK(p4_task_registry_wait_operation_finish_cancel_locked(
        &fixture.registry, &fixture.ledger, fixture.tasks[1], generation) == 1);
    CHECK(fixture.records[1].wait_scope.active == 0);
    CHECK(fixture.ledger.active_operations == 0);
    CHECK(fixture.ledger.active_entries == 0);
    CHECK(owned_sem.ss_NestCount == 0 && owned_sem.ss_QueueCount == -1);
    CHECK(owned_sem.ss_Owner == NULL);
    CHECK(queued_sem.ss_NestCount == 1 && queued_sem.ss_QueueCount == 0);
}

static void test_committed_owned_count_mismatch_refuses_unchanged(void)
{
    struct Fixture fixture;
    struct SignalSemaphore sem;
    struct Task *targets[3] = { task_identity(7), task_identity(7), task_identity(7) };
    struct Fixture before;
    struct SignalSemaphore sem_before;
    uint32_t generation;

    /* Non-last release: committed raw OWNED ledger entries are completed
     * units, but cannot outnumber the actual semaphore nest. */
    empty_sem(&sem);
    fixture_init(&fixture);
    generation = acquire_owned_group(&fixture, &sem, 0, 3);
    CHECK(p4_semaphore_ledger_commit(&fixture.ledger,
        &fixture.operations[0]) == 1);
    CHECK(fixture.ledger.operations[0].committed == 1);
    sem.ss_NestCount = 2;
    sem.ss_QueueCount = 1; /* Preserve valid empty intrusive queue counts. */
    before = fixture;
    sem_before = sem;
    CHECK(p4_semaphore_nodes_release_locked(&fixture.registry,
        &fixture.ledger, fixture.requests, &sem, targets, 3) == -1);
    CHECK(memcmp(&fixture, &before, sizeof(fixture)) == 0);
    CHECK(memcmp(&sem, &sem_before, sizeof(sem)) == 0);
    CHECK(targets[0] == task_identity(7) && targets[1] == task_identity(7) &&
        targets[2] == task_identity(7));
    (void)generation;

    /* Last-unit release must reject the same mismatch before it drops the
     * prior owner or touches caller output. */
    empty_sem(&sem);
    fixture_init(&fixture);
    (void)acquire_owned_group(&fixture, &sem, 0, 2);
    CHECK(p4_semaphore_ledger_commit(&fixture.ledger,
        &fixture.operations[0]) == 1);
    sem.ss_NestCount = 1;
    sem.ss_QueueCount = 0;
    before = fixture;
    sem_before = sem;
    CHECK(p4_semaphore_nodes_release_locked(&fixture.registry,
        &fixture.ledger, fixture.requests, &sem, targets, 3) == -1);
    CHECK(memcmp(&fixture, &before, sizeof(fixture)) == 0);
    CHECK(memcmp(&sem, &sem_before, sizeof(sem)) == 0);
    CHECK(targets[0] == task_identity(7) && targets[1] == task_identity(7) &&
        targets[2] == task_identity(7));

    /* Grant's empty-queue fast path must also account for indexed OWNED
     * entries when Nest is corruptly zero. */
    empty_sem(&sem);
    fixture_init(&fixture);
    (void)acquire_owned_group(&fixture, &sem, 0, 1);
    CHECK(p4_semaphore_ledger_commit(&fixture.ledger,
        &fixture.operations[0]) == 1);
    sem.ss_NestCount = 0;
    sem.ss_QueueCount = -1;
    before = fixture;
    sem_before = sem;
    CHECK(p4_semaphore_nodes_grant_cohort_locked(&fixture.registry,
        &fixture.ledger, fixture.requests, &sem, targets, 3) == -1);
    CHECK(memcmp(&fixture, &before, sizeof(fixture)) == 0);
    CHECK(memcmp(&sem, &sem_before, sizeof(sem)) == 0);
    CHECK(targets[0] == task_identity(7) && targets[1] == task_identity(7) &&
        targets[2] == task_identity(7));
}

int main(void)
{
    long page_size_long;
    CHECK(p4_semaphore_nodes_baseline_main() == 0);
    checks = 0;
    page_size_long = sysconf(_SC_PAGESIZE);
    CHECK(page_size_long > 0);
    task_page_size = (size_t)page_size_long;
    task_page = mmap(NULL, task_page_size, PROT_NONE,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(task_page != MAP_FAILED);
    CHECK((uintptr_t)task_page % 32u == 0);

    test_completed_nonlast_and_empty_last_release();
    test_completed_then_pending_release_restart();
    test_actual_shared_cohort_release();
    test_actual_exclusive_head_release();
    test_last_release_refusals_are_atomic();
    test_unknown_external_node_refuses_release_without_overlay_read();
    test_cancelled_released_owned_unit_finishes_without_rollback();
    test_committed_owned_count_mismatch_refuses_unchanged();

    CHECK(munmap(task_page, task_page_size) == 0);
    printf("p4 semaphore release checks: %u\n", checks);
    return 0;
}
