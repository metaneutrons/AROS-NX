/*
    Copyright (c) 2026, The AROS Development Team. All rights reserved.

    Desc: KrnGetCPUCount() for the smp variant of the esp32p4-riscv target.
*/

#include <aros/kernel.h>
#include <aros/libcall.h>
#include <exec/memory.h>
#include <exec/tasks.h>
#include <proto/exec.h>

#include <kernel_base.h>

#include <proto/kernel.h>
#include "tls.h"

/* The harts that have registered: hart 0 from the start, hart 1 once it
   runs in the kernel. */
AROS_LH0(unsigned int, KrnGetCPUCount,
        struct KernelBase *, KernelBase, 40, Kernel)
{
    AROS_LIBFUNC_INIT

    return __builtin_popcountl(__atomic_load_n(&__p4_harts_online,
                                              __ATOMIC_ACQUIRE));

    AROS_LIBFUNC_EXIT
}
