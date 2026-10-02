/*
 * Private ESP32-P4 spinlock primitives. This is not the KrnSpin* ABI and
 * does not make Exec SMP-safe. Public wrappers must remain opt-in until the
 * caller's lock-placement, fail-hook and scheduler contracts are reviewed.
 */
#ifndef P4_SPINLOCK_H
#define P4_SPINLOCK_H

#include <stdint.h>
#if !defined(P4_SPINLOCK_TEST)
#include <aros/types/spinlock_s.h>
#endif

#define P4_SPINLOCK_MAX_REGIONS 2u

/* Half-open range of canonical, cached internal SRAM addresses. */
struct p4_spinlock_region
{
    uintptr_t begin;
    uintptr_t end;
};

/*
 * Caller supplies the exact SRAM extent containing this lock. For example,
 * an E3 diagnostic can bound a single .bss object. The backend additionally
 * clamps it to the canonical P4 internal-SRAM window, excluding PSRAM and
 * the +0x40000000 uncached alias before any lock dereference.
 *
 * At most two regions are needed for the P4's discontiguous usable SRAM
 * extents. Do not include bytes reserved for L2 cache or ROM state.
 */
struct p4_spinlock_policy
{
    struct p4_spinlock_region regions[P4_SPINLOCK_MAX_REGIONS];
    uint32_t region_count;
};

enum p4_spinlock_result
{
    P4_SPIN_OK = 0,
    P4_SPIN_BUSY,
    P4_SPIN_BAD_ADDRESS,
    P4_SPIN_BAD_MODE,
    P4_SPIN_BAD_STATE,
    P4_SPIN_READ_OVERFLOW,
    P4_SPIN_BAD_ARGUMENT
};

/*
 * Numeric placement check; it never dereferences lock. policy must point to
 * a valid, readable caller-owned object.
 */
int krnP4SpinAddressAllowed(const spinlock_t *lock,
                            const struct p4_spinlock_policy *policy);

/* Initialization requires the caller to ensure the lock is not in use. */
enum p4_spinlock_result krnP4SpinInit(
    spinlock_t *lock, const struct p4_spinlock_policy *policy);

/*
 * One non-blocking acquire attempt. mode is SPINLOCK_MODE_READ or WRITE.
 * A BUSY result means either the lock was held or the single SC lost its
 * reservation. Count BUSY results in caller-owned, per-hart counters.
 */
enum p4_spinlock_result krnP4SpinTryAcquire(
    spinlock_t *lock, uint32_t mode,
    const struct p4_spinlock_policy *policy);

/* One non-blocking release attempt; BUSY means retry outside this helper. */
enum p4_spinlock_result krnP4SpinUnlock(
    spinlock_t *lock, uint32_t mode,
    const struct p4_spinlock_policy *policy);

/* Returns status separately so invalid placement is not reported as free. */
enum p4_spinlock_result krnP4SpinIsLocked(
    const spinlock_t *lock, const struct p4_spinlock_policy *policy,
    int *held);

/*
 * KrnSpinLock's legacy failhook is a notification, not cancellation: the
 * native Exec hook has a void entry that marks the current task TS_SPIN, and
 * the PC backend ignores the Hook return value. A future public wrapper must
 * invoke its Hook outside these primitives after each BUSY result, then
 * retry acquisition. The primitives never call Exec or Hook code.
 */

#endif /* P4_SPINLOCK_H */
