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
    const uint32_t modes[1] = { 0 };
    uint32_t generation;
    register_running(fixture, 0, 0);
    generation = make_operation(fixture, 0, 1, semaphores, modes);
    CHECK(p4_semaphore_nodes_acquire_locked(&fixture->registry,
        &fixture->ledger, fixture->requests, fixture->tasks[0], generation,
        0, sem) == 1);
    CHECK(p4_task_registry_wait_operation_complete_locked(&fixture->registry,
        &fixture->ledger, fixture->tasks[0], generation) == 1);
    CHECK(sem->ss_NestCount == 1 && sem->ss_QueueCount == 0);
    CHECK(sem->ss_Owner == fixture->tasks[0]);
    return generation;
}

static void simulate_last_release(struct SignalSemaphore *sem,
    uint32_t queued)
{
    CHECK(sem->ss_NestCount == 1);
    CHECK(sem->ss_QueueCount == (WORD)queued);
    CHECK(queue_links(sem) == queued);
    sem->ss_NestCount = 0;
    sem->ss_Owner = NULL;
    --sem->ss_QueueCount;
    CHECK((uint32_t)sem->ss_QueueCount + 1u == queued);
}

static void test_shared_head_cohort_and_target_lifetime(void)
{
    struct Fixture fixture;
    struct SignalSemaphore sem, later;
    const uint32_t shared[1] = { 1 };
    const uint32_t exclusive[1] = { 0 };
    struct SignalSemaphore *later_entries[1] = { &later };
    struct Task *targets[4];
    struct Task *target_sentinel = task_identity(7);
    uint32_t generation[4];
    WORD released_queue_count;
    empty_sem(&sem);
    empty_sem(&later);
    fixture_init(&fixture);
    (void)start_holder(&fixture, &sem);
    generation[1] = queue_group(&fixture, &sem, 1, 1, shared);
    generation[2] = queue_group(&fixture, &sem, 2, 1, exclusive);
    generation[3] = queue_group(&fixture, &sem, 3, 1, shared);
    CHECK(queue_links(&sem) == 3);
    CHECK(fixture.records[1].wait_scope.queued_count == 1);
    CHECK(fixture.records[2].wait_scope.queued_count == 1);
    CHECK(fixture.records[3].wait_scope.queued_count == 1);
    simulate_last_release(&sem, 3);
    released_queue_count = sem.ss_QueueCount;
    targets[0] = target_sentinel;
    targets[1] = target_sentinel;
    targets[2] = target_sentinel;
    targets[3] = target_sentinel;

    CHECK(p4_semaphore_nodes_grant_cohort_locked(&fixture.registry,
        &fixture.ledger, fixture.requests, &sem, targets, 4) == 2);
    CHECK(targets[0] == fixture.tasks[1]);
    CHECK(targets[1] == fixture.tasks[3]);
    CHECK(targets[2] == target_sentinel && targets[3] == target_sentinel);
    CHECK(sem.ss_NestCount == 2);
    CHECK(sem.ss_Owner == NULL);
    CHECK(sem.ss_QueueCount == released_queue_count);
    CHECK(queue_links(&sem) == 1);
    CHECK(sem.ss_WaitQueue.mlh_Head == &fixture.requests[1].sr_Link);
    CHECK(fixture.requests[1].sr_Link.mln_Pred ==
        (struct MinNode *)(void *)&sem.ss_WaitQueue.mlh_Head);
    CHECK(fixture.requests[1].sr_Link.mln_Succ ==
        (struct MinNode *)(void *)&sem.ss_WaitQueue.mlh_Tail);
    CHECK(fixture.requests[0].sr_Link.mln_Succ == NULL);
    CHECK(fixture.requests[0].sr_Link.mln_Pred == NULL);
    CHECK(fixture.requests[2].sr_Link.mln_Succ == NULL);
    CHECK(fixture.requests[2].sr_Link.mln_Pred == NULL);
    CHECK(fixture.ledger.entries[0].state == P4_SEMAPHORE_LEDGER_ENTRY_OWNED);
    CHECK(fixture.ledger.entries[1].state == P4_SEMAPHORE_LEDGER_ENTRY_QUEUED);
    CHECK(fixture.ledger.entries[2].state == P4_SEMAPHORE_LEDGER_ENTRY_OWNED);
    CHECK(fixture.records[1].wait_scope.queued_count == 0);
    CHECK(fixture.records[2].wait_scope.queued_count == 1);
    CHECK(fixture.records[3].wait_scope.queued_count == 0);
    CHECK(fixture.records[1].lifetime.notification_pins == 2);
    CHECK(fixture.records[2].lifetime.notification_pins == 1);
    CHECK(fixture.records[3].lifetime.notification_pins == 2);

    /* Complete/reclaim the real request nodes before consuming the copied
     * notification targets; only the ordinary notification pins remain. */
    CHECK(p4_task_registry_wait_operation_complete_locked(&fixture.registry,
        &fixture.ledger, fixture.tasks[1], generation[1]) == 1);
    CHECK(p4_task_registry_wait_operation_complete_locked(&fixture.registry,
        &fixture.ledger, fixture.tasks[3], generation[3]) == 1);
    CHECK(fixture.records[1].lifetime.notification_pins == 1);
    CHECK(fixture.records[3].lifetime.notification_pins == 1);
    CHECK(fixture.records[1].wait_scope.active == 0);
    CHECK(fixture.records[3].wait_scope.active == 0);
    CHECK(fixture.ledger.entries[0].state == P4_SEMAPHORE_LEDGER_ENTRY_FREE);
    CHECK(fixture.ledger.entries[2].state == P4_SEMAPHORE_LEDGER_ENTRY_FREE);

    /* Reuse S1's former absolute node slot while its copied identity remains
     * protected by the ordinary notification pin. */
    CHECK(p4_task_registry_enter_running_locked(&fixture.registry,
        fixture.tasks[1], 1) == 1);
    generation[1] = make_operation(&fixture, 1, 1, later_entries, exclusive);
    CHECK(p4_semaphore_nodes_acquire_locked(&fixture.registry,
        &fixture.ledger, fixture.requests, fixture.tasks[1], generation[1],
        0, &later) == 1);
    CHECK(targets[0] == fixture.tasks[1]);
    CHECK(fixture.entries[0].request == &fixture.requests[0]);
    CHECK(fixture.entries[0].semaphore == &later);
    CHECK(p4_task_registry_wait_operation_complete_locked(&fixture.registry,
        &fixture.ledger, fixture.tasks[1], generation[1]) == 1);
    CHECK(fixture.records[1].lifetime.notification_pins == 1);
    CHECK(p4_task_registry_leave_running_locked(&fixture.registry,
        fixture.tasks[1], 1) == 1);
    CHECK(p4_task_registry_unpin_locked(&fixture.registry, targets[0]) == 1);
    CHECK(p4_task_registry_unpin_locked(&fixture.registry, targets[1]) == 1);
    CHECK(fixture.records[1].lifetime.notification_pins == 0);
    CHECK(fixture.records[3].lifetime.notification_pins == 0);
}

static void test_exclusive_head_only(void)
{
    struct Fixture fixture;
    struct SignalSemaphore sem;
    const uint32_t exclusive[1] = { 0 };
    const uint32_t shared[1] = { 1 };
    struct Task *targets[3] = { NULL, NULL, NULL };
    uint32_t generation[3];
    WORD released_queue_count;
    empty_sem(&sem);
    fixture_init(&fixture);
    (void)start_holder(&fixture, &sem);
    generation[1] = queue_group(&fixture, &sem, 1, 1, exclusive);
    generation[2] = queue_group(&fixture, &sem, 2, 1, shared);
    simulate_last_release(&sem, 2);
    released_queue_count = sem.ss_QueueCount;
    CHECK(p4_semaphore_nodes_grant_cohort_locked(&fixture.registry,
        &fixture.ledger, fixture.requests, &sem, targets, 3) == 1);
    CHECK(targets[0] == fixture.tasks[1]);
    CHECK(targets[1] == NULL && targets[2] == NULL);
    CHECK(sem.ss_NestCount == 1);
    CHECK(sem.ss_Owner == fixture.tasks[1]);
    CHECK(sem.ss_QueueCount == released_queue_count);
    CHECK(queue_links(&sem) == 1);
    CHECK(sem.ss_WaitQueue.mlh_Head == &fixture.requests[1].sr_Link);
    CHECK(fixture.ledger.entries[0].state == P4_SEMAPHORE_LEDGER_ENTRY_OWNED);
    CHECK(fixture.ledger.entries[1].state == P4_SEMAPHORE_LEDGER_ENTRY_QUEUED);
    CHECK(fixture.records[1].wait_scope.queued_count == 0);
    CHECK(fixture.records[2].wait_scope.queued_count == 1);
    CHECK(fixture.records[1].lifetime.notification_pins == 2);
    CHECK(p4_task_registry_wait_operation_complete_locked(&fixture.registry,
        &fixture.ledger, fixture.tasks[1], generation[1]) == 1);
    CHECK(fixture.records[1].lifetime.notification_pins == 1);
    CHECK(p4_task_registry_unpin_locked(&fixture.registry, targets[0]) == 1);
    CHECK(fixture.records[1].lifetime.notification_pins == 0);
}

static void test_duplicate_shared_capacity_overflow_and_complete(void)
{
    struct Fixture fixture;
    struct SignalSemaphore sem;
    struct SignalSemaphore *same[2] = { &sem, &sem };
    const uint32_t shared[2] = { 1, 1 };
    struct Task *targets[4];
    struct Task *sentinel = task_identity(6);
    struct Fixture before;
    struct SignalSemaphore sem_before;
    uint32_t generation;
    unsigned alias;
    void *aliases[7];
    empty_sem(&sem);
    fixture_init(&fixture);
    (void)start_holder(&fixture, &sem);
    register_running(&fixture, 1, 1);
    generation = make_operation(&fixture, 1, 2, same, shared);
    CHECK(p4_semaphore_nodes_acquire_locked(&fixture.registry,
        &fixture.ledger, fixture.requests, fixture.tasks[1], generation,
        0, &sem) == 2);
    CHECK(p4_semaphore_nodes_acquire_locked(&fixture.registry,
        &fixture.ledger, fixture.requests, fixture.tasks[1], generation,
        1, &sem) == 2);
    CHECK(p4_task_registry_leave_running_locked(&fixture.registry,
        fixture.tasks[1], 1) == 1);
    CHECK(queue_links(&sem) == 2);
    simulate_last_release(&sem, 2);
    targets[0] = sentinel;
    targets[1] = sentinel;
    targets[2] = sentinel;
    targets[3] = sentinel;

    before = fixture;
    sem_before = sem;
    CHECK(p4_semaphore_nodes_grant_cohort_locked(&fixture.registry,
        &fixture.ledger, fixture.requests, &sem, targets, 1) == 0);
    CHECK(memcmp(&before, &fixture, sizeof(fixture)) == 0);
    CHECK(memcmp(&sem_before, &sem, sizeof(sem)) == 0);
    CHECK(targets[0] == sentinel && targets[1] == sentinel &&
        targets[2] == sentinel && targets[3] == sentinel);

    aliases[0] = &fixture.registry;
    aliases[1] = &fixture.ledger;
    aliases[2] = fixture.operations;
    aliases[3] = fixture.entries;
    aliases[4] = fixture.requests;
    aliases[5] = &sem;
    aliases[6] = &fixture.records[1];
    for (alias = 0; alias < 7; ++alias)
    {
        before = fixture;
        sem_before = sem;
        CHECK(p4_semaphore_nodes_grant_cohort_locked(&fixture.registry,
            &fixture.ledger, fixture.requests, &sem,
            (struct Task **)aliases[alias], 2) == -1);
        CHECK(memcmp(&before, &fixture, sizeof(fixture)) == 0);
        CHECK(memcmp(&sem_before, &sem, sizeof(sem)) == 0);
        CHECK(targets[0] == sentinel && targets[1] == sentinel &&
            targets[2] == sentinel && targets[3] == sentinel);
    }
    before = fixture;
    sem_before = sem;
    CHECK(p4_semaphore_nodes_grant_cohort_locked(&fixture.registry,
        &fixture.ledger, fixture.requests, &sem,
        (struct Task **)(void *)((unsigned char *)targets + 1u), 2) == -1);
    CHECK(memcmp(&before, &fixture, sizeof(fixture)) == 0);
    CHECK(memcmp(&sem_before, &sem, sizeof(sem)) == 0);
    CHECK(targets[0] == sentinel && targets[1] == sentinel &&
        targets[2] == sentinel && targets[3] == sentinel);
    before = fixture;
    sem_before = sem;
    CHECK(p4_semaphore_nodes_grant_cohort_locked(&fixture.registry,
        &fixture.ledger, fixture.requests, &sem, targets, 0) == 0);
    CHECK(memcmp(&before, &fixture, sizeof(fixture)) == 0);
    CHECK(memcmp(&sem_before, &sem, sizeof(sem)) == 0);
    CHECK(targets[0] == sentinel && targets[1] == sentinel &&
        targets[2] == sentinel && targets[3] == sentinel);
    before = fixture;
    sem_before = sem;
    CHECK(p4_semaphore_nodes_grant_cohort_locked(&fixture.registry,
        &fixture.ledger, fixture.requests, &sem, targets,
        TEST_ENTRIES + 1u) == 0);
    CHECK(memcmp(&before, &fixture, sizeof(fixture)) == 0);
    CHECK(memcmp(&sem_before, &sem, sizeof(sem)) == 0);
    CHECK(targets[0] == sentinel && targets[1] == sentinel &&
        targets[2] == sentinel && targets[3] == sentinel);
    before = fixture;
    sem_before = sem;
    CHECK(p4_semaphore_nodes_grant_cohort_locked(&fixture.registry,
        &fixture.ledger, fixture.requests, &sem, NULL, 2) == -1);
    CHECK(memcmp(&before, &fixture, sizeof(fixture)) == 0);
    CHECK(memcmp(&sem_before, &sem, sizeof(sem)) == 0);
    CHECK(targets[0] == sentinel && targets[1] == sentinel &&
        targets[2] == sentinel && targets[3] == sentinel);

    /* Two notifications for one waiter need two ordinary pins. An overflow
     * refusal leaves the real queue, ledger and output bytes unchanged. */
    fixture.records[1].lifetime.notification_pins = UINT32_MAX - 1u;
    before = fixture;
    sem_before = sem;
    CHECK(p4_semaphore_nodes_grant_cohort_locked(&fixture.registry,
        &fixture.ledger, fixture.requests, &sem, targets, 4) == 0);
    CHECK(memcmp(&before, &fixture, sizeof(fixture)) == 0);
    CHECK(memcmp(&sem_before, &sem, sizeof(sem)) == 0);
    CHECK(targets[0] == sentinel && targets[1] == sentinel &&
        targets[2] == sentinel && targets[3] == sentinel);
    fixture.records[1].lifetime.notification_pins = 1;

    CHECK(p4_semaphore_nodes_grant_cohort_locked(&fixture.registry,
        &fixture.ledger, fixture.requests, &sem, targets, 4) == 2);
    CHECK(targets[0] == fixture.tasks[1] && targets[1] == fixture.tasks[1]);
    CHECK(targets[2] == sentinel && targets[3] == sentinel);
    CHECK(sem.ss_NestCount == 2 && sem.ss_Owner == NULL);
    CHECK(sem.ss_QueueCount == 1 && queue_links(&sem) == 0);
    CHECK(fixture.records[1].wait_scope.queued_count == 0);
    CHECK(fixture.records[1].lifetime.notification_pins == 3);
    CHECK(fixture.ledger.entries[0].state == P4_SEMAPHORE_LEDGER_ENTRY_OWNED);
    CHECK(fixture.ledger.entries[1].state == P4_SEMAPHORE_LEDGER_ENTRY_OWNED);
    CHECK(p4_task_registry_wait_operation_complete_locked(&fixture.registry,
        &fixture.ledger, fixture.tasks[1], generation) == 1);
    CHECK(fixture.records[1].lifetime.notification_pins == 2);
    CHECK(fixture.records[1].wait_scope.active == 0);
    CHECK(p4_task_registry_unpin_locked(&fixture.registry, targets[0]) == 1);
    CHECK(p4_task_registry_unpin_locked(&fixture.registry, targets[1]) == 1);
    CHECK(fixture.records[1].lifetime.notification_pins == 0);
}

static void test_retired_scope_refuses_and_withdraws(void)
{
    struct Fixture fixture;
    struct SignalSemaphore sem;
    const uint32_t shared[1] = { 1 };
    struct SignalSemaphore *owner_sem[1] = { &sem };
    const uint32_t exclusive[1] = { 0 };
    struct Task *targets[2] = { NULL, NULL };
    struct p4_task_wait_cancel_snapshot snapshot;
    struct Fixture before;
    struct SignalSemaphore sem_before;
    uint32_t generation;
    empty_sem(&sem);
    fixture_init(&fixture);
    (void)start_holder(&fixture, &sem);
    generation = queue_group(&fixture, &sem, 1, 1, shared);
    simulate_last_release(&sem, 1);
    CHECK(p4_task_registry_retire_with_ledger_locked(&fixture.registry,
        &fixture.ledger, fixture.tasks[1], 0) == 1);
    before = fixture;
    sem_before = sem;
    CHECK(p4_semaphore_nodes_grant_cohort_locked(&fixture.registry,
        &fixture.ledger, fixture.requests, &sem, targets, 2) == 0);
    CHECK(memcmp(&before, &fixture, sizeof(fixture)) == 0);
    CHECK(memcmp(&sem_before, &sem, sizeof(sem)) == 0);
    CHECK(targets[0] == NULL && targets[1] == NULL);
    CHECK(p4_task_registry_claim_next_wait_cancel_with_ledger_locked(
        &fixture.registry, &fixture.ledger, &snapshot) == 1);
    CHECK(snapshot.identity == fixture.tasks[1]);
    CHECK(p4_semaphore_nodes_withdraw_locked(&fixture.registry,
        &fixture.ledger, fixture.requests, fixture.tasks[1], generation,
        0, &sem) == 1);
    CHECK(queue_links(&sem) == 0);
    CHECK(sem.ss_QueueCount == -1 && sem.ss_NestCount == 0);
    CHECK(p4_task_registry_wait_operation_finish_cancel_locked(
        &fixture.registry, &fixture.ledger, fixture.tasks[1], generation) == 1);
    CHECK(p4_task_registry_claim_reclaim_locked(&fixture.registry,
        fixture.tasks[1]) == &fixture.records[1]);
    (void)owner_sem;
    (void)exclusive;
}

static void test_unknown_external_node_does_not_read_waiter_overlay(void)
{
    struct Fixture fixture;
    struct SignalSemaphore sem;
    const uint32_t shared[1] = { 1 };
    struct Task *targets[2] = { NULL, NULL };
    struct Fixture before;
    struct SignalSemaphore sem_before;
    struct MinNode *external;
    struct MinNode *head;
    struct MinNode *tail;
    unsigned char *pair;
    long page_size_long = sysconf(_SC_PAGESIZE);
    size_t page_size;
    uint32_t generation;
    CHECK(page_size_long > 0);
    page_size = (size_t)page_size_long;
    pair = mmap(NULL, page_size * 2u, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(pair != MAP_FAILED);
    CHECK(mprotect(pair + page_size, page_size, PROT_NONE) == 0);
    external = (struct MinNode *)(void *)(pair + page_size - sizeof(*external));
    head = (struct MinNode *)(void *)&sem.ss_WaitQueue.mlh_Head;
    tail = (struct MinNode *)(void *)&sem.ss_WaitQueue.mlh_Tail;

    empty_sem(&sem);
    fixture_init(&fixture);
    (void)start_holder(&fixture, &sem);
    generation = queue_group(&fixture, &sem, 1, 1, shared);
    CHECK(queue_links(&sem) == 1);
    /* The external MinNode links are readable, but its SemaphoreRequest
     * sr_Waiter overlay starts on the protected following page. */
    external->mln_Pred = head;
    external->mln_Succ = &fixture.requests[0].sr_Link;
    head->mln_Succ = external;
    fixture.requests[0].sr_Link.mln_Pred = external;
    sem.ss_WaitQueue.mlh_TailPred = &fixture.requests[0].sr_Link;
    sem.ss_QueueCount = 1; /* Nest0 + two list links = QueueCount + 1. */
    sem.ss_NestCount = 0;
    sem.ss_Owner = NULL;
    CHECK(tail->mln_Pred == &fixture.requests[0].sr_Link);
    before = fixture;
    sem_before = sem;
    CHECK(p4_semaphore_nodes_grant_cohort_locked(&fixture.registry,
        &fixture.ledger, fixture.requests, &sem, targets, 2) == -1);
    CHECK(memcmp(&before, &fixture, sizeof(fixture)) == 0);
    CHECK(memcmp(&sem_before, &sem, sizeof(sem)) == 0);
    CHECK(targets[0] == NULL && targets[1] == NULL);
    empty_sem(&sem);
    CHECK(munmap(pair, page_size * 2u) == 0);
    (void)generation;
}

static void init_bad_ledger(struct p4_semaphore_ledger *ledger,
    struct p4_semaphore_ledger_operation operations[2],
    struct p4_semaphore_ledger_entry entries[2])
{
    memset(ledger, 0, sizeof(*ledger));
    memset(operations, 0, sizeof(*operations) * 2u);
    memset(entries, 0, sizeof(*entries) * 2u);
    CHECK(p4_semaphore_ledger_init(ledger, operations, 2, entries, 2) == 1);
}

static void test_ledger_registry_and_node_storage_aliases(void)
{
    struct Fixture fixture;
    struct SignalSemaphore sem;
    const uint32_t shared[1] = { 1 };
    struct Task *targets[3] = { NULL, NULL, NULL };
    struct Fixture before;
    struct SignalSemaphore sem_before;
    struct p4_semaphore_ledger bad_ledger, bad_before;
    struct p4_semaphore_ledger_operation bad_operations[2];
    struct p4_semaphore_ledger_entry bad_entries[2];
    uint32_t generation;
    unsigned alias;
    void *array_aliases[2];

    empty_sem(&sem);
    fixture_init(&fixture);
    (void)start_holder(&fixture, &sem);
    generation = queue_group(&fixture, &sem, 1, 1, shared);
    simulate_last_release(&sem, 1);
    CHECK(generation != 0);
    before = fixture;
    sem_before = sem;

    /* A ledger header overlapping either registry header or a linked record
     * must be rejected before the candidate ledger pointer is typed-read. */
    CHECK(p4_semaphore_nodes_grant_cohort_locked(&fixture.registry,
        (struct p4_semaphore_ledger *)(void *)&fixture.registry,
        fixture.requests, &sem, targets, 3) == -1);
    CHECK(memcmp(&before, &fixture, sizeof(fixture)) == 0);
    CHECK(memcmp(&sem_before, &sem, sizeof(sem)) == 0);
    CHECK(targets[0] == NULL && targets[1] == NULL && targets[2] == NULL);
    CHECK(p4_semaphore_nodes_grant_cohort_locked(&fixture.registry,
        (struct p4_semaphore_ledger *)(void *)&fixture.records[1],
        fixture.requests, &sem, targets, 3) == -1);
    CHECK(memcmp(&before, &fixture, sizeof(fixture)) == 0);
    CHECK(memcmp(&sem_before, &sem, sizeof(sem)) == 0);
    CHECK(targets[0] == NULL && targets[1] == NULL && targets[2] == NULL);

    init_bad_ledger(&bad_ledger, bad_operations, bad_entries);
    array_aliases[0] = &fixture.records[1];
    array_aliases[1] = &fixture.registry;
    for (alias = 0; alias < 2; ++alias)
    {
        bad_ledger.operations =
            (struct p4_semaphore_ledger_operation *)array_aliases[alias];
        bad_before = bad_ledger;
        CHECK(p4_semaphore_nodes_grant_cohort_locked(&fixture.registry,
            &bad_ledger, fixture.requests, &sem, targets, 2) == -1);
        CHECK(memcmp(&bad_before, &bad_ledger, sizeof(bad_ledger)) == 0);
        CHECK(memcmp(&before, &fixture, sizeof(fixture)) == 0);
        CHECK(memcmp(&sem_before, &sem, sizeof(sem)) == 0);
        CHECK(targets[0] == NULL && targets[1] == NULL && targets[2] == NULL);
    }
    init_bad_ledger(&bad_ledger, bad_operations, bad_entries);
    bad_ledger.entries =
        (struct p4_semaphore_ledger_entry *)(void *)&fixture.records[1];
    bad_before = bad_ledger;
    CHECK(p4_semaphore_nodes_grant_cohort_locked(&fixture.registry,
        &bad_ledger, fixture.requests, &sem, targets, 2) == -1);
    CHECK(memcmp(&bad_before, &bad_ledger, sizeof(bad_ledger)) == 0);
    CHECK(memcmp(&before, &fixture, sizeof(fixture)) == 0);
    CHECK(memcmp(&sem_before, &sem, sizeof(sem)) == 0);
    CHECK(targets[0] == NULL && targets[1] == NULL && targets[2] == NULL);
    bad_ledger.operations = bad_operations;
    bad_ledger.entries = (struct p4_semaphore_ledger_entry *)(void *)&fixture.registry;
    bad_before = bad_ledger;
    CHECK(p4_semaphore_nodes_grant_cohort_locked(&fixture.registry,
        &bad_ledger, fixture.requests, &sem, targets, 2) == -1);
    CHECK(memcmp(&bad_before, &bad_ledger, sizeof(bad_ledger)) == 0);
    CHECK(memcmp(&before, &fixture, sizeof(fixture)) == 0);
    CHECK(memcmp(&sem_before, &sem, sizeof(sem)) == 0);
    CHECK(targets[0] == NULL && targets[1] == NULL && targets[2] == NULL);
    bad_ledger.entries = bad_entries;
    bad_ledger.operations =
        (struct p4_semaphore_ledger_operation *)(void *)&fixture.registry;
    bad_before = bad_ledger;
    CHECK(p4_semaphore_nodes_grant_cohort_locked(&fixture.registry,
        &bad_ledger, fixture.requests, &sem, targets, 2) == -1);
    CHECK(memcmp(&bad_before, &bad_ledger, sizeof(bad_ledger)) == 0);
    CHECK(memcmp(&before, &fixture, sizeof(fixture)) == 0);
    CHECK(memcmp(&sem_before, &sem, sizeof(sem)) == 0);
    CHECK(targets[0] == NULL && targets[1] == NULL && targets[2] == NULL);

    /* The actual ledger remains valid; node-pool/semaphore aliases into its
     * arrays are refused before either aliased object is used as a semaphore
     * request or SignalSemaphore. */
    CHECK(p4_semaphore_nodes_grant_cohort_locked(&fixture.registry,
        &fixture.ledger,
        (struct SemaphoreRequest *)(void *)fixture.entries,
        &sem, targets, 3) == -1);
    CHECK(memcmp(&before, &fixture, sizeof(fixture)) == 0);
    CHECK(memcmp(&sem_before, &sem, sizeof(sem)) == 0);
    CHECK(targets[0] == NULL && targets[1] == NULL && targets[2] == NULL);
    CHECK(p4_semaphore_nodes_grant_cohort_locked(&fixture.registry,
        &fixture.ledger,
        (struct SemaphoreRequest *)(void *)fixture.operations,
        &sem, targets, 3) == -1);
    CHECK(memcmp(&before, &fixture, sizeof(fixture)) == 0);
    CHECK(memcmp(&sem_before, &sem, sizeof(sem)) == 0);
    CHECK(targets[0] == NULL && targets[1] == NULL && targets[2] == NULL);
    CHECK(p4_semaphore_nodes_grant_cohort_locked(&fixture.registry,
        &fixture.ledger, fixture.requests,
        (struct SignalSemaphore *)(void *)fixture.operations,
        targets, 3) == -1);
    CHECK(memcmp(&before, &fixture, sizeof(fixture)) == 0);
    CHECK(memcmp(&sem_before, &sem, sizeof(sem)) == 0);
    CHECK(targets[0] == NULL && targets[1] == NULL && targets[2] == NULL);
    CHECK(p4_semaphore_nodes_grant_cohort_locked(&fixture.registry,
        &fixture.ledger, fixture.requests,
        (struct SignalSemaphore *)(void *)fixture.entries,
        targets, 3) == -1);
    CHECK(memcmp(&before, &fixture, sizeof(fixture)) == 0);
    CHECK(memcmp(&sem_before, &sem, sizeof(sem)) == 0);
    CHECK(targets[0] == NULL && targets[1] == NULL && targets[2] == NULL);

    /* Verify an otherwise ordinary request remains intact after every alias
     * refusal, then exercise one non-alias refusal using insufficient space. */
    CHECK(p4_semaphore_nodes_grant_cohort_locked(&fixture.registry,
        &fixture.ledger, fixture.requests, &sem, targets, 1) == 1);
    CHECK(targets[0] == fixture.tasks[1]);
    CHECK(fixture.ledger.entries[0].state == P4_SEMAPHORE_LEDGER_ENTRY_OWNED);
    (void)generation;
}

static void test_lost_ledger_queue_nodes_refuse(void)
{
    struct Fixture fixture;
    struct SignalSemaphore sem;
    const uint32_t shared[1] = { 1 };
    struct Task *targets[3] = { NULL, NULL, NULL };
    struct Fixture before;
    struct SignalSemaphore sem_before;
    struct MinNode *head;
    struct MinNode *first;
    struct MinNode *remaining;
    empty_sem(&sem);
    fixture_init(&fixture);
    (void)start_holder(&fixture, &sem);
    (void)queue_group(&fixture, &sem, 1, 1, shared);
    (void)queue_group(&fixture, &sem, 2, 1, shared);
    simulate_last_release(&sem, 2);
    CHECK(queue_links(&sem) == 2);
    head = (struct MinNode *)(void *)&sem.ss_WaitQueue.mlh_Head;
    first = sem.ss_WaitQueue.mlh_Head;
    remaining = first->mln_Succ;
    CHECK(remaining == &fixture.requests[1].sr_Link);
    head->mln_Succ = remaining;
    remaining->mln_Pred = head;
    sem.ss_QueueCount = 0; /* One link remains with NestCount zero. */
    CHECK(queue_links(&sem) == 1);
    before = fixture;
    sem_before = sem;
    CHECK(p4_semaphore_nodes_grant_cohort_locked(&fixture.registry,
        &fixture.ledger, fixture.requests, &sem, targets, 3) == -1);
    CHECK(memcmp(&before, &fixture, sizeof(fixture)) == 0);
    CHECK(memcmp(&sem_before, &sem, sizeof(sem)) == 0);
    CHECK(targets[0] == NULL && targets[1] == NULL && targets[2] == NULL);

    empty_sem(&sem);
    fixture_init(&fixture);
    (void)start_holder(&fixture, &sem);
    (void)queue_group(&fixture, &sem, 1, 1, shared);
    simulate_last_release(&sem, 1);
    CHECK(queue_links(&sem) == 1);
    /* The intrusive queue is valid and empty, but a queued ledger entry for
     * this semaphore was lost before cohort selection. */
    empty_sem(&sem);
    before = fixture;
    sem_before = sem;
    CHECK(p4_semaphore_nodes_grant_cohort_locked(&fixture.registry,
        &fixture.ledger, fixture.requests, &sem, targets, 3) == -1);
    CHECK(memcmp(&before, &fixture, sizeof(fixture)) == 0);
    CHECK(memcmp(&sem_before, &sem, sizeof(sem)) == 0);
    CHECK(targets[0] == NULL && targets[1] == NULL && targets[2] == NULL);
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

    test_shared_head_cohort_and_target_lifetime();
    test_exclusive_head_only();
    test_duplicate_shared_capacity_overflow_and_complete();
    test_retired_scope_refuses_and_withdraws();
    test_unknown_external_node_does_not_read_waiter_overlay();
    test_ledger_registry_and_node_storage_aliases();
    test_lost_ledger_queue_nodes_refuse();

    CHECK(munmap(task_page, task_page_size) == 0);
    printf("p4 semaphore cohort checks: %u\n", checks);
    return 0;
}
