/* Focused actual-source tests for the P4 SMP construction-pin registry leaf.
 * The included admission fixture supplies the host ABI shims and actual
 * lifetime/ledger/registry/node sources; its test main is renamed, not run. */
#include <pthread.h>

#define main p4_operation_admission_fixture_main
#include "p4-semaphore-operation-admission-test.c"
#undef main

#include "../../exec/p4_remove_bank.c"
#include "../../exec/p4_remove_completion.c"

static pthread_mutex_t construction_gate = PTHREAD_MUTEX_INITIALIZER;
static unsigned int construction_gate_depth;

static void test_gate_lock(void)
{
    if (pthread_mutex_lock(&construction_gate) != 0)
    {
        fputs("construction test gate lock failed\n", stderr);
        exit(EXIT_FAILURE);
    }
    ++construction_gate_depth;
    CHECK(construction_gate_depth == 1u);
}

static void test_gate_unlock(void)
{
    CHECK(construction_gate_depth == 1u);
    --construction_gate_depth;
    if (pthread_mutex_unlock(&construction_gate) != 0)
    {
        fputs("construction test gate unlock failed\n", stderr);
        exit(EXIT_FAILURE);
    }
}

static int register_live_identity(struct direct_fixture *fixture,
                                  unsigned int slot, struct Task *identity,
                                  uint32_t hart)
{
    int registered;
    int running = 0;

    test_gate_lock();
    registered = p4_task_registry_register_locked(&fixture->registry,
        &fixture->records[slot], identity);
    if (registered == 1)
        running = p4_task_registry_enter_running_locked(&fixture->registry,
            identity, hart);
    test_gate_unlock();
    return registered == 1 && running == 1 ? 1 : 0;
}

static int register_identity(struct direct_fixture *fixture,
                             unsigned int slot, struct Task *identity)
{
    int result;

    test_gate_lock();
    result = p4_task_registry_register_locked(&fixture->registry,
        &fixture->records[slot], identity);
    test_gate_unlock();
    return result;
}

static int register_constructing(struct direct_fixture *fixture,
                                 struct p4_task_registry_record *record,
                                 struct Task *identity,
                                 struct Task *creator, uint32_t hart,
                                 uint32_t *out_generation)
{
    int result;

    test_gate_lock();
    CHECK(construction_gate_depth == 1u);
    result = p4_task_registry_register_constructing_locked(
        &fixture->registry, record, identity, creator, hart, out_generation);
    test_gate_unlock();
    return result;
}

static int finish_construction(struct direct_fixture *fixture,
                               struct Task *identity, struct Task *creator,
                               uint32_t generation, uint32_t hart)
{
    int result;

    test_gate_lock();
    CHECK(construction_gate_depth == 1u);
    result = p4_task_registry_finish_construction_locked(
        &fixture->registry, identity, creator, generation, hart);
    test_gate_unlock();
    return result;
}

static int pin_identity(struct direct_fixture *fixture,
                        struct Task *identity)
{
    int result;

    test_gate_lock();
    result = p4_task_registry_pin_locked(&fixture->registry, identity);
    test_gate_unlock();
    return result;
}

static int unpin_identity(struct direct_fixture *fixture,
                          struct Task *identity)
{
    int result;

    test_gate_lock();
    result = p4_task_registry_unpin_locked(&fixture->registry, identity);
    test_gate_unlock();
    return result;
}

static int retire_service(struct direct_fixture *fixture,
                          struct Task *identity)
{
    int result;

    test_gate_lock();
    result = p4_task_registry_retire_for_service_locked(
        &fixture->registry, identity);
    test_gate_unlock();
    return result;
}

static int leave_running(struct direct_fixture *fixture,
                         struct Task *identity, uint32_t hart)
{
    int result;

    test_gate_lock();
    result = p4_task_registry_leave_running_locked(
        &fixture->registry, identity, hart);
    test_gate_unlock();
    return result;
}

static struct Task *take_service_ready(struct direct_fixture *fixture,
                                       struct Task *identity)
{
    struct Task *result;

    test_gate_lock();
    result = p4_task_registry_take_service_ready_locked(
        &fixture->registry, identity);
    test_gate_unlock();
    return result;
}

static struct p4_task_registry_record *claim_service(
    struct direct_fixture *fixture, struct Task *identity)
{
    struct p4_task_registry_record *result;

    test_gate_lock();
    result = p4_task_registry_claim_service_locked(
        &fixture->registry, identity);
    test_gate_unlock();
    return result;
}

static struct p4_task_registry_record *claim_queued_service(
    struct direct_fixture *fixture, struct Task *identity)
{
    struct p4_task_registry_record *result;

    test_gate_lock();
    result = p4_task_registry_claim_queued_service_locked(
        &fixture->registry, identity);
    test_gate_unlock();
    return result;
}

static void assert_refused_create_unchanged(
    struct direct_fixture *fixture,
    struct p4_task_registry_record *candidate,
    struct Task *identity, struct Task *creator, uint32_t hart,
    uint32_t *out_generation, int expected_result)
{
    struct byte_snapshot before;
    struct p4_task_registry_record candidate_before;
    uint32_t generation_before;
    int result;

    snapshot_fixture(fixture, &before);
    candidate_before = *candidate;
    generation_before = *out_generation;
    result = register_constructing(fixture, candidate, identity, creator,
                                   hart, out_generation);
    CHECK(result == expected_result);
    CHECK(fixture_matches_snapshot(fixture, &before));
    CHECK(memcmp(candidate, &candidate_before, sizeof(*candidate)) == 0);
    CHECK(*out_generation == generation_before);
}

static void assert_refused_finish_unchanged(
    struct direct_fixture *fixture, struct Task *identity,
    struct Task *creator, uint32_t generation, uint32_t hart)
{
    struct byte_snapshot before;

    snapshot_fixture(fixture, &before);
    CHECK(finish_construction(fixture, identity, creator, generation,
                              hart) == 0);
    CHECK(fixture_matches_snapshot(fixture, &before));
}

static void test_nested_and_multiple_child_construction(void *page)
{
    struct direct_fixture fixture;
    struct Task *creator = guarded_identity(page, 64u);
    struct Task *child_a = guarded_identity(page, 128u);
    struct Task *child_b = guarded_identity(page, 192u);
    struct Task *grandchild = guarded_identity(page, 256u);
    struct Task *wrong_creator = guarded_identity(page, 320u);
    uint32_t gen_a = UINT32_C(0xaaaaaaaa);
    uint32_t gen_b = UINT32_C(0xbbbbbbbb);
    uint32_t gen_grandchild = UINT32_C(0xcccccccc);
    struct p4_task_registry_record *creator_record;
    struct p4_task_registry_record *child_a_record;

    fixture_init(&fixture, TEST_OP_CAPACITY, TEST_ENTRY_CAPACITY);
    CHECK(register_live_identity(&fixture, 0u, creator, 0u) == 1);
    creator_record = &fixture.records[0];
    child_a_record = &fixture.records[1];

    /* An ordinary pin may coexist, but unpin cannot consume the construction
     * reference or a child's designated construction reference. */
    CHECK(pin_identity(&fixture, creator) == 1);
    CHECK(register_constructing(&fixture, &fixture.records[1], child_a,
                                creator, 0u, &gen_a) == 1);
    CHECK(gen_a == 1u && fixture.registry.construction_serial == 1u);
    CHECK(creator_record->construction_children == 1u);
    CHECK(creator_record->lifetime.notification_pins == 2u);
    CHECK(child_a_record->construction_creator == creator);
    CHECK(child_a_record->construction_generation == gen_a);
    CHECK(child_a_record->lifetime.notification_pins == 1u);
    CHECK(unpin_identity(&fixture, creator) == 1);
    CHECK(creator_record->lifetime.notification_pins == 1u);
    CHECK(unpin_identity(&fixture, creator) == 0);
    CHECK(unpin_identity(&fixture, child_a) == 0);
    CHECK(creator_record->construction_children == 1u);
    CHECK(child_a_record->lifetime.notification_pins == 1u);

    /* A creator can have more than one live child construction at once. */
    CHECK(register_constructing(&fixture, &fixture.records[2], child_b,
                                creator, 0u, &gen_b) == 1);
    CHECK(gen_b == 2u && fixture.registry.construction_serial == 2u);
    CHECK(creator_record->construction_children == 2u);
    CHECK(creator_record->lifetime.notification_pins == 2u);
    CHECK(fixture.records[2].construction_creator == creator);
    CHECK(fixture.records[2].lifetime.notification_pins == 1u);

    /* The first child may become a running creator before its parent returns;
     * the leaf keeps nested construction references independently. */
    test_gate_lock();
    CHECK(p4_task_registry_enter_running_locked(&fixture.registry, child_a,
                                                 1u) == 1);
    test_gate_unlock();
    CHECK(register_constructing(&fixture, &fixture.records[3], grandchild,
                                child_a, 1u, &gen_grandchild) == 1);
    CHECK(gen_grandchild == 3u && fixture.registry.construction_serial == 3u);
    CHECK(child_a_record->construction_children == 1u);
    CHECK(child_a_record->lifetime.notification_pins == 2u);
    CHECK(fixture.records[3].construction_creator == child_a);
    CHECK(fixture.records[3].lifetime.notification_pins == 1u);

    /* Refused finish variants are byte-identical, including both designated
     * references and the nonwrapping generation counter. */
    assert_refused_finish_unchanged(&fixture, child_a, wrong_creator,
                                    gen_a, 1u);
    assert_refused_finish_unchanged(&fixture, child_a, creator, 0u, 0u);
    assert_refused_finish_unchanged(&fixture, child_a, creator, gen_a + 1u,
                                    0u);
    assert_refused_finish_unchanged(&fixture, child_a, creator, gen_a, 1u);

    /* Finish the nested child, then the parent's first child and second child.
     * Generations remain monotonic after a completed construction. */
    CHECK(finish_construction(&fixture, grandchild, child_a,
                              gen_grandchild, 1u) == 1);
    CHECK(child_a_record->construction_children == 0u);
    CHECK(child_a_record->lifetime.notification_pins == 1u);
    CHECK(finish_construction(&fixture, child_a, creator, gen_a, 0u) == 1);
    CHECK(finish_construction(&fixture, child_b, creator, gen_b, 0u) == 1);
    CHECK(creator_record->construction_children == 0u);
    CHECK(creator_record->lifetime.notification_pins == 0u);
    CHECK(child_a_record->construction_creator == NULL);
    CHECK(child_a_record->lifetime.notification_pins == 0u);
    assert_refused_finish_unchanged(&fixture, child_a, creator, gen_a, 0u);

    /* Same generation cannot be reused after a successful finish. */
    assert_refused_create_unchanged(&fixture, &fixture.records[1], child_a,
                                    creator, 0u, &gen_a, -1);
    CHECK(fixture.registry.construction_serial == 3u);
    CHECK(leave_running(&fixture, child_a, 1u) == 1);
    CHECK(construction_gate_depth == 0u);
}

static void test_retired_target_waits_for_creator_finish(void *page)
{
    struct direct_fixture fixture;
    struct Task *creator = guarded_identity(page, 384u);
    struct Task *target = guarded_identity(page, 448u);
    struct p4_task_registry_record *detached;
    uint32_t generation = 0u;

    fixture_init(&fixture, TEST_OP_CAPACITY, TEST_ENTRY_CAPACITY);
    CHECK(register_live_identity(&fixture, 0u, creator, 0u) == 1);
    CHECK(register_constructing(&fixture, &fixture.records[1], target,
                                creator, 0u, &generation) == 1);
    CHECK(generation == 1u);
    CHECK(retire_service(&fixture, target) == 1);
    CHECK(fixture.records[1].lifetime.flags == P4_TASK_LIFETIME_RETIRED);
    CHECK(fixture.records[1].lifetime.notification_pins == 2u);
    CHECK(fixture.records[1].service_pending == 1u);

    /* The service pin cannot be claimed while the designated construction pin
     * is still present. Every failed readiness/claim path leaves the fixture
     * unchanged and the target record discoverable. */
    {
        struct byte_snapshot before;
        snapshot_fixture(&fixture, &before);
        CHECK(take_service_ready(&fixture, target) == NULL);
        CHECK(fixture_matches_snapshot(&fixture, &before));
    }
    {
        struct byte_snapshot before;
        snapshot_fixture(&fixture, &before);
        CHECK(claim_service(&fixture, target) == NULL);
        CHECK(fixture_matches_snapshot(&fixture, &before));
    }

    CHECK(finish_construction(&fixture, target, creator, generation, 0u) == 1);
    CHECK(fixture.records[1].construction_creator == NULL);
    CHECK(fixture.records[1].lifetime.notification_pins == 1u);
    CHECK(fixture.records[0].construction_children == 0u);
    CHECK(fixture.records[0].lifetime.notification_pins == 0u);
    CHECK(take_service_ready(&fixture, target) == target);
    detached = claim_queued_service(&fixture, target);
    CHECK(detached == &fixture.records[1]);
    CHECK(fixture.registry.count == 1u);
    CHECK(fixture.registry.head == &fixture.records[0]);
    CHECK(fixture.records[1].next == NULL);
    CHECK(fixture.records[1].identity == target);
    CHECK(fixture.records[1].service_pending == 0u);
}

static void test_retired_creator_retains_construction_references(void *page)
{
    struct direct_fixture fixture;
    struct Task *creator = guarded_identity(page, 512u);
    struct Task *target = guarded_identity(page, 576u);
    uint32_t generation = 0u;
    struct byte_snapshot before;

    fixture_init(&fixture, TEST_OP_CAPACITY, TEST_ENTRY_CAPACITY);
    CHECK(register_live_identity(&fixture, 0u, creator, 0u) == 1);
    CHECK(register_constructing(&fixture, &fixture.records[1], target,
                                creator, 0u, &generation) == 1);
    CHECK(retire_service(&fixture, creator) == 1);
    CHECK(leave_running(&fixture, creator, 0u) == 1);

    CHECK(fixture.records[0].service_pending == 1u);
    CHECK(fixture.records[0].construction_children == 1u);
    CHECK(fixture.records[0].lifetime.notification_pins == 2u);
    CHECK(fixture.records[1].construction_creator == creator);
    CHECK(fixture.records[1].lifetime.notification_pins == 1u);

    snapshot_fixture(&fixture, &before);
    CHECK(take_service_ready(&fixture, creator) == NULL);
    CHECK(fixture_matches_snapshot(&fixture, &before));
    CHECK(claim_service(&fixture, creator) == NULL);
    CHECK(fixture_matches_snapshot(&fixture, &before));
    CHECK(unpin_identity(&fixture, creator) == 0);
    CHECK(fixture_matches_snapshot(&fixture, &before));
    CHECK(finish_construction(&fixture, target, creator, generation, 0u) == 0);
    CHECK(fixture_matches_snapshot(&fixture, &before));
}

static void test_refusal_and_alias_preflight(void *page)
{
    struct direct_fixture fixture;
    struct Task *creator = guarded_identity(page, 640u);
    struct Task *target = guarded_identity(page, 704u);
    struct Task *other = guarded_identity(page, 768u);
    uint32_t output = UINT32_C(0x12345678);
    uint32_t saved_output;
    struct byte_snapshot before;
    struct p4_task_registry_record candidate_before;
    unsigned char raw_output[sizeof(uint32_t) + 8u];
    unsigned char raw_before[sizeof(raw_output)];
    uint32_t *misaligned_output;
    int result;

    fixture_init(&fixture, TEST_OP_CAPACITY, TEST_ENTRY_CAPACITY);
    CHECK(register_live_identity(&fixture, 0u, creator, 0u) == 1);
    CHECK(register_identity(&fixture, 3u, other) == 1); /* No RUN lease. */

    assert_refused_create_unchanged(&fixture, &fixture.records[1], target,
                                    other, 0u, &output, 0);
    assert_refused_create_unchanged(&fixture, &fixture.records[1], target,
                                    creator, 1u, &output, 0);
    assert_refused_create_unchanged(&fixture, &fixture.records[1], creator,
                                    creator, 0u, &output, -1);
    assert_refused_create_unchanged(&fixture, &fixture.records[1], target,
                                    creator, P4_TASK_LIFETIME_HARTS,
                                    &output, -1);

    /* The target identity is already indexed, so a duplicate target is a
     * refusal without consuming a construction serial or changing either
     * registry record. */
    CHECK(register_identity(&fixture, 2u, target) == 1);
    assert_refused_create_unchanged(&fixture, &fixture.records[1], target,
                                    creator, 0u, &output, 0);

    /* Output aliases are rejected before writing the generation. */
    saved_output = output;
    snapshot_fixture(&fixture, &before);
    CHECK(register_constructing(&fixture, &fixture.records[1], target,
              creator, 0u, &fixture.registry.construction_serial) == -1);
    CHECK(fixture_matches_snapshot(&fixture, &before));
    CHECK(output == saved_output);

    snapshot_fixture(&fixture, &before);
    CHECK(register_constructing(&fixture, &fixture.records[1], target,
              creator, 0u,
              &fixture.records[0].lifetime.notification_pins) == -1);
    CHECK(fixture_matches_snapshot(&fixture, &before));

    /* Output cannot overlap a fresh candidate record, even though that record
     * is not linked into the registry yet. */
    snapshot_fixture(&fixture, &before);
    candidate_before = fixture.records[1];
    CHECK(register_constructing(&fixture, &fixture.records[1], other,
              creator, 0u,
              &fixture.records[1].lifetime.notification_pins) == -1);
    CHECK(fixture_matches_snapshot(&fixture, &before));
    CHECK(memcmp(&fixture.records[1], &candidate_before,
                 sizeof(candidate_before)) == 0);

    /* A candidate that is itself an already-linked record is rejected. */
    snapshot_fixture(&fixture, &before);
    candidate_before = fixture.records[0];
    CHECK(register_constructing(&fixture, &fixture.records[0], other,
              creator, 0u, &output) == -1);
    CHECK(fixture_matches_snapshot(&fixture, &before));
    CHECK(memcmp(&fixture.records[0], &candidate_before,
                 sizeof(candidate_before)) == 0);
    CHECK(output == saved_output);

    /* A misaligned output pointer is checked before dereference or mutation. */
    memset(raw_output, 0xa5, sizeof(raw_output));
    memcpy(raw_before, raw_output, sizeof(raw_output));
    misaligned_output = (uint32_t *)(void *)(raw_output + 1u);
    snapshot_fixture(&fixture, &before);
    result = register_constructing(&fixture, &fixture.records[1], other,
                                   creator, 0u, misaligned_output);
    CHECK(result == -1);
    CHECK(fixture_matches_snapshot(&fixture, &before));
    CHECK(memcmp(raw_output, raw_before, sizeof(raw_output)) == 0);

    /* Counter/pin exhaustion is a valid-state refusal. Neither output nor
     * fresh target record changes on these last-fallible checks. */
    fixture.registry.construction_serial = UINT32_MAX;
    assert_refused_create_unchanged(&fixture, &fixture.records[1], other,
                                    creator, 0u, &output, 0);

    fixture.registry.construction_serial = 0u;
    fixture.records[0].lifetime.notification_pins = UINT32_MAX;
    assert_refused_create_unchanged(&fixture, &fixture.records[1], other,
                                    creator, 0u, &output, 0);
}

static void test_corrupt_registry_refuses_unchanged(void *page)
{
    struct direct_fixture fixture;
    struct Task *creator = guarded_identity(page, 832u);
    struct Task *target = guarded_identity(page, 896u);
    uint32_t output = UINT32_C(0xdeadbeef);
    struct byte_snapshot before;

    fixture_init(&fixture, TEST_OP_CAPACITY, TEST_ENTRY_CAPACITY);
    CHECK(register_live_identity(&fixture, 0u, creator, 0u) == 1);
    fixture.records[0].next = &fixture.records[0];
    snapshot_fixture(&fixture, &before);
    CHECK(register_constructing(&fixture, &fixture.records[1], target,
                                creator, 0u, &output) == -1);
    CHECK(fixture_matches_snapshot(&fixture, &before));
    CHECK(output == UINT32_C(0xdeadbeef));
    CHECK(fixture.records[1].identity == NULL);
    CHECK(fixture.records[1].lifetime.notification_pins == 0u);
}

int main(void)
{
    long page_size = sysconf(_SC_PAGESIZE);
    void *page;

    if (page_size <= 0)
    {
        fprintf(stderr, "sysconf(_SC_PAGESIZE) failed: %d\n", errno);
        return 2;
    }
#if defined(MAP_ANONYMOUS)
    page = mmap(NULL, (size_t)page_size, PROT_NONE,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
#else
    page = mmap(NULL, (size_t)page_size, PROT_NONE,
                MAP_PRIVATE | MAP_ANON, -1, 0);
#endif
    if (page == MAP_FAILED)
    {
        fprintf(stderr, "mmap(PROT_NONE) failed: %d\n", errno);
        return 2;
    }

    test_nested_and_multiple_child_construction(page);
    test_retired_target_waits_for_creator_finish(page);
    test_retired_creator_retains_construction_references(page);
    test_refusal_and_alias_preflight(page);
    test_corrupt_registry_refuses_unchanged(page);

    if (construction_gate_depth != 0u || munmap(page, (size_t)page_size) != 0)
    {
        fprintf(stderr, "construction test cleanup failed: %d\n", errno);
        return 2;
    }
    if (failures != 0u)
    {
        fprintf(stderr, "P4 Task construction registry: %u/%u checks failed\n",
                failures, checks);
        return 1;
    }
    printf("P4 Task construction registry: %u checks passed\n", checks);
    return 0;
}
