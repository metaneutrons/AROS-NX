/*
 * Actual-source host fixture for the SMP P4 SD-card timer adapter.
 * It reuses the kernel snapshot fixture, includes both production source
 * bodies, and models the public lock service and register boundary. This is
 * not real concurrent execution or hardware evidence.
 */
#include <stdint.h>

#define __AROSEXEC_SMP__ 1
#define main p4_timer_snapshot_fixture_main
#include "../../kernel/tests/p4-timer-snapshot-test.c"
#undef main

typedef uintptr_t IPTR;
typedef uint32_t ULONG;
typedef uint64_t UQUAD;
typedef int16_t BOOL;

#define TRUE ((BOOL)1)
#define FALSE ((BOOL)0)
/* TAG_USER + KATTR_CPULoad offset + 32-entry end + PlatformTimer offset. */
#define KATTR_PlatformTimer UINT32_C(0x83f0002c)
#define P4_SD_TIME_TEST 1
#define KrnGetSystemAttr(attribute) \
    mock_get_system_attr((uint32_t)(attribute))
#define P4_SD_TIME_FATAL() mock_sd_time_fatal()

struct SDCardBase
{
    uint32_t test_cookie;
};

static IPTR mock_get_system_attr(uint32_t attribute);
static void mock_sd_time_fatal(void) __attribute__((noreturn));

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-parameter"
#include "../sdcard_esp32p4_time.c"
#pragma clang diagnostic pop

static struct SDCardBase sdcard_base;
static IPTR configured_attr_value;
static unsigned get_attr_calls;
static uint32_t last_attr_id;
static unsigned remote_probe_calls;
static enum p4_public_spinlock_result remote_probe_result;

static IPTR mock_get_system_attr(uint32_t attribute)
{
    ++get_attr_calls;
    last_attr_id = attribute;
    CHECK(attribute == KATTR_PlatformTimer);
    return configured_attr_value;
}

static void mock_sd_time_fatal(void)
{
    krnP4PublicSpinFatal(P4_PUBLIC_SPIN_BAD_STATE);
}

static void reset_sd_fixture(void)
{
    reset_fixture();
    p4time_ops = NULL;
    configured_attr_value = 0u;
    get_attr_calls = 0u;
    last_attr_id = 0u;
    remote_probe_calls = 0u;
    remote_probe_result = P4_PUBLIC_SPIN_OK;
    memset(&sdcard_base, 0, sizeof(sdcard_base));
}

static void reject_table_value(IPTR value)
{
    reset_sd_fixture();
    configured_attr_value = value;

    CHECK(p4sd_TimePrepare(&sdcard_base) == FALSE);
    CHECK(get_attr_calls == 1u);
    CHECK(last_attr_id == KATTR_PlatformTimer);
    CHECK(p4time_ops == NULL);

    /* The SMP adapter has no direct-MMIO fallback after a rejected lookup. */
    EXPECT_FATAL(P4_PUBLIC_SPIN_BAD_STATE,
                 (void)sdcard_CurrentTime());
    CHECK(timer_writes == 0u);
    CHECK(op_reads == 0u);
    CHECK(value_reads == 0u);
    CHECK(acquire_calls == 0u);
    CHECK(blocked_acquire_calls == 0u);
}

static void test_table_admission_and_fail_closed(void)
{
    const struct KrnP4TimerOps *valid = krnP4TimerSnapshotOps();
    struct KrnP4TimerOps candidate;

    CHECK(valid != NULL);
    reject_table_value((IPTR)0);
    reject_table_value((IPTR)-1);

    candidate = *valid;
    candidate.version = KRN_P4_TIMER_OPS_VERSION + 1u;
    reject_table_value((IPTR)(uintptr_t)&candidate);

    candidate = *valid;
    candidate.size = (uint32_t)(sizeof(candidate) - 1u);
    reject_table_value((IPTR)(uintptr_t)&candidate);

    candidate = *valid;
    candidate.frequency = (uint32_t)(P4_SYSTIMER_HZ + 1u);
    reject_table_value((IPTR)(uintptr_t)&candidate);

    candidate = *valid;
    candidate.read = NULL;
    reject_table_value((IPTR)(uintptr_t)&candidate);

    reset_sd_fixture();
    configured_attr_value = (IPTR)(uintptr_t)valid;
    CHECK(p4sd_TimePrepare(&sdcard_base) == TRUE);
    CHECK(get_attr_calls == 1u);
    CHECK(last_attr_id == KATTR_PlatformTimer);
    CHECK(p4time_ops == valid);
}

static void remote_acquire_probe(uint32_t offset)
{
    uint32_t saved_hart;
    struct ExecBase *saved_sysbase;
    uint32_t saved_status;

    if (offset != P4_ST_UNIT0_VALUE_HI)
        return;

    CHECK(gate_held);
    CHECK(p4_timer_gate.lock == 1u);
    saved_hart = current_hart;
    saved_sysbase = SysBase;
    saved_status = mock_mstatus[1];
    current_hart = 1u;
    SysBase = &sysbases[1];
    mock_mstatus[1] = 0u;

    remote_probe_result = krnP4PublicSpinTryAcquire(
        &p4_timer_gate, SPINLOCK_MODE_WRITE);
    ++remote_probe_calls;
    CHECK(remote_probe_result == P4_PUBLIC_SPIN_BUSY);
    CHECK(gate_held);
    CHECK(p4_timer_gate.lock == 1u);

    mock_mstatus[1] = saved_status;
    current_hart = saved_hart;
    SysBase = saved_sysbase;
}

static void test_sdcard_callback_uses_shared_snapshot_gate(void)
{
    const struct KrnP4TimerOps *ops = krnP4TimerSnapshotOps();
    const uint64_t count = UINT64_C(0x12345678deadbeef);
    const ULONG expected_us = (ULONG)(count / (P4_SYSTIMER_HZ / 1000000UL));
    ULONG observed_us;
    size_t hi_i, lo_i, final_fence_i, unlock_i;

    reset_sd_fixture();
    current_hart = 0u;
    current_mstatus = 0u;
    CHECK(krnP4TimerSnapshotPrepare() == 1);
    clear_read_trace();
    current_hart = 0u;
    SysBase = &sysbases[0];
    current_mstatus = MSTATUS_MIE;
    timer_hi = (uint32_t)(count >> 32);
    timer_lo = (uint32_t)count;
    valid_after_polls = 2u;
    require_gate_for_mmio = 1;

    configured_attr_value = (IPTR)(uintptr_t)ops;
    CHECK(p4sd_TimePrepare(&sdcard_base) == TRUE);
    CHECK(p4time_ops == ops);
    CHECK(get_attr_calls == 1u);
    CHECK(last_attr_id == KATTR_PlatformTimer);

    /* Attempt another hart's lock acquisition between HI and LO reads. */
    timer_io_probe = remote_acquire_probe;
    observed_us = sdcard_CurrentTime();

    CHECK(observed_us == expected_us);
    CHECK(remote_probe_calls == 1u);
    CHECK(remote_probe_result == P4_PUBLIC_SPIN_BUSY);
    CHECK(blocked_acquire_calls == 1u);
    CHECK(acquire_calls == 2u); /* Outer reader plus the refused contender. */
    CHECK(value_reads == 2u);
    CHECK(op_reads == 2u);
    CHECK(!gate_held);
    CHECK(p4_timer_gate.lock == 0u);
    CHECK(current_mstatus == MSTATUS_MIE);
    CHECK(disable_calls == 0u);
    CHECK(enable_calls == 0u);
    CHECK(mmio_before_gate == 0);

    hi_i = find_event(EVENT_READ, P4_ST_UNIT0_VALUE_HI, 0u);
    lo_i = find_event(EVENT_READ, P4_ST_UNIT0_VALUE_LO, 0u);
    final_fence_i = find_event(EVENT_FENCE, 0u, 1u);
    unlock_i = find_event(EVENT_UNLOCK, 0u, 0u);
    CHECK(hi_i < lo_i);
    CHECK(lo_i < final_fence_i);
    CHECK(final_fence_i < unlock_i);
    CHECK(events[hi_i].gate_held);
    CHECK(events[lo_i].gate_held);
    CHECK(events[final_fence_i].gate_held);

    /* A contender succeeds only after the snapshot unlock has completed. */
    current_hart = 1u;
    SysBase = &sysbases[1];
    current_mstatus = 0u;
    CHECK(krnP4PublicSpinTryAcquire(&p4_timer_gate,
                                     SPINLOCK_MODE_WRITE) ==
          P4_PUBLIC_SPIN_OK);
    CHECK(gate_held);
    CHECK(krnP4PublicSpinUnlock(&p4_timer_gate) == P4_PUBLIC_SPIN_OK);
    CHECK(!gate_held);
}

int main(void)
{
    test_public_timer_ops_table();
    test_table_admission_and_fail_closed();
    test_sdcard_callback_uses_shared_snapshot_gate();
    printf("p4-shared-time-test: PASS (%u checks)\n", checks);
    return 0;
}
