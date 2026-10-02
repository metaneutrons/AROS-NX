/*
 * Actual-source fixture for krnP4TaskDetachedLocked(). The helper and its
 * private list validator are included verbatim from p4_scheduler_queue.c;
 * only platform lock/Exec bindings are mocked.
 */
#define P4_SCHEDULER_QUEUE_TEST 1
#define P4_TASK_DETACH_TEST 1

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { \
    ++checks; \
    if (!(condition)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        return 0; \
    } \
} while (0)

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
    const void *iet_CpuAffinity;
};

struct Task
{
    struct Node tc_Node;
    unsigned int tc_State;
    struct ETask *test_etask;
};

enum
{
    LOCK_READY = 0,
    LOCK_WAIT = 1,
    LOCK_RUNNING = 2,
    LOCK_COUNT = 3
};

enum
{
    TS_READY = 1,
    TS_WAIT = 2,
    TS_RUN = 3
};

typedef struct
{
    unsigned int id;
    unsigned int held;
} spinlock_t;

struct PrivateExecBase
{
    spinlock_t TaskReadySpinLock;
    spinlock_t TaskWaitSpinLock;
    spinlock_t TaskRunningSpinLock;
    struct List TaskRunning;
};

struct ExecBase
{
    struct List TaskReady;
    struct List TaskWait;
    struct PrivateExecBase private_base;
};

#define PrivExecBase(sys_base) (&(sys_base)->private_base)
#define SPINLOCK_MODE_WRITE 1u
#define GetIntETask(task) ((task)->test_etask)

static struct ExecBase fixture_sysbase;
static struct Task fixture_target;
static struct Task fixture_other_a;
static struct Task fixture_other_b;
static struct ETask fixture_etask;
static struct Node cycle_nodes[3];
static struct Node cycle_nodes_before[3];
static struct Node large_nodes[20001];
static struct Node large_nodes_before[20001];
static unsigned int checks;
static unsigned int lock_order[LOCK_COUNT];
static unsigned int unlock_order[LOCK_COUNT];
static unsigned int lock_calls;
static unsigned int unlock_calls;
static int lock_contract_error;

static spinlock_t *lock_for_id(unsigned int id)
{
    switch (id)
    {
        case LOCK_READY: return &fixture_sysbase.private_base.TaskReadySpinLock;
        case LOCK_WAIT: return &fixture_sysbase.private_base.TaskWaitSpinLock;
        case LOCK_RUNNING: return &fixture_sysbase.private_base.TaskRunningSpinLock;
        default: return NULL;
    }
}

static void fixture_lock(spinlock_t *lock, void *hook, unsigned int mode)
{
    unsigned int expected[] = { LOCK_READY, LOCK_WAIT, LOCK_RUNNING };
    if (hook != NULL || mode != SPINLOCK_MODE_WRITE ||
        lock_calls >= LOCK_COUNT || lock->id != expected[lock_calls] ||
        lock->held != 0)
        lock_contract_error = 1;
    if (lock_calls < LOCK_COUNT)
        lock_order[lock_calls] = lock->id;
    ++lock_calls;
    lock->held = 1;
}

static void fixture_unlock(spinlock_t *lock)
{
    unsigned int expected[] = { LOCK_RUNNING, LOCK_WAIT, LOCK_READY };
    if (unlock_calls >= LOCK_COUNT || lock->held != 1 ||
        lock->id != expected[unlock_calls])
        lock_contract_error = 1;
    if (unlock_calls < LOCK_COUNT)
        unlock_order[unlock_calls] = lock->id;
    ++unlock_calls;
    lock->held = 0;
}

#define EXEC_SPINLOCK_LOCK(lock, hook, mode) \
    fixture_lock((lock), (hook), (mode))
#define EXEC_SPINLOCK_UNLOCK(lock) fixture_unlock((lock))

static void init_list(struct List *list)
{
    list->lh_Head = (struct Node *)&list->lh_Tail;
    list->lh_Tail = NULL;
    list->lh_TailPred = (struct Node *)&list->lh_Head;
}

static void append_node(struct List *list, struct Node *node)
{
    struct Node *tail_pred = list->lh_TailPred;
    node->ln_Succ = (struct Node *)&list->lh_Tail;
    node->ln_Pred = tail_pred;
    tail_pred->ln_Succ = node;
    list->lh_TailPred = node;
}

static void remove_node(struct Node *node)
{
    node->ln_Pred->ln_Succ = node->ln_Succ;
    node->ln_Succ->ln_Pred = node->ln_Pred;
}

static void enqueue_node(struct List *list, struct Node *node)
{
    append_node(list, node);
}

#define Remove(node) remove_node((node))
#define Enqueue(list, node) enqueue_node((list), (node))

unsigned int krnP4GetCPUCount(void)
{
    return 0;
}

int krnP4CPUInMask(unsigned int hart, const void *mask)
{
    (void)hart;
    (void)mask;
    return 0;
}

#include "../p4_scheduler_queue.c"

struct fixture_image
{
    struct List ready;
    struct List wait;
    struct List running;
    struct Node target_node;
    struct Node other_a_node;
    struct Node other_b_node;
    unsigned int target_state;
};

static void reset_fixture(void)
{
    memset(&fixture_sysbase, 0, sizeof(fixture_sysbase));
    memset(&fixture_target, 0, sizeof(fixture_target));
    memset(&fixture_other_a, 0, sizeof(fixture_other_a));
    memset(&fixture_other_b, 0, sizeof(fixture_other_b));
    memset(&fixture_etask, 0, sizeof(fixture_etask));
    init_list(&fixture_sysbase.TaskReady);
    init_list(&fixture_sysbase.TaskWait);
    init_list(&fixture_sysbase.private_base.TaskRunning);
    fixture_sysbase.private_base.TaskReadySpinLock.id = LOCK_READY;
    fixture_sysbase.private_base.TaskWaitSpinLock.id = LOCK_WAIT;
    fixture_sysbase.private_base.TaskRunningSpinLock.id = LOCK_RUNNING;
    fixture_target.test_etask = &fixture_etask;
    lock_calls = 0;
    unlock_calls = 0;
    lock_contract_error = 0;
    memset(lock_order, 0xff, sizeof(lock_order));
    memset(unlock_order, 0xff, sizeof(unlock_order));
}

static void capture_image(struct fixture_image *image)
{
    image->ready = fixture_sysbase.TaskReady;
    image->wait = fixture_sysbase.TaskWait;
    image->running = fixture_sysbase.private_base.TaskRunning;
    image->target_node = fixture_target.tc_Node;
    image->other_a_node = fixture_other_a.tc_Node;
    image->other_b_node = fixture_other_b.tc_Node;
    image->target_state = fixture_target.tc_State;
}

static int image_unchanged(const struct fixture_image *image)
{
    return memcmp(&image->ready, &fixture_sysbase.TaskReady,
                  sizeof(image->ready)) == 0 &&
           memcmp(&image->wait, &fixture_sysbase.TaskWait,
                  sizeof(image->wait)) == 0 &&
           memcmp(&image->running, &fixture_sysbase.private_base.TaskRunning,
                  sizeof(image->running)) == 0 &&
           memcmp(&image->target_node, &fixture_target.tc_Node,
                  sizeof(image->target_node)) == 0 &&
           memcmp(&image->other_a_node, &fixture_other_a.tc_Node,
                  sizeof(image->other_a_node)) == 0 &&
           memcmp(&image->other_b_node, &fixture_other_b.tc_Node,
                  sizeof(image->other_b_node)) == 0 &&
           image->target_state == fixture_target.tc_State;
}

static int lock_trace_is_correct(void)
{
    static const unsigned int acquire[] = {
        LOCK_READY, LOCK_WAIT, LOCK_RUNNING
    };
    static const unsigned int release[] = {
        LOCK_RUNNING, LOCK_WAIT, LOCK_READY
    };
    unsigned int i;
    if (lock_contract_error || lock_calls != LOCK_COUNT ||
        unlock_calls != LOCK_COUNT)
        return 0;
    for (i = 0; i < LOCK_COUNT; ++i)
        if (lock_order[i] != acquire[i] || unlock_order[i] != release[i] ||
            lock_for_id(i)->held != 0)
            return 0;
    return 1;
}

static int expect_checked_result(const char *label, struct Task *task,
                                 struct ExecBase *sys_base, int expected)
{
    struct fixture_image before;
    int result;
    capture_image(&before);
    result = krnP4TaskDetachedLocked(task, sys_base);
    CHECK(result == expected);
    CHECK(image_unchanged(&before));
    if (task != NULL && sys_base != NULL)
        CHECK(lock_trace_is_correct());
    else
    {
        CHECK(lock_calls == 0);
        CHECK(unlock_calls == 0);
        CHECK(!lock_contract_error);
    }
    (void)label;
    return 1;
}

static int test_empty_and_unrelated_membership(void)
{
    reset_fixture();
    CHECK(expect_checked_result("all empty", &fixture_target,
                                &fixture_sysbase, 1));

    reset_fixture();
    append_node(&fixture_sysbase.TaskReady, &fixture_other_a.tc_Node);
    append_node(&fixture_sysbase.TaskWait, &fixture_other_b.tc_Node);
    CHECK(expect_checked_result("unrelated ready/wait", &fixture_target,
                                &fixture_sysbase, 1));

    reset_fixture();
    append_node(&fixture_sysbase.private_base.TaskRunning,
                &fixture_other_a.tc_Node);
    CHECK(expect_checked_result("unrelated running", &fixture_target,
                                &fixture_sysbase, 1));
    return 1;
}

static int test_membership_rejected(void)
{
    reset_fixture();
    append_node(&fixture_sysbase.TaskReady, &fixture_target.tc_Node);
    CHECK(expect_checked_result("target on ready", &fixture_target,
                                &fixture_sysbase, 0));

    reset_fixture();
    append_node(&fixture_sysbase.TaskWait, &fixture_target.tc_Node);
    CHECK(expect_checked_result("target on wait", &fixture_target,
                                &fixture_sysbase, 0));

    reset_fixture();
    append_node(&fixture_sysbase.private_base.TaskRunning,
                &fixture_target.tc_Node);
    CHECK(expect_checked_result("target on running", &fixture_target,
                                &fixture_sysbase, 0));
    return 1;
}

static int test_null_inputs(void)
{
    reset_fixture();
    CHECK(expect_checked_result("null task", NULL, &fixture_sysbase, 0));

    reset_fixture();
    CHECK(expect_checked_result("null SysBase", &fixture_target, NULL, 0));
    return 1;
}

static int test_malformed_lists(void)
{
    reset_fixture();
    fixture_sysbase.TaskReady.lh_Tail = (struct Node *)(uintptr_t)1u;
    CHECK(expect_checked_result("non-null tail", &fixture_target,
                                &fixture_sysbase, 0));

    reset_fixture();
    fixture_sysbase.TaskWait.lh_TailPred = &fixture_other_a.tc_Node;
    CHECK(expect_checked_result("bad tail predecessor", &fixture_target,
                                &fixture_sysbase, 0));

    reset_fixture();
    fixture_other_a.tc_Node.ln_Succ =
        (struct Node *)&fixture_sysbase.private_base.TaskRunning.lh_Tail;
    fixture_other_a.tc_Node.ln_Pred = &fixture_other_b.tc_Node;
    fixture_sysbase.private_base.TaskRunning.lh_Head =
        &fixture_other_a.tc_Node;
    fixture_sysbase.private_base.TaskRunning.lh_TailPred =
        &fixture_other_a.tc_Node;
    CHECK(expect_checked_result("bad backlink", &fixture_target,
                                &fixture_sysbase, 0));

    reset_fixture();
    fixture_other_a.tc_Node.ln_Pred =
        (struct Node *)&fixture_sysbase.TaskReady.lh_Head;
    fixture_other_a.tc_Node.ln_Succ = NULL;
    fixture_sysbase.TaskReady.lh_Head = &fixture_other_a.tc_Node;
    fixture_sysbase.TaskReady.lh_TailPred = &fixture_other_a.tc_Node;
    CHECK(expect_checked_result("null successor", &fixture_target,
                                &fixture_sysbase, 0));
    return 1;
}

static int test_consistent_cycle_is_bounded(void)
{
    reset_fixture();
    memset(cycle_nodes, 0, sizeof(cycle_nodes));
    cycle_nodes[0].ln_Pred =
        (struct Node *)&fixture_sysbase.TaskReady.lh_Head;
    cycle_nodes[0].ln_Succ = &cycle_nodes[1];
    cycle_nodes[1].ln_Pred = &cycle_nodes[0];
    cycle_nodes[1].ln_Succ = &cycle_nodes[2];
    cycle_nodes[2].ln_Pred = &cycle_nodes[1];
    cycle_nodes[2].ln_Succ = &cycle_nodes[1];
    fixture_sysbase.TaskReady.lh_Head = &cycle_nodes[0];
    fixture_sysbase.TaskReady.lh_TailPred = &cycle_nodes[2];
    memcpy(cycle_nodes_before, cycle_nodes, sizeof(cycle_nodes));

    CHECK(expect_checked_result("three-node prefix into cycle", &fixture_target,
                                &fixture_sysbase, 0));
    CHECK(memcmp(cycle_nodes, cycle_nodes_before, sizeof(cycle_nodes)) == 0);
    return 1;
}

static int test_over_limit_valid_chain_is_bounded(void)
{
    unsigned int i;
    reset_fixture();
    memset(large_nodes, 0, sizeof(large_nodes));
    for (i = 0; i < 20001u; ++i)
    {
        large_nodes[i].ln_Pred = i == 0
            ? (struct Node *)&fixture_sysbase.TaskReady.lh_Head
            : &large_nodes[i - 1u];
        large_nodes[i].ln_Succ = i == 20000u
            ? (struct Node *)&fixture_sysbase.TaskReady.lh_Tail
            : &large_nodes[i + 1u];
    }
    fixture_sysbase.TaskReady.lh_Head = &large_nodes[0];
    fixture_sysbase.TaskReady.lh_TailPred = &large_nodes[20000];
    memcpy(large_nodes_before, large_nodes, sizeof(large_nodes));

    CHECK(expect_checked_result("over scan limit", &fixture_target,
                                &fixture_sysbase, 0));
    CHECK(memcmp(large_nodes, large_nodes_before, sizeof(large_nodes)) == 0);
    return 1;
}

int main(void)
{
    if (!test_empty_and_unrelated_membership() ||
        !test_membership_rejected() ||
        !test_null_inputs() ||
        !test_malformed_lists() ||
        !test_consistent_cycle_is_bounded() ||
        !test_over_limit_valid_chain_is_bounded())
        return 1;

    printf("P4 TaskDetachedLocked: %u checks passed\n", checks);
    return 0;
}
