/* Host fixture for the actual P4 task-publication queue transactions. */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TS_INVALID 0u
#define TS_RUN 1u
#define TS_READY 2u
#define TS_ADDED 4u
#define TS_WAIT 3u
#define SPINLOCK_MODE_WRITE 1u
#define MAX_EVENTS 24u

struct Node
{
    struct Node *ln_Succ;
    struct Node *ln_Pred;
};

struct List
{
    struct Node *lh_Head;
    struct Node *lh_Tail;
    struct Node *lh_TailPred;
};

struct ETask
{
    const uint32_t *iet_CpuAffinity;
    unsigned int iet_CpuNumber;
};

struct TestSpinlock
{
    unsigned int id;
};
typedef struct TestSpinlock spinlock_t;

struct Task
{
    struct Node tc_Node;
    unsigned int tc_State;
    struct ETask *test_etask;
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

#define PrivExecBase(base) (&(base)->internal)

static struct ExecBase fixture_base;
static struct Task fixture_task;
static struct ETask fixture_etask;
static uint32_t fixture_affinity;
static unsigned int fixture_cpu_count;
static unsigned int fixture_cpu_count_calls;
static unsigned int fixture_mask_calls;
static unsigned int fixture_etask_reads;
static unsigned int fixture_admission_calls;
static unsigned int fixture_admission_allow;
static unsigned int fixture_running_refs;
static unsigned int fixture_owner_hart;
static unsigned int fixture_enqueue_calls;
static unsigned int fixture_mie;
static int fixture_task_lock_held;
static unsigned int fixture_plan;
static unsigned int fixture_held_ids[3];
static unsigned int fixture_held_count;
static unsigned int fixture_events[MAX_EVENTS];
static unsigned int fixture_event_count;
static unsigned int checks;
static unsigned int failures;

enum
{
    LOCK_READY = 1u,
    LOCK_WAIT = 2u,
    LOCK_RUNNING = 3u,
    PLAN_PUBLICATION = 1u,
    PLAN_BOOT = 2u,
    EV_LOCK_READY = 10u,
    EV_LOCK_WAIT = 11u,
    EV_LOCK_RUNNING = 12u,
    EV_ADMIT_HART0 = 20u,
    EV_ENQUEUE_READY = 30u,
    EV_ENQUEUE_RUNNING = 31u,
    EV_UNLOCK_RUNNING = 40u,
    EV_UNLOCK_WAIT = 41u,
    EV_UNLOCK_READY = 42u
};

#define CHECK(condition)                                                     \
    do                                                                       \
    {                                                                        \
        ++checks;                                                            \
        if (!(condition))                                                    \
        {                                                                    \
            ++failures;                                                     \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,       \
                    #condition);                                             \
        }                                                                    \
    } while (0)

static int mock_lock(spinlock_t *lock, unsigned int mode);
static int mock_unlock(spinlock_t *lock);
static struct ETask *mock_get_etask(struct Task *task);
#define EXEC_SPINLOCK_LOCK(lock, hook, mode) mock_lock((lock), (mode))
#define EXEC_SPINLOCK_UNLOCK(lock) mock_unlock((lock))
#define GetIntETask(task) mock_get_etask((task))

unsigned int krnP4GetCPUCount(void);
int krnP4CPUInMask(uint32_t number, const void *mask);
int krnP4TaskRecordEnterRunning(struct Task *task, unsigned int hart,
                                struct ExecBase *base);
void Remove(struct Node *node);
void Enqueue(struct List *list, struct Node *node);

#define P4_SCHEDULER_QUEUE_TEST 1
#define P4_TASK_PUBLICATION_TEST 1
#include "../p4_scheduler_queue.c"

static void record_event(unsigned int event)
{
    CHECK(fixture_event_count < MAX_EVENTS);
    if (fixture_event_count < MAX_EVENTS)
        fixture_events[fixture_event_count++] = event;
}

static void list_init(struct List *list)
{
    list->lh_Head = (struct Node *)&list->lh_Tail;
    list->lh_Tail = NULL;
    list->lh_TailPred = (struct Node *)&list->lh_Head;
}

static void list_append(struct List *list, struct Node *node)
{
    struct Node *pred = list->lh_TailPred;
    node->ln_Succ = (struct Node *)&list->lh_Tail;
    node->ln_Pred = pred;
    pred->ln_Succ = node;
    list->lh_TailPred = node;
}

static void list_remove(struct Node *node)
{
    node->ln_Pred->ln_Succ = node->ln_Succ;
    node->ln_Succ->ln_Pred = node->ln_Pred;
    node->ln_Succ = NULL;
    node->ln_Pred = NULL;
}

static int list_has(const struct List *list, const struct Task *task)
{
    const struct Node *tail = (const struct Node *)&list->lh_Tail;
    const struct Node *node = list->lh_Head;
    unsigned int visited = 0u;
    while (node != NULL && node != tail && visited++ < 8u)
    {
        if (node == &task->tc_Node)
            return 1;
        node = node->ln_Succ;
    }
    return 0;
}

static void reset_fixture(void)
{
    memset(&fixture_base, 0, sizeof(fixture_base));
    memset(&fixture_task, 0, sizeof(fixture_task));
    memset(&fixture_etask, 0, sizeof(fixture_etask));
    fixture_affinity = UINT32_C(3);
    fixture_task.tc_State = TS_ADDED;
    fixture_task.test_etask = &fixture_etask;
    fixture_etask.iet_CpuAffinity = &fixture_affinity;
    fixture_etask.iet_CpuNumber = 0u;
    list_init(&fixture_base.TaskReady);
    list_init(&fixture_base.TaskWait);
    list_init(&fixture_base.internal.TaskRunning);
    fixture_base.internal.TaskReadySpinLock.id = LOCK_READY;
    fixture_base.internal.TaskWaitSpinLock.id = LOCK_WAIT;
    fixture_base.internal.TaskRunningSpinLock.id = LOCK_RUNNING;
    fixture_cpu_count = 2u;
    fixture_cpu_count_calls = 0u;
    fixture_mask_calls = 0u;
    fixture_etask_reads = 0u;
    fixture_admission_calls = 0u;
    fixture_admission_allow = 1u;
    fixture_running_refs = 0u;
    fixture_owner_hart = UINT32_MAX;
    fixture_enqueue_calls = 0u;
    fixture_mie = 0u;
    fixture_task_lock_held = 1;
    fixture_plan = PLAN_PUBLICATION;
    fixture_held_count = 0u;
    fixture_event_count = 0u;
}

static struct ETask *mock_get_etask(struct Task *task)
{
    ++fixture_etask_reads;
    CHECK(task == &fixture_task);
    CHECK(fixture_mie == 0u);
    CHECK(fixture_task_lock_held);
    CHECK(fixture_held_count == 0u);
    return task->test_etask;
}

unsigned int krnP4GetCPUCount(void)
{
    ++fixture_cpu_count_calls;
    CHECK(fixture_mie == 0u);
    CHECK(fixture_task_lock_held);
    CHECK(fixture_held_count == 0u);
    return fixture_cpu_count;
}

int krnP4CPUInMask(uint32_t number, const void *mask)
{
    const uint32_t *bits = (const uint32_t *)mask;
    ++fixture_mask_calls;
    CHECK(fixture_mie == 0u);
    CHECK(fixture_task_lock_held);
    CHECK(fixture_held_count == 0u);
    return bits != NULL && number < 32u &&
           ((*bits & (UINT32_C(1) << number)) != 0u);
}

static int mock_lock(spinlock_t *lock, unsigned int mode)
{
    unsigned int expected;
    CHECK(lock != NULL);
    CHECK(mode == SPINLOCK_MODE_WRITE);
    CHECK(fixture_mie == 0u);
    CHECK(fixture_task_lock_held);
    if (fixture_plan == PLAN_PUBLICATION)
    {
        expected = fixture_held_count == 0u ? LOCK_READY :
                   fixture_held_count == 1u ? LOCK_WAIT : LOCK_RUNNING;
        CHECK(fixture_held_count < 3u);
    }
    else
    {
        expected = LOCK_RUNNING;
        CHECK(fixture_held_count == 0u);
    }
    CHECK(lock->id == expected);
    if (fixture_held_count < 3u)
        fixture_held_ids[fixture_held_count++] = lock->id;
    if (lock->id == LOCK_READY)
        record_event(EV_LOCK_READY);
    else if (lock->id == LOCK_WAIT)
        record_event(EV_LOCK_WAIT);
    else if (lock->id == LOCK_RUNNING)
        record_event(EV_LOCK_RUNNING);
    return 0;
}

static int mock_unlock(spinlock_t *lock)
{
    CHECK(lock != NULL);
    CHECK(fixture_mie == 0u);
    CHECK(fixture_task_lock_held);
    CHECK(fixture_held_count != 0u);
    if (lock != NULL && fixture_held_count != 0u)
    {
        CHECK(fixture_held_ids[fixture_held_count - 1u] == lock->id);
        --fixture_held_count;
    }
    if (lock != NULL && lock->id == LOCK_RUNNING)
        record_event(EV_UNLOCK_RUNNING);
    else if (lock != NULL && lock->id == LOCK_WAIT)
        record_event(EV_UNLOCK_WAIT);
    else if (lock != NULL && lock->id == LOCK_READY)
        record_event(EV_UNLOCK_READY);
    return 0;
}

int krnP4TaskRecordEnterRunning(struct Task *task, unsigned int hart,
                                struct ExecBase *base)
{
    ++fixture_admission_calls;
    CHECK(task == &fixture_task);
    CHECK(base == &fixture_base);
    CHECK(hart == 0u);
    CHECK(fixture_mie == 0u);
    CHECK(fixture_task_lock_held);
    CHECK(fixture_task.tc_State == TS_INVALID);
    CHECK(fixture_held_count == 1u);
    CHECK(fixture_held_ids[0] == LOCK_RUNNING);
    CHECK(!list_has(&base->internal.TaskRunning, task));
    CHECK(fixture_running_refs == 0u);
    if (!fixture_admission_allow)
        return 0;
    /* No later operation in BootTaskRunningLocked can fail. */
    fixture_running_refs = 1u;
    fixture_owner_hart = hart;
    record_event(EV_ADMIT_HART0);
    return 1;
}

void Remove(struct Node *node)
{
    list_remove(node);
}

void Enqueue(struct List *list, struct Node *node)
{
    struct Task *task = (struct Task *)node;
    ++fixture_enqueue_calls;
    CHECK(fixture_mie == 0u);
    CHECK(fixture_task_lock_held);
    if (list == &fixture_base.TaskReady)
    {
        CHECK(fixture_plan == PLAN_PUBLICATION);
        CHECK(fixture_held_count == 3u);
        CHECK(fixture_held_ids[0] == LOCK_READY);
        CHECK(fixture_held_ids[1] == LOCK_WAIT);
        CHECK(fixture_held_ids[2] == LOCK_RUNNING);
        CHECK(task->tc_State == TS_READY);
        CHECK(fixture_running_refs == 0u);
        record_event(EV_ENQUEUE_READY);
    }
    else if (list == &fixture_base.internal.TaskRunning)
    {
        CHECK(fixture_plan == PLAN_BOOT);
        CHECK(fixture_held_count == 1u);
        CHECK(fixture_held_ids[0] == LOCK_RUNNING);
        CHECK(task->tc_State == TS_RUN);
        CHECK(fixture_running_refs == 1u);
        CHECK(fixture_owner_hart == 0u);
        record_event(EV_ENQUEUE_RUNNING);
    }
    else
        CHECK(0 && "publication helper enqueued into an unexpected list");
    list_append(list, node);
}

struct QueueSnapshot
{
    struct List ready;
    struct List wait;
    struct List running;
    struct Node node;
    unsigned int state;
    unsigned int running_refs;
    unsigned int owner_hart;
};

static struct QueueSnapshot snapshot(void)
{
    struct QueueSnapshot result;
    result.ready = fixture_base.TaskReady;
    result.wait = fixture_base.TaskWait;
    result.running = fixture_base.internal.TaskRunning;
    result.node = fixture_task.tc_Node;
    result.state = fixture_task.tc_State;
    result.running_refs = fixture_running_refs;
    result.owner_hart = fixture_owner_hart;
    return result;
}

static int same_list(const struct List *a, const struct List *b)
{
    return a->lh_Head == b->lh_Head && a->lh_Tail == b->lh_Tail &&
           a->lh_TailPred == b->lh_TailPred;
}

static void check_unchanged(const struct QueueSnapshot *before)
{
    CHECK(fixture_task.tc_State == before->state);
    CHECK(fixture_task.tc_Node.ln_Succ == before->node.ln_Succ);
    CHECK(fixture_task.tc_Node.ln_Pred == before->node.ln_Pred);
    CHECK(same_list(&fixture_base.TaskReady, &before->ready));
    CHECK(same_list(&fixture_base.TaskWait, &before->wait));
    CHECK(same_list(&fixture_base.internal.TaskRunning, &before->running));
    CHECK(fixture_running_refs == before->running_refs);
    CHECK(fixture_owner_hart == before->owner_hart);
}

static void check_publication_lock_sequence(int enqueued)
{
    static const unsigned int expected[] = {
        EV_LOCK_READY, EV_LOCK_WAIT, EV_LOCK_RUNNING,
        EV_ENQUEUE_READY, EV_UNLOCK_RUNNING, EV_UNLOCK_WAIT, EV_UNLOCK_READY
    };
    static const unsigned int expected_without_enqueue[] = {
        EV_LOCK_READY, EV_LOCK_WAIT, EV_LOCK_RUNNING,
        EV_UNLOCK_RUNNING, EV_UNLOCK_WAIT, EV_UNLOCK_READY
    };
    unsigned int i;
    const unsigned int *sequence = enqueued ? expected : expected_without_enqueue;
    unsigned int length = enqueued ?
        (unsigned int)(sizeof(expected) / sizeof(expected[0])) :
        (unsigned int)(sizeof(expected_without_enqueue) /
                       sizeof(expected_without_enqueue[0]));
    CHECK(fixture_event_count == length);
    for (i = 0; i < fixture_event_count && i < length; ++i)
        CHECK(fixture_events[i] == sequence[i]);
}

static void test_new_task_ready_publication(void)
{
    int result;
    reset_fixture();
    fixture_affinity = UINT32_C(2); /* hart 1 only; result is hart+1 */
    fixture_plan = PLAN_PUBLICATION;
    result = krnP4NewTaskReadyLocked(&fixture_task, &fixture_base);
    CHECK(result == 2);
    CHECK(fixture_task.tc_State == TS_READY);
    CHECK(list_has(&fixture_base.TaskReady, &fixture_task));
    CHECK(!list_has(&fixture_base.TaskWait, &fixture_task));
    CHECK(!list_has(&fixture_base.internal.TaskRunning, &fixture_task));
    CHECK(fixture_running_refs == 0u);
    CHECK(fixture_admission_calls == 0u);
    CHECK(fixture_enqueue_calls == 1u);
    CHECK(fixture_held_count == 0u);
    CHECK(fixture_mie == 0u && fixture_task_lock_held);
    CHECK(fixture_cpu_count_calls == 1u);
    CHECK(fixture_mask_calls == 2u);
    CHECK(fixture_etask_reads == 3u);
    check_publication_lock_sequence(1);
}

static void test_new_task_bad_state_and_null_arguments(void)
{
    struct QueueSnapshot before;
    reset_fixture();
    fixture_task.tc_State = TS_READY;
    before = snapshot();
    CHECK(krnP4NewTaskReadyLocked(&fixture_task, &fixture_base) == 0);
    check_unchanged(&before);
    CHECK(fixture_cpu_count_calls == 0u);
    CHECK(fixture_held_count == 0u && fixture_event_count == 0u);

    fixture_task.tc_State = TS_ADDED;
    before = snapshot();
    CHECK(krnP4NewTaskReadyLocked(NULL, &fixture_base) == 0);
    CHECK(krnP4NewTaskReadyLocked(&fixture_task, NULL) == 0);
    check_unchanged(&before);
    CHECK(fixture_held_count == 0u && fixture_event_count == 0u);
}

static void test_new_task_affinity_and_cpu_count_refusals(void)
{
    const unsigned int counts[] = {0u, 3u};
    unsigned int i;
    reset_fixture();
    fixture_affinity = 0u;
    {
        struct QueueSnapshot before = snapshot();
        CHECK(krnP4NewTaskReadyLocked(&fixture_task, &fixture_base) == 0);
        check_unchanged(&before);
    }
    CHECK(fixture_cpu_count_calls == 1u);
    CHECK(fixture_mask_calls == 2u);
    CHECK(fixture_held_count == 0u && fixture_event_count == 0u);

    for (i = 0; i < (unsigned int)(sizeof(counts) / sizeof(counts[0])); ++i)
    {
        struct QueueSnapshot before;
        reset_fixture();
        fixture_cpu_count = counts[i];
        before = snapshot();
        CHECK(krnP4NewTaskReadyLocked(&fixture_task, &fixture_base) == 0);
        check_unchanged(&before);
        CHECK(fixture_cpu_count_calls == 1u);
        CHECK(fixture_mask_calls == 0u);
        CHECK(fixture_held_count == 0u && fixture_event_count == 0u);
    }
}

static struct List *select_list(unsigned int which)
{
    if (which == 0u)
        return &fixture_base.TaskReady;
    if (which == 1u)
        return &fixture_base.TaskWait;
    return &fixture_base.internal.TaskRunning;
}

static void test_new_task_duplicate_in_each_queue(void)
{
    unsigned int which;
    for (which = 0; which < 3u; ++which)
    {
        struct QueueSnapshot before;
        reset_fixture();
        list_append(select_list(which), &fixture_task.tc_Node);
        before = snapshot();
        CHECK(krnP4NewTaskReadyLocked(&fixture_task, &fixture_base) == 0);
        check_unchanged(&before);
        CHECK(fixture_enqueue_calls == 0u);
        CHECK(fixture_admission_calls == 0u);
        CHECK(fixture_held_count == 0u);
        check_publication_lock_sequence(0);
    }
}

static void test_new_task_corrupt_queue_refusal(void)
{
    unsigned int which;
    for (which = 0; which < 3u; ++which)
    {
        struct QueueSnapshot before;
        struct List *list;
        reset_fixture();
        list = select_list(which);
        if (which == 0u)
            list->lh_TailPred = &fixture_task.tc_Node;
        else
            list->lh_Tail = &fixture_task.tc_Node;
        before = snapshot();
        CHECK(krnP4NewTaskReadyLocked(&fixture_task, &fixture_base) == 0);
        check_unchanged(&before);
        CHECK(fixture_enqueue_calls == 0u);
        CHECK(fixture_admission_calls == 0u);
        CHECK(fixture_held_count == 0u);
        check_publication_lock_sequence(0);
    }
}

static void test_boot_running_atomic_owner_and_no_etask_read(void)
{
    reset_fixture();
    fixture_plan = PLAN_BOOT;
    fixture_task.tc_State = TS_INVALID;
    fixture_task.test_etask = NULL; /* boot helper must not inspect ETask */
    CHECK(krnP4BootTaskRunningLocked(&fixture_task, &fixture_base) == 1);
    CHECK(fixture_task.tc_State == TS_RUN);
    CHECK(list_has(&fixture_base.internal.TaskRunning, &fixture_task));
    CHECK(!list_has(&fixture_base.TaskReady, &fixture_task));
    CHECK(!list_has(&fixture_base.TaskWait, &fixture_task));
    CHECK(fixture_owner_hart == 0u);
    CHECK(fixture_running_refs == 1u);
    CHECK(fixture_admission_calls == 1u);
    CHECK(fixture_etask_reads == 0u);
    CHECK(fixture_enqueue_calls == 1u);
    CHECK(fixture_held_count == 0u);
    CHECK(fixture_event_count == 4u);
    CHECK(fixture_events[0] == EV_LOCK_RUNNING);
    CHECK(fixture_events[1] == EV_ADMIT_HART0);
    CHECK(fixture_events[2] == EV_ENQUEUE_RUNNING);
    CHECK(fixture_events[3] == EV_UNLOCK_RUNNING);

    /* A second bootstrap attempt cannot reserve or publish another owner. */
    CHECK(krnP4BootTaskRunningLocked(&fixture_task, &fixture_base) == 0);
    CHECK(fixture_running_refs == 1u);
    CHECK(fixture_owner_hart == 0u);
    CHECK(fixture_admission_calls == 1u);
    CHECK(fixture_event_count == 4u);
}

static void test_boot_admission_refusal_and_bad_state(void)
{
    unsigned int state;
    reset_fixture();
    fixture_plan = PLAN_BOOT;
    fixture_task.tc_State = TS_INVALID;
    fixture_task.test_etask = NULL;
    fixture_admission_allow = 0u;
    {
        struct QueueSnapshot before = snapshot();
        CHECK(krnP4BootTaskRunningLocked(&fixture_task, &fixture_base) == 0);
        check_unchanged(&before);
    }
    CHECK(fixture_admission_calls == 1u);
    CHECK(fixture_enqueue_calls == 0u);
    CHECK(fixture_etask_reads == 0u);
    CHECK(fixture_event_count == 2u);
    CHECK(fixture_events[0] == EV_LOCK_RUNNING);
    CHECK(fixture_events[1] == EV_UNLOCK_RUNNING);

    for (state = TS_ADDED; state <= TS_RUN; ++state)
    {
        struct QueueSnapshot before;
        reset_fixture();
        fixture_plan = PLAN_BOOT;
        fixture_task.tc_State = state;
        fixture_task.test_etask = NULL;
        before = snapshot();
        CHECK(krnP4BootTaskRunningLocked(&fixture_task, &fixture_base) == 0);
        check_unchanged(&before);
        CHECK(fixture_admission_calls == 0u);
        CHECK(fixture_etask_reads == 0u);
        CHECK(fixture_held_count == 0u && fixture_event_count == 0u);
    }
}

static void test_boot_running_membership_and_corruption_refusal(void)
{
    unsigned int mode;
    for (mode = 0; mode < 2u; ++mode)
    {
        struct QueueSnapshot before;
        reset_fixture();
        fixture_plan = PLAN_BOOT;
        fixture_task.tc_State = TS_INVALID;
        fixture_task.test_etask = NULL;
        if (mode == 0u)
            list_append(&fixture_base.internal.TaskRunning,
                        &fixture_task.tc_Node);
        else
            fixture_base.internal.TaskRunning.lh_TailPred =
                &fixture_task.tc_Node;
        before = snapshot();
        CHECK(krnP4BootTaskRunningLocked(&fixture_task, &fixture_base) == 0);
        check_unchanged(&before);
        CHECK(fixture_admission_calls == 0u);
        CHECK(fixture_enqueue_calls == 0u);
        CHECK(fixture_etask_reads == 0u);
        CHECK(fixture_event_count == 2u);
        CHECK(fixture_events[0] == EV_LOCK_RUNNING);
        CHECK(fixture_events[1] == EV_UNLOCK_RUNNING);
        CHECK(fixture_held_count == 0u);
    }
}

int main(void)
{
    test_new_task_ready_publication();
    test_new_task_bad_state_and_null_arguments();
    test_new_task_affinity_and_cpu_count_refusals();
    test_new_task_duplicate_in_each_queue();
    test_new_task_corrupt_queue_refusal();
    test_boot_running_atomic_owner_and_no_etask_read();
    test_boot_admission_refusal_and_bad_state();
    test_boot_running_membership_and_corruption_refusal();
    if (failures != 0u)
    {
        fprintf(stderr, "%u/%u checks failed\n", failures, checks);
        return EXIT_FAILURE;
    }
    printf("PASS: P4 task publication queue actual-source fixture (%u checks)\n",
           checks);
    return EXIT_SUCCESS;
}
