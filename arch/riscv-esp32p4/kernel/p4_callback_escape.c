/* Scoped, owner-hart nonlocal handoff for self-removing Exec callbacks.
 * GCC builtins use compiler-owned register saves, not a libc/Task-stack lease.
 * Every producer/consumer is on the same hart with raw MIE masked. */
#if !defined(P4_CALLBACK_ESCAPE_TEST)
#include <exec/execbase.h>
#include <proto/kernel.h>
#include <asm/cpu.h>
#include "cpu_local_runtime.h"
#include "p4_public_spinlock.h"
#include "p4_task_pins.h"
#endif
#include <stdint.h>
#include "p4_callback_escape.h"

struct p4_callback_escape
{
    intptr_t jump[5];
    struct Task *identity;
};

static struct p4_callback_escape *p4_callback_slots[2];

static struct p4_exec_runtime *callback_runtime(struct ExecBase *SysBase)
{
    struct p4_exec_runtime *runtime = p4_exec_runtime_current(SysBase);
    if (runtime == NULL || runtime->hart_id >= 2 ||
        runtime->cpu_local == NULL ||
        (csr_read(mstatus) & MSTATUS_MIE) != 0 ||
        runtime->cpu_local->current_task == NULL)
        krnP4PublicSpinFatal(P4_PUBLIC_SPIN_BAD_STATE);
    return runtime;
}

int krnP4SelfRemoveContextAllowed(struct ExecBase *SysBase)
{
    struct p4_exec_runtime *runtime = callback_runtime(SysBase);
    struct p4_callback_escape *escape = p4_callback_slots[runtime->hart_id];
    if (runtime->cpu_local->trap_depth == 0)
        return escape == NULL;
    return runtime->cpu_local->trap_depth == 1 && escape != NULL &&
           escape->identity == runtime->cpu_local->current_task;
}

int krnP4SelfRemoveCurrentRetired(struct ExecBase *SysBase)
{
    struct p4_exec_runtime *runtime = callback_runtime(SysBase);
    return krnP4TaskRecordServiceOwnerRunning(
        runtime->cpu_local->current_task, runtime->hart_id, SysBase);
}

int krnP4RunTaskCallback(void (*invoke)(void *), void *argument,
                        struct ExecBase *SysBase)
{
    struct p4_exec_runtime *runtime;
    struct p4_callback_escape escape;
    KrnCli();
    runtime = callback_runtime(SysBase);
    if (invoke == NULL || runtime->cpu_local->trap_depth != 1 ||
        p4_callback_slots[runtime->hart_id] != NULL)
        krnP4PublicSpinFatal(P4_PUBLIC_SPIN_BAD_STATE);
    escape.identity = runtime->cpu_local->current_task;
    /* Unlike libc setjmp, the compiler owns the save area and frame restore.
     * The escaping function is a descendant, never this function itself. */
#if defined(__clang__)
    if (__builtin_setjmp((void **)escape.jump) != 0)
#else
    if (__builtin_setjmp(escape.jump) != 0)
#endif
    {
        KrnCli();
        p4_callback_slots[runtime->hart_id] = NULL;
        return 0;
    }
    p4_callback_slots[runtime->hart_id] = &escape;
    invoke(argument);
    KrnCli();
    if (p4_callback_slots[runtime->hart_id] != &escape)
        krnP4PublicSpinFatal(P4_PUBLIC_SPIN_BAD_STATE);
    p4_callback_slots[runtime->hart_id] = NULL;
    return 1;
}

void krnP4SelfRemoveTransfer(struct ExecBase *SysBase)
{
    struct p4_exec_runtime *runtime = callback_runtime(SysBase);
    struct p4_callback_escape *escape = p4_callback_slots[runtime->hart_id];
    if (!krnP4SelfRemoveContextAllowed(SysBase))
        krnP4PublicSpinFatal(P4_PUBLIC_SPIN_BAD_STATE);
    if (escape != NULL)
    {
#if defined(__clang__)
        __builtin_longjmp((void **)escape->jump, 1);
#else
        __builtin_longjmp(escape->jump, 1);
#endif
    }
    KrnDispatch();
    krnP4PublicSpinFatal(P4_PUBLIC_SPIN_BAD_STATE);
}
