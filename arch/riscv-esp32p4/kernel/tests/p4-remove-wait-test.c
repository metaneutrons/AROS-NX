/* Actual-body test for the private P4 external-removal wait loop. */
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

struct Task { uintptr_t marker; };
struct p4_test_cpu_local { uint32_t trap_depth; };
struct p4_test_runtime { uint32_t hart_id; };
struct ExecBase
{
    struct p4_test_cpu_local cpu;
    struct p4_test_runtime runtime;
};

#include "../../exec/p4_remove_wait.h"

#define P4_REMOVE_WAIT_TEST 1
#define SIGF_SINGLE UINT32_C(0x10)
#define GET_THIS_TASK mock_current_task
#define P4_EXEC_CPU_LOCAL(base) (&(base)->cpu)
#define P4_EXEC_RUNTIME(base) (&(base)->runtime)

static struct Task *mock_current_task;
void Disable(void);
void Enable(void);
uint32_t SetSignal(uint32_t, uint32_t);
uint32_t Wait(uint32_t);
int krnP4TaskRemovalPoll(struct Task *, uint32_t,
    struct p4_remove_handle, struct ExecBase *);
int krnP4TaskRemovalDrop(struct Task *, uint32_t,
    struct p4_remove_handle, struct ExecBase *);
void p4_signal_adapter_fail_closed(void) __attribute__((noreturn));
void Exec_P4RemoveSelf(struct Task *, struct ExecBase *) __attribute__((noreturn));

#include "../../exec/p4_remove_wait.c"

#define CHECK(condition) do { \
    ++checks; \
    if (!(condition)) { \
        ++failures; \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                #condition); \
    } \
} while (0)

#define EVENT_CAPACITY 64u
#define WAIT_CAPACITY 8u

struct wait_step
{
    uint32_t hart_after;
    uint8_t migrate;
    uint8_t publish_ack;
};

static unsigned int checks;
static unsigned int failures;
static unsigned int irq_depth;
static unsigned int disable_calls;
static unsigned int enable_calls;
static unsigned int clear_calls;
static unsigned int poll_calls;
static unsigned int drop_calls;
static unsigned int wait_calls;
static unsigned int self_remove_calls;
static unsigned int fatal_calls;
static unsigned int wait_step_count;
static unsigned int wait_step_index;
static unsigned int inject_ack_on_clear;
static unsigned int inject_ack_clear_index;
static unsigned int inject_ack_on_enable;
static unsigned int inject_ack_enable_index;
static unsigned int require_signal_at_wait;
static uint32_t signal_bits;
static int ack_ready;
static int poll_override_enabled;
static int poll_override;
static int drop_result;
static char events[EVENT_CAPACITY];
static unsigned int event_count;
static uint32_t poll_harts[EVENT_CAPACITY];
static struct Task *poll_tasks[EVENT_CAPACITY];
static int poll_results[EVENT_CAPACITY];
static struct wait_step wait_steps[WAIT_CAPACITY];
static struct ExecBase mock_sysbase;
static struct Task *expected_requester;
static struct p4_remove_handle expected_handle;
static jmp_buf nonreturn_env;

static void record_event(char event)
{
    if (event_count < EVENT_CAPACITY)
        events[event_count++] = event;
    else
        ++failures;
}

static void model_reset(struct Task *current, uint32_t hart,
                        struct p4_remove_handle handle)
{
    irq_depth = 0u;
    disable_calls = 0u;
    enable_calls = 0u;
    clear_calls = 0u;
    poll_calls = 0u;
    drop_calls = 0u;
    wait_calls = 0u;
    self_remove_calls = 0u;
    fatal_calls = 0u;
    wait_step_count = 0u;
    wait_step_index = 0u;
    inject_ack_on_clear = 0u;
    inject_ack_clear_index = 0u;
    inject_ack_on_enable = 0u;
    inject_ack_enable_index = 0u;
    require_signal_at_wait = 0u;
    signal_bits = 0u;
    ack_ready = 0;
    poll_override_enabled = 0;
    poll_override = 0;
    drop_result = 1;
    event_count = 0u;
    memset(events, 0, sizeof(events));
    memset(poll_harts, 0, sizeof(poll_harts));
    memset(poll_tasks, 0, sizeof(poll_tasks));
    memset(poll_results, 0, sizeof(poll_results));
    memset(wait_steps, 0, sizeof(wait_steps));
    mock_current_task = current;
    memset(&mock_sysbase, 0, sizeof(mock_sysbase));
    mock_sysbase.runtime.hart_id = hart;
    expected_requester = current;
    expected_handle = handle;
}

static void append_wait_step(uint32_t next_hart, int migrate, int publish_ack)
{
    struct wait_step *step;
    if (wait_step_count >= WAIT_CAPACITY)
    {
        ++failures;
        return;
    }
    step = &wait_steps[wait_step_count++];
    step->hart_after = next_hart;
    step->migrate = (uint8_t)(migrate != 0);
    step->publish_ack = (uint8_t)(publish_ack != 0);
}

static void expect_events(const char *expected)
{
    size_t length = strlen(expected);
    CHECK(event_count == length);
    if (event_count == length)
        CHECK(memcmp(events, expected, length) == 0);
}

void Disable(void)
{
    record_event('D');
    ++disable_calls;
    ++irq_depth;
    CHECK(irq_depth == 1u);
}

void Enable(void)
{
    record_event('E');
    ++enable_calls;
    CHECK(irq_depth == 1u);
    if (irq_depth != 0u)
        --irq_depth;
    if (inject_ack_on_enable != 0u &&
        enable_calls == inject_ack_enable_index)
    {
        ack_ready = 1;
        signal_bits |= SIGF_SINGLE;
    }
}

uint32_t SetSignal(uint32_t new_signals, uint32_t signal_mask)
{
    uint32_t previous = signal_bits;
    record_event('C');
    ++clear_calls;
    CHECK(irq_depth == 1u);
    CHECK(new_signals == 0u && signal_mask == SIGF_SINGLE);
    signal_bits = (signal_bits & ~signal_mask) | (new_signals & signal_mask);
    if (inject_ack_on_clear != 0u && clear_calls == inject_ack_clear_index)
        ack_ready = 1;
    return previous;
}

uint32_t Wait(uint32_t signal_mask)
{
    struct wait_step *step;
    record_event('W');
    ++wait_calls;
    CHECK(irq_depth == 0u);
    CHECK(signal_mask == SIGF_SINGLE);
    if (require_signal_at_wait != 0u)
        CHECK((signal_bits & SIGF_SINGLE) != 0u);
    if (wait_step_index >= wait_step_count)
    {
        ++failures;
        p4_signal_adapter_fail_closed();
    }
    step = &wait_steps[wait_step_index++];
    if (step->migrate != 0u)
        mock_sysbase.runtime.hart_id = step->hart_after;
    if (step->publish_ack != 0u)
    {
        ack_ready = 1;
        signal_bits |= SIGF_SINGLE;
    }
    else
    {
        /* A stale or unrelated SIGF_SINGLE wake is only a hint. */
        signal_bits |= SIGF_SINGLE;
    }
    return signal_bits & signal_mask;
}

int krnP4TaskRemovalPoll(struct Task *requester, uint32_t hart,
                         struct p4_remove_handle handle,
                         struct ExecBase *SysBase)
{
    int result;
    record_event('P');
    CHECK(irq_depth == 1u);
    CHECK(requester == expected_requester);
    CHECK(SysBase == &mock_sysbase);
    CHECK(handle.index == expected_handle.index &&
          handle.generation == expected_handle.generation);
    if (poll_calls < EVENT_CAPACITY)
    {
        poll_harts[poll_calls] = hart;
        poll_tasks[poll_calls] = requester;
    }
    if (poll_override_enabled != 0)
        result = poll_override;
    else
        result = ack_ready != 0 ? 1 : 0;
    if (poll_calls < EVENT_CAPACITY)
        poll_results[poll_calls] = result;
    ++poll_calls;
    return result;
}

int krnP4TaskRemovalDrop(struct Task *requester, uint32_t hart,
                         struct p4_remove_handle handle,
                         struct ExecBase *SysBase)
{
    record_event('R');
    ++drop_calls;
    CHECK(irq_depth == 1u);
    CHECK(requester == expected_requester);
    CHECK(SysBase == &mock_sysbase);
    CHECK(hart == mock_sysbase.runtime.hart_id);
    CHECK(handle.index == expected_handle.index &&
          handle.generation == expected_handle.generation);
    return drop_result;
}

void Exec_P4RemoveSelf(struct Task *requester, struct ExecBase *SysBase)
{
    record_event('X');
    ++self_remove_calls;
    CHECK(irq_depth == 1u);
    CHECK(requester == expected_requester);
    CHECK(SysBase == &mock_sysbase);
    longjmp(nonreturn_env, 2);
}

void p4_signal_adapter_fail_closed(void)
{
    record_event('F');
    ++fatal_calls;
    CHECK(irq_depth == 1u);
    longjmp(nonreturn_env, 3);
}

static int invoke_wait(struct Task *requester, struct p4_remove_handle handle)
{
    int transfer = setjmp(nonreturn_env);
    if (transfer == 0)
    {
        Exec_P4AwaitRemoval(requester, handle, &mock_sysbase);
        return 0;
    }
    return transfer;
}

static void test_ack_visible_at_poll_after_clear(void)
{
    struct Task requester = { 1u };
    struct p4_remove_handle handle = { 2u, 9u };
    int transfer;

    model_reset(&requester, 0u, handle);
    inject_ack_on_clear = 1u;
    inject_ack_clear_index = 1u;
    transfer = invoke_wait(&requester, handle);
    CHECK(transfer == 0);
    expect_events("DCPRE");
    CHECK(clear_calls == 1u && poll_calls == 1u && drop_calls == 1u);
    CHECK(wait_calls == 0u && self_remove_calls == 0u && fatal_calls == 0u);
    CHECK(irq_depth == 0u && disable_calls == 1u && enable_calls == 1u);
    CHECK(poll_harts[0] == 0u && poll_tasks[0] == &requester);
}

static void test_already_ready_before_clear(void)
{
    struct Task requester = { 8u };
    struct p4_remove_handle handle = { 8u, 24u };
    int transfer;

    model_reset(&requester, 0u, handle);
    ack_ready = 1;
    signal_bits = SIGF_SINGLE;
    transfer = invoke_wait(&requester, handle);
    CHECK(transfer == 0);
    expect_events("DCPRE");
    CHECK(poll_calls == 1u && drop_calls == 1u && wait_calls == 0u);
    CHECK(irq_depth == 0u && enable_calls == 1u);
}

static void test_ack_after_false_poll_is_not_cleared_before_wait(void)
{
    struct Task requester = { 9u };
    struct p4_remove_handle handle = { 9u, 25u };
    int transfer;

    model_reset(&requester, 0u, handle);
    inject_ack_on_enable = 1u;
    inject_ack_enable_index = 1u;
    require_signal_at_wait = 1u;
    append_wait_step(0u, 0, 0);
    transfer = invoke_wait(&requester, handle);
    CHECK(transfer == 0);
    expect_events("DCPEWDCPRE");
    CHECK(poll_calls == 2u && drop_calls == 1u && wait_calls == 1u);
    CHECK(clear_calls == 2u && enable_calls == 2u && irq_depth == 0u);
    CHECK(poll_results[0] == 0 && poll_results[1] == 1);
}

static void test_pending_then_ack_with_migration(void)
{
    struct Task requester = { 2u };
    struct p4_remove_handle handle = { 4u, 11u };
    int transfer;

    model_reset(&requester, 0u, handle);
    append_wait_step(1u, 1, 1);
    transfer = invoke_wait(&requester, handle);
    CHECK(transfer == 0);
    expect_events("DCPEWDCPRE");
    CHECK(clear_calls == 2u && poll_calls == 2u && wait_calls == 1u);
    CHECK(drop_calls == 1u && enable_calls == 2u && irq_depth == 0u);
    CHECK(poll_harts[0] == 0u && poll_harts[1] == 1u);
    CHECK(poll_tasks[0] == &requester && poll_tasks[1] == &requester);
}

static void test_stale_wakes_repoll_until_ack(void)
{
    struct Task requester = { 3u };
    struct p4_remove_handle handle = { 1u, 12u };
    int transfer;

    model_reset(&requester, 1u, handle);
    append_wait_step(1u, 0, 0);
    append_wait_step(1u, 0, 1);
    transfer = invoke_wait(&requester, handle);
    CHECK(transfer == 0);
    expect_events("DCPEWDCPEWDCPRE");
    CHECK(clear_calls == 3u && poll_calls == 3u && wait_calls == 2u);
    CHECK(poll_results[0] == 0 && poll_results[1] == 0 &&
          poll_results[2] == 1);
    CHECK(drop_calls == 1u && irq_depth == 0u && enable_calls == 3u);
}

static void test_retired_requester_transfers_without_drop(void)
{
    struct Task requester = { 4u };
    struct p4_remove_handle handle = { 6u, 15u };
    int transfer;

    model_reset(&requester, 0u, handle);
    poll_override_enabled = 1;
    poll_override = 2;
    transfer = invoke_wait(&requester, handle);
    CHECK(transfer == 2);
    expect_events("DCPX");
    CHECK(drop_calls == 0u && wait_calls == 0u && enable_calls == 0u);
    CHECK(self_remove_calls == 1u && fatal_calls == 0u && irq_depth == 1u);
}

static void test_retirement_wins_drop_race(void)
{
    struct Task requester = { 5u };
    struct p4_remove_handle handle = { 0u, 18u };
    int transfer;

    model_reset(&requester, 1u, handle);
    poll_override_enabled = 1;
    poll_override = 1;
    drop_result = 0;
    transfer = invoke_wait(&requester, handle);
    CHECK(transfer == 2);
    expect_events("DCPRX");
    CHECK(drop_calls == 1u && self_remove_calls == 1u);
    CHECK(wait_calls == 0u && enable_calls == 0u && irq_depth == 1u);
}

static void test_fail_closed_context_and_status(void)
{
    struct Task requester = { 6u };
    struct Task other = { 7u };
    struct p4_remove_handle handle = { 3u, 21u };
    int transfer;

    model_reset(&requester, 0u, handle);
    mock_current_task = &other;
    transfer = invoke_wait(&requester, handle);
    CHECK(transfer == 3);
    expect_events("DF");
    CHECK(poll_calls == 0u && clear_calls == 0u && enable_calls == 0u);
    CHECK(fatal_calls == 1u && irq_depth == 1u);

    model_reset(&requester, 0u, handle);
    mock_sysbase.cpu.trap_depth = 1u;
    transfer = invoke_wait(&requester, handle);
    CHECK(transfer == 3);
    expect_events("DF");
    CHECK(poll_calls == 0u && clear_calls == 0u && enable_calls == 0u);
    CHECK(fatal_calls == 1u && irq_depth == 1u);

    model_reset(NULL, 0u, handle);
    transfer = invoke_wait(NULL, handle);
    CHECK(transfer == 3);
    expect_events("DF");
    CHECK(poll_calls == 0u && clear_calls == 0u && irq_depth == 1u);

    model_reset(&requester, 0u, handle);
    poll_override_enabled = 1;
    poll_override = -1;
    transfer = invoke_wait(&requester, handle);
    CHECK(transfer == 3);
    expect_events("DCPF");
    CHECK(drop_calls == 0u && wait_calls == 0u && enable_calls == 0u);
    CHECK(irq_depth == 1u);

    model_reset(&requester, 0u, handle);
    poll_override_enabled = 1;
    poll_override = 1;
    drop_result = -1;
    transfer = invoke_wait(&requester, handle);
    CHECK(transfer == 3);
    expect_events("DCPRF");
    CHECK(self_remove_calls == 0u && wait_calls == 0u && enable_calls == 0u);
    CHECK(irq_depth == 1u);
}

int main(void)
{
    unsigned int packet_checks;

    checks = 0u;
    failures = 0u;
    test_ack_visible_at_poll_after_clear();
    test_already_ready_before_clear();
    test_ack_after_false_poll_is_not_cleared_before_wait();
    test_pending_then_ack_with_migration();
    test_stale_wakes_repoll_until_ack();
    test_retired_requester_transfers_without_drop();
    test_retirement_wins_drop_race();
    test_fail_closed_context_and_status();
    packet_checks = checks;
    if (failures != 0u)
    {
        fprintf(stderr, "P4 remove wait: %u/%u checks failed\n",
                failures, checks);
        return 1;
    }
    printf("P4 remove wait: %u checks passed\n", packet_checks);
    return 0;
}
