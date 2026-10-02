/* Host execution of the actual P4 Wait body with protocol-backed mocks. */
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../p4_signal_protocol.h"
#include "../p4_signal_protocol.c"

typedef uint32_t ULONG;
typedef int8_t BYTE;

struct ExecBase
{
    uint32_t fixture_word;
};

struct Task
{
    uint32_t tc_SigRecvd;
    uint32_t tc_SigWait;
    uint32_t tc_SigExcept;
    uint8_t tc_Flags;
    BYTE tc_TDNestCnt;
    enum p4_signal_owner_state owner;
};

enum switch_action
{
    SWITCH_SPURIOUS = 0,
    SWITCH_SIGNAL_BEFORE_COMMIT,
    SWITCH_SIGNAL_WHILE_WAITING,
    SWITCH_BLOCK_AFTER_COMMIT
};

static unsigned int checks;
static unsigned int failures;
static struct Task fixture_task;
static struct Task *mock_this_task = &fixture_task;
static struct ExecBase fixture_exec_base;
static struct { uint32_t trap_depth; } fixture_cpu_local;
static struct Task *mock_wait_pending;
static int mock_td_nest;
static int mock_id_nest;
static int mock_switch_active;
static int mock_id_nest_before_switch;
static int mock_task_lock_held;
static unsigned int mock_disable_calls;
static unsigned int mock_enable_calls;
static unsigned int mock_lock_calls;
static unsigned int mock_unlock_calls;
static unsigned int mock_wait_arm_calls;
static unsigned int mock_switch_calls;
static unsigned int mock_switch_lock_violations;
static unsigned int mock_td_restore_calls;
static int mock_saved_td_before_switch;
static unsigned int mock_fatal_calls;
static unsigned int mock_fatal_lock_held;
static int mock_fatal_td_nest;
static int mock_fatal_id_nest;
static int mock_force_adapter_failure;
static int mock_force_nonarmed;
static enum switch_action mock_actions[8];
static uint32_t mock_action_signal;
static unsigned int mock_action_count;
static unsigned int mock_action_index;
static enum p4_signal_queue_transition mock_transitions[8];
static jmp_buf fatal_escape;
static int fatal_escape_armed;
static jmp_buf blocked_wait_escape;
static int blocked_wait_escape_armed;
static int mock_current_retired;
static unsigned int mock_retired_probe_calls;
static unsigned int mock_remove_self_calls;
static jmp_buf removed_wait_escape;
static int removed_wait_escape_armed;

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

static void mock_set_td_nest(int value)
{
    if (!mock_switch_active)
        mock_saved_td_before_switch = mock_td_nest;
    else
    {
        ++mock_td_restore_calls;
        mock_switch_active = 0;
    }
    mock_td_nest = value;
}

static BYTE mock_get_td_nest(void)
{
    return (BYTE)mock_td_nest;
}

static struct p4_signal_task_state task_state_load(const struct Task *task)
{
    struct p4_signal_task_state state;
    state.sig_received = task->tc_SigRecvd;
    state.sig_wait = task->tc_SigWait;
    state.sig_except = task->tc_SigExcept;
    state.owner_hart = 0;
    state.flags = task->tc_Flags;
    state.exception_flag_mask = 0;
    state.owner = task->owner;
    return state;
}

static void task_state_flush(struct Task *task,
                             const struct p4_signal_task_state *state)
{
    task->tc_SigRecvd = state->sig_received;
    task->tc_SigWait = state->sig_wait;
    task->tc_SigExcept = state->sig_except;
    task->tc_Flags = state->flags;
    task->owner = state->owner;
}

static void mock_switch_effect_apply(
    struct Task *task,
    enum p4_signal_queue_transition transition)
{
    if (transition == P4_SIGNAL_QUEUE_RUNNING_TO_WAIT)
        task->owner = P4_SIGNAL_OWNER_WAITING;
    else if (transition == P4_SIGNAL_QUEUE_RUNNING_TO_READY)
        task->owner = P4_SIGNAL_OWNER_READY;
}

void Disable(void);
void Enable(void);
void KrnSwitch(void);
void p4_signal_adapter_lock_disabled(struct Task *task);
void p4_signal_adapter_unlock_disabled(struct Task *task);
int p4_signal_adapter_wait_arm_locked(
    struct Task *task, struct ExecBase *sys_base, uint32_t wait_mask,
    struct p4_signal_effect *effect);
void p4_signal_adapter_fail_closed(void) __attribute__((noreturn));
int krnP4SelfRemoveCurrentRetired(struct ExecBase *sys_base);
void Exec_P4RemoveSelf(struct Task *task, struct ExecBase *sys_base)
    __attribute__((noreturn));

#define P4_WAIT_BODY_TEST 1
#define GET_THIS_TASK mock_this_task
#define P4_EXEC_CPU_LOCAL(base) ((void)(base), &fixture_cpu_local)
#define TDNESTCOUNT_GET mock_get_td_nest()
#define TDNESTCOUNT_SET(value) mock_set_td_nest((int)(value))
#define AROS_LHA(type, name, reg) type name
#define AROS_LH1(type, name, arg, base_type, base_name, vector, library) \
    type name(arg, base_type base_name)
#define AROS_LIBFUNC_INIT
#define AROS_LIBFUNC_EXIT
#include "../wait.c"

void Disable(void)
{
    ++mock_disable_calls;
    ++mock_id_nest;
}

void Enable(void)
{
    ++mock_enable_calls;
    CHECK(!mock_task_lock_held);
    CHECK(mock_id_nest >= 0);
    --mock_id_nest;
}

void p4_signal_adapter_lock_disabled(struct Task *task)
{
    ++mock_lock_calls;
    CHECK(task == mock_this_task);
    CHECK(!mock_task_lock_held);
    CHECK(mock_id_nest >= 0);
    mock_task_lock_held = 1;
}

void p4_signal_adapter_unlock_disabled(struct Task *task)
{
    ++mock_unlock_calls;
    CHECK(task == mock_this_task);
    CHECK(mock_task_lock_held);
    mock_task_lock_held = 0;
}

int p4_signal_adapter_wait_arm_locked(
    struct Task *task, struct ExecBase *sys_base, uint32_t wait_mask,
    struct p4_signal_effect *effect)
{
    struct p4_signal_task_state state;
    void *pending;

    ++mock_wait_arm_calls;
    CHECK(task == mock_this_task);
    CHECK(sys_base == &fixture_exec_base);
    CHECK(mock_task_lock_held);
    if (mock_force_adapter_failure)
    {
        effect->status = P4_SIGNAL_STATUS_INVALID_STATE;
        effect->queue_transition = P4_SIGNAL_QUEUE_NONE;
        effect->wait_outcome = P4_SIGNAL_WAIT_NO_CHANGE;
        effect->returned_signals = 0;
        effect->reschedule_hart = 0;
        effect->request_reschedule = 0;
        effect->remote_reschedule = 0;
        return 0;
    }
    if (mock_force_nonarmed)
    {
        effect->status = P4_SIGNAL_STATUS_OK;
        effect->queue_transition = P4_SIGNAL_QUEUE_NONE;
        effect->wait_outcome = P4_SIGNAL_WAIT_NO_CHANGE;
        effect->returned_signals = 0;
        effect->reschedule_hart = 0;
        effect->request_reschedule = 0;
        effect->remote_reschedule = 0;
        return 1;
    }

    state = task_state_load(task);
    pending = mock_wait_pending;
    *effect = p4_signal_wait_arm_locked(&state, &pending, task, wait_mask);
    if (effect->status == P4_SIGNAL_STATUS_OK)
    {
        task_state_flush(task, &state);
        mock_wait_pending = (struct Task *)pending;
    }
    return effect->status == P4_SIGNAL_STATUS_OK;
}

void p4_signal_adapter_fail_closed(void)
{
    ++mock_fatal_calls;
    mock_fatal_lock_held = (unsigned int)mock_task_lock_held;
    mock_fatal_td_nest = mock_td_nest;
    mock_fatal_id_nest = mock_id_nest;
    if (!fatal_escape_armed)
    {
        fputs("unexpected Wait fail-closed path\n", stderr);
        abort();
    }
    longjmp(fatal_escape, 1);
}

int krnP4SelfRemoveCurrentRetired(struct ExecBase *sys_base)
{
    ++mock_retired_probe_calls;
    CHECK(sys_base == &fixture_exec_base);
    CHECK(mock_id_nest >= 0 && !mock_task_lock_held);
    return mock_current_retired;
}

void Exec_P4RemoveSelf(struct Task *task, struct ExecBase *sys_base)
{
    ++mock_remove_self_calls;
    CHECK(task == mock_this_task && sys_base == &fixture_exec_base);
    CHECK(mock_id_nest >= 0 && !mock_task_lock_held);
    if (!removed_wait_escape_armed)
        abort();
    longjmp(removed_wait_escape, 1);
}

void KrnSwitch(void)
{
    enum switch_action action;
    struct p4_signal_task_state state;
    struct p4_signal_effect effect;
    void *pending;

    ++mock_switch_calls;
    if (mock_task_lock_held)
        ++mock_switch_lock_violations;
    CHECK(!mock_task_lock_held);
    CHECK(mock_id_nest >= 0);
    CHECK(mock_td_nest == -1);
    mock_id_nest_before_switch = mock_id_nest;
    mock_switch_active = 1;
    CHECK(mock_this_task != NULL);
    CHECK(mock_this_task != NULL &&
          mock_this_task->tc_TDNestCnt ==
              (BYTE)mock_saved_td_before_switch);
    CHECK(mock_wait_pending == mock_this_task);
    if (mock_action_index >= mock_action_count)
    {
        CHECK(0 && "unexpected KrnSwitch");
        return;
    }
    action = mock_actions[mock_action_index++];

    /* Model an ordinary Signal after Wait drops Task.tc_SpinLock but before
       the saved-context switch hook commits the wait token. */
    if (action == SWITCH_SIGNAL_BEFORE_COMMIT)
    {
        state = task_state_load(mock_this_task);
        effect = p4_signal_raise_locked(
            &state, 1, mock_action_signal, 1u);
        CHECK(effect.status == P4_SIGNAL_STATUS_OK);
        CHECK(effect.queue_transition == P4_SIGNAL_QUEUE_NONE);
        CHECK(effect.request_reschedule == 1u);
        task_state_flush(mock_this_task, &state);
    }

    /* The real switch hook runs after context save, then consumes this hart's
       token under the Task lock. */
    mock_task_lock_held = 1;
    state = task_state_load(mock_this_task);
    pending = mock_wait_pending;
    effect = p4_signal_switch_commit_locked(
        &state, &pending, mock_this_task);
    CHECK(effect.status == P4_SIGNAL_STATUS_OK);
    mock_transitions[mock_switch_calls - 1u] = effect.queue_transition;
    task_state_flush(mock_this_task, &state);
    mock_wait_pending = (struct Task *)pending;
    mock_switch_effect_apply(mock_this_task, effect.queue_transition);
    CHECK(mock_wait_pending == NULL);
    mock_task_lock_held = 0;

    if (action == SWITCH_SIGNAL_WHILE_WAITING)
    {
        struct p4_signal_effect raise_effect;
        CHECK(mock_this_task->owner == P4_SIGNAL_OWNER_WAITING);
        state = task_state_load(mock_this_task);
        raise_effect = p4_signal_raise_locked(
            &state, 0, mock_action_signal, 1u);
        CHECK(raise_effect.status == P4_SIGNAL_STATUS_OK);
        CHECK(raise_effect.queue_transition ==
              P4_SIGNAL_QUEUE_WAITING_TO_READY);
        task_state_flush(mock_this_task, &state);
        mock_this_task->owner = P4_SIGNAL_OWNER_READY;
        mock_this_task->owner = P4_SIGNAL_OWNER_RUNNING;
    }
    else if (action == SWITCH_SPURIOUS)
    {
        CHECK(mock_this_task->owner == P4_SIGNAL_OWNER_WAITING);
        /* An unrelated scheduler event resumes this task with no signal. */
        mock_this_task->owner = P4_SIGNAL_OWNER_RUNNING;
    }
    else if (action == SWITCH_SIGNAL_BEFORE_COMMIT)
    {
        CHECK(mock_this_task->owner == P4_SIGNAL_OWNER_READY);
        mock_this_task->owner = P4_SIGNAL_OWNER_RUNNING;
    }
    else if (action == SWITCH_BLOCK_AFTER_COMMIT)
    {
        CHECK(mock_this_task->owner == P4_SIGNAL_OWNER_WAITING);
        if (!blocked_wait_escape_armed)
        {
            CHECK(0 && "unarmed synthetic blocked-wait escape");
            return;
        }
        longjmp(blocked_wait_escape, 1);
    }

    CHECK(!mock_task_lock_held);
    /* The resumed task regains its own IDNest independently of TDNest. */
    mock_id_nest = mock_id_nest_before_switch;
}

static void reset_case(uint32_t received, uint32_t waiting,
                       int initial_td_nest, int initial_id_nest)
{
    memset(&fixture_task, 0, sizeof(fixture_task));
    fixture_task.tc_SigRecvd = received;
    fixture_task.tc_SigWait = waiting;
    fixture_task.owner = P4_SIGNAL_OWNER_RUNNING;
    mock_this_task = &fixture_task;
    fixture_cpu_local.trap_depth = 0u;
    mock_wait_pending = NULL;
    mock_td_nest = initial_td_nest;
    mock_id_nest = initial_id_nest;
    mock_switch_active = 0;
    mock_id_nest_before_switch = 0;
    mock_task_lock_held = 0;
    mock_disable_calls = 0;
    mock_enable_calls = 0;
    mock_lock_calls = 0;
    mock_unlock_calls = 0;
    mock_wait_arm_calls = 0;
    mock_switch_calls = 0;
    mock_switch_lock_violations = 0;
    mock_td_restore_calls = 0;
    mock_saved_td_before_switch = 0;
    mock_fatal_lock_held = 0;
    mock_fatal_td_nest = 0;
    mock_fatal_id_nest = 0;
    mock_force_adapter_failure = 0;
    mock_force_nonarmed = 0;
    mock_current_retired = 0;
    mock_retired_probe_calls = 0u;
    mock_remove_self_calls = 0u;
    mock_action_signal = 0;
    mock_action_count = 0;
    mock_action_index = 0;
    memset(mock_actions, 0, sizeof(mock_actions));
    memset(mock_transitions, 0, sizeof(mock_transitions));
}

static void set_actions(const enum switch_action *actions,
                        unsigned int count, uint32_t signal)
{
    unsigned int i;
    CHECK(count <= (unsigned int)(sizeof(mock_actions) /
                                  sizeof(mock_actions[0])));
    mock_action_count = count;
    mock_action_index = 0;
    mock_action_signal = signal;
    for (i = 0; i < count; ++i)
        mock_actions[i] = actions[i];
}

static void check_normal_return(int initial_td_nest, int initial_id_nest)
{
    CHECK(!mock_task_lock_held);
    CHECK(mock_td_nest == initial_td_nest);
    CHECK(mock_id_nest == initial_id_nest);
    CHECK(mock_disable_calls == 1u);
    CHECK(mock_enable_calls == 1u);
    CHECK(mock_lock_calls == mock_unlock_calls);
    CHECK(mock_switch_lock_violations == 0u);
    CHECK(mock_td_restore_calls == mock_switch_calls);
    CHECK(mock_fatal_calls == 0u);
}

static void test_immediate_signals(void)
{
    ULONG result;

    reset_case(UINT32_C(0x16), UINT32_C(0x80), 3, -1);
    result = Wait(UINT32_C(0x06), &fixture_exec_base);
    CHECK(result == UINT32_C(0x06));
    CHECK(fixture_task.tc_SigRecvd == UINT32_C(0x10));
    CHECK(fixture_task.tc_SigWait == UINT32_C(0x80));
    CHECK(mock_wait_arm_calls == 1u);
    CHECK(mock_switch_calls == 0u);
    CHECK(mock_wait_pending == NULL);
    check_normal_return(3, -1);

    reset_case(UINT32_C(0x16), UINT32_C(0x80), -1, 2);
    result = Wait(UINT32_C(0x06), &fixture_exec_base);
    CHECK(result == UINT32_C(0x06));
    CHECK(fixture_task.tc_SigRecvd == UINT32_C(0x10));
    CHECK(fixture_task.tc_SigWait == UINT32_C(0x80));
    CHECK(mock_wait_arm_calls == 1u);
    CHECK(mock_switch_calls == 0u);
    CHECK(mock_wait_pending == NULL);
    check_normal_return(-1, 2);

    reset_case(UINT32_C(0x16), UINT32_C(0x80), 0, -1);
    result = Wait(UINT32_C(0x06), &fixture_exec_base);
    CHECK(result == UINT32_C(0x06));
    CHECK(mock_switch_calls == 0u);
    check_normal_return(0, -1);
}

static void test_spurious_resumes_then_signal_waiting(void)
{
    static const enum switch_action actions[] = {
        SWITCH_SPURIOUS,
        SWITCH_SPURIOUS,
        SWITCH_SIGNAL_WHILE_WAITING
    };
    ULONG result;
    reset_case(0u, 0u, -1, 2);
    set_actions(actions, 3u, UINT32_C(0x08));
    result = Wait(UINT32_C(0x0c), &fixture_exec_base);
    CHECK(result == UINT32_C(0x08));
    CHECK(mock_switch_calls == 3u);
    CHECK(mock_wait_arm_calls == 4u);
    CHECK(mock_action_index == 3u);
    CHECK(mock_transitions[0] == P4_SIGNAL_QUEUE_RUNNING_TO_WAIT);
    CHECK(mock_transitions[1] == P4_SIGNAL_QUEUE_RUNNING_TO_WAIT);
    CHECK(mock_transitions[2] == P4_SIGNAL_QUEUE_RUNNING_TO_WAIT);
    CHECK(fixture_task.tc_SigRecvd == 0u);
    CHECK(fixture_task.tc_SigWait == UINT32_C(0x0c));
    CHECK(mock_wait_pending == NULL);
    check_normal_return(-1, 2);

    reset_case(0u, 0u, 3, -1);
    set_actions(actions, 3u, UINT32_C(0x08));
    result = Wait(UINT32_C(0x0c), &fixture_exec_base);
    CHECK(result == UINT32_C(0x08));
    CHECK(mock_switch_calls == 3u);
    CHECK(mock_wait_arm_calls == 4u);
    CHECK(mock_action_index == 3u);
    CHECK(mock_wait_pending == NULL);
    check_normal_return(3, -1);
}

static void test_signal_race_before_switch_commit(void)
{
    static const enum switch_action actions[] = {
        SWITCH_SIGNAL_BEFORE_COMMIT
    };
    ULONG result;

    reset_case(0u, 0u, 3, -1);
    set_actions(actions, 1u, UINT32_C(0x20));
    result = Wait(UINT32_C(0x20), &fixture_exec_base);
    CHECK(result == UINT32_C(0x20));
    CHECK(mock_switch_calls == 1u);
    CHECK(mock_wait_arm_calls == 2u);
    CHECK(mock_transitions[0] == P4_SIGNAL_QUEUE_RUNNING_TO_READY);
    CHECK(mock_wait_pending == NULL);
    CHECK(fixture_task.tc_SigRecvd == 0u);
    check_normal_return(3, -1);

    reset_case(0u, 0u, -1, 2);
    set_actions(actions, 1u, UINT32_C(0x20));
    result = Wait(UINT32_C(0x20), &fixture_exec_base);
    CHECK(result == UINT32_C(0x20));
    CHECK(mock_switch_calls == 1u);
    CHECK(mock_wait_arm_calls == 2u);
    CHECK(mock_transitions[0] == P4_SIGNAL_QUEUE_RUNNING_TO_READY);
    CHECK(mock_wait_pending == NULL);
    CHECK(fixture_task.tc_SigRecvd == 0u);
    check_normal_return(-1, 2);
}

static void expect_fatal_wait(unsigned int expected_disables,
                              unsigned int expected_enables,
                              int expected_td_nest,
                              int expected_id_nest)
{
    unsigned int prior_fatal = mock_fatal_calls;
    int escaped;

    fatal_escape_armed = 1;
    escaped = setjmp(fatal_escape);
    if (escaped == 0)
    {
        (void)Wait(UINT32_C(0x01), &fixture_exec_base);
        fatal_escape_armed = 0;
        CHECK(0 && "Wait returned instead of failing closed");
        return;
    }
    fatal_escape_armed = 0;
    CHECK(mock_fatal_calls == prior_fatal + 1u);
    CHECK(mock_fatal_lock_held == 0u);
    CHECK(mock_disable_calls == expected_disables);
    CHECK(mock_enable_calls == expected_enables);
    CHECK(mock_td_nest == expected_td_nest);
    CHECK(mock_id_nest == expected_id_nest);
    CHECK(!mock_task_lock_held);
}

static void test_null_and_adapter_failure(void)
{
    reset_case(0u, 0u, 3, -1);
    mock_this_task = NULL;
    expect_fatal_wait(1u, 0u, 3, 0);

    reset_case(0u, 0u, 3, -1);
    mock_force_adapter_failure = 1;
    expect_fatal_wait(1u, 0u, 3, 0);
    CHECK(mock_wait_arm_calls == 1u);
    CHECK(mock_lock_calls == 1u && mock_unlock_calls == 1u);
    CHECK(mock_fatal_td_nest == 3);
    CHECK(mock_fatal_id_nest == 0);
    /* Fatal paths halt with Wait's Disable level retained. Restore only the
       host fixture's local counters before starting the next case. */
    mock_td_nest = 3;
    mock_id_nest = -1;
}

static void test_supervisor_rejected_before_wait_state(void)
{
    unsigned int depth;
    for (depth = 1u; depth <= 2u; ++depth)
    {
        int initial_id = depth == 1u ? -1 : 2;
        reset_case(UINT32_C(0x01), UINT32_C(0x80), 3, initial_id);
        fixture_cpu_local.trap_depth = depth;
        mock_wait_pending = &fixture_task;
        expect_fatal_wait(1u, 0u, 3, initial_id + 1);
        CHECK(mock_wait_arm_calls == 0u);
        CHECK(mock_lock_calls == 0u && mock_unlock_calls == 0u);
        CHECK(mock_switch_calls == 0u);
        CHECK(mock_wait_pending == &fixture_task);
        CHECK(fixture_task.tc_SigRecvd == UINT32_C(0x01));
        CHECK(fixture_task.tc_SigWait == UINT32_C(0x80));
        CHECK(fixture_task.owner == P4_SIGNAL_OWNER_RUNNING);
        CHECK(mock_fatal_id_nest == initial_id + 1);
    }
}

static void test_nonarmed_result_fails_closed(void)
{
    reset_case(0u, 0u, -1, 2);
    mock_force_nonarmed = 1;
    expect_fatal_wait(1u, 0u, -1, 3);
    CHECK(mock_wait_arm_calls == 1u);
    CHECK(mock_switch_calls == 0u);
    CHECK(mock_fatal_td_nest == -1);
    CHECK(mock_fatal_id_nest == 3);
    mock_td_nest = -1;
    mock_id_nest = 2;
}

static void test_zero_mask_arms_and_blocks(void)
{
    static const enum switch_action actions[] = {
        SWITCH_BLOCK_AFTER_COMMIT
    };
    int escaped;

    reset_case(UINT32_C(0x40), UINT32_C(0x80), 3, -1);
    set_actions(actions, 1u, 0u);
    blocked_wait_escape_armed = 1;
    escaped = setjmp(blocked_wait_escape);
    if (escaped == 0)
    {
        (void)Wait(0u, &fixture_exec_base);
        blocked_wait_escape_armed = 0;
        CHECK(0 && "Wait(0) returned without a matching ordinary signal");
        return;
    }
    blocked_wait_escape_armed = 0;
    CHECK(mock_wait_arm_calls == 1u);
    CHECK(mock_switch_calls == 1u);
    CHECK(mock_transitions[0] == P4_SIGNAL_QUEUE_RUNNING_TO_WAIT);
    CHECK(fixture_task.owner == P4_SIGNAL_OWNER_WAITING);
    CHECK(fixture_task.tc_SigRecvd == UINT32_C(0x40));
    CHECK(fixture_task.tc_SigWait == 0u);
    CHECK(mock_wait_pending == NULL);
    CHECK(mock_disable_calls == 1u && mock_enable_calls == 0u);
    CHECK(mock_id_nest == 0);
    CHECK(mock_td_nest == -1);
    CHECK(fixture_task.tc_TDNestCnt == 3);
    CHECK(!mock_task_lock_held && mock_switch_lock_violations == 0u);

    /* The mock abandons this deliberately blocked continuation so the host
       process can exit; a real return would restore saved_td and Enable(). */
    mock_td_nest = 3;
    mock_id_nest = -1;
}

static void test_retired_arm_transfers_without_return(void)
{
    reset_case(0u, 0u, 3, -1);
    mock_force_adapter_failure = 1;
    mock_current_retired = 1;
    removed_wait_escape_armed = 1;
    if (setjmp(removed_wait_escape) == 0)
    {
        (void)Wait(1u, &fixture_exec_base);
        CHECK(0 && "retired Wait returned to its caller");
    }
    removed_wait_escape_armed = 0;
    CHECK(mock_retired_probe_calls == 1u && mock_remove_self_calls == 1u);
    CHECK(mock_wait_arm_calls == 1u && mock_switch_calls == 0u);
    CHECK(mock_enable_calls == 0u && mock_id_nest == 0);
    CHECK(mock_td_nest == 3 && !mock_task_lock_held);
    mock_id_nest = -1;
}

int main(void)
{
    test_immediate_signals();
    test_spurious_resumes_then_signal_waiting();
    test_signal_race_before_switch_commit();
    test_null_and_adapter_failure();
    test_nonarmed_result_fails_closed();
    test_zero_mask_arms_and_blocks();
    test_supervisor_rejected_before_wait_state();
    test_retired_arm_transfers_without_return();

    if (failures != 0u)
    {
        fprintf(stderr, "%u of %u checks failed\n", failures, checks);
        return EXIT_FAILURE;
    }
    printf("PASS: %u checks; actual P4 Wait body and signal protocol "
           "with mocked adapter/switch\n", checks);
    return EXIT_SUCCESS;
}
