/* Host fixture for the actual P4 dispatcher. This is not scheduler activation. */
#include <setjmp.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_TASKS 8u
#define DISPATCH_SCAN_LIMIT 20000u
#define TS_READY 2u
#define TS_RUN 1u
#define TS_REMOVED 5u
#define SPINLOCK_MODE_WRITE 1u
#define P4_PUBLIC_SPIN_BAD_STATE 4u
#define ECF_FPU UINT32_C(0x0001)
#define P4_EXEC_SCHEDULE_QUANTUM (UINT32_C(1) << 0)
#define P4_EXEC_SCHEDULE_SWITCH (UINT32_C(1) << 1)
#define P4_EXEC_SCHEDULE_DISPATCH (UINT32_C(1) << 2)

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

struct ExceptionContext
{
    uintptr_t sp;
    uintptr_t pc;
    uint32_t Flags;
    void *fpuContext;
};

struct ETask
{
    unsigned int iet_CpuNumber;
    const uint32_t *iet_CpuAffinity;
    struct ExceptionContext *et_RegFrame;
};

struct TestSpinlock
{
    unsigned int id;
};
typedef struct TestSpinlock spinlock_t;

struct Task
{
    struct Node tc_Node;
    spinlock_t tc_SpinLock;
    unsigned int tc_State;
    unsigned int tc_IDNestCnt;
    uintptr_t tc_SPReg;
    uintptr_t tc_SPLower;
    uintptr_t tc_SPUpper;
    struct ETask *test_etask;
    void (*tc_Launch)(void);
};

struct IntExecBase
{
    struct List TaskRunning;
    spinlock_t TaskReadySpinLock;
    spinlock_t TaskRunningSpinLock;
};

struct ExecBase
{
    struct List TaskReady;
    struct IntExecBase internal;
};

struct p4_cpu_local
{
    struct Task *current_task;
    unsigned int trap_depth;
    unsigned int schedule_flags;
    unsigned int id_nest;
    unsigned int idle_count;
};

struct p4_exec_runtime
{
    unsigned int hart_id;
    struct p4_cpu_local *cpu_local;
    unsigned int quantum;
    unsigned int elapsed;
    unsigned int dispatch_count;
    struct Task *wait_pending;
};

#define PrivExecBase(base) (&(base)->internal)
#define GetETask(task) ((task)->test_etask)

static struct ExecBase fixture_base;
static struct Task fixture_tasks[MAX_TASKS];
static struct ETask fixture_etasks[MAX_TASKS];
static struct ExceptionContext fixture_contexts[MAX_TASKS];
static uint32_t fixture_affinities[MAX_TASKS];
static unsigned int fixture_live[MAX_TASKS];
static unsigned int fixture_pins[MAX_TASKS];
static unsigned int fixture_owner_hart[MAX_TASKS];
static unsigned int fixture_running_refs[MAX_TASKS];
static unsigned int fixture_registry_allow[MAX_TASKS];
static struct p4_cpu_local fixture_cpu_local;
static struct p4_exec_runtime fixture_runtime;
static struct p4_exec_runtime *fixture_runtime_result;
static unsigned int fixture_admission_calls;
static unsigned int fixture_pin_calls;
static unsigned int fixture_unpin_calls;
static unsigned int fixture_user_callbacks;
static unsigned int fixture_mie;
static unsigned int fixture_held_ids[4];
static unsigned int fixture_held_count;
static int fixture_ready_held;
static int fixture_running_held;
static int fixture_task_held;
static int fixture_fatal_seen;
static unsigned int fixture_fatal_code;
static unsigned int fixture_fatal_lock_count;
static unsigned int fixture_fatal_candidate_pins;
static int fixture_fatal_expect_task_lock;
static int fixture_inject_remove;
static struct Task *fixture_remove_task;
static unsigned int fixture_restart_observed;
static unsigned int fixture_attempts[MAX_TASKS];
static unsigned int fixture_attempt_count;
static unsigned int fixture_transition_order[MAX_TASKS];
static unsigned int fixture_transition_count;
static jmp_buf fixture_fatal_env;
static int fixture_fatal_env_active;
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
                    #condition);                                             \
        }                                                                    \
    } while (0)

static int mock_lock(spinlock_t *lock, unsigned int mode);
static int mock_unlock(spinlock_t *lock);
#define EXEC_SPINLOCK_LOCK(lock, hook, mode) mock_lock((lock), (mode))
#define EXEC_SPINLOCK_UNLOCK(lock) mock_unlock((lock))

struct p4_exec_runtime *p4_exec_runtime_current(struct ExecBase *base);
int p4_exec_runtime_set_current_task(struct ExecBase *base,
                                     struct Task *task);
int krnP4SignalTaskPin(struct Task *task, struct ExecBase *base);
void krnP4SignalTaskUnpin(struct Task *task, struct ExecBase *base);
int krnP4ReadyToRunningLocked(struct Task *task, unsigned int hart,
                              struct ExecBase *base);
void krnP4PublicSpinFatal(unsigned int reason);

#define P4_DISPATCH_TEST 1
#include "../p4_dispatch.c"

static int task_index(const struct Task *task)
{
    unsigned int i;
    for (i = 0; i < MAX_TASKS; ++i)
        if (task == &fixture_tasks[i])
            return (int)i;
    return -1;
}

static void list_init(struct List *list)
{
    list->lh_Head = (struct Node *)&list->lh_Tail;
    list->lh_Tail = NULL;
    list->lh_TailPred = (struct Node *)&list->lh_Head;
}

static void list_add_tail(struct List *list, struct Node *node)
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

static unsigned int list_count(const struct List *list)
{
    const struct Node *tail = (const struct Node *)&list->lh_Tail;
    const struct Node *node = list->lh_Head;
    unsigned int n = 0;
    while (node != NULL && node != tail && n < DISPATCH_SCAN_LIMIT + 1u)
    {
        ++n;
        node = node->ln_Succ;
    }
    return n;
}

static int list_has(const struct List *list, const struct Task *task)
{
    const struct Node *tail = (const struct Node *)&list->lh_Tail;
    const struct Node *node = list->lh_Head;
    unsigned int n = 0;
    while (node != NULL && node != tail && n++ <= DISPATCH_SCAN_LIMIT)
    {
        if (node == &task->tc_Node)
            return 1;
        node = node->ln_Succ;
    }
    return 0;
}

static void reset_fixture(void)
{
    unsigned int i;
    memset(&fixture_base, 0, sizeof(fixture_base));
    memset(fixture_tasks, 0, sizeof(fixture_tasks));
    memset(fixture_etasks, 0, sizeof(fixture_etasks));
    memset(fixture_contexts, 0, sizeof(fixture_contexts));
    memset(fixture_affinities, 0, sizeof(fixture_affinities));
    memset(fixture_live, 0, sizeof(fixture_live));
    memset(fixture_pins, 0, sizeof(fixture_pins));
    memset(fixture_running_refs, 0, sizeof(fixture_running_refs));
    memset(fixture_registry_allow, 0, sizeof(fixture_registry_allow));
    memset(fixture_owner_hart, 0xff, sizeof(fixture_owner_hart));
    memset(&fixture_cpu_local, 0, sizeof(fixture_cpu_local));
    memset(&fixture_runtime, 0, sizeof(fixture_runtime));
    memset(fixture_attempts, 0, sizeof(fixture_attempts));
    memset(fixture_transition_order, 0, sizeof(fixture_transition_order));
    list_init(&fixture_base.TaskReady);
    list_init(&fixture_base.internal.TaskRunning);
    fixture_base.internal.TaskReadySpinLock.id = 1u;
    fixture_base.internal.TaskRunningSpinLock.id = 2u;
    fixture_cpu_local.trap_depth = 1u;
    fixture_cpu_local.schedule_flags = P4_EXEC_SCHEDULE_QUANTUM |
        P4_EXEC_SCHEDULE_DISPATCH | UINT32_C(0x80);
    fixture_runtime.hart_id = 0u;
    fixture_runtime.cpu_local = &fixture_cpu_local;
    fixture_runtime.quantum = 4u;
    fixture_runtime.elapsed = 2u;
    fixture_runtime.dispatch_count = 9u;
    fixture_runtime_result = &fixture_runtime;
    fixture_admission_calls = 0u;
    fixture_pin_calls = 0u;
    fixture_unpin_calls = 0u;
    fixture_user_callbacks = 0u;
    fixture_mie = 0u;
    fixture_held_count = 0u;
    fixture_ready_held = 0;
    fixture_running_held = 0;
    fixture_task_held = 0;
    fixture_fatal_seen = 0;
    fixture_fatal_code = 0u;
    fixture_fatal_lock_count = 0u;
    fixture_fatal_candidate_pins = 0u;
    fixture_fatal_expect_task_lock = 0;
    fixture_inject_remove = 0;
    fixture_remove_task = NULL;
    fixture_restart_observed = 0u;
    fixture_attempt_count = 0u;
    fixture_transition_count = 0u;
    fixture_fatal_env_active = 0;
    for (i = 0; i < MAX_TASKS; ++i)
    {
        fixture_tasks[i].tc_SpinLock.id = 100u + i;
        fixture_tasks[i].tc_State = TS_READY;
        fixture_tasks[i].tc_IDNestCnt = 10u + i;
        fixture_tasks[i].tc_SPReg = (uintptr_t)0x3000u;
        fixture_tasks[i].tc_SPLower = (uintptr_t)0x2f00u;
        fixture_tasks[i].tc_SPUpper = (uintptr_t)0x3100u;
        fixture_tasks[i].test_etask = &fixture_etasks[i];
        fixture_etasks[i].iet_CpuNumber = 0u;
        fixture_etasks[i].iet_CpuAffinity = &fixture_affinities[i];
        fixture_etasks[i].et_RegFrame = &fixture_contexts[i];
        fixture_contexts[i].sp = (uintptr_t)0x3000u;
        fixture_contexts[i].pc = (uintptr_t)0x80000002u;
        fixture_affinities[i] = UINT32_C(3);
        fixture_live[i] = 1u;
        fixture_registry_allow[i] = 1u;
    }
}

static void ready_add(unsigned int i)
{
    list_add_tail(&fixture_base.TaskReady, &fixture_tasks[i].tc_Node);
}

static void user_callback(void)
{
    CHECK(fixture_held_count == 0u);
    CHECK(fixture_mie == 0u);
    ++fixture_user_callbacks;
}

static int mock_lock(spinlock_t *lock, unsigned int mode)
{
    unsigned int i;
    CHECK(lock != NULL);
    CHECK(mode == SPINLOCK_MODE_WRITE);
    CHECK(fixture_mie == 0u);
    CHECK(fixture_held_count < 4u);
    if (lock == &fixture_base.internal.TaskReadySpinLock)
    {
        CHECK(!fixture_ready_held);
        CHECK(!fixture_running_held);
        if (fixture_task_held)
            CHECK(fixture_held_count == 1u);
        else
            CHECK(fixture_held_count == 0u);
        fixture_ready_held = 1;
    }
    else if (lock == &fixture_base.internal.TaskRunningSpinLock)
    {
        CHECK(fixture_task_held);
        CHECK(fixture_ready_held);
        CHECK(!fixture_running_held);
        CHECK(fixture_held_count == 2u);
        fixture_running_held = 1;
    }
    else
    {
        int found = 0;
        CHECK(!fixture_task_held);
        CHECK(!fixture_ready_held);
        CHECK(!fixture_running_held);
        CHECK(fixture_held_count == 0u);
        for (i = 0; i < MAX_TASKS; ++i)
            if (lock == &fixture_tasks[i].tc_SpinLock)
                found = 1;
        CHECK(found);
        fixture_task_held = 1;
    }
    if (fixture_held_count < 4u)
        fixture_held_ids[fixture_held_count++] = lock->id;
    return 0;
}

static int mock_unlock(spinlock_t *lock)
{
    CHECK(lock != NULL);
    CHECK(fixture_mie == 0u);
    CHECK(fixture_held_count != 0u);
    if (lock != NULL && fixture_held_count != 0u)
    {
        CHECK(fixture_held_ids[fixture_held_count - 1u] == lock->id);
        --fixture_held_count;
        if (lock == &fixture_base.internal.TaskRunningSpinLock)
        {
            CHECK(fixture_running_held);
            fixture_running_held = 0;
        }
        else if (lock == &fixture_base.internal.TaskReadySpinLock)
        {
            CHECK(fixture_ready_held);
            fixture_ready_held = 0;
            if (fixture_inject_remove)
            {
                fixture_inject_remove = 0;
                CHECK(!fixture_task_held);
                CHECK(fixture_remove_task != NULL);
                if (fixture_remove_task != NULL)
                {
                    list_remove(&fixture_remove_task->tc_Node);
                    fixture_remove_task->tc_State = TS_REMOVED;
                    fixture_restart_observed = 1u;
                }
            }
        }
        else
        {
            CHECK(fixture_task_held);
            fixture_task_held = 0;
        }
    }
    return 0;
}

struct p4_exec_runtime *p4_exec_runtime_current(struct ExecBase *base)
{
    CHECK(base == &fixture_base);
    CHECK(fixture_mie == 0u);
    CHECK(fixture_held_count == 0u);
    return fixture_runtime_result;
}

int p4_exec_runtime_set_current_task(struct ExecBase *base,
                                     struct Task *task)
{
    int i = task_index(task);
    CHECK(base == &fixture_base);
    CHECK(task != NULL);
    CHECK(fixture_mie == 0u);
    CHECK(fixture_task_held);
    CHECK(!fixture_ready_held && !fixture_running_held);
    CHECK(fixture_held_count == 1u);
    CHECK(fixture_cpu_local.current_task == NULL);
    CHECK(fixture_runtime.wait_pending == NULL);
    CHECK(task->tc_State == TS_RUN);
    CHECK(i >= 0 && fixture_running_refs[i] == 1u);
    fixture_cpu_local.current_task = task;
    return 1;
}

int krnP4SignalTaskPin(struct Task *task, struct ExecBase *base)
{
    int i = task_index(task);
    ++fixture_pin_calls;
    CHECK(base == &fixture_base);
    CHECK(fixture_mie == 0u);
    CHECK(fixture_ready_held);
    CHECK(!fixture_task_held && !fixture_running_held);
    if (i < 0)
        return 0;
    if (fixture_attempt_count < MAX_TASKS)
        fixture_attempts[fixture_attempt_count++] = (unsigned int)i;
    if (!fixture_live[i])
        return 0;
    ++fixture_pins[i];
    return 1;
}

void krnP4SignalTaskUnpin(struct Task *task, struct ExecBase *base)
{
    int i = task_index(task);
    ++fixture_unpin_calls;
    CHECK(base == &fixture_base);
    CHECK(i >= 0);
    CHECK(fixture_mie == 0u);
    CHECK(fixture_held_count == 0u);
    CHECK(!fixture_task_held && !fixture_ready_held && !fixture_running_held);
    if (i >= 0)
    {
        CHECK(fixture_pins[i] != 0u);
        if (fixture_pins[i] != 0u)
            --fixture_pins[i];
    }
}

static int mock_task_record_enter_running(struct Task *task,
                                           unsigned int hart)
{
    int i = task_index(task);
    ++fixture_admission_calls;
    CHECK(i >= 0);
    CHECK(fixture_mie == 0u);
    CHECK(fixture_task_held && fixture_ready_held && fixture_running_held);
    CHECK(fixture_held_count == 3u);
    CHECK(task->tc_State == TS_READY);
    CHECK(list_has(&fixture_base.TaskReady, task));
    CHECK(!list_has(&fixture_base.internal.TaskRunning, task));
    if (i < 0 || fixture_running_refs[i] != 0u ||
        !fixture_registry_allow[i])
        return 0;
    /* This is the final fallible admission. Every following mock operation
       is infallible and becomes queue/state publication. */
    fixture_running_refs[i] = 1u;
    fixture_owner_hart[i] = hart;
    return 1;
}

int krnP4ReadyToRunningLocked(struct Task *task, unsigned int hart,
                              struct ExecBase *base)
{
    int i = task_index(task);
    int admitted;
    CHECK(base == &fixture_base);
    CHECK(i >= 0);
    CHECK(fixture_mie == 0u);
    CHECK(fixture_task_held);
    CHECK(!fixture_ready_held && !fixture_running_held);
    CHECK(fixture_held_count == 1u);
    CHECK(i >= 0 && fixture_pins[i] != 0u);
    if (i >= 0 && fixture_transition_count < MAX_TASKS)
        fixture_transition_order[fixture_transition_count++] = (unsigned int)i;
    if (i < 0 || task->tc_State != TS_READY || hart >= 2u ||
        fixture_etasks[i].iet_CpuAffinity == NULL ||
        ((*fixture_etasks[i].iet_CpuAffinity & (UINT32_C(1) << hart)) == 0u))
        return 0;

    (void)mock_lock(&PrivExecBase(base)->TaskReadySpinLock,
                    SPINLOCK_MODE_WRITE);
    (void)mock_lock(&PrivExecBase(base)->TaskRunningSpinLock,
                    SPINLOCK_MODE_WRITE);
    if (list_has(&fixture_base.TaskReady, task))
        admitted = mock_task_record_enter_running(task, hart);
    else
        admitted = 0;
    if (admitted)
    {
        list_remove(&task->tc_Node);
        fixture_etasks[i].iet_CpuNumber = hart;
        task->tc_State = TS_RUN;
        list_add_tail(&PrivExecBase(base)->TaskRunning, &task->tc_Node);
    }
    (void)mock_unlock(&PrivExecBase(base)->TaskRunningSpinLock);
    (void)mock_unlock(&PrivExecBase(base)->TaskReadySpinLock);
    return admitted;
}

void krnP4PublicSpinFatal(unsigned int reason)
{
    unsigned int i;
    fixture_fatal_seen = 1;
    fixture_fatal_code = reason;
    fixture_fatal_lock_count = fixture_held_count;
    fixture_fatal_expect_task_lock = fixture_task_held;
    for (i = 0; i < MAX_TASKS; ++i)
        if (fixture_pins[i] > fixture_fatal_candidate_pins)
            fixture_fatal_candidate_pins = fixture_pins[i];
    CHECK(fixture_mie == 0u);
    CHECK(reason == P4_PUBLIC_SPIN_BAD_STATE);
    if (!fixture_fatal_env_active)
        abort();
    longjmp(fixture_fatal_env, 1);
}

static void run_and_expect_fatal(void)
{
    fixture_fatal_env_active = 1;
    if (setjmp(fixture_fatal_env) == 0)
    {
        (void)krnP4Dispatch(&fixture_base);
        CHECK(0 && "dispatcher should have failed closed");
    }
    fixture_fatal_env_active = 0;
    CHECK(fixture_fatal_seen);
    CHECK(fixture_fatal_code == P4_PUBLIC_SPIN_BAD_STATE);
}

static void test_priority_and_harts(void)
{
    unsigned int hart;
    for (hart = 0; hart < 2u; ++hart)
    {
        unsigned int i;
        struct Task *selected;
        reset_fixture();
        fixture_runtime.hart_id = hart;
        fixture_affinities[1] = UINT32_C(1) << (1u - hart);
        fixture_live[0] = 0u; /* retired high-priority head */
        ready_add(0u);
        ready_add(1u); /* live but ineligible on this hart */
        ready_add(2u); /* first eligible according to Ready priority */
        ready_add(3u); /* lower eligible task must not win */
        fixture_tasks[2].tc_Launch = user_callback;
        fixture_tasks[3].tc_Launch = user_callback;
        if (hart == 1u)
        {
            fixture_contexts[2].Flags = ECF_FPU;
            fixture_contexts[2].fpuContext = (void *)(uintptr_t)0x4000u;
        }

        selected = krnP4Dispatch(&fixture_base);
        CHECK(selected == &fixture_tasks[2]);
        CHECK(fixture_cpu_local.current_task == selected);
        CHECK(fixture_owner_hart[2] == hart);
        CHECK(fixture_running_refs[2] == 1u);
        CHECK(fixture_etasks[2].iet_CpuNumber == hart);
        CHECK(fixture_tasks[2].tc_State == TS_RUN);
        CHECK(list_has(&fixture_base.internal.TaskRunning, selected));
        CHECK(!list_has(&fixture_base.TaskReady, selected));
        CHECK(fixture_cpu_local.id_nest == fixture_tasks[2].tc_IDNestCnt);
        CHECK(fixture_runtime.elapsed == fixture_runtime.quantum);
        CHECK(fixture_runtime.dispatch_count == 10u);
        CHECK(fixture_cpu_local.idle_count == 0u);
        CHECK(fixture_cpu_local.schedule_flags ==
              (P4_EXEC_SCHEDULE_DISPATCH | UINT32_C(0x80)));
        CHECK(fixture_admission_calls == 1u);
        CHECK(fixture_user_callbacks == 0u);
        CHECK(fixture_attempt_count >= 3u);
        CHECK(fixture_attempts[0] == 0u); /* failed retired pin */
        CHECK(fixture_attempts[1] == 1u); /* ineligible candidate */
        CHECK(fixture_attempts[2] == 2u); /* first eligible candidate */
        CHECK(fixture_transition_count == 2u);
        CHECK(fixture_transition_order[0] == 1u);
        CHECK(fixture_transition_order[1] == 2u);
        for (i = 0; i < 4u; ++i)
            CHECK(fixture_pins[i] == 0u);
        CHECK(fixture_held_count == 0u);
    }
}

static void test_idle_paths(void)
{
    unsigned int mode;
    for (mode = 0; mode < 3u; ++mode)
    {
        struct Task *result;
        reset_fixture();
        if (mode == 1u)
        {
            fixture_live[0] = 0u;
            ready_add(0u);
        }
        else if (mode == 2u)
        {
            fixture_affinities[0] = UINT32_C(2);
            ready_add(0u);
        }
        result = krnP4Dispatch(&fixture_base);
        CHECK(result == NULL);
        CHECK(fixture_cpu_local.current_task == NULL);
        CHECK(fixture_admission_calls == 0u);
        CHECK(fixture_transition_count == (mode == 2u ? 1u : 0u));
        CHECK(fixture_cpu_local.idle_count == 1u);
        CHECK(fixture_cpu_local.schedule_flags ==
              (P4_EXEC_SCHEDULE_QUANTUM | P4_EXEC_SCHEDULE_SWITCH |
               P4_EXEC_SCHEDULE_DISPATCH | UINT32_C(0x80)));
        CHECK(fixture_runtime.elapsed == 2u);
        CHECK(fixture_runtime.dispatch_count == 9u);
        CHECK(fixture_held_count == 0u);
        CHECK(fixture_pins[0] == 0u);
    }
}

static void test_invalid_runtime_is_fatal(void)
{
    unsigned int mode;
    for (mode = 0; mode < 6u; ++mode)
    {
        reset_fixture();
        ready_add(0u);
        switch (mode)
        {
            case 0: fixture_runtime_result = NULL; break;
            case 1: fixture_runtime.hart_id = 2u; break;
            case 2: fixture_cpu_local.current_task = &fixture_tasks[4]; break;
            case 3: fixture_runtime.wait_pending = &fixture_tasks[4]; break;
            case 4: fixture_cpu_local.trap_depth = 0u; break;
            default: fixture_runtime.cpu_local = NULL; break;
        }
        run_and_expect_fatal();
        CHECK(fixture_fatal_lock_count == 0u);
        CHECK(fixture_admission_calls == 0u);
        CHECK(fixture_pin_calls == 0u);
        CHECK(fixture_running_refs[0] == 0u);
        CHECK(list_has(&fixture_base.TaskReady, &fixture_tasks[0]));
        CHECK(fixture_cpu_local.current_task ==
              (mode == 2u ? &fixture_tasks[4] : NULL));
    }
}

static void expect_context_fatal(unsigned int bad_case)
{
    struct Task *candidate;
    reset_fixture();
    ready_add(0u);
    ready_add(1u); /* successor pin is live when candidate image is checked */
    candidate = &fixture_tasks[0];
    switch (bad_case)
    {
        case 0: candidate->test_etask = NULL; break;
        case 1: fixture_etasks[0].et_RegFrame = NULL; break;
        case 2: candidate->tc_SPReg = candidate->tc_SPLower; break;
        case 3: candidate->tc_SPReg = candidate->tc_SPUpper + 16u; break;
        case 4: fixture_contexts[0].sp += 8u; break; /* mismatched SP */
        case 5:
            candidate->tc_SPReg += 8u;
            fixture_contexts[0].sp += 8u; /* matching, but misaligned */
            break;
        case 6: fixture_contexts[0].pc = 0u; break;
        case 7: fixture_contexts[0].pc |= 1u; break;
        case 8:
            fixture_contexts[0].Flags = ECF_FPU;
            fixture_contexts[0].fpuContext = NULL;
            break;
        default:
            fixture_contexts[0].Flags = ECF_FPU;
            fixture_contexts[0].fpuContext = (void *)(uintptr_t)0x4002u;
            break;
    }
    run_and_expect_fatal();
    CHECK(fixture_fatal_lock_count == 1u);
    CHECK(fixture_fatal_expect_task_lock);
    CHECK(fixture_fatal_candidate_pins == 1u);
    CHECK(fixture_pins[0] == 1u && fixture_pins[1] == 1u);
    CHECK(fixture_task_held);
    CHECK(!fixture_ready_held && !fixture_running_held);
    CHECK(fixture_admission_calls == 0u);
    CHECK(fixture_running_refs[0] == 0u);
    CHECK(fixture_owner_hart[0] == UINT32_MAX);
    CHECK(fixture_cpu_local.current_task == NULL);
    CHECK(fixture_tasks[0].tc_State == TS_READY);
    CHECK(list_has(&fixture_base.TaskReady, &fixture_tasks[0]));
    CHECK(!list_has(&fixture_base.internal.TaskRunning, &fixture_tasks[0]));
}

static void test_saved_context_fail_closed(void)
{
    unsigned int bad_case;
    for (bad_case = 0; bad_case < 10u; ++bad_case)
        expect_context_fatal(bad_case);
}

static void test_concurrent_candidate_removal_restarts(void)
{
    struct Task *selected;
    reset_fixture();
    ready_add(0u);
    ready_add(1u);
    fixture_remove_task = &fixture_tasks[0];
    fixture_inject_remove = 1;
    selected = krnP4Dispatch(&fixture_base);
    CHECK(fixture_restart_observed == 1u);
    CHECK(selected == &fixture_tasks[1]);
    CHECK(fixture_tasks[0].tc_State == TS_REMOVED);
    CHECK(!list_has(&fixture_base.TaskReady, &fixture_tasks[0]));
    CHECK(fixture_running_refs[0] == 0u);
    CHECK(fixture_running_refs[1] == 1u);
    CHECK(fixture_cpu_local.current_task == selected);
    CHECK(fixture_pins[0] == 0u && fixture_pins[1] == 0u);
    CHECK(fixture_unpin_calls == 2u);
    CHECK(fixture_held_count == 0u);
}

static void test_rejected_admission_preserves_candidate(void)
{
    struct Task *selected;
    reset_fixture();
    ready_add(0u);
    ready_add(1u);
    fixture_registry_allow[0] = 0u; /* final owner admission refuses */
    selected = krnP4Dispatch(&fixture_base);
    CHECK(selected == &fixture_tasks[1]);
    CHECK(fixture_admission_calls == 2u);
    CHECK(fixture_running_refs[0] == 0u);
    CHECK(fixture_tasks[0].tc_State == TS_READY);
    CHECK(list_has(&fixture_base.TaskReady, &fixture_tasks[0]));
    CHECK(fixture_pins[0] == 0u && fixture_pins[1] == 0u);
}

static void expect_list_corruption_fatal(unsigned int kind)
{
    reset_fixture();
    ready_add(0u);
    ready_add(1u);
    if (kind == 0u)
        fixture_base.TaskReady.lh_TailPred = &fixture_tasks[0].tc_Node;
    else if (kind == 1u)
        fixture_base.TaskReady.lh_Tail = &fixture_tasks[1].tc_Node;
    else
    {
        /* A forward cycle with a mismatched predecessor must be caught before
           any pin or candidate state is inspected. */
        fixture_tasks[1].tc_Node.ln_Succ = &fixture_tasks[0].tc_Node;
    }
    run_and_expect_fatal();
    CHECK(fixture_fatal_lock_count == 0u);
    CHECK(!fixture_fatal_expect_task_lock);
    CHECK(fixture_pin_calls == 0u);
    CHECK(fixture_admission_calls == 0u);
    CHECK(fixture_running_refs[0] == 0u && fixture_running_refs[1] == 0u);
    CHECK(!fixture_ready_held && !fixture_task_held);
}

static void test_ready_list_corruption_is_bounded_and_fatal(void)
{
    unsigned int kind;
    for (kind = 0; kind < 3u; ++kind)
        expect_list_corruption_fatal(kind);
}

static void test_full_scan_limit(void)
{
    const unsigned int count = DISPATCH_SCAN_LIMIT + 1u;
    struct Task *tasks;
    struct List list;
    struct Node *previous;
    unsigned int i;
    reset_fixture();
    tasks = (struct Task *)calloc(count, sizeof(*tasks));
    CHECK(tasks != NULL);
    if (tasks == NULL)
        return;
    list_init(&list);
    previous = (struct Node *)&list.lh_Head;
    for (i = 0; i < count; ++i)
    {
        tasks[i].tc_Node.ln_Pred = previous;
        previous->ln_Succ = &tasks[i].tc_Node;
        previous = &tasks[i].tc_Node;
    }
    previous->ln_Succ = (struct Node *)&list.lh_Tail;
    list.lh_TailPred = previous;
    fixture_base.TaskReady = list;
    CHECK(list_count(&fixture_base.TaskReady) == count);
    run_and_expect_fatal();
    CHECK(fixture_fatal_lock_count == 0u);
    CHECK(fixture_pin_calls == 0u);
    CHECK(fixture_admission_calls == 0u);
    CHECK(!fixture_ready_held);
    free(tasks);
}

int main(void)
{
    test_priority_and_harts();
    test_idle_paths();
    test_invalid_runtime_is_fatal();
    test_saved_context_fail_closed();
    test_concurrent_candidate_removal_restarts();
    test_rejected_admission_preserves_candidate();
    test_ready_list_corruption_is_bounded_and_fatal();
    test_full_scan_limit();
    if (failures != 0u)
    {
        fprintf(stderr, "%u/%u checks failed\n", failures, checks);
        return 1;
    }
    printf("PASS: P4 dispatcher actual-source fixture (%u checks)\n", checks);
    return 0;
}
