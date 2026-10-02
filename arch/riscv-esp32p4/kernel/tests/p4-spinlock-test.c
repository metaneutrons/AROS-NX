/*
 * Host regression for the production ESP32-P4 private spinlock state machine.
 * Only its one-word atomic primitives and address admission are mocked.
 */
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define P4_SPINLOCK_TEST

/* The generic public header selects the host CPU's ABI here. This compact
   fixture preserves its exact lock word, bit positions, modes, and owner
   field while allowing the production P4 source to compile on the host. */
#define _AROS_TYPES_SPINLOCK_S_H_
typedef struct
{
    union
    {
        volatile struct
        {
            unsigned int readcount : 24;
            unsigned int _pad2 : 3;
            unsigned int write : 1;
            unsigned int _pad1 : 3;
            unsigned int updating : 1;
        } slock;
        volatile unsigned char block[4];
        volatile unsigned int lock;
    };
    void *s_Owner;
} spinlock_t;
#define SPINLOCK_UNLOCKED 0u
#define SPINLOCKB_WRITE 27u
#define SPINLOCKB_UPDATING 31u
#define SPINLOCKF_WRITE (1u << SPINLOCKB_WRITE)
#define SPINLOCKF_UPDATING (1u << SPINLOCKB_UPDATING)
#define SPINLOCK_MODE_READ 0u
#define SPINLOCK_MODE_WRITE 1u

#include "../p4_spinlock.h"

static spinlock_t test_lock;
static const struct p4_spinlock_policy test_policy = {
    {
        { (uintptr_t)&test_lock, (uintptr_t)&test_lock + sizeof(test_lock) },
        { 0, 0 }
    },
    1
};

static int test_address_enabled = 1;
static _Thread_local uint32_t test_lr_count;
static _Thread_local uint32_t test_sc_count;
static _Thread_local uint32_t test_load_count;
static _Thread_local int test_fail_next_sc;
static _Thread_local int test_change_after_load;
static _Thread_local uint32_t test_change_value;

static uint32_t test_lr(volatile uint32_t *word)
{
    ++test_lr_count;
    return __atomic_load_n((uint32_t *)word, __ATOMIC_ACQUIRE);
}

static uint32_t test_load(volatile uint32_t *word)
{
    uint32_t value;

    ++test_load_count;
    value = __atomic_load_n((uint32_t *)word, __ATOMIC_RELAXED);
    if (test_change_after_load)
    {
        test_change_after_load = 0;
        __atomic_store_n((uint32_t *)word, test_change_value,
                         __ATOMIC_RELAXED);
    }
    return value;
}

static int test_cas_once(volatile uint32_t *word, uint32_t expected,
                         uint32_t desired)
{
    uint32_t observed;
    int succeeded;

    observed = test_lr(word);
    if (observed != expected)
        return 0;

    ++test_sc_count;
    if (test_fail_next_sc)
    {
        test_fail_next_sc = 0;
        return 0;
    }

    succeeded = __atomic_compare_exchange_n((uint32_t *)word, &expected,
                                             desired, 0, __ATOMIC_RELEASE,
                                             __ATOMIC_RELAXED);
    return succeeded;
}

static int test_address_valid(const spinlock_t *lock,
                              const struct p4_spinlock_policy *policy)
{
    return test_address_enabled && lock == &test_lock &&
           policy == &test_policy;
}

#define P4_SPINLOCK_LR(word) test_lr(word)
#define P4_SPINLOCK_LOAD(word) test_load(word)
#define P4_SPINLOCK_CAS_ONCE(word, expected, desired) \
    test_cas_once((word), (expected), (desired))
#define P4_SPINLOCK_ADDRESS_VALID(lock, policy) \
    test_address_valid((lock), (policy))
#include "../p4_spinlock.c"

static unsigned long check_count;

#define CHECK(condition) do { \
    ++check_count; \
    if (!(condition)) { \
        fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); \
        return 0; \
    } \
} while (0)

static void reset_mock_counts(void)
{
    test_lr_count = 0;
    test_sc_count = 0;
    test_load_count = 0;
    test_fail_next_sc = 0;
    test_change_after_load = 0;
    test_change_value = 0;
}

static int test_numeric_address_policy(void)
{
    struct p4_spinlock_policy policy;
    uintptr_t base = (uintptr_t)0x4ff00000u;
    uintptr_t top = (uintptr_t)0x4ffc0000u;

    policy.regions[0].begin = base;
    policy.regions[0].end = base + sizeof(spinlock_t);
    policy.region_count = 1;
    CHECK(krnP4SpinAddressAllowed((const spinlock_t *)base, &policy));

    policy.regions[0].begin = (uintptr_t)0x48000000u;
    policy.regions[0].end = policy.regions[0].begin + sizeof(spinlock_t);
    CHECK(!krnP4SpinAddressAllowed(
        (const spinlock_t *)policy.regions[0].begin, &policy));

    policy.regions[0].begin = (uintptr_t)0x8ff00000u;
    policy.regions[0].end = policy.regions[0].begin + sizeof(spinlock_t);
    CHECK(!krnP4SpinAddressAllowed(
        (const spinlock_t *)policy.regions[0].begin, &policy));

    policy.regions[0].begin = top - sizeof(spinlock_t);
    policy.regions[0].end = top;
    CHECK(krnP4SpinAddressAllowed(
        (const spinlock_t *)policy.regions[0].begin, &policy));

    policy.regions[0].begin = top - sizeof(spinlock_t) + 4u;
    policy.regions[0].end = top;
    CHECK(!krnP4SpinAddressAllowed(
        (const spinlock_t *)policy.regions[0].begin, &policy));

    policy.regions[0].begin = base + 1u;
    policy.regions[0].end = base + 64u;
    CHECK(!krnP4SpinAddressAllowed((const spinlock_t *)(base + 1u),
                                   &policy));

    policy.regions[0].begin = base;
    policy.regions[0].end = base + sizeof(spinlock_t);
    policy.region_count = 0;
    CHECK(!krnP4SpinAddressAllowed((const spinlock_t *)base, &policy));
    policy.region_count = P4_SPINLOCK_MAX_REGIONS + 1u;
    CHECK(!krnP4SpinAddressAllowed((const spinlock_t *)base, &policy));

    policy.region_count = 2;
    policy.regions[0].begin = base;
    policy.regions[0].end = base + 64u;
    policy.regions[1].begin = base + 32u;
    policy.regions[1].end = base + 128u;
    CHECK(!krnP4SpinAddressAllowed((const spinlock_t *)base, &policy));
    CHECK(!krnP4SpinAddressAllowed(NULL, &policy));
    CHECK(!krnP4SpinAddressAllowed((const spinlock_t *)base, NULL));
    return 1;
}

static int test_read_write_and_held(void)
{
    int held = -1;

    memset(&test_lock, 0xa5, sizeof(test_lock));
    CHECK(krnP4SpinInit(&test_lock, &test_policy) == P4_SPIN_OK);
    CHECK(test_lock.lock == SPINLOCK_UNLOCKED);
    CHECK(test_lock.s_Owner == NULL);

    CHECK(krnP4SpinTryAcquire(&test_lock, SPINLOCK_MODE_READ,
                              &test_policy) == P4_SPIN_OK);
    CHECK(test_lock.lock == 1u);
    CHECK(krnP4SpinTryAcquire(&test_lock, SPINLOCK_MODE_READ,
                              &test_policy) == P4_SPIN_OK);
    CHECK(test_lock.lock == 2u);
    CHECK(krnP4SpinIsLocked(&test_lock, &test_policy, &held) == P4_SPIN_OK);
    CHECK(held == 1);

    CHECK(krnP4SpinTryAcquire(&test_lock, SPINLOCK_MODE_WRITE,
                              &test_policy) == P4_SPIN_BUSY);
    CHECK(test_lock.lock == 2u);
    CHECK(krnP4SpinUnlock(&test_lock, SPINLOCK_MODE_READ,
                          &test_policy) == P4_SPIN_OK);
    CHECK(test_lock.lock == 1u);
    CHECK(krnP4SpinTryAcquire(&test_lock, SPINLOCK_MODE_WRITE,
                              &test_policy) == P4_SPIN_BUSY);
    CHECK(krnP4SpinUnlock(&test_lock, SPINLOCK_MODE_READ,
                          &test_policy) == P4_SPIN_OK);
    CHECK(test_lock.lock == SPINLOCK_UNLOCKED);
    CHECK(krnP4SpinIsLocked(&test_lock, &test_policy, &held) == P4_SPIN_OK);
    CHECK(held == 0);

    CHECK(krnP4SpinTryAcquire(&test_lock, SPINLOCK_MODE_WRITE,
                              &test_policy) == P4_SPIN_OK);
    CHECK(test_lock.lock == SPINLOCKF_WRITE);
    CHECK(krnP4SpinTryAcquire(&test_lock, SPINLOCK_MODE_READ,
                              &test_policy) == P4_SPIN_BUSY);
    CHECK(krnP4SpinTryAcquire(&test_lock, SPINLOCK_MODE_WRITE,
                              &test_policy) == P4_SPIN_BUSY);
    CHECK(krnP4SpinUnlock(&test_lock, SPINLOCK_MODE_READ,
                          &test_policy) == P4_SPIN_BAD_STATE);
    CHECK(test_lock.lock == SPINLOCKF_WRITE);
    CHECK(krnP4SpinUnlock(&test_lock, SPINLOCK_MODE_WRITE,
                          &test_policy) == P4_SPIN_OK);
    CHECK(test_lock.lock == SPINLOCK_UNLOCKED);
    return 1;
}

static int test_rejections_and_no_mutation(void)
{
    spinlock_t before;
    uint32_t lr_before, sc_before;
    int held = 0x5a;

    CHECK(krnP4SpinInit(&test_lock, &test_policy) == P4_SPIN_OK);
    test_lock.lock = SPINLOCKF_WRITE;
    test_lock.s_Owner = (void *)(uintptr_t)0x1234u;
    before = test_lock;

    test_address_enabled = 0;
    reset_mock_counts();
    CHECK(krnP4SpinTryAcquire(&test_lock, SPINLOCK_MODE_READ,
                              &test_policy) == P4_SPIN_BAD_ADDRESS);
    CHECK(krnP4SpinUnlock(&test_lock, SPINLOCK_MODE_WRITE,
                          &test_policy) == P4_SPIN_BAD_ADDRESS);
    CHECK(krnP4SpinInit(&test_lock, &test_policy) == P4_SPIN_BAD_ADDRESS);
    CHECK(krnP4SpinIsLocked(&test_lock, &test_policy, &held) ==
          P4_SPIN_BAD_ADDRESS);
    CHECK(test_lock.lock == before.lock);
    CHECK(test_lock.s_Owner == before.s_Owner);
    CHECK(held == 0x5a);
    CHECK(test_load_count == 0);
    CHECK(test_lr_count == 0 && test_sc_count == 0);

    CHECK(krnP4SpinTryAcquire((spinlock_t *)(uintptr_t)0xdeadbeefu,
                              SPINLOCK_MODE_WRITE,
                              &test_policy) == P4_SPIN_BAD_ADDRESS);
    CHECK(krnP4SpinTryAcquire(NULL, SPINLOCK_MODE_WRITE,
                              &test_policy) == P4_SPIN_BAD_ARGUMENT);
    CHECK(krnP4SpinUnlock(&test_lock, SPINLOCK_MODE_WRITE, NULL) ==
          P4_SPIN_BAD_ARGUMENT);
    CHECK(krnP4SpinIsLocked(&test_lock, &test_policy, NULL) ==
          P4_SPIN_BAD_ARGUMENT);
    CHECK(test_lock.lock == before.lock);
    CHECK(test_lock.s_Owner == before.s_Owner);

    test_address_enabled = 1;
    lr_before = test_lr_count;
    sc_before = test_sc_count;
    CHECK(krnP4SpinTryAcquire(&test_lock, 2u, &test_policy) ==
          P4_SPIN_BAD_MODE);
    CHECK(krnP4SpinUnlock(&test_lock, 2u, &test_policy) ==
          P4_SPIN_BAD_MODE);
    CHECK(test_lr_count == lr_before && test_sc_count == sc_before);
    CHECK(test_load_count == 0);
    CHECK(test_lock.lock == before.lock);

    test_lock.lock = P4_SPINLOCK_READ_MASK;
    CHECK(krnP4SpinTryAcquire(&test_lock, SPINLOCK_MODE_READ,
                              &test_policy) == P4_SPIN_READ_OVERFLOW);
    CHECK(test_lock.lock == P4_SPINLOCK_READ_MASK);
    test_lock.lock = SPINLOCKF_UPDATING;
    CHECK(krnP4SpinTryAcquire(&test_lock, SPINLOCK_MODE_READ,
                              &test_policy) == P4_SPIN_BAD_STATE);
    CHECK(test_lock.lock == SPINLOCKF_UPDATING);
    test_lock.lock = SPINLOCKF_WRITE | 1u;
    CHECK(krnP4SpinUnlock(&test_lock, SPINLOCK_MODE_WRITE,
                          &test_policy) == P4_SPIN_BAD_STATE);
    CHECK(test_lock.lock == (SPINLOCKF_WRITE | 1u));
    test_lock.lock = 0x01000000u;
    CHECK(krnP4SpinTryAcquire(&test_lock, SPINLOCK_MODE_WRITE,
                              &test_policy) == P4_SPIN_BAD_STATE);
    CHECK(test_lock.lock == 0x01000000u);
    test_lock.lock = SPINLOCK_UNLOCKED;
    CHECK(krnP4SpinUnlock(&test_lock, SPINLOCK_MODE_READ,
                          &test_policy) == P4_SPIN_BAD_STATE);
    CHECK(test_lock.lock == SPINLOCK_UNLOCKED);
    return 1;
}

static int test_single_attempt_and_release_order(void)
{
    uint32_t lr_before, sc_before;

    CHECK(krnP4SpinInit(&test_lock, &test_policy) == P4_SPIN_OK);
    reset_mock_counts();

    /* A changed snapshot is refused before any store-conditional. */
    test_change_after_load = 1;
    test_change_value = SPINLOCKF_WRITE;
    CHECK(krnP4SpinTryAcquire(&test_lock, SPINLOCK_MODE_READ,
                              &test_policy) == P4_SPIN_BUSY);
    CHECK(test_lock.lock == SPINLOCKF_WRITE);
    CHECK(test_load_count == 1u);
    CHECK(test_lr_count == 1u && test_sc_count == 0u);
    test_lock.lock = SPINLOCK_UNLOCKED;

    reset_mock_counts();
    test_fail_next_sc = 1;
    CHECK(krnP4SpinTryAcquire(&test_lock, SPINLOCK_MODE_WRITE,
                              &test_policy) == P4_SPIN_BUSY);
    CHECK(test_lock.lock == SPINLOCK_UNLOCKED);
    CHECK(test_lr_count == 1 && test_sc_count == 1);

    CHECK(krnP4SpinTryAcquire(&test_lock, SPINLOCK_MODE_WRITE,
                              &test_policy) == P4_SPIN_OK);
    CHECK(test_lock.lock == SPINLOCKF_WRITE);
    lr_before = test_lr_count;
    sc_before = test_sc_count;
    test_fail_next_sc = 1;
    CHECK(krnP4SpinUnlock(&test_lock, SPINLOCK_MODE_WRITE,
                          &test_policy) == P4_SPIN_BUSY);
    CHECK(test_lock.lock == SPINLOCKF_WRITE);
    CHECK(test_lr_count - lr_before == 1u);
    CHECK(test_sc_count - sc_before == 1u);
    CHECK(krnP4SpinUnlock(&test_lock, SPINLOCK_MODE_WRITE,
                          &test_policy) == P4_SPIN_OK);

    CHECK(krnP4SpinTryAcquire(&test_lock, SPINLOCK_MODE_READ,
                              &test_policy) == P4_SPIN_OK);
    test_fail_next_sc = 1;
    CHECK(krnP4SpinUnlock(&test_lock, SPINLOCK_MODE_READ,
                          &test_policy) == P4_SPIN_BUSY);
    CHECK(test_lock.lock == 1u);
    CHECK(krnP4SpinUnlock(&test_lock, SPINLOCK_MODE_READ,
                          &test_policy) == P4_SPIN_OK);
    return 1;
}

typedef int (*test_hook_fn)(spinlock_t *lock, void *context);

/* Models the future KrnSpinLock wrapper, with a test-only attempt cap. */
static spinlock_t *test_blocking_wrapper(uint32_t mode,
                                         const struct p4_spinlock_policy *p,
                                         test_hook_fn hook, void *context,
                                         uint32_t attempt_cap)
{
    uint32_t i;

    for (i = 0; i < attempt_cap; ++i)
    {
        enum p4_spinlock_result result =
            krnP4SpinTryAcquire(&test_lock, mode, p);

        if (result == P4_SPIN_OK)
            return &test_lock;
        if (result != P4_SPIN_BUSY)
            return NULL;
        if (hook != NULL)
            (void)hook(&test_lock, context); /* legacy hook return is ignored */
    }
    return NULL; /* harness deadline, not a KrnSpinLock cancellation result */
}

struct hook_test_state
{
    uint32_t calls;
    int unlock_on_second_call;
};

static int test_fail_hook(spinlock_t *lock, void *context)
{
    struct hook_test_state *state = context;

    ++state->calls;
    if (state->unlock_on_second_call && state->calls == 2u)
        (void)krnP4SpinUnlock(lock, SPINLOCK_MODE_WRITE, &test_policy);
    return 0; /* request-like return value cannot cancel the public API */
}

static int test_hook_retry_and_timeout(void)
{
    struct hook_test_state state = { 0, 1 };

    CHECK(krnP4SpinInit(&test_lock, &test_policy) == P4_SPIN_OK);
    CHECK(krnP4SpinTryAcquire(&test_lock, SPINLOCK_MODE_WRITE,
                              &test_policy) == P4_SPIN_OK);
    CHECK(test_blocking_wrapper(SPINLOCK_MODE_WRITE, &test_policy,
                                test_fail_hook, &state, 4u) == &test_lock);
    CHECK(state.calls == 2u);
    CHECK(test_lock.lock == SPINLOCKF_WRITE);
    CHECK(krnP4SpinUnlock(&test_lock, SPINLOCK_MODE_WRITE,
                          &test_policy) == P4_SPIN_OK);

    state.calls = 0;
    state.unlock_on_second_call = 0;
    CHECK(krnP4SpinTryAcquire(&test_lock, SPINLOCK_MODE_WRITE,
                              &test_policy) == P4_SPIN_OK);
    CHECK(test_blocking_wrapper(SPINLOCK_MODE_READ, &test_policy,
                                test_fail_hook, &state, 3u) == NULL);
    CHECK(state.calls == 3u);
    CHECK(test_lock.lock == SPINLOCKF_WRITE);
    CHECK(krnP4SpinUnlock(&test_lock, SPINLOCK_MODE_WRITE,
                          &test_policy) == P4_SPIN_OK);
    return 1;
}

#define READER_THREADS 3u
#define WRITER_THREADS 2u
#define CONTENTION_ITERATIONS 800u
#define ACQUIRE_LIMIT 1000000u

struct contention_arg
{
    uint32_t mode;
    uint32_t iterations;
};

static uint32_t start_workers;
static uint32_t active_readers;
static uint32_t active_writer;
static uint32_t shared_value;
static uint32_t contention_errors;
static uint32_t busy_acquire_count;
static uint32_t busy_release_count;
static uint32_t overlapping_reader_count;

static int worker_acquire(uint32_t mode)
{
    uint32_t i;

    for (i = 0; i < ACQUIRE_LIMIT; ++i)
    {
        enum p4_spinlock_result result =
            krnP4SpinTryAcquire(&test_lock, mode, &test_policy);

        if (result == P4_SPIN_OK)
            return 1;
        if (result != P4_SPIN_BUSY)
        {
            __atomic_add_fetch(&contention_errors, 1u, __ATOMIC_RELAXED);
            return 0;
        }
        __atomic_add_fetch(&busy_acquire_count, 1u, __ATOMIC_RELAXED);
        if ((i & 0xffu) == 0)
            sched_yield();
    }

    __atomic_add_fetch(&contention_errors, 1u, __ATOMIC_RELAXED);
    return 0;
}

static int worker_unlock(uint32_t mode)
{
    uint32_t i;

    for (i = 0; i < ACQUIRE_LIMIT; ++i)
    {
        enum p4_spinlock_result result =
            krnP4SpinUnlock(&test_lock, mode, &test_policy);

        if (result == P4_SPIN_OK)
            return 1;
        if (result != P4_SPIN_BUSY)
        {
            __atomic_add_fetch(&contention_errors, 1u, __ATOMIC_RELAXED);
            return 0;
        }
        __atomic_add_fetch(&busy_release_count, 1u, __ATOMIC_RELAXED);
        if ((i & 0xffu) == 0)
            sched_yield();
    }

    __atomic_add_fetch(&contention_errors, 1u, __ATOMIC_RELAXED);
    return 0;
}

static void *contention_worker(void *opaque)
{
    const struct contention_arg *arg = opaque;
    uint32_t iteration;

    while (__atomic_load_n(&start_workers, __ATOMIC_ACQUIRE) == 0)
        sched_yield();

    for (iteration = 0; iteration < arg->iterations; ++iteration)
    {
        if (!worker_acquire(arg->mode))
            return NULL;

        if (arg->mode == SPINLOCK_MODE_READ)
        {
            uint32_t previous = __atomic_fetch_add(&active_readers, 1u,
                                                   __ATOMIC_ACQ_REL);

            if (__atomic_load_n(&active_writer, __ATOMIC_ACQUIRE) != 0)
                __atomic_add_fetch(&contention_errors, 1u, __ATOMIC_RELAXED);
            if (previous >= 1u)
                __atomic_add_fetch(&overlapping_reader_count, 1u,
                                   __ATOMIC_RELAXED);
            /* Give other host threads a chance to contend as readers/writers. */
            sched_yield();
            if (__atomic_load_n(&active_writer, __ATOMIC_ACQUIRE) != 0)
                __atomic_add_fetch(&contention_errors, 1u, __ATOMIC_RELAXED);
            (void)__atomic_load_n(&shared_value, __ATOMIC_RELAXED);
            __atomic_fetch_sub(&active_readers, 1u, __ATOMIC_RELEASE);
        }
        else
        {
            uint32_t previous = __atomic_exchange_n(&active_writer, 1u,
                                                    __ATOMIC_ACQ_REL);

            if (previous != 0 ||
                __atomic_load_n(&active_readers, __ATOMIC_ACQUIRE) != 0)
                __atomic_add_fetch(&contention_errors, 1u, __ATOMIC_RELAXED);
            ++shared_value;
            sched_yield();
            __atomic_store_n(&active_writer, 0u, __ATOMIC_RELEASE);
        }

        if (!worker_unlock(arg->mode))
            return NULL;
    }

    return NULL;
}

static int test_reader_writer_contention(void)
{
    pthread_t threads[READER_THREADS + WRITER_THREADS];
    struct contention_arg args[READER_THREADS + WRITER_THREADS];
    uint32_t i;
    uint32_t expected_writes = WRITER_THREADS * CONTENTION_ITERATIONS;

    CHECK(krnP4SpinInit(&test_lock, &test_policy) == P4_SPIN_OK);
    start_workers = 0;
    active_readers = 0;
    active_writer = 0;
    shared_value = 0;
    contention_errors = 0;
    busy_acquire_count = 0;
    busy_release_count = 0;
    overlapping_reader_count = 0;

    for (i = 0; i < READER_THREADS + WRITER_THREADS; ++i)
    {
        args[i].mode = i < READER_THREADS ? SPINLOCK_MODE_READ
                                          : SPINLOCK_MODE_WRITE;
        args[i].iterations = CONTENTION_ITERATIONS;
        CHECK(pthread_create(&threads[i], NULL, contention_worker,
                             &args[i]) == 0);
    }
    __atomic_store_n(&start_workers, 1u, __ATOMIC_RELEASE);

    for (i = 0; i < READER_THREADS + WRITER_THREADS; ++i)
        CHECK(pthread_join(threads[i], NULL) == 0);

    CHECK(__atomic_load_n(&contention_errors, __ATOMIC_ACQUIRE) == 0);
    CHECK(__atomic_load_n(&shared_value, __ATOMIC_ACQUIRE) == expected_writes);
    CHECK(__atomic_load_n(&busy_acquire_count, __ATOMIC_ACQUIRE) > 0);
    CHECK(__atomic_load_n(&overlapping_reader_count, __ATOMIC_ACQUIRE) > 0);
    CHECK(test_lock.lock == SPINLOCK_UNLOCKED);
    CHECK(active_readers == 0 && active_writer == 0);
    return 1;
}

int main(void)
{
    if (!test_numeric_address_policy() ||
        !test_read_write_and_held() ||
        !test_rejections_and_no_mutation() ||
        !test_single_attempt_and_release_order() ||
        !test_hook_retry_and_timeout() ||
        !test_reader_writer_contention())
        return 1;

    printf("PASS P4 spinlock host test (%lu checks; acquire busy=%u, "
           "release busy=%u)\n",
           check_count,
           __atomic_load_n(&busy_acquire_count, __ATOMIC_RELAXED),
           __atomic_load_n(&busy_release_count, __ATOMIC_RELAXED));
    return 0;
}
