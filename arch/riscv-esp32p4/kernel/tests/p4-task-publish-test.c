/* Host fixture for the actual P4 task-publication helpers. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define P4_TASK_PINS_TEST 1
#define P4_PUBLIC_SPINLOCK_TEST 1
#include "../p4_public_spinlock.h"
#include "../p4_task_registry.h"
#include "../p4_task_publish.h"
#include "../../exec/p4_signal_protocol.h"

struct Node
{
    struct Node *ln_Succ;
    struct Node *ln_Pred;
    int8_t ln_Pri;
};

struct List
{
    struct Node *lh_Head;
    struct Node *lh_Tail;
    struct Node *lh_TailPred;
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
    spinlock_t tc_SpinLock;
    struct IntETask *test_etask;
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

struct test_cpu_local
{
    struct Task *current_task;
    int32_t id_nest;
    int32_t td_nest;
};

struct p4_exec_runtime
{
    uint32_t hart_id;
    struct test_cpu_local *cpu_local;
};

#define TS_INVALID 0u
#define TS_ADDED 1u
#define TS_RUN 2u
#define TS_READY 3u
#define TS_WAIT 4u
#define SPINLOCK_MODE_WRITE 1u
#define MEMF_PUBLIC 1u
#define MEMF_CLEAR 2u
#define PrivExecBase(base) (&(base)->internal)
#define GetIntETask(task) ((task)->test_etask)
#define EXEC_SPINLOCK_LOCK(lock, hook, mode) \
    test_exec_lock((lock), (mode))
#define EXEC_SPINLOCK_UNLOCK(lock) test_exec_unlock((lock))

enum test_event
{
    EV_ALLOC = 1,
    EV_FREE,
    EV_TASK_LOCK,
    EV_READY_LOCK,
    EV_WAIT_LOCK,
    EV_RUNNING_LOCK,
    EV_TASK_UNLOCK,
    EV_READY_UNLOCK,
    EV_WAIT_UNLOCK,
    EV_RUNNING_UNLOCK,
    EV_ENQUEUE_READY,
    EV_ENQUEUE_RUNNING,
    EV_CURRENT_TASK,
    EV_FINAL_ENABLE,
    EV_REG_UNPIN,
    EV_POKE
};

struct test_event_record
{
    enum test_event event;
    const void *object;
};

static unsigned int checks;
static struct ExecBase base;
static struct test_cpu_local cpu_local;
static struct p4_exec_runtime fixture_runtime;
static unsigned int cpu_count = 2;
static unsigned int mock_irq_depth;
static int32_t mock_id_nest = -1;
static int32_t mock_td_nest;
static int mock_mie = 1;
static int mock_gate_held;
static unsigned int mock_gate_epoch;
static unsigned int mock_gate_acquires;
static unsigned int mock_registry_registers;
static unsigned int mock_registry_pins;
static unsigned int mock_registry_enter_running;
static unsigned int mock_registry_violations;
static struct p4_task_registry_record *last_record;
static struct Task *mock_task_lock_owner;
static spinlock_t *exec_lock_stack[8];
static unsigned int exec_lock_depth;
static struct test_event_record events[256];
static unsigned int event_count;
static void (*enable_callback)(void);
static int alloc_fail;
static void *allocations[32];
static int allocation_live[32];
static unsigned int allocation_count;
static unsigned int free_count;
static void *last_freed;
static int fatal_count;
static struct p4_signal_effect last_poke;
static unsigned int poke_count;
static struct Task *poison_on_enable;
static unsigned int poison_seen;
static unsigned int current_task_calls;

#define CHECK(condition)                                                     \
    do                                                                       \
    {                                                                        \
        ++checks;                                                            \
        if (!(condition))                                                    \
        {                                                                    \
            fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition);   \
            abort();                                                         \
        }                                                                    \
    } while (0)

static void event(enum test_event event, const void *object)
{
    CHECK(event_count < sizeof(events) / sizeof(events[0]));
    events[event_count].event = event;
    events[event_count].object = object;
    ++event_count;
}

static void clear_events(void)
{
    event_count = 0;
}

static int test_exec_lock(spinlock_t *lock, unsigned int mode)
{
    CHECK(lock != NULL);
    CHECK(mode == SPINLOCK_MODE_WRITE);
    CHECK(mock_irq_depth != 0 && mock_mie == 0);
    CHECK(exec_lock_depth < sizeof(exec_lock_stack) / sizeof(exec_lock_stack[0]));
    CHECK(mock_task_lock_owner != NULL);
    if (lock == &mock_task_lock_owner->tc_SpinLock)
    {
        CHECK(exec_lock_depth == 0);
        event(EV_TASK_LOCK, mock_task_lock_owner);
    }
    else if (lock == &base.internal.TaskReadySpinLock)
    {
        CHECK(mock_task_lock_owner != NULL);
        CHECK(exec_lock_depth >= 1);
        event(EV_READY_LOCK, lock);
    }
    else if (lock == &base.internal.TaskWaitSpinLock)
    {
        CHECK(mock_task_lock_owner != NULL);
        CHECK(exec_lock_depth >= 2);
        event(EV_WAIT_LOCK, lock);
    }
    else if (lock == &base.internal.TaskRunningSpinLock)
    {
        CHECK(mock_task_lock_owner != NULL);
        CHECK(exec_lock_depth >= 1);
        event(EV_RUNNING_LOCK, lock);
    }
    else
        CHECK(0 && "unexpected Exec spinlock");
    exec_lock_stack[exec_lock_depth++] = lock;
    return 0;
}

static int test_exec_unlock(spinlock_t *lock)
{
    CHECK(exec_lock_depth != 0);
    CHECK(exec_lock_stack[exec_lock_depth - 1u] == lock);
    if (lock == &mock_task_lock_owner->tc_SpinLock)
    {
        CHECK(exec_lock_depth == 1);
        event(EV_TASK_UNLOCK, mock_task_lock_owner);
        mock_task_lock_owner = NULL;
    }
    else if (lock == &base.internal.TaskReadySpinLock)
        event(EV_READY_UNLOCK, lock);
    else if (lock == &base.internal.TaskWaitSpinLock)
        event(EV_WAIT_UNLOCK, lock);
    else if (lock == &base.internal.TaskRunningSpinLock)
        event(EV_RUNNING_UNLOCK, lock);
    else
        CHECK(0 && "unexpected Exec spinlock release");
    --exec_lock_depth;
    return 0;
}

void Disable(void);
void Enable(void);
void *AllocMem(size_t size, uint32_t flags);
void FreeMem(void *memory, size_t size);
void Remove(struct Node *node);
void Enqueue(struct List *list, struct Node *node);
unsigned int krnP4GetCPUCount(void);
int krnP4CPUInMask(uint32_t number, const void *mask);
struct p4_exec_runtime *p4_exec_runtime_current(struct ExecBase *sys_base);
int p4_exec_runtime_set_current_task(struct ExecBase *sys_base,
    struct Task *task);
int krnP4NewTaskReadyLocked(struct Task *task, struct ExecBase *sys_base);
int krnP4BootTaskRunningLocked(struct Task *task, struct ExecBase *sys_base);
void krnP4SignalPoke(const struct p4_signal_effect *effect,
    struct ExecBase *sys_base);

#include "../p4_task_lifetime.c"
#include "../p4_task_registry.c"

void test_registry_init(struct p4_task_registry *registry);
int test_registry_register(struct p4_task_registry *registry,
    struct p4_task_registry_record *record, struct Task *identity);
int test_registry_pin(struct p4_task_registry *registry,
    struct Task *identity);
int test_registry_unpin(struct p4_task_registry *registry,
    struct Task *identity);
int test_registry_enter_running(struct p4_task_registry *registry,
    struct Task *identity, uint32_t hart);

#define p4_task_registry_init test_registry_init
#define p4_task_registry_register_locked test_registry_register
#define p4_task_registry_pin_locked test_registry_pin
#define p4_task_registry_unpin_locked test_registry_unpin
#define p4_task_registry_enter_running_locked test_registry_enter_running
#include "../p4_task_pins.c"
#undef p4_task_registry_init
#undef p4_task_registry_register_locked
#undef p4_task_registry_pin_locked
#undef p4_task_registry_unpin_locked
#undef p4_task_registry_enter_running_locked

/* Registry publication fixture: transport remains a mock, but notification
 * may not run while its registry gate is held or use an invalid copied hart. */
void krnP4CleanupPoke(struct ExecBase *sys_base)
{
    (void)sys_base;
    CHECK(!mock_gate_held);
}

void krnP4RetiredOwnerPoke(uint32_t destination, struct ExecBase *sys_base)
{
    (void)sys_base;
    CHECK(!mock_gate_held);
    CHECK(destination < 2u);
}

#define P4_SCHEDULER_QUEUE_TEST 1
#define P4_TASK_PUBLICATION_TEST 1
#include "../p4_scheduler_queue.c"

#define P4_TASK_PUBLISH_TEST 1
#include "../p4_task_publish.c"

void test_registry_init(struct p4_task_registry *registry)
{
    CHECK(mock_mie == 0);
    CHECK(!mock_gate_held);
    p4_task_registry_init(registry);
}

static void check_registry_context(void)
{
    if (!mock_gate_held || mock_mie != 0 || mock_irq_depth == 0)
    {
        ++mock_registry_violations;
        CHECK(mock_gate_held);
        CHECK(mock_mie == 0);
        CHECK(mock_irq_depth != 0);
    }
}

int test_registry_register(struct p4_task_registry *registry,
    struct p4_task_registry_record *record, struct Task *identity)
{
    int result;
    check_registry_context();
    ++mock_registry_registers;
    result = p4_task_registry_register_locked(registry, record, identity);
    if (result)
        last_record = record;
    return result;
}

int test_registry_pin(struct p4_task_registry *registry,
    struct Task *identity)
{
    check_registry_context();
    ++mock_registry_pins;
    return p4_task_registry_pin_locked(registry, identity);
}

int test_registry_unpin(struct p4_task_registry *registry,
    struct Task *identity)
{
    check_registry_context();
    event(EV_REG_UNPIN, identity);
    return p4_task_registry_unpin_locked(registry, identity);
}

int test_registry_enter_running(struct p4_task_registry *registry,
    struct Task *identity, uint32_t hart)
{
    check_registry_context();
    ++mock_registry_enter_running;
    return p4_task_registry_enter_running_locked(registry, identity, hart);
}

void Disable(void)
{
    ++mock_irq_depth;
    ++mock_id_nest;
    mock_mie = mock_id_nest < 0;
}

void Enable(void)
{
    void (*callback)(void);
    CHECK(mock_irq_depth != 0);
    --mock_irq_depth;
    --mock_id_nest;
    mock_mie = mock_id_nest < 0;
    if (mock_irq_depth == 0)
    {
        event(EV_FINAL_ENABLE, NULL);
        callback = enable_callback;
        enable_callback = NULL;
        if (callback != NULL)
            callback();
    }
}

void *AllocMem(size_t size, uint32_t flags)
{
    void *memory;
    CHECK(mock_irq_depth == 0);
    CHECK(!mock_gate_held && exec_lock_depth == 0);
    CHECK((flags & (MEMF_PUBLIC | MEMF_CLEAR)) ==
          (MEMF_PUBLIC | MEMF_CLEAR));
    event(EV_ALLOC, NULL);
    if (alloc_fail)
        return NULL;
    CHECK(allocation_count < sizeof(allocations) / sizeof(allocations[0]));
    memory = calloc(1, size);
    CHECK(memory != NULL);
    allocations[allocation_count] = memory;
    allocation_live[allocation_count] = 1;
    ++allocation_count;
    return memory;
}

void FreeMem(void *memory, size_t size)
{
    unsigned int index;
    (void)size;
    CHECK(mock_irq_depth == 0);
    CHECK(!mock_gate_held && exec_lock_depth == 0);
    for (index = 0; index < allocation_count; ++index)
        if (allocations[index] == memory && allocation_live[index])
            break;
    CHECK(index < allocation_count);
    allocation_live[index] = 0;
    last_freed = memory;
    ++free_count;
    event(EV_FREE, memory);
    free(memory);
}

enum p4_public_spinlock_result krnP4PublicSpinInit(spinlock_t *lock)
{
    CHECK(lock != NULL && mock_irq_depth != 0 && mock_mie == 0);
    CHECK(!mock_gate_held);
    lock->lock = 0;
    lock->s_Owner = 0;
    return P4_PUBLIC_SPIN_OK;
}

enum p4_public_spinlock_result krnP4PublicSpinTryAcquire(
    spinlock_t *lock, uint32_t mode)
{
    CHECK(lock != NULL && mode == SPINLOCK_MODE_WRITE);
    CHECK(mock_irq_depth != 0 && mock_mie == 0 && !mock_gate_held);
    mock_gate_held = 1;
    ++mock_gate_epoch;
    ++mock_gate_acquires;
    lock->lock = SPINLOCK_MODE_WRITE;
    return P4_PUBLIC_SPIN_OK;
}

enum p4_public_spinlock_result krnP4PublicSpinUnlock(spinlock_t *lock)
{
    CHECK(lock != NULL && mock_gate_held);
    CHECK(mock_irq_depth != 0 && mock_mie == 0);
    mock_gate_held = 0;
    lock->lock = 0;
    lock->s_Owner = 0;
    return P4_PUBLIC_SPIN_OK;
}

void krnP4PublicSpinFatal(enum p4_public_spinlock_result reason)
{
    (void)reason;
    ++fatal_count;
    abort();
}

unsigned int krnP4GetCPUCount(void)
{
    return cpu_count;
}

int krnP4CPUInMask(uint32_t number, const void *mask)
{
    const uint32_t *bits = (const uint32_t *)mask;
    return bits != NULL && number < 32u &&
        ((*bits & (UINT32_C(1) << number)) != 0);
}

struct p4_exec_runtime *p4_exec_runtime_current(struct ExecBase *sys_base)
{
    return sys_base == &base ? &fixture_runtime : NULL;
}

int p4_exec_runtime_set_current_task(struct ExecBase *sys_base,
    struct Task *task)
{
    CHECK(sys_base == &base && task != NULL);
    CHECK(mock_irq_depth != 0 && mock_mie == 0 && mock_gate_held == 0);
    CHECK(mock_task_lock_owner == task);
    CHECK(task->tc_State == TS_RUN);
    CHECK(p4_queue_contains(&base.internal.TaskRunning, task) == 1);
    CHECK(cpu_local.current_task == NULL);
    cpu_local.current_task = task;
    ++current_task_calls;
    event(EV_CURRENT_TASK, task);
    return 1;
}

void Remove(struct Node *node)
{
    CHECK(node != NULL && node->ln_Pred != NULL && node->ln_Succ != NULL);
    node->ln_Pred->ln_Succ = node->ln_Succ;
    node->ln_Succ->ln_Pred = node->ln_Pred;
}

void Enqueue(struct List *list, struct Node *node)
{
    struct Node *next = list->lh_Head;
    CHECK(mock_task_lock_owner != NULL && mock_irq_depth != 0);
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
    if (list == &base.TaskReady)
        event(EV_ENQUEUE_READY, node);
    else if (list == &base.internal.TaskRunning)
        event(EV_ENQUEUE_RUNNING, node);
    else
        CHECK(0 && "unexpected task list");
}

static void init_list(struct List *list)
{
    list->lh_Head = (struct Node *)&list->lh_Tail;
    list->lh_Tail = NULL;
    list->lh_TailPred = (struct Node *)&list->lh_Head;
}

static void init_fixture(void)
{
    memset(&base, 0, sizeof(base));
    memset(&cpu_local, 0, sizeof(cpu_local));
    fixture_runtime.hart_id = 0;
    fixture_runtime.cpu_local = &cpu_local;
    cpu_local.id_nest = 0;
    cpu_local.td_nest = 0;
    cpu_count = 2;
    mock_irq_depth = 0;
    mock_id_nest = -1;
    mock_td_nest = 0;
    mock_mie = 1;
    mock_gate_held = 0;
    mock_task_lock_owner = NULL;
    exec_lock_depth = 0;
    event_count = 0;
    enable_callback = NULL;
    alloc_fail = 0;
    free_count = 0;
    last_freed = NULL;
    fatal_count = 0;
    poke_count = 0;
    poison_on_enable = NULL;
    poison_seen = 0;
    current_task_calls = 0;
    init_list(&base.TaskReady);
    init_list(&base.TaskWait);
    init_list(&base.internal.TaskRunning);
    Disable();
    CHECK(krnP4TaskPinsPrepare());
    Enable();
    CHECK(mock_irq_depth == 0 && mock_id_nest == -1 && mock_mie == 1);
}

struct fixture_affinity
{
    struct IntETask etask;
    uint32_t bits;
};

static void init_task(struct Task *task, struct fixture_affinity *affinity,
    unsigned int state, uint32_t affinity_bits)
{
    memset(task, 0, sizeof(*task));
    memset(affinity, 0, sizeof(*affinity));
    task->tc_State = state;
    task->test_etask = &affinity->etask;
    affinity->etask.iet_CpuAffinity = &affinity->bits;
    affinity->bits = affinity_bits;
}

static int list_contains(const struct List *list, const struct Task *task)
{
    const struct Node *tail = (const struct Node *)&list->lh_Tail;
    const struct Node *node = list->lh_Head;
    unsigned int seen = 0;
    while (node != NULL && node != tail && seen++ < 16u)
    {
        if (node == &task->tc_Node)
            return 1;
        node = node->ln_Succ;
    }
    return 0;
}

static struct p4_task_registry_record *find_record(struct Task *task)
{
    struct p4_task_registry_record *record = p4_task_records.head;
    unsigned int remaining = p4_task_records.count;
    while (record != NULL && remaining-- != 0)
    {
        if (record->identity == task)
            return record;
        record = record->next;
    }
    return NULL;
}

static int event_index(enum test_event wanted, const void *object,
    unsigned int start)
{
    unsigned int i;
    for (i = start; i < event_count; ++i)
        if (events[i].event == wanted &&
            (object == NULL || events[i].object == object))
            return (int)i;
    return -1;
}

static void poison_task_after_enable(void)
{
    CHECK(mock_irq_depth == 0 && mock_mie == 1);
    CHECK(exec_lock_depth == 0 && !mock_gate_held);
    CHECK(poison_on_enable != NULL);
    poison_on_enable->tc_State = UINT32_C(0xdeadbeef);
    poison_on_enable->test_etask = NULL;
    poison_seen++;
    poison_on_enable = NULL;
}

void krnP4SignalPoke(const struct p4_signal_effect *effect,
    struct ExecBase *sys_base)
{
    CHECK(effect != NULL && sys_base == &base);
    CHECK(mock_irq_depth != 0 && mock_mie == 0);
    CHECK(exec_lock_depth == 0 && mock_task_lock_owner == NULL);
    last_poke = *effect;
    ++poke_count;
    event(EV_POKE, NULL);
}

static void test_record_create_atomic_pin_and_failed_publication(void)
{
    static struct Task first, duplicate;
    static struct fixture_affinity first_etask, duplicate_etask;
    struct p4_task_registry_record *first_record;
    unsigned int alloc_before = allocation_count;
    unsigned int gate_before = mock_gate_acquires;
    unsigned int register_before = mock_registry_registers;
    unsigned int pin_before = mock_registry_pins;

    init_task(&first, &first_etask, TS_ADDED, 1u);
    init_task(&duplicate, &duplicate_etask, TS_ADDED, 1u);
    CHECK(krnP4TaskRecordCreate(&first, &base));
    first_record = find_record(&first);
    CHECK(first_record != NULL && first_record == last_record);
    CHECK(first_record->identity == &first);
    CHECK(first_record->lifetime.notification_pins == 1u);
    CHECK(first_record->lifetime.running_harts == 0u);
    CHECK(allocation_count == alloc_before + 1u);
    CHECK(mock_gate_acquires == gate_before + 1u);
    CHECK(mock_registry_registers == register_before + 1u);
    CHECK(mock_registry_pins == pin_before + 1u);
    CHECK(mock_registry_violations == 0u);

    alloc_before = allocation_count;
    gate_before = mock_gate_acquires;
    CHECK(!krnP4TaskRecordCreate(NULL, &base));
    CHECK(!krnP4TaskRecordCreate(&duplicate, NULL));
    CHECK(allocation_count == alloc_before);
    CHECK(mock_gate_acquires == gate_before);

    /* A duplicate identity rejects only the newly allocated record. */
    alloc_before = allocation_count;
    gate_before = mock_gate_acquires;
    register_before = mock_registry_registers;
    pin_before = mock_registry_pins;
    CHECK(!krnP4TaskRecordCreate(&first, &base));
    CHECK(allocation_count == alloc_before + 1u);
    CHECK(mock_gate_acquires == gate_before + 1u);
    CHECK(mock_registry_registers == register_before + 1u);
    CHECK(mock_registry_pins == pin_before);
    CHECK(free_count == 1u && last_freed != first_record);
    CHECK(find_record(&first) == first_record);
    CHECK(first_record->lifetime.notification_pins == 1u);

    /* Allocation failure cannot publish or pin the second identity. */
    alloc_before = allocation_count;
    gate_before = mock_gate_acquires;
    alloc_fail = 1;
    CHECK(!krnP4TaskRecordCreate(&duplicate, &base));
    alloc_fail = 0;
    CHECK(allocation_count == alloc_before);
    CHECK(mock_gate_acquires == gate_before);
    CHECK(find_record(&duplicate) == NULL);
    CHECK(p4_task_records.count == 1u);
}

static void test_new_task_ready_copies_effect_before_enable(void)
{
    static struct Task task;
    static struct fixture_affinity affinity;
    struct p4_task_registry_record *record;
    int result;

    init_task(&task, &affinity, TS_ADDED, 2u);
    CHECK(krnP4TaskRecordCreate(&task, &base));
    record = find_record(&task);
    CHECK(record != NULL && record->lifetime.notification_pins == 1u);
    clear_events();
    poison_on_enable = &task;
    enable_callback = poison_task_after_enable;
    mock_task_lock_owner = &task;
    result = krnP4NewTaskReady(&task, &base);
    CHECK(result == 2);
    CHECK(poison_seen == 1u);
    CHECK(task.tc_State == UINT32_C(0xdeadbeef));
    CHECK(task.test_etask == NULL);
    CHECK(poke_count == 1u);
    CHECK(last_poke.status == P4_SIGNAL_STATUS_OK);
    CHECK(last_poke.request_reschedule == 1u);
    CHECK(last_poke.reschedule_hart == 1u);
    CHECK(event_index(EV_TASK_LOCK, &task, 0) >= 0);
    CHECK(event_index(EV_READY_LOCK, NULL, 0) <
          event_index(EV_WAIT_LOCK, NULL, 0));
    CHECK(event_index(EV_WAIT_LOCK, NULL, 0) <
          event_index(EV_RUNNING_LOCK, NULL, 0));
    CHECK(event_index(EV_RUNNING_UNLOCK, NULL, 0) <
          event_index(EV_WAIT_UNLOCK, NULL, 0));
    CHECK(event_index(EV_WAIT_UNLOCK, NULL, 0) <
          event_index(EV_READY_UNLOCK, NULL, 0));
    CHECK(event_index(EV_READY_UNLOCK, NULL, 0) <
          event_index(EV_TASK_UNLOCK, &task, 0));
    CHECK(event_index(EV_TASK_UNLOCK, &task, 0) <
          event_index(EV_FINAL_ENABLE, NULL, 0));
    CHECK(event_index(EV_TASK_UNLOCK, &task, 0) <
          event_index(EV_POKE, NULL, 0));
    CHECK(event_index(EV_POKE, NULL, 0) <
          event_index(EV_FINAL_ENABLE, NULL, 0));
    CHECK(mock_irq_depth == 0 && mock_mie == 1);
    CHECK(exec_lock_depth == 0 && mock_task_lock_owner == NULL);
    /* Unpin uses only the stable identity key; the Task bytes were poisoned. */
    krnP4SignalTaskUnpin(&task, &base);
    CHECK(record->lifetime.notification_pins == 0u);
}

static void test_new_task_ready_rejections_are_quiet(void)
{
    static struct Task task;
    static struct fixture_affinity affinity;
    struct p4_task_registry_record *record;
    unsigned int gate_before;
    unsigned int event_before;

    init_task(&task, &affinity, TS_ADDED, 0u);
    CHECK(krnP4TaskRecordCreate(&task, &base));
    record = find_record(&task);
    CHECK(record != NULL && record->lifetime.notification_pins == 1u);
    poke_count = 0;
    gate_before = mock_gate_acquires;
    event_before = event_count;
    CHECK(krnP4NewTaskReady(&task, NULL) == 0);
    CHECK(mock_gate_acquires == gate_before);
    CHECK(event_count == event_before);
    CHECK(task.tc_State == TS_ADDED);
    CHECK(!list_contains(&base.TaskReady, &task));
    CHECK(poke_count == 0u);

    clear_events();
    mock_task_lock_owner = &task;
    CHECK(krnP4NewTaskReady(&task, &base) == 0);
    CHECK(task.tc_State == TS_ADDED);
    CHECK(!list_contains(&base.TaskReady, &task));
    CHECK(event_index(EV_TASK_LOCK, &task, 0) >= 0);
    CHECK(event_index(EV_READY_LOCK, NULL, 0) < 0);
    CHECK(poke_count == 0u);
    krnP4SignalTaskUnpin(&task, &base);
    CHECK(record->lifetime.notification_pins == 0u);
}

static void reset_boot_predicates(void)
{
    mock_id_nest = 0;
    mock_td_nest = 0;
    mock_mie = 0;
    mock_irq_depth = 0;
    fixture_runtime.hart_id = 0;
    fixture_runtime.cpu_local = &cpu_local;
    cpu_local.current_task = NULL;
    cpu_local.id_nest = 0;
    cpu_local.td_nest = 0;
    cpu_count = 1;
}

static void check_boot_rejected(struct Task *task)
{
    unsigned int alloc_before = allocation_count;
    unsigned int gates_before = mock_gate_acquires;
    CHECK(!krnP4BootTaskRegister(task, &base));
    CHECK(allocation_count == alloc_before);
    CHECK(mock_gate_acquires == gates_before);
    CHECK(task->tc_State == TS_INVALID);
    CHECK(find_record(task) == NULL);
}

static void test_boot_predicates_fail_before_creation(void)
{
    static struct Task task;
    static struct fixture_affinity etask;
    init_task(&task, &etask, TS_INVALID, 1u);
    reset_boot_predicates();
    fixture_runtime.hart_id = 1;
    check_boot_rejected(&task);
    reset_boot_predicates();
    fixture_runtime.cpu_local = NULL;
    check_boot_rejected(&task);
    reset_boot_predicates();
    cpu_local.current_task = &task;
    check_boot_rejected(&task);
    reset_boot_predicates();
    cpu_local.id_nest = -1;
    check_boot_rejected(&task);
    reset_boot_predicates();
    cpu_local.td_nest = -1;
    check_boot_rejected(&task);
    reset_boot_predicates();
    cpu_count = 0;
    check_boot_rejected(&task);
    reset_boot_predicates();
    cpu_count = 2;
    check_boot_rejected(&task);
    reset_boot_predicates();
    CHECK(!krnP4BootTaskRegister(&task, NULL));
    CHECK(find_record(&task) == NULL);
    CHECK(task.tc_State == TS_INVALID);
    reset_boot_predicates();
    {
        unsigned int records_before = p4_task_records.count;
        CHECK(!krnP4BootTaskRegister(NULL, &base));
        CHECK(p4_task_records.count == records_before);
    }
    reset_boot_predicates();
    alloc_fail = 1;
    CHECK(!krnP4BootTaskRegister(&task, &base));
    alloc_fail = 0;
    CHECK(find_record(&task) == NULL);
    CHECK(task.tc_State == TS_INVALID);
}

static void test_boot_registration_order(void)
{
    static struct Task task;
    static struct fixture_affinity etask;
    struct p4_task_registry_record *record;
    int task_lock, running_lock, enqueue_running, current, task_unlock;
    int unpin, enable;

    init_task(&task, &etask, TS_INVALID, 1u);
    reset_boot_predicates();
    clear_events();
    mock_task_lock_owner = &task;
    CHECK(krnP4BootTaskRegister(&task, &base));
    record = find_record(&task);
    CHECK(record != NULL);
    CHECK(record->lifetime.notification_pins == 0u);
    CHECK(record->lifetime.running_harts == 1u);
    CHECK(task.tc_State == TS_RUN);
    CHECK(cpu_local.current_task == &task);
    CHECK(list_contains(&base.internal.TaskRunning, &task));
    CHECK(current_task_calls == 1u);
    CHECK(mock_registry_enter_running == 1u);

    task_lock = event_index(EV_TASK_LOCK, &task, 0);
    running_lock = event_index(EV_RUNNING_LOCK, NULL, 0);
    enqueue_running = event_index(EV_ENQUEUE_RUNNING, &task.tc_Node, 0);
    current = event_index(EV_CURRENT_TASK, &task, 0);
    task_unlock = event_index(EV_TASK_UNLOCK, &task, 0);
    enable = event_index(EV_FINAL_ENABLE, NULL, (unsigned int)task_unlock + 1u);
    CHECK(task_lock >= 0 && running_lock > task_lock);
    CHECK(enqueue_running > running_lock);
    CHECK(current > enqueue_running);
    CHECK(task_unlock > current);
    CHECK(enable > task_unlock);
    /* Creator pin is dropped after RUN/current publication and before Enable. */
    unpin = event_index(EV_REG_UNPIN, &task, 0);
    CHECK(unpin > current && unpin < enable);
    CHECK(mock_id_nest == 0 && mock_td_nest == 0 && mock_mie == 0);
    CHECK(mock_irq_depth == 0);
}

static void cleanup_allocations(void)
{
    unsigned int i;
    p4_task_registry_init(&p4_task_records);
    for (i = 0; i < allocation_count; ++i)
        if (allocation_live[i])
        {
            allocation_live[i] = 0;
            free(allocations[i]);
        }
}

int main(void)
{
    init_fixture();
    test_record_create_atomic_pin_and_failed_publication();
    test_new_task_ready_copies_effect_before_enable();
    test_new_task_ready_rejections_are_quiet();
    test_boot_predicates_fail_before_creation();
    test_boot_registration_order();
    CHECK(mock_registry_violations == 0u);
    CHECK(fatal_count == 0);
    cleanup_allocations();
    printf("p4-task-publish: %u checks passed\n", checks);
    return 0;
}
