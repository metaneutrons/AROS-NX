/*
    Copyright (C) 1995-2007, The AROS Development Team. All rights reserved.

    Free child task information on a dead child.
*/
#include "exec_intern.h"
#include "exec_util.h"
#include "etask.h"
#include <proto/exec.h>

/*****************************************************************************

    NAME */

        AROS_LH1(void, ChildFree,

/*  SYNOPSIS */
        AROS_LHA(ULONG, tid, D0),

/*  LOCATION */
        struct ExecBase *, SysBase, 123, Exec)

/*  FUNCTION
        Clean up after a child process.

    INPUTS
        tid     --  Id of the child to clean up. This is not the same as
                    the Task pointer.

    RESULT
        The child will be freed.

    NOTES
        This function will work correctly only for child tasks that are
        processes created with NP_NotifyOnDeath set to TRUE.

        Calling ChildFree() on a running child is likely to crash your
        system badly.

    EXAMPLE

    BUGS

    SEE ALSO

    INTERNALS

*****************************************************************************/
{
    AROS_LIBFUNC_INIT

    struct ETask *et;

#if defined(__AROSEXEC_SMP__)
    struct Task *ThisTask;
    struct ETask *thisET;
    struct ETask *candidate;

    Forbid();
    ThisTask = GET_THIS_TASK;
    if (ThisTask == NULL)
    {
        Permit();
        return;
    }

    /* GetETask() checks TF_ETASK, but its argument must not be NULL. */
    thisET = GetETask(ThisTask);
    if (thisET == NULL)
    {
        Permit();
        return;
    }

    /* In SMP mode only the durable dead-result list owns a child eligible
     * for ChildFree.  An active child is deliberately not found or freed. */
    et = NULL;
    /* Forbid does not exclude interrupt-time PutMsg on this port. */
    Disable();
    EXEC_SPINLOCK_LOCK(&thisET->et_TaskMsgPort.mp_SpinLock, NULL,
                       SPINLOCK_MODE_WRITE);
    ForeachNode(&thisET->et_TaskMsgPort.mp_MsgList, candidate)
    {
        if (candidate->et_UniqueID == tid)
        {
            Remove((struct Node *)candidate);
            et = candidate;
            break;
        }
    }
    EXEC_SPINLOCK_UNLOCK(&thisET->et_TaskMsgPort.mp_SpinLock);
    Enable();
    Permit();

    /* Removal transfers exclusive result-list ownership.  Expunging can
     * free et, so do not dereference it after this call. */
    if (et != NULL)
        ExpungeETask(et);
#else
    Forbid();
    et = FindChild(tid);
    if(et != NULL)
    {
        Remove((struct Node *)et);

        ExpungeETask(et);
    }
    Permit();
#endif
    
    AROS_LIBFUNC_EXIT
} /* ChildFree */
