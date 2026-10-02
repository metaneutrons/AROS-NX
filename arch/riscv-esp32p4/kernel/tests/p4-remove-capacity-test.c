/* Actual-source tests for canceled removal-ticket slot capacity/reuse. */
#define main p4_remove_capacity_operation_baseline_main
#include "p4-semaphore-operation-admission-test.c"
#undef main

#include "../../exec/p4_remove_bank.c"
#include "../../exec/p4_remove_completion.c"

#define CAP_RECORD_CAPACITY 12u
#define CAP_BANK_CAPACITY P4_REMOVE_BANK_MAX_SLOTS
#define CAP_LEDGER_OPS 4u
#define CAP_LEDGER_ENTRIES 8u

static struct p4_task_registry_record cap_records[CAP_RECORD_CAPACITY];
static struct p4_remove_bank cap_bank;
static struct p4_remove_bank_slot cap_slots[CAP_BANK_CAPACITY];
static struct p4_semaphore_ledger cap_ledger;
static struct p4_semaphore_ledger_operation cap_operations[CAP_LEDGER_OPS];
static struct p4_semaphore_ledger_entry cap_entries[CAP_LEDGER_ENTRIES];
static struct ExecBase cap_sysbase;

struct cap_snapshot
{
    struct p4_task_registry registry;
    struct p4_task_registry_record records[CAP_RECORD_CAPACITY];
    struct p4_remove_bank bank;
    struct p4_semaphore_ledger ledger;
    struct p4_semaphore_ledger_operation operations[CAP_LEDGER_OPS];
    struct p4_semaphore_ledger_entry entries[CAP_LEDGER_ENTRIES];
};

static struct cap_snapshot cap_before;
static struct cap_snapshot cap_current;
static struct p4_remove_bank_slot cap_before_slots[CAP_BANK_CAPACITY];
static struct p4_remove_bank_slot cap_current_slots[CAP_BANK_CAPACITY];

static void cap_gate_enter(void)
{
    p4_task_pins_lock(&cap_sysbase);
    CHECK(mock_gate_held && mock_irq_depth != 0u);
}

static void cap_gate_leave(void)
{
    CHECK(mock_gate_held && mock_irq_depth != 0u);
    p4_task_pins_unlock(&cap_sysbase);
    CHECK(!mock_gate_held);
}

static void cap_reset(uint32_t bank_capacity)
{
    memset(cap_records, 0, sizeof(cap_records));
    memset(cap_slots, 0, sizeof(cap_slots));
    memset(&cap_bank, 0, sizeof(cap_bank));
    memset(&cap_ledger, 0, sizeof(cap_ledger));
    memset(cap_operations, 0, sizeof(cap_operations));
    memset(cap_entries, 0, sizeof(cap_entries));
    memset(&cap_sysbase, 0, sizeof(cap_sysbase));
    p4_task_registry_init(&p4_task_records);
    CHECK(p4_remove_bank_init(&cap_bank, cap_slots, bank_capacity) == 1);
    CHECK(p4_semaphore_ledger_init(&cap_ledger, cap_operations,
        CAP_LEDGER_OPS, cap_entries, CAP_LEDGER_ENTRIES) == 1);
    test_boot_ledger = &cap_ledger;
    if (!p4_task_pins_prepared)
        CHECK(krnP4TaskPinsPrepare() == 1);
    CHECK(p4_task_pins_prepared == 1);
    if (p4_removal_bank == NULL)
        CHECK(krnP4TaskPinsAttachRemovalBank(&cap_bank) == 1);
    else
        CHECK(p4_removal_bank == &cap_bank);

    /* Reset only in a quiescent single-threaded test setup. */
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

static int cap_add_registered(unsigned int index, struct Task *identity)
{
    int result;
    CHECK(index < CAP_RECORD_CAPACITY);
    cap_gate_enter();
    result = p4_task_registry_register_locked(&p4_task_records,
        &cap_records[index], identity);
    cap_gate_leave();
    return result;
}

static int cap_add_running(unsigned int index, struct Task *identity,
                           uint32_t hart)
{
    int result = cap_add_registered(index, identity);
    if (result != 1)
        return result;
    cap_gate_enter();
    result = p4_task_registry_enter_running_locked(&p4_task_records,
        identity, hart);
    cap_gate_leave();
    return result;
}

static struct p4_task_registry_record *cap_lookup(struct Task *identity)
{
    struct p4_task_registry_record *record;
    cap_gate_enter();
    record = p4_task_registry_lookup_locked(&p4_task_records, identity);
    cap_gate_leave();
    return record;
}

static int cap_admit(struct Task *requester, struct Task *target,
                     uint32_t hart, uint32_t retire_live,
                     struct p4_remove_handle *handle)
{
    int result;
    Disable();
    mock_task_lock_held = 1;
    result = krnP4TaskRecordAdmitRemoval(requester, target, hart,
        retire_live, handle, &cap_sysbase);
    CHECK(mock_task_lock_held && mock_irq_depth == 1u && !mock_gate_held);
    mock_task_lock_held = 0;
    Enable();
    return result;
}

static int cap_retire_current(struct Task *identity)
{
    int result;
    Disable();
    mock_task_lock_held = 1;
    result = krnP4TaskRecordRetireForService(identity, &cap_sysbase);
    CHECK(mock_task_lock_held && mock_irq_depth == 1u && !mock_gate_held);
    mock_task_lock_held = 0;
    Enable();
    return result;
}

static struct p4_task_registry_record *cap_claim_service(
    struct Task *identity)
{
    struct p4_task_registry_record *record =
        krnP4TaskRecordClaimNextRetainedService(&cap_sysbase);
    CHECK(record != NULL);
    if (record != NULL)
        CHECK(record->identity == identity && record->service_inflight == 1u);
    return record;
}

static void cap_finish_service(struct Task *identity)
{
    struct p4_task_registry_record *record = cap_claim_service(identity);
    if (record == NULL)
        return;
    krnP4TaskRecordFinishRetainedService(record, &cap_sysbase);
    CHECK(cap_lookup(identity) == NULL);
}

static void cap_snapshot_take(struct cap_snapshot *snapshot,
                              struct p4_remove_bank_slot *slots)
{
    memset(snapshot, 0, sizeof(*snapshot));
    memset(slots, 0, sizeof(cap_slots));
    cap_gate_enter();
    memcpy(&snapshot->registry, &p4_task_records,
           sizeof(snapshot->registry));
    memcpy(snapshot->records, cap_records, sizeof(cap_records));
    memcpy(&snapshot->bank, &cap_bank, sizeof(cap_bank));
    memcpy(&snapshot->ledger, &cap_ledger, sizeof(cap_ledger));
    memcpy(snapshot->operations, cap_operations, sizeof(cap_operations));
    memcpy(snapshot->entries, cap_entries, sizeof(cap_entries));
    memcpy(slots, cap_slots, sizeof(cap_slots));
    cap_gate_leave();
}

static void cap_save_state(void)
{
    cap_snapshot_take(&cap_before, cap_before_slots);
}

static int cap_state_unchanged(void)
{
    cap_snapshot_take(&cap_current, cap_current_slots);
    return memcmp(&cap_current, &cap_before, sizeof(cap_current)) == 0 &&
           memcmp(cap_current_slots, cap_before_slots,
                  sizeof(cap_current_slots)) == 0;
}

static int cap_ready_query(struct p4_remove_handle handle,
                           struct Task *requester, int *ready)
{
    int result;
    cap_gate_enter();
    result = p4_remove_bank_ready(&cap_bank, handle,
        (uintptr_t)requester, ready);
    cap_gate_leave();
    return result;
}

static void cap_expect_quiescent(void)
{
    CHECK(!mock_gate_held && mock_irq_depth == 0u);
    CHECK(!mock_task_lock_held && !mock_outer_semaphore_gate_held);
    CHECK(mock_acquire_calls == mock_unlock_calls);
    CHECK(mock_disable_calls == mock_enable_calls);
    CHECK(mock_fatal_calls == 0u);
}

static void test_duplicate_pending_and_acked_refusals(void *page)
{
    struct Task *requester = guarded_identity(page, 64u);
    struct Task *target_a = guarded_identity(page, 96u);
    struct Task *target_b = guarded_identity(page, 128u);
    struct p4_remove_handle handle_a = { UINT32_MAX, UINT32_MAX };
    struct p4_remove_handle duplicate = { UINT32_C(0x11223344),
                                          UINT32_C(0x55667788) };
    struct p4_remove_handle before_duplicate = duplicate;
    int ready = -1;

    cap_reset(8u);
    CHECK(cap_add_running(0u, requester, 0u) == 1);
    CHECK(cap_add_registered(1u, target_a) == 1);
    CHECK(cap_add_registered(2u, target_b) == 1);
    CHECK(krnP4SignalTaskPin(target_a, &cap_sysbase) == 1);
    CHECK(krnP4SignalTaskPin(target_b, &cap_sysbase) == 1);

    CHECK(cap_admit(requester, target_a, 0u, 1u, &handle_a) == 1);
    cap_save_state();
    CHECK(cap_admit(requester, target_b, 0u, 1u, &duplicate) == 0);
    CHECK(cap_state_unchanged());
    CHECK(memcmp(&duplicate, &before_duplicate, sizeof(duplicate)) == 0);
    CHECK(cap_records[2].lifetime.flags == 0u &&
          cap_records[2].lifetime.notification_pins == 1u);

    krnP4SignalTaskUnpin(target_a, &cap_sysbase);
    cap_finish_service(target_a);
    CHECK(cap_ready_query(handle_a, requester, &ready) == 1 && ready == 1);

    duplicate = before_duplicate;
    cap_save_state();
    CHECK(cap_admit(requester, target_b, 0u, 1u, &duplicate) == 0);
    CHECK(cap_state_unchanged());
    CHECK(memcmp(&duplicate, &before_duplicate, sizeof(duplicate)) == 0);
    CHECK(cap_records[2].lifetime.flags == 0u &&
          cap_records[2].lifetime.notification_pins == 1u);
    CHECK(cap_ready_query(handle_a, requester, &ready) == 1 && ready == 1);
    CHECK(krnP4TaskRemovalDrop(requester, 0u, handle_a,
                              &cap_sysbase) == 1);
    cap_expect_quiescent();
}

static void cap_fill_orphans_after_first(uint32_t capacity,
    struct p4_remove_bank_slot *slots, uint32_t *serial)
{
    uint32_t index;
    for (index = 1u; index < capacity; ++index)
    {
        slots[index].requester_identity = (uintptr_t)0x100000u + index;
        slots[index].target_identity = (uintptr_t)0x200000u + index;
        slots[index].generation = index + 1u;
        slots[index].active = 1u;
        slots[index].completion.requester_ref = 0u;
        slots[index].completion.service_ref = 1u;
        slots[index].completion.completed = 0u;
    }
    *serial = capacity;
}

static void test_full_orphan_pool_reuses_without_releasing_target(void *page)
{
    struct Task *old_requester = guarded_identity(page, 64u);
    struct Task *old_target = guarded_identity(page, 96u);
    struct Task *new_requester = guarded_identity(page, 128u);
    struct Task *new_target = guarded_identity(page, 160u);
    struct p4_task_registry_record *old_target_record;
    struct p4_task_registry_record *old_requester_record;
    struct p4_task_registry_record *record;
    struct p4_remove_handle old_handle = { UINT32_MAX, UINT32_MAX };
    struct p4_remove_handle new_handle = { UINT32_MAX, UINT32_MAX };
    int ready = -1;

    cap_reset(CAP_BANK_CAPACITY);
    CHECK(cap_add_running(0u, old_requester, 0u) == 1);
    CHECK(cap_add_running(1u, old_target, 1u) == 1);
    CHECK(krnP4SignalTaskPin(old_target, &cap_sysbase) == 1);
    CHECK(cap_admit(old_requester, old_target, 0u, 1u, &old_handle) == 1);
    CHECK(old_handle.index == 0u && old_handle.generation == 1u);

    /* Keep an ordinary pin on the soon-retired requester so its own cleanup
     * cannot be claimed before this packet observes the old target. */
    CHECK(krnP4SignalTaskPin(old_requester, &cap_sysbase) == 1);
    CHECK(cap_retire_current(old_requester) == 1);
    old_requester_record = cap_lookup(old_requester);
    old_target_record = cap_lookup(old_target);
    CHECK(old_requester_record != NULL &&
          old_requester_record->lifetime.notification_pins == 2u &&
          old_requester_record->lifetime.running_harts == 1u);
    CHECK(old_target_record != NULL &&
          old_target_record->service_pending == 1u &&
          old_target_record->lifetime.notification_pins == 2u);
    CHECK(cap_slots[0].active == 1u &&
          cap_slots[0].completion.requester_ref == 0u &&
          cap_slots[0].completion.service_ref == 1u);

    krnP4TaskRecordLeaveRunning(old_target, 1u, &cap_sysbase);
    krnP4TaskRecordLeaveRunning(old_requester, 0u, &cap_sysbase);
    CHECK(old_target_record->lifetime.notification_pins == 2u &&
          old_target_record->service_queued == 0u);
    CHECK(old_requester_record->lifetime.notification_pins == 2u &&
          old_requester_record->service_queued == 0u);
    CHECK(cap_add_running(2u, new_requester, 0u) == 1);
    CHECK(cap_add_running(3u, new_target, 1u) == 1);
    CHECK(krnP4SignalTaskPin(new_target, &cap_sysbase) == 1);

    cap_gate_enter();
    cap_fill_orphans_after_first(CAP_BANK_CAPACITY, cap_slots,
                                 &cap_bank.serial);
    cap_gate_leave();
    CHECK(cap_bank.serial == CAP_BANK_CAPACITY);

    CHECK(cap_admit(new_requester, new_target, 0u, 1u, &new_handle) == 1);
    CHECK(new_handle.index == 0u &&
          new_handle.generation == CAP_BANK_CAPACITY + 1u &&
          cap_bank.serial == CAP_BANK_CAPACITY + 1u);
    CHECK(old_target_record->service_pending == 1u &&
          old_target_record->lifetime.notification_pins == 2u &&
          cap_lookup(old_target) == old_target_record);
    CHECK(old_requester_record->service_pending == 1u &&
          old_requester_record->lifetime.notification_pins == 2u &&
          cap_lookup(old_requester) == old_requester_record);

    ready = 77;
    CHECK(cap_ready_query(old_handle, old_requester, &ready) == 0 &&
          ready == 77);
    CHECK(cap_ready_query(new_handle, new_requester, &ready) == 1 &&
          ready == 0);

    /* Reusing the orphan slot did not consume old_target's independent
     * registry service reservation. It can still be claimed and completed. */
    krnP4SignalTaskUnpin(old_target, &cap_sysbase);
    CHECK(old_target_record->service_queued == 1u &&
          old_target_record->lifetime.notification_pins == 1u);
    record = cap_claim_service(old_target);
    CHECK(record == old_target_record &&
          cap_lookup(old_target) == old_target_record);
    krnP4TaskRecordFinishRetainedService(record, &cap_sysbase);
    CHECK(cap_lookup(old_target) == NULL);

    krnP4SignalTaskUnpin(old_requester, &cap_sysbase);
    CHECK(old_requester_record->service_queued == 1u &&
          old_requester_record->lifetime.notification_pins == 1u);
    cap_finish_service(old_requester);

    krnP4TaskRecordLeaveRunning(new_target, 1u, &cap_sysbase);
    krnP4SignalTaskUnpin(new_target, &cap_sysbase);
    cap_finish_service(new_target);
    CHECK(cap_ready_query(new_handle, new_requester, &ready) == 1 &&
          ready == 1);
    CHECK(krnP4TaskRemovalPoll(new_requester, 0u, new_handle,
                              &cap_sysbase) == 1);
    CHECK(krnP4TaskRemovalDrop(new_requester, 0u, new_handle,
                              &cap_sysbase) == 1);
    CHECK(cap_slots[0].active == 0u &&
          cap_slots[0].generation == CAP_BANK_CAPACITY + 1u);
    ready = 88;
    CHECK(cap_ready_query(old_handle, old_requester, &ready) == 0 &&
          ready == 88);
    cap_expect_quiescent();
}

static void test_mixed_live_interests_and_orphans(void *page)
{
    struct Task *requester_a = guarded_identity(page, 64u);
    struct Task *requester_b = guarded_identity(page, 96u);
    struct Task *requester_c = guarded_identity(page, 128u);
    struct Task *target_a = guarded_identity(page, 160u);
    struct Task *target_b = guarded_identity(page, 192u);
    struct Task *target_c = guarded_identity(page, 224u);
    struct p4_task_registry_record *old_target_record;
    struct p4_remove_handle handle_a = { UINT32_MAX, UINT32_MAX };
    struct p4_remove_handle handle_b = { UINT32_MAX, UINT32_MAX };
    struct p4_remove_handle handle_c = { UINT32_MAX, UINT32_MAX };

    cap_reset(3u);
    CHECK(cap_add_running(0u, requester_a, 0u) == 1);
    CHECK(cap_add_running(1u, requester_b, 1u) == 1);
    CHECK(cap_add_registered(2u, target_a) == 1);
    CHECK(cap_add_registered(3u, target_b) == 1);
    CHECK(krnP4SignalTaskPin(target_a, &cap_sysbase) == 1);
    CHECK(krnP4SignalTaskPin(target_b, &cap_sysbase) == 1);
    CHECK(cap_admit(requester_a, target_a, 0u, 1u, &handle_a) == 1);
    CHECK(cap_admit(requester_b, target_b, 1u, 1u, &handle_b) == 1);
    CHECK(handle_a.index == 0u && handle_b.index == 1u);

    CHECK(krnP4SignalTaskPin(requester_a, &cap_sysbase) == 1);
    CHECK(cap_retire_current(requester_a) == 1);
    old_target_record = cap_lookup(target_a);
    CHECK(old_target_record != NULL);
    CHECK(cap_slots[0].completion.requester_ref == 0u &&
          cap_slots[0].completion.service_ref == 1u);
    CHECK(cap_slots[1].completion.requester_ref == 1u &&
          cap_slots[1].completion.service_ref == 1u);
    krnP4TaskRecordLeaveRunning(requester_a, 0u, &cap_sysbase);
    CHECK(cap_add_running(4u, requester_c, 0u) == 1);
    CHECK(cap_add_registered(5u, target_c) == 1);
    CHECK(krnP4SignalTaskPin(target_c, &cap_sysbase) == 1);

    CHECK(cap_admit(requester_c, target_c, 0u, 1u, &handle_c) == 1);
    CHECK(handle_c.index == handle_a.index &&
          handle_c.generation == 3u);
    CHECK(cap_slots[0].requester_identity == (uintptr_t)requester_c &&
          cap_slots[0].target_identity == (uintptr_t)target_c &&
          cap_slots[0].completion.requester_ref == 1u &&
          cap_slots[0].completion.service_ref == 1u);
    CHECK(cap_slots[1].active == 1u &&
          cap_slots[1].requester_identity == (uintptr_t)requester_b &&
          cap_slots[1].target_identity == (uintptr_t)target_b &&
          cap_slots[1].completion.requester_ref == 1u &&
          cap_slots[1].completion.service_ref == 1u);
    CHECK(old_target_record->service_pending == 1u &&
          old_target_record->lifetime.notification_pins == 2u);
    cap_expect_quiescent();
}

static void test_failed_last_retirement_preserves_orphan(void *page)
{
    struct Task *requester = guarded_identity(page, 64u);
    struct Task *target = guarded_identity(page, 96u);
    struct Task *fake_requester = guarded_identity(page, 128u);
    struct Task *fake_target = guarded_identity(page, 160u);
    struct p4_remove_handle orphan_handle = { UINT32_MAX, UINT32_MAX };
    struct p4_remove_handle output = { UINT32_C(0xa5a5a5a5),
                                       UINT32_C(0x5a5a5a5a) };
    struct p4_remove_handle before_output = output;
    struct p4_task_registry_record *target_record;
    uint32_t dropped = UINT32_MAX;

    cap_reset(4u);
    CHECK(cap_add_running(0u, requester, 0u) == 1);
    CHECK(cap_add_running(1u, target, 1u) == 1);
    CHECK(krnP4SignalTaskPin(target, &cap_sysbase) == 1);
    cap_gate_enter();
    CHECK(p4_remove_bank_reserve(&cap_bank, (uintptr_t)fake_requester,
        (uintptr_t)fake_target, &orphan_handle) == 1);
    CHECK(p4_remove_bank_cancel_requester(&cap_bank,
        (uintptr_t)fake_requester, &dropped) == 1 && dropped == 1u);
    /* The bank preflight can select this orphan, but the final service
     * retirement must refuse at exhausted generation capacity. */
    p4_task_records.semaphore_notification_serial = UINT32_MAX;
    cap_gate_leave();
    target_record = cap_lookup(target);
    CHECK(target_record != NULL && target_record->lifetime.flags == 0u &&
          target_record->lifetime.notification_pins == 1u &&
          target_record->service_pending == 0u);

    cap_save_state();
    CHECK(cap_admit(requester, target, 0u, 1u, &output) == 0);
    CHECK(cap_state_unchanged());
    CHECK(memcmp(&output, &before_output, sizeof(output)) == 0);
    CHECK(target_record->lifetime.flags == 0u &&
          target_record->lifetime.notification_pins == 1u &&
          target_record->service_pending == 0u &&
          p4_task_records.semaphore_notification_serial == UINT32_MAX);
    CHECK(cap_slots[orphan_handle.index].active == 1u &&
          cap_slots[orphan_handle.index].requester_identity ==
              (uintptr_t)fake_requester &&
          cap_slots[orphan_handle.index].target_identity ==
              (uintptr_t)fake_target &&
          cap_slots[orphan_handle.index].generation == orphan_handle.generation &&
          cap_slots[orphan_handle.index].completion.requester_ref == 0u &&
          cap_slots[orphan_handle.index].completion.service_ref == 1u);
    cap_expect_quiescent();
}

static void test_malformed_last_retirement_preserves_orphan(void *page)
{
    struct Task *requester = guarded_identity(page, 64u);
    struct Task *target = guarded_identity(page, 96u);
    struct Task *fake_requester = guarded_identity(page, 128u);
    struct Task *fake_target = guarded_identity(page, 160u);
    struct p4_task_wait_operation operation = { NULL, 0u };
    struct p4_semaphore_ledger_operation *ledger_operation;
    struct p4_remove_handle orphan_handle = { UINT32_MAX, UINT32_MAX };
    struct p4_remove_handle output = { UINT32_C(0xa5a5a5a5),
                                       UINT32_C(0x5a5a5a5a) };
    struct p4_remove_handle before_output = output;
    uint32_t dropped = UINT32_MAX;
    int result;

    cap_reset(4u);
    CHECK(cap_add_running(0u, requester, 0u) == 1);
    CHECK(cap_add_running(1u, target, 1u) == 1);
    cap_gate_enter();
    CHECK(p4_remove_bank_reserve(&cap_bank, (uintptr_t)fake_requester,
        (uintptr_t)fake_target, &orphan_handle) == 1);
    CHECK(p4_remove_bank_cancel_requester(&cap_bank,
        (uintptr_t)fake_requester, &dropped) == 1 && dropped == 1u);
    CHECK(p4_task_registry_wait_reserve_operation_locked(
        &p4_task_records, &cap_ledger, target, 1u, &operation) == 1);
    CHECK(p4_task_registry_wait_operation_set_entry_locked(
        &p4_task_records, &cap_ledger, target, operation.generation, 0u,
        (void *)(uintptr_t)0x101u, (void *)(uintptr_t)0x201u) == 1);
    CHECK(p4_task_registry_wait_operation_close_locked(
        &p4_task_records, &cap_ledger, target, operation.generation) == 1);
    ledger_operation = operation.operation;
    CHECK(ledger_operation != NULL && ledger_operation->cancelled == 0u);
    cap_gate_leave();
    CHECK(krnP4SignalTaskPin(target, &cap_sysbase) == 1);

    /* The ledger remains structurally valid, but its active operation no
     * longer matches the live registry scope. This reaches the final
     * retirement resolver and fails closed before orphan-slot publication. */
    cap_gate_enter();
    ledger_operation->cancelled = 1u;
    cap_gate_leave();
    cap_save_state();
    cap_gate_enter();
    result = p4_task_registry_admit_removal_locked(&p4_task_records,
        &cap_ledger, &cap_bank, requester, target, 0u, 1u, &output);
    cap_gate_leave();
    CHECK(result == -1);
    CHECK(cap_state_unchanged());
    CHECK(memcmp(&output, &before_output, sizeof(output)) == 0);
    CHECK(cap_slots[orphan_handle.index].active == 1u &&
          cap_slots[orphan_handle.index].requester_identity ==
              (uintptr_t)fake_requester &&
          cap_slots[orphan_handle.index].target_identity ==
              (uintptr_t)fake_target &&
          cap_slots[orphan_handle.index].generation == orphan_handle.generation &&
          cap_slots[orphan_handle.index].completion.requester_ref == 0u &&
          cap_slots[orphan_handle.index].completion.service_ref == 1u);
    cap_expect_quiescent();
}

int main(void)
{
    long page_size;
    void *page;
    unsigned int baseline_checks;
    unsigned int packet_start;

    if (p4_remove_capacity_operation_baseline_main() != 0)
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
    test_duplicate_pending_and_acked_refusals(page);
    test_full_orphan_pool_reuses_without_releasing_target(page);
    test_mixed_live_interests_and_orphans(page);
    test_failed_last_retirement_preserves_orphan(page);
    test_malformed_last_retirement_preserves_orphan(page);

    CHECK(munmap(page, (size_t)page_size) == 0);
    cap_expect_quiescent();
    if (failures != 0u)
        return 1;
    printf("P4 removal-ticket capacity: %u checks passed; baseline %u\n",
           checks - packet_start, baseline_checks);
    return 0;
}
