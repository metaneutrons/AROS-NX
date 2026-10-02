/*
    Copyright (C) 1995-2017, The AROS Development Team. All rights reserved.

    Desc: Make any children of this task orphans.
*/
#include "exec_intern.h"
#include "exec_util.h"
#include "etask.h"
#include <proto/exec.h>

/*****************************************************************************

    NAME */

        AROS_LH1(ULONG, ChildOrphan,

/*  SYNOPSIS */
        AROS_LHA(ULONG, tid, D0),

/*  LOCATION */
        struct ExecBase *, SysBase, 124, Exec)

/*  FUNCTION
        ChildOrphan() will detach the specified task from its parent
        task's child task tree. This is useful if the parent task will be
        exiting, and no longer needs to be told about child task events.

        Note that the default Task finaliser will orphan any remaining
        children when the task exits. This function can be used if you just
        do not wish to be told about a particular task.

    INPUTS
        tid     --  The ID of the task to orphan, or 0 for all tasks. Note
                    that it is NOT the pointer to the task.

    RESULT
        Will return 0 on success or CHILD_* on an error.

        The child/children will no longer participate in child task
        actions.

    NOTES

    EXAMPLE

    BUGS

    SEE ALSO

    INTERNALS

*****************************************************************************/
{
    AROS_LIBFUNC_INIT

#if defined(__AROSEXEC_SMP__)
    struct Task *ThisTask;
    struct ETask *et, *child;
    struct Node *nextchild;
    BOOL found = FALSE;

    /* The caller must be the current RUN/metadata owner. The ETask lock
     * protects this active-child list; it does not provide topology lifetime
     * discovery for an arbitrary or stale ETask pointer. */
    Forbid();
    ThisTask = GET_THIS_TASK;
    if (ThisTask == NULL)
    {
        Permit();
        return 0;
    }

    /* GetETask() checks TF_ETASK but dereferences its Task argument. */
    et = GetETask(ThisTask);
    if (et == NULL)
    {
        Permit();
        return CHILD_NOTNEW;
    }

    EXEC_SPINLOCK_LOCK(&IntETask(et)->iet_TaskLock, NULL,
                       SPINLOCK_MODE_WRITE);
    if (tid == 0)
    {
        /* Remove before clearing et_Parent. Leaving linked nodes behind while
         * replacing the sentinel would strand them on the old list. */
        ForeachNodeSafe(&et->et_Children, child, nextchild)
        {
            Remove((struct Node *)child);
            child->et_Parent = NULL;
        }
        found = TRUE;
    }
    else
    {
        ForeachNode(&et->et_Children, child)
        {
            if (child->et_UniqueID == tid)
            {
                Remove((struct Node *)child);
                child->et_Parent = NULL;
                found = TRUE;
                break;
            }
        }
    }
    EXEC_SPINLOCK_UNLOCK(&IntETask(et)->iet_TaskLock);
    Permit();

    return found ? 0 : CHILD_NOTFOUND;
#else

    struct Task *ThisTask = GET_THIS_TASK;
    struct ETask *et, *child;

        if (ThisTask)
        {
                et = GetETask(ThisTask);
                if (et == NULL)
                return CHILD_NOTNEW;

                if (tid == 0L)
                {
                        Forbid();
                ForeachNode(&et->et_Children, child)
                {
                        /*
                        Don't need to Remove(), because I'll blow away the entire
                        list at the end of the loop.
                        */
                        child->et_Parent = NULL;
                }
                NEWLIST(&et->et_Children);
                        Permit();
                }
                else
                {
                        Forbid();
                child = FindChild(tid);
                if (child != NULL)
                {
                        child->et_Parent = NULL;
                        Remove((struct Node *)child);
                }
                else
                {
                                Permit();
                        return CHILD_NOTFOUND;
                }
                Permit();
                }
        }
    return 0;
#endif

    AROS_LIBFUNC_EXIT
} /* ChildOrphan */
