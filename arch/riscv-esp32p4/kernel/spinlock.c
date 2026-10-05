/*
    Copyright (c) 2026, The AROS Development Team. All rights reserved.

    Desc: KrnSpinLock() for the smp variant of the esp32p4-riscv target.
*/

#include <aros/types/spinlock_s.h>
#include <aros/kernel.h>
#include <aros/libcall.h>
#include <utility/hooks.h>

#include <kernel_base.h>

#include <proto/kernel.h>
#include "tls.h"

/*
 * The lock word follows <aros/types/spinlock_s.h>: SPINLOCKF_WRITE for a
 * writer, otherwise the number of readers. The operations are the word
 * AMOs and LR/SC loops GCC emits for these builtins, qualified on cached
 * SRAM (E2) and PSRAM (S1) across both harts. There is no wait-for-event
 * instruction to park in, so waiters spin on plain loads, which leave the
 * line shared until the lock is released.
 */

AROS_LH3(spinlock_t *, KrnSpinLock,
        AROS_LHA(spinlock_t *, lock, A1),
        AROS_LHA(struct Hook *, failhook, A0),
        AROS_LHA(ULONG, mode, D0),
        struct KernelBase *, KernelBase, 52, Kernel)
{
    AROS_LIBFUNC_INIT

    unsigned int value;

    /* Like the arm ports, this never calls failhook: waiters spin. */
    (void)failhook;

    if (mode == SPINLOCK_MODE_WRITE)
    {
        for (;;)
        {
            value = SPINLOCK_UNLOCKED;
            if (__atomic_compare_exchange_n(&lock->lock, &value,
                                            SPINLOCKF_WRITE, 1,
                                            __ATOMIC_ACQUIRE,
                                            __ATOMIC_RELAXED))
                break;
            while (__atomic_load_n(&lock->lock, __ATOMIC_RELAXED)
                   != SPINLOCK_UNLOCKED)
                ;
        }
        /* Without an owner a leaked lock is just a set bit. */
        lock->s_Owner = TLS_GET(ThisTask);
    }
    else
    {
        for (;;)
        {
            value = __atomic_load_n(&lock->lock, __ATOMIC_RELAXED);
            if (!(value & SPINLOCKF_WRITE) &&
                __atomic_compare_exchange_n(&lock->lock, &value, value + 1,
                                            1, __ATOMIC_ACQUIRE,
                                            __ATOMIC_RELAXED))
                break;
        }
    }
    return lock;

    AROS_LIBFUNC_EXIT
}
