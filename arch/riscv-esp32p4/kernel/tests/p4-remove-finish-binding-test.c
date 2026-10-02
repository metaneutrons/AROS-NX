/* Actual removal-ticket ACK plus retained-service finish integration. */
#define main p4_remove_finish_admission_baseline_main
#include "p4-semaphore-operation-admission-test.c"
#undef main

#include "../../exec/p4_remove_bank.c"
#include "../../exec/p4_remove_completion.c"

void __cyg_profile_func_enter(void *, void *)
    __attribute__((no_instrument_function));
void __cyg_profile_func_exit(void *, void *)
    __attribute__((no_instrument_function));

#define FINISH_RECORDS 5u
#define FINISH_TICKETS 4u

static struct p4_task_registry_record finish_records[FINISH_RECORDS];
static struct p4_semaphore_ledger finish_ledger;
static struct p4_semaphore_ledger_operation finish_operations[TEST_OP_CAPACITY];
static struct p4_semaphore_ledger_entry finish_entries[TEST_ENTRY_CAPACITY];
static struct ExecBase finish_sysbase;
static struct p4_remove_bank finish_bank;
static struct p4_remove_bank_slot finish_slots[8];
static struct Task *finish_target;
static uint32_t finish_target_generation;
static struct p4_remove_handle finish_tickets[FINISH_TICKETS];
static unsigned int finish_unlock_armed;
static unsigned int finish_unlock_seen;
static unsigned int finish_unlocks_before;
static unsigned int finish_pokes_before;

static void finish_check(int condition, const char *label)
{
    ++checks;
    if (!condition)
    {
        ++failures;
        fprintf(stderr, "FAIL remove-finish-binding: %s\n", label);
    }
}

static int registry_has_identity(const struct Task *identity)
{
    struct p4_task_registry_record *record = p4_task_records.head;
    uint32_t visited = 0u;

    while (record != NULL && visited < p4_task_records.count &&
           visited < P4_TASK_REGISTRY_LIMIT)
    {
        if (record->identity == identity)
            return 1;
        record = record->next;
        ++visited;
    }
    return record != NULL || visited != p4_task_records.count ? -1 : 0;
}

/* With -finstrument-functions this runs immediately after the mock spin
 * unlock returns, before pins.c calls Enable() or the post-unlock poke. */
void __cyg_profile_func_enter(void *function, void *call_site)
{
    (void)function;
    (void)call_site;
}

void __cyg_profile_func_exit(void *function, void *call_site)
{
    (void)function;
    (void)call_site;

    if (finish_unlock_armed != 0u && !mock_gate_held && mock_irq_depth != 0u &&
        mock_unlock_calls > finish_unlocks_before)
    {
        finish_unlock_armed = 0u;
        ++finish_unlock_seen;

        finish_check(mock_irq_depth == 1u,
                     "finish unlock observer runs with IRQs still masked");
        finish_check(!mock_gate_held && p4_task_pin_gate.lock == 0u,
                     "finish transaction has released the registry gate");
        finish_check(!mock_task_lock_held &&
                     !mock_outer_semaphore_gate_held,
                     "finish is outside Task and semaphore locks");
        finish_check(mock_poke_calls == finish_pokes_before,
                     "no wake poke occurs before the finish unlock");
        finish_check(p4_cleanup_wake_pending,
                     "durable worker readiness is latched before unlock");
        finish_check(p4_task_records.count == 4u &&
                     registry_has_identity(finish_target) == 0,
                     "target record is detached in the ACK transaction");

        finish_check(finish_slots[finish_tickets[0].index].active == 0u &&
                     finish_slots[finish_tickets[0].index].completion.service_ref == 0u,
                     "cancelled requester's target ticket is reclaimed");
        finish_check(finish_slots[finish_tickets[1].index].active == 1u &&
                     finish_slots[finish_tickets[1].index].completion.requester_ref == 1u &&
                     finish_slots[finish_tickets[1].index].completion.service_ref == 0u &&
                     finish_slots[finish_tickets[1].index].completion.completed == 1u,
                     "first live requester's ticket is ACKed");
        finish_check(finish_slots[finish_tickets[2].index].active == 1u &&
                     finish_slots[finish_tickets[2].index].completion.requester_ref == 1u &&
                     finish_slots[finish_tickets[2].index].completion.service_ref == 0u &&
                     finish_slots[finish_tickets[2].index].completion.completed == 1u,
                     "second live requester's ticket is ACKed");
        finish_check(finish_slots[finish_tickets[3].index].active == 1u &&
                     finish_slots[finish_tickets[3].index].completion.requester_ref == 1u &&
                     finish_slots[finish_tickets[3].index].completion.service_ref == 0u &&
                     finish_slots[finish_tickets[3].index].completion.completed == 1u,
                     "first live requester’s coalesced ticket is ACKed");

        finish_check(finish_records[2].semaphore_pending == 2u &&
                     finish_records[2].semaphore_pending_generation != 0u &&
                     finish_records[2].semaphore_inflight == 0u &&
                     finish_records[2].lifetime.notification_pins == 2u,
                     "first recipient batch and exact pins are reserved");
        finish_check(finish_records[3].semaphore_pending == 1u &&
                     finish_records[3].semaphore_pending_generation != 0u &&
                     finish_records[3].semaphore_inflight == 0u &&
                     finish_records[3].lifetime.notification_pins == 1u,
                     "second recipient batch and exact pin are reserved");
        finish_check(finish_records[2].semaphore_pending_generation ==
                         finish_target_generation &&
                     finish_records[3].semaphore_pending_generation ==
                         finish_target_generation &&
                     finish_records[2].semaphore_pending_generation ==
                         finish_records[3].semaphore_pending_generation &&
                     p4_task_records.semaphore_notification_serial ==
                         finish_target_generation,
                     "recipient batches share the target's reserved generation");
    }
}

static void publish_finish_task(unsigned int slot, struct Task *identity)
{
    CHECK(slot < FINISH_RECORDS);
    CHECK(krnP4TaskRecordPublish(identity, &finish_records[slot],
                                 &finish_sysbase) == 1);
}

static int reserve_ticket(struct Task *requester, struct Task *target,
                          struct p4_remove_handle *handle)
{
    int result;
    p4_task_pins_lock(&finish_sysbase);
    result = p4_remove_bank_reserve(&finish_bank, (uintptr_t)requester,
                                    (uintptr_t)target, handle);
    p4_task_pins_unlock(&finish_sysbase);
    return result;
}

static int bank_target_count(struct Task *target, uint32_t *out_count)
{
    int result;
    p4_task_pins_lock(&finish_sysbase);
    result = p4_remove_bank_count_target(&finish_bank, (uintptr_t)target,
                                         out_count);
    p4_task_pins_unlock(&finish_sysbase);
    return result;
}

static void reset_finish_state(void)
{
    memset(finish_records, 0, sizeof(finish_records));
    memset(&finish_ledger, 0, sizeof(finish_ledger));
    memset(finish_operations, 0, sizeof(finish_operations));
    memset(finish_entries, 0, sizeof(finish_entries));
    memset(test_boot_requests, 0, sizeof(test_boot_requests));
    memset(&finish_bank, 0, sizeof(finish_bank));
    memset(finish_slots, 0, sizeof(finish_slots));
    memset(&finish_sysbase, 0, sizeof(finish_sysbase));
    p4_task_registry_init(&p4_task_records);
    CHECK(krnP4PublicSpinInit(&p4_task_pin_gate) == P4_PUBLIC_SPIN_OK);
    CHECK(p4_semaphore_ledger_init(&finish_ledger, finish_operations,
        TEST_OP_CAPACITY, finish_entries, TEST_ENTRY_CAPACITY) == 1);
    test_boot_ledger = &finish_ledger;
    p4_cleanup_worker = NULL;
    p4_cleanup_wake_mask = 0u;
    p4_cleanup_wake_pending = false;
    p4_cancel_retry.active = 0u;
    p4_cancel_retry.waiting = 0u;
    p4_removal_bank = NULL;
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
    finish_unlock_armed = 0u;
    finish_unlock_seen = 0u;
    finish_unlocks_before = 0u;
    finish_pokes_before = 0u;
    finish_target_generation = 0u;
}

static int test_finish_ack_and_deliveries(void *page)
{
    struct Task *worker = (struct Task *)((unsigned char *)page + 64u);
    struct Task *target = (struct Task *)((unsigned char *)page + 128u);
    struct Task *live_a = (struct Task *)((unsigned char *)page + 192u);
    struct Task *live_b = (struct Task *)((unsigned char *)page + 256u);
    struct Task *cancelled = (struct Task *)((unsigned char *)page + 320u);
    struct p4_cleanup_wake wake;
    struct p4_semaphore_notification notification;
    struct p4_task_registry_record *retained;
    uint32_t target_tickets = UINT32_MAX;
    unsigned int pokes_before;
    int retire_result;

    reset_finish_state();
    finish_target = target;

    CHECK(p4_task_pins_prepared == 1);
    CHECK(p4_remove_bank_init(&finish_bank, finish_slots, 8u) == 1);
    /* Startup's pre-Exec attachment runs with raw MIE off and no Exec gates. */
    mock_irq_depth = 1u;
    CHECK(krnP4TaskPinsAttachRemovalBank(&finish_bank) == 1);
    CHECK(mock_irq_depth == 1u && !mock_gate_held);
    mock_irq_depth = 0u;

    publish_finish_task(0u, worker);
    publish_finish_task(1u, target);
    publish_finish_task(2u, live_a);
    publish_finish_task(3u, live_b);
    publish_finish_task(4u, cancelled);

    CHECK(krnP4TaskRecordEnterRunning(worker, 0u, &finish_sysbase) == 1);
    CHECK(krnP4ProtectCleanupWorker(worker, &finish_sysbase) == 1);
    CHECK(krnP4ArmCleanupWake(worker, UINT32_C(1) << 4,
                              &finish_sysbase) == 1);
    CHECK(krnP4TaskRecordEnterRunning(target, 1u, &finish_sysbase) == 1);

    CHECK(reserve_ticket(live_a, target, &finish_tickets[1]) == 1);
    CHECK(reserve_ticket(live_b, target, &finish_tickets[2]) == 1);
    CHECK(reserve_ticket(cancelled, target, &finish_tickets[0]) == 1);
    CHECK(reserve_ticket(live_a, target, &finish_tickets[3]) == 1);
    CHECK(bank_target_count(target, &target_tickets) == 1);
    CHECK(target_tickets == FINISH_TICKETS);

    /* Retiring one requester cancels only its requester references. The
     * target service reference survives for the retained cleanup transaction. */
    Disable();
    mock_task_lock_held = 1;
    retire_result = krnP4TaskRecordRetire(cancelled, &finish_sysbase);
    mock_task_lock_held = 0;
    Enable();
    CHECK(retire_result == 1);
    CHECK(finish_records[4].lifetime.flags == P4_TASK_LIFETIME_RETIRED);
    CHECK(finish_slots[finish_tickets[0].index].active == 1u &&
          finish_slots[finish_tickets[0].index].completion.requester_ref == 0u &&
          finish_slots[finish_tickets[0].index].completion.service_ref == 1u);

    /* Task removal retains the target through its live hart's final RUN
     * release; only the protected worker can claim and finish afterward. */
    Disable();
    mock_task_lock_held = 1;
    retire_result = krnP4TaskRecordRetireForService(target, &finish_sysbase);
    mock_task_lock_held = 0;
    Enable();
    CHECK(retire_result == 1);
    finish_target_generation = finish_records[1].service_generation;
    CHECK(finish_target_generation != 0u);
    CHECK(finish_records[1].service_pending == 1u &&
          finish_records[1].lifetime.notification_pins == 1u &&
          finish_records[1].lifetime.running_harts == 2u);
    CHECK(mock_retired_poke_calls == 1u);
    CHECK(!p4_cleanup_wake_pending);

    krnP4TaskRecordLeaveRunning(target, 1u, &finish_sysbase);
    CHECK(finish_records[1].lifetime.running_harts == 0u);
    CHECK(finish_records[1].service_queued == 1u);
    CHECK(p4_cleanup_wake_pending && mock_poke_calls == 1u);
    CHECK(krnP4CleanupWorkerReady(&finish_sysbase) == 1);
    memset(&wake, 0, sizeof(wake));
    CHECK(krnP4TakeCleanupWake(&wake, &finish_sysbase) == 1);
    CHECK(wake.worker == worker && wake.mask == (UINT32_C(1) << 4));
    CHECK(!p4_cleanup_wake_pending);

    retained = krnP4TaskRecordClaimNextRetainedService(&finish_sysbase);
    CHECK(retained == &finish_records[1]);
    CHECK(finish_records[1].service_inflight == 1u);
    CHECK(finish_records[1].lifetime.flags ==
          (P4_TASK_LIFETIME_RETIRED | P4_TASK_LIFETIME_CLAIMED));
    CHECK(registry_has_identity(target) == 1);
    CHECK(mock_task_lock_held == 0 && !mock_outer_semaphore_gate_held);

    finish_pokes_before = mock_poke_calls;
    finish_unlocks_before = mock_unlock_calls;
    finish_unlock_armed = 1u;
    krnP4TaskRecordFinishRetainedService(retained, &finish_sysbase);
    CHECK(finish_unlock_armed == 0u && finish_unlock_seen == 1u);
    CHECK(mock_poke_calls == finish_pokes_before + 1u);
    CHECK(!mock_gate_held && mock_irq_depth == 0u);
    CHECK(finish_records[1].service_inflight == 0u &&
          finish_records[1].next == NULL);
    CHECK(registry_has_identity(target) == 0);
    CHECK(finish_slots[finish_tickets[0].index].active == 0u);
    CHECK(finish_records[2].semaphore_pending == 2u);
    CHECK(finish_records[3].semaphore_pending == 1u);
    CHECK(p4_cleanup_wake_pending);

    /* Consume the finish wake, then drain both actual per-recipient batches
     * through the protected RUN worker's registry Claim/Finish APIs. */
    memset(&wake, 0, sizeof(wake));
    CHECK(krnP4TakeCleanupWake(&wake, &finish_sysbase) == 1);
    CHECK(wake.worker == worker && wake.mask == (UINT32_C(1) << 4));
    CHECK(krnP4TaskRecordRunningOwner(worker, &finish_sysbase) == 1);
    pokes_before = mock_poke_calls;

    memset(&notification, 0, sizeof(notification));
    CHECK(krnP4SemaphoreNotificationClaim(worker, &notification,
                                           &finish_sysbase) == 1);
    CHECK(notification.identity == live_b && notification.count == 1u &&
          notification.generation == finish_records[3].semaphore_inflight_generation);
    CHECK(finish_records[3].semaphore_pending == 0u &&
          finish_records[3].semaphore_inflight == 1u &&
          finish_records[3].lifetime.notification_pins == 1u);
    krnP4SemaphoreNotificationFinish(worker, &notification, &finish_sysbase);
    CHECK(finish_records[3].semaphore_inflight == 0u &&
          finish_records[3].semaphore_inflight_generation == 0u &&
          finish_records[3].lifetime.notification_pins == 0u);

    memset(&notification, 0, sizeof(notification));
    CHECK(krnP4SemaphoreNotificationClaim(worker, &notification,
                                           &finish_sysbase) == 1);
    CHECK(notification.identity == live_a && notification.count == 2u &&
          notification.generation == finish_records[2].semaphore_inflight_generation);
    CHECK(finish_records[2].semaphore_pending == 0u &&
          finish_records[2].semaphore_inflight == 2u &&
          finish_records[2].lifetime.notification_pins == 2u);
    krnP4SemaphoreNotificationFinish(worker, &notification, &finish_sysbase);
    CHECK(finish_records[2].semaphore_inflight == 0u &&
          finish_records[2].semaphore_inflight_generation == 0u &&
          finish_records[2].lifetime.notification_pins == 0u);
    CHECK(krnP4SemaphoreNotificationClaim(worker, &notification,
                                           &finish_sysbase) == 0);
    CHECK(notification.identity == NULL && notification.count == 0u &&
          notification.generation == 0u);
    CHECK(mock_poke_calls == pokes_before);

    CHECK(finish_records[4].lifetime.flags == P4_TASK_LIFETIME_RETIRED &&
          finish_records[4].lifetime.notification_pins == 0u &&
          finish_records[4].semaphore_pending == 0u &&
          finish_records[4].semaphore_inflight == 0u);
    CHECK(finish_records[0].worker_protected == 1u &&
          finish_records[0].lifetime.notification_pins == 1u &&
          finish_records[0].lifetime.running_harts == 1u);
    CHECK(bank_target_count(target, &target_tickets) == 1 &&
          target_tickets == 0u);
    CHECK(mock_fatal_calls == 0u && !mock_gate_held &&
          !mock_outer_semaphore_gate_held && mock_irq_depth == 0u);
    CHECK(mock_disable_calls == mock_enable_calls);
    CHECK(mock_acquire_calls == mock_unlock_calls);

    return failures == 0u;
}

int main(void)
{
    long page_size;
    void *page;
    unsigned int baseline_checks;
    int result;

    if (p4_remove_finish_admission_baseline_main() != 0)
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

    result = test_finish_ack_and_deliveries(page);
    if (munmap(page, (size_t)page_size) != 0)
        return 2;
    if (failures != 0u || result == 0)
        return 1;
    printf("P4 removal finish binding: %u new checks passed; admission baseline %u\n",
        checks - baseline_checks, baseline_checks);
    return 0;
}
