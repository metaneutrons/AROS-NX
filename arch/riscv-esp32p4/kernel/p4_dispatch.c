/*
 * ESP32-P4 SMP dispatcher. List protection is never inverted into a Task
 * lock: take a lifetime pin while holding Ready, drop Ready, then lock Task
 * and revalidate through the Ready-to-Running transaction. Running lifetime
 * ownership survives release of the temporary pin and protects restoration.
 */
#if !defined(P4_DISPATCH_TEST)
#include <stddef.h>
#include <exec/execbase.h>
#include <proto/exec.h>
#include <aros/riscv/cpucontext.h>
#include "exec_intern.h"
#include "etask.h"
#include "cpu_local_runtime.h"
#include "p4_public_spinlock.h"
#include "p4_task_pins.h"
#include "p4_scheduler_queue.h"
#include "p4_dispatch.h"
#if !defined(__AROSEXEC_SMP__) || !defined(__AROSPLATFORM_SMP__)
#error "P4 dispatcher requires the matched Exec SMP ABI"
#endif
_Static_assert(offsetof(struct Task, tc_Node) == 0,
               "Ready node identity must be the Task identity");
#endif

#define P4_DISPATCH_SCAN_LIMIT 20000u

static void p4_dispatch_fatal(void)
{
    krnP4PublicSpinFatal(P4_PUBLIC_SPIN_BAD_STATE);
}

/* Previous is pinned by the caller. Concurrent removal can change the
 * traversal, in which case restart from Head. Validate the entire list before
 * choosing anything; a corrupt backlink after our candidate is still fatal.
 * No Task state, ETask or affinity is read while holding the list lock. */
static struct Task *p4_ready_next_pinned(struct Task *previous,
    struct ExecBase *SysBase)
{
    struct List *list = &SysBase->TaskReady;
    struct Node *node, *pred, *tail, *choice;
    struct Task *task = NULL;
    unsigned int left = P4_DISPATCH_SCAN_LIMIT;
    int valid;
    EXEC_SPINLOCK_LOCK(&PrivExecBase(SysBase)->TaskReadySpinLock,
                       NULL, SPINLOCK_MODE_WRITE);
    tail = (struct Node *)&list->lh_Tail;
    node = list->lh_Head;
    pred = (struct Node *)&list->lh_Head;
    choice = node;
    while (node != NULL && node != tail && left--)
    {
        if (node->ln_Pred != pred || node->ln_Succ == NULL)
            break;
        if (previous != NULL && node == &previous->tc_Node)
            choice = node->ln_Succ;
        pred = node;
        node = node->ln_Succ;
    }
    valid = list->lh_Tail == NULL && node == tail &&
            list->lh_TailPred == pred;
    if (valid)
    {
        /* Retired nodes are refused by the registry. Advancing under Ready
         * protects their node storage even when no new pin can be taken. */
        left = P4_DISPATCH_SCAN_LIMIT;
        while (choice != tail && left--)
        {
            if (krnP4SignalTaskPin((struct Task *)choice, SysBase))
            {
                task = (struct Task *)choice;
                break;
            }
            choice = choice->ln_Succ;
        }
    }
    EXEC_SPINLOCK_UNLOCK(&PrivExecBase(SysBase)->TaskReadySpinLock);
    if (!valid)
        p4_dispatch_fatal();
    return task;
}

struct Task *krnP4Dispatch(struct ExecBase *SysBase)
{
    struct p4_exec_runtime *runtime = p4_exec_runtime_current(SysBase);
    struct Task *candidate, *next;
    unsigned int attempts = P4_DISPATCH_SCAN_LIMIT;
    if (runtime == NULL || runtime->hart_id >= 2 || runtime->cpu_local == NULL ||
        runtime->cpu_local->current_task != NULL ||
        runtime->wait_pending != NULL || runtime->cpu_local->trap_depth == 0)
        p4_dispatch_fatal();
    candidate = p4_ready_next_pinned(NULL, SysBase);
    while (candidate != NULL && attempts--)
    {
        int admitted;
        /* Pin the successor before relinquishing this identity. Do not take
         * a queue lock while holding two canonical Task locks. */
        next = p4_ready_next_pinned(candidate, SysBase);
        EXEC_SPINLOCK_LOCK(&candidate->tc_SpinLock, NULL, SPINLOCK_MODE_WRITE);
        /* Validate the image while it is still Ready. No running ownership
         * may be published for a context that cpu_Dispatch cannot restore.
         * A malformed saved context is terminal until a qualified quarantine
         * path exists; silently resuming it is never permitted. */
        if (candidate->tc_State == TS_READY)
        {
            struct ETask *etask = GetETask(candidate);
            struct ExceptionContext *ctx = etask ? etask->et_RegFrame : NULL;
            if (ctx == NULL || (uintptr_t)candidate->tc_SPReg <= (uintptr_t)candidate->tc_SPLower ||
                (uintptr_t)candidate->tc_SPReg > (uintptr_t)candidate->tc_SPUpper ||
                (uintptr_t)candidate->tc_SPReg != (uintptr_t)ctx->sp ||
                ((uintptr_t)ctx->sp & 15u) != 0 || ctx->pc == 0 ||
                (ctx->pc & 1u) != 0 ||
                ((ctx->Flags & ECF_FPU) != 0 &&
                 (ctx->fpuContext == NULL ||
                  ((uintptr_t)ctx->fpuContext & 3u) != 0)))
                p4_dispatch_fatal();
        }
        admitted = krnP4ReadyToRunningLocked(candidate, runtime->hart_id, SysBase);
        if (admitted)
        {
            if (!p4_exec_runtime_set_current_task(SysBase, candidate))
                p4_dispatch_fatal();
            runtime->cpu_local->id_nest = candidate->tc_IDNestCnt;
            runtime->elapsed = runtime->quantum;
            runtime->cpu_local->schedule_flags &= ~P4_EXEC_SCHEDULE_QUANTUM;
            ++runtime->dispatch_count;
        }
        EXEC_SPINLOCK_UNLOCK(&candidate->tc_SpinLock);
        krnP4SignalTaskUnpin(candidate, SysBase);
        if (admitted)
        {
            if (next != NULL)
                krnP4SignalTaskUnpin(next, SysBase);
            return candidate; /* now protected by the unique running owner */
        }
        candidate = next;
    }
    if (candidate != NULL)
        krnP4SignalTaskUnpin(candidate, SysBase);
    ++runtime->cpu_local->idle_count;
    runtime->cpu_local->schedule_flags |= P4_EXEC_SCHEDULE_SWITCH;
    return NULL;
}
