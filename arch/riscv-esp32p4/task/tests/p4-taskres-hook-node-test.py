#!/usr/bin/env python3
"""Compile actual TaskResource hook bodies and check node/startup invariants."""

from __future__ import annotations

import argparse
import hashlib
from pathlib import Path
import re
import shlex
import subprocess
import tempfile
import os


REPO = Path(__file__).resolve().parents[4]
EVIDENCE = Path(
    "/Volumes/Dev/Source/Amiga/AROS-ESP32-build/evidence/"
    "smp-priority-2026-10-01"
)
SOURCE_PATHS = {
    "AddTaskHook.c": REPO / "rom/task/AddTaskHook.c",
    "RunTaskHooks.c": REPO / "rom/task/RunTaskHooks.c",
    "InitTaskHooks.c": REPO / "rom/task/InitTaskHooks.c",
    "task_support.c": REPO / "rom/task/task_support.c",
    "task_init.c": REPO / "rom/task/task_init.c",
    "task_intern.h": REPO / "rom/task/task_intern.h",
    "hooks.h": REPO / "compiler/include/utility/hooks.h",
}


def extract_block(source: str, start: int, label: str) -> str:
    opening = source.find("{", start)
    if opening < 0:
        raise RuntimeError(f"{label}: opening brace not found")
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
                return source[start : index + 1]
        index += 1
    raise RuntimeError(f"{label}: closing brace not found")


def extract_function(source: str, pattern: str, label: str) -> str:
    match = re.search(pattern, source, re.MULTILINE)
    if match is None:
        raise RuntimeError(f"{label}: function definition not found")
    return extract_block(source, match.start(), label)


def extract_struct(source: str, name: str) -> str:
    match = re.search(rf"(?m)^struct\s+{re.escape(name)}\s*\{{", source)
    if match is None:
        raise RuntimeError(f"struct {name} not found")
    return extract_block(source, match.start(), f"struct {name}") + ";"


def check_source_contracts(sources: dict[str, str]) -> int:
    checks = 0

    def require(condition: bool, message: str) -> None:
        nonlocal checks
        checks += 1
        if not condition:
            raise RuntimeError(f"source contract failed: {message}")

    intern = sources["task_intern.h"]
    node_type = extract_struct(intern, "TaskListHookNode")
    require(
        re.search(r"struct\s+MinNode\s+tln_Node\s*;", node_type) is not None
        and re.search(r"struct\s+Hook\s*\*\s*tln_Hook\s*;", node_type) is not None,
        "TaskListHookNode owns a MinNode followed by the caller Hook pointer",
    )

    add_body = extract_function(
        sources["AddTaskHook.c"],
        r"(?m)^\s*AROS_LH2\s*\(\s*BOOL\s*,\s*AddTaskHook\s*,",
        "AddTaskHook",
    )
    require("if (!tHook)" in add_body, "AddTaskHook rejects a null Hook")
    require("AllocMem(sizeof(*hookEntry)" in add_body,
            "AddTaskHook allocates the full wrapper")
    require("if (!hookEntry)" in add_body, "AddTaskHook checks wrapper allocation")
    require("&hookEntry->tln_Node" in add_body,
            "AddTaskHook links its wrapper node")
    require("&tHook->h_MinNode" not in add_body,
            "AddTaskHook does not link caller-owned Hook storage")

    run_body = extract_function(
        sources["RunTaskHooks.c"],
        r"(?m)^\s*AROS_LH2\s*\(\s*BOOL\s*,\s*RunTaskHooks\s*,",
        "RunTaskHooks",
    )
    require("trCallHook(hookEntry->tln_Hook)" in run_body,
            "RunTaskHooks dispatches the Hook pointer stored in the wrapper")

    init_source = sources["task_init.c"]
    allocation_pattern = re.compile(
        r"if\s*\(\s*\(taskEntry\s*=\s*AllocMem\("
        r"sizeof\(struct TaskListEntry\),\s*MEMF_CLEAR\)\)\s*!=\s*NULL\s*\)"
    )
    allocations = list(allocation_pattern.finditer(init_source))
    require(len(allocations) == 5,
            "all five existing-task constructors are present")
    for number, match in enumerate(allocations, 1):
        block = extract_block(init_source, match.start(), f"task entry constructor {number}")
        init_index = block.find("NEWLIST(&taskEntry->tle_HookTypes);")
        publish_index = block.find("AddTail(&TaskResBase->trb_TaskList")
        require(init_index >= 0 and publish_index > init_index,
                f"existing-task constructor {number} initializes hooks before publication")

    sem_index = init_source.find("InitSemaphore(&TaskResBase->trb_Sem);")
    slot_index = init_source.find(
        "AllocMem(sizeof(struct TaskStorageFreeSlot), MEMF_PUBLIC|MEMF_CLEAR)"
    )
    base_index = init_source.find(
        "SysBase->lb_TaskResBase = (struct Library *)TaskResBase;"
    )
    vector_index = init_source.find("SetFunction((struct Library *)SysBase")
    require(0 <= sem_index < slot_index < base_index < vector_index,
            "SMP semaphore and fallible slot allocation precede base/vector publication")
    fail_index = init_source.find("if (!freeTaskStorageSlot)", slot_index)
    fail_return = init_source.find("return FALSE;", fail_index)
    require(slot_index < fail_index < fail_return < base_index,
            "free-slot init failure returns before TaskRes base exposure")

    exit_match = re.search(r"(?m)^static LONG taskres_Exit\s*\(", init_source)
    require(exit_match is not None, "taskres_Exit definition exists")
    exit_body = extract_block(init_source, exit_match.start(), "taskres_Exit")
    conditional_clear = exit_body.find(
        "if (SysBase->lb_TaskResBase == (struct Library *)TaskResBase)"
    )
    clear = exit_body.find("SysBase->lb_TaskResBase = NULL;", conditional_clear)
    close = exit_body.find("CloseLibrary(TaskResBase->trb_UtilityBase);")
    require(0 <= conditional_clear < clear < close,
            "expunge conditionally unpublishes this base before Utility close")
    return checks


HARNESS_PREFIX = r"""
#define _DEFAULT_SOURCE 1
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
#define MEMF_PUBLIC UINT32_C(1)
#define MEMF_CLEAR UINT32_C(2)
#define MEMF_ANY UINT32_C(0)
#define SPINLOCK_MODE_WRITE 1u
#define TASKRES_ENABLE 1

#define D(...) ((void)0)
#define AROS_LIBFUNC_INIT
#define AROS_LIBFUNC_EXIT
#define AROS_LHA(type, name, reg) type name
#define AROS_LH2(ret, name, arg1, arg2, base_type, base, lvo, library) \
    ret name(arg1, arg2, base_type base)
#define Disable() mock_disable()
#define Enable() mock_enable()
#define Forbid() mock_forbid()
#define Permit() mock_permit()
#define EXEC_SPINLOCK_LOCK(lock, failhook, mode) \
    mock_spin_lock((lock), (mode))
#define EXEC_SPINLOCK_UNLOCK(lock) mock_spin_unlock((lock))
#define NEWLIST(list) mock_list_init((void *)(list))
#define AddTail(list, node) mock_addtail_raw((void *)(list), (void *)(node))
#define ForeachNode(list, node) \
    for (*(void **)&(node) = mock_list_head((const void *)(list)); \
         mock_node_successor((const void *)(node)) != NULL; \
         *(void **)&(node) = mock_node_successor((const void *)(node)))

static void mock_list_init(void *list);
static void mock_addtail_raw(void *list, void *node);
static void *mock_list_head(const void *list);
static void *mock_node_successor(const void *node);

struct Node;
struct Task;
struct TaskResBase;

struct MinNode
{
    struct MinNode *mln_Succ;
    struct MinNode *mln_Pred;
};

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

struct MinList
{
    struct MinNode *mlh_Head;
    struct MinNode *mlh_Tail;
    struct MinNode *mlh_TailPred;
};

struct Hook
{
    struct MinNode h_MinNode;
    APTR h_Entry;
    APTR h_SubEntry;
    APTR h_Data;
};

struct Task
{
    struct Node tc_Node;
};

struct TaskListEntry
{
    struct Node tle_Node;
    struct Task *tle_Task;
    struct List tle_HookTypes;
};

struct TaskResBase
{
    spinlock_t TaskListSpinLock;
    struct List trb_TaskList;
};

typedef int (*TaskResHookDispatcher)(struct Hook *);

"""

HARNESS_RUNTIME = r"""
static unsigned int checks;
static unsigned int failures;
static unsigned int gate_depth;
static unsigned int irq_depth;
static unsigned int forbid_depth;
static unsigned int allocations;
static unsigned int fail_next_alloc;
static struct TaskResBase active_base;
static struct Task current_task;
static struct TaskListEntry current_entry;
static struct Task *current_task_ptr;
static struct Hook *observed_hooks[16];
static unsigned int observed_count;
static unsigned int immediate_count;

#define CHECK(condition) do { \
    ++checks; \
    if (!(condition)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        ++failures; \
    } \
} while (0)

static void __attribute__((unused)) mock_disable(void) { ++irq_depth; }
static void __attribute__((unused)) mock_enable(void)
{
    CHECK(irq_depth > 0u);
    if (irq_depth > 0u)
        --irq_depth;
}
static void __attribute__((unused)) mock_forbid(void) { ++forbid_depth; }
static void __attribute__((unused)) mock_permit(void)
{
    CHECK(forbid_depth > 0u);
    if (forbid_depth > 0u)
        --forbid_depth;
}
static void __attribute__((unused)) mock_spin_lock(spinlock_t *lock, unsigned int mode)
{
    (void)lock;
    CHECK(mode == SPINLOCK_MODE_WRITE);
    CHECK(gate_depth == 0u);
    ++gate_depth;
}
static void __attribute__((unused)) mock_spin_unlock(spinlock_t *lock)
{
    (void)lock;
    CHECK(gate_depth == 1u);
    if (gate_depth > 0u)
        --gate_depth;
}
static void mock_list_init(void *raw_list)
{
    void *head = (unsigned char *)raw_list + sizeof(void *);
    void *tail = NULL;
    void *tail_pred = raw_list;
    memcpy(raw_list, &head, sizeof(head));
    memcpy((unsigned char *)raw_list + sizeof(void *), &tail, sizeof(tail));
    memcpy((unsigned char *)raw_list + 2u * sizeof(void *),
           &tail_pred, sizeof(tail_pred));
}
static void mock_addtail_raw(void *raw_list, void *raw_node)
{
    void *tail_slot = (unsigned char *)raw_list + sizeof(void *);
    void *tail_pred = NULL;
    memcpy(&tail_pred, (unsigned char *)raw_list + 2u * sizeof(void *),
           sizeof(tail_pred));
    memcpy(raw_node, &tail_slot, sizeof(tail_slot));
    memcpy((unsigned char *)raw_node + sizeof(void *), &tail_pred,
           sizeof(tail_pred));
    memcpy(tail_pred, &raw_node, sizeof(raw_node));
    memcpy((unsigned char *)raw_list + 2u * sizeof(void *), &raw_node,
           sizeof(raw_node));
}
static void *mock_list_head(const void *raw_list)
{
    void *head = NULL;
    memcpy(&head, raw_list, sizeof(head));
    return head;
}
static void *mock_node_successor(const void *raw_node)
{
    void *successor = NULL;
    memcpy(&successor, raw_node, sizeof(successor));
    return successor;
}
static void *AllocMem(size_t size, ULONG flags)
{
    (void)flags;
    CHECK(gate_depth == 0u && irq_depth == 0u && forbid_depth == 0u);
    ++allocations;
    if (fail_next_alloc)
    {
        fail_next_alloc = 0u;
        return NULL;
    }
    return calloc(1u, size);
}
static void FreeMem(void *memory, size_t size)
{
    (void)size;
    CHECK(memory != NULL);
    free(memory);
}
static struct Task *FindTask(const char *name)
{
    (void)name;
    return current_task_ptr;
}

struct TaskListEntry *GetTaskEntry(struct Task *, struct TaskResBase *);
struct TaskListHookEntry *GetHookTypeEntry(struct List *, ULONG, BOOL);
BOOL AddTaskHook(struct Hook *, ULONG, struct TaskResBase *);
BOOL RunTaskHooks(APTR, ULONG, struct TaskResBase *);

static int record_hook(struct Hook *hook)
{
    CHECK(gate_depth == 0u && irq_depth == 0u && forbid_depth == 0u);
    CHECK(hook != NULL);
    CHECK(observed_count < sizeof(observed_hooks) / sizeof(observed_hooks[0]));
    if (observed_count < sizeof(observed_hooks) / sizeof(observed_hooks[0]))
        observed_hooks[observed_count++] = hook;
    return TRUE;
}
static int immediate_dispatch(struct Hook *hook)
{
    CHECK(gate_depth == 0u && irq_depth == 0u && forbid_depth == 0u);
    CHECK(hook != NULL);
    ++immediate_count;
    return TRUE;
}

static void reset_observed(void)
{
    observed_count = 0u;
    memset(observed_hooks, 0, sizeof(observed_hooks));
}

static void fixture_init(void)
{
    memset(&active_base, 0, sizeof(active_base));
    memset(&current_task, 0, sizeof(current_task));
    memset(&current_entry, 0, sizeof(current_entry));
    mock_list_init(&active_base.trb_TaskList);
    mock_list_init(&current_entry.tle_HookTypes);
    current_entry.tle_Task = &current_task;
    mock_addtail_raw(&active_base.trb_TaskList, &current_entry.tle_Node);
    current_task_ptr = &current_task;
    gate_depth = 0u;
    irq_depth = 0u;
    forbid_depth = 0u;
    allocations = 0u;
    fail_next_alloc = 0u;
    immediate_count = 0u;
    reset_observed();
}

static void hook_init(struct Hook *hook)
{
    memset(hook, 0, sizeof(*hook));
    hook->h_MinNode.mln_Succ = &hook->h_MinNode;
    hook->h_MinNode.mln_Pred = &hook->h_MinNode;
}

static void check_hook_unlinked(const struct Hook *hook)
{
    CHECK(hook->h_MinNode.mln_Succ == &hook->h_MinNode);
    CHECK(hook->h_MinNode.mln_Pred == &hook->h_MinNode);
}

static void free_hook_storage(void)
{
    struct TaskListHookEntry *type_entry;
    struct Node *type_node = current_entry.tle_HookTypes.lh_Head;
    while (type_node && type_node->ln_Succ)
    {
        struct Node *next_node = type_node->ln_Succ;
        type_entry = (struct TaskListHookEntry *)type_node;
        struct MinNode *hook_node = type_entry->tlhe_Hooks.mlh_Head;
        while (hook_node && hook_node->mln_Succ)
        {
            struct MinNode *next_hook = hook_node->mln_Succ;
            struct TaskListHookNode *wrapper =
                (struct TaskListHookNode *)hook_node;
            FreeMem(wrapper, sizeof(*wrapper));
            hook_node = next_hook;
        }
        FreeMem(type_entry, sizeof(*type_entry));
        type_node = next_node;
    }
}

static APTR dispatcher_arg(TaskResHookDispatcher dispatcher)
{
    APTR result = NULL;
    _Static_assert(sizeof(result) == sizeof(dispatcher),
                   "host fixture requires equal object/function pointer width");
    memcpy(&result, &dispatcher, sizeof(result));
    return result;
}

static void set_type_dispatcher(struct TaskListHookEntry *entry,
                                TaskResHookDispatcher dispatcher)
{
    _Static_assert(sizeof(entry->tlhe_Node.ln_Name) == sizeof(dispatcher),
                   "host fixture requires equal object/function pointer width");
    memcpy(&entry->tlhe_Node.ln_Name, &dispatcher, sizeof(dispatcher));
}

static int test_owned_wrappers_and_dispatch(void)
{
    struct Hook hook_a;
    struct Hook hook_b;
    struct TaskListHookEntry *type_one;
    struct TaskListHookEntry *type_two;
    fixture_init();
    hook_init(&hook_a);
    hook_init(&hook_b);

    CHECK(AddTaskHook(&hook_a, 1u, &active_base));
    CHECK(AddTaskHook(&hook_b, 1u, &active_base));
    CHECK(AddTaskHook(&hook_a, 2u, &active_base));
    CHECK(AddTaskHook(&hook_a, 1u, &active_base));
    check_hook_unlinked(&hook_a);
    check_hook_unlinked(&hook_b);

    type_one = GetHookTypeEntry(&current_entry.tle_HookTypes, 1u, FALSE);
    type_two = GetHookTypeEntry(&current_entry.tle_HookTypes, 2u, FALSE);
    CHECK(type_one != NULL && type_two != NULL);
    CHECK(type_one->tlhe_Hooks.mlh_Head != (struct MinNode *)&type_one->tlhe_Hooks.mlh_Tail);
    CHECK(type_two->tlhe_Hooks.mlh_Head != (struct MinNode *)&type_two->tlhe_Hooks.mlh_Tail);

    CHECK(RunTaskHooks(dispatcher_arg(record_hook), 1u, &active_base));
    CHECK(observed_count == 3u);
    if (observed_count >= 3u)
    {
        CHECK(observed_hooks[0] == &hook_a);
        CHECK(observed_hooks[1] == &hook_b);
        CHECK(observed_hooks[2] == &hook_a);
    }
    reset_observed();
    CHECK(RunTaskHooks(dispatcher_arg(record_hook), 2u, &active_base));
    CHECK(observed_count == 1u);
    if (observed_count == 1u)
        CHECK(observed_hooks[0] == &hook_a);
    CHECK(gate_depth == 0u && irq_depth == 0u && forbid_depth == 0u);
    free_hook_storage();
    return failures == 0u;
}

static int test_immediate_and_refusal(void)
{
    struct Hook immediate_hook;
    struct Hook rejected_hook;
    struct TaskListHookEntry *type_entry;
    fixture_init();
    hook_init(&immediate_hook);
    hook_init(&rejected_hook);

    type_entry = GetHookTypeEntry(&current_entry.tle_HookTypes, 7u, TRUE);
    CHECK(type_entry != NULL);
    if (type_entry)
    {
        type_entry->tlhe_Node.ln_Pri = -1;
        set_type_dispatcher(type_entry, immediate_dispatch);
    }
    CHECK(AddTaskHook(&immediate_hook, 7u, &active_base));
    CHECK(immediate_count == 1u);
    check_hook_unlinked(&immediate_hook);

    CHECK(!AddTaskHook(NULL, 8u, &active_base));
    CHECK(allocations == 1u);

    /* Fail creation of the type entry, then fail creation of its wrapper. */
    fail_next_alloc = 1u;
    CHECK(!AddTaskHook(&rejected_hook, 8u, &active_base));
    CHECK(GetHookTypeEntry(&current_entry.tle_HookTypes, 8u, FALSE) == NULL);
    type_entry = GetHookTypeEntry(&current_entry.tle_HookTypes, 9u, TRUE);
    CHECK(type_entry != NULL);
    fail_next_alloc = 1u;
    CHECK(!AddTaskHook(&rejected_hook, 9u, &active_base));
    CHECK(type_entry->tlhe_Hooks.mlh_Head ==
          (struct MinNode *)&type_entry->tlhe_Hooks.mlh_Tail);
    check_hook_unlinked(&rejected_hook);
    CHECK(gate_depth == 0u && irq_depth == 0u && forbid_depth == 0u);
    free_hook_storage();
    return failures == 0u;
}

static int test_oom_type_entry(void)
{
    struct Hook hook;
    fixture_init();
    hook_init(&hook);
    fail_next_alloc = 1u;
    CHECK(!AddTaskHook(&hook, 99u, &active_base));
    CHECK(GetHookTypeEntry(&current_entry.tle_HookTypes, 99u, FALSE) == NULL);
    CHECK(gate_depth == 0u && irq_depth == 0u && forbid_depth == 0u);
    check_hook_unlinked(&hook);
    free_hook_storage();
    return failures == 0u;
}

int main(void)
{
    (void)alarm(20u);
    if (!test_owned_wrappers_and_dispatch() ||
        !test_immediate_and_refusal() ||
        !test_oom_type_entry())
        return EXIT_FAILURE;
    CHECK(gate_depth == 0u && irq_depth == 0u && forbid_depth == 0u);
    printf("taskres-hook-node mode=%u checks=%u failures=%u passed\n",
           (unsigned int)P4_TEST_SMP, checks, failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
"""


def source_digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cc", default="clang")
    parser.add_argument("--artifact-dir", type=Path)
    args = parser.parse_args()

    sources = {
        name: path.read_text(encoding="latin-1" if name == "hooks.h" else "utf-8")
        for name, path in SOURCE_PATHS.items()
    }
    source_checks = check_source_contracts(sources)
    functions = "\n\n".join(
        [
            extract_function(
                sources["task_support.c"],
                r"(?m)^struct TaskListEntry\s*\*GetTaskEntry\s*\(",
                "GetTaskEntry",
            ),
            extract_function(
                sources["task_support.c"],
                r"(?m)^struct TaskListHookEntry\s*\*GetHookTypeEntry\s*\(",
                "GetHookTypeEntry",
            ),
            extract_function(
                sources["AddTaskHook.c"],
                r"(?m)^\s*AROS_LH2\s*\(\s*BOOL\s*,\s*AddTaskHook\s*,",
                "AddTaskHook",
            ),
            extract_function(
                sources["RunTaskHooks.c"],
                r"(?m)^\s*AROS_LH2\s*\(\s*BOOL\s*,\s*RunTaskHooks\s*,",
                "RunTaskHooks",
            ),
        ]
    )
    source_header_types = "\n\n".join(
        [
            extract_struct(sources["task_intern.h"], "TaskListHookEntry"),
            extract_struct(sources["task_intern.h"], "TaskListHookNode"),
        ]
    )
    fixture = (
        HARNESS_PREFIX
        + source_header_types
        + "\n\n"
        + HARNESS_RUNTIME
        + "\n\n"
        + functions
        + "\n"
    )

    if args.artifact_dir is None:
        EVIDENCE.mkdir(parents=True, exist_ok=True)
        artifact_dir = Path(tempfile.mkdtemp(
            prefix="p4-taskres-hook-node-author-20261002-", dir=EVIDENCE
        ))
    else:
        artifact_dir = args.artifact_dir.resolve()
        if artifact_dir.exists() and any(artifact_dir.iterdir()):
            raise SystemExit(f"artifact directory is not empty: {artifact_dir}")
        artifact_dir.mkdir(parents=True, exist_ok=True)

    fixture_path = artifact_dir / "p4-taskres-hook-node-fixture.c"
    fixture_path.write_text(fixture)
    fixture_hash = source_digest(fixture_path)
    print(f"source-contract-checks={source_checks}")
    print(f"fixture={fixture_path}")
    print(f"fixture-sha256={fixture_hash}")

    common_flags = [
        "-std=c11",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-Wconversion",
        "-Wshadow",
        "-Wstrict-prototypes",
        "-Wmissing-prototypes",
        # Existing GetHookTypeEntry assigns ULONG thType to Node.ln_Type,
        # whose AROS ABI field is UBYTE. Keep the production conversion intact.
        "-Wno-implicit-int-conversion",
        "-fsanitize=address,undefined",
        "-fno-omit-frame-pointer",
    ]
    env = os.environ.copy()
    env["ASAN_OPTIONS"] = "detect_leaks=0:halt_on_error=1"
    env["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
    for mode in (0, 1):
        for optimization in (1, 2):
            label = f"{'smp' if mode else 'normal'}-o{optimization}"
            binary = artifact_dir / f"p4-taskres-hook-node-{label}"
            command = [
                args.cc,
                *common_flags,
                f"-O{optimization}",
                f"-DP4_TEST_SMP={mode}",
                str(fixture_path),
                "-o",
                str(binary),
            ]
            print("compile:", shlex.join(command))
            compiled = subprocess.run(
                command, cwd=REPO, text=True, capture_output=True, timeout=60
            )
            if compiled.stdout:
                print(compiled.stdout, end="")
            if compiled.stderr:
                print(compiled.stderr, end="")
            if compiled.returncode:
                raise SystemExit(f"compile failed ({label}): {compiled.returncode}")
            binary_hash = source_digest(binary)
            binary_size = binary.stat().st_size
            print(f"binary-{label}-bytes={binary_size}")
            print(f"binary-{label}-sha256={binary_hash}")
            run_command = [str(binary)]
            print("run:", shlex.join(run_command))
            result = subprocess.run(
                run_command, cwd=REPO, text=True, capture_output=True,
                timeout=30, env=env,
            )
            if result.stdout:
                print(result.stdout, end="")
            if result.stderr:
                print(result.stderr, end="")
            if result.returncode:
                raise SystemExit(f"run failed ({label}): {result.returncode}")

    for name, path in SOURCE_PATHS.items():
        print(f"source-{name}-sha256={source_digest(path)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
