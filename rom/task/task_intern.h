/*
    Copyright (C) 2015-2023, The AROS Development Team. All rights reserved.
*/

#ifndef TASKRES_INTERN_H
#define TASKRES_INTERN_H

#ifndef EXEC_TYPES_H
#include <exec/types.h>
#endif
#ifndef EXEC_NODES_H
#include <exec/nodes.h>
#endif
#ifndef EXEC_LISTS_H
#include <exec/lists.h>
#endif
#ifndef EXEC_SEMAPHORES_H
#include <exec/semaphores.h>
#endif
#ifndef UTILITY_UTILITY_H
#include <utility/utility.h>
#endif

#include <exec_intern.h>
#include <etask.h>
#if defined(EXEC_PLATFORM_TASKRESOURCE_SERVICE_CLEANUP)
#include "../exec/taskresource_cleanup.h"
#endif

#define TASKRES_ENABLE

#if defined(__AROSEXEC_SMP__)
#include <aros/types/spinlock_s.h>
#include <resources/execlock.h>
#ifndef TASKRES_ENABLE
#define TASKRES_ENABLE
#endif
#endif

#define ETASK_RSVD_SLOTID       1

#if defined(EXEC_PLATFORM_TASKRESOURCE_SERVICE_CLEANUP)
#define TASKRES_BASE_FROM_EXEC(exec) ((struct TaskResBase *)__atomic_load_n( \
    &(exec)->lb_TaskResBase, __ATOMIC_ACQUIRE))
#else
#define TASKRES_BASE_FROM_EXEC(exec) ((struct TaskResBase *)(exec)->lb_TaskResBase)
#endif

#if defined(EXEC_PLATFORM_TASKRESOURCE_SERVICE_CLEANUP)
struct TaskResBase;
BOOL task_RegisterNewTask(struct Task *, APTR);
BOOL task_BootstrapInventory(struct TaskResBase *);
#endif

/* Puddle size, in slots. Must be at least 1 */
#define TASKSTORAGEPUDDLE       16

struct TaskResBase
{
#if defined(EXEC_PLATFORM_TASKRESOURCE_SERVICE_CLEANUP)
    struct taskresource_cleanup_binding trb_CleanupBinding;
#else
    struct Library              trb_LibNode;
#endif
    APTR                        trb_KernelBase;
    APTR                        trb_NewAddTask;
    APTR                        trb_RemTask;
    APTR                        trb_NewStackSwap;
    struct SignalSemaphore      trb_Sem;
#if defined(__AROSEXEC_SMP__)
    spinlock_t                  TaskListSpinLock;
    void *                      trb_ExecLock;
#endif
    struct MinList              trb_TaskStorageSlots;               /* List of free slots, always one element with next slot        */
    struct List                 trb_TaskList;
    struct List                 trb_NewTasks;
    struct List                 trb_LockedLists;
    struct Library *            trb_UtilityBase;
};

struct TaskListEntry
{
    struct Node                 tle_Node;
    struct Task                 *tle_Task;
    struct List                 tle_HookTypes;
};

struct TaskListHookEntry
{
    struct Node                 tlhe_Node;
    struct MinList              tlhe_Hooks;
};

struct TaskListHookNode
{
    /* TaskResource owns the linkage, never the caller's Hook node. */
    struct MinNode              tln_Node;
    struct Hook                 *tln_Hook;
};

#ifdef TASKRES_ENABLE
struct TaskListPrivate
{
    struct Node                 tlp_Node;
    ULONG                       tlp_Flags;
    struct List                 *tlp_Tasks;
    struct TaskListEntry        *tlp_Next;
};

#define __TS_FIRSTSLOT 0

struct TaskStorageFreeSlot
{
    struct MinNode _node;
    LONG FreeSlot;
};

IPTR TaskGetStorageSlot(struct Task * t, LONG id);
#else
struct TaskListPrivate
{
    struct List                 *tlp_TaskList;
    struct Task                *tlp_Current;
};
#endif /* TASKRES_ENABLE */

#ifdef KernelBase
#undef KernelBase
#endif

#define KernelBase TaskResBase->trb_KernelBase

void task_CleanList(struct Task * task, struct TaskResBase *TaskResBase);
/* Detach by opaque identity; never dereference the Task or call RemTask.
 * Both TaskResource lists and the lock-holder decision share one gate.
 * Detached storage is freed only after leaving that gate. */
void task_DetachEntry(struct Task *task, struct TaskResBase *TaskResBase);
#if defined(EXEC_PLATFORM_TASKRESOURCE_SERVICE_CLEANUP)
BOOL task_CleanupRemovedTask(struct Task *, APTR);
#endif
struct TaskListHookEntry *GetHookTypeEntry(struct List *htList, ULONG thType, BOOL create);
struct TaskListEntry *GetTaskEntry(struct Task *thisTask, struct TaskResBase *TaskResBase);

#endif /* TASKRES_INTERN_H */
