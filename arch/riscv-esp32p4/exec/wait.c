/*
    Copyright (C) 1995-2026, The AROS Development Team. All rights reserved.

    P4 SMP variant: the context-save hook owns the RUNNING -> WAIT/READY move.
*/

#if !defined(P4_WAIT_BODY_TEST)
#include <exec/execbase.h>
#include <aros/libcall.h>
#include <proto/exec.h>

#include "exec_intern.h"
#include "p4_signal_adapter.h"
#include "p4_self_remove.h"
#include "../kernel/p4_callback_escape.h"

#if !defined(__AROSPLATFORM_SMP__) || !defined(__AROSEXEC_SMP__)
#error "P4 SMP Wait requires the SMP Task layout"
#endif
#endif

AROS_LH1(ULONG, Wait,
    AROS_LHA(ULONG, signalSet, D0),
    struct ExecBase *, SysBase, 53, Exec)
{
    AROS_LIBFUNC_INIT

    struct Task *thisTask = GET_THIS_TASK;
    struct p4_signal_effect effect;
    BYTE saved_td;
    ULONG received = 0;

    /* Wait() intentionally breaks the caller's active Disable level. */
    Disable();

    /* Even an immediately available signal must not make supervisor Wait
     * appear supported. A nested scheduling ecall cannot save this callback
     * continuation. Reject before touching the Task lock or wait token;
     * use CPU-local state directly, without a kernel.resource lookup. */
    if (P4_EXEC_CPU_LOCAL(SysBase)->trap_depth != 0 || thisTask == NULL)
        p4_signal_adapter_fail_closed();

    for (;;)
    {
        int armed;

        p4_signal_adapter_lock_disabled(thisTask);
        armed = p4_signal_adapter_wait_arm_locked(
            thisTask, SysBase, signalSet, &effect);
        p4_signal_adapter_unlock_disabled(thisTask);

        if (!armed)
        {
            /* A remote registry-published removal can win between a caller's
             * readiness probe and this arm. RUN still owns current storage;
             * transfer only after dropping the Task lock, with MIE masked.
             * Invalid live wait state remains fatal, not apparent success. */
            if (krnP4SelfRemoveCurrentRetired(SysBase))
                Exec_P4RemoveSelf(thisTask, SysBase);
            p4_signal_adapter_fail_closed();
        }

        if (effect.wait_outcome == P4_SIGNAL_WAIT_RETURNED_SIGNALS)
        {
            received = effect.returned_signals;
            break;
        }

        if (effect.wait_outcome != P4_SIGNAL_WAIT_ARMED)
            p4_signal_adapter_fail_closed();

        /* The per-hart wait token remains armed while this context is saved.
         * The switch hook consumes it only after the outgoing registers have
         * been saved, then publishes exactly one WAIT or READY membership. */
        saved_td = TDNESTCOUNT_GET;
        thisTask->tc_TDNestCnt = saved_td;
        TDNESTCOUNT_SET(-1);
        KrnSwitch();
        TDNESTCOUNT_SET(saved_td);
    }

    Enable();
    return received;

    AROS_LIBFUNC_EXIT
}
