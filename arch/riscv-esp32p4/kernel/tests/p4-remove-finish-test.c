/* Actual registry/bank transaction for final removal ACK and detach. */
#define _DEFAULT_SOURCE 1
#define __AROSEXEC_SMP__ 1
#define __AROSPLATFORM_SMP__ 1

#define main p4_remove_finish_admission_baseline_main
#include "p4-semaphore-operation-admission-test.c"
#undef main

#include "../../exec/p4_remove_bank.c"
#include "../../exec/p4_remove_completion.c"

#define TEST_REGISTRY_CAPACITY 16u
#define TEST_BANK_CAPACITY 24u

static struct p4_task_registry_record test_records[TEST_REGISTRY_CAPACITY];
static struct p4_remove_bank test_bank;
static struct p4_remove_bank_slot test_slots[TEST_BANK_CAPACITY];
static struct ExecBase test_sysbase;

struct state_snapshot
{
    struct p4_task_registry registry;
    struct p4_task_registry_record records[TEST_REGISTRY_CAPACITY];
    struct p4_remove_bank bank;
    struct p4_remove_bank_slot slots[TEST_BANK_CAPACITY];
};

static void gate_enter(void)
{
    p4_task_pins_lock(&test_sysbase);
    CHECK(mock_gate_held && mock_irq_depth != 0u);
}

static void gate_leave(void)
{
    CHECK(mock_gate_held && mock_irq_depth != 0u);
    p4_task_pins_unlock(&test_sysbase);
    CHECK(!mock_gate_held);
}

static void reset_case(void)
{
    memset(test_records, 0, sizeof(test_records));
    memset(test_slots, 0, sizeof(test_slots));
    memset(&test_bank, 0, sizeof(test_bank));
    p4_task_registry_init(&p4_task_records);
    CHECK(p4_remove_bank_init(&test_bank, test_slots,
                              TEST_BANK_CAPACITY) == 1);
    mock_irq_depth = 0u;
    mock_gate_held = 0;
    mock_gate_address = &p4_task_pin_gate;
    mock_outer_semaphore_gate_held = 0;
}

static int add_task(unsigned int index, struct Task *identity)
{
    int result;
    CHECK(index < TEST_REGISTRY_CAPACITY);
    gate_enter();
    result = p4_task_registry_register_locked(&p4_task_records,
        &test_records[index], identity);
    gate_leave();
    return result;
}

static int add_ticket(struct Task *requester, struct Task *target,
                      struct p4_remove_handle *handle)
{
    int result;
    gate_enter();
    result = p4_remove_bank_reserve(&test_bank, (uintptr_t)requester,
        (uintptr_t)target, handle);
    gate_leave();
    return result;
}

static int cancel_requester(struct Task *requester, uint32_t *dropped)
{
    int result;
    gate_enter();
    result = p4_remove_bank_cancel_requester(&test_bank,
        (uintptr_t)requester, dropped);
    gate_leave();
    return result;
}

static int drop_requester(struct p4_remove_handle handle,
                          struct Task *requester, int *reclaim)
{
    int result;
    gate_enter();
    result = p4_remove_bank_drop(&test_bank, handle, (uintptr_t)requester,
                                 0, reclaim);
    gate_leave();
    return result;
}

static struct p4_task_registry_record *lookup_task(struct Task *identity)
{
    struct p4_task_registry_record *record;
    gate_enter();
    record = p4_task_registry_lookup_locked(&p4_task_records, identity);
    gate_leave();
    return record;
}

static struct p4_task_registry_record *make_retained_target(
    struct Task *identity)
{
    struct p4_task_registry_record *claimed = NULL;
    struct Task *ready;
    int result;

    gate_enter();
    result = p4_task_registry_retire_for_service_locked(
        &p4_task_records, identity);
    gate_leave();
    CHECK(result == 1);

    gate_enter();
    ready = p4_task_registry_take_service_ready_locked(
        &p4_task_records, identity);
    gate_leave();
    CHECK(ready == identity);

    gate_enter();
    result = p4_task_registry_claim_next_retained_service_locked(
        &p4_task_records, &claimed);
    gate_leave();
    CHECK(result == 1 && claimed != NULL && claimed->identity == identity);
    return claimed;
}

static int finish_target(struct p4_task_registry_record *record)
{
    int result;
    gate_enter();
    result = p4_task_registry_finish_removal_locked(
        &p4_task_records, &test_bank, record);
    CHECK(mock_gate_held && mock_irq_depth != 0u);
    gate_leave();
    return result;
}

static void snapshot_state(struct state_snapshot *snapshot)
{
    gate_enter();
    memcpy(&snapshot->registry, &p4_task_records,
           sizeof(snapshot->registry));
    memcpy(snapshot->records, test_records, sizeof(test_records));
    memcpy(&snapshot->bank, &test_bank, sizeof(test_bank));
    memcpy(snapshot->slots, test_slots, sizeof(test_slots));
    gate_leave();
}

static void check_unchanged(const struct state_snapshot *snapshot,
                            const char *label)
{
    gate_enter();
    CHECK(memcmp(&snapshot->registry, &p4_task_records,
                 sizeof(snapshot->registry)) == 0);
    CHECK(memcmp(snapshot->records, test_records,
                 sizeof(test_records)) == 0);
    CHECK(memcmp(&snapshot->bank, &test_bank,
                 sizeof(test_bank)) == 0);
    CHECK(memcmp(snapshot->slots, test_slots,
                 sizeof(test_slots)) == 0);
    if (memcmp(&snapshot->registry, &p4_task_records,
               sizeof(snapshot->registry)) != 0 ||
        memcmp(snapshot->records, test_records, sizeof(test_records)) != 0 ||
        memcmp(&snapshot->bank, &test_bank, sizeof(test_bank)) != 0 ||
        memcmp(snapshot->slots, test_slots, sizeof(test_slots)) != 0)
        fprintf(stderr, "FAIL state changed after refusal: %s\n", label);
    gate_leave();
}

static int claim_notification(struct p4_semaphore_notification *snapshot)
{
    int result;
    gate_enter();
    result = p4_task_registry_claim_next_semaphore_notification_locked(
        &p4_task_records, snapshot);
    gate_leave();
    return result;
}

static int finish_notification(
    const struct p4_semaphore_notification *snapshot)
{
    int result;
    gate_enter();
    result = p4_task_registry_finish_semaphore_notification_locked(
        &p4_task_records, snapshot);
    gate_leave();
    return result;
}

static void seed_completed_ticket(struct Task *target, struct Task *requester,
                                  struct p4_remove_handle *handle)
{
    struct p4_task_registry_record *retained;
    struct p4_semaphore_notification batch;

    CHECK(add_ticket(requester, target, handle) == 1);
    retained = make_retained_target(target);
    CHECK(finish_target(retained) == 2);
    CHECK(test_slots[handle->index].completion.requester_ref == 1u &&
          test_slots[handle->index].completion.service_ref == 0u &&
          test_slots[handle->index].completion.completed == 1u);
    CHECK(claim_notification(&batch) == 1);
    CHECK(batch.identity == requester && batch.count == 1u);
    CHECK(finish_notification(&batch) == 1);
}

static void test_successful_batch(void *page)
{
    struct Task *seed_a = guarded_identity(page, 64u);
    struct Task *seed_a_pending = guarded_identity(page, 96u);
    struct Task *seed_b = guarded_identity(page, 128u);
    struct Task *target_a = guarded_identity(page, 160u);
    struct Task *target_b = guarded_identity(page, 192u);
    struct Task *other_target = guarded_identity(page, 224u);
    struct Task *requester_a = guarded_identity(page, 256u);
    struct Task *requester_b = guarded_identity(page, 288u);
    struct Task *cancelled_requester = guarded_identity(page, 320u);
    struct p4_remove_handle seed_a_handle, seed_a_pending_handle;
    struct p4_remove_handle seed_b_handle, prior_completed_handle;
    struct p4_remove_handle a1, a2, b1, cancelled, a3, b2, b3;
    struct p4_task_registry_record *retained_a, *retained_b;
    struct p4_semaphore_notification old_a_inflight, new_a_batch;
    struct p4_semaphore_notification b_batch;
    struct p4_task_registry_record *a_record, *b_record;
    struct p4_remove_bank_slot prior_completed;
    uint32_t dropped = UINT32_MAX;
    int reclaim = -1;
    uint32_t initial_count;

    reset_case();
    CHECK(add_task(0u, seed_a) == 1);
    CHECK(add_task(1u, seed_a_pending) == 1);
    CHECK(add_task(2u, seed_b) == 1);
    CHECK(add_task(3u, target_a) == 1);
    CHECK(add_task(4u, target_b) == 1);
    CHECK(add_task(5u, other_target) == 1);
    CHECK(add_task(6u, requester_a) == 1);
    CHECK(add_task(7u, requester_b) == 1);
    CHECK(add_task(8u, cancelled_requester) == 1);

    /* Give A an inflight generation and a newer pending generation. */
    CHECK(add_ticket(requester_a, seed_a, &seed_a_handle) == 1);
    retained_a = make_retained_target(seed_a);
    CHECK(finish_target(retained_a) == 2);
    CHECK(claim_notification(&old_a_inflight) == 1);
    CHECK(old_a_inflight.identity == requester_a &&
          old_a_inflight.count == 1u && old_a_inflight.generation == 1u);
    /* Publish generation 2 while generation 1 remains inflight. */
    CHECK(add_ticket(requester_a, seed_a_pending, &seed_a_pending_handle) == 1);
    retained_a = make_retained_target(seed_a_pending);
    CHECK(finish_target(retained_a) == 2);

    /* A third completed ticket is delivered and consumed for B. It remains
     * requester-owned in the bank but has no service reference. */
    seed_completed_ticket(seed_b, requester_b, &seed_b_handle);
    CHECK(test_bank.serial == 3u);
    prior_completed_handle = seed_b_handle;
    prior_completed = test_slots[prior_completed_handle.index];

    CHECK(add_ticket(requester_a, target_a, &a1) == 1);
    CHECK(add_ticket(requester_a, target_a, &a2) == 1);
    CHECK(add_ticket(requester_b, target_a, &b1) == 1);
    CHECK(add_ticket(cancelled_requester, target_a, &cancelled) == 1);
    CHECK(cancel_requester(cancelled_requester, &dropped) == 1);
    CHECK(dropped == 1u);
    CHECK(test_slots[cancelled.index].active == 1u &&
          test_slots[cancelled.index].completion.requester_ref == 0u &&
          test_slots[cancelled.index].completion.service_ref == 1u);
    CHECK(add_ticket(requester_a, target_b, &a3) == 1);
    CHECK(add_ticket(requester_b, target_b, &b2) == 1);
    CHECK(add_ticket(requester_b, target_b, &b3) == 1);

    initial_count = p4_task_records.count;
    retained_a = make_retained_target(target_a);
    CHECK(finish_target(retained_a) == 4); /* detach + three notifications */
    CHECK(p4_task_records.count == initial_count - 1u);
    CHECK(retained_a->next == NULL && retained_a->identity == target_a);
    CHECK(test_slots[a1.index].completion.service_ref == 0u &&
          test_slots[a1.index].completion.completed == 1u);
    CHECK(test_slots[a2.index].completion.service_ref == 0u &&
          test_slots[b1.index].completion.service_ref == 0u);
    CHECK(test_slots[cancelled.index].active == 0u);
    CHECK(memcmp(&test_slots[prior_completed_handle.index], &prior_completed,
                 sizeof(prior_completed)) == 0);

    a_record = lookup_task(requester_a);
    b_record = lookup_task(requester_b);
    CHECK(a_record != NULL && a_record->semaphore_inflight == 1u &&
          a_record->semaphore_inflight_generation == 1u &&
          a_record->semaphore_pending == 3u &&
          a_record->semaphore_pending_generation == 2u &&
          a_record->lifetime.notification_pins == 4u);
    CHECK(b_record != NULL && b_record->semaphore_pending == 1u &&
          b_record->semaphore_pending_generation == 4u &&
          b_record->lifetime.notification_pins == 1u &&
          p4_task_records.semaphore_notification_serial == 4u);

    /* The old inflight batch is independent of the newer coalesced batch. */
    CHECK(finish_notification(&old_a_inflight) == 1);
    a_record = lookup_task(requester_a);
    CHECK(a_record != NULL && a_record->semaphore_inflight == 0u &&
          a_record->semaphore_pending == 3u &&
          a_record->lifetime.notification_pins == 3u);

    CHECK(claim_notification(&b_batch) == 1);
    CHECK(b_batch.identity == requester_b && b_batch.count == 1u &&
          b_batch.generation == 4u);
    CHECK(finish_notification(&b_batch) == 1);

    retained_b = make_retained_target(target_b);
    CHECK(finish_target(retained_b) == 4); /* detach + three notifications */
    a_record = lookup_task(requester_a);
    b_record = lookup_task(requester_b);
    CHECK(a_record != NULL && a_record->semaphore_pending == 4u &&
          a_record->semaphore_pending_generation == 2u &&
          a_record->lifetime.notification_pins == 4u);
    CHECK(b_record != NULL && b_record->semaphore_pending == 2u &&
          b_record->semaphore_pending_generation == 5u &&
          b_record->lifetime.notification_pins == 2u &&
          p4_task_records.semaphore_notification_serial == 5u);

    {
        struct p4_semaphore_notification first_batch;
        struct p4_semaphore_notification second_batch;
        CHECK(claim_notification(&first_batch) == 1);
        CHECK(claim_notification(&second_batch) == 1);
        if (first_batch.identity == requester_a)
        {
            new_a_batch = first_batch;
            b_batch = second_batch;
        }
        else
        {
            b_batch = first_batch;
            new_a_batch = second_batch;
        }
    }
    CHECK(b_batch.identity == requester_b && b_batch.count == 2u &&
          b_batch.generation == 5u);
    CHECK(new_a_batch.identity == requester_a && new_a_batch.count == 4u &&
          new_a_batch.generation == 2u);
    CHECK(finish_notification(&b_batch) == 1);
    CHECK(finish_notification(&new_a_batch) == 1);
    a_record = lookup_task(requester_a);
    b_record = lookup_task(requester_b);
    CHECK(a_record != NULL && a_record->lifetime.notification_pins == 0u &&
          a_record->semaphore_pending == 0u && a_record->semaphore_inflight == 0u);
    CHECK(b_record != NULL && b_record->lifetime.notification_pins == 0u &&
          b_record->semaphore_pending == 0u && b_record->semaphore_inflight == 0u);

    CHECK(drop_requester(seed_a_handle, requester_a, &reclaim) == 1 &&
          reclaim == 1);
    CHECK(drop_requester(seed_a_pending_handle, requester_a, &reclaim) == 1 &&
          reclaim == 1);
    CHECK(drop_requester(seed_b_handle, requester_b, &reclaim) == 1 &&
          reclaim == 1);
    CHECK(drop_requester(a1, requester_a, &reclaim) == 1 && reclaim == 1);
    CHECK(drop_requester(a2, requester_a, &reclaim) == 1 && reclaim == 1);
    CHECK(drop_requester(b1, requester_b, &reclaim) == 1 && reclaim == 1);
    CHECK(drop_requester(a3, requester_a, &reclaim) == 1 && reclaim == 1);
    CHECK(drop_requester(b2, requester_b, &reclaim) == 1 && reclaim == 1);
    CHECK(drop_requester(b3, requester_b, &reclaim) == 1 && reclaim == 1);
}

static void test_unknown_and_not_inflight(void *page)
{
    struct Task *target = guarded_identity(page, 64u);
    struct Task *requester = guarded_identity(page, 96u);
    struct p4_task_registry_record *raw;
    struct p4_remove_handle handle;
    struct state_snapshot before;
    struct p4_task_registry_record *unknown =
        (struct p4_task_registry_record *)guarded_identity(page, 128u);

    reset_case();
    CHECK(add_task(0u, target) == 1);
    CHECK(add_task(1u, requester) == 1);
    CHECK(add_ticket(requester, target, &handle) == 1);
    snapshot_state(&before);
    CHECK(finish_target(NULL) == 0);
    check_unchanged(&before, "null/non-inflight raw record");
    CHECK(finish_target(unknown) == 0);
    check_unchanged(&before, "unmapped raw record identity");

    /* A registered but live record is not an in-flight service claim. */
    raw = &test_records[0];
    CHECK(finish_target(raw) == 0);
    check_unchanged(&before, "registered target without service claim");
}

static void test_recipient_refusal(void *page, int retired)
{
    struct Task *target = guarded_identity(page, 64u);
    struct Task *requester = guarded_identity(page, 96u);
    struct p4_task_registry_record *retained;
    struct p4_remove_handle handle;
    struct state_snapshot before;
    int result;

    reset_case();
    CHECK(add_task(0u, target) == 1);
    if (retired)
        CHECK(add_task(1u, requester) == 1);
    CHECK(add_ticket(requester, target, &handle) == 1);
    retained = make_retained_target(target);
    if (retired)
    {
        gate_enter();
        result = p4_task_registry_retire_for_service_locked(
            &p4_task_records, requester);
        gate_leave();
        CHECK(result == 1);
    }
    snapshot_state(&before);
    CHECK(finish_target(retained) == -1);
    check_unchanged(&before, retired ? "retired requester" : "missing requester");
}

static void test_duplicate_recipient_identity(void *page)
{
    struct Task *target = guarded_identity(page, 64u);
    struct Task *requester = guarded_identity(page, 96u);
    struct p4_task_registry_record *retained;
    struct p4_remove_handle handle;
    struct state_snapshot before;
    int result;

    reset_case();
    /* Construct a structurally valid list with one duplicated opaque identity.
     * The registry lookup for that requester must fail before commit. */
    memset(test_records, 0, sizeof(test_records));
    test_records[0].identity = target;
    test_records[0].next = &test_records[1];
    test_records[1].identity = requester;
    test_records[1].next = &test_records[2];
    test_records[2].identity = requester;
    p4_task_records.head = &test_records[0];
    p4_task_records.count = 3u;
    CHECK(add_ticket(requester, target, &handle) == 1);

    gate_enter();
    result = p4_task_registry_retire_for_service_locked(
        &p4_task_records, target);
    gate_leave();
    CHECK(result == 1);
    gate_enter();
    CHECK(p4_task_registry_take_service_ready_locked(
        &p4_task_records, target) == target);
    gate_leave();
    gate_enter();
    result = p4_task_registry_claim_next_retained_service_locked(
        &p4_task_records, &retained);
    gate_leave();
    CHECK(result == 1 && retained == &test_records[0]);
    snapshot_state(&before);
    CHECK(finish_target(retained) == -1);
    check_unchanged(&before, "duplicate requester identity");
}

static void test_capacity_refusals(void *page, int mode)
{
    struct Task *target = guarded_identity(page, 64u);
    struct Task *requester = guarded_identity(page, 96u);
    struct p4_task_registry_record *recipient;
    struct p4_task_registry_record *retained;
    struct p4_remove_handle handle;
    struct state_snapshot before;
    int result;

    reset_case();
    CHECK(add_task(0u, target) == 1);
    CHECK(add_task(1u, requester) == 1);
    CHECK(add_ticket(requester, target, &handle) == 1);
    recipient = &test_records[1];
    if (mode == 0)
    {
        p4_task_records.semaphore_notification_serial = UINT32_MAX;
        snapshot_state(&before);
        gate_enter();
        result = p4_task_registry_retire_for_service_locked(
            &p4_task_records, target);
        gate_leave();
        CHECK(result == 0);
        check_unchanged(&before, "service-generation exhaustion before retirement");
        return;
    }
    else if (mode == 1)
    {
        recipient->lifetime.notification_pins = UINT32_MAX;
    }
    else if (mode == 2)
    {
        p4_task_records.semaphore_notification_serial = UINT32_MAX - 1u;
        recipient->semaphore_pending = 1u;
        recipient->semaphore_pending_generation = UINT32_MAX - 1u;
        recipient->lifetime.notification_pins = 1u;
    }
    else
    {
        p4_task_records.semaphore_notification_serial = 7u;
        recipient->semaphore_pending = UINT32_MAX;
        recipient->semaphore_pending_generation = 7u;
        recipient->lifetime.notification_pins = UINT32_MAX;
    }
    retained = make_retained_target(target);
    snapshot_state(&before);
    result = finish_target(retained);
    if (mode == 2)
    {
        /* Retirement reserved UINT32_MAX before freeing. A pending batch
         * keeps its earlier generation; final ACK needs no new serial. */
        CHECK(result == 2);
        CHECK(p4_task_records.semaphore_notification_serial == UINT32_MAX);
        CHECK(recipient->semaphore_pending == 2u &&
              recipient->semaphore_pending_generation == UINT32_MAX - 1u &&
              recipient->lifetime.notification_pins == 2u);
    }
    else
    {
        CHECK(result == 0);
        check_unchanged(&before, "notification/serial headroom refusal");
    }
}

static void test_malformed_bank(void *page)
{
    struct Task *target = guarded_identity(page, 64u);
    struct Task *requester = guarded_identity(page, 96u);
    struct p4_task_registry_record *retained;
    struct p4_remove_handle handle;
    struct state_snapshot before;

    reset_case();
    CHECK(add_task(0u, target) == 1);
    CHECK(add_task(1u, requester) == 1);
    CHECK(add_ticket(requester, target, &handle) == 1);
    retained = make_retained_target(target);
    test_slots[handle.index].completion.service_ref = 0u;
    snapshot_state(&before);
    CHECK(finish_target(retained) == -1);
    check_unchanged(&before, "malformed bank ticket");
}

static void test_finish_alias_refusals(void *page)
{
    struct Task *target = guarded_identity(page, 64u);
    struct Task *requester = guarded_identity(page, 96u);
    struct p4_task_registry_record *retained;
    struct p4_remove_handle handle;
    struct state_snapshot before;
    struct p4_remove_bank alias_bank;
    struct p4_remove_bank alias_before;
    int result;

    reset_case();
    CHECK(add_task(0u, target) == 1);
    CHECK(add_task(1u, requester) == 1);
    CHECK(add_ticket(requester, target, &handle) == 1);
    retained = make_retained_target(target);
    snapshot_state(&before);

    gate_enter();
    result = p4_task_registry_finish_removal_locked(&p4_task_records,
        (struct p4_remove_bank *)(void *)&p4_task_records, retained);
    gate_leave();
    CHECK(result == -1);
    check_unchanged(&before, "bank header overlaps registry");

    /* The root preflight rejects a slot pool overlapping any registry record
     * before the bank scanner types or traverses it as a ticket array. */
    memset(&alias_bank, 0, sizeof(alias_bank));
    alias_bank.magic = P4_REMOVE_BANK_MAGIC;
    alias_bank.capacity = 1u;
    alias_bank.serial = test_bank.serial;
    alias_bank.slots = (struct p4_remove_bank_slot *)(void *)&test_records[1];
    alias_before = alias_bank;
    gate_enter();
    result = p4_task_registry_finish_removal_locked(&p4_task_records,
        &alias_bank, retained);
    gate_leave();
    CHECK(result == -1);
    CHECK(memcmp(&alias_bank, &alias_before, sizeof(alias_bank)) == 0);
    check_unchanged(&before, "bank slot pool overlaps registry record");
}

int main(void)
{
    long page_size;
    void *page;
    unsigned int baseline_checks;
    unsigned int before_new;

    if (p4_remove_finish_admission_baseline_main() != 0)
        return 1;
    baseline_checks = checks;
    CHECK(p4_task_pins_prepared == 1);
    CHECK(!mock_gate_held && mock_irq_depth == 0u);
    page_size = sysconf(_SC_PAGESIZE);
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

    before_new = checks;
    test_successful_batch(page);
    test_unknown_and_not_inflight(page);
    test_recipient_refusal(page, 0);
    test_recipient_refusal(page, 1);
    test_duplicate_recipient_identity(page);
    test_capacity_refusals(page, 0);
    test_capacity_refusals(page, 1);
    test_capacity_refusals(page, 2);
    test_capacity_refusals(page, 3);
    test_malformed_bank(page);
    test_finish_alias_refusals(page);

    CHECK(munmap(page, (size_t)page_size) == 0);
    CHECK(!mock_gate_held && mock_irq_depth == 0u);
    CHECK(mock_fatal_calls == 0u);
    CHECK(mock_acquire_calls == mock_unlock_calls);
    CHECK(mock_disable_calls == mock_enable_calls);
    if (failures != 0u)
        return 1;
    printf("P4 removal finish transaction: %u checks passed; "
           "admission baseline %u\n", checks - before_new, baseline_checks);
    return 0;
}
