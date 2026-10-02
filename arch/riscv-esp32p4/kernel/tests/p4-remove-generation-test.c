/* Actual-source regression for pre-free service notification generations. */
#define main p4_remove_generation_admission_baseline_main
#include "p4-semaphore-operation-admission-test.c"
#undef main

#include "../../exec/p4_remove_bank.c"
#include "../../exec/p4_remove_completion.c"

#define GENERATION_RECORD_CAPACITY 16u
#define GENERATION_BANK_CAPACITY 24u

static struct p4_task_registry_record test_records[GENERATION_RECORD_CAPACITY];
static struct p4_remove_bank test_bank;
static struct p4_remove_bank_slot test_slots[GENERATION_BANK_CAPACITY];
static struct ExecBase test_sysbase;

struct state_snapshot
{
    struct p4_task_registry registry;
    struct p4_task_registry_record records[GENERATION_RECORD_CAPACITY];
    struct p4_remove_bank bank;
    struct p4_remove_bank_slot slots[GENERATION_BANK_CAPACITY];
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
    memset(&test_sysbase, 0, sizeof(test_sysbase));
    p4_task_registry_init(&p4_task_records);
    CHECK(p4_remove_bank_init(&test_bank, test_slots,
                              GENERATION_BANK_CAPACITY) == 1);
    mock_irq_depth = 0u;
    mock_gate_held = 0;
    mock_gate_address = &p4_task_pin_gate;
    mock_outer_semaphore_gate_held = 0;
}

static int add_task(unsigned int index, struct Task *identity)
{
    int result;
    CHECK(index < GENERATION_RECORD_CAPACITY);
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
    struct p4_task_registry_record *retained = NULL;
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
        &p4_task_records, &retained);
    gate_leave();
    CHECK(result == 1 && retained != NULL && retained->identity == identity);
    return retained;
}

static int finish_target(struct p4_task_registry_record *record)
{
    int result;
    gate_enter();
    result = p4_task_registry_finish_removal_locked(
        &p4_task_records, &test_bank, record);
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

static struct p4_semaphore_ledger_operation *generation_prepare_operation(
    struct direct_fixture *fixture, struct Task *identity,
    struct p4_task_wait_operation *operation)
{
    int result;

    fixture_add_task(fixture, 0u, identity, 0u);
    result = p4_task_registry_wait_reserve_operation_locked(
        &fixture->registry, &fixture->ledger, identity, 1u, operation);
    CHECK(result == 1);
    result = p4_task_registry_wait_operation_set_entry_locked(
        &fixture->registry, &fixture->ledger, identity,
        operation->generation, 0u, (const void *)(uintptr_t)0x101u,
        (const void *)(uintptr_t)0x201u);
    CHECK(result == 1);
    result = p4_task_registry_wait_operation_close_locked(
        &fixture->registry, &fixture->ledger, identity,
        operation->generation);
    CHECK(result == 1);
    return operation->operation;
}

static void test_plain_retirement_reservation(void *page)
{
    struct Task *target = guarded_identity(page, 64u);
    struct state_snapshot before;
    int result;

    reset_case();
    CHECK(add_task(0u, target) == 1);
    p4_task_records.semaphore_notification_serial = UINT32_MAX;
    snapshot_state(&before);
    gate_enter();
    result = p4_task_registry_retire_for_service_locked(
        &p4_task_records, target);
    gate_leave();
    CHECK(result == 0);
    check_unchanged(&before, "plain service retirement at exhausted serial");
    CHECK(test_records[0].service_generation == 0u);

    /* The generation limit is specific to service retirement. Ordinary
     * lifetime retirement neither allocates nor resets the shared serial. */
    gate_enter();
    result = p4_task_registry_retire_locked(&p4_task_records, target);
    gate_leave();
    CHECK(result == 1);
    CHECK(p4_task_records.semaphore_notification_serial == UINT32_MAX);
    CHECK(test_records[0].service_generation == 0u);
}

static void test_operation_retirement_reservation(void *page)
{
    struct direct_fixture fixture;
    struct direct_fixture wrong_ledger;
    struct p4_task_wait_operation operation = { 0, 0 };
    struct p4_task_registry_record *record;
    struct byte_snapshot before;
    struct byte_snapshot wrong_before;
    struct Task *identity = guarded_identity(page, 64u);
    int result;

    fixture_init(&fixture, TEST_OP_CAPACITY, TEST_ENTRY_CAPACITY);
    generation_prepare_operation(&fixture, identity, &operation);
    record = &fixture.records[0];
    fixture.registry.semaphore_notification_serial = UINT32_MAX;
    snapshot_fixture(&fixture, &before);
    result = p4_task_registry_retire_with_ledger_locked(
        &fixture.registry, &fixture.ledger, identity, 1u);
    CHECK(result == 0);
    CHECK(fixture_matches_snapshot(&fixture, &before));
    CHECK(record->wait_scope.cancel_requested == 0u &&
          operation.operation->cancelled == 0u &&
          record->service_generation == 0u &&
          fixture.registry.semaphore_notification_serial == UINT32_MAX);

    /* A scope whose ledger has already been cancelled independently is a
     * cross-domain mismatch. Refusal must not issue a target token. */
    fixture.registry.semaphore_notification_serial = 23u;
    CHECK(p4_semaphore_ledger_cancel(&fixture.ledger,
                                     operation.operation) == 1);
    snapshot_fixture(&fixture, &before);
    result = p4_task_registry_retire_with_ledger_locked(
        &fixture.registry, &fixture.ledger, identity, 1u);
    CHECK(result == -1);
    CHECK(fixture_matches_snapshot(&fixture, &before));
    CHECK(fixture.registry.semaphore_notification_serial == 23u &&
          record->service_generation == 0u);

    /* A valid ledger in the wrong arena cannot resolve this active scope. */
    fixture_init(&fixture, TEST_OP_CAPACITY, TEST_ENTRY_CAPACITY);
    generation_prepare_operation(&fixture, identity, &operation);
    fixture_init(&wrong_ledger, TEST_OP_CAPACITY, TEST_ENTRY_CAPACITY);
    fixture.registry.semaphore_notification_serial = 31u;
    snapshot_fixture(&fixture, &before);
    snapshot_fixture(&wrong_ledger, &wrong_before);
    result = p4_task_registry_retire_with_ledger_locked(
        &fixture.registry, &wrong_ledger.ledger, identity, 1u);
    CHECK(result != 1);
    CHECK(fixture_matches_snapshot(&fixture, &before));
    CHECK(fixture_matches_snapshot(&wrong_ledger, &wrong_before));
    CHECK(fixture.registry.semaphore_notification_serial == 31u);

    /* A successful ordinary fused retirement cancels the operation without
     * consuming a service token. A later service retry is refused unchanged. */
    fixture_init(&fixture, TEST_OP_CAPACITY, TEST_ENTRY_CAPACITY);
    generation_prepare_operation(&fixture, identity, &operation);
    fixture.registry.semaphore_notification_serial = 41u;
    result = p4_task_registry_retire_with_ledger_locked(
        &fixture.registry, &fixture.ledger, identity, 0u);
    CHECK(result == 1);
    CHECK(fixture.registry.semaphore_notification_serial == 41u &&
          fixture.records[0].service_generation == 0u &&
          fixture.records[0].wait_scope.cancel_requested == 1u &&
          operation.operation->cancelled == 1u);
    snapshot_fixture(&fixture, &before);
    result = p4_task_registry_retire_with_ledger_locked(
        &fixture.registry, &fixture.ledger, identity, 1u);
    CHECK(result == 0);
    CHECK(fixture_matches_snapshot(&fixture, &before));
    CHECK(fixture.registry.semaphore_notification_serial == 41u);
}

static void test_operation_generation_survives_cleanup(void *page)
{
    struct direct_fixture fixture;
    struct p4_task_wait_operation operation = { 0, 0 };
    struct p4_task_wait_cancel_snapshot cancel;
    struct p4_task_registry_record *record;
    struct p4_task_registry_record *retained = NULL;
    struct Task *identity = guarded_identity(page, 64u);
    uint32_t target_generation;
    int result;

    fixture_init(&fixture, TEST_OP_CAPACITY, TEST_ENTRY_CAPACITY);
    generation_prepare_operation(&fixture, identity, &operation);
    record = &fixture.records[0];
    fixture.registry.semaphore_notification_serial = 100u;
    result = p4_task_registry_retire_with_ledger_locked(
        &fixture.registry, &fixture.ledger, identity, 1u);
    CHECK(result == 1);
    target_generation = 101u;
    CHECK(fixture.registry.semaphore_notification_serial == target_generation &&
          record->service_generation == target_generation &&
          record->service_pending == 1u && record->wait_scope.active == 1u &&
          record->lifetime.notification_pins == 2u &&
          operation.operation->cancelled == 1u);

    CHECK(p4_task_registry_leave_running_locked(
        &fixture.registry, identity, 0u) == 1);
    memset(&cancel, 0, sizeof(cancel));
    result = p4_task_registry_claim_next_wait_cancel_with_ledger_locked(
        &fixture.registry, &fixture.ledger, &cancel);
    CHECK(result == 1 && cancel.identity == identity &&
          cancel.generation == operation.generation &&
          cancel.kind == P4_TASK_WAIT_OPERATION);
    CHECK(p4_task_registry_wait_operation_finish_cancel_locked(
        &fixture.registry, &fixture.ledger, identity,
        operation.generation) == 1);
    CHECK(record->wait_scope.active == 0u &&
          record->lifetime.notification_pins == 1u &&
          record->service_generation == target_generation &&
          fixture.registry.semaphore_notification_serial == target_generation);

    CHECK(p4_task_registry_take_service_ready_locked(
        &fixture.registry, identity) == identity);
    CHECK(record->service_queued == 1u &&
          record->service_generation == target_generation);
    result = p4_task_registry_claim_next_retained_service_locked(
        &fixture.registry, &retained);
    CHECK(result == 1 && retained == record &&
          record->service_inflight == 1u &&
          record->service_generation == target_generation);
    CHECK(p4_task_registry_defer_removal_finish_locked(
        &fixture.registry, retained) == 1);
    CHECK(record->service_postfree == 1u &&
          record->service_generation == target_generation);
    CHECK(p4_task_registry_finish_retained_service_locked(
        &fixture.registry, retained) == 1);
    CHECK(fixture.registry.count == 0u && record->next == NULL &&
          record->service_generation == 0u &&
          fixture.registry.semaphore_notification_serial == target_generation);
}

static void test_reserved_generation_lifecycle(void *page)
{
    struct Task *target = guarded_identity(page, 64u);
    struct Task *requester_a = guarded_identity(page, 96u);
    struct Task *requester_b = guarded_identity(page, 128u);
    struct Task *wrong_identity = guarded_identity(page, 160u);
    struct p4_task_registry_record *retained;
    struct p4_task_registry_record *postfree = NULL;
    struct p4_semaphore_notification first_batch;
    struct p4_semaphore_notification second_batch;
    struct p4_semaphore_notification batch_a;
    struct p4_semaphore_notification batch_b;
    struct p4_semaphore_notification wrong;
    struct p4_remove_handle ticket_a, ticket_b;
    struct state_snapshot before;
    uint32_t target_generation;
    int result;

    reset_case();
    CHECK(add_task(0u, target) == 1);
    CHECK(add_task(1u, requester_a) == 1);
    CHECK(add_task(2u, requester_b) == 1);
    CHECK(add_task(3u, wrong_identity) == 1);
    CHECK(add_ticket(requester_a, target, &ticket_a) == 1);
    CHECK(add_ticket(requester_b, target, &ticket_b) == 1);

    gate_enter();
    result = p4_task_registry_retire_for_service_locked(
        &p4_task_records, target);
    gate_leave();
    CHECK(result == 1);
    target_generation = test_records[0].service_generation;
    CHECK(target_generation == 1u &&
          p4_task_records.semaphore_notification_serial == target_generation &&
          test_records[0].service_pending == 1u);

    gate_enter();
    CHECK(p4_task_registry_take_service_ready_locked(
        &p4_task_records, target) == target);
    gate_leave();
    CHECK(test_records[0].service_queued == 1u &&
          test_records[0].service_generation == target_generation);

    gate_enter();
    result = p4_task_registry_claim_next_retained_service_locked(
        &p4_task_records, &retained);
    gate_leave();
    CHECK(result == 1 && retained == &test_records[0]);
    CHECK(test_records[0].service_pending == 0u &&
          test_records[0].service_queued == 0u &&
          test_records[0].service_inflight == 1u &&
          test_records[0].service_generation == target_generation);

    gate_enter();
    CHECK(p4_task_registry_defer_removal_finish_locked(
        &p4_task_records, retained) == 1);
    result = p4_task_registry_next_postfree_after_locked(
        &p4_task_records, 0u, &postfree);
    gate_leave();
    CHECK(result == 1 && postfree == retained);
    CHECK(test_records[0].service_postfree == 1u &&
          test_records[0].service_generation == target_generation);

    CHECK(finish_target(retained) == 3);
    CHECK(test_records[0].service_generation == 0u &&
          test_records[0].service_inflight == 0u &&
          test_records[0].service_postfree == 0u);
    CHECK(lookup_task(target) == NULL);
    CHECK(test_records[1].semaphore_pending == 1u &&
          test_records[2].semaphore_pending == 1u &&
          test_records[1].semaphore_pending_generation == target_generation &&
          test_records[2].semaphore_pending_generation == target_generation &&
          p4_task_records.semaphore_notification_serial == target_generation);

    CHECK(claim_notification(&first_batch) == 1);
    CHECK(claim_notification(&second_batch) == 1);
    if (first_batch.identity == requester_a)
    {
        batch_a = first_batch;
        batch_b = second_batch;
    }
    else
    {
        batch_b = first_batch;
        batch_a = second_batch;
    }
    CHECK(batch_a.identity == requester_a &&
          batch_a.generation == target_generation && batch_a.count == 1u);
    CHECK(batch_b.identity == requester_b &&
          batch_b.generation == target_generation && batch_b.count == 1u);

    wrong = batch_a;
    wrong.identity = wrong_identity;
    snapshot_state(&before);
    CHECK(finish_notification(&wrong) == 0);
    check_unchanged(&before, "same token with a different recipient identity");
    CHECK(finish_notification(&batch_a) == 1);
    CHECK(finish_notification(&batch_b) == 1);
}

static void test_max_reserved_token_finishes(void *page)
{
    struct Task *target = guarded_identity(page, 64u);
    struct Task *requester = guarded_identity(page, 96u);
    struct p4_task_registry_record *retained;
    struct p4_semaphore_notification batch;
    struct p4_remove_handle ticket;

    reset_case();
    CHECK(add_task(0u, target) == 1);
    CHECK(add_task(1u, requester) == 1);
    CHECK(add_ticket(requester, target, &ticket) == 1);
    p4_task_records.semaphore_notification_serial = UINT32_MAX - 1u;
    retained = make_retained_target(target);
    CHECK(retained->service_generation == UINT32_MAX);
    CHECK(p4_task_records.semaphore_notification_serial == UINT32_MAX);
    CHECK(finish_target(retained) == 2);
    CHECK(test_records[1].semaphore_pending == 1u &&
          test_records[1].semaphore_pending_generation == UINT32_MAX &&
          p4_task_records.semaphore_notification_serial == UINT32_MAX);
    CHECK(claim_notification(&batch) == 1);
    CHECK(batch.identity == requester && batch.generation == UINT32_MAX);
    CHECK(finish_notification(&batch) == 1);
}

static void test_older_target_token_than_inflight(void *page)
{
    struct Task *target_old = guarded_identity(page, 64u);
    struct Task *target_later = guarded_identity(page, 96u);
    struct Task *requester = guarded_identity(page, 128u);
    struct Task *wrong_identity = guarded_identity(page, 160u);
    struct p4_task_registry_record *retained_old;
    struct p4_task_registry_record *retained_later;
    struct p4_semaphore_notification later_batch;
    struct p4_semaphore_notification old_batch;
    struct p4_semaphore_notification forged;
    struct p4_remove_handle later_ticket, late_ticket;
    struct state_snapshot before;

    reset_case();
    CHECK(add_task(0u, target_old) == 1);
    CHECK(add_task(1u, target_later) == 1);
    CHECK(add_task(2u, requester) == 1);
    CHECK(add_task(3u, wrong_identity) == 1);

    /* Reserve the older token, but delay its final ACK while another target
     * publishes and delivers a newer generation to this recipient. */
    retained_old = make_retained_target(target_old);
    CHECK(retained_old->service_generation == 1u);
    CHECK(add_ticket(requester, target_later, &later_ticket) == 1);
    retained_later = make_retained_target(target_later);
    CHECK(retained_later->service_generation == 2u);
    CHECK(finish_target(retained_later) == 2);
    CHECK(test_records[2].semaphore_pending_generation == 2u);
    CHECK(claim_notification(&later_batch) == 1);
    CHECK(later_batch.identity == requester && later_batch.generation == 2u);

    /* A join after retirement uses the target's earlier pre-free token. */
    CHECK(add_ticket(requester, target_old, &late_ticket) == 1);
    CHECK(finish_target(retained_old) == 2);
    CHECK(test_records[2].semaphore_pending == 1u &&
          test_records[2].semaphore_pending_generation == 1u &&
          test_records[2].semaphore_inflight == 1u &&
          test_records[2].semaphore_inflight_generation == 2u &&
          test_records[2].lifetime.notification_pins == 2u &&
          p4_task_records.semaphore_notification_serial == 2u);

    /* The later batch is still inflight, so this record cannot issue a
     * second concurrent notification snapshot yet. Its distinct old token
     * stays pending until the newer generation finishes. */
    CHECK(claim_notification(&old_batch) == 0);
    CHECK(old_batch.identity == NULL && old_batch.count == 0u &&
          old_batch.generation == 0u);
    CHECK(finish_notification(&later_batch) == 1);
    CHECK(claim_notification(&old_batch) == 1);
    CHECK(old_batch.identity == requester && old_batch.generation == 1u);
    forged = old_batch;
    forged.identity = wrong_identity;
    snapshot_state(&before);
    CHECK(finish_notification(&forged) == 0);
    check_unchanged(&before, "inflight generation without its identity");
    CHECK(finish_notification(&old_batch) == 1);
    CHECK(test_records[2].lifetime.notification_pins == 0u &&
          test_records[2].semaphore_pending == 0u &&
          test_records[2].semaphore_inflight == 0u);
}

int main(void)
{
    long page_size;
    void *page;
    unsigned int baseline_checks;
    unsigned int before_generation;

    if (p4_remove_generation_admission_baseline_main() != 0)
        return 1;
    baseline_checks = checks;
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

    before_generation = checks;
    test_plain_retirement_reservation(page);
    test_operation_retirement_reservation(page);
    test_operation_generation_survives_cleanup(page);
    test_reserved_generation_lifecycle(page);
    test_max_reserved_token_finishes(page);
    test_older_target_token_than_inflight(page);

    CHECK(munmap(page, (size_t)page_size) == 0);
    CHECK(!mock_gate_held && mock_irq_depth == 0u);
    CHECK(mock_fatal_calls == 0u);
    CHECK(mock_acquire_calls == mock_unlock_calls);
    CHECK(mock_disable_calls == mock_enable_calls);
    if (failures != 0u)
        return 1;
    printf("P4 pre-free removal generations: %u checks passed; "
           "admission baseline %u\n", checks - before_generation,
           baseline_checks);
    return 0;
}
