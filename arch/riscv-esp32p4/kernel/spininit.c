/*
    Copyright (c) 2026, The AROS Development Team. All rights reserved.

    Desc: KrnSpinInit() for the smp variant of the esp32p4-riscv target.
*/

#include <aros/types/spinlock_s.h>
#include <aros/kernel.h>
#include <aros/libcall.h>
#include <utility/hooks.h>

#include <kernel_base.h>

#include <proto/kernel.h>

AROS_LH1(void, KrnSpinInit,
        AROS_LHA(spinlock_t *, lock, A0),
        struct KernelBase *, KernelBase, 49, Kernel)
{
    AROS_LIBFUNC_INIT

    lock->s_Owner = NULL;
    __atomic_store_n(&lock->lock, SPINLOCK_UNLOCKED, __ATOMIC_RELEASE);

    AROS_LIBFUNC_EXIT
}
