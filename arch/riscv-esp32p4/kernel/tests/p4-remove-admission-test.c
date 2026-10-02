/* Actual-source tests for atomic external-removal admission. */
#define main p4_remove_admission_leaf_baseline_main
#include "p4-semaphore-operation-admission-test.c"
#undef main

#include "../p4_task_registry.h"
#include "../p4_task_pins.h"
#include "../p4_semaphore_ledger.h"
#include "../p4_semaphore_nodes.h"
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

static int pin_identity(struct Task *identity)
{
    int result;
    gate_enter();
    result = p4_task_registry_pin_locked(&p4_task_records, identity);
    gate_leave();
    return result;
}

static int unpin_identity(struct Task *identity)
{
    int result;
    gate_enter();
    result = p4_task_registry_unpin_locked(&p4_task_records, identity);
    gate_leave();
    return result;
}

static int leave_hart(struct Task *identity, uint32_t hart)
{
    int result;
    gate_enter();
    result = p4_task_registry_leave_running_locked(&p4_task_records,
        identity, hart);
    gate_leave();
    return result;
}

static int admit(struct p4_remove_bank *bank, struct Task *requester,
                 struct Task *target, uint32_t hart, uint32_t retire_live,
                 struct p4_remove_handle *handle)
{
    int result;
    gate_enter();
    result = p4_task_registry_admit_removal_locked(&p4_task_records,
        &admit_ledger, bank, requester, target, hart, retire_live, handle);
    gate_leave();
    return result;
}

static int add_bank_ticket(struct Task *requester, struct Task *target,
                           struct p4_remove_handle *handle)
{
    int result;
    gate_enter();
    result = p4_remove_bank_reserve(&admit_bank, (uintptr_t)requester,
                                    (uintptr_t)target, handle);
    gate_leave();
    return result;
}

static void take_snapshot(struct admit_snapshot *snapshot)
{
    snapshot_state(&snapshot->shared);
    memcpy(&snapshot->ledger, &admit_ledger, sizeof(admit_ledger));
    memcpy(snapshot->operations, admit_operations, sizeof(admit_operations));
    memcpy(snapshot->entries, admit_entries, sizeof(admit_entries));
}

static int snapshot_unchanged(const struct admit_snapshot *snapshot)
{
    struct state_snapshot current;
    snapshot_state(&current);
    return memcmp(&current, &snapshot->shared, sizeof(current)) == 0 &&
           memcmp(&admit_ledger, &snapshot->ledger, sizeof(admit_ledger)) == 0 &&
           memcmp(admit_operations, snapshot->operations,
                  sizeof(admit_operations)) == 0 &&
           memcmp(admit_entries, snapshot->entries,
                  sizeof(admit_entries)) == 0;
}

static void expect_admit_unchanged(const char *label,
                                   struct p4_remove_bank *bank,
                                   struct Task *requester,
                                   struct Task *target, uint32_t hart,
                                   uint32_t retire_live, int expected)
{
    struct p4_remove_handle output = { UINT32_C(0xa5a5a5a5),
                                       UINT32_C(0x5a5a5a5a) };
    struct p4_remove_handle before_output = output;
    struct admit_snapshot before;
    int result;

    take_snapshot(&before);
    result = admit(bank, requester, target, hart, retire_live, &output);
    if (result != expected)
        fprintf(stderr, "admission case %s: got %d, expected %d\n",
                label, result, expected);
    CHECK(result == expected);
    CHECK(snapshot_unchanged(&before));
    CHECK(memcmp(&output, &before_output, sizeof(output)) == 0);
}

static int prepare_existing_retained(struct Task *identity,
                                     struct p4_task_registry_record **out_record)
{
    struct Task *ready;
    int result;

    gate_enter();
    ready = p4_task_registry_take_service_ready_locked(
        &p4_task_records, identity);
    result = ready == identity ?
        p4_task_registry_claim_next_retained_service_locked(
            &p4_task_records, out_record) : 0;
    gate_leave();
    if (result != 1)
        return result;

    gate_enter();
    result = p4_task_registry_defer_removal_finish_locked(
        &p4_task_records, *out_record);
    gate_leave();
    return result;
}

static int retire_and_prepare(struct Task *identity,
                              struct p4_task_registry_record **out_record)
{
    int result;
    gate_enter();
    result = p4_task_registry_retire_for_service_locked(
        &p4_task_records, identity);
    gate_leave();
    return result == 1 ? prepare_existing_retained(identity, out_record) : result;
}

static int finish_admitted_target(struct p4_task_registry_record *retained,
                                  struct p4_remove_handle handle,
                                  struct Task *requester, int *out_ready)
{
    int result;
    gate_enter();
    result = p4_task_registry_finish_removal_locked(&p4_task_records,
        &admit_bank, retained);
    if (result > 0)
    {
        int ready = -1;
        int query = p4_remove_bank_ready(&admit_bank, handle,
            (uintptr_t)requester, &ready);
        CHECK(query == 1);
        *out_ready = ready;
    }
    gate_leave();
    return result;
}

static void test_live_admission_and_completion(void *page)
{
    struct Task *requester = guarded_identity(page, 64u);
    struct Task *target = guarded_identity(page, 96u);
    struct p4_remove_handle handle = { UINT32_MAX, UINT32_MAX };
    struct p4_task_registry_record *retained = NULL;
    struct p4_semaphore_notification notification;
    int ready = -1;
    int result;

    admission_reset();
    CHECK(add_running(0u, requester, 0u) == 1);
    CHECK(add_running(1u, target, 1u) == 1);
    CHECK(pin_identity(target) == 1); /* Ordinary admission pin. */
    result = admit(&admit_bank, requester, target, 0u, 1u, &handle);
    CHECK(result == 1);
    CHECK(handle.index == 0u && handle.generation == 1u);
    CHECK(admit_records[1].lifetime.flags == P4_TASK_LIFETIME_RETIRED);
    CHECK(admit_records[1].service_pending == 1u &&
          admit_records[1].lifetime.notification_pins == 2u &&
          admit_records[1].service_generation == 1u);
    CHECK(admit_slots[handle.index].active == 1u &&
          admit_slots[handle.index].requester_identity == (uintptr_t)requester &&
          admit_slots[handle.index].target_identity == (uintptr_t)target);
    CHECK(leave_hart(target, 1u) == 1);
    CHECK(unpin_identity(target) == 1);
    CHECK(prepare_existing_retained(target, &retained) == 1);
    CHECK(retained == &admit_records[1] && retained->service_postfree == 1u);
    result = finish_admitted_target(retained, handle, requester, &ready);
    CHECK(result == 2 && ready == 1);
    CHECK(lookup_record(target) == NULL);
    CHECK(admit_records[0].semaphore_pending == 1u &&
          admit_records[0].semaphore_pending_generation == 1u &&
          admit_records[0].lifetime.notification_pins == 1u);
    memset(&notification, 0, sizeof(notification));
    gate_enter();
    result = p4_task_registry_claim_next_semaphore_notification_locked(
        &p4_task_records, &notification);
    gate_leave();
    CHECK(result == 1 && notification.identity == requester &&
          notification.count == 1u && notification.generation == 1u);
    gate_enter();
    result = p4_task_registry_finish_semaphore_notification_locked(
        &p4_task_records, &notification);
    gate_leave();
    CHECK(result == 1 && admit_records[0].lifetime.notification_pins == 0u);
}

static void test_live_target_refusals(void *page)
{
    struct Task *requester = guarded_identity(page, 64u);
    struct Task *target = guarded_identity(page, 96u);
    struct Task *missing = guarded_identity(page, 128u);
    struct Task *no_run = guarded_identity(page, 160u);

    admission_reset();
    CHECK(add_running(0u, requester, 0u) == 1);
    CHECK(add_running(1u, target, 1u) == 1);
    CHECK(pin_identity(target) == 1);
    expect_admit_unchanged("live target requires retire_live", &admit_bank,
                           requester, target, 0u, 0u, 0);
    CHECK(unpin_identity(target) == 1);
    expect_admit_unchanged("live target without ordinary pin", &admit_bank,
                           requester, target, 0u, 1u, 0);
    expect_admit_unchanged("missing target", &admit_bank, requester,
                           missing, 0u, 1u, 0);
    expect_admit_unchanged("missing requester", &admit_bank, missing,
                           target, 0u, 1u, 0);
    CHECK(add_registered(2u, no_run) == 1);
    expect_admit_unchanged("requester without RUN", &admit_bank, no_run,
                           target, 0u, 1u, 0);
    expect_admit_unchanged("wrong requester hart", &admit_bank, requester,
                           target, 1u, 1u, 0);
    CHECK(pin_identity(target) == 1);
    gate_enter();
    CHECK(p4_task_registry_protect_worker_locked(&p4_task_records,
                                                 requester) == 1);
    gate_leave();
    expect_admit_unchanged("protected requester", &admit_bank, requester,
                           target, 0u, 1u, 0);

    admission_reset();
    CHECK(add_running(0u, requester, 0u) == 1);
    CHECK(add_running(1u, target, 1u) == 1);
    CHECK(pin_identity(target) == 1);
    gate_enter();
    CHECK(p4_task_registry_retire_locked(&p4_task_records, requester) == 1);
    gate_leave();
    expect_admit_unchanged("retired requester", &admit_bank, requester,
                           target, 0u, 1u, 0);
}

static void test_capacity_and_token_refusals(void *page)
{
    struct Task *requester = guarded_identity(page, 64u);
    struct Task *target = guarded_identity(page, 96u);
    struct Task *other = guarded_identity(page, 128u);
    struct p4_remove_handle filler;

    admission_reset();
    CHECK(add_running(0u, requester, 0u) == 1);
    CHECK(add_running(1u, target, 1u) == 1);
    CHECK(pin_identity(target) == 1);
    admit_bank.capacity = 1u;
    CHECK(add_bank_ticket(other, other, &filler) == 1);
    expect_admit_unchanged("full ticket bank", &admit_bank, requester,
                           target, 0u, 1u, 0);

    admission_reset();
    CHECK(add_running(0u, requester, 0u) == 1);
    CHECK(add_running(1u, target, 1u) == 1);
    CHECK(pin_identity(target) == 1);
    admit_bank.serial = UINT32_MAX;
    expect_admit_unchanged("ticket generation exhausted", &admit_bank,
                           requester, target, 0u, 1u, 0);

    admission_reset();
    CHECK(add_running(0u, requester, 0u) == 1);
    CHECK(add_running(1u, target, 1u) == 1);
    CHECK(pin_identity(target) == 1);
    p4_task_records.semaphore_notification_serial = UINT32_MAX;
    expect_admit_unchanged("service generation exhausted", &admit_bank,
                           requester, target, 0u, 1u, 0);
}

static void test_retired_target_modes(void *page)
{
    struct Task *requester = guarded_identity(page, 64u);
    struct Task *target = guarded_identity(page, 96u);
    struct p4_task_registry_record *retained = NULL;
    struct p4_remove_handle output;
    struct p4_remove_handle before_output;
    struct p4_remove_handle handle;
    struct admit_snapshot before;
    int ready = -1;

    admission_reset();
    CHECK(add_running(0u, requester, 0u) == 1);
    CHECK(add_registered(1u, target) == 1);
    gate_enter();
    CHECK(p4_task_registry_retire_for_service_locked(
        &p4_task_records, target) == 1);
    gate_leave();
    output.index = UINT32_C(0x11223344);
    output.generation = UINT32_C(0x55667788);
    before_output = output;
    take_snapshot(&before);
    CHECK(admit(&admit_bank, requester, target, 0u, 1u, &output) == 0);
    CHECK(snapshot_unchanged(&before));
    CHECK(memcmp(&output, &before_output, sizeof(output)) == 0);
    CHECK(admit(&admit_bank, requester, target, 0u, 0u, &handle) == 2);
    CHECK(handle.index == 0u && handle.generation == 1u);
    gate_enter();
    CHECK(p4_task_registry_take_service_ready_locked(
        &p4_task_records, target) == target);
    CHECK(p4_task_registry_claim_next_retained_service_locked(
        &p4_task_records, &retained) == 1);
    CHECK(p4_task_registry_defer_removal_finish_locked(
        &p4_task_records, retained) == 1);
    gate_leave();
    CHECK(finish_admitted_target(retained, handle, requester, &ready) == 2);
    CHECK(ready == 1);

    /* A retained post-free record is also joinable solely by raw identity. */
    admission_reset();
    CHECK(add_running(0u, requester, 0u) == 1);
    CHECK(add_registered(1u, target) == 1);
    CHECK(retire_and_prepare(target, &retained) == 1);
    CHECK(retained->service_postfree == 1u && retained->service_inflight == 1u);
    CHECK(admit(&admit_bank, requester, target, 0u, 0u, &handle) == 2);
    CHECK(finish_admitted_target(retained, handle, requester, &ready) == 2);
    CHECK(ready == 1);
}

static void test_operation_cancellation_is_fused(void *page)
{
    struct Task *requester = guarded_identity(page, 64u);
    struct Task *target = guarded_identity(page, 96u);
    struct p4_task_wait_operation operation = { NULL, 0u };
    struct p4_task_wait_cancel_snapshot cancel;
    struct p4_task_registry_record *retained = NULL;
    struct p4_remove_handle handle;
    struct admit_snapshot before;
    uint32_t dropped;
    int ready = -1;

    admission_reset();
    CHECK(add_running(0u, requester, 0u) == 1);
    CHECK(add_running(1u, target, 1u) == 1);
    gate_enter();
    CHECK(p4_task_registry_wait_reserve_operation_locked(
        &p4_task_records, &admit_ledger, target, 1u, &operation) == 1);
    CHECK(p4_task_registry_wait_operation_set_entry_locked(
        &p4_task_records, &admit_ledger, target, operation.generation, 0u,
        (void *)(uintptr_t)0x101u, (void *)(uintptr_t)0x201u) == 1);
    CHECK(p4_task_registry_wait_operation_close_locked(
        &p4_task_records, &admit_ledger, target, operation.generation) == 1);
    CHECK(p4_task_registry_pin_locked(&p4_task_records, target) == 1);
    gate_leave();

    /* Bank-capacity refusal is before the last-fallible fused retirement, so
     * it cannot cancel the real semaphore operation or consume its pins. */
    admit_bank.serial = UINT32_MAX;
    take_snapshot(&before);
    handle.index = 17u;
    handle.generation = 19u;
    {
        struct p4_remove_handle unchanged = handle;
        CHECK(admit(&admit_bank, requester, target, 0u, 1u, &handle) == 0);
        CHECK(snapshot_unchanged(&before));
        CHECK(memcmp(&handle, &unchanged, sizeof(handle)) == 0);
    }

    admit_bank.serial = 0u;
    CHECK(admit(&admit_bank, requester, target, 0u, 1u, &handle) == 1);
    CHECK(admit_records[1].wait_scope.cancel_requested == 1u &&
          operation.operation->cancelled == 1u &&
          admit_records[1].service_pending == 1u &&
          admit_records[1].lifetime.notification_pins == 3u);
    CHECK(leave_hart(target, 1u) == 1);
    gate_enter();
    dropped = UINT32_MAX;
    CHECK(p4_task_registry_claim_next_wait_cancel_with_ledger_locked(
        &p4_task_records, &admit_ledger, &cancel) == 1);
    CHECK(cancel.identity == target && cancel.kind == P4_TASK_WAIT_OPERATION);
    CHECK(p4_task_registry_wait_operation_finish_cancel_locked(
        &p4_task_records, &admit_ledger, target, operation.generation) == 1);
    CHECK(p4_task_registry_unpin_locked(&p4_task_records, target) == 1);
    CHECK(p4_remove_bank_cancel_requester(&admit_bank, (uintptr_t)target,
                                           &dropped) == 1);
    gate_leave();
    CHECK(dropped == 0u);
    CHECK(prepare_existing_retained(target, &retained) == 1);
    CHECK(finish_admitted_target(retained, handle, requester, &ready) == 2);
    CHECK(ready == 1);
}

static void test_target_requester_refs_are_cancelled(void *page)
{
    struct Task *requester = guarded_identity(page, 64u);
    struct Task *target = guarded_identity(page, 96u);
    struct Task *other_a = guarded_identity(page, 128u);
    struct Task *other_b = guarded_identity(page, 160u);
    struct p4_remove_handle pending_handle;
    struct p4_remove_handle completed_handle;
    struct p4_remove_handle new_handle;
    struct p4_task_registry_record *retained = NULL;
    uintptr_t notify = UINTPTR_MAX;
    int reclaim = -1;
    int ready = -1;

    admission_reset();
    CHECK(add_running(0u, requester, 0u) == 1);
    CHECK(add_running(1u, target, 1u) == 1);
    CHECK(add_registered(2u, other_a) == 1);
    CHECK(add_registered(3u, other_b) == 1);
    CHECK(add_bank_ticket(target, other_a, &pending_handle) == 1);
    CHECK(add_bank_ticket(target, other_b, &completed_handle) == 1);
    gate_enter();
    CHECK(p4_remove_bank_ack(&admit_bank, completed_handle,
        (uintptr_t)other_b, &notify, &reclaim) == 1);
    gate_leave();
    CHECK(notify == (uintptr_t)target && reclaim == 0);
    CHECK(pin_identity(target) == 1);
    CHECK(admit(&admit_bank, requester, target, 0u, 1u, &new_handle) == 1);
    CHECK(new_handle.index != pending_handle.index &&
          new_handle.index != completed_handle.index);
    CHECK(admit_slots[pending_handle.index].active == 1u &&
          admit_slots[pending_handle.index].requester_identity == (uintptr_t)target &&
          admit_slots[pending_handle.index].completion.requester_ref == 0u &&
          admit_slots[pending_handle.index].completion.service_ref == 1u &&
          admit_slots[pending_handle.index].generation ==
              pending_handle.generation);
    CHECK(admit_slots[completed_handle.index].active == 0u &&
          admit_slots[completed_handle.index].requester_identity == 0u &&
          admit_slots[completed_handle.index].completion.requester_ref == 0u &&
          admit_slots[completed_handle.index].generation ==
              completed_handle.generation);
    CHECK(leave_hart(target, 1u) == 1);
    CHECK(unpin_identity(target) == 1);
    CHECK(prepare_existing_retained(target, &retained) == 1);
    CHECK(finish_admitted_target(retained, new_handle, requester,
                                 &ready) == 2);
    CHECK(ready == 1);
    CHECK(admit_slots[pending_handle.index].active == 1u &&
          admit_slots[pending_handle.index].completion.service_ref == 1u);
}

static void test_malformed_and_alias_refusals(void *page)
{
    struct Task *requester = guarded_identity(page, 64u);
    struct Task *target = guarded_identity(page, 96u);
    struct p4_remove_bank malformed_bank;
    struct p4_remove_handle output;
    struct p4_remove_handle saved_output;
    struct p4_remove_bank_slot malformed_slots[2];
    struct admit_snapshot before;
    struct p4_semaphore_ledger saved_ledger;
    int result;

    admission_reset();
    CHECK(add_running(0u, requester, 0u) == 1);
    CHECK(add_running(1u, target, 1u) == 1);
    CHECK(pin_identity(target) == 1);
    output.index = 0x12345678u;
    output.generation = 0x87654321u;
    saved_output = output;

    take_snapshot(&before);
    result = admit((struct p4_remove_bank *)(void *)&p4_task_records,
        requester, target, 0u, 1u, &output);
    CHECK(result == -1 && snapshot_unchanged(&before));
    CHECK(memcmp(&output, &saved_output, sizeof(output)) == 0);

    take_snapshot(&before);
    gate_enter();
    result = p4_task_registry_admit_removal_locked(&p4_task_records,
        &admit_ledger, &admit_bank, requester, target, 0u, 1u,
        (struct p4_remove_handle *)(void *)&admit_records[0]);
    gate_leave();
    CHECK(result == -1);
    CHECK(snapshot_unchanged(&before));

    take_snapshot(&before);
    gate_enter();
    result = p4_task_registry_admit_removal_locked(&p4_task_records,
        &admit_ledger, &admit_bank, requester, target, 0u, 1u,
        (struct p4_remove_handle *)(void *)&admit_slots[0]);
    gate_leave();
    CHECK(result == -1 && snapshot_unchanged(&before));

    /* Header, pool, ledger and output aliases are refused before typed
     * backing storage is traversed. */
    malformed_bank = admit_bank;
    malformed_bank.slots = (struct p4_remove_bank_slot *)(void *)admit_records;
    take_snapshot(&before);
    gate_enter();
    result = p4_task_registry_admit_removal_locked(&p4_task_records,
        &admit_ledger, &malformed_bank, requester, target, 0u, 1u, &output);
    gate_leave();
    CHECK(result == -1 && snapshot_unchanged(&before));

    memset(&malformed_bank, 0, sizeof(malformed_bank));
    memset(malformed_slots, 0, sizeof(malformed_slots));
    CHECK(p4_remove_bank_init(&malformed_bank, malformed_slots, 2u) == 1);
    take_snapshot(&before);
    gate_enter();
    result = p4_task_registry_admit_removal_locked(&p4_task_records,
        &admit_ledger, &malformed_bank, requester, target, 0u, 1u,
        (struct p4_remove_handle *)(void *)&malformed_bank);
    gate_leave();
    CHECK(result == -1 && snapshot_unchanged(&before));
    CHECK(malformed_bank.serial == 0u && malformed_slots[0].active == 0u);

    take_snapshot(&before);
    gate_enter();
    result = p4_task_registry_admit_removal_locked(&p4_task_records,
        (struct p4_semaphore_ledger *)(void *)&admit_bank,
        &admit_bank, requester, target, 0u, 1u, &output);
    gate_leave();
    CHECK(result == -1 && snapshot_unchanged(&before));

    take_snapshot(&before);
    gate_enter();
    result = p4_task_registry_admit_removal_locked(&p4_task_records,
        (struct p4_semaphore_ledger *)(void *)&p4_task_records,
        &admit_bank, requester, target, 0u, 1u, &output);
    gate_leave();
    CHECK(result == -1 && snapshot_unchanged(&before));

    gate_enter();
    result = p4_task_registry_admit_removal_locked(&p4_task_records,
        &admit_ledger, &admit_bank, requester, target, 0u, 1u,
        (struct p4_remove_handle *)(void *)&admit_ledger);
    gate_leave();
    CHECK(result == -1);
    CHECK(snapshot_unchanged(&before));

    /* A valid mapped ledger with corrupt counts is a metadata error. */
    saved_ledger = admit_ledger;
    ++admit_ledger.active_entries;
    take_snapshot(&before);
    gate_enter();
    result = p4_task_registry_admit_removal_locked(&p4_task_records,
        &admit_ledger, &admit_bank, requester, target, 0u, 1u, &output);
    gate_leave();
    CHECK(result == -1 && snapshot_unchanged(&before));
    admit_ledger = saved_ledger;

    saved_ledger = admit_ledger;
    admit_ledger.operations =
        (struct p4_semaphore_ledger_operation *)(void *)admit_records;
    take_snapshot(&before);
    gate_enter();
    result = p4_task_registry_admit_removal_locked(&p4_task_records,
        &admit_ledger, &admit_bank, requester, target, 0u, 1u, &output);
    gate_leave();
    CHECK(result == -1 && snapshot_unchanged(&before));
    admit_ledger = saved_ledger;

    saved_ledger = admit_ledger;
    admit_ledger.entries =
        (struct p4_semaphore_ledger_entry *)(void *)admit_records;
    take_snapshot(&before);
    gate_enter();
    result = p4_task_registry_admit_removal_locked(&p4_task_records,
        &admit_ledger, &admit_bank, requester, target, 0u, 1u, &output);
    gate_leave();
    CHECK(result == -1 && snapshot_unchanged(&before));
    admit_ledger = saved_ledger;

    /* Misaligned mapped handle storage is rejected without writes. */
    take_snapshot(&before);
    gate_enter();
    result = p4_task_registry_admit_removal_locked(&p4_task_records,
        &admit_ledger, &admit_bank, requester, target, 0u, 1u,
        (struct p4_remove_handle *)((unsigned char *)&output + 1u));
    gate_leave();
    CHECK(result == -1 && snapshot_unchanged(&before));
}

int main(void)
{
    long page_size;
    void *page;
    unsigned int baseline_checks;
    unsigned int packet_start;

    if (p4_remove_admission_leaf_baseline_main() != 0)
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
    test_live_admission_and_completion(page);
    test_live_target_refusals(page);
    test_capacity_and_token_refusals(page);
    test_retired_target_modes(page);
    test_operation_cancellation_is_fused(page);
    test_target_requester_refs_are_cancelled(page);
    test_malformed_and_alias_refusals(page);

    CHECK(munmap(page, (size_t)page_size) == 0);
    CHECK(!mock_gate_held && mock_irq_depth == 0u);
    CHECK(mock_fatal_calls == 0u);
    CHECK(mock_acquire_calls == mock_unlock_calls);
    CHECK(mock_disable_calls == mock_enable_calls);
    if (failures != 0u)
        return 1;
    printf("P4 removal admission: %u checks passed; baseline %u\n",
           checks - packet_start, baseline_checks);
    return 0;
}
