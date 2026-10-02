/* Host fixture for the actual rom/kernel/kernel_intr.c return path. */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct test_regs
{
    uint32_t token;
} regs_t;

static int mock_tdnest_count;
static int mock_switch_pending;

#define INTB_SOFTINT ((unsigned char)5u)
#define SC_CAUSE 0x000
#define SC_DISPATCH 0x001
#define SC_SWITCH 0x002
#define SC_SCHEDULE 0x003
#define EXEC_PLATFORM_SOFTINT_PENDING() softint_pending_mock()
#define TDNESTCOUNT_GET mock_tdnest_count
#define FLAG_SCHEDSWITCH_ISSET mock_switch_pending

void core_Cause(unsigned char interrupt_bit, unsigned int mask);
void KrnCli(void);
int core_Schedule(void);
void cpu_Switch(regs_t *regs);
void cpu_Dispatch(regs_t *regs);
static int softint_pending_mock(void);

#define P4_CORE_EXIT_TEST 1
#include "../../../../rom/kernel/kernel_intr.c"

enum event
{
    EV_PENDING_PROBE = 1,
    EV_CAUSE_BEGIN,
    EV_DELIVER_FIRST,
    EV_EMPTY_RELEASE,
    EV_ENQUEUE_SECOND,
    EV_NESTED_IPI,
    EV_DELIVER_SECOND,
    EV_DELIVER_REPEAT,
    EV_CAUSE_RETURN,
    EV_KRN_CLI,
    EV_SCHEDULE,
    EV_SWITCH,
    EV_DISPATCH
};

enum cause_mode
{
    CAUSE_MODE_DRAIN_ONE = 0,
    CAUSE_MODE_EMPTY_RELEASE_NESTED_DOORBELL,
    CAUSE_MODE_REPEATED_WORK
};

#define EVENT_CAPACITY 128u

static unsigned int checks;
static unsigned int failures;
static enum event events[EVENT_CAPACITY];
static size_t event_count;
static unsigned int mock_mie;
static unsigned int mock_hart;
static unsigned int pending_probes;
static unsigned int hart1_probes;
static int pending_results[8];
static unsigned int cause_calls;
static unsigned int deliveries;
static unsigned int work_queued;
static int mock_schedule_result;
static regs_t *expected_regs;
static enum cause_mode mock_cause_mode;

#define CHECK(condition) do { \
    ++checks; \
    if (!(condition)) { \
        ++failures; \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
    } \
} while (0)

static void record(enum event value)
{
    if (event_count < EVENT_CAPACITY)
        events[event_count++] = value;
    else
    {
        ++failures;
        fprintf(stderr, "FAIL event log overflow\n");
    }
}

static size_t event_index(enum event wanted, unsigned int occurrence)
{
    size_t i;
    unsigned int seen = 0;
    for (i = 0; i < event_count; ++i)
    {
        if (events[i] == wanted)
        {
            if (seen == occurrence)
                return i;
            ++seen;
        }
    }
    return SIZE_MAX;
}

static int event_before(enum event first, unsigned int first_occurrence,
                        enum event second, unsigned int second_occurrence)
{
    size_t first_index = event_index(first, first_occurrence);
    size_t second_index = event_index(second, second_occurrence);
    return first_index != SIZE_MAX && second_index != SIZE_MAX &&
        first_index < second_index;
}

static unsigned int event_count_of(enum event wanted)
{
    size_t i;
    unsigned int count = 0;
    for (i = 0; i < event_count; ++i)
        if (events[i] == wanted)
            ++count;
    return count;
}

static void reset_simulation(void)
{
    memset(events, 0, sizeof(events));
    event_count = 0;
    mock_mie = 0u;
    mock_hart = 0u;
    pending_probes = 0u;
    hart1_probes = 0u;
    memset(pending_results, 0, sizeof(pending_results));
    cause_calls = 0u;
    deliveries = 0u;
    work_queued = 0u;
    mock_tdnest_count = 0;
    mock_switch_pending = 0;
    mock_schedule_result = 0;
    expected_regs = NULL;
    mock_cause_mode = CAUSE_MODE_DRAIN_ONE;
}

static int softint_pending_mock(void)
{
    unsigned int probe = pending_probes;
    int result;
    record(EV_PENDING_PROBE);
    ++pending_probes;
    if (mock_hart != 0u)
    {
        ++hart1_probes;
        if (probe < 8u)
            pending_results[probe] = 0;
        return 0;
    }
    CHECK(mock_mie == 0u);
    result = work_queued != 0u;
    if (probe < 8u)
        pending_results[probe] = result;
    else
        CHECK(0);
    return result;
}

void core_Cause(unsigned char interrupt_bit, unsigned int mask)
{
    unsigned int call = cause_calls++;
    CHECK(interrupt_bit == INTB_SOFTINT);
    CHECK(mask == (1u << INTB_SOFTINT));
    CHECK(mock_mie == 0u);
    record(EV_CAUSE_BEGIN);

    /* Cause/SoftIntDispatch may enable MIE while a callback runs. */
    mock_mie = 1u;
    if (mock_cause_mode == CAUSE_MODE_EMPTY_RELEASE_NESTED_DOORBELL)
    {
        if (call == 0u)
        {
            CHECK(work_queued == 1u);
            --work_queued;
            ++deliveries;
            record(EV_DELIVER_FIRST);

            /* The first dispatcher has released its empty/active reservation. */
            record(EV_EMPTY_RELEASE);
            CHECK(work_queued == 0u);
            ++work_queued;
            record(EV_ENQUEUE_SECOND);

            /* The nested IPI consumes only its doorbell. Softint dispatch is
             * not run here; the queued item must survive for the outer
             * core_ExitInterrupt repoll and its second Cause() call. */
            record(EV_NESTED_IPI);
            CHECK(mock_mie == 1u);
            CHECK(work_queued == 1u);
        }
        else if (call == 1u)
        {
            CHECK(work_queued == 1u);
            --work_queued;
            ++deliveries;
            record(EV_DELIVER_SECOND);
        }
        else
        {
            CHECK(0);
        }
    }
    else if (mock_cause_mode == CAUSE_MODE_REPEATED_WORK)
    {
        CHECK(work_queued != 0u);
        --work_queued;
        ++deliveries;
        record(call == 0u ? EV_DELIVER_FIRST : EV_DELIVER_REPEAT);
    }
    else
    {
        CHECK(work_queued != 0u);
        --work_queued;
        ++deliveries;
        record(EV_DELIVER_FIRST);
    }

    /* Explicitly return with MIE enabled to test the corrected repoll edge. */
    mock_mie = 1u;
    record(EV_CAUSE_RETURN);
}

void KrnCli(void)
{
    CHECK(mock_mie == 1u);
    record(EV_KRN_CLI);
    mock_mie = 0u;
}

int core_Schedule(void)
{
    record(EV_SCHEDULE);
    CHECK(mock_mie == 0u);
    return mock_schedule_result;
}

void cpu_Switch(regs_t *regs)
{
    record(EV_SWITCH);
    CHECK(regs == expected_regs);
}

void cpu_Dispatch(regs_t *regs)
{
    record(EV_DISPATCH);
    CHECK(regs == expected_regs);
}

static void test_empty_release_nested_ipi_repoll(void)
{
    regs_t regs = {UINT32_C(0x43554558)};
    reset_simulation();
    expected_regs = &regs;
    work_queued = 1u;
    mock_tdnest_count = -1;
    mock_switch_pending = 1;
    mock_schedule_result = 1;
    mock_cause_mode = CAUSE_MODE_EMPTY_RELEASE_NESTED_DOORBELL;

    core_ExitInterrupt(&regs);

    CHECK(cause_calls == 2u);
    CHECK(deliveries == 2u);
    CHECK(work_queued == 0u);
    CHECK(pending_probes == 3u);
    CHECK(pending_results[0] == 1);
    CHECK(pending_results[1] == 1);
    CHECK(pending_results[2] == 0);
    CHECK(event_count_of(EV_KRN_CLI) == 2u);
    CHECK(event_before(EV_CAUSE_RETURN, 0u, EV_KRN_CLI, 0u));
    CHECK(event_before(EV_KRN_CLI, 0u, EV_PENDING_PROBE, 1u));
    CHECK(event_before(EV_EMPTY_RELEASE, 0u, EV_ENQUEUE_SECOND, 0u));
    CHECK(event_before(EV_ENQUEUE_SECOND, 0u, EV_NESTED_IPI, 0u));
    CHECK(event_before(EV_NESTED_IPI, 0u, EV_CAUSE_BEGIN, 1u));
    CHECK(event_before(EV_PENDING_PROBE, 1u, EV_CAUSE_BEGIN, 1u));
    CHECK(event_before(EV_CAUSE_BEGIN, 1u, EV_DELIVER_SECOND, 0u));
    CHECK(event_before(EV_CAUSE_RETURN, 1u, EV_KRN_CLI, 1u));
    CHECK(event_before(EV_KRN_CLI, 1u, EV_PENDING_PROBE, 2u));
    CHECK(event_before(EV_DELIVER_SECOND, 0u, EV_SCHEDULE, 0u));
    CHECK(event_before(EV_SCHEDULE, 0u, EV_SWITCH, 0u));
    CHECK(event_before(EV_SWITCH, 0u, EV_DISPATCH, 0u));
    CHECK(mock_mie == 0u);
}

static void test_repeated_work_until_quiescent(void)
{
    regs_t regs = {UINT32_C(0x52505431)};
    reset_simulation();
    expected_regs = &regs;
    work_queued = 2u;
    mock_cause_mode = CAUSE_MODE_REPEATED_WORK;
    mock_tdnest_count = -1;
    mock_switch_pending = 1;
    mock_schedule_result = 1;

    core_ExitInterrupt(&regs);

    CHECK(cause_calls == 2u);
    CHECK(deliveries == 2u);
    CHECK(work_queued == 0u);
    CHECK(pending_probes == 3u);
    CHECK(event_count_of(EV_KRN_CLI) == 2u);
    CHECK(event_before(EV_CAUSE_RETURN, 0u, EV_KRN_CLI, 0u));
    CHECK(event_before(EV_KRN_CLI, 0u, EV_PENDING_PROBE, 1u));
    CHECK(event_before(EV_CAUSE_RETURN, 1u, EV_KRN_CLI, 1u));
    CHECK(event_before(EV_KRN_CLI, 1u, EV_PENDING_PROBE, 2u));
    CHECK(event_before(EV_DELIVER_REPEAT, 0u, EV_SCHEDULE, 0u));
    CHECK(event_before(EV_SCHEDULE, 0u, EV_SWITCH, 0u));
    CHECK(event_before(EV_SWITCH, 0u, EV_DISPATCH, 0u));
}

static void test_no_work_hart_one_and_tdnest(void)
{
    regs_t regs = {UINT32_C(0x4e4f574b)};

    reset_simulation();
    expected_regs = &regs;
    mock_tdnest_count = 0;
    mock_switch_pending = 1;
    core_ExitInterrupt(&regs);
    CHECK(pending_probes == 1u);
    CHECK(cause_calls == 0u);
    CHECK(event_count_of(EV_SCHEDULE) == 0u);

    reset_simulation();
    expected_regs = &regs;
    mock_hart = 1u;
    work_queued = 1u;
    mock_tdnest_count = -1;
    core_ExitInterrupt(&regs);
    CHECK(pending_probes == 1u);
    CHECK(hart1_probes == 1u);
    CHECK(cause_calls == 0u);
    CHECK(work_queued == 1u);
    CHECK(event_count_of(EV_SCHEDULE) == 0u);

    /* Softints still drain while task switching is forbidden; only the
     * scheduler/switch portion is gated by TDNest. */
    reset_simulation();
    expected_regs = &regs;
    work_queued = 1u;
    mock_tdnest_count = 0;
    mock_cause_mode = CAUSE_MODE_DRAIN_ONE;
    mock_switch_pending = 1;
    core_ExitInterrupt(&regs);
    CHECK(cause_calls == 1u);
    CHECK(deliveries == 1u);
    CHECK(event_count_of(EV_SCHEDULE) == 0u);
    CHECK(event_count_of(EV_SWITCH) == 0u);
}

static void test_switch_flag_and_syscall_routes(void)
{
    regs_t regs = {UINT32_C(0x53595343)};

    reset_simulation();
    expected_regs = &regs;
    mock_tdnest_count = -1;
    mock_schedule_result = 1;
    mock_switch_pending = 0;
    core_ExitInterrupt(&regs);
    CHECK(event_count_of(EV_SCHEDULE) == 0u);
    CHECK(event_count_of(EV_SWITCH) == 0u);
    CHECK(event_count_of(EV_DISPATCH) == 0u);

    reset_simulation();
    expected_regs = &regs;
    mock_schedule_result = 0;
    core_SysCall(SC_SCHEDULE, &regs);
    CHECK(event_count_of(EV_SCHEDULE) == 1u);
    CHECK(event_count_of(EV_SWITCH) == 0u);
    CHECK(event_count_of(EV_DISPATCH) == 0u);

    reset_simulation();
    expected_regs = &regs;
    core_SysCall(SC_SWITCH, &regs);
    CHECK(event_count_of(EV_SWITCH) == 1u);
    CHECK(event_count_of(EV_DISPATCH) == 1u);

    reset_simulation();
    expected_regs = &regs;
    core_SysCall(SC_DISPATCH, &regs);
    CHECK(event_count_of(EV_SWITCH) == 0u);
    CHECK(event_count_of(EV_DISPATCH) == 1u);

    reset_simulation();
    expected_regs = &regs;
    core_SysCall(SC_CAUSE, &regs);
    CHECK(pending_probes == 1u);
    CHECK(cause_calls == 0u);
}

int main(void)
{
    test_empty_release_nested_ipi_repoll();
    test_repeated_work_until_quiescent();
    test_no_work_hart_one_and_tdnest();
    test_switch_flag_and_syscall_routes();
    if (failures != 0u)
    {
        fprintf(stderr, "FAIL p4_softint_exit checks=%u failures=%u\n",
                checks, failures);
        return 1;
    }
    printf("PASS p4_softint_exit checks=%u\n", checks);
    return 0;
}
