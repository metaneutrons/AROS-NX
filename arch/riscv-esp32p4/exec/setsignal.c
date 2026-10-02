/*
    Copyright (C) 1995-2026, The AROS Development Team. All rights reserved.

    P4 SMP variant: signal-mask read/modify/write shares the Task state lock.
*/

#if !defined(P4_SET_SIGNAL_BODY_TEST)
#include <exec/execbase.h>
#include <aros/libcall.h>
#include <proto/exec.h>

#include "exec_intern.h"
#include "p4_signal_adapter.h"
#include "p4_self_remove.h"
#include "../kernel/p4_callback_escape.h"

#if !defined(__AROSPLATFORM_SMP__) || !defined(__AROSEXEC_SMP__)
#error "P4 SMP SetSignal requires the SMP Task layout"
#endif
#endif

AROS_LH2(ULONG, SetSignal,
    AROS_LHA(ULONG, newSignals, D0),
    AROS_LHA(ULONG, signalSet, D1),
    struct ExecBase *, SysBase, 51, Exec)
{
    AROS_LIBFUNC_INIT

    struct Task *thisTask = GET_THIS_TASK;
    struct p4_signal_effect effect;

    if (thisTask == NULL)
        p4_signal_adapter_fail_closed();

    /* Keep the owning RUN context and all Task access in one local mask
     * interval; no Task pointer is retained after the outer Enable. */
    Disable();
    p4_signal_adapter_lock_disabled(thisTask);
    effect = p4_signal_adapter_set_locked(
        thisTask, SysBase, signalSet, newSignals);
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

    Enable();
    return effect.returned_signals;

    AROS_LIBFUNC_EXIT
}
