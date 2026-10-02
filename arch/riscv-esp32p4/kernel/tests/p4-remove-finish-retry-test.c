/* Actual pin/registry/bank source for bounded post-free completion retry. */
#define _DEFAULT_SOURCE 1

#define main p4_remove_finish_retry_admission_baseline_main
#include "p4-semaphore-operation-admission-test.c"
#undef main

#include "../../exec/p4_remove_bank.c"
#include "../../exec/p4_remove_completion.c"

#define RETRY_RECORD_CAPACITY 16u
#define RETRY_BANK_CAPACITY 24u

static struct p4_task_registry_record retry_records[RETRY_RECORD_CAPACITY];
static struct p4_remove_bank retry_bank;
static struct p4_remove_bank_slot retry_slots[RETRY_BANK_CAPACITY];
static struct ExecBase retry_sysbase;
static struct p4_semaphore_ledger retry_ledger;
static struct p4_semaphore_ledger_operation retry_operations[2];
static struct p4_semaphore_ledger_entry retry_entries[2];

struct retry_snapshot
{
    struct p4_task_registry registry;
    struct p4_task_registry_record records[RETRY_RECORD_CAPACITY];
    struct p4_remove_bank bank;
    struct p4_remove_bank_slot slots[RETRY_BANK_CAPACITY];
};

static void retry_gate_enter(void)
{
    p4_task_pins_lock(&retry_sysbase);
    CHECK(mock_gate_held && mock_irq_depth != 0u);
}

static void retry_gate_leave(void)
{
    CHECK(mock_gate_held && mock_irq_depth != 0u);
    p4_task_pins_unlock(&retry_sysbase);
    CHECK(!mock_gate_held);
}

static void retry_reset(void)
{
    memset(retry_records, 0, sizeof(retry_records));
    memset(retry_slots, 0, sizeof(retry_slots));
    memset(&retry_bank, 0, sizeof(retry_bank));
    memset(&retry_ledger, 0, sizeof(retry_ledger));
    memset(retry_operations, 0, sizeof(retry_operations));
    memset(retry_entries, 0, sizeof(retry_entries));
    memset(test_boot_requests, 0, sizeof(test_boot_requests));
    p4_task_registry_init(&p4_task_records);
    CHECK(p4_remove_bank_init(&retry_bank, retry_slots,
        RETRY_BANK_CAPACITY) == 1);
    CHECK(p4_semaphore_ledger_init(&retry_ledger, retry_operations,
        2u, retry_entries, 2u) == 1);
    test_boot_ledger = &retry_ledger;

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

    p4_cleanup_worker = NULL;
    p4_cleanup_wake_mask = 0u;
    p4_cleanup_wake_pending = false;
    p4_cancel_retry.active = 0u;
    p4_cancel_retry.waiting = 0u;
    p4_removal_bank = NULL;
    p4_removal_finish_waiting = false;
    p4_postfree_after = 0u;
    p4_postfree_blocked = false;
    p4_postfree_restart = false;
    CHECK(p4_task_pins_prepared == 1);
    CHECK(krnP4TaskPinsAttachRemovalBank(&retry_bank) == 1);
}

static int retry_add_task(unsigned int index, struct Task *identity)
{
    int result;
    CHECK(index < RETRY_RECORD_CAPACITY);
    retry_gate_enter();
    result = p4_task_registry_register_locked(&p4_task_records,
        &retry_records[index], identity);
    retry_gate_leave();
    return result;
}

static struct p4_task_registry_record *retry_lookup(struct Task *identity)
{
    struct p4_task_registry_record *record;
    retry_gate_enter();
    record = p4_task_registry_lookup_locked(&p4_task_records, identity);
    retry_gate_leave();
    return record;
}

static int retry_add_ticket(struct Task *requester, struct Task *target,
                            struct p4_remove_handle *handle)
{
    int result;
    retry_gate_enter();
    result = p4_remove_bank_reserve(&retry_bank, (uintptr_t)requester,
        (uintptr_t)target, handle);
    retry_gate_leave();
    return result;
}

static struct p4_task_registry_record *retry_make_retained(
    struct Task *identity)
{
    struct p4_task_registry_record *record = NULL;
    struct Task *ready;
    int result;

    retry_gate_enter();
    result = p4_task_registry_retire_for_service_locked(
        &p4_task_records, identity);
    retry_gate_leave();
    CHECK(result == 1);
    retry_gate_enter();
    ready = p4_task_registry_take_service_ready_locked(
        &p4_task_records, identity);
    retry_gate_leave();
    CHECK(ready == identity);
    retry_gate_enter();
    result = p4_task_registry_claim_next_retained_service_locked(
        &p4_task_records, &record);
    retry_gate_leave();
    CHECK(result == 1 && record != NULL && record->identity == identity);
    return record;
}

static int retry_finish_leaf(struct p4_task_registry_record *record)
{
    int result;
    retry_gate_enter();
    result = p4_task_registry_finish_removal_locked(&p4_task_records,
        &retry_bank, record);
    retry_gate_leave();
    return result;
}

static int retry_claim_notification(struct p4_semaphore_notification *snapshot)
{
    int result;
    retry_gate_enter();
    result = p4_task_registry_claim_next_semaphore_notification_locked(
        &p4_task_records, snapshot);
    retry_gate_leave();
    return result;
}

static void retry_set_pins(struct Task *identity, uint32_t pins)
{
    struct p4_task_registry_record *record = retry_lookup(identity);
    CHECK(record != NULL);
    retry_gate_enter();
    record->lifetime.notification_pins = pins;
    retry_gate_leave();
}

static void retry_add_worker(struct Task *worker)
{
    CHECK(retry_add_task(0u, worker) == 1);
    CHECK(krnP4TaskRecordEnterRunning(worker, 0u, &retry_sysbase) == 1);
    CHECK(krnP4ProtectCleanupWorker(worker, &retry_sysbase) == 1);
    CHECK(krnP4ArmCleanupWake(worker, UINT32_C(1), &retry_sysbase) == 1);
    CHECK(krnP4CleanupWorkerReady(&retry_sysbase) == 1);
}

static void retry_snapshot_state(struct retry_snapshot *snapshot)
{
    retry_gate_enter();
    memcpy(&snapshot->registry, &p4_task_records,
        sizeof(snapshot->registry));
    memcpy(snapshot->records, retry_records, sizeof(retry_records));
    memcpy(&snapshot->bank, &retry_bank, sizeof(retry_bank));
    memcpy(snapshot->slots, retry_slots, sizeof(retry_slots));
    retry_gate_leave();
}

static void retry_expect_unchanged(const struct retry_snapshot *snapshot,
                                   const char *label)
{
    int unchanged;
    retry_gate_enter();
    unchanged = memcmp(&snapshot->registry, &p4_task_records,
                   sizeof(snapshot->registry)) == 0 &&
        memcmp(snapshot->records, retry_records, sizeof(retry_records)) == 0 &&
        memcmp(&snapshot->bank, &retry_bank, sizeof(retry_bank)) == 0 &&
        memcmp(snapshot->slots, retry_slots, sizeof(retry_slots)) == 0;
    retry_gate_leave();
    CHECK(unchanged);
    if (!unchanged)
        fprintf(stderr, "FAIL state changed after refusal: %s\n", label);
}

static void test_ordinary_unpin_retries_and_detaches(void *page)
{
    struct Task *worker = guarded_identity(page, 64u);
    struct Task *requester = guarded_identity(page, 96u);
    struct Task *target = guarded_identity(page, 128u);
    struct p4_task_registry_record *record;
    struct p4_task_registry_record *finished;
    struct p4_remove_handle handle;
    int ready = 0;
    unsigned int pokes;

    retry_reset();
    retry_add_worker(worker);
    CHECK(retry_add_task(1u, requester) == 1);
    CHECK(retry_add_task(2u, target) == 1);
    CHECK(retry_add_ticket(requester, target, &handle) == 1);
    retry_set_pins(requester, UINT32_MAX);
    record = retry_make_retained(target);
    pokes = mock_poke_calls;

    CHECK(krnP4TaskRecordTryFinishPostFree(record, &retry_sysbase) == 0);
    retry_gate_enter();
    CHECK(record->service_postfree == 1u);
    CHECK(p4_removal_finish_waiting && !p4_cleanup_wake_pending);
    CHECK(retry_slots[handle.index].completion.requester_ref == 1u);
    CHECK(retry_slots[handle.index].completion.service_ref == 1u);
    CHECK(mock_poke_calls == pokes);
    retry_gate_leave();
    CHECK(mock_poke_calls == pokes); /* pressure alone must not self-poke */

    krnP4SignalTaskUnpin(requester, &retry_sysbase);
    record = retry_lookup(requester);
    retry_gate_enter();
    CHECK(record != NULL);
    CHECK(record->lifetime.notification_pins == UINT32_MAX - 1u);
    CHECK(p4_cleanup_wake_pending);
    retry_gate_leave();
    CHECK(mock_poke_calls == pokes + 1u);
    CHECK(!mock_gate_held && mock_irq_depth == 0u);

    finished = krnP4TaskRecordRetryPostFree(&retry_sysbase);
    CHECK(finished == &retry_records[2]);
    CHECK(retry_lookup(target) == NULL);
    retry_gate_enter();
    CHECK(p4_remove_bank_ready(&retry_bank, handle,
        (uintptr_t)requester, &ready) == 1);
    CHECK(ready == 1);
    record = p4_task_registry_lookup_locked(&p4_task_records, requester);
    CHECK(record != NULL);
    CHECK(record->semaphore_pending == 1u);
    CHECK(record->lifetime.notification_pins == UINT32_MAX);
    CHECK(retry_slots[handle.index].completion.requester_ref == 1u);
    CHECK(retry_slots[handle.index].completion.service_ref == 0u);
    CHECK(retry_slots[handle.index].completion.completed == 1u);
    CHECK(p4_removal_finish_waiting);
    retry_gate_leave();
    CHECK(mock_poke_calls == pokes + 2u);
    CHECK(!mock_gate_held && mock_irq_depth == 0u);
}

static void test_notification_finish_releases_capacity(void *page)
{
    struct Task *worker = guarded_identity(page, 64u);
    struct Task *requester = guarded_identity(page, 96u);
    struct Task *seed = guarded_identity(page, 128u);
    struct Task *target = guarded_identity(page, 160u);
    struct p4_task_registry_record *record;
    struct p4_task_registry_record *finished;
    struct p4_semaphore_notification batch;
    struct p4_remove_handle seed_handle, target_handle;
    unsigned int pokes;

    retry_reset();
    retry_add_worker(worker);
    CHECK(retry_add_task(1u, requester) == 1);
    CHECK(retry_add_task(2u, seed) == 1);
    CHECK(retry_add_task(3u, target) == 1);
    CHECK(retry_add_ticket(requester, seed, &seed_handle) == 1);
    record = retry_make_retained(seed);
    CHECK(retry_finish_leaf(record) == 2);
    CHECK(retry_claim_notification(&batch) == 1);
    CHECK(batch.identity == requester && batch.count == 1u);

    /* Keep a real inflight batch and model additional opaque references in
     * the pin count. Finishing the batch frees one unit for the later ACK. */
    retry_set_pins(requester, UINT32_MAX);
    CHECK(retry_add_ticket(requester, target, &target_handle) == 1);
    record = retry_make_retained(target);
    pokes = mock_poke_calls;
    CHECK(krnP4TaskRecordTryFinishPostFree(record, &retry_sysbase) == 0);
    retry_gate_enter();
    CHECK(p4_removal_finish_waiting);
    CHECK(retry_slots[target_handle.index].completion.service_ref == 1u);
    retry_gate_leave();

    krnP4SemaphoreNotificationFinish(worker, &batch, &retry_sysbase);
    retry_gate_enter();
    record = p4_task_registry_lookup_locked(&p4_task_records, requester);
    CHECK(record != NULL);
    CHECK(record->semaphore_inflight == 0u);
    CHECK(record->lifetime.notification_pins == UINT32_MAX - 1u);
    CHECK(p4_cleanup_wake_pending);
    retry_gate_leave();
    CHECK(mock_poke_calls == pokes + 1u);
    CHECK(!mock_gate_held && mock_irq_depth == 0u);

    finished = krnP4TaskRecordRetryPostFree(&retry_sysbase);
    CHECK(finished == &retry_records[3]);
    CHECK(retry_lookup(target) == NULL);
    retry_gate_enter();
    record = p4_task_registry_lookup_locked(&p4_task_records, requester);
    CHECK(record != NULL);
    CHECK(record->semaphore_pending == 1u);
    CHECK(record->lifetime.notification_pins == UINT32_MAX);
    CHECK(retry_slots[target_handle.index].completion.completed == 1u);
    CHECK(retry_slots[seed_handle.index].completion.completed == 1u);
    retry_gate_leave();
    CHECK(mock_poke_calls == pokes + 2u);
}

static void test_requester_cancellation_retries_without_signal(void *page)
{
    struct Task *worker = guarded_identity(page, 64u);
    struct Task *requester = guarded_identity(page, 96u);
    struct Task *target = guarded_identity(page, 128u);
    struct p4_task_registry_record *record;
    struct p4_task_registry_record *finished;
    struct p4_remove_handle handle;
    unsigned int pokes;

    retry_reset();
    retry_add_worker(worker);
    CHECK(retry_add_task(1u, requester) == 1);
    CHECK(retry_add_task(2u, target) == 1);
    CHECK(retry_add_ticket(requester, target, &handle) == 1);
    retry_set_pins(requester, UINT32_MAX);
    record = retry_make_retained(target);
    pokes = mock_poke_calls;
    CHECK(krnP4TaskRecordTryFinishPostFree(record, &retry_sysbase) == 0);
    CHECK(mock_poke_calls == pokes);

    /* Actual retirement cancels this requester's ticket under the same pin
     * gate. The service reference survives until target completion retry. */
    CHECK(krnP4TaskRecordRetire(requester, &retry_sysbase) == 1);
    CHECK(mock_poke_calls == pokes + 1u);
    retry_gate_enter();
    CHECK(retry_slots[handle.index].active == 1u);
    CHECK(retry_slots[handle.index].completion.requester_ref == 0u);
    CHECK(retry_slots[handle.index].completion.service_ref == 1u);
    CHECK(p4_cleanup_wake_pending);
    retry_gate_leave();

    finished = krnP4TaskRecordRetryPostFree(&retry_sysbase);
    CHECK(finished == &retry_records[2]);
    CHECK(retry_lookup(target) == NULL);
    retry_gate_enter();
    CHECK(retry_slots[handle.index].active == 0u);
    CHECK(retry_slots[handle.index].completion.requester_ref == 0u);
    CHECK(retry_slots[handle.index].completion.service_ref == 0u);
    CHECK(retry_slots[handle.index].completion.completed == 0u);
    retry_gate_leave();
    CHECK(mock_poke_calls == pokes + 1u); /* requester cancellation: no signal */
}

static void test_bounded_cursor_skips_blocked_and_restarts(void *page)
{
    struct Task *worker = guarded_identity(page, 64u);
    struct Task *blocked_requester = guarded_identity(page, 96u);
    struct Task *cancelled_requester = guarded_identity(page, 128u);
    struct Task *targets[10];
    struct p4_task_registry_record *finished;
    struct p4_remove_handle handles[10];
    unsigned int index;
    unsigned int pokes;

    retry_reset();
    retry_add_worker(worker);
    CHECK(retry_add_task(1u, blocked_requester) == 1);
    CHECK(retry_add_task(2u, cancelled_requester) == 1);
    retry_set_pins(blocked_requester, UINT32_MAX);
    retry_set_pins(cancelled_requester, UINT32_MAX);

    for (index = 0u; index < 10u; ++index)
    {
        unsigned int slot = index + 3u;
        struct Task *requester = index == 9u ? cancelled_requester :
                                                  blocked_requester;
        targets[index] = guarded_identity(page,
            (size_t)(160u + index * 32u));
        CHECK(retry_add_task(slot, targets[index]) == 1);
        CHECK(retry_add_ticket(requester, targets[index], &handles[index]) == 1);
        CHECK(retry_make_retained(targets[index]) == &retry_records[slot]);
        CHECK(krnP4TaskRecordTryFinishPostFree(&retry_records[slot],
            &retry_sysbase) == 0);
    }

    pokes = mock_poke_calls;
    finished = krnP4TaskRecordRetryPostFree(&retry_sysbase);
    CHECK(finished == NULL);
    retry_gate_enter();
    CHECK(p4_postfree_after == (uintptr_t)&retry_records[10]);
    CHECK(p4_postfree_blocked && p4_removal_finish_waiting);
    CHECK(p4_cleanup_wake_pending);
    retry_gate_leave();
    CHECK(mock_poke_calls == pokes + 1u); /* bounded continuation */
    CHECK(!mock_gate_held && mock_irq_depth == 0u);

    /* Make the last-address recipient disappear after that address was
     * deferred. Later blocked records must not hide this finishable record. */
    CHECK(krnP4TaskRecordRetire(cancelled_requester, &retry_sysbase) == 1);
    CHECK(mock_poke_calls == pokes + 2u);
    finished = krnP4TaskRecordRetryPostFree(&retry_sysbase);
    CHECK(finished == &retry_records[12]);
    CHECK(retry_lookup(targets[9]) == NULL);
    retry_gate_enter();
    CHECK(retry_slots[handles[9].index].active == 0u);
    CHECK(p4_postfree_after == (uintptr_t)&retry_records[12]);
    CHECK(p4_removal_finish_waiting && p4_postfree_restart);
    retry_gate_leave();

    /* The notification-pin drop belongs to a lower-address record already
     * visited by the cursor. The producer marks restart under the registry
     * gate; after a completed upper slice the lower record is revisited. */
    krnP4SignalTaskUnpin(blocked_requester, &retry_sysbase);
    retry_gate_enter();
    CHECK(p4_postfree_restart);
    CHECK(p4_cleanup_wake_pending);
    retry_gate_leave();
    finished = krnP4TaskRecordRetryPostFree(&retry_sysbase);
    CHECK(finished == NULL);
    retry_gate_enter();
    CHECK(p4_postfree_after == 0u);
    CHECK(!p4_postfree_restart);
    CHECK(p4_removal_finish_waiting);
    retry_gate_leave();

    finished = krnP4TaskRecordRetryPostFree(&retry_sysbase);
    CHECK(finished == &retry_records[3]);
    CHECK(retry_lookup(targets[0]) == NULL);
    retry_gate_enter();
    CHECK(retry_slots[handles[0].index].completion.completed == 1u);
    CHECK(retry_records[4].service_postfree == 1u);
    retry_gate_leave();
}

static void test_quiet_empty_scan_and_bad_leaf_inputs(void *page)
{
    struct Task *worker = guarded_identity(page, 64u);
    struct Task *target = guarded_identity(page, 96u);
    struct p4_task_registry_record *record;
    struct p4_task_registry_record *output;
    struct retry_snapshot before;
    unsigned int pokes;
    int result;

    retry_reset();
    retry_add_worker(worker);
    pokes = mock_poke_calls;
    retry_gate_enter();
    p4_removal_finish_waiting = true;
    p4_cleanup_wake_pending = true;
    retry_gate_leave();
    CHECK(krnP4TaskRecordRetryPostFree(&retry_sysbase) == NULL);
    retry_gate_enter();
    CHECK(!p4_removal_finish_waiting);
    CHECK(p4_cleanup_wake_pending); /* empty scan preserves another wake */
    CHECK(p4_postfree_after == 0u && !p4_postfree_restart);
    retry_gate_leave();
    CHECK(mock_poke_calls == pokes);

    retry_reset();
    retry_add_worker(worker);
    CHECK(retry_add_task(1u, target) == 1);
    record = retry_make_retained(target);
    retry_gate_enter();
    CHECK(p4_task_registry_defer_removal_finish_locked(
        &p4_task_records, record) == 1);
    retry_gate_leave();

    retry_snapshot_state(&before);
    retry_gate_enter();
    result = p4_task_registry_defer_removal_finish_locked(
        &p4_task_records, (struct p4_task_registry_record *)page);
    retry_gate_leave();
    CHECK(result == 0);
    retry_expect_unchanged(&before, "unknown retained record pointer");

    output = (struct p4_task_registry_record *)(uintptr_t)0x1234u;
    retry_gate_enter();
    result = p4_task_registry_next_postfree_after_locked(&p4_task_records,
        0u, (struct p4_task_registry_record **)(void *)&p4_task_records);
    retry_gate_leave();
    CHECK(result == -1);
    retry_expect_unchanged(&before, "postfree output aliases registry");
    CHECK(output == (struct p4_task_registry_record *)(uintptr_t)0x1234u);

    retry_gate_enter();
    result = p4_task_registry_next_postfree_after_locked(&p4_task_records,
        0u, &output);
    retry_gate_leave();
    CHECK(result == 1 && output == record);
    retry_gate_enter();
    record->service_postfree = 2u;
    retry_gate_leave();
    retry_snapshot_state(&before);
    output = (struct p4_task_registry_record *)(uintptr_t)0x5678u;
    retry_gate_enter();
    result = p4_task_registry_next_postfree_after_locked(&p4_task_records,
        0u, &output);
    retry_gate_leave();
    CHECK(result == -1);
    CHECK(output == (struct p4_task_registry_record *)(uintptr_t)0x5678u);
    retry_expect_unchanged(&before, "malformed postfree marker");
}

int main(void)
{
    long page_size;
    void *page;
    unsigned int baseline_checks;
    unsigned int retry_start;

    if (p4_remove_finish_retry_admission_baseline_main() != 0)
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

    retry_start = checks;
    test_ordinary_unpin_retries_and_detaches(page);
    test_notification_finish_releases_capacity(page);
    test_requester_cancellation_retries_without_signal(page);
    test_bounded_cursor_skips_blocked_and_restarts(page);
    test_quiet_empty_scan_and_bad_leaf_inputs(page);

    CHECK(munmap(page, (size_t)page_size) == 0);
    CHECK(!mock_gate_held && mock_irq_depth == 0u);
    CHECK(mock_fatal_calls == 0u);
    CHECK(mock_acquire_calls == mock_unlock_calls);
    CHECK(mock_disable_calls == mock_enable_calls);
    if (failures != 0u)
        return 1;
    printf("P4 removal post-free retry: %u new checks passed; "
           "baseline %u; total %u\n", checks - retry_start,
           baseline_checks, checks);
    return 0;
}
