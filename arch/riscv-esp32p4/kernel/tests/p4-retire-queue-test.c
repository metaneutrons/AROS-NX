/*
 * Actual-source fixture for krnP4TaskRetireLocked().  The queue helper,
 * private bounded list validator, registry transitions and lifetime state
 * machine are compiled directly from the production sources.  Exec lists,
 * spinlocks, caller Task-lock/IRQ state and the kernel binding are host mocks.
 */
#define P4_SCHEDULER_QUEUE_TEST 1
#define P4_TASK_RETIRE_TEST 1

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../p4_task_registry.h"

static unsigned int checks;

#define CHECK(condition) do { \
    ++checks; \
    if (!(condition)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        abort(); \
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
    uint32_t marker;
    const void *iet_CpuAffinity;
};

struct Task
{
    struct Node tc_Node;
    unsigned int tc_State;
    struct ETask *test_etask;
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

enum
{
    LOCK_READY = 0,
    LOCK_WAIT = 1,
    LOCK_RUNNING = 2,
    LOCK_COUNT = 3
};

enum
{
    TS_INVALID = 0,
    TS_ADDED = 1,
    TS_READY = 2,
    TS_WAIT = 3,
    TS_RUN = 4,
    TS_REMOVED = 5
};

#define PrivExecBase(sys_base) (&(sys_base)->private_base)
#define GetIntETask(task) fixture_get_etask((task))
#define SPINLOCK_MODE_WRITE 1u
#define EXEC_SPINLOCK_LOCK(lock, hook, mode) \
    fixture_lock((lock), (hook), (mode))
#define EXEC_SPINLOCK_UNLOCK(lock) fixture_unlock((lock))

static struct ExecBase fixture_sysbase;
static struct Task fixture_target;
static struct Task fixture_other;
static struct ETask fixture_etask;
static struct p4_task_registry fixture_registry;
static struct p4_task_registry_record fixture_record;
static unsigned int lock_order[LOCK_COUNT];
static unsigned int unlock_order[LOCK_COUNT];
static unsigned int lock_calls;
static unsigned int unlock_calls;
static unsigned int owner_calls;
static int owner_result;
static unsigned int retire_calls;
static unsigned int etask_reads;
static int lock_contract_error;
static int mock_task_lock_held;
static int mock_irq_disabled;
static struct Node cycle_nodes[2];
static struct Node cycle_nodes_before[2];

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
    static const unsigned int expected[] = {
        LOCK_READY, LOCK_WAIT, LOCK_RUNNING
    };
    if (hook != NULL || mode != SPINLOCK_MODE_WRITE || lock == NULL ||
        !mock_task_lock_held || !mock_irq_disabled ||
        lock_calls >= LOCK_COUNT || lock->id != expected[lock_calls] ||
        lock->held != 0)
        lock_contract_error = 1;
    if (lock != NULL)
        lock->held = 1;
    if (lock_calls < LOCK_COUNT)
        lock_order[lock_calls] = lock->id;
    ++lock_calls;
}

static void fixture_unlock(spinlock_t *lock)
{
    static const unsigned int expected[] = {
        LOCK_RUNNING, LOCK_WAIT, LOCK_READY
    };
    if (lock == NULL || !mock_task_lock_held || !mock_irq_disabled ||
        unlock_calls >= LOCK_COUNT || lock->held != 1 ||
        lock->id != expected[unlock_calls])
        lock_contract_error = 1;
    if (unlock_calls < LOCK_COUNT)
        unlock_order[unlock_calls] = lock->id;
    ++unlock_calls;
    if (lock != NULL)
        lock->held = 0;
}

static struct ETask *fixture_get_etask(struct Task *task)
{
    ++etask_reads;
    return task->test_etask;
}

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

static unsigned int fixture_cpu_count(void)
{
    return 2;
}

static int fixture_cpu_in_mask(unsigned int hart, const void *mask)
{
    (void)hart;
    (void)mask;
    return 0;
}

#define krnP4GetCPUCount fixture_cpu_count
#define krnP4CPUInMask fixture_cpu_in_mask

#include "../p4_task_lifetime.c"
#include "../p4_task_registry.c"

static int krnP4TaskRecordRetireForService(struct Task *task,
                                           struct ExecBase *sys_base)
{
    CHECK(task != NULL);
    CHECK(sys_base == &fixture_sysbase);
    CHECK(mock_task_lock_held);
    CHECK(mock_irq_disabled);
    CHECK(fixture_sysbase.private_base.TaskReadySpinLock.held == 1);
    CHECK(fixture_sysbase.private_base.TaskWaitSpinLock.held == 1);
    CHECK(fixture_sysbase.private_base.TaskRunningSpinLock.held == 1);
    ++retire_calls;
    return p4_task_registry_retire_for_service_locked(&fixture_registry, task);
}

static int krnP4TaskRecordRunningOwner(struct Task *task,
                                      struct ExecBase *sys_base)
{
    CHECK(task != NULL);
    CHECK(sys_base == &fixture_sysbase);
    CHECK(mock_task_lock_held);
    CHECK(mock_irq_disabled);
    CHECK(fixture_sysbase.private_base.TaskReadySpinLock.held == 1);
    CHECK(fixture_sysbase.private_base.TaskWaitSpinLock.held == 1);
    CHECK(fixture_sysbase.private_base.TaskRunningSpinLock.held == 1);
    ++owner_calls;
    owner_result = p4_task_registry_running_owner_locked(&fixture_registry,
                                                          task);
    return owner_result;
}

#include "../p4_scheduler_queue.c"

struct fixture_image
{
    struct List ready;
    struct List wait;
    struct List running;
    struct Node target_node;
    struct Node other_node;
    unsigned int target_state;
    struct p4_task_registry registry;
    struct p4_task_registry_record record;
};

static void reset_trace(void)
{
    lock_calls = 0;
    unlock_calls = 0;
    lock_contract_error = 0;
    memset(lock_order, 0xff, sizeof(lock_order));
    memset(unlock_order, 0xff, sizeof(unlock_order));
}

static void reset_fixture(void)
{
    memset(&fixture_sysbase, 0, sizeof(fixture_sysbase));
    memset(&fixture_target, 0, sizeof(fixture_target));
    memset(&fixture_other, 0, sizeof(fixture_other));
    memset(&fixture_etask, 0, sizeof(fixture_etask));
    memset(&fixture_record, 0, sizeof(fixture_record));
    init_list(&fixture_sysbase.TaskReady);
    init_list(&fixture_sysbase.TaskWait);
    init_list(&fixture_sysbase.private_base.TaskRunning);
    fixture_sysbase.private_base.TaskReadySpinLock.id = LOCK_READY;
    fixture_sysbase.private_base.TaskWaitSpinLock.id = LOCK_WAIT;
    fixture_sysbase.private_base.TaskRunningSpinLock.id = LOCK_RUNNING;
    fixture_target.test_etask = &fixture_etask;
    fixture_etask.marker = UINT32_C(0x51a7e7a5);
    p4_task_registry_init(&fixture_registry);
    retire_calls = 0;
    owner_calls = 0;
    owner_result = -99;
    etask_reads = 0;
    mock_task_lock_held = 0;
    mock_irq_disabled = 0;
    reset_trace();
}

static int register_and_pin(unsigned int ordinary_pins)
{
    unsigned int i;
    if (!p4_task_registry_register_locked(&fixture_registry,
                                           &fixture_record,
                                           &fixture_target))
        return 0;
    for (i = 0; i < ordinary_pins; ++i)
        if (!p4_task_registry_pin_locked(&fixture_registry, &fixture_target))
            return 0;
    return 1;
}

static void enqueue_target(unsigned int state)
{
    fixture_target.tc_State = state;
    if (state == TS_READY)
        append_node(&fixture_sysbase.TaskReady, &fixture_target.tc_Node);
    else if (state == TS_WAIT)
        append_node(&fixture_sysbase.TaskWait, &fixture_target.tc_Node);
    else if (state == TS_RUN)
        append_node(&fixture_sysbase.private_base.TaskRunning,
                    &fixture_target.tc_Node);
}

static void capture_image(struct fixture_image *image)
{
    image->ready = fixture_sysbase.TaskReady;
    image->wait = fixture_sysbase.TaskWait;
    image->running = fixture_sysbase.private_base.TaskRunning;
    image->target_node = fixture_target.tc_Node;
    image->other_node = fixture_other.tc_Node;
    image->target_state = fixture_target.tc_State;
    image->registry = fixture_registry;
    image->record = fixture_record;
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
           memcmp(&image->other_node, &fixture_other.tc_Node,
                  sizeof(image->other_node)) == 0 &&
           image->target_state == fixture_target.tc_State &&
           memcmp(&image->registry, &fixture_registry,
                  sizeof(image->registry)) == 0 &&
           memcmp(&image->record, &fixture_record,
                  sizeof(image->record)) == 0;
}

static int list_is_empty(const struct List *list)
{
    return list->lh_Head == (const struct Node *)&list->lh_Tail &&
           list->lh_Tail == NULL &&
           list->lh_TailPred == (const struct Node *)&list->lh_Head;
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

static int call_retire(struct Task *task, struct ExecBase *sys_base)
{
    int result;
    reset_trace();
    if (task != NULL && sys_base != NULL)
    {
        mock_irq_disabled = 1;
        mock_task_lock_held = 1;
    }
    result = krnP4TaskRetireLocked(task, sys_base);
    if (task != NULL && sys_base != NULL)
        CHECK(lock_trace_is_correct());
    else
    {
        CHECK(lock_calls == 0);
        CHECK(unlock_calls == 0);
        CHECK(!lock_contract_error);
    }
    CHECK(fixture_sysbase.private_base.TaskReadySpinLock.held == 0);
    CHECK(fixture_sysbase.private_base.TaskWaitSpinLock.held == 0);
    CHECK(fixture_sysbase.private_base.TaskRunningSpinLock.held == 0);
    mock_task_lock_held = 0;
    mock_irq_disabled = 0;
    return result;
}

static void check_queue_detached(void)
{
    CHECK(list_is_empty(&fixture_sysbase.TaskReady));
    CHECK(list_is_empty(&fixture_sysbase.TaskWait));
    CHECK(list_is_empty(&fixture_sysbase.private_base.TaskRunning));
}

static void check_retire_success(unsigned int task_state,
                                 unsigned int run_hart)
{
    uint32_t expected_running = run_hart < 2u
        ? (UINT32_C(1) << run_hart) : 0u;
    struct p4_task_registry_record *claimed;
    struct p4_task_registry_record after_claim;
    unsigned int i;

    reset_fixture();
    enqueue_target(task_state);
    CHECK(register_and_pin(4)); /* caller + three ordinary notification pins */
    if (run_hart < 2u)
        CHECK(p4_task_registry_enter_running_locked(&fixture_registry,
                                                     &fixture_target,
                                                     run_hart));

    CHECK(call_retire(&fixture_target, &fixture_sysbase) == 1);
    CHECK(fixture_target.tc_State == TS_REMOVED);
    check_queue_detached();
    CHECK(owner_calls == 1u);
    CHECK(retire_calls == 1);
    CHECK(fixture_record.identity == &fixture_target);
    CHECK(fixture_registry.head == &fixture_record);
    CHECK(fixture_registry.count == 1u);
    CHECK(fixture_record.service_pending == 1u);
    CHECK(fixture_record.service_queued == 0u);
    CHECK(fixture_record.lifetime.flags == P4_TASK_LIFETIME_RETIRED);
    CHECK(fixture_record.lifetime.notification_pins == 5u);
    CHECK(fixture_record.lifetime.running_harts == expected_running);
    CHECK(owner_result == (expected_running == 0u ? 0 : (int)run_hart + 1));
    CHECK(etask_reads == 0u);
    CHECK(fixture_etask.marker == UINT32_C(0x51a7e7a5));

    /* The service pin cannot be consumed by an ordinary unpin. */
    for (i = 0; i < 4u; ++i)
        CHECK(p4_task_registry_unpin_locked(&fixture_registry,
                                            &fixture_target));
    CHECK(fixture_record.lifetime.notification_pins == 1u);
    CHECK(!p4_task_registry_unpin_locked(&fixture_registry,
                                         &fixture_target));
    CHECK(fixture_record.lifetime.notification_pins == 1u);
    if (expected_running != 0u)
    {
        struct p4_task_registry_record before = fixture_record;
        struct p4_task_registry registry_before = fixture_registry;
        CHECK(p4_task_registry_claim_service_locked(&fixture_registry,
                                                    &fixture_target) == NULL);
        CHECK(memcmp(&before, &fixture_record, sizeof(before)) == 0);
        CHECK(memcmp(&registry_before, &fixture_registry,
                     sizeof(registry_before)) == 0);
        CHECK(p4_task_registry_leave_running_locked(&fixture_registry,
                                                     &fixture_target,
                                                     run_hart));
        CHECK(fixture_record.lifetime.running_harts == 0u);
    }

    claimed = p4_task_registry_claim_service_locked(&fixture_registry,
                                                    &fixture_target);
    CHECK(claimed == &fixture_record);
    CHECK(fixture_registry.head == NULL);
    CHECK(fixture_registry.count == 0u);
    after_claim = fixture_record;
    CHECK(after_claim.next == NULL);
    CHECK(after_claim.identity == &fixture_target);
    CHECK(after_claim.service_pending == 0u);
    CHECK(after_claim.service_queued == 0u);
    CHECK(after_claim.lifetime.notification_pins == 0u);
    CHECK(after_claim.lifetime.running_harts == 0u);
    CHECK(after_claim.lifetime.flags ==
          (P4_TASK_LIFETIME_RETIRED | P4_TASK_LIFETIME_CLAIMED));
}

static int test_ready_wait_run_and_callback_ready(void)
{
    check_retire_success(TS_READY, 2u);
    check_retire_success(TS_WAIT, 2u);
    /* Running-owner encoding distinguishes hart 0 from hart 1. */
    check_retire_success(TS_RUN, 0u);
    check_retire_success(TS_RUN, 1u);
    /* Signal callback has moved the list/state to READY before its old
     * context releases the RUN owner. Retirement must preserve either owner. */
    check_retire_success(TS_READY, 0u);
    check_retire_success(TS_READY, 1u);
    return 1;
}

static int expect_failure_unchanged(const char *label, int expected_calls)
{
    struct fixture_image before;
    int result;
    capture_image(&before);
    result = call_retire(&fixture_target, &fixture_sysbase);
    CHECK(result == 0);
    CHECK(owner_calls == 1u);
    CHECK(retire_calls == (unsigned int)expected_calls);
    CHECK(image_unchanged(&before));
    CHECK(etask_reads == 0u);
    (void)label;
    return 1;
}

static int test_wrong_state_membership_refused(void)
{
    reset_fixture();
    enqueue_target(TS_READY);
    CHECK(register_and_pin(1));
    fixture_target.tc_State = TS_WAIT;
    CHECK(expect_failure_unchanged("ready node with wait state", 0));

    reset_fixture();
    enqueue_target(TS_WAIT);
    CHECK(register_and_pin(1));
    fixture_target.tc_State = TS_READY;
    CHECK(expect_failure_unchanged("wait node with ready state", 0));

    reset_fixture();
    fixture_target.tc_State = TS_READY;
    CHECK(register_and_pin(1));
    CHECK(expect_failure_unchanged("state without membership", 0));

    reset_fixture();
    enqueue_target(TS_REMOVED);
    CHECK(register_and_pin(1));
    CHECK(expect_failure_unchanged("removed state still queued", 0));
    return 1;
}

static int test_run_membership_requires_running_owner(void)
{
    reset_fixture();
    enqueue_target(TS_RUN);
    CHECK(register_and_pin(1));
    /* Structurally on Running, but the actual registry reports no owner. */
    CHECK(expect_failure_unchanged("RUN without an owner", 0));
    CHECK(owner_result == 0);
    return 1;
}

static int test_malformed_each_queue_refused(void)
{
    reset_fixture();
    enqueue_target(TS_READY);
    CHECK(register_and_pin(1));
    fixture_sysbase.TaskWait.lh_TailPred = &fixture_other.tc_Node;
    CHECK(expect_failure_unchanged("malformed Wait", 0));

    reset_fixture();
    enqueue_target(TS_WAIT);
    CHECK(register_and_pin(1));
    fixture_sysbase.private_base.TaskRunning.lh_Tail =
        (struct Node *)(uintptr_t)1u;
    CHECK(expect_failure_unchanged("malformed Running", 0));

    reset_fixture();
    enqueue_target(TS_READY);
    CHECK(register_and_pin(1));
    fixture_sysbase.TaskReady.lh_Tail =
        (struct Node *)(uintptr_t)1u;
    CHECK(expect_failure_unchanged("malformed Ready", 0));
    return 1;
}

static int test_cycle_refused_without_mutation(void)
{
    reset_fixture();
    enqueue_target(TS_READY);
    CHECK(register_and_pin(1));
    cycle_nodes[0].ln_Pred =
        (struct Node *)&fixture_sysbase.TaskWait.lh_Head;
    cycle_nodes[0].ln_Succ = &cycle_nodes[1];
    cycle_nodes[1].ln_Pred = &cycle_nodes[0];
    cycle_nodes[1].ln_Succ = &cycle_nodes[1];
    fixture_sysbase.TaskWait.lh_Head = &cycle_nodes[0];
    fixture_sysbase.TaskWait.lh_TailPred = &cycle_nodes[1];
    memcpy(cycle_nodes_before, cycle_nodes, sizeof(cycle_nodes));
    CHECK(expect_failure_unchanged("cyclic Wait", 0));
    CHECK(memcmp(cycle_nodes, cycle_nodes_before, sizeof(cycle_nodes)) == 0);
    return 1;
}

static int test_registry_retirement_refusals(void)
{
    reset_fixture();
    enqueue_target(TS_READY);
    CHECK(register_and_pin(1));
    fixture_record.lifetime.flags |= P4_TASK_LIFETIME_RETIRED;
    CHECK(expect_failure_unchanged("already retired owner", 0));
    CHECK(owner_result == -1);

    reset_fixture();
    enqueue_target(TS_WAIT);
    CHECK(register_and_pin(1));
    fixture_record.lifetime.notification_pins = UINT32_MAX;
    CHECK(expect_failure_unchanged("service pin overflow", 1));

    reset_fixture();
    enqueue_target(TS_RUN);
    CHECK(register_and_pin(1));
    fixture_record.lifetime.flags = P4_TASK_LIFETIME_RETIRED;
    fixture_record.service_pending = 1u;
    CHECK(expect_failure_unchanged("service retirement already pending", 0));
    CHECK(owner_result == -1);

    reset_fixture();
    enqueue_target(TS_READY);
    CHECK(register_and_pin(1));
    fixture_registry.count = 2u; /* inconsistent bound: lookup must fail */
    CHECK(expect_failure_unchanged("corrupt owner lookup", 0));
    CHECK(owner_result == -1);

    reset_fixture();
    enqueue_target(TS_WAIT);
    CHECK(register_and_pin(1));
    fixture_record.lifetime.running_harts = 3u; /* corrupt two-owner mask */
    CHECK(expect_failure_unchanged("corrupt running-owner mask", 0));
    CHECK(owner_result == -1);
    return 1;
}

static int test_missing_running_owner_record(void)
{
    reset_fixture();
    enqueue_target(TS_RUN);
    /* Missing record differs from a registered task with owner mask zero. */
    CHECK(expect_failure_unchanged("missing running-owner record", 0));
    CHECK(owner_result == -1);
    return 1;
}

static int test_null_inputs(void)
{
    reset_fixture();
    CHECK(call_retire(NULL, &fixture_sysbase) == 0);
    CHECK(lock_calls == 0u && unlock_calls == 0u && owner_calls == 0u &&
          retire_calls == 0u);

    reset_fixture();
    CHECK(call_retire(&fixture_target, NULL) == 0);
    CHECK(lock_calls == 0u && unlock_calls == 0u && owner_calls == 0u &&
          retire_calls == 0u);
    return 1;
}

int main(void)
{
    if (!test_ready_wait_run_and_callback_ready() ||
        !test_wrong_state_membership_refused() ||
        !test_run_membership_requires_running_owner() ||
        !test_malformed_each_queue_refused() ||
        !test_cycle_refused_without_mutation() ||
        !test_registry_retirement_refusals() ||
        !test_missing_running_owner_record() ||
        !test_null_inputs())
        return EXIT_FAILURE;

    printf("P4 TaskRetireLocked: %u checks passed\n", checks);
    return EXIT_SUCCESS;
}
