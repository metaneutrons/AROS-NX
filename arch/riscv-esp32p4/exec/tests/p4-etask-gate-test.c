/*
 * Actual-source host fixture for the P4 ETask serialization gate.
 * P4_ETASK_GATE_TEST suppresses only target/platform includes. The production
 * gate C body is included verbatim; CPU/runtime/public-spin boundaries are
 * deterministic mocks, not a hardware or SMP-runtime test.
 */
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "../p4_etask_gate.h"

#define P4_ETASK_GATE_TEST 1
#define P4_SRAMDATA
#define MSTATUS_MIE UINT32_C(8)
#define SPINLOCK_MODE_WRITE UINT32_C(1)

enum p4_public_spinlock_result
{
    P4_PUBLIC_SPIN_OK = 0,
    P4_PUBLIC_SPIN_BUSY,
    P4_PUBLIC_SPIN_TRY_TIMEOUT,
    P4_PUBLIC_SPIN_CLAIMED_FAILURE,
    P4_PUBLIC_SPIN_RELEASE_FAILURE,
    P4_PUBLIC_SPIN_SERVICE_TIMEOUT,
    P4_PUBLIC_SPIN_BAD_ARGUMENT,
    P4_PUBLIC_SPIN_BAD_MODE,
    P4_PUBLIC_SPIN_BAD_ADDRESS,
    P4_PUBLIC_SPIN_BAD_STATE,
    P4_PUBLIC_SPIN_READ_OVERFLOW,
    P4_PUBLIC_SPIN_NOT_PREPARED,
    P4_PUBLIC_SPIN_SERVICE_ERROR
};

typedef struct spinlock
{
    uint32_t lock;
    uint32_t owner;
} spinlock_t;

struct ExecBase
{
    uint32_t cookie;
};

struct p4_cpu_local
{
    uint32_t hart_id;
};

struct p4_exec_runtime
{
    uint32_t hart_id;
    struct p4_cpu_local *cpu_local;
};

static struct ExecBase expected_base;
static struct p4_exec_runtime *runtime_result;
static struct p4_cpu_local cpu_local;
static uint32_t mock_hart;
static uint32_t mock_mstatus;
static uint32_t old_status_seen;
static unsigned int mask_calls;
static unsigned int restore_calls;
static unsigned int fence_calls;
static unsigned int runtime_calls;
static unsigned int init_calls;
static unsigned int try_calls;
static unsigned int unlock_calls;
static unsigned int event_counter;
static unsigned int unlock_event;
static unsigned int restore_event;
static int gate_held;
static int mock_violation;
static enum p4_public_spinlock_result init_result = P4_PUBLIC_SPIN_OK;
static enum p4_public_spinlock_result try_default = P4_PUBLIC_SPIN_OK;
static enum p4_public_spinlock_result unlock_result = P4_PUBLIC_SPIN_OK;
static enum p4_public_spinlock_result fatal_reason;
static enum p4_public_spinlock_result try_script[8];
static unsigned int try_script_count;
static unsigned int try_script_index;
static spinlock_t *initialized_lock;
static jmp_buf fatal_env;
static int fatal_armed;
static unsigned long checks;

static uint32_t mock_csr_read(unsigned int csr);
static uint32_t mock_read_hart(void);
static void mock_mask(uint32_t *old_status);
static void mock_restore(void);
static void mock_fence(void);
static struct p4_exec_runtime *p4_exec_runtime_current(
    struct ExecBase *base);
enum p4_public_spinlock_result krnP4PublicSpinInit(spinlock_t *lock);
enum p4_public_spinlock_result krnP4PublicSpinTryAcquire(
    spinlock_t *lock, uint32_t mode);
enum p4_public_spinlock_result krnP4PublicSpinUnlock(spinlock_t *lock);
void krnP4PublicSpinFatal(enum p4_public_spinlock_result reason)
    __attribute__((noreturn));

#define mstatus 0
#define csr_read(csr) mock_csr_read((csr))
#define P4_CPU_LOCAL_READ_MHARTID() mock_read_hart()
#define P4_ETASK_GATE_MASK(status) mock_mask(&(status))
#define P4_ETASK_GATE_RESTORE() mock_restore()
#define P4_ETASK_GATE_FENCE() mock_fence()

static uint32_t mock_csr_read(unsigned int csr)
{
    (void)csr;
    return mock_mstatus;
}

static uint32_t mock_read_hart(void)
{
    return mock_hart;
}

static void mock_mask(uint32_t *old_status)
{
    ++mask_calls;
    old_status_seen = mock_mstatus;
    *old_status = mock_mstatus;
    mock_mstatus &= ~MSTATUS_MIE;
}

static void mock_restore(void)
{
    ++restore_calls;
    restore_event = ++event_counter;
    if (gate_held)
        mock_violation = 1;
    mock_mstatus |= MSTATUS_MIE;
}

static void mock_fence(void)
{
    ++fence_calls;
}

static struct p4_exec_runtime *p4_exec_runtime_current(struct ExecBase *base)
{
    ++runtime_calls;
    if (mock_mstatus & MSTATUS_MIE)
        mock_violation = 1;
    if (base != &expected_base)
        return NULL;
    return runtime_result;
}

enum p4_public_spinlock_result krnP4PublicSpinInit(spinlock_t *lock)
{
    ++init_calls;
    if ((mock_mstatus & MSTATUS_MIE) != 0 || lock == NULL ||
        init_result != P4_PUBLIC_SPIN_OK)
        return init_result == P4_PUBLIC_SPIN_OK ?
            P4_PUBLIC_SPIN_BAD_STATE : init_result;
    initialized_lock = lock;
    lock->lock = 0;
    lock->owner = 0;
    return P4_PUBLIC_SPIN_OK;
}

enum p4_public_spinlock_result krnP4PublicSpinTryAcquire(
    spinlock_t *lock, uint32_t mode)
{
    enum p4_public_spinlock_result result;
    ++try_calls;
    if ((mock_mstatus & MSTATUS_MIE) != 0 || lock == NULL ||
        lock != initialized_lock || mode != SPINLOCK_MODE_WRITE || gate_held)
    {
        mock_violation = 1;
        return P4_PUBLIC_SPIN_BAD_STATE;
    }
    if (try_script_index < try_script_count)
        result = try_script[try_script_index++];
    else
        result = try_default;
    if (result == P4_PUBLIC_SPIN_OK)
    {
        if (lock->lock != 0)
            return P4_PUBLIC_SPIN_BAD_STATE;
        lock->lock = 1;
        gate_held = 1;
    }
    return result;
}

enum p4_public_spinlock_result krnP4PublicSpinUnlock(spinlock_t *lock)
{
    ++unlock_calls;
    if ((mock_mstatus & MSTATUS_MIE) != 0 || lock == NULL ||
        lock != initialized_lock || !gate_held)
    {
        mock_violation = 1;
        return P4_PUBLIC_SPIN_BAD_STATE;
    }
    if (unlock_result != P4_PUBLIC_SPIN_OK)
        return unlock_result;
    lock->lock = 0;
    gate_held = 0;
    unlock_event = ++event_counter;
    return P4_PUBLIC_SPIN_OK;
}

void krnP4PublicSpinFatal(enum p4_public_spinlock_result reason)
{
    fatal_reason = reason;
    if (!fatal_armed)
        abort();
    longjmp(fatal_env, 1);
}

#include "../p4_etask_gate.c"

#define CHECK(condition) do { \
    ++checks; \
    if (!(condition)) { \
        fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); \
        return 0; \
    } \
} while (0)

static void reset_mocks(void)
{
    runtime_result = NULL;
    mock_hart = 0;
    mock_mstatus = 0;
    old_status_seen = 0;
    mask_calls = 0;
    restore_calls = 0;
    runtime_calls = 0;
    init_calls = 0;
    try_calls = 0;
    unlock_calls = 0;
    event_counter = 0;
    unlock_event = 0;
    restore_event = 0;
    gate_held = 0;
    mock_violation = 0;
    init_result = P4_PUBLIC_SPIN_OK;
    try_default = P4_PUBLIC_SPIN_OK;
    unlock_result = P4_PUBLIC_SPIN_OK;
    try_script_count = 0;
    try_script_index = 0;
    initialized_lock = etask_gate_prepared ? &etask_gate : NULL;
}

static int test_prepare_preconditions_and_publication(void)
{
    reset_mocks();
    mock_hart = 1;
    CHECK(!krnP4ETaskGatePrepare());
    CHECK(init_calls == 0 && fence_calls == 0);
    mock_hart = 0;
    mock_mstatus = MSTATUS_MIE | UINT32_C(0x100);
    CHECK(!krnP4ETaskGatePrepare());
    CHECK(init_calls == 0 && fence_calls == 0);
    mock_mstatus = UINT32_C(0x100);
    init_result = P4_PUBLIC_SPIN_SERVICE_TIMEOUT;
    CHECK(!krnP4ETaskGatePrepare());
    CHECK(init_calls == 1 && fence_calls == 0);
    CHECK(etask_gate_prepared == 0);
    init_result = P4_PUBLIC_SPIN_OK;
    CHECK(krnP4ETaskGatePrepare());
    CHECK(init_calls == 2 && fence_calls == 1);
    CHECK(etask_gate_prepared == 1);
    CHECK(mock_mstatus == UINT32_C(0x100));
    CHECK(!krnP4ETaskGatePrepare());
    CHECK(init_calls == 2);
    CHECK(mock_violation == 0);
    return 1;
}

static void install_valid_runtime(uint32_t hart)
{
    cpu_local.hart_id = hart;
    static struct p4_exec_runtime valid_runtime;
    valid_runtime.hart_id = hart;
    valid_runtime.cpu_local = &cpu_local;
    runtime_result = &valid_runtime;
}

static int test_lock_mie_zero_and_enabled_restore(void)
{
    uint32_t token;
    reset_mocks();
    mock_hart = 0;
    install_valid_runtime(0);
    mock_mstatus = UINT32_C(0x100);
    token = krnP4ETaskGateLock(&expected_base);
    CHECK(token == 0);
    CHECK(gate_held && mock_mstatus == UINT32_C(0x100));
    CHECK(mask_calls == 1 && old_status_seen == UINT32_C(0x100));
    CHECK(runtime_calls == 1 && try_calls == 1);
    krnP4ETaskGateUnlock(token);
    CHECK(!gate_held && unlock_calls == 1);
    CHECK(mock_mstatus == UINT32_C(0x100));
    CHECK(restore_calls == 0 && mock_violation == 0);

    reset_mocks();
    mock_hart = 0;
    install_valid_runtime(0);
    mock_mstatus = MSTATUS_MIE | UINT32_C(0x100);
    try_script[0] = P4_PUBLIC_SPIN_BUSY;
    try_script[1] = P4_PUBLIC_SPIN_TRY_TIMEOUT;
    try_script[2] = P4_PUBLIC_SPIN_OK;
    try_script_count = 3;
    token = krnP4ETaskGateLock(&expected_base);
    CHECK(token == MSTATUS_MIE);
    CHECK(gate_held && mock_mstatus == UINT32_C(0x100));
    CHECK(try_calls == 3 && runtime_calls == 1);
    krnP4ETaskGateUnlock(token);
    CHECK(!gate_held && unlock_calls == 1);
    CHECK(mock_mstatus == (MSTATUS_MIE | UINT32_C(0x100)));
    CHECK(restore_calls == 1 && unlock_event < restore_event);
    CHECK(mock_violation == 0);
    return 1;
}

static int test_invalid_runtime_fails_closed(void)
{
    reset_mocks();
    mock_hart = 0;
    mock_mstatus = MSTATUS_MIE | UINT32_C(0x200);
    runtime_result = NULL;
    fatal_armed = 1;
    if (setjmp(fatal_env) == 0)
    {
        (void)krnP4ETaskGateLock(&expected_base);
        fatal_armed = 0;
        CHECK(0);
    }
    fatal_armed = 0;
    CHECK(fatal_reason == P4_PUBLIC_SPIN_BAD_STATE);
    CHECK(mock_mstatus == UINT32_C(0x200));
    CHECK(try_calls == 0 && restore_calls == 0 && !gate_held);

    reset_mocks();
    mock_hart = 0;
    install_valid_runtime(0);
    fatal_armed = 1;
    if (setjmp(fatal_env) == 0)
    {
        (void)krnP4ETaskGateLock(NULL);
        fatal_armed = 0;
        CHECK(0);
    }
    fatal_armed = 0;
    CHECK(fatal_reason == P4_PUBLIC_SPIN_BAD_STATE);
    CHECK(try_calls == 0 && mock_mstatus == 0 && !gate_held);
    return 1;
}

static int test_bad_runtime_identity_fails_closed(void)
{
    reset_mocks();
    mock_hart = 0;
    install_valid_runtime(0);
    cpu_local.hart_id = 1;
    fatal_armed = 1;
    if (setjmp(fatal_env) == 0)
    {
        (void)krnP4ETaskGateLock(&expected_base);
        fatal_armed = 0;
        CHECK(0);
    }
    fatal_armed = 0;
    CHECK(fatal_reason == P4_PUBLIC_SPIN_BAD_STATE);
    CHECK(try_calls == 0 && mock_mstatus == 0);
    return 1;
}

static int test_outer_retry_bound_and_fatal_results(void)
{
    reset_mocks();
    mock_hart = 0;
    install_valid_runtime(0);
    try_default = P4_PUBLIC_SPIN_BUSY;
    fatal_armed = 1;
    if (setjmp(fatal_env) == 0)
    {
        (void)krnP4ETaskGateLock(&expected_base);
        fatal_armed = 0;
        CHECK(0);
    }
    fatal_armed = 0;
    CHECK(fatal_reason == P4_PUBLIC_SPIN_SERVICE_TIMEOUT);
    CHECK(try_calls == 20000u && mock_mstatus == 0 && !gate_held);
    CHECK(restore_calls == 0 && mock_violation == 0);

    reset_mocks();
    mock_hart = 0;
    install_valid_runtime(0);
    try_default = P4_PUBLIC_SPIN_CLAIMED_FAILURE;
    fatal_armed = 1;
    if (setjmp(fatal_env) == 0)
    {
        (void)krnP4ETaskGateLock(&expected_base);
        fatal_armed = 0;
        CHECK(0);
    }
    fatal_armed = 0;
    CHECK(fatal_reason == P4_PUBLIC_SPIN_CLAIMED_FAILURE);
    CHECK(try_calls == 1 && mock_mstatus == 0);
    return 1;
}

static int test_invalid_token_and_unlock_failure(void)
{
    reset_mocks();
    fatal_armed = 1;
    if (setjmp(fatal_env) == 0)
    {
        krnP4ETaskGateUnlock(MSTATUS_MIE | UINT32_C(1));
        fatal_armed = 0;
        CHECK(0);
    }
    fatal_armed = 0;
    CHECK(fatal_reason == P4_PUBLIC_SPIN_BAD_STATE);
    CHECK(unlock_calls == 0 && restore_calls == 0);

    reset_mocks();
    mock_hart = 0;
    install_valid_runtime(0);
    (void)krnP4ETaskGateLock(&expected_base);
    CHECK(gate_held);
    unlock_result = P4_PUBLIC_SPIN_RELEASE_FAILURE;
    fatal_armed = 1;
    if (setjmp(fatal_env) == 0)
    {
        krnP4ETaskGateUnlock(0);
        fatal_armed = 0;
        CHECK(0);
    }
    fatal_armed = 0;
    CHECK(fatal_reason == P4_PUBLIC_SPIN_RELEASE_FAILURE);
    CHECK(gate_held && mock_mstatus == 0 && restore_calls == 0);
    return 1;
}

int main(void)
{
    if (!test_prepare_preconditions_and_publication() ||
        !test_lock_mie_zero_and_enabled_restore() ||
        !test_invalid_runtime_fails_closed() ||
        !test_bad_runtime_identity_fails_closed() ||
        !test_outer_retry_bound_and_fatal_results() ||
        !test_invalid_token_and_unlock_failure())
        return 1;

    printf("P4 ETask gate actual-source host fixture: %lu checks passed\n",
           checks);
    return 0;
}
