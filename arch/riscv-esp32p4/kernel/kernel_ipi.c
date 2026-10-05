/*
    Copyright (c) 2026, The AROS Development Team. All rights reserved.

    Desc: inter-hart calls for the smp variant of the esp32p4-riscv target.
*/

#include <exec/types.h>

#include "kernel_ipi.h"

/*
 * One hart takes part so far: hart 1 stays in reset, KrnGetCPUCount()
 * reports one CPU and EXECF_CPUAffinity is not set, so exec never asks
 * for a call on another hart. Nothing was delivered, which is what 0
 * reports. The CLIC software interrupts qualified in E2 replace this when
 * hart 1 comes online (SMP.md, S3).
 */
int core_DoCallIPI(struct Hook *hook, void *cpu_mask, int async,
                   int nargs, IPTR *args, APTR _KB)
{
    (void)hook;
    (void)cpu_mask;
    (void)async;
    (void)nargs;
    (void)args;
    (void)_KB;

    return 0;
}
