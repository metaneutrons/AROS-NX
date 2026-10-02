/*
 * Copyright (c) 2026, The AROS Development Team. All rights reserved.
 *
 * P4/AROS binding for the portable signal/wait protocol. This is deliberately
 * separate from generic Exec until every P4 task-state writer is integrated.
 */
#ifndef P4_SIGNAL_ADAPTER_H
#define P4_SIGNAL_ADAPTER_H

#include <exec/tasks.h>
#include <exec/execbase.h>

#include "p4_signal_protocol.h"

#if !defined(__AROSPLATFORM_SMP__) || !defined(__AROSEXEC_SMP__)
#error "The P4 signal adapter requires the SMP Task layout"
#endif

#if !defined(P4_E3_RUNTIME_READY)
#error "The P4 signal adapter requires the reviewed P4 runtime layout"
#endif

/* These two helpers own queue/list locking; caller already holds tc_SpinLock. */
extern int krnP4SignalQueueApplyLocked(
    struct Task *task,
    enum p4_signal_queue_transition transition,
    struct ExecBase *sys_base);

/* This helper performs only an asynchronous scheduler/mailbox poke. */
extern void krnP4SignalPoke(
    const struct p4_signal_effect *effect,
    struct ExecBase *sys_base);

/* Pair _disabled helpers with an already active local Disable(). */
void p4_signal_adapter_lock_disabled(struct Task *task);
void p4_signal_adapter_unlock_disabled(struct Task *task);

/* Pair these helpers when the caller does not already hold local Disable(). */
void p4_signal_adapter_lock(struct Task *task);
void p4_signal_adapter_unlock(struct Task *task);
void p4_signal_adapter_fail_closed(void) __attribute__((noreturn));

/* The following functions require task->tc_SpinLock to be held. */
int p4_signal_adapter_load_locked(
    struct Task *task,
    struct ExecBase *sys_base,
    struct p4_signal_task_state *state);
void p4_signal_adapter_flush_locked(
    struct Task *task,
    const struct p4_signal_task_state *state);
int p4_signal_adapter_wait_arm_locked(
    struct Task *task,
    struct ExecBase *sys_base,
    uint32_t wait_mask,
    struct p4_signal_effect *effect);
struct p4_signal_effect p4_signal_adapter_switch_commit_locked(
    struct Task *task,
    struct ExecBase *sys_base);
struct p4_signal_effect p4_signal_adapter_raise_locked(
    struct Task *task,
    struct ExecBase *sys_base,
    uint32_t signal_set);
struct p4_signal_effect p4_signal_adapter_set_locked(
    struct Task *task,
    struct ExecBase *sys_base,
    uint32_t signal_mask,
    uint32_t new_signals);
struct p4_signal_effect p4_signal_adapter_set_except_locked(
    struct Task *task,
    struct ExecBase *sys_base,
    uint32_t signal_mask,
    uint32_t new_exceptions);

/* Apply queue_transition before dropping tc_SpinLock; poke only after unlock. */
int p4_signal_adapter_apply_queue_locked(
    struct Task *task,
    struct ExecBase *sys_base,
    struct p4_signal_effect *effect);
void p4_signal_adapter_poke_after_unlock(
    const struct p4_signal_effect *effect,
    struct ExecBase *sys_base);

#endif /* P4_SIGNAL_ADAPTER_H */
