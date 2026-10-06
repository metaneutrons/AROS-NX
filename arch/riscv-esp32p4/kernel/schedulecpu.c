/*
    Copyright (c) 2026, The AROS Development Team. All rights reserved.

    Desc: KrnScheduleCPU() for the smp variant of the esp32p4-riscv target.
*/

#include <aros/kernel.h>
#include <aros/libcall.h>
#include <exec/tasks.h>

#include <kernel_base.h>
#include <kernel_syscall.h>
#include <exec_platform.h>

#include <proto/kernel.h>

#include "kernel_intern.h"
#include "kernel_cpu.h"
#include "tls.h"

/*
 * Ask the harts in the mask to look for something to run. Another hart
 * gets an inter-hart interrupt; this one reschedules now, or, inside a
 * trap, on its way out of it.
 */
AROS_LH1(void, KrnScheduleCPU,
        AROS_LHA(void *, cpu_mask, A0),
        struct KernelBase *, KernelBase, 47, Kernel)
{
    AROS_LIBFUNC_INIT

    unsigned int me = GetCPUNumber();
    ULONG mask, online;
    unsigned int cpu;

    if (!cpu_mask)
        return;
    if ((IPTR)cpu_mask == TASKAFFINITY_ANY)
        mask = (1UL << P4_TLS_HARTS) - 1;
    else
        mask = *(ULONG *)cpu_mask;

    online = __atomic_load_n(&__p4_harts_online, __ATOMIC_ACQUIRE);
    for (cpu = 0; cpu < P4_TLS_HARTS; cpu++)
    {
        if (cpu != me && (mask & online & (1UL << cpu)))
            krnP4IPISend(cpu, P4_IPI_SCHEDULE);
    }

    if (mask & (1UL << me))
    {
        if (TLS_GET(TrapDepth) > 0)
        {
            FLAG_SCHEDQUANTUM_SET;
            FLAG_SCHEDSWITCH_SET;
        }
        else
            krnSysCall(SC_SCHEDULE);
    }

    AROS_LIBFUNC_EXIT
}
