/*
 * Private ESP32-P4 packed read/write spinlock state machine.
 *
 * Every public operation validates placement before reading or writing the
 * lock. Each acquire/release attempt uses one LR and at most one SC. There is
 * no allocator, library, Exec, or Hook call in this file's SRAM entry points.
 */
#include <stddef.h>
#include <stdint.h>

#include "p4_spinlock.h"

#define P4_SPINLOCK_SRAM_BASE  ((uintptr_t)0x4ff00000u)
#define P4_SPINLOCK_SRAM_LIMIT ((uintptr_t)0x4ffc0000u)

#define P4_SPINLOCK_READ_MASK  0x00ffffffu
#define P4_SPINLOCK_WRITE_BIT  SPINLOCKF_WRITE
#define P4_SPINLOCK_UPDATING   SPINLOCKF_UPDATING
#define P4_SPINLOCK_VALID_MASK (P4_SPINLOCK_READ_MASK | P4_SPINLOCK_WRITE_BIT)

#if defined(P4_SPINLOCK_TEST)
#ifndef P4_SPINLOCK_CODE
#define P4_SPINLOCK_CODE
#endif
#else
#if !defined(__riscv)
#error "p4_spinlock.c is ESP32-P4 RISC-V code; host builds must use its test harness"
#endif
#define P4_SPINLOCK_CODE \
    __attribute__((section(".sramtext"), noinline, used, optimize("Os")))
#endif

#if !defined(P4_SPINLOCK_LR)
static inline __attribute__((always_inline)) uint32_t
p4_spinlock_lr_acquire(volatile uint32_t *word)
{
    uint32_t value;

    __asm__ volatile("lr.w.aq %0,(%1)"
                     : "=r"(value) : "r"(word) : "memory");
    return value;
}
#define P4_SPINLOCK_LR(word) p4_spinlock_lr_acquire(word)
#endif

#if !defined(P4_SPINLOCK_LOAD)
#define P4_SPINLOCK_LOAD(word) \
    __atomic_load_n((const uint32_t *)(word), __ATOMIC_RELAXED)
#endif

#if !defined(P4_SPINLOCK_CAS_ONCE)
static inline __attribute__((always_inline)) int
p4_spinlock_compare_exchange_once(volatile uint32_t *word,
                                  uint32_t expected, uint32_t desired)
{
    uint32_t observed, failed;

    /*
     * Keep the LR/SC constrained sequence in one asm block. The snapshot and
     * state computation happen before LR, so there is no backward branch,
     * load, store, or call between LR and SC. A changed snapshot is BUSY.
     */
    __asm__ volatile("lr.w.aq %0,(%2)\n"
                     "bne %0,%3,1f\n"
                     "sc.w.rl %1,%4,(%2)\n"
                     "j 2f\n"
                     "1: li %1,1\n"
                     "2:"
                     : "=&r"(observed), "=&r"(failed)
                     : "r"(word), "r"(expected), "r"(desired)
                     : "memory");
    return observed == expected && failed == 0;
}
#define P4_SPINLOCK_CAS_ONCE(word, expected, desired) \
    p4_spinlock_compare_exchange_once((word), (expected), (desired))
#endif

#if !defined(P4_SPINLOCK_ADDRESS_VALID)
#define P4_SPINLOCK_ADDRESS_VALID(lock, policy) \
    krnP4SpinAddressAllowed((lock), (policy))
#endif

_Static_assert(offsetof(spinlock_t, lock) == 0,
               "the atomic word must lead spinlock_t");
_Static_assert(sizeof(((spinlock_t *)0)->lock) == sizeof(uint32_t),
               "P4 spinlocks require a 32-bit public lock word");
#if defined(__riscv) && defined(__riscv_xlen) && (__riscv_xlen == 32)
_Static_assert(offsetof(spinlock_t, s_Owner) == sizeof(uint32_t),
               "RV32 public owner field must immediately follow the lock word");
#endif

static inline __attribute__((always_inline)) int
p4_spinlock_policy_valid(const struct p4_spinlock_policy *policy)
{
    uint32_t i, j;

    if (policy == NULL || policy->region_count == 0 ||
        policy->region_count > P4_SPINLOCK_MAX_REGIONS)
        return 0;

    for (i = 0; i < policy->region_count; ++i)
    {
        const uintptr_t begin = policy->regions[i].begin;
        const uintptr_t end = policy->regions[i].end;

        if ((begin & 3u) != 0 || (end & 3u) != 0 || begin >= end ||
            begin < P4_SPINLOCK_SRAM_BASE || end > P4_SPINLOCK_SRAM_LIMIT)
            return 0;

        for (j = 0; j < i; ++j)
        {
            const uintptr_t other_begin = policy->regions[j].begin;
            const uintptr_t other_end = policy->regions[j].end;

            if (begin < other_end && other_begin < end)
                return 0;
        }
    }

    return 1;
}

static inline __attribute__((always_inline)) int
p4_spinlock_state_valid(uint32_t value)
{
    if ((value & ~P4_SPINLOCK_VALID_MASK) != 0)
        return 0;
    if ((value & P4_SPINLOCK_WRITE_BIT) != 0 &&
        (value & P4_SPINLOCK_READ_MASK) != 0)
        return 0;
    return 1;
}

P4_SPINLOCK_CODE int krnP4SpinAddressAllowed(
    const spinlock_t *lock, const struct p4_spinlock_policy *policy)
{
    uintptr_t address, limit;
    uint32_t i;

    if (lock == NULL || !p4_spinlock_policy_valid(policy))
        return 0;

    address = (uintptr_t)lock;
    if ((address & 3u) != 0 || address > UINTPTR_MAX - sizeof(*lock))
        return 0;
    limit = address + sizeof(*lock);

    for (i = 0; i < policy->region_count; ++i)
    {
        if (address >= policy->regions[i].begin &&
            limit <= policy->regions[i].end)
            return 1;
    }

    return 0;
}

P4_SPINLOCK_CODE enum p4_spinlock_result krnP4SpinInit(
    spinlock_t *lock, const struct p4_spinlock_policy *policy)
{
    if (lock == NULL || policy == NULL)
        return P4_SPIN_BAD_ARGUMENT;
    if (!P4_SPINLOCK_ADDRESS_VALID(lock, policy))
        return P4_SPIN_BAD_ADDRESS;

    /* The caller guarantees quiescence; publication is its responsibility. */
    lock->lock = SPINLOCK_UNLOCKED;
    lock->s_Owner = NULL;
    return P4_SPIN_OK;
}

P4_SPINLOCK_CODE enum p4_spinlock_result krnP4SpinTryAcquire(
    spinlock_t *lock, uint32_t mode,
    const struct p4_spinlock_policy *policy)
{
    volatile uint32_t *word;
    uint32_t old_value, new_value;

    if (mode != SPINLOCK_MODE_READ && mode != SPINLOCK_MODE_WRITE)
        return P4_SPIN_BAD_MODE;
    if (lock == NULL || policy == NULL)
        return P4_SPIN_BAD_ARGUMENT;
    if (!P4_SPINLOCK_ADDRESS_VALID(lock, policy))
        return P4_SPIN_BAD_ADDRESS;

    word = (volatile uint32_t *)&lock->lock;
    old_value = P4_SPINLOCK_LOAD(word);
    if (!p4_spinlock_state_valid(old_value))
        return P4_SPIN_BAD_STATE;

    if (mode == SPINLOCK_MODE_WRITE)
    {
        if (old_value != SPINLOCK_UNLOCKED)
            return P4_SPIN_BUSY;
        new_value = P4_SPINLOCK_WRITE_BIT;
    }
    else
    {
        uint32_t readers;

        if ((old_value & P4_SPINLOCK_WRITE_BIT) != 0)
            return P4_SPIN_BUSY;
        readers = old_value & P4_SPINLOCK_READ_MASK;
        if (readers == P4_SPINLOCK_READ_MASK)
            return P4_SPIN_READ_OVERFLOW;
        new_value = old_value + 1u;
    }

    if (!P4_SPINLOCK_CAS_ONCE(word, old_value, new_value))
        return P4_SPIN_BUSY;
    return P4_SPIN_OK;
}

P4_SPINLOCK_CODE enum p4_spinlock_result krnP4SpinUnlock(
    spinlock_t *lock, uint32_t mode,
    const struct p4_spinlock_policy *policy)
{
    volatile uint32_t *word;
    uint32_t old_value, new_value;

    if (mode != SPINLOCK_MODE_READ && mode != SPINLOCK_MODE_WRITE)
        return P4_SPIN_BAD_MODE;
    if (lock == NULL || policy == NULL)
        return P4_SPIN_BAD_ARGUMENT;
    if (!P4_SPINLOCK_ADDRESS_VALID(lock, policy))
        return P4_SPIN_BAD_ADDRESS;

    word = (volatile uint32_t *)&lock->lock;
    old_value = P4_SPINLOCK_LOAD(word);
    if (!p4_spinlock_state_valid(old_value))
        return P4_SPIN_BAD_STATE;

    if (mode == SPINLOCK_MODE_WRITE)
    {
        if (old_value != P4_SPINLOCK_WRITE_BIT)
            return P4_SPIN_BAD_STATE;
        new_value = SPINLOCK_UNLOCKED;
    }
    else
    {
        uint32_t readers = old_value & P4_SPINLOCK_READ_MASK;

        if ((old_value & P4_SPINLOCK_WRITE_BIT) != 0 || readers == 0)
            return P4_SPIN_BAD_STATE;
        new_value = old_value - 1u;
    }

    if (!P4_SPINLOCK_CAS_ONCE(word, old_value, new_value))
        return P4_SPIN_BUSY;
    return P4_SPIN_OK;
}

P4_SPINLOCK_CODE enum p4_spinlock_result krnP4SpinIsLocked(
    const spinlock_t *lock, const struct p4_spinlock_policy *policy,
    int *held)
{
    const volatile uint32_t *word;
    uint32_t value;

    if (held == NULL)
        return P4_SPIN_BAD_ARGUMENT;
    if (lock == NULL || policy == NULL)
        return P4_SPIN_BAD_ARGUMENT;
    if (!P4_SPINLOCK_ADDRESS_VALID(lock, policy))
        return P4_SPIN_BAD_ADDRESS;

    word = (const volatile uint32_t *)&lock->lock;
    value = P4_SPINLOCK_LR((volatile uint32_t *)word);
    *held = value != SPINLOCK_UNLOCKED;
    return P4_SPIN_OK;
}
