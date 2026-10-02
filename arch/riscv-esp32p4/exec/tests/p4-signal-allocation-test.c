/*
 * Host fixture for actual P4 AllocSignal()/AllocTaskSignal()/FreeSignal().
 * Only Exec ABI macros, Task storage, and the disabled Task-lock boundary are
 * mocked. The production allocation/free bodies are included verbatim.
 */
#define _DARWIN_C_SOURCE 1
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

typedef int8_t BYTE;
typedef int32_t LONG;
typedef uint32_t ULONG;

struct Task
{
    uint32_t tc_SigAlloc;
    uint32_t tc_SigExcept;
    uint32_t tc_SigWait;
    uint32_t tc_SigRecvd;
    uint32_t tc_SpinLock;
    uint32_t tc_State;
    void *tc_UnionETask;
    uint32_t fixture_registered;
};

struct ExecBase
{
    unsigned int marker;
};

#define AROS_LHA(type, name, reg) type name
#define AROS_LH1(ret, name, arg1, base_type, base_name, offset, lib) \
    ret name(arg1, base_type base_name __attribute__((unused)))
#define AROS_LIBFUNC_INIT
#define AROS_LIBFUNC_EXIT

static struct Task *current_task;
static struct ExecBase test_sysbase;
static struct Task *mapped_task;
static void *task_page;
static size_t task_page_size;

static struct Task *mock_get_this_task(void);
#define GET_THIS_TASK mock_get_this_task()

LONG AllocTaskSignal(struct Task *thisTask, LONG signalNum,
                     struct ExecBase *SysBase);
void Disable(void);
void Enable(void);
void p4_signal_adapter_lock_disabled(struct Task *task);
void p4_signal_adapter_unlock_disabled(struct Task *task);

#define P4_SIGNAL_ALLOCATION_TEST 1
#include "../allocsignal.c"
#include "../freesignal.c"

enum event
{
    EV_DISABLE = 1,
    EV_LOCK,
    EV_UNLOCK,
    EV_ENABLE
};

static unsigned int checks;
static int mock_failed;
static int ok;
static enum event events[8];
static size_t event_count;
static unsigned int disable_depth;
static int raw_mie;
static int saved_mie;
static int task_lock_held;
static int poison_at_enable;
static int task_poisoned;
static unsigned int get_task_calls;
static unsigned int public_lock_calls;
static unsigned int registry_calls;
static unsigned int pin_calls;
static unsigned int etask_accesses;

static uint32_t snapshot_alloc;
static uint32_t snapshot_except;
static uint32_t snapshot_wait;
static uint32_t snapshot_received;
static uint32_t snapshot_state;
static uint32_t snapshot_lockword;
static void *snapshot_etask;
static uint32_t snapshot_registered;

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

static void reset_events(void)
{
    memset(events, 0, sizeof(events));
    event_count = 0;
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

static const enum event locked_operation[] = {
    EV_DISABLE, EV_LOCK, EV_UNLOCK, EV_ENABLE
};

static int fixture_init(int initial_mie)
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
    mapped_task->tc_SigAlloc = 0;
    mapped_task->tc_SigExcept = UINT32_C(0xa5a5a5a5);
    mapped_task->tc_SigWait = UINT32_C(0x5a5a5a5a);
    mapped_task->tc_SigRecvd = UINT32_C(0xdeadbeef);
    mapped_task->tc_SpinLock = UINT32_C(0x51a1c0de);
    mapped_task->tc_State = UINT32_C(0xfeed1234);
    mapped_task->tc_UnionETask = NULL;
    mapped_task->fixture_registered = 0;
    current_task = mapped_task;

    reset_events();
    disable_depth = 0;
    raw_mie = initial_mie;
    saved_mie = initial_mie;
    task_lock_held = 0;
    poison_at_enable = 0;
    task_poisoned = 0;
    get_task_calls = 0;
    public_lock_calls = 0;
    registry_calls = 0;
    pin_calls = 0;
    etask_accesses = 0;
    snapshot_alloc = 0;
    snapshot_except = 0;
    snapshot_wait = 0;
    snapshot_received = 0;
    snapshot_state = 0;
    snapshot_lockword = 0;
    snapshot_etask = (void *)(uintptr_t)1U;
    snapshot_registered = UINT32_MAX;
    mock_failed = 0;
    ok = 1;
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

static struct Task *mock_get_this_task(void)
{
    ++get_task_calls;
    return current_task;
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
    if (poison_at_enable)
        poison_task_page();
}

void p4_signal_adapter_lock_disabled(struct Task *task)
{
    REQUIRE(task == mapped_task);
    REQUIRE(disable_depth == 1);
    REQUIRE(raw_mie == 0);
    REQUIRE(!task_lock_held);
    REQUIRE(task->tc_SpinLock == UINT32_C(0x51a1c0de));
    /* Constructor-time allocation is allowed before registry publication. */
    REQUIRE(task->fixture_registered == 0);
    REQUIRE(task->tc_UnionETask == NULL);
    ++public_lock_calls;
    task_lock_held = 1;
    record_event(EV_LOCK);
}

void p4_signal_adapter_unlock_disabled(struct Task *task)
{
    REQUIRE(task == mapped_task);
    REQUIRE(disable_depth == 1);
    REQUIRE(raw_mie == 0);
    REQUIRE(task_lock_held);
    snapshot_alloc = task->tc_SigAlloc;
    snapshot_except = task->tc_SigExcept;
    snapshot_wait = task->tc_SigWait;
    snapshot_received = task->tc_SigRecvd;
    snapshot_state = task->tc_State;
    snapshot_lockword = task->tc_SpinLock;
    snapshot_etask = task->tc_UnionETask;
    snapshot_registered = task->fixture_registered;
    task_lock_held = 0;
    record_event(EV_UNLOCK);
}

static int check_locked_boundary(void)
{
    return event_sequence_is(locked_operation,
        sizeof(locked_operation) / sizeof(locked_operation[0])) &&
        disable_depth == 0 && !task_lock_held &&
        public_lock_calls != 0 && raw_mie >= 0 && raw_mie <= 1 &&
        registry_calls == 0 && pin_calls == 0 && etask_accesses == 0;
}

static int test_explicit_all_32(void)
{
    unsigned int bit;

    CHECK(fixture_init(1));
    mapped_task->tc_SigExcept = UINT32_MAX;
    mapped_task->tc_SigWait = UINT32_MAX;
    mapped_task->tc_SigRecvd = UINT32_MAX;
    for (bit = 0; bit < 32U; ++bit)
    {
        uint32_t bit_mask = UINT32_C(1) << bit;
        uint32_t expected_remaining = UINT32_MAX & ~(bit_mask | (bit_mask - 1U));
        LONG result;

        raw_mie = (bit & 1U) == 0U ? 1 : 0;
        reset_events();
        result = AllocTaskSignal(mapped_task, (LONG)bit, &test_sysbase);
        CHECK(result == (LONG)bit);
        CHECK(snapshot_alloc == ((bit_mask - 1U) | bit_mask));
        CHECK(snapshot_except == expected_remaining);
        CHECK(snapshot_wait == expected_remaining);
        CHECK(snapshot_received == expected_remaining);
        CHECK(snapshot_state == UINT32_C(0xfeed1234));
        CHECK(snapshot_lockword == UINT32_C(0x51a1c0de));
        CHECK(snapshot_etask == NULL && snapshot_registered == 0);
        CHECK(event_sequence_is(locked_operation,
            sizeof(locked_operation) / sizeof(locked_operation[0])));
        CHECK(disable_depth == 0 && !task_lock_held &&
            raw_mie == ((bit & 1U) == 0U ? 1 : 0));
        CHECK(!mock_failed);
    }
    CHECK(snapshot_alloc == UINT32_MAX);
    CHECK(public_lock_calls == 32U);
    CHECK(registry_calls == 0 && pin_calls == 0 && etask_accesses == 0);
    fixture_destroy();
    return ok;
}

static int test_anybit_exhaustion_and_reuse(void)
{
    LONG result;
    uint32_t saved_except;
    uint32_t saved_wait;
    uint32_t saved_received;

    CHECK(fixture_init(0));
    mapped_task->tc_SigAlloc = UINT32_MAX;
    saved_except = mapped_task->tc_SigExcept;
    saved_wait = mapped_task->tc_SigWait;
    saved_received = mapped_task->tc_SigRecvd;
    reset_events();
    result = AllocTaskSignal(mapped_task, -1, &test_sysbase);
    CHECK(result == -1);
    CHECK(snapshot_alloc == UINT32_MAX);
    CHECK(snapshot_except == saved_except && snapshot_wait == saved_wait &&
        snapshot_received == saved_received);
    CHECK(check_locked_boundary());

    reset_events();
    FreeSignal(0, &test_sysbase);
    CHECK(snapshot_alloc == UINT32_MAX - 1U);
    CHECK(snapshot_except == saved_except && snapshot_wait == saved_wait &&
        snapshot_received == saved_received);
    CHECK(check_locked_boundary());

    reset_events();
    result = AllocTaskSignal(mapped_task, -7, &test_sysbase);
    CHECK(result == 0);
    CHECK(snapshot_alloc == UINT32_MAX);
    CHECK(snapshot_except == (saved_except & ~UINT32_C(1)));
    CHECK(snapshot_wait == (saved_wait & ~UINT32_C(1)));
    CHECK(snapshot_received == (saved_received & ~UINT32_C(1)));
    CHECK(check_locked_boundary());

    /* FreeSignal only clears allocation; subsequent explicit allocation reuses it. */
    saved_except = snapshot_except;
    saved_wait = snapshot_wait;
    saved_received = snapshot_received;
    reset_events();
    FreeSignal(17, &test_sysbase);
    CHECK(snapshot_alloc == (UINT32_MAX & ~(UINT32_C(1) << 17)));
    CHECK(snapshot_except == saved_except && snapshot_wait == saved_wait &&
        snapshot_received == saved_received);
    CHECK(check_locked_boundary());
    reset_events();
    result = AllocTaskSignal(mapped_task, 17, &test_sysbase);
    CHECK(result == 17);
    CHECK(snapshot_alloc == UINT32_MAX);
    CHECK((snapshot_except & (UINT32_C(1) << 17)) == 0);
    CHECK((snapshot_wait & (UINT32_C(1) << 17)) == 0);
    CHECK((snapshot_received & (UINT32_C(1) << 17)) == 0);
    CHECK(check_locked_boundary());
    CHECK(!mock_failed);
    fixture_destroy();
    return ok;
}

static int test_internal_invalid_numbers_and_public_byte_abi(void)
{
    LONG result;
    BYTE public_result;
    uint32_t before_alloc;
    uint32_t before_except;
    uint32_t before_wait;
    uint32_t before_received;

    CHECK(fixture_init(1));
    mapped_task->tc_SigAlloc = UINT32_C(0x0000ffff);
    before_alloc = mapped_task->tc_SigAlloc;
    before_except = mapped_task->tc_SigExcept;
    before_wait = mapped_task->tc_SigWait;
    before_received = mapped_task->tc_SigRecvd;

    reset_events();
    result = AllocTaskSignal(mapped_task, 7, &test_sysbase);
    CHECK(result == -1);
    CHECK(snapshot_alloc == before_alloc && snapshot_except == before_except &&
        snapshot_wait == before_wait && snapshot_received == before_received);
    CHECK(check_locked_boundary());

    reset_events();
    result = AllocTaskSignal(mapped_task, 32, &test_sysbase);
    CHECK(result == -1);
    CHECK(snapshot_alloc == before_alloc && snapshot_except == before_except &&
        snapshot_wait == before_wait && snapshot_received == before_received);
    CHECK(check_locked_boundary());
    reset_events();
    result = AllocTaskSignal(mapped_task, INT32_MAX, &test_sysbase);
    CHECK(result == -1);
    CHECK(snapshot_alloc == before_alloc);
    CHECK(check_locked_boundary());

    /* 0xff and 0x80 become negative signed BYTE values, hence any-bit. */
    mapped_task->tc_SigAlloc = UINT32_MAX & ~UINT32_C(1);
    reset_events();
    public_result = AllocSignal(INT32_C(255), &test_sysbase);
    CHECK(public_result == 0);
    CHECK(snapshot_alloc == UINT32_MAX);
    CHECK(get_task_calls == 1);
    CHECK(check_locked_boundary());

    mapped_task->tc_SigAlloc = UINT32_MAX & ~(UINT32_C(1) << 31);
    reset_events();
    public_result = AllocSignal(INT32_C(128), &test_sysbase);
    CHECK(public_result == 31);
    CHECK(snapshot_alloc == UINT32_MAX);
    CHECK(check_locked_boundary());

    /* A positive BYTE value >= 32 is refused by AllocTaskSignal. */
    mapped_task->tc_SigAlloc = 0;
    reset_events();
    public_result = AllocSignal(INT32_C(32), &test_sysbase);
    CHECK(public_result == (BYTE)-1);
    CHECK(snapshot_alloc == 0);
    CHECK(check_locked_boundary());

    /* ABI conversion uses the low byte: 0x100 narrows to explicit signal 0. */
    reset_events();
    public_result = AllocSignal(INT32_C(256), &test_sysbase);
    CHECK(public_result == 0);
    CHECK(snapshot_alloc == UINT32_C(1));
    CHECK(check_locked_boundary());
    CHECK(!mock_failed);
    fixture_destroy();
    return ok;
}

static int test_public_exhaustion_and_final_enable_poison(void)
{
    BYTE public_result;

    CHECK(fixture_init(1));
    mapped_task->tc_SigAlloc = UINT32_MAX;
    poison_at_enable = 1;
    reset_events();
    public_result = AllocSignal(-1, &test_sysbase);
    CHECK(public_result == (BYTE)-1);
    CHECK(task_poisoned);
    CHECK(get_task_calls == 1);
    CHECK(event_sequence_is(locked_operation,
        sizeof(locked_operation) / sizeof(locked_operation[0])));
    CHECK(disable_depth == 0 && raw_mie == 1 && !task_lock_held);
    CHECK(!mock_failed);
    fixture_destroy();
    return ok;
}

static int test_free_signal_noop_range_and_poison(void)
{
    int32_t invalid_values[] = { -1, -2, 32, 33, INT32_MAX };
    size_t index;
    uint32_t saved_except;
    uint32_t saved_wait;
    uint32_t saved_received;

    CHECK(fixture_init(1));
    mapped_task->tc_SigAlloc = UINT32_MAX;
    saved_except = mapped_task->tc_SigExcept;
    saved_wait = mapped_task->tc_SigWait;
    saved_received = mapped_task->tc_SigRecvd;
    for (index = 0; index < sizeof(invalid_values) / sizeof(invalid_values[0]);
         ++index)
    {
        reset_events();
        get_task_calls = 0;
        FreeSignal((LONG)invalid_values[index], &test_sysbase);
        CHECK(event_count == 0 && get_task_calls == 0);
        CHECK(mapped_task->tc_SigAlloc == UINT32_MAX);
        CHECK(mapped_task->tc_SigExcept == saved_except &&
            mapped_task->tc_SigWait == saved_wait &&
            mapped_task->tc_SigRecvd == saved_received);
    }

    poison_at_enable = 1;
    reset_events();
    get_task_calls = 0;
    FreeSignal(31, &test_sysbase);
    CHECK(task_poisoned);
    CHECK(get_task_calls == 1);
    CHECK(snapshot_alloc == (UINT32_MAX & ~(UINT32_C(1) << 31)));
    CHECK(snapshot_except == saved_except && snapshot_wait == saved_wait &&
        snapshot_received == saved_received);
    CHECK(event_sequence_is(locked_operation,
        sizeof(locked_operation) / sizeof(locked_operation[0])));
    CHECK(disable_depth == 0 && raw_mie == 1 && !task_lock_held);
    CHECK(!mock_failed);
    fixture_destroy();
    return ok;
}

static int test_free_signal_all_32_bits(void)
{
    unsigned int bit;
    uint32_t saved_except;
    uint32_t saved_wait;
    uint32_t saved_received;

    CHECK(fixture_init(1));
    mapped_task->tc_SigAlloc = UINT32_MAX;
    saved_except = mapped_task->tc_SigExcept;
    saved_wait = mapped_task->tc_SigWait;
    saved_received = mapped_task->tc_SigRecvd;
    for (bit = 0; bit < 32U; ++bit)
    {
        uint32_t bit_mask = UINT32_C(1) << bit;
        LONG signal_num = (LONG)bit;

        raw_mie = (bit & 1U) == 0U ? 1 : 0;
        reset_events();
        FreeSignal(signal_num, &test_sysbase);
        CHECK(snapshot_alloc == (UINT32_MAX & ~(bit_mask | (bit_mask - 1U))));
        CHECK(snapshot_except == saved_except && snapshot_wait == saved_wait &&
            snapshot_received == saved_received);
        CHECK(event_sequence_is(locked_operation,
            sizeof(locked_operation) / sizeof(locked_operation[0])));
        CHECK(disable_depth == 0 && !task_lock_held &&
            raw_mie == ((bit & 1U) == 0U ? 1 : 0));
        CHECK(!mock_failed);
    }
    CHECK(snapshot_alloc == 0);
    CHECK(public_lock_calls == 32U);
    CHECK(registry_calls == 0 && pin_calls == 0 && etask_accesses == 0);
    fixture_destroy();
    return ok;
}

static int test_null_task_behavior(void)
{
    LONG internal_result;
    BYTE public_result;

    CHECK(fixture_init(1));
    current_task = NULL;
    get_task_calls = 0;
    internal_result = AllocTaskSignal(NULL, -1, &test_sysbase);
    CHECK(internal_result == -1);
    CHECK(event_count == 0 && public_lock_calls == 0);

    public_result = AllocSignal(0, &test_sysbase);
    CHECK(public_result == (BYTE)-1);
    CHECK(get_task_calls == 1);
    CHECK(event_count == 0 && public_lock_calls == 0);

    FreeSignal(0, &test_sysbase);
    CHECK(get_task_calls == 2);
    CHECK(event_count == 0 && public_lock_calls == 0);
    CHECK(disable_depth == 0 && raw_mie == 1 && !task_lock_held);
    CHECK(!mock_failed);
    fixture_destroy();
    return ok;
}

static int test_invalid_free_does_not_touch_poisoned_task(void)
{
    CHECK(fixture_init(0));
    CHECK(mprotect(task_page, task_page_size, PROT_NONE) == 0);
    task_poisoned = 1;
    current_task = mapped_task;
    get_task_calls = 0;
    reset_events();
    FreeSignal(32, &test_sysbase);
    FreeSignal(-1, &test_sysbase);
    CHECK(get_task_calls == 0 && event_count == 0);
    CHECK(!mock_failed);
    fixture_destroy();
    return ok;
}

int main(void)
{
    int passed = 1;

    passed &= test_explicit_all_32();
    passed &= test_anybit_exhaustion_and_reuse();
    passed &= test_internal_invalid_numbers_and_public_byte_abi();
    passed &= test_public_exhaustion_and_final_enable_poison();
    passed &= test_free_signal_noop_range_and_poison();
    passed &= test_free_signal_all_32_bits();
    passed &= test_null_task_behavior();
    passed &= test_invalid_free_does_not_touch_poisoned_task();
    if (!passed || mock_failed)
        return 1;

    printf("PASS p4-signal-allocation checks=%u\n", checks);
    return 0;
}
