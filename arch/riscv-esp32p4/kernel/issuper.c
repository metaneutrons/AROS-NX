/*
    Copyright (c) 2026, The AROS Development Team. All rights reserved.

    Desc: Report supervisor state, ESP32-P4 version.
*/

#include <aros/kernel.h>
#include <aros/libcall.h>

#include <kernel_base.h>

#include <proto/kernel.h>

/*
 * See rom/kernel/issuper.c for documentation.
 *
 * Everything runs in machine mode, so the privilege level cannot
 * distinguish task context from kernel context. What callers such as the
 * exec semaphores actually need to know is whether we are inside a trap,
 * where sleeping is impossible; the trap handler maintains the count.
 */
int __esp32p4_trap_depth;

AROS_LH0I(int, KrnIsSuper,
          struct KernelBase *, KernelBase, 13, Kernel)
{
    AROS_LIBFUNC_INIT

    return __esp32p4_trap_depth > 0;

    AROS_LIBFUNC_EXIT
}
