/*
    Copyright (c) 2013-2026, The AROS Development Team. All rights reserved.

    Desc: scheduler list helpers of the smp variant of the esp32p4-riscv
          target, from arch/aarch64-native.
*/

#include <aros/symbolsets.h>
#include <exec/execbase.h>
#include <exec/tasks.h>

#include <proto/exec.h>
#include <proto/kernel.h>

#include "exec_intern.h"
#include "etask.h"
#include "tls.h"

/* kernel.resource, linked into the same image */
extern void krnP4IPISend(unsigned int hart, ULONG work);
#define P4_IPI_SCHEDULE         (1UL << 2)

static inline unsigned int this_hart(void)
{
    unsigned long hart;

    asm volatile("csrr %0, mhartid" : "=r"(hart));
    return hart;
}

/*
 * A task has just become ready. Another hart it may run on, running
 * something of lower priority - typically its idle task - would only
 * notice at its next quantum; tell it now. Reading that hart's current
 * task without its lock is only a hint: a wrong guess costs one
 * interrupt or one quantum, nothing else.
 */
static void kick_other_harts(struct Task *task)
{
    ULONG online = __atomic_load_n(&__p4_harts_online, __ATOMIC_ACQUIRE);
    unsigned int me = this_hart();
    unsigned int hart;
    void *aff;

    if (!(task->tc_Flags & TF_ETASK) || !task->tc_UnionETask.tc_ETask)
        return;
    aff = IntETask(task->tc_UnionETask.tc_ETask)->iet_CpuAffinity;

    for (hart = 0; hart < P4_TLS_HARTS; hart++)
    {
        struct Task *running;

        if (hart == me || !(online & (1UL << hart)) || !KrnCPUInMask(hart, aff))
            continue;
        running = __atomic_load_n(&__p4_tls[hart].ThisTask, __ATOMIC_RELAXED);
        if (!running || running->tc_Node.ln_Pri < task->tc_Node.ln_Pri)
            krnP4IPISend(hart, P4_IPI_SCHEDULE);
    }
}

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
            if (PrivExecBase(SysBase)->IntFlags & EXECF_CPUAffinity)
                kick_other_harts(task);
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

/*
 * Early in exec's init, before its service task and anything else is
 * created: turn on the affinity paths (signals across harts, the masks
 * new tasks inherit) and bind the boot task to hart 0, whose cold start
 * is not safe to move. Everything it creates inherits that, so only tasks
 * created for another hart or for any hart run elsewhere.
 */
int Exec_P4SMPInit(struct ExecBase *SysBase)
{
    struct Task *boot = GET_THIS_TASK;

    PrivExecBase(SysBase)->IntFlags |= EXECF_CPUAffinity;

    if (boot && (boot->tc_Flags & TF_ETASK) && boot->tc_UnionETask.tc_ETask)
    {
        struct IntETask *iet = IntETask(boot->tc_UnionETask.tc_ETask);

        if (!iet->iet_CpuAffinity)
        {
            void *aff = KrnAllocCPUMask();

            if (aff)
            {
                KrnGetCPUMask(0, aff);
                iet->iet_CpuAffinity = aff;
                iet->iet_CpuNumber = 0;
            }
        }
    }
    return TRUE;
}

ADD2INITLIB(Exec_P4SMPInit, -127)
