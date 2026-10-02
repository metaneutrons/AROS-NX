/* Host fixture for the actual P4 dispatch callback/liveness helper. */
#include <setjmp.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint32_t ULONG;
typedef void *APTR;

struct ExecBase
{
    uint32_t marker;
};

struct TestSpinlock
{
    uint32_t marker;
};

struct Task
{
    struct TestSpinlock tc_SpinLock;
    ULONG tc_State;
    ULONG tc_Flags;
    APTR tc_Launch;
};

struct p4_cpu_local
{
    struct Task *current_task;
};

struct p4_exec_runtime
{
    struct p4_cpu_local *cpu_local;
};

#define TS_RUN        UINT32_C(1)
#define TS_READY      UINT32_C(2)
#define TS_REMOVED    UINT32_C(3)
#define TS_INVALID    UINT32_C(4)
#define TS_TOMBSTONED UINT32_C(5)
#define TF_LAUNCH     (UINT32_C(1) << 0)
#define TF_EXCEPT     (UINT32_C(1) << 1)
#define SPINLOCK_MODE_WRITE 1u
#define P4_PUBLIC_SPIN_BAD_STATE 7

static struct ExecBase test_sysbase;
static struct Task *active_task;
static struct p4_exec_runtime *mock_runtime;
static int mock_mie;
static unsigned int mock_task_lock_depth;
static unsigned int mock_lock_calls;
static unsigned int mock_unlock_calls;
static unsigned int mock_exception_calls;
static unsigned int mock_callback_wrapper_calls;
static unsigned int mock_launch_a_calls;
static unsigned int mock_launch_b_calls;
static unsigned int mock_fatal_calls;
static int mock_fatal_code;
static unsigned int mock_fatal_lock_depth;
static int fatal_armed;
static jmp_buf fatal_jump;
static APTR launch_a_identity;
static APTR launch_b_identity;

static void mock_fatal(int code) __attribute__((noreturn));
static void mock_task_lock(struct TestSpinlock *lock, struct ExecBase *base,
                           unsigned int mode);
static void mock_task_unlock(struct TestSpinlock *lock);
static void mock_launch_invoke(APTR launch, struct ExecBase *base);
static struct p4_exec_runtime *p4_exec_runtime_current(struct ExecBase *base);
int krnP4RunTaskCallback(void (*invoke)(void *), void *argument,
                         struct ExecBase *base);
void KrnCli(void);
void Exception(void);

#define EXEC_SPINLOCK_LOCK(lock, base, mode) \
    mock_task_lock((struct TestSpinlock *)(lock), (base), (mode))
#define EXEC_SPINLOCK_UNLOCK(lock) \
    mock_task_unlock((struct TestSpinlock *)(lock))
#define AROS_UFCA(type, value, reg) (value)
#define AROS_UFC1(ret, func, arg1) \
    mock_launch_invoke((APTR)(func), (struct ExecBase *)(arg1))
#define krnP4PublicSpinFatal mock_fatal

#define P4_DISPATCH_CALLBACKS_TEST 1
#include "../p4_dispatch_callbacks.c"

enum event
{
    EV_CLI = 1,
    EV_RUNTIME,
    EV_TASK_LOCK,
    EV_TASK_UNLOCK,
    EV_EXCEPTION_ENTER,
    EV_EXCEPTION_RETURN,
    EV_LAUNCH_A_ENTER,
    EV_LAUNCH_A_RETURN,
    EV_LAUNCH_B_ENTER,
    EV_LAUNCH_B_RETURN,
    EV_FATAL
};

#define EVENT_CAPACITY 128u

static unsigned int checks;
static unsigned int failures;
static enum event events[EVENT_CAPACITY];
static size_t event_count;

enum exception_action
{
    EXCEPTION_NO_CHANGE = 0,
    EXCEPTION_REPLACE_LAUNCH,
    EXCEPTION_RETIRE_TASK
};

enum launch_action
{
    LAUNCH_KEEP_LIVE = 0,
    LAUNCH_RETIRE_TASK
};

static enum exception_action current_exception_action;
static enum launch_action current_launch_action;

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

static void reset_test(struct Task *task, struct p4_exec_runtime *runtime,
                       struct p4_cpu_local *cpu_local)
{
    memset(&test_sysbase, 0, sizeof(test_sysbase));
    memset(task, 0, sizeof(*task));
    task->tc_State = TS_RUN;
    active_task = task;
    cpu_local->current_task = task;
    runtime->cpu_local = cpu_local;
    mock_runtime = runtime;
    mock_mie = 1;
    mock_task_lock_depth = 0u;
    mock_lock_calls = 0u;
    mock_unlock_calls = 0u;
    mock_exception_calls = 0u;
    mock_callback_wrapper_calls = 0u;
    mock_launch_a_calls = 0u;
    mock_launch_b_calls = 0u;
    mock_fatal_calls = 0u;
    mock_fatal_code = 0;
    mock_fatal_lock_depth = 0u;
    fatal_armed = 0;
    event_count = 0;
    memset(events, 0, sizeof(events));
    current_exception_action = EXCEPTION_NO_CHANGE;
    current_launch_action = LAUNCH_KEEP_LIVE;
}

static void mock_fatal(int code)
{
    ++mock_fatal_calls;
    mock_fatal_code = code;
    mock_fatal_lock_depth = mock_task_lock_depth;
    record(EV_FATAL);
    if (fatal_armed)
        longjmp(fatal_jump, 1);
    abort();
}

static void mock_task_lock(struct TestSpinlock *lock, struct ExecBase *base,
                           unsigned int mode)
{
    struct Task *task = (struct Task *)((char *)lock -
        offsetof(struct Task, tc_SpinLock));
    CHECK(base == NULL);
    CHECK(mode == SPINLOCK_MODE_WRITE);
    CHECK(task == active_task);
    CHECK(mock_task_lock_depth == 0u);
    ++mock_task_lock_depth;
    ++mock_lock_calls;
    record(EV_TASK_LOCK);
}

static void mock_task_unlock(struct TestSpinlock *lock)
{
    struct Task *task = (struct Task *)((char *)lock -
        offsetof(struct Task, tc_SpinLock));
    CHECK(task == active_task);
    CHECK(mock_task_lock_depth == 1u);
    if (mock_task_lock_depth != 0u)
        --mock_task_lock_depth;
    ++mock_unlock_calls;
    record(EV_TASK_UNLOCK);
}

static struct p4_exec_runtime *p4_exec_runtime_current(struct ExecBase *base)
{
    CHECK(base == &test_sysbase);
    record(EV_RUNTIME);
    return mock_runtime;
}

void KrnCli(void)
{
    record(EV_CLI);
    mock_mie = 0;
}

int krnP4RunTaskCallback(void (*invoke)(void *), void *argument,
                         struct ExecBase *base)
{
    CHECK(invoke != NULL);
    CHECK(base == &test_sysbase);
    CHECK(mock_task_lock_depth == 0u);
    ++mock_callback_wrapper_calls;
    invoke(argument);
    /* The production wrapper re-masks after arbitrary callback code returns. */
    KrnCli();
    return 1;
}

static void mutate_task(ULONG state, ULONG flags, APTR launch)
{
    mock_task_lock(&active_task->tc_SpinLock, NULL, SPINLOCK_MODE_WRITE);
    active_task->tc_State = state;
    active_task->tc_Flags = flags;
    active_task->tc_Launch = launch;
    mock_task_unlock(&active_task->tc_SpinLock);
}

void Exception(void)
{
    CHECK(mock_task_lock_depth == 0u);
    CHECK(mock_mie == 0);
    ++mock_exception_calls;
    record(EV_EXCEPTION_ENTER);
    mock_mie = 1;
    if (current_exception_action == EXCEPTION_REPLACE_LAUNCH)
        mutate_task(TS_RUN, TF_LAUNCH, launch_b_identity);
    else if (current_exception_action == EXCEPTION_RETIRE_TASK)
        mutate_task(TS_REMOVED, TF_LAUNCH, launch_b_identity);
    record(EV_EXCEPTION_RETURN);
}

static void mock_launch_invoke(APTR launch, struct ExecBase *base)
{
    CHECK(base == &test_sysbase);
    CHECK(mock_task_lock_depth == 0u);
    CHECK(mock_mie == 0);
    if (launch == launch_a_identity)
    {
        ++mock_launch_a_calls;
        record(EV_LAUNCH_A_ENTER);
        mock_mie = 1;
        record(EV_LAUNCH_A_RETURN);
        return;
    }
    if (launch == launch_b_identity)
    {
        ++mock_launch_b_calls;
        record(EV_LAUNCH_B_ENTER);
        mock_mie = 1;
        if (current_launch_action == LAUNCH_RETIRE_TASK)
            mutate_task(TS_REMOVED, 0u, NULL);
        record(EV_LAUNCH_B_RETURN);
        return;
    }
    CHECK(0);
}

static int expect_fatal(struct Task *task, struct ExecBase *base)
{
    fatal_armed = 1;
    if (setjmp(fatal_jump) == 0)
    {
        (void)krnP4DispatchCallbacks(task, base);
        fatal_armed = 0;
        return 0;
    }
    fatal_armed = 0;
    return 1;
}

static void test_live_without_callbacks(void)
{
    struct Task task;
    struct p4_cpu_local cpu_local;
    struct p4_exec_runtime runtime;

    reset_test(&task, &runtime, &cpu_local);
    CHECK(krnP4DispatchCallbacks(&task, &test_sysbase) == 1);
    CHECK(mock_mie == 0);
    CHECK(mock_exception_calls == 0u);
    CHECK(mock_launch_a_calls == 0u && mock_launch_b_calls == 0u);
    CHECK(mock_lock_calls == 3u && mock_unlock_calls == 3u);
    CHECK(mock_task_lock_depth == 0u);
}

static void test_exception_refreshes_launch_snapshot(void)
{
    struct Task task;
    struct p4_cpu_local cpu_local;
    struct p4_exec_runtime runtime;
    static int launch_a_cookie;
    static int launch_b_cookie;

    launch_a_identity = &launch_a_cookie;
    launch_b_identity = &launch_b_cookie;
    reset_test(&task, &runtime, &cpu_local);
    task.tc_Flags = TF_EXCEPT | TF_LAUNCH;
    task.tc_Launch = launch_a_identity;
    current_exception_action = EXCEPTION_REPLACE_LAUNCH;

    CHECK(krnP4DispatchCallbacks(&task, &test_sysbase) == 1);
    CHECK(mock_exception_calls == 1u);
    CHECK(mock_launch_a_calls == 0u);
    CHECK(mock_launch_b_calls == 1u);
    CHECK(mock_mie == 0);
    CHECK(mock_task_lock_depth == 0u);
    CHECK(mock_lock_calls == mock_unlock_calls);
    CHECK(event_before(EV_EXCEPTION_RETURN, 0u, EV_CLI, 1u));
    CHECK(event_before(EV_LAUNCH_B_RETURN, 0u, EV_CLI, 2u));
}

static void test_retired_before_callbacks(void)
{
    struct Task task;
    struct p4_cpu_local cpu_local;
    struct p4_exec_runtime runtime;
    static int launch_cookie;

    launch_a_identity = &launch_cookie;
    reset_test(&task, &runtime, &cpu_local);
    task.tc_State = TS_REMOVED;
    task.tc_Flags = TF_EXCEPT | TF_LAUNCH;
    task.tc_Launch = launch_a_identity;

    CHECK(krnP4DispatchCallbacks(&task, &test_sysbase) == 0);
    CHECK(mock_mie == 0);
    CHECK(mock_exception_calls == 0u);
    CHECK(mock_launch_a_calls == 0u && mock_launch_b_calls == 0u);
    CHECK(mock_lock_calls == 1u && mock_unlock_calls == 1u);
}

static void test_retired_during_exception(void)
{
    struct Task task;
    struct p4_cpu_local cpu_local;
    struct p4_exec_runtime runtime;
    static int launch_cookie;

    launch_b_identity = &launch_cookie;
    reset_test(&task, &runtime, &cpu_local);
    task.tc_Flags = TF_EXCEPT | TF_LAUNCH;
    task.tc_Launch = launch_b_identity;
    current_exception_action = EXCEPTION_RETIRE_TASK;

    CHECK(krnP4DispatchCallbacks(&task, &test_sysbase) == 0);
    CHECK(mock_mie == 0);
    CHECK(task.tc_State == TS_REMOVED);
    CHECK(mock_exception_calls == 1u);
    CHECK(mock_launch_a_calls == 0u && mock_launch_b_calls == 0u);
    CHECK(mock_lock_calls == 3u && mock_unlock_calls == 3u);
    CHECK(event_before(EV_EXCEPTION_RETURN, 0u, EV_CLI, 1u));
}

static void test_retired_during_launch(void)
{
    struct Task task;
    struct p4_cpu_local cpu_local;
    struct p4_exec_runtime runtime;
    static int launch_cookie;

    launch_b_identity = &launch_cookie;
    reset_test(&task, &runtime, &cpu_local);
    task.tc_Flags = TF_LAUNCH;
    task.tc_Launch = launch_b_identity;
    current_launch_action = LAUNCH_RETIRE_TASK;

    CHECK(krnP4DispatchCallbacks(&task, &test_sysbase) == 0);
    CHECK(mock_mie == 0);
    CHECK(task.tc_State == TS_REMOVED);
    CHECK(mock_launch_b_calls == 1u);
    CHECK(mock_exception_calls == 0u);
    CHECK(mock_lock_calls == 4u && mock_unlock_calls == 4u);
    CHECK(event_before(EV_LAUNCH_B_RETURN, 0u, EV_CLI, 2u));
}

static void test_callback_returns_live(void)
{
    struct Task task;
    struct p4_cpu_local cpu_local;
    struct p4_exec_runtime runtime;
    static int launch_cookie;

    launch_a_identity = &launch_cookie;
    reset_test(&task, &runtime, &cpu_local);
    task.tc_Flags = TF_LAUNCH;
    task.tc_Launch = launch_a_identity;

    CHECK(krnP4DispatchCallbacks(&task, &test_sysbase) == 1);
    CHECK(mock_launch_a_calls == 1u);
    CHECK(mock_mie == 0);
    CHECK(mock_task_lock_depth == 0u);
    CHECK(mock_lock_calls == mock_unlock_calls);
    CHECK(event_before(EV_LAUNCH_A_RETURN, 0u, EV_CLI, 2u));
}

static void test_fail_closed_inputs(void)
{
    struct Task task;
    struct Task other_task;
    struct p4_cpu_local cpu_local;
    struct p4_exec_runtime runtime;

    reset_test(&task, &runtime, &cpu_local);
    cpu_local.current_task = &other_task;
    CHECK(expect_fatal(&task, &test_sysbase));
    CHECK(mock_fatal_calls == 1u);
    CHECK(mock_fatal_code == P4_PUBLIC_SPIN_BAD_STATE);
    CHECK(mock_mie == 0);
    CHECK(mock_lock_calls == 0u);

    reset_test(&task, &runtime, &cpu_local);
    mock_runtime = NULL;
    CHECK(expect_fatal(&task, &test_sysbase));
    CHECK(mock_fatal_calls == 1u);
    CHECK(mock_mie == 0);
    CHECK(mock_lock_calls == 0u);

    reset_test(&task, &runtime, &cpu_local);
    task.tc_State = TS_READY;
    CHECK(expect_fatal(&task, &test_sysbase));
    CHECK(mock_fatal_calls == 1u);
    CHECK(mock_mie == 0);
    CHECK(mock_lock_calls == 1u && mock_unlock_calls == 0u);
    CHECK(mock_fatal_lock_depth == 1u);
    CHECK(mock_task_lock_depth == 1u);
    /* Invalid-state fatal is non-returning while its task lock is held. */

    reset_test(&task, &runtime, &cpu_local);
    task.tc_Flags = TF_LAUNCH;
    task.tc_Launch = NULL;
    CHECK(expect_fatal(&task, &test_sysbase));
    CHECK(mock_fatal_calls == 1u);
    CHECK(mock_mie == 0);
    CHECK(mock_exception_calls == 0u);
    CHECK(mock_launch_a_calls == 0u && mock_launch_b_calls == 0u);
    CHECK(mock_lock_calls == 2u && mock_unlock_calls == 2u);
    CHECK(mock_fatal_lock_depth == 0u);
    CHECK(mock_task_lock_depth == 0u);

    reset_test(&task, &runtime, &cpu_local);
    CHECK(expect_fatal(NULL, &test_sysbase));
    CHECK(mock_fatal_calls == 1u);
    CHECK(mock_mie == 0);
    CHECK(mock_lock_calls == 0u);
}

int main(void)
{
    test_live_without_callbacks();
    test_exception_refreshes_launch_snapshot();
    test_retired_before_callbacks();
    test_retired_during_exception();
    test_retired_during_launch();
    test_callback_returns_live();
    test_fail_closed_inputs();
    if (failures != 0u)
    {
        fprintf(stderr, "FAIL p4_dispatch_callbacks checks=%u failures=%u\n",
                checks, failures);
        return 1;
    }
    printf("PASS p4_dispatch_callbacks checks=%u\n", checks);
    return 0;
}
