/* Actual-source tests for the private RUN-stack creation capability leaves. */
#define main p4_creation_channel_dependency_main
#include "p4-semaphore-operation-admission-test.c"
#undef main
#include "../../exec/p4_remove_bank.c"
#include "../../exec/p4_remove_completion.c"

static void expect_begin(struct direct_fixture *fixture, struct Task *owner,
    uint32_t hart, struct Task *target, struct p4_creation_channel *frame,
    int expected, int compare_frame)
{
    struct byte_snapshot before;
    struct p4_creation_channel frame_before;
    int result;

    snapshot_fixture(fixture, &before);
    if (compare_frame)
        memcpy(&frame_before, frame, sizeof(frame_before));
    result = p4_task_registry_creation_begin_locked(
        &fixture->registry, owner, hart, target, frame);
    CHECK(result == expected);
    CHECK(fixture_matches_snapshot(fixture, &before));
    if (compare_frame)
        CHECK(memcmp(&frame_before, frame, sizeof(frame_before)) == 0);
}

static void expect_end(struct direct_fixture *fixture, struct Task *owner,
    uint32_t hart, uint32_t generation, struct p4_creation_channel *frame,
    int expected)
{
    struct byte_snapshot before;
    struct p4_creation_channel frame_before;
    int result;

    snapshot_fixture(fixture, &before);
    if (frame != NULL)
        memcpy(&frame_before, frame, sizeof(frame_before));
    result = p4_task_registry_creation_end_locked(
        &fixture->registry, owner, hart, generation, frame);
    CHECK(result == expected);
    CHECK(fixture_matches_snapshot(fixture, &before));
    if (frame != NULL)
        CHECK(memcmp(&frame_before, frame, sizeof(frame_before)) == 0);
}

static void expect_resolve(struct direct_fixture *fixture,
    struct Task *owner, uint32_t hart, struct Task *target,
    uint32_t generation, uintptr_t tag_value, uintptr_t expected)
{
    struct byte_snapshot before;
    uintptr_t result;

    snapshot_fixture(fixture, &before);
    result = p4_task_registry_creation_resolve_locked(
        &fixture->registry, owner, hart, target, generation, tag_value);
    CHECK(result == expected);
    CHECK(fixture_matches_snapshot(fixture, &before) || expected != 0u);
}

static void expect_inaccessible_frame_end(struct direct_fixture *fixture,
    struct Task *owner, uint32_t hart, uint32_t generation,
    struct p4_creation_channel *frame, int expected)
{
    struct byte_snapshot before;
    snapshot_fixture(fixture, &before);
    CHECK(p4_task_registry_creation_end_locked(
        &fixture->registry, owner, hart, generation, frame) == expected);
    CHECK(fixture_matches_snapshot(fixture, &before));
}

static void test_nested_scopes_and_single_use(void *identity_page)
{
    struct direct_fixture fixture;
    struct p4_task_registry_record *owner_record;
    struct p4_creation_channel outer = { 0 };
    struct p4_creation_channel inner = { 0 };
    struct p4_creation_channel inner_after_claim = { 0 };
    struct p4_creation_channel migration = { 0 };
    struct Task *owner = guarded_identity(identity_page, 64u);
    struct Task *other = guarded_identity(identity_page, 128u);
    struct Task *target_a = guarded_identity(identity_page, 256u);
    struct Task *target_b = guarded_identity(identity_page, 320u);
    struct Task *target_c = guarded_identity(identity_page, 384u);
    struct Task *target_m = guarded_identity(identity_page, 448u);
    uint32_t outer_generation = 0u;
    uint32_t inner_generation = 0u;
    uint32_t claimed_inner_generation = 0u;
    uint32_t migration_generation = 0u;
    uintptr_t outer_tag, inner_tag, inner_after_claim_tag, migration_tag;

    fixture_init(&fixture, TEST_OP_CAPACITY, TEST_ENTRY_CAPACITY);
    owner_record = fixture_add_task(&fixture, 0u, owner, 0u);
    (void)fixture_add_task(&fixture, 1u, other, 1u);

    CHECK(p4_task_registry_creation_begin_locked(&fixture.registry, owner,
        0u, target_a, &outer) == 1);
    outer_generation = outer.generation;
    outer_tag = (uintptr_t)&outer.value;
    CHECK(outer_generation != 0u && outer.value == 1u);
    CHECK(owner_record->creation_frame == (uintptr_t)&outer);
    CHECK(owner_record->creation_target == (uintptr_t)target_a);
    CHECK(owner_record->creation_claimed == 0u);

    /* Wrong owner/hart/target, stale generation, and an arbitrary PROT_NONE
     * tag scalar are rejected without consuming the top frame's one-use claim. */
    expect_resolve(&fixture, other, 1u, target_a, outer_generation,
                   outer_tag, 0u);
    expect_resolve(&fixture, owner, 1u, target_a, outer_generation,
                   outer_tag, 0u);
    expect_resolve(&fixture, owner, 0u, target_b, outer_generation,
                   outer_tag, 0u);
    expect_resolve(&fixture, owner, 0u, target_a, outer_generation + 1u,
                   outer_tag, 0u);
    expect_resolve(&fixture, owner, 0u, target_a, outer_generation,
                   (uintptr_t)identity_page, 0u);
    CHECK(owner_record->creation_claimed == 0u);
    expect_end(&fixture, other, 1u, outer_generation, &outer, 0);
    expect_end(&fixture, owner, 1u, outer_generation, &outer, 0);
    expect_end(&fixture, owner, 0u, outer_generation + 1u, &outer, 0);

    /* An unconsumed outer claim is saved/restored across a nested scope.
     * Resolver authority is only for the top target/generation/address. */
    outer.value = 0x21u;
    CHECK(p4_task_registry_creation_begin_locked(&fixture.registry, owner,
        0u, target_b, &inner) == 1);
    inner_generation = inner.generation;
    inner_tag = (uintptr_t)&inner.value;
    CHECK(inner.previous_frame == (uintptr_t)&outer);
    CHECK(inner.previous_generation == outer_generation);
    CHECK(inner.previous_target == (uintptr_t)target_a);
    CHECK(inner.previous_claimed == 0u);
    CHECK(inner.value == 1u);
    expect_resolve(&fixture, owner, 0u, target_a, outer_generation,
                   outer_tag, 0u);
    expect_resolve(&fixture, owner, 0u, target_b, inner_generation,
                   outer_tag, 0u);
    CHECK(owner_record->creation_claimed == 0u);
    CHECK(p4_task_registry_creation_resolve_locked(&fixture.registry, owner,
        0u, target_b, inner_generation, inner_tag) == inner_tag);
    CHECK(owner_record->creation_claimed == 1u);
    expect_resolve(&fixture, owner, 0u, target_b, inner_generation,
                   inner_tag, 0u);
    expect_end(&fixture, owner, 0u, inner_generation + 1u, &inner, 0);
    inner.value = 0x32u;
    CHECK(p4_task_registry_creation_end_locked(&fixture.registry, owner,
        0u, inner_generation, &inner) == 1);
    CHECK(inner.value == 0x32u);
    CHECK(inner.generation == 0u && inner.previous_frame == 0u &&
          inner.previous_generation == 0u && inner.previous_target == 0u &&
          inner.previous_claimed == 0u);
    CHECK(owner_record->creation_frame == (uintptr_t)&outer);
    CHECK(owner_record->creation_generation == outer_generation);
    CHECK(owner_record->creation_target == (uintptr_t)target_a);
    CHECK(owner_record->creation_claimed == 0u);
    CHECK(outer.value == 0x21u);
    CHECK(p4_task_registry_creation_resolve_locked(&fixture.registry, owner,
        0u, target_a, outer_generation, outer_tag) == outer_tag);
    CHECK(owner_record->creation_claimed == 1u);
    expect_resolve(&fixture, owner, 0u, target_a, outer_generation,
                   outer_tag, 0u);

    /* A nested scope entered after outer resolution must restore the consumed
     * state as well; reentrant/vector replay cannot reacquire the old frame. */
    CHECK(p4_task_registry_creation_begin_locked(&fixture.registry, owner,
        0u, target_c, &inner_after_claim) == 1);
    claimed_inner_generation = inner_after_claim.generation;
    inner_after_claim_tag = (uintptr_t)&inner_after_claim.value;
    CHECK(inner_after_claim.previous_claimed == 1u);
    CHECK(p4_task_registry_creation_resolve_locked(&fixture.registry, owner,
        0u, target_c, claimed_inner_generation,
        inner_after_claim_tag) == inner_after_claim_tag);
    expect_resolve(&fixture, owner, 0u, target_c,
                   claimed_inner_generation, inner_after_claim_tag, 0u);
    CHECK(p4_task_registry_creation_end_locked(&fixture.registry, owner,
        0u, claimed_inner_generation, &inner_after_claim) == 1);
    CHECK(owner_record->creation_frame == (uintptr_t)&outer);
    CHECK(owner_record->creation_target == (uintptr_t)target_a);
    CHECK(owner_record->creation_claimed == 1u);
    CHECK(outer.value == 0x21u);
    expect_resolve(&fixture, owner, 0u, target_a, outer_generation,
                   outer_tag, 0u);
    CHECK(p4_task_registry_creation_end_locked(&fixture.registry, owner,
        0u, outer_generation, &outer) == 1);
    CHECK(outer.value == 0x21u);
    CHECK(owner_record->creation_frame == 0u &&
          owner_record->creation_generation == 0u &&
          owner_record->creation_target == 0u &&
          owner_record->creation_claimed == 0u);

    /* A live frame remains opaque while RUN is drained and becomes usable
     * again only after the same identity re-enters on its new hart. */
    CHECK(p4_task_registry_leave_running_locked(
        &fixture.registry, other, 1u) == 1);
    CHECK(p4_task_registry_creation_begin_locked(&fixture.registry, owner,
        0u, target_m, &migration) == 1);
    migration_generation = migration.generation;
    migration_tag = (uintptr_t)&migration.value;
    CHECK(p4_task_registry_leave_running_locked(
        &fixture.registry, owner, 0u) == 1);
    expect_resolve(&fixture, owner, 0u, target_m, migration_generation,
                   migration_tag, 0u);
    expect_begin(&fixture, owner, 0u, target_m,
        (struct p4_creation_channel *)identity_page, 0, 0);
    expect_inaccessible_frame_end(&fixture, owner, 0u,
        migration_generation, (struct p4_creation_channel *)identity_page, 0);
    CHECK(p4_task_registry_enter_running_locked(
        &fixture.registry, owner, 1u) == 1);
    expect_resolve(&fixture, owner, 0u, target_m, migration_generation,
                   migration_tag, 0u);
    CHECK(p4_task_registry_creation_resolve_locked(&fixture.registry, owner,
        1u, target_m, migration_generation, migration_tag) == migration_tag);
    CHECK(p4_task_registry_creation_end_locked(&fixture.registry, owner,
        1u, migration_generation, &migration) == 1);
    CHECK(migration.value == 1u);
}

static void test_alias_malformed_and_exhaustion(void *identity_page)
{
    struct direct_fixture fixture;
    struct p4_task_registry_record *owner_record;
    struct p4_creation_channel frame = { 0 };
    struct p4_creation_channel *record_alias;
    struct p4_creation_channel unaligned_storage[2] = { { 0 }, { 0 } };
    struct Task *owner = guarded_identity(identity_page, 512u);
    struct Task *target = guarded_identity(identity_page, 576u);
    struct byte_snapshot before;
    struct p4_creation_channel frame_before;
    uint32_t generation;

    fixture_init(&fixture, TEST_OP_CAPACITY, TEST_ENTRY_CAPACITY);
    owner_record = fixture_add_task(&fixture, 0u, owner, 0u);
    record_alias = (struct p4_creation_channel *)(void *)owner_record;
    CHECK((uintptr_t)record_alias % _Alignof(struct p4_creation_channel) == 0u);
    expect_begin(&fixture, owner, 0u, target, record_alias, -1, 1);

    frame.previous_target = (uintptr_t)target;
    expect_begin(&fixture, owner, 0u, target, &frame, -1, 1);
    memset(&frame, 0, sizeof(frame));
    expect_begin(&fixture, owner, 0u, owner, &frame, -1, 1);
    expect_begin(&fixture, owner, 0u, NULL, &frame, -1, 1);
    expect_begin(&fixture, owner, 0u, target,
        (struct p4_creation_channel *)(void *)
            ((unsigned char *)unaligned_storage + 1u), -1, 0);

    CHECK(p4_task_registry_creation_begin_locked(&fixture.registry, owner,
        0u, target, &frame) == 1);
    generation = frame.generation;
    snapshot_fixture(&fixture, &before);
    memcpy(&frame_before, &frame, sizeof(frame_before));
    CHECK(p4_task_registry_creation_begin_locked(&fixture.registry, owner,
        0u, target, &frame) == -1); /* active-frame overlap */
    CHECK(fixture_matches_snapshot(&fixture, &before));
    CHECK(memcmp(&frame, &frame_before, sizeof(frame)) == 0);

    frame.previous_target = (uintptr_t)target; /* outer frame has no parent */
    snapshot_fixture(&fixture, &before);
    memcpy(&frame_before, &frame, sizeof(frame_before));
    CHECK(p4_task_registry_creation_end_locked(&fixture.registry, owner,
        0u, generation, &frame) == -1);
    CHECK(fixture_matches_snapshot(&fixture, &before));
    CHECK(memcmp(&frame, &frame_before, sizeof(frame)) == 0);
    frame.previous_target = 0u;
    CHECK(p4_task_registry_creation_end_locked(&fixture.registry, owner,
        0u, generation, &frame) == 1);

    memset(&frame, 0, sizeof(frame));
    fixture.registry.creation_channel_serial = UINT32_MAX;
    snapshot_fixture(&fixture, &before);
    memcpy(&frame_before, &frame, sizeof(frame_before));
    CHECK(p4_task_registry_creation_begin_locked(&fixture.registry, owner,
        0u, target, &frame) == 0);
    CHECK(fixture_matches_snapshot(&fixture, &before));
    CHECK(memcmp(&frame, &frame_before, sizeof(frame)) == 0);
    CHECK(mock_fatal_calls == 0u);
}

static void test_retired_unmapped_frame_and_same_address_reuse(
    void *identity_page, size_t page_size)
{
    struct direct_fixture fixture;
    struct p4_task_registry_record *record;
    struct p4_task_registry_record *detached;
    struct p4_creation_channel *frame_page;
    struct p4_creation_channel *frame;
    struct Task *owner = guarded_identity(identity_page, 704u);
    struct Task *old_target = guarded_identity(identity_page, 768u);
    struct Task *new_target = guarded_identity(identity_page, 832u);
    uint32_t old_generation, new_generation;
    uintptr_t old_tag, new_tag;
    struct byte_snapshot before;

    frame_page = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANON, -1, 0);
    CHECK(frame_page != MAP_FAILED);
    if (frame_page == MAP_FAILED)
        return;
    frame = frame_page;
    memset(frame_page, 0, page_size);
    fixture_init(&fixture, TEST_OP_CAPACITY, TEST_ENTRY_CAPACITY);
    record = fixture_add_task(&fixture, 0u, owner, 0u);
    CHECK(p4_task_registry_creation_begin_locked(&fixture.registry, owner,
        0u, old_target, frame) == 1);
    old_generation = frame->generation;
    old_tag = (uintptr_t)&frame->value;
    CHECK(old_generation != 0u);
    CHECK(p4_task_registry_creation_resolve_locked(&fixture.registry, owner,
        0u, old_target, old_generation, old_tag) == old_tag);
    CHECK(record->creation_claimed == 1u);
    CHECK(p4_task_registry_retire_for_service_locked(
        &fixture.registry, owner) == 1);
    CHECK(p4_task_registry_leave_running_locked(
        &fixture.registry, owner, 0u) == 1);
    CHECK(mprotect(frame_page, page_size, PROT_NONE) == 0);

    /* No operation on the retired owner may inspect the now-inaccessible
     * saved frame, including End with its old expected generation. */
    expect_inaccessible_frame_end(&fixture, owner, 0u, old_generation,
        frame, 0);
    snapshot_fixture(&fixture, &before);
    CHECK(p4_task_registry_creation_begin_locked(&fixture.registry, owner,
        0u, new_target, frame) == 0);
    CHECK(fixture_matches_snapshot(&fixture, &before));
    snapshot_fixture(&fixture, &before);
    CHECK(p4_task_registry_creation_resolve_locked(&fixture.registry, owner,
        0u, old_target, old_generation, old_tag) == 0u);
    CHECK(fixture_matches_snapshot(&fixture, &before));

    CHECK(p4_task_registry_take_service_ready_locked(
        &fixture.registry, owner) == owner);
    detached = p4_task_registry_claim_queued_service_locked(
        &fixture.registry, owner);
    CHECK(detached == record);
    memset(detached, 0, sizeof(*detached));
    CHECK(fixture.registry.count == 0u);
    CHECK(mprotect(frame_page, page_size, PROT_READ | PROT_WRITE) == 0);

    /* Re-register the same record and raw Task identity, then reuse exactly
     * the same frame address. Both stale Resolve and stale End carry the old
     * generation and must refuse before changing the new one-use capability. */
    memset(frame_page, 0, page_size);
    CHECK(p4_task_registry_register_locked(&fixture.registry, record, owner) == 1);
    CHECK(p4_task_registry_enter_running_locked(
        &fixture.registry, owner, 0u) == 1);
    CHECK(p4_task_registry_creation_begin_locked(&fixture.registry, owner,
        0u, new_target, frame) == 1);
    new_generation = frame->generation;
    new_tag = (uintptr_t)&frame->value;
    CHECK(new_generation > old_generation);
    frame->value = 0x55u;

    CHECK(mprotect(frame_page, page_size, PROT_NONE) == 0);
    snapshot_fixture(&fixture, &before);
    CHECK(p4_task_registry_creation_end_locked(&fixture.registry, owner,
        0u, old_generation, frame) == 0);
    CHECK(fixture_matches_snapshot(&fixture, &before));
    snapshot_fixture(&fixture, &before);
    CHECK(p4_task_registry_creation_resolve_locked(&fixture.registry, owner,
        0u, old_target, old_generation, old_tag) == 0u);
    CHECK(fixture_matches_snapshot(&fixture, &before));
    CHECK(mprotect(frame_page, page_size, PROT_READ | PROT_WRITE) == 0);
    CHECK(frame->generation == new_generation && frame->value == 0x55u);
    CHECK(record->creation_generation == new_generation);
    CHECK(record->creation_target == (uintptr_t)new_target);
    CHECK(record->creation_claimed == 0u);

    CHECK(p4_task_registry_creation_resolve_locked(&fixture.registry, owner,
        0u, old_target, old_generation, new_tag) == 0u);
    CHECK(record->creation_claimed == 0u);
    CHECK(p4_task_registry_creation_resolve_locked(&fixture.registry, owner,
        0u, new_target, new_generation, new_tag) == new_tag);
    CHECK(record->creation_claimed == 1u);
    CHECK(p4_task_registry_creation_resolve_locked(&fixture.registry, owner,
        0u, new_target, new_generation, new_tag) == 0u);
    CHECK(p4_task_registry_creation_end_locked(&fixture.registry, owner,
        0u, new_generation, frame) == 1);
    CHECK(frame->value == 0x55u && frame->generation == 0u);
    CHECK(record->creation_frame == 0u && record->creation_target == 0u &&
          record->creation_generation == 0u &&
          record->creation_claimed == 0u);
    CHECK(munmap(frame_page, page_size) == 0);
    CHECK(mock_fatal_calls == 0u);
}

int main(void)
{
    long page_size = sysconf(_SC_PAGESIZE);
    void *identity_page;

    if (page_size <= 0)
        return 2;
#if defined(MAP_ANONYMOUS)
    identity_page = mmap(NULL, (size_t)page_size, PROT_NONE,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
#else
    identity_page = mmap(NULL, (size_t)page_size, PROT_NONE,
        MAP_PRIVATE | MAP_ANON, -1, 0);
#endif
    if (identity_page == MAP_FAILED)
        return 2;

    test_nested_scopes_and_single_use(identity_page);
    test_alias_malformed_and_exhaustion(identity_page);
    test_retired_unmapped_frame_and_same_address_reuse(
        identity_page, (size_t)page_size);

    if (munmap(identity_page, (size_t)page_size) != 0)
        return 2;
    if (failures != 0u)
        return 1;
    printf("P4 creation channel: %u checks passed\n", checks);
    return 0;
}
