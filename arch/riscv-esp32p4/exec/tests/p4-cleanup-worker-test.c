/* Host fixture for the actual P4 cleanup-worker implementation. */
#define P4_CLEANUP_WORKER_TEST 1

#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../p4_signal_protocol.h"
#include "../p4_semaphore_cancel_worker.h"
#include "../../kernel/p4_task_pins.h"

#define SIGF_SINGLE UINT32_C(0x10)

struct Task
{
    uint32_t fixture_word;
};

struct MsgPort
{
    struct Task *mp_SigTask;
    uint8_t mp_SigBit;
};

struct ExecBase
{
    struct MsgPort *ServicePort;
};

static struct ExecBase test_sysbase;
static struct Task mock_current_task;

#define GET_THIS_TASK (&mock_current_task)
#define PrivExecBase(base) ((struct ExecBase *)(base))
#define SetSignal(new_signals, signal_set) \
    mock_set_signal((new_signals), (signal_set))
#define Wait(signal_set) mock_wait((signal_set))

void Disable(void);
void Enable(void);
uint32_t mock_set_signal(uint32_t new_signals, uint32_t signal_set);
uint32_t mock_wait(uint32_t signal_set);
int krnP4TakeCleanupWake(struct p4_cleanup_wake *, struct ExecBase *);
int krnP4ArmCleanupWake(struct Task *, uint32_t, struct ExecBase *);
int krnP4CleanupOneRetiredTask(struct ExecBase *);
int Exec_P4SemaphoreCancellationPass(struct Task *, struct ExecBase *);
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

#include "../p4_cleanup_worker.c"

enum event_kind
{
    EV_DISABLE = 1,
    EV_TAKE,
    EV_TASK_LOCK,
    EV_RAISE,
    EV_QUEUE_APPLY,
    EV_TASK_UNLOCK,
    EV_POST_WAKE,
    EV_ENABLE,
    EV_FATAL,
    EV_ARM,
    EV_SET_SIGNAL,
    EV_CLEANUP,
    EV_CANCEL_PASS,
    EV_WAIT_ENTER,
    EV_WAIT_INJECT,
    EV_WAIT_WAKE,
    EV_WAIT_BLOCK,
    EV_NOTIFICATION_CLAIM,
    EV_NOTIFICATION_FINISH
};

enum service_work_kind
{
    SERVICE_NOTIFICATION = 1,
    SERVICE_CANCELLATION,
    SERVICE_CLEANUP
};

static unsigned int checks;
static unsigned int failures;
static unsigned int event_count;
static enum event_kind events[64];
static int mock_mie = 1;
static unsigned int mock_disable_depth;
static unsigned int disable_calls;
static unsigned int enable_calls;
static int task_lock_held;
static unsigned int task_lock_calls;
static unsigned int task_unlock_calls;
static struct Task *adapter_expected_task;
static uint32_t adapter_expected_signal;
static int adapter_is_notification;
static unsigned int take_calls;
static int take_result;
static struct Task *take_worker;
static uint32_t take_mask;
static unsigned int raise_calls;
static enum p4_signal_status raise_status;
static enum p4_signal_queue_transition raise_transition;
static uint8_t raise_request_reschedule;
static unsigned int queue_apply_calls;
static int queue_apply_succeeds = 1;
static unsigned int post_calls;
static unsigned int fatal_calls;
static int fatal_armed;
static int fatal_expected_mie;
static unsigned int fatal_expected_disable_depth;
static jmp_buf fatal_jump;
static unsigned int arm_calls;
static int arm_result = 1;
static struct Task *arm_expected_worker;
static uint32_t arm_expected_mask;
static unsigned int cleanup_calls;
static int cleanup_script[8];
static unsigned int cleanup_script_count;
static unsigned int cleanup_script_index;
static unsigned int cancel_pass_calls;
static int cancel_pass_script[8];
static unsigned int cancel_pass_script_count;
static unsigned int cancel_pass_script_index;
static unsigned int last_cleanup_clear_generation;
static unsigned int clear_generation;
static int cleanup_in_current_generation;
static unsigned int set_signal_calls;
static unsigned int wait_calls;
static uint32_t mock_signal_bits;
static unsigned int notification_claim_calls;
static unsigned int notification_finish_calls;
static int notification_claim_result;
static struct p4_semaphore_notification notification_snapshot;
static struct Task notification_target;
static uint32_t notification_target_pins;
static int notification_target_storage_live;
static unsigned int notification_target_reads;
static unsigned int notification_target_reads_after_finish;
static struct p4_semaphore_notification last_claimed_snapshot;
static uint32_t notification_script[8];
static unsigned int notification_script_count;
static unsigned int notification_script_index;
static unsigned int notification_generation;
static unsigned int service_work_order[32];
static unsigned int service_work_order_count;
static int in_service_loop;
static uint32_t service_mask;
static int inject_on_next_wait;
static int injected_wake;
static int wait_stop_armed;
static jmp_buf wait_stop_jump;

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

static void record_event(enum event_kind event)
{
    CHECK(event_count < sizeof(events) / sizeof(events[0]));
    if (event_count < sizeof(events) / sizeof(events[0]))
        events[event_count++] = event;
}

static void clear_trace(void)
{
    /* Keep checks/failures/fatal_calls cumulative; reset only per-call traces. */
    event_count = 0;
    disable_calls = 0;
    enable_calls = 0;
    task_lock_calls = 0;
    task_unlock_calls = 0;
    take_calls = 0;
    raise_calls = 0;
    queue_apply_calls = 0;
    post_calls = 0;
    arm_calls = 0;
    cleanup_calls = 0;
    set_signal_calls = 0;
    wait_calls = 0;
    notification_claim_calls = 0;
    notification_finish_calls = 0;
    notification_script_index = 0;
    service_work_order_count = 0;
    cleanup_script_index = 0;
    cancel_pass_calls = 0;
    cancel_pass_script_index = 0;
    last_cleanup_clear_generation = 0;
    clear_generation = 0;
    cleanup_in_current_generation = 0;
    mock_signal_bits = 0;
    injected_wake = 0;
    inject_on_next_wait = 0;
    task_lock_held = 0;
    notification_target_reads = 0;
    notification_target_reads_after_finish = 0;
    mock_disable_depth = 0;
    mock_mie = 1;
}

static void expect_event_sequence(const enum event_kind *expected, size_t count)
{
    size_t index;
    CHECK(event_count == count);
    for (index = 0; index < count && index < event_count; ++index)
        CHECK(events[index] == expected[index]);
}

static void expect_fatal(void (*operation)(void), int mie, unsigned int depth)
{
    unsigned int prior_fatal = fatal_calls;
    int caught;

    fatal_expected_mie = mie;
    fatal_expected_disable_depth = depth;
    fatal_armed = 1;
    caught = setjmp(fatal_jump);
    if (caught == 0)
    {
        operation();
        fatal_armed = 0;
        CHECK(0 && "expected fail-closed path did not fire");
        return;
    }
    fatal_armed = 0;
    CHECK(fatal_calls == prior_fatal + 1u);
    CHECK(mock_mie == mie);
    CHECK(mock_disable_depth == depth);
    CHECK(!task_lock_held);
    /* Fatal longjmp simulates the production non-returning halt. Reinitialize
       only the mock interrupt context for the next independent case. */
    mock_disable_depth = 0;
    mock_mie = 1;
}

void Disable(void)
{
    ++disable_calls;
    ++mock_disable_depth;
    mock_mie = 0;
    record_event(EV_DISABLE);
}

void Enable(void)
{
    CHECK(mock_disable_depth > 0u);
    CHECK(!task_lock_held);
    ++enable_calls;
    if (mock_disable_depth > 0u)
        --mock_disable_depth;
    if (mock_disable_depth == 0u)
        mock_mie = 1;
    record_event(EV_ENABLE);
}

int krnP4TakeCleanupWake(struct p4_cleanup_wake *out, struct ExecBase *base)
{
    CHECK(base == &test_sysbase);
    CHECK(mock_mie == 0 && mock_disable_depth > 0u);
    CHECK(!task_lock_held);
    ++take_calls;
    record_event(EV_TAKE);
    if (out != NULL)
    {
        out->worker = take_worker;
        out->mask = take_mask;
    }
    return take_result;
}

int krnP4ArmCleanupWake(
    struct Task *worker,
    uint32_t mask,
    struct ExecBase *base)
{
    CHECK(base == &test_sysbase);
    CHECK(mock_mie == 1 && mock_disable_depth == 0u);
    CHECK(!task_lock_held);
    CHECK(worker == arm_expected_worker);
    CHECK(mask == arm_expected_mask);
    ++arm_calls;
    record_event(EV_ARM);
    return arm_result;
}

int krnP4SemaphoreNotificationClaim(
    struct Task *worker,
    struct p4_semaphore_notification *snapshot,
    struct ExecBase *base)
{
    int result = 0;
    CHECK(base == &test_sysbase);
    CHECK(worker == &mock_current_task);
    CHECK(mock_mie == 1 && mock_disable_depth == 0u);
    CHECK(!task_lock_held);
    CHECK(snapshot != NULL);
    ++notification_claim_calls;
    if (in_service_loop && service_work_order_count <
        sizeof(service_work_order) / sizeof(service_work_order[0]))
        service_work_order[service_work_order_count++] = SERVICE_NOTIFICATION;
    record_event(EV_NOTIFICATION_CLAIM);
    if (notification_script_index < notification_script_count)
        result = (int)notification_script[notification_script_index++];
    else
        result = notification_claim_result;
    if (snapshot != NULL)
    {
        if (result == 1)
        {
            notification_snapshot.identity = &notification_target;
            notification_snapshot.count = 1u;
            notification_snapshot.generation = ++notification_generation;
            *snapshot = notification_snapshot;
            last_claimed_snapshot = *snapshot;
            adapter_expected_task = snapshot->identity;
            adapter_expected_signal = SIGF_SINGLE;
            adapter_is_notification = 1;
        }
        else
        {
            memset(snapshot, 0, sizeof(*snapshot));
        }
    }
    return result;
}

void krnP4SemaphoreNotificationFinish(
    struct Task *worker,
    const struct p4_semaphore_notification *snapshot,
    struct ExecBase *base)
{
    CHECK(base == &test_sysbase);
    CHECK(worker == &mock_current_task);
    CHECK(snapshot != NULL);
    CHECK(mock_mie == 1 && mock_disable_depth == 0u);
    CHECK(!task_lock_held);
    CHECK(snapshot != NULL && snapshot->identity == last_claimed_snapshot.identity);
    CHECK(snapshot != NULL && snapshot->count == last_claimed_snapshot.count);
    CHECK(snapshot != NULL && snapshot->generation == last_claimed_snapshot.generation);
    CHECK(notification_target_storage_live);
    CHECK(notification_target_pins >= last_claimed_snapshot.count);
    ++notification_finish_calls;
    record_event(EV_NOTIFICATION_FINISH);
    if (snapshot != NULL && notification_target_pins >= snapshot->count)
    {
        notification_target_pins -= snapshot->count;
        if (notification_target_pins == 0u)
            notification_target_storage_live = 0;
    }
    adapter_is_notification = 0;
}

int krnP4CleanupOneRetiredTask(struct ExecBase *base)
{
    int result = 0;
    CHECK(base == &test_sysbase);
    CHECK(mock_mie == 1 && mock_disable_depth == 0u);
    CHECK(!task_lock_held);
    if (!cleanup_in_current_generation)
    {
        CHECK(clear_generation > last_cleanup_clear_generation);
        last_cleanup_clear_generation = clear_generation;
        cleanup_in_current_generation = 1;
    }
    ++cleanup_calls;
    if (in_service_loop && service_work_order_count <
        sizeof(service_work_order) / sizeof(service_work_order[0]))
        service_work_order[service_work_order_count++] = SERVICE_CLEANUP;
    record_event(EV_CLEANUP);
    if (cleanup_script_index < cleanup_script_count)
        result = cleanup_script[cleanup_script_index++];
    return result;
}

int Exec_P4SemaphoreCancellationPass(struct Task *worker, struct ExecBase *base)
{
    int result = 0;
    CHECK(base == &test_sysbase);
    CHECK(worker == &mock_current_task);
    CHECK(mock_mie == 1 && mock_disable_depth == 0u);
    CHECK(!task_lock_held);
    ++cancel_pass_calls;
    if (in_service_loop && service_work_order_count <
        sizeof(service_work_order) / sizeof(service_work_order[0]))
        service_work_order[service_work_order_count++] = SERVICE_CANCELLATION;
    record_event(EV_CANCEL_PASS);
    if (cancel_pass_script_index < cancel_pass_script_count)
        result = cancel_pass_script[cancel_pass_script_index++];
    return result;
}

void p4_signal_adapter_lock_disabled(struct Task *task)
{
    CHECK(task == adapter_expected_task);
    CHECK(mock_mie == 0 && mock_disable_depth > 0u);
    CHECK(!task_lock_held);
    if (adapter_is_notification)
    {
        if (!notification_target_storage_live || notification_target_pins == 0u)
            ++notification_target_reads_after_finish;
        CHECK(notification_target_storage_live);
        CHECK(notification_target_pins > 0u);
        ++notification_target_reads;
    }
    task_lock_held = 1;
    ++task_lock_calls;
    record_event(EV_TASK_LOCK);
}

struct p4_signal_effect p4_signal_adapter_raise_locked(
    struct Task *task,
    struct ExecBase *base,
    uint32_t signal_set)
{
    struct p4_signal_effect effect;
    memset(&effect, 0, sizeof(effect));
    CHECK(base == &test_sysbase);
    CHECK(task == adapter_expected_task);
    CHECK(signal_set == adapter_expected_signal);
    CHECK(mock_mie == 0 && mock_disable_depth > 0u);
    CHECK(task_lock_held);
    if (adapter_is_notification)
    {
        if (!notification_target_storage_live || notification_target_pins == 0u)
            ++notification_target_reads_after_finish;
        CHECK(notification_target_storage_live);
        CHECK(notification_target_pins > 0u);
        ++notification_target_reads;
    }
    ++raise_calls;
    record_event(EV_RAISE);
    effect.status = raise_status;
    effect.queue_transition = raise_transition;
    effect.request_reschedule = raise_request_reschedule;
    effect.reschedule_hart = 1u;
    return effect;
}

int p4_signal_adapter_apply_queue_locked(
    struct Task *task,
    struct ExecBase *base,
    struct p4_signal_effect *effect)
{
    CHECK(base == &test_sysbase);
    CHECK(task == take_worker);
    CHECK(effect != NULL);
    CHECK(effect != NULL && effect->queue_transition != P4_SIGNAL_QUEUE_NONE);
    CHECK(mock_mie == 0 && mock_disable_depth > 0u);
    CHECK(task_lock_held);
    ++queue_apply_calls;
    record_event(EV_QUEUE_APPLY);
    if (!queue_apply_succeeds)
    {
        effect->status = P4_SIGNAL_STATUS_INVALID_STATE;
        return 0;
    }
    return 1;
}

void p4_signal_adapter_unlock_disabled(struct Task *task)
{
    CHECK(task == adapter_expected_task);
    CHECK(mock_mie == 0 && mock_disable_depth > 0u);
    CHECK(task_lock_held);
    task_lock_held = 0;
    ++task_unlock_calls;
    record_event(EV_TASK_UNLOCK);
}

void p4_signal_adapter_poke_after_unlock(
    const struct p4_signal_effect *effect,
    struct ExecBase *base)
{
    CHECK(base == &test_sysbase);
    CHECK(effect != NULL);
    CHECK(mock_mie == 0 && mock_disable_depth > 0u);
    CHECK(!task_lock_held);
    ++post_calls;
    record_event(EV_POST_WAKE);
}

void p4_signal_adapter_fail_closed(void)
{
    ++fatal_calls;
    record_event(EV_FATAL);
    CHECK(mock_mie == fatal_expected_mie);
    CHECK(mock_disable_depth == fatal_expected_disable_depth);
    CHECK(!task_lock_held);
    if (!fatal_armed)
    {
        fputs("unexpected cleanup-worker fatal\n", stderr);
        abort();
    }
    longjmp(fatal_jump, 1);
}

uint32_t mock_set_signal(uint32_t new_signals, uint32_t signal_set)
{
    uint32_t old_signals = mock_signal_bits;
    CHECK(mock_mie == 1 && mock_disable_depth == 0u);
    CHECK(!task_lock_held);
    CHECK(new_signals == 0u);
    CHECK(signal_set == service_mask);
    ++set_signal_calls;
    ++clear_generation;
    cleanup_in_current_generation = 0;
    mock_signal_bits = (mock_signal_bits & ~signal_set) | new_signals;
    record_event(EV_SET_SIGNAL);
    return old_signals;
}

uint32_t mock_wait(uint32_t signal_set)
{
    CHECK(mock_mie == 1 && mock_disable_depth == 0u);
    CHECK(!task_lock_held);
    CHECK(signal_set == service_mask);
    CHECK(last_cleanup_clear_generation == clear_generation);
    ++wait_calls;
    record_event(EV_WAIT_ENTER);

    /* Inject exactly after the empty drain returned and before Wait tests its
       pending mask. This models the inter-hart signal race at the sleep edge. */
    if (inject_on_next_wait)
    {
        inject_on_next_wait = 0;
        mock_signal_bits |= signal_set;
        injected_wake = 1;
        record_event(EV_WAIT_INJECT);
    }

    if ((mock_signal_bits & signal_set) != 0u)
    {
        mock_signal_bits &= ~signal_set;
        record_event(EV_WAIT_WAKE);
        return signal_set;
    }

    record_event(EV_WAIT_BLOCK);
    if (wait_stop_armed)
        longjmp(wait_stop_jump, 1);
    return 0u;
}

static void operation_deliver_wake(void)
{
    Exec_P4DeliverCleanupWake(&test_sysbase);
}

static void operation_service_loop(void)
{
    Exec_P4ServiceLoop(&test_sysbase);
}

static void setup_delivery(int result, struct Task *worker, uint32_t mask)
{
    clear_trace();
    take_result = result;
    take_worker = worker;
    take_mask = mask;
    adapter_expected_task = worker;
    adapter_expected_signal = mask;
    adapter_is_notification = 0;
    notification_claim_result = 0;
    raise_status = P4_SIGNAL_STATUS_OK;
    raise_transition = P4_SIGNAL_QUEUE_NONE;
    raise_request_reschedule = 0u;
    queue_apply_succeeds = 1;
}

static int test_empty_and_malformed_wake_paths(void)
{
    static const enum event_kind empty_expected[] = {
        EV_DISABLE, EV_TAKE, EV_ENABLE
    };
    struct Task worker = {0};

    setup_delivery(0, NULL, 0u);
    Exec_P4DeliverCleanupWake(&test_sysbase);
    expect_event_sequence(empty_expected,
                          sizeof(empty_expected) / sizeof(empty_expected[0]));
    CHECK(take_calls == 1u && task_lock_calls == 0u && raise_calls == 0u);
    CHECK(queue_apply_calls == 0u && post_calls == 0u && fatal_calls == 0u);
    CHECK(mock_mie == 1 && mock_disable_depth == 0u);

    setup_delivery(-1, NULL, 0u);
    expect_fatal(operation_deliver_wake, 0, 1u);
    CHECK(take_calls == 1u && task_lock_calls == 0u && raise_calls == 0u);
    CHECK(enable_calls == 0u);

    setup_delivery(1, NULL, UINT32_C(1) << 3);
    expect_fatal(operation_deliver_wake, 0, 1u);
    CHECK(task_lock_calls == 0u && raise_calls == 0u && enable_calls == 0u);

    setup_delivery(1, &worker, 0u);
    expect_fatal(operation_deliver_wake, 0, 1u);
    CHECK(task_lock_calls == 0u && raise_calls == 0u && enable_calls == 0u);
    return 1;
}

static int test_raise_queue_unlock_post_order(void)
{
    static const enum event_kind expected[] = {
        EV_DISABLE, EV_TAKE, EV_TASK_LOCK, EV_RAISE, EV_QUEUE_APPLY,
        EV_TASK_UNLOCK, EV_POST_WAKE, EV_ENABLE
    };
    /* This address models the permanently protected worker snapshot. Adapter
       mocks compare it as an identity and intentionally never dereference it. */
    struct Task *worker = (struct Task *)(uintptr_t)0x12345000u;
    setup_delivery(1, worker, UINT32_C(1) << 5);
    raise_transition = P4_SIGNAL_QUEUE_RUNNING_TO_READY;
    raise_request_reschedule = 1u;
    Exec_P4DeliverCleanupWake(&test_sysbase);
    expect_event_sequence(expected, sizeof(expected) / sizeof(expected[0]));
    CHECK(take_calls == 1u && task_lock_calls == 1u && task_unlock_calls == 1u);
    CHECK(raise_calls == 1u && queue_apply_calls == 1u && post_calls == 1u);
    CHECK(mock_mie == 1 && mock_disable_depth == 0u && !task_lock_held);

    /* No queue transition still unlocks before the post-effect adapter; a
       no-reschedule effect produces no underlying poke in this mock. */
    setup_delivery(1, worker, UINT32_C(1) << 5);
    Exec_P4DeliverCleanupWake(&test_sysbase);
    CHECK(queue_apply_calls == 0u && task_lock_calls == 1u);
    CHECK(task_unlock_calls == 1u && post_calls == 1u);
    CHECK(mock_mie == 1 && mock_disable_depth == 0u && !task_lock_held);
    return 1;
}

static int test_raise_and_queue_fail_closed_after_unlock(void)
{
    struct Task worker = {0};

    setup_delivery(1, &worker, UINT32_C(1) << 6);
    raise_status = P4_SIGNAL_STATUS_INVALID_STATE;
    expect_fatal(operation_deliver_wake, 0, 1u);
    CHECK(task_lock_calls == 1u && task_unlock_calls == 1u);
    CHECK(post_calls == 0u && enable_calls == 0u);

    setup_delivery(1, &worker, UINT32_C(1) << 6);
    raise_transition = P4_SIGNAL_QUEUE_RUNNING_TO_READY;
    queue_apply_succeeds = 0;
    expect_fatal(operation_deliver_wake, 0, 1u);
    CHECK(queue_apply_calls == 1u && task_lock_calls == 1u);
    CHECK(task_unlock_calls == 1u && post_calls == 0u && enable_calls == 0u);
    return 1;
}

static void setup_service(struct MsgPort *port, uint8_t bit)
{
    clear_trace();
    memset(port, 0, sizeof(*port));
    port->mp_SigTask = &mock_current_task;
    port->mp_SigBit = bit;
    test_sysbase.ServicePort = port;
    arm_expected_worker = &mock_current_task;
    service_mask = bit < 32u ? UINT32_C(1) << bit : 0u;
    arm_expected_mask = service_mask;
    arm_result = 1;
    raise_status = P4_SIGNAL_STATUS_OK;
    raise_transition = P4_SIGNAL_QUEUE_NONE;
    raise_request_reschedule = 0u;
    queue_apply_succeeds = 1;
    notification_claim_result = 0;
    notification_script_count = 0;
    notification_script_index = 0;
    notification_generation = 0;
    notification_snapshot.identity = NULL;
    notification_snapshot.count = 0;
    notification_snapshot.generation = 0;
    notification_target.fixture_word = 0u;
    notification_target_pins = 0u;
    notification_target_storage_live = 1;
    in_service_loop = 0;
    cleanup_script_count = 0;
    cleanup_script_index = 0;
    cancel_pass_script_count = 0;
    cancel_pass_script_index = 0;
    wait_stop_armed = 0;
}

static int test_service_validation_and_arm_once(void)
{
    struct MsgPort port;

    setup_service(&port, 7u);
    mock_current_task.fixture_word = 17u;
    port.mp_SigTask = NULL;
    expect_fatal(operation_service_loop, 1, 0u);
    CHECK(arm_calls == 0u && set_signal_calls == 0u && cleanup_calls == 0u);

    setup_service(&port, 7u);
    test_sysbase.ServicePort = NULL;
    expect_fatal(operation_service_loop, 1, 0u);
    CHECK(arm_calls == 0u && set_signal_calls == 0u);

    setup_service(&port, 7u);
    port.mp_SigTask = (struct Task *)(uintptr_t)0x8800u;
    expect_fatal(operation_service_loop, 1, 0u);
    CHECK(arm_calls == 0u && set_signal_calls == 0u);

    setup_service(&port, 32u);
    expect_fatal(operation_service_loop, 1, 0u);
    CHECK(arm_calls == 0u && set_signal_calls == 0u);

    setup_service(&port, 4u);
    arm_result = 0;
    expect_fatal(operation_service_loop, 1, 0u);
    CHECK(arm_calls == 1u && set_signal_calls == 0u && cleanup_calls == 0u);

    /* The valid top signal bit is armed exactly once. A wake injected at the
       drain-to-Wait edge remains sticky and causes an immediate next pass. */
    setup_service(&port, 31u);
    cleanup_script[0] = 1;
    cleanup_script[1] = 1;
    cleanup_script[2] = 0;
    cleanup_script[3] = 0;
    cleanup_script_count = 4u;
    notification_script[0] = 1u;
    notification_script[1] = 1u;
    notification_script[2] = 0u;
    notification_script[3] = 0u;
    notification_script_count = 4u;
    notification_target_pins = 2u; /* two already-published durable grants */
    in_service_loop = 1;
    inject_on_next_wait = 1;
    wait_stop_armed = 1;
    if (setjmp(wait_stop_jump) == 0)
        Exec_P4ServiceLoop(&test_sysbase);
    wait_stop_armed = 0;
    in_service_loop = 0;

    CHECK(arm_calls == 1u);
    CHECK(set_signal_calls == 2u && cleanup_calls == 4u);
    CHECK(wait_calls == 2u && injected_wake == 1);
    CHECK(notification_claim_calls == 4u && notification_finish_calls == 2u);
    CHECK(cancel_pass_calls == 4u);
    CHECK(service_work_order_count == 12u);
    for (unsigned int index = 0; index < service_work_order_count; ++index)
        CHECK(service_work_order[index] ==
              (index % 3u == 0u ? SERVICE_NOTIFICATION :
               (index % 3u == 1u ? SERVICE_CANCELLATION : SERVICE_CLEANUP)));
    CHECK(notification_target_pins == 0u && !notification_target_storage_live);
    CHECK(notification_target_reads == 4u);
    CHECK(notification_target_reads_after_finish == 0u);
    CHECK(service_mask == (UINT32_C(1) << 31));
    CHECK(mock_signal_bits == 0u);
    CHECK(mock_mie == 1 && mock_disable_depth == 0u && !task_lock_held);
    CHECK(fatal_calls == 10u);
    {
        static const enum event_kind expected[] = {
            EV_ARM, EV_SET_SIGNAL,
            EV_NOTIFICATION_CLAIM, EV_DISABLE, EV_TASK_LOCK, EV_RAISE,
            EV_TASK_UNLOCK, EV_POST_WAKE, EV_ENABLE, EV_NOTIFICATION_FINISH,
            EV_CANCEL_PASS, EV_CLEANUP,
            EV_NOTIFICATION_CLAIM, EV_DISABLE, EV_TASK_LOCK, EV_RAISE,
            EV_TASK_UNLOCK, EV_POST_WAKE, EV_ENABLE, EV_NOTIFICATION_FINISH,
            EV_CANCEL_PASS, EV_CLEANUP,
            EV_NOTIFICATION_CLAIM, EV_CANCEL_PASS, EV_CLEANUP,
            EV_WAIT_ENTER, EV_WAIT_INJECT, EV_WAIT_WAKE,
            EV_SET_SIGNAL, EV_NOTIFICATION_CLAIM, EV_CANCEL_PASS, EV_CLEANUP,
            EV_WAIT_ENTER, EV_WAIT_BLOCK
        };
        expect_event_sequence(expected,
                              sizeof(expected) / sizeof(expected[0]));
    }
    return 1;
}

static int test_cancel_pass_progress_extends_drain(void)
{
    struct MsgPort port;
    static const enum event_kind expected[] = {
        EV_ARM, EV_SET_SIGNAL,
        EV_NOTIFICATION_CLAIM, EV_CANCEL_PASS, EV_CLEANUP,
        EV_NOTIFICATION_CLAIM, EV_CANCEL_PASS, EV_CLEANUP,
        EV_WAIT_ENTER, EV_WAIT_BLOCK
    };

    setup_service(&port, 9u);
    cancel_pass_script[0] = 1;
    cancel_pass_script[1] = 0;
    cancel_pass_script_count = 2u;
    in_service_loop = 1;
    wait_stop_armed = 1;
    if (setjmp(wait_stop_jump) == 0)
        Exec_P4ServiceLoop(&test_sysbase);
    wait_stop_armed = 0;
    in_service_loop = 0;

    CHECK(cancel_pass_calls == 2u);
    CHECK(notification_claim_calls == 2u && cleanup_calls == 2u);
    CHECK(set_signal_calls == 1u && wait_calls == 1u);
    CHECK(service_work_order_count == 6u);
    CHECK(service_work_order[0] == SERVICE_NOTIFICATION);
    CHECK(service_work_order[1] == SERVICE_CANCELLATION);
    CHECK(service_work_order[2] == SERVICE_CLEANUP);
    CHECK(service_work_order[3] == SERVICE_NOTIFICATION);
    CHECK(service_work_order[4] == SERVICE_CANCELLATION);
    CHECK(service_work_order[5] == SERVICE_CLEANUP);
    expect_event_sequence(expected, sizeof(expected) / sizeof(expected[0]));
    CHECK(mock_mie == 1 && mock_disable_depth == 0u && !task_lock_held);
    return 1;
}

static int test_blocked_cancel_pass_does_not_spin(void)
{
    struct MsgPort port;
    static const enum event_kind expected[] = {
        EV_ARM, EV_SET_SIGNAL,
        EV_NOTIFICATION_CLAIM, EV_CANCEL_PASS, EV_CLEANUP,
        EV_WAIT_ENTER, EV_WAIT_BLOCK
    };

    setup_service(&port, 10u);
    cancel_pass_script[0] = 0; /* A blocked/no-progress pass remains zero. */
    cancel_pass_script_count = 1u;
    in_service_loop = 1;
    wait_stop_armed = 1;
    if (setjmp(wait_stop_jump) == 0)
        Exec_P4ServiceLoop(&test_sysbase);
    wait_stop_armed = 0;
    in_service_loop = 0;

    CHECK(cancel_pass_calls == 1u);
    CHECK(notification_claim_calls == 1u && cleanup_calls == 1u);
    CHECK(set_signal_calls == 1u && wait_calls == 1u);
    CHECK(service_work_order_count == 3u);
    CHECK(service_work_order[0] == SERVICE_NOTIFICATION);
    CHECK(service_work_order[1] == SERVICE_CANCELLATION);
    CHECK(service_work_order[2] == SERVICE_CLEANUP);
    expect_event_sequence(expected, sizeof(expected) / sizeof(expected[0]));
    CHECK(mock_mie == 1 && mock_disable_depth == 0u && !task_lock_held);
    return 1;
}

int main(void)
{
    test_empty_and_malformed_wake_paths();
    test_raise_queue_unlock_post_order();
    test_raise_and_queue_fail_closed_after_unlock();
    test_service_validation_and_arm_once();
    test_cancel_pass_progress_extends_drain();
    test_blocked_cancel_pass_does_not_spin();

    CHECK(!task_lock_held);
    CHECK(mock_disable_depth == 0u && mock_mie == 1);
    if (failures != 0u)
    {
        fprintf(stderr, "%u of %u checks failed\n", failures, checks);
        return EXIT_FAILURE;
    }
    printf("P4 cleanup-worker checks: %u passed\n", checks);
    return EXIT_SUCCESS;
}
