/* Forced owner drain precedes generic Forbid/priority scheduling decisions. */
#if !defined(P4_RETIRED_TRAP_TEST)
#include <exec/execbase.h>
#include <exec/tasks.h>
#include <asm/cpu.h>
#include "exec_intern.h"
#include "cpu_local_runtime.h"
#include "p4_public_spinlock.h"
#include "p4_discard.h"
#include "kernel_cpu.h"
#include <kernel_intr.h>
#include "p4_retired_trap.h"
#endif

int krnP4RetiredTrap(struct ExceptionContext *ctx, struct ExecBase *SysBase)
{
    struct p4_exec_runtime *runtime = p4_exec_runtime_current(SysBase);
    struct Task *task;
    int retired;

    if (ctx == NULL || runtime == NULL || runtime->hart_id >= 2 ||
        runtime->cpu_local == NULL || runtime->cpu_local->trap_depth != 1 ||
        (csr_read(mstatus) & MSTATUS_MIE) != 0)
        krnP4PublicSpinFatal(P4_PUBLIC_SPIN_BAD_STATE);
    task = runtime->cpu_local->current_task;
    if (task == NULL)
        return 0;
    /* The current hart's RUN reference protects identity through both Task
     * locks. A remote remover cannot release this hart's reference. */
    EXEC_SPINLOCK_LOCK(&task->tc_SpinLock, NULL, SPINLOCK_MODE_WRITE);
    retired = task->tc_State == TS_REMOVED || task->tc_State == TS_INVALID ||
              task->tc_State == TS_TOMBSTONED;
    EXEC_SPINLOCK_UNLOCK(&task->tc_SpinLock);
    if (!retired)
        return 0;
    /* Discard proves detachment/service ownership and releases RUN without
     * reading the old context. Always replace the trap return frame afterwards;
     * neither the retired syscall nor generic scheduling may run again. */
    krnP4DiscardCurrent(SysBase);
    cpu_Dispatch(ctx);
    return 1;
}
