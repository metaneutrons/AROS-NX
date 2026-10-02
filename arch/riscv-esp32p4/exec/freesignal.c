/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
 * P4 SMP: allocation-mask writes share the canonical Task lock.
 */
#if !defined(P4_SIGNAL_ALLOCATION_TEST)
#include <exec/execbase.h>
#include <exec/tasks.h>
#include <aros/libcall.h>
#include <proto/exec.h>
#include "exec_intern.h"
#include "p4_signal_adapter.h"

#if !defined(__AROSPLATFORM_SMP__) || !defined(__AROSEXEC_SMP__)
#error "P4 SMP FreeSignal requires the SMP Task layout"
#endif
#endif

AROS_LH1(void, FreeSignal,
    AROS_LHA(LONG, signalNum, D0),
    struct ExecBase *, SysBase, 56, Exec)
{
    AROS_LIBFUNC_INIT

    struct Task *thisTask;

    /* -1 is the documented no-op. Reject other invalid bit numbers without
     * an undefined shift; unlike AllocSignal, FreeSignal has no BYTE cast. */
    if (signalNum < 0 || (ULONG)signalNum >= 32U)
        return;

    thisTask = GET_THIS_TASK;
    if (thisTask == NULL)
        return;

    Disable();
    p4_signal_adapter_lock_disabled(thisTask);
    thisTask->tc_SigAlloc &= ~(UINT32_C(1) << (unsigned int)signalNum);
    p4_signal_adapter_unlock_disabled(thisTask);
    /* Nothing from the Task may be accessed after this scheduling boundary.
     * FreeSignal deliberately leaves received/wait/exception masks intact. */
    Enable();

    AROS_LIBFUNC_EXIT
}
