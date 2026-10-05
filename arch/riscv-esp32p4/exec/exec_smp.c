/*
    Copyright (c) 2013-2026, The AROS Development Team. All rights reserved.

    Desc: scheduler list helpers of the smp variant of the esp32p4-riscv
          target, from arch/aarch64-native.
*/

#include <exec/execbase.h>
#include <exec/tasks.h>

#include <proto/exec.h>

#include "exec_intern.h"
#include "etask.h"

/*
 * Move a task to the list matching newState, for krnSysCallReschedTask().
 * The caller must not hold tc_SpinLock: it is taken here, so that reading
 * the state, picking the list, moving the node and writing the state are
 * one step for any observer. Lock order: tc_SpinLock, then a list lock.
 * Interrupts are masked directly, not with Disable(), as on the arm ports.
 */
void Exec_ReschedTask(struct Task *task, ULONG newState)
{
    spinlock_t *fromLock = NULL;
    unsigned int __if = EXEC_IRQFIQ_DISABLE();

    EXEC_SPINLOCK_LOCK(&task->tc_SpinLock, NULL, SPINLOCK_MODE_WRITE);

    if (newState == TS_READY)
    {
        /* Only a waiting or freshly added task moves: another hart may
           have woken it already. Its signal bits are set either way. */
        switch (task->tc_State)
        {
            case TS_WAIT:
            case TS_INVALID:
            case TS_ADDED:
                break;
            default:
                EXEC_SPINLOCK_UNLOCK(&task->tc_SpinLock);
                EXEC_IRQFIQ_RESTORE(__if);
                return;
        }
    }

    switch (task->tc_State)
    {
        case TS_RUN:
            fromLock = &PrivExecBase(SysBase)->TaskRunningSpinLock;
            break;
        case TS_READY:
            fromLock = &PrivExecBase(SysBase)->TaskReadySpinLock;
            break;
        case TS_WAIT:
            fromLock = &PrivExecBase(SysBase)->TaskWaitSpinLock;
            break;
        default:
            /* Not on a scheduler list */
            break;
    }

    if (fromLock)
    {
        EXEC_SPINLOCK_LOCK(fromLock, NULL, SPINLOCK_MODE_WRITE);
        Remove(&task->tc_Node);
        EXEC_SPINLOCK_UNLOCK(fromLock);
    }

    task->tc_State = newState;

    switch (newState)
    {
        case TS_READY:
            exec_TaskEnqueueReady(task);
            break;
        case TS_WAIT:
            exec_TaskEnqueueWait(task);
            break;
        default:
            /* TS_REMOVED, TS_TOMBSTONED: on no list */
            break;
    }

    EXEC_SPINLOCK_UNLOCK(&task->tc_SpinLock);
    EXEC_IRQFIQ_RESTORE(__if);
}

/* RemTask()'s self-removal: detach and tombstone for the service task,
   then return; unlike KrnSwitch() this does not dispatch. */
void Exec_SuicideSwitch(void)
{
    struct Task *task = GET_THIS_TASK;
    unsigned int __if = EXEC_IRQFIQ_DISABLE();

    EXEC_SPINLOCK_LOCK(&task->tc_SpinLock, NULL, SPINLOCK_MODE_WRITE);
    EXEC_SPINLOCK_LOCK(&PrivExecBase(SysBase)->TaskRunningSpinLock, NULL,
                       SPINLOCK_MODE_WRITE);
    Remove(&task->tc_Node);
    EXEC_SPINLOCK_UNLOCK(&PrivExecBase(SysBase)->TaskRunningSpinLock);
    task->tc_State = TS_TOMBSTONED;
    EXEC_SPINLOCK_UNLOCK(&task->tc_SpinLock);
    EXEC_IRQFIQ_RESTORE(__if);
}
