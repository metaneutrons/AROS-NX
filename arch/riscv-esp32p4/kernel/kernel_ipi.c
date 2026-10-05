/*
    Copyright (c) 2026, The AROS Development Team. All rights reserved.

    Desc: inter-hart calls for the smp variant of the esp32p4-riscv target.
*/

#include <exec/types.h>

#include "kernel_ipi.h"

/*
 * Exec only asks for a call on another hart with EXECF_CPUAffinity set,
 * which the platform does not set before S4 (SMP.md): until then hart 1
 * runs only its own idle task and nothing signals across harts. Nothing
 * was delivered, which is what 0 reports. The inter-hart interrupt itself
 * exists since S3 (kernel_smp.c), for the cache-off park.
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
