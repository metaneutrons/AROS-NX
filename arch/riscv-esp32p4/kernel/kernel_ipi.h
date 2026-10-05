#ifndef KERNEL_IPI_H_
#define KERNEL_IPI_H_
/*
    Copyright (c) 2026, The AROS Development Team. All rights reserved.

    Desc: inter-hart calls for the smp variant of the esp32p4-riscv target.
*/

#include <utility/hooks.h>

/*
 * The interface rom/exec/signal.c needs. Its cross-hart path only runs
 * with EXECF_CPUAffinity set, which the platform sets once the second
 * hart takes part (S3); until then there is one hart and nothing to call.
 * This is the plain variant without KERNEL_IPI_CALL_CANCELABLE.
 */

#define IPI_CALL_HOOK_MAX_ARGS  5

struct IPIHook
{
    struct Hook ih_Hook;
    IPTR        ih_Args[IPI_CALL_HOOK_MAX_ARGS];
};

extern int core_DoCallIPI(struct Hook *hook, void *cpu_mask, int async,
                          int nargs, IPTR *args, APTR _KB);

#endif /* KERNEL_IPI_H_ */
