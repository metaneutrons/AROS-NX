/*
    Copyright (C) 1995-2026, The AROS Development Team. All rights reserved.

    P4 SMP locked signal writer, bound to registered-task lifetime pins.
    Full configured SMP still requires the remaining removal/startup gates.
*/

#if !defined(P4_SIGNAL_WAKE_TEST)
#define __AROS_KERNEL__
#include <exec/execbase.h>
#include <aros/libcall.h>
#include <proto/exec.h>

#include "exec_intern.h"
#include "p4_signal_adapter.h"

#if !defined(__AROSPLATFORM_SMP__) || !defined(__AROSEXEC_SMP__)
#error "P4 SMP Signal requires the SMP Task layout"
#endif

#if !defined(P4_SIGNAL_TASK_PIN_BOUND)
#error "P4 Signal requires the registered-task lifetime pin binding"
#endif

/* The pin operation resolves a registered live Task and retains its storage
 * before this wrapper takes tc_SpinLock. Unpin is the final Task-related
 * operation; the post-unlock poke consumes only the immutable effect. */
extern int krnP4SignalTaskPin(struct Task *task, struct ExecBase *sys_base);
extern void krnP4SignalTaskUnpin(struct Task *task, struct ExecBase *sys_base);
#endif

AROS_LH2(void, Signal,
    AROS_LHA(struct Task *, task, A1),
    AROS_LHA(ULONG, signalSet, D0),
    struct ExecBase *, SysBase, 54, Exec)
{
    AROS_LIBFUNC_INIT

    struct p4_signal_effect effect;

    effect.status = P4_SIGNAL_STATUS_NOT_LIVE;
    effect.queue_transition = P4_SIGNAL_QUEUE_NONE;
    effect.wait_outcome = P4_SIGNAL_WAIT_NO_CHANGE;
    effect.returned_signals = 0;
    effect.reschedule_hart = 0;
    effect.request_reschedule = 0;
    effect.remote_reschedule = 0;

    /* Signal() is callable from interrupt context. The pin hook is bounded,
     * nonallocating and must be safe there; failure means the Task has retired. */
    Disable();
    if (task == NULL || !krnP4SignalTaskPin(task, SysBase))
    {
        Enable();
        return;
    }

    p4_signal_adapter_lock_disabled(task);
    effect = p4_signal_adapter_raise_locked(task, SysBase, signalSet);
    if (effect.status == P4_SIGNAL_STATUS_OK &&
        effect.queue_transition != P4_SIGNAL_QUEUE_NONE)
    {
        (void)p4_signal_adapter_apply_queue_locked(task, SysBase, &effect);
    }
    p4_signal_adapter_unlock_disabled(task);

    krnP4SignalTaskUnpin(task, SysBase);
    if (effect.status != P4_SIGNAL_STATUS_OK &&
        effect.status != P4_SIGNAL_STATUS_NOT_LIVE)
        p4_signal_adapter_fail_closed();
    p4_signal_adapter_poke_after_unlock(&effect, SysBase);
    Enable();

    AROS_LIBFUNC_EXIT
}
