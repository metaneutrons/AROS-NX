/* Host fixture for the actual rom/exec/childwait.c function body. */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define __AROSEXEC_SMP__ 1
#define __AROSPLATFORM_SMP__ 1
#define P4_CHILD_WAIT_BODY_TEST 1

typedef uint32_t ULONG;
typedef uintptr_t IPTR;
typedef uint8_t UBYTE;
typedef struct ExecBase ExecBase;

struct ExecBase { int unused; };

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

struct TestLock { int held; };

struct MsgPort
{
    struct List mp_MsgList;
    struct TestLock mp_SpinLock;
};

struct ETask
{
    struct Node et_Node;
    ULONG et_UniqueID;
    struct MsgPort et_TaskMsgPort;
};

struct Task
{
    UBYTE tc_Flags;
    union { struct ETask *tc_ETask; } tc_UnionETask;
};

#define TF_ETASK UINT32_C(8)
#define SIGF_CHILD UINT32_C(2)
#define CHILD_NOTNEW ((IPTR)1)
#define SPINLOCK_MODE_READ 0

#define AROS_LHA(type, name, reg) type name
#define AROS_LH1(type, name, arg1, base_type, base, lvo, library) \
    type name(arg1, base_type base __attribute__((unused)))
#define AROS_LIBFUNC_INIT
#define AROS_LIBFUNC_EXIT
#define GET_THIS_TASK mock_current_task
#define Forbid() mock_forbid()
#define Permit() mock_permit()
#ifdef P4_TEST_STRIP_PORT_IRQ_GATE
#define Disable() mock_noop_irq()
#define Enable() mock_noop_irq()
#else
#define Disable() mock_disable()
#define Enable() mock_enable()
#endif
#define SetSignal(new_signals, signal_set) \
    mock_set_signal((new_signals), (signal_set))
#define Wait(signal_set) mock_wait((signal_set))
#define EXEC_SPINLOCK_LOCK(lock, hook, mode) \
    mock_lock((lock), (hook), (mode))
#define EXEC_SPINLOCK_UNLOCK(lock) mock_unlock((lock))
#define ForeachNode(list, node) \
    for ((node) = mock_foreach_first((list)); \
         mock_foreach_valid((node)); \
         (node) = mock_foreach_next((list), (node)))

#define CHECK(condition) do { \
    ++check_count; \
    if (!(condition)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        exit(2); \
    } \
} while (0)

enum injection_stage
{
    INJECT_NONE,
    INJECT_BEFORE_CLEAR,
    INJECT_AFTER_CLEAR_BEFORE_QUERY,
    INJECT_AFTER_EMPTY_UNLOCK
};

struct wait_event { ULONG unique_id; };

static struct Task *mock_current_task;
static struct ETask parent_etask;
static struct Task parent_task;
static struct ETask child_storage[8];
static size_t child_count;
static enum injection_stage inject_stage;
static ULONG inject_unique_id;
static struct wait_event wait_events[4];
static size_t wait_event_count;
static size_t wait_event_index;
static ULONG pending_signals;
static unsigned int clear_calls;
static unsigned int scan_calls;
static unsigned int lock_calls;
static unsigned int unlock_calls;
static unsigned int wait_calls;
static unsigned int forbid_depth;
static unsigned int forbid_calls;
static unsigned int permit_calls;
static unsigned int irq_depth;
static unsigned int irq_entry_depth;
static unsigned int disable_calls;
static unsigned int enable_calls;
static unsigned int check_count;

static void list_init(struct List *list)
{
    list->lh_Head = (struct Node *)&list->lh_Tail;
    list->lh_Tail = NULL;
    list->lh_TailPred = (struct Node *)&list->lh_Head;
}

static struct ETask *append_child(ULONG unique_id)
{
    struct ETask *child;
    struct Node *previous;

    CHECK(child_count < (sizeof(child_storage) / sizeof(child_storage[0])));
    child = &child_storage[child_count++];
    child->et_UniqueID = unique_id;
    previous = parent_etask.et_TaskMsgPort.mp_MsgList.lh_TailPred;
    child->et_Node.ln_Succ =
        (struct Node *)&parent_etask.et_TaskMsgPort.mp_MsgList.lh_Tail;
    child->et_Node.ln_Pred = previous;
    previous->ln_Succ = &child->et_Node;
    parent_etask.et_TaskMsgPort.mp_MsgList.lh_TailPred = &child->et_Node;
    return child;
}

static void publish_child(ULONG unique_id)
{
    (void)append_child(unique_id);
    pending_signals |= SIGF_CHILD;
}

static struct ETask *mock_foreach_first(struct List *list)
{
    ++scan_calls;
    CHECK(parent_etask.et_TaskMsgPort.mp_SpinLock.held != 0);
    CHECK(irq_depth > irq_entry_depth);
    CHECK(clear_calls == scan_calls);
    return (struct ETask *)(void *)list->lh_Head;
}

static int mock_foreach_valid(struct ETask *child)
{
    CHECK(child != NULL);
    return ((struct Node *)(void *)child)->ln_Succ != NULL;
}

static struct ETask *mock_foreach_next(struct List *list, struct ETask *child)
{
    struct Node *next;
    (void)list;
    CHECK(parent_etask.et_TaskMsgPort.mp_SpinLock.held != 0);
    CHECK(irq_depth > irq_entry_depth);
    next = ((struct Node *)(void *)child)->ln_Succ;
    return (struct ETask *)(void *)next;
}

static void mock_forbid(void)
{
    ++forbid_calls;
    ++forbid_depth;
}

static void mock_permit(void)
{
    CHECK(forbid_depth != 0);
    --forbid_depth;
    ++permit_calls;
}

static void __attribute__((unused)) mock_noop_irq(void) { }

static void __attribute__((unused)) mock_disable(void)
{
    ++irq_depth;
    ++disable_calls;
}

static void __attribute__((unused)) mock_enable(void)
{
    CHECK(irq_depth > irq_entry_depth);
    CHECK(parent_etask.et_TaskMsgPort.mp_SpinLock.held == 0);
    if (irq_depth > irq_entry_depth)
        --irq_depth;
    ++enable_calls;
    if (inject_stage == INJECT_AFTER_EMPTY_UNLOCK)
    {
        inject_stage = INJECT_NONE;
        publish_child(inject_unique_id);
    }
}

static ULONG mock_set_signal(ULONG new_signals, ULONG signal_set)
{
    ULONG previous;

    CHECK(parent_etask.et_TaskMsgPort.mp_SpinLock.held == 0);
    CHECK(irq_depth == irq_entry_depth);
    CHECK(new_signals == 0);
    CHECK(signal_set == SIGF_CHILD);
    if (inject_stage == INJECT_BEFORE_CLEAR)
    {
        inject_stage = INJECT_NONE;
        publish_child(inject_unique_id);
    }

    previous = pending_signals;
    pending_signals = (pending_signals & ~signal_set) |
                      (new_signals & signal_set);
    ++clear_calls;
    return previous;
}

static ULONG mock_wait(ULONG signal_set)
{
    unsigned int saved_forbid;

    CHECK(parent_etask.et_TaskMsgPort.mp_SpinLock.held == 0);
    CHECK(irq_depth == irq_entry_depth);
    CHECK(disable_calls == enable_calls);
    CHECK(signal_set == SIGF_CHILD);
    ++wait_calls;

    /* Model Wait dropping Forbid while it sleeps, then restoring the caller's
     * nesting when that same Task resumes. A missing notification is fatal. */
    saved_forbid = forbid_depth;
    forbid_depth = 0;

    if ((pending_signals & signal_set) == 0 &&
        wait_event_index < wait_event_count)
    {
        ULONG unique_id = wait_events[wait_event_index++].unique_id;
        publish_child(unique_id);
    }

    CHECK((pending_signals & signal_set) != 0);
    pending_signals &= ~signal_set;
    forbid_depth = saved_forbid;
    return signal_set;
}

static void mock_lock(struct TestLock *lock, void *hook, int mode)
{
    CHECK(lock == &parent_etask.et_TaskMsgPort.mp_SpinLock);
    CHECK(hook == NULL);
    CHECK(mode == SPINLOCK_MODE_READ);
    CHECK(lock->held == 0);
    CHECK(irq_depth > irq_entry_depth);
    if (inject_stage == INJECT_AFTER_CLEAR_BEFORE_QUERY)
    {
        inject_stage = INJECT_NONE;
        publish_child(inject_unique_id);
    }
    lock->held = 1;
    ++lock_calls;
}

static void mock_unlock(struct TestLock *lock)
{
    CHECK(lock == &parent_etask.et_TaskMsgPort.mp_SpinLock);
    CHECK(lock->held != 0);
    CHECK(irq_depth > irq_entry_depth);
    lock->held = 0;
    ++unlock_calls;
}

#include "../../../../rom/exec/childwait.c"

static void reset_fixture_with_irq_depth(unsigned int initial_depth)
{
    parent_etask = (struct ETask){0};
    parent_task = (struct Task){0};
    list_init(&parent_etask.et_TaskMsgPort.mp_MsgList);
    parent_task.tc_Flags = (UBYTE)TF_ETASK;
    parent_task.tc_UnionETask.tc_ETask = &parent_etask;
    mock_current_task = &parent_task;
    child_count = 0;
    inject_stage = INJECT_NONE;
    inject_unique_id = 0;
    wait_event_count = 0;
    wait_event_index = 0;
    pending_signals = 0;
    clear_calls = 0;
    scan_calls = 0;
    lock_calls = 0;
    unlock_calls = 0;
    wait_calls = 0;
    forbid_depth = 0;
    forbid_calls = 0;
    permit_calls = 0;
    irq_depth = initial_depth;
    irq_entry_depth = initial_depth;
    disable_calls = 0;
    enable_calls = 0;
}

static void reset_fixture(void)
{
    reset_fixture_with_irq_depth(0u);
}

static void check_balanced_success(void)
{
    CHECK(forbid_calls == 1);
    CHECK(permit_calls == 1);
    CHECK(forbid_depth == 0);
    CHECK(irq_depth == irq_entry_depth);
    CHECK(disable_calls == enable_calls);
    CHECK(parent_etask.et_TaskMsgPort.mp_SpinLock.held == 0);
    CHECK(lock_calls == unlock_calls);
    CHECK(clear_calls == scan_calls);
}

static void test_already_queued_and_any(void)
{
    struct ETask *first;
    struct ETask *second;

    reset_fixture();
    first = append_child(11);
    second = append_child(22);
    (void)second;
    pending_signals = SIGF_CHILD;
    CHECK(ChildWait(0, NULL) == (IPTR)first);
    check_balanced_success();
    CHECK(wait_calls == 0);
    CHECK(clear_calls == 1 && scan_calls == 1);

    reset_fixture();
    (void)append_child(71);
    second = append_child(72);
    CHECK(ChildWait(72, NULL) == (IPTR)second);
    check_balanced_success();
    CHECK(wait_calls == 0);
    CHECK(scan_calls == 1);
}

static void test_before_clear_and_after_clear_before_query(void)
{
    struct ETask *expected;

    reset_fixture();
    inject_stage = INJECT_BEFORE_CLEAR;
    inject_unique_id = 31;
    expected = &child_storage[0];
    CHECK(ChildWait(31, NULL) == (IPTR)expected);
    check_balanced_success();
    CHECK(wait_calls == 0);
    CHECK(pending_signals == 0);

    reset_fixture();
    inject_stage = INJECT_AFTER_CLEAR_BEFORE_QUERY;
    inject_unique_id = 42;
    expected = &child_storage[0];
    CHECK(ChildWait(42, NULL) == (IPTR)expected);
    check_balanced_success();
    CHECK(wait_calls == 0);
}

static void test_exit_between_empty_scan_and_wait(void)
{
    struct ETask *expected;

    reset_fixture();
    inject_stage = INJECT_AFTER_EMPTY_UNLOCK;
    inject_unique_id = 51;
    expected = &child_storage[0];
    CHECK(ChildWait(51, NULL) == (IPTR)expected);
    check_balanced_success();
    CHECK(wait_calls == 1);
    CHECK(clear_calls == 2 && scan_calls == 2);
    CHECK((pending_signals & SIGF_CHILD) == 0);
}

static void test_exit_during_wait_and_unrelated_then_matching(void)
{
    struct ETask *expected;

    reset_fixture();
    wait_events[0].unique_id = 61;
    wait_events[1].unique_id = 62;
    wait_event_count = 2;
    expected = &child_storage[1];
    CHECK(ChildWait(62, NULL) == (IPTR)expected);
    check_balanced_success();
    CHECK(wait_calls == 2);
    CHECK(clear_calls == 3 && scan_calls == 3);
}

static void test_not_new_task_paths(void)
{
    reset_fixture();
    mock_current_task = NULL;
    CHECK(ChildWait(0, NULL) == CHILD_NOTNEW);
    CHECK(forbid_calls == 0 && clear_calls == 0 && wait_calls == 0);

    reset_fixture();
    parent_task.tc_Flags = 0;
    parent_task.tc_UnionETask.tc_ETask = NULL;
    CHECK(ChildWait(7, NULL) == CHILD_NOTNEW);
    CHECK(forbid_calls == 0 && clear_calls == 0 && wait_calls == 0);
}

static void test_pre_disabled_caller_and_wait_after_enable(void)
{
    struct ETask *expected;

    reset_fixture_with_irq_depth(1u);
    expected = append_child(81u);
    pending_signals = SIGF_CHILD;
    CHECK(ChildWait(81u, NULL) == (IPTR)expected);
    check_balanced_success();
    CHECK(irq_depth == 1u && disable_calls == 1u && enable_calls == 1u);
    CHECK(wait_calls == 0u);

    reset_fixture_with_irq_depth(1u);
    wait_events[0].unique_id = 82u;
    wait_event_count = 1u;
    expected = &child_storage[0];
    CHECK(ChildWait(82u, NULL) == (IPTR)expected);
    check_balanced_success();
    CHECK(irq_depth == 1u && disable_calls == enable_calls);
    CHECK(wait_calls == 1u);
}

int main(void)
{
    test_already_queued_and_any();
    test_before_clear_and_after_clear_before_query();
    test_exit_between_empty_scan_and_wait();
    test_exit_during_wait_and_unrelated_then_matching();
    test_not_new_task_paths();
    test_pre_disabled_caller_and_wait_after_enable();
    printf("PASS: %u checks over actual ChildWait body\n", check_count);
    return 0;
}
