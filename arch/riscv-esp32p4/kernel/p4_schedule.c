/*
 * Bounded P4-SMP scheduling decision candidate.
 *
 * The P4 SMP core_Schedule override selects this helper. It assumes the
 * current task's unique RUN reference protects Task storage, and that every
 * Ready-list priority/state writer follows Task -> Ready lock ordering.
 */
#if !defined(P4_SCHEDULE_TEST)
#include <stddef.h>
#include <exec/execbase.h>
#include <exec/tasks.h>
#include <proto/exec.h>
#include <asm/cpu.h>
#include "exec_intern.h"
#include "cpu_local_runtime.h"
#include "p4_public_spinlock.h"
#if !defined(__AROSEXEC_SMP__) || !defined(__AROSPLATFORM_SMP__)
#error "P4 scheduling decision requires the matched Exec SMP ABI"
#endif
#else
#include <stddef.h>
#include <stdint.h>
#endif
#include "p4_schedule.h"

#define P4_SCHEDULE_READY_SCAN_LIMIT 20000u

#if !defined(P4_SCHEDULE_PRIORITY)
#define P4_SCHEDULE_PRIORITY(task) ((task)->tc_Node.ln_Pri)
#endif

/* Validate sentinel, all backlinks, all successors and the exact tail before
 * permitting any priority read. The hard bound also refuses cyclic lists. */
static int p4_schedule_ready_valid(const struct List *list)
{
    const struct Node *node = list->lh_Head;
    const struct Node *previous = (const struct Node *)&list->lh_Head;
    const struct Node *tail = (const struct Node *)&list->lh_Tail;
    unsigned int left = P4_SCHEDULE_READY_SCAN_LIMIT;

    if (list->lh_Tail != NULL)
        return 0;

    while (node != NULL && node != tail && left--)
    {
        if (node->ln_Pred != previous || node->ln_Succ == NULL)
            return 0;
        previous = node;
        node = node->ln_Succ;
    }

    return node == tail && list->lh_TailPred == previous;
}

int krnP4ShouldSchedule(struct ExecBase *SysBase)
{
    struct p4_exec_runtime *runtime = p4_exec_runtime_current(SysBase);
    struct p4_cpu_local *local;
    struct Task *task;
    unsigned char state, flags;
    BYTE current_priority;
    int schedule;

    if (runtime == NULL || runtime->hart_id >= P4_EXEC_RUNTIME_HART_COUNT ||
        runtime->cpu_local == NULL ||
        runtime->cpu_local->hart_id != runtime->hart_id ||
        runtime->cpu_local->trap_depth != 1 ||
        (csr_read(mstatus) & MSTATUS_MIE) != 0)
        krnP4PublicSpinFatal(P4_PUBLIC_SPIN_BAD_STATE);

    local = runtime->cpu_local;
    /* The request is per-hart and is consumed even when there is no task. */
    local->schedule_flags &= ~P4_EXEC_SCHEDULE_SWITCH;
    task = local->current_task;
    if (task == NULL)
        return 0;

    /* The unique RUN reference acquired by dispatch protects this identity. */
    EXEC_SPINLOCK_LOCK(&task->tc_SpinLock, NULL, SPINLOCK_MODE_WRITE);
    state = task->tc_State;
    flags = task->tc_Flags;
    current_priority = task->tc_Node.ln_Pri;

    if (state == TS_REMOVED || state == TS_INVALID ||
        state == TS_TOMBSTONED)
    {
        EXEC_SPINLOCK_UNLOCK(&task->tc_SpinLock);
        return 1;
    }

    if (state != TS_RUN)
    {
        EXEC_SPINLOCK_UNLOCK(&task->tc_SpinLock);
        krnP4PublicSpinFatal(P4_PUBLIC_SPIN_BAD_STATE);
    }

    /* Preserve generic Exec's exception-first scheduling decision. */
    if (flags & TF_EXCEPT)
    {
        EXEC_SPINLOCK_UNLOCK(&task->tc_SpinLock);
        return 1;
    }

    /* SetTaskPri and all other Ready writers must use this same order. */
    EXEC_SPINLOCK_LOCK(&PrivExecBase(SysBase)->TaskReadySpinLock,
                       NULL, SPINLOCK_MODE_WRITE);
    if (!p4_schedule_ready_valid(&SysBase->TaskReady))
    {
        EXEC_SPINLOCK_UNLOCK(&PrivExecBase(SysBase)->TaskReadySpinLock);
        EXEC_SPINLOCK_UNLOCK(&task->tc_SpinLock);
        krnP4PublicSpinFatal(P4_PUBLIC_SPIN_BAD_STATE);
    }

    if (SysBase->TaskReady.lh_Head ==
        (struct Node *)&SysBase->TaskReady.lh_Tail)
    {
        schedule = 0;
    }
    else
    {
        /* Node identity is Task identity (tc_Node is the first field). */
        const struct Task *head = (const struct Task *)SysBase->TaskReady.lh_Head;
        BYTE head_priority = P4_SCHEDULE_PRIORITY(head);

        schedule = head_priority > current_priority ||
                   (local->schedule_flags & P4_EXEC_SCHEDULE_QUANTUM) != 0;
    }

    EXEC_SPINLOCK_UNLOCK(&PrivExecBase(SysBase)->TaskReadySpinLock);
    EXEC_SPINLOCK_UNLOCK(&task->tc_SpinLock);
    return schedule;
}
