/*
    Copyright (C) 2015-2017, The AROS Development Team. All rights reserved.
*/

#define DEBUG 0

#include <aros/debug.h>
#include <exec/types.h>
#include <aros/libcall.h>
#include <proto/utility.h>
#include <resources/task.h>

#include <resources/task.h>

#include "task_intern.h"

/*****************************************************************************

    NAME */
#include <proto/task.h>

        AROS_LH2(void, UnLockTaskList,

/*  SYNOPSIS */
        AROS_LHA(struct TaskList *, tlist, A0),
        AROS_LHA(ULONG, flags, D0),

/*  LOCATION */
        struct TaskResBase *, TaskResBase, 2, Task)

/*  FUNCTION
        Frees a lock on the task lists given by LockTaskList().

    INPUTS
        flags - the same value as given to LockTaskList().

    RESULT

    NOTES

    EXAMPLE

    BUGS

    SEE ALSO
        LockTaskList(), NextTaskEntry()

    INTERNALS

*****************************************************************************/
{
    AROS_LIBFUNC_INIT

#ifdef TASKRES_ENABLE
    struct TaskListPrivate *taskList, *tltmp;
    struct TaskListPrivate *detached = NULL;
    struct Task *thisTask = FindTask(NULL);
#endif /* TASKRES_ENABLE */

    D(bug("[TaskRes] UnLockTaskList: flags = $%lx\n", flags));

#ifdef TASKRES_ENABLE
#if !defined(__AROSEXEC_SMP__)
    Forbid();
#else
    Disable();
    EXEC_SPINLOCK_LOCK(&TaskResBase->TaskListSpinLock, NULL, SPINLOCK_MODE_WRITE);
#endif
    ForeachNodeSafe(&TaskResBase->trb_LockedLists, taskList, tltmp)
    {
        if (((struct TaskList *)taskList == tlist) &&
            ((struct Task *)taskList->tlp_Node.ln_Name == thisTask) &&
            (taskList->tlp_Flags == flags))
        {
            D(bug("[TaskRes] UnLockTaskList: Releasing TaskList @ 0x%p\n", taskList));
            Remove(&taskList->tlp_Node);
            detached = taskList;
            break;
        }
    }
#if !defined(__AROSEXEC_SMP__)
    Permit();
#else
    EXEC_SPINLOCK_UNLOCK(&TaskResBase->TaskListSpinLock);
    Enable();
#endif
    /* The marker is removed while its semaphore ownership still exists.
     * Invalid handles must not release another iterator's semaphore count. */
    if (!detached)
        return;
    FreeMem(detached, sizeof(*detached));
    ReleaseSemaphore(&TaskResBase->trb_Sem);

    /* Purge expired entries from the list... */
    task_CleanList(NULL, TaskResBase);
#else
    Enable();
    FreeVec(tlist);
#endif /* TASKRES_ENABLE */

    AROS_LIBFUNC_EXIT
} /* UnLockTaskList */
