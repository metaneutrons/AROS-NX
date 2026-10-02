#!/usr/bin/env python3
"""Compile the actual selected TaskResource registration body with host gates."""

from __future__ import annotations

import argparse
import hashlib
from pathlib import Path
import re
import subprocess


ROOT = Path(__file__).resolve().parents[3]

HARNESS = r'''#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef uint32_t ULONG;
typedef int BOOL;
typedef void *APTR;
#define TRUE 1
#define FALSE 0
#define MEMF_PUBLIC UINT32_C(0x00000001)
#define MEMF_CLEAR UINT32_C(0x00010000)
#define MEMF_NO_EXPUNGE UINT32_C(0x00020000)
#define SPINLOCK_MODE_WRITE 1
#define TASKRESOURCE_CLEANUP_CONTRACT UINT32_C(@CONTRACT@)
_Static_assert(TASKRESOURCE_CLEANUP_CONTRACT == UINT32_C(0x50345233),
    "unexpected private TaskResource contract");

struct Task { unsigned marker; };
struct Node { struct Node *ln_Succ; struct Node *ln_Pred; };
struct List { struct Node *lh_Head; struct Node *lh_Tail; struct Node *lh_TailPred; };
struct TaskListEntry {
    struct Node tle_Node;
    struct Task *tle_Task;
    struct List tle_HookTypes;
};
struct TaskResBase {
    struct List trb_TaskList;
    struct List trb_NewTasks;
    struct List trb_LockedLists;
    unsigned TaskListSpinLock;
};

static unsigned checks, irq_depth, gate_depth, fail_alloc;
static unsigned alloc_calls, last_alloc_flags, event_count;
static unsigned events[32];
static struct TaskListEntry allocated_entry;
static struct Task *expected_task;
enum { EV_ALLOC = 1, EV_DISABLE, EV_LOCK, EV_EMPTY, EV_ADD, EV_UNLOCK, EV_ENABLE };

#define CHECK(expression) do { \
    if (!(expression)) { \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #expression); \
        exit(EXIT_FAILURE); \
    } \
    ++checks; \
} while (0)

static void event(unsigned value)
{
    CHECK(event_count < sizeof(events) / sizeof(events[0]));
    events[event_count++] = value;
}

static void *AllocMem(size_t size, ULONG flags)
{
    CHECK(irq_depth == 0 && gate_depth == 0);
    CHECK(size == sizeof(allocated_entry));
    ++alloc_calls;
    last_alloc_flags = flags;
    event(EV_ALLOC);
    if (fail_alloc)
        return NULL;
    memset(&allocated_entry, 0, sizeof(allocated_entry));
    return &allocated_entry;
}

static void Disable(void)
{
    ++irq_depth;
    event(EV_DISABLE);
}

static void Enable(void)
{
    CHECK(irq_depth == 1 && gate_depth == 0);
    --irq_depth;
    event(EV_ENABLE);
}

static void mock_lock(unsigned *lock, void *state, unsigned mode)
{
    CHECK(lock != NULL && state == NULL && mode == SPINLOCK_MODE_WRITE);
    CHECK(irq_depth == 1 && gate_depth == 0);
    CHECK(allocated_entry.tle_Task == expected_task);
    CHECK(allocated_entry.tle_HookTypes.lh_Head ==
        (struct Node *)&allocated_entry.tle_HookTypes.lh_Tail);
    CHECK(allocated_entry.tle_HookTypes.lh_Tail == NULL);
    ++gate_depth;
    event(EV_LOCK);
}

static void mock_unlock(unsigned *lock)
{
    CHECK(lock != NULL && irq_depth == 1 && gate_depth == 1);
    --gate_depth;
    event(EV_UNLOCK);
}

#define EXEC_SPINLOCK_LOCK(lock, state, mode) mock_lock((lock), (state), (mode))
#define EXEC_SPINLOCK_UNLOCK(lock) mock_unlock((lock))

static void list_init(struct List *list)
{
    list->lh_Head = (struct Node *)&list->lh_Tail;
    list->lh_Tail = NULL;
    list->lh_TailPred = (struct Node *)&list->lh_Head;
}

#define NEWLIST(list) list_init((list))

static BOOL IsListEmpty(const struct List *list)
{
    CHECK(irq_depth == 1 && gate_depth == 1);
    event(EV_EMPTY);
    return list->lh_Head == (const struct Node *)&list->lh_Tail;
}

static void AddTail(struct List *list, struct Node *node)
{
    CHECK(irq_depth == 1 && gate_depth == 1);
    CHECK(list != NULL && node != NULL);
    node->ln_Succ = (struct Node *)&list->lh_Tail;
    node->ln_Pred = list->lh_TailPred;
    list->lh_TailPred->ln_Succ = node;
    list->lh_TailPred = node;
    event(EV_ADD);
}

static unsigned list_count(const struct List *list)
{
    const struct Node *node = list->lh_Head;
    const struct Node *tail = (const struct Node *)&list->lh_Tail;
    unsigned count = 0;
    while (node != tail) {
        CHECK(node != NULL && count < 8);
        ++count;
        node = node->ln_Succ;
    }
    return count;
}

@BODY@

static void reset(struct TaskResBase *base, struct Task *task)
{
    memset(base, 0, sizeof(*base));
    list_init(&base->trb_TaskList);
    list_init(&base->trb_NewTasks);
    list_init(&base->trb_LockedLists);
    task->marker = UINT32_C(0x71a5);
    expected_task = task;
    irq_depth = gate_depth = fail_alloc = alloc_calls = last_alloc_flags = event_count = 0;
    memset(events, 0, sizeof(events));
    memset(&allocated_entry, 0xa5, sizeof(allocated_entry));
}

static void test_allocation_refusal_is_quiet(void)
{
    struct TaskResBase base;
    struct Task task;
    struct TaskListEntry before_entry;
    struct List before_task_list, before_new_tasks, before_locked_lists;

    reset(&base, &task);
    before_task_list = base.trb_TaskList;
    before_new_tasks = base.trb_NewTasks;
    before_locked_lists = base.trb_LockedLists;
    before_entry = allocated_entry;
    fail_alloc = 1;
    CHECK(task_RegisterNewTask(&task, &base) == FALSE);
    CHECK(alloc_calls == 1 && last_alloc_flags ==
        (MEMF_PUBLIC | MEMF_CLEAR | MEMF_NO_EXPUNGE));
    CHECK(event_count == 1 && events[0] == EV_ALLOC);
    CHECK(irq_depth == 0 && gate_depth == 0);
    CHECK(memcmp(&base.trb_TaskList, &before_task_list, sizeof(before_task_list)) == 0);
    CHECK(memcmp(&base.trb_NewTasks, &before_new_tasks, sizeof(before_new_tasks)) == 0);
    CHECK(memcmp(&base.trb_LockedLists, &before_locked_lists, sizeof(before_locked_lists)) == 0);
    CHECK(memcmp(&allocated_entry, &before_entry, sizeof(before_entry)) == 0);
}

static void test_registers_on_main_list(void)
{
    struct TaskResBase base;
    struct Task task;

    reset(&base, &task);
    CHECK(task_RegisterNewTask(&task, &base) == TRUE);
    CHECK(irq_depth == 0 && gate_depth == 0 && alloc_calls == 1);
    CHECK(last_alloc_flags == (MEMF_PUBLIC | MEMF_CLEAR | MEMF_NO_EXPUNGE));
    CHECK(list_count(&base.trb_TaskList) == 1 && list_count(&base.trb_NewTasks) == 0);
    CHECK((struct TaskListEntry *)base.trb_TaskList.lh_Head == &allocated_entry);
    CHECK(allocated_entry.tle_Task == &task);
    CHECK(allocated_entry.tle_HookTypes.lh_Head ==
        (struct Node *)&allocated_entry.tle_HookTypes.lh_Tail);
    CHECK(event_count == 7 && events[0] == EV_ALLOC && events[1] == EV_DISABLE &&
        events[2] == EV_LOCK && events[3] == EV_EMPTY && events[4] == EV_ADD &&
        events[5] == EV_UNLOCK && events[6] == EV_ENABLE);
}

static void test_registers_on_new_tasks_while_locked_lists_exist(void)
{
    struct TaskResBase base;
    struct Task task;
    struct Node locked_marker = { 0 };

    reset(&base, &task);
    locked_marker.ln_Succ = (struct Node *)&base.trb_LockedLists.lh_Tail;
    locked_marker.ln_Pred = (struct Node *)&base.trb_LockedLists.lh_Head;
    base.trb_LockedLists.lh_Head = &locked_marker;
    base.trb_LockedLists.lh_TailPred = &locked_marker;
    event_count = 0;
    CHECK(task_RegisterNewTask(&task, &base) == TRUE);
    CHECK(list_count(&base.trb_TaskList) == 0 && list_count(&base.trb_NewTasks) == 1);
    CHECK((struct TaskListEntry *)base.trb_NewTasks.lh_Head == &allocated_entry);
    CHECK(allocated_entry.tle_Task == &task);
    CHECK(irq_depth == 0 && gate_depth == 0);
}

int main(void)
{
    alarm(20);
    test_allocation_refusal_is_quiet();
    test_registers_on_main_list();
    test_registers_on_new_tasks_while_locked_lists_exist();
    printf("registration-result: %u checks passed\n", checks);
    return EXIT_SUCCESS;
}
'''


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def extract_function(source: str, name: str) -> str:
    match = re.search(r"(?m)^BOOL\s+" + re.escape(name) + r"\s*\(", source)
    if match is None:
        raise AssertionError(f"BOOL function not found: {name}")
    brace = source.find("{", match.end())
    depth = 0
    for index in range(brace, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[match.start():index + 1]
    raise AssertionError(f"unterminated function: {name}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--artifact-dir", type=Path, required=True)
    args = parser.parse_args()
    args.artifact_dir.mkdir(parents=True, exist_ok=True)

    source_path = ROOT / "rom/task/task_execfuncs.c"
    source_text = source_path.read_text()
    interface_path = ROOT / "rom/exec/taskresource_cleanup.h"
    interface = interface_path.read_text()
    contract_match = re.search(
        r"(?m)^#define TASKRESOURCE_CLEANUP_CONTRACT (0x[0-9a-fA-F]+)UL$",
        interface,
    )
    if contract_match is None:
        raise AssertionError("cleanup contract value not found in private interface")
    if "BOOL (*prelaunch)(struct Task *, APTR);" not in interface:
        raise AssertionError("private prelaunch binding is not BOOL-returning")
    contract = contract_match.group(1)
    body = extract_function(source_text, "task_RegisterNewTask")
    generated = args.artifact_dir / "registration-result.c"
    generated.write_text(
        HARNESS.replace("@BODY@", body).replace("@CONTRACT@", contract)
    )
    print("source", digest(source_path), source_path)
    print("interface", digest(interface_path), interface_path)
    print("generated", digest(generated), generated)

    for optimization in ("O1", "O2"):
        binary = args.artifact_dir / f"registration-result-{optimization}"
        command = [
            "clang", "-std=c11", "-D_DEFAULT_SOURCE", f"-{optimization}",
            "-Wall", "-Wextra", "-Wconversion", "-Wpedantic", "-Werror",
            "-fno-strict-aliasing", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
            str(generated), "-o", str(binary),
        ]
        print("compile", " ".join(command), flush=True)
        subprocess.run(command, check=True)
        subprocess.run([str(binary)], check=True, timeout=20)
        print("artifact", binary.stat().st_size, digest(binary), binary, flush=True)
    print("Limits: allocator, IRQ mask, spinlock and Exec lists are modeled host primitives.")


if __name__ == "__main__":
    main()
