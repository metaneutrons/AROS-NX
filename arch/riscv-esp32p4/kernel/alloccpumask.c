/*
    Copyright (c) 2026, The AROS Development Team. All rights reserved.

    Desc: Allocate an ESP32-P4 one-word CPU affinity mask.
*/

#include <aros/kernel.h>
#include <aros/libcall.h>

#include <kernel_base.h>

#include "p4_cpu_masks.h"

AROS_LH0(void *, KrnAllocCPUMask,
         struct KernelBase *, KernelBase, 42, Kernel)
{
    AROS_LIBFUNC_INIT

    (void)KernelBase;
    return krnP4AllocCPUMask();

    AROS_LIBFUNC_EXIT
}
