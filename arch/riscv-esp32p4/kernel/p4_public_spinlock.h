/*
 * Bounded adapter for AROS's public RV32 spinlock ABI.
 *
 * The public lock word and owner slot are ordinary memory only at rest. All
 * concurrent accesses to either field must go through the prepared P4
 * software-atomic service. This adapter does not select the public LVO
 * wrappers or make Exec SMP-safe by itself.
 */
#ifndef P4_PUBLIC_SPINLOCK_H
#define P4_PUBLIC_SPINLOCK_H

#include <stdint.h>

#if defined(P4_PUBLIC_SPINLOCK_TEST)
#ifndef P4_PUBLIC_SPINLOCK_TEST_TYPES
typedef struct
{
    volatile uint32_t lock;
    volatile uint32_t s_Owner;
} spinlock_t;
#endif
#ifndef SPINLOCK_MODE_READ
#define SPINLOCK_MODE_READ 0u
#define SPINLOCK_MODE_WRITE 1u
#endif
#ifndef SPINLOCKF_WRITE
#define SPINLOCKF_WRITE (UINT32_C(1) << 27)
#define SPINLOCKF_UPDATING (UINT32_C(1) << 31)
#endif
#else
#include <aros/types/spinlock_s.h>
#endif

/* The bound is in addition to the software-atomic service's per-call bound. */
#define P4_PUBLIC_SPINLOCK_RETRIES 8u

enum p4_public_spinlock_result
{
    P4_PUBLIC_SPIN_OK = 0,
    P4_PUBLIC_SPIN_BUSY,
    /* Try-acquire did not claim the public lock; callers may return/retry. */
    P4_PUBLIC_SPIN_TRY_TIMEOUT,
    /* A write claim was made but its owner/stable publication did not finish. */
    P4_PUBLIC_SPIN_CLAIMED_FAILURE,
    /* A release operation did not complete; the caller must halt. */
    P4_PUBLIC_SPIN_RELEASE_FAILURE,
    /* Init/query exhausted bounded service operations without a lock claim. */
    P4_PUBLIC_SPIN_SERVICE_TIMEOUT,
    P4_PUBLIC_SPIN_BAD_ARGUMENT,
    P4_PUBLIC_SPIN_BAD_MODE,
    P4_PUBLIC_SPIN_BAD_ADDRESS,
    P4_PUBLIC_SPIN_BAD_STATE,
    P4_PUBLIC_SPIN_READ_OVERFLOW,
    P4_PUBLIC_SPIN_NOT_PREPARED,
    P4_PUBLIC_SPIN_SERVICE_ERROR
};

/* Quiescent initialization: both ABI words are written through the service. */
enum p4_public_spinlock_result krnP4PublicSpinInit(spinlock_t *lock);

/* One nonblocking acquire attempt. BUSY and TRY_TIMEOUT have claimed nothing. */
enum p4_public_spinlock_result krnP4PublicSpinTryAcquire(
    spinlock_t *lock, uint32_t mode);

/*
 * One blocking-loop iteration. A non-NULL hook bridge is called after each
 * BUSY/TRY_TIMEOUT result, after the atomic service has returned. Its value
 * is deliberately ignored, matching the legacy notification-only contract.
 */
typedef uintptr_t (*p4_public_spinlock_hook_bridge)(spinlock_t *lock,
                                                    void *context);
enum p4_public_spinlock_result krnP4PublicSpinBlockingAttempt(
    spinlock_t *lock, uint32_t mode, p4_public_spinlock_hook_bridge hook,
    void *context);

/* Mode-less public release; this helper retries bounded concurrent changes. */
enum p4_public_spinlock_result krnP4PublicSpinUnlock(spinlock_t *lock);

enum p4_public_spinlock_result krnP4PublicSpinIsLocked(
    const spinlock_t *lock, int *held);

/* LVO wrappers must fail closed for every result other than OK/BUSY/TRY_TIMEOUT. */
void krnP4PublicSpinFatal(enum p4_public_spinlock_result reason)
    __attribute__((noreturn));

#endif /* P4_PUBLIC_SPINLOCK_H */
