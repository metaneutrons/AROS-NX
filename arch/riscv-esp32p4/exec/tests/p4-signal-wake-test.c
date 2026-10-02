/*
 * Host fixture compiling the actual public Signal() and SetExcept() bodies.
 * P4_SIGNAL_WAKE_TEST suppresses only production includes and layout guards;
 * the bodies below are included verbatim. Adapter, lifetime, queue and poke
 * functions are explicit mocks. The portable state-machine implementation is
 * real and is called by the adapter mocks.
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
    ULONG tc_State;
    uint32_t tc_SigRecvd;
    uint32_t tc_SigWait;
    uint32_t tc_SigExcept;
    uint8_t tc_Flags;
    uint32_t owner_hart;
};

struct ExecBase { unsigned int unused; };

static struct Task *current_task;
static struct ExecBase test_sysbase;

#define GET_THIS_TASK current_task
#define AROS_LHA(type, name, reg) type name
#define AROS_LH2(ret, name, arg1, arg2, base_type, base_name, offset, lib) \
    ret name(arg1, arg2, base_type base_name)
#define AROS_LIBFUNC_INIT
#define AROS_LIBFUNC_EXIT

void Disable(void);
void Enable(void);
int krnP4SignalTaskPin(struct Task *task, struct ExecBase *sys_base);
void krnP4SignalTaskUnpin(struct Task *task, struct ExecBase *sys_base);
void krnP4SignalPoke(
    const struct p4_signal_effect *effect,
    struct ExecBase *sys_base);
void p4_signal_adapter_lock_disabled(struct Task *task);
void p4_signal_adapter_unlock_disabled(struct Task *task);
struct p4_signal_effect p4_signal_adapter_raise_locked(
    struct Task *task,
    struct ExecBase *sys_base,
    uint32_t signal_set);
struct p4_signal_effect p4_signal_adapter_set_except_locked(
    struct Task *task,
    struct ExecBase *sys_base,
    uint32_t signal_mask,
    uint32_t new_exceptions);
int p4_signal_adapter_apply_queue_locked(
    struct Task *task,
    struct ExecBase *sys_base,
    struct p4_signal_effect *effect);
void p4_signal_adapter_poke_after_unlock(
    const struct p4_signal_effect *effect,
    struct ExecBase *sys_base);
void p4_signal_adapter_fail_closed(void) __attribute__((noreturn));
int krnP4SelfRemoveCurrentRetired(struct ExecBase *sys_base);
void Exec_P4RemoveSelf(struct Task *task, struct ExecBase *sys_base)
    __attribute__((noreturn));

#define P4_SIGNAL_WAKE_TEST 1
#include "../signal.c"
#include "../setexcept.c"

enum event
{
    EV_DISABLE = 1,
    EV_PIN,
    EV_RETIRE,
    EV_LOCK,
    EV_RAISE,
    EV_SET_EXCEPT,
    EV_QUEUE_APPLY,
    EV_UNLOCK,
    EV_UNPIN,
    EV_POKE,
    EV_ENABLE,
    EV_CURRENT_RETIRED,
    EV_REMOVE_SELF,
    EV_FAIL_CLOSED
};

static unsigned int checks;
static int mock_failed;
static enum event events[32];
static size_t event_count;
static unsigned int disable_depth;
static int raw_mie;
static unsigned int pin_count;
static int task_lock_held;
static int queue_lock_held;
static int pin_fails;
static int retire_at_task_lock;
static int poison_after_unpin;
static int poison_at_final_enable;
static int task_poisoned;
static int retired_probe_result;
static unsigned int retired_probe_calls;
static unsigned int remove_self_calls;
static unsigned int fail_closed_calls;
static int remove_handoff_armed;
static int fail_closed_handoff_armed;
static unsigned int setexcept_return_count;
static jmp_buf retirement_handoff;
static int current_hart;
static int queue_hart_plus1;
static int last_poke_hart;
static int last_poke_remote;
static int poke_count;
static struct Task *wait_pending[2];
static struct Task *mapped_task;
static void *task_page;
static size_t task_page_size;
static struct p4_signal_effect last_core_effect;
static uint32_t last_received;
static uint32_t last_wait;
static uint32_t last_except;
static uint8_t last_flags;
static ULONG last_state;

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
        return 0; \
    } \
} while (0)

static void event(enum event event_value)
{
    if (event_count < sizeof(events) / sizeof(events[0]))
        events[event_count++] = event_value;
    else
        mock_failed = 1;
}

static void poison_task(void)
{
    if (mapped_task == NULL || task_page == NULL)
    {
        mock_failed = 1;
        return;
    }
    if (mprotect(task_page, task_page_size, PROT_NONE) != 0)
        mock_failed = 1;
    else
        task_poisoned = 1;
}

int krnP4SelfRemoveCurrentRetired(struct ExecBase *sys_base)
{
    REQUIRE(sys_base == &test_sysbase);
    REQUIRE(disable_depth == 1);
    REQUIRE(raw_mie == 0);
    REQUIRE(!task_lock_held);
    REQUIRE(!queue_lock_held);
    event(EV_CURRENT_RETIRED);
    ++retired_probe_calls;
    return retired_probe_result;
}

void Exec_P4RemoveSelf(struct Task *task, struct ExecBase *sys_base)
{
    REQUIRE(task == mapped_task);
    REQUIRE(sys_base == &test_sysbase);
    REQUIRE(disable_depth == 1);
    REQUIRE(raw_mie == 0);
    REQUIRE(!task_lock_held);
    REQUIRE(!queue_lock_held);
    event(EV_REMOVE_SELF);
    ++remove_self_calls;
    if (!remove_handoff_armed)
        abort();
    remove_handoff_armed = 0;
    poison_task();
    longjmp(retirement_handoff, 1);
}

static int event_index(enum event wanted)
{
    size_t i;

    for (i = 0; i < event_count; ++i)
    {
        if (events[i] == wanted)
            return (int)i;
    }
    return -1;
}

static int event_before(enum event first, enum event second)
{
    int first_index = event_index(first);
    int second_index = event_index(second);

    return first_index >= 0 && second_index >= 0 &&
        first_index < second_index;
}

static int fixture_init(ULONG state, uint32_t owner_hart)
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
    mapped_task->owner_hart = owner_hart;
    current_task = mapped_task;
    memset(events, 0, sizeof(events));
    memset(wait_pending, 0, sizeof(wait_pending));
    event_count = 0;
    disable_depth = 0;
    raw_mie = 1;
    pin_count = 0;
    task_lock_held = 0;
    queue_lock_held = 0;
    pin_fails = 0;
    retire_at_task_lock = 0;
    poison_after_unpin = 1;
    poison_at_final_enable = 0;
    task_poisoned = 0;
    retired_probe_result = 0;
    retired_probe_calls = 0;
    remove_self_calls = 0;
    fail_closed_calls = 0;
    remove_handoff_armed = 0;
    fail_closed_handoff_armed = 0;
    setexcept_return_count = 0;
    current_hart = 0;
    queue_hart_plus1 = 0;
    last_poke_hart = -1;
    last_poke_remote = -1;
    poke_count = 0;
    mock_failed = 0;
    memset(&last_core_effect, 0, sizeof(last_core_effect));
    last_received = 0;
    last_wait = 0;
    last_except = 0;
    last_flags = 0;
    last_state = 0;
    return 1;
}

static void fixture_destroy(void)
{
    if (task_page != NULL)
    {
        (void)mprotect(task_page, task_page_size,
            PROT_READ | PROT_WRITE);
        (void)munmap(task_page, task_page_size);
    }
    task_page = NULL;
    mapped_task = NULL;
    current_task = NULL;
}

void Disable(void)
{
    ++disable_depth;
    raw_mie = 0;
    event(EV_DISABLE);
}

void Enable(void)
{
    REQUIRE(disable_depth != 0);
    if (disable_depth != 0)
        --disable_depth;
    event(EV_ENABLE);
    if (disable_depth == 0)
    {
        raw_mie = 1;
        if (poison_at_final_enable)
            poison_task();
    }
}

int krnP4SignalTaskPin(struct Task *task, struct ExecBase *sys_base)
{
    REQUIRE(sys_base == &test_sysbase);
    REQUIRE(disable_depth == 1);
    REQUIRE(!task_lock_held);
    REQUIRE(task == mapped_task);
    event(EV_PIN);
    if (pin_fails)
        return 0;
    ++pin_count;
    return 1;
}

void krnP4SignalTaskUnpin(struct Task *task, struct ExecBase *sys_base)
{
    REQUIRE(sys_base == &test_sysbase);
    REQUIRE(task == mapped_task);
    REQUIRE(disable_depth == 1);
    REQUIRE(!task_lock_held);
    REQUIRE(pin_count == 1);
    event(EV_UNPIN);
    if (pin_count != 0)
        --pin_count;
    if (poison_after_unpin)
        poison_task();
}

void p4_signal_adapter_lock_disabled(struct Task *task)
{
    REQUIRE(task == mapped_task);
    REQUIRE(disable_depth == 1);
    REQUIRE(!task_lock_held);
    task_lock_held = 1;
    event(EV_LOCK);
    if (retire_at_task_lock)
    {
        event(EV_RETIRE);
        task->tc_State = TS_REMOVED;
        retire_at_task_lock = 0;
    }
}

void p4_signal_adapter_unlock_disabled(struct Task *task)
{
    REQUIRE(task == mapped_task);
    REQUIRE(disable_depth == 1);
    REQUIRE(task_lock_held);
    event(EV_UNLOCK);
    task_lock_held = 0;
}

static enum p4_signal_owner_state owner_for(ULONG state)
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

static void snapshot_task(const struct Task *task)
{
    last_received = task->tc_SigRecvd;
    last_wait = task->tc_SigWait;
    last_except = task->tc_SigExcept;
    last_flags = task->tc_Flags;
    last_state = task->tc_State;
}

struct p4_signal_effect p4_signal_adapter_raise_locked(
    struct Task *task,
    struct ExecBase *sys_base,
    uint32_t signal_set)
{
    struct p4_signal_task_state state;
    int wait_armed = 0;

    REQUIRE(task == mapped_task);
    REQUIRE(sys_base == &test_sysbase);
    REQUIRE(disable_depth == 1);
    REQUIRE(task_lock_held);
    event(EV_RAISE);

    state.sig_received = task->tc_SigRecvd;
    state.sig_wait = task->tc_SigWait;
    state.sig_except = task->tc_SigExcept;
    state.flags = task->tc_Flags;
    state.exception_flag_mask = TF_EXCEPT;
    state.owner = owner_for(task->tc_State);
    state.owner_hart = task->owner_hart;
    if (state.owner == P4_SIGNAL_OWNER_RUNNING)
        wait_armed = wait_pending[task->owner_hart] == task;

    last_core_effect = p4_signal_raise_locked(
        &state, wait_armed, signal_set, (uint32_t)current_hart);
    if (last_core_effect.status == P4_SIGNAL_STATUS_OK)
    {
        task->tc_SigRecvd = state.sig_received;
        task->tc_SigWait = state.sig_wait;
        task->tc_SigExcept = state.sig_except;
        task->tc_Flags = state.flags;
    }
    snapshot_task(task);
    return last_core_effect;
}

struct p4_signal_effect p4_signal_adapter_set_except_locked(
    struct Task *task,
    struct ExecBase *sys_base,
    uint32_t signal_mask,
    uint32_t new_exceptions)
{
    struct p4_signal_task_state state;

    REQUIRE(task == mapped_task);
    REQUIRE(sys_base == &test_sysbase);
    REQUIRE(disable_depth == 1);
    REQUIRE(task_lock_held);
    event(EV_SET_EXCEPT);

    state.sig_received = task->tc_SigRecvd;
    state.sig_wait = task->tc_SigWait;
    state.sig_except = task->tc_SigExcept;
    state.flags = task->tc_Flags;
    state.exception_flag_mask = TF_EXCEPT;
    state.owner = owner_for(task->tc_State);
    state.owner_hart = task->owner_hart;
    last_core_effect = p4_signal_set_except_locked(
        &state, signal_mask, new_exceptions, (uint32_t)current_hart);
    if (last_core_effect.status == P4_SIGNAL_STATUS_OK)
    {
        task->tc_SigRecvd = state.sig_received;
        task->tc_SigWait = state.sig_wait;
        task->tc_SigExcept = state.sig_except;
        task->tc_Flags = state.flags;
    }
    snapshot_task(task);
    return last_core_effect;
}

int p4_signal_adapter_apply_queue_locked(
    struct Task *task,
    struct ExecBase *sys_base,
    struct p4_signal_effect *effect)
{
    REQUIRE(task == mapped_task);
    REQUIRE(sys_base == &test_sysbase);
    REQUIRE(disable_depth == 1);
    REQUIRE(task_lock_held);
    REQUIRE(!queue_lock_held);
    event(EV_QUEUE_APPLY);
    if (effect == NULL || effect->queue_transition !=
        P4_SIGNAL_QUEUE_WAITING_TO_READY || task->tc_State != TS_WAIT)
    {
        if (effect != NULL)
            effect->status = P4_SIGNAL_STATUS_INVALID_STATE;
        return 0;
    }

    queue_lock_held = 1;
    task->tc_State = TS_READY;
    queue_lock_held = 0;
    snapshot_task(task);
    if (queue_hart_plus1 > 0)
    {
        effect->request_reschedule = 1;
        effect->reschedule_hart = (uint32_t)(queue_hart_plus1 - 1);
        effect->remote_reschedule =
            (uint8_t)(effect->reschedule_hart != (uint32_t)current_hart);
    }
    return 1;
}

void krnP4SignalPoke(
    const struct p4_signal_effect *effect,
    struct ExecBase *sys_base)
{
    REQUIRE(effect != NULL);
    REQUIRE(sys_base == &test_sysbase);
    REQUIRE(disable_depth == 1);
    REQUIRE(!task_lock_held);
    REQUIRE(!queue_lock_held);
    REQUIRE(pin_count == 0);
    REQUIRE(effect->request_reschedule != 0);
    event(EV_POKE);
    ++poke_count;
    last_poke_hart = (int)effect->reschedule_hart;
    last_poke_remote = (int)effect->remote_reschedule;
}

void p4_signal_adapter_poke_after_unlock(
    const struct p4_signal_effect *effect,
    struct ExecBase *sys_base)
{
    if (effect != NULL && effect->status == P4_SIGNAL_STATUS_OK &&
        effect->request_reschedule)
        krnP4SignalPoke(effect, sys_base);
}

void p4_signal_adapter_fail_closed(void)
{
    ++fail_closed_calls;
    event(EV_FAIL_CLOSED);
    REQUIRE(disable_depth == 1);
    REQUIRE(raw_mie == 0);
    REQUIRE(!task_lock_held);
    REQUIRE(!queue_lock_held);
    if (fail_closed_handoff_armed)
    {
        fail_closed_handoff_armed = 0;
        poison_task();
        longjmp(retirement_handoff, 2);
    }
    fprintf(stderr, "unexpected fail-closed path in host fixture\n");
    abort();
}

static int test_pin_failure_balanced_noop(void)
{
    ULONG result;

    CHECK(fixture_init(TS_RUN, 0));
    CHECK(mprotect(task_page, task_page_size, PROT_NONE) == 0);
    task_poisoned = 1;
    pin_fails = 1;
    Signal(mapped_task, UINT32_C(0x01), &test_sysbase);
    CHECK(!mock_failed);
    CHECK(event_count == 3);
    CHECK(events[0] == EV_DISABLE && events[1] == EV_PIN &&
        events[2] == EV_ENABLE);
    CHECK(disable_depth == 0 && pin_count == 0);
    CHECK(!task_lock_held && poke_count == 0);
    result = (ULONG)event_count;
    fixture_destroy();
    return result == 3U;
}

static int test_running_signal_local_target(void)
{
    CHECK(fixture_init(TS_RUN, 0));
    mapped_task->tc_SigWait = UINT32_C(0x04);
    wait_pending[0] = mapped_task;
    Signal(mapped_task, UINT32_C(0x04), &test_sysbase);
    CHECK(!mock_failed);
    CHECK(last_core_effect.status == P4_SIGNAL_STATUS_OK);
    CHECK(last_received == UINT32_C(0x04));
    CHECK(last_core_effect.request_reschedule == 1);
    CHECK(last_core_effect.reschedule_hart == 0);
    CHECK(last_core_effect.remote_reschedule == 0);
    CHECK(poke_count == 1 && last_poke_hart == 0 && last_poke_remote == 0);
    CHECK(task_poisoned && disable_depth == 0 && pin_count == 0);
    CHECK(event_before(EV_DISABLE, EV_PIN));
    CHECK(event_before(EV_PIN, EV_LOCK));
    CHECK(event_before(EV_LOCK, EV_RAISE));
    CHECK(event_before(EV_RAISE, EV_UNLOCK));
    CHECK(event_before(EV_UNLOCK, EV_UNPIN));
    CHECK(event_before(EV_UNPIN, EV_POKE));
    CHECK(event_before(EV_POKE, EV_ENABLE));
    fixture_destroy();
    return 1;
}

static int test_running_signal_remote_target(void)
{
    CHECK(fixture_init(TS_RUN, 1));
    mapped_task->tc_SigWait = UINT32_C(0x08);
    wait_pending[1] = mapped_task;
    Signal(mapped_task, UINT32_C(0x08), &test_sysbase);
    CHECK(!mock_failed);
    CHECK(last_core_effect.request_reschedule == 1);
    CHECK(last_core_effect.reschedule_hart == 1);
    CHECK(last_core_effect.remote_reschedule == 1);
    CHECK(poke_count == 1 && last_poke_hart == 1 && last_poke_remote == 1);
    CHECK(event_before(EV_UNPIN, EV_POKE));
    CHECK(event_before(EV_POKE, EV_ENABLE));
    CHECK(task_poisoned);
    fixture_destroy();
    return 1;
}

static int test_waiting_signal_queue_then_remote_poke(void)
{
    CHECK(fixture_init(TS_WAIT, 0));
    mapped_task->tc_SigWait = UINT32_C(0x10);
    queue_hart_plus1 = 2;
    Signal(mapped_task, UINT32_C(0x10), &test_sysbase);
    CHECK(!mock_failed);
    CHECK(last_core_effect.queue_transition ==
        P4_SIGNAL_QUEUE_WAITING_TO_READY);
    CHECK(last_state == TS_READY);
    CHECK(poke_count == 1 && last_poke_hart == 1 && last_poke_remote == 1);
    CHECK(event_before(EV_RAISE, EV_QUEUE_APPLY));
    CHECK(event_before(EV_QUEUE_APPLY, EV_UNLOCK));
    CHECK(event_before(EV_UNLOCK, EV_UNPIN));
    CHECK(event_before(EV_UNPIN, EV_POKE));
    CHECK(event_before(EV_POKE, EV_ENABLE));
    CHECK(task_poisoned);
    fixture_destroy();
    return 1;
}

static int test_postpin_retirement_is_quiet(void)
{
    CHECK(fixture_init(TS_RUN, 0));
    retire_at_task_lock = 1;
    Signal(mapped_task, UINT32_C(0x20), &test_sysbase);
    CHECK(!mock_failed);
    CHECK(last_core_effect.status == P4_SIGNAL_STATUS_NOT_LIVE);
    CHECK(last_received == 0);
    CHECK(poke_count == 0);
    CHECK(event_before(EV_PIN, EV_RETIRE));
    CHECK(event_before(EV_RETIRE, EV_RAISE));
    CHECK(event_before(EV_RAISE, EV_UNLOCK));
    CHECK(event_before(EV_UNLOCK, EV_UNPIN));
    CHECK(task_poisoned && disable_depth == 0 && pin_count == 0);
    fixture_destroy();
    return 1;
}

static int test_setexcept_pending_exception_before_enable(void)
{
    ULONG old_mask;

    CHECK(fixture_init(TS_RUN, 0));
    mapped_task->tc_SigRecvd = UINT32_C(0x40);
    poison_at_final_enable = 1;
    poison_after_unpin = 0;
    old_mask = SetExcept(UINT32_C(0x40), UINT32_C(0x40), &test_sysbase);
    CHECK(!mock_failed);
    CHECK(old_mask == 0);
    CHECK(last_except == UINT32_C(0x40));
    CHECK((last_flags & TF_EXCEPT) != 0);
    CHECK(last_core_effect.request_reschedule == 1);
    CHECK(last_core_effect.reschedule_hart == 0);
    CHECK(poke_count == 1 && last_poke_hart == 0 && last_poke_remote == 0);
    CHECK(event_before(EV_DISABLE, EV_LOCK));
    CHECK(event_before(EV_LOCK, EV_SET_EXCEPT));
    CHECK(event_before(EV_SET_EXCEPT, EV_UNLOCK));
    CHECK(event_before(EV_UNLOCK, EV_POKE));
    CHECK(event_before(EV_POKE, EV_ENABLE));
    CHECK(task_poisoned && disable_depth == 0);
    fixture_destroy();
    return 1;
}

static int test_setexcept_retired_current_transfers(void)
{
    int handoff;

    CHECK(fixture_init(TS_RUN, 0));
    retire_at_task_lock = 1;
    retired_probe_result = 1;
    remove_handoff_armed = 1;
    handoff = setjmp(retirement_handoff);
    if (handoff == 0)
    {
        (void)SetExcept(UINT32_C(0x01), UINT32_C(0x01), &test_sysbase);
        ++setexcept_return_count;
    }

    CHECK(handoff == 1);
    CHECK(setexcept_return_count == 0);
    CHECK(last_core_effect.status == P4_SIGNAL_STATUS_NOT_LIVE);
    CHECK(last_state == TS_REMOVED);
    CHECK(retired_probe_calls == 1 && remove_self_calls == 1);
    CHECK(fail_closed_calls == 0);
    CHECK(event_before(EV_UNLOCK, EV_CURRENT_RETIRED));
    CHECK(event_before(EV_CURRENT_RETIRED, EV_REMOVE_SELF));
    CHECK(event_index(EV_POKE) < 0 && event_index(EV_ENABLE) < 0);
    CHECK(task_poisoned);
    CHECK(disable_depth == 1 && raw_mie == 0);
    CHECK(!task_lock_held && !queue_lock_held && pin_count == 0);
    CHECK(!mock_failed);
    fixture_destroy();
    return 1;
}

static int test_setexcept_nonretired_failure_stays_failclosed(void)
{
    int handoff;

    CHECK(fixture_init(TS_RUN, 0));
    retire_at_task_lock = 1;
    retired_probe_result = 0;
    fail_closed_handoff_armed = 1;
    handoff = setjmp(retirement_handoff);
    if (handoff == 0)
    {
        (void)SetExcept(UINT32_C(0x01), UINT32_C(0x01), &test_sysbase);
        ++setexcept_return_count;
    }

    CHECK(handoff == 2);
    CHECK(setexcept_return_count == 0);
    CHECK(last_core_effect.status == P4_SIGNAL_STATUS_NOT_LIVE);
    CHECK(last_state == TS_REMOVED);
    CHECK(retired_probe_calls == 1 && remove_self_calls == 0);
    CHECK(fail_closed_calls == 1);
    CHECK(event_before(EV_UNLOCK, EV_CURRENT_RETIRED));
    CHECK(event_before(EV_CURRENT_RETIRED, EV_FAIL_CLOSED));
    CHECK(event_index(EV_POKE) < 0 && event_index(EV_ENABLE) < 0);
    CHECK(task_poisoned);
    CHECK(disable_depth == 1 && raw_mie == 0);
    CHECK(!task_lock_held && !queue_lock_held && pin_count == 0);
    CHECK(!mock_failed);
    fixture_destroy();
    return 1;
}

int main(void)
{
    int ok = 1;

    ok &= test_pin_failure_balanced_noop();
    ok &= test_running_signal_local_target();
    ok &= test_running_signal_remote_target();
    ok &= test_waiting_signal_queue_then_remote_poke();
    ok &= test_postpin_retirement_is_quiet();
    ok &= test_setexcept_pending_exception_before_enable();
    ok &= test_setexcept_retired_current_transfers();
    ok &= test_setexcept_nonretired_failure_stays_failclosed();
    if (!ok || mock_failed)
        return 1;
    printf("P4 Signal/SetExcept actual-body wake-order fixture: %u checks passed\n",
        checks);
    return 0;
}
