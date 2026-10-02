/*
 * Copyright (c) 2026, The AROS Development Team. All rights reserved.
 *
 * P4/AROS binding for the portable signal/wait state machine. Queue and
 * scheduler operations remain explicit external integration hooks.
 */
#define __AROS_KERNEL__
#include "exec_intern.h"

#include <aros/types/spinlock_s.h>
#include <resources/execlock.h>

#include "etask.h"
#include "../kernel/cpu_local_runtime.h"
#include "../kernel/p4_scheduler_queue.h"
#include "../kernel/p4_public_spinlock.h"
#include "p4_signal_adapter.h"

#if !defined(__AROSPLATFORM_SMP__) || !defined(__AROSEXEC_SMP__)
#error "The P4 signal adapter requires the SMP Task layout"
#endif

#if !defined(P4_E3_RUNTIME_READY)
#error "The P4 signal adapter requires the reviewed P4 runtime layout"
#endif

static struct p4_signal_effect p4_signal_adapter_error(
    enum p4_signal_status status)
{
    struct p4_signal_effect effect;

    effect.status = status;
    effect.queue_transition = P4_SIGNAL_QUEUE_NONE;
    effect.wait_outcome = P4_SIGNAL_WAIT_NO_CHANGE;
    effect.returned_signals = 0;
    effect.reschedule_hart = 0;
    effect.request_reschedule = 0;
    effect.remote_reschedule = 0;
    return effect;
}

void p4_signal_adapter_lock_disabled(struct Task *task)
{
    EXEC_SPINLOCK_LOCK(&task->tc_SpinLock, NULL, SPINLOCK_MODE_WRITE);
}

void p4_signal_adapter_unlock_disabled(struct Task *task)
{
    EXEC_SPINLOCK_UNLOCK(&task->tc_SpinLock);
}

void p4_signal_adapter_lock(struct Task *task)
{
    Disable();
    p4_signal_adapter_lock_disabled(task);
}

void p4_signal_adapter_unlock(struct Task *task)
{
    p4_signal_adapter_unlock_disabled(task);
    Enable();
}

void p4_signal_adapter_fail_closed(void)
{
    krnP4PublicSpinFatal(P4_PUBLIC_SPIN_BAD_STATE);
}

static enum p4_signal_owner_state p4_signal_adapter_owner(ULONG state)
{
    switch (state)
    {
        case TS_RUN:
            return P4_SIGNAL_OWNER_RUNNING;
        case TS_READY:
            return P4_SIGNAL_OWNER_READY;
        case TS_WAIT:
            return P4_SIGNAL_OWNER_WAITING;
        case TS_REMOVED:
        case TS_TOMBSTONED:
            return P4_SIGNAL_OWNER_REMOVED;
        case TS_INVALID:
        default:
            return P4_SIGNAL_OWNER_INVALID;
    }
}

int p4_signal_adapter_load_locked(
    struct Task *task,
    struct ExecBase *sys_base,
    struct p4_signal_task_state *state)
{
    struct p4_exec_runtime *runtime;

    if (task == NULL || sys_base == NULL || state == NULL)
        return 0;

    runtime = p4_exec_runtime_current(sys_base);
    if (runtime == NULL || runtime->hart_id >= P4_EXEC_RUNTIME_HART_COUNT)
        return 0;

    state->sig_received = task->tc_SigRecvd;
    state->sig_wait = task->tc_SigWait;
    state->sig_except = task->tc_SigExcept;
    state->flags = task->tc_Flags;
    state->exception_flag_mask = (uint8_t)TF_EXCEPT;
    state->owner = p4_signal_adapter_owner(task->tc_State);
    state->owner_hart = runtime->hart_id;

    /* A running remote task's hart comes from the state protected by this
     * same canonical Task lock. Non-running owners do not use this field. */
    if (state->owner == P4_SIGNAL_OWNER_RUNNING)
    {
        struct ETask *etask = task->tc_UnionETask.tc_ETask;
        IPTR owner_hart;

        if ((task->tc_Flags & TF_ETASK) == 0 || etask == NULL)
            return 0;

        owner_hart = IntETask(etask)->iet_CpuNumber;
        if (owner_hart >= P4_EXEC_RUNTIME_HART_COUNT)
            return 0;
        state->owner_hart = (uint32_t)owner_hart;
    }

    return 1;
}

void p4_signal_adapter_flush_locked(
    struct Task *task,
    const struct p4_signal_task_state *state)
{
    task->tc_SigRecvd = state->sig_received;
    task->tc_SigWait = state->sig_wait;
    task->tc_SigExcept = state->sig_except;
    task->tc_Flags = state->flags;
}

int p4_signal_adapter_wait_arm_locked(
    struct Task *task,
    struct ExecBase *sys_base,
    uint32_t wait_mask,
    struct p4_signal_effect *effect)
{
    struct p4_exec_runtime *runtime;
    struct p4_signal_task_state state;
    void *pending;

    if (effect == NULL)
        return 0;

    runtime = p4_exec_runtime_current(sys_base);
    if (runtime == NULL || runtime->cpu_local == NULL ||
        runtime->cpu_local->current_task != task)
    {
        *effect = p4_signal_adapter_error(P4_SIGNAL_STATUS_INVALID_ARGUMENT);
        return 0;
    }

    if (!p4_signal_adapter_load_locked(task, sys_base, &state))
    {
        *effect = p4_signal_adapter_error(P4_SIGNAL_STATUS_INVALID_STATE);
        return 0;
    }

    pending = (void *)runtime->wait_pending;
    *effect = p4_signal_wait_arm_locked(&state, &pending, task, wait_mask);
    if (effect->status == P4_SIGNAL_STATUS_OK)
    {
        p4_signal_adapter_flush_locked(task, &state);
        runtime->wait_pending = (struct Task *)pending;
    }

    return effect->status == P4_SIGNAL_STATUS_OK;
}

struct p4_signal_effect p4_signal_adapter_switch_commit_locked(
    struct Task *task,
    struct ExecBase *sys_base)
{
    struct p4_exec_runtime *runtime;
    struct p4_signal_task_state state;
    struct p4_signal_effect effect;
    void *pending;

    runtime = p4_exec_runtime_current(sys_base);
    if (runtime == NULL)
        return p4_signal_adapter_error(P4_SIGNAL_STATUS_INVALID_ARGUMENT);
    if (!p4_signal_adapter_load_locked(task, sys_base, &state))
        return p4_signal_adapter_error(P4_SIGNAL_STATUS_INVALID_STATE);

    pending = (void *)runtime->wait_pending;
    effect = p4_signal_switch_commit_locked(&state, &pending, task);
    if (effect.status == P4_SIGNAL_STATUS_OK)
    {
        p4_signal_adapter_flush_locked(task, &state);
        runtime->wait_pending = (struct Task *)pending;
    }
    return effect;
}

struct p4_signal_effect p4_signal_adapter_raise_locked(
    struct Task *task,
    struct ExecBase *sys_base,
    uint32_t signal_set)
{
    struct p4_exec_runtime *runtime;
    struct p4_exec_runtime *owner_runtime;
    struct p4_signal_task_state state;
    struct p4_signal_effect effect;
    int wait_armed = 0;

    runtime = p4_exec_runtime_current(sys_base);
    if (runtime == NULL)
        return p4_signal_adapter_error(P4_SIGNAL_STATUS_INVALID_ARGUMENT);
    if (!p4_signal_adapter_load_locked(task, sys_base, &state))
        return p4_signal_adapter_error(P4_SIGNAL_STATUS_INVALID_STATE);

    if (state.owner == P4_SIGNAL_OWNER_RUNNING &&
        state.owner_hart < P4_EXEC_RUNTIME_HART_COUNT)
    {
        owner_runtime = &__p4_exec_runtimes[state.owner_hart];
        if (owner_runtime->wait_pending != NULL &&
            owner_runtime->wait_pending != task)
            return p4_signal_adapter_error(P4_SIGNAL_STATUS_PENDING_OWNER_MISMATCH);
        wait_armed = owner_runtime->wait_pending == task;
    }

    effect = p4_signal_raise_locked(
        &state, wait_armed, signal_set, runtime->hart_id);
    if (effect.status == P4_SIGNAL_STATUS_OK &&
        state.owner == P4_SIGNAL_OWNER_READY)
    {
        int destination = krnP4SignalEligibleHartLocked(task);
        if (destination < 1)
            return p4_signal_adapter_error(P4_SIGNAL_STATUS_INVALID_STATE);
        /* Admission/preemption priority is decided by the destination
         * scheduler. A Ready task must not wait for an unrelated timer IRQ.
         * Poking unconditionally is conservative, not a priority inversion. */
        effect.request_reschedule = 1;
        effect.reschedule_hart = (uint32_t)destination - 1U;
        effect.remote_reschedule =
            (uint8_t)(effect.reschedule_hart != runtime->hart_id);
    }
    if (effect.status == P4_SIGNAL_STATUS_OK)
        p4_signal_adapter_flush_locked(task, &state);
    return effect;
}

struct p4_signal_effect p4_signal_adapter_set_locked(
    struct Task *task,
    struct ExecBase *sys_base,
    uint32_t signal_mask,
    uint32_t new_signals)
{
    struct p4_signal_task_state state;
    struct p4_signal_effect effect;

    if (!p4_signal_adapter_load_locked(task, sys_base, &state))
        return p4_signal_adapter_error(P4_SIGNAL_STATUS_INVALID_STATE);

    effect = p4_signal_set_locked(&state, signal_mask, new_signals);
    if (effect.status == P4_SIGNAL_STATUS_OK)
        p4_signal_adapter_flush_locked(task, &state);
    return effect;
}

struct p4_signal_effect p4_signal_adapter_set_except_locked(
    struct Task *task,
    struct ExecBase *sys_base,
    uint32_t signal_mask,
    uint32_t new_exceptions)
{
    struct p4_exec_runtime *runtime;
    struct p4_signal_task_state state;
    struct p4_signal_effect effect;

    runtime = p4_exec_runtime_current(sys_base);
    if (runtime == NULL)
        return p4_signal_adapter_error(P4_SIGNAL_STATUS_INVALID_ARGUMENT);
    if (!p4_signal_adapter_load_locked(task, sys_base, &state))
        return p4_signal_adapter_error(P4_SIGNAL_STATUS_INVALID_STATE);

    effect = p4_signal_set_except_locked(
        &state, signal_mask, new_exceptions, runtime->hart_id);
    if (effect.status == P4_SIGNAL_STATUS_OK)
        p4_signal_adapter_flush_locked(task, &state);
    return effect;
}

int p4_signal_adapter_apply_queue_locked(
    struct Task *task,
    struct ExecBase *sys_base,
    struct p4_signal_effect *effect)
{
    struct p4_exec_runtime *runtime;
    int result;

    if (effect == NULL)
        return 0;
    if (effect->queue_transition == P4_SIGNAL_QUEUE_NONE)
        return 1;

    /* Validate every local prerequisite before a helper is allowed to move
     * list membership. The queue helper validates selected-hart affinity. */
    runtime = p4_exec_runtime_current(sys_base);
    if (runtime == NULL || runtime->hart_id >= P4_EXEC_RUNTIME_HART_COUNT)
    {
        effect->status = P4_SIGNAL_STATUS_INVALID_ARGUMENT;
        return 0;
    }

    result = krnP4SignalQueueApplyLocked(
        task, effect->queue_transition, sys_base);
    if (result < 0)
    {
        effect->status = P4_SIGNAL_STATUS_INVALID_STATE;
        return 0;
    }
    if (result == 0)
        return 1;

    if ((uint32_t)result > P4_EXEC_RUNTIME_HART_COUNT)
    {
        effect->status = P4_SIGNAL_STATUS_INVALID_STATE;
        return 0;
    }

    effect->request_reschedule = 1;
    effect->reschedule_hart = (uint32_t)result - 1U;
    effect->remote_reschedule =
        (uint8_t)(effect->reschedule_hart != runtime->hart_id);
    return 1;
}

void p4_signal_adapter_poke_after_unlock(
    const struct p4_signal_effect *effect,
    struct ExecBase *sys_base)
{
    if (effect != NULL && effect->status == P4_SIGNAL_STATUS_OK &&
        effect->request_reschedule)
        krnP4SignalPoke(effect, sys_base);
}
