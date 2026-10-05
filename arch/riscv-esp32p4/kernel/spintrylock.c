/*
    Copyright (c) 2026, The AROS Development Team. All rights reserved.

    Desc: KrnSpinTryLock() for the smp variant of the esp32p4-riscv target.
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

AROS_LH2(spinlock_t *, KrnSpinTryLock,
        AROS_LHA(spinlock_t *, lock, A0),
        AROS_LHA(ULONG, mode, D0),
        struct KernelBase *, KernelBase, 51, Kernel)
{
    AROS_LIBFUNC_INIT

    unsigned int value;

    if (mode == SPINLOCK_MODE_WRITE)
    {
        value = SPINLOCK_UNLOCKED;
        if (!__atomic_compare_exchange_n(&lock->lock, &value, SPINLOCKF_WRITE,
                                         0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
            return NULL;
        lock->s_Owner = TLS_GET(ThisTask);
        return lock;
    }

    value = __atomic_load_n(&lock->lock, __ATOMIC_RELAXED);
    do
    {
        if (value & SPINLOCKF_WRITE)
            return NULL;
    } while (!__atomic_compare_exchange_n(&lock->lock, &value, value + 1,
                                          0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED));
    return lock;

    AROS_LIBFUNC_EXIT
}
