/* Actual registry selection only: no worker, scheduler or hardware claim. */
#define main p4_wait_registry_baseline_main
#include "p4-semaphore-wait-registry-test.c"
#undef main

int main(void)
{
    struct p4_task_registry registry;
    struct p4_task_registry_record records[3] = {0};
    struct p4_task_wait_cancel_snapshot snapshot, before;
    uint32_t generations[3];
    const unsigned insertion[3] = {1, 0, 2};
    uintptr_t cursor = 0;
    struct p4_task_registry_record records_before[3];
    unsigned i;

    CHECK(p4_wait_registry_baseline_main() == EXIT_SUCCESS);
    checks = 0;
    unmapped_identities = mmap(0, mapped_size, PROT_NONE,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(unmapped_identities != MAP_FAILED);
    p4_task_registry_init(&registry);
    gate_lock();
    for (i = 0; i < 3; ++i)
    {
        unsigned n = insertion[i];
        struct Task *identity = opaque_task(n * 64u);
        CHECK(p4_task_registry_register_locked(&registry, &records[n],
            identity) == 1);
        CHECK(p4_task_registry_enter_running_locked(&registry, identity, 0)
            == 1);
        CHECK(p4_task_registry_wait_begin_locked(&registry, identity,
            P4_TASK_WAIT_SINGLE, opaque_index(256), opaque_index(512), 0,
            &generations[n]) == 1);
        CHECK(p4_task_registry_retire_locked(&registry, identity) == 1);
        CHECK(p4_task_registry_leave_running_locked(&registry, identity, 0)
            == 1);
    }
    /* The operation consumer must never strand legacy scope claims. */
    memcpy(records_before, records, sizeof(records));
    CHECK(p4_task_registry_claim_wait_operation_cancel_after_locked(&registry,
        0, &snapshot) == 0);
    CHECK(snapshot.identity == 0);
    CHECK(memcmp(records_before, records, sizeof(records)) == 0);
    /* Head is highest address, not the fair minimum. Preserve legacy order. */
    CHECK(p4_task_registry_claim_next_wait_cancel_locked(&registry,
        &snapshot) == 1);
    CHECK(snapshot.identity == opaque_task(128));
    CHECK(p4_task_registry_wait_release_cancel_locked(&registry,
        snapshot.identity, snapshot.generation) == 1);
    for (i = 0; i < 3; ++i)
    {
        CHECK(p4_task_registry_claim_wait_cancel_after_locked(&registry,
            cursor, &snapshot) == 1);
        CHECK(snapshot.identity == opaque_task(i * 64u));
        CHECK((uintptr_t)snapshot.identity > cursor);
        cursor = (uintptr_t)snapshot.identity;
        /* A blocked scope stays eligible; cursor prevents immediate reclaim. */
        CHECK(p4_task_registry_wait_release_cancel_locked(&registry,
            snapshot.identity, snapshot.generation) == 1);
    }
    memset(&snapshot, 0xa5, sizeof(snapshot));
    CHECK(p4_task_registry_claim_wait_cancel_after_locked(&registry, cursor,
        &snapshot) == 0);
    CHECK(snapshot.identity == 0 && snapshot.generation == 0);
    CHECK(p4_task_registry_claim_wait_cancel_after_locked(&registry,
        UINTPTR_MAX, &snapshot) == 0);
    CHECK(p4_task_registry_claim_wait_cancel_after_locked(&registry, 0,
        &snapshot) == 1);
    CHECK(snapshot.identity == opaque_task(0));
    CHECK(p4_task_registry_wait_release_cancel_locked(&registry,
        snapshot.identity, snapshot.generation) == 1);
    before = snapshot;
    CHECK(p4_task_registry_claim_wait_cancel_after_locked(&registry, 0,
        (struct p4_task_wait_cancel_snapshot *)&records[1]) == -1);
    CHECK(records[0].wait_scope.cancel_claimed == 0 &&
        records[1].wait_scope.cancel_claimed == 0 &&
        records[2].wait_scope.cancel_claimed == 0);
    registry.count = 2;
    CHECK(p4_task_registry_claim_wait_cancel_after_locked(&registry, 0,
        &snapshot) == -1);
    CHECK(wait_snapshots_equal(&snapshot, &before));
    registry.count = 3;
    CHECK(p4_task_registry_claim_wait_cancel_after_locked(&registry, 0, 0)
        == -1);
    gate_unlock();
    CHECK(munmap(unmapped_identities, mapped_size) == 0);
    printf("P4 fair cancellation selection: %u checks passed\n", checks);
    return EXIT_SUCCESS;
}
