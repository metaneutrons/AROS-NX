/* Actual-source tests for the private semaphore cancellation snapshot leaf. */
#define main p4_semaphore_nodes_baseline_main
#include "p4-semaphore-nodes-test.c"
#undef main

#include "../p4_semaphore_cancel.c"

static void *semaphore_page;
static size_t semaphore_page_size;

static struct p4_semaphore_ledger_operation *operation_for_task(
    struct Fixture *fixture, unsigned task)
{
    return (struct p4_semaphore_ledger_operation *)
        fixture->records[task].wait_scope.list_index;
}

static void retire_and_claim(struct Fixture *fixture, unsigned task,
    struct p4_task_wait_cancel_snapshot *cancel)
{
    CHECK(p4_task_registry_retire_with_ledger_locked(&fixture->registry,
        &fixture->ledger, fixture->tasks[task], 0) == 1);
    CHECK(p4_task_registry_claim_next_wait_cancel_with_ledger_locked(
        &fixture->registry, &fixture->ledger, cancel) == 1);
    CHECK(cancel->identity == fixture->tasks[task]);
}

static void check_snapshot(struct Fixture *fixture, uint32_t generation,
    uint32_t ordinal, struct SignalSemaphore *expected_sem,
    uint32_t expected_count, uint32_t expected_state, uint32_t committed,
    uint32_t running_harts)
{
    struct p4_semaphore_cancel_entry_snapshot output;
    struct Fixture before = *fixture;
    int result;

    memset(&output, 0xa5, sizeof(output));
    result = p4_semaphore_cancel_describe_locked(&fixture->registry,
        &fixture->ledger, fixture->requests, fixture->tasks[0], generation,
        ordinal, &output);
    CHECK(result == 1);
    CHECK(output.semaphore == expected_sem);
    CHECK(output.entry_count == expected_count);
    CHECK(output.state == expected_state);
    CHECK(output.committed == committed);
    CHECK(output.running_harts == running_harts);
    CHECK(memcmp(&before, fixture, sizeof(*fixture)) == 0);
}

static void test_all_entry_states_and_reuse(void)
{
    struct Fixture fixture;
    struct SignalSemaphore unvisited, owned, queued;
    struct SignalSemaphore *semaphores[4];
    const uint32_t modes[4] = { 0, 0, 0, 0 };
    struct p4_task_wait_cancel_snapshot cancel;
    struct p4_semaphore_cancel_entry_snapshot output;
    struct p4_semaphore_cancel_entry_snapshot saved;
    struct p4_semaphore_ledger_operation *operation;
    const void *old_request;
    struct Fixture before;
    struct SignalSemaphore owned_before, queued_before;
    uint32_t holder_generation, generation;
    struct SignalSemaphore *reuse_sem[1];
    const uint32_t reuse_mode[1] = { 0 };
    uint32_t reuse_generation;

    empty_sem(&unvisited);
    empty_sem(&owned);
    empty_sem(&queued);
    fixture_init(&fixture);
    register_running(&fixture, 0, 0);
    register_running(&fixture, 1, 1);
    register_running(&fixture, 2, 0);
    register_running(&fixture, 3, 1);

    holder_generation = make_operation(&fixture, 1, 1,
        (struct SignalSemaphore *const[]){ &queued }, reuse_mode);
    CHECK(p4_semaphore_nodes_acquire_locked(&fixture.registry,
        &fixture.ledger, fixture.requests, fixture.tasks[1],
        holder_generation, 0, &queued) == 1);
    CHECK(p4_task_registry_wait_operation_complete_locked(&fixture.registry,
        &fixture.ledger, fixture.tasks[1], holder_generation) == 1);

    semaphores[0] = &unvisited;
    semaphores[1] = &owned;
    semaphores[2] = &queued;
    semaphores[3] = &owned;
    generation = make_operation(&fixture, 0, 4, semaphores, modes);
    CHECK(p4_semaphore_nodes_acquire_locked(&fixture.registry,
        &fixture.ledger, fixture.requests, fixture.tasks[0], generation, 1,
        &owned) == 1);
    CHECK(p4_semaphore_nodes_acquire_locked(&fixture.registry,
        &fixture.ledger, fixture.requests, fixture.tasks[0], generation, 2,
        &queued) == 2);
    CHECK(p4_semaphore_nodes_acquire_locked(&fixture.registry,
        &fixture.ledger, fixture.requests, fixture.tasks[0], generation, 3,
        &owned) == 1);
    operation = operation_for_task(&fixture, 0);
    CHECK(operation != NULL && operation->entry_count == 4);
    old_request = operation->first->request;
    CHECK(operation->first->state == P4_SEMAPHORE_LEDGER_ENTRY_UNVISITED);
    CHECK(operation->first->operation_next->state ==
        P4_SEMAPHORE_LEDGER_ENTRY_OWNED);
    CHECK(operation->first->operation_next->operation_next->state ==
        P4_SEMAPHORE_LEDGER_ENTRY_QUEUED);
    CHECK(operation->first->operation_next->operation_next->operation_next->state ==
        P4_SEMAPHORE_LEDGER_ENTRY_OWNED);

    /* An active, not-yet-retired scope and a stale generation do not publish
     * a partial output. These checks intentionally precede cancellation. */
    memset(&output, 0x5a, sizeof(output));
    saved = output;
    before = fixture;
    CHECK(p4_semaphore_cancel_describe_locked(&fixture.registry,
        &fixture.ledger, fixture.requests, fixture.tasks[0], generation, 0,
        &output) == 0);
    CHECK(memcmp(&output, &saved, sizeof(output)) == 0);
    CHECK(memcmp(&before, &fixture, sizeof(fixture)) == 0);
    CHECK(p4_semaphore_cancel_describe_locked(&fixture.registry,
        &fixture.ledger, fixture.requests, fixture.tasks[0], generation + 1u,
        0, &output) == 0);
    CHECK(memcmp(&output, &saved, sizeof(output)) == 0);
    CHECK(p4_semaphore_cancel_describe_locked(&fixture.registry,
        &fixture.ledger, fixture.requests, task_identity(4), generation,
        0, &output) == 0);
    CHECK(memcmp(&output, &saved, sizeof(output)) == 0);
    CHECK(p4_semaphore_cancel_describe_locked(&fixture.registry,
        &fixture.ledger, fixture.requests, fixture.tasks[3], generation,
        0, &output) == 0);
    CHECK(memcmp(&output, &saved, sizeof(output)) == 0);

    retire_and_claim(&fixture, 0, &cancel);
    CHECK(cancel.generation == generation && cancel.queued_count == 1);
    CHECK(p4_semaphore_cancel_describe_locked(&fixture.registry,
        &fixture.ledger, fixture.requests, fixture.tasks[0], generation, 99,
        &output) == 0);
    CHECK(memcmp(&output, &saved, sizeof(output)) == 0);

    owned_before = owned;
    queued_before = queued;
    check_snapshot(&fixture, generation, 0, semaphores[0], 4,
        P4_SEMAPHORE_LEDGER_ENTRY_UNVISITED, 0, 1);
    check_snapshot(&fixture, generation, 1, &owned, 4,
        P4_SEMAPHORE_LEDGER_ENTRY_OWNED, 0, 1);
    check_snapshot(&fixture, generation, 2, &queued, 4,
        P4_SEMAPHORE_LEDGER_ENTRY_QUEUED, 0, 1);
    check_snapshot(&fixture, generation, 3, &owned, 4,
        P4_SEMAPHORE_LEDGER_ENTRY_OWNED, 0, 1);
    CHECK(memcmp(&owned_before, &owned, sizeof(owned)) == 0);
    CHECK(memcmp(&queued_before, &queued, sizeof(queued)) == 0);

    /* The queue entry is withdrawn through the real node helper. The other
     * owned recursion is rolled back nonlast, yielding a RELEASED entry. */
    CHECK(p4_semaphore_nodes_withdraw_locked(&fixture.registry,
        &fixture.ledger, fixture.requests, fixture.tasks[0], generation, 2,
        &queued) == 1);
    CHECK(p4_task_registry_leave_running_locked(&fixture.registry,
        fixture.tasks[0], 0) == 1);
    CHECK(p4_semaphore_nodes_rollback_locked(&fixture.registry,
        &fixture.ledger, fixture.requests, fixture.tasks[0], generation, 3,
        &owned) == 1);
    CHECK(owned.ss_NestCount == 1 && owned.ss_QueueCount == 0);
    check_snapshot(&fixture, generation, 0, semaphores[0], 4,
        P4_SEMAPHORE_LEDGER_ENTRY_UNVISITED, 0, 0);
    check_snapshot(&fixture, generation, 1, &owned, 4,
        P4_SEMAPHORE_LEDGER_ENTRY_OWNED, 0, 0);
    check_snapshot(&fixture, generation, 2, &queued, 4,
        P4_SEMAPHORE_LEDGER_ENTRY_WITHDRAWN, 0, 0);
    check_snapshot(&fixture, generation, 3, &owned, 4,
        P4_SEMAPHORE_LEDGER_ENTRY_RELEASED, 0, 0);

    /* Retire the final owned unit, finish the cancellation, then reuse the
     * arena slots. A prior snapshot remains a value copy, not an arena borrow. */
    CHECK(p4_semaphore_nodes_rollback_locked(&fixture.registry,
        &fixture.ledger, fixture.requests, fixture.tasks[0], generation, 1,
        &owned) == 1);
    CHECK(owned.ss_NestCount == 0 && owned.ss_Owner == NULL);
    memset(&output, 0, sizeof(output));
    CHECK(p4_semaphore_cancel_describe_locked(&fixture.registry,
        &fixture.ledger, fixture.requests, fixture.tasks[0], generation, 0,
        &output) == 1);
    saved = output;
    CHECK(output.semaphore == semaphores[0]);
    CHECK(p4_task_registry_wait_operation_finish_cancel_locked(
        &fixture.registry, &fixture.ledger, fixture.tasks[0], generation) == 1);
    reuse_sem[0] = &unvisited;
    reuse_generation = make_operation(&fixture, 2, 1, reuse_sem, reuse_mode);
    CHECK(reuse_generation != 0);
    CHECK(operation_for_task(&fixture, 2)->first->request == old_request);
    CHECK(memcmp(&output, &saved, sizeof(output)) == 0);
    CHECK(p4_semaphore_cancel_describe_locked(&fixture.registry,
        &fixture.ledger, fixture.requests, fixture.tasks[0], generation, 0,
        &output) == 0);
    CHECK(memcmp(&output, &saved, sizeof(output)) == 0);
}

static void test_opaque_semaphore_identity_is_not_dereferenced(void)
{
    struct Fixture fixture;
    struct p4_task_wait_operation wait;
    struct p4_task_wait_cancel_snapshot cancel;
    struct p4_semaphore_cancel_entry_snapshot output, saved;
    struct Fixture before;

    fixture_init(&fixture);
    register_running(&fixture, 0, 0);
    register_running(&fixture, 1, 1);
    CHECK(p4_task_registry_wait_reserve_operation_locked(&fixture.registry,
        &fixture.ledger, fixture.tasks[0], 1, &wait) == 1);
    CHECK(wait.generation != 0);
    CHECK(p4_task_registry_wait_operation_set_entry_locked(&fixture.registry,
        &fixture.ledger, fixture.tasks[0], wait.generation, 0, semaphore_page,
        &fixture.requests[0]) == 1);
    CHECK(p4_task_registry_wait_operation_close_locked(&fixture.registry,
        &fixture.ledger, fixture.tasks[0], wait.generation) == 1);
    CHECK(p4_task_registry_leave_running_locked(&fixture.registry,
        fixture.tasks[0], 0) == 1);
    retire_and_claim(&fixture, 0, &cancel);

    before = fixture;
    memset(&output, 0x3d, sizeof(output));
    CHECK(p4_semaphore_cancel_describe_locked(&fixture.registry,
        &fixture.ledger, fixture.requests, fixture.tasks[0], wait.generation,
        0, &output) == 1);
    CHECK(output.semaphore == (struct SignalSemaphore *)semaphore_page);
    CHECK(output.entry_count == 1);
    CHECK(output.state == P4_SEMAPHORE_LEDGER_ENTRY_UNVISITED);
    CHECK(output.committed == 0 && output.running_harts == 0);
    CHECK(memcmp(&before, &fixture, sizeof(fixture)) == 0);

    /* The output itself aliases the inaccessible semaphore identity. The
     * overlap check must reject it without reading or writing that page. */
    CHECK(p4_semaphore_cancel_describe_locked(&fixture.registry,
        &fixture.ledger, fixture.requests, fixture.tasks[0], wait.generation,
        0, (struct p4_semaphore_cancel_entry_snapshot *)semaphore_page) == -1);
    CHECK(memcmp(&before, &fixture, sizeof(fixture)) == 0);
    saved = output;
    CHECK(p4_task_registry_wait_operation_finish_cancel_locked(
        &fixture.registry, &fixture.ledger, fixture.tasks[0], wait.generation) == 1);

    /* Reuse the operation/request slots for another scope; the copied output
     * remains unchanged and contains no ledger or request address. */
    CHECK(p4_task_registry_wait_reserve_operation_locked(&fixture.registry,
        &fixture.ledger, fixture.tasks[1], 1, &wait) == 1);
    CHECK(p4_task_registry_wait_operation_set_entry_locked(&fixture.registry,
        &fixture.ledger, fixture.tasks[1], wait.generation, 0, semaphore_page,
        &fixture.requests[0]) == 1);
    CHECK(p4_task_registry_wait_operation_close_locked(&fixture.registry,
        &fixture.ledger, fixture.tasks[1], wait.generation) == 1);
    CHECK(memcmp(&output, &saved, sizeof(output)) == 0);
}

static void test_committed_snapshot_and_no_rollback(void)
{
    struct Fixture fixture;
    struct SignalSemaphore sem;
    struct SignalSemaphore *semaphores[1] = { &sem };
    const uint32_t modes[1] = { 0 };
    struct p4_task_wait_cancel_snapshot cancel;
    struct p4_semaphore_cancel_entry_snapshot output;
    struct Fixture before;
    struct SignalSemaphore sem_before;
    uint32_t generation;

    empty_sem(&sem);
    fixture_init(&fixture);
    register_running(&fixture, 0, 0);
    generation = make_operation(&fixture, 0, 1, semaphores, modes);
    CHECK(p4_semaphore_nodes_acquire_locked(&fixture.registry,
        &fixture.ledger, fixture.requests, fixture.tasks[0], generation, 0,
        &sem) == 1);
    CHECK(p4_semaphore_ledger_commit(&fixture.ledger,
        operation_for_task(&fixture, 0)) == 1);
    CHECK(p4_task_registry_retire_with_ledger_locked(&fixture.registry,
        &fixture.ledger, fixture.tasks[0], 0) == 1);
    CHECK(p4_task_registry_leave_running_locked(&fixture.registry,
        fixture.tasks[0], 0) == 1);
    CHECK(p4_task_registry_claim_next_wait_cancel_with_ledger_locked(
        &fixture.registry, &fixture.ledger, &cancel) == 1);
    CHECK(cancel.identity == fixture.tasks[0]);

    before = fixture;
    sem_before = sem;
    CHECK(p4_semaphore_cancel_describe_locked(&fixture.registry,
        &fixture.ledger, fixture.requests, fixture.tasks[0], generation, 0,
        &output) == 1);
    CHECK(output.semaphore == &sem && output.entry_count == 1);
    CHECK(output.state == P4_SEMAPHORE_LEDGER_ENTRY_OWNED);
    CHECK(output.committed == 1 && output.running_harts == 0);
    CHECK(memcmp(&before, &fixture, sizeof(fixture)) == 0);
    CHECK(memcmp(&sem_before, &sem, sizeof(sem)) == 0);

    CHECK(p4_semaphore_nodes_rollback_durable_locked(&fixture.registry,
        &fixture.ledger, fixture.requests, fixture.tasks[0], generation, 0,
        &sem) == 0);
    CHECK(memcmp(&before, &fixture, sizeof(fixture)) == 0);
    CHECK(memcmp(&sem_before, &sem, sizeof(sem)) == 0);
}

static void test_alias_malformed_and_invalid_outputs(void)
{
    struct Fixture fixture;
    struct SignalSemaphore unvisited, owned, queued;
    struct SignalSemaphore *semaphores[4];
    const uint32_t modes[4] = { 0, 0, 0, 0 };
    struct p4_task_wait_cancel_snapshot cancel;
    struct p4_semaphore_cancel_entry_snapshot output, saved;
    struct Fixture before;
    struct SignalSemaphore unvisited_before, owned_before, queued_before;
    _Alignas(16) unsigned char misaligned_storage[
        sizeof(output) + _Alignof(struct p4_semaphore_cancel_entry_snapshot)];
    uintptr_t wrapped_value;
    struct p4_semaphore_cancel_entry_snapshot *wrapped;
    uint32_t holder_generation, generation;

    empty_sem(&unvisited);
    empty_sem(&owned);
    empty_sem(&queued);
    fixture_init(&fixture);
    register_running(&fixture, 0, 0);
    register_running(&fixture, 1, 1);
    register_running(&fixture, 2, 0);
    register_running(&fixture, 3, 1);
    holder_generation = make_operation(&fixture, 1, 1,
        (struct SignalSemaphore *const[]){ &queued },
        (const uint32_t[]){ 0 });
    CHECK(p4_semaphore_nodes_acquire_locked(&fixture.registry,
        &fixture.ledger, fixture.requests, fixture.tasks[1],
        holder_generation, 0, &queued) == 1);
    semaphores[0] = &unvisited;
    semaphores[1] = &owned;
    semaphores[2] = &queued;
    semaphores[3] = &owned;
    generation = make_operation(&fixture, 0, 4, semaphores, modes);
    CHECK(p4_semaphore_nodes_acquire_locked(&fixture.registry,
        &fixture.ledger, fixture.requests, fixture.tasks[0], generation, 1,
        &owned) == 1);
    CHECK(p4_semaphore_nodes_acquire_locked(&fixture.registry,
        &fixture.ledger, fixture.requests, fixture.tasks[0], generation, 2,
        &queued) == 2);
    CHECK(p4_semaphore_nodes_acquire_locked(&fixture.registry,
        &fixture.ledger, fixture.requests, fixture.tasks[0], generation, 3,
        &owned) == 1);
    retire_and_claim(&fixture, 0, &cancel);

    memset(&output, 0x6c, sizeof(output));
    saved = output;
    before = fixture;
    unvisited_before = unvisited;
    owned_before = owned;
    queued_before = queued;
#define CHECK_ALIAS_REFUSED(alias_pointer, ordinal_value) do { \
    CHECK(p4_semaphore_cancel_describe_locked(&fixture.registry, \
        &fixture.ledger, fixture.requests, fixture.tasks[0], generation, (ordinal_value), \
        (struct p4_semaphore_cancel_entry_snapshot *)(void *)(alias_pointer)) == -1); \
    CHECK(memcmp(&before, &fixture, sizeof(fixture)) == 0); \
    CHECK(memcmp(&unvisited_before, &unvisited, sizeof(unvisited)) == 0); \
    CHECK(memcmp(&owned_before, &owned, sizeof(owned)) == 0); \
    CHECK(memcmp(&queued_before, &queued, sizeof(queued)) == 0); \
} while (0)
    CHECK_ALIAS_REFUSED(&fixture.registry, 0);
    CHECK_ALIAS_REFUSED(&fixture.ledger, 0);
    CHECK_ALIAS_REFUSED(&fixture.operations[0], 0);
    CHECK_ALIAS_REFUSED(&fixture.entries[0], 0);
    CHECK_ALIAS_REFUSED(&fixture.requests[0], 0);
    CHECK_ALIAS_REFUSED(&fixture.records[0], 0);
    CHECK_ALIAS_REFUSED(&unvisited, 0);
    CHECK_ALIAS_REFUSED(&owned, 1);
#undef CHECK_ALIAS_REFUSED
    CHECK(p4_semaphore_cancel_describe_locked(&fixture.registry,
        &fixture.ledger, fixture.requests, fixture.tasks[0], generation, 0,
        NULL) == -1);
    CHECK(p4_semaphore_cancel_describe_locked(&fixture.registry,
        &fixture.ledger, fixture.requests, fixture.tasks[0], generation, 0,
        (struct p4_semaphore_cancel_entry_snapshot *)(void *)
            (misaligned_storage + 1u)) == -1);
    wrapped_value = UINTPTR_MAX &
        ~(uintptr_t)(_Alignof(struct p4_semaphore_cancel_entry_snapshot) - 1u);
    wrapped = (struct p4_semaphore_cancel_entry_snapshot *)wrapped_value;
    CHECK(wrapped_value > UINTPTR_MAX - sizeof(*wrapped));
    CHECK(p4_semaphore_cancel_describe_locked(&fixture.registry,
        &fixture.ledger, fixture.requests, fixture.tasks[0], generation, 0,
        wrapped) == -1);
    CHECK(memcmp(&before, &fixture, sizeof(fixture)) == 0);
    CHECK(memcmp(&saved, &output, sizeof(output)) == 0);
    CHECK(memcmp(&owned_before, &owned, sizeof(owned)) == 0);
    CHECK(memcmp(&queued_before, &queued, sizeof(queued)) == 0);

    /* Invalid ledger shape is rejected before publishing any snapshot. */
    fixture.ledger.active_entries += 1u;
    before = fixture;
    memset(&output, 0x39, sizeof(output));
    saved = output;
    CHECK(p4_semaphore_cancel_describe_locked(&fixture.registry,
        &fixture.ledger, fixture.requests, fixture.tasks[0], generation, 0,
        &output) == -1);
    CHECK(memcmp(&before, &fixture, sizeof(fixture)) == 0);
    CHECK(memcmp(&saved, &output, sizeof(output)) == 0);
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
    semaphore_page_size = task_page_size;
    semaphore_page = mmap(NULL, semaphore_page_size, PROT_NONE,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(semaphore_page != MAP_FAILED);
    CHECK((uintptr_t)semaphore_page % _Alignof(struct SignalSemaphore) == 0);

    test_all_entry_states_and_reuse();
    test_opaque_semaphore_identity_is_not_dereferenced();
    test_committed_snapshot_and_no_rollback();
    test_alias_malformed_and_invalid_outputs();

    CHECK(munmap(semaphore_page, semaphore_page_size) == 0);
    CHECK(munmap(task_page, task_page_size) == 0);
    printf("p4 semaphore cancel-snapshot checks: %u\n", checks);
    return 0;
}
