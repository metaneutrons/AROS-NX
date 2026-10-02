/* Actual P4 pins/registry retirement hook plus the real removal bank. */
#define _DEFAULT_SOURCE 1
#define __AROSEXEC_SMP__ 1
#define __AROSPLATFORM_SMP__ 1

#include <stdint.h>

#include "../../exec/p4_remove_bank.h"

int p4_remove_bank_cancel_requester_observer(
    struct p4_remove_bank *, uintptr_t, uint32_t *);

/* Interpose only to assert the real leaf is called while the pins gate is
 * held and after the registry retirement has become visible. */
#define p4_remove_bank_cancel_requester \
    p4_remove_bank_cancel_requester_observer
#define main p4_remove_cancel_admission_baseline_main
#include "p4-semaphore-operation-admission-test.c"
#undef main
#undef p4_remove_bank_cancel_requester

#include "../../exec/p4_remove_bank.c"
#include "../../exec/p4_remove_completion.c"

static struct p4_remove_bank removal_bank;
static struct p4_remove_bank_slot removal_slots[12];
static struct p4_remove_bank serial_bank;
static struct p4_remove_bank_slot serial_slots[1];
static struct p4_task_registry_record binding_records[8];
static struct p4_semaphore_ledger binding_ledger;
static struct p4_semaphore_ledger_operation binding_operations[TEST_OP_CAPACITY];
static struct p4_semaphore_ledger_entry binding_entries[TEST_ENTRY_CAPACITY];
static struct ExecBase binding_sysbase;
static struct p4_remove_bank *expected_bank;
static struct Task *expected_cancel_requester;
static uint32_t expected_cancel_dropped;
static int expected_cancel_service;
static unsigned int cancel_observer_calls;
static unsigned int cancel_observer_failures;
static unsigned int pokes_before_cancel;
static unsigned int retired_pokes_before_cancel;
static uint32_t observed_cancel_dropped;

static void record_check(int condition, const char *label)
{
    ++checks;
    if (!condition)
    {
        ++failures;
        ++cancel_observer_failures;
        fprintf(stderr, "FAIL remove-cancel-binding: %s\n", label);
    }
}

int p4_remove_bank_cancel_requester_observer(
    struct p4_remove_bank *bank, uintptr_t requester_identity,
    uint32_t *out_dropped)
{
    struct p4_task_registry_record *record;
    unsigned int poke_count = mock_poke_calls;
    unsigned int retired_poke_count = mock_retired_poke_calls;
    uint32_t dropped = UINT32_MAX;
    int result;

    ++cancel_observer_calls;
    record_check(mock_gate_held && mock_irq_depth > 0u,
                 "requester cancellation executes under the registry gate");
    record_check(mock_task_lock_held,
                 "retirement caller keeps Task lock through cancellation");
    record_check(bank == expected_bank && bank == &removal_bank,
                 "runtime retirement uses the attached permanent bank");
    record_check(requester_identity == (uintptr_t)expected_cancel_requester,
                 "cancellation identity matches the retiring Task");
    record_check(mock_poke_calls == pokes_before_cancel &&
                 mock_retired_poke_calls == retired_pokes_before_cancel,
                 "no wake is issued before requester tickets are cancelled");

    record = p4_task_registry_lookup_locked(&p4_task_records,
        (struct Task *)requester_identity);
    record_check(record != NULL &&
                 (record->lifetime.flags & P4_TASK_LIFETIME_RETIRED) != 0u,
                 "registry retirement is published before ticket cancellation");
    if (expected_cancel_service)
    {
        record_check(record != NULL && record->service_pending == 1u &&
                     record->wait_scope.cancel_requested == 1u,
                     "service retirement and operation cancellation precede bank update");
    }
    else
    {
        record_check(record != NULL && record->service_pending == 0u,
                     "ordinary retirement does not invent a service reference");
    }

    result = p4_remove_bank_cancel_requester(bank, requester_identity, &dropped);
    record_check(result == 1,
                 "actual bank cancellation leaf succeeds after preflight");
    record_check(dropped == expected_cancel_dropped,
                 "bank cancellation drops exactly the expected requester refs");
    record_check(mock_poke_calls == poke_count &&
                 mock_retired_poke_calls == retired_poke_count,
                 "bank cancellation itself does not call wake transport");
    observed_cancel_dropped = dropped;
    if (out_dropped != NULL)
        *out_dropped = dropped;
    return result;
}

static void reset_binding_runtime(void)
{
    memset(binding_records, 0, sizeof(binding_records));
    memset(&binding_ledger, 0, sizeof(binding_ledger));
    memset(binding_operations, 0, sizeof(binding_operations));
    memset(binding_entries, 0, sizeof(binding_entries));
    memset(test_boot_requests, 0, sizeof(test_boot_requests));
    p4_task_registry_init(&p4_task_records);
    test_boot_ledger = &binding_ledger;
    CHECK(p4_semaphore_ledger_init(&binding_ledger, binding_operations,
        TEST_OP_CAPACITY, binding_entries, TEST_ENTRY_CAPACITY) == 1);

    p4_cleanup_worker = NULL;
    p4_cleanup_wake_mask = 0u;
    p4_cleanup_wake_pending = false;
    p4_cancel_retry.active = 0u;
    p4_cancel_retry.waiting = 0u;

    mock_irq_depth = 0u;
    mock_disable_calls = 0u;
    mock_enable_calls = 0u;
    mock_acquire_calls = 0u;
    mock_unlock_calls = 0u;
    mock_fatal_calls = 0u;
    mock_poke_calls = 0u;
    mock_retired_poke_calls = 0u;
    mock_gate_held = 0;
    mock_outer_semaphore_gate_held = 0;
    mock_task_lock_held = 0;
    mock_gate_address = &p4_task_pin_gate;
    cancel_observer_calls = 0u;
    cancel_observer_failures = 0u;
    expected_bank = &removal_bank;
    expected_cancel_requester = NULL;
    expected_cancel_dropped = 0u;
    expected_cancel_service = 0;
    observed_cancel_dropped = UINT32_MAX;
}

static void pins_gate_enter(void)
{
    p4_task_pins_lock(&binding_sysbase);
}

static void pins_gate_leave(void)
{
    p4_task_pins_unlock(&binding_sysbase);
}

static int bank_reserve(struct p4_remove_bank *bank, struct Task *requester,
                        struct Task *target,
                        struct p4_remove_handle *handle)
{
    int result;
    pins_gate_enter();
    result = p4_remove_bank_reserve(bank, (uintptr_t)requester,
                                    (uintptr_t)target, handle);
    pins_gate_leave();
    return result;
}

static int bank_ack(struct p4_remove_bank *bank,
                    struct p4_remove_handle handle, struct Task *target,
                    uintptr_t *notify, int *reclaim)
{
    int result;
    pins_gate_enter();
    result = p4_remove_bank_ack(bank, handle, (uintptr_t)target,
                                notify, reclaim);
    pins_gate_leave();
    return result;
}

static int bank_ready(struct p4_remove_bank *bank,
                      struct p4_remove_handle handle, struct Task *requester,
                      int *ready)
{
    int result;
    pins_gate_enter();
    result = p4_remove_bank_ready(bank, handle, (uintptr_t)requester, ready);
    pins_gate_leave();
    return result;
}

static int bank_drop(struct p4_remove_bank *bank,
                     struct p4_remove_handle handle, struct Task *requester,
                     int cancel, int *reclaim)
{
    int result;
    pins_gate_enter();
    result = p4_remove_bank_drop(bank, handle, (uintptr_t)requester,
                                 cancel, reclaim);
    pins_gate_leave();
    return result;
}

static int bank_count(struct p4_remove_bank *bank, struct Task *target,
                      uint32_t *count)
{
    int result;
    pins_gate_enter();
    result = p4_remove_bank_count_target(bank, (uintptr_t)target, count);
    pins_gate_leave();
    return result;
}

static void publish_task(unsigned int slot, struct Task *identity)
{
    CHECK(slot < 8u);
    CHECK(krnP4TaskRecordPublish(identity, &binding_records[slot],
                                 &binding_sysbase) == 1);
}

static void retire_call(struct Task *identity, int service, int *result)
{
    Disable();
    mock_task_lock_held = 1;
    pokes_before_cancel = mock_poke_calls;
    retired_pokes_before_cancel = mock_retired_poke_calls;
    *result = service ?
        krnP4TaskRecordRetireForService(identity, &binding_sysbase) :
        krnP4TaskRecordRetire(identity, &binding_sysbase);
    mock_task_lock_held = 0;
    Enable();
}

static void test_attach(void *page)
{
    static struct p4_task_registry_record nonempty_record;
    static struct Task *nonempty_task;
    struct p4_remove_bank bank_before;
    struct p4_remove_bank_slot slots_before[12];
    struct p4_remove_bank used_before;
    struct p4_remove_bank_slot used_slot_before[1];
    struct p4_remove_handle used_handle = {0u, 0u};
    struct Task *used_requester = guarded_identity(page, 640u);
    struct Task *used_target = guarded_identity(page, 704u);
    int reclaim = -1;
    unsigned int disable_before, enable_before, acquire_before, unlock_before;

    reset_binding_runtime();
    CHECK(p4_task_pins_prepared == 1);
    CHECK(p4_task_records.count == 0u && p4_task_records.head == NULL);
    CHECK(p4_removal_bank == NULL);

    nonempty_task = guarded_identity(page, 768u);
    memset(&nonempty_record, 0, sizeof(nonempty_record));
    CHECK(krnP4TaskRecordPublish(nonempty_task, &nonempty_record,
                                 &binding_sysbase) == 1);
    memcpy(&bank_before, &removal_bank, sizeof(bank_before));
    memcpy(slots_before, removal_slots, sizeof(slots_before));
    disable_before = mock_disable_calls;
    enable_before = mock_enable_calls;
    acquire_before = mock_acquire_calls;
    unlock_before = mock_unlock_calls;
    mock_irq_depth = 1u;
    CHECK(krnP4TaskPinsAttachRemovalBank(&removal_bank) == 0);
    CHECK(mock_irq_depth == 1u && !mock_gate_held);
    CHECK(mock_disable_calls == disable_before &&
          mock_enable_calls == enable_before &&
          mock_acquire_calls == acquire_before &&
          mock_unlock_calls == unlock_before);
    CHECK(memcmp(&removal_bank, &bank_before, sizeof(bank_before)) == 0);
    CHECK(memcmp(removal_slots, slots_before, sizeof(slots_before)) == 0);
    CHECK(p4_task_records.count == 1u);
    mock_irq_depth = 0u;

    p4_task_registry_init(&p4_task_records);
    memset(&nonempty_record, 0, sizeof(nonempty_record));
    memset(&serial_bank, 0, sizeof(serial_bank));
    memset(serial_slots, 0, sizeof(serial_slots));
    CHECK(p4_remove_bank_init(&serial_bank, serial_slots, 1u) == 1);
    CHECK(bank_reserve(&serial_bank, used_requester, used_target,
                       &used_handle) == 1);
    CHECK(bank_drop(&serial_bank, used_handle, used_requester, 1,
                    &reclaim) == 1);
    CHECK(reclaim == 0);
    CHECK(bank_ack(&serial_bank, used_handle, used_target,
                   &(uintptr_t){UINTPTR_MAX}, &reclaim) == 1);
    CHECK(reclaim == 1 && serial_bank.serial == 1u);
    memcpy(&used_before, &serial_bank, sizeof(used_before));
    memcpy(used_slot_before, serial_slots, sizeof(used_slot_before));

    disable_before = mock_disable_calls;
    enable_before = mock_enable_calls;
    acquire_before = mock_acquire_calls;
    unlock_before = mock_unlock_calls;
    mock_irq_depth = 1u;
    CHECK(krnP4TaskPinsAttachRemovalBank(&serial_bank) == 0);
    CHECK(mock_irq_depth == 1u && !mock_gate_held);
    CHECK(mock_disable_calls == disable_before &&
          mock_enable_calls == enable_before &&
          mock_acquire_calls == acquire_before &&
          mock_unlock_calls == unlock_before);
    CHECK(memcmp(&serial_bank, &used_before, sizeof(used_before)) == 0);
    CHECK(memcmp(serial_slots, used_slot_before,
                 sizeof(used_slot_before)) == 0);
    CHECK(p4_removal_bank == NULL);

    memcpy(&bank_before, &removal_bank, sizeof(bank_before));
    memcpy(slots_before, removal_slots, sizeof(slots_before));
    disable_before = mock_disable_calls;
    enable_before = mock_enable_calls;
    acquire_before = mock_acquire_calls;
    unlock_before = mock_unlock_calls;
    CHECK(krnP4TaskPinsAttachRemovalBank(&removal_bank) == 1);
    CHECK(p4_removal_bank == &removal_bank);
    CHECK(mock_irq_depth == 1u && !mock_gate_held);
    CHECK(mock_disable_calls == disable_before &&
          mock_enable_calls == enable_before &&
          mock_acquire_calls == acquire_before &&
          mock_unlock_calls == unlock_before);
    CHECK(removal_bank.serial == 0u);
    CHECK(memcmp(slots_before, removal_slots, sizeof(slots_before)) == 0);
    CHECK(memcmp(&removal_bank, &bank_before, sizeof(bank_before)) == 0);
    CHECK(krnP4TaskPinsAttachRemovalBank(&removal_bank) == 0);
    CHECK(p4_removal_bank == &removal_bank);
    CHECK(memcmp(&removal_bank, &bank_before, sizeof(bank_before)) == 0);
    mock_irq_depth = 0u;

    p4_task_registry_init(&p4_task_records);
    CHECK(p4_task_records.count == 0u && p4_task_records.head == NULL);
}

static void test_retire_cancels_requester(void *page)
{
    struct Task *requester = guarded_identity(page, 64u);
    struct Task *unrelated = guarded_identity(page, 128u);
    struct Task *target_a = guarded_identity(page, 256u);
    struct Task *target_b = guarded_identity(page, 320u);
    struct Task *missing = guarded_identity(page, 384u);
    struct p4_remove_handle a_pending, a_completed, b_pending, missing_ticket;
    struct p4_remove_bank_slot before[12];
    struct p4_task_registry_record record_before[8];
    uintptr_t notify = UINTPTR_MAX;
    int reclaim = -1;
    int ready = -1;
    int result;
    uint32_t count = UINT32_MAX;
    unsigned int observer_before;
    unsigned int poke_before;
    unsigned int retired_poke_before;

    reset_binding_runtime();
    publish_task(0u, requester);
    publish_task(1u, unrelated);

    CHECK(bank_reserve(&removal_bank, requester, target_a, &a_pending) == 1);
    CHECK(bank_reserve(&removal_bank, requester, target_b, &a_completed) == 1);
    CHECK(bank_reserve(&removal_bank, unrelated, target_a, &b_pending) == 1);
    /* Create an already-ACKed requester ticket; requester retirement must
     * reclaim this slot while leaving its other target service-owned. */
    CHECK(bank_ack(&removal_bank, a_completed, target_b,
                   &notify, &reclaim) == 1);
    CHECK(notify == (uintptr_t)requester && reclaim == 0);

    expected_cancel_requester = requester;
    expected_cancel_dropped = 2u;
    expected_cancel_service = 0;
    observer_before = cancel_observer_calls;
    poke_before = mock_poke_calls;
    retire_call(requester, 0, &result);
    CHECK(result == 1);
    CHECK(cancel_observer_calls == observer_before + 1u);
    CHECK(observed_cancel_dropped == 2u);
    CHECK(mock_poke_calls >= poke_before);
    CHECK((binding_records[0].lifetime.flags & P4_TASK_LIFETIME_RETIRED) != 0u);
    CHECK(binding_records[0].service_pending == 0u);

    CHECK(removal_slots[a_pending.index].active == 1u);
    CHECK(removal_slots[a_pending.index].generation == a_pending.generation);
    CHECK(removal_slots[a_pending.index].completion.requester_ref == 0u);
    CHECK(removal_slots[a_pending.index].completion.service_ref == 1u);
    CHECK(removal_slots[a_completed.index].active == 0u);
    CHECK(removal_slots[b_pending.index].active == 1u);
    CHECK(removal_slots[b_pending.index].completion.requester_ref == 1u);
    CHECK(removal_slots[b_pending.index].completion.service_ref == 1u);
    CHECK(bank_count(&removal_bank, target_a, &count) == 1);
    CHECK(count == 2u);

    notify = UINTPTR_MAX;
    reclaim = -1;
    CHECK(bank_ack(&removal_bank, a_pending, target_a,
                   &notify, &reclaim) == 1);
    CHECK(notify == 0u && reclaim == 1);
    CHECK(bank_ack(&removal_bank, b_pending, target_a,
                   &notify, &reclaim) == 1);
    CHECK(notify == (uintptr_t)unrelated && reclaim == 0);
    CHECK(bank_ready(&removal_bank, b_pending, unrelated, &ready) == 1);
    CHECK(ready == 1);
    CHECK(bank_drop(&removal_bank, b_pending, unrelated, 0, &reclaim) == 1);
    CHECK(reclaim == 1);

    CHECK(bank_reserve(&removal_bank, missing, target_b, &missing_ticket) == 1);
    memcpy(before, removal_slots, sizeof(before));
    memcpy(record_before, binding_records, sizeof(record_before));
    observer_before = cancel_observer_calls;
    poke_before = mock_poke_calls;
    retired_poke_before = mock_retired_poke_calls;
    expected_cancel_requester = missing;
    expected_cancel_dropped = 1u;
    retire_call(missing, 0, &result);
    CHECK(result == 0);
    CHECK(cancel_observer_calls == observer_before);
    CHECK(mock_poke_calls == poke_before);
    CHECK(mock_retired_poke_calls == retired_poke_before);
    CHECK(memcmp(before, removal_slots, sizeof(before)) == 0);
    CHECK(memcmp(record_before, binding_records,
                 sizeof(record_before)) == 0);
    retire_call(missing, 1, &result);
    CHECK(result == 0);
    CHECK(cancel_observer_calls == observer_before);
    CHECK(mock_poke_calls == poke_before);
    CHECK(mock_retired_poke_calls == retired_poke_before);
    CHECK(memcmp(before, removal_slots, sizeof(before)) == 0);

    /* A refused retirement must leave its bank references untouched. Clean up
     * that still-live requester ticket through the real ACK/drop lifecycle. */
    notify = UINTPTR_MAX;
    reclaim = -1;
    CHECK(bank_ack(&removal_bank, missing_ticket, target_b,
                   &notify, &reclaim) == 1);
    CHECK(notify == (uintptr_t)missing && reclaim == 0);
    CHECK(bank_ready(&removal_bank, missing_ticket, missing, &ready) == 1);
    CHECK(ready == 1);
    CHECK(bank_drop(&removal_bank, missing_ticket, missing, 0,
                    &reclaim) == 1);
    CHECK(reclaim == 1);
}

static void test_service_retire_cancels_ledger_owner(void *page)
{
    struct Task *requester = guarded_identity(page, 448u);
    struct Task *unrelated = guarded_identity(page, 512u);
    struct Task *target_a = guarded_identity(page, 576u);
    struct Task *target_b = guarded_identity(page, 640u);
    struct p4_task_wait_operation operation = { NULL, 0u };
    struct p4_remove_handle pending, completed, unrelated_ticket;
    uintptr_t notify = UINTPTR_MAX;
    int reclaim = -1;
    int result;
    uint32_t count = UINT32_MAX;
    unsigned int observer_before;
    unsigned int retired_poke_before;

    reset_binding_runtime();
    publish_task(2u, requester);
    publish_task(3u, unrelated);
    CHECK(krnP4TaskRecordEnterRunning(requester, 1u,
                                      &binding_sysbase) == 1);
    CHECK(krnP4SemaphoreOperationReserve(requester, 1u, &operation,
                                         &binding_sysbase) == 1);
    CHECK(operation.operation != NULL && operation.generation != 0u);
    CHECK(binding_records[2].wait_scope.kind == P4_TASK_WAIT_OPERATION);

    CHECK(bank_reserve(&removal_bank, requester, target_a, &pending) == 1);
    CHECK(bank_reserve(&removal_bank, requester, target_b, &completed) == 1);
    CHECK(bank_reserve(&removal_bank, unrelated, target_a,
                       &unrelated_ticket) == 1);
    CHECK(bank_ack(&removal_bank, completed, target_b,
                   &notify, &reclaim) == 1);
    CHECK(notify == (uintptr_t)requester && reclaim == 0);

    expected_cancel_requester = requester;
    expected_cancel_dropped = 2u;
    expected_cancel_service = 1;
    observer_before = cancel_observer_calls;
    retired_poke_before = mock_retired_poke_calls;
    retire_call(requester, 1, &result);
    CHECK(result == 1);
    CHECK(cancel_observer_calls == observer_before + 1u);
    CHECK(observed_cancel_dropped == 2u);
    CHECK(mock_retired_poke_calls == retired_poke_before + 1u);
    CHECK(binding_records[2].service_pending == 1u);
    CHECK(binding_records[2].lifetime.running_harts == 2u);
    CHECK(binding_records[2].wait_scope.cancel_requested == 1u);
    CHECK(operation.operation->cancelled == 1u);

    CHECK(removal_slots[pending.index].active == 1u);
    CHECK(removal_slots[pending.index].completion.requester_ref == 0u);
    CHECK(removal_slots[pending.index].completion.service_ref == 1u);
    CHECK(removal_slots[completed.index].active == 0u);
    CHECK(removal_slots[unrelated_ticket.index].active == 1u);
    CHECK(removal_slots[unrelated_ticket.index].completion.requester_ref == 1u);
    CHECK(removal_slots[unrelated_ticket.index].completion.service_ref == 1u);
    CHECK(bank_count(&removal_bank, target_a, &count) == 1);
    CHECK(count == 2u);

    /* Model the later service ACK boundary after target cleanup; this does
     * not execute the production worker or free Task storage. */
    notify = UINTPTR_MAX;
    reclaim = -1;
    CHECK(bank_ack(&removal_bank, pending, target_a, &notify, &reclaim) == 1);
    CHECK(notify == 0u && reclaim == 1);
    CHECK(bank_ack(&removal_bank, unrelated_ticket, target_a,
                   &notify, &reclaim) == 1);
    CHECK(notify == (uintptr_t)unrelated && reclaim == 0);
    CHECK(bank_drop(&removal_bank, unrelated_ticket, unrelated, 1,
                    &reclaim) == 1);
    CHECK(reclaim == 1);
}

int main(void)
{
    long page_size;
    void *page;
    unsigned int baseline_checks;

    memset(&removal_bank, 0, sizeof(removal_bank));
    memset(removal_slots, 0, sizeof(removal_slots));
    CHECK(p4_remove_bank_init(&removal_bank, removal_slots, 12u) == 1);
    CHECK(p4_task_pins_prepared == 0);
    mock_irq_depth = 1u; /* Simulated raw-MIE-off boot section. */
    CHECK(krnP4TaskPinsAttachRemovalBank(&removal_bank) == 0);
    CHECK(mock_irq_depth == 1u && !mock_gate_held);
    CHECK(mock_disable_calls == 0u && mock_acquire_calls == 0u);
    mock_irq_depth = 0u;

    if (p4_remove_cancel_admission_baseline_main() != 0)
        return 1;
    baseline_checks = checks;
    CHECK(p4_task_pins_prepared == 1);
    CHECK(p4_removal_bank == NULL);

    reset_binding_runtime();
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

    test_attach(page);
    test_retire_cancels_requester(page);
    test_service_retire_cancels_ledger_owner(page);

    CHECK(munmap(page, (size_t)page_size) == 0);
    CHECK(!mock_gate_held && mock_irq_depth == 0u);
    CHECK(mock_fatal_calls == 0u);
    CHECK(mock_acquire_calls == mock_unlock_calls);
    CHECK(mock_disable_calls == mock_enable_calls);
    if (failures != 0u)
        return 1;
    printf("P4 removal cancellation binding: %u checks passed; admission baseline %u\n",
        checks - baseline_checks, baseline_checks);
    return 0;
}
