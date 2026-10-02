/*
    Copyright (C) 1995-2023, The AROS Development Team. All rights reserved.

    Desc: Exec utility functions.
*/

#define DEBUG 0
#include <aros/debug.h>

#include <exec/lists.h>
#include <exec/tasks.h>
#include <exec/memory.h>
#include <exec/execbase.h>
#include <dos/dosextens.h>

#include <proto/exec.h>

#include "etask.h"
#include "exec_intern.h"
#include "exec_util.h"

/****************************************************************************

    NAME */
#include "exec_util.h"

        struct ETask * Exec_FindChild(

/*  SYNOPSIS */
        ULONG       id,
        struct ExecBase *SysBase)

/*  FUNCTION
        Scan through the current tasks children list searching for the task
        whose et_UniqueID field matches.

    INPUTS
        id      -   The task ID to match.

    RESULT
        Address of the ETask structure that matches, or
        NULL otherwise.

    NOTES
        This is an internal exec.library function not exported from the
        library.

    EXAMPLE

    BUGS

    SEE ALSO

    INTERNALS

******************************************************************************/
{
    struct Task *ThisTask = GET_THIS_TASK;
    struct ETask *et, *retVal = NULL;
    struct ETask *thisET;

    thisET = GetETask(ThisTask);
    if (thisET != NULL)
    {
#if defined(__AROSEXEC_SMP__)
        EXEC_SPINLOCK_LOCK(&IntETask(thisET)->iet_TaskLock, NULL, SPINLOCK_MODE_READ);
#endif
        ForeachNode (&thisET->et_Children, et)
        {
            if (et->et_UniqueID == id)
            {
                retVal = et;
                break;
            }
        }
#if defined(__AROSEXEC_SMP__)
        EXEC_SPINLOCK_UNLOCK(&IntETask(thisET)->iet_TaskLock);
#endif
        if (!retVal)
        {
#if defined(__AROSEXEC_SMP__)
            /* PutMsg may acquire this port from a local interrupt. */
            Disable();
            EXEC_SPINLOCK_LOCK(&thisET->et_TaskMsgPort.mp_SpinLock, NULL, SPINLOCK_MODE_READ);
#endif
            ForeachNode(&thisET->et_TaskMsgPort.mp_MsgList, et)
            {
                if (et->et_UniqueID == id)
                {
                    retVal = et;
                    break;
                }
            }
#if defined(__AROSEXEC_SMP__)
            EXEC_SPINLOCK_UNLOCK(&thisET->et_TaskMsgPort.mp_SpinLock);
            Enable();
#endif
        }
    }
    return retVal;
}

BOOL
Exec_InitETask(struct Task *task, struct Task *parent, struct ExecBase *SysBase)
{
    /*
     *  We don't add this to the task memory, it isn't free'd by
     *  RemTask(), rather by somebody else calling ChildFree().
     *  Alternatively, an orphaned task will free its own ETask.
     */
#if defined(__AROSEXEC_SMP__)
    int cpunum;
#endif
    struct ETask *et =
        AllocMem(sizeof(struct IntETask), MEMF_PUBLIC | MEMF_CLEAR);

    D(
        bug("[EXEC:ETask] Init: Allocated ETask for Task '%s' @ %p\n",
        task->tc_Node.ln_Name, task);
        bug("[EXEC:ETask] Init:            ETask @ 0x%p, %d bytes\n",
        et, sizeof(struct IntETask));
    )

    task->tc_UnionETask.tc_ETask = et;
    if (!et)
        return FALSE;
#if defined(EXEC_PLATFORM_ETASK_RESERVE)
    /* Reserve before publication, including fallible affinity/context setup.
     * A retained dead child's metadata keeps its PID until final expunge. */
    if (!EXEC_PLATFORM_ETASK_RESERVE(et))
    {
        task->tc_UnionETask.tc_ETask = NULL;
        FreeMem(et, sizeof(struct IntETask));
        return FALSE;
    }
#endif
    task->tc_Flags |= TF_ETASK;

#if defined(__AROSEXEC_SMP__)
    cpunum = KrnGetCPUNumber();
    EXEC_SPINLOCK_INIT(&IntETask(et)->iet_TaskLock);
    if (PrivExecBase(SysBase)->IntFlags & EXECF_CPUAffinity)
    {
        IntETask(et)->iet_CpuAffinity = KrnAllocCPUMask();
        if (IntETask(et)->iet_CpuAffinity == NULL)
        {
            /* NULL is TASKAFFINITY_BOOT, not a successful allocation. This
             * ETask is still unpublished: undo construction rather than
             * silently move a hart-1 task onto the boot CPU. */
            task->tc_UnionETask.tc_ETask = NULL;
            task->tc_Flags &= ~TF_ETASK;
#if defined(EXEC_PLATFORM_ETASK_RELEASE)
            if (!EXEC_PLATFORM_ETASK_RELEASE(et))
                return FALSE;
#endif
            FreeMem(et, sizeof(struct IntETask));
            return FALSE;
        }
        KrnGetCPUMask(cpunum, IntETask(et)->iet_CpuAffinity);

        D(bug("[EXEC:ETask] Init: CPU #%d, mask %08x\n", cpunum, IntETask(et)->iet_CpuAffinity);)
    }
#endif

    et->et_Parent = parent;
    NewMinList(&et->et_Children);

    D(bug("[EXEC:ETask] Init: Parent @ 0x%p\n", et->et_Parent);)

    /* Initialise the message list */
    InitMsgPort(&et->et_TaskMsgPort);
    et->et_TaskMsgPort.mp_SigTask = task;
    et->et_TaskMsgPort.mp_SigBit = SIGB_CHILD;

    /* Initialise the trap fields */
    et->et_TrapAlloc = SysBase->TaskTrapAlloc;
    et->et_TrapAble = 0;

#ifdef DEBUG_ETASK
    {
        int len = strlen(task->tc_Node.ln_Name) + 1;
        IntETask(et)->iet_Me = AllocVec(len, MEMF_CLEAR|MEMF_PUBLIC);
        if (IntETask(et)->iet_Me != NULL)
            CopyMem(task->tc_Node.ln_Name, IntETask(et)->iet_Me, len);
    }
#endif

    D(bug("[EXEC:ETask] Init: Generating Unique ID...\n");)

    /* Get a unique identifier for this task */
    Forbid();
#if !defined(EXEC_PLATFORM_ETASK_RESERVE)
    while(et->et_UniqueID == 0)
    {
        //TODO: Replace with UUID!

        /*
         *      Add some fuzz on wrapping. It's likely that the early numbers
         *      where taken by somebody else.
         */
        if(++SysBase->ex_TaskID == 0)
            SysBase->ex_TaskID = 1024;

        Disable();
        D(bug("[EXEC:ETask] Init: Checking for existing ID...\n");)
        if (FindTaskByPID(SysBase->ex_TaskID) == NULL)
            et->et_UniqueID = SysBase->ex_TaskID;
        D(bug("[EXEC:ETask] Init: done\n");)
        Enable();
    }
#endif
   
    D(bug("[EXEC:ETask] Init:     Task ID : %08x\n", et->et_UniqueID);)

    /* Finally if the parent task is an ETask, add myself as its child */
    if(et->et_Parent && ((struct Task*) et->et_Parent)->tc_Flags & TF_ETASK)
    {
        struct ETask * parentEtask = GetETask(et->et_Parent);

#if defined(__AROSEXEC_SMP__)
        EXEC_SPINLOCK_LOCK(&IntETask(parentEtask)->iet_TaskLock, NULL, SPINLOCK_MODE_WRITE);
#endif
        AddHead((struct List *)&parentEtask->et_Children, (struct Node *)et);
#if defined(__AROSEXEC_SMP__)
        EXEC_SPINLOCK_UNLOCK(&IntETask(parentEtask)->iet_TaskLock);
#endif
    }
    Permit();

    D(bug("[EXEC:ETask] Init: Initialized\n");)

    return TRUE;
}

#if defined(EXEC_PLATFORM_ETASK_STAGED)
void Exec_PublishConstructedETask(struct Task *task, struct Task *parent,
                                struct ExecBase *SysBase)
{
    struct ETask *et = task ? GetETask(task) : NULL;
    struct ETask *parent_et;

    /* Only NewAddTask's current RUN owner may attach a fresh child. The
     * child is creator-pinned and has not reached Ready or a hook. */
    if (et == NULL || parent != GET_THIS_TASK || et->et_Parent != NULL ||
        ((struct Node *)et)->ln_Succ != NULL ||
        ((struct Node *)et)->ln_Pred != NULL)
    {
        EXEC_PLATFORM_TASK_FATAL();
        return;
    }
    Disable();
    parent_et = parent ? GetETask(parent) : NULL;
    if (parent_et)
        EXEC_SPINLOCK_LOCK(&IntETask(parent_et)->iet_TaskLock, NULL,
                           SPINLOCK_MODE_WRITE);
    et->et_Parent = parent;
    if (parent_et)
        AddHead((struct List *)&parent_et->et_Children, (struct Node *)et);
    if (parent_et)
        EXEC_SPINLOCK_UNLOCK(&IntETask(parent_et)->iet_TaskLock);
    /* No Task/ETask access after Enable. Generic remote RemTask still breaks
     * RUN protection; drained cleanup and the metadata gate remain required
     * before the SMP runtime may be enabled. */
    Enable();
}
#endif

void
Exec_CleanupETask(struct Task *task, struct ExecBase *SysBase)
{
    struct ETask *et, *child, *nextchild, *parent;
    struct Node *tmpNode;
    BOOL expunge = TRUE;
#if defined(__AROSEXEC_SMP__)
    struct List detached_results;
#endif

    if(!(task->tc_Flags & TF_ETASK) ||
        ((et = task->tc_UnionETask.tc_ETask) == NULL))
        return;

    D(bug("[EXEC:ETask] Cleanup: Task @ 0x%p, ETask @ 0x%p\n", task, et);)

    Forbid();

#if defined(EXEC_PLATFORM_ETASK_CLEANUP_BEGIN)
    /* Logical ownership transition only. The caller must already own drained
     * Task storage; generic remote RemTask does not yet establish that. */
    EXEC_PLATFORM_ETASK_CLEANUP_BEGIN(et);
#endif
#if defined(__AROSEXEC_SMP__)
    if ((PrivExecBase(SysBase)->IntFlags & EXECF_CPUAffinity) && (IntETask(et)->iet_CpuAffinity))
    {
        if ((IPTR)IntETask(et)->iet_CpuAffinity != TASKAFFINITY_ANY)
            KrnFreeCPUMask(IntETask(et)->iet_CpuAffinity);
    }
#endif

#if defined(__AROSEXEC_SMP__)
    NEWLIST(&detached_results);
    /* Forbid excludes task switches, not interrupt-time PutMsg. */
    Disable();
    EXEC_SPINLOCK_LOCK(&et->et_TaskMsgPort.mp_SpinLock, NULL, SPINLOCK_MODE_WRITE);
#endif
    /* Clean up after all the children that the task didn't do itself. */
    ForeachNodeSafe(&et->et_TaskMsgPort.mp_MsgList, child, tmpNode)
    {
#if defined(__AROSEXEC_SMP__)
        /* Never free an intrusive node while the port still links it. */
        Remove((struct Node *)child);
        AddTail(&detached_results, (struct Node *)child);
#else
        ExpungeETask(child);
#endif
    }
#if defined(__AROSEXEC_SMP__)
    EXEC_SPINLOCK_UNLOCK(&et->et_TaskMsgPort.mp_SpinLock);
    Enable();
    /* Transfer every result before freeing any metadata. Final expunge may
     * acquire the PID/metadata gate and call the allocator; neither belongs
     * in the message-port critical section. The local list is worker-owned. */
    while ((child = (struct ETask *)RemHead(&detached_results)) != NULL)
        ExpungeETask(child);
#endif
    /* If we have an ETask parent, tell it we have exited. */
    if(et->et_Parent != NULL)
    {
        parent = GetETask(et->et_Parent);

#if defined(__AROSEXEC_SMP__)
        EXEC_SPINLOCK_LOCK(&IntETask(et)->iet_TaskLock, NULL, SPINLOCK_MODE_WRITE);
#endif
        /* Link children to our parent. */
        ForeachNodeSafe(&et->et_Children, child, nextchild)
        {
#if defined(__AROSEXEC_SMP__)
            /* Remove from the old sentinel before publishing in the new list.
             * ForeachNodeSafe already captured the next old-list node. */
            Remove((struct Node *)child);
#endif
            child->et_Parent = et->et_Parent;
            //Forbid();
            if (parent)
            {
#if defined(__AROSEXEC_SMP__)
                EXEC_SPINLOCK_LOCK(&IntETask(parent)->iet_TaskLock, NULL, SPINLOCK_MODE_WRITE);
#endif
                ADDTAIL(&parent->et_Children, child);
#if defined(__AROSEXEC_SMP__)
                EXEC_SPINLOCK_UNLOCK(&IntETask(parent)->iet_TaskLock);
#endif
            }
            //Permit();
        }
#if defined(__AROSEXEC_SMP__)
        EXEC_SPINLOCK_UNLOCK(&IntETask(et)->iet_TaskLock);
#endif

        /* Notify parent only if child was created with NP_NotifyOnDeath set
           to TRUE */
        if(parent != NULL)
        {
#if defined(__AROSEXEC_SMP__)
            EXEC_SPINLOCK_LOCK(&IntETask(parent)->iet_TaskLock, NULL, SPINLOCK_MODE_WRITE);
#endif
            REMOVE(et);
#if defined(__AROSEXEC_SMP__)
            EXEC_SPINLOCK_UNLOCK(&IntETask(parent)->iet_TaskLock);
#endif
            if(
               (((struct Task *)task)->tc_Node.ln_Type == NT_PROCESS) &&
               (((struct Process*) task)->pr_Flags & PRF_NOTIFYONDEATH)
            )
            {
#if defined(EXEC_PLATFORM_ETASK_RESULT_RETAIN)
                /* Transfer ownership before PutMsg makes the result visible
                 * and can wake a parent that immediately calls ChildFree. */
                EXEC_PLATFORM_ETASK_RESULT_RETAIN(et);
#endif
                PutMsg(&parent->et_TaskMsgPort, (struct Message *)et);
                expunge = FALSE;
            }
        }
    }
    else
    {
#if defined(__AROSEXEC_SMP__)
        EXEC_SPINLOCK_LOCK(&IntETask(et)->iet_TaskLock, NULL, SPINLOCK_MODE_WRITE);
#endif
        /* Orphan all our remaining children. */
#if defined(__AROSEXEC_SMP__)
        ForeachNodeSafe(&et->et_Children, child, nextchild)
        {
            Remove((struct Node *)child);
            child->et_Parent = NULL;
        }
#else
        ForeachNode(&et->et_Children, child)
            child->et_Parent = NULL;
#endif
#if defined(__AROSEXEC_SMP__)
        EXEC_SPINLOCK_UNLOCK(&IntETask(et)->iet_TaskLock);
#endif
    }

    if(expunge)
        ExpungeETask(et);

    Permit();
}

void
Exec_ExpungeETask(struct ETask *et, struct ExecBase *SysBase)
{
#if defined(EXEC_PLATFORM_ETASK_RELEASE)
    /* Existing readers retain all metadata storage, including Result2. */
    if (EXEC_PLATFORM_ETASK_RELEASE(et))
        Exec_FreeETaskStorage(et, SysBase);
}

void
Exec_FreeETaskStorage(struct ETask *et, struct ExecBase *SysBase)
{
#endif
    FreeVec(et->et_Result2);

#ifdef DEBUG_ETASK
    FreeVec(IntETask(et)->iet_Me);
#endif
    D(bug("[EXEC:ETask] Expunge: Freeing ETask @ 0x%p\n", et);)
    FreeMem(et, sizeof(struct IntETask));
}

BOOL Exec_CheckTask(struct Task *task, struct ExecBase *SysBase)
{
    struct Task *t;

    if (!task)
        return FALSE;

    Forbid();
#if defined(__AROSEXEC_SMP__)
    EXEC_SPINLOCK_LOCK(&PrivExecBase(SysBase)->TaskRunningSpinLock, NULL, SPINLOCK_MODE_READ);
    ForeachNode(&PrivExecBase(SysBase)->TaskRunning, t)
    {
        if (task == t)
        {
            EXEC_SPINLOCK_UNLOCK(&PrivExecBase(SysBase)->TaskRunningSpinLock);
            Permit();
            return TRUE;
        }
    }
    EXEC_SPINLOCK_UNLOCK(&PrivExecBase(SysBase)->TaskRunningSpinLock);
    Permit();
    Forbid();
    EXEC_SPINLOCK_LOCK(&PrivExecBase(SysBase)->TaskSpinningLock, NULL, SPINLOCK_MODE_READ);
    ForeachNode(&PrivExecBase(SysBase)->TaskSpinning, t)
    {
        if (task == t)
        {
            EXEC_SPINLOCK_UNLOCK(&PrivExecBase(SysBase)->TaskSpinningLock);
            Permit();
            return TRUE;
        }
    }
    EXEC_SPINLOCK_UNLOCK(&PrivExecBase(SysBase)->TaskSpinningLock);
    Permit();
    Forbid();
    EXEC_SPINLOCK_LOCK(&PrivExecBase(SysBase)->TaskReadySpinLock, NULL, SPINLOCK_MODE_READ);
#else
    if (task == GET_THIS_TASK)
    {
        Permit();
        return TRUE;
    }
#endif

    ForeachNode(&SysBase->TaskReady, t)
    {
        if (task == t)
        {
#if defined(__AROSEXEC_SMP__)
            EXEC_SPINLOCK_UNLOCK(&PrivExecBase(SysBase)->TaskReadySpinLock);
#endif
            Permit();
            return TRUE;
        }
    }
#if defined(__AROSEXEC_SMP__)
    EXEC_SPINLOCK_UNLOCK(&PrivExecBase(SysBase)->TaskReadySpinLock);
    Permit();
    Forbid();
    EXEC_SPINLOCK_LOCK(&PrivExecBase(SysBase)->TaskWaitSpinLock, NULL, SPINLOCK_MODE_READ);
#endif
    ForeachNode(&SysBase->TaskWait, t)
    {
        if (task == t)
        {
#if defined(__AROSEXEC_SMP__)
            EXEC_SPINLOCK_UNLOCK(&PrivExecBase(SysBase)->TaskWaitSpinLock);
#endif
            Permit();
            return TRUE;
        }
    }
#if defined(__AROSEXEC_SMP__)
    EXEC_SPINLOCK_UNLOCK(&PrivExecBase(SysBase)->TaskWaitSpinLock);
#endif
    Permit();

    return FALSE;
}
