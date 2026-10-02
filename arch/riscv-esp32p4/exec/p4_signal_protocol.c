/*
 * Copyright (c) 2026, The AROS Development Team. All rights reserved.
 *
 * Portable state-machine core for the ESP32-P4 Exec signal/wait protocol.
 * Callers provide serialization and apply the returned scheduler effects.
 */

#include "p4_signal_protocol.h"

static struct p4_signal_effect p4_signal_effect_init(void)
{
    struct p4_signal_effect effect;

    effect.status = P4_SIGNAL_STATUS_OK;
    effect.queue_transition = P4_SIGNAL_QUEUE_NONE;
    effect.wait_outcome = P4_SIGNAL_WAIT_NO_CHANGE;
    effect.returned_signals = 0;
    effect.reschedule_hart = 0;
    effect.request_reschedule = 0;
    effect.remote_reschedule = 0;
    return effect;
}

static struct p4_signal_effect p4_signal_error(
    enum p4_signal_status status)
{
    struct p4_signal_effect effect = p4_signal_effect_init();

    effect.status = status;
    return effect;
}

static int p4_signal_state_valid(
    const struct p4_signal_task_state *state)
{
    return state != 0 &&
        state->owner >= P4_SIGNAL_OWNER_INVALID &&
        state->owner <= P4_SIGNAL_OWNER_REMOVED;
}

static int p4_signal_task_live(
    const struct p4_signal_task_state *state)
{
    return state->owner == P4_SIGNAL_OWNER_RUNNING ||
        state->owner == P4_SIGNAL_OWNER_READY ||
        state->owner == P4_SIGNAL_OWNER_WAITING;
}

static int p4_signal_sleep_wakeup_pending(
    const struct p4_signal_task_state *state)
{
    return (state->sig_received & state->sig_wait) != 0 ||
        (state->exception_flag_mask != 0 &&
         (state->flags & state->exception_flag_mask) != 0);
}

static void p4_signal_mark_exception(
    struct p4_signal_task_state *state)
{
    if (state->exception_flag_mask != 0 &&
        (state->sig_received & state->sig_except) != 0)
    {
        state->flags |= state->exception_flag_mask;
    }
}

static void p4_signal_request_reschedule(
    struct p4_signal_effect *effect,
    const struct p4_signal_task_state *state,
    uint32_t caller_hart)
{
    effect->request_reschedule = 1;
    effect->reschedule_hart = state->owner_hart;
    effect->remote_reschedule =
        (uint8_t)(caller_hart != state->owner_hart);
}

struct p4_signal_effect p4_signal_wait_arm_locked(
    struct p4_signal_task_state *state,
    void **wait_pending_slot,
    void *task_key,
    uint32_t wait_mask)
{
    struct p4_signal_effect effect = p4_signal_effect_init();
    uint32_t received;

    if (!p4_signal_state_valid(state) || wait_pending_slot == 0 ||
        task_key == 0)
        return p4_signal_error(P4_SIGNAL_STATUS_INVALID_ARGUMENT);

    if (state->owner != P4_SIGNAL_OWNER_RUNNING)
        return p4_signal_error(P4_SIGNAL_STATUS_INVALID_STATE);

    if (*wait_pending_slot != 0)
        return p4_signal_error(P4_SIGNAL_STATUS_PENDING_BUSY);

    received = state->sig_received & wait_mask;
    if (received != 0)
    {
        /* Wait returns the matching bits and clears the requested mask. */
        state->sig_received &= ~wait_mask;
        effect.returned_signals = received;
        effect.wait_outcome = P4_SIGNAL_WAIT_RETURNED_SIGNALS;
        return effect;
    }

    state->sig_wait = wait_mask;
    *wait_pending_slot = task_key;
    effect.wait_outcome = P4_SIGNAL_WAIT_ARMED;
    return effect;
}

struct p4_signal_effect p4_signal_switch_commit_locked(
    struct p4_signal_task_state *state,
    void **wait_pending_slot,
    void *task_key)
{
    struct p4_signal_effect effect = p4_signal_effect_init();
    int armed;

    if (!p4_signal_state_valid(state) || wait_pending_slot == 0 ||
        task_key == 0)
        return p4_signal_error(P4_SIGNAL_STATUS_INVALID_ARGUMENT);

    armed = (*wait_pending_slot == task_key);
    if (*wait_pending_slot != 0 && !armed)
        return p4_signal_error(P4_SIGNAL_STATUS_PENDING_OWNER_MISMATCH);

    if (state->owner != P4_SIGNAL_OWNER_RUNNING)
        return p4_signal_error(P4_SIGNAL_STATUS_INVALID_STATE);

    if (armed)
    {
        /* Consume only this task's token, exactly once. */
        *wait_pending_slot = 0;
        if (p4_signal_sleep_wakeup_pending(state))
        {
            effect.queue_transition =
                P4_SIGNAL_QUEUE_RUNNING_TO_READY;
        }
        else
        {
            effect.queue_transition =
                P4_SIGNAL_QUEUE_RUNNING_TO_WAIT;
        }
    }
    else
    {
        /* Ordinary preemption still moves a running task to Ready. */
        effect.queue_transition = P4_SIGNAL_QUEUE_RUNNING_TO_READY;
    }

    return effect;
}

struct p4_signal_effect p4_signal_raise_locked(
    struct p4_signal_task_state *state,
    int wait_armed,
    uint32_t signal_set,
    uint32_t caller_hart)
{
    struct p4_signal_effect effect = p4_signal_effect_init();

    if (!p4_signal_state_valid(state))
        return p4_signal_error(P4_SIGNAL_STATUS_INVALID_ARGUMENT);
    if (!p4_signal_task_live(state))
        return p4_signal_error(P4_SIGNAL_STATUS_NOT_LIVE);

    state->sig_received |= signal_set;
    p4_signal_mark_exception(state);

    if (state->owner == P4_SIGNAL_OWNER_WAITING)
    {
        if (p4_signal_sleep_wakeup_pending(state))
            effect.queue_transition = P4_SIGNAL_QUEUE_WAITING_TO_READY;
    }
    else if (state->owner == P4_SIGNAL_OWNER_RUNNING)
    {
        /*
         * Keep a running task running. In particular, an armed Wait is
         * resolved by the post-context-save commit, not by Signal. A stale
         * tc_SigWait mask after a prior wake is not evidence of a current
         * Wait; only its owner's per-hart pending token authorizes this
         * ordinary-signal reschedule. Exception delivery is independent of
         * that token because the exception flag itself is pending work.
         */
        if ((wait_armed &&
             (state->sig_received & state->sig_wait) != 0) ||
            (state->exception_flag_mask != 0 &&
             (state->flags & state->exception_flag_mask) != 0))
            p4_signal_request_reschedule(&effect, state, caller_hart);
    }

    return effect;
}

struct p4_signal_effect p4_signal_set_locked(
    struct p4_signal_task_state *state,
    uint32_t signal_mask,
    uint32_t new_signals)
{
    struct p4_signal_effect effect = p4_signal_effect_init();
    uint32_t old;

    if (!p4_signal_state_valid(state))
        return p4_signal_error(P4_SIGNAL_STATUS_INVALID_ARGUMENT);
    if (!p4_signal_task_live(state))
        return p4_signal_error(P4_SIGNAL_STATUS_NOT_LIVE);

    old = state->sig_received;
    state->sig_received = (old & ~signal_mask) |
        (new_signals & signal_mask);
    effect.returned_signals = old;
    return effect;
}

struct p4_signal_effect p4_signal_set_except_locked(
    struct p4_signal_task_state *state,
    uint32_t signal_mask,
    uint32_t new_exceptions,
    uint32_t caller_hart)
{
    struct p4_signal_effect effect = p4_signal_effect_init();
    uint32_t old;
    uint32_t active_exceptions;

    if (!p4_signal_state_valid(state))
        return p4_signal_error(P4_SIGNAL_STATUS_INVALID_ARGUMENT);
    if (!p4_signal_task_live(state))
        return p4_signal_error(P4_SIGNAL_STATUS_NOT_LIVE);

    old = state->sig_except;
    state->sig_except = (old & ~signal_mask) |
        (new_exceptions & signal_mask);
    active_exceptions = state->sig_except & state->sig_received;
    p4_signal_mark_exception(state);

    if (state->owner == P4_SIGNAL_OWNER_WAITING &&
        p4_signal_sleep_wakeup_pending(state))
    {
        effect.queue_transition = P4_SIGNAL_QUEUE_WAITING_TO_READY;
    }
    else if (state->owner == P4_SIGNAL_OWNER_RUNNING &&
        active_exceptions != 0)
    {
        p4_signal_request_reschedule(&effect, state, caller_hart);
    }

    effect.returned_signals = old;
    return effect;
}

uint32_t p4_signal_exception_take_locked(
    struct p4_signal_task_state *state)
{
    uint32_t active;

    if (!p4_signal_state_valid(state) || !p4_signal_task_live(state))
        return 0;

    if (state->exception_flag_mask != 0)
        state->flags &= (uint8_t)~state->exception_flag_mask;

    active = state->sig_except & state->sig_received;
    state->sig_except &= ~active;
    state->sig_received &= ~active;
    return active;
}

uint32_t p4_signal_exception_merge_locked(
    struct p4_signal_task_state *state,
    uint32_t callback_mask)
{
    if (!p4_signal_state_valid(state) || !p4_signal_task_live(state))
        return 0;

    state->sig_except |= callback_mask;
    p4_signal_mark_exception(state);
    return state->sig_except & state->sig_received;
}
