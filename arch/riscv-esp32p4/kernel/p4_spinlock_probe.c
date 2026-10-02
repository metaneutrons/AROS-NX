/*
 * Production SRAM lock qualification on two private diagnostic harts.
 * No Exec, allocator, console, driver or fail-hook calls. Both harts have
 * MIE masked. All retries and barriers have cycle and iteration bounds.
 */
#include <stdint.h>
#include "p4_spinlock.h"
#include "p4_spinlock_probe.h"
#if defined(__GNUC__) && !defined(__clang__)
/* This bounded diagnostic must not inherit an unoptimized kernel make. */
#pragma GCC optimize ("Os")
#endif
#define SRAM __attribute__((section(".sramtext.secondary_mailbox"), noinline))
#define INLINE static inline __attribute__((always_inline))
#define N 4096u
static spinlock_t probe_lock __attribute__((aligned(64)));
static struct p4_spinlock_policy policy;
static volatile uint32_t protected_count, protected_inverse;
struct lane { uint32_t ready, done, busy, forced; uint32_t padding[12]; };
static struct { struct lane lane[2]; uint32_t go; }
    publication __attribute__((aligned(64)));
INLINE volatile struct lane *lane(uint32_t h)
{
    uintptr_t out;
    __asm__ volatile("add %0,%1,%2" : "=r"(out) :
        "r"((uintptr_t)&publication.lane[h]), "r"(0x40000000u));
    return (volatile struct lane *)out;
}
INLINE volatile uint32_t *go(void)
{
    uintptr_t out;
    __asm__ volatile("add %0,%1,%2" : "=r"(out) :
        "r"((uintptr_t)&publication.go), "r"(0x40000000u));
    return (volatile uint32_t *)out;
}
INLINE void fence(void) { __asm__ volatile("fence iorw,iorw" ::: "memory"); }
INLINE uint32_t tick(void)
{ uint32_t n; __asm__ volatile("csrr %0,mcycle" : "=r"(n)); return n; }
INLINE int wait(volatile uint32_t *p, uint32_t value)
{
    uint32_t start = tick(), left = 2000000;
    do { uint32_t n = *p; fence(); if (n == value) return 1; }
    while (--left && (uint32_t)(tick() - start) < 360000000u);
    return 0;
}
INLINE int attempt(uint32_t h, uint32_t mode, int release)
{
    uint32_t start = tick(), left = 2000000;
    enum p4_spinlock_result result;
    do {
        result = release ? krnP4SpinUnlock(&probe_lock, mode, &policy) :
            krnP4SpinTryAcquire(&probe_lock, mode, &policy);
        if (result == P4_SPIN_OK) return 1;
        if (result != P4_SPIN_BUSY) return 0;
        lane(h)->busy++;
    } while (--left && (uint32_t)(tick() - start) < 360000000u);
    return 0;
}
int krnP4SpinProbePrepare(void)
{
    uint32_t h, i;
    int held = -1;
    /* Caller keeps hart1 reset-held. Evict cached publication aliases before
       writing their uncached lanes, as the existing E2 controller does. */
    extern void krnP4CacheSyncData(void *, unsigned long);
    krnP4CacheSyncData(&publication, sizeof publication);
    for (h = 0; h < 2; ++h) {
        volatile uint32_t *p = (volatile uint32_t *)lane(h);
        for (i = 0; i < 16; ++i) p[i] = 0;
    }
    *go() = 0;
    policy.region_count = 1;
    policy.regions[0].begin = (uintptr_t)&probe_lock;
    policy.regions[0].end = (uintptr_t)&probe_lock + sizeof probe_lock;
    policy.regions[1].begin = policy.regions[1].end = 0;
    protected_count = 0; protected_inverse = UINT32_MAX;
    if (krnP4SpinInit(&probe_lock, &policy) != P4_SPIN_OK ||
        krnP4SpinTryAcquire(&probe_lock, 2, &policy) != P4_SPIN_BAD_MODE ||
        krnP4SpinUnlock(&probe_lock, SPINLOCK_MODE_READ, &policy) != P4_SPIN_BAD_STATE ||
        krnP4SpinTryAcquire((spinlock_t *)(uintptr_t)0x48000000u,
            SPINLOCK_MODE_WRITE, &policy) != P4_SPIN_BAD_ADDRESS ||
        krnP4SpinTryAcquire((spinlock_t *)(uintptr_t)((uintptr_t)&probe_lock + 0x40000000u),
            SPINLOCK_MODE_WRITE, &policy) != P4_SPIN_BAD_ADDRESS ||
        krnP4SpinTryAcquire((spinlock_t *)(uintptr_t)((uintptr_t)&probe_lock + 1),
            SPINLOCK_MODE_WRITE, &policy) != P4_SPIN_BAD_ADDRESS ||
        krnP4SpinIsLocked(&probe_lock, &policy, &held) != P4_SPIN_OK || held)
        return 0;
    /* This real lock is deliberately held until hart1 proves both refusals. */
    if (!attempt(0, SPINLOCK_MODE_WRITE, 0)) return 0;
    fence();
    return 1;
}
SRAM int krnP4SpinProbeExercise(uint32_t h)
{
    uint32_t actual, status, i;
    __asm__ volatile("csrr %0,mhartid\ncsrr %1,mstatus" : "=r"(actual), "=r"(status));
    if (h > 1 || actual != h || (status & 8u)) return 0;
    if (h) {
        if (krnP4SpinTryAcquire(&probe_lock, SPINLOCK_MODE_WRITE, &policy) != P4_SPIN_BUSY ||
            krnP4SpinTryAcquire(&probe_lock, SPINLOCK_MODE_READ, &policy) != P4_SPIN_BUSY)
            return 0;
        lane(h)->forced = 1; lane(h)->busy += 2;
        fence(); lane(h)->ready = 1; fence();
        if (!wait(go(), 1)) return 0;
    } else {
        if (!wait(&lane(1)->ready, 1) || !lane(1)->forced ||
            !attempt(h, SPINLOCK_MODE_WRITE, 1)) return 0;
        *go() = 1; fence();
    }
    for (i = 0; i < N; ++i) {
        if (!attempt(h, SPINLOCK_MODE_WRITE, 0)) return 0;
        if (protected_inverse != ~protected_count) return 0;
        ++protected_count;
        protected_inverse = ~protected_count;
        if (!attempt(h, SPINLOCK_MODE_WRITE, 1)) return 0;
    }
    fence(); lane(h)->done = 1; fence();
    if (!wait(&lane(h ^ 1u)->done, 1) ||
        !attempt(h, SPINLOCK_MODE_READ, 0)) return 0;
    lane(h)->ready = 2; fence();
    if (!wait(&lane(h ^ 1u)->ready, 2) || protected_count != 2 * N ||
        protected_inverse != ~protected_count ||
        krnP4SpinTryAcquire(&probe_lock, SPINLOCK_MODE_WRITE, &policy) != P4_SPIN_BUSY)
        return 0;
    lane(h)->forced++;
    if (!attempt(h, SPINLOCK_MODE_READ, 1)) return 0;
    fence(); lane(h)->done = 2; fence();
    return wait(&lane(h ^ 1u)->done, 2);
}
int krnP4SpinProbeCheck(void)
{
    int held = -1;
    fence();
    return protected_count == 2 * N && protected_inverse == ~protected_count &&
        lane(0)->done == 2 && lane(1)->done == 2 &&
        lane(0)->forced == 1 && lane(1)->forced == 2 && lane(1)->busy >= 2 &&
        krnP4SpinIsLocked(&probe_lock, &policy, &held) == P4_SPIN_OK && !held;
}
