/* Task creation/bootstrap hooks; record allocation is outside the gate. */
#if !defined(P4_TASK_PUBLISH_TEST)
#include <exec/memory.h>
#include <exec/execbase.h>
#include <proto/exec.h>
#include "exec_intern.h"
#include "cpu_local_runtime.h"
#include "p4_task_pins.h"
#include "p4_scheduler_queue.h"
#include "p4_public_spinlock.h"
#include "p4_task_publish.h"
#include "p4_cpu_masks.h"
#include "../exec/p4_signal_adapter.h"
#endif

int krnP4TaskRecordCreate(struct Task *task, struct ExecBase *SysBase)
{
    struct p4_task_registry_record *record;
    if (task == NULL || SysBase == NULL)
        return 0;
    record = AllocMem(sizeof(*record), MEMF_PUBLIC | MEMF_CLEAR);
    if (record == NULL)
        return 0;
    if (!krnP4TaskRecordPublishPinned(task, record, SysBase))
    {
        FreeMem(record, sizeof(*record));
        return 0;
    }
    return 1;
}

int krnP4NewTaskReady(struct Task *task, struct ExecBase *SysBase)
{
    int destination;
    struct p4_signal_effect effect = {0};
    if (task == NULL || p4_exec_runtime_current(SysBase) == NULL)
        return 0;
    Disable();
    EXEC_SPINLOCK_LOCK(&task->tc_SpinLock, NULL, SPINLOCK_MODE_WRITE);
    destination = krnP4NewTaskReadyLocked(task, SysBase);
    EXEC_SPINLOCK_UNLOCK(&task->tc_SpinLock);
    if (destination > 0)
    {
        /* Publish the asynchronous wake before Enable can discard or switch
         * this caller. Only the copied destination crosses the Task unlock. */
        effect.status = P4_SIGNAL_STATUS_OK;
        effect.request_reschedule = 1;
        effect.reschedule_hart = (uint32_t)destination - 1;
        krnP4SignalPoke(&effect, SysBase);
    }
    Enable();
    return destination;
}

int krnP4BootTaskRegister(struct Task *task, struct ExecBase *SysBase)
{
    struct p4_exec_runtime *runtime = p4_exec_runtime_current(SysBase);
    int registered;
    if (runtime == NULL || runtime->hart_id != 0 || runtime->cpu_local == NULL ||
        runtime->cpu_local->current_task != NULL ||
        runtime->cpu_local->id_nest < 0 || runtime->cpu_local->td_nest < 0 ||
        krnP4GetCPUCount() != 1 ||
        !krnP4TaskRecordCreate(task, SysBase))
        return 0;
    Disable();
    EXEC_SPINLOCK_LOCK(&task->tc_SpinLock, NULL, SPINLOCK_MODE_WRITE);
    registered = krnP4BootTaskRunningLocked(task, SysBase);
    if (!registered || !p4_exec_runtime_set_current_task(SysBase, task))
        krnP4PublicSpinFatal(P4_PUBLIC_SPIN_BAD_STATE);
    EXEC_SPINLOCK_UNLOCK(&task->tc_SpinLock);
    krnP4SignalTaskUnpin(task, SysBase); /* unique RUN owner now protects it */
    Enable();
    return 1;
}
