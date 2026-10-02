/*
 * Save before queue publication, retain RUN ownership through the unlocked
 * switch hook, then clear local identity and release by raw identity only.
 * RemTask/ServiceTask must defer destruction until this owner has drained.
 */
#if !defined(P4_SWITCH_TEST)
#include <exec/execbase.h>
#include <exec/tasks.h>
#include <proto/exec.h>
#include <aros/riscv/cpucontext.h>
#include <asm/cpu.h>
#include "exec_intern.h"
#include "etask.h"
#include "cpu_local_runtime.h"
#include "p4_task_pins.h"
#include "p4_discard.h"
#include "p4_public_spinlock.h"
#include "p4_trap_frame.h"
#include "../exec/p4_signal_adapter.h"
#include "p4_switch.h"
#include "p4_callback_escape.h"
#endif

struct p4_switch_callback_args
{
    VOID_FUNC hook;
    struct ExecBase *sys_base;
};

static void switch_callback_invoke(void *opaque)
{
    struct p4_switch_callback_args *args = opaque;
    AROS_UFC1NR(void, args->hook,
                AROS_UFCA(struct ExecBase *, args->sys_base, A6));
}

void krnP4Switch(struct ExceptionContext *regs, struct ExecBase *SysBase)
{
    struct p4_exec_runtime *runtime = p4_exec_runtime_current(SysBase);
    struct Task *task;
    struct ExceptionContext *saved;
    struct p4_signal_effect effect;
    VOID_FUNC switch_hook = NULL;
    unsigned int i;

    if (regs == NULL || runtime == NULL || runtime->hart_id >= 2 ||
        runtime->cpu_local == NULL || runtime->cpu_local->trap_depth != 1 ||
        (csr_read(mstatus) & MSTATUS_MIE) != 0 ||
        runtime->cpu_local->current_task == NULL)
        krnP4PublicSpinFatal(P4_PUBLIC_SPIN_BAD_STATE);
    task = runtime->cpu_local->current_task;
    /* The unique RUN reference protects the identity before locking it. */
    EXEC_SPINLOCK_LOCK(&task->tc_SpinLock, NULL, SPINLOCK_MODE_WRITE);
    if (task->tc_State == TS_REMOVED || task->tc_State == TS_INVALID ||
        task->tc_State == TS_TOMBSTONED)
    {
        /* Remote retirement wins before any context/ETask/FPU-image read.
         * Retain RUN through this unlock; discard revalidates detached queues
         * and the executing hart's service-owned reference under Task lock. */
        EXEC_SPINLOCK_UNLOCK(&task->tc_SpinLock);
        krnP4DiscardCurrent(SysBase);
        return;
    }
    if (task->tc_State != TS_RUN || GetETask(task) == NULL ||
        GetIntETask(task)->iet_CpuNumber != runtime->hart_id ||
        (saved = GetETask(task)->et_RegFrame) == NULL ||
        (uintptr_t)regs->sp <= (uintptr_t)task->tc_SPLower ||
        (uintptr_t)regs->sp > (uintptr_t)task->tc_SPUpper ||
        ((uintptr_t)regs->sp & 15u) != 0 || regs->pc == 0 ||
        (regs->pc & 1u) != 0 || runtime->cpu_local->id_nest < -1 ||
        runtime->cpu_local->id_nest > 127)
        krnP4PublicSpinFatal(P4_PUBLIC_SPIN_BAD_STATE);

    if ((saved->Flags & ECF_FPU) != 0 &&
        (saved->fpuContext == NULL ||
         ((uintptr_t)saved->fpuContext & 3u) != 0))
        krnP4PublicSpinFatal(P4_PUBLIC_SPIN_BAD_STATE);

    if (p4_trap_fpu_state_is_dirty(regs->sr) &&
        (saved->fpuContext == NULL || regs->fpuContext == NULL ||
         ((uintptr_t)saved->fpuContext & 3u) != 0 ||
         ((uintptr_t)regs->fpuContext & 3u) != 0))
        krnP4PublicSpinFatal(P4_PUBLIC_SPIN_BAD_STATE);
    for (i = 0; i < RISCV_REGSAVE_CNT; ++i)
        saved->x[i] = regs->x[i];
    saved->pc = regs->pc;
    saved->sr = regs->sr;
    task->tc_SPReg = (APTR)regs->sp;
    task->tc_IDNestCnt = runtime->cpu_local->id_nest;
    if (p4_trap_fpu_state_is_dirty(saved->sr))
    {
        p4_trap_fpu_copy_entry_spill(saved->fpuContext, regs->fpuContext);
        saved->Flags |= ECF_FPU;
        saved->sr = (saved->sr & ~MSTATUS_FS) | MSTATUS_FS_CLEAN;
    }

    effect = p4_signal_adapter_switch_commit_locked(task, SysBase);
    if (effect.status != P4_SIGNAL_STATUS_OK ||
        !p4_signal_adapter_apply_queue_locked(task, SysBase, &effect))
        krnP4PublicSpinFatal(P4_PUBLIC_SPIN_BAD_STATE);
    if (task->tc_State == TS_READY && (task->tc_Flags & TF_SWITCH))
    {
        switch_hook = task->tc_Switch;
        if (switch_hook == NULL)
            krnP4PublicSpinFatal(P4_PUBLIC_SPIN_BAD_STATE);
    }
    EXEC_SPINLOCK_UNLOCK(&task->tc_SpinLock);
    if (switch_hook != NULL)
    {
        /* Coherent READY membership, but admission still refuses because
         * the old RUN owner is retained. No Task/list/gate lock crosses C. */
        struct p4_switch_callback_args args = {switch_hook, SysBase};
        /* A self-removing callback abandons its continuation here. The
         * final Task unlock/RUN release below still belongs to this switch. */
        (void)krnP4RunTaskCallback(switch_callback_invoke, &args, SysBase);
    }

    EXEC_SPINLOCK_LOCK(&task->tc_SpinLock, NULL, SPINLOCK_MODE_WRITE);
    if (runtime->cpu_local->current_task != task ||
        runtime->wait_pending != NULL ||
        !p4_exec_runtime_set_current_task(SysBase, NULL))
        krnP4PublicSpinFatal(P4_PUBLIC_SPIN_BAD_STATE);
    runtime->cpu_local->fpu_owner = NULL;
    /* Never re-enqueue here: callback-time retirement must win. */
    EXEC_SPINLOCK_UNLOCK(&task->tc_SpinLock);
    /* Still protected through unlock. From here only the raw identity goes
     * into the leaf registry; even retired storage can now be reclaimed. */
    krnP4TaskRecordLeaveRunning(task, runtime->hart_id, SysBase);
    p4_signal_adapter_poke_after_unlock(&effect, SysBase);
}
