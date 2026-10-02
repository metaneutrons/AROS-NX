/* Actual-source tests for cancelled last-unit durable semaphore handoff. */
#define main p4_semaphore_nodes_baseline_main
#include "p4-semaphore-nodes-test.c"
#undef main

static void register_all_running(struct Fixture *fixture)
{
    unsigned i;
    for (i = 0; i < TEST_TASKS; ++i)
        register_running(fixture, i, i & 1u);
}

static void ensure_running(struct Fixture *fixture, unsigned task,
    uint32_t hart)
{
    uint32_t expected = UINT32_C(1) << hart;
    if (fixture->records[task].lifetime.running_harts == 0)
        CHECK(p4_task_registry_enter_running_locked(&fixture->registry,
            fixture->tasks[task], hart) == 1);
    else
        CHECK(fixture->records[task].lifetime.running_harts == expected);
}

static uint32_t completed_holder(struct Fixture *fixture, unsigned task,
    uint32_t hart, struct SignalSemaphore *sem)
{
    struct SignalSemaphore *semaphores[1] = { sem };
    const uint32_t modes[1] = { 0 };
    uint32_t generation;
    ensure_running(fixture, task, hart);
    generation = make_operation(fixture, task, 1, semaphores, modes);
    CHECK(p4_semaphore_nodes_acquire_locked(&fixture->registry,
        &fixture->ledger, fixture->requests, fixture->tasks[task], generation,
        0, sem) == 1);
    CHECK(p4_task_registry_wait_operation_complete_locked(&fixture->registry,
        &fixture->ledger, fixture->tasks[task], generation) == 1);
    CHECK(p4_task_registry_leave_running_locked(&fixture->registry,
        fixture->tasks[task], hart) == 1);
    return generation;
}

static uint32_t queued_waiter(struct Fixture *fixture, unsigned task,
    uint32_t hart, struct SignalSemaphore *sem, uint32_t shared)
{
    struct SignalSemaphore *semaphores[1] = { sem };
    const uint32_t modes[1] = { shared };
    uint32_t generation;
    ensure_running(fixture, task, hart);
    generation = make_operation(fixture, task, 1, semaphores, modes);
    CHECK(p4_semaphore_nodes_acquire_locked(&fixture->registry,
        &fixture->ledger, fixture->requests, fixture->tasks[task], generation,
        0, sem) == 2);
    CHECK(p4_task_registry_leave_running_locked(&fixture->registry,
        fixture->tasks[task], hart) == 1);
    return generation;
}

static void complete_woken_waiter(struct Fixture *fixture, unsigned task,
    uint32_t hart, uint32_t generation)
{
    CHECK(p4_task_registry_enter_running_locked(&fixture->registry,
        fixture->tasks[task], hart) == 1);
    CHECK(p4_task_registry_wait_operation_complete_locked(&fixture->registry,
        &fixture->ledger, fixture->tasks[task], generation) == 1);
    CHECK(p4_task_registry_leave_running_locked(&fixture->registry,
        fixture->tasks[task], hart) == 1);
}

static struct p4_semaphore_ledger_operation *operation_for(
    struct Fixture *fixture, unsigned task)
{
    return (struct p4_semaphore_ledger_operation *)
        fixture->records[task].wait_scope.list_index;
}

static void cancel_and_claim(struct Fixture *fixture, unsigned task,
    uint32_t generation, struct p4_task_wait_cancel_snapshot *snapshot)
{
    CHECK(p4_task_registry_retire_with_ledger_locked(&fixture->registry,
        &fixture->ledger, fixture->tasks[task], 0) == 1);
    if (fixture->records[task].lifetime.running_harts != 0)
        CHECK(p4_task_registry_leave_running_locked(&fixture->registry,
            fixture->tasks[task], task & 1u) == 1);
    CHECK(p4_task_registry_claim_next_wait_cancel_with_ledger_locked(
        &fixture->registry, &fixture->ledger, snapshot) == 1);
    CHECK(snapshot->identity == fixture->tasks[task]);
    CHECK(snapshot->generation == generation);
}

static void test_cancelled_last_unit_grants_shared_cohort(void)
{
    struct Fixture fixture;
    struct SignalSemaphore primary, blocked;
    struct SignalSemaphore *owner_entries[2] = { &primary, &blocked };
    const uint32_t owner_modes[2] = { 0, 0 };
    struct p4_task_wait_cancel_snapshot cancel;
    struct p4_semaphore_notification note;
    struct Fixture before;
    struct SignalSemaphore sem_before;
    struct p4_task_registry_record peer1_before, peer3_before;
    uint32_t owner_generation, s1_generation, e_generation, s2_generation;
    int16_t valid_queue_count;

    empty_sem(&primary);
    empty_sem(&blocked);
    fixture_init(&fixture);
    register_all_running(&fixture);

    /* Task 2 owns a second semaphore; the cancelling Task's operation has
     * one OWNED request on primary and one QUEUED request on blocked. */
    completed_holder(&fixture, 2, 0, &blocked);
    owner_generation = make_operation(&fixture, 0, 2, owner_entries,
        owner_modes);
    CHECK(p4_semaphore_nodes_acquire_locked(&fixture.registry,
        &fixture.ledger, fixture.requests, fixture.tasks[0], owner_generation,
        0, &primary) == 1);
    CHECK(p4_semaphore_nodes_acquire_locked(&fixture.registry,
        &fixture.ledger, fixture.requests, fixture.tasks[0], owner_generation,
        1, &blocked) == 2);

    s1_generation = queued_waiter(&fixture, 1, 1, &primary, 1);
    e_generation = queued_waiter(&fixture, 2, 0, &primary, 0);
    s2_generation = queued_waiter(&fixture, 3, 1, &primary, 1);
    CHECK(queue_links(&primary) == 3);
    CHECK(primary.ss_NestCount == 1 && primary.ss_Owner == fixture.tasks[0]);
    CHECK(fixture.records[0].wait_scope.queued_count == 1);

    CHECK(p4_task_registry_retire_with_ledger_locked(&fixture.registry,
        &fixture.ledger, fixture.tasks[0], 0) == 1);
    /* A's outstanding blocked request makes cancellation claimable while A
     * is still RUN. Rollback itself must remain deferred until RUN drains. */
    CHECK(p4_task_registry_claim_next_wait_cancel_with_ledger_locked(
        &fixture.registry, &fixture.ledger, &cancel) == 1);
    CHECK(cancel.identity == fixture.tasks[0]);
    CHECK(cancel.queued_count == 1);
    before = fixture;
    sem_before = primary;
    CHECK(p4_semaphore_nodes_rollback_durable_locked(&fixture.registry,
        &fixture.ledger, fixture.requests, fixture.tasks[0], owner_generation,
        0, &primary) == 0);
    CHECK(memcmp(&before, &fixture, sizeof(fixture)) == 0);
    CHECK(memcmp(&sem_before, &primary, sizeof(primary)) == 0);

    /* Withdraw the cancelled operation's other queue entry while its claim is
     * held, then let the old RUN reference drain. */
    CHECK(p4_semaphore_nodes_withdraw_locked(&fixture.registry,
        &fixture.ledger, fixture.requests, fixture.tasks[0], owner_generation,
        1, &blocked) == 1);
    CHECK(fixture.records[0].wait_scope.queued_count == 0);
    CHECK(p4_task_registry_leave_running_locked(&fixture.registry,
        fixture.tasks[0], 0) == 1);

    /* Malformed queue accounting and aliased node storage refuse before the
     * cancelled unit, queue, ledger, pins, or other records change. */
    valid_queue_count = primary.ss_QueueCount;
    primary.ss_QueueCount = (WORD)(valid_queue_count - 1);
    before = fixture;
    sem_before = primary;
    CHECK(p4_semaphore_nodes_rollback_durable_locked(&fixture.registry,
        &fixture.ledger, fixture.requests, fixture.tasks[0], owner_generation,
        0, &primary) == -1);
    CHECK(memcmp(&before, &fixture, sizeof(fixture)) == 0);
    CHECK(memcmp(&sem_before, &primary, sizeof(primary)) == 0);
    primary.ss_QueueCount = valid_queue_count;

    before = fixture;
    sem_before = primary;
    CHECK(p4_semaphore_nodes_rollback_durable_locked(&fixture.registry,
        &fixture.ledger,
        (struct SemaphoreRequest *)(void *)&fixture.ledger,
        fixture.tasks[0], owner_generation, 0, &primary) == -1);
    CHECK(memcmp(&before, &fixture, sizeof(fixture)) == 0);
    CHECK(memcmp(&sem_before, &primary, sizeof(primary)) == 0);

    CHECK(p4_semaphore_nodes_rollback_durable_locked(&fixture.registry,
        &fixture.ledger, fixture.requests, fixture.tasks[0], owner_generation,
        0, &primary) == 3);
    CHECK(primary.ss_NestCount == 2 && primary.ss_Owner == NULL);
    CHECK(primary.ss_QueueCount == 2 && queue_links(&primary) == 1);
    CHECK(operation_for(&fixture, 0)->first->state ==
        P4_SEMAPHORE_LEDGER_ENTRY_RELEASED);
    CHECK(operation_for(&fixture, 0)->first->operation_next->state ==
        P4_SEMAPHORE_LEDGER_ENTRY_WITHDRAWN);
    CHECK(operation_for(&fixture, 1)->first->state ==
        P4_SEMAPHORE_LEDGER_ENTRY_OWNED);
    CHECK(operation_for(&fixture, 2)->first->state ==
        P4_SEMAPHORE_LEDGER_ENTRY_QUEUED);
    CHECK(operation_for(&fixture, 3)->first->state ==
        P4_SEMAPHORE_LEDGER_ENTRY_OWNED);
    CHECK(fixture.records[1].semaphore_pending == 1);
    CHECK(fixture.records[3].semaphore_pending == 1);
    CHECK(fixture.registry.semaphore_notification_serial == 2);
    peer1_before = fixture.records[1];
    peer3_before = fixture.records[3];
    CHECK(p4_task_registry_wait_operation_finish_cancel_locked(
        &fixture.registry, &fixture.ledger, fixture.tasks[0],
        owner_generation) == 1);
    CHECK(fixture.records[1].semaphore_pending ==
        peer1_before.semaphore_pending);
    CHECK(fixture.records[1].semaphore_pending_generation ==
        peer1_before.semaphore_pending_generation);
    CHECK(fixture.records[1].lifetime.notification_pins ==
        peer1_before.lifetime.notification_pins);
    CHECK(fixture.records[3].semaphore_pending ==
        peer3_before.semaphore_pending);
    CHECK(fixture.records[3].semaphore_pending_generation ==
        peer3_before.semaphore_pending_generation);
    CHECK(fixture.records[3].lifetime.notification_pins ==
        peer3_before.lifetime.notification_pins);
    CHECK(fixture.records[0].wait_scope.active == 0);

    complete_woken_waiter(&fixture, 1, 1, s1_generation);
    complete_woken_waiter(&fixture, 3, 1, s2_generation);
    CHECK(p4_task_registry_claim_next_semaphore_notification_locked(
        &fixture.registry, &note) == 1);
    CHECK(note.identity == fixture.tasks[3] && note.count == 1);
    CHECK(p4_task_registry_finish_semaphore_notification_locked(
        &fixture.registry, &note) == 1);
    CHECK(p4_task_registry_claim_next_semaphore_notification_locked(
        &fixture.registry, &note) == 1);
    CHECK(note.identity == fixture.tasks[1] && note.count == 1);
    CHECK(p4_task_registry_finish_semaphore_notification_locked(
        &fixture.registry, &note) == 1);
    CHECK(p4_semaphore_nodes_release_durable_locked(&fixture.registry,
        &fixture.ledger, fixture.requests, &primary) == 1);
    CHECK(primary.ss_NestCount == 1 && primary.ss_QueueCount == 1);
    CHECK(p4_semaphore_nodes_release_durable_locked(&fixture.registry,
        &fixture.ledger, fixture.requests, &primary) == 2);
    CHECK(primary.ss_NestCount == 1 && primary.ss_QueueCount == 0);
    CHECK(p4_task_registry_enter_running_locked(&fixture.registry,
        fixture.tasks[2], 0) == 1);
    CHECK(p4_task_registry_wait_operation_complete_locked(&fixture.registry,
        &fixture.ledger, fixture.tasks[2], e_generation) == 1);
    CHECK(p4_task_registry_leave_running_locked(&fixture.registry,
        fixture.tasks[2], 0) == 1);
    CHECK(p4_task_registry_claim_next_semaphore_notification_locked(
        &fixture.registry, &note) == 1);
    CHECK(note.identity == fixture.tasks[2] && note.count == 1);
    CHECK(p4_task_registry_finish_semaphore_notification_locked(
        &fixture.registry, &note) == 1);
}

static void test_cancelled_queue_peer_requires_unlink_retry(void)
{
    struct Fixture fixture;
    struct SignalSemaphore sem;
    struct SignalSemaphore *owner_sem[1] = { &sem };
    const uint32_t exclusive[1] = { 0 };
    struct p4_task_wait_cancel_snapshot cancel_a, cancel_b;
    struct p4_semaphore_notification note;
    struct Fixture before;
    struct SignalSemaphore sem_before;
    uint32_t gen_a, gen_b, gen_c, gen_d;

    empty_sem(&sem);
    fixture_init(&fixture);
    register_all_running(&fixture);
    gen_a = make_operation(&fixture, 0, 1, owner_sem, exclusive);
    CHECK(p4_semaphore_nodes_acquire_locked(&fixture.registry,
        &fixture.ledger, fixture.requests, fixture.tasks[0], gen_a, 0,
        &sem) == 1);
    CHECK(p4_task_registry_leave_running_locked(&fixture.registry,
        fixture.tasks[0], 0) == 1);
    gen_b = queued_waiter(&fixture, 1, 1, &sem, 1);
    gen_c = queued_waiter(&fixture, 2, 0, &sem, 1);
    gen_d = queued_waiter(&fixture, 3, 1, &sem, 1);

    CHECK(p4_task_registry_retire_with_ledger_locked(&fixture.registry,
        &fixture.ledger, fixture.tasks[0], 0) == 1);
    CHECK(p4_task_registry_claim_wait_cancel_after_with_ledger_locked(
        &fixture.registry, &fixture.ledger, 0, &cancel_a) == 1);
    CHECK(cancel_a.identity == fixture.tasks[0]);
    CHECK(cancel_a.generation == gen_a);
    CHECK(p4_task_registry_retire_with_ledger_locked(&fixture.registry,
        &fixture.ledger, fixture.tasks[1], 0) == 1);
    CHECK(p4_task_registry_claim_wait_cancel_after_with_ledger_locked(
        &fixture.registry, &fixture.ledger,
        (uintptr_t)cancel_a.identity, &cancel_b) == 1);
    CHECK(cancel_b.identity == fixture.tasks[1]);
    CHECK(cancel_b.generation == gen_b);

    /* The first queued request is cancelled. A's claim cannot drop its old
     * unit until B's own claim removes the intrusive node. */
    before = fixture;
    sem_before = sem;
    CHECK(p4_semaphore_nodes_rollback_durable_locked(&fixture.registry,
        &fixture.ledger, fixture.requests, fixture.tasks[0], gen_a, 0,
        &sem) == 0);
    CHECK(memcmp(&before, &fixture, sizeof(fixture)) == 0);
    CHECK(memcmp(&sem_before, &sem, sizeof(sem)) == 0);
    CHECK(p4_semaphore_nodes_withdraw_locked(&fixture.registry,
        &fixture.ledger, fixture.requests, fixture.tasks[1], gen_b, 0,
        &sem) == 1);
    CHECK(p4_task_registry_wait_operation_finish_cancel_locked(
        &fixture.registry, &fixture.ledger, fixture.tasks[1], gen_b) == 1);
    CHECK(p4_task_registry_wait_release_cancel_locked(&fixture.registry,
        fixture.tasks[0], gen_a) == 1);

    /* The address cursor does not wrap to A inside B's pass. A new pass from
     * zero finds A's sticky cancellation after B has been drained. */
    {
        uintptr_t after_b = (uintptr_t)cancel_b.identity;
        CHECK(p4_task_registry_claim_wait_cancel_after_with_ledger_locked(
            &fixture.registry, &fixture.ledger, after_b, &cancel_b) == 0);
        CHECK(cancel_b.identity == NULL);
        CHECK(p4_task_registry_claim_wait_cancel_after_with_ledger_locked(
            &fixture.registry, &fixture.ledger, 0, &cancel_a) == 1);
        CHECK(cancel_a.identity == fixture.tasks[0]);
    }

    CHECK(p4_semaphore_nodes_rollback_durable_locked(&fixture.registry,
        &fixture.ledger, fixture.requests, fixture.tasks[0], gen_a, 0,
        &sem) == 3);
    CHECK(sem.ss_NestCount == 2 && sem.ss_Owner == NULL);
    CHECK(queue_links(&sem) == 0);
    CHECK(fixture.records[2].semaphore_pending == 1);
    CHECK(fixture.records[3].semaphore_pending == 1);
    CHECK(p4_task_registry_wait_operation_finish_cancel_locked(
        &fixture.registry, &fixture.ledger, fixture.tasks[0], gen_a) == 1);
    complete_woken_waiter(&fixture, 2, 0, gen_c);
    complete_woken_waiter(&fixture, 3, 1, gen_d);
    CHECK(p4_task_registry_claim_next_semaphore_notification_locked(
        &fixture.registry, &note) == 1);
    CHECK(note.identity == fixture.tasks[3]);
    CHECK(p4_task_registry_finish_semaphore_notification_locked(
        &fixture.registry, &note) == 1);
    CHECK(p4_task_registry_claim_next_semaphore_notification_locked(
        &fixture.registry, &note) == 1);
    CHECK(note.identity == fixture.tasks[2]);
    CHECK(p4_task_registry_finish_semaphore_notification_locked(
        &fixture.registry, &note) == 1);
}

static void test_nonlast_recursive_and_shared_rollback(void)
{
    struct Fixture fixture;
    struct SignalSemaphore recursive, shared_sem;
    struct SignalSemaphore *recursive_entries[2] = { &recursive, &recursive };
    struct SignalSemaphore *shared_entry[1] = { &shared_sem };
    const uint32_t exclusive[2] = { 0, 0 };
    const uint32_t shared[1] = { 1 };
    struct p4_task_wait_cancel_snapshot cancel;
    uint32_t generation;

    empty_sem(&recursive);
    fixture_init(&fixture);
    register_all_running(&fixture);
    generation = make_operation(&fixture, 0, 2, recursive_entries, exclusive);
    CHECK(p4_semaphore_nodes_acquire_locked(&fixture.registry,
        &fixture.ledger, fixture.requests, fixture.tasks[0], generation,
        0, &recursive) == 1);
    CHECK(p4_semaphore_nodes_acquire_locked(&fixture.registry,
        &fixture.ledger, fixture.requests, fixture.tasks[0], generation,
        1, &recursive) == 1);
    CHECK(recursive.ss_NestCount == 2 && recursive.ss_QueueCount == 1);
    cancel_and_claim(&fixture, 0, generation, &cancel);
    CHECK(p4_semaphore_nodes_rollback_durable_locked(&fixture.registry,
        &fixture.ledger, fixture.requests, fixture.tasks[0], generation, 0,
        &recursive) == 1);
    CHECK(recursive.ss_NestCount == 1 &&
        recursive.ss_Owner == fixture.tasks[0]);
    CHECK(fixture.registry.semaphore_notification_serial == 0);
    CHECK(p4_semaphore_nodes_rollback_durable_locked(&fixture.registry,
        &fixture.ledger, fixture.requests, fixture.tasks[0], generation, 1,
        &recursive) == 1);
    CHECK(recursive.ss_NestCount == 0 && recursive.ss_QueueCount == -1);
    CHECK(recursive.ss_Owner == NULL);
    CHECK(fixture.registry.semaphore_notification_serial == 0);
    CHECK(p4_task_registry_wait_operation_finish_cancel_locked(
        &fixture.registry, &fixture.ledger, fixture.tasks[0], generation) == 1);

    empty_sem(&shared_sem);
    fixture_init(&fixture);
    register_all_running(&fixture);
    generation = make_operation(&fixture, 0, 1, shared_entry, shared);
    CHECK(p4_semaphore_nodes_acquire_locked(&fixture.registry,
        &fixture.ledger, fixture.requests, fixture.tasks[0], generation, 0,
        &shared_sem) == 1);
    {
        uint32_t other = make_operation(&fixture, 1, 1, shared_entry, shared);
        CHECK(p4_semaphore_nodes_acquire_locked(&fixture.registry,
            &fixture.ledger, fixture.requests, fixture.tasks[1], other, 0,
            &shared_sem) == 1);
        CHECK(shared_sem.ss_NestCount == 2 && shared_sem.ss_Owner == NULL);
        cancel_and_claim(&fixture, 0, generation, &cancel);
        CHECK(p4_semaphore_nodes_rollback_durable_locked(&fixture.registry,
            &fixture.ledger, fixture.requests, fixture.tasks[0], generation,
            0, &shared_sem) == 1);
        CHECK(shared_sem.ss_NestCount == 1 && shared_sem.ss_Owner == NULL);
        CHECK(fixture.registry.semaphore_notification_serial == 0);
        CHECK(p4_task_registry_wait_operation_finish_cancel_locked(
            &fixture.registry, &fixture.ledger, fixture.tasks[0],
            generation) == 1);
        CHECK(p4_task_registry_wait_operation_complete_locked(
            &fixture.registry, &fixture.ledger, fixture.tasks[1], other) == 1);
        CHECK(p4_task_registry_leave_running_locked(&fixture.registry,
            fixture.tasks[1], 1) == 1);
    }
}

static void test_serial_and_pin_overflow_keep_cancelled_unit(void)
{
    struct Fixture fixture;
    struct SignalSemaphore sem;
    struct SignalSemaphore *owner_sem[1] = { &sem };
    const uint32_t exclusive[1] = { 0 };
    struct p4_task_wait_cancel_snapshot cancel;
    struct Fixture before;
    struct SignalSemaphore sem_before;
    uint32_t generation;

    empty_sem(&sem);
    fixture_init(&fixture);
    register_all_running(&fixture);
    generation = make_operation(&fixture, 0, 1, owner_sem, exclusive);
    CHECK(p4_semaphore_nodes_acquire_locked(&fixture.registry,
        &fixture.ledger, fixture.requests, fixture.tasks[0], generation, 0,
        &sem) == 1);
    CHECK(p4_task_registry_leave_running_locked(&fixture.registry,
        fixture.tasks[0], 0) == 1);
    queued_waiter(&fixture, 1, 1, &sem, 1);
    cancel_and_claim(&fixture, 0, generation, &cancel);
    fixture.registry.semaphore_notification_serial = UINT32_MAX;
    before = fixture;
    sem_before = sem;
    CHECK(p4_semaphore_nodes_rollback_durable_locked(&fixture.registry,
        &fixture.ledger, fixture.requests, fixture.tasks[0], generation, 0,
        &sem) == 0);
    CHECK(memcmp(&before, &fixture, sizeof(fixture)) == 0);
    CHECK(memcmp(&sem_before, &sem, sizeof(sem)) == 0);
    CHECK(sem.ss_NestCount == 1 && sem.ss_Owner == fixture.tasks[0]);

    /* Fresh state isolates per-recipient pin overflow from serial exhaustion. */
    empty_sem(&sem);
    fixture_init(&fixture);
    register_all_running(&fixture);
    generation = make_operation(&fixture, 0, 1, owner_sem, exclusive);
    CHECK(p4_semaphore_nodes_acquire_locked(&fixture.registry,
        &fixture.ledger, fixture.requests, fixture.tasks[0], generation, 0,
        &sem) == 1);
    CHECK(p4_task_registry_leave_running_locked(&fixture.registry,
        fixture.tasks[0], 0) == 1);
    queued_waiter(&fixture, 1, 1, &sem, 1);
    cancel_and_claim(&fixture, 0, generation, &cancel);
    fixture.records[1].lifetime.notification_pins = UINT32_MAX;
    before = fixture;
    sem_before = sem;
    CHECK(p4_semaphore_nodes_rollback_durable_locked(&fixture.registry,
        &fixture.ledger, fixture.requests, fixture.tasks[0], generation, 0,
        &sem) == 0);
    CHECK(memcmp(&before, &fixture, sizeof(fixture)) == 0);
    CHECK(memcmp(&sem_before, &sem, sizeof(sem)) == 0);
    CHECK(sem.ss_NestCount == 1 && sem.ss_Owner == fixture.tasks[0]);
}

static void test_unclaimed_and_committed_wrong_state(void)
{
    struct Fixture fixture;
    struct SignalSemaphore sem;
    struct SignalSemaphore *sem_ref[1] = { &sem };
    const uint32_t exclusive[1] = { 0 };
    struct Fixture before;
    struct SignalSemaphore sem_before;
    struct p4_task_wait_cancel_snapshot cancel;
    uint32_t generation;

    empty_sem(&sem);
    fixture_init(&fixture);
    register_all_running(&fixture);
    generation = make_operation(&fixture, 0, 1, sem_ref, exclusive);
    CHECK(p4_semaphore_nodes_acquire_locked(&fixture.registry,
        &fixture.ledger, fixture.requests, fixture.tasks[0], generation, 0,
        &sem) == 1);
    before = fixture;
    sem_before = sem;
    CHECK(p4_semaphore_nodes_rollback_durable_locked(&fixture.registry,
        &fixture.ledger, fixture.requests, fixture.tasks[0], generation, 0,
        &sem) == 0);
    CHECK(memcmp(&before, &fixture, sizeof(fixture)) == 0);
    CHECK(memcmp(&sem_before, &sem, sizeof(sem)) == 0);

    operation_for(&fixture, 0)->committed = 1;
    cancel_and_claim(&fixture, 0, generation, &cancel);
    before = fixture;
    sem_before = sem;
    CHECK(p4_semaphore_nodes_rollback_durable_locked(&fixture.registry,
        &fixture.ledger, fixture.requests, fixture.tasks[0], generation, 0,
        &sem) == 0);
    CHECK(memcmp(&before, &fixture, sizeof(fixture)) == 0);
    CHECK(memcmp(&sem_before, &sem, sizeof(sem)) == 0);
}

static void test_unknown_procure_overlay_not_read(void)
{
    struct Fixture fixture;
    struct SignalSemaphore sem;
    struct SignalSemaphore *sem_ref[1] = { &sem };
    const uint32_t exclusive[1] = { 0 };
    struct Fixture before;
    struct SignalSemaphore sem_before;
    struct p4_task_wait_cancel_snapshot cancel;
    long page_size_long = sysconf(_SC_PAGESIZE);
    size_t page_size;
    unsigned char *mapping;
    struct MinNode *unknown;
    struct MinNode *head, *tail;
    uint32_t generation;

    CHECK(page_size_long > 0);
    page_size = (size_t)page_size_long;
    CHECK(offsetof(struct SemaphoreRequest, sr_Waiter) == sizeof(struct MinNode));
    mapping = mmap(NULL, page_size * 2u, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(mapping != MAP_FAILED);
    CHECK(mprotect(mapping + page_size, page_size, PROT_NONE) == 0);
    unknown = (struct MinNode *)(void *)(mapping + page_size - sizeof(*unknown));

    empty_sem(&sem);
    fixture_init(&fixture);
    register_all_running(&fixture);
    generation = make_operation(&fixture, 0, 1, sem_ref, exclusive);
    CHECK(p4_semaphore_nodes_acquire_locked(&fixture.registry,
        &fixture.ledger, fixture.requests, fixture.tasks[0], generation, 0,
        &sem) == 1);
    cancel_and_claim(&fixture, 0, generation, &cancel);

    head = (struct MinNode *)(void *)&sem.ss_WaitQueue.mlh_Head;
    tail = (struct MinNode *)(void *)&sem.ss_WaitQueue.mlh_Tail;
    unknown->mln_Pred = head;
    unknown->mln_Succ = tail;
    sem.ss_WaitQueue.mlh_Head = unknown;
    sem.ss_WaitQueue.mlh_TailPred = unknown;
    sem.ss_QueueCount = 1;
    CHECK(queue_links(&sem) == 1);
    before = fixture;
    sem_before = sem;
    CHECK(p4_semaphore_nodes_rollback_durable_locked(&fixture.registry,
        &fixture.ledger, fixture.requests, fixture.tasks[0], generation, 0,
        &sem) == -1);
    CHECK(memcmp(&before, &fixture, sizeof(fixture)) == 0);
    CHECK(memcmp(&sem_before, &sem, sizeof(sem)) == 0);
    CHECK(munmap(mapping, page_size * 2u) == 0);
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

    test_cancelled_last_unit_grants_shared_cohort();
    test_cancelled_queue_peer_requires_unlink_retry();
    test_nonlast_recursive_and_shared_rollback();
    test_serial_and_pin_overflow_keep_cancelled_unit();
    test_unclaimed_and_committed_wrong_state();
    test_unknown_procure_overlay_not_read();

    CHECK(munmap(task_page, task_page_size) == 0);
    printf("p4 semaphore cancel-handoff checks: %u\n", checks);
    return 0;
}
