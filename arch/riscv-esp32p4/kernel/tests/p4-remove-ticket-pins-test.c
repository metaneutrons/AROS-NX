/* Actual-source tests for the Task-removal ticket pins binding. */
#define main p4_remove_ticket_pins_admission_baseline_main
#include "p4-semaphore-operation-admission-test.c"
#undef main

#include "../../exec/p4_remove_bank.c"
#include "../../exec/p4_remove_completion.c"

#define ADMIT_RECORD_CAPACITY 16u
#define ADMIT_BANK_CAPACITY 12u
#define ADMIT_LEDGER_OPS 4u
#define ADMIT_LEDGER_ENTRIES 8u

static struct p4_task_registry_record admit_records[ADMIT_RECORD_CAPACITY];
static struct p4_remove_bank admit_bank;
static struct p4_remove_bank_slot admit_slots[ADMIT_BANK_CAPACITY];
static struct p4_semaphore_ledger admit_ledger;
static struct p4_semaphore_ledger_operation admit_operations[ADMIT_LEDGER_OPS];
static struct p4_semaphore_ledger_entry admit_entries[ADMIT_LEDGER_ENTRIES];
static struct ExecBase admit_sysbase;

struct state_snapshot
{
    struct p4_task_registry registry;
    struct p4_task_registry_record records[ADMIT_RECORD_CAPACITY];
    struct p4_remove_bank bank;
    struct p4_remove_bank_slot slots[ADMIT_BANK_CAPACITY];
};

struct admit_snapshot
{
    struct state_snapshot shared;
    struct p4_semaphore_ledger ledger;
    struct p4_semaphore_ledger_operation operations[ADMIT_LEDGER_OPS];
    struct p4_semaphore_ledger_entry entries[ADMIT_LEDGER_ENTRIES];
};

static void gate_enter(void)
{
    p4_task_pins_lock(&admit_sysbase);
    CHECK(mock_gate_held && mock_irq_depth != 0u);
}

static void gate_leave(void)
{
    CHECK(mock_gate_held && mock_irq_depth != 0u);
    p4_task_pins_unlock(&admit_sysbase);
    CHECK(!mock_gate_held);
}

static void snapshot_state(struct state_snapshot *snapshot)
{
    gate_enter();
    memcpy(&snapshot->registry, &p4_task_records, sizeof(snapshot->registry));
    memcpy(snapshot->records, admit_records, sizeof(admit_records));
    memcpy(&snapshot->bank, &admit_bank, sizeof(admit_bank));
    memcpy(snapshot->slots, admit_slots, sizeof(admit_slots));
    gate_leave();
}

static void admission_reset(void)
{
    memset(admit_records, 0, sizeof(admit_records));
    memset(admit_slots, 0, sizeof(admit_slots));
    memset(&admit_bank, 0, sizeof(admit_bank));
    memset(&admit_ledger, 0, sizeof(admit_ledger));
    memset(admit_operations, 0, sizeof(admit_operations));
    memset(admit_entries, 0, sizeof(admit_entries));
    memset(&admit_sysbase, 0, sizeof(admit_sysbase));
    p4_task_registry_init(&p4_task_records);
    CHECK(p4_remove_bank_init(&admit_bank, admit_slots,
                              ADMIT_BANK_CAPACITY) == 1);
    CHECK(p4_semaphore_ledger_init(&admit_ledger, admit_operations,
              ADMIT_LEDGER_OPS, admit_entries, ADMIT_LEDGER_ENTRIES) == 1);
    mock_irq_depth = 0u;
    mock_gate_held = 0;
    mock_gate_address = &p4_task_pin_gate;
    mock_outer_semaphore_gate_held = 0;
    mock_task_lock_held = 0;
}

static int add_registered(unsigned int index, struct Task *identity)
{
    int result;
    CHECK(index < ADMIT_RECORD_CAPACITY);
    gate_enter();
    result = p4_task_registry_register_locked(&p4_task_records,
        &admit_records[index], identity);
    gate_leave();
    return result;
}

static int add_running(unsigned int index, struct Task *identity,
                       uint32_t hart)
{
    int result = add_registered(index, identity);
    if (result != 1)
        return result;
    gate_enter();
    result = p4_task_registry_enter_running_locked(&p4_task_records,
        identity, hart);
    gate_leave();
    return result;
}

static struct p4_task_registry_record *lookup_record(struct Task *identity)
{
    struct p4_task_registry_record *record;
    gate_enter();
    record = p4_task_registry_lookup_locked(&p4_task_records, identity);
    gate_leave();
    return record;
}

static void ticket_pins_reset(void)
{
    admission_reset();
    test_boot_ledger = &admit_ledger;

    if (!p4_task_pins_prepared)
        CHECK(krnP4TaskPinsPrepare() == 1);
    CHECK(p4_task_pins_prepared == 1);
    if (p4_removal_bank == NULL)
        CHECK(krnP4TaskPinsAttachRemovalBank(&admit_bank) == 1);
    else
        CHECK(p4_removal_bank == &admit_bank);

    /* The included admission packet leaves the global pin binding prepared.
     * This reset is test-only and runs with no active callers or workers. */
    p4_cleanup_worker = NULL;
    p4_cleanup_wake_mask = 0u;
    p4_cleanup_wake_pending = false;
    memset(&p4_cancel_retry, 0, sizeof(p4_cancel_retry));
    p4_removal_finish_waiting = false;
    p4_postfree_after = 0u;
    p4_postfree_blocked = false;
    p4_postfree_restart = false;

    mock_irq_depth = 0u;
    mock_disable_calls = 0u;
    mock_enable_calls = 0u;
    mock_acquire_calls = 0u;
    mock_unlock_calls = 0u;
    mock_fatal_calls = 0u;
    mock_poke_calls = 0u;
    mock_retired_poke_calls = 0u;
    mock_gate_held = 0;
    mock_gate_address = &p4_task_pin_gate;
    mock_outer_semaphore_gate_held = 0;
    mock_task_lock_held = 0;
}

static int ticket_pins_admit_from_locked_caller(
    struct Task *requester, struct Task *target, uint32_t hart,
    uint32_t retire_live, struct p4_remove_handle *handle)
{
    int result;
    Disable();
    mock_task_lock_held = 1;
    result = krnP4TaskRecordAdmitRemoval(requester, target, hart,
        retire_live, handle, &admit_sysbase);
    CHECK(mock_task_lock_held && mock_irq_depth == 1u && !mock_gate_held);
    mock_task_lock_held = 0;
    Enable();
    return result;
}

static int ticket_pins_retire_current(struct Task *identity)
{
    int result;
    Disable();
    mock_task_lock_held = 1;
    result = krnP4TaskRecordRetireForService(identity, &admit_sysbase);
    CHECK(mock_task_lock_held && mock_irq_depth == 1u && !mock_gate_held);
    mock_task_lock_held = 0;
    Enable();
    return result;
}

static void ticket_pins_capture(struct admit_snapshot *snapshot)
{
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot_state(&snapshot->shared);
    memcpy(&snapshot->ledger, &admit_ledger, sizeof(admit_ledger));
    memcpy(snapshot->operations, admit_operations, sizeof(admit_operations));
    memcpy(snapshot->entries, admit_entries, sizeof(admit_entries));
}

static int ticket_pins_snapshot_unchanged(
    const struct admit_snapshot *snapshot)
{
    struct admit_snapshot current;
    ticket_pins_capture(&current);
    return memcmp(&current, snapshot, sizeof(current)) == 0;
}

static struct p4_task_registry_record *ticket_pins_claim_service(
    struct Task *identity)
{
    struct p4_task_registry_record *record =
        krnP4TaskRecordClaimNextRetainedService(&admit_sysbase);
    CHECK(record != NULL);
    if (record != NULL)
        CHECK(record->identity == identity && record->service_inflight == 1u);
    return record;
}

static void ticket_pins_finish_service(struct Task *identity)
{
    struct p4_task_registry_record *record =
        ticket_pins_claim_service(identity);
    if (record == NULL)
        return;
    krnP4TaskRecordFinishRetainedService(record, &admit_sysbase);
    CHECK(lookup_record(identity) == NULL);
}

static void ticket_pins_expect_quiescent(void)
{
    CHECK(!mock_gate_held && mock_irq_depth == 0u);
    CHECK(!mock_task_lock_held && !mock_outer_semaphore_gate_held);
    CHECK(mock_acquire_calls == mock_unlock_calls);
    CHECK(mock_disable_calls == mock_enable_calls);
    CHECK(mock_fatal_calls == 0u);
}

static void test_live_ticket_and_drop(void *page)
{
    struct Task *requester = guarded_identity(page, 64u);
    struct Task *target = guarded_identity(page, 96u);
    struct p4_remove_handle handle = { UINT32_C(0xa5a5a5a5),
                                       UINT32_C(0x5a5a5a5a) };
    struct p4_task_registry_record *target_record;
    unsigned int pokes_before;
    int result;

    ticket_pins_reset();
    CHECK(add_running(0u, requester, 0u) == 1);
    CHECK(add_running(1u, target, 1u) == 1);
    CHECK(krnP4SignalTaskPin(target, &admit_sysbase) == 1);
    target_record = lookup_record(target);
    CHECK(target_record != NULL &&
          target_record->lifetime.notification_pins == 1u);

    result = ticket_pins_admit_from_locked_caller(requester, target, 0u,
        1u, &handle);
    CHECK(result == 1 && handle.index == 0u && handle.generation == 1u);
    CHECK(mock_poke_calls == 0u && p4_cleanup_wake_pending);
    CHECK(target_record->service_pending == 1u &&
          target_record->service_generation == 1u &&
          target_record->lifetime.notification_pins == 2u);
    CHECK(admit_slots[handle.index].active == 1u &&
          admit_slots[handle.index].completion.requester_ref == 1u &&
          admit_slots[handle.index].completion.service_ref == 1u);
    CHECK(krnP4TaskRemovalPoll(requester, 0u, handle,
                              &admit_sysbase) == 0);

    /* Release the retired target's RUN and ordinary admission pin before
     * the protected-service claim. This leaves only its durable service pin. */
    krnP4TaskRecordLeaveRunning(target, 1u, &admit_sysbase);
    krnP4SignalTaskUnpin(target, &admit_sysbase);
    CHECK(mock_poke_calls == 1u && !mock_gate_held);
    CHECK(target_record->service_queued == 1u &&
          target_record->lifetime.notification_pins == 1u);
    ticket_pins_finish_service(target);

    CHECK(krnP4TaskRemovalPoll(requester, 0u, handle,
                              &admit_sysbase) == 1);
    CHECK(admit_records[0].semaphore_pending == 1u &&
          admit_records[0].lifetime.notification_pins == 1u);

    /* Model an already-blocked post-free retry owner. Drop must reserve its
     * new wake under the gate, reclaim the completed slot, then poke outside. */
    gate_enter();
    p4_removal_finish_waiting = true;
    p4_cleanup_wake_pending = false;
    gate_leave();
    pokes_before = mock_poke_calls;
    CHECK(krnP4TaskRemovalDrop(requester, 0u, handle,
                               &admit_sysbase) == 1);
    CHECK(mock_poke_calls == pokes_before + 1u && !mock_gate_held);
    CHECK(p4_cleanup_wake_pending && admit_slots[handle.index].active == 0u &&
          admit_slots[handle.index].requester_identity == 0u &&
          admit_slots[handle.index].target_identity == 0u);
    CHECK(lookup_record(target) == NULL);
    ticket_pins_expect_quiescent();
}

static void test_retired_target_late_join(void *page)
{
    struct Task *requester = guarded_identity(page, 64u);
    struct Task *target = guarded_identity(page, 96u);
    struct p4_task_registry_record *target_record;
    struct p4_remove_handle handle = { UINT32_MAX, UINT32_MAX };

    ticket_pins_reset();
    CHECK(add_running(0u, requester, 0u) == 1);
    CHECK(add_registered(1u, target) == 1);
    CHECK(krnP4TaskRecordRetireForService(target, &admit_sysbase) == 1);
    target_record = lookup_record(target);
    CHECK(target_record != NULL && target_record->service_pending == 1u &&
          target_record->lifetime.notification_pins == 1u);

    CHECK(ticket_pins_admit_from_locked_caller(requester, target, 0u,
        0u, &handle) == 2);
    CHECK(handle.index == 0u && handle.generation == 1u);
    CHECK(target_record->lifetime.notification_pins == 1u &&
          target_record->service_pending == 1u);
    CHECK(krnP4TaskRemovalPoll(requester, 0u, handle,
                              &admit_sysbase) == 0);
    ticket_pins_finish_service(target);
    CHECK(krnP4TaskRemovalPoll(requester, 0u, handle,
                              &admit_sysbase) == 1);
    CHECK(krnP4TaskRemovalDrop(requester, 0u, handle,
                               &admit_sysbase) == 1);
    CHECK(admit_slots[handle.index].active == 0u);
    CHECK(lookup_record(target) == NULL);
    ticket_pins_expect_quiescent();
}

static void test_requester_retired_while_running(void *page)
{
    struct Task *requester = guarded_identity(page, 64u);
    struct Task *target = guarded_identity(page, 96u);
    struct p4_task_registry_record *target_record;
    struct p4_task_registry_record *requester_record;
    struct p4_remove_handle handle = { UINT32_MAX, UINT32_MAX };
    struct p4_remove_bank_slot before_slots[ADMIT_BANK_CAPACITY];
    unsigned int retired_pokes;

    ticket_pins_reset();
    CHECK(add_running(0u, requester, 0u) == 1);
    CHECK(add_running(1u, target, 1u) == 1);
    CHECK(krnP4SignalTaskPin(target, &admit_sysbase) == 1);
    CHECK(ticket_pins_admit_from_locked_caller(requester, target, 0u,
        1u, &handle) == 1);
    target_record = lookup_record(target);
    requester_record = lookup_record(requester);
    CHECK(target_record != NULL && requester_record != NULL);

    retired_pokes = mock_retired_poke_calls;
    CHECK(ticket_pins_retire_current(requester) == 1);
    CHECK(mock_retired_poke_calls == retired_pokes + 1u && !mock_gate_held);
    CHECK(admit_slots[handle.index].active == 1u &&
          admit_slots[handle.index].completion.requester_ref == 0u &&
          admit_slots[handle.index].completion.service_ref == 1u);
    memcpy(before_slots, admit_slots, sizeof(before_slots));
    CHECK(krnP4TaskRemovalPoll(requester, 0u, handle,
                              &admit_sysbase) == 2);
    CHECK(memcmp(before_slots, admit_slots, sizeof(before_slots)) == 0);
    CHECK(krnP4TaskRemovalDrop(requester, 0u, handle,
                               &admit_sysbase) == 0);
    CHECK(memcmp(before_slots, admit_slots, sizeof(before_slots)) == 0);
    CHECK(requester_record->lifetime.running_harts == 1u &&
          requester_record->lifetime.flags == P4_TASK_LIFETIME_RETIRED);

    /* The target's completion sees the cancelled requester reference and
     * reclaims the bank slot without creating a notification for retired A. */
    krnP4TaskRecordLeaveRunning(target, 1u, &admit_sysbase);
    krnP4SignalTaskUnpin(target, &admit_sysbase);
    ticket_pins_finish_service(target);
    CHECK(admit_slots[handle.index].active == 0u);
    CHECK(lookup_record(target) == NULL);

    /* Only after the tested RUN-held poll/drop do we drain the requester. */
    krnP4TaskRecordLeaveRunning(requester, 0u, &admit_sysbase);
    ticket_pins_finish_service(requester);
    CHECK(lookup_record(requester) == NULL);
    ticket_pins_expect_quiescent();
}

static void test_hart_migration_requeries_owner(void *page)
{
    struct Task *requester = guarded_identity(page, 64u);
    struct Task *target = guarded_identity(page, 96u);
    struct p4_remove_handle handle = { UINT32_MAX, UINT32_MAX };
    unsigned int pokes_before;

    ticket_pins_reset();
    CHECK(add_running(0u, requester, 0u) == 1);
    CHECK(add_running(1u, target, 1u) == 1);
    CHECK(krnP4SignalTaskPin(target, &admit_sysbase) == 1);
    CHECK(ticket_pins_admit_from_locked_caller(requester, target, 0u,
        1u, &handle) == 1);
    CHECK(krnP4TaskRemovalPoll(requester, 0u, handle,
                              &admit_sysbase) == 0);

    /* Vacate hart 1 before the requester migrates there. The public binding
     * must validate the current RUN owner anew on every call. */
    krnP4TaskRecordLeaveRunning(target, 1u, &admit_sysbase);
    krnP4TaskRecordLeaveRunning(requester, 0u, &admit_sysbase);
    CHECK(krnP4TaskRecordEnterRunning(requester, 1u,
                                      &admit_sysbase) == 1);
    CHECK(krnP4TaskRemovalPoll(requester, 1u, handle,
                              &admit_sysbase) == 0);

    krnP4SignalTaskUnpin(target, &admit_sysbase);
    ticket_pins_finish_service(target);
    CHECK(krnP4TaskRemovalPoll(requester, 1u, handle,
                              &admit_sysbase) == 1);
    gate_enter();
    p4_removal_finish_waiting = true;
    p4_cleanup_wake_pending = false;
    gate_leave();
    pokes_before = mock_poke_calls;
    CHECK(krnP4TaskRemovalDrop(requester, 1u, handle,
                               &admit_sysbase) == 1);
    CHECK(mock_poke_calls == pokes_before + 1u && !mock_gate_held);
    CHECK(admit_slots[handle.index].active == 0u &&
          p4_cleanup_wake_pending);
    ticket_pins_expect_quiescent();
}

static void test_refusal_is_quiet_and_unchanged(void *page)
{
    struct Task *requester = guarded_identity(page, 64u);
    struct Task *target = guarded_identity(page, 96u);
    struct p4_remove_handle output = { UINT32_C(0x11223344),
                                       UINT32_C(0x55667788) };
    struct p4_remove_handle before_output = output;
    struct admit_snapshot before;
    unsigned int pokes_before;

    ticket_pins_reset();
    CHECK(add_running(0u, requester, 0u) == 1);
    CHECK(add_running(1u, target, 1u) == 1);
    ticket_pins_capture(&before);
    pokes_before = mock_poke_calls;
    CHECK(!p4_cleanup_wake_pending);
    CHECK(ticket_pins_admit_from_locked_caller(requester, target, 0u,
        1u, &output) == 0);
    CHECK(ticket_pins_snapshot_unchanged(&before));
    CHECK(memcmp(&output, &before_output, sizeof(output)) == 0);
    CHECK(!p4_cleanup_wake_pending && mock_poke_calls == pokes_before);
    CHECK(admit_slots[0].active == 0u &&
          admit_records[1].lifetime.flags == 0u &&
          admit_records[1].lifetime.notification_pins == 0u);
    ticket_pins_expect_quiescent();
}

int main(void)
{
    long page_size;
    void *page;
    unsigned int baseline_checks;
    unsigned int packet_start;

    if (p4_remove_ticket_pins_admission_baseline_main() != 0)
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

    packet_start = checks;
    test_live_ticket_and_drop(page);
    test_retired_target_late_join(page);
    test_requester_retired_while_running(page);
    test_hart_migration_requeries_owner(page);
    test_refusal_is_quiet_and_unchanged(page);

    CHECK(munmap(page, (size_t)page_size) == 0);
    ticket_pins_expect_quiescent();
    if (failures != 0u)
        return 1;
    printf("P4 removal-ticket pins: %u checks passed; baseline %u\n",
           checks - packet_start, baseline_checks);
    return 0;
}
