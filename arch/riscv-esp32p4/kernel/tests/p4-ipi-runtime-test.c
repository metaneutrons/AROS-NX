/* Host fixture for the actual owner-only IPI batch application helper. */
#include <setjmp.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

struct ExecBase
{
    uint32_t identity;
};

struct KernelBase
{
    uint32_t identity;
};

struct Task
{
    uint32_t identity;
};

static uint32_t mock_hart_id;
static uint32_t mock_mstatus;

#define P4_CPU_LOCAL_READ_MHARTID 0u
#include "../cpu_local_runtime.h"
#include "../p4_ipi.h"

#define MSTATUS_MIE UINT32_C(0x00000008)
#define mstatus 0
#define csr_read(reg) ((void)(reg), mock_mstatus)
#define P4_PUBLIC_SPINLOCK_TEST 1
#include "../p4_public_spinlock.h"

#define CHECK(condition) do { \
    ++checks; \
    if (!(condition)) { \
        ++failures; \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
    } \
} while (0)

struct GlobalState
{
    uint32_t vblank_count;
    uint32_t softint_count;
    uint32_t dos_tick_count;
    uint32_t callback_count;
};

struct Fixture
{
    struct ExecBase base;
    struct KernelBase kernel;
    struct Task tasks[2];
    struct p4_cpu_local local;
    struct p4_exec_runtime runtime;
    struct p4_ipi_batch batch;
    struct GlobalState global;
};

static struct Fixture fixture;
static struct Fixture fixture_before;
static struct ExecBase *mock_sysbase;
static struct p4_exec_runtime *mock_runtime;
static jmp_buf fatal_jump;
static unsigned int checks;
static unsigned int failures;
static unsigned int fatal_count;
static unsigned int fatal_reason;
static int fatal_armed;

struct p4_exec_runtime *p4_exec_runtime_current(struct ExecBase *sys_base)
{
    if (sys_base != mock_sysbase || mock_runtime == NULL ||
        mock_runtime->hart_id != mock_hart_id)
        return NULL;
    return mock_runtime;
}

#define P4_IPI_RUNTIME_TEST 1
#include "../p4_ipi_runtime.c"

void krnP4PublicSpinFatal(enum p4_public_spinlock_result reason)
    __attribute__((noreturn));
void krnP4PublicSpinFatal(enum p4_public_spinlock_result reason)
{
    ++fatal_count;
    fatal_reason = (unsigned int)reason;
    if (fatal_armed)
        longjmp(fatal_jump, 1);
    __builtin_trap();
}

static void fixture_reset(uint32_t hart)
{
    memset(&fixture, 0, sizeof(fixture));
    fixture.base.identity = 0xA000u + hart;
    fixture.kernel.identity = 0xB000u + hart;
    fixture.tasks[0].identity = 0xC000u;
    fixture.tasks[1].identity = 0xC001u;
    fixture.local.hart_id = hart;
    fixture.local.current_task = &fixture.tasks[0];
    fixture.local.sys_base = &fixture.base;
    fixture.local.kernel_base = &fixture.kernel;
    fixture.local.trap_depth = 1u;
    fixture.local.schedule_flags = P4_EXEC_SCHEDULE_QUANTUM | (1u << 12);
    fixture.local.id_nest = -3;
    fixture.local.td_nest = 7;
    fixture.local.idle_count = 23u;
    fixture.local.ticks = 101u;
    fixture.local.fpu_owner = &fixture.tasks[1];
    fixture.runtime.hart_id = hart;
    fixture.runtime.initialized = 1u;
    fixture.runtime.cpu_local = &fixture.local;
    fixture.runtime.stage = P4_EXEC_RUNTIME_STAGE_FINALIZED;
    fixture.runtime.quantum = 4u;
    fixture.runtime.elapsed = 9u;
    fixture.runtime.dispatch_count = 0x55u;
    fixture.runtime.wait_pending = &fixture.tasks[1];
    memset(fixture.runtime.reserved, 0xA5, sizeof(fixture.runtime.reserved));
    fixture.global.vblank_count = 31u;
    fixture.global.softint_count = 37u;
    fixture.global.dos_tick_count = 41u;
    fixture.global.callback_count = 43u;
    fixture.batch.schedule_flags = 0u;
    fixture.batch.ticks = 0u;
    mock_hart_id = hart;
    mock_mstatus = 0u;
    mock_sysbase = &fixture.base;
    mock_runtime = &fixture.runtime;
    fatal_count = 0u;
    fatal_reason = 0u;
    fatal_armed = 0;
}

static void expect_fixture_unchanged(void)
{
    CHECK(memcmp(&fixture, &fixture_before, sizeof(fixture)) == 0);
}

static void expect_refusal(const struct p4_ipi_batch *batch,
                           struct ExecBase *sys_base)
{
    memcpy(&fixture_before, &fixture, sizeof(fixture));
    fatal_count = 0u;
    fatal_reason = 0u;
    fatal_armed = 1;
    if (setjmp(fatal_jump) == 0)
    {
        krnP4IpiApplyBatch(batch, sys_base);
        fatal_armed = 0;
        CHECK(0 && "invalid batch application returned");
    }
    else
        fatal_armed = 0;

    CHECK(fatal_count == 1u);
    CHECK(fatal_reason == (unsigned int)P4_PUBLIC_SPIN_BAD_STATE);
    expect_fixture_unchanged();
}

static void expect_success(struct p4_ipi_batch *batch,
                           uint32_t expected_flags, uint32_t expected_ticks,
                           uint32_t expected_elapsed)
{
    struct p4_cpu_local expected_local;
    struct p4_exec_runtime expected_runtime;
    struct GlobalState expected_global;

    memcpy(&expected_local, &fixture.local, sizeof(expected_local));
    memcpy(&expected_runtime, &fixture.runtime, sizeof(expected_runtime));
    memcpy(&expected_global, &fixture.global, sizeof(expected_global));
    expected_local.schedule_flags |= expected_flags;
    expected_local.ticks += expected_ticks;
    expected_runtime.elapsed = expected_elapsed;

    krnP4IpiApplyBatch(batch, &fixture.base);

    CHECK(memcmp(&fixture.local, &expected_local, sizeof(expected_local)) == 0);
    CHECK(memcmp(&fixture.runtime, &expected_runtime,
                 sizeof(expected_runtime)) == 0);
    CHECK(memcmp(&fixture.global, &expected_global,
                 sizeof(expected_global)) == 0);
}

static void test_hart0_flags_without_ticks(void)
{
    fixture_reset(0u);
    fixture.runtime.elapsed = 6u;
    fixture.batch.schedule_flags = P4_EXEC_SCHEDULE_SWITCH |
                                   P4_EXEC_SCHEDULE_DISPATCH;
    fixture.batch.ticks = 0u;
    expect_success(&fixture.batch,
                   P4_EXEC_SCHEDULE_SWITCH | P4_EXEC_SCHEDULE_DISPATCH,
                   0u, 6u);
}

static void test_hart1_flags_and_tick_elapsed_cases(void)
{
    struct
    {
        uint32_t elapsed;
        uint32_t ticks;
        uint32_t expected_elapsed;
        uint32_t expected_tick_flags;
    } cases[] = {
        { 9u, 4u, 5u, 0u },       /* below remaining quantum */
        { 4u, 4u, 0u, P4_EXEC_SCHEDULE_QUANTUM |
                         P4_EXEC_SCHEDULE_SWITCH }, /* exact expiry */
        { 3u, 8u, 0u, P4_EXEC_SCHEDULE_QUANTUM |
                         P4_EXEC_SCHEDULE_SWITCH }, /* saturates at zero */
        { 0u, 0u, 0u, 0u },       /* no tick does not expire zero */
        { 0u, 1u, 0u, P4_EXEC_SCHEDULE_QUANTUM |
                         P4_EXEC_SCHEDULE_SWITCH }  /* tick at zero */
    };
    unsigned int index;

    for (index = 0; index < sizeof(cases) / sizeof(cases[0]); ++index)
    {
        fixture_reset(1u);
        /* Start without QUANTUM so expiry must set it, while preserving an
         * unrelated pre-existing per-hart flag. */
        fixture.local.schedule_flags = UINT32_C(1) << 12;
        fixture.runtime.elapsed = cases[index].elapsed;
        fixture.batch.schedule_flags = P4_EXEC_SCHEDULE_DISPATCH;
        fixture.batch.ticks = cases[index].ticks;
        expect_success(&fixture.batch,
                       P4_EXEC_SCHEDULE_DISPATCH |
                           cases[index].expected_tick_flags,
                       cases[index].ticks, cases[index].expected_elapsed);
        CHECK((fixture.local.schedule_flags & (1u << 12)) != 0u);
        CHECK(fixture.local.current_task == &fixture.tasks[0]);
        CHECK(fixture.local.fpu_owner == &fixture.tasks[1]);
        CHECK(fixture.local.id_nest == -3 && fixture.local.td_nest == 7);
    }
}

static void test_diagnostic_tick_counter_wraps_unsigned(void)
{
    fixture_reset(1u);
    fixture.local.schedule_flags = UINT32_C(1) << 12;
    fixture.local.ticks = UINT32_MAX - 1u;
    fixture.runtime.elapsed = 12u;
    fixture.batch.schedule_flags = 0u;
    fixture.batch.ticks = 3u;
    expect_success(&fixture.batch, 0u, 3u, 9u);
    CHECK(fixture.local.ticks == 1u);
}

static void test_refusals_do_not_mutate(void)
{
    fixture_reset(1u);
    fixture.batch.schedule_flags = P4_EXEC_SCHEDULE_SWITCH;

    expect_refusal(NULL, &fixture.base);
    expect_refusal(&fixture.batch, NULL);

    fixture_reset(1u);
    fixture.batch.schedule_flags = P4_EXEC_SCHEDULE_SWITCH;
    mock_runtime = NULL;
    expect_refusal(&fixture.batch, &fixture.base);

    fixture_reset(1u);
    fixture.batch.schedule_flags = P4_EXEC_SCHEDULE_SWITCH;
    expect_refusal(&fixture.batch, (struct ExecBase *)(uintptr_t)1u);

    fixture_reset(0u);
    fixture.batch.schedule_flags = P4_EXEC_SCHEDULE_SWITCH;
    fixture.runtime.hart_id = 2u;
    fixture.local.hart_id = 2u;
    mock_hart_id = 2u;
    expect_refusal(&fixture.batch, &fixture.base);

    fixture_reset(1u);
    fixture.batch.schedule_flags = P4_EXEC_SCHEDULE_SWITCH;
    fixture.runtime.cpu_local = NULL;
    expect_refusal(&fixture.batch, &fixture.base);

    fixture_reset(1u);
    fixture.batch.schedule_flags = P4_EXEC_SCHEDULE_SWITCH;
    fixture.local.hart_id = 0u;
    expect_refusal(&fixture.batch, &fixture.base);

    fixture_reset(1u);
    fixture.batch.schedule_flags = P4_EXEC_SCHEDULE_SWITCH;
    fixture.local.trap_depth = 0u;
    expect_refusal(&fixture.batch, &fixture.base);

    fixture_reset(1u);
    fixture.batch.schedule_flags = P4_EXEC_SCHEDULE_SWITCH;
    mock_mstatus = MSTATUS_MIE;
    expect_refusal(&fixture.batch, &fixture.base);

    fixture_reset(1u);
    fixture.batch.schedule_flags = P4_EXEC_SCHEDULE_QUANTUM;
    expect_refusal(&fixture.batch, &fixture.base);

    fixture_reset(1u);
    fixture.batch.schedule_flags = UINT32_C(1) << 9;
    expect_refusal(&fixture.batch, &fixture.base);

    fixture_reset(0u);
    fixture.batch.schedule_flags = P4_EXEC_SCHEDULE_SWITCH;
    fixture.batch.ticks = 1u;
    expect_refusal(&fixture.batch, &fixture.base);

    /* A returned runtime from a non-owning hart is not accepted by the
     * current-runtime mock, matching its real mhartid-selection contract. */
    fixture_reset(0u);
    fixture.batch.schedule_flags = P4_EXEC_SCHEDULE_SWITCH;
    mock_hart_id = 0u;
    mock_runtime->hart_id = 1u;
    mock_runtime->cpu_local = &fixture.local;
    fixture.local.hart_id = 1u;
    expect_refusal(&fixture.batch, &fixture.base);
}

int main(void)
{
    checks = 0u;
    failures = 0u;
    test_hart0_flags_without_ticks();
    test_hart1_flags_and_tick_elapsed_cases();
    test_diagnostic_tick_counter_wraps_unsigned();
    test_refusals_do_not_mutate();
    printf("P4 IPI runtime fixture: %u checks, %u failures\n",
           checks, failures);
    return failures == 0u ? 0 : 1;
}
