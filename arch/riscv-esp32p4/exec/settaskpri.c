/* P4 matched-SMP priority writer; full runtime activation remains gated. */
#if !defined(P4_TASK_PRIORITY_WRAPPER_TEST)
#include <exec/execbase.h>
#include <aros/libcall.h>
#include <proto/exec.h>
#include "exec_intern.h"
#include "p4_signal_adapter.h"
#include "../kernel/p4_task_pins.h"
#include "../kernel/p4_scheduler_queue.h"
#endif

AROS_LH2(BYTE, SetTaskPri,
    AROS_LHA(struct Task *, task, A1),
    AROS_LHA(LONG, priority, D0),
    struct ExecBase *, SysBase, 50, Exec)
{
    AROS_LIBFUNC_INIT
    int8_t old_priority;
    uint32_t destinations;
    unsigned int hart;
    struct p4_signal_effect effect = {0};

    if (task == NULL)
        task = GET_THIS_TASK;
    /* Pin's nested Enable must not discard this caller while it owns a
     * foreign Task reference. Keep the whole reference/wake interval masked. */
    Disable();
    /* No state/name/ETask access before lifetime acquisition. */
    if (task == NULL || !krnP4SignalTaskPin(task, SysBase))
        p4_signal_adapter_fail_closed();
    p4_signal_adapter_lock_disabled(task);
    if (task->tc_State == TS_REMOVED || task->tc_State == TS_INVALID ||
        task->tc_State == TS_TOMBSTONED)
    {
        /* Retirement may win after a successful ordinary pin. Its cleanup
         * remains blocked by our pin; return a locked snapshot, no mutation. */
        old_priority = task->tc_Node.ln_Pri;
        destinations = 0;
    }
    else if (!krnP4TaskPriorityLocked(task, (int8_t)priority, &old_priority,
                               &destinations, SysBase))
    {
        p4_signal_adapter_unlock_disabled(task);
        krnP4SignalTaskUnpin(task, SysBase);
        p4_signal_adapter_fail_closed();
    }
    p4_signal_adapter_unlock_disabled(task);
    krnP4SignalTaskUnpin(task, SysBase); /* last Task identity use */
    /* Publish the asynchronous request before an Enable can schedule or
     * discard this caller. No lock/pin or Task access crosses the poke. */
    for (hart = 0; hart < 2; ++hart)
    {
        if ((destinations & (UINT32_C(1) << hart)) == 0)
            continue;
        effect.status = P4_SIGNAL_STATUS_OK;
        effect.request_reschedule = 1;
        effect.reschedule_hart = hart;
        krnP4SignalPoke(&effect, SysBase);
    }
    Enable();
    return (BYTE)old_priority;
    AROS_LIBFUNC_EXIT
}
