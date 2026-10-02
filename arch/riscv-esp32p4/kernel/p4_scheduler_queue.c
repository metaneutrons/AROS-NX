/*
 * Real Exec-list adapter for the separately tested signal state machine.
 * SMP-only candidate: other task-list writers and lifetime reclamation
 * must adopt its locking contract before the P4 SMP gate can be enabled.
 */
#if !defined(P4_SCHEDULER_QUEUE_TEST)
#include <exec/execbase.h>
#include <proto/exec.h>
#include "exec_intern.h"
#include "etask.h"
#include "p4_cpu_masks.h"
#include "p4_task_pins.h"
#if !defined(__AROSEXEC_SMP__) || !defined(__AROSPLATFORM_SMP__)
#error "P4 scheduler queues require the matched SMP Task/Exec ABI"
#endif
#endif
#include "p4_scheduler_queue.h"

/* A corrupt/cyclic list must not turn a failed membership check into an
 * unbounded masked-interrupt loop. Legitimate larger queues are refused. */
#define P4_QUEUE_SCAN_LIMIT 20000u

/* Unlike a scheduler selection operation, this helper receives an already
 * pinned, Task-locked candidate. Never take its Task lock under a list lock.
 * All fallible validation precedes running-reference admission; admission
 * is the last fallible operation before the list/state publication. */

static int p4_queue_contains(struct List *list, const struct Task *task)
{
    struct Node *node = list->lh_Head;
    struct Node *previous = (struct Node *)&list->lh_Head;
    struct Node *tail = (struct Node *)&list->lh_Tail;
    unsigned int left = P4_QUEUE_SCAN_LIMIT;
    int found = 0;

    if (list->lh_Tail != NULL)
        return -1;
    while (node != NULL && node != tail && left--)
    {
        if (node->ln_Pred != previous || node->ln_Succ == NULL)
            return -1;
        if (node == &task->tc_Node)
            found = 1;
        previous = node;
        node = node->ln_Succ;
    }
    return node == tail && list->lh_TailPred == previous ? found : -1;
}

int krnP4SignalEligibleHartLocked(struct Task *task)
{
    unsigned int hart, count;
    if (task == NULL || GetIntETask(task) == NULL)
        return -1;
    count = krnP4GetCPUCount();
    if (count == 0 || count > 2)
        return -1;
    for (hart = 0; hart < count; ++hart)
        if (krnP4CPUInMask(hart, GetIntETask(task)->iet_CpuAffinity))
            return (int)hart + 1;
    return -1;
}

#if !defined(P4_SCHEDULER_QUEUE_TEST) || defined(P4_RUNNING_QUEUE_TEST)
int krnP4ReadyToRunningLocked(struct Task *task, unsigned int hart,
    struct ExecBase *SysBase)
{
    spinlock_t *ready, *running;
    unsigned int count;
    int accepted = 0;
    if (task == NULL || SysBase == NULL || GetIntETask(task) == NULL)
        return 0;
    count = krnP4GetCPUCount();
    if (count == 0 || count > 2 || hart >= count ||
        !krnP4CPUInMask(hart, GetIntETask(task)->iet_CpuAffinity))
        return 0;
    ready = &PrivExecBase(SysBase)->TaskReadySpinLock;
    running = &PrivExecBase(SysBase)->TaskRunningSpinLock;
    EXEC_SPINLOCK_LOCK(ready, NULL, SPINLOCK_MODE_WRITE);
    EXEC_SPINLOCK_LOCK(running, NULL, SPINLOCK_MODE_WRITE);
    if (task->tc_State == TS_READY &&
        p4_queue_contains(&SysBase->TaskReady, task) == 1 &&
        p4_queue_contains(&PrivExecBase(SysBase)->TaskRunning, task) == 0 &&
        krnP4TaskRecordEnterRunning(task, hart, SysBase))
    {
        Remove(&task->tc_Node);
        GetIntETask(task)->iet_CpuNumber = hart;
        task->tc_State = TS_RUN;
        Enqueue(&PrivExecBase(SysBase)->TaskRunning, &task->tc_Node);
        accepted = 1;
    }
    EXEC_SPINLOCK_UNLOCK(running);
    EXEC_SPINLOCK_UNLOCK(ready);
    return accepted;
}
#endif

#if !defined(P4_SCHEDULER_QUEUE_TEST) || defined(P4_TASK_PUBLICATION_TEST)
int krnP4NewTaskReadyLocked(struct Task *task, struct ExecBase *SysBase)
{
    int destination;
    if (task == NULL || SysBase == NULL || task->tc_State != TS_ADDED)
        return 0;
    destination = krnP4SignalEligibleHartLocked(task);
    if (destination < 1)
        return 0;
    EXEC_SPINLOCK_LOCK(&PrivExecBase(SysBase)->TaskReadySpinLock, NULL, SPINLOCK_MODE_WRITE);
    EXEC_SPINLOCK_LOCK(&PrivExecBase(SysBase)->TaskWaitSpinLock, NULL, SPINLOCK_MODE_WRITE);
    EXEC_SPINLOCK_LOCK(&PrivExecBase(SysBase)->TaskRunningSpinLock, NULL, SPINLOCK_MODE_WRITE);
    if (p4_queue_contains(&SysBase->TaskReady, task) != 0 ||
        p4_queue_contains(&SysBase->TaskWait, task) != 0 ||
        p4_queue_contains(&PrivExecBase(SysBase)->TaskRunning, task) != 0)
        destination = 0;
    else
    {
        task->tc_State = TS_READY;
        Enqueue(&SysBase->TaskReady, &task->tc_Node);
    }
    EXEC_SPINLOCK_UNLOCK(&PrivExecBase(SysBase)->TaskRunningSpinLock);
    EXEC_SPINLOCK_UNLOCK(&PrivExecBase(SysBase)->TaskWaitSpinLock);
    EXEC_SPINLOCK_UNLOCK(&PrivExecBase(SysBase)->TaskReadySpinLock);
    return destination;
}

int krnP4BootTaskRunningLocked(struct Task *task, struct ExecBase *SysBase)
{
    int result = 0;
    if (task == NULL || SysBase == NULL || task->tc_State != TS_INVALID)
        return 0;
    EXEC_SPINLOCK_LOCK(&PrivExecBase(SysBase)->TaskRunningSpinLock, NULL, SPINLOCK_MODE_WRITE);
    if (p4_queue_contains(&PrivExecBase(SysBase)->TaskRunning, task) == 0 &&
        krnP4TaskRecordEnterRunning(task, 0, SysBase))
    {
        task->tc_State = TS_RUN;
        Enqueue(&PrivExecBase(SysBase)->TaskRunning, &task->tc_Node);
        result = 1;
    }
    EXEC_SPINLOCK_UNLOCK(&PrivExecBase(SysBase)->TaskRunningSpinLock);
    return result;
}
#endif

#if !defined(P4_SCHEDULER_QUEUE_TEST) || defined(P4_TASK_DETACH_TEST)
int krnP4TaskDetachedLocked(struct Task *task, struct ExecBase *SysBase)
{
    int detached;
    if (task == NULL || SysBase == NULL)
        return 0;
    EXEC_SPINLOCK_LOCK(&PrivExecBase(SysBase)->TaskReadySpinLock, NULL, SPINLOCK_MODE_WRITE);
    EXEC_SPINLOCK_LOCK(&PrivExecBase(SysBase)->TaskWaitSpinLock, NULL, SPINLOCK_MODE_WRITE);
    EXEC_SPINLOCK_LOCK(&PrivExecBase(SysBase)->TaskRunningSpinLock, NULL, SPINLOCK_MODE_WRITE);
    detached = p4_queue_contains(&SysBase->TaskReady, task) == 0 &&
               p4_queue_contains(&SysBase->TaskWait, task) == 0 &&
               p4_queue_contains(&PrivExecBase(SysBase)->TaskRunning, task) == 0;
    EXEC_SPINLOCK_UNLOCK(&PrivExecBase(SysBase)->TaskRunningSpinLock);
    EXEC_SPINLOCK_UNLOCK(&PrivExecBase(SysBase)->TaskWaitSpinLock);
    EXEC_SPINLOCK_UNLOCK(&PrivExecBase(SysBase)->TaskReadySpinLock);
    return detached;
}
#endif

#if !defined(P4_SCHEDULER_QUEUE_TEST) || defined(P4_TASK_RETIRE_TEST)
int krnP4TaskRetireLocked(struct Task *task, struct ExecBase *SysBase)
{
    int ready, waiting, running, owner, accepted = 0;
    if (task == NULL || SysBase == NULL)
        return 0;
    EXEC_SPINLOCK_LOCK(&PrivExecBase(SysBase)->TaskReadySpinLock, NULL, SPINLOCK_MODE_WRITE);
    EXEC_SPINLOCK_LOCK(&PrivExecBase(SysBase)->TaskWaitSpinLock, NULL, SPINLOCK_MODE_WRITE);
    EXEC_SPINLOCK_LOCK(&PrivExecBase(SysBase)->TaskRunningSpinLock, NULL, SPINLOCK_MODE_WRITE);
    ready = p4_queue_contains(&SysBase->TaskReady, task);
    waiting = p4_queue_contains(&SysBase->TaskWait, task);
    running = p4_queue_contains(&PrivExecBase(SysBase)->TaskRunning, task);
    owner = krnP4TaskRecordRunningOwner(task, SysBase);
    if (owner >= 0 &&
        ((task->tc_State == TS_READY && ready == 1 && waiting == 0 && running == 0) ||
         (task->tc_State == TS_WAIT && ready == 0 && waiting == 1 && running == 0) ||
         (task->tc_State == TS_RUN && ready == 0 && waiting == 0 && running == 1 && owner > 0)) &&
        krnP4TaskRecordRetireForService(task, SysBase))
    {
        /* Retirement is the last fallible operation. Its service reference
         * and the caller's ordinary pin protect storage after detachment;
         * a RUN owner remains until the executing hart discards/switches. */
        Remove(&task->tc_Node);
        task->tc_State = TS_REMOVED;
        accepted = 1;
    }
    EXEC_SPINLOCK_UNLOCK(&PrivExecBase(SysBase)->TaskRunningSpinLock);
    EXEC_SPINLOCK_UNLOCK(&PrivExecBase(SysBase)->TaskWaitSpinLock);
    EXEC_SPINLOCK_UNLOCK(&PrivExecBase(SysBase)->TaskReadySpinLock);
    return accepted;
}
#endif

/* External retirement has the same Task -> Ready -> Wait -> Running ->
 * registry order, but commits requester ticket and retirement together. */
#if !defined(P4_SCHEDULER_QUEUE_TEST) || defined(P4_TASK_REMOVE_TICKET_TEST)
int krnP4TaskRetireWithTicketLocked(struct Task *task, struct Task *requester,
    uint32_t hart, struct p4_remove_handle *handle, struct ExecBase *SysBase)
{
    int ready, waiting, running, owner, accepted = 0;
    if (task == NULL || requester == NULL || task == requester ||
        SysBase == NULL || handle == NULL || hart > 1u)
        return 0;
    EXEC_SPINLOCK_LOCK(&PrivExecBase(SysBase)->TaskReadySpinLock, NULL, SPINLOCK_MODE_WRITE);
    EXEC_SPINLOCK_LOCK(&PrivExecBase(SysBase)->TaskWaitSpinLock, NULL, SPINLOCK_MODE_WRITE);
    EXEC_SPINLOCK_LOCK(&PrivExecBase(SysBase)->TaskRunningSpinLock, NULL, SPINLOCK_MODE_WRITE);
    ready = p4_queue_contains(&SysBase->TaskReady, task);
    waiting = p4_queue_contains(&SysBase->TaskWait, task);
    running = p4_queue_contains(&PrivExecBase(SysBase)->TaskRunning, task);
    owner = krnP4TaskRecordRunningOwner(task, SysBase);
    if (owner >= 0 &&
        ((task->tc_State == TS_READY && ready == 1 && waiting == 0 && running == 0 && owner == 0) ||
         (task->tc_State == TS_WAIT && ready == 0 && waiting == 1 && running == 0 && owner == 0) ||
         (task->tc_State == TS_RUN && ready == 0 && waiting == 0 && running == 1 && owner > 0)) &&
        krnP4TaskRecordAdmitRemoval(requester, task, hart, 1u, handle, SysBase) == 1)
    {
        /* The ticket and service retirement are already committed together.
         * An ordinary pin and any RUN reference protect the final Task/list
         * accesses. Publication below has no failure/callout path. */
        Remove(&task->tc_Node);
        task->tc_State = TS_REMOVED;
        accepted = 1;
    }
    EXEC_SPINLOCK_UNLOCK(&PrivExecBase(SysBase)->TaskRunningSpinLock);
    EXEC_SPINLOCK_UNLOCK(&PrivExecBase(SysBase)->TaskWaitSpinLock);
    EXEC_SPINLOCK_UNLOCK(&PrivExecBase(SysBase)->TaskReadySpinLock);
    return accepted;
}
#endif

#if !defined(P4_SCHEDULER_QUEUE_TEST) || defined(P4_TASK_PRIORITY_TEST)
int krnP4TaskPriorityLocked(struct Task *task, int8_t priority,
    int8_t *old_priority, uint32_t *poke_mask, struct ExecBase *SysBase)
{
    int ready, waiting, running, owner, accepted = 0;
    uint32_t destinations = 0;
    if (task == NULL || SysBase == NULL || old_priority == NULL || poke_mask == NULL)
        return 0;
    if (task->tc_State == TS_READY)
    {
        unsigned int hart, count = krnP4GetCPUCount();
        if (count == 0 || count > 2 || GetIntETask(task) == NULL)
            return 0;
        for (hart = 0; hart < count; ++hart)
            if (krnP4CPUInMask(hart, GetIntETask(task)->iet_CpuAffinity))
                destinations |= UINT32_C(1) << hart;
        if (destinations == 0)
            return 0;
    }
    EXEC_SPINLOCK_LOCK(&PrivExecBase(SysBase)->TaskReadySpinLock, NULL, SPINLOCK_MODE_WRITE);
    EXEC_SPINLOCK_LOCK(&PrivExecBase(SysBase)->TaskWaitSpinLock, NULL, SPINLOCK_MODE_WRITE);
    EXEC_SPINLOCK_LOCK(&PrivExecBase(SysBase)->TaskRunningSpinLock, NULL, SPINLOCK_MODE_WRITE);
    ready = p4_queue_contains(&SysBase->TaskReady, task);
    waiting = p4_queue_contains(&SysBase->TaskWait, task);
    running = p4_queue_contains(&PrivExecBase(SysBase)->TaskRunning, task);
    owner = krnP4TaskRecordRunningOwner(task, SysBase);
    if (owner >= 0 &&
        ((task->tc_State == TS_READY && ready == 1 && waiting == 0 && running == 0) ||
         (task->tc_State == TS_WAIT && ready == 0 && waiting == 1 && running == 0) ||
         (task->tc_State == TS_RUN && ready == 0 && waiting == 0 && running == 1 && owner > 0)))
    {
        *old_priority = task->tc_Node.ln_Pri;
        task->tc_Node.ln_Pri = priority;
        if (task->tc_State == TS_READY)
        {
            Remove(&task->tc_Node);
            Enqueue(&SysBase->TaskReady, &task->tc_Node);
        }
        else if (task->tc_State == TS_RUN)
            destinations = UINT32_C(1) << (unsigned int)(owner - 1);
        *poke_mask = destinations;
        accepted = 1;
    }
    EXEC_SPINLOCK_UNLOCK(&PrivExecBase(SysBase)->TaskRunningSpinLock);
    EXEC_SPINLOCK_UNLOCK(&PrivExecBase(SysBase)->TaskWaitSpinLock);
    EXEC_SPINLOCK_UNLOCK(&PrivExecBase(SysBase)->TaskReadySpinLock);
    return accepted;
}
#endif

int krnP4SignalQueueApplyLocked(struct Task *task,
    enum p4_signal_queue_transition transition, struct ExecBase *SysBase)
{
    struct List *source, *destination;
    spinlock_t *first, *second;
    unsigned int expected, next;
    int poke = 0;

    if (task == NULL || SysBase == NULL)
        return -1;

    switch (transition)
    {
        case P4_SIGNAL_QUEUE_WAITING_TO_READY:
            source = &SysBase->TaskWait;
            destination = &SysBase->TaskReady;
            first = &PrivExecBase(SysBase)->TaskReadySpinLock;
            second = &PrivExecBase(SysBase)->TaskWaitSpinLock;
            expected = TS_WAIT;
            next = TS_READY;
            break;
        case P4_SIGNAL_QUEUE_RUNNING_TO_READY:
            source = &PrivExecBase(SysBase)->TaskRunning;
            destination = &SysBase->TaskReady;
            first = &PrivExecBase(SysBase)->TaskReadySpinLock;
            second = &PrivExecBase(SysBase)->TaskRunningSpinLock;
            expected = TS_RUN;
            next = TS_READY;
            break;
        case P4_SIGNAL_QUEUE_RUNNING_TO_WAIT:
            source = &PrivExecBase(SysBase)->TaskRunning;
            destination = &SysBase->TaskWait;
            first = &PrivExecBase(SysBase)->TaskWaitSpinLock;
            second = &PrivExecBase(SysBase)->TaskRunningSpinLock;
            expected = TS_RUN;
            next = TS_WAIT;
            break;
        default:
            return -1;
    }

    /* Determine a valid target before any mutation, while the Task lock
     * protects its ETask/affinity. Snapshot only the hart, never Task* for
     * the later unlocked IPI operation. */
    if (next == TS_READY)
    {
        poke = krnP4SignalEligibleHartLocked(task);
        if (poke < 0)
            return -1;
    }

    EXEC_SPINLOCK_LOCK(first, NULL, SPINLOCK_MODE_WRITE);
    EXEC_SPINLOCK_LOCK(second, NULL, SPINLOCK_MODE_WRITE);
    if (task->tc_State != expected ||
        p4_queue_contains(source, task) != 1 ||
        p4_queue_contains(destination, task) != 0)
    {
        EXEC_SPINLOCK_UNLOCK(second);
        EXEC_SPINLOCK_UNLOCK(first);
        return -1;
    }

    Remove(&task->tc_Node);
    task->tc_State = next;
    Enqueue(destination, &task->tc_Node);
    EXEC_SPINLOCK_UNLOCK(second);
    EXEC_SPINLOCK_UNLOCK(first);
    return poke;
}
