/*
 * P4 scheduler front door. Selected only for the matched-SMP experiment
 * (__AROSEXEC_SMP__) or the transitional Giant build (P4_GIANT); every
 * other P4 build uses rom/kernel/kernel_scheduler.c unchanged.
 */
#include <exec/alerts.h>
#include <exec/execbase.h>
#include <proto/exec.h>
#include <kernel_scheduler.h>

#if defined(__AROSEXEC_SMP__)

#include "p4_schedule.h"
#include "p4_public_spinlock.h"
#if !defined(__AROSPLATFORM_SMP__)
#error "P4 scheduler override requires matched Exec SMP"
#endif

BOOL core_Schedule(void)
{
    return krnP4ShouldSchedule(SysBase) ? TRUE : FALSE;
}

/* P4 cpu_Switch/cpu_Dispatch call the lifetime-aware P4 implementations.
 * No generic queue mutation may bypass those ownership transactions. */
void core_Switch(void)
{
    krnP4PublicSpinFatal(P4_PUBLIC_SPIN_BAD_STATE);
}

struct Task *core_Dispatch(void)
{
    krnP4PublicSpinFatal(P4_PUBLIC_SPIN_BAD_STATE);
}

#elif defined(P4_GIANT)

/*
 * The generic scheduler with three changes, all running under the Giant D
 * lock that every trap holds:
 *
 *  - only tasks allowed on this hart are considered (p4_giant.h flags);
 *  - a task that resumes inside Disable() needs F as well, which a trap
 *    may only try-lock; if F is busy the task is left in TaskReady and
 *    the F holder wakes this hart when it lets go;
 *  - a task put back into TaskReady may wake the other hart.
 */
#include <kernel_base.h>
#include <kernel_debug.h>
#include <exec_platform.h>
#include "p4_giant.h"

#define D(x)

static struct Task *first_runnable(uint32_t hart)
{
    struct Task *t;

    ForeachNode(&SysBase->TaskReady, t)
        if (p4_giant_task_allowed(t, hart))
            return t;
    return NULL;
}

BOOL core_Schedule(void)
{
    struct Task *task = GET_THIS_TASK;
    uint32_t hart = p4_giant_hart();
    struct Task *best;

    FLAG_SCHEDSWITCH_CLEAR;

    if (task == NULL)
        return TRUE;
    if (task->tc_State == TS_REMOVED)
        return FALSE;
    /* The task may no longer be allowed here (SetTaskPri()-style flag
       changes are taken at the next scheduling point). */
    if (!p4_giant_task_allowed(task, hart))
        return TRUE;

    if (!(task->tc_Flags & TF_EXCEPT))
    {
        best = first_runnable(hart);
        if (best == NULL)
            return FALSE;
        if (best->tc_Node.ln_Pri <= task->tc_Node.ln_Pri &&
            !FLAG_SCHEDQUANTUM_ISSET)
            return FALSE;
    }
    return TRUE;
}

void core_Switch(void)
{
    struct Task *task = GET_THIS_TASK;
    ULONG showAlert = 0;

    if (task->tc_State != TS_RUN)
        Remove(&task->tc_Node);

    if ((task->tc_State != TS_WAIT) && (task->tc_State != TS_REMOVED))
        task->tc_State = TS_READY;

    if (task->tc_SPReg <= task->tc_SPLower || task->tc_SPReg > task->tc_SPUpper)
    {
        bug("[KRN] Task %s went out of stack limits\n", task->tc_Node.ln_Name);
        bug("[KRN] Lower %p, upper %p, SP %p\n", task->tc_SPLower,
            task->tc_SPUpper, task->tc_SPReg);
        task->tc_SigWait    = 0;
        task->tc_State      = TS_WAIT;
        showAlert = AN_StackProbe;
    }

    task->tc_IDNestCnt = IDNESTCOUNT_GET;

    if (task->tc_State == TS_READY)
    {
        if (task->tc_Flags & TF_SWITCH)
            AROS_UFC1NR(void, task->tc_Switch,
                        AROS_UFCA(struct ExecBase *, SysBase, A6));
        Enqueue(&SysBase->TaskReady, &task->tc_Node);
        p4_giant_task_ready(task);
    }
    else if (task->tc_State != TS_REMOVED)
    {
        Enqueue(&SysBase->TaskWait, &task->tc_Node);
    }
    if (showAlert)
        Alert(showAlert);
}

struct Task *core_Dispatch(void)
{
    uint32_t hart = p4_giant_hart();
    struct p4_giant_cpu *cpu = &p4_giant_cpus[hart];
    struct Task *task, *found = NULL;
    int need_f = 0;

    ForeachNode(&SysBase->TaskReady, task)
    {
        if (!p4_giant_task_allowed(task, hart))
            continue;
        /* A task that blocked in Wait() resumes inside its Disable().
           Announce the interest before trying, so an F holder releasing
           right now still wakes this hart. */
        need_f = task->tc_IDNestCnt >= 0;
        if (need_f)
        {
            cpu->want_f = 1;
            __asm__ volatile("fence rw, rw" : : : "memory");
            if (!p4_giant_try_f())
            {
                cpu->n_frefuse++;
                continue;
            }
        }
        found = task;
        break;
    }

    if (found == NULL)
    {
        /* Whatever F was held for, the switched-out task no longer runs. */
        p4_giant_release_f();
        SysBase->IdleCount++;
        FLAG_SCHEDSWITCH_SET;
        return NULL;
    }

    task = found;
    Remove(&task->tc_Node);
    cpu->want_f = 0;
    if (!need_f)
        p4_giant_release_f();

    if (task->tc_State == TS_READY)
    {
        IDNESTCOUNT_SET(task->tc_IDNestCnt);
        SET_THIS_TASK(task);
        SCHEDELAPSED_SET(SCHEDQUANTUM_GET);
        FLAG_SCHEDQUANTUM_CLEAR;
        if (task->tc_SPReg <= task->tc_SPLower || task->tc_SPReg > task->tc_SPUpper)
            task->tc_State     = TS_WAIT;
        else
            task->tc_State     = TS_RUN;
    }

    if (task->tc_State != TS_RUN)
    {
        core_Switch();
        task = core_Dispatch();
    }
    else
    {
        SysBase->DispCount++;
    }

    return task;
}

#else
#error "P4 scheduler override requires __AROSEXEC_SMP__ or P4_GIANT"
#endif
