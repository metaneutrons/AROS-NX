/*
 * Actual-source fixture for the ticket-aware scheduler-queue retirement
 * wrapper. Reuses the existing RetireLocked fixture for real bounded list
 * validation, real registry RUN-owner lookup, and the queue lock harness.
 * Registry/bank ticket admission remains an explicit mock boundary.
 */
#include <stdint.h>

#include "../../exec/p4_remove_bank.h"

struct Task;
struct ExecBase;
int krnP4TaskRecordAdmitRemoval(struct Task *, struct Task *, uint32_t,
    uint32_t, struct p4_remove_handle *, struct ExecBase *);
void krnP4CleanupPoke(struct ExecBase *);
void krnP4RetiredOwnerPoke(uint32_t, struct ExecBase *);

#define P4_TASK_REMOVE_TICKET_TEST 1
#define main p4_retire_queue_baseline_main
#include "p4-retire-queue-test.c"
#undef main

static struct Task fixture_requester;
static struct p4_remove_handle *expected_handle;
static struct p4_remove_handle mock_handle;
static uint32_t expected_hart;
static int mock_admit_result;
static unsigned int admit_calls;
static unsigned int unexpected_pokes;
static int admission_contract_error;

int krnP4TaskRecordAdmitRemoval(struct Task *requester, struct Task *target,
    uint32_t hart, uint32_t retire_live, struct p4_remove_handle *handle,
    struct ExecBase *sys_base)
{
    ++admit_calls;
    if (requester != &fixture_requester || target != &fixture_target ||
        hart != expected_hart || retire_live != 1u ||
        handle == NULL || handle != expected_handle ||
        sys_base != &fixture_sysbase || !mock_task_lock_held ||
        !mock_irq_disabled || lock_calls != LOCK_COUNT || unlock_calls != 0u ||
        fixture_sysbase.private_base.TaskReadySpinLock.held != 1u ||
        fixture_sysbase.private_base.TaskWaitSpinLock.held != 1u ||
        fixture_sysbase.private_base.TaskRunningSpinLock.held != 1u)
        admission_contract_error = 1;

    if (mock_admit_result == 1)
        *handle = mock_handle;
    return mock_admit_result;
}

void krnP4CleanupPoke(struct ExecBase *sys_base)
{
    (void)sys_base;
    ++unexpected_pokes;
}

void krnP4RetiredOwnerPoke(uint32_t hart, struct ExecBase *sys_base)
{
    (void)hart;
    (void)sys_base;
    ++unexpected_pokes;
}

static void reset_ticket_mocks(struct p4_remove_handle *handle,
    uint32_t hart, int admission_result)
{
    memset(&fixture_requester, 0, sizeof(fixture_requester));
    expected_handle = handle;
    expected_hart = hart;
    mock_handle.index = UINT32_C(0x1234);
    mock_handle.generation = UINT32_C(0x5678);
    mock_admit_result = admission_result;
    admit_calls = 0u;
    unexpected_pokes = 0u;
    admission_contract_error = 0;
}

static int call_ticket_retire(struct Task *target, struct Task *requester,
    uint32_t hart, struct p4_remove_handle *handle, struct ExecBase *sys_base)
{
    int result;
    reset_trace();
    if (target != NULL && requester != NULL && sys_base != NULL &&
        handle != NULL && target != requester && hart <= 1u)
    {
        mock_irq_disabled = 1;
        mock_task_lock_held = 1;
    }
    result = krnP4TaskRetireWithTicketLocked(target, requester, hart, handle,
                                             sys_base);
    if (target != NULL && requester != NULL && sys_base != NULL &&
        handle != NULL && target != requester && hart <= 1u)
        CHECK(lock_trace_is_correct());
    else
    {
        CHECK(lock_calls == 0u);
        CHECK(unlock_calls == 0u);
        CHECK(!lock_contract_error);
    }
    CHECK(fixture_sysbase.private_base.TaskReadySpinLock.held == 0u);
    CHECK(fixture_sysbase.private_base.TaskWaitSpinLock.held == 0u);
    CHECK(fixture_sysbase.private_base.TaskRunningSpinLock.held == 0u);
    mock_task_lock_held = 0;
    mock_irq_disabled = 0;
    return result;
}

static void setup_live_target(unsigned int state, int run_hart)
{
    reset_fixture();
    memset(&fixture_requester, 0, sizeof(fixture_requester));
    enqueue_target(state);
    CHECK(register_and_pin(1u)); /* caller's ordinary target pin */
    if (run_hart >= 0)
        CHECK(p4_task_registry_enter_running_locked(&fixture_registry,
            &fixture_target, (uint32_t)run_hart));
}

static void capture_handle(struct p4_remove_handle *handle,
    struct p4_remove_handle *before)
{
    before->index = handle->index;
    before->generation = handle->generation;
}

static int handle_equal(const struct p4_remove_handle *left,
    const struct p4_remove_handle *right)
{
    return left->index == right->index &&
           left->generation == right->generation;
}

static void check_success(unsigned int state, int run_hart, uint32_t caller_hart)
{
    struct fixture_image before;
    struct p4_remove_handle handle = { UINT32_C(0xa1a1), UINT32_C(0xb2b2) };
    struct p4_remove_handle old_handle;
    unsigned int expected_owner = run_hart < 0 ? 0u : (unsigned int)run_hart + 1u;

    setup_live_target(state, run_hart);
    capture_image(&before);
    capture_handle(&handle, &old_handle);
    reset_ticket_mocks(&handle, caller_hart, 1);

    CHECK(call_ticket_retire(&fixture_target, &fixture_requester, caller_hart,
                             &handle, &fixture_sysbase) == 1);
    CHECK(admit_calls == 1u);
    CHECK(!admission_contract_error);
    CHECK(owner_calls == 1u);
    CHECK(owner_result == (int)expected_owner);
    CHECK(fixture_target.tc_State == TS_REMOVED);
    check_queue_detached();
    CHECK(handle_equal(&handle, &mock_handle));
    CHECK(!handle_equal(&handle, &old_handle));
    CHECK(unexpected_pokes == 0u);
    CHECK(etask_reads == 0u);
    /* The mocked admission leaves registry retirement itself unclaimed. */
    CHECK(memcmp(&before.registry, &fixture_registry,
                 sizeof(fixture_registry)) == 0);
    CHECK(memcmp(&before.record, &fixture_record,
                 sizeof(fixture_record)) == 0);
    CHECK(fixture_record.lifetime.flags == 0u);
    CHECK(fixture_record.lifetime.notification_pins == 1u);
    CHECK(fixture_registry.head == &fixture_record);
    (void)before;
}

static void test_ready_wait_and_run_acceptance(void)
{
    check_success(TS_READY, -1, 0u);
    check_success(TS_WAIT, -1, 1u);
    check_success(TS_RUN, 0, 1u);
    check_success(TS_RUN, 1, 0u);
}

static void test_admission_refusal_is_atomic_at_queue_boundary(void)
{
    struct fixture_image before;
    struct p4_remove_handle handle = { UINT32_C(0xc3c3), UINT32_C(0xd4d4) };
    struct p4_remove_handle old_handle;

    setup_live_target(TS_READY, -1);
    capture_image(&before);
    capture_handle(&handle, &old_handle);
    reset_ticket_mocks(&handle, 0u, 0);
    CHECK(call_ticket_retire(&fixture_target, &fixture_requester, 0u,
                             &handle, &fixture_sysbase) == 0);
    CHECK(admit_calls == 1u);
    CHECK(!admission_contract_error);
    CHECK(image_unchanged(&before));
    CHECK(handle_equal(&handle, &old_handle));
    CHECK(unexpected_pokes == 0u);
}

static void expect_pre_admission_refusal(const char *label)
{
    struct fixture_image before;
    struct p4_remove_handle handle = { UINT32_C(0xe5e5), UINT32_C(0xf6f6) };
    struct p4_remove_handle old_handle;

    capture_image(&before);
    capture_handle(&handle, &old_handle);
    reset_ticket_mocks(&handle, 0u, 1);
    CHECK(call_ticket_retire(&fixture_target, &fixture_requester, 0u,
                             &handle, &fixture_sysbase) == 0);
    CHECK(admit_calls == 0u);
    CHECK(owner_calls == 1u);
    CHECK(image_unchanged(&before));
    CHECK(handle_equal(&handle, &old_handle));
    CHECK(unexpected_pokes == 0u);
    (void)label;
}

static void test_wrong_membership_and_owner_refuse_before_admission(void)
{
    setup_live_target(TS_READY, -1);
    /* A second list head aliases the same node: its predecessor cannot match
     * that list's sentinel, so the bounded validator rejects it. */
    fixture_sysbase.TaskWait.lh_Head = &fixture_target.tc_Node;
    fixture_sysbase.TaskWait.lh_TailPred = &fixture_target.tc_Node;
    expect_pre_admission_refusal("double-list alias");

    setup_live_target(TS_WAIT, -1);
    fixture_target.tc_State = TS_READY;
    expect_pre_admission_refusal("state/list mismatch");

    setup_live_target(TS_READY, 0);
    expect_pre_admission_refusal("READY with a live RUN owner");
    CHECK(owner_result == 1);

    setup_live_target(TS_RUN, -1);
    expect_pre_admission_refusal("RUN without a live RUN owner");
    CHECK(owner_result == 0);

    setup_live_target(TS_READY, -1);
    fixture_sysbase.TaskWait.lh_Tail = &fixture_other.tc_Node;
    expect_pre_admission_refusal("malformed unrelated queue");
}

static void test_missing_record_and_bad_arguments_refuse(void)
{
    struct p4_remove_handle handle = { 7u, 9u };
    struct p4_remove_handle old_handle;

    reset_fixture();
    enqueue_target(TS_READY);
    capture_handle(&handle, &old_handle);
    reset_ticket_mocks(&handle, 0u, 1);
    expect_pre_admission_refusal("missing target record");
    CHECK(owner_result == -1);

    setup_live_target(TS_READY, -1);
    reset_ticket_mocks(&handle, 0u, 1);
    CHECK(call_ticket_retire(&fixture_target, &fixture_target, 0u,
        &handle, &fixture_sysbase) == 0);
    CHECK(admit_calls == 0u);
    CHECK(handle_equal(&handle, &old_handle));

    CHECK(call_ticket_retire(NULL, &fixture_requester, 0u,
        &handle, &fixture_sysbase) == 0);
    CHECK(call_ticket_retire(&fixture_target, &fixture_requester, 2u,
        &handle, &fixture_sysbase) == 0);
    CHECK(call_ticket_retire(&fixture_target, &fixture_requester, 0u,
        NULL, &fixture_sysbase) == 0);
    CHECK(admit_calls == 0u);
    CHECK(handle_equal(&handle, &old_handle));
}

int main(void)
{
    unsigned int baseline_checks;

    if (p4_retire_queue_baseline_main() != EXIT_SUCCESS)
        return EXIT_FAILURE;
    baseline_checks = checks;

#if defined(P4_REMOVE_TICKET_QUEUE_NEGATIVE_CONTROL)
    CHECK(0 && "intentional ticket-fixture negative control");
#endif

    test_ready_wait_and_run_acceptance();
    test_admission_refusal_is_atomic_at_queue_boundary();
    test_wrong_membership_and_owner_refuse_before_admission();
    test_missing_record_and_bad_arguments_refuse();

    printf("P4 TaskRetireWithTicketLocked: %u new checks passed; %u total\n",
           checks - baseline_checks, checks);
    return EXIT_SUCCESS;
}
