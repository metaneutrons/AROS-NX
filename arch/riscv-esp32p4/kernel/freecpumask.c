/*
    Copyright (c) 2026, The AROS Development Team. All rights reserved.

    Desc: KrnFreeCPUMask() for the smp variant of the esp32p4-riscv target.
*/

#include <aros/kernel.h>
#include <aros/libcall.h>
#include <exec/memory.h>
#include <exec/tasks.h>
#include <proto/exec.h>

#include <kernel_base.h>

#include <proto/kernel.h>

/* A CPU mask is one 32-bit word, bit n for hart n; TASKAFFINITY_ANY is a
   sentinel, never a buffer. */

AROS_LH1(void *, KrnFreeCPUMask,
        AROS_LHA(void *, mask, A0),
        struct KernelBase *, KernelBase, 43, Kernel)
{
    AROS_LIBFUNC_INIT

    if (mask && (IPTR)mask != TASKAFFINITY_ANY)
        FreeMem(mask, sizeof(uint32_t));
    return NULL;

    AROS_LIBFUNC_EXIT
}
