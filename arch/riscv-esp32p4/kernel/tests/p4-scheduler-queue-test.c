/* Host fixture for the actual P4 scheduler queue adapter. */
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

static int mock_lock(spinlock_t *lock, unsigned int mode);
static int mock_unlock(spinlock_t *lock);
#define EXEC_SPINLOCK_LOCK(lock, hook, mode) \
    mock_lock((lock), (mode))
#define EXEC_SPINLOCK_UNLOCK(lock) mock_unlock((lock))

static unsigned int mock_cpu_count;
static unsigned int cpu_count_calls;
static unsigned int cpu_mask_calls;
unsigned int krnP4GetCPUCount(void);
int krnP4CPUInMask(uint32_t number, const void *mask);

void Remove(struct Node *node);
void Enqueue(struct List *list, struct Node *node);

#define P4_SCHEDULER_QUEUE_TEST 1
#include "../p4_scheduler_queue.c"

#define TEST_TASKS 8u
#define CHECK(condition) do { \
    ++checks; \
    if (!(condition)) { \
        fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); \
        return 0; \
    } \
} while (0)

struct Fixture
{
    struct ExecBase base;
    struct Task tasks[TEST_TASKS];
    struct IntETask etasks[TEST_TASKS];
    uint32_t affinities[TEST_TASKS];
};

static unsigned int checks;
static unsigned int lock_events[8];
static unsigned int lock_event_count;
static spinlock_t *held_locks[4];
static unsigned int held_lock_count;

static int mock_lock(spinlock_t *lock, unsigned int mode)
{
    if (lock == NULL || mode != SPINLOCK_MODE_WRITE ||
        lock_event_count >= sizeof(lock_events) / sizeof(lock_events[0]) ||
        held_lock_count >= sizeof(held_locks) / sizeof(held_locks[0]))
        return -1;

    lock_events[lock_event_count++] = 10u + lock->id;
    held_locks[held_lock_count++] = lock;
    return 0;
}

static int mock_unlock(spinlock_t *lock)
{
    if (lock == NULL || held_lock_count == 0 ||
        held_locks[held_lock_count - 1u] != lock ||
        lock_event_count >= sizeof(lock_events) / sizeof(lock_events[0]))
        return -1;

    --held_lock_count;
    lock_events[lock_event_count++] = 20u + lock->id;
    return 0;
}

unsigned int krnP4GetCPUCount(void)
{
    ++cpu_count_calls;
    return mock_cpu_count;
}

int krnP4CPUInMask(uint32_t number, const void *mask)
{
    const uint32_t *bits = (const uint32_t *)mask;

    ++cpu_mask_calls;
    return bits != NULL && number < 32u &&
           ((*bits & (UINT32_C(1) << number)) != 0);
}

static void reset_mocks(void)
{
    memset(lock_events, 0, sizeof(lock_events));
    memset(held_locks, 0, sizeof(held_locks));
    lock_event_count = 0;
    held_lock_count = 0;
    cpu_count_calls = 0;
    cpu_mask_calls = 0;
    mock_cpu_count = 2;
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
                      unsigned int state, int priority, uint32_t affinity)
{
    struct Task *task = &fixture->tasks[index];

    memset(task, 0, sizeof(*task));
    task->tc_State = state;
    task->tc_Node.ln_Pri = (int8_t)priority;
    task->test_etask = &fixture->etasks[index];
    fixture->affinities[index] = affinity;
    fixture->etasks[index].iet_CpuAffinity = &fixture->affinities[index];
}

static void add_tail(struct List *list, struct Node *node)
{
    node->ln_Succ = (struct Node *)&list->lh_Tail;
    node->ln_Pred = list->lh_TailPred;
    list->lh_TailPred->ln_Succ = node;
    list->lh_TailPred = node;
}

void Remove(struct Node *node)
{
    node->ln_Pred->ln_Succ = node->ln_Succ;
    node->ln_Succ->ln_Pred = node->ln_Pred;
}

void Enqueue(struct List *list, struct Node *node)
{
    struct Node *next = list->lh_Head;

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

static int lock_trace_is(unsigned int first, unsigned int second)
{
    return lock_event_count == 4u && held_lock_count == 0 &&
           lock_events[0] == 10u + first &&
           lock_events[1] == 10u + second &&
           lock_events[2] == 20u + second &&
           lock_events[3] == 20u + first;
}

static int reject_unchanged(struct Fixture *fixture, struct Task *task,
                            enum p4_signal_queue_transition transition)
{
    struct Fixture before = *fixture;
    int result = krnP4SignalQueueApplyLocked(task, transition,
                                              &fixture->base);
    return result < 0 && memcmp(&before, fixture, sizeof(before)) == 0;
}

static int test_running_to_ready_priority_hart0(void)
{
    struct Fixture fixture;
    struct Task *expected[4];
    int result;

    init_fixture(&fixture);
    init_task(&fixture, 0, TS_RUN, 7, UINT32_C(1) << 0);
    init_task(&fixture, 1, TS_READY, 10, UINT32_C(3));
    init_task(&fixture, 2, TS_READY, 5, UINT32_C(3));
    init_task(&fixture, 3, TS_READY, 5, UINT32_C(3));
    add_tail(&fixture.base.internal.TaskRunning,
             &fixture.tasks[0].tc_Node);
    add_tail(&fixture.base.TaskReady, &fixture.tasks[1].tc_Node);
    add_tail(&fixture.base.TaskReady, &fixture.tasks[2].tc_Node);
    add_tail(&fixture.base.TaskReady, &fixture.tasks[3].tc_Node);
    expected[0] = &fixture.tasks[1];
    expected[1] = &fixture.tasks[0];
    expected[2] = &fixture.tasks[2];
    expected[3] = &fixture.tasks[3];

    result = krnP4SignalQueueApplyLocked(&fixture.tasks[0],
        P4_SIGNAL_QUEUE_RUNNING_TO_READY, &fixture.base);
    CHECK(result == 1);
    CHECK(fixture.tasks[0].tc_State == TS_READY);
    CHECK(list_valid(&fixture.base.internal.TaskRunning, 0));
    CHECK(list_order_is(&fixture.base.TaskReady, expected, 4));
    CHECK(lock_trace_is(LOCK_READY, LOCK_RUNNING));
    CHECK(cpu_count_calls == 1 && cpu_mask_calls == 1);
    return 1;
}

static int test_running_to_wait_order(void)
{
    struct Fixture fixture;
    struct Task *expected[3];
    int result;

    init_fixture(&fixture);
    init_task(&fixture, 0, TS_RUN, 5, UINT32_C(3));
    init_task(&fixture, 1, TS_WAIT, 9, UINT32_C(3));
    init_task(&fixture, 2, TS_WAIT, 2, UINT32_C(3));
    add_tail(&fixture.base.internal.TaskRunning,
             &fixture.tasks[0].tc_Node);
    add_tail(&fixture.base.TaskWait, &fixture.tasks[1].tc_Node);
    add_tail(&fixture.base.TaskWait, &fixture.tasks[2].tc_Node);
    expected[0] = &fixture.tasks[1];
    expected[1] = &fixture.tasks[0];
    expected[2] = &fixture.tasks[2];

    result = krnP4SignalQueueApplyLocked(&fixture.tasks[0],
        P4_SIGNAL_QUEUE_RUNNING_TO_WAIT, &fixture.base);
    CHECK(result == 0);
    CHECK(fixture.tasks[0].tc_State == TS_WAIT);
    CHECK(list_valid(&fixture.base.internal.TaskRunning, 0));
    CHECK(list_order_is(&fixture.base.TaskWait, expected, 3));
    CHECK(lock_trace_is(LOCK_WAIT, LOCK_RUNNING));
    CHECK(cpu_count_calls == 0 && cpu_mask_calls == 0);
    return 1;
}

static int test_waiting_to_ready_priority_hart1(void)
{
    struct Fixture fixture;
    struct Task *expected[3];
    int result;

    init_fixture(&fixture);
    init_task(&fixture, 0, TS_WAIT, 6, UINT32_C(1) << 1);
    init_task(&fixture, 1, TS_READY, 8, UINT32_C(3));
    init_task(&fixture, 2, TS_READY, 4, UINT32_C(3));
    add_tail(&fixture.base.TaskWait, &fixture.tasks[0].tc_Node);
    add_tail(&fixture.base.TaskReady, &fixture.tasks[1].tc_Node);
    add_tail(&fixture.base.TaskReady, &fixture.tasks[2].tc_Node);
    expected[0] = &fixture.tasks[1];
    expected[1] = &fixture.tasks[0];
    expected[2] = &fixture.tasks[2];

    result = krnP4SignalQueueApplyLocked(&fixture.tasks[0],
        P4_SIGNAL_QUEUE_WAITING_TO_READY, &fixture.base);
    CHECK(result == 2);
    CHECK(fixture.tasks[0].tc_State == TS_READY);
    CHECK(list_valid(&fixture.base.TaskWait, 0));
    CHECK(list_order_is(&fixture.base.TaskReady, expected, 3));
    CHECK(lock_trace_is(LOCK_READY, LOCK_WAIT));
    CHECK(cpu_count_calls == 1 && cpu_mask_calls == 2);
    return 1;
}

static int test_eligibility_and_default_rejections(void)
{
    struct Fixture fixture;

    init_fixture(&fixture);
    init_task(&fixture, 0, TS_RUN, 1, 0);
    add_tail(&fixture.base.internal.TaskRunning,
             &fixture.tasks[0].tc_Node);
    CHECK(reject_unchanged(&fixture, &fixture.tasks[0],
                           P4_SIGNAL_QUEUE_RUNNING_TO_READY));
    CHECK(lock_event_count == 0 && held_lock_count == 0);

    init_fixture(&fixture);
    init_task(&fixture, 0, TS_RUN, 1, UINT32_C(1));
    add_tail(&fixture.base.internal.TaskRunning,
             &fixture.tasks[0].tc_Node);
    mock_cpu_count = 0;
    CHECK(reject_unchanged(&fixture, &fixture.tasks[0],
                           P4_SIGNAL_QUEUE_RUNNING_TO_READY));
    CHECK(lock_event_count == 0);

    init_fixture(&fixture);
    init_task(&fixture, 0, TS_RUN, 1, UINT32_C(3));
    add_tail(&fixture.base.internal.TaskRunning,
             &fixture.tasks[0].tc_Node);
    mock_cpu_count = 3;
    CHECK(reject_unchanged(&fixture, &fixture.tasks[0],
                           P4_SIGNAL_QUEUE_RUNNING_TO_READY));
    CHECK(lock_event_count == 0);

    init_fixture(&fixture);
    init_task(&fixture, 0, TS_RUN, 1, UINT32_C(3));
    fixture.tasks[0].test_etask = NULL;
    add_tail(&fixture.base.internal.TaskRunning,
             &fixture.tasks[0].tc_Node);
    CHECK(reject_unchanged(&fixture, &fixture.tasks[0],
                           P4_SIGNAL_QUEUE_RUNNING_TO_READY));
    CHECK(lock_event_count == 0);

    init_fixture(&fixture);
    init_task(&fixture, 0, TS_RUN, 1, UINT32_C(3));
    add_tail(&fixture.base.internal.TaskRunning,
             &fixture.tasks[0].tc_Node);
    CHECK(reject_unchanged(&fixture, &fixture.tasks[0],
                           P4_SIGNAL_QUEUE_NONE));
    CHECK(lock_event_count == 0);
    CHECK(krnP4SignalQueueApplyLocked(NULL, P4_SIGNAL_QUEUE_NONE,
                                      &fixture.base) < 0);
    CHECK(krnP4SignalQueueApplyLocked(&fixture.tasks[0],
                                      P4_SIGNAL_QUEUE_NONE, NULL) < 0);
    CHECK(lock_event_count == 0);
    return 1;
}

static int test_absent_duplicate_and_state_mismatch(void)
{
    struct Fixture fixture;

    /* Correct state but no source membership. */
    init_fixture(&fixture);
    init_task(&fixture, 0, TS_RUN, 4, UINT32_C(1));
    CHECK(reject_unchanged(&fixture, &fixture.tasks[0],
                           P4_SIGNAL_QUEUE_RUNNING_TO_READY));
    CHECK(lock_trace_is(LOCK_READY, LOCK_RUNNING));

    /* Source contains the task while destination has a duplicate reference
     * to the same intrusive node. Such dual membership cannot have valid
     * backlinks in both lists; the destination validator must reject it. */
    init_fixture(&fixture);
    init_task(&fixture, 0, TS_RUN, 4, UINT32_C(1));
    add_tail(&fixture.base.internal.TaskRunning,
             &fixture.tasks[0].tc_Node);
    fixture.base.TaskReady.lh_Head = &fixture.tasks[0].tc_Node;
    fixture.base.TaskReady.lh_TailPred = &fixture.tasks[0].tc_Node;
    CHECK(reject_unchanged(&fixture, &fixture.tasks[0],
                           P4_SIGNAL_QUEUE_RUNNING_TO_READY));
    CHECK(lock_trace_is(LOCK_READY, LOCK_RUNNING));

    /* Membership and list shape are valid, but tc_State disagrees. */
    init_fixture(&fixture);
    init_task(&fixture, 0, TS_WAIT, 4, UINT32_C(1));
    add_tail(&fixture.base.internal.TaskRunning,
             &fixture.tasks[0].tc_Node);
    CHECK(reject_unchanged(&fixture, &fixture.tasks[0],
                           P4_SIGNAL_QUEUE_RUNNING_TO_READY));
    CHECK(lock_trace_is(LOCK_READY, LOCK_RUNNING));
    return 1;
}

static int test_full_list_validation_after_target(void)
{
    struct Fixture fixture;

    /* The target is found first, but the helper must validate the remaining
     * predecessor chain instead of accepting early membership. */
    init_fixture(&fixture);
    init_task(&fixture, 0, TS_RUN, 4, UINT32_C(1));
    init_task(&fixture, 1, TS_RUN, 3, UINT32_C(1));
    add_tail(&fixture.base.internal.TaskRunning,
             &fixture.tasks[0].tc_Node);
    add_tail(&fixture.base.internal.TaskRunning,
             &fixture.tasks[1].tc_Node);
    fixture.tasks[1].tc_Node.ln_Pred =
        (struct Node *)&fixture.base.internal.TaskRunning.lh_Head;
    CHECK(reject_unchanged(&fixture, &fixture.tasks[0],
                           P4_SIGNAL_QUEUE_RUNNING_TO_READY));
    CHECK(lock_trace_is(LOCK_READY, LOCK_RUNNING));

    /* Tail predecessor corruption after the target also rejects. */
    init_fixture(&fixture);
    init_task(&fixture, 0, TS_RUN, 4, UINT32_C(1));
    add_tail(&fixture.base.internal.TaskRunning,
             &fixture.tasks[0].tc_Node);
    fixture.base.internal.TaskRunning.lh_TailPred =
        (struct Node *)&fixture.base.internal.TaskRunning.lh_Head;
    CHECK(reject_unchanged(&fixture, &fixture.tasks[0],
                           P4_SIGNAL_QUEUE_RUNNING_TO_READY));

    /* The destination is fully checked too, even though it must not contain
     * the target and contains a malformed node after a normal first entry. */
    init_fixture(&fixture);
    init_task(&fixture, 0, TS_RUN, 4, UINT32_C(1));
    init_task(&fixture, 1, TS_READY, 9, UINT32_C(1));
    init_task(&fixture, 2, TS_READY, 3, UINT32_C(1));
    add_tail(&fixture.base.internal.TaskRunning,
             &fixture.tasks[0].tc_Node);
    add_tail(&fixture.base.TaskReady, &fixture.tasks[1].tc_Node);
    add_tail(&fixture.base.TaskReady, &fixture.tasks[2].tc_Node);
    fixture.tasks[2].tc_Node.ln_Pred =
        (struct Node *)&fixture.base.TaskReady.lh_Head;
    CHECK(reject_unchanged(&fixture, &fixture.tasks[0],
                           P4_SIGNAL_QUEUE_RUNNING_TO_READY));
    CHECK(lock_trace_is(LOCK_READY, LOCK_RUNNING));

    /* A non-NULL lh_Tail is a malformed Exec list. */
    init_fixture(&fixture);
    init_task(&fixture, 0, TS_RUN, 4, UINT32_C(1));
    add_tail(&fixture.base.internal.TaskRunning,
             &fixture.tasks[0].tc_Node);
    fixture.base.internal.TaskRunning.lh_Tail = &fixture.tasks[1].tc_Node;
    CHECK(reject_unchanged(&fixture, &fixture.tasks[0],
                           P4_SIGNAL_QUEUE_RUNNING_TO_READY));
    return 1;
}

static int test_cycle_rejected_without_mutation(void)
{
    struct Fixture fixture;

    init_fixture(&fixture);
    init_task(&fixture, 0, TS_RUN, 4, UINT32_C(1));
    init_task(&fixture, 1, TS_RUN, 3, UINT32_C(1));
    add_tail(&fixture.base.internal.TaskRunning,
             &fixture.tasks[0].tc_Node);
    add_tail(&fixture.base.internal.TaskRunning,
             &fixture.tasks[1].tc_Node);
    fixture.tasks[1].tc_Node.ln_Succ = &fixture.tasks[0].tc_Node;
    CHECK(reject_unchanged(&fixture, &fixture.tasks[0],
                           P4_SIGNAL_QUEUE_RUNNING_TO_READY));
    CHECK(lock_trace_is(LOCK_READY, LOCK_RUNNING));
    return 1;
}

static int test_scan_bound_rejected_without_mutation(void)
{
    const unsigned int count = P4_QUEUE_SCAN_LIMIT + 1u;
    struct ExecBase base;
    struct Task *tasks;
    struct Task *before_tasks;
    struct List before_running;
    struct List before_ready;
    unsigned int i;
    int result;

    memset(&base, 0, sizeof(base));
    init_list(&base.TaskReady);
    init_list(&base.TaskWait);
    init_list(&base.internal.TaskRunning);
    base.internal.TaskReadySpinLock.id = LOCK_READY;
    base.internal.TaskWaitSpinLock.id = LOCK_WAIT;
    base.internal.TaskRunningSpinLock.id = LOCK_RUNNING;
    tasks = (struct Task *)calloc(count, sizeof(*tasks));
    before_tasks = (struct Task *)malloc(count * sizeof(*tasks));
    CHECK(tasks != NULL && before_tasks != NULL);
    for (i = 0; i < count; ++i)
    {
        tasks[i].tc_State = TS_RUN;
        tasks[i].tc_Node.ln_Pri = 0;
        add_tail(&base.internal.TaskRunning, &tasks[i].tc_Node);
    }
    tasks[count - 1u].test_etask = (struct IntETask *)malloc(
        sizeof(*tasks[count - 1u].test_etask));
    CHECK(tasks[count - 1u].test_etask != NULL);
    {
        uint32_t *affinity = (uint32_t *)malloc(sizeof(*affinity));
        CHECK(affinity != NULL);
        *affinity = UINT32_C(1);
        tasks[count - 1u].test_etask->iet_CpuAffinity = affinity;
    }

    before_running = base.internal.TaskRunning;
    before_ready = base.TaskReady;
    memcpy(before_tasks, tasks, count * sizeof(*tasks));
    reset_mocks();
    result = krnP4SignalQueueApplyLocked(&tasks[count - 1u],
        P4_SIGNAL_QUEUE_RUNNING_TO_READY, &base);
    CHECK(result < 0);
    CHECK(memcmp(&before_running, &base.internal.TaskRunning,
                 sizeof(before_running)) == 0);
    CHECK(memcmp(&before_ready, &base.TaskReady, sizeof(before_ready)) == 0);
    CHECK(memcmp(before_tasks, tasks, count * sizeof(*tasks)) == 0);
    CHECK(lock_trace_is(LOCK_READY, LOCK_RUNNING));

    free((void *)tasks[count - 1u].test_etask->iet_CpuAffinity);
    free(tasks[count - 1u].test_etask);
    free(before_tasks);
    free(tasks);
    return 1;
}

int main(void)
{
    if (!test_running_to_ready_priority_hart0() ||
        !test_running_to_wait_order() ||
        !test_waiting_to_ready_priority_hart1() ||
        !test_eligibility_and_default_rejections() ||
        !test_absent_duplicate_and_state_mismatch() ||
        !test_full_list_validation_after_target() ||
        !test_cycle_rejected_without_mutation() ||
        !test_scan_bound_rejected_without_mutation())
        return 1;

    printf("PASS: %u P4 scheduler queue checks\n", checks);
    return 0;
}
