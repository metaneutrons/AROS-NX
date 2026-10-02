/* Self RemTask: retirement and no-save handoff precede every cleanup access. */
#if !defined(P4_SELF_REMOVE_TEST)
#include <exec/execbase.h>
#include <proto/exec.h>
#include <proto/kernel.h>
#include "exec_intern.h"
#include "p4_signal_adapter.h"
#include "../kernel/p4_task_pins.h"
#include "../kernel/p4_scheduler_queue.h"
#include "../kernel/p4_callback_escape.h"
#endif
#include "p4_self_remove.h"

void Exec_P4RemoveSelf(struct Task *task, struct ExecBase *SysBase)
{
    int retired;
    /* RUN owns identity before pin/Task lock. The ordinary pin is also
     * required by retirement admission; no Task field is logged or read here. */
    Disable();
    if (task == NULL || task != GET_THIS_TASK ||
        !krnP4SelfRemoveContextAllowed(SysBase))
        p4_signal_adapter_fail_closed();
    retired = 0;
    if (krnP4SignalTaskPin(task, SysBase))
    {
        p4_signal_adapter_lock_disabled(task);
        retired = krnP4TaskRetireLocked(task, SysBase);
        p4_signal_adapter_unlock_disabled(task);
        krnP4SignalTaskUnpin(task, SysBase);
    }
    /* A remote remover can win before pin or while waiting for the Task lock.
     * RUN still owns current storage, but no new ordinary pin is admitted.
     * Prove service ownership by raw registry identity, never Task fields. */
    if (!retired && !krnP4SelfRemoveCurrentRetired(SysBase))
        p4_signal_adapter_fail_closed();

    /* No Task/context access after final pin drop. Our RUN reference remains
     * until retired-trap/discard clears current_task and releases it. This
     * transfer must replace the return frame or unwind a registered callback;
     * it must not save the old context or issue a nested scheduling ecall. */
    TDNESTCOUNT_SET(-1);
    krnP4SelfRemoveTransfer(SysBase);
    p4_signal_adapter_fail_closed();
}
