/* Exact-ticket completion waiting; no target storage or borrowed bank slot. */
#if !defined(P4_REMOVE_WAIT_TEST)
#include <exec/execbase.h>
#include <proto/exec.h>
#include "exec_intern.h"
#include "p4_signal_adapter.h"
#include "../kernel/p4_task_pins.h"
#endif
#include "p4_remove_wait.h"
#include "p4_self_remove.h"

void Exec_P4AwaitRemoval(struct Task *requester,
    struct p4_remove_handle handle, struct ExecBase *SysBase)
{
    for (;;)
    {
        uint32_t hart;
        int result;

        Disable();
        if (requester == NULL || requester != GET_THIS_TASK ||
            P4_EXEC_CPU_LOCAL(SysBase)->trap_depth != 0u)
            p4_signal_adapter_fail_closed();
        hart = P4_EXEC_RUNTIME(SysBase)->hart_id;

        /* Clear BEFORE the readiness probe, never between a false probe and
         * Wait. An ACK after this clear either makes the exact ticket ready
         * or supplies a wake which Wait consumes. A stale/shared signal may
         * cause another iteration but cannot establish completion. */
        SetSignal(0, SIGF_SINGLE);
        result = krnP4TaskRemovalPoll(requester, hart, handle, SysBase);
        if (result == 2)
            Exec_P4RemoveSelf(requester, SysBase);
        if (result == 1)
        {
            result = krnP4TaskRemovalDrop(requester, hart, handle, SysBase);
            if (result == 0)
                Exec_P4RemoveSelf(requester, SysBase);
            if (result != 1)
                p4_signal_adapter_fail_closed();
            Enable();
            return;
        }
        if (result != 0)
            p4_signal_adapter_fail_closed();
        Enable();
        /* No borrowed CPU-local, target, record or slot pointer crosses this
         * scheduling boundary. Wait handles retirement racing with its arm. */
        Wait(SIGF_SINGLE);
    }
}
