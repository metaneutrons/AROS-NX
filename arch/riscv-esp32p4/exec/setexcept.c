/*
    Copyright (C) 1995-2026, The AROS Development Team. All rights reserved.

    P4 SMP variant: exception mask, received signals and TF_EXCEPT are joined
    by the canonical Task state lock. Scheduler pokes are out of lock.
*/

#if !defined(P4_SIGNAL_WAKE_TEST)
#include <exec/execbase.h>
#include <aros/libcall.h>
#include <proto/exec.h>

#include "exec_intern.h"
#include "p4_signal_adapter.h"
#include "p4_self_remove.h"
#include "../kernel/p4_callback_escape.h"

#if !defined(__AROSPLATFORM_SMP__) || !defined(__AROSEXEC_SMP__)
#error "P4 SMP SetExcept requires the SMP Task layout"
#endif
#endif

AROS_LH2(ULONG, SetExcept,
    AROS_LHA(ULONG, newSignals, D0),
    AROS_LHA(ULONG, signalSet, D1),
    struct ExecBase *, SysBase, 52, Exec)
{
    AROS_LIBFUNC_INIT

    struct Task *thisTask = GET_THIS_TASK;
    struct p4_signal_effect effect;

    if (thisTask == NULL)
        p4_signal_adapter_fail_closed();

    Disable();
    p4_signal_adapter_lock_disabled(thisTask);
    effect = p4_signal_adapter_set_except_locked(
        thisTask, SysBase, signalSet, newSignals);
    if (effect.status == P4_SIGNAL_STATUS_OK &&
        effect.queue_transition != P4_SIGNAL_QUEUE_NONE)
    {
        p4_signal_adapter_apply_queue_locked(thisTask, SysBase, &effect);
    }
    p4_signal_adapter_unlock_disabled(thisTask);

    if (effect.status != P4_SIGNAL_STATUS_OK)
    {
        /* A remote remover may publish service retirement while this RUN
         * continuation owns the Task lock. The lock is released, but MIE is
         * still masked, so transfer rather than return into retired Task use. */
        if (krnP4SelfRemoveCurrentRetired(SysBase))
            Exec_P4RemoveSelf(thisTask, SysBase);
        p4_signal_adapter_fail_closed();
    }

    p4_signal_adapter_poke_after_unlock(&effect, SysBase);
    Enable();
    return effect.returned_signals;

    AROS_LIBFUNC_EXIT
}
