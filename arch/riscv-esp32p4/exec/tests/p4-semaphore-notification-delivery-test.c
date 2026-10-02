/* Actual-source fixture for durable semaphore notification delivery. */
#define P4_CLEANUP_WORKER_TEST 1

#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../kernel/p4_task_pins.h"
#include "../p4_signal_protocol.h"

#define SIGF_SINGLE UINT32_C(0x10)

_Static_assert(sizeof(struct p4_semaphore_notification) ==
               sizeof(struct Task *) + 2u * sizeof(uint32_t),
               "notification snapshot must not retain request/record pointers");

struct Task
{
    uint32_t fixture_word;
    uint32_t retired;
};

struct MsgPort
{
    struct Task *mp_SigTask;
    uint8_t mp_SigBit;
};

struct ExecBase
{
    struct MsgPort *ServicePort;
    uint32_t fixture_word;
};

static struct ExecBase test_sysbase;
static struct Task mock_worker;
static struct Task mock_target;

void Disable(void);
void Enable(void);
uint32_t mock_set_signal(uint32_t, uint32_t);
uint32_t mock_wait(uint32_t);
int krnP4TakeCleanupWake(struct p4_cleanup_wake *, struct ExecBase *);
int krnP4ArmCleanupWake(struct Task *, uint32_t, struct ExecBase *);
int krnP4CleanupOneRetiredTask(struct ExecBase *);
int krnP4SemaphoreNotificationClaim(
    struct Task *, struct p4_semaphore_notification *, struct ExecBase *);
void krnP4SemaphoreNotificationFinish(
    struct Task *, const struct p4_semaphore_notification *, struct ExecBase *);
void p4_signal_adapter_lock_disabled(struct Task *);
void p4_signal_adapter_unlock_disabled(struct Task *);
struct p4_signal_effect p4_signal_adapter_raise_locked(
    struct Task *, struct ExecBase *, uint32_t);
int p4_signal_adapter_apply_queue_locked(
    struct Task *, struct ExecBase *, struct p4_signal_effect *);
void p4_signal_adapter_poke_after_unlock(
    const struct p4_signal_effect *, struct ExecBase *);
void p4_signal_adapter_fail_closed(void) __attribute__((noreturn));
void Signal(struct Task *, uint32_t);

#define GET_THIS_TASK (&mock_worker)
#define PrivExecBase(base) ((struct ExecBase *)(base))
#define SetSignal(new_signals, signal_set) \
    mock_set_signal((new_signals), (signal_set))
#define Wait(signal_set) mock_wait((signal_set))

#include "../p4_cleanup_worker.c"

enum event_kind
{
    EV_CLAIM = 1,
    EV_DISABLE,
    EV_TASK_LOCK,
    EV_RAISE,
    EV_QUEUE_APPLY,
    EV_TASK_UNLOCK,
    EV_POKE,
    EV_ENABLE,
    EV_FINISH,
    EV_FATAL
};

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

static unsigned int checks;
static unsigned int failures;
static enum event_kind events[32];
static unsigned int event_count;
static int mock_mie = 1;
static unsigned int disable_depth;
static int task_lock_held;
static unsigned int task_reads;
static unsigned int task_reads_after_finish;
static unsigned int claim_calls;
static unsigned int finish_calls;
static unsigned int raise_calls;
static unsigned int apply_calls;
static unsigned int poke_calls;
static unsigned int fatal_calls;
static unsigned int public_signal_calls;
static int claim_result;
static struct p4_semaphore_notification claim_snapshot;
static struct p4_semaphore_notification claimed_snapshot;
static uint32_t durable_pins;
static uint32_t pending_count;
static int target_storage_live;
static int target_retired;
static int simulate_reentrant_completion;
static int operation_completed;
static int request_recycled;
static enum p4_signal_status raise_status;
static enum p4_signal_queue_transition raise_transition;
static int apply_succeeds = 1;
static int fatal_armed;
static jmp_buf fatal_jump;

uint32_t mock_set_signal(uint32_t new_signals, uint32_t signal_set)
{
    (void)new_signals;
    (void)signal_set;
    return 0u;
}

uint32_t mock_wait(uint32_t signal_set)
{
    (void)signal_set;
    return 0u;
}

int krnP4TakeCleanupWake(struct p4_cleanup_wake *out, struct ExecBase *base)
{
    (void)out;
    (void)base;
    return 0;
}

int krnP4ArmCleanupWake(struct Task *worker, uint32_t mask,
                        struct ExecBase *base)
{
    (void)worker;
    (void)mask;
    (void)base;
    return 1;
}

int krnP4CleanupOneRetiredTask(struct ExecBase *base)
{
    (void)base;
    return 0;
}

static void record_event(enum event_kind event)
{
    CHECK(event_count < sizeof(events) / sizeof(events[0]));
    if (event_count < sizeof(events) / sizeof(events[0]))
        events[event_count++] = event;
}

static void reset_fixture(void)
{
    event_count = 0;
    mock_mie = 1;
    disable_depth = 0;
    task_lock_held = 0;
    task_reads = 0;
    task_reads_after_finish = 0;
    claim_calls = 0;
    finish_calls = 0;
    raise_calls = 0;
    apply_calls = 0;
    poke_calls = 0;
    public_signal_calls = 0;
    claim_result = 1;
    memset(&claim_snapshot, 0, sizeof(claim_snapshot));
    memset(&claimed_snapshot, 0, sizeof(claimed_snapshot));
    claim_snapshot.identity = &mock_target;
    claim_snapshot.count = 2u;
    claim_snapshot.generation = 17u;
    durable_pins = claim_snapshot.count;
    pending_count = 0u;
    target_storage_live = 1;
    target_retired = 0;
    simulate_reentrant_completion = 0;
    operation_completed = 0;
    request_recycled = 0;
    raise_status = P4_SIGNAL_STATUS_OK;
    raise_transition = P4_SIGNAL_QUEUE_WAITING_TO_READY;
    apply_succeeds = 1;
    mock_target.fixture_word = UINT32_C(0x1234abcd);
    mock_target.retired = 0u;
}

static void expect_events(const enum event_kind *expected, size_t count)
{
    size_t index;
    CHECK(event_count == count);
    for (index = 0; index < count && index < event_count; ++index)
        CHECK(events[index] == expected[index]);
}

void Disable(void)
{
    ++disable_depth;
    mock_mie = 0;
    record_event(EV_DISABLE);
}

void Enable(void)
{
    CHECK(disable_depth > 0u);
    CHECK(!task_lock_held);
    if (disable_depth > 0u)
        --disable_depth;
    if (disable_depth == 0u)
        mock_mie = 1;
    record_event(EV_ENABLE);
}

int krnP4SemaphoreNotificationClaim(
    struct Task *worker,
    struct p4_semaphore_notification *snapshot,
    struct ExecBase *base)
{
    CHECK(worker == &mock_worker);
    CHECK(base == &test_sysbase);
    CHECK(snapshot != NULL);
    CHECK(mock_mie == 1 && disable_depth == 0u);
    CHECK(!task_lock_held);
    ++claim_calls;
    record_event(EV_CLAIM);
    if (snapshot != NULL)
    {
        if (claim_result == 1)
        {
            *snapshot = claim_snapshot;
            claimed_snapshot = *snapshot;
        }
        else
        {
            memset(snapshot, 0, sizeof(*snapshot));
        }
    }
    return claim_result;
}

void krnP4SemaphoreNotificationFinish(
    struct Task *worker,
    const struct p4_semaphore_notification *snapshot,
    struct ExecBase *base)
{
    CHECK(worker == &mock_worker);
    CHECK(base == &test_sysbase);
    CHECK(snapshot != NULL);
    CHECK(mock_mie == 1 && disable_depth == 0u);
    CHECK(!task_lock_held);
    CHECK(snapshot != NULL && snapshot->identity == claimed_snapshot.identity);
    CHECK(snapshot != NULL && snapshot->count == claimed_snapshot.count);
    CHECK(snapshot != NULL && snapshot->generation == claimed_snapshot.generation);
    CHECK(target_storage_live);
    CHECK(durable_pins >= claimed_snapshot.count);
    ++finish_calls;
    record_event(EV_FINISH);
    if (snapshot != NULL && durable_pins >= snapshot->count)
    {
        durable_pins -= snapshot->count;
        if (target_retired && durable_pins == 0u)
            target_storage_live = 0;
    }
}

void p4_signal_adapter_lock_disabled(struct Task *task)
{
    CHECK(task == &mock_target);
    CHECK(mock_mie == 0 && disable_depth > 0u);
    CHECK(!task_lock_held);
    if (!target_storage_live || durable_pins == 0u)
        ++task_reads_after_finish;
    CHECK(target_storage_live);
    CHECK(durable_pins > 0u);
    ++task_reads;
    task_lock_held = 1;
    record_event(EV_TASK_LOCK);
}

struct p4_signal_effect p4_signal_adapter_raise_locked(
    struct Task *task,
    struct ExecBase *base,
    uint32_t signal_set)
{
    struct p4_signal_effect effect;
    memset(&effect, 0, sizeof(effect));
    CHECK(task == &mock_target);
    CHECK(base == &test_sysbase);
    CHECK(signal_set == SIGF_SINGLE);
    CHECK(mock_mie == 0 && disable_depth > 0u);
    CHECK(task_lock_held);
    if (!target_storage_live || durable_pins == 0u)
        ++task_reads_after_finish;
    CHECK(target_storage_live);
    CHECK(durable_pins > 0u);
    ++task_reads;
    ++raise_calls;
    record_event(EV_RAISE);

    /* A retired identity remains mapped through the held reservation. */
    effect.status = mock_target.retired != 0u ?
        P4_SIGNAL_STATUS_NOT_LIVE : raise_status;
    effect.queue_transition = raise_transition;
    effect.request_reschedule =
        (uint8_t)(raise_status == P4_SIGNAL_STATUS_OK &&
                  mock_target.retired == 0u &&
                  raise_transition != P4_SIGNAL_QUEUE_NONE);
    effect.reschedule_hart = 1u;

    if (simulate_reentrant_completion)
    {
        CHECK(task_lock_held);
        CHECK(durable_pins >= claimed_snapshot.count);
        operation_completed = 1;
        request_recycled = 1;
        pending_count = 3u;
        durable_pins += pending_count;
        simulate_reentrant_completion = 0;
    }
    return effect;
}

int p4_signal_adapter_apply_queue_locked(
    struct Task *task,
    struct ExecBase *base,
    struct p4_signal_effect *effect)
{
    CHECK(task == &mock_target);
    CHECK(base == &test_sysbase);
    CHECK(effect != NULL);
    CHECK(effect != NULL && effect->queue_transition ==
          P4_SIGNAL_QUEUE_WAITING_TO_READY);
    CHECK(mock_mie == 0 && disable_depth > 0u);
    CHECK(task_lock_held);
    ++apply_calls;
    record_event(EV_QUEUE_APPLY);
    if (!apply_succeeds && effect != NULL)
    {
        effect->status = P4_SIGNAL_STATUS_INVALID_STATE;
        return 0;
    }
    return 1;
}

void p4_signal_adapter_unlock_disabled(struct Task *task)
{
    CHECK(task == &mock_target);
    CHECK(mock_mie == 0 && disable_depth > 0u);
    CHECK(task_lock_held);
    task_lock_held = 0;
    record_event(EV_TASK_UNLOCK);
}

void p4_signal_adapter_poke_after_unlock(
    const struct p4_signal_effect *effect,
    struct ExecBase *base)
{
    CHECK(base == &test_sysbase);
    CHECK(effect != NULL);
    CHECK(mock_mie == 0 && disable_depth > 0u);
    CHECK(!task_lock_held);
    ++poke_calls;
    record_event(EV_POKE);
}

void p4_signal_adapter_fail_closed(void)
{
    ++fatal_calls;
    record_event(EV_FATAL);
    CHECK(!task_lock_held);
    if (!fatal_armed)
    {
        fputs("unexpected semaphore-notification fatal\n", stderr);
        abort();
    }
    longjmp(fatal_jump, 1);
}

void Signal(struct Task *task, uint32_t signal_set)
{
    (void)task;
    (void)signal_set;
    ++public_signal_calls;
    CHECK(0 && "public Signal must not take a second reservation pin");
}

static void expect_fail_closed(void)
{
    unsigned int old_fatal = fatal_calls;
    fatal_armed = 1;
    if (setjmp(fatal_jump) == 0)
    {
        (void)Exec_P4DeliverOneSemaphoreNotification(&mock_worker,
            &test_sysbase);
        fatal_armed = 0;
        CHECK(0 && "expected fail-closed path");
        return;
    }
    fatal_armed = 0;
    CHECK(fatal_calls == old_fatal + 1u);
    CHECK(mock_mie == 1 && disable_depth == 0u);
    CHECK(!task_lock_held);
}

static void test_empty_claim_does_not_read_target(void)
{
    static const enum event_kind expected[] = {EV_CLAIM};
    reset_fixture();
    claim_result = 0;
    claim_snapshot.identity = NULL;
    CHECK(Exec_P4DeliverOneSemaphoreNotification(&mock_worker,
        &test_sysbase) == 0);
    expect_events(expected, sizeof(expected) / sizeof(expected[0]));
    CHECK(claim_calls == 1u && finish_calls == 0u);
    CHECK(task_reads == 0u && task_reads_after_finish == 0u);
    CHECK(target_storage_live && durable_pins == 2u);
}

static void test_malformed_claims_fail_closed(void)
{
    reset_fixture();
    claim_result = -1;
    expect_fail_closed();
    CHECK(claim_calls == 1u && task_reads == 0u && finish_calls == 0u);

    reset_fixture();
    claim_snapshot.identity = NULL;
    expect_fail_closed();
    CHECK(task_reads == 0u && finish_calls == 0u);

    reset_fixture();
    claim_snapshot.count = 0u;
    expect_fail_closed();
    CHECK(task_reads == 0u && finish_calls == 0u);

    reset_fixture();
    claim_snapshot.generation = 0u;
    expect_fail_closed();
    CHECK(task_reads == 0u && finish_calls == 0u);
}

static void test_live_signal_finishes_exact_batch_after_unlock(void)
{
    static const enum event_kind expected[] = {
        EV_CLAIM, EV_DISABLE, EV_TASK_LOCK, EV_RAISE, EV_QUEUE_APPLY,
        EV_TASK_UNLOCK, EV_POKE, EV_ENABLE, EV_FINISH
    };
    reset_fixture();
    CHECK(Exec_P4DeliverOneSemaphoreNotification(&mock_worker,
        &test_sysbase) == 1);
    expect_events(expected, sizeof(expected) / sizeof(expected[0]));
    CHECK(claim_calls == 1u && raise_calls == 1u && apply_calls == 1u);
    CHECK(finish_calls == 1u && durable_pins == 0u);
    CHECK(task_reads == 2u && task_reads_after_finish == 0u);
    CHECK(public_signal_calls == 0u);
    CHECK(mock_mie == 1 && disable_depth == 0u && !task_lock_held);
    CHECK(target_storage_live);
}

static void test_reentrant_operation_completion_and_new_pending(void)
{
    reset_fixture();
    simulate_reentrant_completion = 1;
    raise_transition = P4_SIGNAL_QUEUE_NONE;
    CHECK(Exec_P4DeliverOneSemaphoreNotification(&mock_worker,
        &test_sysbase) == 1);
    CHECK(operation_completed && request_recycled);
    CHECK(finish_calls == 1u && pending_count == 3u && durable_pins == 3u);
    CHECK(claimed_snapshot.count == 2u && claimed_snapshot.generation == 17u);
    CHECK(task_reads_after_finish == 0u && public_signal_calls == 0u);
    CHECK(mock_mie == 1 && disable_depth == 0u && !task_lock_held);
}

static void test_retired_target_consumes_reservation_without_queue_apply(void)
{
    static const enum event_kind expected[] = {
        EV_CLAIM, EV_DISABLE, EV_TASK_LOCK, EV_RAISE, EV_TASK_UNLOCK,
        EV_POKE, EV_ENABLE, EV_FINISH
    };
    reset_fixture();
    mock_target.retired = 1u;
    target_retired = 1;
    CHECK(Exec_P4DeliverOneSemaphoreNotification(&mock_worker,
        &test_sysbase) == 1);
    expect_events(expected, sizeof(expected) / sizeof(expected[0]));
    CHECK(apply_calls == 0u && finish_calls == 1u);
    CHECK(task_reads == 2u && task_reads_after_finish == 0u);
    CHECK(durable_pins == 0u && !target_storage_live);
    CHECK(public_signal_calls == 0u);
    CHECK(mock_mie == 1 && disable_depth == 0u && !task_lock_held);
}

int main(void)
{
    test_empty_claim_does_not_read_target();
    test_malformed_claims_fail_closed();
    test_live_signal_finishes_exact_batch_after_unlock();
    test_reentrant_operation_completion_and_new_pending();
    test_retired_target_consumes_reservation_without_queue_apply();

    CHECK(!task_lock_held);
    CHECK(mock_mie == 1 && disable_depth == 0u);
    if (failures != 0u)
    {
        fprintf(stderr, "%u of %u checks failed\n", failures, checks);
        return EXIT_FAILURE;
    }
    printf("P4 semaphore-notification-delivery checks: %u passed\n", checks);
    return EXIT_SUCCESS;
}
