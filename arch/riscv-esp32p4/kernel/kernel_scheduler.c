/*
    Copyright (c) 2015-2026, The AROS Development Team. All rights reserved.

    Desc: the scheduler of the smp variant of the esp32p4-riscv target.

    It replaces the generic one. Adapted from arch/aarch64-native: the
    lists are locked task lock first, list lock second, and a running task
    is on TaskRunning, which the generic scheduler does not know about.
*/

#include <exec/alerts.h>
#include <exec/execbase.h>
#include <exec/lists.h>
#include <proto/exec.h>

#include <kernel_base.h>
#include <kernel_scheduler.h>

#include <exec_platform.h>

#include <aros/types/spinlock_s.h>

#include <etask.h>

#include "exec_intern.h"
#include "kernel_intern.h"

/* exec_intern.h brings <aros/debug.h>, whose bug() is kprintf(); the
   kernel's own goes through KernelBase like the generic scheduler's. */
#undef bug
#include <kernel_debug.h>

#define DSCHED(x)

/* iet_CpuAffinity is a cpumask buffer or the TASKAFFINITY_ANY sentinel,
   never a raw bitmask. NULL means "run anywhere". */
static inline BOOL core_AffinityMatch(struct Task *t, uint32_t cpumask)
{
    void *aff = (void *)(IPTR)GetIntETask(t)->iet_CpuAffinity;

    if (!aff || (IPTR)aff == TASKAFFINITY_ANY)
        return TRUE;

    return (((uint32_t *)aff)[0] & cpumask) != 0;
}

/* A single attempt: everyone else takes the task lock before a list lock,
   so blocking here, with the list lock held, could deadlock. */
static inline BOOL core_TrySpinLockWrite(spinlock_t *lock)
{
    unsigned int value = SPINLOCK_UNLOCKED;

    if (!__atomic_compare_exchange_n(&lock->lock, &value, SPINLOCKF_WRITE, 0,
                                     __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
        return FALSE;

    lock->s_Owner = GET_THIS_TASK;
    return TRUE;
}

/* Check whether the task running on this hart should be rescheduled */
BOOL core_Schedule(void)
{
    struct Task *task = GET_THIS_TASK;
    BOOL corereschedule = TRUE;

    DSCHED(bug("[Kernel:%02d] core_Schedule()\n", GetCPUNumber()));

    FLAG_SCHEDSWITCH_CLEAR;

    /* A pending exception needs the dispatcher, so reschedule */
    if (!(task->tc_Flags & TF_EXCEPT))
    {
        /* Masked: interrupt handlers on this hart take these locks too */
        unsigned int __if = EXEC_IRQFIQ_DISABLE();

        EXEC_SPINLOCK_LOCK(&PrivExecBase(SysBase)->TaskReadySpinLock, NULL,
                           SPINLOCK_MODE_READ);

        /* Nothing else is ready: let the running task work */
        if (IsListEmpty(&SysBase->TaskReady))
            corereschedule = FALSE;
        else
        {
            struct Task *nexttask;
            uint32_t cpumask = 1 << GetCPUNumber();

            /*
             * If the first task ready for this hart has equal or lower
             * priority and the running task has not used up its quantum,
             * let it work on.
             */
            for (nexttask = (struct Task *)GetHead(&SysBase->TaskReady);
                 nexttask != NULL;
                 nexttask = (struct Task *)GetSucc(nexttask))
            {
                if (core_AffinityMatch(nexttask, cpumask))
                {
                    if (nexttask->tc_Node.ln_Pri <= task->tc_Node.ln_Pri)
                    {
                        if (!FLAG_SCHEDQUANTUM_ISSET)
                            corereschedule = FALSE;
                    }
                    break;
                }
            }
        }

        EXEC_SPINLOCK_UNLOCK(&PrivExecBase(SysBase)->TaskReadySpinLock);
        EXEC_IRQFIQ_RESTORE(__if);
    }

    return corereschedule;
}

/* Switch the task running on this hart to the ready state */
void core_Switch(void)
{
    struct Task *task = GET_THIS_TASK;
    unsigned int __if;

    DSCHED(bug("[Kernel:%02d] core_Switch(%08x)\n", GetCPUNumber(), task->tc_State));

    /* Every outgoing task, not only TS_RUN: Wait() switches away in
       TS_WAIT holding a Disable() level. */
    task->tc_IDNestCnt = IDNESTCOUNT_GET;

    /* Carry the unused slice with the task, or a task preempted more
       often than the tick never uses up its quantum. */
    if (GetIntETask(task))
        GetIntETask(task)->iet_QuantumLeft = SCHEDELAPSED_GET;

    if (task->tc_State != TS_RUN)
        return;

    /* tc_SpinLock across the whole RUN->READY change of state and list, so
       nobody sees TS_READY while the task is on no list. */
    __if = EXEC_IRQFIQ_DISABLE();
    EXEC_SPINLOCK_LOCK(&task->tc_SpinLock, NULL, SPINLOCK_MODE_WRITE);
    exec_TaskRemoveRunning(task);
    task->tc_State = TS_READY;

    /* A task out of its stack bounds is suspended before it does more harm */
    if (task->tc_SPReg <= task->tc_SPLower || task->tc_SPReg > task->tc_SPUpper)
    {
        bug("[Kernel:%02d] '%s' @ 0x%p went out of stack limits\n",
            GetCPUNumber(), task->tc_Node.ln_Name, task);
        bug("[Kernel:%02d]  - Lower 0x%p, upper 0x%p, SP 0x%p\n",
            GetCPUNumber(), task->tc_SPLower, task->tc_SPUpper, task->tc_SPReg);

        task->tc_SigWait = 0;
        task->tc_State   = TS_WAIT;
        exec_TaskEnqueueWait(task);

        Alert(AN_StackProbe);
    }

    if (task->tc_Flags & TF_SWITCH)
        AROS_UFC1NR(void, task->tc_Switch, AROS_UFCA(struct ExecBase *, SysBase, A6));

    if (task->tc_State == TS_READY)
        exec_TaskEnqueueReady(task);

    EXEC_SPINLOCK_UNLOCK(&task->tc_SpinLock);
    EXEC_IRQFIQ_RESTORE(__if);
}

/* Dispatch a new ready task on this hart */
struct Task *core_Dispatch(void)
{
    struct Task *newtask;
    BOOL taskStateLocked = FALSE;
    uint32_t cpumask = 1 << GetCPUNumber();
    BOOL sawContended;
    BOOL launchtask = TRUE;
    /* Masked for the whole dispatch, which holds list locks and
       tc_SpinLock that interrupt handlers on this hart also take. Nests
       across the recursive call below. */
    unsigned int __if = EXEC_IRQFIQ_DISABLE();

    DSCHED(bug("[Kernel:%02d] core_Dispatch()\n", GetCPUNumber()));

dispatch_rescan:
    sawContended = FALSE;
    EXEC_SPINLOCK_LOCK(&PrivExecBase(SysBase)->TaskReadySpinLock, NULL,
                       SPINLOCK_MODE_WRITE);
    for (newtask = (struct Task *)GetHead(&SysBase->TaskReady);
         newtask != NULL;
         newtask = (struct Task *)GetSucc(newtask))
    {
        if (core_AffinityMatch(newtask, cpumask))
        {
            /* Locked before it leaves the list, so that the change of
               state and list is atomic to anyone trusting tc_State. */
            if (!core_TrySpinLockWrite(&newtask->tc_SpinLock))
            {
                sawContended = TRUE;
                continue;
            }
            taskStateLocked = TRUE;
            Remove(&newtask->tc_Node);
            break;
        }
    }
    EXEC_SPINLOCK_UNLOCK(&PrivExecBase(SysBase)->TaskReadySpinLock);

    /* Every candidate was locked elsewhere for a moment: look again
       rather than idle with runnable work on the list. */
    if (!newtask && sawContended)
    {
#ifdef P4_SPIN_WATCHDOG
        static unsigned int rescans;

        if (++rescans == 5000000u)
            krnP4SpinReport("dispatch-rescan", NULL, __builtin_return_address(0));
#endif
        goto dispatch_rescan;
    }

    /*
     * No fallback to the outgoing task, unlike aarch64-native. With two
     * harts it is in none of the states that one allows: core_Switch() has
     * put it on the ready list, where the scan above finds it if it may
     * run here (if its new mask excludes this hart, resuming it here
     * would leave it on that list for the other hart as well); it is
     * waiting; RemTask() has removed it; or, when this pointer is left
     * over from an earlier pass, the other hart may be running it. Each
     * hart has an idle task bound to it, so the scan finds that at least,
     * and NULL only happens before it exists.
     */

    if (newtask != NULL)
    {
        if (newtask->tc_State == TS_READY || newtask->tc_State == TS_RUN)
        {
            struct IntETask *iet = GetIntETask(newtask);
            ULONG left = iet ? iet->iet_QuantumLeft : 0;

            DSCHED(bug("[Kernel:%02d] Preparing to run '%s' @ 0x%p\n",
                       GetCPUNumber(), newtask->tc_Node.ln_Name, newtask));

            SysBase->DispCount++;
            IDNESTCOUNT_SET(newtask->tc_IDNestCnt);
            /* Every task here comes off the ready list. Kept as a guard:
               a task in TS_RUN is already ThisTask and on TaskRunning,
               and a second AddHead() would corrupt that list. */
            if (newtask->tc_State == TS_READY)
                SET_THIS_TASK(newtask);
            /* A fresh slice only when the task used its own up */
            SCHEDELAPSED_SET(left ? left : SCHEDQUANTUM_GET);
            FLAG_SCHEDQUANTUM_CLEAR;

            /* Check the stack of the task about to run */
            if (newtask->tc_SPReg <= newtask->tc_SPLower ||
                newtask->tc_SPReg > newtask->tc_SPUpper)
            {
                if (!taskStateLocked)
                {
                    EXEC_SPINLOCK_LOCK(&newtask->tc_SpinLock, NULL,
                                       SPINLOCK_MODE_WRITE);
                    taskStateLocked = TRUE;
                }
                newtask->tc_State = TS_WAIT;
            }
            else
                newtask->tc_State = TS_RUN;
        }

        if (newtask->tc_State == TS_WAIT)
        {
            if (!taskStateLocked)
            {
                EXEC_SPINLOCK_LOCK(&newtask->tc_SpinLock, NULL,
                                   SPINLOCK_MODE_WRITE);
                taskStateLocked = TRUE;
            }
            /* SET_THIS_TASK() above put it on TaskRunning; it must leave
               that list before it joins TaskWait, or its node would be
               linked into both. */
            exec_TaskRemoveRunning(newtask);
            exec_TaskEnqueueWait(newtask);
            launchtask = FALSE;
        }

        if (taskStateLocked)
        {
            EXEC_SPINLOCK_UNLOCK(&newtask->tc_SpinLock);
            taskStateLocked = FALSE;
        }

        if (!launchtask)
        {
            /* The new task must not run: reschedule */
            DSCHED(bug("[Kernel:%02d] Skipping '%s' @ 0x%p (state %08x)\n",
                       GetCPUNumber(), newtask->tc_Node.ln_Name, newtask,
                       newtask->tc_State));

            core_Switch();
            newtask = core_Dispatch();
        }
    }
    else
    {
        /* Nothing to do: the caller idles. Counted on every entry. */
        SysBase->IdleCount++;
        FLAG_SCHEDSWITCH_SET;
    }

    EXEC_IRQFIQ_RESTORE(__if);
    return newtask;
}
