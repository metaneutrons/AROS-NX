/* Host regression for the production P4 public-spinlock adapter helpers. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define P4_PUBLIC_SPINLOCK_TEST 1

static uint32_t mock_hart_id;
#define P4_CPU_LOCAL_READ_MHARTID mock_read_hart_id
static uint32_t mock_read_hart_id(void)
{
    return mock_hart_id;
}

#include "../cpu_local.h"

struct ExecBase { uint32_t marker; };
struct Task { uint32_t marker; };
struct KernelBase { uint32_t marker; };

static struct ExecBase test_sysbase;
static struct Task test_task;
static struct KernelBase test_kernelbase;
struct ExecBase *SysBase;
struct p4_cpu_local __p4_cpu_locals[P4_CPU_LOCAL_HART_COUNT];

struct p4_cpu_local *p4_cpu_local_for_hart(uint32_t hart_id)
{
    return hart_id < P4_CPU_LOCAL_HART_COUNT ? &__p4_cpu_locals[hart_id] :
        NULL;
}

struct p4_cpu_local *p4_cpu_local_current(void)
{
    return p4_cpu_local_for_hart(mock_read_hart_id());
}

#define P4_PUBLIC_OWNER_TO_WORD(owner) \
    ((owner) == NULL ? 0u : UINT32_C(0x51a7c0de))

#include "../p4_public_spinlock.h"
#include "../p4_soft_atomic.h"

#define CHECK(condition) do { \
    ++check_count; \
    if (!(condition)) { \
        fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); \
        return 0; \
    } \
} while (0)

static unsigned long check_count;
static spinlock_t test_lock;
static enum p4_soft_atomic_result forced_results[128];
static uint32_t forced_count;
static uint32_t forced_index;
static uint32_t service_calls;
static uint32_t load_calls;
static uint32_t store_calls;
static uint32_t cas_calls;
static uint32_t state_calls;
static uint32_t owner_calls;
static int service_active;
static int hook_ran_inside_service;
static int mutate_state_before_cas;
static uint32_t mutate_state_value;

static enum p4_soft_atomic_result mock_next_result(void)
{
    if (forced_index < forced_count)
        return forced_results[forced_index++];
    return P4_SOFT_ATOMIC_OK;
}

static void record_target(volatile void *address, uint32_t width)
{
    ++service_calls;
    if (width != sizeof(uint32_t))
        return;
    if (address == (volatile void *)&test_lock.lock)
        ++state_calls;
    else if (address == (volatile void *)&test_lock.s_Owner)
        ++owner_calls;
}

enum p4_soft_atomic_result krnP4SoftAtomicOperate(
    volatile void *address, uint32_t width,
    enum p4_soft_atomic_operation operation, uint32_t operand,
    uint32_t *old_value)
{
    enum p4_soft_atomic_result result;
    volatile uint32_t *target = (volatile uint32_t *)address;
    uint32_t before;

    record_target(address, width);
    service_active = 1;
    result = mock_next_result();
    if (result != P4_SOFT_ATOMIC_OK)
    {
        service_active = 0;
        return result;
    }
    if (address == (volatile void *)&test_lock.lock ||
        address == (volatile void *)&test_lock.s_Owner)
    {
        /* Both ABI words must be reached through this mocked service. */
    }
    else
    {
        service_active = 0;
        return P4_SOFT_ATOMIC_BAD_ADDRESS;
    }
    if (width != sizeof(uint32_t) || old_value == NULL)
    {
        service_active = 0;
        return P4_SOFT_ATOMIC_BAD_WIDTH;
    }

    before = *target;
    if (operation == P4_SOFT_ATOMIC_LOAD)
        ++load_calls;
    else if (operation == P4_SOFT_ATOMIC_STORE)
    {
        ++store_calls;
        *target = operand;
    }
    else
    {
        service_active = 0;
        return P4_SOFT_ATOMIC_BAD_OPERATION;
    }
    *old_value = before;
    service_active = 0;
    return P4_SOFT_ATOMIC_OK;
}

enum p4_soft_atomic_result krnP4SoftAtomicCompareExchange(
    volatile void *address, uint32_t width, uint32_t expected,
    uint32_t desired, uint32_t *old_value)
{
    enum p4_soft_atomic_result result;
    volatile uint32_t *target = (volatile uint32_t *)address;
    uint32_t before;

    record_target(address, width);
    service_active = 1;
    result = mock_next_result();
    if (result != P4_SOFT_ATOMIC_OK)
    {
        service_active = 0;
        return result;
    }
    if (address != (volatile void *)&test_lock.lock)
    {
        service_active = 0;
        return P4_SOFT_ATOMIC_BAD_ADDRESS;
    }
    if (width != sizeof(uint32_t) || old_value == NULL)
    {
        service_active = 0;
        return P4_SOFT_ATOMIC_BAD_WIDTH;
    }

    ++cas_calls;
    if (mutate_state_before_cas)
    {
        *target = mutate_state_value;
        mutate_state_before_cas = 0;
    }
    before = *target;
    if (before == expected)
        *target = desired;
    *old_value = before;
    service_active = 0;
    return P4_SOFT_ATOMIC_OK;
}

#include "../p4_public_spinlock.c"

static void set_forced(const enum p4_soft_atomic_result *results,
                       uint32_t count)
{
    uint32_t i;

    forced_count = count;
    forced_index = 0;
    for (i = 0; i < count; ++i)
        forced_results[i] = results[i];
}

static void reset_mock(void)
{
    forced_count = 0;
    forced_index = 0;
    service_calls = 0;
    load_calls = 0;
    store_calls = 0;
    cas_calls = 0;
    state_calls = 0;
    owner_calls = 0;
    service_active = 0;
    hook_ran_inside_service = 0;
    mutate_state_before_cas = 0;
    mutate_state_value = 0;
}

static void setup(uint32_t state)
{
    memset(&test_lock, 0, sizeof(test_lock));
    test_lock.lock = state;
    memset(__p4_cpu_locals, 0, sizeof(__p4_cpu_locals));
    SysBase = &test_sysbase;
    mock_hart_id = 0;
    __p4_cpu_locals[0].hart_id = 0;
    __p4_cpu_locals[0].sys_base = SysBase;
    __p4_cpu_locals[0].kernel_base = &test_kernelbase;
    __p4_cpu_locals[0].current_task = &test_task;
    reset_mock();
}

static int test_init_and_nested_readers(void)
{
    int held = -1;

    setup(UINT32_C(0xdeadbeef));
    SysBase = NULL; /* Init must not require a current task or SysBase. */
    CHECK(krnP4PublicSpinInit(&test_lock) == P4_PUBLIC_SPIN_OK);
    CHECK(test_lock.lock == 0u && test_lock.s_Owner == 0u);
    CHECK(store_calls == 2u && owner_calls == 1u && state_calls == 1u);

    CHECK(krnP4PublicSpinTryAcquire(&test_lock, SPINLOCK_MODE_READ) ==
          P4_PUBLIC_SPIN_OK);
    CHECK(krnP4PublicSpinTryAcquire(&test_lock, SPINLOCK_MODE_READ) ==
          P4_PUBLIC_SPIN_OK);
    CHECK(krnP4PublicSpinTryAcquire(&test_lock, SPINLOCK_MODE_READ) ==
          P4_PUBLIC_SPIN_OK);
    CHECK(test_lock.lock == 3u);
    CHECK(krnP4PublicSpinTryAcquire(&test_lock, SPINLOCK_MODE_WRITE) ==
          P4_PUBLIC_SPIN_BUSY);
    CHECK(test_lock.lock == 3u && test_lock.s_Owner == 0u);
    CHECK(krnP4PublicSpinIsLocked(&test_lock, &held) == P4_PUBLIC_SPIN_OK);
    CHECK(held == 1);
    CHECK(krnP4PublicSpinUnlock(&test_lock) == P4_PUBLIC_SPIN_OK);
    CHECK(test_lock.lock == 2u);
    CHECK(krnP4PublicSpinUnlock(&test_lock) == P4_PUBLIC_SPIN_OK);
    CHECK(test_lock.lock == 1u);
    CHECK(krnP4PublicSpinUnlock(&test_lock) == P4_PUBLIC_SPIN_OK);
    CHECK(test_lock.lock == 0u);
    CHECK(krnP4PublicSpinIsLocked(&test_lock, &held) == P4_PUBLIC_SPIN_OK);
    CHECK(held == 0);
    CHECK(krnP4PublicSpinUnlock(&test_lock) == P4_PUBLIC_SPIN_BAD_STATE);
    return 1;
}

static int test_writer_owner_and_exclusion(void)
{
    int held = 0;

    setup(0u);
    CHECK(krnP4PublicSpinTryAcquire(&test_lock, SPINLOCK_MODE_WRITE) ==
          P4_PUBLIC_SPIN_OK);
    CHECK(test_lock.lock == SPINLOCKF_WRITE);
    CHECK(test_lock.s_Owner == UINT32_C(0x51a7c0de));
    CHECK(krnP4PublicSpinIsLocked(&test_lock, &held) == P4_PUBLIC_SPIN_OK);
    CHECK(held == 1);
    CHECK(krnP4PublicSpinTryAcquire(&test_lock, SPINLOCK_MODE_READ) ==
          P4_PUBLIC_SPIN_BUSY);
    CHECK(krnP4PublicSpinTryAcquire(&test_lock, SPINLOCK_MODE_WRITE) ==
          P4_PUBLIC_SPIN_BUSY);
    CHECK(krnP4PublicSpinUnlock(&test_lock) == P4_PUBLIC_SPIN_OK);
    CHECK(test_lock.lock == 0u && test_lock.s_Owner == 0u);
    return 1;
}

static int test_owner_null_contexts(void)
{
    setup(0u);
    SysBase = NULL;
    CHECK(krnP4PublicSpinTryAcquire(&test_lock, SPINLOCK_MODE_WRITE) ==
          P4_PUBLIC_SPIN_OK);
    CHECK(test_lock.s_Owner == 0u);
    CHECK(krnP4PublicSpinUnlock(&test_lock) == P4_PUBLIC_SPIN_OK);

    setup(0u);
    __p4_cpu_locals[0].trap_depth = 1u;
    CHECK(krnP4PublicSpinTryAcquire(&test_lock, SPINLOCK_MODE_WRITE) ==
          P4_PUBLIC_SPIN_OK);
    CHECK(test_lock.s_Owner == 0u);
    CHECK(krnP4PublicSpinUnlock(&test_lock) == P4_PUBLIC_SPIN_OK);

    setup(0u);
    mock_hart_id = P4_CPU_LOCAL_HART_COUNT;
    CHECK(krnP4PublicSpinTryAcquire(&test_lock, SPINLOCK_MODE_WRITE) ==
          P4_PUBLIC_SPIN_OK);
    CHECK(test_lock.s_Owner == 0u);
    CHECK(krnP4PublicSpinUnlock(&test_lock) == P4_PUBLIC_SPIN_OK);

    setup(0u);
    __p4_cpu_locals[0].current_task = NULL;
    CHECK(krnP4PublicSpinTryAcquire(&test_lock, SPINLOCK_MODE_WRITE) ==
          P4_PUBLIC_SPIN_OK);
    CHECK(test_lock.s_Owner == 0u);
    CHECK(krnP4PublicSpinUnlock(&test_lock) == P4_PUBLIC_SPIN_OK);

    setup(0u);
    __p4_cpu_locals[0].sys_base = NULL;
    CHECK(krnP4PublicSpinTryAcquire(&test_lock, SPINLOCK_MODE_WRITE) ==
          P4_PUBLIC_SPIN_OK);
    CHECK(test_lock.s_Owner == 0u);
    CHECK(krnP4PublicSpinUnlock(&test_lock) == P4_PUBLIC_SPIN_OK);

    setup(0u);
    __p4_cpu_locals[0].kernel_base = NULL;
    CHECK(krnP4PublicSpinTryAcquire(&test_lock, SPINLOCK_MODE_WRITE) ==
          P4_PUBLIC_SPIN_OK);
    CHECK(test_lock.s_Owner == 0u);
    CHECK(krnP4PublicSpinUnlock(&test_lock) == P4_PUBLIC_SPIN_OK);

    return 1;
}

static int test_overflow_malformed_and_transition(void)
{
    int held = 0;

    setup(UINT32_C(0x00ffffff));
    CHECK(krnP4PublicSpinTryAcquire(&test_lock, SPINLOCK_MODE_READ) ==
          P4_PUBLIC_SPIN_READ_OVERFLOW);
    CHECK(cas_calls == 0u && test_lock.lock == UINT32_C(0x00ffffff));

    setup(UINT32_C(1) << 25);
    CHECK(krnP4PublicSpinTryAcquire(&test_lock, SPINLOCK_MODE_READ) ==
          P4_PUBLIC_SPIN_BAD_STATE);
    CHECK(krnP4PublicSpinIsLocked(&test_lock, &held) ==
          P4_PUBLIC_SPIN_BAD_STATE);
    CHECK(krnP4PublicSpinUnlock(&test_lock) == P4_PUBLIC_SPIN_BAD_STATE);

    setup(SPINLOCKF_UPDATING);
    CHECK(krnP4PublicSpinIsLocked(&test_lock, &held) ==
          P4_PUBLIC_SPIN_BAD_STATE);

    setup(SPINLOCKF_WRITE | SPINLOCKF_UPDATING);
    CHECK(krnP4PublicSpinIsLocked(&test_lock, &held) == P4_PUBLIC_SPIN_OK);
    CHECK(held == 1);
    CHECK(krnP4PublicSpinTryAcquire(&test_lock, SPINLOCK_MODE_READ) ==
          P4_PUBLIC_SPIN_BUSY);
    CHECK(krnP4PublicSpinUnlock(&test_lock) == P4_PUBLIC_SPIN_BAD_STATE);
    return 1;
}

static int test_mismatch_and_reader_release_retry(void)
{
    setup(0u);
    mutate_state_before_cas = 1;
    mutate_state_value = 2u;
    CHECK(krnP4PublicSpinTryAcquire(&test_lock, SPINLOCK_MODE_READ) ==
          P4_PUBLIC_SPIN_BUSY);
    CHECK(test_lock.lock == 2u && load_calls == 1u && cas_calls == 1u);

    reset_mock();
    setup(2u);
    mutate_state_before_cas = 1;
    mutate_state_value = 3u;
    CHECK(krnP4PublicSpinUnlock(&test_lock) == P4_PUBLIC_SPIN_OK);
    CHECK(test_lock.lock == 2u);
    CHECK(cas_calls == 2u && load_calls == 2u);
    return 1;
}

struct hook_fixture
{
    uint32_t calls;
};

static uintptr_t test_hook_bridge(spinlock_t *lock, void *context)
{
    struct hook_fixture *fixture = (struct hook_fixture *)context;

    if (service_active)
        hook_ran_inside_service = 1;
    ++fixture->calls;
    if (fixture->calls == 2u)
        (void)krnP4PublicSpinUnlock(lock);
    return UINTPTR_MAX; /* A notification result cannot cancel acquisition. */
}

static int test_hook_notification_bridge(void)
{
    struct hook_fixture fixture = { 0u };
    enum p4_public_spinlock_result result;
    uint32_t attempts;

    setup(0u);
    CHECK(krnP4PublicSpinTryAcquire(&test_lock, SPINLOCK_MODE_WRITE) ==
          P4_PUBLIC_SPIN_OK);
    reset_mock();

    for (attempts = 0; attempts < 4u; ++attempts)
    {
        result = krnP4PublicSpinBlockingAttempt(
            &test_lock, SPINLOCK_MODE_WRITE, test_hook_bridge, &fixture);
        if (result == P4_PUBLIC_SPIN_OK)
            break;
        CHECK(result == P4_PUBLIC_SPIN_BUSY);
    }

    CHECK(result == P4_PUBLIC_SPIN_OK);
    CHECK(fixture.calls == 2u);
    CHECK(!hook_ran_inside_service);
    CHECK(test_lock.lock == SPINLOCKF_WRITE);
    CHECK(krnP4PublicSpinUnlock(&test_lock) == P4_PUBLIC_SPIN_OK);
    return 1;
}

static int test_preclaim_timeout_and_hard_error(void)
{
    const enum p4_soft_atomic_result load_timeout[] = {
        P4_SOFT_ATOMIC_TIMEOUT
    };
    const enum p4_soft_atomic_result cas_timeout[] = {
        P4_SOFT_ATOMIC_OK, P4_SOFT_ATOMIC_TIMEOUT
    };
    const enum p4_soft_atomic_result not_prepared[] = {
        P4_SOFT_ATOMIC_NOT_PREPARED
    };

    setup(0u);
    set_forced(load_timeout, 1u);
    CHECK(krnP4PublicSpinTryAcquire(&test_lock, SPINLOCK_MODE_READ) ==
          P4_PUBLIC_SPIN_TRY_TIMEOUT);
    CHECK(test_lock.lock == 0u);

    setup(0u);
    set_forced(cas_timeout, 2u);
    CHECK(krnP4PublicSpinTryAcquire(&test_lock, SPINLOCK_MODE_READ) ==
          P4_PUBLIC_SPIN_TRY_TIMEOUT);
    CHECK(test_lock.lock == 0u);

    setup(0u);
    set_forced(not_prepared, 1u);
    CHECK(krnP4PublicSpinTryAcquire(&test_lock, SPINLOCK_MODE_READ) ==
          P4_PUBLIC_SPIN_NOT_PREPARED);
    CHECK(test_lock.lock == 0u);
    return 1;
}

static int test_claimed_timeout_retry_and_failure(void)
{
    const enum p4_soft_atomic_result recovered[] = {
        P4_SOFT_ATOMIC_OK, P4_SOFT_ATOMIC_TIMEOUT, P4_SOFT_ATOMIC_OK,
        P4_SOFT_ATOMIC_TIMEOUT, P4_SOFT_ATOMIC_OK
    };
    enum p4_soft_atomic_result repeated[P4_PUBLIC_SPINLOCK_RETRIES + 1u];
    const enum p4_soft_atomic_result hard_after_claim[] = {
        P4_SOFT_ATOMIC_OK, P4_SOFT_ATOMIC_NOT_PREPARED
    };
    uint32_t i;

    setup(0u);
    set_forced(recovered, sizeof(recovered) / sizeof(recovered[0]));
    CHECK(krnP4PublicSpinTryAcquire(&test_lock, SPINLOCK_MODE_WRITE) ==
          P4_PUBLIC_SPIN_OK);
    CHECK(test_lock.lock == SPINLOCKF_WRITE);
    CHECK(test_lock.s_Owner == UINT32_C(0x51a7c0de));
    CHECK(krnP4PublicSpinUnlock(&test_lock) == P4_PUBLIC_SPIN_OK);

    setup(0u);
    repeated[0] = P4_SOFT_ATOMIC_OK; /* Claim CAS succeeds. */
    for (i = 1; i < P4_PUBLIC_SPINLOCK_RETRIES + 1u; ++i)
        repeated[i] = P4_SOFT_ATOMIC_TIMEOUT; /* Owner STORE keeps timing out. */
    set_forced(repeated, P4_PUBLIC_SPINLOCK_RETRIES + 1u);
    CHECK(krnP4PublicSpinTryAcquire(&test_lock, SPINLOCK_MODE_WRITE) ==
          P4_PUBLIC_SPIN_CLAIMED_FAILURE);
    CHECK(test_lock.lock == (SPINLOCKF_WRITE | SPINLOCKF_UPDATING));
    CHECK(test_lock.s_Owner == 0u);

    setup(0u);
    set_forced(hard_after_claim,
               sizeof(hard_after_claim) / sizeof(hard_after_claim[0]));
    CHECK(krnP4PublicSpinTryAcquire(&test_lock, SPINLOCK_MODE_WRITE) ==
          P4_PUBLIC_SPIN_CLAIMED_FAILURE);
    CHECK(test_lock.lock == (SPINLOCKF_WRITE | SPINLOCKF_UPDATING));
    CHECK(test_lock.s_Owner == 0u);
    return 1;
}

static int test_release_and_query_bounds(void)
{
    enum p4_soft_atomic_result repeated[P4_PUBLIC_SPINLOCK_RETRIES];
    int held = 0;
    uint32_t i;

    setup(1u);
    for (i = 0; i < P4_PUBLIC_SPINLOCK_RETRIES; ++i)
        repeated[i] = P4_SOFT_ATOMIC_TIMEOUT;
    set_forced(repeated, P4_PUBLIC_SPINLOCK_RETRIES);
    CHECK(krnP4PublicSpinUnlock(&test_lock) ==
          P4_PUBLIC_SPIN_RELEASE_FAILURE);
    CHECK(test_lock.lock == 1u);

    setup(0u);
    set_forced(repeated, P4_PUBLIC_SPINLOCK_RETRIES);
    CHECK(krnP4PublicSpinIsLocked(&test_lock, &held) ==
          P4_PUBLIC_SPIN_SERVICE_TIMEOUT);
    return 1;
}

static int test_invalid_modes_and_arguments(void)
{
    setup(0u);
    CHECK(krnP4PublicSpinTryAcquire(&test_lock, 99u) ==
          P4_PUBLIC_SPIN_BAD_MODE);
    CHECK(krnP4PublicSpinInit(NULL) == P4_PUBLIC_SPIN_BAD_ARGUMENT);
    CHECK(krnP4PublicSpinTryAcquire(NULL, SPINLOCK_MODE_READ) ==
          P4_PUBLIC_SPIN_BAD_ARGUMENT);
    CHECK(krnP4PublicSpinIsLocked(&test_lock, NULL) ==
          P4_PUBLIC_SPIN_BAD_ARGUMENT);
    return 1;
}

int main(void)
{
    if (!test_init_and_nested_readers() ||
        !test_writer_owner_and_exclusion() ||
        !test_owner_null_contexts() ||
        !test_overflow_malformed_and_transition() ||
        !test_mismatch_and_reader_release_retry() ||
        !test_hook_notification_bridge() ||
        !test_preclaim_timeout_and_hard_error() ||
        !test_claimed_timeout_retry_and_failure() ||
        !test_release_and_query_bounds() ||
        !test_invalid_modes_and_arguments())
        return 1;

    printf("PASS P4 public spinlock helper host test (%lu checks)\n",
           check_count);
    return 0;
}
