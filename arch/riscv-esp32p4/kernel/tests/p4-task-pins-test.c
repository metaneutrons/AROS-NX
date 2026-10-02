/* Host fixture for the actual P4 Task-pin registry binding. */
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif

#define P4_TASK_PINS_TEST 1
#define P4_PUBLIC_SPINLOCK_TEST 1

#include "../p4_public_spinlock.h"
#include "../p4_task_registry.h"

struct Task
{
    uint32_t fixture_word;
};

/* Only the direct private binding field is used by Observe. Other existing
 * pin operations still treat their synthetic ExecBase values opaquely. */
struct Library { uint32_t fixture_word; };
struct ExecBase { struct Library *lb_TaskResBase; };

static unsigned int checks;
static unsigned int failures;

#define CHECK(condition)                                                     \
    do                                                                       \
    {                                                                        \
        ++checks;                                                            \
        if (!(condition))                                                    \
        {                                                                    \
            ++failures;                                                     \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,       \
                    #condition);                                            \
        }                                                                    \
    } while (0)

static int mock_mie = 1;
static unsigned int mock_disable_depth;
static unsigned int mock_enable_calls;
static unsigned int mock_enable_gate_violations;
static unsigned int mock_registry_calls;
static unsigned int mock_registry_service_retires;
static unsigned int mock_registry_service_claims;
static unsigned int mock_registry_service_ready;
static unsigned int mock_registry_worker_protects;
static unsigned int mock_registry_worker_checks;
static unsigned int mock_registry_context_violations;
enum wait_registry_call
{
    WAIT_REG_BEGIN = 0,
    WAIT_REG_ADD,
    WAIT_REG_CLOSE,
    WAIT_REG_READY,
    WAIT_REG_GRANT,
    WAIT_REG_WITHDRAW,
    WAIT_REG_FINISH,
    WAIT_REG_CANCEL_READY,
    WAIT_REG_CLAIM_CANCEL,
    WAIT_REG_FINISH_CANCEL,
    WAIT_REG_RELEASE_CANCEL,
    WAIT_REG_CALL_COUNT
};
static unsigned int mock_wait_registry_calls[WAIT_REG_CALL_COUNT];
static unsigned int mock_registry_init_calls;
static unsigned int mock_spin_init_calls;
static unsigned int mock_try_calls;
static unsigned int mock_unlock_calls;
static unsigned int mock_callback_calls;
static int mock_gate_held;
static void (*mock_enable_callback)(void);
static struct p4_task_registry *mock_test_registry;
static struct Task *cleanup_worker_identity;
static struct p4_task_registry_record cleanup_worker_record;
static void *cleanup_worker_mapping;
static size_t cleanup_worker_mapping_size;
static unsigned int mock_cleanup_poke_calls;
static unsigned int mock_owner_poke_calls[2];
static int mock_owner_poke_mie;
static unsigned int mock_owner_poke_depth;
static struct ExecBase *mock_owner_poke_base;
static unsigned int mock_cleanup_poke_context_violations;
static int mock_cleanup_poke_expected_mie = 1;
static unsigned int mock_cleanup_poke_expected_disable_depth;
static struct ExecBase *mock_cleanup_poke_expected_base;
static int mock_cleanup_poke_observed_mie;
static unsigned int mock_cleanup_poke_observed_disable_depth;

static enum p4_public_spinlock_result mock_spin_init_result =
    P4_PUBLIC_SPIN_OK;
static enum p4_public_spinlock_result mock_spin_unlock_result =
    P4_PUBLIC_SPIN_OK;
static enum p4_public_spinlock_result mock_try_script[8];
static unsigned int mock_try_script_count;
static unsigned int mock_try_script_index;
static enum p4_public_spinlock_result mock_try_fallback =
    P4_PUBLIC_SPIN_OK;

static jmp_buf mock_fatal_jump;
static int mock_fatal_armed;
static unsigned int mock_fatal_calls;
static enum p4_public_spinlock_result mock_fatal_reason;
static int mock_fatal_mie_before;
static int mock_fatal_mie_after;

void Disable(void);
void Enable(void);
void krnP4CleanupPoke(struct ExecBase *SysBase);
void krnP4RetiredOwnerPoke(uint32_t destination, struct ExecBase *SysBase);

#include "../p4_task_registry.c"
#include "../p4_task_lifetime.c"

/* Interpose only the production binding's registry calls. Operational calls
 * check the gate/IRQ contract; init separately checks masked, prepublication
 * initialization. All wrappers delegate to the real registry above. */
void p4TaskPinsTestRegistryInit(struct p4_task_registry *registry);
int p4TaskPinsTestRegistryRegister(
    struct p4_task_registry *registry,
    struct p4_task_registry_record *record,
    struct Task *identity);
int p4TaskPinsTestRegistryPin(
    struct p4_task_registry *registry, struct Task *identity);
int p4TaskPinsTestRegistryUnpin(
    struct p4_task_registry *registry, struct Task *identity);
int p4TaskPinsTestRegistryProtectWorker(
    struct p4_task_registry *registry, struct Task *identity);
int p4TaskPinsTestRegistryIsWorker(
    struct p4_task_registry *registry, struct Task *identity);
int p4TaskPinsTestRegistryRetire(
    struct p4_task_registry *registry, struct Task *identity);
int p4TaskPinsTestRegistryRetireForService(
    struct p4_task_registry *registry, struct Task *identity);
int p4TaskPinsTestRegistryEnterRunning(
    struct p4_task_registry *registry, struct Task *identity, uint32_t hart);
int p4TaskPinsTestRegistryLeaveRunning(
    struct p4_task_registry *registry, struct Task *identity, uint32_t hart);
struct p4_task_registry_record *p4TaskPinsTestRegistryClaimReclaim(
    struct p4_task_registry *registry, struct Task *identity);
struct p4_task_registry_record *p4TaskPinsTestRegistryClaimService(
    struct p4_task_registry *registry, struct Task *identity);
struct p4_task_registry_record *p4TaskPinsTestRegistryClaimQueuedService(
    struct p4_task_registry *registry, struct Task *identity);
struct Task *p4TaskPinsTestRegistryTakeServiceReady(
    struct p4_task_registry *registry, struct Task *identity);
int p4TaskPinsTestRegistryClaimNextService(
    struct p4_task_registry *registry,
    struct p4_task_registry_record **out_record);
int p4TaskPinsTestRegistryServiceOwnerRunning(
    struct p4_task_registry *registry, struct Task *identity, uint32_t hart);
int p4TaskPinsTestRegistryRunningOwner(
    struct p4_task_registry *registry, struct Task *identity);
int p4TaskPinsTestRegistryClaimRetained(
    struct p4_task_registry *, struct p4_task_registry_record **);
int p4TaskPinsTestRegistryFinishRetained(
    struct p4_task_registry *, struct p4_task_registry_record *);
int p4TaskPinsTestRegistryWaitBegin(
    struct p4_task_registry *, struct Task *, uint32_t, const void *,
    const void *, const void *, uint32_t *);
int p4TaskPinsTestRegistryWaitAddRequest(
    struct p4_task_registry *, struct Task *, uint32_t);
int p4TaskPinsTestRegistryWaitClose(
    struct p4_task_registry *, struct Task *, uint32_t);
int p4TaskPinsTestRegistryWaitReady(
    struct p4_task_registry *, struct Task *, uint32_t);
int p4TaskPinsTestRegistryWaitGrantRequest(
    struct p4_task_registry *, struct Task *, uint32_t);
int p4TaskPinsTestRegistryWaitWithdrawRequest(
    struct p4_task_registry *, struct Task *, uint32_t);
int p4TaskPinsTestRegistryWaitFinish(
    struct p4_task_registry *, struct Task *, uint32_t);
int p4TaskPinsTestRegistryWaitCancelReady(
    struct p4_task_registry *, struct Task *);
int p4TaskPinsTestRegistryClaimNextWaitCancel(
    struct p4_task_registry *, struct p4_task_wait_cancel_snapshot *);
int p4TaskPinsTestRegistryWaitFinishCancel(
    struct p4_task_registry *, struct Task *, uint32_t);
int p4TaskPinsTestRegistryWaitReleaseCancel(
    struct p4_task_registry *, struct Task *, uint32_t);

#define p4_task_registry_init p4TaskPinsTestRegistryInit
#define p4_task_registry_register_locked p4TaskPinsTestRegistryRegister
#define p4_task_registry_pin_locked p4TaskPinsTestRegistryPin
#define p4_task_registry_unpin_locked p4TaskPinsTestRegistryUnpin
#define p4_task_registry_protect_worker_locked \
    p4TaskPinsTestRegistryProtectWorker
#define p4_task_registry_is_worker_locked p4TaskPinsTestRegistryIsWorker
#define p4_task_registry_retire_locked p4TaskPinsTestRegistryRetire
#define p4_task_registry_retire_for_service_locked \
    p4TaskPinsTestRegistryRetireForService
#define p4_task_registry_enter_running_locked \
    p4TaskPinsTestRegistryEnterRunning
#define p4_task_registry_leave_running_locked \
    p4TaskPinsTestRegistryLeaveRunning
#define p4_task_registry_claim_reclaim_locked \
    p4TaskPinsTestRegistryClaimReclaim
#define p4_task_registry_claim_service_locked \
    p4TaskPinsTestRegistryClaimService
#define p4_task_registry_service_owner_running_locked \
    p4TaskPinsTestRegistryServiceOwnerRunning
#define p4_task_registry_claim_queued_service_locked \
    p4TaskPinsTestRegistryClaimQueuedService
#define p4_task_registry_take_service_ready_locked \
    p4TaskPinsTestRegistryTakeServiceReady
#define p4_task_registry_claim_next_service_locked \
    p4TaskPinsTestRegistryClaimNextService
#define p4_task_registry_running_owner_locked p4TaskPinsTestRegistryRunningOwner
#define p4_task_registry_claim_next_retained_service_locked p4TaskPinsTestRegistryClaimRetained
#define p4_task_registry_finish_retained_service_locked p4TaskPinsTestRegistryFinishRetained
#define p4_task_registry_wait_begin_locked p4TaskPinsTestRegistryWaitBegin
#define p4_task_registry_wait_add_request_locked p4TaskPinsTestRegistryWaitAddRequest
#define p4_task_registry_wait_close_locked p4TaskPinsTestRegistryWaitClose
#define p4_task_registry_wait_ready_locked p4TaskPinsTestRegistryWaitReady
#define p4_task_registry_wait_grant_request_locked p4TaskPinsTestRegistryWaitGrantRequest
#define p4_task_registry_wait_withdraw_request_locked p4TaskPinsTestRegistryWaitWithdrawRequest
#define p4_task_registry_wait_finish_locked p4TaskPinsTestRegistryWaitFinish
#define p4_task_registry_wait_cancel_ready_locked p4TaskPinsTestRegistryWaitCancelReady
#define p4_task_registry_claim_next_wait_cancel_locked p4TaskPinsTestRegistryClaimNextWaitCancel
#define p4_task_registry_wait_finish_cancel_locked p4TaskPinsTestRegistryWaitFinishCancel
#define p4_task_registry_wait_release_cancel_locked p4TaskPinsTestRegistryWaitReleaseCancel
#include "../p4_task_pins.c"
#undef p4_task_registry_init
#undef p4_task_registry_register_locked
#undef p4_task_registry_pin_locked
#undef p4_task_registry_unpin_locked
#undef p4_task_registry_protect_worker_locked
#undef p4_task_registry_is_worker_locked
#undef p4_task_registry_retire_locked
#undef p4_task_registry_retire_for_service_locked
#undef p4_task_registry_enter_running_locked
#undef p4_task_registry_leave_running_locked
#undef p4_task_registry_claim_reclaim_locked
#undef p4_task_registry_claim_service_locked
#undef p4_task_registry_service_owner_running_locked
#undef p4_task_registry_claim_queued_service_locked
#undef p4_task_registry_take_service_ready_locked
#undef p4_task_registry_claim_next_service_locked
#undef p4_task_registry_running_owner_locked
#undef p4_task_registry_claim_next_retained_service_locked
#undef p4_task_registry_finish_retained_service_locked
#undef p4_task_registry_wait_begin_locked
#undef p4_task_registry_wait_add_request_locked
#undef p4_task_registry_wait_close_locked
#undef p4_task_registry_wait_ready_locked
#undef p4_task_registry_wait_grant_request_locked
#undef p4_task_registry_wait_withdraw_request_locked
#undef p4_task_registry_wait_finish_locked
#undef p4_task_registry_wait_cancel_ready_locked
#undef p4_task_registry_claim_next_wait_cancel_locked
#undef p4_task_registry_wait_finish_cancel_locked
#undef p4_task_registry_wait_release_cancel_locked

void krnP4CleanupPoke(struct ExecBase *SysBase)
{
    ++mock_cleanup_poke_calls;
    mock_cleanup_poke_observed_mie = mock_mie;
    mock_cleanup_poke_observed_disable_depth = mock_disable_depth;
    if (mock_gate_held || mock_mie != mock_cleanup_poke_expected_mie ||
        mock_disable_depth != mock_cleanup_poke_expected_disable_depth ||
        SysBase != mock_cleanup_poke_expected_base)
        ++mock_cleanup_poke_context_violations;
    CHECK(!mock_gate_held);
    CHECK(mock_mie == mock_cleanup_poke_expected_mie);
    CHECK(mock_disable_depth == mock_cleanup_poke_expected_disable_depth);
    CHECK(SysBase == mock_cleanup_poke_expected_base);
}

static void expect_cleanup_poke_context(struct ExecBase *SysBase)
{
    mock_cleanup_poke_expected_mie = mock_mie;
    mock_cleanup_poke_expected_disable_depth = mock_disable_depth;
    mock_cleanup_poke_expected_base = SysBase;
}

void krnP4RetiredOwnerPoke(uint32_t destination, struct ExecBase *SysBase)
{
    (void)SysBase;
    CHECK(!mock_gate_held);
    CHECK(destination < 2u);
    mock_owner_poke_mie = mock_mie;
    mock_owner_poke_depth = mock_disable_depth;
    mock_owner_poke_base = SysBase;
    if (destination < 2u)
        ++mock_owner_poke_calls[destination];
}

static void test_retired_owner_poke(void)
{
    uint32_t hart;
    struct ExecBase *base = (struct ExecBase *)(uintptr_t)1u;
    for (hart = 0u; hart < 2u; ++hart)
    {
        struct p4_task_registry_record record = {0};
        struct p4_cleanup_wake wake;
        void *mapping = mmap(NULL, cleanup_worker_mapping_size, PROT_NONE,
                              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        struct Task *identity;
        unsigned int prior = mock_owner_poke_calls[hart];
        CHECK(mapping != MAP_FAILED);
        if (mapping == MAP_FAILED)
            return;
        identity = (struct Task *)mapping;
        CHECK(krnP4TaskRecordPublishPinned(identity, &record, base) == 1);
        CHECK(krnP4TaskRecordEnterRunning(identity, hart, base) == 1);
        Disable();
        expect_cleanup_poke_context(base);
        CHECK(krnP4TaskRecordRetireForService(identity, base) == 1);
        CHECK(mock_owner_poke_calls[hart] == prior + 1u);
        CHECK(mock_owner_poke_mie == 0 && mock_owner_poke_depth == 1u);
        CHECK(mock_owner_poke_base == base);
        CHECK(mock_disable_depth == 1u && mock_mie == 0);
        CHECK(record.service_queued == 0u);
        CHECK(krnP4TaskRecordRetireForService(identity, base) == 0);
        CHECK(mock_owner_poke_calls[hart] == prior + 1u);
        krnP4TaskRecordLeaveRunning(identity, hart, base);
        CHECK(record.service_queued == 0u);
        krnP4SignalTaskUnpin(identity, base);
        CHECK(record.service_queued == 1u);
        Enable();
        CHECK(krnP4TakeCleanupWake(&wake, base) == 1);
        CHECK(krnP4TaskRecordClaimQueuedService(identity, base) == &record);
        CHECK(munmap(mapping, cleanup_worker_mapping_size) == 0);
    }
    {
        struct p4_task_registry_record record = {0};
        struct Task identity = {901u};
        struct p4_cleanup_wake wake;
        unsigned int prior = mock_owner_poke_calls[0] + mock_owner_poke_calls[1];
        expect_cleanup_poke_context(base);
        CHECK(krnP4TaskRecordPublish(&identity, &record, base) == 1);
        CHECK(krnP4TaskRecordRetireForService(&identity, base) == 1);
        CHECK(mock_owner_poke_calls[0] + mock_owner_poke_calls[1] == prior);
        CHECK(krnP4TakeCleanupWake(&wake, base) == 1);
        CHECK(krnP4TaskRecordClaimQueuedService(&identity, base) == &record);
        CHECK(krnP4TaskRecordRetireForService(&identity, base) == 0);
        CHECK(krnP4TaskRecordRetireForService(cleanup_worker_identity, base) == 0);
        CHECK(mock_owner_poke_calls[0] + mock_owner_poke_calls[1] == prior);
    }
}

static void test_retained_service_binding(void)
{
    struct p4_task_registry_record record = {0};
    struct p4_task_registry_record duplicate = {0};
    struct Task identity = {902u};
    struct ExecBase *base = (struct ExecBase *)(uintptr_t)1u;
    struct p4_cleanup_wake wake;
    uint32_t prior_count = mock_test_registry->count;
    expect_cleanup_poke_context(base);
    CHECK(krnP4TaskRecordPublish(&identity, &record, base) == 1);
    CHECK(krnP4TaskRecordRetireForService(&identity, base) == 1);
    CHECK(krnP4TakeCleanupWake(&wake, base) == 1);
    CHECK(krnP4TaskRecordClaimNextRetainedService(base) == &record);
    CHECK(mock_test_registry->count == prior_count + 1u);
    CHECK(record.service_inflight == 1u);
    CHECK(krnP4TaskRecordClaimNextRetainedService(base) == NULL);
    CHECK(krnP4TaskRecordClaimQueuedService(&identity, base) == NULL);
    CHECK(krnP4TaskRecordClaimReclaim(&identity, base) == NULL);
    CHECK(krnP4SignalTaskPin(&identity, base) == 0);
    CHECK(krnP4TaskRecordPublish(&identity, &duplicate, base) == 0);
    krnP4TaskRecordFinishRetainedService(&record, base);
    CHECK(mock_test_registry->count == prior_count);
    CHECK(record.service_inflight == 0u && record.next == NULL);
    CHECK(!mock_gate_held && mock_disable_depth == 0u && mock_mie == 1);
}

static int test_cleanup_wake_binding(void)
{
    struct p4_task_registry_record leave_record = {0};
    struct p4_task_registry_record unpin_record = {0};
    struct p4_task_registry_record immediate_record = {0};
    struct p4_task_registry_record coalesced_record = {0};
    struct p4_task_registry_record late_record = {0};
    struct p4_task_registry_record nested_record = {0};
    struct Task leave_task = {801u};
    struct Task unpin_task_identity = {802u};
    struct Task immediate_task = {803u};
    struct Task coalesced_task = {804u};
    struct Task late_task = {805u};
    struct Task nested_task = {806u};
    struct p4_cleanup_wake wake;
    struct ExecBase *invalid_base = (struct ExecBase *)(uintptr_t)0xdead1000u;
    struct Task *unknown = (struct Task *)(uintptr_t)0xdead2000u;
    unsigned int prior_pokes;
    uint32_t expected_mask = UINT32_C(0x5);

    CHECK(p4_cleanup_worker == cleanup_worker_identity);
    CHECK(p4_cleanup_wake_mask == 0u);
    CHECK(p4_cleanup_wake_pending); /* readiness accumulated before worker arm */
    expect_cleanup_poke_context(NULL);
    CHECK(krnP4CleanupWorkerReady(invalid_base) == 0);
    CHECK(krnP4ArmCleanupWake(unknown, expected_mask, invalid_base) == 0);
    CHECK(krnP4ArmCleanupWake(cleanup_worker_identity, 0u,
                              invalid_base) == 0);
    CHECK(p4_cleanup_wake_mask == 0u);
    CHECK(krnP4TakeCleanupWake(NULL, invalid_base) == -1);

    /* LeaveRunning makes a service record ready before the worker is armed.
       The transport sees the poke but cannot consume the still-unarmed event. */
    CHECK(krnP4TaskRecordPublishPinned(&leave_task, &leave_record,
                                       invalid_base) == 1);
    CHECK(krnP4TaskRecordEnterRunning(&leave_task, 0u, invalid_base) == 1);
    CHECK(krnP4TaskRecordRetireForService(&leave_task, invalid_base) == 1);
    CHECK(p4_cleanup_wake_pending); /* still no RUN drain, but old event remains */
    krnP4SignalTaskUnpin(&leave_task, invalid_base);
    CHECK(leave_record.lifetime.notification_pins == 1u);
    CHECK(leave_record.service_queued == 0u);
    CHECK(p4_cleanup_wake_pending);
    prior_pokes = mock_cleanup_poke_calls;
    expect_cleanup_poke_context(invalid_base);
    krnP4TaskRecordLeaveRunning(&leave_task, 0u, invalid_base);
    CHECK(leave_record.service_queued == 1u);
    CHECK(p4_cleanup_wake_pending);
    CHECK(mock_cleanup_poke_calls == prior_pokes + 1u);
    CHECK(mock_cleanup_poke_context_violations == 0u);

    wake.worker = unknown;
    wake.mask = UINT32_MAX;
    CHECK(krnP4TakeCleanupWake(&wake, invalid_base) == 0);
    CHECK(wake.worker == NULL && wake.mask == 0u);
    CHECK(p4_cleanup_wake_pending); /* unarmed event is durable */
    CHECK(krnP4CleanupWorkerReady(invalid_base) == 0);

    /* Even the protected singleton cannot arm without a current RUN owner. */
    krnP4TaskRecordLeaveRunning(cleanup_worker_identity, 1u, invalid_base);
    CHECK(krnP4ArmCleanupWake(cleanup_worker_identity, expected_mask,
                              invalid_base) == 0);
    CHECK(p4_cleanup_wake_mask == 0u);
    CHECK(krnP4TaskRecordEnterRunning(cleanup_worker_identity, 1u,
                                      invalid_base) == 1);

    prior_pokes = mock_cleanup_poke_calls;
    expect_cleanup_poke_context(invalid_base);
    CHECK(krnP4ArmCleanupWake(cleanup_worker_identity, expected_mask,
                              invalid_base) == 1);
    CHECK(p4_cleanup_wake_mask == expected_mask);
    CHECK(mock_cleanup_poke_calls == prior_pokes + 1u); /* old pending work */
    CHECK(krnP4CleanupWorkerReady(invalid_base) == 1);
    CHECK(krnP4ArmCleanupWake(cleanup_worker_identity, expected_mask,
                              invalid_base) == 0); /* exactly once */
    CHECK(krnP4TakeCleanupWake(&wake, invalid_base) == 1);
    CHECK(wake.worker == cleanup_worker_identity);
    CHECK(wake.mask == expected_mask);
    CHECK(!p4_cleanup_wake_pending);
    wake.worker = unknown;
    wake.mask = UINT32_MAX;
    CHECK(krnP4TakeCleanupWake(&wake, invalid_base) == 0);
    CHECK(wake.worker == NULL && wake.mask == 0u); /* duplicate take */
    CHECK(krnP4TaskRecordClaimQueuedService(&leave_task, invalid_base) ==
          &leave_record);

    /* SignalTaskUnpin is the second producer: the worker is already armed. */
    CHECK(krnP4TaskRecordPublishPinned(&unpin_task_identity, &unpin_record,
                                       invalid_base) == 1);
    CHECK(krnP4TaskRecordRetireForService(&unpin_task_identity,
                                          invalid_base) == 1);
    CHECK(!p4_cleanup_wake_pending);
    prior_pokes = mock_cleanup_poke_calls;
    expect_cleanup_poke_context(invalid_base);
    krnP4SignalTaskUnpin(&unpin_task_identity, invalid_base);
    CHECK(unpin_record.service_queued == 1u);
    CHECK(p4_cleanup_wake_pending);
    CHECK(mock_cleanup_poke_calls == prior_pokes + 1u);
    CHECK(krnP4TakeCleanupWake(&wake, invalid_base) == 1);
    CHECK(wake.worker == cleanup_worker_identity && wake.mask == expected_mask);
    CHECK(krnP4TaskRecordClaimQueuedService(&unpin_task_identity,
                                            invalid_base) == &unpin_record);

    /* RetireForService itself wakes when its new service pin is already the
       sole reference and there is no Running owner. Two ready records coalesce
       into one durable wake bit, but both remain independently queued. */
    CHECK(krnP4TaskRecordPublish(&immediate_task, &immediate_record,
                                  invalid_base) == 1);
    CHECK(krnP4TaskRecordPublish(&coalesced_task, &coalesced_record,
                                  invalid_base) == 1);
    prior_pokes = mock_cleanup_poke_calls;
    expect_cleanup_poke_context(invalid_base);
    CHECK(krnP4TaskRecordRetireForService(&immediate_task,
                                          invalid_base) == 1);
    CHECK(immediate_record.service_queued == 1u);
    CHECK(p4_cleanup_wake_pending);
    CHECK(mock_cleanup_poke_calls == prior_pokes + 1u);
    CHECK(krnP4TaskRecordRetireForService(&coalesced_task,
                                          invalid_base) == 1);
    CHECK(coalesced_record.service_queued == 1u);
    CHECK(p4_cleanup_wake_pending);
    CHECK(mock_cleanup_poke_calls == prior_pokes + 2u);

    /* Bad protected metadata must not consume the pending wake. */
    p4_task_pins_lock(invalid_base);
    cleanup_worker_record.worker_protected = 2u;
    p4_task_pins_unlock(invalid_base);
    CHECK(krnP4CleanupWorkerReady(invalid_base) == 0);
    wake.worker = unknown;
    wake.mask = UINT32_MAX;
    CHECK(krnP4TakeCleanupWake(&wake, invalid_base) == -1);
    CHECK(wake.worker == NULL && wake.mask == 0u);
    CHECK(p4_cleanup_wake_pending);
    p4_task_pins_lock(invalid_base);
    cleanup_worker_record.worker_protected = 1u;
    p4_task_pins_unlock(invalid_base);

    CHECK(krnP4TakeCleanupWake(&wake, invalid_base) == 1);
    CHECK(wake.worker == cleanup_worker_identity && wake.mask == expected_mask);
    CHECK(!p4_cleanup_wake_pending);
    CHECK(krnP4TaskRecordClaimQueuedService(&immediate_task,
                                            invalid_base) == &immediate_record);
    CHECK(krnP4TaskRecordClaimQueuedService(&coalesced_task,
                                            invalid_base) == &coalesced_record);

    /* A producer after consumption establishes a new wake rather than
       relying on the previous poke/interrupt. */
    CHECK(krnP4TaskRecordPublish(&late_task, &late_record, invalid_base) == 1);
    prior_pokes = mock_cleanup_poke_calls;
    CHECK(krnP4TaskRecordRetireForService(&late_task, invalid_base) == 1);
    CHECK(p4_cleanup_wake_pending);
    CHECK(mock_cleanup_poke_calls == prior_pokes + 1u);
    CHECK(krnP4TakeCleanupWake(&wake, invalid_base) == 1);
    CHECK(wake.worker == cleanup_worker_identity && wake.mask == expected_mask);
    CHECK(krnP4TaskRecordClaimQueuedService(&late_task, invalid_base) ==
          &late_record);

    /* Nested IRQ masking is restored before the poke, including the caller's
       disabled nesting level. The wake API never reads SysBase or Task. */
    CHECK(krnP4TaskRecordPublishPinned(&nested_task, &nested_record,
                                       invalid_base) == 1);
    CHECK(krnP4TaskRecordRetireForService(&nested_task, invalid_base) == 1);
    CHECK(!p4_cleanup_wake_pending);
    prior_pokes = mock_cleanup_poke_calls;
    Disable();
    expect_cleanup_poke_context(invalid_base);
    krnP4SignalTaskUnpin(&nested_task, invalid_base);
    CHECK(mock_cleanup_poke_calls == prior_pokes + 1u);
    CHECK(mock_cleanup_poke_observed_mie == 0);
    CHECK(mock_cleanup_poke_observed_disable_depth == 1u);
    CHECK(mock_mie == 0 && mock_disable_depth == 1u);
    CHECK(!mock_gate_held && p4_cleanup_wake_pending);
    Enable();
    CHECK(krnP4TakeCleanupWake(&wake, invalid_base) == 1);
    CHECK(wake.worker == cleanup_worker_identity && wake.mask == expected_mask);
    CHECK(krnP4TaskRecordClaimQueuedService(&nested_task,
                                            invalid_base) == &nested_record);

    CHECK(mock_cleanup_poke_context_violations == 0u);
    CHECK(krnP4CleanupWorkerReady(invalid_base) == 1);
    return 1;
}

static void check_registry_gate(void)
{
    ++mock_registry_calls;
    if (mock_mie != 0 || mock_disable_depth == 0 || !mock_gate_held)
    {
        ++mock_registry_context_violations;
        CHECK(mock_mie == 0);
        CHECK(mock_disable_depth != 0);
        CHECK(mock_gate_held);
    }
}

static void check_wait_registry_gate(enum wait_registry_call call)
{
    check_registry_gate();
    ++mock_wait_registry_calls[call];
}

int p4TaskPinsTestRegistryWaitBegin(
    struct p4_task_registry *registry,
    struct Task *identity,
    uint32_t kind,
    const void *semaphore_index,
    const void *request_index,
    const void *list_index,
    uint32_t *out_generation)
{
    check_wait_registry_gate(WAIT_REG_BEGIN);
    return p4_task_registry_wait_begin_locked(
        registry, identity, kind, semaphore_index, request_index, list_index,
        out_generation);
}

int p4TaskPinsTestRegistryWaitAddRequest(
    struct p4_task_registry *registry,
    struct Task *identity,
    uint32_t generation)
{
    check_wait_registry_gate(WAIT_REG_ADD);
    return p4_task_registry_wait_add_request_locked(
        registry, identity, generation);
}

int p4TaskPinsTestRegistryWaitClose(
    struct p4_task_registry *registry,
    struct Task *identity,
    uint32_t generation)
{
    check_wait_registry_gate(WAIT_REG_CLOSE);
    return p4_task_registry_wait_close_locked(registry, identity, generation);
}

int p4TaskPinsTestRegistryWaitReady(
    struct p4_task_registry *registry,
    struct Task *identity,
    uint32_t generation)
{
    check_wait_registry_gate(WAIT_REG_READY);
    return p4_task_registry_wait_ready_locked(registry, identity, generation);
}

int p4TaskPinsTestRegistryWaitGrantRequest(
    struct p4_task_registry *registry,
    struct Task *identity,
    uint32_t generation)
{
    check_wait_registry_gate(WAIT_REG_GRANT);
    return p4_task_registry_wait_grant_request_locked(
        registry, identity, generation);
}

int p4TaskPinsTestRegistryWaitWithdrawRequest(
    struct p4_task_registry *registry,
    struct Task *identity,
    uint32_t generation)
{
    check_wait_registry_gate(WAIT_REG_WITHDRAW);
    return p4_task_registry_wait_withdraw_request_locked(
        registry, identity, generation);
}

int p4TaskPinsTestRegistryWaitFinish(
    struct p4_task_registry *registry,
    struct Task *identity,
    uint32_t generation)
{
    check_wait_registry_gate(WAIT_REG_FINISH);
    return p4_task_registry_wait_finish_locked(
        registry, identity, generation);
}

int p4TaskPinsTestRegistryWaitCancelReady(
    struct p4_task_registry *registry, struct Task *identity)
{
    check_wait_registry_gate(WAIT_REG_CANCEL_READY);
    return p4_task_registry_wait_cancel_ready_locked(registry, identity);
}

int p4TaskPinsTestRegistryClaimNextWaitCancel(
    struct p4_task_registry *registry,
    struct p4_task_wait_cancel_snapshot *out_snapshot)
{
    check_wait_registry_gate(WAIT_REG_CLAIM_CANCEL);
    return p4_task_registry_claim_next_wait_cancel_locked(
        registry, out_snapshot);
}

int p4TaskPinsTestRegistryWaitFinishCancel(
    struct p4_task_registry *registry,
    struct Task *identity,
    uint32_t generation)
{
    check_wait_registry_gate(WAIT_REG_FINISH_CANCEL);
    return p4_task_registry_wait_finish_cancel_locked(
        registry, identity, generation);
}

int p4TaskPinsTestRegistryWaitReleaseCancel(
    struct p4_task_registry *registry,
    struct Task *identity,
    uint32_t generation)
{
    check_wait_registry_gate(WAIT_REG_RELEASE_CANCEL);
    return p4_task_registry_wait_release_cancel_locked(
        registry, identity, generation);
}

void p4TaskPinsTestRegistryInit(struct p4_task_registry *registry)
{
    ++mock_registry_init_calls;
    CHECK(mock_mie == 0);
    CHECK(!mock_gate_held);
    mock_test_registry = registry;
    p4_task_registry_init(registry);
}

int p4TaskPinsTestRegistryRegister(
    struct p4_task_registry *registry,
    struct p4_task_registry_record *record,
    struct Task *identity)
{
    check_registry_gate();
    return p4_task_registry_register_locked(registry, record, identity);
}

int p4TaskPinsTestRegistryPin(
    struct p4_task_registry *registry, struct Task *identity)
{
    check_registry_gate();
    return p4_task_registry_pin_locked(registry, identity);
}

int p4TaskPinsTestRegistryUnpin(
    struct p4_task_registry *registry, struct Task *identity)
{
    check_registry_gate();
    return p4_task_registry_unpin_locked(registry, identity);
}

int p4TaskPinsTestRegistryProtectWorker(
    struct p4_task_registry *registry, struct Task *identity)
{
    check_registry_gate();
    ++mock_registry_worker_protects;
    return p4_task_registry_protect_worker_locked(registry, identity);
}

int p4TaskPinsTestRegistryIsWorker(
    struct p4_task_registry *registry, struct Task *identity)
{
    check_registry_gate();
    ++mock_registry_worker_checks;
    return p4_task_registry_is_worker_locked(registry, identity);
}

int p4TaskPinsTestRegistryRetire(
    struct p4_task_registry *registry, struct Task *identity)
{
    check_registry_gate();
    return p4_task_registry_retire_locked(registry, identity);
}

int p4TaskPinsTestRegistryRetireForService(
    struct p4_task_registry *registry, struct Task *identity)
{
    check_registry_gate();
    ++mock_registry_service_retires;
    return p4_task_registry_retire_for_service_locked(registry, identity);
}

int p4TaskPinsTestRegistryEnterRunning(
    struct p4_task_registry *registry, struct Task *identity, uint32_t hart)
{
    check_registry_gate();
    return p4_task_registry_enter_running_locked(registry, identity, hart);
}

int p4TaskPinsTestRegistryLeaveRunning(
    struct p4_task_registry *registry, struct Task *identity, uint32_t hart)
{
    check_registry_gate();
    return p4_task_registry_leave_running_locked(registry, identity, hart);
}

struct p4_task_registry_record *p4TaskPinsTestRegistryClaimReclaim(
    struct p4_task_registry *registry, struct Task *identity)
{
    check_registry_gate();
    return p4_task_registry_claim_reclaim_locked(registry, identity);
}

struct p4_task_registry_record *p4TaskPinsTestRegistryClaimService(
    struct p4_task_registry *registry, struct Task *identity)
{
    check_registry_gate();
    ++mock_registry_service_claims;
    return p4_task_registry_claim_service_locked(registry, identity);
}

struct p4_task_registry_record *p4TaskPinsTestRegistryClaimQueuedService(
    struct p4_task_registry *registry, struct Task *identity)
{
    check_registry_gate();
    return p4_task_registry_claim_queued_service_locked(registry, identity);
}

struct Task *p4TaskPinsTestRegistryTakeServiceReady(
    struct p4_task_registry *registry, struct Task *identity)
{
    check_registry_gate();
    ++mock_registry_service_ready;
    return p4_task_registry_take_service_ready_locked(registry, identity);
}

int p4TaskPinsTestRegistryClaimNextService(
    struct p4_task_registry *registry,
    struct p4_task_registry_record **out_record)
{
    check_registry_gate();
    return p4_task_registry_claim_next_service_locked(registry, out_record);
}

int p4TaskPinsTestRegistryClaimRetained(struct p4_task_registry *registry,
    struct p4_task_registry_record **out_record)
{
    check_registry_gate();
    return p4_task_registry_claim_next_retained_service_locked(registry, out_record);
}

int p4TaskPinsTestRegistryFinishRetained(struct p4_task_registry *registry,
    struct p4_task_registry_record *record)
{
    check_registry_gate();
    return p4_task_registry_finish_retained_service_locked(registry, record);
}

int p4TaskPinsTestRegistryRunningOwner(
    struct p4_task_registry *registry, struct Task *identity)
{
    check_registry_gate();
    return p4_task_registry_running_owner_locked(registry, identity);
}

int p4TaskPinsTestRegistryServiceOwnerRunning(
    struct p4_task_registry *registry, struct Task *identity, uint32_t hart)
{
    check_registry_gate();
    return p4_task_registry_service_owner_running_locked(registry, identity, hart);
}

void Disable(void)
{
    ++mock_disable_depth;
    mock_mie = 0;
}

void Enable(void)
{
    void (*callback)(void);

    ++mock_enable_calls;
    if (mock_gate_held)
    {
        ++mock_enable_gate_violations;
        CHECK(!mock_gate_held);
    }
    if (mock_disable_depth == 0)
    {
        CHECK(0 && "unbalanced Enable");
        return;
    }

    --mock_disable_depth;
    if (mock_disable_depth == 0)
    {
        mock_mie = 1;
        callback = mock_enable_callback;
        mock_enable_callback = NULL;
        if (callback != NULL)
            callback();
    }
}

enum p4_public_spinlock_result krnP4PublicSpinInit(spinlock_t *lock)
{
    ++mock_spin_init_calls;
    CHECK(lock == &p4_task_pin_gate);
    CHECK(mock_mie == 0);
    CHECK(!mock_gate_held);
    if (mock_spin_init_result == P4_PUBLIC_SPIN_OK)
    {
        lock->lock = 0;
        lock->s_Owner = 0;
    }
    return mock_spin_init_result;
}

enum p4_public_spinlock_result krnP4PublicSpinTryAcquire(
    spinlock_t *lock, uint32_t mode)
{
    enum p4_public_spinlock_result result;

    ++mock_try_calls;
    CHECK(lock == &p4_task_pin_gate);
    CHECK(mode == SPINLOCK_MODE_WRITE);
    CHECK(mock_mie == 0);
    CHECK(!mock_gate_held);

    if (mock_try_script_index < mock_try_script_count)
        result = mock_try_script[mock_try_script_index++];
    else
        result = mock_try_fallback;

    if (result == P4_PUBLIC_SPIN_OK)
    {
        mock_gate_held = 1;
        lock->lock = SPINLOCK_MODE_WRITE;
    }
    return result;
}

enum p4_public_spinlock_result krnP4PublicSpinUnlock(spinlock_t *lock)
{
    ++mock_unlock_calls;
    CHECK(lock == &p4_task_pin_gate);
    CHECK(mock_mie == 0);
    CHECK(mock_gate_held);

    if (mock_spin_unlock_result == P4_PUBLIC_SPIN_OK)
    {
        mock_gate_held = 0;
        lock->lock = 0;
        lock->s_Owner = 0;
    }
    return mock_spin_unlock_result;
}

void krnP4PublicSpinFatal(enum p4_public_spinlock_result reason)
{
    ++mock_fatal_calls;
    mock_fatal_reason = reason;
    mock_fatal_mie_before = mock_mie;
    /* Model the production RV32 fatal's csrci mstatus, 8 before halting. */
    mock_mie = 0;
    mock_fatal_mie_after = mock_mie;

    if (!mock_fatal_armed)
    {
        fputs("unexpected P4 public-spin fatal\n", stderr);
        abort();
    }
    longjmp(mock_fatal_jump, 1);
}

static void mock_set_try_script(
    const enum p4_public_spinlock_result *script,
    unsigned int count,
    enum p4_public_spinlock_result fallback)
{
    unsigned int i;

    mock_try_script_count = count;
    mock_try_script_index = 0;
    mock_try_fallback = fallback;
    for (i = 0; i < count; ++i)
        mock_try_script[i] = script[i];
    mock_try_calls = 0;
}

static void mock_reset_gate_after_fatal(void)
{
    mock_gate_held = 0;
    p4_task_pin_gate.lock = 0;
    p4_task_pin_gate.s_Owner = 0;
    mock_spin_unlock_result = P4_PUBLIC_SPIN_OK;
    mock_disable_depth = 0;
    mock_mie = 1;
}

static void expect_fatal(
    enum p4_public_spinlock_result reason,
    void (*operation)(void))
{
    unsigned int prior_calls = mock_fatal_calls;
    int caught;

    mock_fatal_armed = 1;
    caught = setjmp(mock_fatal_jump);
    if (caught == 0)
    {
        operation();
        mock_fatal_armed = 0;
        CHECK(0 && "operation returned instead of failing closed");
        return;
    }

    mock_fatal_armed = 0;
    CHECK(mock_fatal_calls == prior_calls + 1u);
    CHECK(mock_fatal_reason == reason);
    CHECK(mock_fatal_mie_after == 0);
}

static void operation_finish_unknown_retained(void)
{
    krnP4TaskRecordFinishRetainedService(NULL, NULL);
}

static void test_retained_finish_fails_closed(void)
{
    unsigned int enables = mock_enable_calls;
    expect_fatal(P4_PUBLIC_SPIN_BAD_STATE, operation_finish_unknown_retained);
    CHECK(mock_gate_held && mock_mie == 0 && mock_disable_depth == 1u);
    CHECK(mock_enable_calls == enables);
    mock_reset_gate_after_fatal();
}

static struct Task *callback_task;
static int callback_pin_result;
static struct Task *publish_probe_task;
static struct p4_task_registry_record *publish_probe_record;
static unsigned int publish_probe_registry_base;
static unsigned int publish_probe_try_base;
static unsigned int publish_probe_calls;
static int publish_probe_retire_result;
static struct p4_task_registry_record *publish_probe_claim_result;
static int publish_probe_pin_after_retire;

static void reentrant_enable_callback(void)
{
    ++mock_callback_calls;
    CHECK(mock_mie == 1);
    CHECK(!mock_gate_held);
    callback_pin_result = krnP4SignalTaskPin(callback_task, NULL);
}

static void publish_pinned_enable_observer(void)
{
    ++publish_probe_calls;
    CHECK(mock_mie == 1);
    CHECK(!mock_gate_held);
    CHECK(publish_probe_record != NULL);
    CHECK(publish_probe_record != NULL &&
          publish_probe_record->identity == publish_probe_task);
    /* Before Enable, PublishPinned must have completed both registry steps
       under its one acquired lifetime gate. A reentrant retire/claim probe
       now observes the creator pin rather than reclaiming between them. */
    CHECK(mock_registry_calls == publish_probe_registry_base + 2u);
    CHECK(mock_try_calls == publish_probe_try_base + 1u);
    publish_probe_retire_result =
        krnP4TaskRecordRetire(publish_probe_task, NULL);
    publish_probe_claim_result =
        krnP4TaskRecordClaimReclaim(publish_probe_task, NULL);
    publish_probe_pin_after_retire =
        krnP4SignalTaskPin(publish_probe_task, NULL);
}

static struct Task *service_probe_identity;
static struct p4_task_registry_record *service_probe_record;
static struct p4_task_registry_record *service_probe_claim_result;
static int service_probe_pin_result;
static unsigned int service_probe_claim_base;
static unsigned int service_probe_try_base;
static unsigned int service_probe_calls;

static void service_claim_enable_observer(void)
{
    ++service_probe_calls;
    CHECK(mock_mie == 1);
    CHECK(!mock_gate_held);
    CHECK(service_probe_record != NULL);
    /* The identity is deliberately an invalid Task address. The reentrant
       claim/pin must consult only the detached registry record, not Task. */
    CHECK(mock_registry_service_claims == service_probe_claim_base + 1u);
    CHECK(mock_try_calls == service_probe_try_base + 1u);
    service_probe_claim_result =
        krnP4TaskRecordClaimService(service_probe_identity, NULL);
    service_probe_pin_result =
        krnP4SignalTaskPin(service_probe_identity, NULL);
}

static void operation_invalid_lock_state(void)
{
    (void)krnP4SignalTaskPin((struct Task *)(uintptr_t)0x1000u, NULL);
}

static void operation_gate_timeout(void)
{
    (void)krnP4SignalTaskPin((struct Task *)(uintptr_t)0x1000u, NULL);
}

static void operation_unpin_underflow(void)
{
    krnP4SignalTaskUnpin((struct Task *)(uintptr_t)0x3000u, NULL);
}

static void operation_service_unpin_underflow(void)
{
    krnP4SignalTaskUnpin(service_probe_identity, NULL);
}

static void operation_cleanup_worker_unpin_floor(void)
{
    krnP4SignalTaskUnpin(cleanup_worker_identity, NULL);
}

static void operation_release_failure(void)
{
    (void)krnP4SignalTaskPin((struct Task *)(uintptr_t)0x4000u, NULL);
}

static int test_prepare_once(void)
{
    CHECK(mock_disable_depth == 0);
    Disable();
    CHECK(krnP4TaskPinsPrepare() == 1);
    CHECK(mock_mie == 0);
    CHECK(!mock_gate_held);
    CHECK(mock_spin_init_calls == 1);
    CHECK(mock_registry_init_calls == 1);
    Enable();
    CHECK(mock_mie == 1);
    CHECK(krnP4TaskPinsPrepare() == 0);
    CHECK(mock_spin_init_calls == 1);
    CHECK(mock_registry_init_calls == 1);
    return 1;
}

static int test_invalid_identity_and_lifetime(void)
{
    struct p4_task_registry_record record = {0};
    struct Task *identity = (struct Task *)(uintptr_t)0x1000u;
    struct Task *invalid = (struct Task *)(uintptr_t)1u;

    /* Registry identity matching must not dereference an arbitrary Task value. */
    CHECK(krnP4SignalTaskPin(invalid, NULL) == 0);
    CHECK(krnP4TaskRecordPublish(identity, &record, NULL) == 1);
    CHECK(krnP4SignalTaskPin(identity, NULL) == 1);
    CHECK(krnP4TaskRecordRetire(identity, NULL) == 1);
    CHECK(krnP4SignalTaskPin(identity, NULL) == 0);
    CHECK(krnP4TaskRecordClaimReclaim(identity, NULL) == NULL);
    krnP4SignalTaskUnpin(identity, NULL);
    CHECK(krnP4TaskRecordClaimReclaim(identity, NULL) == &record);
    CHECK(record.identity == identity);
    CHECK((record.lifetime.flags & P4_TASK_LIFETIME_CLAIMED) != 0);
    CHECK(krnP4SignalTaskPin(invalid, NULL) == 0);
    return 1;
}

static int test_publish_pinned_creator_pin_and_registration_failure(void)
{
    struct p4_task_registry_record existing = {0};
    struct p4_task_registry_record duplicate = {0};
    struct p4_task_registry_record creator = {0};
    struct Task *existing_identity = (struct Task *)(uintptr_t)0x4800u;
    struct Task *creator_identity = (struct Task *)(uintptr_t)0x5000u;
    unsigned int prior_registry_calls;

    CHECK(krnP4TaskRecordPublish(existing_identity, &existing, NULL) == 1);
    prior_registry_calls = mock_registry_calls;

    /* A duplicate identity cannot publish the caller's fresh record or alter
       the already registered record. */
    CHECK(krnP4TaskRecordPublishPinned(existing_identity, &duplicate,
                                       NULL) == 0);
    CHECK(mock_registry_calls == prior_registry_calls + 1u);
    CHECK(duplicate.next == NULL);
    CHECK(duplicate.identity == NULL);
    CHECK(duplicate.lifetime.notification_pins == 0u);
    CHECK(duplicate.lifetime.running_harts == 0u);
    CHECK(duplicate.lifetime.flags == 0u);
    CHECK(existing.identity == existing_identity);
    CHECK(existing.next == NULL);
    CHECK(existing.lifetime.notification_pins == 0u);
    CHECK(existing.lifetime.running_harts == 0u);
    CHECK(existing.lifetime.flags == 0u);

    /* Register and creator-pin happen under the one binding gate. The pin is
       visible immediately on return, before retirement can reclaim storage. */
    prior_registry_calls = mock_registry_calls;
    publish_probe_task = creator_identity;
    publish_probe_record = &creator;
    publish_probe_registry_base = mock_registry_calls;
    publish_probe_try_base = mock_try_calls;
    publish_probe_calls = 0u;
    publish_probe_retire_result = 0;
    publish_probe_claim_result = NULL;
    publish_probe_pin_after_retire = -1;
    mock_enable_callback = publish_pinned_enable_observer;
    CHECK(krnP4TaskRecordPublishPinned(creator_identity, &creator, NULL) == 1);
    mock_enable_callback = NULL;
    CHECK(publish_probe_calls == 1u);
    CHECK(publish_probe_retire_result == 1);
    CHECK(publish_probe_claim_result == NULL);
    CHECK(publish_probe_pin_after_retire == 0);
    /* Retirement now checks both cancellation and service-wake readiness
       before it opens the gate. */
    CHECK(mock_registry_calls == prior_registry_calls + 7u);
    CHECK(!mock_gate_held);
    CHECK(creator.identity == creator_identity);
    CHECK(creator.next == &existing);
    CHECK(creator.lifetime.notification_pins == 1u);
    CHECK(creator.lifetime.running_harts == 0u);
    CHECK(creator.lifetime.flags == P4_TASK_LIFETIME_RETIRED);
    CHECK(krnP4SignalTaskPin(creator_identity, NULL) == 0);
    CHECK(krnP4TaskRecordClaimReclaim(creator_identity, NULL) == NULL);
    CHECK(creator.lifetime.notification_pins == 1u);
    krnP4SignalTaskUnpin(creator_identity, NULL);
    CHECK(krnP4TaskRecordClaimReclaim(creator_identity, NULL) == &creator);
    CHECK((creator.lifetime.flags & P4_TASK_LIFETIME_CLAIMED) != 0u);

    /* Removing the creator record leaves the old record intact and claimable. */
    CHECK(existing.identity == existing_identity);
    CHECK(existing.next == NULL);
    CHECK(existing.lifetime.notification_pins == 0u);
    CHECK(existing.lifetime.running_harts == 0u);
    CHECK(existing.lifetime.flags == 0u);
    CHECK(krnP4TaskRecordRetire(existing_identity, NULL) == 1);
    CHECK(krnP4TaskRecordClaimReclaim(existing_identity, NULL) == &existing);
    return 1;
}

static int test_unique_running_handoff(void)
{
    struct p4_task_registry_record record = {0};
    struct Task *identity = (struct Task *)(uintptr_t)0x2000u;

    CHECK(krnP4TaskRecordPublish(identity, &record, NULL) == 1);
    CHECK(krnP4TaskRecordEnterRunning(identity, 0, NULL) == 1);
    CHECK(krnP4TaskRecordEnterRunning(identity, 1, NULL) == 0);
    CHECK(record.lifetime.running_harts == (UINT32_C(1) << 0));
    krnP4TaskRecordLeaveRunning(identity, 0, NULL);
    CHECK(record.lifetime.running_harts == 0u);
    CHECK(krnP4TaskRecordEnterRunning(identity, 1, NULL) == 1);
    krnP4TaskRecordLeaveRunning(identity, 1, NULL);
    CHECK(krnP4TaskRecordRetire(identity, NULL) == 1);
    CHECK(krnP4TaskRecordClaimReclaim(identity, NULL) == &record);
    return 1;
}

static int test_service_reference_drain_and_claim(void)
{
    struct p4_task_registry_record record = {0};
    struct Task *identity = (struct Task *)(uintptr_t)0x6000u;
    struct p4_task_registry_record *claimed;
    unsigned int prior_claims;
    unsigned int prior_tries;

    CHECK(krnP4TaskRecordPublish(identity, &record, NULL) == 1);
    CHECK(krnP4TaskRecordEnterRunning(identity, 0, NULL) == 1);
    CHECK(krnP4SignalTaskPin(identity, NULL) == 1); /* outstanding signal */
    service_probe_identity = identity;
    CHECK(krnP4TaskRecordRetireForService(identity, NULL) == 1);
    CHECK(record.lifetime.flags == P4_TASK_LIFETIME_RETIRED);
    CHECK(record.lifetime.notification_pins == 2u);
    CHECK(record.lifetime.running_harts == 1u);
    CHECK(record.service_pending == 1u);

    /* Duplicate service ownership and generic reclaim are refused without
       consuming the designated service pin. */
    CHECK(krnP4TaskRecordRetireForService(identity, NULL) == 0);
    CHECK(krnP4TaskRecordClaimReclaim(identity, NULL) == NULL);
    CHECK(record.lifetime.notification_pins == 2u);
    CHECK(record.service_pending == 1u);

    prior_claims = mock_registry_service_claims;
    prior_tries = mock_try_calls;
    CHECK(krnP4TaskRecordClaimService(identity, NULL) == NULL);
    CHECK(mock_registry_service_claims == prior_claims + 1u);
    CHECK(mock_try_calls == prior_tries + 1u);
    CHECK(record.lifetime.notification_pins == 2u);
    CHECK(record.lifetime.running_harts == 1u);
    CHECK(record.service_pending == 1u);

    /* Ordinary unpin drains an additional notification pin but refuses the
       final service-owned pin and fails closed without losing that pin. */
    krnP4SignalTaskUnpin(identity, NULL);
    CHECK(record.lifetime.notification_pins == 1u);
    expect_fatal(P4_PUBLIC_SPIN_BAD_STATE,
        operation_service_unpin_underflow);
    CHECK(record.lifetime.notification_pins == 1u);
    CHECK(record.service_pending == 1u);
    CHECK((record.lifetime.flags & P4_TASK_LIFETIME_RETIRED) != 0u);
    CHECK(!mock_gate_held);
    mock_reset_gate_after_fatal();

    prior_claims = mock_registry_service_claims;
    CHECK(krnP4TaskRecordClaimService(identity, NULL) == NULL);
    CHECK(mock_registry_service_claims == prior_claims + 1u);
    CHECK(record.lifetime.notification_pins == 1u);
    CHECK(record.lifetime.running_harts == 1u);
    CHECK(record.service_pending == 1u);

    krnP4TaskRecordLeaveRunning(identity, 0, NULL);
    CHECK(record.lifetime.running_harts == 0u);
    service_probe_identity = identity;
    service_probe_record = &record;
    service_probe_claim_result = (struct p4_task_registry_record *)(uintptr_t)1u;
    service_probe_pin_result = -1;
    service_probe_claim_base = mock_registry_service_claims;
    service_probe_try_base = mock_try_calls;
    service_probe_calls = 0u;
    mock_enable_callback = service_claim_enable_observer;
    claimed = krnP4TaskRecordClaimService(identity, NULL);
    mock_enable_callback = NULL;

    CHECK(claimed == &record);
    CHECK(service_probe_calls == 1u);
    CHECK(service_probe_claim_result == NULL);
    CHECK(service_probe_pin_result == 0);
    CHECK(mock_registry_service_claims == service_probe_claim_base + 2u);
    CHECK(mock_try_calls == service_probe_try_base + 3u);
    CHECK(record.identity == identity);
    CHECK(record.next == NULL);
    CHECK(record.service_pending == 0u);
    CHECK(record.lifetime.notification_pins == 0u);
    CHECK(record.lifetime.running_harts == 0u);
    CHECK((record.lifetime.flags & P4_TASK_LIFETIME_CLAIMED) != 0u);
    CHECK(!mock_gate_held);
    return 1;
}

static int test_nested_disable_and_callback_reentry(void)
{
    struct p4_task_registry_record record = {0};
    struct Task *identity = (struct Task *)(uintptr_t)0x2800u;

    CHECK(krnP4TaskRecordPublish(identity, &record, NULL) == 1);
    Disable();
    CHECK(krnP4SignalTaskPin(identity, NULL) == 1);
    CHECK(mock_disable_depth == 1);
    CHECK(mock_mie == 0);
    Enable();
    CHECK(mock_mie == 1);

    callback_task = identity;
    callback_pin_result = 0;
    mock_callback_calls = 0;
    mock_enable_callback = reentrant_enable_callback;
    CHECK(krnP4SignalTaskPin(identity, NULL) == 1);
    mock_enable_callback = NULL;
    CHECK(mock_callback_calls == 1);
    CHECK(callback_pin_result == 1);
    CHECK(!mock_gate_held);

    krnP4SignalTaskUnpin(identity, NULL);
    krnP4SignalTaskUnpin(identity, NULL);
    krnP4SignalTaskUnpin(identity, NULL);
    CHECK(krnP4TaskRecordRetire(identity, NULL) == 1);
    CHECK(krnP4TaskRecordClaimReclaim(identity, NULL) == &record);
    return 1;
}

static int test_busy_timeout_retry_then_success(void)
{
    static const enum p4_public_spinlock_result script[] = {
        P4_PUBLIC_SPIN_BUSY,
        P4_PUBLIC_SPIN_TRY_TIMEOUT,
        P4_PUBLIC_SPIN_OK
    };
    struct p4_task_registry_record record = {0};
    struct Task *identity = (struct Task *)(uintptr_t)0x2c00u;

    CHECK(krnP4TaskRecordPublish(identity, &record, NULL) == 1);
    mock_set_try_script(script, 3u, P4_PUBLIC_SPIN_BAD_STATE);
    CHECK(krnP4SignalTaskPin(identity, NULL) == 1);
    CHECK(mock_try_calls == 3u);
    CHECK(mock_gate_held == 0);
    CHECK(mock_mie == 1);
    mock_set_try_script(NULL, 0u, P4_PUBLIC_SPIN_OK);
    krnP4SignalTaskUnpin(identity, NULL);
    CHECK(krnP4TaskRecordRetire(identity, NULL) == 1);
    CHECK(krnP4TaskRecordClaimReclaim(identity, NULL) == &record);
    return 1;
}

static int test_invalid_state_fails_closed(void)
{
    static const enum p4_public_spinlock_result script[] = {
        P4_PUBLIC_SPIN_BAD_STATE
    };

    mock_set_try_script(script, 1u, P4_PUBLIC_SPIN_OK);
    expect_fatal(P4_PUBLIC_SPIN_BAD_STATE, operation_invalid_lock_state);
    CHECK(mock_try_calls == 1u);
    CHECK(!mock_gate_held);
    mock_reset_gate_after_fatal();
    return 1;
}

static int test_bounded_timeout_fails_closed(void)
{
    mock_set_try_script(NULL, 0u, P4_PUBLIC_SPIN_BUSY);
    expect_fatal(P4_PUBLIC_SPIN_SERVICE_TIMEOUT, operation_gate_timeout);
    CHECK(mock_try_calls == P4_TASK_PIN_GATE_ATTEMPTS);
    CHECK(!mock_gate_held);
    mock_reset_gate_after_fatal();
    mock_set_try_script(NULL, 0u, P4_PUBLIC_SPIN_OK);
    return 1;
}

static int test_unpin_underflow_fails_closed(void)
{
    struct p4_task_registry_record record = {0};
    struct Task *identity = (struct Task *)(uintptr_t)0x3000u;

    CHECK(krnP4TaskRecordPublish(identity, &record, NULL) == 1);
    expect_fatal(P4_PUBLIC_SPIN_BAD_STATE, operation_unpin_underflow);
    CHECK(mock_fatal_mie_after == 0);
    CHECK(mock_fatal_mie_before == 1);
    CHECK(!mock_gate_held);
    mock_reset_gate_after_fatal();
    CHECK(krnP4TaskRecordRetire(identity, NULL) == 1);
    CHECK(krnP4TaskRecordClaimReclaim(identity, NULL) == &record);
    return 1;
}

static int test_release_error_fails_closed(void)
{
    struct p4_task_registry_record record = {0};
    struct Task *identity = (struct Task *)(uintptr_t)0x4000u;
    static const enum p4_public_spinlock_result script[] = {
        P4_PUBLIC_SPIN_OK
    };

    CHECK(krnP4TaskRecordPublish(identity, &record, NULL) == 1);
    mock_set_try_script(script, 1u, P4_PUBLIC_SPIN_OK);
    mock_spin_unlock_result = P4_PUBLIC_SPIN_RELEASE_FAILURE;
    expect_fatal(P4_PUBLIC_SPIN_RELEASE_FAILURE, operation_release_failure);
    CHECK(mock_fatal_mie_before == 0);
    CHECK(mock_gate_held);
    mock_reset_gate_after_fatal();
    krnP4SignalTaskUnpin(identity, NULL);
    CHECK(krnP4TaskRecordRetire(identity, NULL) == 1);
    CHECK(krnP4TaskRecordClaimReclaim(identity, NULL) == &record);
    return 1;
}

static int test_service_owner_binding(void)
{
    unsigned int hart;
    for (hart = 0; hart < 2u; ++hart)
    {
        struct p4_task_registry_record record = {0};
        struct Task *identity = (struct Task *)(uintptr_t)(0x7100u + hart * 16u);
        unsigned int prior_calls;
        unsigned int prior_ready;
        CHECK(krnP4TaskRecordPublishPinned(identity, &record, NULL) == 1);
        CHECK(krnP4TaskRecordRunningOwner(identity, NULL) == 0);
        CHECK(krnP4TaskRecordEnterRunning(identity, hart, NULL) == 1);
        CHECK(krnP4TaskRecordRunningOwner(identity, NULL) == (int)hart + 1);
        CHECK(krnP4TaskRecordServiceOwnerRunning(identity, hart, NULL) == 0);
        CHECK(krnP4TaskRecordRetireForService(identity, NULL) == 1);
        CHECK(krnP4TaskRecordRunningOwner(identity, NULL) == -1);
        prior_calls = mock_registry_calls;
        Disable();
        CHECK(krnP4TaskRecordServiceOwnerRunning(identity, hart, NULL) == 1);
        CHECK(mock_registry_calls == prior_calls + 1u);
        CHECK(mock_disable_depth == 1u && mock_mie == 0 && !mock_gate_held);
        Enable();
        CHECK(krnP4TaskRecordServiceOwnerRunning(identity, 1u - hart, NULL) == 0);
        CHECK(krnP4TaskRecordServiceOwnerRunning(identity, 2u, NULL) == 0);
        CHECK(krnP4TaskRecordServiceOwnerRunning(NULL, hart, NULL) == 0);
        CHECK(record.lifetime.notification_pins == 2u);
        krnP4SignalTaskUnpin(identity, NULL);
        CHECK(krnP4TaskRecordServiceOwnerRunning(identity, hart, NULL) == 1);
        prior_ready = mock_registry_service_ready;
        krnP4TaskRecordLeaveRunning(identity, hart, NULL);
        CHECK(mock_registry_service_ready == prior_ready + 1u);
        CHECK(record.service_queued == 1u);
        CHECK(krnP4TaskRecordServiceOwnerRunning(identity, hart, NULL) == 0);
        CHECK(krnP4TaskRecordClaimService(identity, NULL) == &record);
        CHECK(krnP4TaskRecordServiceOwnerRunning(identity, hart, NULL) == 0);
        CHECK(mock_mie == 1 && mock_disable_depth == 0u && !mock_gate_held);
    }
    return 1;
}

static int test_queued_service_binding(void)
{
    struct p4_task_registry_record record = {0};
    struct Task *identity = (struct Task *)(uintptr_t)0x7500u;
    unsigned int prior_calls;
    unsigned int prior_ready;
    CHECK(krnP4TaskRecordPublishPinned(identity, &record, NULL) == 1);
    CHECK(krnP4TaskRecordRetireForService(identity, NULL) == 1);
    prior_ready = mock_registry_service_ready;
    krnP4SignalTaskUnpin(identity, NULL);
    CHECK(mock_registry_service_ready == prior_ready + 1u);
    CHECK(record.service_pending == 1u && record.service_queued == 1u);
    prior_calls = mock_registry_calls;
    Disable();
    CHECK(krnP4TaskRecordClaimQueuedService(identity, NULL) == &record);
    CHECK(mock_registry_calls == prior_calls + 1u);
    CHECK(mock_disable_depth == 1u && mock_mie == 0 && !mock_gate_held);
    Enable();
    CHECK(record.service_pending == 0u && record.service_queued == 0u);
    CHECK(record.lifetime.notification_pins == 0u && record.next == NULL);
    CHECK(krnP4TaskRecordClaimQueuedService(identity, NULL) == NULL);
    CHECK(krnP4TaskRecordClaimQueuedService(NULL, NULL) == NULL);
    CHECK(mock_mie == 1 && mock_disable_depth == 0u && !mock_gate_held);
    return 1;
}

static void check_registry_record_equal(
    const struct p4_task_registry_record *actual,
    const struct p4_task_registry_record *expected)
{
    CHECK(actual->next == expected->next);
    CHECK(actual->identity == expected->identity);
    CHECK(actual->service_pending == expected->service_pending);
    CHECK(actual->service_queued == expected->service_queued);
    CHECK(actual->worker_protected == expected->worker_protected);
    CHECK(actual->lifetime.notification_pins ==
          expected->lifetime.notification_pins);
    CHECK(actual->lifetime.running_harts == expected->lifetime.running_harts);
    CHECK(actual->lifetime.flags == expected->lifetime.flags);
}

static int test_cleanup_worker_owner_binding(void)
{
    struct p4_task_registry_record not_running_record = {0};
    struct p4_task_registry_record other_record = {0};
    struct p4_task_registry_record before;
    struct p4_task_registry_record corrupt;
    struct Task not_running_task = {0};
    struct Task other_task = {0};
    struct Task *missing = (struct Task *)(uintptr_t)0x7a00u;
    unsigned int prior_protects;
    unsigned int prior_checks;
    unsigned int prior_fatal;

    CHECK(mock_test_registry != NULL);
    CHECK(mock_test_registry != NULL && mock_test_registry->count == 0u);

    /* NULL and unknown identities fail closed, and worker protection cannot
       fabricate a registry owner for either. */
    prior_protects = mock_registry_worker_protects;
    CHECK(krnP4ProtectCleanupWorker(NULL, NULL) == 0);
    CHECK(krnP4ProtectCleanupWorker(missing, NULL) == 0);
    CHECK(mock_registry_worker_protects == prior_protects + 2u);
    prior_checks = mock_registry_worker_checks;
    CHECK(krnP4TaskRemovalAllowed(NULL, NULL) == 0);
    CHECK(krnP4TaskRemovalAllowed(missing, NULL) == 0);
    CHECK(mock_registry_worker_checks == prior_checks + 2u);

    /* A valid published identity without a Running owner is not eligible. */
    CHECK(krnP4TaskRecordPublish(&not_running_task, &not_running_record,
                                 NULL) == 1);
    before = not_running_record;
    prior_protects = mock_registry_worker_protects;
    CHECK(krnP4ProtectCleanupWorker(&not_running_task, NULL) == 0);
    CHECK(mock_registry_worker_protects == prior_protects + 1u);
    check_registry_record_equal(&not_running_record, &before);
    CHECK(krnP4TaskRemovalAllowed(&not_running_task, NULL) == 1);
    CHECK(krnP4TaskRecordRetire(&not_running_task, NULL) == 1);
    CHECK(krnP4TaskRecordClaimReclaim(&not_running_task, NULL) ==
          &not_running_record);

    /* Model the live service Task with static Task/record storage. Drop its
       creator pin before binding; the protected binding adds the sole
       permanent pin while a single hart owns the RUN reference. */
    CHECK(krnP4TaskRecordPublishPinned(cleanup_worker_identity,
                                      &cleanup_worker_record, NULL) == 1);
    CHECK(krnP4TaskRecordEnterRunning(cleanup_worker_identity, 0u, NULL) == 1);
    CHECK(cleanup_worker_record.lifetime.notification_pins == 1u);
    krnP4SignalTaskUnpin(cleanup_worker_identity, NULL);
    CHECK(cleanup_worker_record.lifetime.notification_pins == 0u);
    prior_protects = mock_registry_worker_protects;
    CHECK(krnP4ProtectCleanupWorker(cleanup_worker_identity, NULL) == 1);
    CHECK(mock_registry_worker_protects == prior_protects + 1u);
    CHECK(cleanup_worker_record.identity == cleanup_worker_identity);
    CHECK(cleanup_worker_record.worker_protected == 1u);
    CHECK(cleanup_worker_record.lifetime.notification_pins == 1u);
    CHECK(cleanup_worker_record.lifetime.running_harts == UINT32_C(1));
    CHECK(cleanup_worker_record.lifetime.flags == 0u);
    CHECK(krnP4TaskRemovalAllowed(cleanup_worker_identity, NULL) == 0);

    /* The singleton binding refuses a duplicate and a second live worker
       without changing either record or acquiring another permanent pin. */
    before = cleanup_worker_record;
    prior_protects = mock_registry_worker_protects;
    CHECK(krnP4ProtectCleanupWorker(cleanup_worker_identity, NULL) == 0);
    CHECK(mock_registry_worker_protects == prior_protects);
    check_registry_record_equal(&cleanup_worker_record, &before);

    CHECK(krnP4TaskRecordPublish(&other_task, &other_record, NULL) == 1);
    CHECK(krnP4TaskRecordEnterRunning(&other_task, 1u, NULL) == 1);
    before = other_record;
    prior_protects = mock_registry_worker_protects;
    CHECK(krnP4ProtectCleanupWorker(&other_task, NULL) == 0);
    CHECK(mock_registry_worker_protects == prior_protects);
    check_registry_record_equal(&other_record, &before);
    CHECK(krnP4TaskRemovalAllowed(&other_task, NULL) == 1);
    krnP4TaskRecordLeaveRunning(&other_task, 1u, NULL);
    CHECK(krnP4TaskRecordRetire(&other_task, NULL) == 1);
    CHECK(krnP4TaskRecordClaimReclaim(&other_task, NULL) == &other_record);

    /* Neither ordinary nor service retirement may consume the protected
       worker's permanent identity pin. */
    before = cleanup_worker_record;
    CHECK(krnP4TaskRecordRetire(cleanup_worker_identity, NULL) == 0);
    check_registry_record_equal(&cleanup_worker_record, &before);
    CHECK(krnP4TaskRecordRetireForService(cleanup_worker_identity, NULL) == 0);
    check_registry_record_equal(&cleanup_worker_record, &before);

    /* Transient signal references can drain, but the final permanent pin is
       an invariant floor and an attempted underflow fails closed. */
    CHECK(krnP4SignalTaskPin(cleanup_worker_identity, NULL) == 1);
    CHECK(cleanup_worker_record.lifetime.notification_pins == 2u);
    krnP4SignalTaskUnpin(cleanup_worker_identity, NULL);
    CHECK(cleanup_worker_record.lifetime.notification_pins == 1u);
    before = cleanup_worker_record;
    prior_fatal = mock_fatal_calls;
    expect_fatal(P4_PUBLIC_SPIN_BAD_STATE,
                 operation_cleanup_worker_unpin_floor);
    CHECK(mock_fatal_calls == prior_fatal + 1u);
    mock_reset_gate_after_fatal();
    check_registry_record_equal(&cleanup_worker_record, &before);

    /* Corrupt lookup state also denies removal; the check is read-only. */
    cleanup_worker_record.worker_protected = 2u;
    corrupt = cleanup_worker_record;
    CHECK(krnP4TaskRemovalAllowed(cleanup_worker_identity, NULL) == 0);
    check_registry_record_equal(&cleanup_worker_record, &corrupt);
    cleanup_worker_record.worker_protected = 1u;

    /* The permanent record survives RUN-owner handoff and remains protected. */
    CHECK(krnP4TaskRecordRunningOwner(cleanup_worker_identity, NULL) == 1);
    krnP4TaskRecordLeaveRunning(cleanup_worker_identity, 0u, NULL);
    CHECK(cleanup_worker_record.lifetime.running_harts == 0u);
    CHECK(krnP4TaskRecordEnterRunning(cleanup_worker_identity, 1u, NULL) == 1);
    CHECK(krnP4TaskRecordRunningOwner(cleanup_worker_identity, NULL) == 2);
    CHECK(krnP4TaskRemovalAllowed(cleanup_worker_identity, NULL) == 0);
    CHECK(krnP4TaskRecordRetire(cleanup_worker_identity, NULL) == 0);
    CHECK(krnP4TaskRecordRetireForService(cleanup_worker_identity, NULL) == 0);
    CHECK(cleanup_worker_record.worker_protected == 1u);
    CHECK(cleanup_worker_record.lifetime.notification_pins == 1u);
    CHECK(cleanup_worker_record.lifetime.running_harts == UINT32_C(2));
    CHECK(cleanup_worker_record.lifetime.flags == 0u);
    CHECK(mock_test_registry->count == 1u);
    CHECK(mock_test_registry->head == &cleanup_worker_record);
    CHECK(cleanup_worker_record.next == NULL);
    CHECK(mock_registry_context_violations == 0u);
    return 1;
}

static void test_semaphore_wait_scope_binding(void)
{
    struct ExecBase *base = (struct ExecBase *)(uintptr_t)UINT32_C(0x5a100000);
    uint32_t wait_call;

    /* A completed list wait drops only its scope pin. Grants have independent
       notification pins, and service cleanup remains blocked until RUN drains. */
    {
        struct p4_task_registry_record record = {0};
        struct p4_cleanup_wake wake;
        uint32_t generation = 0u;
        int list_cookie = 1;
        uint32_t prior_count = mock_test_registry->count;
        void *mapping = mmap(NULL, cleanup_worker_mapping_size, PROT_NONE,
                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        struct Task *identity;

        CHECK(mapping != MAP_FAILED);
        if (mapping == MAP_FAILED)
            return;
        identity = (struct Task *)mapping;
        CHECK(!p4_cleanup_wake_pending);
        CHECK(krnP4TaskRecordPublish(identity, &record, base) == 1);
        CHECK(krnP4TaskRecordEnterRunning(identity, 0u, base) == 1);
        CHECK(krnP4SemaphoreWaitBegin(identity, P4_TASK_WAIT_LIST, NULL, NULL,
                                      &list_cookie, &generation, base) == 1);
        CHECK(generation == 1u);
        CHECK(record.wait_scope.active == 1u);
        CHECK(record.wait_scope.kind == P4_TASK_WAIT_LIST);
        CHECK(record.lifetime.notification_pins == 1u);
        CHECK(krnP4SemaphoreWaitAddRequest(identity, generation, base) == 1);
        CHECK(krnP4SemaphoreWaitAddRequest(identity, generation, base) == 1);
        CHECK(krnP4SemaphoreWaitClose(identity, generation, base) == 1);
        CHECK(record.wait_scope.queued_count == 2u);
        CHECK(krnP4SemaphoreWaitReady(identity, generation, base) == 0);
        CHECK(krnP4SemaphoreWaitGrantRequest(identity, generation, base) == 1);
        CHECK(record.wait_scope.queued_count == 1u);
        CHECK(record.lifetime.notification_pins == 2u);
        CHECK(krnP4SemaphoreWaitGrantRequest(identity, generation, base) == 1);
        CHECK(record.wait_scope.queued_count == 0u);
        CHECK(record.lifetime.notification_pins == 3u);
        CHECK(krnP4SemaphoreWaitReady(identity, generation, base) == 1);
        CHECK(krnP4SemaphoreWaitFinish(identity, generation, base) == 1);
        CHECK(record.wait_scope.active == 0u);
        CHECK(record.wait_scope.generation == generation);
        CHECK(record.lifetime.notification_pins == 2u);

        expect_cleanup_poke_context(base);
        CHECK(krnP4TaskRecordRetireForService(identity, base) == 1);
        CHECK(record.service_pending == 1u);
        CHECK(record.service_queued == 0u);
        CHECK(record.lifetime.notification_pins == 3u);
        krnP4SignalTaskUnpin(identity, base);
        CHECK(record.lifetime.notification_pins == 2u);
        CHECK(record.service_queued == 0u);
        krnP4SignalTaskUnpin(identity, base);
        CHECK(record.lifetime.notification_pins == 1u);
        CHECK(record.service_queued == 0u);
        CHECK(!p4_cleanup_wake_pending);

        expect_cleanup_poke_context(base);
        krnP4TaskRecordLeaveRunning(identity, 0u, base);
        CHECK(record.service_queued == 1u);
        CHECK(p4_cleanup_wake_pending);
        CHECK(krnP4TakeCleanupWake(&wake, base) == 1);
        CHECK(wake.worker == cleanup_worker_identity);
        CHECK(krnP4TaskRecordClaimNextRetainedService(base) == &record);
        CHECK(record.service_inflight == 1u);
        krnP4TaskRecordFinishRetainedService(&record, base);
        CHECK(mock_test_registry->count == prior_count);
        CHECK(!mock_gate_held && mock_disable_depth == 0u && mock_mie == 1);
        CHECK(munmap(mapping, cleanup_worker_mapping_size) == 0);
    }

    /* Retirement with queued requests wakes cancellation before pin drain. A
       claimed zero-queue cancellation stays quiet while RUN is held, becomes
       eligible on final RUN leave, and blocks retained cleanup until its scope
       has finished. */
    {
        struct p4_task_registry_record record = {0};
        struct p4_task_wait_cancel_snapshot snapshot;
        struct p4_task_wait_cancel_snapshot duplicate;
        struct p4_cleanup_wake wake;
        struct p4_task_registry_record before_finish;
        uint32_t generation = 0u;
        int list_cookie = 2;
        uint32_t prior_count = mock_test_registry->count;
        unsigned int prior_pokes;
        void *mapping = mmap(NULL, cleanup_worker_mapping_size, PROT_NONE,
                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        struct Task *identity;

        CHECK(mapping != MAP_FAILED);
        if (mapping == MAP_FAILED)
            return;
        identity = (struct Task *)mapping;
        CHECK(!p4_cleanup_wake_pending);
        CHECK(krnP4TaskRecordPublishPinned(identity, &record, base) == 1);
        CHECK(krnP4TaskRecordEnterRunning(identity, 1u, base) == 1);
        CHECK(krnP4SemaphoreWaitBegin(identity, P4_TASK_WAIT_LIST, NULL, NULL,
                                      &list_cookie, &generation, base) == 1);
        CHECK(krnP4SemaphoreWaitAddRequest(identity, generation, base) == 1);
        CHECK(krnP4SemaphoreWaitAddRequest(identity, generation, base) == 1);
        CHECK(krnP4SemaphoreWaitClose(identity, generation, base) == 1);
        CHECK(record.lifetime.notification_pins == 2u);
        prior_pokes = mock_cleanup_poke_calls;
        expect_cleanup_poke_context(base);
        CHECK(krnP4TaskRecordRetireForService(identity, base) == 1);
        CHECK(record.lifetime.notification_pins == 3u);
        CHECK(record.wait_scope.active == 1u);
        CHECK(record.wait_scope.queued_count == 2u);
        CHECK(record.service_queued == 0u);
        CHECK(p4_cleanup_wake_pending);
        CHECK(mock_cleanup_poke_calls == prior_pokes + 1u);

        CHECK(krnP4TakeCleanupWake(&wake, base) == 1);
        memset(&snapshot, 0, sizeof(snapshot));
        CHECK(krnP4SemaphoreWaitClaimCancel(&snapshot, base) == 1);
        CHECK(snapshot.identity == identity);
        CHECK(snapshot.generation == generation);
        CHECK(snapshot.kind == P4_TASK_WAIT_LIST);
        CHECK(snapshot.queued_count == 2u);
        CHECK(snapshot.construction_closed == 1u);
        CHECK(snapshot.cancel_requested == 1u);
        CHECK(record.wait_scope.cancel_requested == 1u);
        CHECK(record.wait_scope.cancel_claimed == 1u);

        memset(&duplicate, 0xa5, sizeof(duplicate));
        CHECK(krnP4SemaphoreWaitClaimCancel(&duplicate, base) == 0);
        CHECK(duplicate.identity == NULL);
        CHECK(duplicate.generation == 0u);
        CHECK(krnP4SemaphoreWaitWithdrawRequest(identity, generation, base) == 1);
        CHECK(krnP4SemaphoreWaitWithdrawRequest(identity, generation, base) == 1);
        CHECK(record.wait_scope.queued_count == 0u);
        before_finish = record;
        CHECK(krnP4SemaphoreWaitFinishCancel(identity, generation, base) == 0);
        CHECK(memcmp(&record, &before_finish, sizeof(record)) == 0);
        CHECK(krnP4SemaphoreWaitReleaseCancel(identity, generation, base) == 1);
        CHECK(record.wait_scope.cancel_requested == 1u);
        CHECK(record.wait_scope.cancel_claimed == 0u);
        CHECK(record.wait_scope.queued_count == 0u);
        CHECK(!p4_cleanup_wake_pending);
        CHECK(krnP4TaskRecordClaimNextRetainedService(base) == NULL);

        krnP4SignalTaskUnpin(identity, base); /* release creator's ordinary pin */
        CHECK(record.lifetime.notification_pins == 2u);
        CHECK(!p4_cleanup_wake_pending);
        CHECK(mock_cleanup_poke_calls == prior_pokes + 1u);
        CHECK(krnP4TaskRecordClaimNextRetainedService(base) == NULL);

        expect_cleanup_poke_context(base);
        krnP4TaskRecordLeaveRunning(identity, 1u, base);
        CHECK(p4_cleanup_wake_pending);
        CHECK(record.service_queued == 0u);
        CHECK(mock_cleanup_poke_calls == prior_pokes + 2u);
        CHECK(krnP4TakeCleanupWake(&wake, base) == 1);
        memset(&snapshot, 0, sizeof(snapshot));
        CHECK(krnP4SemaphoreWaitClaimCancel(&snapshot, base) == 1);
        CHECK(snapshot.identity == identity);
        CHECK(snapshot.generation == generation);
        CHECK(snapshot.queued_count == 0u);
        CHECK(snapshot.cancel_requested == 1u);
        CHECK(krnP4TaskRecordClaimNextRetainedService(base) == NULL);

        expect_cleanup_poke_context(base);
        CHECK(krnP4SemaphoreWaitFinishCancel(identity, generation, base) == 1);
        CHECK(record.wait_scope.active == 0u);
        CHECK(record.wait_scope.cancel_requested == 0u);
        CHECK(record.lifetime.notification_pins == 1u);
        CHECK(record.service_queued == 1u);
        CHECK(p4_cleanup_wake_pending);
        CHECK(mock_cleanup_poke_calls == prior_pokes + 3u);
        CHECK(krnP4TakeCleanupWake(&wake, base) == 1);
        CHECK(krnP4TaskRecordClaimNextRetainedService(base) == &record);
        CHECK(record.service_inflight == 1u);
        krnP4TaskRecordFinishRetainedService(&record, base);
        CHECK(mock_test_registry->count == prior_count);
        CHECK(!p4_cleanup_wake_pending);
        CHECK(mock_cleanup_poke_context_violations == 0u);
        CHECK(munmap(mapping, cleanup_worker_mapping_size) == 0);
    }

    /* Generic retirement must publish cancellation before the scope pin can
       drain. If RUN leaves while the exclusive cancel claim is held, that
       transition is intentionally quiet; releasing the claim must publish a
       fresh durable wake. The opaque Task identity remains PROT_NONE. */
    {
        struct p4_task_registry_record record = {0};
        struct p4_task_wait_cancel_snapshot snapshot;
        struct p4_cleanup_wake wake;
        uint32_t generation = 0u;
        uint32_t ready_calls;
        int list_cookie = 3;
        uint32_t prior_count = mock_test_registry->count;
        unsigned int prior_pokes;
        void *mapping = mmap(NULL, cleanup_worker_mapping_size, PROT_NONE,
                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        struct Task *identity;

        CHECK(mapping != MAP_FAILED);
        if (mapping == MAP_FAILED)
            return;
        identity = (struct Task *)mapping;
        CHECK(!p4_cleanup_wake_pending);
        CHECK(krnP4TaskRecordPublish(identity, &record, base) == 1);
        CHECK(krnP4TaskRecordEnterRunning(identity, 1u, base) == 1);
        CHECK(krnP4SemaphoreWaitBegin(identity, P4_TASK_WAIT_LIST, NULL, NULL,
                                      &list_cookie, &generation, base) == 1);
        CHECK(krnP4SemaphoreWaitAddRequest(identity, generation, base) == 1);
        CHECK(krnP4SemaphoreWaitAddRequest(identity, generation, base) == 1);
        CHECK(krnP4SemaphoreWaitClose(identity, generation, base) == 1);
        CHECK(record.lifetime.notification_pins == 1u);
        CHECK(record.wait_scope.queued_count == 2u);

        /* The callback runs after its registry gate is released while the
           caller's outer Disable remains in force. */
        prior_pokes = mock_cleanup_poke_calls;
        ready_calls = mock_wait_registry_calls[WAIT_REG_CANCEL_READY];
        Disable();
        expect_cleanup_poke_context(base);
        CHECK(krnP4TaskRecordRetire(identity, base) == 1);
        CHECK(mock_cleanup_poke_calls == prior_pokes + 1u);
        CHECK(mock_cleanup_poke_observed_mie == 0);
        CHECK(mock_cleanup_poke_observed_disable_depth == 1u);
        CHECK(mock_wait_registry_calls[WAIT_REG_CANCEL_READY] ==
              ready_calls + 1u);
        CHECK(record.lifetime.notification_pins == 1u);
        CHECK(record.service_pending == 0u);
        CHECK(record.service_queued == 0u);
        CHECK(p4_cleanup_wake_pending);
        CHECK(!mock_gate_held);
        Enable();
        CHECK(mock_disable_depth == 0u && mock_mie == 1);

        CHECK(krnP4TakeCleanupWake(&wake, base) == 1);
        memset(&snapshot, 0, sizeof(snapshot));
        CHECK(krnP4SemaphoreWaitClaimCancel(&snapshot, base) == 1);
        CHECK(snapshot.identity == identity);
        CHECK(snapshot.generation == generation);
        CHECK(snapshot.queued_count == 2u);
        CHECK(snapshot.cancel_requested == 1u);
        CHECK(record.wait_scope.cancel_claimed == 1u);
        CHECK(krnP4SemaphoreWaitWithdrawRequest(identity, generation, base) ==
              1);
        CHECK(krnP4SemaphoreWaitWithdrawRequest(identity, generation, base) ==
              1);
        CHECK(record.wait_scope.queued_count == 0u);
        CHECK(krnP4SemaphoreWaitFinishCancel(identity, generation, base) == 0);
        CHECK(krnP4TaskRecordClaimReclaim(identity, base) == NULL);

        /* Releasing RUN while cancellation is exclusively claimed cannot
           publish a second wake because the consumer still owns the claim. */
        prior_pokes = mock_cleanup_poke_calls;
        krnP4TaskRecordLeaveRunning(identity, 1u, base);
        CHECK(record.lifetime.running_harts == 0u);
        CHECK(record.wait_scope.cancel_claimed == 1u);
        CHECK(!p4_cleanup_wake_pending);
        CHECK(mock_cleanup_poke_calls == prior_pokes);
        CHECK(krnP4TaskRecordClaimReclaim(identity, base) == NULL);

        /* Once the claim is released, the now-eligible zero-queue scope must
           be durably announced outside the gate at the caller's masked depth. */
        prior_pokes = mock_cleanup_poke_calls;
        ready_calls = mock_wait_registry_calls[WAIT_REG_CANCEL_READY];
        Disable();
        expect_cleanup_poke_context(base);
        CHECK(krnP4SemaphoreWaitReleaseCancel(identity, generation, base) ==
              1);
        CHECK(mock_cleanup_poke_calls == prior_pokes + 1u);
        CHECK(mock_cleanup_poke_observed_mie == 0);
        CHECK(mock_cleanup_poke_observed_disable_depth == 1u);
        CHECK(mock_wait_registry_calls[WAIT_REG_CANCEL_READY] ==
              ready_calls + 1u);
        CHECK(record.wait_scope.cancel_claimed == 0u);
        CHECK(record.wait_scope.cancel_requested == 1u);
        CHECK(p4_cleanup_wake_pending);
        CHECK(!mock_gate_held);
        CHECK(krnP4TaskRecordClaimReclaim(identity, base) == NULL);
        Enable();
        CHECK(mock_disable_depth == 0u && mock_mie == 1);

        CHECK(krnP4TakeCleanupWake(&wake, base) == 1);
        memset(&snapshot, 0, sizeof(snapshot));
        CHECK(krnP4SemaphoreWaitClaimCancel(&snapshot, base) == 1);
        CHECK(snapshot.identity == identity);
        CHECK(snapshot.generation == generation);
        CHECK(snapshot.queued_count == 0u);
        CHECK(snapshot.cancel_requested == 1u);
        CHECK(krnP4SemaphoreWaitFinishCancel(identity, generation, base) == 1);
        CHECK(record.wait_scope.active == 0u);
        CHECK(record.lifetime.notification_pins == 0u);
        CHECK(krnP4TaskRecordClaimReclaim(identity, base) == &record);
        CHECK(mock_test_registry->count == prior_count);
        CHECK(!p4_cleanup_wake_pending);
        CHECK(mock_cleanup_poke_context_violations == 0u);
        CHECK(munmap(mapping, cleanup_worker_mapping_size) == 0);
    }

    for (wait_call = 0u; wait_call < WAIT_REG_CALL_COUNT; ++wait_call)
        CHECK(mock_wait_registry_calls[wait_call] != 0u);
    CHECK(mock_wait_registry_calls[WAIT_REG_CANCEL_READY] >= 3u);
    CHECK(mock_registry_context_violations == 0u);
    CHECK(mock_enable_gate_violations == 0u);
    CHECK(mock_disable_depth == 0u && mock_mie == 1);
}

static struct Task *observer_bad_identity;
static struct ExecBase *observer_bad_base;
static struct Library *observer_bad_output;
static void operation_observe_malformed_registry(void)
{
    (void)krnP4TaskResourceObserve(observer_bad_identity,
        &observer_bad_output, observer_bad_base);
}

static unsigned int bootstrap_apply_calls;
static int test_bootstrap_apply_callback(void *context)
{
    CHECK(mock_gate_held && mock_mie == 0 && mock_disable_depth == 2u);
    ++bootstrap_apply_calls;
    return *(int *)context;
}

static void operation_bootstrap_invalid_outcome(void)
{
    int outcome = -1;
    Disable();
    (void)krnP4TaskResourceBootstrapApply(test_bootstrap_apply_callback,
        &outcome, observer_bad_base);
}

static void test_taskresource_observer_binding(void)
{
    struct ExecBase base = {0};
    struct Library library = {0};
    struct Library *observed = &library;
    struct p4_task_registry_record record = {0};
    unsigned int locks;
    void *mapping = mmap(NULL, cleanup_worker_mapping_size, PROT_NONE,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    struct Task *identity = (struct Task *)mapping;
    CHECK(mapping != MAP_FAILED);
    if (mapping == MAP_FAILED)
        return;
    CHECK(krnP4TaskRecordPublishPinned(identity, &record, &base) == 1);
    CHECK(krnP4TaskResourceObserve(identity, &observed, &base) == 1);
    CHECK(observed == NULL && record.taskresource_unobserved == 1u);
    CHECK(record.lifetime.notification_pins == 1u);
    {
        int outcome = 1;
        bootstrap_apply_calls = 0u;
        Disable();
        CHECK(krnP4TaskResourceBootstrapApply(test_bootstrap_apply_callback,
            &outcome, &base) == 0); /* Ordinary constructor pin refuses. */
        CHECK(bootstrap_apply_calls == 0u);
        CHECK(mock_disable_depth == 1u && !mock_gate_held && mock_mie == 0);
        Enable();
    }
    observed = &library;
    CHECK(krnP4TaskResourceObserve(identity, &observed, &base) == 1);
    CHECK(observed == NULL && record.taskresource_unobserved == 1u);
    base.lb_TaskResBase = &library;
    CHECK(krnP4TaskResourceObserve(identity, &observed, &base) == 1);
    CHECK(observed == &library && record.taskresource_unobserved == 1u);
    observed = &library;
    CHECK(krnP4TaskResourceObserve(NULL, &observed, &base) == 0);
    CHECK(observed == &library);
    locks = mock_try_calls;
    CHECK(krnP4TaskResourceObserve(identity, NULL, &base) == 0);
    CHECK(krnP4TaskResourceObserve(identity, &observed, NULL) == 0);
    CHECK(mock_try_calls == locks);
    record.taskresource_unobserved = 2u;
    observer_bad_identity = identity;
    observer_bad_base = &base;
    observer_bad_output = &library;
    expect_fatal(P4_PUBLIC_SPIN_BAD_STATE,
        operation_observe_malformed_registry);
    CHECK(observer_bad_output == &library);
    CHECK(mock_gate_held && mock_disable_depth == 1u && mock_mie == 0);
    mock_reset_gate_after_fatal();
    record.taskresource_unobserved = 1u;
    Disable();
    CHECK(krnP4TaskResourceObserve(identity, &observed, &base) == 1);
    CHECK(mock_disable_depth == 1u && mock_mie == 0 && !mock_gate_held);
    Enable();
    krnP4SignalTaskUnpin(identity, &base);
    {
        int outcome;
        for (outcome = 0; outcome <= 2; ++outcome)
        {
            Disable();
            CHECK(krnP4TaskResourceBootstrapApply(test_bootstrap_apply_callback,
                &outcome, &base) == outcome);
            CHECK(mock_disable_depth == 1u && !mock_gate_held && mock_mie == 0);
            Enable();
        }
        CHECK(bootstrap_apply_calls == 3u);
        /* This callback foundation deliberately does not process debt yet. */
        CHECK(record.taskresource_unobserved == 1u);
        locks = mock_try_calls;
        CHECK(krnP4TaskResourceBootstrapApply(NULL, &outcome, &base) == 0);
        CHECK(krnP4TaskResourceBootstrapApply(test_bootstrap_apply_callback,
            NULL, &base) == 0);
        CHECK(krnP4TaskResourceBootstrapApply(test_bootstrap_apply_callback,
            &outcome, NULL) == 0);
        CHECK(mock_try_calls == locks);
        record.taskresource_unobserved = 2u;
        CHECK(krnP4TaskResourceBootstrapApply(test_bootstrap_apply_callback,
            &outcome, &base) == 0);
        CHECK(bootstrap_apply_calls == 3u);
        record.taskresource_unobserved = 1u;
        expect_fatal(P4_PUBLIC_SPIN_BAD_STATE,
            operation_bootstrap_invalid_outcome);
        CHECK(bootstrap_apply_calls == 4u);
        CHECK(mock_gate_held && mock_disable_depth == 2u && mock_mie == 0);
        mock_reset_gate_after_fatal();
    }
    observed = &library;
    CHECK(krnP4TaskResourceObserve(identity, &observed, &base) == 0);
    CHECK(observed == &library && record.taskresource_unobserved == 1u);
    CHECK(krnP4TaskRecordRetire(identity, &base) == 1);
    CHECK(krnP4TaskResourceObserve(identity, &observed, &base) == 0);
    CHECK(observed == &library);
    CHECK(krnP4TaskRecordClaimReclaim(identity, &base) == &record);
    CHECK(record.taskresource_unobserved == 0u);
    CHECK(mock_disable_depth == 0u && mock_mie == 1 && !mock_gate_held);
    CHECK(munmap(mapping, cleanup_worker_mapping_size) == 0);
}

int main(void)
{
    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0)
    {
        fputs("could not determine page size\n", stderr);
        return EXIT_FAILURE;
    }
    cleanup_worker_mapping_size = (size_t)page_size;
    cleanup_worker_mapping = mmap(NULL, cleanup_worker_mapping_size, PROT_NONE,
                                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (cleanup_worker_mapping == MAP_FAILED)
    {
        perror("mmap PROT_NONE cleanup worker identity");
        return EXIT_FAILURE;
    }
    cleanup_worker_identity = (struct Task *)cleanup_worker_mapping;

    (void)test_prepare_once();
    (void)test_invalid_identity_and_lifetime();
    (void)test_publish_pinned_creator_pin_and_registration_failure();
    (void)test_unique_running_handoff();
    (void)test_service_reference_drain_and_claim();
    (void)test_service_owner_binding();
    (void)test_queued_service_binding();
    (void)test_cleanup_worker_owner_binding();
    (void)test_cleanup_wake_binding();
    test_retired_owner_poke();
    test_retained_service_binding();
    (void)test_nested_disable_and_callback_reentry();
    (void)test_busy_timeout_retry_then_success();
    (void)test_invalid_state_fails_closed();
    (void)test_bounded_timeout_fails_closed();
    (void)test_unpin_underflow_fails_closed();
    (void)test_release_error_fails_closed();
    test_retained_finish_fails_closed();
    test_semaphore_wait_scope_binding();
    test_taskresource_observer_binding();

    CHECK(mock_registry_context_violations == 0u);
    CHECK(mock_enable_gate_violations == 0u);
    CHECK(mock_cleanup_poke_context_violations == 0u);
    CHECK(!mock_gate_held);
    CHECK(mock_disable_depth == 0u);

    if (munmap(cleanup_worker_mapping, cleanup_worker_mapping_size) != 0)
    {
        perror("munmap PROT_NONE cleanup worker identity");
        return EXIT_FAILURE;
    }

    if (failures != 0u)
    {
        fprintf(stderr, "%u of %u checks failed\n", failures, checks);
        return EXIT_FAILURE;
    }

    printf("P4 Task-pin binding checks: %u passed\n", checks);
    return EXIT_SUCCESS;
}
