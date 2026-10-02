/* Selected P4 boot inventory: no task identities escape an unfrozen snapshot.
 * This only runs before hart1 release; ordinary runtime uses the topology gate.
 */
#include "task_intern.h"
#if defined(EXEC_PLATFORM_TASKRESOURCE_SERVICE_CLEANUP)
#include <exec/memory.h>
#include <proto/exec.h>

/* Same bound as the P4 lifetime registry. Bound traversal and retries rather
 * than holding IRQs off while allocating or invoking memory handlers. */
#define TASKRES_BOOT_LIMIT 20000UL
#define TASKRES_BOOT_ATTEMPTS 4U

static BOOL count_list(struct List *list, UBYTE state, ULONG *count)
{
    struct Node *node = list->lh_Head;
    struct Node *previous = (struct Node *)&list->lh_Head;
    struct Node *tail = (struct Node *)&list->lh_Tail;
    if (list->lh_Tail != NULL)
        return FALSE;
    while (node != NULL && node != tail)
    {
        struct Task *task = (struct Task *)node;
        if (*count >= TASKRES_BOOT_LIMIT || node->ln_Pred != previous ||
            node->ln_Succ == NULL || task->tc_State != state)
            return FALSE;
        ++*count;
        previous = node;
        node = node->ln_Succ;
    }
    return node == tail &&
        list->lh_TailPred == previous;
}

static BOOL count_inventory(ULONG *count)
{
    *count = 0;
    return count_list(&PrivExecBase(SysBase)->TaskRunning, TS_RUN, count) &&
        count_list(&PrivExecBase(SysBase)->TaskSpinning, TS_SPIN, count) &&
        count_list(&SysBase->TaskReady, TS_READY, count) &&
        count_list(&SysBase->TaskWait, TS_WAIT, count) && *count != 0;
}

static void take_list(struct List *source, struct List *pool,
    struct TaskResBase *TaskResBase)
{
    struct Task *task;
    ForeachNode(source, task)
    {
        struct TaskListEntry *entry = (struct TaskListEntry *)RemHead(pool);
        /* The complete count was checked under this same IRQ mask. */
        NEWLIST(&entry->tle_HookTypes);
        entry->tle_Task = task;
        AddTail(&TaskResBase->trb_TaskList, &entry->tle_Node);
    }
}

static void free_pool(struct List *pool)
{
    struct TaskListEntry *entry;
    while ((entry = (struct TaskListEntry *)RemHead(pool)) != NULL)
        FreeMem(entry, sizeof(*entry));
}

struct taskres_boot_transaction
{
    struct TaskResBase *resource;
    struct List *pool;
    ULONG capacity;
};

/* Hardware isolation and the lifetime gate cover count, assembly and binding
 * publication together. Refusal/retry precedes every list mutation. */
static int commit_inventory(void *opaque)
{
    struct taskres_boot_transaction *transaction = opaque;
    struct TaskResBase *TaskResBase = transaction->resource;
    struct List *pool = transaction->pool;
    ULONG count;
    if (!count_inventory(&count))
        return 0;
    if (count > transaction->capacity)
        return 2;
    take_list(&PrivExecBase(SysBase)->TaskRunning, pool, TaskResBase);
    take_list(&PrivExecBase(SysBase)->TaskSpinning, pool, TaskResBase);
    take_list(&SysBase->TaskReady, pool, TaskResBase);
    take_list(&SysBase->TaskWait, pool, TaskResBase);
    __atomic_store_n(&SysBase->lb_TaskResBase,
        (struct Library *)TaskResBase, __ATOMIC_RELEASE);
    return 1;
}

BOOL task_BootstrapInventory(struct TaskResBase *TaskResBase)
{
    struct List pool;
    ULONG capacity = 0, count;
    unsigned int attempt;
    NEWLIST(&pool);
    for (attempt = 0; attempt < TASKRES_BOOT_ATTEMPTS; ++attempt)
    {
        BOOL valid;
        Disable();
        valid = EXEC_PLATFORM_TASKRESOURCE_BOOTSTRAP_ISOLATED() &&
            count_inventory(&count);
        Enable();
        if (!valid)
            break;
        /* Allocate only empty entries, outside the snapshot. No Task pointer
         * from the count pass is retained or dereferenced after Enable. */
        while (capacity < count)
        {
            struct TaskListEntry *entry = AllocMem(sizeof(*entry),
                MEMF_PUBLIC | MEMF_CLEAR | MEMF_NO_EXPUNGE);
            if (entry == NULL)
            {
                free_pool(&pool);
                return FALSE;
            }
            AddTail(&pool, &entry->tle_Node);
            ++capacity;
        }
        struct taskres_boot_transaction transaction = {TaskResBase, &pool, capacity};
        int outcome;
        Disable();
        outcome = EXEC_PLATFORM_TASKRESOURCE_BOOTSTRAP_TRANSACTION(
            commit_inventory, &transaction);
        Enable();
        if (outcome != 1)
        {
            if (outcome != 2)
                break;
            continue;
        }
        /* These are unused empty entries, not entries visible to callbacks. */
        free_pool(&pool);
        return TRUE;
    }
    free_pool(&pool);
    return FALSE;
}
#endif
