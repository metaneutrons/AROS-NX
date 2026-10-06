/*
    Copyright (C) 2017-2026, The AROS Development Team. All rights reserved.
*/

#define DEBUG 0

#include <aros/debug.h>

#include <proto/exec.h>

#include "task_intern.h"

void task_DetachEntry(struct Task *task, struct TaskResBase *TaskResBase)
{
    struct TaskListEntry *entry, *next, *detached = NULL;
    BOOL found = FALSE;

    if (!task)
        return;

#if !defined(__AROSEXEC_SMP__)
    Forbid();
#else
    Disable();
    EXEC_SPINLOCK_LOCK(&TaskResBase->TaskListSpinLock, NULL, SPINLOCK_MODE_WRITE);
#endif
    /* NewTasks has no published iterator; reclaim after removing its node. */
    ForeachNodeSafe(&TaskResBase->trb_NewTasks, entry, next)
    {
        if (entry->tle_Task == task)
        {
            Remove(&entry->tle_Node);
            detached = entry;
            found = TRUE;
            break;
        }
    }
    if (!found)
    {
        ForeachNodeSafe(&TaskResBase->trb_TaskList, entry, next)
        {
            if (entry->tle_Task == task)
            {
                /* Active iterators retain the node, not a live Task. */
                if (IsListEmpty(&TaskResBase->trb_LockedLists))
                {
                    Remove(&entry->tle_Node);
                    detached = entry;
                }
                else
                    entry->tle_Task = NULL;
                break;
            }
        }
    }
#if !defined(__AROSEXEC_SMP__)
    Permit();
#else
    EXEC_SPINLOCK_UNLOCK(&TaskResBase->TaskListSpinLock);
    Enable();
#endif
    /* No allocator/Task/ETask access while holding the topology gate. */
    if (detached)
        FreeMem(detached, sizeof(*detached));
}


void task_CleanList(struct Task * task, struct TaskResBase *TaskResBase)
{
    struct TaskListEntry *taskEntry, *tetmp;
    struct List detached;
    NEWLIST(&detached);

#if !defined(__AROSEXEC_SMP__)
    Forbid();
#else
    Disable();
    EXEC_SPINLOCK_LOCK(&TaskResBase->TaskListSpinLock, NULL, SPINLOCK_MODE_WRITE);
#endif
    /* Are there any lock holders?, if not, do housecleaning */
    if (IsListEmpty(&TaskResBase->trb_LockedLists))
    {
        ForeachNodeSafe(&TaskResBase->trb_TaskList, taskEntry, tetmp)
        {
            if ((!taskEntry->tle_Task) ||
                ((task) && (task == taskEntry->tle_Task)))
            {
                D(bug("[TaskRes] RemTask: destroying old taskentry @ 0x%p\n", taskEntry));
                Remove(&taskEntry->tle_Node);
                AddTail(&detached, &taskEntry->tle_Node);
            }
        }
        ForeachNodeSafe(&TaskResBase->trb_NewTasks, taskEntry, tetmp)
        {
            Remove(&taskEntry->tle_Node);
            AddTail(&TaskResBase->trb_TaskList, &taskEntry->tle_Node);
        }

    }
#if !defined(__AROSEXEC_SMP__)
    Permit();
#else
    EXEC_SPINLOCK_UNLOCK(&TaskResBase->TaskListSpinLock);
    Enable();
#endif
    ForeachNodeSafe(&detached, taskEntry, tetmp)
    {
        Remove(&taskEntry->tle_Node);
        FreeMem(taskEntry, sizeof(*taskEntry));
    }
}

struct TaskListEntry *GetTaskEntry(struct Task *task, struct TaskResBase *TaskResBase)
{
    struct TaskListEntry *currNode, *retval = NULL;

#if !defined(__AROSEXEC_SMP__)
        /* Don't let any other task interfere with us at the moment */
        Forbid();
#else
        Disable();
        EXEC_SPINLOCK_LOCK(&TaskResBase->TaskListSpinLock, NULL, SPINLOCK_MODE_WRITE);
#endif

    ForeachNode(&TaskResBase->trb_TaskList, currNode)
    {
        if (currNode->tle_Task == task)
        {
            retval = currNode;
            break;
        }
    }

#if !defined(__AROSEXEC_SMP__)
        Permit();
#else
        EXEC_SPINLOCK_UNLOCK(&TaskResBase->TaskListSpinLock);
        Enable();
#endif

    return retval;
}

/* task hook support */
struct TaskListHookEntry *GetHookTypeEntry(struct List *htList, ULONG thType, BOOL create)
{
    struct TaskListHookEntry *currEntry;

    D(bug("%s(0x%p)\n", __func__, htList));

    ForeachNode(htList, currEntry)
    {
        if (currEntry->tlhe_Node.ln_Type == thType)
        {
            D(bug("Hook Type Entry Found @ 0x%p\n", currEntry));
            return currEntry;
        }
    }

    D(bug("Hook Type %d Not Found\n", thType));

    if (create)
    {
        currEntry = AllocMem(sizeof(struct TaskListHookEntry), MEMF_ANY|MEMF_CLEAR);
        if (currEntry)
        {
            D(bug("New Hook Type Entry @ 0x%p for type %d\n", currEntry, thType));

            currEntry->tlhe_Node.ln_Type = thType;
            NEWLIST(&currEntry->tlhe_Hooks);
            AddTail(htList, &currEntry->tlhe_Node);

            return currEntry;
        }
    }

    return NULL;
}

VOID TaskHookTypeDispose(struct TaskListHookEntry *task, ULONG type)
{
    (void)task;
    (void)type;
    D(bug("%s(0x%p)\n", __func__));
}

VOID TaskHooksDispose(struct TaskListHookEntry *task)
{
    (void)task;
    D(bug("%s(0x%p)\n", __func__));
}

/*
 * Deliver a task lifecycle notification to every registered hook.
 * Called from the NewAddTask()/RemTask() wrappers, never under Forbid().
 */
void taskres_NotifyTasks(struct TaskResBase *TaskResBase, ULONG action, struct Task *task, struct Task *parent, struct TagItem *tags)
{
    struct TaskNotifyMsg msg;
    struct Hook *hook;

    msg.tnm_Action = action;
    msg.tnm_Task = task;
    msg.tnm_Parent = parent;
    msg.tnm_Tags = tags;

    ObtainSemaphoreShared(&TaskResBase->trb_NotifySem);
    ForeachNode(&TaskResBase->trb_NotifyHooks, hook)
    {
        CALLHOOKPKT(hook, task, &msg);
    }
    ReleaseSemaphore(&TaskResBase->trb_NotifySem);
}
