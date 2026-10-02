#!/usr/bin/env python3
"""Compile TaskRes topology functions from the production sources and test them."""

from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile


REPO = Path(__file__).resolve().parents[3]
EVIDENCE = Path(
    "/Volumes/Dev/Source/Amiga/AROS-ESP32-build/evidence/"
    "smp-priority-2026-10-01"
)
SOURCES = {
    "task_support.c": REPO / "rom/task/task_support.c",
    "task_execfuncs.c": REPO / "rom/task/task_execfuncs.c",
    "task_intern.h": REPO / "rom/task/task_intern.h",
    "LockTaskList.c": REPO / "rom/task/LockTaskList.c",
    "UnLockTaskList.c": REPO / "rom/task/UnLockTaskList.c",
    "NextTaskEntry.c": REPO / "rom/task/NextTaskEntry.c",
}


def extract_function(source: str, pattern: str, label: str) -> str:
    match = re.search(pattern, source, re.MULTILINE)
    if match is None:
        raise RuntimeError(f"{label} definition not found")
    opening = source.find("{", match.start())
    if opening < 0:
        raise RuntimeError(f"{label} opening brace not found")

    depth = 0
    state = "code"
    escaped = False
    index = opening
    while index < len(source):
        char = source[index]
        following = source[index + 1] if index + 1 < len(source) else ""
        if state == "line_comment":
            if char == "\n":
                state = "code"
        elif state == "block_comment":
            if char == "*" and following == "/":
                state = "code"
                index += 1
        elif state in ("string", "char"):
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif (state == "string" and char == '"') or (
                state == "char" and char == "'"
            ):
                state = "code"
        elif char == "/" and following == "/":
            state = "line_comment"
            index += 1
        elif char == "/" and following == "*":
            state = "block_comment"
            index += 1
        elif char == '"':
            state = "string"
        elif char == "'":
            state = "char"
        elif char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return source[match.start() : index + 1]
        index += 1
    raise RuntimeError(f"{label} closing brace not found")


def load_functions() -> tuple[dict[str, str], dict[str, str]]:
    source_text = {name: path.read_text() for name, path in SOURCES.items()}
    functions = {
        "task_DetachEntry": extract_function(
            source_text["task_support.c"],
            r"(?m)^void\s+task_DetachEntry\s*\(",
            "task_DetachEntry",
        ),
        "task_CleanList": extract_function(
            source_text["task_support.c"],
            r"(?m)^void\s+task_CleanList\s*\(",
            "task_CleanList",
        ),
        "GetTaskEntry": extract_function(
            source_text["task_support.c"],
            r"(?m)^struct TaskListEntry\s*\*\s*GetTaskEntry\s*\(",
            "GetTaskEntry",
        ),
        "TaskRes_PreLaunch": extract_function(
            source_text["task_execfuncs.c"],
            r"AROS_UFH3\s*\(\s*void\s*,\s*TaskRes_PreLaunch\s*,",
            "TaskRes_PreLaunch",
        ),
        "LockTaskList": extract_function(
            source_text["LockTaskList.c"],
            r"AROS_LH1\s*\(\s*struct TaskList\s*\*\s*,\s*LockTaskList\s*,",
            "LockTaskList",
        ),
        "UnLockTaskList": extract_function(
            source_text["UnLockTaskList.c"],
            r"AROS_LH2\s*\(\s*void\s*,\s*UnLockTaskList\s*,",
            "UnLockTaskList",
        ),
        "NextTaskEntry": extract_function(
            source_text["NextTaskEntry.c"],
            r"AROS_LH2\s*\(\s*struct Task\s*\*\s*,\s*NextTaskEntry\s*,",
            "NextTaskEntry",
        ),
    }
    return source_text, functions


HARNESS_PREFIX = r"""
#define _DEFAULT_SOURCE 1
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#ifndef P4_TEST_SMP
#define P4_TEST_SMP 1
#endif
#if P4_TEST_SMP
#define __AROSEXEC_SMP__ 1
#endif

typedef uint32_t ULONG;
typedef uintptr_t IPTR;
typedef void *APTR;
typedef int BOOL;
typedef uint32_t spinlock_t;

#define TRUE 1
#define FALSE 0
#define MEMF_CLEAR UINT32_C(0x00010000)
#define MEMF_PUBLIC UINT32_C(0x00000001)
#define MEMF_ANY UINT32_C(0x00000000)
#define SPINLOCK_MODE_WRITE 1u
#define TASKRES_ENABLE 1
#define LTF_WRITE UINT32_C(0x0001)
#define LTF_RUNNING UINT32_C(0x0002)
#define LTF_READY UINT32_C(0x0004)
#define LTF_WAITING UINT32_C(0x0008)
#define LTF_ALL (LTF_RUNNING | LTF_READY | LTF_WAITING)
#define TS_RUN UINT32_C(0x0001)
#define TS_READY UINT32_C(0x0002)
#define TS_WAIT UINT32_C(0x0004)
#define TS_SPIN UINT32_C(0x0008)

#define D(...) do { } while (0);
#define AROS_LIBFUNC_INIT
#define AROS_LIBFUNC_EXIT
#define AROS_USERFUNC_INIT
#define AROS_USERFUNC_EXIT
#define AROS_LHA(type, name, reg) type name
#define AROS_UFHA(type, name, reg) type name
#define AROS_LH1(ret, name, a1, base_type, base, lvo, library) \
    ret name(a1, base_type base __attribute__((unused)))
#define AROS_LH2(ret, name, a1, a2, base_type, base, lvo, library) \
    ret name(a1, a2, base_type base __attribute__((unused)))
#define AROS_UFH3(ret, name, a1, a2, a3) ret name(a1, a2, a3)
#define Disable() mock_disable()
#define Enable() mock_enable()

struct Task;
struct TaskList;
struct TaskResBase;

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
};

struct SignalSemaphore
{
    struct Task *owner;
    unsigned int nest;
};

struct Hook
{
    void *h_Entry;
    void *h_Data;
};

struct Task
{
    struct Node tc_Node;
    ULONG tc_State;
};

struct TaskListEntry
{
    struct Node tle_Node;
    struct Task *tle_Task;
    struct List tle_HookTypes;
};

struct TaskListPrivate
{
    struct Node tlp_Node;
    ULONG tlp_Flags;
    struct List *tlp_Tasks;
    struct TaskListEntry *tlp_Next;
};

struct TaskResBase
{
    struct SignalSemaphore trb_Sem;
    spinlock_t TaskListSpinLock;
    struct List trb_TaskList;
    struct List trb_NewTasks;
    struct List trb_LockedLists;
};

void task_DetachEntry(struct Task *, struct TaskResBase *);
void task_CleanList(struct Task *, struct TaskResBase *);
void TaskRes_PreLaunch(struct Hook *, struct Task *, IPTR);
struct TaskList *LockTaskList(ULONG, struct TaskResBase *);
void UnLockTaskList(struct TaskList *, ULONG, struct TaskResBase *);
struct Task *NextTaskEntry(struct TaskList *, ULONG, struct TaskResBase *);
struct TaskListEntry *GetTaskEntry(struct Task *, struct TaskResBase *);

enum event
{
    EV_ALLOC = 1,
    EV_FREE,
    EV_SEM_OBTAIN,
    EV_SEM_RELEASE,
    EV_GATE_LOCK,
    EV_GATE_UNLOCK,
    EV_FORBID,
    EV_PERMIT,
    EV_DISABLE,
    EV_ENABLE
};

static unsigned int checks;
static int failed;
static struct TaskResBase *active_base;
static struct Task *current_task;
static unsigned int gate_depth;
static unsigned int forbid_depth;
static unsigned int sem_depth;
static unsigned int irq_depth;
static unsigned int allocations;
static unsigned int frees;
static unsigned int live_allocations;
static unsigned int fail_next_allocation;
static unsigned int setup_mode;
static unsigned int event_count;
static unsigned int events[512];

#define CHECK(condition) do { \
    ++checks; \
    if (!(condition)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", \
                __FILE__, __LINE__, #condition); \
        failed = 1; \
    } \
} while (0)

static void event_add(unsigned int event)
{
    if (event_count < sizeof(events) / sizeof(events[0]))
        events[event_count++] = event;
    else
        CHECK(0 && "event log overflow");
}

static void events_reset(void)
{
    event_count = 0;
}

static struct List *topology_lists[3];

static int is_topology_list(const struct List *list)
{
    return active_base &&
        (list == &active_base->trb_TaskList ||
         list == &active_base->trb_NewTasks ||
         list == &active_base->trb_LockedLists);
}

static int raw_contains_node(const struct List *list, const struct Node *wanted)
{
    const struct Node *node = list->lh_Head;
    unsigned int bound = 0;
    while (node && node->ln_Succ && bound++ < 128u)
    {
        if (node == wanted)
            return 1;
        node = node->ln_Succ;
    }
    return 0;
}

static void require_topology_gate(const struct List *list)
{
    if (!is_topology_list(list) || setup_mode)
        return;
#if P4_TEST_SMP
    CHECK(gate_depth == 1u);
    CHECK(forbid_depth == 0u);
    CHECK(irq_depth > 0u);
#else
    CHECK(forbid_depth == 1u);
    CHECK(gate_depth == 0u);
#endif
}

static void require_node_gate(const struct Node *node)
{
    if (!active_base || setup_mode)
        return;
    for (unsigned int index = 0; index < 3u; ++index)
        if (raw_contains_node(topology_lists[index], node))
        {
            require_topology_gate(topology_lists[index]);
            return;
        }
}

static void init_list(struct List *list)
{
    if (is_topology_list(list) && !setup_mode)
        require_topology_gate(list);
    list->lh_Head = (struct Node *)&list->lh_Tail;
    list->lh_Tail = NULL;
    list->lh_TailPred = (struct Node *)list;
}

static void add_tail(struct List *list, struct Node *node)
{
    if (is_topology_list(list))
        require_topology_gate(list);
    node->ln_Succ = (struct Node *)&list->lh_Tail;
    node->ln_Pred = list->lh_TailPred;
    list->lh_TailPred->ln_Succ = node;
    list->lh_TailPred = node;
}

static struct Node *Remove(struct Node *node)
{
    require_node_gate(node);
    CHECK(node != NULL);
    CHECK(node->ln_Succ != NULL);
    CHECK(node->ln_Pred != NULL);
    node->ln_Succ->ln_Pred = node->ln_Pred;
    node->ln_Pred->ln_Succ = node->ln_Succ;
    return node;
}

static int raw_list_empty(const struct List *list)
{
    return list->lh_Head == (const struct Node *)&list->lh_Tail &&
           list->lh_Tail == NULL &&
           list->lh_TailPred == (const struct Node *)list;
}

static int mock_list_empty(const struct List *list)
{
    require_topology_gate(list);
    return raw_list_empty(list);
}

static struct Node *mock_get_head(struct List *list)
{
    require_topology_gate(list);
    return list->lh_Head == (struct Node *)&list->lh_Tail ? NULL : list->lh_Head;
}

static void foreach_begin(struct List *list, void **current, void **next)
{
    require_topology_gate(list);
    *current = list->lh_Head;
    *next = *current ? ((struct Node *)*current)->ln_Succ : NULL;
}

static int foreach_more(struct List *list, void *current)
{
    require_topology_gate(list);
    return current && ((struct Node *)current)->ln_Succ != NULL;
}

static void foreach_advance(struct List *list, void **current, void **next)
{
    require_topology_gate(list);
    *current = *next;
    *next = *current ? ((struct Node *)*current)->ln_Succ : NULL;
}

static struct Node *mock_get_succ(struct Node *node)
{
    struct Node *successor;
    require_node_gate(node);
    if (!node || !node->ln_Succ)
        return NULL;
    successor = node->ln_Succ;
    return successor->ln_Succ ? successor : NULL;
}

#define NEWLIST(list) init_list((struct List *)(list))
#define AddTail(list, node) add_tail((struct List *)(list), (struct Node *)(node))
#define Remove(node) Remove((struct Node *)(node))
#define IsListEmpty(list) mock_list_empty((struct List *)(list))
#define GetHead(list) mock_get_head((struct List *)(list))
#define GetSucc(node) mock_get_succ((struct Node *)(node))
#define ForeachNodeSafe(list, current, next) \
    for (foreach_begin((struct List *)(list), (void **)&(current), (void **)&(next)); \
         foreach_more((struct List *)(list), (void *)(current)); \
         foreach_advance((struct List *)(list), (void **)&(current), (void **)&(next)))
#define ForeachNode(list, current) \
    for ((current) = (void *)mock_get_head((struct List *)(list)); \
         (current) != NULL; \
         (current) = (void *)mock_get_succ((struct Node *)(current)))

static void __attribute__((unused)) mock_gate_lock(
    spinlock_t *lock, void *hook, unsigned int mode)
{
    CHECK(active_base != NULL);
    CHECK(lock == &active_base->TaskListSpinLock);
    CHECK(hook == NULL);
    CHECK(mode == SPINLOCK_MODE_WRITE);
    CHECK(gate_depth == 0u);
#if P4_TEST_SMP
    CHECK(forbid_depth == 0u);
    CHECK(irq_depth > 0u);
#endif
    ++gate_depth;
    event_add(EV_GATE_LOCK);
}

static void __attribute__((unused)) mock_gate_unlock(spinlock_t *lock)
{
    CHECK(active_base != NULL);
    CHECK(lock == &active_base->TaskListSpinLock);
    CHECK(gate_depth == 1u);
    --gate_depth;
    event_add(EV_GATE_UNLOCK);
}

static void __attribute__((unused)) mock_disable(void)
{
    CHECK(gate_depth == 0u);
    ++irq_depth;
    event_add(EV_DISABLE);
}

static void __attribute__((unused)) mock_enable(void)
{
    CHECK(gate_depth == 0u);
    CHECK(irq_depth > 0u);
    if (irq_depth > 0u)
        --irq_depth;
    event_add(EV_ENABLE);
}

#define EXEC_SPINLOCK_LOCK(lock, hook, mode) \
    mock_gate_lock((spinlock_t *)(lock), (void *)(hook), (mode))
#define EXEC_SPINLOCK_UNLOCK(lock) mock_gate_unlock((spinlock_t *)(lock))

static void __attribute__((unused)) Forbid(void)
{
    CHECK(forbid_depth == 0u);
    CHECK(gate_depth == 0u);
    ++forbid_depth;
    event_add(EV_FORBID);
}

static void __attribute__((unused)) Permit(void)
{
    CHECK(forbid_depth == 1u);
    CHECK(gate_depth == 0u);
    --forbid_depth;
    event_add(EV_PERMIT);
}

static struct Task *FindTask(const char *name)
{
    CHECK(name == NULL);
    return current_task;
}

static void *AllocMem(size_t size, ULONG flags)
{
    CHECK(gate_depth == 0u);
    CHECK(forbid_depth == 0u);
    CHECK((flags & MEMF_CLEAR) != 0u);
    event_add(EV_ALLOC);
    ++allocations;
    if (fail_next_allocation)
    {
        --fail_next_allocation;
        return NULL;
    }
    void *memory = calloc(1u, size);
    if (memory)
        ++live_allocations;
    return memory;
}

static void FreeMem(void *memory, size_t size)
{
    CHECK(memory != NULL);
    CHECK(size != 0u);
    CHECK(gate_depth == 0u);
    CHECK(forbid_depth == 0u);
    event_add(EV_FREE);
    ++frees;
    CHECK(live_allocations > 0u);
    if (live_allocations > 0u)
        --live_allocations;
    free(memory);
}

static void ObtainSemaphore(struct SignalSemaphore *semaphore)
{
    CHECK(gate_depth == 0u);
    CHECK(forbid_depth == 0u);
    CHECK(semaphore != NULL);
    CHECK(current_task != NULL);
    CHECK(semaphore->owner == NULL || semaphore->owner == current_task);
    if (semaphore->nest == 0u)
        semaphore->owner = current_task;
    ++semaphore->nest;
    ++sem_depth;
    event_add(EV_SEM_OBTAIN);
}

static void ObtainSemaphoreShared(struct SignalSemaphore *semaphore)
{
    ObtainSemaphore(semaphore);
}

static void ReleaseSemaphore(struct SignalSemaphore *semaphore)
{
    CHECK(gate_depth == 0u);
    CHECK(forbid_depth == 0u);
    CHECK(semaphore != NULL);
    CHECK(semaphore->owner == current_task);
    CHECK(semaphore->nest > 0u);
    CHECK(sem_depth > 0u);
    if (semaphore->nest > 0u)
        --semaphore->nest;
    if (sem_depth > 0u)
        --sem_depth;
    if (semaphore->nest == 0u)
        semaphore->owner = NULL;
    event_add(EV_SEM_RELEASE);
}

static unsigned int raw_count(const struct List *list)
{
    const struct Node *node = list->lh_Head;
    unsigned int count = 0;
    while (node && node->ln_Succ && count < 128u)
    {
        ++count;
        node = node->ln_Succ;
    }
    return node && count == 128u ? UINT32_MAX : count;
}

static struct TaskListEntry *find_task_entry(const struct List *list,
                                             const struct Task *task)
{
    const struct Node *node = list->lh_Head;
    unsigned int count = 0;
    while (node && node->ln_Succ && count++ < 128u)
    {
        struct TaskListEntry *entry =
            (struct TaskListEntry *)((char *)node - offsetof(struct TaskListEntry, tle_Node));
        if (entry->tle_Task == task)
            return entry;
        node = node->ln_Succ;
    }
    return NULL;
}

static unsigned int event_index(unsigned int wanted)
{
    for (unsigned int index = 0; index < event_count; ++index)
        if (events[index] == wanted)
            return index;
    return UINT32_MAX;
}

static unsigned int event_index_after(unsigned int wanted, unsigned int after)
{
    for (unsigned int index = after + 1u; index < event_count; ++index)
        if (events[index] == wanted)
            return index;
    return UINT32_MAX;
}

static unsigned int topology_enter_event(void)
{
#if P4_TEST_SMP
    return EV_GATE_LOCK;
#else
    return EV_FORBID;
#endif
}

static unsigned int topology_exit_event(void)
{
#if P4_TEST_SMP
    return EV_GATE_UNLOCK;
#else
    return EV_PERMIT;
#endif
}

static void fixture_init(struct TaskResBase *base)
{
    memset(base, 0, sizeof(*base));
    active_base = base;
    topology_lists[0] = &base->trb_TaskList;
    topology_lists[1] = &base->trb_NewTasks;
    topology_lists[2] = &base->trb_LockedLists;
    setup_mode = 1u;
    init_list(&base->trb_TaskList);
    init_list(&base->trb_NewTasks);
    init_list(&base->trb_LockedLists);
    setup_mode = 0u;
    gate_depth = 0u;
    forbid_depth = 0u;
    sem_depth = 0u;
    irq_depth = 0u;
    current_task = NULL;
    fail_next_allocation = 0u;
    events_reset();
}

static void prelaunch(struct TaskResBase *base, struct Task *task)
{
    struct Hook hook = { .h_Data = base };
    TaskRes_PreLaunch(&hook, task, 0u);
}

static void test_publication_detach_opaque(void *opaque_page)
{
    struct TaskResBase base;
    struct Task *identity_a = (struct Task *)opaque_page;
    struct Task *identity_b = (struct Task *)((char *)opaque_page + 64);
    unsigned int before_free;
    fixture_init(&base);
    events_reset();
    prelaunch(&base, identity_a);
    CHECK(raw_count(&base.trb_TaskList) == 1u);
    CHECK(raw_count(&base.trb_NewTasks) == 0u);
    CHECK(find_task_entry(&base.trb_TaskList, identity_a) != NULL);
    CHECK(event_index(EV_ALLOC) < event_index(topology_enter_event()));
#if P4_TEST_SMP
    CHECK(event_index(EV_DISABLE) < event_index(EV_GATE_LOCK));
    CHECK(event_index(EV_GATE_UNLOCK) < event_index(EV_ENABLE));
#endif
    CHECK(gate_depth == 0u && forbid_depth == 0u);
    CHECK(irq_depth == 0u);

    before_free = frees;
    events_reset();
    task_DetachEntry(identity_a, &base);
    CHECK(raw_count(&base.trb_TaskList) == 0u);
    CHECK(frees == before_free + 1u);
    CHECK(event_index(topology_exit_event()) < event_index(EV_FREE));
#if P4_TEST_SMP
    CHECK(event_index(EV_GATE_UNLOCK) < event_index(EV_ENABLE));
    CHECK(event_index(EV_ENABLE) < event_index(EV_FREE));
#endif
    CHECK(irq_depth == 0u);
    before_free = frees;
    task_DetachEntry(identity_b, &base);
    CHECK(raw_count(&base.trb_TaskList) == 0u);
    CHECK(frees == before_free);
    task_DetachEntry(NULL, &base);
    CHECK(gate_depth == 0u && forbid_depth == 0u);
    events_reset();
    task_DetachEntry(NULL, &base);
    CHECK(event_count == 0u);
}

static void test_newtasks_and_last_marker(void *opaque_page)
{
    struct TaskResBase base;
    struct Task task_owner = {0};
    struct Task *identity_a = (struct Task *)opaque_page;
    struct Task *identity_b = (struct Task *)((char *)opaque_page + 64);
    struct TaskList *handle;
    unsigned int before_free;
    fixture_init(&base);
    current_task = &task_owner;
    handle = LockTaskList(LTF_ALL, &base);
    CHECK(handle != NULL);
    CHECK(sem_depth == 1u && base.trb_Sem.nest == 1u);
    CHECK(raw_count(&base.trb_LockedLists) == 1u);
    prelaunch(&base, identity_a);
    CHECK(raw_count(&base.trb_NewTasks) == 1u);
    CHECK(raw_count(&base.trb_TaskList) == 0u);

    before_free = frees;
    task_DetachEntry(identity_a, &base);
    CHECK(raw_count(&base.trb_NewTasks) == 0u);
    CHECK(frees == before_free + 1u);

    prelaunch(&base, identity_b);
    CHECK(raw_count(&base.trb_NewTasks) == 1u);
    events_reset();
    UnLockTaskList(handle, LTF_ALL, &base);
    CHECK(raw_count(&base.trb_LockedLists) == 0u);
    CHECK(raw_count(&base.trb_NewTasks) == 0u);
    CHECK(raw_count(&base.trb_TaskList) == 1u);
    CHECK(find_task_entry(&base.trb_TaskList, identity_b) != NULL);
    CHECK(sem_depth == 0u && base.trb_Sem.nest == 0u);
    CHECK(event_index(topology_exit_event()) < event_index(EV_FREE));
    CHECK(event_index(EV_FREE) < event_index(EV_SEM_RELEASE));
    CHECK(event_index(EV_SEM_RELEASE) < event_index_after(
          topology_enter_event(),
          event_index(topology_enter_event())));
    CHECK(irq_depth == 0u);
    task_DetachEntry(identity_b, &base);
    CHECK(raw_count(&base.trb_TaskList) == 0u);
}

static void test_active_iterator_null_marker_and_traversal(void *opaque_page)
{
    struct TaskResBase base;
    struct Task owner_a = {0};
    struct Task entry_task_a = { .tc_State = TS_WAIT };
    struct Task entry_task_b = { .tc_State = TS_RUN };
    (void)opaque_page;
    struct TaskList *handle_a;
    struct TaskList *handle_b;
    struct TaskListEntry *entry_a;
    unsigned int before_free;
    fixture_init(&base);
    prelaunch(&base, &entry_task_a);
    prelaunch(&base, &entry_task_b);
    entry_a = find_task_entry(&base.trb_TaskList, &entry_task_a);
    CHECK(entry_a != NULL);
    current_task = &owner_a;
    handle_a = LockTaskList(LTF_ALL, &base);
    current_task = &owner_a;
    handle_b = LockTaskList(LTF_ALL, &base);
    CHECK(handle_a != NULL && handle_b != NULL && handle_a != handle_b);
    CHECK(raw_count(&base.trb_LockedLists) == 2u);

    before_free = frees;
    task_DetachEntry(&entry_task_a, &base);
    CHECK(entry_a->tle_Task == NULL);
    CHECK(frees == before_free);
    events_reset();
    CHECK(NextTaskEntry(NULL, LTF_ALL, &base) == NULL);
    CHECK(event_count == 0u);
    CHECK(gate_depth == 0u && forbid_depth == 0u);
    CHECK(irq_depth == 0u);
    events_reset();
    irq_depth = 2u;
    current_task = &owner_a;
    CHECK(NextTaskEntry(handle_a, LTF_RUNNING, &base) == &entry_task_b);
    CHECK(NextTaskEntry(handle_a, LTF_RUNNING, &base) == NULL);
    CHECK(irq_depth == 2u);
    current_task = &owner_a;
    CHECK(NextTaskEntry(handle_b, LTF_READY, &base) == &entry_task_b);
    CHECK(NextTaskEntry(handle_b, LTF_READY, &base) == NULL);
    CHECK(irq_depth == 2u);
#if P4_TEST_SMP
    CHECK(event_index(EV_DISABLE) < event_index(EV_GATE_LOCK));
    CHECK(event_index(EV_GATE_UNLOCK) < event_index(EV_ENABLE));
#endif
    irq_depth = 0u;
    CHECK(gate_depth == 0u && forbid_depth == 0u);

    current_task = &owner_a;
    UnLockTaskList(handle_a, LTF_ALL, &base);
    CHECK(raw_count(&base.trb_TaskList) == 2u);
    CHECK(entry_a->tle_Task == NULL);
    current_task = &owner_a;
    UnLockTaskList(handle_b, LTF_ALL, &base);
    CHECK(raw_count(&base.trb_TaskList) == 1u);
    CHECK(raw_count(&base.trb_LockedLists) == 0u);
    CHECK(frees == before_free + 3u); /* detached entry and two handles */
    task_DetachEntry(&entry_task_b, &base);
    CHECK(raw_count(&base.trb_TaskList) == 0u);
}

static void test_get_task_entry_identity_only(void *opaque_page)
{
    struct TaskResBase base;
    struct Task *identity = (struct Task *)opaque_page;
    struct Task *missing = (struct Task *)((char *)opaque_page + 64);
    struct TaskListEntry *entry;
    fixture_init(&base);
    prelaunch(&base, identity);
    events_reset();
    entry = GetTaskEntry(identity, &base);
    CHECK(entry != NULL && entry->tle_Task == identity);
    CHECK(event_index(topology_enter_event()) != UINT32_MAX);
    CHECK(irq_depth == 0u);
    events_reset();
    CHECK(GetTaskEntry(missing, &base) == NULL);
    CHECK(irq_depth == 0u);
    task_DetachEntry(identity, &base);
    CHECK(raw_count(&base.trb_TaskList) == 0u);
}

static void test_invalid_unlock_and_nested_semaphore(void)
{
    struct TaskResBase base;
    struct Task owner = {0};
    struct Task foreign = {0};
    struct TaskList *handle_a;
    struct TaskList *handle_b;
    unsigned int before_free;
    fixture_init(&base);
    current_task = &owner;
    handle_a = LockTaskList(LTF_ALL, &base);
    handle_b = LockTaskList(LTF_RUNNING, &base);
    CHECK(handle_a != NULL && handle_b != NULL);
    CHECK(sem_depth == 2u && base.trb_Sem.nest == 2u);
    before_free = frees;

    current_task = &foreign;
    UnLockTaskList(handle_a, LTF_ALL, &base);
    CHECK(raw_count(&base.trb_LockedLists) == 2u);
    CHECK(sem_depth == 2u && frees == before_free);
    current_task = &owner;
    UnLockTaskList(handle_a, LTF_RUNNING, &base);
    CHECK(raw_count(&base.trb_LockedLists) == 2u);
    CHECK(sem_depth == 2u && frees == before_free);
    UnLockTaskList((struct TaskList *)((char *)handle_a + 1), LTF_ALL, &base);
    CHECK(raw_count(&base.trb_LockedLists) == 2u);
    CHECK(sem_depth == 2u && frees == before_free);

    UnLockTaskList(handle_b, LTF_RUNNING, &base);
    CHECK(raw_count(&base.trb_LockedLists) == 1u);
    CHECK(sem_depth == 1u && base.trb_Sem.nest == 1u);
    UnLockTaskList(handle_a, LTF_ALL, &base);
    CHECK(raw_count(&base.trb_LockedLists) == 0u);
    CHECK(sem_depth == 0u && base.trb_Sem.nest == 0u);
    CHECK(frees == before_free + 2u);
}

static void test_lock_allocation_failure_balances_semaphore(void)
{
    struct TaskResBase base;
    struct Task owner = {0};
    struct TaskList *handle;
    fixture_init(&base);
    current_task = &owner;
    fail_next_allocation = 1u;
    events_reset();
    handle = LockTaskList(LTF_WRITE, &base);
    CHECK(handle == NULL);
    CHECK(sem_depth == 0u && base.trb_Sem.nest == 0u);
    CHECK(base.trb_Sem.owner == NULL);
    CHECK(raw_count(&base.trb_LockedLists) == 0u);
    CHECK(event_index(EV_SEM_OBTAIN) < event_index(EV_ALLOC));
    CHECK(event_index(EV_ALLOC) < event_index(EV_SEM_RELEASE));
    CHECK(event_index(topology_enter_event()) == UINT32_MAX);
    CHECK(irq_depth == 0u);
}

int main(void)
{
    long page_size = sysconf(_SC_PAGESIZE);
    void *opaque_page;
    if (page_size <= 0)
        return 2;
    opaque_page = mmap(NULL, (size_t)page_size, PROT_NONE,
                       MAP_PRIVATE | MAP_ANON, -1, 0);
    if (opaque_page == MAP_FAILED)
    {
        perror("mmap(PROT_NONE)");
        return 2;
    }

    test_publication_detach_opaque(opaque_page);
    test_newtasks_and_last_marker(opaque_page);
    test_active_iterator_null_marker_and_traversal(opaque_page);
    test_get_task_entry_identity_only(opaque_page);
    test_invalid_unlock_and_nested_semaphore();
    test_lock_allocation_failure_balances_semaphore();
    CHECK(gate_depth == 0u && forbid_depth == 0u && sem_depth == 0u && irq_depth == 0u);
    CHECK(live_allocations == 0u);
    CHECK(munmap(opaque_page, (size_t)page_size) == 0);
    printf("TaskRes topology (SMP=%u): %u checks %s\n",
           (unsigned int)P4_TEST_SMP, checks, failed ? "FAILED" : "passed");
    return failed ? 1 : 0;
}
"""


def source_assertions(source_text: dict[str, str], functions: dict[str, str]) -> None:
    remtask = extract_function(
        source_text["task_execfuncs.c"],
        r"AROS_LH1\s*\(\s*void\s*,\s*RemTask\s*,",
        "RemTask",
    )
    detach_call = remtask.find("task_DetachEntry(findTask, TaskResBase);")
    etask_read = remtask.find("findTask->tc_UnionETask.tc_ETask")
    if detach_call < 0 or etask_read < 0 or detach_call >= etask_read:
        raise RuntimeError("RemTask no longer detaches TaskRes identity before ETask access")
    if "task->" in functions["task_DetachEntry"]:
        raise RuntimeError("task_DetachEntry unexpectedly dereferences the Task identity")
    for name, body in functions.items():
        if "EXEC_SPINLOCK_LOCK" not in body:
            continue
        disable = body.find("Disable();")
        acquire = body.find("EXEC_SPINLOCK_LOCK")
        release = body.find("EXEC_SPINLOCK_UNLOCK")
        enable = body.find("Enable();", release)
        if min(disable, acquire, release, enable) < 0 or not (
            disable < acquire < release < enable
        ):
            raise RuntimeError(f"{name} SMP gate no longer masks through lock release")

    init_source = (REPO / "rom/task/task_init.c").read_text()
    init_func = extract_function(
        init_source,
        r"(?m)^static LONG taskres_Init\s*\(",
        "taskres_Init",
    )
    lock_init = init_func.find("EXEC_SPINLOCK_INIT(&TaskResBase->TaskListSpinLock);")
    vector_install = init_func.find("SetFunction((struct Library *)SysBase")
    list_init = init_func.find("NEWLIST(&TaskResBase->trb_LockedLists);")
    if min(lock_init, vector_install, list_init) < 0 or not (list_init < lock_init < vector_install):
        raise RuntimeError("TaskRes topology lists/lock are not initialized before vector publication")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--artifact-dir",
        type=Path,
        default=EVIDENCE,
        help="directory for retained test executables",
    )
    artifact_dir = parser.parse_args().artifact_dir
    source_text, functions = load_functions()
    source_assertions(source_text, functions)
    hashes = {
        name: hashlib.sha256(path.read_bytes()).hexdigest()
        for name, path in SOURCES.items()
    }
    init_path = REPO / "rom/task/task_init.c"
    hashes["task_init.c"] = hashlib.sha256(init_path.read_bytes()).hexdigest()
    generated = HARNESS_PREFIX + "\n".join(functions.values()) + "\n"
    artifact_dir.mkdir(parents=True, exist_ok=True)
    artifact_records: list[tuple[str, int, str, str]] = []
    for optimization in ("O1", "O2"):
        for smp in (1, 0):
            name = f"p4-taskres-topology-{('smp' if smp else 'normal')}-{optimization}"
            source_file = None
            with tempfile.NamedTemporaryFile(
                mode="w", suffix=".c", prefix="p4-taskres-topology-", delete=False
            ) as temporary:
                temporary.write(generated)
                source_file = Path(temporary.name)
            binary = artifact_dir / name
            command = [
                "clang",
                "-std=c11",
                "-D_DEFAULT_SOURCE",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-Wconversion",
                "-Wpedantic",
                f"-{optimization}",
                "-g",
                "-fsanitize=address,undefined",
                "-fno-omit-frame-pointer",
                f"-DP4_TEST_SMP={smp}",
                str(source_file),
                "-o",
                str(binary),
            ]
            try:
                compile_result = subprocess.run(
                    command,
                    cwd=REPO,
                    text=True,
                    capture_output=True,
                    timeout=30,
                    check=False,
                )
                if compile_result.returncode != 0:
                    print(shlex.join(command))
                    print(compile_result.stdout, end="")
                    print(compile_result.stderr, end="")
                    return compile_result.returncode
                environment = os.environ.copy()
                environment["ASAN_OPTIONS"] = "detect_leaks=0:halt_on_error=1"
                environment["UBSAN_OPTIONS"] = "halt_on_error=1"
                run = subprocess.run(
                    [str(binary)],
                    cwd=REPO,
                    text=True,
                    capture_output=True,
                    timeout=20,
                    check=False,
                    env=environment,
                )
                if run.stdout:
                    print(run.stdout, end="")
                if run.stderr:
                    print(run.stderr, end="")
                if run.returncode != 0:
                    return run.returncode
                binary_bytes = binary.read_bytes()
                artifact_records.append(
                    (name, len(binary_bytes), hashlib.sha256(binary_bytes).hexdigest(), shlex.join(command))
                )
            finally:
                source_file.unlink(missing_ok=True)

    print("Production source SHA-256:")
    for name, digest in sorted(hashes.items()):
        print(f"  {name}: {digest}")
    print("Retained test binaries:")
    for name, size, digest, command in artifact_records:
        print(f"  {name}: {size} bytes SHA-256 {digest}")
        print(f"    compile: {command}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
