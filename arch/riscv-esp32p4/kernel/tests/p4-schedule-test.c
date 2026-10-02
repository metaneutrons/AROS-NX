/* Host fixture for the actual P4 scheduler decision helper. */
#include <setjmp.h>
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

typedef int8_t BYTE;

struct TestSpinlock
{
    unsigned int id;
};
typedef struct TestSpinlock spinlock_t;

struct Task
{
    struct Node tc_Node;
    uint8_t tc_Flags;
    uint8_t tc_State;
    spinlock_t tc_SpinLock;
};

struct ExecBase
{
    struct List TaskReady;
};

struct IntExecBase
{
    struct ExecBase pub;
    spinlock_t TaskReadySpinLock;
};

struct p4_cpu_local
{
    uint32_t hart_id;
    struct Task *current_task;
    uint32_t trap_depth;
    uint32_t schedule_flags;
};

struct p4_exec_runtime
{
    uint32_t hart_id;
    struct p4_cpu_local *cpu_local;
};

#define PrivExecBase(base) ((struct IntExecBase *)(base))
#define SPINLOCK_MODE_WRITE 1u
#define P4_EXEC_RUNTIME_HART_COUNT 2u
#define P4_EXEC_SCHEDULE_QUANTUM (UINT32_C(1) << 0)
#define P4_EXEC_SCHEDULE_SWITCH  (UINT32_C(1) << 1)
#define TS_INVALID    0u
#define TS_RUN        2u
#define TS_READY      3u
#define TS_REMOVED    6u
#define TS_TOMBSTONED 7u
#define TF_EXCEPT     (UINT8_C(1) << 5)
#define MSTATUS_MIE   UINT32_C(8)
#define mstatus       0

enum lock_id
{
    LOCK_TASK = 1,
    LOCK_READY = 2
};

enum lock_event
{
    EVENT_TASK_LOCK = 10,
    EVENT_READY_LOCK = 20,
    EVENT_READY_UNLOCK = 30,
    EVENT_TASK_UNLOCK = 40
};

#define MAX_LOCK_EVENTS 8u
#define MAX_HELD_LOCKS 4u
#define TEST_QUEUE_LIMIT 20000u
#define TEST_LARGE_QUEUE (TEST_QUEUE_LIMIT + 1u)
#define CHECK(condition) do { \
    ++checks; \
    if (!(condition)) { \
        ++failures; \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
    } \
} while (0)

static unsigned int checks;
static unsigned int failures;
static unsigned int mock_mstatus;
static unsigned int mock_lock_events[MAX_LOCK_EVENTS];
static unsigned int mock_lock_event_count;
static spinlock_t *mock_held_locks[MAX_HELD_LOCKS];
static unsigned int mock_held_count;
static struct ExecBase *mock_sys_base;
static struct p4_exec_runtime *mock_runtime;
static struct Task *mock_locked_task;
static jmp_buf fatal_jump;
static int fatal_armed;
static unsigned int fatal_count;
static int fatal_reason;
static unsigned int priority_reads;

static int mock_lock(spinlock_t *lock, unsigned int mode);
static int mock_unlock(spinlock_t *lock);
static int8_t mock_priority(const struct Task *task);

#define P4_PUBLIC_SPINLOCK_TEST 1
#define P4_PUBLIC_SPINLOCK_TEST_TYPES 1
#include "../p4_public_spinlock.h"

struct p4_exec_runtime *p4_exec_runtime_current(struct ExecBase *sys_base)
{
    return sys_base == mock_sys_base ? mock_runtime : NULL;
}

static int mock_lock(spinlock_t *lock, unsigned int mode)
{
    CHECK(lock != NULL);
    CHECK(mode == SPINLOCK_MODE_WRITE);
    CHECK((mock_mstatus & MSTATUS_MIE) == 0u);
    CHECK(mock_lock_event_count < MAX_LOCK_EVENTS);
    CHECK(mock_held_count < MAX_HELD_LOCKS);

    if (lock == &mock_locked_task->tc_SpinLock)
    {
        CHECK(mock_held_count == 0u);
        CHECK(lock->id == LOCK_TASK);
        if (mock_lock_event_count < MAX_LOCK_EVENTS)
            mock_lock_events[mock_lock_event_count++] = EVENT_TASK_LOCK;
    }
    else if (mock_sys_base != NULL &&
             lock == &PrivExecBase(mock_sys_base)->TaskReadySpinLock)
    {
        CHECK(mock_held_count == 1u);
        CHECK(mock_held_locks[0] == &mock_locked_task->tc_SpinLock);
        CHECK(lock->id == LOCK_READY);
        if (mock_lock_event_count < MAX_LOCK_EVENTS)
            mock_lock_events[mock_lock_event_count++] = EVENT_READY_LOCK;
    }
    else
    {
        CHECK(0 && "unexpected lock");
    }

    if (mock_held_count < MAX_HELD_LOCKS)
        mock_held_locks[mock_held_count++] = lock;
    return 0;
}

static int mock_unlock(spinlock_t *lock)
{
    CHECK(lock != NULL);
    CHECK((mock_mstatus & MSTATUS_MIE) == 0u);
    CHECK(mock_held_count != 0u);
    if (lock == &mock_locked_task->tc_SpinLock)
    {
        CHECK(mock_held_count == 1u);
        CHECK(mock_held_locks[mock_held_count - 1u] == lock);
        if (mock_lock_event_count < MAX_LOCK_EVENTS)
            mock_lock_events[mock_lock_event_count++] = EVENT_TASK_UNLOCK;
    }
    else if (mock_sys_base != NULL &&
             lock == &PrivExecBase(mock_sys_base)->TaskReadySpinLock)
    {
        CHECK(mock_held_count == 2u);
        CHECK(mock_held_locks[mock_held_count - 1u] == lock);
        if (mock_lock_event_count < MAX_LOCK_EVENTS)
            mock_lock_events[mock_lock_event_count++] = EVENT_READY_UNLOCK;
    }
    else
    {
        CHECK(0 && "unexpected unlock");
    }

    if (mock_held_count != 0u)
        --mock_held_count;
    return 0;
}

static int8_t mock_priority(const struct Task *task)
{
    ++priority_reads;
    return task->tc_Node.ln_Pri;
}

#define csr_read(reg) (mock_mstatus)
#define EXEC_SPINLOCK_LOCK(lock, hook, mode) mock_lock((lock), (mode))
#define EXEC_SPINLOCK_UNLOCK(lock) mock_unlock((lock))
#define P4_SCHEDULE_TEST 1
#define P4_SCHEDULE_PRIORITY(task) mock_priority((task))
#include "../p4_schedule.c"

void krnP4PublicSpinFatal(enum p4_public_spinlock_result reason)
    __attribute__((noreturn));
void krnP4PublicSpinFatal(enum p4_public_spinlock_result reason)
{
    ++fatal_count;
    fatal_reason = (int)reason;
    if (fatal_armed)
        longjmp(fatal_jump, 1);
    abort();
}

static void init_list(struct List *list)
{
    list->lh_Head = (struct Node *)&list->lh_Tail;
    list->lh_Tail = NULL;
    list->lh_TailPred = (struct Node *)&list->lh_Head;
    list->lh_Type = 0;
    list->l_pad = 0;
}

static void add_tail(struct List *list, struct Node *node)
{
    struct Node *tail = (struct Node *)&list->lh_Tail;
    node->ln_Succ = tail;
    node->ln_Pred = list->lh_TailPred;
    list->lh_TailPred->ln_Succ = node;
    list->lh_TailPred = node;
}

static void reset_observers(void)
{
    memset(mock_lock_events, 0, sizeof(mock_lock_events));
    memset(mock_held_locks, 0, sizeof(mock_held_locks));
    mock_lock_event_count = 0;
    mock_held_count = 0;
    fatal_count = 0;
    fatal_reason = -1;
    priority_reads = 0;
}

struct Fixture
{
    struct IntExecBase base;
    struct p4_exec_runtime runtimes[2];
    struct p4_cpu_local locals[2];
    struct Task tasks[3];
};

static void init_fixture(struct Fixture *fixture, unsigned int hart)
{
    unsigned int i;
    memset(fixture, 0, sizeof(*fixture));
    init_list(&fixture->base.pub.TaskReady);
    fixture->base.TaskReadySpinLock.id = LOCK_READY;
    for (i = 0; i < 3u; ++i)
    {
        fixture->tasks[i].tc_SpinLock.id = LOCK_TASK;
        fixture->tasks[i].tc_State = TS_RUN;
        fixture->tasks[i].tc_Node.ln_Pri = 10;
    }
    fixture->runtimes[hart].hart_id = hart;
    fixture->runtimes[hart].cpu_local = &fixture->locals[hart];
    fixture->locals[hart].hart_id = hart;
    fixture->locals[hart].current_task = &fixture->tasks[0];
    fixture->locals[hart].trap_depth = 1;
    fixture->locals[hart].schedule_flags = P4_EXEC_SCHEDULE_SWITCH;
    mock_sys_base = &fixture->base.pub;
    mock_runtime = &fixture->runtimes[hart];
    mock_locked_task = &fixture->tasks[0];
    mock_mstatus = 0;
    reset_observers();
}

static int invoke_schedule(void)
{
    return krnP4ShouldSchedule(mock_sys_base);
}

static int catch_fatal(void)
{
    int called = 0;
    fatal_armed = 1;
    if (setjmp(fatal_jump) == 0)
    {
        (void)invoke_schedule();
    }
    else
    {
        called = 1;
    }
    fatal_armed = 0;
    return called;
}

static void test_both_harts_clear_only_local_switch(void)
{
    unsigned int hart;
    for (hart = 0; hart < 2u; ++hart)
    {
        struct Fixture fixture;
        unsigned int other = hart ^ 1u;
        init_fixture(&fixture, hart);
        fixture.locals[other].schedule_flags =
            P4_EXEC_SCHEDULE_SWITCH | P4_EXEC_SCHEDULE_QUANTUM;
        CHECK(invoke_schedule() == 0);
        CHECK((fixture.locals[hart].schedule_flags &
               P4_EXEC_SCHEDULE_SWITCH) == 0u);
        CHECK((fixture.locals[other].schedule_flags &
               P4_EXEC_SCHEDULE_SWITCH) != 0u);
        CHECK(mock_lock_event_count == 4u);
        CHECK(mock_lock_events[0] == EVENT_TASK_LOCK);
        CHECK(mock_lock_events[1] == EVENT_READY_LOCK);
        CHECK(mock_lock_events[2] == EVENT_READY_UNLOCK);
        CHECK(mock_lock_events[3] == EVENT_TASK_UNLOCK);
        CHECK(mock_held_count == 0u);
        CHECK(fatal_count == 0u);
    }
}

static void test_priority_and_quantum_policy(void)
{
    struct Fixture fixture;

    init_fixture(&fixture, 0);
    fixture.tasks[1].tc_Node.ln_Pri = 11;
    add_tail(&fixture.base.pub.TaskReady, &fixture.tasks[1].tc_Node);
    CHECK(invoke_schedule() == 1);
    CHECK(priority_reads == 1u);
    CHECK(mock_lock_event_count == 4u);

    init_fixture(&fixture, 1);
    fixture.tasks[1].tc_Node.ln_Pri = 10;
    add_tail(&fixture.base.pub.TaskReady, &fixture.tasks[1].tc_Node);
    CHECK(invoke_schedule() == 0);
    CHECK(priority_reads == 1u);

    init_fixture(&fixture, 0);
    fixture.locals[0].schedule_flags |= P4_EXEC_SCHEDULE_QUANTUM;
    fixture.tasks[1].tc_Node.ln_Pri = 10;
    add_tail(&fixture.base.pub.TaskReady, &fixture.tasks[1].tc_Node);
    CHECK(invoke_schedule() == 1);
    CHECK(priority_reads == 1u);

    init_fixture(&fixture, 1);
    fixture.locals[1].schedule_flags |= P4_EXEC_SCHEDULE_QUANTUM;
    fixture.tasks[1].tc_Node.ln_Pri = 9;
    add_tail(&fixture.base.pub.TaskReady, &fixture.tasks[1].tc_Node);
    CHECK(invoke_schedule() == 1);
    CHECK(priority_reads == 1u);
}

static void test_exception_removed_and_null_current(void)
{
    static const uint8_t removed_states[] =
        { TS_REMOVED, TS_INVALID, TS_TOMBSTONED };
    struct Fixture fixture;
    unsigned int i;

    init_fixture(&fixture, 0);
    fixture.tasks[0].tc_Flags = TF_EXCEPT;
    CHECK(invoke_schedule() == 1);
    CHECK(mock_lock_event_count == 2u);
    CHECK(mock_lock_events[0] == EVENT_TASK_LOCK);
    CHECK(mock_lock_events[1] == EVENT_TASK_UNLOCK);

    for (i = 0; i < sizeof(removed_states) / sizeof(removed_states[0]); ++i)
    {
        init_fixture(&fixture, 1);
        fixture.tasks[0].tc_State = removed_states[i];
        CHECK(invoke_schedule() == 1);
        CHECK(mock_lock_event_count == 2u);
        CHECK(mock_held_count == 0u);
    }

    init_fixture(&fixture, 0);
    fixture.locals[0].current_task = NULL;
    CHECK(invoke_schedule() == 0);
    CHECK(mock_lock_event_count == 0u);
    CHECK((fixture.locals[0].schedule_flags &
           P4_EXEC_SCHEDULE_SWITCH) == 0u);
}

static void test_bad_task_state_fails_closed(void)
{
    struct Fixture fixture;
    init_fixture(&fixture, 0);
    fixture.tasks[0].tc_State = TS_READY;
    CHECK(catch_fatal());
    CHECK(fatal_count == 1u);
    CHECK(fatal_reason == P4_PUBLIC_SPIN_BAD_STATE);
    CHECK(mock_lock_event_count == 2u);
    CHECK(mock_lock_events[0] == EVENT_TASK_LOCK);
    CHECK(mock_lock_events[1] == EVENT_TASK_UNLOCK);
    CHECK(mock_held_count == 0u);
}

static void test_invalid_runtime_and_trap_fail_closed(void)
{
    struct Fixture fixture;
    struct p4_exec_runtime saved_runtime;

    init_fixture(&fixture, 0);
    saved_runtime = fixture.runtimes[0];
    mock_runtime = NULL;
    CHECK(catch_fatal());
    CHECK(mock_lock_event_count == 0u);
    mock_runtime = &fixture.runtimes[0];

    fixture.runtimes[0].hart_id = 2u;
    CHECK(catch_fatal());
    fixture.runtimes[0] = saved_runtime;

    fixture.runtimes[0].cpu_local = NULL;
    CHECK(catch_fatal());
    fixture.runtimes[0].cpu_local = &fixture.locals[0];

    fixture.locals[0].hart_id = 1u;
    CHECK(catch_fatal());
    fixture.locals[0].hart_id = 0u;

    fixture.locals[0].trap_depth = 0u;
    CHECK(catch_fatal());
    fixture.locals[0].trap_depth = 2u;
    CHECK(catch_fatal());
    fixture.locals[0].trap_depth = 1u;

    mock_mstatus = MSTATUS_MIE;
    CHECK(catch_fatal());
    mock_mstatus = 0u;

    CHECK(mock_lock_event_count == 0u);
    CHECK((fixture.locals[0].schedule_flags &
           P4_EXEC_SCHEDULE_SWITCH) != 0u);
    CHECK(mock_held_count == 0u);
}

static void test_malformed_ready_lists_fail_before_priority(void)
{
    struct Fixture fixture;

    /* A malformed successor after the candidate must still be detected. */
    init_fixture(&fixture, 0);
    add_tail(&fixture.base.pub.TaskReady, &fixture.tasks[1].tc_Node);
    add_tail(&fixture.base.pub.TaskReady, &fixture.tasks[2].tc_Node);
    fixture.tasks[2].tc_Node.ln_Pred = &fixture.tasks[0].tc_Node;
    CHECK(catch_fatal());
    CHECK(priority_reads == 0u);
    CHECK(mock_lock_events[0] == EVENT_TASK_LOCK);
    CHECK(mock_lock_events[1] == EVENT_READY_LOCK);
    CHECK(mock_lock_events[2] == EVENT_READY_UNLOCK);
    CHECK(mock_lock_events[3] == EVENT_TASK_UNLOCK);
    CHECK(mock_held_count == 0u);

    init_fixture(&fixture, 1);
    add_tail(&fixture.base.pub.TaskReady, &fixture.tasks[1].tc_Node);
    fixture.base.pub.TaskReady.lh_TailPred = &fixture.tasks[0].tc_Node;
    CHECK(catch_fatal());
    CHECK(priority_reads == 0u);
    CHECK(mock_held_count == 0u);

    init_fixture(&fixture, 0);
    add_tail(&fixture.base.pub.TaskReady, &fixture.tasks[1].tc_Node);
    fixture.base.pub.TaskReady.lh_Tail = &fixture.tasks[2].tc_Node;
    CHECK(catch_fatal());
    CHECK(priority_reads == 0u);
    CHECK(mock_held_count == 0u);

    init_fixture(&fixture, 1);
    add_tail(&fixture.base.pub.TaskReady, &fixture.tasks[1].tc_Node);
    fixture.tasks[1].tc_Node.ln_Succ = NULL;
    CHECK(catch_fatal());
    CHECK(priority_reads == 0u);
    CHECK(mock_held_count == 0u);

    init_fixture(&fixture, 0);
    add_tail(&fixture.base.pub.TaskReady, &fixture.tasks[1].tc_Node);
    fixture.tasks[1].tc_Node.ln_Succ = &fixture.tasks[1].tc_Node;
    CHECK(catch_fatal());
    CHECK(priority_reads == 0u);
    CHECK(mock_held_count == 0u);
}

static void test_ready_scan_bound(void)
{
    struct Fixture fixture;
    struct Task *tasks;
    unsigned int i;

    init_fixture(&fixture, 0);
    tasks = calloc(TEST_LARGE_QUEUE, sizeof(*tasks));
    CHECK(tasks != NULL);
    if (tasks == NULL)
        return;
    for (i = 0; i < TEST_LARGE_QUEUE; ++i)
    {
        tasks[i].tc_Node.ln_Pri = 127;
        add_tail(&fixture.base.pub.TaskReady, &tasks[i].tc_Node);
    }
    CHECK(catch_fatal());
    CHECK(priority_reads == 0u);
    CHECK(mock_held_count == 0u);
    free(tasks);
}

int main(void)
{
    test_both_harts_clear_only_local_switch();
    test_priority_and_quantum_policy();
    test_exception_removed_and_null_current();
    test_bad_task_state_fails_closed();
    test_invalid_runtime_and_trap_fail_closed();
    test_malformed_ready_lists_fail_before_priority();
    test_ready_scan_bound();

    printf("P4 schedule decision fixture: %u checks, %u failures\n",
           checks, failures);
    return failures == 0u ? 0 : 1;
}
