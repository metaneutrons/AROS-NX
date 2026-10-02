/*
 * Host fixture for the actual P4 SMP SetSignal() body. Only Exec ABI macros,
 * Task layout, raw interrupt state, lock boundary, and adapter binding are
 * mocked; p4_signal_set_locked() is compiled from the production protocol.
 */
#define _DARWIN_C_SOURCE 1
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "../p4_signal_protocol.h"
#include "../p4_self_remove.h"
#include "../../kernel/p4_callback_escape.h"

typedef uint32_t ULONG;

#define TS_INVALID    UINT32_C(0)
#define TS_RUN        UINT32_C(1)
#define TS_READY      UINT32_C(2)
#define TS_WAIT       UINT32_C(3)
#define TS_REMOVED    UINT32_C(4)
#define TS_TOMBSTONED UINT32_C(5)
#define TF_EXCEPT     UINT8_C(0x80)

struct Task
{
    uint32_t tc_State;
    uint32_t tc_SigRecvd;
    uint32_t tc_SigWait;
    uint32_t tc_SigExcept;
    uint8_t tc_Flags;
    uint8_t padding[3];
    uint32_t tc_SpinLock;
};

struct ExecBase
{
    unsigned int marker;
};

static struct Task *current_task;
static struct ExecBase test_sysbase;

#define GET_THIS_TASK current_task
#define AROS_LHA(type, name, reg) type name
#define AROS_LH2(ret, name, arg1, arg2, base_type, base_name, offset, lib) \
    ret name(arg1, arg2, base_type base_name)
#define AROS_LIBFUNC_INIT
#define AROS_LIBFUNC_EXIT

ULONG SetSignal(ULONG newSignals, ULONG signalSet,
    struct ExecBase *SysBase);
void Disable(void);
void Enable(void);
void p4_signal_adapter_lock_disabled(struct Task *task);
void p4_signal_adapter_unlock_disabled(struct Task *task);
struct p4_signal_effect p4_signal_adapter_set_locked(
    struct Task *task,
    struct ExecBase *sys_base,
    uint32_t signal_mask,
    uint32_t new_signals);
void p4_signal_adapter_fail_closed(void) __attribute__((noreturn));

#define P4_SET_SIGNAL_BODY_TEST 1
#include "../setsignal.c"

enum event
{
    EV_DISABLE = 1,
    EV_LOCK,
    EV_SET,
    EV_UNLOCK,
    EV_ENABLE,
    EV_RETIRE_QUERY,
    EV_REMOVE_SELF,
    EV_FATAL
};

static unsigned int checks;
static int mock_failed;
static int ok;
static enum event events[16];
static size_t event_count;
static unsigned int disable_depth;
static int raw_mie;
static int saved_mie;
static int task_lock_held;
static int poison_at_enable;
static int task_poisoned;
static int fatal_expected;
static int fatal_count;
static jmp_buf fatal_env;
static int retired_query_result;
static unsigned int retired_query_calls;
static int remove_expected;
static unsigned int remove_calls;
static jmp_buf remove_env;
static int poison_after_unlock;
static struct Task *mapped_task;
static void *task_page;
static size_t task_page_size;
static struct p4_signal_effect last_effect;
static uint32_t snapshot_received;
static uint32_t snapshot_wait;
static uint32_t snapshot_except;
static uint32_t snapshot_state;
static uint8_t snapshot_flags;
static enum p4_signal_owner_state snapshot_owner;

#define REQUIRE(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "MOCK CONTRACT FAIL line %d: %s\n", \
            __LINE__, #condition); \
        mock_failed = 1; \
    } \
} while (0)

#define CHECK(condition) do { \
    ++checks; \
    if (!(condition)) { \
        fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); \
        ok = 0; \
    } \
} while (0)

static void record_event(enum event value)
{
    if (event_count < sizeof(events) / sizeof(events[0]))
        events[event_count++] = value;
    else
        mock_failed = 1;
}

static void poison_task_page(void)
{
    if (task_page == NULL || mapped_task == NULL)
    {
        mock_failed = 1;
        return;
    }
    if (mprotect(task_page, task_page_size, PROT_NONE) != 0)
        mock_failed = 1;
    else
        task_poisoned = 1;
}

static int fixture_init(uint32_t state, int initial_mie)
{
    void *page;

    task_page_size = (size_t)sysconf(_SC_PAGESIZE);
    if (task_page_size == 0)
        return 0;
    page = mmap(NULL, task_page_size, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANON, -1, 0);
    if (page == MAP_FAILED)
        return 0;

    task_page = page;
    mapped_task = (struct Task *)page;
    memset(mapped_task, 0, sizeof(*mapped_task));
    mapped_task->tc_State = state;
    mapped_task->tc_SigRecvd = UINT32_C(0x89abcdef);
    mapped_task->tc_SigWait = UINT32_C(0x13572468);
    mapped_task->tc_SigExcept = UINT32_C(0x24681357);
    mapped_task->tc_Flags = UINT8_C(0x25);
    mapped_task->tc_SpinLock = UINT32_C(0x5a5a5a5a);
    current_task = mapped_task;
    memset(events, 0, sizeof(events));
    event_count = 0;
    disable_depth = 0;
    raw_mie = initial_mie;
    saved_mie = initial_mie;
    task_lock_held = 0;
    poison_at_enable = 0;
    task_poisoned = 0;
    fatal_expected = 0;
    fatal_count = 0;
    retired_query_result = 0;
    retired_query_calls = 0;
    remove_expected = 0;
    remove_calls = 0;
    poison_after_unlock = 0;
    mock_failed = 0;
    ok = 1;
    memset(&last_effect, 0, sizeof(last_effect));
    snapshot_received = 0;
    snapshot_wait = 0;
    snapshot_except = 0;
    snapshot_state = 0;
    snapshot_flags = 0;
    snapshot_owner = P4_SIGNAL_OWNER_INVALID;
    return 1;
}

static void fixture_destroy(void)
{
    if (task_page != NULL)
    {
        (void)mprotect(task_page, task_page_size, PROT_READ | PROT_WRITE);
        (void)munmap(task_page, task_page_size);
    }
    task_page = NULL;
    mapped_task = NULL;
    current_task = NULL;
}

void Disable(void)
{
    REQUIRE(disable_depth == 0);
    saved_mie = raw_mie;
    raw_mie = 0;
    ++disable_depth;
    record_event(EV_DISABLE);
}

void Enable(void)
{
    REQUIRE(disable_depth == 1);
    REQUIRE(!task_lock_held);
    REQUIRE(raw_mie == 0);
    --disable_depth;
    raw_mie = saved_mie;
    record_event(EV_ENABLE);
    /* Model a pending scheduler action discarding the current Task at Enable. */
    if (poison_at_enable)
        poison_task_page();
}

void p4_signal_adapter_lock_disabled(struct Task *task)
{
    REQUIRE(task == mapped_task);
    REQUIRE(disable_depth == 1);
    REQUIRE(raw_mie == 0);
    REQUIRE(!task_lock_held);
    task_lock_held = 1;
    record_event(EV_LOCK);
}

void p4_signal_adapter_unlock_disabled(struct Task *task)
{
    REQUIRE(task == mapped_task);
    REQUIRE(disable_depth == 1);
    REQUIRE(raw_mie == 0);
    REQUIRE(task_lock_held);
    task_lock_held = 0;
    record_event(EV_UNLOCK);
    if (poison_after_unlock)
        poison_task_page();
}

int krnP4SelfRemoveCurrentRetired(struct ExecBase *sys_base)
{
    REQUIRE(sys_base == &test_sysbase);
    REQUIRE(disable_depth == 1);
    REQUIRE(raw_mie == 0);
    REQUIRE(!task_lock_held);
    ++retired_query_calls;
    record_event(EV_RETIRE_QUERY);
    return retired_query_result;
}

void Exec_P4RemoveSelf(struct Task *task, struct ExecBase *sys_base)
{
    REQUIRE(task == mapped_task);
    REQUIRE(sys_base == &test_sysbase);
    REQUIRE(disable_depth == 1);
    REQUIRE(raw_mie == 0);
    REQUIRE(!task_lock_held);
    REQUIRE(task_poisoned);
    ++remove_calls;
    record_event(EV_REMOVE_SELF);
    if (!remove_expected)
    {
        fprintf(stderr, "unexpected self-removal transfer\n");
        abort();
    }
    longjmp(remove_env, 1);
}

static enum p4_signal_owner_state owner_for(uint32_t state)
{
    switch (state)
    {
    case TS_RUN: return P4_SIGNAL_OWNER_RUNNING;
    case TS_READY: return P4_SIGNAL_OWNER_READY;
    case TS_WAIT: return P4_SIGNAL_OWNER_WAITING;
    case TS_REMOVED:
    case TS_TOMBSTONED: return P4_SIGNAL_OWNER_REMOVED;
    default: return P4_SIGNAL_OWNER_INVALID;
    }
}

struct p4_signal_effect p4_signal_adapter_set_locked(
    struct Task *task,
    struct ExecBase *sys_base,
    uint32_t signal_mask,
    uint32_t new_signals)
{
    struct p4_signal_task_state state;

    REQUIRE(task == mapped_task);
    REQUIRE(sys_base == &test_sysbase);
    REQUIRE(disable_depth == 1);
    REQUIRE(raw_mie == 0);
    REQUIRE(task_lock_held);
    record_event(EV_SET);

    state.sig_received = task->tc_SigRecvd;
    state.sig_wait = task->tc_SigWait;
    state.sig_except = task->tc_SigExcept;
    state.flags = task->tc_Flags;
    state.exception_flag_mask = TF_EXCEPT;
    state.owner = owner_for(task->tc_State);
    state.owner_hart = 0;

    /* Exercise the production primitive, not a fixture reimplementation. */
    last_effect = p4_signal_set_locked(&state, signal_mask, new_signals);
    if (last_effect.status == P4_SIGNAL_STATUS_OK)
    {
        task->tc_SigRecvd = state.sig_received;
        task->tc_SigWait = state.sig_wait;
        task->tc_SigExcept = state.sig_except;
        task->tc_Flags = state.flags;
        task->tc_State = state.owner == P4_SIGNAL_OWNER_RUNNING ? TS_RUN :
            (state.owner == P4_SIGNAL_OWNER_READY ? TS_READY : TS_WAIT);
    }

    snapshot_received = task->tc_SigRecvd;
    snapshot_wait = task->tc_SigWait;
    snapshot_except = task->tc_SigExcept;
    snapshot_flags = task->tc_Flags;
    snapshot_state = task->tc_State;
    snapshot_owner = state.owner;
    return last_effect;
}

void p4_signal_adapter_fail_closed(void)
{
    ++fatal_count;
    record_event(EV_FATAL);
    if (remove_expected)
        longjmp(remove_env, 2);
    if (!fatal_expected)
    {
        fprintf(stderr, "unexpected fail-closed path\n");
        abort();
    }
    longjmp(fatal_env, 1);
}

static int event_sequence_is(const enum event *expected, size_t count)
{
    size_t index;

    if (event_count != count)
        return 0;
    for (index = 0; index < count; ++index)
    {
        if (events[index] != expected[index])
            return 0;
    }
    return 1;
}

static int test_value_case(uint32_t old_signals, uint32_t signal_mask,
                           uint32_t new_signals, int poison, int initial_mie)
{
    static const enum event expected_events[] = {
        EV_DISABLE, EV_LOCK, EV_SET, EV_UNLOCK, EV_ENABLE
    };
    ULONG returned;
    uint32_t expected;
    uint32_t wait_before;
    uint32_t except_before;
    uint32_t state_before;
    uint8_t flags_before;
    uint32_t spin_before;

    CHECK(fixture_init(TS_RUN, initial_mie));
    mapped_task->tc_SigRecvd = old_signals;
    wait_before = mapped_task->tc_SigWait;
    except_before = mapped_task->tc_SigExcept;
    state_before = mapped_task->tc_State;
    flags_before = mapped_task->tc_Flags;
    spin_before = mapped_task->tc_SpinLock;
    expected = (old_signals & ~signal_mask) | (new_signals & signal_mask);
    poison_at_enable = poison;

    returned = SetSignal(new_signals, signal_mask, &test_sysbase);

    CHECK(returned == old_signals);
    CHECK(last_effect.status == P4_SIGNAL_STATUS_OK);
    CHECK(last_effect.returned_signals == old_signals);
    CHECK(snapshot_received == expected);
    CHECK(snapshot_wait == wait_before);
    CHECK(snapshot_except == except_before);
    CHECK(snapshot_flags == flags_before);
    CHECK(snapshot_state == state_before && snapshot_owner ==
        P4_SIGNAL_OWNER_RUNNING);
    CHECK(event_sequence_is(expected_events,
        sizeof(expected_events) / sizeof(expected_events[0])));
    CHECK(disable_depth == 0 && raw_mie == initial_mie && !task_lock_held);
    CHECK(task_poisoned == poison);
    CHECK(!mock_failed);
    if (!task_poisoned)
    {
        CHECK(mapped_task->tc_SpinLock == spin_before);
        CHECK(mapped_task->tc_SigWait == wait_before);
        CHECK(mapped_task->tc_SigExcept == except_before);
        CHECK(mapped_task->tc_Flags == flags_before);
        CHECK(mapped_task->tc_State == state_before);
    }
    fixture_destroy();
    return ok;
}

static int test_null_current_fails_closed(void)
{
    int jumped;

    CHECK(fixture_init(TS_RUN, 1));
    current_task = NULL;
    fatal_expected = 1;
    jumped = setjmp(fatal_env);
    if (jumped == 0)
    {
        (void)SetSignal(UINT32_C(0xffffffff), UINT32_C(0x12345678),
            &test_sysbase);
        CHECK(0);
    }
    CHECK(fatal_count == 1);
    CHECK(event_sequence_is((const enum event[]){ EV_FATAL }, 1));
    CHECK(disable_depth == 0 && raw_mie == 1 && !task_lock_held);
    CHECK(!mock_failed);
    fixture_destroy();
    return ok;
}

static int test_retired_adapter_refusal_fails_closed(void)
{
    static const enum event expected_events[] = {
        EV_DISABLE, EV_LOCK, EV_SET, EV_UNLOCK, EV_RETIRE_QUERY, EV_FATAL
    };
    int jumped;
    uint32_t received_before;
    uint32_t wait_before;
    uint32_t except_before;
    uint8_t flags_before;

    CHECK(fixture_init(TS_REMOVED, 1));
    received_before = mapped_task->tc_SigRecvd;
    wait_before = mapped_task->tc_SigWait;
    except_before = mapped_task->tc_SigExcept;
    flags_before = mapped_task->tc_Flags;
    fatal_expected = 1;
    jumped = setjmp(fatal_env);
    if (jumped == 0)
    {
        (void)SetSignal(UINT32_C(0x1), UINT32_C(0xffffffff),
            &test_sysbase);
        CHECK(0);
    }
    CHECK(last_effect.status == P4_SIGNAL_STATUS_NOT_LIVE);
    CHECK(fatal_count == 1);
    CHECK(event_sequence_is(expected_events,
        sizeof(expected_events) / sizeof(expected_events[0])));
    CHECK(disable_depth == 1 && raw_mie == 0 && !task_lock_held);
    CHECK(mapped_task->tc_SigRecvd == received_before);
    CHECK(mapped_task->tc_SigWait == wait_before);
    CHECK(mapped_task->tc_SigExcept == except_before);
    CHECK(mapped_task->tc_Flags == flags_before);
    CHECK(!mock_failed);
    fixture_destroy();
    return ok;
}

static int test_current_retired_transfers_after_unlock(void)
{
    static const enum event expected_events[] = {
        EV_DISABLE, EV_LOCK, EV_SET, EV_UNLOCK,
        EV_RETIRE_QUERY, EV_REMOVE_SELF
    };
    int jumped;

    CHECK(fixture_init(TS_REMOVED, 1));
    retired_query_result = 1;
    remove_expected = 1;
    poison_after_unlock = 1;
    jumped = setjmp(remove_env);
    if (jumped == 0)
    {
        (void)SetSignal(UINT32_C(0x1), UINT32_C(0xffffffff),
            &test_sysbase);
        CHECK(0); /* A retired RUN continuation must not return. */
    }

    CHECK(jumped == 1);
    CHECK(last_effect.status == P4_SIGNAL_STATUS_NOT_LIVE);
    CHECK(retired_query_calls == 1u);
    CHECK(remove_calls == 1u);
    CHECK(fatal_count == 0);
    CHECK(event_sequence_is(expected_events,
        sizeof(expected_events) / sizeof(expected_events[0])));
    CHECK(disable_depth == 1 && raw_mie == 0 && !task_lock_held);
    CHECK(task_poisoned);
    CHECK(!mock_failed);
    fixture_destroy();
    return ok;
}

int main(void)
{
    int passed = 1;

    passed &= test_value_case(UINT32_C(0xa5a55a5a), 0,
        UINT32_C(0xffffffff), 0, 1);
    passed &= test_value_case(UINT32_C(0x89abcdef), UINT32_C(0xffffffff),
        UINT32_C(0x13579bdf), 0, 1);
    passed &= test_value_case(UINT32_C(0xab123456), UINT32_C(0x00ff0f0f),
        UINT32_C(0x654321ba), 0, 1);
    passed &= test_value_case(UINT32_C(0x01234567), UINT32_C(0x80000000),
        UINT32_C(0xf1234567), 1, 1);
    passed &= test_value_case(UINT32_C(0xf0f0f0f0), UINT32_C(0xf0000000),
        UINT32_C(0x01234567), 0, 0);
    passed &= test_null_current_fails_closed();
    passed &= test_retired_adapter_refusal_fails_closed();
    passed &= test_current_retired_transfers_after_unlock();
    if (!passed || mock_failed)
        return 1;

    printf("PASS p4-setsignal-body checks=%u\n", checks);
    return 0;
}
