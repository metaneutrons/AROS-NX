/* External removal: all target frees belong to the protected cleanup worker. */
#if !defined(P4_REMOVE_EXTERNAL_TEST)
#include <exec/execbase.h>
#include <proto/exec.h>
#include "exec_intern.h"
#include "p4_signal_adapter.h"
#include "../kernel/p4_task_pins.h"
#include "../kernel/p4_scheduler_queue.h"
#include "../kernel/p4_ipi.h"
#include "../kernel/p4_callback_escape.h"
#endif
#include "p4_remove_external.h"
#include "p4_remove_wait.h"
#include "p4_self_remove.h"

void Exec_P4RemoveExternal(struct Task *target, struct Task *requester,
    struct ExecBase *SysBase)
{
    struct p4_remove_handle handle;
    uint32_t hart;
    int result, owner = 0;

    Disable();
    if (target == NULL || requester == NULL || target == requester ||
        requester != GET_THIS_TASK ||
        P4_EXEC_CPU_LOCAL(SysBase)->trap_depth != 0u)
        p4_signal_adapter_fail_closed();
    hart = P4_EXEC_RUNTIME(SysBase)->hart_id;

    /* A peer may already own retirement/cleanup. Join solely by indexed raw
     * identity, before any target dereference; no second service pin/free.
     * Live-mode admission cannot replace this with a separate reservation. */
    result = krnP4TaskRecordAdmitRemoval(requester, target, hart, 0u,
        &handle, SysBase);
    if (result == 0)
    {
        if (!krnP4SignalTaskPin(target, SysBase))
        {
            /* Retirement may have won after the first join probe. Retry
             * under its leaf gate; never inspect a failed-pin target. A
             * refusal here is not apparent success: generation exhaustion
             * and invalid identities fail closed before any local free. */
            result = krnP4TaskRecordAdmitRemoval(requester, target, hart, 0u,
                &handle, SysBase);
        }
        else
        {
            p4_signal_adapter_lock_disabled(target);
            owner = krnP4TaskRecordRunningOwner(target, SysBase);
            if (owner < 0 || owner > 2)
                p4_signal_adapter_fail_closed();
            result = krnP4TaskRetireWithTicketLocked(target, requester, hart,
                &handle, SysBase);
            if (result == 0)
            {
                /* Peer retirement while acquiring the Task lock: storage
                 * is still pinned, but use only identity for the late join. */
                result = krnP4TaskRecordAdmitRemoval(requester, target, hart,
                    0u, &handle, SysBase);
                owner = 0; /* The winning remover owns the retired-owner poke. */
            }
            p4_signal_adapter_unlock_disabled(target);
            krnP4SignalTaskUnpin(target, SysBase);
            /* No target field, lock, context or list access below this point. */
        }
    }
    if (result != 1 && result != 2)
    {
        /* Admission can refuse because a peer retired this requester. Its
         * RUN still owns this continuation; never panic or return as a live
         * remover when the correct boundary is no-save retirement. */
        if (krnP4SelfRemoveCurrentRetired(SysBase))
            Exec_P4RemoveSelf(requester, SysBase);
        p4_signal_adapter_fail_closed();
    }
    /* All outer locks and the ordinary target pin are gone. Only copied owner
     * and exact handle survive; ticket/service work was latched under gate. */
    if (owner > 0)
        krnP4RetiredOwnerPoke((uint32_t)owner - 1u, SysBase);
    krnP4CleanupPoke(SysBase);
    Enable();
    Exec_P4AwaitRemoval(requester, handle, SysBase);
}
