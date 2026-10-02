/*
 * Actual-source host fixture for the P4 SYSTIMER UNIT0 snapshot helper.
 * p4_timer_snapshot.c is included verbatim. CSR/MIE, runtime, software-lock,
 * fence and MMIO boundaries are modeled here; this is not hardware timing
 * evidence.
 */
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define P4_TIMER_SNAPSHOT_TEST 1
#define P4_PUBLIC_SPINLOCK_TEST 1
#define MSTATUS_MIE UINT32_C(0x00000008)
#define P4_SYSTIMER_HZ 16000000UL

#define P4_ST_UNIT0_OP UINT32_C(0x0004)
#define P4_ST_UNIT0_UPDATE (UINT32_C(1) << 30)
#define P4_ST_UNIT0_VALID (UINT32_C(1) << 29)
#define P4_ST_UNIT0_VALUE_HI UINT32_C(0x0040)
#define P4_ST_UNIT0_VALUE_LO UINT32_C(0x0044)

struct ExecBase
{
    uint32_t id;
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

struct ExecBase *SysBase;

static uint32_t mock_csr_read(void);
static uint32_t mock_hart_read(void);
static uint32_t mock_timer_mask(void);
static void mock_timer_restore(uint32_t saved_mie);
static uint32_t mock_timer_read(uint32_t offset);
static void mock_timer_write(uint32_t offset, uint32_t value);
static void mock_timer_fence(void);

#define csr_read(reg) mock_csr_read()
#define P4_CPU_LOCAL_READ_MHARTID() mock_hart_read()
#define P4_TIMER_MASK() mock_timer_mask()
#define P4_TIMER_RESTORE(saved) mock_timer_restore((saved))
#define P4_TIMER_READ(offset) mock_timer_read((offset))
#define P4_TIMER_WRITE(offset, value) mock_timer_write((offset), (value))
#define P4_TIMER_FENCE() mock_timer_fence()
#define Disable() mock_disable()
#define Enable() mock_enable()

#include "../p4_public_spinlock.h"

struct p4_exec_runtime *p4_exec_runtime_current(struct ExecBase *base);

#include "../p4_timer_snapshot.c"

enum event_kind
{
    EVENT_MASK = 1,
    EVENT_ACQUIRE,
    EVENT_WRITE,
    EVENT_FENCE,
    EVENT_READ,
    EVENT_UNLOCK,
    EVENT_RESTORE
};

struct event
{
    enum event_kind kind;
    uint32_t hart;
    uint32_t detail;
    uint32_t value;
    int gate_held;
    uint32_t mstatus;
};

#define EVENT_CAPACITY 2048u
#define FATAL_NONE (-1)

static struct event events[EVENT_CAPACITY];
static size_t event_count;
static unsigned checks;
static uint32_t mock_mstatus[8];
static uint32_t current_hart;
static struct ExecBase sysbases[2];
static struct p4_cpu_local cpu_locals[2];
static struct p4_exec_runtime runtimes[2];
static int runtime_valid[2];
static uint32_t exec_nesting[2];
static unsigned disable_calls;
static unsigned enable_calls;
static unsigned mask_calls;
static unsigned restore_calls;
static unsigned init_calls;
static unsigned acquire_calls;
static unsigned blocked_acquire_calls;
static unsigned acquire_busy_before_success;
static enum p4_public_spinlock_result retry_result;
static enum p4_public_spinlock_result forced_acquire_result;
static enum p4_public_spinlock_result init_result;
static enum p4_public_spinlock_result unlock_result;
static unsigned op_reads;
static unsigned value_reads;
static unsigned timer_writes;
static unsigned timer_fences;
static uint32_t valid_after_polls;
static uint32_t timer_hi;
static uint32_t timer_lo;
static int gate_held;
static int require_gate_for_mmio;
static int mmio_before_gate;
static int fatal_armed;
static enum p4_public_spinlock_result fatal_reason;
static jmp_buf fatal_env;
static void (*timer_io_probe)(uint32_t offset);

#define current_mstatus (mock_mstatus[current_hart])

static void fail(const char *expr, int line)
{
    fprintf(stderr, "p4-timer-snapshot-test:%d: CHECK failed: %s\n",
            line, expr);
    exit(1);
}

#define CHECK(expr) do { ++checks; if (!(expr)) fail(#expr, __LINE__); } while (0)

#define EXPECT_FATAL(expected_reason, statement) do { \
    fatal_reason = P4_PUBLIC_SPIN_OK; \
    fatal_armed = 1; \
    if (setjmp(fatal_env) == 0) { \
        statement; \
        fatal_armed = 0; \
        fail("expected fatal", __LINE__); \
    } else { \
        CHECK(fatal_reason == (expected_reason)); \
    } \
    fatal_armed = 0; \
} while (0)

static void record_event(enum event_kind kind, uint32_t detail, uint32_t value)
{
    if (event_count >= EVENT_CAPACITY)
        fail("event capacity", __LINE__);
    events[event_count].kind = kind;
    events[event_count].hart = current_hart;
    events[event_count].detail = detail;
    events[event_count].value = value;
    events[event_count].gate_held = gate_held;
    events[event_count].mstatus = current_mstatus;
    ++event_count;
}

static void clear_read_trace(void)
{
    memset(events, 0, sizeof(events));
    event_count = 0u;
    mask_calls = 0u;
    restore_calls = 0u;
    acquire_calls = 0u;
    blocked_acquire_calls = 0u;
    op_reads = 0u;
    value_reads = 0u;
    timer_writes = 0u;
    timer_fences = 0u;
    mmio_before_gate = 0;
}

static uint32_t mock_csr_read(void)
{
    return current_hart < 2u ? mock_mstatus[current_hart] : 0u;
}

static uint32_t mock_hart_read(void)
{
    return current_hart;
}

static uint32_t mock_timer_mask(void)
{
    uint32_t saved = current_mstatus & MSTATUS_MIE;
    ++mask_calls;
    current_mstatus &= ~MSTATUS_MIE;
    record_event(EVENT_MASK, 0u, saved);
    return saved;
}

static void mock_timer_restore(uint32_t saved_mie)
{
    ++restore_calls;
    if (saved_mie != 0u)
        current_mstatus |= MSTATUS_MIE;
    else
        current_mstatus &= ~MSTATUS_MIE;
    record_event(EVENT_RESTORE, 0u, saved_mie);
}

static void check_mmio_gate(void)
{
    if (require_gate_for_mmio && !gate_held)
    {
        ++mmio_before_gate;
        CHECK(gate_held);
    }
}

static uint32_t mock_timer_read(uint32_t offset)
{
    check_mmio_gate();
    if (offset == P4_ST_UNIT0_OP)
    {
        uint32_t result = 0u;
        ++op_reads;
        if (op_reads >= valid_after_polls)
            result = P4_ST_UNIT0_VALID;
        record_event(EVENT_READ, offset, result);
        return result;
    }
    if (offset == P4_ST_UNIT0_VALUE_HI)
    {
        ++value_reads;
        record_event(EVENT_READ, offset, timer_hi);
        if (timer_io_probe != NULL)
            timer_io_probe(offset);
        return timer_hi;
    }
    if (offset == P4_ST_UNIT0_VALUE_LO)
    {
        ++value_reads;
        record_event(EVENT_READ, offset, timer_lo);
        if (timer_io_probe != NULL)
            timer_io_probe(offset);
        return timer_lo;
    }
    CHECK(0 && "unexpected timer register read");
    return 0u;
}

static void mock_timer_write(uint32_t offset, uint32_t value)
{
    check_mmio_gate();
    CHECK(offset == P4_ST_UNIT0_OP);
    CHECK(value == P4_ST_UNIT0_UPDATE);
    ++timer_writes;
    record_event(EVENT_WRITE, offset, value);
}

static void mock_timer_fence(void)
{
    check_mmio_gate();
    ++timer_fences;
    record_event(EVENT_FENCE, 0u, 0u);
}

struct p4_exec_runtime *p4_exec_runtime_current(struct ExecBase *base)
{
    if (current_hart >= 2u || !runtime_valid[current_hart] ||
        base != &sysbases[current_hart])
        return NULL;
    return &runtimes[current_hart];
}

void mock_disable(void)
{
    ++disable_calls;
    current_mstatus &= ~MSTATUS_MIE;
}

void mock_enable(void)
{
    ++enable_calls;
    current_mstatus |= MSTATUS_MIE;
}

enum p4_public_spinlock_result krnP4PublicSpinInit(spinlock_t *lock)
{
    CHECK(lock == &p4_timer_gate);
    CHECK(current_hart == 0u);
    CHECK((current_mstatus & MSTATUS_MIE) == 0u);
    CHECK(SysBase == NULL);
    ++init_calls;
    if (init_result == P4_PUBLIC_SPIN_OK)
    {
        lock->lock = 0u;
        lock->s_Owner = 0u;
    }
    return init_result;
}

enum p4_public_spinlock_result krnP4PublicSpinTryAcquire(
    spinlock_t *lock, uint32_t mode)
{
    CHECK(lock == &p4_timer_gate);
    CHECK(mode == SPINLOCK_MODE_WRITE);
    CHECK((current_mstatus & MSTATUS_MIE) == 0u);
    ++acquire_calls;

    if (gate_held)
    {
        ++blocked_acquire_calls;
        return P4_PUBLIC_SPIN_BUSY;
    }

    if (forced_acquire_result != P4_PUBLIC_SPIN_OK)
        return forced_acquire_result;
    if (acquire_calls <= acquire_busy_before_success)
        return retry_result;

    CHECK(lock->lock == 0u);
    lock->lock = 1u;
    gate_held = 1;
    record_event(EVENT_ACQUIRE, mode, 0u);
    return P4_PUBLIC_SPIN_OK;
}

enum p4_public_spinlock_result krnP4PublicSpinUnlock(spinlock_t *lock)
{
    CHECK(lock == &p4_timer_gate);
    CHECK(gate_held);
    record_event(EVENT_UNLOCK, 0u, (uint32_t)unlock_result);
    if (unlock_result != P4_PUBLIC_SPIN_OK)
        return unlock_result;
    lock->lock = 0u;
    gate_held = 0;
    return P4_PUBLIC_SPIN_OK;
}

enum p4_public_spinlock_result krnP4PublicSpinBlockingAttempt(
    spinlock_t *lock, uint32_t mode, p4_public_spinlock_hook_bridge hook,
    void *context)
{
    (void)lock;
    (void)mode;
    (void)hook;
    (void)context;
    return P4_PUBLIC_SPIN_BAD_STATE;
}

enum p4_public_spinlock_result krnP4PublicSpinIsLocked(
    const spinlock_t *lock, int *held)
{
    if (lock != &p4_timer_gate || held == NULL)
        return P4_PUBLIC_SPIN_BAD_ARGUMENT;
    *held = gate_held;
    return P4_PUBLIC_SPIN_OK;
}

void krnP4PublicSpinFatal(enum p4_public_spinlock_result reason)
{
    fatal_reason = reason;
    if (!fatal_armed)
    {
        fprintf(stderr, "unexpected fatal: %d\n", (int)reason);
        abort();
    }
    longjmp(fatal_env, 1);
}

static void reset_fixture(void)
{
    memset(&p4_timer_gate, 0, sizeof(p4_timer_gate));
    p4_timer_prepared = 0u;
    memset(events, 0, sizeof(events));
    memset(mock_mstatus, 0, sizeof(mock_mstatus));
    memset(sysbases, 0, sizeof(sysbases));
    memset(cpu_locals, 0, sizeof(cpu_locals));
    memset(runtimes, 0, sizeof(runtimes));
    memset(runtime_valid, 0, sizeof(runtime_valid));
    memset(exec_nesting, 0, sizeof(exec_nesting));
    event_count = 0u;
    current_hart = 0u;
    SysBase = NULL;
    disable_calls = 0u;
    enable_calls = 0u;
    mask_calls = 0u;
    restore_calls = 0u;
    init_calls = 0u;
    acquire_calls = 0u;
    blocked_acquire_calls = 0u;
    acquire_busy_before_success = 0u;
    retry_result = P4_PUBLIC_SPIN_BUSY;
    forced_acquire_result = P4_PUBLIC_SPIN_OK;
    init_result = P4_PUBLIC_SPIN_OK;
    unlock_result = P4_PUBLIC_SPIN_OK;
    op_reads = 0u;
    value_reads = 0u;
    timer_writes = 0u;
    timer_fences = 0u;
    valid_after_polls = 1u;
    timer_hi = UINT32_C(0x89abcdef);
    timer_lo = UINT32_C(0x01234567);
    gate_held = 0;
    require_gate_for_mmio = 0;
    mmio_before_gate = 0;
    fatal_armed = 0;
    fatal_reason = P4_PUBLIC_SPIN_OK;
    timer_io_probe = NULL;

    for (uint32_t hart = 0u; hart < 2u; ++hart)
    {
        sysbases[hart].id = hart;
        cpu_locals[hart].hart_id = hart;
        runtimes[hart].hart_id = hart;
        runtimes[hart].cpu_local = &cpu_locals[hart];
        runtime_valid[hart] = 1;
        exec_nesting[hart] = UINT32_C(0x12340000) + hart;
    }
}

static void prepare_ok(void)
{
    current_hart = 0u;
    current_mstatus = 0u;
    SysBase = NULL;
    CHECK(krnP4TimerSnapshotPrepare() == 1);
}

static size_t find_event(enum event_kind kind, uint32_t detail,
                         unsigned occurrence)
{
    for (size_t i = 0u; i < event_count; ++i)
    {
        if (events[i].kind == kind && events[i].detail == detail)
        {
            if (occurrence == 0u)
                return i;
            --occurrence;
        }
    }
    return event_count;
}

static void test_prepare_admission_and_init_failure(void)
{
    reset_fixture();
    current_mstatus = MSTATUS_MIE;
    CHECK(krnP4TimerSnapshotPrepare() == 0);
    CHECK(init_calls == 0u);
    CHECK(p4_timer_prepared == 0u);

    current_mstatus = 0u;
    current_hart = 1u;
    CHECK(krnP4TimerSnapshotPrepare() == 0);
    CHECK(init_calls == 0u);

    current_hart = 0u;
    SysBase = &sysbases[0];
    CHECK(krnP4TimerSnapshotPrepare() == 0);
    CHECK(init_calls == 0u);

    SysBase = NULL;
    init_result = P4_PUBLIC_SPIN_SERVICE_ERROR;
    CHECK(krnP4TimerSnapshotPrepare() == 0);
    CHECK(init_calls == 1u);
    CHECK(p4_timer_prepared == 0u);

    init_result = P4_PUBLIC_SPIN_OK;
    event_count = 0u;
    CHECK(krnP4TimerSnapshotPrepare() == 1);
    CHECK(init_calls == 2u);
    CHECK(p4_timer_prepared == 1u);
    CHECK(krnP4TimerSnapshotPrepare() == 0);
    CHECK(init_calls == 2u);
    CHECK(timer_writes == 0u);
    CHECK(op_reads == 0u);
}

static void test_public_timer_ops_table(void)
{
    const struct KrnP4TimerOps *ops = krnP4TimerSnapshotOps();

    CHECK(ops != NULL);
    CHECK(ops->version == KRN_P4_TIMER_OPS_VERSION);
    CHECK(ops->size == sizeof(*ops));
    CHECK(ops->frequency == P4_SYSTIMER_HZ);
    CHECK(ops->read == krnP4TimerSnapshotRead);
    CHECK(krnP4TimerSnapshotOps() == ops);
}

static void test_early_primary_read(void)
{
    uint64_t value;
    uint64_t expected = ((uint64_t)timer_hi << 32) | timer_lo;
    size_t write_i, fence_i, op_i, hi_i, lo_i, end_fence_i, restore_i;

    reset_fixture();
    current_hart = 0u;
    current_mstatus = MSTATUS_MIE;
    exec_nesting[0] = UINT32_C(0x76543210);
    value = krnP4TimerSnapshotRead();

    CHECK(value == expected);
    CHECK(op_reads == 1u);
    CHECK(value_reads == 2u);
    CHECK(acquire_calls == 0u);
    CHECK(!gate_held);
    CHECK(current_mstatus == MSTATUS_MIE);
    CHECK(exec_nesting[0] == UINT32_C(0x76543210));
    CHECK(mask_calls == 1u);
    CHECK(restore_calls == 1u);
    CHECK(disable_calls == 0u);
    CHECK(enable_calls == 0u);
    CHECK(p4_timer_prepared == 0u);

    write_i = find_event(EVENT_WRITE, P4_ST_UNIT0_OP, 0u);
    fence_i = find_event(EVENT_FENCE, 0u, 0u);
    op_i = find_event(EVENT_READ, P4_ST_UNIT0_OP, 0u);
    hi_i = find_event(EVENT_READ, P4_ST_UNIT0_VALUE_HI, 0u);
    lo_i = find_event(EVENT_READ, P4_ST_UNIT0_VALUE_LO, 0u);
    end_fence_i = find_event(EVENT_FENCE, 0u, 1u);
    restore_i = find_event(EVENT_RESTORE, 0u, 0u);
    CHECK(write_i < fence_i);
    CHECK(fence_i < op_i);
    CHECK(events[op_i].value == P4_ST_UNIT0_VALID);
    CHECK(op_i < hi_i);
    CHECK(hi_i < lo_i);
    CHECK(lo_i < end_fence_i);
    CHECK(end_fence_i < restore_i);
}

static void test_both_published_harts_serialized(void)
{
    for (uint32_t hart = 0u; hart < 2u; ++hart)
    {
        uint64_t value;
        uint64_t expected = ((uint64_t)timer_hi << 32) | timer_lo;
        size_t acquire_i, write_i, first_fence_i, first_op_i;
        size_t hi_i, lo_i, final_fence_i, unlock_i, restore_i;

        reset_fixture();
        prepare_ok();
        clear_read_trace();
        current_hart = hart;
        SysBase = &sysbases[hart];
        current_mstatus = (hart == 0u) ? MSTATUS_MIE : 0u;
        exec_nesting[hart] = UINT32_C(0xaabb0000) + hart;
        acquire_busy_before_success = 2u;
        retry_result = (hart == 0u) ? P4_PUBLIC_SPIN_BUSY :
                                      P4_PUBLIC_SPIN_TRY_TIMEOUT;
        valid_after_polls = hart + 2u;
        require_gate_for_mmio = 1;

        value = krnP4TimerSnapshotRead();

        CHECK(value == expected);
        CHECK(acquire_calls == 3u);
        CHECK(blocked_acquire_calls == 0u);
        CHECK(op_reads == hart + 2u);
        CHECK(value_reads == 2u);
        CHECK(timer_writes == 1u);
        CHECK(timer_fences == 2u);
        CHECK(!gate_held);
        CHECK(p4_timer_gate.lock == 0u);
        CHECK(current_mstatus == ((hart == 0u) ? MSTATUS_MIE : 0u));
        CHECK(exec_nesting[hart] == UINT32_C(0xaabb0000) + hart);
        CHECK(mask_calls == 1u);
        CHECK(restore_calls == 1u);
        CHECK(disable_calls == 0u);
        CHECK(enable_calls == 0u);
        CHECK(mmio_before_gate == 0);

        acquire_i = find_event(EVENT_ACQUIRE, SPINLOCK_MODE_WRITE, 0u);
        write_i = find_event(EVENT_WRITE, P4_ST_UNIT0_OP, 0u);
        first_fence_i = find_event(EVENT_FENCE, 0u, 0u);
        first_op_i = find_event(EVENT_READ, P4_ST_UNIT0_OP, 0u);
        hi_i = find_event(EVENT_READ, P4_ST_UNIT0_VALUE_HI, 0u);
        lo_i = find_event(EVENT_READ, P4_ST_UNIT0_VALUE_LO, 0u);
        final_fence_i = find_event(EVENT_FENCE, 0u, 1u);
        unlock_i = find_event(EVENT_UNLOCK, 0u, 0u);
        restore_i = find_event(EVENT_RESTORE, 0u, 0u);
        CHECK(acquire_i < write_i);
        CHECK(write_i < first_fence_i);
        CHECK(first_fence_i < first_op_i);
        CHECK(first_op_i < hi_i);
        CHECK(hi_i < lo_i);
        CHECK(lo_i < final_fence_i);
        CHECK(final_fence_i < unlock_i);
        CHECK(unlock_i < restore_i);

        for (size_t i = write_i; i <= final_fence_i; ++i)
            CHECK(events[i].gate_held);

        {
            unsigned seen_polls = 0u;
            size_t last_poll = event_count;
            for (size_t i = first_op_i; i < hi_i; ++i)
            {
                if (events[i].kind == EVENT_READ &&
                    events[i].detail == P4_ST_UNIT0_OP)
                {
                    ++seen_polls;
                    last_poll = i;
                    CHECK(events[i].value ==
                          ((seen_polls == op_reads) ? P4_ST_UNIT0_VALID : 0u));
                }
            }
            CHECK(seen_polls == op_reads);
            CHECK(last_poll < hi_i);
        }
    }
}

static void test_poll_timeout_unlocks_then_fatals(void)
{
    reset_fixture();
    prepare_ok();
    current_hart = 1u;
    SysBase = &sysbases[1];
    current_mstatus = MSTATUS_MIE;
    valid_after_polls = UINT32_MAX;
    require_gate_for_mmio = 1;
    clear_read_trace();

    EXPECT_FATAL(P4_PUBLIC_SPIN_SERVICE_TIMEOUT,
                 (void)krnP4TimerSnapshotRead());

    CHECK(op_reads == 1000u);
    CHECK(value_reads == 0u);
    CHECK(timer_writes == 1u);
    CHECK(timer_fences == 2u);
    CHECK(acquire_calls == 1u);
    CHECK(!gate_held);
    CHECK(p4_timer_gate.lock == 0u);
    CHECK(current_mstatus == 0u);
    CHECK(restore_calls == 0u);
    CHECK(disable_calls == 0u);
    CHECK(enable_calls == 0u);
    CHECK(mmio_before_gate == 0);
    CHECK(find_event(EVENT_UNLOCK, 0u, 0u) < event_count);
    CHECK(find_event(EVENT_RESTORE, 0u, 0u) == event_count);
}

static void test_acquire_bounds_and_service_error(void)
{
    reset_fixture();
    prepare_ok();
    current_hart = 0u;
    SysBase = &sysbases[0];
    current_mstatus = MSTATUS_MIE;
    retry_result = P4_PUBLIC_SPIN_TRY_TIMEOUT;
    acquire_busy_before_success = UINT32_MAX;
    require_gate_for_mmio = 1;
    clear_read_trace();

    EXPECT_FATAL(P4_PUBLIC_SPIN_SERVICE_TIMEOUT,
                 (void)krnP4TimerSnapshotRead());

    CHECK(acquire_calls == 20000u);
    CHECK(timer_writes == 0u);
    CHECK(op_reads == 0u);
    CHECK(value_reads == 0u);
    CHECK(timer_fences == 0u);
    CHECK(!gate_held);
    CHECK(current_mstatus == 0u);
    CHECK(restore_calls == 0u);
    CHECK(find_event(EVENT_ACQUIRE, SPINLOCK_MODE_WRITE, 0u) == event_count);

    reset_fixture();
    prepare_ok();
    current_hart = 1u;
    SysBase = &sysbases[1];
    current_mstatus = MSTATUS_MIE;
    forced_acquire_result = P4_PUBLIC_SPIN_SERVICE_ERROR;
    require_gate_for_mmio = 1;
    clear_read_trace();

    EXPECT_FATAL(P4_PUBLIC_SPIN_SERVICE_ERROR,
                 (void)krnP4TimerSnapshotRead());

    CHECK(acquire_calls == 1u);
    CHECK(timer_writes == 0u);
    CHECK(op_reads == 0u);
    CHECK(value_reads == 0u);
    CHECK(!gate_held);
    CHECK(current_mstatus == 0u);
    CHECK(restore_calls == 0u);
    CHECK(find_event(EVENT_UNLOCK, 0u, 0u) == event_count);

    reset_fixture();
    prepare_ok();
    current_hart = 0u;
    SysBase = &sysbases[0];
    current_mstatus = MSTATUS_MIE;
    forced_acquire_result = P4_PUBLIC_SPIN_CLAIMED_FAILURE;
    require_gate_for_mmio = 1;
    clear_read_trace();

    EXPECT_FATAL(P4_PUBLIC_SPIN_CLAIMED_FAILURE,
                 (void)krnP4TimerSnapshotRead());

    CHECK(acquire_calls == 1u);
    CHECK(timer_writes == 0u);
    CHECK(op_reads == 0u);
    CHECK(value_reads == 0u);
    CHECK(!gate_held);
    CHECK(current_mstatus == 0u);
    CHECK(restore_calls == 0u);
    CHECK(find_event(EVENT_UNLOCK, 0u, 0u) == event_count);
}

static void test_unlock_error_fails_closed(void)
{
    reset_fixture();
    prepare_ok();
    current_hart = 0u;
    SysBase = &sysbases[0];
    current_mstatus = MSTATUS_MIE;
    unlock_result = P4_PUBLIC_SPIN_RELEASE_FAILURE;
    require_gate_for_mmio = 1;
    clear_read_trace();

    EXPECT_FATAL(P4_PUBLIC_SPIN_RELEASE_FAILURE,
                 (void)krnP4TimerSnapshotRead());

    CHECK(op_reads == 1u);
    CHECK(value_reads == 2u);
    CHECK(timer_writes == 1u);
    CHECK(gate_held);
    CHECK(p4_timer_gate.lock == 1u);
    CHECK(current_mstatus == 0u);
    CHECK(restore_calls == 0u);
    CHECK(disable_calls == 0u);
    CHECK(enable_calls == 0u);
    CHECK(mmio_before_gate == 0);
}

static void expect_invalid_owner(uint32_t hart,
                                 enum p4_public_spinlock_result reason)
{
    require_gate_for_mmio = 1;
    clear_read_trace();
    current_hart = hart;
    current_mstatus = MSTATUS_MIE;

    EXPECT_FATAL(reason, (void)krnP4TimerSnapshotRead());

    CHECK(timer_writes == 0u);
    CHECK(op_reads == 0u);
    CHECK(value_reads == 0u);
    CHECK(acquire_calls == 0u);
    CHECK(!gate_held);
    CHECK(current_hart >= 2u || current_mstatus == 0u);
    CHECK(restore_calls == 0u);
    CHECK(mmio_before_gate == 0);
}

static void test_invalid_runtime_local_hart_and_prepared(void)
{
    reset_fixture();
    prepare_ok();
    current_hart = 0u;
    SysBase = &sysbases[0];
    p4_timer_prepared = 0u;
    expect_invalid_owner(0u, P4_PUBLIC_SPIN_BAD_STATE);

    reset_fixture();
    prepare_ok();
    current_hart = 0u;
    SysBase = &sysbases[0];
    runtime_valid[0] = 0;
    expect_invalid_owner(0u, P4_PUBLIC_SPIN_BAD_STATE);

    reset_fixture();
    prepare_ok();
    current_hart = 0u;
    SysBase = &sysbases[0];
    runtimes[0].hart_id = 2u;
    expect_invalid_owner(0u, P4_PUBLIC_SPIN_BAD_STATE);

    reset_fixture();
    prepare_ok();
    current_hart = 0u;
    SysBase = &sysbases[0];
    runtimes[0].cpu_local = NULL;
    expect_invalid_owner(0u, P4_PUBLIC_SPIN_BAD_STATE);

    reset_fixture();
    prepare_ok();
    current_hart = 1u;
    SysBase = &sysbases[1];
    cpu_locals[1].hart_id = 0u;
    expect_invalid_owner(1u, P4_PUBLIC_SPIN_BAD_STATE);

    reset_fixture();
    current_hart = 1u;
    SysBase = NULL;
    expect_invalid_owner(1u, P4_PUBLIC_SPIN_BAD_STATE);

    reset_fixture();
    current_hart = 2u;
    SysBase = NULL;
    expect_invalid_owner(2u, P4_PUBLIC_SPIN_BAD_STATE);
}

int main(void)
{
    test_prepare_admission_and_init_failure();
    test_public_timer_ops_table();
    test_early_primary_read();
    test_both_published_harts_serialized();
    test_poll_timeout_unlocks_then_fatals();
    test_acquire_bounds_and_service_error();
    test_unlock_error_fails_closed();
    test_invalid_runtime_local_hart_and_prepared();
    printf("p4-timer-snapshot-test: PASS (%u checks)\n", checks);
    return 0;
}
