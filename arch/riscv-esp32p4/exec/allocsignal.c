/*
    Copyright (C) 1995-2026, The AROS Development Team. All rights reserved.

    P4 SMP variant. Allocation and all per-task signal masks share tc_SpinLock.
*/

#if !defined(P4_SIGNAL_ALLOCATION_TEST)
#include <exec/execbase.h>
#include <exec/tasks.h>
#include <aros/libcall.h>
#include <proto/exec.h>

#include "exec_intern.h"
#include "exec_util.h"
#include "p4_signal_adapter.h"

#if !defined(__AROSPLATFORM_SMP__) || !defined(__AROSEXEC_SMP__)
#error "P4 SMP AllocSignal requires the SMP Task layout"
#endif
#endif

AROS_LH1(BYTE, AllocSignal,
    AROS_LHA(LONG, signalNum, D0),
    struct ExecBase *, SysBase, 55, Exec)
{
    AROS_LIBFUNC_INIT

    /* Preserve the public AOS/68k ABI: callers may initialize only D0.b. */
    return (BYTE)AllocTaskSignal(GET_THIS_TASK, (BYTE)signalNum, SysBase);

    AROS_LIBFUNC_EXIT
}

LONG AllocTaskSignal(struct Task *thisTask, LONG signalNum,
                     struct ExecBase *SysBase)
{
    ULONG mask;
    ULONG mask1;
    LONG result = -1;

    if (thisTask == NULL)
        return -1;

    (void)SysBase;
    /* Public caller owns its current RUN context. The sole internal caller,
     * NewCreateTaskA, owns unpublished storage and initializes tc_SpinLock
     * before this helper. Do not try to pin an unregistered constructor Task.
     * Both callers must retain the storage through the final unlock. */
    Disable();
    p4_signal_adapter_lock_disabled(thisTask);

    mask = thisTask->tc_SigAlloc;
    if (signalNum < 0)
    {
        /* Select the least-significant free signal bit. */
        mask1 = ~mask & -~mask;
        if (mask1 == 0)
        {
            goto out;
        }

        result = (mask1 & UINT32_C(0xffff0000) ? 16 : 0) +
                 (mask1 & UINT32_C(0xff00ff00) ? 8 : 0) +
                 (mask1 & UINT32_C(0xf0f0f0f0) ? 4 : 0) +
                 (mask1 & UINT32_C(0xcccccccc) ? 2 : 0) +
                 (mask1 & UINT32_C(0xaaaaaaaa) ? 1 : 0);
    }
    else
    {
        if ((ULONG)signalNum >= 32U)
        {
            goto out;
        }

        mask1 = UINT32_C(1) << (unsigned int)signalNum;
        if ((thisTask->tc_SigAlloc & mask1) != 0)
        {
            goto out;
        }
        result = signalNum;
    }

    thisTask->tc_SigAlloc |= mask1;
    thisTask->tc_SigExcept &= ~mask1;
    thisTask->tc_SigWait &= ~mask1;
    thisTask->tc_SigRecvd &= ~mask1;

out:
    p4_signal_adapter_unlock_disabled(thisTask);
    Enable();
    /* Only the copied scalar survives the scheduling boundary. */
    return result;
}
