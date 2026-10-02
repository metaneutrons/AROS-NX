/*
    Copyright (C) 1995-2026, The AROS Development Team. All rights reserved.

    P4 SMP variant: exception snapshots are consumed under tc_SpinLock;
    callbacks run with no task or queue lock held.
*/

#if !defined(P4_EXCEPTION_BODY_TEST)
#include <exec/execbase.h>
#include <aros/asmcall.h>
#include <aros/libcall.h>
#include <proto/exec.h>
#include <proto/kernel.h>

#include "exec_intern.h"
#include "p4_signal_adapter.h"

#if !defined(__AROSPLATFORM_SMP__) || !defined(__AROSEXEC_SMP__)
#error "P4 SMP Exception requires the SMP Task layout"
#endif
#endif

AROS_LH0(void, Exception,
    struct ExecBase *, SysBase, 11, Exec)
{
    AROS_LIBFUNC_INIT

    struct Task *task = GET_THIS_TASK;
    BYTE nest_count;

    if (task == NULL)
        p4_signal_adapter_fail_closed();

    nest_count = IDNESTCOUNT_GET;
    IDNESTCOUNT_SET(0);
    KrnCli();

    for (;;)
    {
        struct p4_signal_task_state state;
        APTR exception_code;
        APTR exception_data;
        ULONG flags;

        p4_signal_adapter_lock_disabled(task);
        if (task->tc_State == TS_REMOVED || task->tc_State == TS_INVALID ||
            task->tc_State == TS_TOMBSTONED)
        {
            p4_signal_adapter_unlock_disabled(task);
            break;
        }
        if (!p4_signal_adapter_load_locked(task, SysBase, &state) ||
            state.owner != P4_SIGNAL_OWNER_RUNNING)
        {
            p4_signal_adapter_unlock_disabled(task);
            p4_signal_adapter_fail_closed();
        }

        flags = p4_signal_exception_take_locked(&state);
        p4_signal_adapter_flush_locked(task, &state);
        exception_code = task->tc_ExceptCode;
        exception_data = task->tc_ExceptData;
        p4_signal_adapter_unlock_disabled(task);

        if (flags == 0)
            break;

        /* Exception() is entered from the disabled dispatcher path. Match
         * Exec's contract by enabling only around user callback execution. */
        Enable();
        /* Enable does not set raw MIE in supervisor context. The dispatcher
         * holds RUN, and callbacks deliberately run with interrupts enabled. */
        KrnSti();
        if (exception_code != NULL)
        {
            ULONG callback_mask = AROS_UFC3(ULONG, exception_code,
                AROS_UFCA(APTR, exception_data, A1),
                AROS_UFCA(ULONG, flags, D0),
                AROS_UFCA(struct ExecBase *, SysBase, A6));

            KrnCli();
            Disable();
            p4_signal_adapter_lock_disabled(task);
            if (task->tc_State == TS_REMOVED || task->tc_State == TS_INVALID ||
                task->tc_State == TS_TOMBSTONED)
            {
                p4_signal_adapter_unlock_disabled(task);
                break;
            }
            if (!p4_signal_adapter_load_locked(task, SysBase, &state) ||
                state.owner != P4_SIGNAL_OWNER_RUNNING)
            {
                p4_signal_adapter_unlock_disabled(task);
                p4_signal_adapter_fail_closed();
            }
            (void)p4_signal_exception_merge_locked(&state, callback_mask);
            p4_signal_adapter_flush_locked(task, &state);
            p4_signal_adapter_unlock_disabled(task);
        }
        else
        {
            KrnCli();
            Disable();
        }
    }

    IDNESTCOUNT_SET(nest_count);

    AROS_LIBFUNC_EXIT
}
