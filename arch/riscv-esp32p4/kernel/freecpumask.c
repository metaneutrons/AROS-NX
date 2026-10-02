/*
    Copyright (c) 2026, The AROS Development Team. All rights reserved.

    Desc: Free an ESP32-P4 one-word CPU affinity mask.
*/

#include <aros/kernel.h>
#include <aros/libcall.h>

#include <kernel_base.h>

#include "p4_cpu_masks.h"

AROS_LH1(void *, KrnFreeCPUMask,
         AROS_LHA(void *, mask, A0),
         struct KernelBase *, KernelBase, 43, Kernel)
{
    AROS_LIBFUNC_INIT

    (void)KernelBase;
    krnP4FreeCPUMask(mask);
    return NULL;

    AROS_LIBFUNC_EXIT
}
