/* Durable semaphore-notification reservation tests over real leaf sources. */
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
    ensure_running(fixture, task, hart);
    uint32_t generation = make_operation(fixture, task, 1, semaphores, modes);
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
    ensure_running(fixture, task, hart);
    uint32_t generation = make_operation(fixture, task, 1, semaphores, modes);
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

static void test_mixed_cohort_and_durable_delivery(void)
{
    struct Fixture fixture;
    struct SignalSemaphore sem, reuse_owner, reuse_target, second_sem;
    struct SignalSemaphore *owner_sem[1] = { &sem };
    const uint32_t owner_mode[1] = { 0 };
    struct SignalSemaphore *same_task_sem[1];
    const uint32_t same_task_mode[1] = { 0 };
    struct p4_semaphore_notification first, second, stale;
    struct Fixture before;
    uint32_t gen_owner, gen_s1, gen_e, gen_s2, gen_reuse, gen_second;
    struct SemaphoreRequest *old_s1_request, *old_e_request;
    struct SemaphoreRequest *old_s2_request;
    uint32_t serial_after_cohort;
    struct p4_task_wait_operation reuse_wait;
    int reserve_result;

    empty_sem(&sem);
    empty_sem(&reuse_owner);
    empty_sem(&reuse_target);
    empty_sem(&second_sem);
    fixture_init(&fixture);
    register_all_running(&fixture);

    gen_owner = make_operation(&fixture, 0, 1, owner_sem, owner_mode);
    CHECK(p4_semaphore_nodes_acquire_locked(&fixture.registry,
        &fixture.ledger, fixture.requests, fixture.tasks[0], gen_owner, 0,
        &sem) == 1);
    CHECK(p4_task_registry_wait_operation_complete_locked(&fixture.registry,
        &fixture.ledger, fixture.tasks[0], gen_owner) == 1);
    CHECK(p4_task_registry_leave_running_locked(&fixture.registry,
        fixture.tasks[0], 0) == 1);

    gen_s1 = queued_waiter(&fixture, 1, 1, &sem, 1);
    gen_e = queued_waiter(&fixture, 2, 0, &sem, 0);
    gen_s2 = queued_waiter(&fixture, 3, 1, &sem, 1);
    old_s1_request = (struct SemaphoreRequest *)(uintptr_t)
        operation_for(&fixture, 1)->first->request;
    old_e_request = (struct SemaphoreRequest *)(uintptr_t)
        operation_for(&fixture, 2)->first->request;
    old_s2_request = (struct SemaphoreRequest *)(uintptr_t)
        operation_for(&fixture, 3)->first->request;
    CHECK(old_s1_request == &fixture.requests[0]);
    CHECK(old_e_request == &fixture.requests[1]);
    CHECK(old_s2_request == &fixture.requests[2]);
    CHECK(queue_links(&sem) == 3);

    /* S1, E, S2: the shared head grants both shared entries across E. */
    CHECK(p4_semaphore_nodes_release_durable_locked(&fixture.registry,
        &fixture.ledger, fixture.requests, &sem) == 3);
    CHECK(sem.ss_NestCount == 2 && sem.ss_Owner == NULL);
    CHECK(sem.ss_QueueCount == 2 && queue_links(&sem) == 1);
    CHECK(old_s1_request->sr_Link.mln_Succ == NULL);
    CHECK(old_s1_request->sr_Link.mln_Pred == NULL);
    CHECK(old_e_request->sr_Link.mln_Succ != NULL);
    CHECK(old_e_request->sr_Waiter == fixture.tasks[2]);
    CHECK(old_e_request->sr_Link.mln_Pred != NULL);
    CHECK(fixture.entries[0].state == P4_SEMAPHORE_LEDGER_ENTRY_OWNED);
    CHECK(fixture.entries[1].state == P4_SEMAPHORE_LEDGER_ENTRY_QUEUED);
    CHECK(fixture.entries[2].state == P4_SEMAPHORE_LEDGER_ENTRY_OWNED);
    CHECK(fixture.records[1].semaphore_pending == 1);
    CHECK(fixture.records[3].semaphore_pending == 1);
    CHECK(fixture.records[2].semaphore_pending == 0);
    CHECK(fixture.records[1].semaphore_pending_generation != 0);
    CHECK(fixture.records[3].semaphore_pending_generation != 0);
    CHECK(fixture.records[1].semaphore_pending_generation !=
        fixture.records[3].semaphore_pending_generation);
    CHECK(fixture.registry.semaphore_notification_serial == 2);
    serial_after_cohort = fixture.registry.semaphore_notification_serial;

    complete_woken_waiter(&fixture, 1, 1, gen_s1);
    complete_woken_waiter(&fixture, 3, 1, gen_s2);
    CHECK(fixture.records[1].lifetime.notification_pins == 1);
    CHECK(fixture.records[3].lifetime.notification_pins == 1);
    CHECK(fixture.records[3].semaphore_pending == 1);
    CHECK(old_s2_request->sr_Link.mln_Succ == NULL);
    CHECK(old_s2_request->sr_Link.mln_Pred == NULL);

    /* Keep the lowest free request slot occupied, then reuse S2's exact
     * detached request storage for an unrelated completed operation. */
    CHECK(p4_task_registry_enter_running_locked(&fixture.registry,
        fixture.tasks[0], 0) == 1);
    gen_reuse = make_operation(&fixture, 0, 1,
        (struct SignalSemaphore *const[]){ &reuse_owner }, same_task_mode);
    CHECK(p4_semaphore_nodes_acquire_locked(&fixture.registry,
        &fixture.ledger, fixture.requests, fixture.tasks[0], gen_reuse, 0,
        &reuse_owner) == 1);

    CHECK(p4_task_registry_enter_running_locked(&fixture.registry,
        fixture.tasks[3], 1) == 1);
    same_task_sem[0] = &reuse_target;
    reserve_result = p4_task_registry_wait_reserve_operation_locked(
        &fixture.registry, &fixture.ledger, fixture.tasks[3], 1u, &reuse_wait);
    if (reserve_result != 1)
        fprintf(stderr, "S2 reuse reserve=%d flags=%u RUN=%u active=%u gen=%u "
            "pins=%u pending=%u inflight=%u serial=%u ops=%u entries=%u\n",
            reserve_result, fixture.records[3].lifetime.flags,
            fixture.records[3].lifetime.running_harts,
            fixture.records[3].wait_scope.active,
            fixture.records[3].wait_scope.generation,
            fixture.records[3].lifetime.notification_pins,
            fixture.records[3].semaphore_pending,
            fixture.records[3].semaphore_inflight,
            fixture.registry.semaphore_notification_serial,
            fixture.ledger.active_operations, fixture.ledger.active_entries);
    CHECK(reserve_result == 1);
    gen_reuse = reuse_wait.generation;
    CHECK(p4_semaphore_nodes_configure_locked(&fixture.registry,
        &fixture.ledger, fixture.requests, fixture.tasks[3], gen_reuse, 0,
        &reuse_target, 0) == 1);
    CHECK(p4_task_registry_wait_operation_close_locked(&fixture.registry,
        &fixture.ledger, fixture.tasks[3], gen_reuse) == 1);
    CHECK(operation_for(&fixture, 3)->first->request == old_s2_request);
    CHECK(p4_semaphore_nodes_acquire_locked(&fixture.registry,
        &fixture.ledger, fixture.requests, fixture.tasks[3], gen_reuse, 0,
        &reuse_target) == 1);
    CHECK(p4_task_registry_wait_operation_complete_locked(&fixture.registry,
        &fixture.ledger, fixture.tasks[3], gen_reuse) == 1);
    CHECK(p4_task_registry_leave_running_locked(&fixture.registry,
        fixture.tasks[3], 1) == 1);
    CHECK(fixture.records[3].semaphore_pending == 1);
    CHECK(fixture.records[3].lifetime.notification_pins == 1);

    CHECK(p4_task_registry_wait_operation_complete_locked(&fixture.registry,
        &fixture.ledger, fixture.tasks[0], gen_reuse) == 1);
    CHECK(p4_task_registry_leave_running_locked(&fixture.registry,
        fixture.tasks[0], 0) == 1);

    memset(&first, 0x5a, sizeof(first));
    CHECK(p4_task_registry_claim_next_semaphore_notification_locked(
        &fixture.registry, &first) == 1);
    CHECK(first.identity == fixture.tasks[3]);
    CHECK(first.count == 1);
    CHECK(first.generation == fixture.records[3].semaphore_inflight_generation);
    CHECK(first.generation != 0);
    CHECK(fixture.records[3].semaphore_pending == 0);
    CHECK(fixture.records[3].semaphore_inflight == 1);
    CHECK(fixture.records[3].lifetime.notification_pins == 1);

    /* A reserved pin cannot be consumed as ordinary ownership; spare pins
     * remain independently borrowable and releasable. */
    before = fixture;
    CHECK(p4_task_registry_unpin_locked(&fixture.registry,
        fixture.tasks[3]) == 0);
    CHECK(memcmp(&before, &fixture, sizeof(fixture)) == 0);
    CHECK(p4_task_registry_pin_locked(&fixture.registry,
        fixture.tasks[3]) == 1);
    CHECK(fixture.records[3].lifetime.notification_pins == 2);
    CHECK(p4_task_registry_unpin_locked(&fixture.registry,
        fixture.tasks[3]) == 1);
    CHECK(fixture.records[3].lifetime.notification_pins == 1);

    /* A new batch for the same identity can coexist with the first inflight
     * batch, but it receives a different registry-global generation. */
    CHECK(p4_task_registry_enter_running_locked(&fixture.registry,
        fixture.tasks[0], 0) == 1);
    gen_owner = make_operation(&fixture, 0, 1,
        (struct SignalSemaphore *const[]){ &second_sem }, owner_mode);
    CHECK(p4_semaphore_nodes_acquire_locked(&fixture.registry,
        &fixture.ledger, fixture.requests, fixture.tasks[0], gen_owner, 0,
        &second_sem) == 1);
    CHECK(p4_task_registry_wait_operation_complete_locked(&fixture.registry,
        &fixture.ledger, fixture.tasks[0], gen_owner) == 1);
    CHECK(p4_task_registry_leave_running_locked(&fixture.registry,
        fixture.tasks[0], 0) == 1);
    CHECK(p4_task_registry_enter_running_locked(&fixture.registry,
        fixture.tasks[3], 1) == 1);
    gen_second = queued_waiter(&fixture, 3, 1, &second_sem, 0);
    CHECK(p4_semaphore_nodes_release_durable_locked(&fixture.registry,
        &fixture.ledger, fixture.requests, &second_sem) == 2);
    CHECK(fixture.records[3].semaphore_pending == 1);
    CHECK(fixture.records[3].semaphore_inflight == 1);
    CHECK(fixture.records[3].semaphore_pending_generation > first.generation);
    CHECK(fixture.records[3].semaphore_pending_generation !=
        fixture.records[3].semaphore_inflight_generation);
    CHECK(fixture.registry.semaphore_notification_serial ==
        serial_after_cohort + 1u);
    complete_woken_waiter(&fixture, 3, 1, gen_second);

    CHECK(p4_task_registry_finish_semaphore_notification_locked(
        &fixture.registry, &first) == 1);
    CHECK(fixture.records[3].semaphore_inflight == 0);
    CHECK(fixture.records[3].semaphore_pending == 1);
    CHECK(fixture.records[3].lifetime.notification_pins == 1);
    stale = first;
    before = fixture;
    CHECK(p4_task_registry_finish_semaphore_notification_locked(
        &fixture.registry, &stale) == 0);
    CHECK(memcmp(&before, &fixture, sizeof(fixture)) == 0);
    CHECK(memcmp(&first, &stale, sizeof(first)) == 0);

    memset(&second, 0, sizeof(second));
    CHECK(p4_task_registry_claim_next_semaphore_notification_locked(
        &fixture.registry, &second) == 1);
    CHECK(second.identity == fixture.tasks[3]);
    CHECK(second.count == 1 && second.generation > first.generation);
    CHECK(p4_task_registry_finish_semaphore_notification_locked(
        &fixture.registry, &second) == 1);
    CHECK(fixture.records[3].lifetime.notification_pins == 0);

    /* The unclaimed S1 reservation remains independent. */
    CHECK(p4_task_registry_claim_next_semaphore_notification_locked(
        &fixture.registry, &second) == 1);
    CHECK(second.identity == fixture.tasks[1] && second.count == 1);
    CHECK(p4_task_registry_finish_semaphore_notification_locked(
        &fixture.registry, &second) == 1);
    CHECK(fixture.records[1].lifetime.notification_pins == 0);

    /* The remaining exclusive queue head grants exactly one waiter. */
    CHECK(p4_semaphore_nodes_release_durable_locked(&fixture.registry,
        &fixture.ledger, fixture.requests, &sem) == 1);
    CHECK(fixture.registry.semaphore_notification_serial ==
        serial_after_cohort + 1u);
    CHECK(sem.ss_NestCount == 1 && sem.ss_Owner == NULL);
    CHECK(p4_semaphore_nodes_release_durable_locked(&fixture.registry,
        &fixture.ledger, fixture.requests, &sem) == 2);
    CHECK(sem.ss_NestCount == 1 && sem.ss_Owner == fixture.tasks[2]);
    CHECK(queue_links(&sem) == 0);
    CHECK(fixture.records[2].semaphore_pending == 1);
    CHECK(fixture.registry.semaphore_notification_serial ==
        serial_after_cohort + 2u);
    complete_woken_waiter(&fixture, 2, 0, gen_e);
    CHECK(p4_task_registry_claim_next_semaphore_notification_locked(
        &fixture.registry, &second) == 1);
    CHECK(second.identity == fixture.tasks[2]);
    CHECK(p4_task_registry_finish_semaphore_notification_locked(
        &fixture.registry, &second) == 1);
    CHECK(p4_semaphore_nodes_release_durable_locked(&fixture.registry,
        &fixture.ledger, fixture.requests, &sem) == 1);
    CHECK(fixture.registry.semaphore_notification_serial ==
        serial_after_cohort + 2u);
    CHECK(p4_semaphore_nodes_release_durable_locked(&fixture.registry,
        &fixture.ledger, fixture.requests, &second_sem) == 1);
    CHECK(p4_semaphore_nodes_release_durable_locked(&fixture.registry,
        &fixture.ledger, fixture.requests, &reuse_owner) == 1);
    CHECK(fixture.registry.semaphore_notification_serial ==
        serial_after_cohort + 2u);
}

static void test_duplicate_shared_batch_and_empty_release(void)
{
    struct Fixture fixture;
    struct SignalSemaphore sem, empty;
    struct SignalSemaphore *dups[2] = { &sem, &sem };
    const uint32_t shared_modes[2] = { 1, 1 };
    struct p4_semaphore_notification snapshot;
    uint32_t owner_generation;
    uint32_t waiter_generation;

    empty_sem(&sem);
    empty_sem(&empty);
    fixture_init(&fixture);
    register_all_running(&fixture);
    owner_generation = completed_holder(&fixture, 0, 0, &sem);
    (void)owner_generation;
    waiter_generation = make_operation(&fixture, 1, 2, dups, shared_modes);
    CHECK(p4_semaphore_nodes_acquire_locked(&fixture.registry,
        &fixture.ledger, fixture.requests, fixture.tasks[1], waiter_generation,
        0, &sem) == 2);
    CHECK(p4_semaphore_nodes_acquire_locked(&fixture.registry,
        &fixture.ledger, fixture.requests, fixture.tasks[1], waiter_generation,
        1, &sem) == 2);
    CHECK(p4_task_registry_leave_running_locked(&fixture.registry,
        fixture.tasks[1], 1) == 1);
    CHECK(queue_links(&sem) == 2);
    CHECK(p4_semaphore_nodes_release_durable_locked(&fixture.registry,
        &fixture.ledger, fixture.requests, &sem) == 3);
    CHECK(sem.ss_NestCount == 2 && sem.ss_Owner == NULL);
    CHECK(queue_links(&sem) == 0);
    CHECK(fixture.records[1].semaphore_pending == 2);
    CHECK(fixture.records[1].lifetime.notification_pins == 3);
    CHECK(fixture.records[1].semaphore_pending_generation == 1);
    CHECK(fixture.registry.semaphore_notification_serial == 1);
    complete_woken_waiter(&fixture, 1, 1, waiter_generation);
    CHECK(fixture.records[1].lifetime.notification_pins == 2);
    CHECK(p4_task_registry_claim_next_semaphore_notification_locked(
        &fixture.registry, &snapshot) == 1);
    CHECK(snapshot.identity == fixture.tasks[1]);
    CHECK(snapshot.count == 2 && snapshot.generation == 1);
    CHECK(fixture.records[1].semaphore_inflight == 2);
    CHECK(p4_task_registry_unpin_locked(&fixture.registry,
        fixture.tasks[1]) == 0);
    CHECK(p4_task_registry_finish_semaphore_notification_locked(
        &fixture.registry, &snapshot) == 1);
    CHECK(fixture.records[1].lifetime.notification_pins == 0);

    fixture_init(&fixture);
    register_all_running(&fixture);
    completed_holder(&fixture, 0, 0, &empty);
    CHECK(fixture.registry.semaphore_notification_serial == 0);
    CHECK(p4_semaphore_nodes_release_durable_locked(&fixture.registry,
        &fixture.ledger, fixture.requests, &empty) == 1);
    CHECK(empty.ss_NestCount == 0 && empty.ss_QueueCount == -1);
    CHECK(empty.ss_Owner == NULL);
    CHECK(fixture.registry.semaphore_notification_serial == 0);
    memset(&snapshot, 0x5a, sizeof(snapshot));
    CHECK(p4_task_registry_claim_next_semaphore_notification_locked(
        &fixture.registry, &snapshot) == 0);
    CHECK(snapshot.identity == NULL && snapshot.count == 0 &&
        snapshot.generation == 0);
}

static void test_serial_exhaustion_and_same_batch_coalescing(void)
{
    struct Fixture fixture;
    struct SignalSemaphore sem, sem2;
    const uint32_t exclusive[1] = { 0 };
    struct Fixture before;
    struct SignalSemaphore sem_before;
    struct p4_semaphore_notification snapshot;
    uint32_t waiter_generation;

    empty_sem(&sem);
    fixture_init(&fixture);
    register_all_running(&fixture);
    completed_holder(&fixture, 0, 0, &sem);
    waiter_generation = queued_waiter(&fixture, 1, 1, &sem, 1);
    (void)waiter_generation;
    fixture.registry.semaphore_notification_serial = UINT32_MAX;
    before = fixture;
    sem_before = sem;
    CHECK(p4_semaphore_nodes_release_durable_locked(&fixture.registry,
        &fixture.ledger, fixture.requests, &sem) == 0);
    CHECK(memcmp(&before, &fixture, sizeof(fixture)) == 0);
    CHECK(memcmp(&sem_before, &sem, sizeof(sem)) == 0);
    CHECK(sem.ss_NestCount == 1 && sem.ss_QueueCount == 1);

    empty_sem(&sem);
    empty_sem(&sem2);
    fixture_init(&fixture);
    register_all_running(&fixture);
    completed_holder(&fixture, 0, 0, &sem);
    waiter_generation = queued_waiter(&fixture, 1, 1, &sem, 1);
    CHECK(p4_semaphore_nodes_release_durable_locked(&fixture.registry,
        &fixture.ledger, fixture.requests, &sem) == 2);
    CHECK(fixture.registry.semaphore_notification_serial == 1);
    complete_woken_waiter(&fixture, 1, 1, waiter_generation);
    CHECK(fixture.records[1].semaphore_pending == 1);
    fixture.registry.semaphore_notification_serial = UINT32_MAX;

    completed_holder(&fixture, 0, 0, &sem2);
    /* The waiter is stopped after the prior completion; publish a new closed
     * operation while its existing batch is pending. */
    CHECK(p4_task_registry_enter_running_locked(&fixture.registry,
        fixture.tasks[1], 1) == 1);
    waiter_generation = make_operation(&fixture, 1, 1,
        (struct SignalSemaphore *const[]){ &sem2 }, exclusive);
    CHECK(p4_semaphore_nodes_acquire_locked(&fixture.registry,
        &fixture.ledger, fixture.requests, fixture.tasks[1], waiter_generation,
        0, &sem2) == 2);
    CHECK(p4_task_registry_leave_running_locked(&fixture.registry,
        fixture.tasks[1], 1) == 1);
    CHECK(p4_semaphore_nodes_release_durable_locked(&fixture.registry,
        &fixture.ledger, fixture.requests, &sem2) == 2);
    CHECK(fixture.registry.semaphore_notification_serial == UINT32_MAX);
    CHECK(fixture.records[1].semaphore_pending == 2);
    CHECK(fixture.records[1].semaphore_pending_generation == 1);
    complete_woken_waiter(&fixture, 1, 1, waiter_generation);
    CHECK(p4_task_registry_claim_next_semaphore_notification_locked(
        &fixture.registry, &snapshot) == 1);
    CHECK(snapshot.identity == fixture.tasks[1]);
    CHECK(snapshot.count == 2 && snapshot.generation == 1);
    CHECK(p4_task_registry_finish_semaphore_notification_locked(
        &fixture.registry, &snapshot) == 1);
}

static void test_cancelled_owned_scope_keeps_pending_pin(void)
{
    struct Fixture fixture;
    struct SignalSemaphore prior, rollback;
    struct SignalSemaphore *prior_ref[1] = { &prior };
    const uint32_t shared[1] = { 1 };
    struct SignalSemaphore *rollback_ref[1] = { &rollback };
    const uint32_t exclusive[1] = { 0 };
    struct p4_task_wait_cancel_snapshot cancel;
    struct p4_semaphore_notification notification;
    uint32_t prior_generation, rollback_generation;

    empty_sem(&prior);
    empty_sem(&rollback);
    fixture_init(&fixture);
    register_all_running(&fixture);
    completed_holder(&fixture, 0, 0, &prior);
    prior_generation = make_operation(&fixture, 1, 1, prior_ref, shared);
    CHECK(p4_semaphore_nodes_acquire_locked(&fixture.registry,
        &fixture.ledger, fixture.requests, fixture.tasks[1], prior_generation,
        0, &prior) == 2);
    CHECK(p4_task_registry_leave_running_locked(&fixture.registry,
        fixture.tasks[1], 1) == 1);
    CHECK(p4_semaphore_nodes_release_durable_locked(&fixture.registry,
        &fixture.ledger, fixture.requests, &prior) == 2);
    complete_woken_waiter(&fixture, 1, 1, prior_generation);
    CHECK(fixture.records[1].semaphore_pending == 1);

    CHECK(p4_task_registry_enter_running_locked(&fixture.registry,
        fixture.tasks[1], 1) == 1);
    rollback_generation = make_operation(&fixture, 1, 1, rollback_ref,
        exclusive);
    CHECK(p4_semaphore_nodes_acquire_locked(&fixture.registry,
        &fixture.ledger, fixture.requests, fixture.tasks[1],
        rollback_generation, 0, &rollback) == 1);
    CHECK(rollback.ss_NestCount == 1 && rollback.ss_QueueCount == 0);
    CHECK(p4_task_registry_retire_with_ledger_locked(&fixture.registry,
        &fixture.ledger, fixture.tasks[1], 1) == 1);
    CHECK(p4_task_registry_leave_running_locked(&fixture.registry,
        fixture.tasks[1], 1) == 1);
    CHECK(p4_task_registry_claim_next_wait_cancel_with_ledger_locked(
        &fixture.registry, &fixture.ledger, &cancel) == 1);
    CHECK(cancel.identity == fixture.tasks[1]);
    CHECK(cancel.generation == rollback_generation);
    CHECK(p4_semaphore_nodes_rollback_locked(&fixture.registry,
        &fixture.ledger, fixture.requests, fixture.tasks[1],
        rollback_generation, 0, &rollback) == 1);
    CHECK(rollback.ss_NestCount == 0 && rollback.ss_QueueCount == -1);
    CHECK(p4_task_registry_wait_operation_finish_cancel_locked(
        &fixture.registry, &fixture.ledger, fixture.tasks[1],
        rollback_generation) == 1);
    CHECK(fixture.records[1].wait_scope.active == 0);
    CHECK(fixture.records[1].service_pending == 1);
    CHECK(fixture.records[1].semaphore_pending == 1);
    CHECK(fixture.records[1].lifetime.flags == P4_TASK_LIFETIME_RETIRED);
    CHECK(fixture.records[1].lifetime.notification_pins == 2);
    CHECK(fixture.ledger.active_operations == 0);

    CHECK(p4_task_registry_claim_next_semaphore_notification_locked(
        &fixture.registry, &notification) == 1);
    CHECK(notification.identity == fixture.tasks[1]);
    CHECK(fixture.records[1].semaphore_inflight == 1);
    CHECK(p4_task_registry_finish_semaphore_notification_locked(
        &fixture.registry, &notification) == 1);
    CHECK(fixture.records[1].semaphore_pending == 0);
    CHECK(fixture.records[1].semaphore_inflight == 0);
    CHECK(fixture.records[1].service_pending == 1);
    CHECK(fixture.records[1].lifetime.notification_pins == 1);
    CHECK(fixture.registry.count == TEST_TASKS);
    CHECK(p4_task_registry_claim_reclaim_locked(&fixture.registry,
        fixture.tasks[1]) == NULL);
}

static void test_task_record_reuse_keeps_serial_aba_safe(void)
{
    struct Fixture fixture;
    struct SignalSemaphore old_sem, new_sem;
    const uint32_t shared[1] = { 1 };
    struct p4_semaphore_notification old_snapshot, new_snapshot;
    struct p4_task_registry_record *detached;
    struct Fixture before;
    uint32_t generation;
    uint32_t old_serial;

    empty_sem(&old_sem);
    empty_sem(&new_sem);
    fixture_init(&fixture);
    register_all_running(&fixture);
    completed_holder(&fixture, 0, 0, &old_sem);
    generation = queued_waiter(&fixture, 1, 1, &old_sem, 1);
    CHECK(p4_semaphore_nodes_release_durable_locked(&fixture.registry,
        &fixture.ledger, fixture.requests, &old_sem) == 2);
    complete_woken_waiter(&fixture, 1, 1, generation);
    old_serial = fixture.registry.semaphore_notification_serial;
    CHECK(old_serial == 1);
    CHECK(p4_task_registry_retire_locked(&fixture.registry,
        fixture.tasks[1]) == 1);
    CHECK(p4_task_registry_claim_reclaim_locked(&fixture.registry,
        fixture.tasks[1]) == NULL);
    CHECK(p4_task_registry_claim_next_semaphore_notification_locked(
        &fixture.registry, &old_snapshot) == 1);
    CHECK(old_snapshot.identity == fixture.tasks[1]);
    CHECK(p4_task_registry_finish_semaphore_notification_locked(
        &fixture.registry, &old_snapshot) == 1);
    CHECK(fixture.records[1].lifetime.notification_pins == 0);
    detached = p4_task_registry_claim_reclaim_locked(&fixture.registry,
        fixture.tasks[1]);
    CHECK(detached == &fixture.records[1]);
    memset(detached, 0, sizeof(*detached));
    CHECK(p4_task_registry_register_locked(&fixture.registry, detached,
        fixture.tasks[1]) == 1);
    CHECK(fixture.registry.semaphore_notification_serial == old_serial);
    CHECK(fixture.records[1].semaphore_pending == 0);
    CHECK(fixture.records[1].semaphore_inflight == 0);

    completed_holder(&fixture, 0, 0, &new_sem);
    CHECK(p4_task_registry_enter_running_locked(&fixture.registry,
        fixture.tasks[1], 1) == 1);
    generation = make_operation(&fixture, 1, 1,
        (struct SignalSemaphore *const[]){ &new_sem }, shared);
    CHECK(p4_semaphore_nodes_acquire_locked(&fixture.registry,
        &fixture.ledger, fixture.requests, fixture.tasks[1], generation,
        0, &new_sem) == 2);
    CHECK(p4_task_registry_leave_running_locked(&fixture.registry,
        fixture.tasks[1], 1) == 1);
    CHECK(p4_semaphore_nodes_release_durable_locked(&fixture.registry,
        &fixture.ledger, fixture.requests, &new_sem) == 2);
    CHECK(fixture.registry.semaphore_notification_serial == old_serial + 1u);
    complete_woken_waiter(&fixture, 1, 1, generation);
    CHECK(p4_task_registry_claim_next_semaphore_notification_locked(
        &fixture.registry, &new_snapshot) == 1);
    CHECK(new_snapshot.identity == old_snapshot.identity);
    CHECK(new_snapshot.count == old_snapshot.count);
    CHECK(new_snapshot.generation > old_snapshot.generation);

    before = fixture;
    CHECK(p4_task_registry_finish_semaphore_notification_locked(
        &fixture.registry, &old_snapshot) == 0);
    CHECK(memcmp(&before, &fixture, sizeof(fixture)) == 0);
    CHECK(p4_task_registry_finish_semaphore_notification_locked(
        &fixture.registry, &new_snapshot) == 1);
}

static void test_overflow_floor_and_bad_registry_shapes(void)
{
    struct Fixture fixture;
    struct SignalSemaphore sem;
    struct SignalSemaphore *dups[2] = { &sem, &sem };
    const uint32_t shared[2] = { 1, 1 };
    struct Fixture before;
    struct SignalSemaphore sem_before;
    struct p4_semaphore_notification output, output_before;
    unsigned char misaligned[sizeof(struct p4_semaphore_notification) + 1u];
    uintptr_t wrapped_value;
    struct p4_semaphore_notification *wrapped;

    empty_sem(&sem);
    fixture_init(&fixture);
    register_all_running(&fixture);
    completed_holder(&fixture, 0, 0, &sem);
    make_operation(&fixture, 1, 2, dups, shared);
    CHECK(p4_semaphore_nodes_acquire_locked(&fixture.registry,
        &fixture.ledger, fixture.requests, fixture.tasks[1],
        fixture.records[1].wait_scope.generation, 0, &sem) == 2);
    CHECK(p4_semaphore_nodes_acquire_locked(&fixture.registry,
        &fixture.ledger, fixture.requests, fixture.tasks[1],
        fixture.records[1].wait_scope.generation, 1, &sem) == 2);
    CHECK(p4_task_registry_leave_running_locked(&fixture.registry,
        fixture.tasks[1], 1) == 1);
    /* Valid boundary state: one scope pin plus near-maximum reservations. */
    fixture.registry.semaphore_notification_serial = 1;
    fixture.records[1].semaphore_pending = UINT32_MAX - 1u;
    fixture.records[1].semaphore_pending_generation = 1;
    fixture.records[1].lifetime.notification_pins = UINT32_MAX;
    before = fixture;
    sem_before = sem;
    CHECK(p4_semaphore_nodes_release_durable_locked(&fixture.registry,
        &fixture.ledger, fixture.requests, &sem) == 0);
    CHECK(memcmp(&before, &fixture, sizeof(fixture)) == 0);
    CHECK(memcmp(&sem_before, &sem, sizeof(sem)) == 0);

    /* Claim and finish reject malformed generation/floor without changing
     * the registry or an already initialized output snapshot. */
    fixture_init(&fixture);
    register_all_running(&fixture);
    fixture.records[0].semaphore_pending = 1;
    fixture.records[0].semaphore_pending_generation = 1;
    fixture.records[0].lifetime.notification_pins = 1;
    memset(&output, 0x6b, sizeof(output));
    output_before = output;
    before = fixture;
    CHECK(p4_task_registry_claim_next_semaphore_notification_locked(
        &fixture.registry, &output) == -1);
    CHECK(memcmp(&before, &fixture, sizeof(fixture)) == 0);
    CHECK(memcmp(&output_before, &output, sizeof(output)) == 0);

    fixture.registry.semaphore_notification_serial = 1;
    fixture.records[0].semaphore_pending_generation = 0;
    before = fixture;
    CHECK(p4_task_registry_claim_next_semaphore_notification_locked(
        &fixture.registry, &output) == -1);
    CHECK(memcmp(&before, &fixture, sizeof(fixture)) == 0);
    CHECK(memcmp(&output_before, &output, sizeof(output)) == 0);

    fixture.records[0].semaphore_pending = 0;
    fixture.records[0].semaphore_pending_generation = 0;
    fixture.records[0].lifetime.notification_pins = 0;
    fixture.records[0].semaphore_inflight = 1;
    fixture.records[0].semaphore_inflight_generation = 1;
    before = fixture;
    CHECK(p4_task_registry_claim_next_semaphore_notification_locked(
        &fixture.registry, &output) == -1);
    CHECK(memcmp(&before, &fixture, sizeof(fixture)) == 0);
    CHECK(memcmp(&output_before, &output, sizeof(output)) == 0);

    /* Output aliases, null, misalignment, and address-range wrap are all
     * refused before any read/write through the snapshot pointer. */
    fixture_init(&fixture);
    register_all_running(&fixture);
    memset(&output, 0x39, sizeof(output));
    output_before = output;
    before = fixture;
    CHECK(p4_task_registry_claim_next_semaphore_notification_locked(
        &fixture.registry, NULL) == -1);
    CHECK(p4_task_registry_claim_next_semaphore_notification_locked(
        &fixture.registry,
        (struct p4_semaphore_notification *)(void *)&fixture.registry) == -1);
    CHECK(p4_task_registry_claim_next_semaphore_notification_locked(
        &fixture.registry,
        (struct p4_semaphore_notification *)(void *)&fixture.records[0]) == -1);
    CHECK(p4_task_registry_claim_next_semaphore_notification_locked(
        &fixture.registry,
        (struct p4_semaphore_notification *)(void *)&fixture.records[0].wait_scope) == -1);
    CHECK(p4_task_registry_claim_next_semaphore_notification_locked(
        &fixture.registry,
        (struct p4_semaphore_notification *)(void *)(misaligned + 1u)) == -1);
    wrapped_value = UINTPTR_MAX &
        ~(uintptr_t)(_Alignof(struct p4_semaphore_notification) - 1u);
    wrapped = (struct p4_semaphore_notification *)wrapped_value;
    CHECK(wrapped_value > UINTPTR_MAX - sizeof(*wrapped));
    CHECK(p4_task_registry_claim_next_semaphore_notification_locked(
        &fixture.registry, wrapped) == -1);
    CHECK(memcmp(&before, &fixture, sizeof(fixture)) == 0);
    CHECK(memcmp(&output_before, &output, sizeof(output)) == 0);
    CHECK(p4_task_registry_finish_semaphore_notification_locked(
        &fixture.registry, NULL) == -1);
    CHECK(p4_task_registry_finish_semaphore_notification_locked(
        &fixture.registry,
        (const struct p4_semaphore_notification *)(const void *)&fixture.registry) == -1);
    CHECK(p4_task_registry_finish_semaphore_notification_locked(
        &fixture.registry,
        (const struct p4_semaphore_notification *)(const void *)&fixture.records[0]) == -1);
    CHECK(p4_task_registry_finish_semaphore_notification_locked(
        &fixture.registry,
        (const struct p4_semaphore_notification *)(const void *)(misaligned + 1u)) == -1);
    CHECK(p4_task_registry_finish_semaphore_notification_locked(
        &fixture.registry, wrapped) == -1);
    CHECK(memcmp(&before, &fixture, sizeof(fixture)) == 0);
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

    test_mixed_cohort_and_durable_delivery();
    test_duplicate_shared_batch_and_empty_release();
    test_serial_exhaustion_and_same_batch_coalescing();
    test_cancelled_owned_scope_keeps_pending_pin();
    test_task_record_reuse_keeps_serial_aba_safe();
    test_overflow_floor_and_bad_registry_shapes();

    CHECK(munmap(task_page, task_page_size) == 0);
    printf("p4 semaphore durable-notification checks: %u\n", checks);
    return 0;
}
