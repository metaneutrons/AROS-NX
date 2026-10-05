/*
    Copyright (c) 2026, The AROS Development Team. All rights reserved.

    Desc: KrnSpinUnLock() for the smp variant of the esp32p4-riscv target.
*/

#include <aros/types/spinlock_s.h>
#include <aros/kernel.h>
#include <aros/libcall.h>
#include <utility/hooks.h>

#include <kernel_base.h>

#include <proto/kernel.h>

AROS_LH1(void, KrnSpinUnLock,
        AROS_LHA(spinlock_t *, lock, A0),
        struct KernelBase *, KernelBase, 53, Kernel)
{
    AROS_LIBFUNC_INIT

    /* Only the writer can hold the write bit, so it cannot change under
       us; readers leave one at a time. */
    if (__atomic_load_n(&lock->lock, __ATOMIC_RELAXED) & SPINLOCKF_WRITE)
    {
        lock->s_Owner = NULL;
        __atomic_store_n(&lock->lock, SPINLOCK_UNLOCKED, __ATOMIC_RELEASE);
    }
    else
        __atomic_fetch_sub(&lock->lock, 1, __ATOMIC_RELEASE);

    AROS_LIBFUNC_EXIT
}
