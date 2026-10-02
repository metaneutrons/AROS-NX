/* Host fixture for the actual P4 Exception() body and portable protocol. */
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../p4_signal_protocol.h"
#include "../p4_signal_protocol.c"

typedef uint32_t ULONG;
typedef int8_t BYTE;
typedef void *APTR;

#define TS_RUN        UINT32_C(1)
#define TS_READY      UINT32_C(2)
#define TS_WAIT       UINT32_C(3)
#define TS_REMOVED    UINT32_C(4)
#define TS_INVALID    UINT32_C(5)
#define TS_TOMBSTONED UINT32_C(6)
#define TF_EXCEPT     UINT8_C(0x80)

struct Task
{
    ULONG tc_State;
    uint32_t tc_SigRecvd;
    uint32_t tc_SigWait;
    uint32_t tc_SigExcept;
    uint8_t tc_Flags;
    APTR tc_ExceptCode;
    APTR tc_ExceptData;
};

struct ExecBase
{
    uint32_t marker;
};

static struct Task *mock_current_task;
static struct Task *mock_locked_task;
static struct ExecBase test_sysbase;
static BYTE mock_idnest;
static unsigned int mock_mie;
static unsigned int mock_task_lock_depth;
static unsigned int mock_lock_calls;
static unsigned int mock_unlock_calls;
static unsigned int mock_load_calls;
static unsigned int mock_flush_calls;
static unsigned int mock_take_calls;
static unsigned int mock_merge_calls;
static unsigned int mock_callback_calls;
static unsigned int mock_fatal_calls;
static int fatal_armed;
static jmp_buf fatal_jump;
static ULONG callback_flags[8];
static ULONG callback_returns[8];
static APTR expected_exception_code;
static APTR expected_exception_data;
static uint32_t callback_injected_signals;
static int inject_on_first_callback;
static int retire_on_first_callback;
static ULONG terminal_on_entry;

enum event
{
    EV_ID_SET_ZERO = 1,
    EV_CLI,
    EV_DISABLE,
    EV_TASK_LOCK,
    EV_LOAD,
    EV_TAKE,
    EV_FLUSH,
    EV_TASK_UNLOCK,
    EV_ENABLE,
    EV_STI,
    EV_CALLBACK_ENTER,
    EV_CALLBACK_RETURN,
    EV_MERGE,
    EV_RETIRE,
    EV_ID_RESTORE,
    EV_FATAL
};

#define EVENT_CAPACITY 256u
static enum event events[EVENT_CAPACITY];
static size_t event_count;
static unsigned int checks;
static unsigned int failures;

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
    unsigned int seen = 0u;
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

static void reset_fixture(struct Task *task, BYTE original_idnest)
{
    memset(task, 0, sizeof(*task));
    memset(&test_sysbase, 0, sizeof(test_sysbase));
    mock_current_task = task;
    mock_locked_task = NULL;
    mock_idnest = original_idnest;
    mock_mie = 1u;
    mock_task_lock_depth = 0u;
    mock_lock_calls = 0u;
    mock_unlock_calls = 0u;
    mock_load_calls = 0u;
    mock_flush_calls = 0u;
    mock_take_calls = 0u;
    mock_merge_calls = 0u;
    mock_callback_calls = 0u;
    mock_fatal_calls = 0u;
    fatal_armed = 0;
    memset(callback_flags, 0, sizeof(callback_flags));
    memset(callback_returns, 0, sizeof(callback_returns));
    expected_exception_code = NULL;
    expected_exception_data = NULL;
    callback_injected_signals = 0u;
    inject_on_first_callback = 0;
    retire_on_first_callback = 0;
    terminal_on_entry = 0u;
    event_count = 0;
    memset(events, 0, sizeof(events));
}

static void p4_signal_adapter_fail_closed(void) __attribute__((noreturn));
static void p4_signal_adapter_fail_closed(void)
{
    ++mock_fatal_calls;
    record(EV_FATAL);
    if (fatal_armed)
        longjmp(fatal_jump, 1);
    abort();
}

static void p4_signal_adapter_lock_disabled(struct Task *task)
{
    CHECK(task == mock_current_task);
    CHECK(mock_mie == 0u);
    CHECK(mock_task_lock_depth == 0u);
    mock_locked_task = task;
    mock_task_lock_depth = 1u;
    ++mock_lock_calls;
    record(EV_TASK_LOCK);
    if (terminal_on_entry != 0u)
    {
        task->tc_State = terminal_on_entry;
        terminal_on_entry = 0u;
        record(EV_RETIRE);
    }
}

static void p4_signal_adapter_unlock_disabled(struct Task *task)
{
    CHECK(task == mock_locked_task);
    CHECK(mock_task_lock_depth == 1u);
    CHECK(mock_mie == 0u);
    mock_task_lock_depth = 0u;
    mock_locked_task = NULL;
    ++mock_unlock_calls;
    record(EV_TASK_UNLOCK);
}

static int p4_signal_adapter_load_locked(
    struct Task *task,
    struct ExecBase *sys_base,
    struct p4_signal_task_state *state)
{
    CHECK(task == mock_locked_task);
    CHECK(sys_base == &test_sysbase);
    CHECK(state != NULL);
    CHECK(mock_task_lock_depth == 1u);
    CHECK(mock_mie == 0u);
    ++mock_load_calls;
    record(EV_LOAD);
    state->sig_received = task->tc_SigRecvd;
    state->sig_wait = task->tc_SigWait;
    state->sig_except = task->tc_SigExcept;
    state->flags = task->tc_Flags;
    state->exception_flag_mask = TF_EXCEPT;
    state->owner = P4_SIGNAL_OWNER_RUNNING;
    state->owner_hart = 0u;
    return task->tc_State == TS_RUN;
}

static void p4_signal_adapter_flush_locked(
    struct Task *task,
    const struct p4_signal_task_state *state)
{
    CHECK(task == mock_locked_task);
    CHECK(state != NULL);
    CHECK(mock_task_lock_depth == 1u);
    CHECK(mock_mie == 0u);
    ++mock_flush_calls;
    record(EV_FLUSH);
    task->tc_SigRecvd = state->sig_received;
    task->tc_SigWait = state->sig_wait;
    task->tc_SigExcept = state->sig_except;
    task->tc_Flags = state->flags;
}

static uint32_t (*const real_exception_take)(
    struct p4_signal_task_state *) = p4_signal_exception_take_locked;
static uint32_t (*const real_exception_merge)(
    struct p4_signal_task_state *, uint32_t) =
        p4_signal_exception_merge_locked;

static uint32_t mock_exception_take_locked(
    struct p4_signal_task_state *state)
{
    CHECK(mock_task_lock_depth == 1u);
    CHECK(mock_locked_task == mock_current_task);
    CHECK(mock_mie == 0u);
    ++mock_take_calls;
    record(EV_TAKE);
    return real_exception_take(state);
}

static uint32_t mock_exception_merge_locked(
    struct p4_signal_task_state *state,
    uint32_t callback_mask)
{
    CHECK(mock_task_lock_depth == 1u);
    CHECK(mock_locked_task == mock_current_task);
    CHECK(mock_mie == 0u);
    ++mock_merge_calls;
    record(EV_MERGE);
    return real_exception_merge(state, callback_mask);
}

static ULONG callback_exceptions(APTR code, APTR data, ULONG flags,
                                 struct ExecBase *sys_base);

#define AROS_LH0(ret, name, base_type, base_name, offset, lib) \
    ret name(base_type base_name)
#define AROS_LIBFUNC_INIT
#define AROS_LIBFUNC_EXIT
#define AROS_UFCA(type, value, reg) (value)
#define AROS_UFC3(ret, func, arg1, arg2, arg3) \
    callback_exceptions((APTR)(func), (APTR)(arg1), (ULONG)(arg2), \
        (struct ExecBase *)(arg3))
#define GET_THIS_TASK mock_current_task
#define IDNESTCOUNT_GET (mock_idnest)
#define IDNESTCOUNT_SET(value) \
    do { mock_idnest = (BYTE)(value); \
         record((BYTE)(value) == 0 ? EV_ID_SET_ZERO : EV_ID_RESTORE); } while (0)
#define p4_signal_exception_take_locked(state) \
    mock_exception_take_locked((state))
#define p4_signal_exception_merge_locked(state, mask) \
    mock_exception_merge_locked((state), (mask))

void KrnCli(void);
void KrnSti(void);
void Enable(void);
void Disable(void);

#define P4_EXCEPTION_BODY_TEST 1
#include "../exception.c"

void KrnCli(void)
{
    record(EV_CLI);
    mock_mie = 0u;
}

void KrnSti(void)
{
    CHECK(mock_mie == 0u);
    record(EV_STI);
    mock_mie = 1u;
}

void Enable(void)
{
    CHECK(mock_mie == 0u);
    CHECK(mock_idnest != INT8_MIN);
    if (mock_idnest != INT8_MIN)
        mock_idnest = (BYTE)(mock_idnest - 1);
    record(EV_ENABLE);
}

void Disable(void)
{
    KrnCli();
    CHECK(mock_idnest != INT8_MAX);
    if (mock_idnest != INT8_MAX)
        mock_idnest = (BYTE)(mock_idnest + 1);
    record(EV_DISABLE);
}

static void inject_callback_signal(uint32_t signal_bits)
{
    unsigned int old_mie = mock_mie;
    Disable();
    p4_signal_adapter_lock_disabled(mock_current_task);
    mock_current_task->tc_SigRecvd |= signal_bits;
    p4_signal_adapter_unlock_disabled(mock_current_task);
    Enable();
    mock_mie = old_mie;
}

static void retire_during_callback(void)
{
    Disable();
    p4_signal_adapter_lock_disabled(mock_current_task);
    mock_current_task->tc_State = TS_REMOVED;
    p4_signal_adapter_unlock_disabled(mock_current_task);
    Enable();
    mock_mie = 1u;
    record(EV_RETIRE);
}

static ULONG callback_exceptions(APTR code, APTR data, ULONG flags,
                                 struct ExecBase *sys_base)
{
    unsigned int index = mock_callback_calls;
    CHECK(code == expected_exception_code);
    CHECK(data == expected_exception_data);
    CHECK(sys_base == &test_sysbase);
    CHECK(mock_task_lock_depth == 0u);
    CHECK(mock_mie == 1u);
    CHECK(mock_idnest == -1);
    if (index < 8u)
    {
        callback_flags[index] = flags;
        ++mock_callback_calls;
    }
    else
        CHECK(0);
    record(EV_CALLBACK_ENTER);

    if (index == 0u && inject_on_first_callback)
        inject_callback_signal(callback_injected_signals);
    if (index == 0u && retire_on_first_callback)
        retire_during_callback();

    record(EV_CALLBACK_RETURN);
    return index < 8u ? callback_returns[index] : 0u;
}

static void prepare_exception_task(struct Task *task, BYTE original_idnest)
{
    static uint32_t code_cookie;
    static uint32_t data_cookie;
    reset_fixture(task, original_idnest);
    expected_exception_code = &code_cookie;
    expected_exception_data = &data_cookie;
    task->tc_State = TS_RUN;
    task->tc_ExceptCode = expected_exception_code;
    task->tc_ExceptData = expected_exception_data;
}

static int expect_no_task_fatal(void)
{
    fatal_armed = 1;
    if (setjmp(fatal_jump) == 0)
    {
        Exception(&test_sysbase);
        fatal_armed = 0;
        return 0;
    }
    fatal_armed = 0;
    return 1;
}

static void test_signal_added_during_callback_is_merged(void)
{
    struct Task task;

    prepare_exception_task(&task, 5);
    task.tc_SigRecvd = UINT32_C(0x01);
    task.tc_SigWait = UINT32_C(0x10);
    task.tc_SigExcept = UINT32_C(0x01);
    task.tc_Flags = TF_EXCEPT;
    inject_on_first_callback = 1;
    callback_injected_signals = UINT32_C(0x06);
    callback_returns[0] = UINT32_C(0x02);
    callback_returns[1] = 0u;

    Exception(&test_sysbase);

    CHECK(mock_callback_calls == 2u);
    CHECK(callback_flags[0] == UINT32_C(0x01));
    CHECK(callback_flags[1] == UINT32_C(0x02));
    CHECK(task.tc_SigRecvd == UINT32_C(0x04));
    CHECK(task.tc_SigExcept == 0u);
    CHECK(task.tc_SigWait == UINT32_C(0x10));
    CHECK((task.tc_Flags & TF_EXCEPT) == 0u);
    CHECK(mock_take_calls == 3u);
    CHECK(mock_merge_calls == 2u);
    CHECK(mock_load_calls == 5u && mock_flush_calls == 5u);
    CHECK(mock_idnest == 5);
    CHECK(mock_mie == 0u);
    CHECK(mock_task_lock_depth == 0u);
    CHECK(mock_lock_calls == mock_unlock_calls);
    CHECK(event_before(EV_TASK_LOCK, 0u, EV_TAKE, 0u));
    CHECK(event_before(EV_TAKE, 0u, EV_FLUSH, 0u));
    CHECK(event_before(EV_FLUSH, 0u, EV_TASK_UNLOCK, 0u));
    CHECK(event_before(EV_ENABLE, 0u, EV_STI, 0u));
    CHECK(event_before(EV_STI, 0u, EV_CALLBACK_ENTER, 0u));
    CHECK(event_before(EV_CALLBACK_RETURN, 0u, EV_MERGE, 0u));
}

static void test_no_flags_and_null_callback_code(void)
{
    struct Task task;

    prepare_exception_task(&task, 3);
    task.tc_SigRecvd = UINT32_C(0x40);
    task.tc_SigWait = UINT32_C(0x08);
    task.tc_ExceptCode = expected_exception_code;
    Exception(&test_sysbase);
    CHECK(mock_callback_calls == 0u);
    CHECK(task.tc_SigRecvd == UINT32_C(0x40));
    CHECK(task.tc_SigWait == UINT32_C(0x08));
    CHECK(mock_take_calls == 1u && mock_merge_calls == 0u);
    CHECK(mock_flush_calls == 1u);
    CHECK(mock_idnest == 3);
    CHECK(mock_mie == 0u);

    prepare_exception_task(&task, -2);
    task.tc_SigRecvd = UINT32_C(0x01);
    task.tc_SigExcept = UINT32_C(0x01);
    task.tc_Flags = TF_EXCEPT;
    task.tc_ExceptCode = NULL;
    Exception(&test_sysbase);
    CHECK(mock_callback_calls == 0u);
    CHECK(task.tc_SigRecvd == 0u);
    CHECK(task.tc_SigExcept == 0u);
    CHECK((task.tc_Flags & TF_EXCEPT) == 0u);
    CHECK(mock_take_calls == 2u && mock_merge_calls == 0u);
    CHECK(mock_flush_calls == 2u);
    CHECK(mock_idnest == -2);
    CHECK(mock_mie == 0u);
}

static void test_terminal_before_consume(void)
{
    static const ULONG terminal_states[] = {
        TS_REMOVED, TS_INVALID, TS_TOMBSTONED
    };
    struct Task task;
    size_t i;

    for (i = 0; i < sizeof(terminal_states) / sizeof(terminal_states[0]); ++i)
    {
        prepare_exception_task(&task, 2);
        task.tc_State = terminal_states[i];
        task.tc_SigRecvd = UINT32_C(0x03);
        task.tc_SigExcept = UINT32_C(0x01);
        task.tc_Flags = TF_EXCEPT;
        Exception(&test_sysbase);
        CHECK(mock_callback_calls == 0u);
        CHECK(mock_load_calls == 0u && mock_take_calls == 0u);
        CHECK(mock_flush_calls == 0u && mock_merge_calls == 0u);
        CHECK(task.tc_SigRecvd == UINT32_C(0x03));
        CHECK(task.tc_SigExcept == UINT32_C(0x01));
        CHECK(task.tc_Flags == TF_EXCEPT);
        CHECK(mock_idnest == 2);
        CHECK(mock_mie == 0u);
        CHECK(mock_task_lock_depth == 0u);
    }
}

static void test_retire_after_callback_skips_merge(void)
{
    struct Task task;

    prepare_exception_task(&task, 4);
    task.tc_SigRecvd = UINT32_C(0x01);
    task.tc_SigExcept = UINT32_C(0x01);
    task.tc_Flags = TF_EXCEPT;
    inject_on_first_callback = 1;
    callback_injected_signals = UINT32_C(0x02);
    callback_returns[0] = UINT32_C(0x02);
    retire_on_first_callback = 1;

    Exception(&test_sysbase);

    CHECK(task.tc_State == TS_REMOVED);
    CHECK(mock_callback_calls == 1u);
    CHECK(mock_load_calls == 1u);
    CHECK(mock_take_calls == 1u);
    CHECK(mock_flush_calls == 1u);
    CHECK(mock_merge_calls == 0u);
    CHECK(task.tc_SigRecvd == UINT32_C(0x02));
    CHECK(task.tc_SigExcept == 0u);
    CHECK(mock_idnest == 4);
    CHECK(mock_mie == 0u);
    CHECK(mock_task_lock_depth == 0u);
    CHECK(mock_lock_calls == mock_unlock_calls);
}

static void test_null_task_fails_closed(void)
{
    prepare_exception_task(&(struct Task){0}, 7);
    mock_current_task = NULL;
    mock_idnest = 7;
    mock_mie = 1u;
    CHECK(expect_no_task_fatal());
    CHECK(mock_fatal_calls == 1u);
    CHECK(mock_idnest == 7);
    CHECK(mock_mie == 1u);
    CHECK(mock_lock_calls == 0u);
    CHECK(mock_callback_calls == 0u);
}

int main(void)
{
    test_signal_added_during_callback_is_merged();
    test_no_flags_and_null_callback_code();
    test_terminal_before_consume();
    test_retire_after_callback_skips_merge();
    test_null_task_fails_closed();
    if (failures != 0u)
    {
        fprintf(stderr, "FAIL p4_exception_body checks=%u failures=%u\n",
                checks, failures);
        return 1;
    }
    printf("PASS p4_exception_body checks=%u\n", checks);
    return 0;
}
