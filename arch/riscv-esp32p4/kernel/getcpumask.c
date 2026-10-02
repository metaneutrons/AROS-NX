/*
    Copyright (c) 2026, The AROS Development Team. All rights reserved.

    Desc: Set an ESP32-P4 one-word CPU affinity mask.
*/

#include <aros/kernel.h>
#include <aros/libcall.h>

#include <kernel_base.h>

#include "p4_cpu_masks.h"

AROS_LH2(void, KrnGetCPUMask,
         AROS_LHA(uint32_t, number, D0),
         AROS_LHA(void *, mask, A0),
         struct KernelBase *, KernelBase, 45, Kernel)
{
    AROS_LIBFUNC_INIT

    (void)KernelBase;
    krnP4GetCPUMask(number, mask);

    AROS_LIBFUNC_EXIT
}
