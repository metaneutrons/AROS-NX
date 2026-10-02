/* Host fixture for the actual P4 Ready-to-Running queue helper. */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct Node
{
    struct Node *ln_Succ;
    struct Node *ln_Pred;
    uint8_t ln_Type;
    int8_t ln_Pri;
    char *ln_Name;
};

struct List
{
    struct Node *lh_Head;
    struct Node *lh_Tail;
    struct Node *lh_TailPred;
    uint8_t lh_Type;
    uint8_t l_pad;
};

struct IntETask
{
    const uint32_t *iet_CpuAffinity;
    unsigned int iet_CpuNumber;
};

struct Task
{
    struct Node tc_Node;
    unsigned int tc_State;
    struct IntETask *test_etask;
};

struct TestSpinlock
{
    unsigned int id;
};
typedef struct TestSpinlock spinlock_t;

enum
{
    LOCK_READY = 0,
    LOCK_WAIT = 1,
    LOCK_RUNNING = 2,
    LOCK_COUNT = 3
};

struct IntExecBase
{
    struct List TaskRunning;
    spinlock_t TaskReadySpinLock;
    spinlock_t TaskWaitSpinLock;
    spinlock_t TaskRunningSpinLock;
};

struct ExecBase
{
    struct List TaskReady;
    struct List TaskWait;
    struct IntExecBase internal;
};

#define TS_RUN   1u
#define TS_READY 2u
#define TS_WAIT  3u
#define SPINLOCK_MODE_WRITE 1u
#define PrivExecBase(base) (&(base)->internal)
#define GetIntETask(task) ((task)->test_etask)

enum
{
    EVENT_LOCK_READY = 10,
    EVENT_LOCK_RUNNING = 12,
    EVENT_ADMIT_RUNNING = 30,
    EVENT_REMOVE_READY = 40,
    EVENT_ENQUEUE_RUNNING = 50,
    EVENT_UNLOCK_RUNNING = 22,
    EVENT_UNLOCK_READY = 20
};

#define MAX_EVENTS 16u
#define TEST_TASKS 4u

static unsigned int checks;
static unsigned int failures;

#define CHECK(condition)                                                     \
    do                                                                       \
    {                                                                        \
        ++checks;                                                            \
        if (!(condition))                                                    \
        {                                                                    \
            ++failures;                                                     \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,       \
                    #condition);                                            \
        }                                                                    \
    } while (0)

static unsigned int mock_cpu_count = 2u;
static unsigned int mock_cpu_count_calls;
static unsigned int mock_cpu_mask_calls;
static int mock_mie = 1;
static int mock_task_lock_held;
static int mock_candidate_pinned;
static unsigned int mock_admission_result = 1u;
static unsigned int mock_admission_calls;
static unsigned int mock_remove_calls;
static unsigned int mock_enqueue_calls;
static unsigned int mock_events[MAX_EVENTS];
static unsigned int mock_event_count;
static spinlock_t *mock_held_locks[4];
static unsigned int mock_held_count;
struct Fixture;
static struct Fixture *mock_fixture;
static struct Task *mock_selected_task;
static unsigned int mock_requested_hart;
static unsigned int mock_original_cpu_number;
static unsigned int mock_admission_checks;
static unsigned int mock_remove_checks;
static unsigned int mock_enqueue_checks;

struct Fixture
{
    struct ExecBase base;
    struct Task tasks[TEST_TASKS];
    struct IntETask etasks[TEST_TASKS];
    uint32_t affinities[TEST_TASKS];
};

static int mock_lock(spinlock_t *lock, unsigned int mode);
static int mock_unlock(spinlock_t *lock);
#define EXEC_SPINLOCK_LOCK(lock, hook, mode) mock_lock((lock), (mode))
#define EXEC_SPINLOCK_UNLOCK(lock) mock_unlock((lock))

unsigned int krnP4GetCPUCount(void);
int krnP4CPUInMask(uint32_t number, const void *mask);
int krnP4TaskRecordEnterRunning(
    struct Task *task, unsigned int hart, struct ExecBase *SysBase);
void Remove(struct Node *node);
void Enqueue(struct List *list, struct Node *node);

#define P4_SCHEDULER_QUEUE_TEST 1
#define P4_RUNNING_QUEUE_TEST 1
#include "../p4_scheduler_queue.c"

static void record_event(unsigned int event)
{
    CHECK(mock_event_count < MAX_EVENTS);
    if (mock_event_count < MAX_EVENTS)
        mock_events[mock_event_count++] = event;
}

static int mock_lock(spinlock_t *lock, unsigned int mode)
{
    CHECK(lock != NULL);
    CHECK(mode == SPINLOCK_MODE_WRITE);
    CHECK(mock_mie == 0);
    CHECK(mock_task_lock_held);
    CHECK(mock_candidate_pinned);
    CHECK(mock_fixture != NULL);
    CHECK(mock_held_count < sizeof(mock_held_locks) /
                            sizeof(mock_held_locks[0]));

    if (mock_fixture != NULL && lock != NULL)
    {
        if (mock_held_count == 0)
        {
            CHECK(lock == &mock_fixture->base.internal.TaskReadySpinLock);
            CHECK(lock->id == LOCK_READY);
            record_event(EVENT_LOCK_READY);
        }
        else if (mock_held_count == 1)
        {
            CHECK(lock == &mock_fixture->base.internal.TaskRunningSpinLock);
            CHECK(lock->id == LOCK_RUNNING);
            CHECK(mock_held_locks[0] ==
                  &mock_fixture->base.internal.TaskReadySpinLock);
            record_event(EVENT_LOCK_RUNNING);
        }
        else
        {
            CHECK(0 && "unexpected nested queue lock");
        }
    }

    if (lock != NULL && mock_held_count <
        sizeof(mock_held_locks) / sizeof(mock_held_locks[0]))
        mock_held_locks[mock_held_count++] = lock;
    return 0;
}

static int mock_unlock(spinlock_t *lock)
{
    CHECK(lock != NULL);
    CHECK(mock_mie == 0);
    CHECK(mock_task_lock_held);
    CHECK(mock_candidate_pinned);
    CHECK(mock_held_count != 0u);
    if (lock != NULL && mock_held_count != 0u)
    {
        CHECK(mock_held_locks[mock_held_count - 1u] == lock);
        if (mock_fixture != NULL &&
            lock == &mock_fixture->base.internal.TaskRunningSpinLock)
            record_event(EVENT_UNLOCK_RUNNING);
        else if (mock_fixture != NULL &&
                 lock == &mock_fixture->base.internal.TaskReadySpinLock)
            record_event(EVENT_UNLOCK_READY);
        --mock_held_count;
    }
    return 0;
}

unsigned int krnP4GetCPUCount(void)
{
    ++mock_cpu_count_calls;
    CHECK(mock_mie == 0);
    CHECK(mock_task_lock_held);
    return mock_cpu_count;
}

int krnP4CPUInMask(uint32_t number, const void *mask)
{
    const uint32_t *bits = (const uint32_t *)mask;

    ++mock_cpu_mask_calls;
    CHECK(mock_mie == 0);
    CHECK(mock_task_lock_held);
    return bits != NULL && number < 32u &&
           ((*bits & (UINT32_C(1) << number)) != 0);
}

int krnP4TaskRecordEnterRunning(
    struct Task *task, unsigned int hart, struct ExecBase *SysBase)
{
    ++mock_admission_calls;
    ++mock_admission_checks;
    record_event(EVENT_ADMIT_RUNNING);
    CHECK(task == mock_selected_task);
    CHECK(hart == mock_requested_hart);
    CHECK(SysBase == &mock_fixture->base);
    CHECK(mock_mie == 0);
    CHECK(mock_task_lock_held);
    CHECK(mock_candidate_pinned);
    CHECK(mock_held_count == 2u);
    CHECK(mock_held_locks[0] ==
          &mock_fixture->base.internal.TaskReadySpinLock);
    CHECK(mock_held_locks[1] ==
          &mock_fixture->base.internal.TaskRunningSpinLock);
    CHECK(task->tc_State == TS_READY);
    CHECK(GetIntETask(task)->iet_CpuNumber == mock_original_cpu_number);
    CHECK(p4_queue_contains(&SysBase->TaskReady, task) == 1);
    CHECK(p4_queue_contains(&PrivExecBase(SysBase)->TaskRunning, task) == 0);
    CHECK(mock_cpu_count_calls == 1u);
    CHECK(mock_cpu_mask_calls == 1u);
    return (int)mock_admission_result;
}

void Remove(struct Node *node)
{
    struct Task *task = (struct Task *)node;

    ++mock_remove_calls;
    ++mock_remove_checks;
    record_event(EVENT_REMOVE_READY);
    CHECK(task == mock_selected_task);
    CHECK(mock_mie == 0);
    CHECK(mock_task_lock_held);
    CHECK(mock_candidate_pinned);
    CHECK(mock_held_count == 2u);
    CHECK(task->tc_State == TS_READY);
    CHECK(GetIntETask(task)->iet_CpuNumber == mock_original_cpu_number);
    CHECK(p4_queue_contains(&mock_fixture->base.TaskReady, task) == 1);
    CHECK(p4_queue_contains(&mock_fixture->base.internal.TaskRunning,
                            task) == 0);
    node->ln_Pred->ln_Succ = node->ln_Succ;
    node->ln_Succ->ln_Pred = node->ln_Pred;
}

void Enqueue(struct List *list, struct Node *node)
{
    struct Task *task = (struct Task *)node;
    struct Node *next = list->lh_Head;

    ++mock_enqueue_calls;
    ++mock_enqueue_checks;
    record_event(EVENT_ENQUEUE_RUNNING);
    CHECK(list == &mock_fixture->base.internal.TaskRunning);
    CHECK(task == mock_selected_task);
    CHECK(mock_mie == 0);
    CHECK(mock_task_lock_held);
    CHECK(mock_candidate_pinned);
    CHECK(mock_held_count == 2u);
    CHECK(task->tc_State == TS_RUN);
    CHECK(GetIntETask(task)->iet_CpuNumber == mock_requested_hart);
    CHECK(p4_queue_contains(&mock_fixture->base.TaskReady, task) == 0);
    CHECK(p4_queue_contains(&mock_fixture->base.internal.TaskRunning,
                            task) == 0);

    while (next->ln_Succ != NULL)
    {
        if (node->ln_Pri > next->ln_Pri)
            break;
        next = next->ln_Succ;
    }

    node->ln_Pred = next->ln_Pred;
    node->ln_Succ = next;
    next->ln_Pred->ln_Succ = node;
    next->ln_Pred = node;
}

static void reset_mocks(void)
{
    mock_cpu_count = 2u;
    mock_cpu_count_calls = 0;
    mock_cpu_mask_calls = 0;
    mock_mie = 1;
    mock_task_lock_held = 0;
    mock_candidate_pinned = 0;
    mock_admission_result = 1u;
    mock_admission_calls = 0;
    mock_remove_calls = 0;
    mock_enqueue_calls = 0;
    mock_event_count = 0;
    mock_held_count = 0;
    mock_fixture = NULL;
    mock_selected_task = NULL;
    mock_requested_hart = UINT32_MAX;
    mock_original_cpu_number = UINT32_MAX;
    mock_admission_checks = 0;
    mock_remove_checks = 0;
    mock_enqueue_checks = 0;
    memset(mock_events, 0, sizeof(mock_events));
    memset(mock_held_locks, 0, sizeof(mock_held_locks));
}

static void init_list(struct List *list)
{
    list->lh_Head = (struct Node *)&list->lh_Tail;
    list->lh_Tail = NULL;
    list->lh_TailPred = (struct Node *)&list->lh_Head;
    list->lh_Type = 0;
    list->l_pad = 0;
}

static void init_fixture(struct Fixture *fixture)
{
    memset(fixture, 0, sizeof(*fixture));
    init_list(&fixture->base.TaskReady);
    init_list(&fixture->base.TaskWait);
    init_list(&fixture->base.internal.TaskRunning);
    fixture->base.internal.TaskReadySpinLock.id = LOCK_READY;
    fixture->base.internal.TaskWaitSpinLock.id = LOCK_WAIT;
    fixture->base.internal.TaskRunningSpinLock.id = LOCK_RUNNING;
    reset_mocks();
}

static void init_task(struct Fixture *fixture, unsigned int index,
                      unsigned int state, int priority, uint32_t affinity,
                      unsigned int cpu_number)
{
    struct Task *task = &fixture->tasks[index];

    memset(task, 0, sizeof(*task));
    memset(&fixture->etasks[index], 0, sizeof(fixture->etasks[index]));
    task->tc_State = state;
    task->tc_Node.ln_Pri = (int8_t)priority;
    task->test_etask = &fixture->etasks[index];
    fixture->affinities[index] = affinity;
    fixture->etasks[index].iet_CpuAffinity = &fixture->affinities[index];
    fixture->etasks[index].iet_CpuNumber = cpu_number;
}

static void add_tail(struct List *list, struct Node *node)
{
    node->ln_Succ = (struct Node *)&list->lh_Tail;
    node->ln_Pred = list->lh_TailPred;
    list->lh_TailPred->ln_Succ = node;
    list->lh_TailPred = node;
}

static int list_valid(const struct List *list, unsigned int expected_count)
{
    const struct Node *tail = (const struct Node *)&list->lh_Tail;
    const struct Node *previous = (const struct Node *)&list->lh_Head;
    const struct Node *node = list->lh_Head;
    unsigned int count = 0;

    if (list->lh_Tail != NULL)
        return 0;
    while (node != tail && node != NULL && count <= expected_count)
    {
        if (node->ln_Succ == NULL || node->ln_Pred != previous)
            return 0;
        previous = node;
        node = node->ln_Succ;
        ++count;
    }
    return node == tail && list->lh_TailPred == previous &&
           count == expected_count;
}

static int list_order_is(const struct List *list,
                         struct Task **expected, unsigned int count)
{
    const struct Node *node = list->lh_Head;
    unsigned int i;

    if (!list_valid(list, count))
        return 0;
    for (i = 0; i < count; ++i)
    {
        if (node != &expected[i]->tc_Node)
            return 0;
        node = node->ln_Succ;
    }
    return node == (const struct Node *)&list->lh_Tail;
}

static int event_trace_is(const unsigned int *expected, unsigned int count)
{
    return mock_event_count == count && mock_held_count == 0u &&
           memcmp(mock_events, expected, count * sizeof(expected[0])) == 0;
}

static int invoke_ready_to_running(struct Fixture *fixture,
                                   unsigned int task_index,
                                   unsigned int hart,
                                   unsigned int admission_result)
{
    struct Task *task = &fixture->tasks[task_index];

    mock_fixture = fixture;
    mock_selected_task = task;
    mock_requested_hart = hart;
    mock_original_cpu_number = task->test_etask != NULL ?
        task->test_etask->iet_CpuNumber : UINT32_MAX;
    mock_admission_result = admission_result;
    mock_task_lock_held = 1;
    mock_candidate_pinned = 1;
    mock_mie = 0;
    {
        int accepted = krnP4ReadyToRunningLocked(task, hart, &fixture->base);
        CHECK(mock_mie == 0);
        CHECK(mock_task_lock_held);
        CHECK(mock_candidate_pinned);
        CHECK(mock_held_count == 0u);
        mock_task_lock_held = 0;
        mock_candidate_pinned = 0;
        mock_mie = 1;
        return accepted;
    }
}

static int fixture_unchanged_after_refusal(struct Fixture *fixture,
                                           unsigned int task_index,
                                           unsigned int hart)
{
    struct Fixture before = *fixture;
    int result = invoke_ready_to_running(fixture, task_index, hart, 1u);

    return result == 0 && memcmp(&before, fixture, sizeof(before)) == 0;
}

static int test_hart0_owner_state_and_visibility_order(void)
{
    static const unsigned int expected_events[] = {
        EVENT_LOCK_READY, EVENT_LOCK_RUNNING, EVENT_ADMIT_RUNNING,
        EVENT_REMOVE_READY, EVENT_ENQUEUE_RUNNING,
        EVENT_UNLOCK_RUNNING, EVENT_UNLOCK_READY
    };
    struct Fixture fixture;
    struct Task *expected_ready[1];
    struct Task *expected_running[2];

    init_fixture(&fixture);
    init_task(&fixture, 0, TS_READY, 6, UINT32_C(1) << 0, 77u);
    init_task(&fixture, 1, TS_READY, 8, UINT32_C(3), 99u);
    init_task(&fixture, 2, TS_RUN, 10, UINT32_C(3), 1u);
    add_tail(&fixture.base.TaskReady, &fixture.tasks[0].tc_Node);
    add_tail(&fixture.base.TaskReady, &fixture.tasks[1].tc_Node);
    add_tail(&fixture.base.internal.TaskRunning,
             &fixture.tasks[2].tc_Node);

    CHECK(invoke_ready_to_running(&fixture, 0u, 0u, 1u) == 1);
    expected_ready[0] = &fixture.tasks[1];
    expected_running[0] = &fixture.tasks[2];
    expected_running[1] = &fixture.tasks[0];
    CHECK(fixture.tasks[0].test_etask->iet_CpuNumber == 0u);
    CHECK(fixture.tasks[0].tc_State == TS_RUN);
    CHECK(list_order_is(&fixture.base.TaskReady, expected_ready, 1u));
    CHECK(list_order_is(&fixture.base.internal.TaskRunning,
                        expected_running, 2u));
    CHECK(event_trace_is(expected_events,
                         sizeof(expected_events) / sizeof(expected_events[0])));
    CHECK(mock_admission_calls == 1u);
    CHECK(mock_remove_calls == 1u && mock_enqueue_calls == 1u);
    CHECK(mock_admission_checks != 0u && mock_remove_checks != 0u &&
          mock_enqueue_checks != 0u);
    CHECK(mock_cpu_count_calls == 1u && mock_cpu_mask_calls == 1u);
    return failures == 0u;
}

static int test_hart1_owner_before_running_visibility(void)
{
    static const unsigned int expected_events[] = {
        EVENT_LOCK_READY, EVENT_LOCK_RUNNING, EVENT_ADMIT_RUNNING,
        EVENT_REMOVE_READY, EVENT_ENQUEUE_RUNNING,
        EVENT_UNLOCK_RUNNING, EVENT_UNLOCK_READY
    };
    struct Fixture fixture;

    init_fixture(&fixture);
    init_task(&fixture, 0, TS_READY, 5, UINT32_C(1) << 1, 44u);
    add_tail(&fixture.base.TaskReady, &fixture.tasks[0].tc_Node);

    CHECK(invoke_ready_to_running(&fixture, 0u, 1u, 1u) == 1);
    CHECK(fixture.tasks[0].test_etask->iet_CpuNumber == 1u);
    CHECK(fixture.tasks[0].tc_State == TS_RUN);
    CHECK(list_valid(&fixture.base.TaskReady, 0u));
    CHECK(list_valid(&fixture.base.internal.TaskRunning, 1u));
    CHECK(fixture.base.internal.TaskRunning.lh_Head ==
          &fixture.tasks[0].tc_Node);
    CHECK(event_trace_is(expected_events,
                         sizeof(expected_events) / sizeof(expected_events[0])));
    CHECK(mock_admission_calls == 1u);
    return failures == 0u;
}

static int test_invalid_hart_and_affinity_refuse_without_mutation(void)
{
    struct Fixture fixture;

    init_fixture(&fixture);
    init_task(&fixture, 0, TS_READY, 3, UINT32_C(3), 8u);
    add_tail(&fixture.base.TaskReady, &fixture.tasks[0].tc_Node);
    mock_cpu_count = 0u;
    CHECK(fixture_unchanged_after_refusal(&fixture, 0u, 0u));
    CHECK(mock_event_count == 0u && mock_admission_calls == 0u);
    CHECK(mock_cpu_count_calls == 1u && mock_cpu_mask_calls == 0u);

    init_fixture(&fixture);
    init_task(&fixture, 0, TS_READY, 3, UINT32_C(3), 8u);
    add_tail(&fixture.base.TaskReady, &fixture.tasks[0].tc_Node);
    mock_cpu_count = 3u;
    CHECK(fixture_unchanged_after_refusal(&fixture, 0u, 0u));
    CHECK(mock_event_count == 0u && mock_admission_calls == 0u);
    CHECK(mock_cpu_count_calls == 1u && mock_cpu_mask_calls == 0u);

    init_fixture(&fixture);
    init_task(&fixture, 0, TS_READY, 3, UINT32_C(1) << 1, 8u);
    add_tail(&fixture.base.TaskReady, &fixture.tasks[0].tc_Node);
    mock_cpu_count = 1u;
    CHECK(fixture_unchanged_after_refusal(&fixture, 0u, 1u));
    CHECK(mock_event_count == 0u && mock_admission_calls == 0u);
    CHECK(mock_cpu_count_calls == 1u && mock_cpu_mask_calls == 0u);

    init_fixture(&fixture);
    init_task(&fixture, 0, TS_READY, 3, UINT32_C(3), 8u);
    add_tail(&fixture.base.TaskReady, &fixture.tasks[0].tc_Node);
    CHECK(fixture_unchanged_after_refusal(&fixture, 0u, 2u));
    CHECK(mock_event_count == 0u && mock_admission_calls == 0u);
    CHECK(mock_cpu_count_calls == 1u && mock_cpu_mask_calls == 0u);

    init_fixture(&fixture);
    init_task(&fixture, 0, TS_READY, 3, UINT32_C(1) << 0, 8u);
    add_tail(&fixture.base.TaskReady, &fixture.tasks[0].tc_Node);
    CHECK(fixture_unchanged_after_refusal(&fixture, 0u, 1u));
    CHECK(mock_event_count == 0u && mock_admission_calls == 0u);
    CHECK(mock_cpu_count_calls == 1u && mock_cpu_mask_calls == 1u);
    return failures == 0u;
}

static int test_corrupt_source_missing_and_wrong_state_refuse(void)
{
    static const unsigned int locked_reject_events[] = {
        EVENT_LOCK_READY, EVENT_LOCK_RUNNING,
        EVENT_UNLOCK_RUNNING, EVENT_UNLOCK_READY
    };
    struct Fixture fixture;

    init_fixture(&fixture);
    init_task(&fixture, 0, TS_READY, 3, UINT32_C(1), 8u);
    add_tail(&fixture.base.TaskReady, &fixture.tasks[0].tc_Node);
    fixture.base.TaskReady.lh_TailPred =
        (struct Node *)&fixture.base.TaskReady.lh_Head;
    CHECK(fixture_unchanged_after_refusal(&fixture, 0u, 0u));
    CHECK(event_trace_is(locked_reject_events,
        sizeof(locked_reject_events) / sizeof(locked_reject_events[0])));
    CHECK(mock_admission_calls == 0u);

    init_fixture(&fixture);
    init_task(&fixture, 0, TS_READY, 3, UINT32_C(1), 8u);
    CHECK(fixture_unchanged_after_refusal(&fixture, 0u, 0u));
    CHECK(event_trace_is(locked_reject_events,
        sizeof(locked_reject_events) / sizeof(locked_reject_events[0])));
    CHECK(mock_admission_calls == 0u);

    init_fixture(&fixture);
    init_task(&fixture, 0, TS_RUN, 3, UINT32_C(1), 8u);
    add_tail(&fixture.base.TaskReady, &fixture.tasks[0].tc_Node);
    CHECK(fixture_unchanged_after_refusal(&fixture, 0u, 0u));
    CHECK(event_trace_is(locked_reject_events,
        sizeof(locked_reject_events) / sizeof(locked_reject_events[0])));
    CHECK(mock_admission_calls == 0u);

    return failures == 0u;
}

static int test_duplicate_running_membership_refuses(void)
{
    static const unsigned int locked_reject_events[] = {
        EVENT_LOCK_READY, EVENT_LOCK_RUNNING,
        EVENT_UNLOCK_RUNNING, EVENT_UNLOCK_READY
    };
    struct Fixture fixture;

    init_fixture(&fixture);
    init_task(&fixture, 0, TS_READY, 3, UINT32_C(1), 8u);
    add_tail(&fixture.base.TaskReady, &fixture.tasks[0].tc_Node);
    /* Model a corrupt duplicate intrusive membership in TaskRunning. The
     * shared Node links still describe Ready, so validation must reject the
     * Running view rather than detach or re-enqueue it. */
    fixture.base.internal.TaskRunning.lh_Head = &fixture.tasks[0].tc_Node;
    fixture.base.internal.TaskRunning.lh_TailPred =
        &fixture.tasks[0].tc_Node;

    CHECK(fixture_unchanged_after_refusal(&fixture, 0u, 0u));
    CHECK(event_trace_is(locked_reject_events,
        sizeof(locked_reject_events) / sizeof(locked_reject_events[0])));
    CHECK(mock_admission_calls == 0u);

    /* A duplicate dispatch of an already-running task is refused while its
     * original Running membership and owner remain intact. */
    init_fixture(&fixture);
    init_task(&fixture, 0, TS_RUN, 3, UINT32_C(1), 0u);
    add_tail(&fixture.base.internal.TaskRunning,
             &fixture.tasks[0].tc_Node);
    CHECK(fixture_unchanged_after_refusal(&fixture, 0u, 0u));
    CHECK(event_trace_is(locked_reject_events,
        sizeof(locked_reject_events) / sizeof(locked_reject_events[0])));
    CHECK(mock_admission_calls == 0u);
    return failures == 0u;
}

static int test_admission_failure_is_last_before_mutation(void)
{
    static const unsigned int expected_events[] = {
        EVENT_LOCK_READY, EVENT_LOCK_RUNNING, EVENT_ADMIT_RUNNING,
        EVENT_UNLOCK_RUNNING, EVENT_UNLOCK_READY
    };
    struct Fixture fixture;
    struct Fixture before;

    init_fixture(&fixture);
    init_task(&fixture, 0, TS_READY, 4, UINT32_C(1) << 1, 55u);
    add_tail(&fixture.base.TaskReady, &fixture.tasks[0].tc_Node);
    before = fixture;

    CHECK(invoke_ready_to_running(&fixture, 0u, 1u, 0u) == 0);
    CHECK(memcmp(&before, &fixture, sizeof(fixture)) == 0);
    CHECK(fixture.tasks[0].test_etask->iet_CpuNumber == 55u);
    CHECK(fixture.tasks[0].tc_State == TS_READY);
    CHECK(list_valid(&fixture.base.TaskReady, 1u));
    CHECK(list_valid(&fixture.base.internal.TaskRunning, 0u));
    CHECK(event_trace_is(expected_events,
        sizeof(expected_events) / sizeof(expected_events[0])));
    CHECK(mock_admission_calls == 1u);
    CHECK(mock_remove_calls == 0u && mock_enqueue_calls == 0u);
    CHECK(mock_cpu_count_calls == 1u && mock_cpu_mask_calls == 1u);
    return failures == 0u;
}

int main(void)
{
    (void)test_hart0_owner_state_and_visibility_order();
    (void)test_hart1_owner_before_running_visibility();
    (void)test_invalid_hart_and_affinity_refuse_without_mutation();
    (void)test_corrupt_source_missing_and_wrong_state_refuse();
    (void)test_duplicate_running_membership_refuses();
    (void)test_admission_failure_is_last_before_mutation();

    CHECK(mock_held_count == 0u);
    CHECK(mock_mie == 1);
    CHECK(!mock_task_lock_held);
    CHECK(!mock_candidate_pinned);

    if (failures != 0u)
    {
        fprintf(stderr, "%u of %u checks failed\n", failures, checks);
        return EXIT_FAILURE;
    }

    printf("P4 Ready-to-Running checks: %u passed\n", checks);
    return EXIT_SUCCESS;
}
