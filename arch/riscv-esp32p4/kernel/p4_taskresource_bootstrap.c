/* Bootstrap snapshot authority, not a runtime substitute for SMP locking. */
#include <asm/cpu.h>
#include <exec/execbase.h>
#include "cpu_local_runtime.h"
#include "hardware.h"
#include "secondary_hw.h"
#include "p4_cpu_masks.h"
#include "p4_taskresource_bootstrap.h"
#include "p4_task_pins.h"

static int bootstrap_hardware_isolated(struct ExecBase *SysBase)
{
    if (SysBase == NULL)
        return 0;
    struct p4_exec_runtime *runtime = p4_exec_runtime_current(SysBase);
    struct p4_exec_runtime *secondary = p4_exec_runtime_for_hart(1);
    if (runtime == NULL || runtime->hart_id != 0 ||
        runtime->stage != P4_EXEC_RUNTIME_STAGE_FINALIZED ||
        runtime->cpu_local == NULL ||
        runtime->cpu_local->current_task == NULL ||
        runtime->cpu_local->id_nest < 0 ||
        (csr_read(mstatus) & MSTATUS_MIE) != 0 ||
        secondary == NULL || secondary->initialized != 1 ||
        secondary->hart_id != 1 ||
        secondary->stage != P4_EXEC_RUNTIME_STAGE_PREPARED ||
        secondary->cpu_local == NULL || secondary->cpu_local->hart_id != 1 ||
        secondary->cpu_local->current_task != NULL ||
        secondary->cpu_local->sys_base != NULL ||
        secondary->cpu_local->kernel_base != NULL ||
        SysBase->lb_TaskResBase != NULL)
        return 0;
    /* With local IRQs masked and hart1 reset/clock-gated, no scheduler,
     * Signal ISR, service owner or secondary launcher can change the queues.
     * Check actual hardware, not CPUCount alone. No hardware is changed. */
    __asm__ volatile("fence iorw,iorw" ::: "memory");
    return (p4_r32(P4_SECONDARY_RESET_REG) & P4_SECONDARY_RESET_BIT) != 0 &&
        (p4_r32(P4_SECONDARY_CLOCK_REG) & P4_SECONDARY_CLOCK_BIT) == 0 &&
        p4_r32(P4_SECONDARY_BOOT_REG) == 0 && krnP4GetCPUCount() == 1;
}

int krnP4TaskResourceBootstrapIsolated(struct ExecBase *SysBase)
{
    return bootstrap_hardware_isolated(SysBase) &&
        krnP4TaskResourceBootstrapQuiescent(SysBase);
}

int krnP4TaskResourceBootstrapTransaction(struct ExecBase *SysBase,
    int (*transaction)(void *), void *context)
{
    if (!bootstrap_hardware_isolated(SysBase))
        return 0;
    return krnP4TaskResourceBootstrapApply(transaction, context, SysBase);
}
