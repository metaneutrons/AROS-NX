/*
 * Actual-source fixture for the P4 Task-priority queue helper, public wrapper,
 * and scheduler decision. Intrusive lists and Task registry transitions are
 * production sources; locks, CPU masks, IRQ state and asynchronous pokes are
 * deterministic host models.
 */
#include <errno.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../p4_task_registry.h"
#include "../../exec/p4_signal_protocol.h"

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

struct Task;
enum lock_kind
{
    LOCK_KIND_TASK = 1,
    LOCK_KIND_READY,
    LOCK_KIND_WAIT,
    LOCK_KIND_RUNNING
};

typedef struct
{
    pthread_mutex_t mutex;
    enum lock_kind kind;
    unsigned int initialized;
} spinlock_t;

struct ETask
{
    const void *iet_CpuAffinity;
    uint32_t iet_CpuNumber;
};

struct Task
{
    struct Node tc_Node;
    uint8_t tc_Flags;
    uint8_t tc_State;
    struct ETask *test_etask;
    spinlock_t tc_SpinLock;
};

struct ExecBase
{
    struct List TaskReady;
    struct List TaskWait;
};

struct IntExecBase
{
    struct ExecBase pub;
    spinlock_t TaskReadySpinLock;
    spinlock_t TaskWaitSpinLock;
    spinlock_t TaskRunningSpinLock;
    struct List TaskRunning;
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

struct Fixture
{
    struct IntExecBase base;
    struct Task tasks[4];
    struct ETask etasks[4];
    struct p4_task_registry registry;
    struct p4_task_registry_record records[4];
    struct p4_exec_runtime runtimes[2];
    struct p4_cpu_local locals[2];
    uint32_t online_mask;
    unsigned int init_locks;
};

enum task_state
{
    TS_INVALID = 0,
    TS_ADDED = 1,
    TS_RUN = 2,
    TS_READY = 3,
    TS_WAIT = 4,
    TS_EXCEPT = 5,
    TS_REMOVED = 6,
    TS_TOMBSTONED = 7
};

#define TF_EXCEPT (UINT8_C(1) << 5)
#define SPINLOCK_MODE_WRITE 1u
#define P4_EXEC_RUNTIME_HART_COUNT 2u
#define P4_EXEC_SCHEDULE_QUANTUM (UINT32_C(1) << 0)
#define P4_EXEC_SCHEDULE_SWITCH  (UINT32_C(1) << 1)
#define MSTATUS_MIE UINT32_C(8)
#define PrivExecBase(base) ((struct IntExecBase *)(base))

enum event
{
    EV_PIN = 1,
    EV_DISABLE,
    EV_TASK_LOCK,
    EV_READY_LOCK,
    EV_WAIT_LOCK,
    EV_RUNNING_LOCK,
    EV_RUNNING_UNLOCK,
    EV_WAIT_UNLOCK,
    EV_READY_UNLOCK,
    EV_TASK_UNLOCK,
    EV_UNPIN,
    EV_POKE0,
    EV_POKE1,
    EV_ENABLE
};

enum thread_role
{
    ROLE_MAIN = 0,
    ROLE_SCHEDULER,
    ROLE_PRIORITY
};

struct Trace
{
    unsigned int events[64];
    unsigned int count;
    unsigned int list_depth;
    unsigned int task_depth;
    spinlock_t *held[8];
    unsigned int held_count;
    enum thread_role role;
};

struct RaceControl
{
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    int initialized;
    int enabled;
    int scheduler_at_priority;
    int release_scheduler;
    int priority_waiting_for_ready;
};

#define CHECK(test) do { \
    atomic_fetch_add_explicit(&checks, 1u, memory_order_relaxed); \
    if (!(test)) { \
        atomic_fetch_add_explicit(&failures, 1u, memory_order_relaxed); \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #test); \
    } \
} while (0)

static _Atomic unsigned int checks;
static _Atomic unsigned int failures;
static _Thread_local struct Fixture *tls_fixture;
static _Thread_local struct Trace *tls_trace;
static _Thread_local struct ExecBase *tls_sysbase;
static _Thread_local struct p4_exec_runtime *tls_runtime;
static _Thread_local struct Task *tls_current_task;
static _Thread_local int tls_irq_disabled;
static _Thread_local unsigned int tls_registry_depth;

static pthread_mutex_t registry_gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t race_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t race_condition = PTHREAD_COND_INITIALIZER;
static int race_enabled;
static int race_scheduler_at_priority;
static int race_release_scheduler;
static int race_priority_waiting_for_ready;
static unsigned int pin_calls;
static unsigned int unpin_calls;
static unsigned int etask_reads;
static unsigned int poke_count;
static uint32_t poke_mask_seen;
static int poison_on_final_unpin;
static struct Task *poison_identity;
static struct Task *retire_after_pin;
static struct Task *migrate_after_pin;
static jmp_buf fatal_jump;
static int fatal_armed;
static unsigned int fatal_count;
static unsigned int fatal_reason;
static _Atomic unsigned int schedule_priority_reads;

static void mock_lock(spinlock_t *lock, void *hook, unsigned int mode);
static void mock_unlock(spinlock_t *lock);
static int8_t mock_schedule_priority(const struct Task *task);
static void fixture_remove(struct Node *node);
static void fixture_enqueue(struct List *list, struct Node *node);
static struct ETask *fixture_get_etask(struct Task *task);
static unsigned int fixture_cpu_count(void);
static int fixture_cpu_in_mask(uint32_t hart, const void *mask);
static void mock_disable(void);
static void mock_enable(void);
static void p4_signal_adapter_lock_disabled(struct Task *task);
static void p4_signal_adapter_unlock_disabled(struct Task *task);

typedef int8_t BYTE;
typedef int32_t LONG;
#define A1 1
#define D0 0
#define AROS_LHA(type, name, reg) type name
#define AROS_LH2(ret, name, arg1, arg2, base_type, base_name, version, lib) \
    ret name(arg1, arg2, base_type base_name)
#define AROS_LIBFUNC_INIT
#define AROS_LIBFUNC_EXIT
#define GET_THIS_TASK (tls_current_task)
#define Disable() mock_disable()
#define Enable() mock_enable()
#define csr_read(reg) (tls_irq_disabled ? 0u : MSTATUS_MIE)
#define mstatus 0
#define krnP4GetCPUCount fixture_cpu_count
#define krnP4CPUInMask fixture_cpu_in_mask
#define EXEC_SPINLOCK_LOCK(lock, hook, mode) mock_lock((lock), (hook), (mode))
#define EXEC_SPINLOCK_UNLOCK(lock) mock_unlock((lock))
#define Remove(node) fixture_remove((node))
#define Enqueue(list, node) fixture_enqueue((list), (node))
#define GetIntETask(task) fixture_get_etask((task))

#define P4_PUBLIC_SPINLOCK_TEST 1
#define P4_PUBLIC_SPINLOCK_TEST_TYPES 1
#include "../p4_public_spinlock.h"

static void trace_event(unsigned int event)
{
    if (tls_trace != NULL && tls_trace->count <
        sizeof(tls_trace->events) / sizeof(tls_trace->events[0]))
        tls_trace->events[tls_trace->count++] = event;
    else if (tls_trace != NULL)
        CHECK(0 && "trace overflow");
}

static void registry_lock(void)
{
    CHECK(tls_registry_depth == 0u);
    CHECK(pthread_mutex_lock(&registry_gate) == 0);
    ++tls_registry_depth;
}

static void registry_unlock(void)
{
    CHECK(tls_registry_depth == 1u);
    --tls_registry_depth;
    CHECK(pthread_mutex_unlock(&registry_gate) == 0);
}

static int lock_init(spinlock_t *lock, enum lock_kind kind)
{
    memset(lock, 0, sizeof(*lock));
    lock->kind = kind;
    lock->initialized = pthread_mutex_init(&lock->mutex, NULL) == 0;
    return (int)lock->initialized;
}

static void lock_destroy(spinlock_t *lock)
{
    if (lock->initialized)
    {
        CHECK(pthread_mutex_destroy(&lock->mutex) == 0);
        lock->initialized = 0;
    }
}

static unsigned int lock_event(enum lock_kind kind, int unlock)
{
    if (unlock)
    {
        if (kind == LOCK_KIND_RUNNING) return EV_RUNNING_UNLOCK;
        if (kind == LOCK_KIND_WAIT) return EV_WAIT_UNLOCK;
        if (kind == LOCK_KIND_READY) return EV_READY_UNLOCK;
        return EV_TASK_UNLOCK;
    }
    if (kind == LOCK_KIND_RUNNING) return EV_RUNNING_LOCK;
    if (kind == LOCK_KIND_WAIT) return EV_WAIT_LOCK;
    if (kind == LOCK_KIND_READY) return EV_READY_LOCK;
    return EV_TASK_LOCK;
}

static void race_signal_priority_read(void)
{
    CHECK(pthread_mutex_lock(&race_mutex) == 0);
    race_scheduler_at_priority = 1;
    CHECK(pthread_cond_broadcast(&race_condition) == 0);
    while (!race_release_scheduler)
        CHECK(pthread_cond_wait(&race_condition, &race_mutex) == 0);
    CHECK(pthread_mutex_unlock(&race_mutex) == 0);
}

static void mock_lock(spinlock_t *lock, void *hook, unsigned int mode)
{
    int result;
    CHECK(lock != NULL);
    CHECK(hook == NULL);
    CHECK(mode == SPINLOCK_MODE_WRITE);
    CHECK(tls_irq_disabled != 0);
    CHECK(tls_registry_depth == 0u);
    CHECK(tls_trace != NULL);
    CHECK(tls_trace->held_count <
          sizeof(tls_trace->held) / sizeof(tls_trace->held[0]));

    if (lock->kind == LOCK_KIND_TASK)
    {
        CHECK(tls_trace->task_depth == 0u);
        CHECK(tls_trace->list_depth == 0u);
    }
    else
    {
        CHECK(tls_trace->task_depth > 0u);
        if (lock->kind == LOCK_KIND_READY)
        {
            CHECK(tls_trace->list_depth == 0u);
        }
        else if (lock->kind == LOCK_KIND_WAIT)
        {
            CHECK(tls_trace->list_depth == 1u);
            CHECK(tls_trace->held[tls_trace->held_count - 1u]->kind ==
                  LOCK_KIND_READY);
        }
        else if (lock->kind == LOCK_KIND_RUNNING)
        {
            CHECK(tls_trace->list_depth == 2u);
            CHECK(tls_trace->held[tls_trace->held_count - 1u]->kind ==
                  LOCK_KIND_WAIT);
        }
    }

    trace_event(lock_event(lock->kind, 0));
    if (race_enabled && tls_trace->role == ROLE_PRIORITY &&
        lock->kind == LOCK_KIND_READY)
    {
        result = pthread_mutex_trylock(&lock->mutex);
        if (result == EBUSY)
        {
            CHECK(pthread_mutex_lock(&race_mutex) == 0);
            race_priority_waiting_for_ready = 1;
            CHECK(pthread_cond_broadcast(&race_condition) == 0);
            CHECK(pthread_mutex_unlock(&race_mutex) == 0);
            result = pthread_mutex_lock(&lock->mutex);
        }
    }
    else
    {
        result = pthread_mutex_lock(&lock->mutex);
    }
    CHECK(result == 0);

    if (tls_trace->held_count <
        sizeof(tls_trace->held) / sizeof(tls_trace->held[0]))
        tls_trace->held[tls_trace->held_count++] = lock;
    if (lock->kind == LOCK_KIND_TASK)
        ++tls_trace->task_depth;
    else
        ++tls_trace->list_depth;
}

static void mock_unlock(spinlock_t *lock)
{
    CHECK(lock != NULL);
    CHECK(tls_irq_disabled != 0);
    CHECK(tls_trace != NULL);
    CHECK(tls_trace->held_count > 0u);
    if (tls_trace != NULL && tls_trace->held_count > 0u)
    {
        CHECK(tls_trace->held[tls_trace->held_count - 1u] == lock);
        --tls_trace->held_count;
    }
    if (lock->kind == LOCK_KIND_TASK)
    {
        CHECK(tls_trace->task_depth == 1u);
        CHECK(tls_trace->list_depth == 0u);
        --tls_trace->task_depth;
    }
    else
    {
        CHECK(tls_trace->list_depth > 0u);
        --tls_trace->list_depth;
    }
    trace_event(lock_event(lock->kind, 1));
    CHECK(pthread_mutex_unlock(&lock->mutex) == 0);
}

static void init_list(struct List *list)
{
    list->lh_Head = (struct Node *)&list->lh_Tail;
    list->lh_Tail = NULL;
    list->lh_TailPred = (struct Node *)&list->lh_Head;
    list->lh_Type = 0;
    list->l_pad = 0;
}

static void fixture_remove(struct Node *node)
{
    CHECK(node != NULL && node->ln_Pred != NULL && node->ln_Succ != NULL);
    node->ln_Pred->ln_Succ = node->ln_Succ;
    node->ln_Succ->ln_Pred = node->ln_Pred;
    node->ln_Pred = NULL;
    node->ln_Succ = NULL;
}

/* Exec Enqueue inserts by descending priority, preserving equal-priority
 * insertion order. This mock is intentionally not an append-only shortcut. */
static void fixture_enqueue(struct List *list, struct Node *node)
{
    struct Node *tail = (struct Node *)&list->lh_Tail;
    struct Node *at = list->lh_Head;
    struct Node *previous;
    while (at != tail && at->ln_Pri >= node->ln_Pri)
        at = at->ln_Succ;
    previous = at->ln_Pred;
    node->ln_Pred = previous;
    node->ln_Succ = at;
    previous->ln_Succ = node;
    at->ln_Pred = node;
    if (at == tail)
        list->lh_TailPred = node;
}

static void append_node(struct List *list, struct Node *node)
{
    struct Node *tail = (struct Node *)&list->lh_Tail;
    node->ln_Pred = list->lh_TailPred;
    node->ln_Succ = tail;
    list->lh_TailPred->ln_Succ = node;
    list->lh_TailPred = node;
    tail->ln_Pred = node;
}

static struct ETask *fixture_get_etask(struct Task *task)
{
    ++etask_reads;
    return task->test_etask;
}

static unsigned int fixture_cpu_count(void)
{
    return (unsigned int)((tls_fixture->online_mask & 1u) != 0u) +
           (unsigned int)((tls_fixture->online_mask & 2u) != 0u);
}

static int fixture_cpu_in_mask(uint32_t hart, const void *mask)
{
    const uint32_t *affinity = (const uint32_t *)mask;
    return hart < 2u && affinity != NULL &&
           (tls_fixture->online_mask & (UINT32_C(1) << hart)) != 0u &&
           (*affinity & (UINT32_C(1) << hart)) != 0u;
}

static struct p4_exec_runtime *p4_exec_runtime_current(
    struct ExecBase *sys_base)
{
    return tls_fixture != NULL && sys_base == &tls_fixture->base.pub
        ? tls_runtime : NULL;
}

static int8_t mock_schedule_priority(const struct Task *task)
{
    CHECK(tls_trace != NULL && tls_trace->task_depth > 0u &&
          tls_trace->list_depth == 1u &&
          tls_trace->held[tls_trace->held_count - 1u]->kind == LOCK_KIND_READY);
    atomic_fetch_add_explicit(&schedule_priority_reads, 1u,
                              memory_order_relaxed);
    if (race_enabled && tls_trace->role == ROLE_SCHEDULER)
        race_signal_priority_read();
    return task->tc_Node.ln_Pri;
}

static void registry_register(struct Fixture *fixture, unsigned int index)
{
    registry_lock();
    CHECK(p4_task_registry_register_locked(&fixture->registry,
          &fixture->records[index], &fixture->tasks[index]));
    registry_unlock();
}

static void fixture_init(struct Fixture *fixture, uint32_t online_mask)
{
    unsigned int i;
    memset(fixture, 0, sizeof(*fixture));
    init_list(&fixture->base.pub.TaskReady);
    init_list(&fixture->base.pub.TaskWait);
    init_list(&fixture->base.TaskRunning);
    CHECK(lock_init(&fixture->base.TaskReadySpinLock, LOCK_KIND_READY));
    CHECK(lock_init(&fixture->base.TaskWaitSpinLock, LOCK_KIND_WAIT));
    CHECK(lock_init(&fixture->base.TaskRunningSpinLock, LOCK_KIND_RUNNING));
    for (i = 0; i < 4u; ++i)
    {
        CHECK(lock_init(&fixture->tasks[i].tc_SpinLock, LOCK_KIND_TASK));
        fixture->tasks[i].test_etask = &fixture->etasks[i];
        fixture->etasks[i].iet_CpuAffinity = &fixture->etasks[i].iet_CpuNumber;
        fixture->etasks[i].iet_CpuNumber = 3u;
    }
    for (i = 0; i < 2u; ++i)
    {
        fixture->runtimes[i].hart_id = i;
        fixture->runtimes[i].cpu_local = &fixture->locals[i];
        fixture->locals[i].hart_id = i;
        fixture->locals[i].trap_depth = 1;
    }
    fixture->online_mask = online_mask;
    p4_task_registry_init(&fixture->registry);
    fixture->init_locks = 1u;
    pin_calls = 0;
    unpin_calls = 0;
    etask_reads = 0;
    poke_count = 0;
    poke_mask_seen = 0;
    poison_on_final_unpin = 0;
    poison_identity = NULL;
    retire_after_pin = NULL;
    migrate_after_pin = NULL;
    fatal_count = 0;
    fatal_reason = 0;
}

static void fixture_destroy(struct Fixture *fixture)
{
    unsigned int i;
    if (!fixture->init_locks)
        return;
    for (i = 0; i < 4u; ++i)
        lock_destroy(&fixture->tasks[i].tc_SpinLock);
    lock_destroy(&fixture->base.TaskReadySpinLock);
    lock_destroy(&fixture->base.TaskWaitSpinLock);
    lock_destroy(&fixture->base.TaskRunningSpinLock);
    fixture->init_locks = 0u;
}

static void setup_task(struct Fixture *fixture, unsigned int index,
                       unsigned int state, int priority, uint32_t affinity)
{
    struct Task *task = &fixture->tasks[index];
    task->tc_State = (uint8_t)state;
    task->tc_Node.ln_Pri = (int8_t)priority;
    fixture->etasks[index].iet_CpuNumber = affinity;
    registry_register(fixture, index);
    if (state == TS_READY)
        fixture_enqueue(&fixture->base.pub.TaskReady, &task->tc_Node);
    else if (state == TS_WAIT)
        append_node(&fixture->base.pub.TaskWait, &task->tc_Node);
    else if (state == TS_RUN)
        append_node(&fixture->base.TaskRunning, &task->tc_Node);
}

static void set_running_owner(struct Fixture *fixture, unsigned int index,
                              unsigned int hart)
{
    registry_lock();
    CHECK(p4_task_registry_enter_running_locked(&fixture->registry,
          &fixture->tasks[index], hart));
    registry_unlock();
    fixture->etasks[index].iet_CpuNumber = hart;
}

static void bind_thread(struct Fixture *fixture, struct Trace *trace,
                        unsigned int hart)
{
    memset(trace, 0, sizeof(*trace));
    tls_fixture = fixture;
    tls_trace = trace;
    tls_sysbase = &fixture->base.pub;
    tls_runtime = &fixture->runtimes[hart];
    tls_current_task = &fixture->tasks[0];
    tls_irq_disabled = 0;
    tls_registry_depth = 0u;
}

static void mock_disable(void)
{
    CHECK(tls_irq_disabled == 0);
    CHECK(tls_trace == NULL ||
          (tls_trace->held_count == 0u && tls_registry_depth == 0u));
    tls_irq_disabled = 1;
    trace_event(EV_DISABLE);
}

/* Model a remote state transition after the wrapper's pin has returned but
 * before it takes the target Task lock. The transition uses the same lock
 * order as the production queue writers. */
static void apply_pending_post_pin_transition(void)
{
    if (retire_after_pin != NULL)
    {
        struct Task *task = retire_after_pin;
        retire_after_pin = NULL;
        mock_lock(&task->tc_SpinLock, NULL, SPINLOCK_MODE_WRITE);
        mock_lock(&tls_fixture->base.TaskReadySpinLock, NULL,
                  SPINLOCK_MODE_WRITE);
        mock_lock(&tls_fixture->base.TaskWaitSpinLock, NULL,
                  SPINLOCK_MODE_WRITE);
        mock_lock(&tls_fixture->base.TaskRunningSpinLock, NULL,
                  SPINLOCK_MODE_WRITE);
        registry_lock();
        CHECK(p4_task_registry_retire_for_service_locked(
              &tls_fixture->registry, task));
        registry_unlock();
        fixture_remove(&task->tc_Node);
        task->tc_State = TS_REMOVED;
        mock_unlock(&tls_fixture->base.TaskRunningSpinLock);
        mock_unlock(&tls_fixture->base.TaskWaitSpinLock);
        mock_unlock(&tls_fixture->base.TaskReadySpinLock);
        mock_unlock(&task->tc_SpinLock);
    }
    if (migrate_after_pin != NULL)
    {
        struct Task *task = migrate_after_pin;
        migrate_after_pin = NULL;
        mock_lock(&task->tc_SpinLock, NULL, SPINLOCK_MODE_WRITE);
        mock_lock(&tls_fixture->base.TaskReadySpinLock, NULL,
                  SPINLOCK_MODE_WRITE);
        mock_lock(&tls_fixture->base.TaskWaitSpinLock, NULL,
                  SPINLOCK_MODE_WRITE);
        fixture_remove(&task->tc_Node);
        task->tc_State = TS_READY;
        fixture_enqueue(&tls_fixture->base.pub.TaskReady, &task->tc_Node);
        mock_unlock(&tls_fixture->base.TaskWaitSpinLock);
        mock_unlock(&tls_fixture->base.TaskReadySpinLock);
        mock_unlock(&task->tc_SpinLock);
    }
}

static void mock_enable(void)
{
    CHECK(tls_irq_disabled != 0);
    CHECK(tls_trace == NULL ||
          (tls_trace->held_count == 0u && tls_registry_depth == 0u));
    trace_event(EV_ENABLE);
    tls_irq_disabled = 0;
}

static void p4_signal_adapter_lock_disabled(struct Task *task)
{
    CHECK(task != NULL && tls_irq_disabled != 0);
    apply_pending_post_pin_transition();
    mock_lock(&task->tc_SpinLock, NULL, SPINLOCK_MODE_WRITE);
}

static void p4_signal_adapter_unlock_disabled(struct Task *task)
{
    CHECK(task != NULL && tls_irq_disabled != 0);
    mock_unlock(&task->tc_SpinLock);
}

static void p4_signal_adapter_fail_closed(void)
    __attribute__((noreturn));
static void p4_signal_adapter_fail_closed(void)
{
    ++fatal_count;
    fatal_reason = P4_PUBLIC_SPIN_BAD_STATE;
    if (fatal_armed)
        longjmp(fatal_jump, 1);
    abort();
}

static int krnP4SignalTaskPin(struct Task *task, struct ExecBase *sys_base)
{
    int result;
    CHECK(tls_irq_disabled != 0);
    CHECK(tls_trace == NULL || tls_trace->held_count == 0u);
    CHECK(tls_registry_depth == 0u);
    CHECK(sys_base == &tls_fixture->base.pub);
    ++pin_calls;
    trace_event(EV_PIN);
    registry_lock();
    result = p4_task_registry_pin_locked(&tls_fixture->registry, task);
    registry_unlock();
    return result;
}

static void krnP4SignalTaskUnpin(struct Task *task, struct ExecBase *sys_base)
{
    int result;
    CHECK(task != NULL);
    CHECK(tls_irq_disabled != 0);
    CHECK(tls_trace == NULL || tls_trace->held_count == 0u);
    CHECK(tls_registry_depth == 0u);
    CHECK(sys_base == &tls_fixture->base.pub);
    ++unpin_calls;
    trace_event(EV_UNPIN);
    registry_lock();
    result = p4_task_registry_unpin_locked(&tls_fixture->registry, task);
    registry_unlock();
    CHECK(result == 1);
    if (poison_on_final_unpin && task == poison_identity)
    {
        /* Poison only Task fields, leaving its host mutex intact. Any wrapper
         * dereference after this call returns observes sentinel values. */
        task->tc_Node.ln_Pri = INT8_MIN;
        task->tc_State = UINT8_MAX;
        task->tc_Flags = UINT8_MAX;
        task->test_etask = NULL;
        poison_on_final_unpin = 0;
    }
}

static int krnP4TaskRecordRunningOwner(struct Task *task,
                                       struct ExecBase *sys_base)
{
    int result;
    CHECK(task != NULL);
    CHECK(sys_base == &tls_fixture->base.pub);
    CHECK(tls_trace != NULL && tls_trace->task_depth > 0u &&
          tls_trace->list_depth == 3u);
    registry_lock();
    result = p4_task_registry_running_owner_locked(&tls_fixture->registry,
                                                   task);
    registry_unlock();
    return result;
}

static void krnP4SignalPoke(const struct p4_signal_effect *effect,
                           struct ExecBase *sys_base)
{
    CHECK(effect != NULL && sys_base == &tls_fixture->base.pub);
    CHECK(effect->status == P4_SIGNAL_STATUS_OK);
    CHECK(effect->request_reschedule == 1u);
    CHECK(tls_irq_disabled != 0);
    CHECK(tls_trace != NULL && tls_trace->task_depth == 0u &&
          tls_trace->list_depth == 0u && tls_trace->held_count == 0u);
    CHECK(tls_registry_depth == 0u);
    CHECK(effect->reschedule_hart < 2u);
    ++poke_count;
    poke_mask_seen |= UINT32_C(1) << effect->reschedule_hart;
    trace_event(effect->reschedule_hart == 0u ? EV_POKE0 : EV_POKE1);
}

void krnP4PublicSpinFatal(enum p4_public_spinlock_result reason)
    __attribute__((noreturn));
void krnP4PublicSpinFatal(enum p4_public_spinlock_result reason)
{
    ++fatal_count;
    fatal_reason = (unsigned int)reason;
    if (fatal_armed)
        longjmp(fatal_jump, 1);
    abort();
}

#define P4_SCHEDULER_QUEUE_TEST 1
#define P4_TASK_PRIORITY_TEST 1
#include "../p4_task_lifetime.c"
#include "../p4_task_registry.c"
#include "../p4_scheduler_queue.c"

#define P4_SCHEDULE_TEST 1
#define P4_SCHEDULE_PRIORITY(task) mock_schedule_priority((task))
#include "../p4_schedule.c"

#define P4_TASK_PRIORITY_WRAPPER_TEST 1
#include "../../exec/settaskpri.c"

static void direct_priority_change(struct Fixture *fixture,
                                   struct Task *task, int8_t priority,
                                   int8_t *old_priority, uint32_t *poke_mask,
                                   int *result)
{
    struct Trace trace;
    bind_thread(fixture, &trace, 0);
    tls_irq_disabled = 1;
    p4_signal_adapter_lock_disabled(task);
    *result = krnP4TaskPriorityLocked(task, priority, old_priority,
                                     poke_mask, &fixture->base.pub);
    p4_signal_adapter_unlock_disabled(task);
    tls_irq_disabled = 0;
    CHECK(trace.held_count == 0u && trace.task_depth == 0u &&
          trace.list_depth == 0u);
}

static int list_nodes_are(const struct List *list,
                          struct Node *const *expected, unsigned int count)
{
    struct Node *node = list->lh_Head;
    struct Node *tail = (struct Node *)&list->lh_Tail;
    unsigned int i = 0;
    while (node != tail && i < count)
    {
        if (node != expected[i])
            return 0;
        node = node->ln_Succ;
        ++i;
    }
    return i == count && node == tail && list->lh_Tail == NULL;
}

struct QueueImage
{
    struct List ready;
    struct List wait;
    struct List running;
    struct Node nodes[4];
    uint8_t states[4];
    uint32_t registry_count;
    struct p4_task_registry_record records[4];
};

static void queue_image_capture(const struct Fixture *fixture,
                                struct QueueImage *image)
{
    unsigned int i;
    image->ready = fixture->base.pub.TaskReady;
    image->wait = fixture->base.pub.TaskWait;
    image->running = fixture->base.TaskRunning;
    image->registry_count = fixture->registry.count;
    memcpy(image->records, fixture->records, sizeof(image->records));
    for (i = 0; i < 4u; ++i)
    {
        image->nodes[i] = fixture->tasks[i].tc_Node;
        image->states[i] = fixture->tasks[i].tc_State;
    }
}

static int queue_image_unchanged(const struct Fixture *fixture,
                                 const struct QueueImage *image)
{
    unsigned int i;
    if (memcmp(&image->ready, &fixture->base.pub.TaskReady,
               sizeof(image->ready)) != 0 ||
        memcmp(&image->wait, &fixture->base.pub.TaskWait,
               sizeof(image->wait)) != 0 ||
        memcmp(&image->running, &fixture->base.TaskRunning,
               sizeof(image->running)) != 0 ||
        image->registry_count != fixture->registry.count ||
        memcmp(image->records, fixture->records, sizeof(image->records)) != 0)
        return 0;
    for (i = 0; i < 4u; ++i)
        if (memcmp(&image->nodes[i], &fixture->tasks[i].tc_Node,
                   sizeof(image->nodes[i])) != 0 ||
            image->states[i] != fixture->tasks[i].tc_State)
            return 0;
    return 1;
}

static void test_ready_reorders_and_returns_all_online_harts(void)
{
    struct Fixture fixture;
    int8_t old_priority = -99;
    uint32_t poke_mask = UINT32_C(0xA5A5);
    int result = 0;
    struct Node *expected[3];
    struct Trace trace;
    BYTE old;

    fixture_init(&fixture, 3u);
    setup_task(&fixture, 0, TS_READY, 10, 3u);
    setup_task(&fixture, 1, TS_READY, 20, 3u);
    setup_task(&fixture, 2, TS_READY, 5, 3u);
    direct_priority_change(&fixture, &fixture.tasks[0], 25,
                           &old_priority, &poke_mask, &result);
    CHECK(result == 1);
    CHECK(old_priority == 10);
    CHECK(poke_mask == 3u);
    CHECK(fixture.tasks[0].tc_Node.ln_Pri == 25);
    expected[0] = &fixture.tasks[0].tc_Node;
    expected[1] = &fixture.tasks[1].tc_Node;
    expected[2] = &fixture.tasks[2].tc_Node;
    CHECK(list_nodes_are(&fixture.base.pub.TaskReady, expected, 3u));

    /* Exercise the actual wrapper, which consumes its pin before poking. */
    bind_thread(&fixture, &trace, 0);
    old = SetTaskPri(&fixture.tasks[1], 30, &fixture.base.pub);
    CHECK(old == 20);
    CHECK(fixture.tasks[1].tc_Node.ln_Pri == 30);
    CHECK(poke_count == 2u);
    CHECK(poke_mask_seen == 3u);
    CHECK(trace.count >= 10u);
    CHECK(trace.events[0] == EV_DISABLE);
    CHECK(trace.events[1] == EV_PIN);
    CHECK(trace.events[2] == EV_TASK_LOCK);
    CHECK(trace.events[3] == EV_READY_LOCK);
    CHECK(trace.events[4] == EV_WAIT_LOCK);
    CHECK(trace.events[5] == EV_RUNNING_LOCK);
    CHECK(trace.events[6] == EV_RUNNING_UNLOCK);
    CHECK(trace.events[7] == EV_WAIT_UNLOCK);
    CHECK(trace.events[8] == EV_READY_UNLOCK);
    CHECK(trace.events[9] == EV_TASK_UNLOCK);
    CHECK(trace.events[10] == EV_UNPIN);
    CHECK(trace.events[11] == EV_POKE0);
    CHECK(trace.events[12] == EV_POKE1);
    CHECK(trace.events[13] == EV_ENABLE);
    CHECK(trace.task_depth == 0u && trace.list_depth == 0u &&
          trace.held_count == 0u);
    fixture_destroy(&fixture);
}

static void test_ready_online_affinity_filter(void)
{
    struct Fixture fixture;
    int8_t old_priority = -9;
    uint32_t poke_mask = UINT32_MAX;
    int result = 0;

    fixture_init(&fixture, 1u); /* hart 1 is not online yet */
    setup_task(&fixture, 0, TS_READY, 4, 3u);
    direct_priority_change(&fixture, &fixture.tasks[0], 7,
                           &old_priority, &poke_mask, &result);
    CHECK(result == 1);
    CHECK(old_priority == 4);
    CHECK(poke_mask == 1u);
    CHECK(fixture.tasks[0].tc_Node.ln_Pri == 7);
    fixture_destroy(&fixture);
}

static void test_wait_changes_priority_without_reordering_or_poke(void)
{
    struct Fixture fixture;
    struct Trace trace;
    int8_t old_priority = -9;
    uint32_t poke_mask = UINT32_MAX;
    int result = 0;
    struct Node *expected[2];
    BYTE old;

    fixture_init(&fixture, 3u);
    setup_task(&fixture, 0, TS_WAIT, 5, 3u);
    setup_task(&fixture, 1, TS_WAIT, 15, 3u);
    expected[0] = &fixture.tasks[0].tc_Node;
    expected[1] = &fixture.tasks[1].tc_Node;
    direct_priority_change(&fixture, &fixture.tasks[0], 30,
                           &old_priority, &poke_mask, &result);
    CHECK(result == 1);
    CHECK(old_priority == 5);
    CHECK(poke_mask == 0u);
    CHECK(fixture.tasks[0].tc_Node.ln_Pri == 30);
    CHECK(list_nodes_are(&fixture.base.pub.TaskWait, expected, 2u));

    bind_thread(&fixture, &trace, 0);
    old = SetTaskPri(&fixture.tasks[1], 1, &fixture.base.pub);
    CHECK(old == 15);
    CHECK(fixture.tasks[1].tc_Node.ln_Pri == 1);
    CHECK(poke_count == 0u);
    CHECK(trace.events[trace.count - 1u] == EV_ENABLE);
    fixture_destroy(&fixture);
}

static void test_running_task_pokes_only_actual_owner(void)
{
    unsigned int hart;
    for (hart = 0; hart < 2u; ++hart)
    {
        struct Fixture fixture;
        struct Trace trace;
        BYTE old;
        fixture_init(&fixture, 3u);
        setup_task(&fixture, 0, TS_RUN, 8, 3u);
        set_running_owner(&fixture, 0, hart);
        bind_thread(&fixture, &trace, 0);
        old = SetTaskPri(&fixture.tasks[0], 9, &fixture.base.pub);
        CHECK(old == 8);
        CHECK(fixture.tasks[0].tc_Node.ln_Pri == 9);
        CHECK(poke_count == 1u);
        CHECK(poke_mask_seen == (UINT32_C(1) << hart));
        CHECK(trace.events[trace.count - 2u] ==
              (hart == 0u ? EV_POKE0 : EV_POKE1));
        CHECK(trace.events[trace.count - 1u] == EV_ENABLE);
        fixture_destroy(&fixture);
        poke_count = 0;
        poke_mask_seen = 0;
    }
}

static void test_missing_corrupt_retired_and_run_owner_refusals(void)
{
    struct Fixture fixture;
    struct QueueImage before;
    int8_t old_priority;
    uint32_t poke_mask;
    int result;

    fixture_init(&fixture, 3u);
    setup_task(&fixture, 0, TS_READY, 11, 3u);
    fixture_remove(&fixture.tasks[0].tc_Node); /* state but no Ready member */
    queue_image_capture(&fixture, &before);
    old_priority = -55;
    poke_mask = UINT32_C(0xC0FFEE);
    direct_priority_change(&fixture, &fixture.tasks[0], 99,
                           &old_priority, &poke_mask, &result);
    CHECK(result == 0);
    CHECK(old_priority == -55 && poke_mask == UINT32_C(0xC0FFEE));
    CHECK(fixture.tasks[0].tc_Node.ln_Pri == 11);
    CHECK(queue_image_unchanged(&fixture, &before));
    fixture_destroy(&fixture);

    fixture_init(&fixture, 3u);
    setup_task(&fixture, 0, TS_READY, 11, 3u);
    fixture.base.pub.TaskReady.lh_TailPred = &fixture.tasks[1].tc_Node;
    queue_image_capture(&fixture, &before);
    old_priority = -56;
    poke_mask = UINT32_C(0x1234);
    direct_priority_change(&fixture, &fixture.tasks[0], 99,
                           &old_priority, &poke_mask, &result);
    CHECK(result == 0);
    CHECK(old_priority == -56 && poke_mask == UINT32_C(0x1234));
    CHECK(fixture.tasks[0].tc_Node.ln_Pri == 11);
    CHECK(fixture.base.pub.TaskReady.lh_Head == &fixture.tasks[0].tc_Node);
    CHECK(queue_image_unchanged(&fixture, &before));
    fixture_destroy(&fixture);

    fixture_init(&fixture, 3u);
    setup_task(&fixture, 0, TS_READY, 11, 3u);
    registry_lock();
    CHECK(p4_task_registry_retire_locked(&fixture.registry,
                                         &fixture.tasks[0]));
    registry_unlock();
    queue_image_capture(&fixture, &before);
    old_priority = -57;
    poke_mask = UINT32_C(0x4321);
    direct_priority_change(&fixture, &fixture.tasks[0], 99,
                           &old_priority, &poke_mask, &result);
    CHECK(result == 0);
    CHECK(old_priority == -57 && poke_mask == UINT32_C(0x4321));
    CHECK(fixture.tasks[0].tc_Node.ln_Pri == 11);
    CHECK(queue_image_unchanged(&fixture, &before));
    fixture_destroy(&fixture);

    fixture_init(&fixture, 3u);
    setup_task(&fixture, 0, TS_RUN, 11, 3u);
    registry_lock();
    CHECK(p4_task_registry_enter_running_locked(&fixture.registry,
                                                 &fixture.tasks[0], 1u));
    CHECK(p4_task_registry_leave_running_locked(&fixture.registry,
                                                &fixture.tasks[0], 1u));
    registry_unlock();
    queue_image_capture(&fixture, &before);
    old_priority = -58;
    poke_mask = UINT32_C(0x5678);
    direct_priority_change(&fixture, &fixture.tasks[0], 99,
                           &old_priority, &poke_mask, &result);
    CHECK(result == 0);
    CHECK(old_priority == -58 && poke_mask == UINT32_C(0x5678));
    CHECK(fixture.tasks[0].tc_Node.ln_Pri == 11);
    CHECK(queue_image_unchanged(&fixture, &before));
    fixture_destroy(&fixture);
}

static void test_bad_arguments_and_unavailable_ready_affinity(void)
{
    struct Fixture fixture;
    int8_t old_priority = -41;
    uint32_t poke_mask = UINT32_C(0x1111);

    fixture_init(&fixture, 3u);
    CHECK(krnP4TaskPriorityLocked(NULL, 4, &old_priority, &poke_mask,
                                  &fixture.base.pub) == 0);
    CHECK(krnP4TaskPriorityLocked(&fixture.tasks[0], 4, NULL, &poke_mask,
                                  &fixture.base.pub) == 0);
    CHECK(krnP4TaskPriorityLocked(&fixture.tasks[0], 4, &old_priority, NULL,
                                  &fixture.base.pub) == 0);
    CHECK(krnP4TaskPriorityLocked(&fixture.tasks[0], 4, &old_priority,
                                  &poke_mask, NULL) == 0);
    CHECK(old_priority == -41 && poke_mask == UINT32_C(0x1111));

    setup_task(&fixture, 0, TS_READY, 11, 0u);
    direct_priority_change(&fixture, &fixture.tasks[0], 99,
                           &old_priority, &poke_mask, &(int){0});
    CHECK(old_priority == -41 && poke_mask == UINT32_C(0x1111));
    CHECK(fixture.tasks[0].tc_Node.ln_Pri == 11);
    fixture_destroy(&fixture);
}

static void test_wrapper_null_current_and_pin_before_dereference(void)
{
    struct Fixture fixture;
    struct Trace trace;
    BYTE old;

    fixture_init(&fixture, 3u);
    setup_task(&fixture, 0, TS_WAIT, 7, 3u);
    bind_thread(&fixture, &trace, 0);
    tls_current_task = &fixture.tasks[0];
    old = SetTaskPri(NULL, 12, &fixture.base.pub);
    CHECK(old == 7);
    CHECK(pin_calls == 1u && unpin_calls == 1u);
    CHECK(fixture.tasks[0].tc_Node.ln_Pri == 12);
    CHECK(poke_count == 0u);
    fixture_destroy(&fixture);

    fixture_init(&fixture, 3u);
    bind_thread(&fixture, &trace, 0);
    tls_current_task = NULL;
    fatal_count = 0;
    fatal_armed = 1;
    if (setjmp(fatal_jump) == 0)
        (void)SetTaskPri(NULL, 12, &fixture.base.pub);
    fatal_armed = 0;
    CHECK(fatal_count == 1u);
    CHECK(pin_calls == 0u && unpin_calls == 0u);
    CHECK(trace.count == 1u && trace.events[0] == EV_DISABLE);
    CHECK(tls_irq_disabled != 0);
    fixture_destroy(&fixture);

    /* Registry lookup compares raw identity only. A pre-pin Task dereference
     * would fault on this deliberately invalid, unpublished pointer. */
    fixture_init(&fixture, 3u);
    bind_thread(&fixture, &trace, 0);
    fatal_count = 0;
    fatal_reason = 0;
    fatal_armed = 1;
    if (setjmp(fatal_jump) == 0)
        (void)SetTaskPri((struct Task *)(uintptr_t)1u, 12,
                         &fixture.base.pub);
    fatal_armed = 0;
    CHECK(fatal_count == 1u);
    CHECK(fatal_reason == P4_PUBLIC_SPIN_BAD_STATE);
    CHECK(pin_calls == 1u && unpin_calls == 0u);
    CHECK(trace.count == 2u && trace.events[0] == EV_DISABLE &&
          trace.events[1] == EV_PIN);
    CHECK(tls_irq_disabled != 0);
    fixture_destroy(&fixture);
}

static void test_wrapper_migration_after_pin_before_snapshot(void)
{
    struct Fixture fixture;
    struct Trace trace;
    struct Node *expected[2];
    BYTE old;

    fixture_init(&fixture, 3u);
    setup_task(&fixture, 0, TS_WAIT, 4, 3u);
    setup_task(&fixture, 1, TS_READY, 15, 3u);
    migrate_after_pin = &fixture.tasks[0];
    bind_thread(&fixture, &trace, 0);
    old = SetTaskPri(&fixture.tasks[0], 20, &fixture.base.pub);
    CHECK(old == 4);
    CHECK(fixture.tasks[0].tc_State == TS_READY);
    CHECK(fixture.tasks[0].tc_Node.ln_Pri == 20);
    expected[0] = &fixture.tasks[0].tc_Node;
    expected[1] = &fixture.tasks[1].tc_Node;
    CHECK(list_nodes_are(&fixture.base.pub.TaskReady, expected, 2u));
    CHECK(poke_count == 2u && poke_mask_seen == 3u);
    CHECK(trace.events[trace.count - 3u] == EV_POKE0);
    CHECK(trace.events[trace.count - 2u] == EV_POKE1);
    CHECK(trace.events[trace.count - 1u] == EV_ENABLE);
    fixture_destroy(&fixture);
    poke_count = 0;
    poke_mask_seen = 0;
}

static void test_wrapper_retirement_after_pin_is_locked_snapshot(void)
{
    struct Fixture fixture;
    struct Trace trace;
    BYTE old;

    fixture_init(&fixture, 3u);
    setup_task(&fixture, 0, TS_WAIT, 6, 3u);
    retire_after_pin = &fixture.tasks[0];
    bind_thread(&fixture, &trace, 0);
    old = SetTaskPri(&fixture.tasks[0], 99, &fixture.base.pub);
    CHECK(old == 6);
    CHECK(fixture.tasks[0].tc_State == TS_REMOVED);
    CHECK(fixture.tasks[0].tc_Node.ln_Pri == 6);
    CHECK(fixture.base.pub.TaskWait.lh_Head ==
          (struct Node *)&fixture.base.pub.TaskWait.lh_Tail);
    CHECK(fixture.records[0].service_pending == 1u);
    CHECK(fixture.records[0].lifetime.notification_pins == 1u);
    CHECK(poke_count == 0u);
    CHECK(trace.events[trace.count - 2u] == EV_UNPIN);
    CHECK(trace.events[trace.count - 1u] == EV_ENABLE);
    fixture_destroy(&fixture);
}

static void test_wrapper_poisons_task_at_final_unpin(void)
{
    struct Fixture fixture;
    struct Trace trace;
    BYTE old;

    fixture_init(&fixture, 3u);
    setup_task(&fixture, 0, TS_WAIT, 33, 3u);
    bind_thread(&fixture, &trace, 0);
    poison_identity = &fixture.tasks[0];
    poison_on_final_unpin = 1;
    old = SetTaskPri(&fixture.tasks[0], 12, &fixture.base.pub);
    CHECK(old == 33);
    CHECK(fixture.tasks[0].tc_Node.ln_Pri == INT8_MIN);
    CHECK(fixture.tasks[0].tc_State == UINT8_MAX);
    CHECK(fixture.tasks[0].test_etask == NULL);
    CHECK(unpin_calls == 1u);
    CHECK(trace.events[trace.count - 1u] == EV_ENABLE);
    CHECK(poke_count == 0u);
    fixture_destroy(&fixture);
    poison_identity = NULL;
}

static void test_wrapper_helper_refusal_unpins_before_fatal(void)
{
    struct Fixture fixture;
    struct Trace trace;
    struct QueueImage before;

    fixture_init(&fixture, 3u);
    setup_task(&fixture, 0, TS_READY, 13, 3u);
    fixture_remove(&fixture.tasks[0].tc_Node);
    queue_image_capture(&fixture, &before);
    bind_thread(&fixture, &trace, 0);
    fatal_count = 0;
    fatal_reason = 0;
    fatal_armed = 1;
    if (setjmp(fatal_jump) == 0)
        (void)SetTaskPri(&fixture.tasks[0], 22, &fixture.base.pub);
    fatal_armed = 0;
    CHECK(fatal_count == 1u);
    CHECK(fatal_reason == P4_PUBLIC_SPIN_BAD_STATE);
    CHECK(pin_calls == 1u && unpin_calls == 1u);
    CHECK(poke_count == 0u);
    CHECK(queue_image_unchanged(&fixture, &before));
    CHECK(trace.held_count == 0u && trace.task_depth == 0u &&
          trace.list_depth == 0u);
    CHECK(trace.events[trace.count - 1u] == EV_UNPIN);
    CHECK(tls_irq_disabled != 0); /* fatal occurred before outer Enable */
    fixture_destroy(&fixture);
    tls_irq_disabled = 0;
}

struct WorkerArgs
{
    struct Fixture *fixture;
    struct Trace trace;
    int result;
    BYTE old_priority;
};

static void *scheduler_worker(void *argument)
{
    struct WorkerArgs *args = argument;
    bind_thread(args->fixture, &args->trace, 0);
    args->trace.role = ROLE_SCHEDULER;
    tls_irq_disabled = 1; /* outer trap: MIE already clear */
    args->result = krnP4ShouldSchedule(&args->fixture->base.pub);
    tls_irq_disabled = 0;
    return NULL;
}

static void *priority_worker(void *argument)
{
    struct WorkerArgs *args = argument;
    bind_thread(args->fixture, &args->trace, 1);
    args->trace.role = ROLE_PRIORITY;
    tls_current_task = &args->fixture->tasks[0];
    args->old_priority = SetTaskPri(&args->fixture->tasks[1], 25,
                                    &args->fixture->base.pub);
    return NULL;
}

static void test_scheduler_decision_serializes_with_priority_change(void)
{
    struct Fixture fixture;
    struct WorkerArgs scheduler_args, priority_args;
    struct Trace main_trace;
    pthread_t scheduler_thread, priority_thread;
    int scheduler_result;

    fixture_init(&fixture, 3u);
    setup_task(&fixture, 0, TS_RUN, 20, 3u);
    set_running_owner(&fixture, 0, 0);
    setup_task(&fixture, 1, TS_READY, 15, 3u);
    fixture.locals[0].current_task = &fixture.tasks[0];
    fixture.locals[0].schedule_flags = P4_EXEC_SCHEDULE_SWITCH;

    CHECK(pthread_mutex_lock(&race_mutex) == 0);
    race_enabled = 1;
    race_scheduler_at_priority = 0;
    race_release_scheduler = 0;
    race_priority_waiting_for_ready = 0;
    CHECK(pthread_mutex_unlock(&race_mutex) == 0);

    memset(&scheduler_args, 0, sizeof(scheduler_args));
    memset(&priority_args, 0, sizeof(priority_args));
    scheduler_args.fixture = &fixture;
    priority_args.fixture = &fixture;
    CHECK(pthread_create(&scheduler_thread, NULL, scheduler_worker,
                         &scheduler_args) == 0);

    CHECK(pthread_mutex_lock(&race_mutex) == 0);
    while (!race_scheduler_at_priority)
        CHECK(pthread_cond_wait(&race_condition, &race_mutex) == 0);
    CHECK(pthread_mutex_unlock(&race_mutex) == 0);

    CHECK(pthread_create(&priority_thread, NULL, priority_worker,
                         &priority_args) == 0);
    CHECK(pthread_mutex_lock(&race_mutex) == 0);
    while (!race_priority_waiting_for_ready)
        CHECK(pthread_cond_wait(&race_condition, &race_mutex) == 0);
    race_release_scheduler = 1;
    CHECK(pthread_cond_broadcast(&race_condition) == 0);
    CHECK(pthread_mutex_unlock(&race_mutex) == 0);

    CHECK(pthread_join(scheduler_thread, NULL) == 0);
    CHECK(pthread_join(priority_thread, NULL) == 0);
    scheduler_result = scheduler_args.result;
    CHECK(race_priority_waiting_for_ready == 1);
    CHECK(scheduler_result == 0); /* saw old queued priority 15 <= current 20 */
    CHECK(priority_args.old_priority == 15);
    CHECK(fixture.tasks[1].tc_Node.ln_Pri == 25);
    CHECK(fixture.base.pub.TaskReady.lh_Head == &fixture.tasks[1].tc_Node);
    CHECK(poke_count == 2u && poke_mask_seen == 3u);
    CHECK(atomic_load_explicit(&schedule_priority_reads,
                               memory_order_relaxed) >= 1u);

    /* After the priority writer releases Ready, the same actual decision
     * helper sees the new queue head and requests a switch. */
    race_enabled = 0;
    bind_thread(&fixture, &main_trace, 0);
    tls_irq_disabled = 1;
    CHECK(krnP4ShouldSchedule(&fixture.base.pub) == 1);
    tls_irq_disabled = 0;
    CHECK(main_trace.events[0] == EV_TASK_LOCK);
    CHECK(main_trace.events[1] == EV_READY_LOCK);
    CHECK(main_trace.events[2] == EV_READY_UNLOCK);
    CHECK(main_trace.events[3] == EV_TASK_UNLOCK);

    fixture_destroy(&fixture);
    poke_count = 0;
    poke_mask_seen = 0;
}

int main(void)
{
    atomic_init(&checks, 0u);
    atomic_init(&failures, 0u);
    atomic_init(&schedule_priority_reads, 0u);
    test_ready_reorders_and_returns_all_online_harts();
    test_ready_online_affinity_filter();
    test_wait_changes_priority_without_reordering_or_poke();
    test_running_task_pokes_only_actual_owner();
    test_missing_corrupt_retired_and_run_owner_refusals();
    test_bad_arguments_and_unavailable_ready_affinity();
    test_wrapper_null_current_and_pin_before_dereference();
    test_wrapper_migration_after_pin_before_snapshot();
    test_wrapper_retirement_after_pin_is_locked_snapshot();
    test_wrapper_poisons_task_at_final_unpin();
    test_wrapper_helper_refusal_unpins_before_fatal();
    test_scheduler_decision_serializes_with_priority_change();

    printf("P4 priority fixture: %u checks, %u failures\n",
           atomic_load_explicit(&checks, memory_order_relaxed),
           atomic_load_explicit(&failures, memory_order_relaxed));
    return atomic_load_explicit(&failures, memory_order_relaxed) == 0u ? 0 : 1;
}
