/* Actual orphan final-release leaf; single-threaded caller serialization.
 * No constructor/worker/topology cancellation is modeled or qualified. */
#define main p4_orphan_dependency_main
#include "p4-semaphore-operation-admission-test.c"
#undef main
#include "../../exec/p4_remove_bank.c"
#include "../../exec/p4_remove_completion.c"

static void refused_orphan(struct direct_fixture *fixture, struct Task *target,
    struct Task *creator, uint32_t generation, int expected)
{
    struct byte_snapshot before;
    snapshot_fixture(fixture, &before);
    CHECK(p4_task_registry_finish_orphan_construction_locked(
        &fixture->registry, target, creator, generation) == expected);
    CHECK(fixture_matches_snapshot(fixture, &before));
}

int main(void)
{
    struct direct_fixture fixture;
    long page_size = sysconf(_SC_PAGESIZE);
    void *page;
    struct Task *creator, *target, *wrong;
    uint32_t generation = 0;
    if (page_size <= 0)
        return 2;
    page = mmap(NULL, (size_t)page_size, PROT_NONE,
        MAP_PRIVATE | MAP_ANON, -1, 0);
    if (page == MAP_FAILED)
        return 2;
    creator = guarded_identity(page, 64u);
    target = guarded_identity(page, 128u);
    wrong = guarded_identity(page, 192u);
    fixture_init(&fixture, TEST_OP_CAPACITY, TEST_ENTRY_CAPACITY);
    CHECK(p4_task_registry_register_locked(&fixture.registry,
        &fixture.records[0], creator) == 1);
    CHECK(p4_task_registry_enter_running_locked(&fixture.registry,
        creator, 0u) == 1);
    CHECK(p4_task_registry_register_constructing_locked(&fixture.registry,
        &fixture.records[1], target, creator, 0u, &generation) == 1);
    refused_orphan(&fixture, target, creator, generation, 0);
    CHECK(p4_task_registry_retire_for_service_locked(&fixture.registry,
        creator) == 1);
    refused_orphan(&fixture, target, creator, generation, 0);
    CHECK(p4_task_registry_leave_running_locked(&fixture.registry,
        creator, 0u) == 1);
    refused_orphan(&fixture, target, creator, generation, 0);
    CHECK(p4_task_registry_enter_running_locked(&fixture.registry,
        target, 1u) == 1);
    CHECK(p4_task_registry_retire_for_service_locked(&fixture.registry,
        target) == 1);
    refused_orphan(&fixture, target, creator, generation, 0);
    CHECK(p4_task_registry_leave_running_locked(&fixture.registry,
        target, 1u) == 1);
    refused_orphan(&fixture, target, creator, 0u, 0);
    refused_orphan(&fixture, target, creator, generation + 1u, 0);
    refused_orphan(&fixture, target, wrong, generation, 0);
    refused_orphan(&fixture, target, target, generation, -1);
    CHECK(p4_task_registry_unpin_locked(&fixture.registry, target) == 0);
    CHECK(p4_task_registry_unpin_locked(&fixture.registry, creator) == 0);
    CHECK(p4_task_registry_take_service_ready_locked(&fixture.registry,
        target) == NULL);
    CHECK(p4_task_registry_take_service_ready_locked(&fixture.registry,
        creator) == NULL);
    CHECK(p4_task_registry_finish_orphan_construction_locked(
        &fixture.registry, target, creator, generation) == 1);
    CHECK(fixture.records[1].construction_creator == NULL);
    CHECK(fixture.records[0].construction_children == 0u);
    CHECK(fixture.records[0].lifetime.notification_pins == 1u);
    CHECK(fixture.records[1].lifetime.notification_pins == 1u);
    CHECK(fixture.records[0].service_pending == 1u);
    CHECK(fixture.records[1].service_pending == 1u);
    refused_orphan(&fixture, target, creator, generation, 0);
    CHECK(p4_task_registry_take_service_ready_locked(&fixture.registry,
        target) == target);
    CHECK(p4_task_registry_take_service_ready_locked(&fixture.registry,
        creator) == creator);
    CHECK(p4_task_registry_claim_queued_service_locked(&fixture.registry,
        target) == &fixture.records[1]);
    CHECK(p4_task_registry_claim_queued_service_locked(&fixture.registry,
        creator) == &fixture.records[0]);
    CHECK(fixture.registry.count == 0u);
    CHECK(fixture.registry.head == NULL);
    if (munmap(page, (size_t)page_size) != 0)
        return 2;
    if (failures != 0u)
        return 1;
    printf("P4 construction orphan leaf: %u checks passed\n", checks);
    return 0;
}
