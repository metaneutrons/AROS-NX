/* No-save handoff for a removed task; never reuse ordinary Switch callbacks. */
#if !defined(P4_DISCARD_TEST)
#include <exec/execbase.h>
#include <exec/tasks.h>
#include <proto/exec.h>
#include <asm/cpu.h>
#include "exec_intern.h"
#include "cpu_local_runtime.h"
#include "p4_task_pins.h"
#include "p4_public_spinlock.h"
#include "p4_scheduler_queue.h"
#include "p4_discard.h"
#endif

void krnP4DiscardCurrent(struct ExecBase *SysBase)
{
    struct p4_exec_runtime *runtime = p4_exec_runtime_current(SysBase);
    struct Task *task;
    if (runtime == NULL || runtime->hart_id >= 2 ||
        runtime->cpu_local == NULL || runtime->cpu_local->trap_depth != 1 ||
        (csr_read(mstatus) & MSTATUS_MIE) != 0 ||
        runtime->cpu_local->current_task == NULL)
        krnP4PublicSpinFatal(P4_PUBLIC_SPIN_BAD_STATE);
    task = runtime->cpu_local->current_task;
    /* RUN reference remains live throughout the final Task unlock. Neither
     * ETask nor the old register/FPU image is inspected or restored here. */
    EXEC_SPINLOCK_LOCK(&task->tc_SpinLock, NULL, SPINLOCK_MODE_WRITE);
    if ((task->tc_State != TS_REMOVED && task->tc_State != TS_INVALID &&
         task->tc_State != TS_TOMBSTONED) ||
        (runtime->wait_pending != NULL && runtime->wait_pending != task) ||
        !krnP4TaskDetachedLocked(task, SysBase) ||
        !krnP4TaskRecordServiceOwnerRunning(task, runtime->hart_id, SysBase))
        krnP4PublicSpinFatal(P4_PUBLIC_SPIN_BAD_STATE);
    runtime->wait_pending = NULL;
    if (!p4_exec_runtime_set_current_task(SysBase, NULL))
        krnP4PublicSpinFatal(P4_PUBLIC_SPIN_BAD_STATE);
    runtime->cpu_local->fpu_owner = NULL;
    EXEC_SPINLOCK_UNLOCK(&task->tc_SpinLock);
    /* Raw identity only; no Task storage access after final ownership drop. */
    krnP4TaskRecordLeaveRunning(task, runtime->hart_id, SysBase);
}
