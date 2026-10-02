/* Actual self-removal body; pin/retirement and trap transfer are boundaries. */
#define P4_SELF_REMOVE_TEST 1
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

struct Task { unsigned identity; };
struct ExecBase { unsigned identity; };
static struct Task identity;
static struct Task other;
static struct ExecBase base;
static struct Task *current;
static unsigned checks, failures, event_count, sequence[24];
static int depth, locked, pinned, task_deref_allowed, td;
static int pin_ok, retire_ok, current_retired_ok;
static int context_ok, transfer_escapes, dispatch_returns;
static jmp_buf stop;

enum event
{
    EV_DISABLE = 1,
    EV_CONTEXT,
    EV_PIN,
    EV_LOCK,
    EV_RETIRE,
    EV_UNLOCK,
    EV_UNPIN,
    EV_RETIRED_PROOF,
    EV_TRANSFER,
    EV_DISPATCH,
    EV_FATAL
};

#define GET_THIS_TASK current
#define TDNESTCOUNT_SET(value) (td = (value))
#define CHECK(value) do { \
    ++checks; \
    if (!(value)) { \
        ++failures; \
        fprintf(stderr, "FAIL %s:%d: %s\n", \
                __FILE__, __LINE__, #value); \
    } \
} while (0)

static void event(unsigned value)
{
    CHECK(event_count < sizeof(sequence) / sizeof(sequence[0]));
    if (event_count < sizeof(sequence) / sizeof(sequence[0]))
        sequence[event_count++] = value;
}

static void Disable(void)
{
    ++depth;
    event(EV_DISABLE);
}

static int krnP4SelfRemoveContextAllowed(struct ExecBase *sys_base)
{
    CHECK(sys_base == &base);
    event(EV_CONTEXT);
    return context_ok;
}

static int krnP4SignalTaskPin(struct Task *task, struct ExecBase *sys_base)
{
    CHECK(task == current && sys_base == &base && depth > 0);
    event(EV_PIN);
    if (!pin_ok)
        task_deref_allowed = 0;
    else
        pinned = 1;
    return pin_ok;
}

static void p4_signal_adapter_lock_disabled(struct Task *task)
{
    CHECK(task == current && pinned && task_deref_allowed);
    CHECK(!locked && depth > 0);
    locked = 1;
    event(EV_LOCK);
}

static int krnP4TaskRetireLocked(struct Task *task, struct ExecBase *sys_base)
{
    CHECK(task == current && sys_base == &base);
    CHECK(locked && pinned && task_deref_allowed);
    event(EV_RETIRE);
    if (!retire_ok)
        task_deref_allowed = 0; /* Remote retirement won before this lock. */
    return retire_ok;
}

static void p4_signal_adapter_unlock_disabled(struct Task *task)
{
    CHECK(task == current && locked && pinned);
    locked = 0;
    event(EV_UNLOCK);
}

static void krnP4SignalTaskUnpin(struct Task *task, struct ExecBase *sys_base)
{
    CHECK(task == current && sys_base == &base && pinned && !locked);
    event(EV_UNPIN);
    pinned = 0;
    task_deref_allowed = 0;
}

static int krnP4SelfRemoveCurrentRetired(struct ExecBase *sys_base)
{
    /* Production proof uses only the protected RUN identity and registry. */
    CHECK(sys_base == &base && !pinned && !locked);
    CHECK(!task_deref_allowed);
    event(EV_RETIRED_PROOF);
    return current_retired_ok;
}

static void KrnDispatch(void)
{
    CHECK(!locked && !pinned && !task_deref_allowed && td == -1);
    event(EV_DISPATCH);
    if (!dispatch_returns)
        longjmp(stop, 1);
}

static void krnP4SelfRemoveTransfer(struct ExecBase *sys_base)
{
    CHECK(sys_base == &base);
    CHECK(!locked && !pinned && !task_deref_allowed && td == -1);
    event(EV_TRANSFER);
    if (transfer_escapes)
        longjmp(stop, 1);
    KrnDispatch();
}

static void p4_signal_adapter_fail_closed(void) __attribute__((noreturn));
static void p4_signal_adapter_fail_closed(void)
{
    CHECK(!locked);
    event(EV_FATAL);
    longjmp(stop, 2);
}

#include "../p4_self_remove.c"

static int run_case(struct Task *task, int expected)
{
    int result;
    event_count = 0;
    depth = 0;
    locked = 0;
    pinned = 0;
    task_deref_allowed = 1;
    td = 3;
    current = &identity;
    result = setjmp(stop);
    if (result == 0)
        Exec_P4RemoveSelf(task, &base);
    CHECK(result == expected);
    CHECK(depth == 1 && !locked && !pinned);
    return result;
}

static void check_sequence(const unsigned *expected, size_t length)
{
    size_t index;
    CHECK(event_count == length);
    for (index = 0; index < length && index < event_count; ++index)
        CHECK(sequence[index] == expected[index]);
}

static void test_ordinary_no_save_dispatch(void)
{
    const unsigned expected[] = {
        EV_DISABLE, EV_CONTEXT, EV_PIN, EV_LOCK, EV_RETIRE, EV_UNLOCK,
        EV_UNPIN, EV_TRANSFER, EV_DISPATCH
    };
    context_ok = pin_ok = retire_ok = 1;
    current_retired_ok = transfer_escapes = dispatch_returns = 0;
    CHECK(run_case(&identity, 1) == 1);
    check_sequence(expected, sizeof(expected) / sizeof(expected[0]));
    CHECK(!task_deref_allowed);
}

static void test_pin_refusal_with_existing_service_owner(void)
{
    const unsigned expected[] = {
        EV_DISABLE, EV_CONTEXT, EV_PIN, EV_RETIRED_PROOF, EV_TRANSFER
    };
    context_ok = 1;
    pin_ok = 0;
    retire_ok = 0;
    current_retired_ok = 1;
    transfer_escapes = 1;
    dispatch_returns = 0;
    CHECK(run_case(&identity, 1) == 1);
    check_sequence(expected, sizeof(expected) / sizeof(expected[0]));
    CHECK(!task_deref_allowed);
}

static void test_remote_retirement_wins_after_pin(void)
{
    const unsigned expected[] = {
        EV_DISABLE, EV_CONTEXT, EV_PIN, EV_LOCK, EV_RETIRE, EV_UNLOCK,
        EV_UNPIN, EV_RETIRED_PROOF, EV_TRANSFER
    };
    context_ok = 1;
    pin_ok = 1;
    retire_ok = 0;
    current_retired_ok = 1;
    transfer_escapes = 1;
    dispatch_returns = 0;
    CHECK(run_case(&identity, 1) == 1);
    check_sequence(expected, sizeof(expected) / sizeof(expected[0]));
    CHECK(!task_deref_allowed);
}

static void test_refused_unowned_removal_is_fatal(void)
{
    const unsigned expected[] = {
        EV_DISABLE, EV_CONTEXT, EV_PIN, EV_RETIRED_PROOF, EV_FATAL
    };
    context_ok = 1;
    pin_ok = 0;
    retire_ok = 0;
    current_retired_ok = 0;
    transfer_escapes = 0;
    dispatch_returns = 0;
    CHECK(run_case(&identity, 2) == 2);
    check_sequence(expected, sizeof(expected) / sizeof(expected[0]));
}

static void test_context_and_identity_refusal_precede_pin(void)
{
    const unsigned context_events[] = {EV_DISABLE, EV_CONTEXT, EV_FATAL};
    const unsigned null_events[] = {EV_DISABLE, EV_FATAL};
    context_ok = 0;
    pin_ok = 1;
    retire_ok = 1;
    current_retired_ok = 0;
    transfer_escapes = 0;
    dispatch_returns = 0;
    CHECK(run_case(&identity, 2) == 2);
    check_sequence(context_events,
                   sizeof(context_events) / sizeof(context_events[0]));

    context_ok = 1;
    CHECK(run_case(NULL, 2) == 2);
    check_sequence(null_events,
                   sizeof(null_events) / sizeof(null_events[0]));
    CHECK(run_case(&other, 2) == 2);
    check_sequence(null_events,
                   sizeof(null_events) / sizeof(null_events[0]));
}

static void test_unexpected_dispatch_return_fails_closed(void)
{
    const unsigned expected[] = {
        EV_DISABLE, EV_CONTEXT, EV_PIN, EV_LOCK, EV_RETIRE, EV_UNLOCK,
        EV_UNPIN, EV_TRANSFER, EV_DISPATCH, EV_FATAL
    };
    context_ok = 1;
    pin_ok = retire_ok = 1;
    current_retired_ok = transfer_escapes = 0;
    dispatch_returns = 1;
    CHECK(run_case(&identity, 2) == 2);
    check_sequence(expected, sizeof(expected) / sizeof(expected[0]));
}

int main(void)
{
    test_ordinary_no_save_dispatch();
    test_pin_refusal_with_existing_service_owner();
    test_remote_retirement_wins_after_pin();
    test_refused_unowned_removal_is_fatal();
    test_context_and_identity_refusal_precede_pin();
    test_unexpected_dispatch_return_fails_closed();
    if (failures != 0u)
    {
        fprintf(stderr, "FAIL p4_self_remove checks=%u failures=%u\n",
                checks, failures);
        return EXIT_FAILURE;
    }
    printf("PASS p4_self_remove checks=%u\n", checks);
    return EXIT_SUCCESS;
}
