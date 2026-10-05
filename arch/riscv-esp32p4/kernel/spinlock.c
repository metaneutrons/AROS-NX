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

#ifdef P4_SPIN_WATCHDOG
#include <exec/tasks.h>
#include "kernel_intern.h"

/*
 * Diagnostic: a lock that does not come free within about a second is
 * reported once, through waiting output, with its owner, the waiter, the
 * caller and the stack words around the waiter. The wait then goes on.
 */
#define P4_SPIN_LIMIT   50000000u

static int p4_spin_reported;

static void p4_put_name(struct Task *t)
{
    unsigned long a = (unsigned long)t;
    const char *n;
    int i;

    if (!((a >= 0x48000000ul && a < 0x4a000000ul) ||
          (a >= 0x4ff00000ul && a < 0x4ffc0000ul)))
        return;
    n = t->tc_Node.ln_Name;
    a = (unsigned long)n;
    if (!((a >= 0x48000000ul && a < 0x4a000000ul) ||
          (a >= 0x4ff00000ul && a < 0x4ffc0000ul) ||
          (a >= 0x40000000ul && a < 0x44000000ul)))
        return;
    krnP4PutStr(" '");
    for (i = 0; i < 24 && n[i]; i++)
    {
        char c[2] = { (n[i] >= 32 && n[i] < 127) ? n[i] : '?', 0 };
        krnP4PutStr(c);
    }
    krnP4PutStr("'");
}

void krnP4SpinReport(const char *what, void *lockp, void *caller)
{
    spinlock_t *lock = lockp;
    unsigned long sp, status, hart;
    unsigned int i;

    if (p4_spin_reported)
        return;
    p4_spin_reported = 1;

    asm volatile("mv %0, sp" : "=r"(sp));
    asm volatile("csrr %0, mstatus" : "=r"(status));
    asm volatile("csrr %0, mhartid" : "=r"(hart));
    krnP4ConsoleBlocking();
    krnP4PutStr("\n[spin] STUCK ");
    krnP4PutStr(what);
    krnP4PutStr(" lock="); krnP4PutHex32((uint32_t)(unsigned long)lock);
    if (lock)
    {
        krnP4PutStr(" value="); krnP4PutHex32(lock->lock);
        krnP4PutStr(" owner="); krnP4PutHex32((uint32_t)(unsigned long)lock->s_Owner);
        p4_put_name(lock->s_Owner);
    }
    krnP4PutStr(" me="); krnP4PutHex32((uint32_t)(unsigned long)__p4_tls[hart & 1].ThisTask);
    p4_put_name(__p4_tls[hart & 1].ThisTask);
    krnP4PutStr("\n[spin] caller="); krnP4PutHex32((uint32_t)(unsigned long)caller);
    krnP4PutStr(" mstatus="); krnP4PutHex32((uint32_t)status);
    krnP4PutStr(" hart="); krnP4PutHex32((uint32_t)hart);
    krnP4PutStr(" idnest="); krnP4PutHex32((uint32_t)(int)__p4_tls[hart & 1].IDNestCnt);
    krnP4PutStr(" tdnest="); krnP4PutHex32((uint32_t)(int)__p4_tls[hart & 1].TDNestCnt);
    krnP4PutStr(" sp="); krnP4PutHex32((uint32_t)sp);
    for (i = 0; i < 64; i++)
    {
        if (!(i & 7))
            krnP4PutStr("\n[spin] stack");
        krnP4PutStr(" ");
        krnP4PutHex32(((uint32_t *)sp)[i]);
    }
    krnP4PutStr("\n");
}
#define P4_SPIN_CHECK(n) \
    do { if (++(n) == P4_SPIN_LIMIT) \
        krnP4SpinReport(mode == SPINLOCK_MODE_WRITE ? "write" : "read", \
                        lock, __builtin_return_address(0)); } while (0)
#else
#define P4_SPIN_CHECK(n) do { } while (0)
#endif

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
#ifdef P4_SPIN_WATCHDOG
    unsigned int spins = 0;
#endif

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
                P4_SPIN_CHECK(spins);
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
            P4_SPIN_CHECK(spins);
        }
    }
    return lock;

    AROS_LIBFUNC_EXIT
}
