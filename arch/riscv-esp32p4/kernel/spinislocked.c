/*
    Copyright (c) 2026, The AROS Development Team. All rights reserved.

    Desc: KrnSpinIsLocked() for the smp variant of the esp32p4-riscv target.
*/

#include <aros/types/spinlock_s.h>
#include <aros/kernel.h>
#include <aros/libcall.h>
#include <utility/hooks.h>

#include <kernel_base.h>

#include <proto/kernel.h>

AROS_LH1(int, KrnSpinIsLocked,
        AROS_LHA(spinlock_t *, lock, A0),
        struct KernelBase *, KernelBase, 50, Kernel)
{
    AROS_LIBFUNC_INIT

    return __atomic_load_n(&lock->lock, __ATOMIC_ACQUIRE) != SPINLOCK_UNLOCKED;

    AROS_LIBFUNC_EXIT
}
