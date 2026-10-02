/*
 * Actual-source host fixture for the P4 SMP semaphore gate.
 *
 * The production implementation is included verbatim. Only the Exec IRQ
 * nesting and public spinlock service are deterministic host mocks; this is
 * not a silicon or integrated Exec semaphore test.
 */
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define P4_PUBLIC_SPINLOCK_TEST 1
#define P4_SEMAPHORE_GATE_TEST 1
#include "../../kernel/p4_public_spinlock.h"
#include "../p4_semaphore_gate.h"

struct ExecBase
{
    uint32_t marker;
};

struct SemaphoreLink
{
    uint32_t before[3];
    spinlock_t sr_SpinLock;
    uint32_t after[3];
};

struct SignalSemaphore
{
    uint32_t before[4];
    struct SemaphoreLink ss_MultipleLink;
    uint32_t after[4];
};

static unsigned int interrupt_nesting;
static unsigned int mie_enabled;
static unsigned int disable_calls;
static unsigned int enable_calls;
static unsigned int try_calls;
static unsigned int unlock_calls;
static unsigned int external_effect_calls;
static unsigned int event_count;
static unsigned int disable_event;
static unsigned int try_first_event;
static unsigned int unlock_event;
static unsigned int enable_event;
static spinlock_t *expected_lock;
static spinlock_t *held_lock;
static enum p4_public_spinlock_result try_script[8];
static unsigned int try_script_count;
static unsigned int try_script_pos;
static enum p4_public_spinlock_result try_default;
static enum p4_public_spinlock_result unlock_result;
static enum p4_public_spinlock_result fatal_reason;
static unsigned int fatal_nesting;
static unsigned int fatal_mie;
static jmp_buf fatal_env;
static int fatal_armed;
static unsigned long checks;

void Disable(void);
void Enable(void);

static void check_irq_masked(void)
{
    if (mie_enabled != 0u)
    {
        (void)fprintf(stderr, "mock spin service called with MIE enabled\n");
        abort();
    }
}

void Disable(void)
{
    ++disable_calls;
    disable_event = ++event_count;
    ++interrupt_nesting;
    mie_enabled = 0u;
}

void Enable(void)
{
    ++enable_calls;
    enable_event = ++event_count;
    if (interrupt_nesting == 0u)
    {
        (void)fprintf(stderr, "unbalanced mock Enable\n");
        abort();
    }
    --interrupt_nesting;
    if (interrupt_nesting == 0u)
        mie_enabled = 1u;
}

enum p4_public_spinlock_result krnP4PublicSpinTryAcquire(
    spinlock_t *lock, uint32_t mode)
{
    enum p4_public_spinlock_result result;

    ++try_calls;
    if (try_first_event == 0u)
        try_first_event = ++event_count;
    else
        ++event_count;
    check_irq_masked();
    if (lock == NULL || lock != expected_lock ||
        mode != SPINLOCK_MODE_WRITE || held_lock != NULL)
        return P4_PUBLIC_SPIN_BAD_STATE;

    if (try_script_pos < try_script_count)
        result = try_script[try_script_pos++];
    else
        result = try_default;

    if (result == P4_PUBLIC_SPIN_OK)
    {
        lock->lock = UINT32_C(1);
        lock->s_Owner = UINT32_C(0x51);
        held_lock = lock;
    }
    return result;
}

enum p4_public_spinlock_result krnP4PublicSpinUnlock(spinlock_t *lock)
{
    ++unlock_calls;
    unlock_event = ++event_count;
    check_irq_masked();
    if (lock == NULL || lock != expected_lock || held_lock != lock)
        return P4_PUBLIC_SPIN_BAD_STATE;
    if (unlock_result != P4_PUBLIC_SPIN_OK)
        return unlock_result;

    lock->lock = 0u;
    lock->s_Owner = 0u;
    held_lock = NULL;
    return P4_PUBLIC_SPIN_OK;
}

void krnP4PublicSpinFatal(enum p4_public_spinlock_result reason)
{
    fatal_reason = reason;
    fatal_nesting = interrupt_nesting;
    fatal_mie = mie_enabled;
    if (!fatal_armed)
        abort();
    longjmp(fatal_env, 1);
}

#include "../p4_semaphore_gate.c"

#define CHECK(condition) do { \
    ++checks; \
    if (!(condition)) { \
        (void)fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); \
        return 0; \
    } \
} while (0)

static struct SignalSemaphore semaphores[2];
static struct ExecBase exec_base;

static void reset_fixture(struct SignalSemaphore *sem,
                          unsigned int initial_nesting,
                          unsigned int initial_mie)
{
    unsigned int i;

    (void)memset(sem, 0, sizeof(*sem));
    for (i = 0; i < 4u; ++i)
    {
        sem->before[i] = UINT32_C(0x1000) + i;
        sem->after[i] = UINT32_C(0x2000) + i;
    }
    for (i = 0; i < 3u; ++i)
    {
        sem->ss_MultipleLink.before[i] = UINT32_C(0x3000) + i;
        sem->ss_MultipleLink.after[i] = UINT32_C(0x4000) + i;
    }
    interrupt_nesting = initial_nesting;
    mie_enabled = initial_mie;
    disable_calls = 0u;
    enable_calls = 0u;
    try_calls = 0u;
    unlock_calls = 0u;
    external_effect_calls = 0u;
    event_count = 0u;
    disable_event = 0u;
    try_first_event = 0u;
    unlock_event = 0u;
    enable_event = 0u;
    expected_lock = &sem->ss_MultipleLink.sr_SpinLock;
    held_lock = NULL;
    try_script_count = 0u;
    try_script_pos = 0u;
    try_default = P4_PUBLIC_SPIN_OK;
    unlock_result = P4_PUBLIC_SPIN_OK;
    fatal_reason = P4_PUBLIC_SPIN_OK;
    fatal_nesting = 0u;
    fatal_mie = 0u;
}

static int expect_lock_fatal(struct SignalSemaphore *sem,
                             enum p4_public_spinlock_result reason)
{
    fatal_armed = 1;
    if (setjmp(fatal_env) == 0)
    {
        Exec_P4SemaphoreLock(sem, &exec_base);
        fatal_armed = 0;
        CHECK(0);
    }
    fatal_armed = 0;
    CHECK(fatal_reason == reason);
    CHECK(fatal_mie == 0u);
    CHECK(disable_calls == 1u);
    CHECK(enable_calls == 0u);
    CHECK(interrupt_nesting == fatal_nesting);
    return 1;
}

static int expect_unlock_fatal(struct SignalSemaphore *sem,
                               enum p4_public_spinlock_result reason)
{
    fatal_armed = 1;
    if (setjmp(fatal_env) == 0)
    {
        Exec_P4SemaphoreUnlock(sem, &exec_base);
        fatal_armed = 0;
        CHECK(0);
    }
    fatal_armed = 0;
    CHECK(fatal_reason == reason);
    CHECK(fatal_mie == 0u);
    CHECK(enable_calls == 0u);
    CHECK(interrupt_nesting == fatal_nesting);
    return 1;
}

static int test_retry_then_unlock_and_field_preservation(void)
{
    struct SignalSemaphore before;
    struct SignalSemaphore *sem = &semaphores[0];

    reset_fixture(sem, 0u, 1u);
    before = *sem;
    try_script[0] = P4_PUBLIC_SPIN_BUSY;
    try_script[1] = P4_PUBLIC_SPIN_TRY_TIMEOUT;
    try_script_count = 2u;

    Exec_P4SemaphoreLock(sem, &exec_base);
    CHECK(disable_calls == 1u && enable_calls == 0u);
    CHECK(interrupt_nesting == 1u && mie_enabled == 0u);
    CHECK(try_calls == 3u && held_lock == expected_lock);
    CHECK(disable_event < try_first_event);
    CHECK(sem->ss_MultipleLink.sr_SpinLock.lock == 1u);
    CHECK(sem->ss_MultipleLink.sr_SpinLock.s_Owner == UINT32_C(0x51));

    Exec_P4SemaphoreUnlock(sem, &exec_base);
    CHECK(unlock_calls == 1u && enable_calls == 1u);
    CHECK(unlock_event < enable_event);
    CHECK(interrupt_nesting == 0u && mie_enabled == 1u);
    CHECK(held_lock == NULL);
    CHECK(memcmp(sem, &before, sizeof(*sem)) == 0);
    CHECK(external_effect_calls == 0u);
    return 1;
}

static int test_existing_irq_nesting_is_preserved(void)
{
    struct SignalSemaphore *sem = &semaphores[0];

    reset_fixture(sem, 3u, 0u);
    Exec_P4SemaphoreLock(sem, &exec_base);
    CHECK(interrupt_nesting == 4u && mie_enabled == 0u);
    CHECK(disable_calls == 1u && enable_calls == 0u);
    Exec_P4SemaphoreUnlock(sem, &exec_base);
    CHECK(interrupt_nesting == 3u && mie_enabled == 0u);
    CHECK(disable_calls == 1u && enable_calls == 1u);
    CHECK(try_calls == 1u && unlock_calls == 1u);
    return 1;
}

static int test_distinct_semaphore_lock_addresses(void)
{
    struct SignalSemaphore *first = &semaphores[0];
    struct SignalSemaphore *second = &semaphores[1];

    reset_fixture(first, 0u, 1u);
    Exec_P4SemaphoreLock(first, &exec_base);
    CHECK(held_lock == &first->ss_MultipleLink.sr_SpinLock);
    Exec_P4SemaphoreUnlock(first, &exec_base);

    reset_fixture(second, 0u, 1u);
    Exec_P4SemaphoreLock(second, &exec_base);
    CHECK(held_lock == &second->ss_MultipleLink.sr_SpinLock);
    CHECK(held_lock != &first->ss_MultipleLink.sr_SpinLock);
    Exec_P4SemaphoreUnlock(second, &exec_base);
    CHECK(interrupt_nesting == 0u && mie_enabled == 1u);
    return 1;
}

static int test_unexpected_acquire_result_fails_closed(void)
{
    enum p4_public_spinlock_result result;

    for (result = P4_PUBLIC_SPIN_CLAIMED_FAILURE;
         result <= P4_PUBLIC_SPIN_SERVICE_ERROR;
         result = (enum p4_public_spinlock_result)(result + 1))
    {
        reset_fixture(&semaphores[0], 2u, 0u);
        try_default = result;
        CHECK(expect_lock_fatal(&semaphores[0], result));
        CHECK(try_calls == 1u);
        CHECK(held_lock == NULL);
    }
    return 1;
}

static int test_retry_bound_is_exact(void)
{
    reset_fixture(&semaphores[0], 0u, 1u);
    try_default = P4_PUBLIC_SPIN_BUSY;
    CHECK(expect_lock_fatal(&semaphores[0], P4_PUBLIC_SPIN_SERVICE_TIMEOUT));
    CHECK(try_calls == 20000u);
    CHECK(try_script_pos == 0u);
    CHECK(interrupt_nesting == 1u && mie_enabled == 0u);
    return 1;
}

static int test_unlock_failure_does_not_enable(void)
{
    reset_fixture(&semaphores[0], 0u, 1u);
    Exec_P4SemaphoreLock(&semaphores[0], &exec_base);
    unlock_result = P4_PUBLIC_SPIN_RELEASE_FAILURE;
    CHECK(expect_unlock_fatal(&semaphores[0],
                              P4_PUBLIC_SPIN_RELEASE_FAILURE));
    CHECK(unlock_calls == 1u);
    CHECK(held_lock == expected_lock);
    CHECK(interrupt_nesting == 1u && mie_enabled == 0u);
    return 1;
}

static int test_null_arguments_fail_masked(void)
{
    reset_fixture(&semaphores[0], 0u, 1u);
    CHECK(expect_lock_fatal(NULL, P4_PUBLIC_SPIN_BAD_ARGUMENT));
    CHECK(try_calls == 0u && unlock_calls == 0u);

    reset_fixture(&semaphores[0], 2u, 0u);
    CHECK(expect_unlock_fatal(NULL, P4_PUBLIC_SPIN_BAD_ARGUMENT));
    CHECK(try_calls == 0u && unlock_calls == 0u);
    CHECK(interrupt_nesting == 3u && mie_enabled == 0u);
    return 1;
}

int main(void)
{
    if (!test_retry_then_unlock_and_field_preservation() ||
        !test_existing_irq_nesting_is_preserved() ||
        !test_distinct_semaphore_lock_addresses() ||
        !test_unexpected_acquire_result_fails_closed() ||
        !test_retry_bound_is_exact() ||
        !test_unlock_failure_does_not_enable() ||
        !test_null_arguments_fail_masked())
        return EXIT_FAILURE;

    (void)printf("PASS: %lu semaphore gate checks\n", checks);
    return EXIT_SUCCESS;
}
