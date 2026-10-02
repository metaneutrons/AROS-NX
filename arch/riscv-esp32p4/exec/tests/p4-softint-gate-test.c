/*
 * Actual-source host fixture for the P4 shared softint gate.
 *
 * P4_SOFTINT_GATE_TEST suppresses only platform/Exec includes and replaces
 * the three local CSR/fence instructions. The p4_softint.c implementation is
 * included verbatim; runtime, public-spin and queue leaves are controlled
 * mocks so this fixture tests the wrapper's locking protocol, not hardware.
 */
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define __AROSEXEC_SMP__ 1
#define __AROSPLATFORM_SMP__ 1
#define P4_SOFTINT_GATE_TEST 1

#define MSTATUS_MIE UINT32_C(8)
#define SPINLOCK_MODE_WRITE UINT32_C(1)
#define P4_SRAMDATA

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

struct p4_softint_state
{
    uint32_t pending;
    uint32_t active;
};

struct ExecBase
{
    uint32_t cookie;
};

struct Interrupt
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

static struct ExecBase *SysBase;
static uint32_t current_hart;
static uint32_t mie_enabled;
static unsigned int fence_count;
static unsigned int mask_count;
static unsigned int restore_count;
static unsigned int init_calls;
static unsigned int try_calls;
static unsigned int unlock_calls;
static unsigned int queue_enqueue_calls;
static unsigned int queue_begin_calls;
static unsigned int queue_take_calls;
static unsigned int callback_calls;
static unsigned int busy_remaining;
static enum p4_public_spinlock_result busy_result = P4_PUBLIC_SPIN_BUSY;
static enum p4_public_spinlock_result eventual_try_result = P4_PUBLIC_SPIN_OK;
static enum p4_public_spinlock_result init_result = P4_PUBLIC_SPIN_OK;
static enum p4_public_spinlock_result unlock_result = P4_PUBLIC_SPIN_OK;
static enum p4_public_spinlock_result fatal_reason;
static int gate_held;
static spinlock_t *observed_gate;
static struct ExecBase *expected_base;
static int runtime_valid[2];
static struct p4_cpu_local cpu_local[2];
static struct p4_exec_runtime runtime[2];
static struct Interrupt *next_take;
static jmp_buf fatal_env;
static int fatal_armed;
static int fatal_gate_held;
static uint32_t fatal_mie;

#define mstatus 0
#define csr_read(csr) mock_csr_read((csr))
#define P4_CPU_LOCAL_READ_MHARTID() mock_read_hart()
#define P4_SOFTINT_FENCE() mock_fence()
#define P4_SOFTINT_MASK(status) mock_mask(&(status))
#define P4_SOFTINT_RESTORE() mock_restore()

static uint32_t mock_csr_read(unsigned int csr)
{
    (void)csr;
    return mie_enabled ? MSTATUS_MIE : 0u;
}

static uint32_t mock_read_hart(void)
{
    return current_hart;
}

static void mock_fence(void)
{
    ++fence_count;
}

static void mock_mask(uint32_t *status)
{
    *status = mie_enabled ? MSTATUS_MIE : 0u;
    mie_enabled = 0u;
    ++mask_count;
}

static void mock_restore(void)
{
    mie_enabled = 1u;
    ++restore_count;
}

static struct p4_exec_runtime *p4_exec_runtime_current(
    struct ExecBase *sys_base)
{
    if (current_hart >= 2u || !runtime_valid[current_hart] ||
        sys_base == NULL || sys_base != expected_base)
        return NULL;
    return &runtime[current_hart];
}

enum p4_public_spinlock_result krnP4PublicSpinInit(spinlock_t *lock)
{
    ++init_calls;
    if (lock == NULL || init_result != P4_PUBLIC_SPIN_OK)
        return init_result;
    observed_gate = lock;
    lock->lock = 0u;
    lock->owner = 0u;
    return P4_PUBLIC_SPIN_OK;
}

enum p4_public_spinlock_result krnP4PublicSpinTryAcquire(
    spinlock_t *lock, uint32_t mode)
{
    ++try_calls;
    if (lock == NULL || lock != observed_gate || mode != SPINLOCK_MODE_WRITE ||
        mie_enabled != 0u || gate_held)
        return P4_PUBLIC_SPIN_BAD_STATE;
    if (busy_remaining != 0u)
    {
        --busy_remaining;
        return busy_result;
    }
    if (eventual_try_result != P4_PUBLIC_SPIN_OK)
        return eventual_try_result;
    gate_held = 1;
    lock->lock = 1u;
    return P4_PUBLIC_SPIN_OK;
}

enum p4_public_spinlock_result krnP4PublicSpinUnlock(spinlock_t *lock)
{
    ++unlock_calls;
    if (lock == NULL || lock != observed_gate || !gate_held || mie_enabled != 0u)
        return P4_PUBLIC_SPIN_BAD_STATE;
    if (unlock_result != P4_PUBLIC_SPIN_OK)
        return unlock_result;
    lock->lock = 0u;
    gate_held = 0;
    return P4_PUBLIC_SPIN_OK;
}

_Noreturn void krnP4PublicSpinFatal(enum p4_public_spinlock_result reason)
{
    fatal_reason = reason;
    fatal_gate_held = gate_held;
    fatal_mie = mie_enabled;
    if (fatal_armed)
        longjmp(fatal_env, 1);
    fprintf(stderr, "unexpected fatal: %d\n", (int)reason);
    for (;;)
        ;
}

int p4_softint_enqueue(struct p4_softint_state *state,
                       struct ExecBase *base,
                       struct Interrupt *interrupt)
{
    if (!gate_held || mie_enabled != 0u || state == NULL ||
        state->pending > 1u || state->active > 1u || base != expected_base ||
        interrupt == NULL)
        return 0;
    ++queue_enqueue_calls;
    state->pending = 1u;
    return 1;
}

int p4_softint_begin(struct p4_softint_state *state, uint32_t hart)
{
    if (!gate_held || mie_enabled != 0u || state == NULL)
        return -1;
    ++queue_begin_calls;
    if (hart != 0u || state->pending > 1u || state->active > 1u)
        return -1;
    if (state->pending == 0u || state->active != 0u)
        return 0;
    state->active = 1u;
    return 1;
}

int p4_softint_take(struct p4_softint_state *state, struct ExecBase *base,
                    uint32_t hart, struct Interrupt **interrupt)
{
    if (!gate_held || mie_enabled != 0u || state == NULL ||
        base != expected_base || hart != 0u || interrupt == NULL ||
        state->active != 1u)
        return 0;
    ++queue_take_calls;
    if (next_take != NULL)
    {
        *interrupt = next_take;
        next_take = NULL;
        return 1;
    }
    *interrupt = NULL;
    state->pending = 0u;
    state->active = 0u;
    return 1;
}

#include "../p4_softint.c"

static unsigned int checks;

#define CHECK(condition) do { \
    ++checks; \
    if (!(condition)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        return 0; \
    } \
} while (0)

static int expect_fatal(void (*call)(void),
                        enum p4_public_spinlock_result reason,
                        int held, uint32_t mie)
{
    fatal_armed = 1;
    if (setjmp(fatal_env) == 0)
    {
        call();
        fatal_armed = 0;
        fprintf(stderr, "FAIL expected fatal %d\n", (int)reason);
        return 0;
    }
    fatal_armed = 0;
    CHECK(fatal_reason == reason);
    CHECK(fatal_gate_held == held);
    CHECK(fatal_mie == mie);
    return 1;
}

static struct ExecBase base;
static struct Interrupt interrupt_a, interrupt_b;

static void call_pending(void)
{
    (void)krnP4SoftIntPending(&base);
}

static void call_bad_runtime(void)
{
    (void)krnP4SoftIntPending(&base);
}

static void call_bad_try_result(void)
{
    (void)krnP4SoftIntPending(&base);
}

static void call_bad_unlock(void)
{
    (void)krnP4SoftIntPending(&base);
}

static void call_nonowner_begin(void)
{
    (void)krnP4SoftIntBegin(&base);
}

static int test_prepare(void)
{
    unsigned int inits = init_calls;
    SysBase = &base;
    CHECK(krnP4SoftIntPrepare() == 0);
    CHECK(init_calls == inits);
    SysBase = NULL;

    current_hart = 1u;
    CHECK(krnP4SoftIntPrepare() == 0);
    CHECK(init_calls == inits);
    current_hart = 0u;

    mie_enabled = 1u;
    CHECK(krnP4SoftIntPrepare() == 0);
    CHECK(init_calls == inits);
    mie_enabled = 0u;

    init_result = P4_PUBLIC_SPIN_SERVICE_TIMEOUT;
    CHECK(krnP4SoftIntPrepare() == 0);
    CHECK(init_calls == inits + 1u);
    CHECK(fence_count == 0u);
    init_result = P4_PUBLIC_SPIN_OK;
    CHECK(krnP4SoftIntPrepare() == 1);
    CHECK(init_calls == inits + 2u);
    CHECK(fence_count == 1u);
    CHECK(observed_gate != NULL && observed_gate->lock == 0u);
    CHECK(krnP4SoftIntPrepare() == 0);
    CHECK(init_calls == inits + 2u);
    return 1;
}

static void runtime_init(void)
{
    expected_base = &base;
    base.cookie = UINT32_C(0x50445349);
    cpu_local[0].hart_id = 0u;
    cpu_local[1].hart_id = 1u;
    runtime[0].hart_id = 0u;
    runtime[0].cpu_local = &cpu_local[0];
    runtime[1].hart_id = 1u;
    runtime[1].cpu_local = &cpu_local[1];
    runtime_valid[0] = 1;
    runtime_valid[1] = 1;
    SysBase = &base;
    current_hart = 0u;
    mie_enabled = 0u;
}

static int test_mask_restore_and_owners(void)
{
    unsigned int masks, restores, tries;
    int pending;

    runtime_init();
    interrupt_a.cookie = 1u;
    interrupt_b.cookie = 2u;

    mie_enabled = 1u;
    krnP4SoftIntEnqueue(&interrupt_a, &base);
    CHECK(mie_enabled == 1u);
    CHECK(queue_enqueue_calls == 1u);
    CHECK(softint_state.pending == 1u && softint_state.active == 0u);
    CHECK(!gate_held);

    current_hart = 1u;
    mie_enabled = 0u;
    krnP4SoftIntEnqueue(&interrupt_b, &base);
    CHECK(mie_enabled == 0u);
    CHECK(queue_enqueue_calls == 2u);
    CHECK(!gate_held);

    masks = mask_count;
    tries = try_calls;
    pending = krnP4SoftIntPending(&base);
    CHECK(pending == 0);
    CHECK(mask_count == masks);
    CHECK(try_calls == tries); /* Hart 1 must not touch the shared gate. */

    current_hart = 0u;
    mie_enabled = 0u;
    masks = mask_count;
    restores = restore_count;
    CHECK(krnP4SoftIntPending(&base) == 1);
    CHECK(mie_enabled == 0u);
    CHECK(mask_count == masks + 1u && restore_count == restores);

    mie_enabled = 1u;
    masks = mask_count;
    restores = restore_count;
    CHECK(krnP4SoftIntPending(&base) == 1);
    CHECK(mie_enabled == 1u);
    CHECK(mask_count == masks + 1u && restore_count == restores + 1u);

    CHECK(krnP4SoftIntBegin(&base) == 1);
    CHECK(mie_enabled == 1u);
    CHECK(softint_state.active == 1u && !gate_held);
    CHECK(krnP4SoftIntPending(&base) == 0);
    CHECK(mie_enabled == 1u);
    next_take = &interrupt_a;
    CHECK(krnP4SoftIntTake(&base) == &interrupt_a);
    CHECK(mie_enabled == 1u);
    CHECK(queue_take_calls == 1u && !gate_held);
    CHECK(softint_state.pending == 1u && softint_state.active == 1u);
    next_take = NULL;
    CHECK(krnP4SoftIntTake(&base) == NULL);
    CHECK(mie_enabled == 1u);
    CHECK(softint_state.pending == 0u && softint_state.active == 0u);
    CHECK(!gate_held);

    /* Begin/take also preserve a caller that entered with MIE already clear. */
    softint_state.pending = 1u;
    mie_enabled = 0u;
    CHECK(krnP4SoftIntBegin(&base) == 1);
    CHECK(mie_enabled == 0u);
    next_take = &interrupt_b;
    CHECK(krnP4SoftIntTake(&base) == &interrupt_b);
    CHECK(mie_enabled == 0u);
    next_take = NULL;
    CHECK(krnP4SoftIntTake(&base) == NULL);
    CHECK(mie_enabled == 0u);
    CHECK(!gate_held);
    CHECK(callback_calls == 0u); /* The gate wrapper never calls callbacks. */
    return 1;
}

static int test_busy_and_fail_closed(void)
{
    unsigned int tries;

    runtime_init();
    busy_remaining = 3u;
    busy_result = P4_PUBLIC_SPIN_BUSY;
    mie_enabled = 1u;
    tries = try_calls;
    CHECK(krnP4SoftIntPending(&base) == 0);
    CHECK(try_calls == tries + 4u);
    CHECK(mie_enabled == 1u && !gate_held);

    busy_remaining = 2u;
    busy_result = P4_PUBLIC_SPIN_TRY_TIMEOUT;
    tries = try_calls;
    CHECK(krnP4SoftIntPending(&base) == 0);
    CHECK(try_calls == tries + 3u);
    CHECK(mie_enabled == 1u && !gate_held);

    runtime_valid[0] = 0;
    mie_enabled = 1u;
    CHECK(expect_fatal(call_bad_runtime, P4_PUBLIC_SPIN_BAD_STATE, 0, 0u));
    CHECK(!gate_held && mie_enabled == 0u);
    runtime_valid[0] = 1;

    eventual_try_result = P4_PUBLIC_SPIN_CLAIMED_FAILURE;
    mie_enabled = 1u;
    tries = try_calls;
    CHECK(expect_fatal(call_bad_try_result,
                       P4_PUBLIC_SPIN_CLAIMED_FAILURE, 0, 0u));
    CHECK(try_calls == tries + 1u && !gate_held);
    eventual_try_result = P4_PUBLIC_SPIN_OK;

    unlock_result = P4_PUBLIC_SPIN_RELEASE_FAILURE;
    mie_enabled = 1u;
    CHECK(expect_fatal(call_bad_unlock,
                       P4_PUBLIC_SPIN_RELEASE_FAILURE, 1, 0u));
    CHECK(gate_held && mie_enabled == 0u);
    /* Restore only the mock so subsequent tests can make fresh acquisitions. */
    gate_held = 0;
    observed_gate->lock = 0u;
    unlock_result = P4_PUBLIC_SPIN_OK;

    busy_remaining = 20000u;
    busy_result = P4_PUBLIC_SPIN_BUSY;
    mie_enabled = 1u;
    tries = try_calls;
    CHECK(expect_fatal(call_pending, P4_PUBLIC_SPIN_SERVICE_TIMEOUT, 0, 0u));
    CHECK(try_calls == tries + 20000u);
    CHECK(mie_enabled == 0u && !gate_held);
    busy_remaining = 0u;
    return 1;
}

static int test_nonowner_and_runtime_mismatch(void)
{
    runtime_init();
    current_hart = 1u;
    mie_enabled = 1u;
    CHECK(expect_fatal(call_nonowner_begin,
                       P4_PUBLIC_SPIN_BAD_STATE, 1, 0u));
    CHECK(gate_held && mie_enabled == 0u);
    /* This invalid API use is fail-closed; reset only fixture bookkeeping. */
    gate_held = 0;
    observed_gate->lock = 0u;

    current_hart = 0u;
    cpu_local[0].hart_id = 1u;
    mie_enabled = 1u;
    CHECK(expect_fatal(call_bad_runtime, P4_PUBLIC_SPIN_BAD_STATE, 0, 0u));
    CHECK(!gate_held && mie_enabled == 0u);
    return 1;
}

int main(void)
{
    if (!test_prepare() || !test_mask_restore_and_owners() ||
        !test_busy_and_fail_closed() || !test_nonowner_and_runtime_mismatch())
        return 1;
    printf("PASS p4_softint_gate checks=%u queue_calls=%u/%u/%u\n",
           checks, queue_enqueue_calls, queue_begin_calls, queue_take_calls);
    return 0;
}
