/* Actual-source boundary tests for orphaned constructor-reference release. */
#define main p4_orphan_boundary_dependency_main
#include "p4-semaphore-operation-admission-test.c"
#undef main
#include "../../exec/p4_remove_bank.c"
#include "../../exec/p4_remove_completion.c"

static void expect_orphan_result(struct direct_fixture *fixture,
    struct Task *target, struct Task *creator, uint32_t generation,
    int expected)
{
    struct byte_snapshot before;
    int result;

    snapshot_fixture(fixture, &before);
    result = p4_task_registry_finish_orphan_construction_locked(
        &fixture->registry, target, creator, generation);
    CHECK(result == expected);
    CHECK(fixture_matches_snapshot(fixture, &before));
}

static void retire_service_and_drain(struct direct_fixture *fixture,
    struct Task *identity, uint32_t hart, int was_running)
{
    CHECK(p4_task_registry_retire_for_service_locked(
        &fixture->registry, identity) == 1);
    if (was_running)
        CHECK(p4_task_registry_leave_running_locked(
            &fixture->registry, identity, hart) == 1);
}

static void test_two_children_and_reserved_pins(void *page)
{
    struct direct_fixture fixture;
    struct p4_task_registry_record *creator_record;
    struct p4_task_registry_record *first_record;
    struct p4_task_registry_record *second_record;
    struct p4_task_registry_record *worker_record;
    struct p4_task_wait_cancel_snapshot cancel;
    struct SignalSemaphore semaphore;
    struct SemaphoreRequest request;
    struct Task *creator = guarded_identity(page, 64u);
    struct Task *first = guarded_identity(page, 128u);
    struct Task *second = guarded_identity(page, 192u);
    struct Task *worker = guarded_identity(page, 256u);
    uint32_t first_generation = 0u;
    uint32_t second_generation = 0u;
    uint32_t wait_generation = 0u;

    memset(&semaphore, 0, sizeof(semaphore));
    memset(&request, 0, sizeof(request));
    fixture_init(&fixture, TEST_OP_CAPACITY, TEST_ENTRY_CAPACITY);
    creator_record = fixture_add_task(&fixture, 0u, creator, 0u);
    CHECK(p4_task_registry_register_constructing_locked(
        &fixture.registry, &fixture.records[1], first, creator, 0u,
        &first_generation) == 1);
    CHECK(p4_task_registry_register_constructing_locked(
        &fixture.registry, &fixture.records[2], second, creator, 0u,
        &second_generation) == 1);
    first_record = &fixture.records[1];
    second_record = &fixture.records[2];
    CHECK(creator_record->construction_children == 2u);
    CHECK(first_generation != 0u && second_generation != 0u &&
          first_generation != second_generation);

    /* Preserve ordinary references and another real reserved pin category
     * while the worker releases only each edge's designated construction pin. */
    CHECK(p4_task_registry_pin_locked(&fixture.registry, creator) == 1);
    CHECK(p4_task_registry_pin_locked(&fixture.registry, first) == 1);
    CHECK(p4_task_registry_pin_locked(&fixture.registry, second) == 1);
    worker_record = fixture_add_task(&fixture, 3u, worker, 1u);
    CHECK(p4_task_registry_protect_worker_locked(
        &fixture.registry, worker) == 1);
    CHECK(worker_record->worker_protected == 1u);
    CHECK(worker_record->lifetime.notification_pins == 1u);

    /* A real wait-scope reservation demonstrates that orphan release leaves
     * an unrelated designated pin and cancellation state untouched. The
     * fixture does not insert this opaque request into an Exec queue. */
    CHECK(p4_task_registry_enter_running_locked(
        &fixture.registry, first, 0u) == 1);
    CHECK(p4_task_registry_wait_begin_locked(&fixture.registry, first,
        P4_TASK_WAIT_SINGLE, &semaphore, &request, NULL,
        &wait_generation) == 1);
    CHECK(wait_generation != 0u);
    CHECK(p4_task_registry_wait_close_locked(
        &fixture.registry, first, wait_generation) == 1);

    /* Before retirement there is no orphan service authority. */
    expect_orphan_result(&fixture, first, creator, first_generation, 0);

    retire_service_and_drain(&fixture, creator, 0u, 1);
    retire_service_and_drain(&fixture, first, 0u, 1);
    retire_service_and_drain(&fixture, second, 0u, 0);
    CHECK(p4_task_registry_claim_next_wait_cancel_locked(
        &fixture.registry, &cancel) == 1);
    CHECK(cancel.identity == first && cancel.generation == wait_generation);
    CHECK(cancel.cancel_requested == 1u);
    CHECK(first_record->wait_scope.cancel_claimed == 1u);

    CHECK(creator_record->lifetime.notification_pins == 4u);
    CHECK(first_record->lifetime.notification_pins == 4u);
    CHECK(second_record->lifetime.notification_pins == 3u);
    CHECK(p4_task_registry_finish_orphan_construction_locked(
        &fixture.registry, first, creator, first_generation) == 1);
    CHECK(first_record->construction_creator == NULL);
    CHECK(creator_record->construction_children == 1u);
    CHECK(creator_record->lifetime.notification_pins == 3u);
    CHECK(first_record->lifetime.notification_pins == 3u);
    CHECK(first_record->service_pending == 1u);
    CHECK(first_record->service_generation != 0u);
    CHECK(first_record->wait_scope.active == 1u);
    CHECK(first_record->wait_scope.cancel_claimed == 1u);
    CHECK(worker_record->worker_protected == 1u);
    CHECK(worker_record->lifetime.notification_pins == 1u);

    /* The released edge is gone, while the second edge remains independently
     * live and its refusal changes no byte. */
    expect_orphan_result(&fixture, first, creator, first_generation, 0);
    CHECK(p4_task_registry_wait_finish_cancel_locked(
        &fixture.registry, first, wait_generation) == 1);
    CHECK(first_record->wait_scope.active == 0u);
    CHECK(first_record->lifetime.notification_pins == 2u);
    CHECK(p4_task_registry_unpin_locked(&fixture.registry, first) == 1);
    CHECK(first_record->lifetime.notification_pins == 1u);
    CHECK(p4_task_registry_take_service_ready_locked(
        &fixture.registry, first) == first);
    CHECK(first_record->service_queued == 1u);

    CHECK(p4_task_registry_finish_orphan_construction_locked(
        &fixture.registry, second, creator, second_generation) == 1);
    CHECK(second_record->construction_creator == NULL);
    CHECK(creator_record->construction_children == 0u);
    CHECK(creator_record->lifetime.notification_pins == 2u);
    CHECK(second_record->lifetime.notification_pins == 2u);
    CHECK(second_record->service_pending == 1u);
    CHECK(second_record->service_generation != 0u);
    CHECK(p4_task_registry_unpin_locked(&fixture.registry, second) == 1);
    CHECK(second_record->lifetime.notification_pins == 1u);
    CHECK(p4_task_registry_take_service_ready_locked(
        &fixture.registry, second) == second);
    CHECK(p4_task_registry_unpin_locked(&fixture.registry, creator) == 1);
    CHECK(creator_record->lifetime.notification_pins == 1u);
    CHECK(p4_task_registry_take_service_ready_locked(
        &fixture.registry, creator) == creator);
    CHECK(creator_record->service_queued == 1u);
    CHECK(worker_record->worker_protected == 1u);
    CHECK(worker_record->lifetime.notification_pins == 1u);
    CHECK(mock_fatal_calls == 0u);
}

static void test_nested_retired_chain(void *page)
{
    struct direct_fixture fixture;
    struct p4_task_registry_record *a_record;
    struct p4_task_registry_record *b_record;
    struct p4_task_registry_record *c_record;
    struct Task *a = guarded_identity(page, 320u);
    struct Task *b = guarded_identity(page, 384u);
    struct Task *c = guarded_identity(page, 448u);
    uint32_t ab_generation = 0u;
    uint32_t bc_generation = 0u;

    fixture_init(&fixture, TEST_OP_CAPACITY, TEST_ENTRY_CAPACITY);
    a_record = fixture_add_task(&fixture, 0u, a, 0u);
    CHECK(p4_task_registry_register_constructing_locked(
        &fixture.registry, &fixture.records[1], b, a, 0u,
        &ab_generation) == 1);
    b_record = &fixture.records[1];
    CHECK(p4_task_registry_enter_running_locked(
        &fixture.registry, b, 1u) == 1);
    CHECK(p4_task_registry_register_constructing_locked(
        &fixture.registry, &fixture.records[2], c, b, 1u,
        &bc_generation) == 1);
    c_record = &fixture.records[2];
    CHECK(p4_task_registry_pin_locked(&fixture.registry, a) == 1);
    CHECK(p4_task_registry_pin_locked(&fixture.registry, b) == 1);
    CHECK(p4_task_registry_pin_locked(&fixture.registry, c) == 1);
    CHECK(a_record->construction_children == 1u);
    CHECK(b_record->construction_children == 1u);

    retire_service_and_drain(&fixture, a, 0u, 1);
    retire_service_and_drain(&fixture, b, 1u, 1);
    retire_service_and_drain(&fixture, c, 0u, 0);
    CHECK(p4_task_registry_finish_orphan_construction_locked(
        &fixture.registry, c, b, bc_generation) == 1);
    CHECK(b_record->construction_children == 0u);
    CHECK(b_record->lifetime.notification_pins == 3u);
    CHECK(c_record->construction_creator == NULL);
    CHECK(c_record->lifetime.notification_pins == 2u);
    CHECK(a_record->construction_children == 1u);
    CHECK(b_record->construction_creator == a);
    CHECK(p4_task_registry_finish_orphan_construction_locked(
        &fixture.registry, b, a, ab_generation) == 1);
    CHECK(a_record->construction_children == 0u);
    CHECK(b_record->construction_creator == NULL);
    CHECK(a_record->lifetime.notification_pins == 2u);
    CHECK(b_record->lifetime.notification_pins == 2u);
    CHECK(a_record->service_pending == 1u && b_record->service_pending == 1u &&
          c_record->service_pending == 1u);
    CHECK(mock_fatal_calls == 0u);
}

static void test_refusals_and_generation_reuse(void *page)
{
    struct direct_fixture fixture;
    struct p4_task_registry_record *a_record;
    struct p4_task_registry_record *b_record;
    struct Task *a = guarded_identity(page, 512u);
    struct Task *b = guarded_identity(page, 576u);
    struct Task *wrong_creator = guarded_identity(page, 640u);
    uint32_t generation = 0u;
    uint32_t fresh_generation = 0u;
    struct p4_task_registry_record *detached;

    fixture_init(&fixture, TEST_OP_CAPACITY, TEST_ENTRY_CAPACITY);
    a_record = fixture_add_task(&fixture, 0u, a, 0u);
    CHECK(p4_task_registry_register_constructing_locked(
        &fixture.registry, &fixture.records[1], b, a, 0u,
        &generation) == 1);
    b_record = &fixture.records[1];
    CHECK(p4_task_registry_register_locked(&fixture.registry,
        &fixture.records[2], wrong_creator) == 1);
    CHECK(p4_task_registry_enter_running_locked(
        &fixture.registry, b, 1u) == 1);

    expect_orphan_result(&fixture, b, a, 0u, 0);
    expect_orphan_result(&fixture, b, a, generation + 1u, 0);
    expect_orphan_result(&fixture, b, wrong_creator, generation, 0);
    expect_orphan_result(&fixture, b, a, generation, 0);
    expect_orphan_result(&fixture, a, a, generation, -1);

    /* Break only the global child-count invariant. The checked leaf must
     * reject before changing either association or any pin. */
    a_record->construction_children = 0u;
    expect_orphan_result(&fixture, b, a, generation, -1);
    a_record->construction_children = 1u;

    CHECK(p4_task_registry_retire_for_service_locked(
        &fixture.registry, a) == 1);
    CHECK(p4_task_registry_retire_for_service_locked(
        &fixture.registry, b) == 1);
    expect_orphan_result(&fixture, b, a, generation, 0); /* both RUN */
    CHECK(p4_task_registry_leave_running_locked(
        &fixture.registry, b, 1u) == 1);
    expect_orphan_result(&fixture, b, a, generation, 0); /* creator RUN */
    CHECK(p4_task_registry_leave_running_locked(
        &fixture.registry, a, 0u) == 1);
    CHECK(p4_task_registry_finish_orphan_construction_locked(
        &fixture.registry, b, a, generation) == 1);
    CHECK(a_record->construction_children == 0u);
    CHECK(b_record->construction_creator == NULL);

    /* Reclaim both service records, recycle the same storage and raw Task
     * identities, then prove the old nonwrapping edge token cannot release
     * the new association (an ABA-shaped identity reuse case). */
    CHECK(p4_task_registry_take_service_ready_locked(
        &fixture.registry, a) == a);
    CHECK(p4_task_registry_take_service_ready_locked(
        &fixture.registry, b) == b);
    detached = p4_task_registry_claim_queued_service_locked(
        &fixture.registry, b);
    CHECK(detached == b_record);
    memset(detached, 0, sizeof(*detached));
    detached = p4_task_registry_claim_queued_service_locked(
        &fixture.registry, a);
    CHECK(detached == a_record);
    memset(detached, 0, sizeof(*detached));
    CHECK(fixture.registry.count == 1u); /* wrong_creator remains registered */
    CHECK(p4_task_registry_register_locked(&fixture.registry,
        a_record, a) == 1);
    CHECK(p4_task_registry_enter_running_locked(
        &fixture.registry, a, 0u) == 1);
    CHECK(p4_task_registry_register_constructing_locked(
        &fixture.registry, b_record, b, a, 0u,
        &fresh_generation) == 1);
    CHECK(fresh_generation > generation);
    expect_orphan_result(&fixture, b, a, generation, 0);
    CHECK(b_record->construction_creator == a);
    CHECK(b_record->construction_generation == fresh_generation);
    CHECK(p4_task_registry_finish_construction_locked(
        &fixture.registry, b, a, fresh_generation, 0u) == 1);
    CHECK(a_record->construction_children == 0u);
    CHECK(b_record->construction_creator == NULL);
    CHECK(a_record->lifetime.notification_pins == 0u);
    CHECK(b_record->lifetime.notification_pins == 0u);
    CHECK(mock_fatal_calls == 0u);
}

int main(void)
{
    long page_size = sysconf(_SC_PAGESIZE);
    void *page;

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

    test_two_children_and_reserved_pins(page);
    test_nested_retired_chain(page);
    test_refusals_and_generation_reuse(page);

    if (munmap(page, (size_t)page_size) != 0)
        return 2;
    if (failures != 0u)
        return 1;
    printf("P4 construction orphan boundary: %u checks passed\n", checks);
    return 0;
}
