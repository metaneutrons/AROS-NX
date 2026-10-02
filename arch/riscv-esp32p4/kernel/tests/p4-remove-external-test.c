/* Actual-body tests for the P4 external-removal admission wrapper. */
#define _DEFAULT_SOURCE 1

#include <errno.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define P4_REMOVE_EXTERNAL_TEST 1

struct Task
{
    uint32_t marker;
};

struct p4_exec_cpu_local
{
    uint32_t trap_depth;
};

struct p4_exec_runtime
{
    uint32_t hart_id;
};

struct ExecBase
{
    struct p4_exec_cpu_local cpu_local;
    struct p4_exec_runtime runtime;
};

static struct Task *mock_current_task;
#define GET_THIS_TASK (mock_current_task)
#define P4_EXEC_CPU_LOCAL(base) (&((struct ExecBase *)(base))->cpu_local)
#define P4_EXEC_RUNTIME(base) (&((struct ExecBase *)(base))->runtime)

#include "../../exec/p4_remove_wait.h"
#include "../../exec/p4_remove_external.h"

void Disable(void);
void Enable(void);
int krnP4TaskRecordAdmitRemoval(struct Task *, struct Task *, uint32_t,
                                uint32_t, struct p4_remove_handle *,
                                struct ExecBase *);
int krnP4SignalTaskPin(struct Task *, struct ExecBase *);
void krnP4SignalTaskUnpin(struct Task *, struct ExecBase *);
void p4_signal_adapter_lock_disabled(struct Task *);
void p4_signal_adapter_unlock_disabled(struct Task *);
int krnP4TaskRecordRunningOwner(struct Task *, struct ExecBase *);
int krnP4TaskRetireWithTicketLocked(struct Task *, struct Task *, uint32_t,
                                    struct p4_remove_handle *,
                                    struct ExecBase *);
void krnP4RetiredOwnerPoke(uint32_t, struct ExecBase *);
void krnP4CleanupPoke(struct ExecBase *);
void Exec_P4AwaitRemoval(struct Task *, struct p4_remove_handle,
                         struct ExecBase *);
int krnP4SelfRemoveCurrentRetired(struct ExecBase *);
void p4_signal_adapter_fail_closed(void) __attribute__((noreturn));

#include "../../exec/p4_remove_external.c"

#define CHECK(condition) \
    do \
    { \
        ++check_count; \
        if (!(condition)) \
        { \
            ++failure_count; \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                    #condition); \
        } \
    } while (0)

enum
{
    EVENT_CAPACITY = 64,
    ADMISSION_CAPACITY = 4
};

static unsigned int check_count;
static unsigned int failure_count;
static char events[EVENT_CAPACITY];
static size_t event_count;
static int irq_depth;
static int task_lock_held;
static int target_pin_held;
static int target_was_poisoned;
static int fatal_calls;
static int self_remove_calls;
static int await_calls;
static int pin_calls;
static int unpin_calls;
static int lock_calls;
static int unlock_calls;
static int owner_calls;
static int retire_calls;
static int owner_poke_calls;
static int cleanup_poke_calls;
static int self_retired_calls;
static int pin_result;
static int running_owner;
static int retire_result;
static int requester_retired;
static uint32_t current_hart;
static uint32_t last_poke_hart;
static int admission_results[ADMISSION_CAPACITY];
static size_t admission_count;
static size_t admission_index;
static struct Task *expected_requester;
static struct Task *expected_target;
static struct ExecBase mock_sysbase;
static struct p4_remove_handle expected_handle;
static void *target_mapping;
static size_t target_mapping_size;
static struct Task *target_task;
static jmp_buf transfer_env;

static void record_event(char event)
{
    if (event_count >= EVENT_CAPACITY)
    {
        ++failure_count;
        return;
    }
    events[event_count++] = event;
}

static void expect_events(const char *expected)
{
    size_t expected_len = strlen(expected);
    CHECK(event_count == expected_len);
    if (event_count != expected_len)
    {
        fprintf(stderr, "expected events: %s; actual: ", expected);
        (void)fwrite(events, 1u, event_count, stderr);
        fputc('\n', stderr);
    }
    else
    {
        int matched = memcmp(events, expected, expected_len) == 0;
        CHECK(matched != 0);
        if (matched == 0)
        {
            fprintf(stderr, "expected events: %s; actual: ", expected);
            (void)fwrite(events, 1u, event_count, stderr);
            fputc('\n', stderr);
        }
    }
}

static int target_protect_none(void)
{
    if (mprotect(target_mapping, target_mapping_size, PROT_NONE) != 0)
    {
        ++failure_count;
        perror("mprotect target PROT_NONE");
        return 0;
    }
    target_was_poisoned = 1;
    return 1;
}

static void release_target_mapping(void)
{
    if (target_mapping == NULL)
        return;
    if (target_was_poisoned != 0)
    {
        if (mprotect(target_mapping, target_mapping_size,
                     PROT_READ | PROT_WRITE) != 0)
        {
            ++failure_count;
            perror("mprotect restore target");
        }
    }
    if (munmap(target_mapping, target_mapping_size) != 0)
    {
        ++failure_count;
        perror("munmap target");
    }
    target_mapping = NULL;
    target_mapping_size = 0u;
    target_task = NULL;
    target_was_poisoned = 0;
}

static struct Task *allocate_target(int initially_protected)
{
    long page_size = sysconf(_SC_PAGESIZE);
    int protection = initially_protected != 0 ? PROT_NONE :
                                                (PROT_READ | PROT_WRITE);

    if (page_size <= 0)
    {
        ++failure_count;
        return NULL;
    }
    target_mapping_size = (size_t)page_size;
    target_mapping = mmap(NULL, target_mapping_size, protection,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (target_mapping == MAP_FAILED)
    {
        target_mapping = NULL;
        target_mapping_size = 0u;
        ++failure_count;
        perror("mmap target");
        return NULL;
    }
    target_task = (struct Task *)target_mapping;
    target_was_poisoned = initially_protected != 0;
    if (initially_protected == 0)
        target_task->marker = UINT32_C(0x51a7cafe);
    return target_task;
}

static void reset_case(uint32_t hart)
{
    release_target_mapping();
    memset(events, 0, sizeof(events));
    memset(admission_results, 0, sizeof(admission_results));
    memset(&mock_sysbase, 0, sizeof(mock_sysbase));
    event_count = 0u;
    irq_depth = 0;
    task_lock_held = 0;
    target_pin_held = 0;
    fatal_calls = 0;
    self_remove_calls = 0;
    await_calls = 0;
    pin_calls = 0;
    unpin_calls = 0;
    lock_calls = 0;
    unlock_calls = 0;
    owner_calls = 0;
    retire_calls = 0;
    owner_poke_calls = 0;
    cleanup_poke_calls = 0;
    self_retired_calls = 0;
    pin_result = 1;
    running_owner = 0;
    retire_result = 1;
    requester_retired = 0;
    current_hart = hart;
    last_poke_hart = UINT32_MAX;
    admission_count = 0u;
    admission_index = 0u;
    mock_current_task = expected_requester;
    mock_sysbase.runtime.hart_id = hart;
    mock_sysbase.cpu_local.trap_depth = 0u;
    expected_handle.index = 17u;
    expected_handle.generation = 91u;
}

static void set_admission_results(const int *results, size_t count)
{
    size_t i;
    CHECK(count <= ADMISSION_CAPACITY);
    if (count > ADMISSION_CAPACITY)
        return;
    for (i = 0u; i < count; ++i)
        admission_results[i] = results[i];
    admission_count = count;
}

void Disable(void)
{
    record_event('D');
    ++irq_depth;
    CHECK(irq_depth == 1);
}

void Enable(void)
{
    record_event('E');
    CHECK(irq_depth == 1);
    CHECK(task_lock_held == 0);
    CHECK(target_pin_held == 0);
    --irq_depth;
}

int krnP4TaskRecordAdmitRemoval(struct Task *requester,
                                struct Task *target,
                                uint32_t hart, uint32_t retire_live,
                                struct p4_remove_handle *handle,
                                struct ExecBase *base)
{
    int result;
    record_event('A');
    CHECK(base == &mock_sysbase);
    CHECK(requester == expected_requester);
    CHECK(target == expected_target);
    CHECK(hart == current_hart);
    CHECK(retire_live == 0u);
    CHECK(handle != NULL);
    CHECK(irq_depth == 1);
    if (admission_index >= admission_count)
    {
        ++failure_count;
        return -1;
    }
    result = admission_results[admission_index++];
    if (result == 1 || result == 2)
    {
        handle->index = expected_handle.index;
        handle->generation = expected_handle.generation;
    }
    return result;
}

int krnP4SignalTaskPin(struct Task *task, struct ExecBase *base)
{
    record_event('p');
    ++pin_calls;
    CHECK(base == &mock_sysbase);
    CHECK(task == expected_target);
    CHECK(irq_depth == 1);
    CHECK(task_lock_held == 0);
    if (pin_result != 0)
        target_pin_held = 1;
    return pin_result;
}

void p4_signal_adapter_lock_disabled(struct Task *task)
{
    record_event('L');
    ++lock_calls;
    CHECK(task == expected_target);
    CHECK(irq_depth == 1);
    CHECK(target_pin_held == 1);
    CHECK(task_lock_held == 0);
    task_lock_held = 1;
}

void p4_signal_adapter_unlock_disabled(struct Task *task)
{
    record_event('U');
    ++unlock_calls;
    CHECK(task == expected_target);
    CHECK(irq_depth == 1);
    CHECK(target_pin_held == 1);
    CHECK(task_lock_held == 1);
    task_lock_held = 0;
}

int krnP4TaskRecordRunningOwner(struct Task *task, struct ExecBase *base)
{
    record_event('O');
    ++owner_calls;
    CHECK(task == expected_target);
    CHECK(base == &mock_sysbase);
    CHECK(irq_depth == 1);
    CHECK(target_pin_held == 1);
    CHECK(task_lock_held == 1);
    return running_owner;
}

int krnP4TaskRetireWithTicketLocked(struct Task *task,
                                    struct Task *requester, uint32_t hart,
                                    struct p4_remove_handle *handle,
                                    struct ExecBase *base)
{
    record_event('T');
    ++retire_calls;
    CHECK(task == expected_target);
    CHECK(requester == expected_requester);
    CHECK(hart == current_hart);
    CHECK(base == &mock_sysbase);
    CHECK(irq_depth == 1);
    CHECK(target_pin_held == 1);
    CHECK(task_lock_held == 1);
    if (retire_result == 1)
    {
        handle->index = expected_handle.index;
        handle->generation = expected_handle.generation;
    }
    return retire_result;
}

void krnP4SignalTaskUnpin(struct Task *task, struct ExecBase *base)
{
    record_event('u');
    ++unpin_calls;
    CHECK(task == expected_target);
    CHECK(base == &mock_sysbase);
    CHECK(irq_depth == 1);
    CHECK(task_lock_held == 0);
    CHECK(target_pin_held == 1);
    target_pin_held = 0;
    (void)target_protect_none();
}

void krnP4RetiredOwnerPoke(uint32_t hart, struct ExecBase *base)
{
    record_event('R');
    ++owner_poke_calls;
    last_poke_hart = hart;
    CHECK(base == &mock_sysbase);
    CHECK(irq_depth == 1);
    CHECK(task_lock_held == 0);
    CHECK(target_pin_held == 0);
}

void krnP4CleanupPoke(struct ExecBase *base)
{
    record_event('C');
    ++cleanup_poke_calls;
    CHECK(base == &mock_sysbase);
    CHECK(irq_depth == 1);
    CHECK(task_lock_held == 0);
    CHECK(target_pin_held == 0);
}

void Exec_P4AwaitRemoval(struct Task *requester,
                         struct p4_remove_handle handle,
                         struct ExecBase *base)
{
    record_event('W');
    ++await_calls;
    CHECK(requester == expected_requester);
    CHECK(base == &mock_sysbase);
    CHECK(handle.index == expected_handle.index);
    CHECK(handle.generation == expected_handle.generation);
    CHECK(irq_depth == 0);
    CHECK(task_lock_held == 0);
    CHECK(target_pin_held == 0);
    CHECK(target_was_poisoned != 0);
}

int krnP4SelfRemoveCurrentRetired(struct ExecBase *base)
{
    record_event('S');
    ++self_retired_calls;
    CHECK(base == &mock_sysbase);
    CHECK(irq_depth == 1);
    CHECK(task_lock_held == 0);
    CHECK(target_pin_held == 0);
    return requester_retired;
}

void Exec_P4RemoveSelf(struct Task *requester, struct ExecBase *base)
{
    record_event('X');
    ++self_remove_calls;
    CHECK(requester == expected_requester);
    CHECK(base == &mock_sysbase);
    CHECK(irq_depth == 1);
    CHECK(task_lock_held == 0);
    CHECK(target_pin_held == 0);
    longjmp(transfer_env, 2);
}

void p4_signal_adapter_fail_closed(void)
{
    record_event('F');
    ++fatal_calls;
    CHECK(irq_depth == 1);
    longjmp(transfer_env, 3);
}

static int invoke_remove(struct Task *target, struct Task *requester)
{
    int transfer = setjmp(transfer_env);
    if (transfer == 0)
    {
        Exec_P4RemoveExternal(target, requester, &mock_sysbase);
        return 0;
    }
    return transfer;
}

static void test_raw_retired_join_uses_identity_only(void)
{
    struct Task requester = { UINT32_C(0x1111) };
    const int results[] = { 2 };
    int transfer;

    expected_requester = &requester;
    reset_case(0u);
    expected_target = allocate_target(1);
    set_admission_results(results, 1u);
    transfer = invoke_remove(expected_target, expected_requester);
    CHECK(transfer == 0);
    expect_events("DACEW");
    CHECK(admission_index == 1u && pin_calls == 0);
    CHECK(lock_calls == 0 && owner_calls == 0 && retire_calls == 0);
    CHECK(unpin_calls == 0 && owner_poke_calls == 0);
    CHECK(cleanup_poke_calls == 1 && await_calls == 1);
    release_target_mapping();
}

static void test_new_live_removal_order_and_post_unpin_poison(void)
{
    struct Task requester = { UINT32_C(0x2222) };
    const int results[] = { 0 };
    int transfer;

    expected_requester = &requester;
    reset_case(1u);
    expected_target = allocate_target(0);
    running_owner = 2;
    set_admission_results(results, 1u);
    transfer = invoke_remove(expected_target, expected_requester);
    CHECK(transfer == 0);
    expect_events("DApLOTUuRCEW");
    CHECK(admission_index == 1u && pin_calls == 1 && unpin_calls == 1);
    CHECK(owner_calls == 1 && retire_calls == 1);
    CHECK(owner_poke_calls == 1 && last_poke_hart == 1u);
    CHECK(cleanup_poke_calls == 1 && await_calls == 1);
    CHECK(target_was_poisoned != 0 && target_pin_held == 0);
    release_target_mapping();
}

static void test_failed_pin_retries_as_identity_only_join(void)
{
    struct Task requester = { UINT32_C(0x3333) };
    const int results[] = { 0, 2 };
    int transfer;

    expected_requester = &requester;
    reset_case(0u);
    expected_target = allocate_target(1);
    pin_result = 0;
    set_admission_results(results, 2u);
    transfer = invoke_remove(expected_target, expected_requester);
    CHECK(transfer == 0);
    expect_events("DApACEW");
    CHECK(admission_index == 2u && pin_calls == 1);
    CHECK(lock_calls == 0 && owner_calls == 0 && retire_calls == 0);
    CHECK(unpin_calls == 0 && owner_poke_calls == 0);
    CHECK(cleanup_poke_calls == 1 && await_calls == 1);
    release_target_mapping();
}

static void test_peer_retirement_during_locked_attempt_joins_without_poke(void)
{
    struct Task requester = { UINT32_C(0x4444) };
    const int results[] = { 0, 2 };
    int transfer;

    expected_requester = &requester;
    reset_case(0u);
    expected_target = allocate_target(0);
    running_owner = 2;
    retire_result = 0;
    set_admission_results(results, 2u);
    transfer = invoke_remove(expected_target, expected_requester);
    CHECK(transfer == 0);
    expect_events("DApLOTAUuCEW");
    CHECK(admission_index == 2u && pin_calls == 1 && unpin_calls == 1);
    CHECK(owner_calls == 1 && retire_calls == 1);
    CHECK(owner_poke_calls == 0 && cleanup_poke_calls == 1);
    CHECK(await_calls == 1 && target_was_poisoned != 0);
    release_target_mapping();
}

static void test_refusal_fails_closed_after_target_unlock_and_unpin(void)
{
    struct Task requester = { UINT32_C(0x5555) };
    const int results[] = { 0, 0 };
    int transfer;

    expected_requester = &requester;
    reset_case(1u);
    expected_target = allocate_target(0);
    running_owner = 1;
    retire_result = 0;
    requester_retired = 0;
    set_admission_results(results, 2u);
    transfer = invoke_remove(expected_target, expected_requester);
    CHECK(transfer == 3);
    expect_events("DApLOTAUuSF");
    CHECK(admission_index == 2u && pin_calls == 1 && unpin_calls == 1);
    CHECK(lock_calls == 1 && unlock_calls == 1);
    CHECK(owner_poke_calls == 0 && cleanup_poke_calls == 0);
    CHECK(await_calls == 0 && self_remove_calls == 0 && fatal_calls == 1);
    CHECK(target_was_poisoned != 0 && target_pin_held == 0);
    release_target_mapping();
}

static void test_requester_retirement_refusal_transfers_no_save(void)
{
    struct Task requester = { UINT32_C(0x6666) };
    const int results[] = { 0, 0 };
    int transfer;

    expected_requester = &requester;
    reset_case(0u);
    expected_target = allocate_target(1);
    pin_result = 0;
    requester_retired = 1;
    set_admission_results(results, 2u);
    transfer = invoke_remove(expected_target, expected_requester);
    CHECK(transfer == 2);
    expect_events("DApASX");
    CHECK(admission_index == 2u && pin_calls == 1 && unpin_calls == 0);
    CHECK(self_retired_calls == 1 && self_remove_calls == 1);
    CHECK(cleanup_poke_calls == 0 && await_calls == 0 && fatal_calls == 0);
    release_target_mapping();
}

static void test_bad_context_and_owner_refusal(void)
{
    struct Task requester = { UINT32_C(0x7777) };
    struct Task other = { UINT32_C(0x8888) };
    int transfer;

    expected_requester = &requester;
    reset_case(0u);
    expected_target = allocate_target(1);
    mock_current_task = NULL;
    transfer = invoke_remove(expected_target, expected_requester);
    CHECK(transfer == 3);
    expect_events("DF");
    CHECK(admission_index == 0u && pin_calls == 0 && fatal_calls == 1);
    release_target_mapping();

    reset_case(0u);
    expected_target = allocate_target(1);
    mock_current_task = &other;
    transfer = invoke_remove(expected_target, expected_requester);
    CHECK(transfer == 3);
    expect_events("DF");
    CHECK(admission_index == 0u && pin_calls == 0 && fatal_calls == 1);
    release_target_mapping();

    reset_case(0u);
    expected_target = allocate_target(1);
    transfer = invoke_remove(expected_target, NULL);
    CHECK(transfer == 3);
    expect_events("DF");
    CHECK(admission_index == 0u && pin_calls == 0 && fatal_calls == 1);
    release_target_mapping();

    reset_case(0u);
    expected_target = allocate_target(1);
    transfer = invoke_remove(NULL, expected_requester);
    CHECK(transfer == 3);
    expect_events("DF");
    CHECK(admission_index == 0u && pin_calls == 0 && fatal_calls == 1);
    release_target_mapping();

    reset_case(0u);
    expected_target = &requester;
    transfer = invoke_remove(expected_target, expected_requester);
    CHECK(transfer == 3);
    expect_events("DF");
    CHECK(admission_index == 0u && pin_calls == 0 && fatal_calls == 1);

    reset_case(0u);
    expected_target = allocate_target(1);
    mock_sysbase.cpu_local.trap_depth = 1u;
    transfer = invoke_remove(expected_target, expected_requester);
    CHECK(transfer == 3);
    expect_events("DF");
    CHECK(admission_index == 0u && pin_calls == 0 && fatal_calls == 1);
    release_target_mapping();

    reset_case(0u);
    expected_target = allocate_target(0);
    {
        const int results[] = { 0 };
        set_admission_results(results, 1u);
    }
    running_owner = 3;
    transfer = invoke_remove(expected_target, expected_requester);
    CHECK(transfer == 3);
    expect_events("DApLOF");
    CHECK(lock_calls == 1 && unlock_calls == 0);
    CHECK(pin_calls == 1 && unpin_calls == 0 && retire_calls == 0);
    CHECK(await_calls == 0 && owner_poke_calls == 0 && cleanup_poke_calls == 0);
    /* Fail-closed is noreturn; restore mock lock/pin state for test cleanup. */
    task_lock_held = 0;
    target_pin_held = 0;
    release_target_mapping();
}

int main(void)
{
    struct Task request_identity = { UINT32_C(0xaaaa) };
    expected_requester = &request_identity;
    test_raw_retired_join_uses_identity_only();
    test_new_live_removal_order_and_post_unpin_poison();
    test_failed_pin_retries_as_identity_only_join();
    test_peer_retirement_during_locked_attempt_joins_without_poke();
    test_refusal_fails_closed_after_target_unlock_and_unpin();
    test_requester_retirement_refusal_transfers_no_save();
    test_bad_context_and_owner_refusal();
    if (failure_count != 0u)
    {
        fprintf(stderr, "P4 external removal: %u failures, last case %u checks\n",
                failure_count, check_count);
        return 1;
    }
    printf("P4 external removal: %u checks passed\n", check_count);
    return 0;
}
