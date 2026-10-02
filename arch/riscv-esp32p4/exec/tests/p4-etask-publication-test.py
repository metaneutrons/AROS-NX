#!/usr/bin/env python3
"""Compile actual NewAddTask/ETask-publication code against host mocks."""
from pathlib import Path
import hashlib
import os
import subprocess
import tempfile


REPO = Path(__file__).resolve().parents[4]
NEWADD_PATH = REPO / "rom/exec/newaddtask.c"
UTIL_PATH = REPO / "rom/exec/exec_util.c"
newadd_source = NEWADD_PATH.read_text()
util_source = UTIL_PATH.read_text()


def extract_braced(source: str, signature: str) -> str:
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 0
    state = "code"
    i = opening
    while i < len(source):
        c = source[i]
        n = source[i + 1] if i + 1 < len(source) else ""
        if state == "code":
            if c == "/" and n == "*":
                state = "block_comment"
                i += 2
                continue
            if c == "/" and n == "/":
                state = "line_comment"
                i += 2
                continue
            if c == '"':
                state = "string"
            elif c == "'":
                state = "character"
            elif c == "{":
                depth += 1
            elif c == "}":
                depth -= 1
                if depth == 0:
                    return source[start:i + 1]
        elif state == "block_comment":
            if c == "*" and n == "/":
                state = "code"
                i += 2
                continue
        elif state == "line_comment":
            if c == "\n":
                state = "code"
        elif state == "string":
            if c == "\\":
                i += 2
                continue
            if c == '"':
                state = "code"
        elif state == "character":
            if c == "\\":
                i += 2
                continue
            if c == "'":
                state = "code"
        i += 1
    raise ValueError(f"unterminated function after {signature!r}")


lib_start = newadd_source.index("AROS_LH4(APTR, NewAddTask,")
body_start = newadd_source.index("AROS_LIBFUNC_INIT", lib_start) + len(
    "AROS_LIBFUNC_INIT"
)
body_end = newadd_source.index("AROS_LIBFUNC_EXIT", body_start)
newadd_body = newadd_source[body_start:body_end]
task_launch = extract_braced(newadd_source, "static void TaskLaunch(")
publish_helper = extract_braced(
    util_source, "void Exec_PublishConstructedETask("
)


PRELUDE = r'''
#define _GNU_SOURCE
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

typedef uint32_t ULONG;
typedef uint16_t UWORD;
typedef uint8_t UBYTE;
typedef int32_t LONG;
typedef intptr_t IPTR;
typedef void *APTR;
typedef const char *CONST_STRPTR;
typedef void *RAWARG;
typedef int BOOL;

#define TRUE 1
#define FALSE 0
#define MEMF_PUBLIC 1u
#define MEMF_CLEAR 2u
#define TF_ETASK 0x0040u
#define TF_STACKCHK 0x0080u
#define TF_SWITCH 0x0100u
#define TF_LAUNCH 0x0200u
#define EXECF_StackSnoop 0x0002u
#define NT_TASK 1u
#define TS_ADDED 2u
#define TS_READY 3u
#define TS_RUN 4u
#define TS_REMOVED 5u
#define SP_OFFSET 0u
#define AROS_STACKALIGN 16u
#define TAG_DONE 0u
#define TASKTAG_PRELAUNCHHOOK 11u
#define TASKTAG_ERROR 1u
#define TASKERROR_OK 0u
#define TASKERROR_NOMEMORY 1u
#define RAWFMTFUNC_STRING ((void (*)(void))0)
#define ASSERT_VALID_PTR(ptr) ((void)(ptr))
#define DADDTASK(...) ((void)0)
#define D(...) ((void)0)
#define NEWLIST(list) InitList(list)
#define PrivExecBase(base) (base)
#define GET_THIS_TASK (currentTask)
#define IntETask(et) ((struct IntETask *)(et))
#define CALLHOOKPKT(hook, object, message) \
    call_hook((hook), (object), (message))

#if defined(TEST_STAGED)
#define __AROSEXEC_SMP__ 1
#define __AROSPLATFORM_SMP__ 1
#define EXEC_PLATFORM_ETASK_STAGED 1
#define EXEC_PLATFORM_TASK_RECORD_CREATE(task) \
    task_record_create((task))
#define EXEC_PLATFORM_TASK_RECORD_RELEASE(task) \
    task_record_release((task))
#define EXEC_PLATFORM_NEW_TASK_READY(task) task_ready((task))
#define EXEC_PLATFORM_TASK_FATAL() task_fatal()
#define EXEC_SPINLOCK_INIT(lock) task_spin_init((lock))
#define EXEC_SPINLOCK_LOCK(lock, hook, mode) \
    task_spin_lock((lock), (hook), (mode))
#define EXEC_SPINLOCK_UNLOCK(lock) task_spin_unlock((lock))
#define SPINLOCK_MODE_WRITE 1u
#endif

struct Node {
    struct Node *ln_Succ;
    struct Node *ln_Pred;
    UBYTE ln_Type;
    int8_t ln_Pri;
    char *ln_Name;
};

struct List {
    struct Node *lh_Head;
    struct Node *lh_Tail;
    struct Node *lh_TailPred;
};

struct MemEntry {
    union { ULONG meu_Reqs; APTR meu_Addr; } me_Un;
    size_t me_Length;
};
#define me_Reqs me_Un.meu_Reqs
#define me_Addr me_Un.meu_Addr

struct MemList {
    struct Node ml_Node;
    UWORD ml_NumEntries;
    struct MemEntry ml_ME[1];
};

struct TagItem { ULONG ti_Tag; IPTR ti_Data; };
struct MsgPort {
    struct Node mp_Node;
    ULONG mp_SigBit;
    struct Task *mp_SigTask;
    struct List mp_MsgList;
};
struct Hook {
    void (*entry)(struct Hook *, APTR, APTR);
};

struct ETask {
    struct Node node;
    struct Task *et_Parent;
    struct List et_Children;
    APTR et_RegFrame;
    struct MsgPort et_TaskMsgPort;
};
struct IntETask {
    struct ETask pub;
    ULONG iet_TaskLock;
};

struct Task {
    struct Node tc_Node;
    struct List tc_MemEntry;
    LONG tc_IDNestCnt;
    LONG tc_TDNestCnt;
    ULONG tc_State;
    ULONG tc_Flags;
    ULONG tc_SigWait;
    ULONG tc_SigRecvd;
    ULONG tc_SigExcept;
    ULONG tc_SigAlloc;
    APTR tc_TrapCode;
    APTR tc_ExceptCode;
    APTR tc_SPLower;
    APTR tc_SPUpper;
    APTR tc_SPReg;
    union { struct ETask *tc_ETask; } tc_UnionETask;
    APTR tc_UserData;
    ULONG tc_Switch;
    ULONG tc_Launch;
    ULONG tc_SpinLock;
};

struct ExecBase {
    ULONG IntFlags;
    ULONG TaskSigAlloc;
    APTR TaskTrapCode;
    APTR TaskExceptCode;
    APTR TaskExitCode;
    struct List TaskReady;
};

static unsigned checks;
#define CHECK(condition) do { \
    ++checks; \
    if (!(condition)) { \
        fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); \
        exit(1); \
    } \
} while (0)

enum {
    EV_TASK_LOCK_INIT = 1,
    EV_INIT,
    EV_PREPARE,
    EV_RECORD_CREATE,
    EV_DISABLE,
    EV_LOCK,
    EV_ADD_HEAD,
    EV_UNLOCK,
    EV_ENABLE,
    EV_HOOK,
    EV_READY,
    EV_RECORD_RELEASE,
    EV_CLEANUP,
    EV_DELETE_CONTEXT,
    EV_FATAL,
    EV_ENQUEUE,
    EV_RESCHEDULE
};

static int events[128];
static size_t event_count;
static unsigned fatal_calls;
static unsigned init_calls;
static unsigned prepare_calls;
static unsigned cleanup_calls;
static unsigned delete_context_calls;
static unsigned parent_linked_notifications;
static unsigned record_create_calls;
static unsigned record_release_calls;
static unsigned ready_calls;
static unsigned hook_calls;
static unsigned creator_pins;
static int init_fails;
static int prepare_fails;
static int record_create_fails;
static int interrupt_enabled = 1;
static int task_lock_held;
static int poison_on_enable;
static struct Task *poison_child_task;
static struct Task *poison_parent_task;
static void *poison_child_page;
static void *poison_parent_page;
static size_t page_size;
static struct Task *currentTask;
static struct ExecBase exec_base;
static struct ExecBase *SysBase = &exec_base;
static struct Task *observed_init_parent;
static struct Hook *active_hook;
static int simulate_early_retirement;
static void mock_disable(void);
static void mock_enable(void);
#define Disable() do { (void)SysBase; mock_disable(); } while (0)
#define Enable() do { (void)SysBase; mock_enable(); } while (0)

static void event(int value)
{
    CHECK(event_count < sizeof(events) / sizeof(events[0]));
    events[event_count++] = value;
}

static void clear_events(void)
{
    event_count = 0;
}

#if defined(TEST_STAGED)
static int list_empty(const struct List *list)
{
    return list->lh_Head == (const struct Node *)&list->lh_Tail;
}
#endif

static int list_contains(const struct List *list, const struct Node *node)
{
    const struct Node *it = list->lh_Head;
    size_t bound = 32;
    while (it != (const struct Node *)&list->lh_Tail && bound-- != 0) {
        if (it == node)
            return 1;
        it = it->ln_Succ;
    }
    return 0;
}

static void InitList(struct List *list)
{
    list->lh_Head = (struct Node *)&list->lh_Tail;
    list->lh_Tail = NULL;
    list->lh_TailPred = (struct Node *)&list->lh_Head;
}

static void AddHead(struct List *list, struct Node *node)
{
    event(EV_ADD_HEAD);
    node->ln_Succ = list->lh_Head;
    node->ln_Pred = (struct Node *)&list->lh_Head;
    list->lh_Head->ln_Pred = node;
    list->lh_Head = node;
}

static void AddTail(struct List *list, struct Node *node)
{
    node->ln_Succ = (struct Node *)&list->lh_Tail;
    node->ln_Pred = list->lh_TailPred;
    list->lh_TailPred->ln_Succ = node;
    list->lh_TailPred = node;
}

static void Remove(struct Node *node)
{
    node->ln_Pred->ln_Succ = node->ln_Succ;
    node->ln_Succ->ln_Pred = node->ln_Pred;
    node->ln_Succ = NULL;
    node->ln_Pred = NULL;
}

static struct ETask *GetETask(struct Task *task)
{
    if (!task || !(task->tc_Flags & TF_ETASK))
        return NULL;
    return task->tc_UnionETask.tc_ETask;
}

static void *AllocMem(size_t size, ULONG flags)
{
    void *memory = malloc(size);
    if (memory && (flags & MEMF_CLEAR))
        memset(memory, 0, size);
    return memory;
}

static void FreeMem(APTR memory, size_t size)
{
    (void)size;
    free(memory);
}

static size_t memlist_size(UWORD count)
{
    return sizeof(struct MemList) - sizeof(struct MemEntry) +
           sizeof(struct MemEntry) * (size_t)count;
}

static void FreeEntry(struct MemList *entry)
{
    UWORD i;
    for (i = 0; i < entry->ml_NumEntries; ++i)
        FreeMem(entry->ml_ME[i].me_Addr, (size_t)entry->ml_ME[i].me_Length);
    FreeMem(entry, memlist_size(entry->ml_NumEntries));
}

static void RawDoFmt(CONST_STRPTR format, RAWARG args,
                     void (*output)(void), char *destination)
{
    (void)format;
    (void)args;
    (void)output;
    (void)destination;
}

static IPTR LibGetTagData(ULONG tag, IPTR default_value,
                          struct TagItem *tags)
{
    if (tag == TASKTAG_PRELAUNCHHOOK && tags && tags[0].ti_Tag == tag)
        return (IPTR)(uintptr_t)active_hook;
    return default_value;
}

static void call_hook(struct Hook *hook, APTR object, APTR message);

static BOOL InitETask(struct Task *task, struct Task *parent)
{
    struct IntETask *internal;
    ++init_calls;
    event(EV_INIT);
    observed_init_parent = parent;
    if (init_fails) {
        task->tc_UnionETask.tc_ETask = NULL;
        return FALSE;
    }
#if defined(TEST_STAGED)
    CHECK(task->tc_SpinLock == 0);
#else
    CHECK(task->tc_SpinLock == 0xa5a5a5a5u);
#endif
    internal = AllocMem(sizeof(*internal), MEMF_PUBLIC | MEMF_CLEAR);
    if (!internal)
        return FALSE;
    InitList(&internal->pub.et_Children);
    InitList(&internal->pub.et_TaskMsgPort.mp_MsgList);
    internal->pub.et_TaskMsgPort.mp_SigTask = task;
    task->tc_UnionETask.tc_ETask = &internal->pub;
    task->tc_Flags |= TF_ETASK;
    if (parent) {
        struct ETask *parent_et = GetETask(parent);
        internal->pub.et_Parent = parent;
        if (parent_et)
            AddHead(&parent_et->et_Children,
                    (struct Node *)&internal->pub);
    }
    return TRUE;
}

static void CleanupETask(struct Task *task)
{
    struct ETask *et = GetETask(task);
    ++cleanup_calls;
    event(EV_CLEANUP);
    if (!et)
        return;
    if (et->et_Parent)
        ++parent_linked_notifications;
    FreeMem(IntETask(et), sizeof(struct IntETask));
}

static BOOL PrepareContext(struct Task *task, APTR initial_pc,
                           APTR final_pc, struct TagItem *tag_list,
                           struct ExecBase *base)
{
    struct ETask *et = GetETask(task);
    (void)initial_pc;
    (void)final_pc;
    (void)tag_list;
    (void)base;
    ++prepare_calls;
    event(EV_PREPARE);
#if defined(TEST_STAGED)
    CHECK(observed_init_parent == NULL);
    CHECK(et && et->et_Parent == NULL);
    CHECK(list_empty(&((struct IntETask *)GetETask(currentTask))->
                     pub.et_Children));
#else
    CHECK(observed_init_parent == currentTask);
    CHECK(et && et->et_Parent == currentTask);
    CHECK(list_contains(&((struct IntETask *)GetETask(currentTask))->
                        pub.et_Children, (struct Node *)et));
#endif
    if (prepare_fails)
        return FALSE;
    if (et)
        et->et_RegFrame = (APTR)(uintptr_t)0x12340u;
    return TRUE;
}

static void mock_disable(void)
{
    CHECK(interrupt_enabled);
    interrupt_enabled = 0;
    event(EV_DISABLE);
}

static void mock_enable(void)
{
    CHECK(!task_lock_held);
    event(EV_ENABLE);
    interrupt_enabled = 1;
    if (poison_on_enable) {
        CHECK(poison_child_page != NULL && poison_parent_page != NULL);
        CHECK(mprotect(poison_child_page, page_size, PROT_NONE) == 0);
        CHECK(mprotect(poison_parent_page, page_size, PROT_NONE) == 0);
    }
}

#if defined(TEST_STAGED)
static void task_spin_init(ULONG *lock)
{
    *lock = 0;
    event(EV_TASK_LOCK_INIT);
}

static void task_spin_lock(ULONG *lock, void *hook, ULONG mode)
{
    (void)hook;
    CHECK(!interrupt_enabled);
    CHECK(mode == SPINLOCK_MODE_WRITE);
    CHECK(*lock == 0);
    *lock = 1;
    task_lock_held = 1;
    event(EV_LOCK);
}

static void task_spin_unlock(ULONG *lock)
{
    CHECK(!interrupt_enabled);
    CHECK(task_lock_held && *lock == 1);
    *lock = 0;
    task_lock_held = 0;
    event(EV_UNLOCK);
}

static int task_record_create(struct Task *task)
{
    struct ETask *et = GetETask(task);
    ++record_create_calls;
    event(EV_RECORD_CREATE);
    CHECK(prepare_calls == 1);
    CHECK(et && et->et_Parent == NULL);
    CHECK(et->node.ln_Succ == NULL && et->node.ln_Pred == NULL);
    CHECK(!task_lock_held);
    if (record_create_fails)
        return 0;
    CHECK(creator_pins == 0);
    creator_pins = 1;
    return 1;
}

static void task_record_release(struct Task *task)
{
    ++record_release_calls;
    event(EV_RECORD_RELEASE);
    CHECK(creator_pins == 1);
    CHECK(ready_calls == 1);
    CHECK(task == poison_child_task);
    creator_pins = 0;
    /* Return-pointer comparison is allowed; any later Task dereference faults. */
    CHECK(mprotect(poison_child_page, page_size, PROT_NONE) == 0);
}

static int task_ready(struct Task *task)
{
    ++ready_calls;
    event(EV_READY);
    CHECK(creator_pins == 1);
    CHECK(hook_calls == 1);
    CHECK(!task_lock_held && interrupt_enabled);
    CHECK(list_contains(&((struct IntETask *)GetETask(currentTask))->
                        pub.et_Children,
                        (struct Node *)GetETask(task)));
    CHECK(task->tc_Flags & TF_SWITCH);
    CHECK(task->tc_Flags & TF_LAUNCH);
    CHECK(task->tc_State == TS_ADDED);
    task->tc_State = simulate_early_retirement ? TS_REMOVED : TS_READY;
    return 1;
}

static void task_fatal(void)
{
    ++fatal_calls;
    event(EV_FATAL);
}
#else
static void Enqueue(struct List *list, struct Node *node)
{
    event(EV_ENQUEUE);
    AddTail(list, node);
}

static void Reschedule(void)
{
    event(EV_RESCHEDULE);
}
#endif

static void call_hook(struct Hook *hook, APTR object, APTR message)
{
    CHECK(hook && hook->entry);
    hook->entry(hook, object, message);
}

#if defined(TEST_STAGED)
static void test_prelaunch_hook(struct Hook *hook, APTR object, APTR message)
{
    struct Task *task = object;
    struct ETask *et = GetETask(task);
    (void)hook;
    (void)message;
    ++hook_calls;
    event(EV_HOOK);
    CHECK(creator_pins == 1);
    CHECK(ready_calls == 0);
    CHECK(!task_lock_held && interrupt_enabled);
    CHECK(et && et->et_Parent == currentTask);
    CHECK(list_contains(&((struct IntETask *)GetETask(currentTask))->
                        pub.et_Children, (struct Node *)et));
    CHECK(task->tc_State == TS_ADDED);
}
#endif

#if defined(TEST_STAGED)
static void KrnDeleteContext(APTR context)
{
    CHECK(context == (APTR)(uintptr_t)0x12340u);
    ++delete_context_calls;
    event(EV_DELETE_CONTEXT);
}
#endif

static struct Task *map_task(unsigned char fill)
{
    void *page = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(page != MAP_FAILED);
    memset(page, fill, page_size);
    return (struct Task *)page;
}

#if defined(TEST_STAGED)
static void unmap_task(struct Task *task)
{
    CHECK(mprotect(task, page_size, PROT_READ | PROT_WRITE) == 0);
    CHECK(munmap(task, page_size) == 0);
}
#endif

static void init_parent(struct Task *task, struct IntETask *internal)
{
    memset(task, 0, sizeof(*task));
    memset(internal, 0, sizeof(*internal));
    task->tc_Node.ln_Name = (char *)"parent";
    task->tc_Node.ln_Pri = 10;
    task->tc_State = TS_RUN;
    task->tc_Flags = TF_ETASK;
    task->tc_UnionETask.tc_ETask = &internal->pub;
    InitList(&internal->pub.et_Children);
    InitList(&internal->pub.et_TaskMsgPort.mp_MsgList);
    internal->pub.et_TaskMsgPort.mp_SigTask = task;
    internal->iet_TaskLock = 0;
}

static void init_new_task(struct Task *task)
{
    task->tc_Node.ln_Name = (char *)"child";
    task->tc_Node.ln_Pri = 5;
    task->tc_MemEntry.lh_Head = NULL;
    task->tc_SPLower = (APTR)(uintptr_t)0x20000u;
    task->tc_SPUpper = (APTR)(uintptr_t)0x22000u;
    task->tc_SPReg = task->tc_SPUpper;
    task->tc_SigAlloc = 0;
    task->tc_TrapCode = NULL;
    task->tc_ExceptCode = NULL;
    task->tc_Switch = 1;
    task->tc_Launch = 1;
}

static void reset_counters(void)
{
    clear_events();
    fatal_calls = 0;
    init_calls = 0;
    prepare_calls = 0;
    cleanup_calls = 0;
    delete_context_calls = 0;
    parent_linked_notifications = 0;
    record_create_calls = 0;
    record_release_calls = 0;
    ready_calls = 0;
    hook_calls = 0;
    creator_pins = 0;
    init_fails = 0;
    prepare_fails = 0;
    record_create_fails = 0;
    interrupt_enabled = 1;
    task_lock_held = 0;
    poison_on_enable = 0;
    poison_child_task = NULL;
    poison_parent_task = NULL;
    poison_child_page = NULL;
    poison_parent_page = NULL;
    observed_init_parent = NULL;
    active_hook = NULL;
    simulate_early_retirement = 0;
    memset(&exec_base, 0, sizeof(exec_base));
    InitList(&exec_base.TaskReady);
    SysBase = &exec_base;
}

#if defined(TEST_STAGED)
static int no_parent_link_or_notification(struct IntETask *parent_et)
{
    return list_empty(&parent_et->pub.et_Children) &&
           parent_linked_notifications == 0;
}
#endif

static APTR NewAddTask(struct Task *task, APTR initialPC, APTR finalPC,
                       struct TagItem *tagList);
#if defined(TEST_STAGED)
void Exec_PublishConstructedETask(struct Task *task, struct Task *parent,
                                  struct ExecBase *base);
#endif

#if defined(TEST_STAGED)
static void test_failure_paths(struct Task *parent,
                               struct IntETask *parent_et)
{
    struct Task *task;
    APTR result;

    currentTask = parent;
    init_parent(parent, parent_et);
    task = map_task(0xa5);
    init_new_task(task);
    task->tc_SpinLock = 0xa5a5a5a5u;
    init_fails = 1;
    clear_events();
    result = NewAddTask(task, (APTR)(uintptr_t)0x30000u, NULL, NULL);
    CHECK(result == NULL);
    CHECK(init_calls == 1 && prepare_calls == 0);
    CHECK(record_create_calls == 0 && cleanup_calls == 0);
    CHECK(no_parent_link_or_notification(parent_et));
    CHECK(task->tc_UnionETask.tc_ETask == NULL);
    CHECK((task->tc_Flags & TF_ETASK) == 0);
    CHECK(!list_contains(&parent_et->pub.et_Children,
                         (struct Node *)GetETask(task)));
    unmap_task(task);

    reset_counters();
    init_parent(parent, parent_et);
    currentTask = parent;
    task = map_task(0xa5);
    init_new_task(task);
    task->tc_SpinLock = 0xa5a5a5a5u;
    prepare_fails = 1;
    clear_events();
    result = NewAddTask(task, (APTR)(uintptr_t)0x30000u, NULL, NULL);
    CHECK(result == NULL);
    CHECK(init_calls == 1 && prepare_calls == 1);
    CHECK(record_create_calls == 0 && cleanup_calls == 1);
    CHECK(no_parent_link_or_notification(parent_et));
    CHECK(task->tc_UnionETask.tc_ETask == NULL);
    CHECK((task->tc_Flags & TF_ETASK) == 0);
    CHECK(GetETask(task) == NULL);
    unmap_task(task);

    reset_counters();
    init_parent(parent, parent_et);
    currentTask = parent;
    task = map_task(0xa5);
    init_new_task(task);
    task->tc_SpinLock = 0xa5a5a5a5u;
    record_create_fails = 1;
    clear_events();
    result = NewAddTask(task, (APTR)(uintptr_t)0x30000u, NULL, NULL);
    CHECK(result == NULL);
    CHECK(init_calls == 1 && prepare_calls == 1);
    CHECK(record_create_calls == 1 && record_release_calls == 0);
    CHECK(delete_context_calls == 1 && cleanup_calls == 1);
    CHECK(no_parent_link_or_notification(parent_et));
    CHECK(task->tc_UnionETask.tc_ETask == NULL);
    CHECK((task->tc_Flags & TF_ETASK) == 0);
    CHECK(GetETask(task) == NULL);
    unmap_task(task);
}

static void test_staged_success(struct Task *parent,
                                struct IntETask *parent_et)
{
    struct Task *task;
    struct Hook hook = { test_prelaunch_hook };
    struct TagItem tags[2];
    APTR result;
    const int expected[] = {
        EV_TASK_LOCK_INIT, EV_INIT, EV_PREPARE, EV_RECORD_CREATE,
        EV_DISABLE, EV_LOCK, EV_ADD_HEAD, EV_UNLOCK, EV_ENABLE,
        EV_HOOK, EV_READY, EV_RECORD_RELEASE
    };
    size_t i;

    reset_counters();
    init_parent(parent, parent_et);
    currentTask = parent;
    task = map_task(0xa5);
    init_new_task(task);
    task->tc_SpinLock = 0xa5a5a5a5u;
    poison_child_task = task;
    poison_child_page = task;
    simulate_early_retirement = 1;
    active_hook = &hook;
    tags[0].ti_Tag = TASKTAG_PRELAUNCHHOOK;
    tags[0].ti_Data = (IPTR)(uintptr_t)&hook;
    tags[1].ti_Tag = TAG_DONE;
    tags[1].ti_Data = 0;
    clear_events();

    result = NewAddTask(task, (APTR)(uintptr_t)0x30000u,
                        (APTR)(uintptr_t)0x31000u, tags);
    /* Task storage is protected in the final unpin callback. Only compare the
     * returned identity until the test explicitly reopens the mapping. */
    CHECK(result == task);
    CHECK(event_count == sizeof(expected) / sizeof(expected[0]));
    for (i = 0; i < event_count; ++i)
        CHECK(events[i] == expected[i]);
    CHECK(record_create_calls == 1 && record_release_calls == 1);
    CHECK(creator_pins == 0 && hook_calls == 1 && ready_calls == 1);
    CHECK(fatal_calls == 0 && cleanup_calls == 0);
    CHECK(parent_et->iet_TaskLock == 0 && !task_lock_held);
    CHECK(mprotect(task, page_size, PROT_READ | PROT_WRITE) == 0);
    CHECK(task->tc_SpinLock == 0);
    CHECK(task->tc_State == TS_REMOVED);
    CHECK(task->tc_Flags & TF_ETASK);
    CHECK(task->tc_Flags & TF_SWITCH);
    CHECK(task->tc_Flags & TF_LAUNCH);
    {
        struct ETask *et = GetETask(task);
        CHECK(et && et->et_Parent == parent);
        CHECK(list_contains(&parent_et->pub.et_Children,
                            (struct Node *)et));
        Remove((struct Node *)et);
        et->et_Parent = NULL;
    }
    CHECK(parent_linked_notifications == 0);
    CleanupETask(task);
    CHECK(cleanup_calls == 1 && parent_linked_notifications == 0);
    CHECK(munmap(task, page_size) == 0);
}

static void init_helper_task(struct Task *task, struct IntETask *internal)
{
    memset(task, 0, sizeof(*task));
    memset(internal, 0, sizeof(*internal));
    task->tc_Flags = TF_ETASK;
    task->tc_UnionETask.tc_ETask = &internal->pub;
    InitList(&internal->pub.et_Children);
    internal->iet_TaskLock = 0;
    internal->pub.node.ln_Succ = NULL;
    internal->pub.node.ln_Pred = NULL;
}

static void expect_helper_refusal_from(struct Task *task,
                                       struct Task *parent,
                                       struct Task *current)
{
    unsigned old_fatal = fatal_calls;
    size_t old_events = event_count;
    currentTask = current;
    Exec_PublishConstructedETask(task, parent, SysBase);
    CHECK(fatal_calls == old_fatal + 1);
    CHECK(event_count == old_events + 1);
    CHECK(events[event_count - 1] == EV_FATAL);
}

static void expect_helper_refusal(struct Task *task, struct Task *parent)
{
    expect_helper_refusal_from(task, parent, parent);
}

static void test_helper_validation_and_lock_order(void)
{
    struct Task *parent = map_task(0xa5);
    struct Task *child = map_task(0x5a);
    struct Task *other = map_task(0x3c);
    struct Task *plain_parent = map_task(0x69);
    struct IntETask parent_et;
    struct IntETask child_et;
    struct IntETask other_et;
    size_t before;
    const int expected[] = { EV_DISABLE, EV_LOCK, EV_ADD_HEAD,
                             EV_UNLOCK, EV_ENABLE };

    reset_counters();
    init_helper_task(parent, &parent_et);
    init_helper_task(child, &child_et);
    init_helper_task(other, &other_et);
    memset(plain_parent, 0, sizeof(*plain_parent));
    expect_helper_refusal(NULL, parent);
    CHECK(child_et.pub.et_Parent == NULL);
    expect_helper_refusal(plain_parent, parent);
    CHECK(GetETask(plain_parent) == NULL);
    expect_helper_refusal_from(child, other, parent);
    CHECK(child_et.pub.et_Parent == NULL);

    child_et.pub.et_Parent = other;
    expect_helper_refusal(child, parent);
    CHECK(child_et.pub.et_Parent == other);
    child_et.pub.et_Parent = NULL;
    child_et.pub.node.ln_Succ = (struct Node *)(uintptr_t)1u;
    child_et.pub.node.ln_Pred = (struct Node *)(uintptr_t)2u;
    expect_helper_refusal(child, parent);
    CHECK(child_et.pub.node.ln_Succ == (struct Node *)(uintptr_t)1u);
    CHECK(child_et.pub.node.ln_Pred == (struct Node *)(uintptr_t)2u);

    child_et.pub.node.ln_Succ = NULL;
    child_et.pub.node.ln_Pred = NULL;
    clear_events();
    interrupt_enabled = 1;
    poison_on_enable = 1;
    poison_child_task = child;
    poison_parent_task = parent;
    poison_child_page = child;
    poison_parent_page = parent;
    currentTask = parent;
    before = fatal_calls;
    Exec_PublishConstructedETask(child, parent, SysBase);
    CHECK(fatal_calls == before);
    CHECK(event_count == sizeof(expected) / sizeof(expected[0]));
    for (size_t i = 0; i < event_count; ++i)
        CHECK(events[i] == expected[i]);
    CHECK(interrupt_enabled && !task_lock_held);
    CHECK(mprotect(child, page_size, PROT_READ | PROT_WRITE) == 0);
    CHECK(mprotect(parent, page_size, PROT_READ | PROT_WRITE) == 0);
    CHECK(child_et.pub.et_Parent == parent);
    CHECK(list_contains(&parent_et.pub.et_Children,
                        (struct Node *)&child_et.pub));
    CHECK(parent_et.iet_TaskLock == 0);

    /* A current parent without an ETask is accepted but has no child list
     * or task lock to update. */
    reset_counters();
    init_helper_task(child, &child_et);
    memset(plain_parent, 0, sizeof(*plain_parent));
    currentTask = plain_parent;
    Exec_PublishConstructedETask(child, plain_parent, SysBase);
    CHECK(child_et.pub.et_Parent == plain_parent);
    CHECK(event_count == 2 && events[0] == EV_DISABLE &&
          events[1] == EV_ENABLE);

    reset_counters();
    init_helper_task(child, &child_et);
    currentTask = NULL;
    Exec_PublishConstructedETask(child, NULL, SysBase);
    CHECK(child_et.pub.et_Parent == NULL);
    CHECK(event_count == 2 && events[0] == EV_DISABLE &&
          events[1] == EV_ENABLE);

    unmap_task(parent);
    unmap_task(child);
    unmap_task(other);
    unmap_task(plain_parent);
}

static int run_staged(void)
{
    struct Task parent;
    struct IntETask parent_et;
    reset_counters();
    page_size = (size_t)sysconf(_SC_PAGESIZE);
    CHECK(page_size >= sizeof(struct Task));
    init_parent(&parent, &parent_et);
    test_failure_paths(&parent, &parent_et);
    test_staged_success(&parent, &parent_et);
    test_helper_validation_and_lock_order();
    return 1;
}
#else
static int run_normal(void)
{
    struct Task parent;
    struct IntETask parent_et;
    struct Task *task;
    APTR result;

    reset_counters();
    page_size = (size_t)sysconf(_SC_PAGESIZE);
    CHECK(page_size >= sizeof(struct Task));
    init_parent(&parent, &parent_et);
    currentTask = &parent;
    task = map_task(0x9b);
    init_new_task(task);
    task->tc_SpinLock = 0xa5a5a5a5u;
    result = NewAddTask(task, (APTR)(uintptr_t)0x30000u, NULL, NULL);
    CHECK(result == task);
    CHECK(init_calls == 1 && observed_init_parent == &parent);
    CHECK(record_create_calls == 0 && record_release_calls == 0);
    CHECK(list_contains(&parent_et.pub.et_Children,
                        (struct Node *)GetETask(task)));
    CHECK(task->tc_State == TS_READY);
    CHECK(parent_linked_notifications == 0);
    Remove((struct Node *)GetETask(task));
    GetETask(task)->et_Parent = NULL;
    CleanupETask(task);
    CHECK(munmap(task, page_size) == 0);
    return 1;
}
#endif

int main(void)
{
    int ok;
#if defined(TEST_STAGED)
    ok = run_staged();
    if (ok)
        printf("PASS staged ETask publication: %u checks\n", checks);
#else
    ok = run_normal();
    if (ok)
        printf("PASS ordinary ETask initialization: %u checks\n", checks);
#endif
    return ok ? 0 : 1;
}
'''


def compile_and_run(staged: bool) -> str:
    actual = "\n".join(
        [
            "#if defined(TEST_STAGED)",
            publish_helper,
            "#endif",
            task_launch,
            "static APTR NewAddTask(struct Task *task, APTR initialPC, "
            "APTR finalPC, struct TagItem *tagList)\n{",
            newadd_body,
            "}\n",
        ]
    )
    source = PRELUDE + "\n" + actual
    compiler = os.environ.get("CC", "clang")
    flags = [
        "-std=gnu11",
        "-O1",
        "-g",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-Wconversion",
        "-Wpedantic",
        "-fsanitize=address,undefined",
        "-fno-omit-frame-pointer",
    ]
    if staged:
        flags.append("-DTEST_STAGED=1")
    with tempfile.TemporaryDirectory(prefix="p4-etask-publication-") as temp:
        src = Path(temp) / "fixture.c"
        exe = Path(temp) / "fixture"
        src.write_text(source)
        subprocess.run([compiler, *flags, str(src), "-o", str(exe)],
                       check=True)
        run = subprocess.run([str(exe)], check=False, text=True,
                             capture_output=True)
        if run.returncode != 0:
            raise RuntimeError(run.stdout + run.stderr)
        return run.stdout.strip()


if __name__ == "__main__":
    print("production newaddtask.c sha256:",
          hashlib.sha256(NEWADD_PATH.read_bytes()).hexdigest())
    print("production exec_util.c sha256:",
          hashlib.sha256(UTIL_PATH.read_bytes()).hexdigest())
    print("staged:", compile_and_run(True))
    print("normal:", compile_and_run(False))
