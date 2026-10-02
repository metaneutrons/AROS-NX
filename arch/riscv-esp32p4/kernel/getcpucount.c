/*
    Copyright (c) 2026, The AROS Development Team. All rights reserved.

    Desc: Report the currently published ESP32-P4 online hart count.
*/

#include <aros/kernel.h>
#include <aros/libcall.h>

#include <kernel_base.h>

#include "p4_cpu_masks.h"

AROS_LH0(unsigned int, KrnGetCPUCount,
         struct KernelBase *, KernelBase, 40, Kernel)
{
    AROS_LIBFUNC_INIT

    (void)KernelBase;
    return krnP4GetCPUCount();

    AROS_LIBFUNC_EXIT
}
