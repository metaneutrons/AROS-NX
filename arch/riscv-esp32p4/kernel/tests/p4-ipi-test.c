/*
 * Actual-source host fixture for the private P4 IPI mailbox.
 * p4_ipi.c is included verbatim; only Exec runtime, gate operations, CSR/MIE,
 * CLIC, latch, route, and fence boundaries are mocked. This is not hardware or
 * cross-hart execution evidence.
 */
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define P4_IPI_TEST 1
#define P4_PUBLIC_SPINLOCK_TEST 1
#define MSTATUS_MIE (UINT32_C(1) << 3)
#define P4_EXEC_SCHEDULE_QUANTUM  (UINT32_C(1) << 0)
#define P4_EXEC_SCHEDULE_SWITCH   (UINT32_C(1) << 1)
#define P4_EXEC_SCHEDULE_DISPATCH (UINT32_C(1) << 2)

struct ExecBase
{
    uint32_t id;
};

struct p4_cpu_local
{
    uint32_t hart_id;
    int32_t id_nest;
};

struct p4_exec_runtime
{
    uint32_t hart_id;
    struct p4_cpu_local *cpu_local;
};

static uint32_t mock_csr_read(void);
static uint32_t mock_hart_read(void);
static void mock_disable(void);
static void mock_enable(void);
static void mock_latch(uint32_t hart, uint32_t value);
static void mock_route(uint32_t hart);
static void mock_clic_clear(uint32_t line);
static void mock_clic_enable(uint32_t line, uint32_t flags);
static void mock_fence(void);
static void mock_event(int kind, uint32_t hart, uint32_t value);

#define csr_read(reg) mock_csr_read()
#define P4_CPU_LOCAL_READ_MHARTID() mock_hart_read()
#define Disable() mock_disable()
#define Enable() mock_enable()
#define P4_IPI_FENCE() mock_fence()
#define P4_IPI_LATCH(hart, value) mock_latch((hart), (value))
#define P4_IPI_ROUTE(hart) mock_route((hart))
#define krnCLICClear(line) mock_clic_clear((line))
#define krnCLICEnable(line, flags) mock_clic_enable((line), (flags))

#include "../../exec/p4_signal_protocol.h"
#include "../p4_public_spinlock.h"
#include "../p4_ipi.h"

struct p4_exec_runtime *p4_exec_runtime_current(struct ExecBase *SysBase);
int krnP4CleanupWorkerReady(struct ExecBase *SysBase);
static int cleanup_ready = 1;

enum event_kind
{
    EVENT_FENCE = 1,
    EVENT_LATCH,
    EVENT_ROUTE,
    EVENT_CLIC_CLEAR,
    EVENT_CLIC_ENABLE,
    EVENT_GATE_UNLOCK,
    EVENT_ENABLE
};

struct event
{
    int kind;
    uint32_t hart;
    uint32_t value;
};

#define EVENT_CAPACITY 1024u
#define FATAL_NONE (-1)

static struct event events[EVENT_CAPACITY];
static size_t event_count;
static unsigned checks;
static uint32_t mock_mstatus_by_hart[2];
static uint32_t current_hart;
static struct ExecBase sysbases[2];
static struct p4_cpu_local cpu_locals[2];
static struct p4_exec_runtime runtimes[2];
static int runtime_valid[2];
static unsigned disable_depth[2];
static uint32_t disable_saved_mie[2][32];
static unsigned disable_calls;
static unsigned enable_calls;
static unsigned disable_calls_by_hart[2];
static unsigned enable_calls_by_hart[2];
static unsigned init_calls;
static unsigned route_calls[2];
static unsigned latch_writes[2][2];
static uint32_t latch_state[2];
static int gate_held;
static int verify_drain_order;
static int unlock_hook_pending;
static uint32_t unlock_hook_hart;
static struct p4_signal_effect unlock_hook_effect;
static struct ExecBase *unlock_hook_sysbase;
static jmp_buf fatal_env;
static int fatal_armed;
static enum p4_public_spinlock_result fatal_reason;

#define mock_mstatus (mock_mstatus_by_hart[current_hart])

static void fail(const char *expr, int line)
{
    fprintf(stderr, "p4-ipi-test:%d: CHECK failed: %s\n", line, expr);
    exit(1);
}

#define CHECK(expr) do { ++checks; if (!(expr)) fail(#expr, __LINE__); } while (0)

#define EXPECT_FATAL(expected_reason, statement) do { \
    int caught_fatal = 0; \
    fatal_reason = P4_PUBLIC_SPIN_OK; \
    fatal_armed = 1; \
    if (setjmp(fatal_env) == 0) { \
        statement; \
        fatal_armed = 0; \
        fail("expected fatal", __LINE__); \
    } else { \
        caught_fatal = 1; \
    } \
    fatal_armed = 0; \
    CHECK(caught_fatal); \
    CHECK(fatal_reason == (expected_reason)); \
} while (0)

#include "../p4_ipi.c"

static uint32_t mock_csr_read(void)
{
    return current_hart < 2u ? mock_mstatus_by_hart[current_hart] : 0u;
}

static uint32_t mock_hart_read(void)
{
    return current_hart;
}

struct p4_exec_runtime *p4_exec_runtime_current(struct ExecBase *SysBase)
{
    if (current_hart >= 2u || !runtime_valid[current_hart] ||
        SysBase != &sysbases[current_hart])
        return NULL;
    return &runtimes[current_hart];
}

static void mock_event(int kind, uint32_t hart, uint32_t value)
{
    if (event_count >= EVENT_CAPACITY)
        fail("event capacity", __LINE__);
    events[event_count].kind = kind;
    events[event_count].hart = hart;
    events[event_count].value = value;
    ++event_count;
}

static void mock_disable(void)
{
    if (current_hart >= 2u)
    {
        ++disable_calls;
        return;
    }
    CHECK(disable_depth[current_hart] < 32u);
    disable_saved_mie[current_hart][disable_depth[current_hart]] =
        mock_mstatus & MSTATUS_MIE;
    ++disable_depth[current_hart];
    ++disable_calls;
    ++disable_calls_by_hart[current_hart];
    mock_mstatus &= ~MSTATUS_MIE;
}

static void mock_enable(void)
{
    CHECK(current_hart < 2u);
    CHECK(disable_depth[current_hart] > 0u);
    CHECK(!gate_held);
    mock_event(EVENT_ENABLE, current_hart, 0u);
    --disable_depth[current_hart];
    if (disable_depth[current_hart] == 0u &&
        disable_saved_mie[current_hart][0] != 0u)
        mock_mstatus |= MSTATUS_MIE;
    ++enable_calls;
    ++enable_calls_by_hart[current_hart];
}

static void mock_latch(uint32_t hart, uint32_t value)
{
    CHECK(gate_held);
    CHECK(hart < 2u);
    CHECK(value <= 1u);
    if (verify_drain_order && value == 0u)
    {
        CHECK(p4_ipi_slots[hart].schedule_flags != 0u ||
              p4_ipi_slots[hart].ticks != 0u);
    }
    ++latch_writes[hart][value];
    latch_state[hart] = value;
    mock_event(EVENT_LATCH, hart, value);
}

static void mock_route(uint32_t hart)
{
    CHECK(gate_held);
    CHECK(hart < 2u);
    ++route_calls[hart];
    mock_event(EVENT_ROUTE, hart, 22u);
}

static void mock_clic_clear(uint32_t line)
{
    CHECK(gate_held);
    CHECK(line == P4_IPI_LINE);
    if (verify_drain_order)
        CHECK(p4_ipi_slots[current_hart].schedule_flags != 0u ||
              p4_ipi_slots[current_hart].ticks != 0u);
    mock_event(EVENT_CLIC_CLEAR, current_hart, line);
}

static void mock_clic_enable(uint32_t line, uint32_t flags)
{
    CHECK(gate_held);
    CHECK(line == P4_IPI_LINE);
    CHECK(flags == 0u);
    mock_event(EVENT_CLIC_ENABLE, current_hart, line);
}

static void mock_fence(void)
{
    mock_event(EVENT_FENCE, current_hart, 0u);
}

enum p4_public_spinlock_result krnP4PublicSpinInit(spinlock_t *lock)
{
    CHECK(lock != NULL);
    CHECK((mock_mstatus & MSTATUS_MIE) == 0u);
    CHECK(current_hart == 0u);
    lock->lock = 0u;
    lock->s_Owner = 0u;
    ++init_calls;
    return P4_PUBLIC_SPIN_OK;
}

enum p4_public_spinlock_result krnP4PublicSpinTryAcquire(
    spinlock_t *lock, uint32_t mode)
{
    CHECK(lock == &p4_ipi_gate);
    CHECK(mode == SPINLOCK_MODE_WRITE);
    CHECK(!gate_held);
    if (lock->lock != 0u)
        return P4_PUBLIC_SPIN_BUSY;
    lock->lock = 1u;
    gate_held = 1;
    return P4_PUBLIC_SPIN_OK;
}

enum p4_public_spinlock_result krnP4PublicSpinUnlock(spinlock_t *lock)
{
    CHECK(lock == &p4_ipi_gate);
    CHECK(gate_held);
    if (verify_drain_order)
    {
        CHECK(p4_ipi_slots[current_hart].schedule_flags == 0u);
        CHECK(p4_ipi_slots[current_hart].ticks == 0u);
        /* Scope the assertion to this drain's release; a later producer may
         * legitimately repopulate the slot after the gate is open. */
        verify_drain_order = 0;
    }
    lock->lock = 0u;
    gate_held = 0;
    mock_event(EVENT_GATE_UNLOCK, current_hart, 0u);
    if (unlock_hook_pending)
    {
        uint32_t saved_hart = current_hart;
        unlock_hook_pending = 0;
        CHECK(!gate_held);
        current_hart = unlock_hook_hart;
        krnP4SignalPoke(&unlock_hook_effect, unlock_hook_sysbase);
        current_hart = saved_hart;
    }
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
    if (lock != &p4_ipi_gate || held == NULL)
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
    memset(p4_ipi_slots, 0, sizeof(p4_ipi_slots));
    memset(&p4_ipi_gate, 0, sizeof(p4_ipi_gate));
    p4_ipi_prepared = 0u;
    memset(events, 0, sizeof(events));
    memset(sysbases, 0, sizeof(sysbases));
    memset(cpu_locals, 0, sizeof(cpu_locals));
    memset(runtimes, 0, sizeof(runtimes));
    memset(runtime_valid, 0, sizeof(runtime_valid));
    memset(route_calls, 0, sizeof(route_calls));
    memset(latch_writes, 0, sizeof(latch_writes));
    memset(latch_state, 0, sizeof(latch_state));
    memset(disable_saved_mie, 0, sizeof(disable_saved_mie));
    memset(disable_depth, 0, sizeof(disable_depth));
    memset(disable_calls_by_hart, 0, sizeof(disable_calls_by_hart));
    memset(enable_calls_by_hart, 0, sizeof(enable_calls_by_hart));
    memset(mock_mstatus_by_hart, 0, sizeof(mock_mstatus_by_hart));
    event_count = 0u;
    current_hart = 0u;
    disable_calls = 0u;
    enable_calls = 0u;
    init_calls = 0u;
    gate_held = 0;
    verify_drain_order = 0;
    unlock_hook_pending = 0;
    unlock_hook_hart = 0u;
    unlock_hook_sysbase = NULL;
    fatal_armed = 0;
    fatal_reason = P4_PUBLIC_SPIN_OK;
    for (uint32_t h = 0; h < 2u; ++h)
    {
        sysbases[h].id = h;
        cpu_locals[h].hart_id = h;
        runtimes[h].hart_id = h;
        runtimes[h].cpu_local = &cpu_locals[h];
        runtime_valid[h] = 1;
    }
}

static void prepare_ok(void)
{
    current_hart = 0u;
    mock_mstatus = 0u;
    CHECK(krnP4IpiPrepare() == 1);
}

static void arm_hart(uint32_t hart)
{
    unsigned before_disable = disable_calls_by_hart[hart];
    unsigned before_enable = enable_calls_by_hart[hart];
    current_hart = hart;
    mock_mstatus &= ~MSTATUS_MIE;
    krnP4IpiArm(&sysbases[hart]);
    CHECK(disable_calls_by_hart[hart] == before_disable);
    CHECK(enable_calls_by_hart[hart] == before_enable);
    CHECK(disable_depth[hart] == 0u);
    CHECK((mock_mstatus & MSTATUS_MIE) == 0u);
}

static struct p4_ipi_batch drain_hart(uint32_t hart)
{
    unsigned before_disable = disable_calls_by_hart[hart];
    unsigned before_enable = enable_calls_by_hart[hart];
    struct p4_ipi_batch batch;
    current_hart = hart;
    mock_mstatus &= ~MSTATUS_MIE;
    batch = krnP4IpiDrain(&sysbases[hart]);
    CHECK(disable_calls_by_hart[hart] == before_disable);
    CHECK(enable_calls_by_hart[hart] == before_enable);
    CHECK(disable_depth[hart] == 0u);
    CHECK((mock_mstatus & MSTATUS_MIE) == 0u);
    return batch;
}

static struct p4_signal_effect poke_effect(uint32_t hart)
{
    struct p4_signal_effect effect;
    memset(&effect, 0, sizeof(effect));
    effect.status = P4_SIGNAL_STATUS_OK;
    effect.request_reschedule = 1u;
    effect.reschedule_hart = hart;
    return effect;
}

static size_t first_event(int kind, size_t start)
{
    for (size_t i = start; i < event_count; ++i)
        if (events[i].kind == kind)
            return i;
    return event_count;
}

static size_t event_for(int kind, uint32_t hart, uint32_t value, size_t start)
{
    for (size_t i = start; i < event_count; ++i)
        if (events[i].kind == kind && events[i].hart == hart &&
            events[i].value == value)
            return i;
    return event_count;
}

static void test_prepare_admission(void)
{
    reset_fixture();
    mock_mstatus = MSTATUS_MIE;
    CHECK(krnP4IpiPrepare() == 0);
    CHECK(init_calls == 0u);
    CHECK(p4_ipi_prepared == 0u);
    mock_mstatus = 0u;
    current_hart = 1u;
    CHECK(krnP4IpiPrepare() == 0);
    CHECK(init_calls == 0u);
    current_hart = 2u;
    CHECK(krnP4IpiPrepare() == 0);
    CHECK(init_calls == 0u);
    current_hart = 0u;
    CHECK(krnP4IpiPrepare() == 1);
    CHECK(init_calls == 1u);
    CHECK(krnP4IpiPrepare() == 0);
    CHECK(init_calls == 1u);
    CHECK(p4_ipi_prepared == 1u);
}

static void test_prearm_and_both_harts(void)
{
    struct p4_signal_effect effect;
    size_t start, latch_clear, route, clic_clear, clic_enable, relatch, unlock;

    reset_fixture();
    prepare_ok();
    effect = poke_effect(1u);
    current_hart = 0u;
    krnP4SignalPoke(&effect, &sysbases[0]);
    CHECK(p4_ipi_slots[1].schedule_flags ==
          (P4_EXEC_SCHEDULE_SWITCH | P4_EXEC_SCHEDULE_DISPATCH));
    CHECK(p4_ipi_slots[1].armed == 0u);
    CHECK(latch_state[1] == 0u);

    event_count = 0u;
    arm_hart(0u);
    CHECK(p4_ipi_slots[0].armed == 1u);
    CHECK(route_calls[0] == 1u);
    CHECK(latch_state[0] == 0u);

    event_count = 0u;
    arm_hart(1u);
    CHECK(p4_ipi_slots[1].armed == 1u);
    CHECK(route_calls[1] == 1u);
    CHECK(latch_state[1] == 1u);
    CHECK(p4_ipi_slots[1].schedule_flags ==
          (P4_EXEC_SCHEDULE_SWITCH | P4_EXEC_SCHEDULE_DISPATCH));

    start = 0u;
    latch_clear = event_for(EVENT_LATCH, 1u, 0u, start);
    route = event_for(EVENT_ROUTE, 1u, 22u, start);
    clic_clear = event_for(EVENT_CLIC_CLEAR, 1u, P4_IPI_LINE, start);
    clic_enable = event_for(EVENT_CLIC_ENABLE, 1u, P4_IPI_LINE, start);
    relatch = event_for(EVENT_LATCH, 1u, 1u, start);
    unlock = first_event(EVENT_GATE_UNLOCK, start);
    CHECK(latch_clear < route);
    CHECK(route < clic_clear);
    CHECK(clic_clear < clic_enable);
    CHECK(clic_enable < relatch);
    CHECK(relatch < unlock);
    CHECK(!gate_held);
    CHECK(disable_depth[1] == 0u);
    CHECK((mock_mstatus & MSTATUS_MIE) == 0u);

    verify_drain_order = 1;
    current_hart = 1u;
    mock_mstatus = 0u;
    {
        struct p4_ipi_batch batch = drain_hart(1u);
        CHECK(batch.schedule_flags ==
              (P4_EXEC_SCHEDULE_SWITCH | P4_EXEC_SCHEDULE_DISPATCH));
        CHECK(batch.ticks == 0u);
    }
    verify_drain_order = 0;
    CHECK(latch_state[1] == 0u);
    CHECK(p4_ipi_slots[1].schedule_flags == 0u);
    CHECK(p4_ipi_slots[1].ticks == 0u);
    CHECK(first_event(EVENT_GATE_UNLOCK, 0u) < event_count);
}

static void test_postarm_coalescing_and_drain_order(void)
{
    struct p4_signal_effect local_effect, remote_effect;
    struct p4_ipi_batch batch;
    unsigned before_sets;
    size_t start, latch_clear, clic_clear, unlock;

    reset_fixture();
    prepare_ok();
    arm_hart(0u);
    arm_hart(1u);
    local_effect = poke_effect(1u);
    remote_effect = poke_effect(1u);

    current_hart = 1u;
    mock_mstatus = MSTATUS_MIE;
    before_sets = latch_writes[1][1];
    krnP4SignalPoke(&local_effect, &sysbases[1]);
    CHECK((mock_mstatus & MSTATUS_MIE) != 0u);
    CHECK(latch_state[1] == 1u);
    CHECK(latch_writes[1][1] == before_sets + 1u);

    current_hart = 0u;
    mock_mstatus = MSTATUS_MIE;
    krnP4SignalPoke(&remote_effect, &sysbases[0]);
    CHECK((mock_mstatus & MSTATUS_MIE) != 0u);
    CHECK(p4_ipi_slots[1].schedule_flags ==
          (P4_EXEC_SCHEDULE_SWITCH | P4_EXEC_SCHEDULE_DISPATCH));
    CHECK(latch_state[1] == 1u);
    CHECK(latch_writes[1][1] == before_sets + 2u);

    event_count = 0u;
    verify_drain_order = 1;
    current_hart = 1u;
    mock_mstatus = 0u;
    batch = drain_hart(1u);
    verify_drain_order = 0;
    CHECK(batch.schedule_flags ==
          (P4_EXEC_SCHEDULE_SWITCH | P4_EXEC_SCHEDULE_DISPATCH));
    CHECK(batch.ticks == 0u);
    CHECK(p4_ipi_slots[1].schedule_flags == 0u);
    CHECK(p4_ipi_slots[1].ticks == 0u);
    CHECK(latch_state[1] == 0u);

    start = 0u;
    latch_clear = event_for(EVENT_LATCH, 1u, 0u, start);
    clic_clear = event_for(EVENT_CLIC_CLEAR, 1u, P4_IPI_LINE, start);
    unlock = first_event(EVENT_GATE_UNLOCK, start);
    CHECK(latch_clear < clic_clear);
    CHECK(clic_clear < unlock);
    CHECK(events[unlock].kind == EVENT_GATE_UNLOCK);

    /* An empty drain is harmless and stays empty. */
    batch = drain_hart(1u);
    CHECK(batch.schedule_flags == 0u);
    CHECK(batch.ticks == 0u);
}

static void test_publish_after_unlock_not_lost(void)
{
    struct p4_signal_effect before, during;
    struct p4_ipi_batch first, second;
    size_t drain_unlock, relatch;

    reset_fixture();
    prepare_ok();
    arm_hart(0u);
    arm_hart(1u);
    before = poke_effect(1u);
    during = poke_effect(1u);
    current_hart = 0u;
    krnP4SignalPoke(&before, &sysbases[0]);
    krnP4IpiPostTick(1u, &sysbases[0]);
    CHECK(p4_ipi_slots[1].ticks == 1u);

    /* Inject a remote producer after the drain releases the shared gate,
     * while the receiving hart still has MIE clear in its trap. */
    unlock_hook_effect = during;
    unlock_hook_hart = 0u;
    unlock_hook_sysbase = &sysbases[0];
    unlock_hook_pending = 1;
    current_hart = 1u;
    mock_mstatus = 0u;
    mock_mstatus_by_hart[0] = MSTATUS_MIE;
    event_count = 0u;
    verify_drain_order = 1;
    first = drain_hart(1u);
    verify_drain_order = 0;
    CHECK(first.schedule_flags ==
          (P4_EXEC_SCHEDULE_SWITCH | P4_EXEC_SCHEDULE_DISPATCH));
    CHECK(first.ticks == 1u);
    CHECK(unlock_hook_pending == 0);
    CHECK(!gate_held);
    CHECK(mock_mstatus_by_hart[1] == 0u);
    CHECK((mock_mstatus_by_hart[0] & MSTATUS_MIE) != 0u);
    CHECK(disable_depth[0] == 0u);
    CHECK(disable_depth[1] == 0u);
    CHECK(p4_ipi_slots[1].schedule_flags ==
          (P4_EXEC_SCHEDULE_SWITCH | P4_EXEC_SCHEDULE_DISPATCH));
    CHECK(p4_ipi_slots[1].ticks == 0u);
    CHECK(latch_state[1] == 1u);
    drain_unlock = event_for(EVENT_GATE_UNLOCK, 1u, 0u, 0u);
    relatch = event_for(EVENT_LATCH, 1u, 1u, drain_unlock + 1u);
    CHECK(drain_unlock < relatch);

    second = drain_hart(1u);
    CHECK(second.schedule_flags ==
          (P4_EXEC_SCHEDULE_SWITCH | P4_EXEC_SCHEDULE_DISPATCH));
    CHECK(second.ticks == 0u);
    CHECK(p4_ipi_slots[1].schedule_flags == 0u);
    CHECK(latch_state[1] == 0u);
}

static void test_tick_admission_and_overflow(void)
{
    struct p4_ipi_batch batch;

    reset_fixture();
    prepare_ok();
    current_hart = 0u;
    krnP4IpiPostTick(1u, &sysbases[0]);
    CHECK(p4_ipi_slots[1].ticks == 0u);
    CHECK(latch_state[1] == 0u);
    arm_hart(1u);
    CHECK(p4_ipi_slots[1].ticks == 0u);
    current_hart = 0u;
    mock_mstatus = MSTATUS_MIE;
    krnP4IpiPostTick(1u, &sysbases[0]);
    krnP4IpiPostTick(1u, &sysbases[0]);
    CHECK(p4_ipi_slots[1].ticks == 2u);
    CHECK(latch_state[1] == 1u);
    current_hart = 1u;
    mock_mstatus = 0u;
    batch = drain_hart(1u);
    CHECK(batch.ticks == 2u);
    CHECK(batch.schedule_flags == 0u);

    current_hart = 0u;
    mock_mstatus = 0u;
    EXPECT_FATAL(P4_PUBLIC_SPIN_BAD_STATE,
                 krnP4IpiPostTick(0u, &sysbases[0]));
    CHECK(p4_ipi_slots[0].ticks == 0u);
    CHECK(p4_ipi_slots[1].ticks == 0u);
    reset_fixture();
    prepare_ok();
    EXPECT_FATAL(P4_PUBLIC_SPIN_BAD_ARGUMENT,
                 krnP4IpiPostTick(2u, &sysbases[0]));
    CHECK(!gate_held);

    reset_fixture();
    prepare_ok();
    arm_hart(1u);
    current_hart = 1u;
    mock_mstatus = 0u;
    EXPECT_FATAL(P4_PUBLIC_SPIN_BAD_STATE,
                 krnP4IpiPostTick(1u, &sysbases[1]));
    CHECK(p4_ipi_slots[1].ticks == 0u);
    CHECK(latch_writes[1][1] == 0u);
    CHECK(gate_held);

    reset_fixture();
    prepare_ok();
    arm_hart(1u);
    p4_ipi_slots[1].ticks = UINT32_MAX;
    current_hart = 0u;
    mock_mstatus = 0u;
    EXPECT_FATAL(P4_PUBLIC_SPIN_BAD_STATE,
                 krnP4IpiPostTick(1u, &sysbases[0]));
    CHECK(p4_ipi_slots[1].ticks == UINT32_MAX);
    CHECK(latch_state[1] == 0u);
    CHECK(gate_held); /* Fatal is non-returning in production. */
}

static void test_fail_closed_guards(void)
{
    struct p4_signal_effect effect, invalid;
    unsigned disables;
    unsigned before_latch[2][2];

    reset_fixture();
    effect = poke_effect(1u);
    EXPECT_FATAL(P4_PUBLIC_SPIN_BAD_STATE,
                 krnP4SignalPoke(&effect, &sysbases[0]));
    CHECK(p4_ipi_slots[1].schedule_flags == 0u);
    reset_fixture();
    prepare_ok();
    runtime_valid[0] = 0;
    EXPECT_FATAL(P4_PUBLIC_SPIN_BAD_STATE,
                 krnP4SignalPoke(&effect, &sysbases[0]));
    CHECK(p4_ipi_slots[1].schedule_flags == 0u);

    reset_fixture();
    prepare_ok();
    current_hart = 2u;
    EXPECT_FATAL(P4_PUBLIC_SPIN_BAD_STATE,
                 krnP4SignalPoke(&effect, &sysbases[0]));
    CHECK(p4_ipi_slots[1].schedule_flags == 0u);
    CHECK(!gate_held);

    reset_fixture();
    prepare_ok();
    cpu_locals[0].hart_id = 1u;
    EXPECT_FATAL(P4_PUBLIC_SPIN_BAD_STATE,
                 krnP4SignalPoke(&effect, &sysbases[0]));
    CHECK(p4_ipi_slots[1].schedule_flags == 0u);

    reset_fixture();
    prepare_ok();
    disables = disable_calls;
    krnP4SignalPoke(NULL, &sysbases[0]);
    invalid = poke_effect(1u);
    invalid.status = P4_SIGNAL_STATUS_INVALID_STATE;
    krnP4SignalPoke(&invalid, &sysbases[0]);
    invalid.status = P4_SIGNAL_STATUS_OK;
    invalid.request_reschedule = 0u;
    krnP4SignalPoke(&invalid, &sysbases[0]);
    CHECK(disable_calls == disables);
    CHECK(p4_ipi_slots[1].schedule_flags == 0u);
    CHECK(latch_state[1] == 0u);
    invalid = poke_effect(2u);
    EXPECT_FATAL(P4_PUBLIC_SPIN_BAD_ARGUMENT,
                 krnP4SignalPoke(&invalid, &sysbases[0]));
    CHECK(p4_ipi_slots[1].schedule_flags == 0u);

    reset_fixture();
    prepare_ok();
    effect = poke_effect(0u);
    arm_hart(0u);
    before_latch[0][0] = latch_writes[0][0];
    before_latch[0][1] = latch_writes[0][1];
    mock_mstatus = MSTATUS_MIE;
    EXPECT_FATAL(P4_PUBLIC_SPIN_BAD_STATE,
                 krnP4IpiArm(&sysbases[0]));
    CHECK(latch_writes[0][0] == before_latch[0][0]);
    CHECK(latch_writes[0][1] == before_latch[0][1]);
    CHECK(route_calls[0] == 1u);

    current_hart = 0u;
    mock_mstatus = 0u;
    EXPECT_FATAL(P4_PUBLIC_SPIN_BAD_STATE,
                 krnP4IpiArm(&sysbases[0]));
    CHECK(latch_writes[0][0] == before_latch[0][0]);
    CHECK(route_calls[0] == 1u);

    reset_fixture();
    prepare_ok();
    mock_mstatus = MSTATUS_MIE;
    EXPECT_FATAL(P4_PUBLIC_SPIN_BAD_STATE,
                 krnP4IpiDrain(&sysbases[0]));
    CHECK(!gate_held);

    reset_fixture();
    prepare_ok();
    current_hart = 1u;
    mock_mstatus = 0u;
    EXPECT_FATAL(P4_PUBLIC_SPIN_BAD_STATE,
                 krnP4IpiDrain(&sysbases[1]));
    CHECK(latch_writes[1][0] == 0u);
    CHECK(latch_writes[1][1] == 0u);
    CHECK(gate_held); /* Missing arm is fatal; no partial acknowledgement. */
}

static void test_cleanup_doorbell(void)
{
    struct p4_ipi_batch batch;
    unsigned disable_before, enable_before;
    unsigned route_before[2];
    unsigned latch_before[2][2];
    size_t start, latch0, latch1, unlock;
    const uint32_t schedule =
        P4_EXEC_SCHEDULE_SWITCH | P4_EXEC_SCHEDULE_DISPATCH;

    /* Both destinations are unarmed: publish sticky mailbox flags only.
     * No remote CLIC/MMIO access is allowed before owner-hart arm. */
    reset_fixture();
    prepare_ok();
    current_hart = 1u;
    mock_mstatus = MSTATUS_MIE;
    disable_before = disable_calls;
    enable_before = enable_calls;
    route_before[0] = route_calls[0];
    route_before[1] = route_calls[1];
    krnP4CleanupPoke(&sysbases[1]);
    CHECK(p4_ipi_slots[0].schedule_flags == schedule);
    CHECK(p4_ipi_slots[1].schedule_flags == schedule);
    CHECK(latch_state[0] == 0u);
    CHECK(latch_state[1] == 0u);
    CHECK(latch_writes[0][1] == 0u && latch_writes[1][1] == 0u);
    CHECK(route_calls[0] == route_before[0] && route_calls[1] == route_before[1]);
    CHECK(disable_calls == disable_before + 1u);
    CHECK(enable_calls == enable_before + 1u);
    CHECK(disable_depth[1] == 0u && !gate_held);
    CHECK(mock_mstatus == MSTATUS_MIE);

    /* A trap already entered with MIE clear must stay masked; an existing
     * caller Disable must retain its outer nesting across the fanout. */
    current_hart = 1u;
    mock_mstatus = 0u;
    disable_before = disable_calls;
    enable_before = enable_calls;
    krnP4CleanupPoke(&sysbases[1]);
    CHECK(mock_mstatus == 0u);
    CHECK(disable_calls == disable_before + 1u);
    CHECK(enable_calls == enable_before + 1u);
    CHECK(disable_depth[1] == 0u && !gate_held);
    Disable();
    CHECK(mock_mstatus == 0u && disable_depth[1] == 1u);
    disable_before = disable_calls;
    enable_before = enable_calls;
    krnP4CleanupPoke(&sysbases[1]);
    CHECK(mock_mstatus == 0u && disable_depth[1] == 1u);
    CHECK(disable_calls == disable_before + 1u);
    CHECK(enable_calls == enable_before + 1u);
    CHECK(!gate_held);
    Enable();
    CHECK(disable_depth[1] == 0u && mock_mstatus == 0u);

    /* Each owner arms its own CLIC line and relatches only its own retained
     * work. Arming one hart does not touch the other hart's controller. */
    route_before[0] = route_calls[0];
    route_before[1] = route_calls[1];
    arm_hart(0u);
    CHECK(route_calls[0] == route_before[0] + 1u);
    CHECK(route_calls[1] == route_before[1]);
    CHECK(latch_state[0] == 1u);
    CHECK(latch_state[1] == 0u);
    batch = krnP4IpiDrain(&sysbases[0]);
    CHECK(batch.schedule_flags == schedule);
    CHECK(batch.ticks == 0u);
    CHECK(latch_state[0] == 0u);

    route_before[0] = route_calls[0];
    route_before[1] = route_calls[1];
    arm_hart(1u);
    CHECK(route_calls[0] == route_before[0]);
    CHECK(route_calls[1] == route_before[1] + 1u);
    CHECK(latch_state[1] == 1u);
    batch = krnP4IpiDrain(&sysbases[1]);
    CHECK(batch.schedule_flags == schedule);
    CHECK(batch.ticks == 0u);
    CHECK(latch_state[1] == 0u);

    /* One armed destination receives a latch; the unarmed destination keeps
     * its sticky flags and is latched by its own later arm. */
    reset_fixture();
    prepare_ok();
    arm_hart(1u);
    current_hart = 0u;
    mock_mstatus = MSTATUS_MIE;
    disable_before = disable_calls;
    enable_before = enable_calls;
    krnP4CleanupPoke(&sysbases[0]);
    CHECK(p4_ipi_slots[0].schedule_flags == schedule);
    CHECK(p4_ipi_slots[1].schedule_flags == schedule);
    CHECK(latch_state[0] == 0u);
    CHECK(latch_state[1] == 1u);
    CHECK(disable_calls == disable_before + 1u);
    CHECK(enable_calls == enable_before + 1u);
    CHECK(mock_mstatus == MSTATUS_MIE && !gate_held);
    batch = drain_hart(1u);
    CHECK(batch.schedule_flags == schedule && batch.ticks == 0u);
    CHECK(latch_state[1] == 0u);
    arm_hart(0u);
    CHECK(latch_state[0] == 1u);
    batch = drain_hart(0u);
    CHECK(batch.schedule_flags == schedule && batch.ticks == 0u);

    /* With both armed, repeated cleanup notifications coalesce in each slot,
     * publish both latches under one gate hold, and preserve caller MIE. */
    current_hart = 0u;
    mock_mstatus = MSTATUS_MIE;
    latch_before[0][0] = latch_writes[0][0];
    latch_before[0][1] = latch_writes[0][1];
    latch_before[1][0] = latch_writes[1][0];
    latch_before[1][1] = latch_writes[1][1];
    disable_before = disable_calls;
    enable_before = enable_calls;
    start = event_count;
    krnP4CleanupPoke(&sysbases[0]);
    krnP4CleanupPoke(&sysbases[0]);
    CHECK(latch_state[0] == 1u);
    CHECK(latch_state[1] == 1u);
    CHECK(p4_ipi_slots[0].schedule_flags == schedule);
    CHECK(p4_ipi_slots[1].schedule_flags == schedule);
    CHECK(disable_calls == disable_before + 2u);
    CHECK(enable_calls == enable_before + 2u);
    CHECK(disable_depth[0] == 0u && !gate_held);
    CHECK(mock_mstatus == MSTATUS_MIE);
    CHECK(latch_writes[0][1] == latch_before[0][1] + 2u);
    CHECK(latch_writes[1][1] == latch_before[1][1] + 2u);
    CHECK(latch_writes[0][0] == latch_before[0][0]);
    CHECK(latch_writes[1][0] == latch_before[1][0]);
    latch0 = event_for(EVENT_LATCH, 0u, 1u, start);
    latch1 = event_for(EVENT_LATCH, 1u, 1u, start);
    unlock = event_for(EVENT_GATE_UNLOCK, 0u, 0u, start);
    CHECK(latch0 < unlock && latch1 < unlock);
    CHECK(event_for(EVENT_GATE_UNLOCK, 0u, 0u, unlock + 1u) < event_count);

    batch = drain_hart(0u);
    CHECK(batch.schedule_flags == schedule);
    CHECK(batch.ticks == 0u);
    batch = drain_hart(0u);
    CHECK(batch.schedule_flags == 0u);
    batch = drain_hart(1u);
    CHECK(batch.schedule_flags == schedule);
    CHECK(batch.ticks == 0u);
    batch = drain_hart(1u);
    CHECK(batch.schedule_flags == 0u);
}

int krnP4CleanupWorkerReady(struct ExecBase *SysBase)
{
    CHECK(SysBase == &sysbases[current_hart]);
    CHECK(!gate_held);
    CHECK(mock_mstatus == 0u);
    CHECK(cpu_locals[current_hart].id_nest >= 0);
    return cleanup_ready;
}

static void test_secondary_cleanup_admission(void)
{
    reset_fixture();
    prepare_ok();
    current_hart = 1u;
    mock_mstatus = 0u;
    cleanup_ready = 0;
    EXPECT_FATAL(P4_PUBLIC_SPIN_BAD_STATE, krnP4IpiArm(&sysbases[1]));
    CHECK(!gate_held && p4_ipi_slots[1].armed == 0u);
    CHECK(route_calls[1] == 0u && latch_writes[1][0] == 0u);
    cleanup_ready = 1;
    cpu_locals[1].id_nest = -1;
    EXPECT_FATAL(P4_PUBLIC_SPIN_BAD_STATE, krnP4IpiArm(&sysbases[1]));
    CHECK(!gate_held && p4_ipi_slots[1].armed == 0u);
    CHECK(route_calls[1] == 0u && latch_writes[1][0] == 0u);
    cpu_locals[1].id_nest = 0;
    krnP4IpiArm(&sysbases[1]);
    CHECK(p4_ipi_slots[1].armed == 1u);
}

static void test_retired_owner_poke(void)
{
    const uint32_t schedule =
        P4_EXEC_SCHEDULE_SWITCH | P4_EXEC_SCHEDULE_DISPATCH;
    unsigned disable_before, enable_before;
    unsigned latch_before[2];
    size_t start, unlock, enable;
    struct p4_ipi_batch batch;

    /* The publisher API carries only a copied hart identity. Before either
     * owner arms, each destination keeps its own sticky flags and no remote
     * CLIC latch is touched. */
    reset_fixture();
    prepare_ok();
    current_hart = 0u;
    mock_mstatus = 0u;
    krnP4RetiredOwnerPoke(0u, &sysbases[0]);
    krnP4RetiredOwnerPoke(1u, &sysbases[0]);
    CHECK(p4_ipi_slots[0].schedule_flags == schedule);
    CHECK(p4_ipi_slots[1].schedule_flags == schedule);
    CHECK(p4_ipi_slots[0].armed == 0u && p4_ipi_slots[1].armed == 0u);
    CHECK(latch_state[0] == 0u && latch_state[1] == 0u);
    CHECK(latch_writes[0][1] == 0u && latch_writes[1][1] == 0u);

    arm_hart(0u);
    CHECK(latch_state[0] == 1u && latch_state[1] == 0u);
    batch = drain_hart(0u);
    CHECK(batch.schedule_flags == schedule && batch.ticks == 0u);
    arm_hart(1u);
    CHECK(latch_state[1] == 1u);
    batch = drain_hart(1u);
    CHECK(batch.schedule_flags == schedule && batch.ticks == 0u);

    /* Once armed, each call latches only its explicit destination. */
    reset_fixture();
    prepare_ok();
    arm_hart(0u);
    arm_hart(1u);
    latch_before[0] = latch_writes[0][1];
    latch_before[1] = latch_writes[1][1];

    current_hart = 0u;
    mock_mstatus = MSTATUS_MIE;
    disable_before = disable_calls;
    enable_before = enable_calls;
    krnP4RetiredOwnerPoke(1u, &sysbases[0]);
    CHECK(p4_ipi_slots[1].schedule_flags == schedule);
    CHECK(p4_ipi_slots[0].schedule_flags == 0u);
    CHECK(latch_state[1] == 1u && latch_state[0] == 0u);
    CHECK(latch_writes[1][1] == latch_before[1] + 1u);
    CHECK(latch_writes[0][1] == latch_before[0]);
    CHECK(disable_calls == disable_before + 1u);
    CHECK(enable_calls == enable_before + 1u);
    CHECK(mock_mstatus == MSTATUS_MIE && disable_depth[0] == 0u);
    batch = drain_hart(1u);
    CHECK(batch.schedule_flags == schedule && batch.ticks == 0u);

    /* A remote owner poke nested inside an existing Disable must not reopen
     * MIE or consume the caller's outer nesting. */
    current_hart = 1u;
    mock_mstatus = MSTATUS_MIE;
    Disable();
    CHECK(disable_depth[1] == 1u && mock_mstatus == 0u);
    disable_before = disable_calls;
    enable_before = enable_calls;
    start = event_count;
    krnP4RetiredOwnerPoke(0u, &sysbases[1]);
    CHECK(disable_calls == disable_before + 1u);
    CHECK(enable_calls == enable_before + 1u);
    CHECK(disable_depth[1] == 1u && mock_mstatus == 0u);
    CHECK(!gate_held);
    CHECK(p4_ipi_slots[0].schedule_flags == schedule);
    CHECK(p4_ipi_slots[1].schedule_flags == 0u);
    CHECK(latch_state[0] == 1u && latch_state[1] == 0u);
    unlock = event_for(EVENT_GATE_UNLOCK, 1u, 0u, start);
    enable = event_for(EVENT_ENABLE, 1u, 0u, start);
    CHECK(unlock < enable && enable < event_count);
    Enable();
    CHECK(disable_depth[1] == 0u && mock_mstatus == MSTATUS_MIE);
    batch = drain_hart(0u);
    CHECK(batch.schedule_flags == schedule && batch.ticks == 0u);

    /* Destination validation is fail-closed before Disable, gate acquisition,
     * mailbox publication, or latch access. */
    reset_fixture();
    prepare_ok();
    current_hart = 0u;
    mock_mstatus = MSTATUS_MIE;
    disable_before = disable_calls;
    enable_before = enable_calls;
    start = event_count;
    EXPECT_FATAL(P4_PUBLIC_SPIN_BAD_ARGUMENT,
                 krnP4RetiredOwnerPoke(2u, &sysbases[0]));
    EXPECT_FATAL(P4_PUBLIC_SPIN_BAD_ARGUMENT,
                 krnP4RetiredOwnerPoke(UINT32_MAX, &sysbases[0]));
    CHECK(disable_calls == disable_before);
    CHECK(enable_calls == enable_before);
    CHECK(event_count == start);
    CHECK(!gate_held && disable_depth[0] == 0u);
    CHECK(mock_mstatus == MSTATUS_MIE);
    CHECK(p4_ipi_slots[0].schedule_flags == 0u);
    CHECK(p4_ipi_slots[1].schedule_flags == 0u);
    CHECK(latch_state[0] == 0u && latch_state[1] == 0u);
}

int main(void)
{
    test_prepare_admission();
    test_prearm_and_both_harts();
    test_postarm_coalescing_and_drain_order();
    test_publish_after_unlock_not_lost();
    test_tick_admission_and_overflow();
    test_fail_closed_guards();
    test_cleanup_doorbell();
    test_secondary_cleanup_admission();
    test_retired_owner_poke();
    printf("p4-ipi-test: PASS (%u checks)\n", checks);
    return 0;
}
