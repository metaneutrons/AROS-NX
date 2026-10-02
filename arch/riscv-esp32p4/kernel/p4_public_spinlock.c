/*
 * Bounded implementation helpers for the P4 public KrnSpin* ABI.
 *
 * The software-atomic service is the only path to the public lock word and
 * owner slot. In particular, this file never emits an AMO/LR/SC against a
 * caller's lock, which may live in PSRAM.
 */
#include <stddef.h>
#include <stdint.h>

#include "p4_public_spinlock.h"
#include "p4_soft_atomic.h"
#include "cpu_local.h"

#if !defined(P4_PUBLIC_SPINLOCK_TEST) && !defined(__riscv)
#error "p4_public_spinlock.c is P4/RV32 code; use its host fixture for tests"
#endif

_Static_assert(offsetof(spinlock_t, lock) == 0,
               "the RV32 public lock word must be first");
_Static_assert(offsetof(spinlock_t, s_Owner) == sizeof(uint32_t),
               "the RV32 owner word must be at byte offset four");
_Static_assert(sizeof(((spinlock_t *)0)->lock) == sizeof(uint32_t),
               "the P4 public lock word must be 32 bits");
#if !defined(P4_PUBLIC_SPINLOCK_TEST)
_Static_assert(sizeof(void *) == sizeof(uint32_t),
               "P4 public spinlocks require the RV32 pointer ABI");
#endif

#define P4_PUBLIC_READ_MASK  UINT32_C(0x00ffffff)
#define P4_PUBLIC_WRITE      UINT32_C(0x08000000)
#define P4_PUBLIC_UPDATING   UINT32_C(0x80000000)
#define P4_PUBLIC_VALID_MASK \
    (P4_PUBLIC_READ_MASK | P4_PUBLIC_WRITE | P4_PUBLIC_UPDATING)
#define P4_PUBLIC_TRANSITION (P4_PUBLIC_WRITE | P4_PUBLIC_UPDATING)

#ifndef P4_PUBLIC_OWNER_TO_WORD
#define P4_PUBLIC_OWNER_TO_WORD(owner) ((uint32_t)(uintptr_t)(owner))
#endif

extern struct ExecBase *SysBase;

enum p4_public_atomic_result
{
    P4_PUBLIC_ATOMIC_OK = 0,
    P4_PUBLIC_ATOMIC_TIMEOUT,
    P4_PUBLIC_ATOMIC_BAD_ADDRESS,
    P4_PUBLIC_ATOMIC_NOT_PREPARED,
    P4_PUBLIC_ATOMIC_HARD_ERROR
};

static enum p4_public_atomic_result
p4_public_map_atomic(enum p4_soft_atomic_result result)
{
    switch (result)
    {
        case P4_SOFT_ATOMIC_OK:
            return P4_PUBLIC_ATOMIC_OK;
        case P4_SOFT_ATOMIC_TIMEOUT:
            return P4_PUBLIC_ATOMIC_TIMEOUT;
        case P4_SOFT_ATOMIC_BAD_ADDRESS:
        case P4_SOFT_ATOMIC_BAD_ALIGNMENT:
            return P4_PUBLIC_ATOMIC_BAD_ADDRESS;
        case P4_SOFT_ATOMIC_NOT_PREPARED:
            return P4_PUBLIC_ATOMIC_NOT_PREPARED;
        default:
            return P4_PUBLIC_ATOMIC_HARD_ERROR;
    }
}

static enum p4_public_atomic_result
p4_public_load(const volatile void *address, uint32_t *old_value)
{
    *old_value = 0;
    return p4_public_map_atomic(krnP4SoftAtomicOperate(
        (volatile void *)address, sizeof(uint32_t), P4_SOFT_ATOMIC_LOAD,
        0u, old_value));
}

static enum p4_public_atomic_result
p4_public_store(volatile void *address, uint32_t value, uint32_t *old_value)
{
    *old_value = 0;
    return p4_public_map_atomic(krnP4SoftAtomicOperate(
        address, sizeof(uint32_t), P4_SOFT_ATOMIC_STORE, value, old_value));
}

static enum p4_public_atomic_result
p4_public_compare_exchange(volatile void *address, uint32_t expected,
                           uint32_t desired, uint32_t *old_value)
{
    *old_value = 0;
    return p4_public_map_atomic(krnP4SoftAtomicCompareExchange(
        address, sizeof(uint32_t), expected, desired, old_value));
}

static volatile void *p4_public_lock_word(spinlock_t *lock)
{
    return (volatile void *)&lock->lock;
}

static volatile void *p4_public_owner_word(spinlock_t *lock)
{
    return (volatile void *)((uintptr_t)lock +
                             offsetof(spinlock_t, s_Owner));
}

static int p4_public_lock_argument_valid(const spinlock_t *lock)
{
    uintptr_t address = (uintptr_t)lock;

    return lock != NULL && (address & 3u) == 0 &&
           address <= UINTPTR_MAX - sizeof(*lock);
}

static int p4_public_state_valid(uint32_t state)
{
    uint32_t readers = state & P4_PUBLIC_READ_MASK;

    if ((state & ~P4_PUBLIC_VALID_MASK) != 0)
        return 0;
    if ((state & P4_PUBLIC_WRITE) != 0 && readers != 0)
        return 0;
    if ((state & P4_PUBLIC_UPDATING) != 0 &&
        (state & P4_PUBLIC_WRITE) == 0)
        return 0;
    return 1;
}

static enum p4_public_spinlock_result
p4_public_public_result(enum p4_public_atomic_result result)
{
    switch (result)
    {
        case P4_PUBLIC_ATOMIC_BAD_ADDRESS:
            return P4_PUBLIC_SPIN_BAD_ADDRESS;
        case P4_PUBLIC_ATOMIC_NOT_PREPARED:
            return P4_PUBLIC_SPIN_NOT_PREPARED;
        case P4_PUBLIC_ATOMIC_HARD_ERROR:
            return P4_PUBLIC_SPIN_SERVICE_ERROR;
        default:
            return P4_PUBLIC_SPIN_SERVICE_ERROR;
    }
}

static uint32_t p4_public_current_owner_word(void)
{
    struct p4_cpu_local *local;
    uint32_t hart_id;

    if (SysBase == NULL)
        return 0;

    hart_id = P4_CPU_LOCAL_READ_MHARTID();
    if (hart_id >= P4_CPU_LOCAL_HART_COUNT)
        return 0;

    local = p4_cpu_local_current();
    if (local == NULL || local != p4_cpu_local_for_hart(hart_id) ||
        local->hart_id != hart_id || local->sys_base != SysBase ||
        local->kernel_base == NULL || local->trap_depth != 0 ||
        local->current_task == NULL)
        return 0;

    return P4_PUBLIC_OWNER_TO_WORD(local->current_task);
}

static enum p4_public_atomic_result
p4_public_store_owner_retry(spinlock_t *lock)
{
    uint32_t old_value;
    uint32_t attempt;
    uint32_t owner = p4_public_current_owner_word();

    for (attempt = 0; attempt < P4_PUBLIC_SPINLOCK_RETRIES; ++attempt)
    {
        enum p4_public_atomic_result result = p4_public_store(
            p4_public_owner_word(lock), owner, &old_value);

        if (result != P4_PUBLIC_ATOMIC_TIMEOUT)
            return result;
    }

    return P4_PUBLIC_ATOMIC_TIMEOUT;
}

enum p4_public_spinlock_result krnP4PublicSpinInit(spinlock_t *lock)
{
    uint32_t old_value;
    uint32_t attempt;
    enum p4_public_atomic_result result;

    if (lock == NULL)
        return P4_PUBLIC_SPIN_BAD_ARGUMENT;
    if (!p4_public_lock_argument_valid(lock))
        return P4_PUBLIC_SPIN_BAD_ADDRESS;

    /* Publish an empty owner before the unlocked state, under caller quiescence. */
    result = P4_PUBLIC_ATOMIC_TIMEOUT;
    for (attempt = 0; attempt < P4_PUBLIC_SPINLOCK_RETRIES; ++attempt)
    {
        result = p4_public_store(p4_public_owner_word(lock), 0u, &old_value);
        if (result != P4_PUBLIC_ATOMIC_TIMEOUT)
            break;
    }
    if (result != P4_PUBLIC_ATOMIC_OK)
        return result == P4_PUBLIC_ATOMIC_TIMEOUT ?
            P4_PUBLIC_SPIN_SERVICE_TIMEOUT : p4_public_public_result(result);

    result = P4_PUBLIC_ATOMIC_TIMEOUT;
    for (attempt = 0; attempt < P4_PUBLIC_SPINLOCK_RETRIES; ++attempt)
    {
        result = p4_public_store(p4_public_lock_word(lock), 0u, &old_value);
        if (result != P4_PUBLIC_ATOMIC_TIMEOUT)
            break;
    }
    if (result != P4_PUBLIC_ATOMIC_OK)
        return result == P4_PUBLIC_ATOMIC_TIMEOUT ?
            P4_PUBLIC_SPIN_SERVICE_TIMEOUT : p4_public_public_result(result);

    return P4_PUBLIC_SPIN_OK;
}

enum p4_public_spinlock_result krnP4PublicSpinTryAcquire(
    spinlock_t *lock, uint32_t mode)
{
    enum p4_public_atomic_result atomic_result;
    uint32_t old_value;

    if (mode != SPINLOCK_MODE_READ && mode != SPINLOCK_MODE_WRITE)
        return P4_PUBLIC_SPIN_BAD_MODE;
    if (lock == NULL)
        return P4_PUBLIC_SPIN_BAD_ARGUMENT;
    if (!p4_public_lock_argument_valid(lock))
        return P4_PUBLIC_SPIN_BAD_ADDRESS;

    if (mode == SPINLOCK_MODE_READ)
    {
        uint32_t readers;
        uint32_t desired;

        atomic_result = p4_public_load(p4_public_lock_word(lock), &old_value);
        if (atomic_result == P4_PUBLIC_ATOMIC_TIMEOUT)
            return P4_PUBLIC_SPIN_TRY_TIMEOUT;
        if (atomic_result != P4_PUBLIC_ATOMIC_OK)
            return p4_public_public_result(atomic_result);
        if (!p4_public_state_valid(old_value))
            return P4_PUBLIC_SPIN_BAD_STATE;
        if ((old_value & (P4_PUBLIC_WRITE | P4_PUBLIC_UPDATING)) != 0)
            return P4_PUBLIC_SPIN_BUSY;

        readers = old_value & P4_PUBLIC_READ_MASK;
        if (readers == P4_PUBLIC_READ_MASK)
            return P4_PUBLIC_SPIN_READ_OVERFLOW;
        desired = old_value + 1u;

        atomic_result = p4_public_compare_exchange(
            p4_public_lock_word(lock), old_value, desired, &old_value);
        if (atomic_result == P4_PUBLIC_ATOMIC_TIMEOUT)
            return P4_PUBLIC_SPIN_TRY_TIMEOUT;
        if (atomic_result != P4_PUBLIC_ATOMIC_OK)
            return p4_public_public_result(atomic_result);
        if (old_value == desired - 1u)
            return P4_PUBLIC_SPIN_OK;
        return p4_public_state_valid(old_value) ? P4_PUBLIC_SPIN_BUSY :
            P4_PUBLIC_SPIN_BAD_STATE;
    }

    atomic_result = p4_public_compare_exchange(
        p4_public_lock_word(lock), 0u, P4_PUBLIC_TRANSITION, &old_value);
    if (atomic_result == P4_PUBLIC_ATOMIC_TIMEOUT)
        return P4_PUBLIC_SPIN_TRY_TIMEOUT;
    if (atomic_result != P4_PUBLIC_ATOMIC_OK)
        return p4_public_public_result(atomic_result);
    if (old_value != 0u)
    {
        if (!p4_public_state_valid(old_value))
            return P4_PUBLIC_SPIN_BAD_STATE;
        return P4_PUBLIC_SPIN_BUSY;
    }

    /* From this point on every failure is fatal to the public wrapper. */
    atomic_result = p4_public_store_owner_retry(lock);
    if (atomic_result != P4_PUBLIC_ATOMIC_OK)
        return P4_PUBLIC_SPIN_CLAIMED_FAILURE;

    atomic_result = P4_PUBLIC_ATOMIC_TIMEOUT;
    {
        uint32_t attempt;

        for (attempt = 0; attempt < P4_PUBLIC_SPINLOCK_RETRIES; ++attempt)
        {
            atomic_result = p4_public_compare_exchange(
                p4_public_lock_word(lock), P4_PUBLIC_TRANSITION,
                P4_PUBLIC_WRITE, &old_value);
            if (atomic_result == P4_PUBLIC_ATOMIC_TIMEOUT)
                continue;
            break;
        }
    }
    if (atomic_result != P4_PUBLIC_ATOMIC_OK ||
        old_value != P4_PUBLIC_TRANSITION)
        return P4_PUBLIC_SPIN_CLAIMED_FAILURE;

    return P4_PUBLIC_SPIN_OK;
}

enum p4_public_spinlock_result krnP4PublicSpinBlockingAttempt(
    spinlock_t *lock, uint32_t mode, p4_public_spinlock_hook_bridge hook,
    void *context)
{
    enum p4_public_spinlock_result result =
        krnP4PublicSpinTryAcquire(lock, mode);

    if ((result == P4_PUBLIC_SPIN_BUSY ||
         result == P4_PUBLIC_SPIN_TRY_TIMEOUT) && hook != NULL)
        (void)hook(lock, context);

    return result;
}

enum p4_public_spinlock_result krnP4PublicSpinUnlock(spinlock_t *lock)
{
    uint32_t attempt;
    uint32_t old_value;

    if (lock == NULL)
        return P4_PUBLIC_SPIN_BAD_ARGUMENT;
    if (!p4_public_lock_argument_valid(lock))
        return P4_PUBLIC_SPIN_BAD_ADDRESS;

    for (attempt = 0; attempt < P4_PUBLIC_SPINLOCK_RETRIES; ++attempt)
    {
        enum p4_public_atomic_result atomic_result;
        uint32_t state;

        atomic_result = p4_public_load(p4_public_lock_word(lock), &state);
        if (atomic_result == P4_PUBLIC_ATOMIC_TIMEOUT)
            continue;
        if (atomic_result != P4_PUBLIC_ATOMIC_OK)
            return p4_public_public_result(atomic_result);
        if (!p4_public_state_valid(state) || state == 0u ||
            state == P4_PUBLIC_TRANSITION)
            return P4_PUBLIC_SPIN_BAD_STATE;

        if ((state & P4_PUBLIC_WRITE) != 0)
        {
            atomic_result = p4_public_store(
                p4_public_owner_word(lock), 0u, &old_value);
            if (atomic_result == P4_PUBLIC_ATOMIC_TIMEOUT)
                continue;
            if (atomic_result != P4_PUBLIC_ATOMIC_OK)
                return p4_public_public_result(atomic_result);

            atomic_result = p4_public_compare_exchange(
                p4_public_lock_word(lock), P4_PUBLIC_WRITE, 0u, &old_value);
            if (atomic_result == P4_PUBLIC_ATOMIC_TIMEOUT)
                continue;
            if (atomic_result != P4_PUBLIC_ATOMIC_OK)
                return p4_public_public_result(atomic_result);
            return old_value == P4_PUBLIC_WRITE ? P4_PUBLIC_SPIN_OK :
                P4_PUBLIC_SPIN_BAD_STATE;
        }

        /* A reader-count race is ordinary contention; reload and retry it. */
        atomic_result = p4_public_compare_exchange(
            p4_public_lock_word(lock), state, state - 1u, &old_value);
        if (atomic_result == P4_PUBLIC_ATOMIC_TIMEOUT)
            continue;
        if (atomic_result != P4_PUBLIC_ATOMIC_OK)
            return p4_public_public_result(atomic_result);
        if (old_value == state)
            return P4_PUBLIC_SPIN_OK;
        if (!p4_public_state_valid(old_value) ||
            (old_value & P4_PUBLIC_READ_MASK) == 0u ||
            (old_value & P4_PUBLIC_WRITE) != 0)
            return P4_PUBLIC_SPIN_BAD_STATE;
    }

    return P4_PUBLIC_SPIN_RELEASE_FAILURE;
}

enum p4_public_spinlock_result krnP4PublicSpinIsLocked(
    const spinlock_t *lock, int *held)
{
    uint32_t attempt;
    uint32_t state = 0;

    if (held == NULL || lock == NULL)
        return P4_PUBLIC_SPIN_BAD_ARGUMENT;
    if (!p4_public_lock_argument_valid(lock))
        return P4_PUBLIC_SPIN_BAD_ADDRESS;

    for (attempt = 0; attempt < P4_PUBLIC_SPINLOCK_RETRIES; ++attempt)
    {
        enum p4_public_atomic_result atomic_result =
            p4_public_load(p4_public_lock_word((spinlock_t *)lock), &state);

        if (atomic_result == P4_PUBLIC_ATOMIC_TIMEOUT)
            continue;
        if (atomic_result != P4_PUBLIC_ATOMIC_OK)
            return p4_public_public_result(atomic_result);
        if (!p4_public_state_valid(state))
            return P4_PUBLIC_SPIN_BAD_STATE;
        *held = state != 0u;
        return P4_PUBLIC_SPIN_OK;
    }

    return P4_PUBLIC_SPIN_SERVICE_TIMEOUT;
}

void krnP4PublicSpinFatal(enum p4_public_spinlock_result reason)
{
    (void)reason;
#if defined(__riscv)
    __asm__ volatile("csrci mstatus, 8" : : : "memory");
    for (;;)
        __asm__ volatile("nop" : : : "memory");
#else
    __builtin_trap();
#endif
}
