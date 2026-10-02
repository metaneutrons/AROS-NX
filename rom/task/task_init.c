/*
    Copyright (C) 2015-2023, The AROS Development Team. All rights reserved.
*/

#include <aros/debug.h>
#include <aros/symbolsets.h>

#include <proto/exec.h>
#include <proto/execlock.h>
#include <resources/execlock.h>
#include <resources/task.h>

#include "etask.h"
#if defined(__AROSEXEC_SMP__)
#include "kernel_debug.h"
#endif
#include "task_intern.h"

extern APTR AROS_SLIB_ENTRY(NewAddTask, Task, 176)();
extern void AROS_SLIB_ENTRY(RemTask, Task, 48)();

static LONG taskres_Init(struct TaskResBase *TaskResBase)
{
#ifdef TASKRES_ENABLE
    struct TaskStorageFreeSlot *freeTaskStorageSlot;
#if !defined(EXEC_PLATFORM_TASKRESOURCE_SERVICE_CLEANUP)
    struct TaskListEntry *taskEntry = NULL;
    struct Task *curTask = NULL;
#endif

#if defined(__AROSEXEC_SMP__)
    void *ExecLockBase = NULL;
#endif
    NEWLIST(&TaskResBase->trb_TaskStorageSlots);

#endif /* TASKRES_ENABLE */

    KernelBase = OpenResource((CONST_STRPTR)"kernel.resource");
    if (!KernelBase)
    {
        /* we should probably halt the system ..*/
        return FALSE;
    }

#if defined(__AROSEXEC_SMP__)
    TaskResBase->trb_ExecLock = OpenResource((CONST_STRPTR)"execlock.resource");
    ExecLockBase = TaskResBase->trb_ExecLock;
    if (!ExecLockBase)
    {
        krnPanic(KernelBase,"Failed to open Exec's private locking resource!");
    }
#endif

    TaskResBase->trb_UtilityBase = TaggedOpenLibrary(TAGGEDOPEN_UTILITY);
    if (!TaskResBase->trb_UtilityBase)
        return FALSE;

    NEWLIST(&TaskResBase->trb_TaskList);
    NEWLIST(&TaskResBase->trb_NewTasks);
    NEWLIST(&TaskResBase->trb_LockedLists);
#if defined(__AROSEXEC_SMP__)
    EXEC_SPINLOCK_INIT(&TaskResBase->TaskListSpinLock);
#endif

    InitSemaphore(&TaskResBase->trb_Sem);

#ifdef TASKRES_ENABLE
    /* Initialize free task storage slots management */
    freeTaskStorageSlot = AllocMem(sizeof(struct TaskStorageFreeSlot), MEMF_PUBLIC|MEMF_CLEAR);
    if (!freeTaskStorageSlot)
    {
         D(bug("[TaskRes] %S: FATAL: Failed to allocate inital free task storage slot!", __func__);)
        return FALSE;
    }
    freeTaskStorageSlot->FreeSlot = __TS_FIRSTSLOT + 1;
    AddHead((struct List *)&TaskResBase->trb_TaskStorageSlots, (struct Node *)freeTaskStorageSlot);

    /* Fallible base setup and semaphore initialization precede exposure. */
#if defined(EXEC_PLATFORM_TASKRESOURCE_SERVICE_CLEANUP)
    TaskResBase->trb_CleanupBinding.contract = TASKRESOURCE_CLEANUP_CONTRACT;
    TaskResBase->trb_CleanupBinding.cleanup = task_CleanupRemovedTask;
    TaskResBase->trb_CleanupBinding.prelaunch = task_RegisterNewTask;
#else
    SysBase->lb_TaskResBase = (struct Library *)TaskResBase;
#endif

#if !defined(EXEC_PLATFORM_TASKRESOURCE_SERVICE_CLEANUP)
    TaskResBase->trb_RemTask = SetFunction((struct Library *)SysBase, -48*LIB_VECTSIZE, AROS_SLIB_ENTRY(RemTask, Task, 48));
    TaskResBase->trb_NewAddTask = SetFunction((struct Library *)SysBase, -176*LIB_VECTSIZE, AROS_SLIB_ENTRY(NewAddTask, Task, 176));
#if (0)
    TaskResBase->trb_NewStackSwap = SetFunction((struct Library *)SysBase, -134*LIB_VECTSIZE, AROS_SLIB_ENTRY(NewStackSwap, Task, 134));
#endif
#endif /* Legacy vector interposition; selected P4 uses direct core callbacks. */

#if defined(EXEC_PLATFORM_TASKRESOURCE_SERVICE_CLEANUP)
    if (!task_BootstrapInventory(TaskResBase))
    {
        /* No binding was published and the helper freed its empty-node pool.
         * This slot is the only remaining allocation owned by failed init. */
        Remove((struct Node *)freeTaskStorageSlot);
        FreeMem(freeTaskStorageSlot, sizeof(*freeTaskStorageSlot));
        return FALSE;
    }
    return TRUE;
#else
    /* Add existing tasks using the legacy platform path. */
#if defined(__AROSEXEC_SMP__)
    ObtainSystemLock(&PrivExecBase(SysBase)->TaskRunning, SPINLOCK_MODE_READ, LOCKF_DISABLE);
    ForeachNode(&PrivExecBase(SysBase)->TaskRunning, curTask)
    {
        if (curTask->tc_State & TS_RUN)
        {
            if ((taskEntry = AllocMem(sizeof(struct TaskListEntry), MEMF_CLEAR)) != NULL)
            {
                D(bug("[TaskRes] 0x%p [R  ] %02d %s\n", curTask, GetIntETask(curTask)->iet_CpuNumber, curTask->tc_Node.ln_Name));
                NEWLIST(&taskEntry->tle_HookTypes);
                taskEntry->tle_Task = curTask;
                AddTail(&TaskResBase->trb_TaskList, &taskEntry->tle_Node);
            }
            else
            {
                bug("[TaskRes] Failed to allocate storage for task @  0x%p!!\n", curTask);
            }
        }
        else
        {
            bug("[TaskRes] Invalid Task State %08x for task @ 0x%p\n", curTask->tc_State, curTask);
        }
    }
    ReleaseSystemLock(&PrivExecBase(SysBase)->TaskRunning, LOCKF_DISABLE);

    ObtainSystemLock(&PrivExecBase(SysBase)->TaskSpinning, SPINLOCK_MODE_READ, LOCKF_DISABLE);
    ForeachNode(&PrivExecBase(SysBase)->TaskSpinning, curTask)
    {
        if (curTask->tc_State & TS_SPIN)
        {
            if ((taskEntry = AllocMem(sizeof(struct TaskListEntry), MEMF_CLEAR)) != NULL)
            {
                D(bug("[TaskRes] 0x%p [  S] %02d %s\n", curTask, GetIntETask(curTask)->iet_CpuNumber, curTask->tc_Node.ln_Name));
                NEWLIST(&taskEntry->tle_HookTypes);
                taskEntry->tle_Task = curTask;
                AddTail(&TaskResBase->trb_TaskList, &taskEntry->tle_Node);
            }
            else
            {
                bug("[TaskRes] Failed to allocate storage for task @  0x%p!!\n", curTask);
            }
        }
        else
        {
            bug("[TaskRes] Invalid Task State %08x for task @ 0x%p\n", curTask->tc_State, curTask);
        }
    }
    ReleaseSystemLock(&PrivExecBase(SysBase)->TaskSpinning, LOCKF_DISABLE);

    ObtainSystemLock(&SysBase->TaskReady, SPINLOCK_MODE_READ, LOCKF_DISABLE);
    // TODO : list TaskSpinning tasks..
#else
    Disable();
    if (SysBase->ThisTask)
    {
        if (SysBase->ThisTask->tc_State & TS_RUN)
        {
            if ((taskEntry = AllocMem(sizeof(struct TaskListEntry), MEMF_CLEAR)) != NULL)
            {
                D(bug("[TaskRes] 0x%p [R--] 00 %s\n", SysBase->ThisTask, SysBase->ThisTask->tc_Node.ln_Name));
                NEWLIST(&taskEntry->tle_HookTypes);
                taskEntry->tle_Task = SysBase->ThisTask;
                AddTail(&TaskResBase->trb_TaskList, &taskEntry->tle_Node);
            }
            else
            {
                bug("[TaskRes] Failed to allocate storage for task @  0x%p!!\n", SysBase->ThisTask);
            }
        }
        else
        {
            bug("[TaskRes] Invalid Task State %08x for task @ 0x%p\n", SysBase->ThisTask->tc_State, curTask);
        }
    }
#endif
    ForeachNode(&SysBase->TaskReady, curTask)
    {
        if (curTask->tc_State & (TS_READY|TS_RUN))
        {
            if ((taskEntry = AllocMem(sizeof(struct TaskListEntry), MEMF_CLEAR)) != NULL)
            {
                D(bug("[TaskRes] 0x%p [-R-] -- %s\n", curTask, curTask->tc_Node.ln_Name));
                NEWLIST(&taskEntry->tle_HookTypes);
                taskEntry->tle_Task = curTask;
                AddTail(&TaskResBase->trb_TaskList, &taskEntry->tle_Node);
            }
            else
            {
                bug("[TaskRes] Failed to allocate storage for task @  0x%p!!\n", curTask);
            }
        }
        else
        {
            bug("[TaskRes] Invalid Task State %08x for task @ 0x%p\n", curTask->tc_State, curTask);
        }
    }
#if defined(__AROSEXEC_SMP__)
    ReleaseSystemLock(&SysBase->TaskReady, LOCKF_DISABLE);

    ObtainSystemLock(&SysBase->TaskWait, SPINLOCK_MODE_READ, LOCKF_DISABLE);
#endif
    ForeachNode(&SysBase->TaskWait, curTask)
    {
        if (curTask->tc_State & TS_WAIT)
        {
            if ((taskEntry = AllocMem(sizeof(struct TaskListEntry), MEMF_CLEAR)) != NULL)
            {
                D(bug("[TaskRes] 0x%p [--W] -- %s\n", curTask, curTask->tc_Node.ln_Name));
                NEWLIST(&taskEntry->tle_HookTypes);
                taskEntry->tle_Task = curTask;
                AddTail(&TaskResBase->trb_TaskList, &taskEntry->tle_Node);
            }
            else
            {
                bug("[TaskRes] Failed to allocate storage for task @  0x%p!!\n", curTask);
            }
        }
        else
        {
            bug("[TaskRes] Invalid Task State %08x for task @ 0x%p\n", curTask->tc_State, curTask);
        }
    }
#if defined(__AROSEXEC_SMP__)
    ReleaseSystemLock(&SysBase->TaskWait, LOCKF_DISABLE);
#else
    Enable();
#endif

#endif /* Selected P4 transaction or legacy enumeration. */
#else
    SysBase->lb_TaskResBase = (struct Library *)TaskResBase;
#endif /* TASKRES_ENABLE */

    return TRUE;
}

static LONG taskres_Exit(struct TaskResBase *TaskResBase)
{
#if defined(EXEC_PLATFORM_TASKRESOURCE_SERVICE_CLEANUP)
    /* Published callback and interposers are boot-lifetime resident. Failed
     * initialization still follows the ordinary rollback below. */
    if (__atomic_load_n(&SysBase->lb_TaskResBase, __ATOMIC_ACQUIRE) ==
        (struct Library *)TaskResBase)
        return FALSE;
#endif
#ifdef TASKRES_ENABLE
    /*
     * Only put back what was actually taken. The expunge set also runs
     * when the init set fails, and taskres_Init has several exits before
     * it patches anything, so an unconditional restore writes a NULL
     * over a vector exec installed itself - leaving AddTask and RemTask
     * jumping to address zero in a system that is otherwise fine.
     */
    if (TaskResBase->trb_NewAddTask)
        SetFunction((struct Library *)SysBase, -176*LIB_VECTSIZE, TaskResBase->trb_NewAddTask);
    if (TaskResBase->trb_RemTask)
        SetFunction((struct Library *)SysBase, -48*LIB_VECTSIZE, TaskResBase->trb_RemTask);
#endif /* TASKRES_ENABLE */

    if (SysBase->lb_TaskResBase == (struct Library *)TaskResBase)
        SysBase->lb_TaskResBase = NULL;

    CloseLibrary(TaskResBase->trb_UtilityBase);

    return TRUE;
}

ADD2INITLIB(taskres_Init, 0)

ADD2EXPUNGELIB(taskres_Exit, 0)
