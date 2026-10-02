/* Drained cleanup consumer. Full topology/completion binding pending. */
#if !defined(P4_TASK_CLEANUP_TEST)
#include <exec/execbase.h>
#include <exec/memory.h>
#include <proto/exec.h>
#include <proto/kernel.h>
#include "etask.h"
#include "exec_util.h"
#include "../kernel/p4_task_pins.h"
#include "../kernel/p4_public_spinlock.h"
#include "p4_task_cleanup.h"
#include "exec_intern.h"
#if defined(EXEC_PLATFORM_TASKRESOURCE_SERVICE_CLEANUP)
#include "../../../rom/exec/taskresource_cleanup.h"
#endif
#endif

int krnP4CleanupOneRetiredTask(struct ExecBase *SysBase)
{
    struct p4_task_registry_record *record;
    struct Task *task;
    struct ETask *et;
    struct MemList *entry;
    struct List owned_entries;

#if defined(__AROSEXEC_SMP__) && defined(__AROSPLATFORM_SMP__)
    /* Completion-only records retain independently allocated storage, not
     * their freed Task. Skip pressure-blocked records without blocking new
     * cleanup; finish at most one here so notification/cancellation runs too. */
    record = krnP4TaskRecordRetryPostFree(SysBase);
    if (record != NULL)
    {
        FreeMem(record, sizeof(*record));
        return 1;
    }
#endif
    /* Retained claim validates retirement, sole service pin, no RUN owner,
     * and durable readiness, keeping the opaque identity indexed until finish.
     * Until success even debug logging must not read Task storage. */
    record = krnP4TaskRecordClaimNextRetainedService(SysBase);
    if (record == NULL)
        return 0;
    task = record->identity;
    if (task == NULL ||
        (task->tc_State != TS_REMOVED && task->tc_State != TS_INVALID &&
         task->tc_State != TS_TOMBSTONED))
    {
        krnP4PublicSpinFatal(P4_PUBLIC_SPIN_BAD_STATE);
        return 0;
    }

    /* The claimed record grants exclusive target ownership, not ownership
     * of parent ETask metadata. CleanupETask still requires the final topology
     * binding before configured SMP runtime is enabled. */
#if defined(EXEC_PLATFORM_TASKRESOURCE_SERVICE_CLEANUP)
    {
        struct taskresource_cleanup_binding *resource =
            (struct taskresource_cleanup_binding *)__atomic_load_n(
                &SysBase->lb_TaskResBase, __ATOMIC_ACQUIRE);
        if (resource != NULL &&
            (resource->contract != TASKRESOURCE_CLEANUP_CONTRACT ||
             resource->cleanup == NULL || !resource->cleanup(task, resource)))
        {
            krnP4PublicSpinFatal(P4_PUBLIC_SPIN_BAD_STATE);
            return 0;
        }
    }
#endif
    et = GetETask(task);
    if (et != NULL && et->et_RegFrame != NULL)
    {
        KrnDeleteContext(et->et_RegFrame);
        et->et_RegFrame = NULL;
    }
    CleanupETask(task);
    task->tc_UnionETask.tc_ETask = NULL;
    task->tc_Flags &= ~TF_ETASK;

    /* Move every intrusive node before freeing any entry. The first entry
     * may own the Task and its embedded list sentinel, so subsequent traversal
     * must use only this housekeeper-owned sentinel and surviving MemLists. */
    NEWLIST(&owned_entries);
    while ((entry = (struct MemList *)RemHead(&task->tc_MemEntry)) != NULL)
        AddTail(&owned_entries, &entry->ml_Node);
    /* Final Task access ended above. Never inspect it after any FreeEntry. */
    while ((entry = (struct MemList *)RemHead(&owned_entries)) != NULL)
        FreeEntry(entry);

    /* Final target accesses ended. With the boot removal bank attached, this
     * binding fuses all target ACKs, durable recipient pins and retained-record
     * detach under the registry gate. Only its separate record remains live
     * here; no Task or entry may be inspected. Public admission and headroom
     * retry ownership still require qualification before SMP activation. */
#if defined(__AROSEXEC_SMP__) && defined(__AROSPLATFORM_SMP__)
    if (krnP4TaskRecordTryFinishPostFree(record, SysBase))
        FreeMem(record, sizeof(*record));
    /* On pressure the separately allocated record remains registry-owned.
     * This call did actual cleanup, so report progress once; retries that
     * cannot finish and find no new work subsequently return0, not busy-loop. */
#else
    krnP4TaskRecordFinishRetainedService(record, SysBase);
    FreeMem(record, sizeof(*record));
#endif
    return 1;
}
