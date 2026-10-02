#!/usr/bin/env python3
"""Compile the actual Exec_FindChild body in SMP and non-SMP host models."""

from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile


REPO = Path(__file__).resolve().parents[4]
SOURCE_PATH = REPO / "rom/exec/exec_util.c"


def extract_find_child(source: str) -> str:
    start = source.find("struct ETask * Exec_FindChild(")
    if start < 0:
        raise RuntimeError("Exec_FindChild definition not found")
    opening = source.find("{", start)
    if opening < 0:
        raise RuntimeError("Exec_FindChild opening brace not found")

    depth = 0
    state = "code"
    escaped = False
    index = opening
    while index < len(source):
        ch = source[index]
        following = source[index + 1] if index + 1 < len(source) else ""
        if state == "line_comment":
            if ch == "\n":
                state = "code"
        elif state == "block_comment":
            if ch == "*" and following == "/":
                state = "code"
                index += 1
        elif state in ("string", "char"):
            if escaped:
                escaped = False
            elif ch == "\\":
                escaped = True
            elif (state == "string" and ch == '"') or (
                state == "char" and ch == "'"
            ):
                state = "code"
        elif ch == "/" and following == "/":
            state = "line_comment"
            index += 1
        elif ch == "/" and following == "*":
            state = "block_comment"
            index += 1
        elif ch == '"':
            state = "string"
        elif ch == "'":
            state = "char"
        elif ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                return source[opening : index + 1]
        index += 1
    raise RuntimeError("Exec_FindChild closing brace not found")


HARNESS_PREFIX = r"""
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef P4_TEST_SMP
#define P4_TEST_SMP 1
#endif
#if P4_TEST_SMP
#define __AROSEXEC_SMP__ 1
#endif
#ifdef P4_TEST_STRIP_PORT_IRQ_GATE
#define Disable() mock_noop_irq()
#define Enable() mock_noop_irq()
#else
#define Disable() mock_disable()
#define Enable() mock_enable()
#endif

typedef uint32_t ULONG;
typedef int spinlock_t;
struct ExecBase { int unused; };
struct Node { struct Node *ln_Succ; struct Node *ln_Pred; };
struct List
{
    struct Node *lh_Head;
    struct Node *lh_Tail;
    struct Node *lh_TailPred;
};
struct MsgPort { struct List mp_MsgList; spinlock_t mp_SpinLock; };
struct ETask
{
    struct Node et_Node;
    ULONG et_UniqueID;
    struct List et_Children;
    struct MsgPort et_TaskMsgPort;
};
struct IntETask { struct ETask iet_ETask; spinlock_t iet_TaskLock; };
struct Task
{
    ULONG tc_Flags;
    union { struct ETask *tc_ETask; void *tc_other; } tc_UnionETask;
};

#define TF_ETASK UINT32_C(8)
#define SPINLOCK_MODE_READ 2u
#define GET_THIS_TASK mock_current_task
#define GetETask(task) mock_get_etask((struct Task *)(task))
#define IntETask(et) ((struct IntETask *)(void *)(et))
#define EXEC_SPINLOCK_LOCK(lock, hook, mode) \
    mock_lock((spinlock_t *)(lock), (hook), (mode))
#define EXEC_SPINLOCK_UNLOCK(lock) mock_unlock((spinlock_t *)(lock))
#define ForeachNode(list, node) \
    for ((node) = mock_first((struct List *)(list)); \
         mock_continue((struct List *)(list), (node)); \
         (node) = (struct ETask *)(void *)((struct Node *)(void *)(node))->ln_Succ)

static unsigned int checks;
static unsigned int failed;
static unsigned int irq_depth;
static unsigned int irq_entry_depth;
static unsigned int disable_calls;
static unsigned int enable_calls;
static unsigned int lock_depth;
static unsigned int get_etask_calls;
static unsigned int allocator_calls;
static struct Task *mock_current_task;
static struct List *active_list;
static struct List *result_list;
static spinlock_t *active_lock;
static spinlock_t *port_lock;
static spinlock_t *held_lock;

#define CHECK(condition) do { \
    ++checks; \
    if (!(condition)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        failed = 1u; \
    } \
} while (0)

static void __attribute__((unused)) mock_noop_irq(void) { }
static void __attribute__((unused)) mock_disable(void)
{
    ++irq_depth;
    ++disable_calls;
}
static void __attribute__((unused)) mock_enable(void)
{
    CHECK(irq_depth > irq_entry_depth);
    CHECK(lock_depth == 0u);
    if (irq_depth > irq_entry_depth)
        --irq_depth;
    ++enable_calls;
}

static void list_init(struct List *list)
{
    list->lh_Head = (struct Node *)&list->lh_Tail;
    list->lh_Tail = NULL;
    list->lh_TailPred = (struct Node *)&list->lh_Head;
}

static void list_add_tail(struct List *list, struct ETask *task)
{
    struct Node *node = &task->et_Node;
    node->ln_Succ = (struct Node *)&list->lh_Tail;
    node->ln_Pred = list->lh_TailPred;
    list->lh_TailPred->ln_Succ = node;
    list->lh_TailPred = node;
}

static struct ETask *mock_get_etask(struct Task *task)
{
    ++get_etask_calls;
    if (task == NULL || (task->tc_Flags & TF_ETASK) == 0u)
        return NULL;
    return task->tc_UnionETask.tc_ETask;
}

static void __attribute__((unused)) mock_lock(
    spinlock_t *lock, void *hook, unsigned int mode)
{
    CHECK(lock_depth == 0u);
    CHECK(hook == NULL);
    CHECK(mode == SPINLOCK_MODE_READ);
    if (lock == port_lock)
        CHECK(irq_depth > irq_entry_depth);
    else
        CHECK(lock == active_lock);
    held_lock = lock;
    ++lock_depth;
}

static void __attribute__((unused)) mock_unlock(spinlock_t *lock)
{
    CHECK(lock_depth == 1u && lock == held_lock);
    if (lock == port_lock)
        CHECK(irq_depth > irq_entry_depth);
    --lock_depth;
    held_lock = NULL;
}

static struct ETask *mock_first(struct List *list)
{
    if (list == result_list)
    {
#if P4_TEST_SMP
        CHECK(lock_depth == 1u && held_lock == port_lock);
        CHECK(irq_depth > irq_entry_depth);
#else
        CHECK(lock_depth == 0u);
#endif
    }
    else
    {
        CHECK(list == active_list);
#if P4_TEST_SMP
        CHECK(lock_depth == 1u && held_lock == active_lock);
#else
        CHECK(lock_depth == 0u);
#endif
    }
    return (struct ETask *)(void *)list->lh_Head;
}

static int mock_continue(struct List *list, struct ETask *task)
{
    struct Node *node = (struct Node *)(void *)task;
    if (list == result_list)
    {
#if P4_TEST_SMP
        CHECK(lock_depth == 1u && held_lock == port_lock);
        CHECK(irq_depth > irq_entry_depth);
#else
        CHECK(lock_depth == 0u);
#endif
    }
    return node != NULL && node->ln_Succ != NULL;
}

static void reset_observation(unsigned int initial_irq_depth)
{
    irq_depth = initial_irq_depth;
    irq_entry_depth = initial_irq_depth;
    disable_calls = 0u;
    enable_calls = 0u;
    lock_depth = 0u;
    held_lock = NULL;
    get_etask_calls = 0u;
    allocator_calls = 0u;
}

struct ETask *Exec_FindChild(
    ULONG id,
    struct ExecBase *SysBase __attribute__((unused))
)
"""


HARNESS_SUFFIX = r"""

static void setup_owner(struct IntETask *owner, struct Task *task)
{
    memset(owner, 0, sizeof(*owner));
    memset(task, 0, sizeof(*task));
    list_init(&owner->iet_ETask.et_Children);
    list_init(&owner->iet_ETask.et_TaskMsgPort.mp_MsgList);
    task->tc_Flags = TF_ETASK;
    task->tc_UnionETask.tc_ETask = &owner->iet_ETask;
    active_list = &owner->iet_ETask.et_Children;
    result_list = &owner->iet_ETask.et_TaskMsgPort.mp_MsgList;
    active_lock = &owner->iet_TaskLock;
    port_lock = &owner->iet_ETask.et_TaskMsgPort.mp_SpinLock;
    mock_current_task = task;
}

static void check_balanced(void)
{
    CHECK(lock_depth == 0u && held_lock == NULL);
    CHECK(irq_depth == irq_entry_depth);
    CHECK(disable_calls == enable_calls);
    CHECK(allocator_calls == 0u);
}

static void test_null_and_non_etask(void)
{
    struct Task task = {0};
    reset_observation(0u);
    mock_current_task = NULL;
    CHECK(Exec_FindChild(1u, NULL) == NULL);
    CHECK(get_etask_calls == 1u);
    CHECK(lock_depth == 0u && disable_calls == 0u && enable_calls == 0u);

    reset_observation(0u);
    mock_current_task = &task;
    CHECK(Exec_FindChild(1u, NULL) == NULL);
    CHECK(get_etask_calls == 1u);
    CHECK(lock_depth == 0u && disable_calls == 0u && enable_calls == 0u);
}

static void test_active_match_does_not_take_port_gate(void)
{
    struct IntETask owner;
    struct Task task;
    struct ETask child = {0};
    setup_owner(&owner, &task);
    list_add_tail(active_list, &child);
    child.et_UniqueID = 11u;
    reset_observation(0u);
    CHECK(Exec_FindChild(11u, NULL) == &child);
    CHECK(disable_calls == 0u && enable_calls == 0u);
#if P4_TEST_SMP
    CHECK(lock_depth == 0u && held_lock == NULL);
#endif
    check_balanced();
}

static void test_result_match_and_missing_id(void)
{
    struct IntETask owner;
    struct Task task;
    struct ETask dead = {0};
    setup_owner(&owner, &task);
    dead.et_UniqueID = 22u;
    list_add_tail(result_list, &dead);

    reset_observation(0u);
    CHECK(Exec_FindChild(22u, NULL) == &dead);
#if P4_TEST_SMP
    CHECK(disable_calls == 1u && enable_calls == 1u);
#else
    CHECK(disable_calls == 0u && enable_calls == 0u);
#endif
    check_balanced();

    reset_observation(0u);
    CHECK(Exec_FindChild(99u, NULL) == NULL);
#if P4_TEST_SMP
    CHECK(disable_calls == 1u && enable_calls == 1u);
#else
    CHECK(disable_calls == 0u && enable_calls == 0u);
#endif
    check_balanced();
}

static void test_pre_disabled_caller(void)
{
    struct IntETask owner;
    struct Task task;
    struct ETask dead = {0};
    setup_owner(&owner, &task);
    dead.et_UniqueID = 33u;
    list_add_tail(result_list, &dead);
    reset_observation(1u);
    CHECK(Exec_FindChild(33u, NULL) == &dead);
    CHECK(irq_depth == 1u);
#if P4_TEST_SMP
    CHECK(disable_calls == 1u && enable_calls == 1u);
#else
    CHECK(disable_calls == 0u && enable_calls == 0u);
#endif
    check_balanced();
}

int main(void)
{
    test_null_and_non_etask();
    test_active_match_does_not_take_port_gate();
    test_result_match_and_missing_id();
    test_pre_disabled_caller();
    if (failed != 0u)
        return 1;
#if P4_TEST_SMP
    printf("p4-find-child-port-irq-test SMP: PASS (%u checks)\n", checks);
#else
    printf("p4-find-child-port-irq-test non-SMP: PASS (%u checks)\n", checks);
#endif
    return 0;
}
"""


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--artifact-dir", type=Path)
    args = parser.parse_args()
    source = SOURCE_PATH.read_text(encoding="utf-8")
    body = extract_find_child(source)
    source_hash = hashlib.sha256(source.encode("utf-8")).hexdigest()
    body_hash = hashlib.sha256(body.encode("utf-8")).hexdigest()
    fixture_hash = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
    compiler = shutil.which(os.environ.get("CC", "clang"))
    if compiler is None:
        raise SystemExit("clang was not found; set CC to a Clang executable")
    run_env = os.environ.copy()
    run_env["ASAN_OPTIONS"] = "detect_leaks=0:halt_on_error=1"
    run_env["UBSAN_OPTIONS"] = "halt_on_error=1"

    common = [
        compiler,
        "-std=c11",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-Wconversion",
        "-Wsign-conversion",
        "-Wpedantic",
        "-fsanitize=address,undefined",
        "-fno-omit-frame-pointer",
        "-O1",
    ]

    def build_and_run(output: Path) -> None:
        print("Exec_FindChild source SHA-256:", source_hash)
        print("Exec_FindChild body SHA-256:", body_hash)
        print("Fixture SHA-256:", fixture_hash)
        c_path = output / "find_child_fixture.c"
        c_path.write_text(HARNESS_PREFIX + body + HARNESS_SUFFIX, encoding="utf-8")
        for mode, name in ((1, "smp"), (0, "normal")):
            exe = output / f"find_child_{name}"
            command = common + [f"-DP4_TEST_SMP={mode}", str(c_path), "-o", str(exe)]
            print(f"Compile ({name}):", shlex.join(command))
            subprocess.run(command, check=True, timeout=30)
            subprocess.run([str(exe)], check=True, timeout=20, env=run_env)

        negative = output / "find_child_smp_no_irq_gate"
        negative_command = common + [
            "-DP4_TEST_SMP=1",
            "-DP4_TEST_STRIP_PORT_IRQ_GATE=1",
            str(c_path),
            "-o",
            str(negative),
        ]
        print("Compile (stripped IRQ gate):", shlex.join(negative_command))
        subprocess.run(negative_command, check=True, timeout=30)
        result = subprocess.run(
            [str(negative)], capture_output=True, text=True, timeout=20, env=run_env
        )
        text = result.stdout + result.stderr
        (output / "find_child_negative-control.log").write_text(text, encoding="utf-8")
        if result.returncode == 0 or "irq_depth > irq_entry_depth" not in text:
            raise RuntimeError("stripped Exec_FindChild IRQ gate was not detected")
        print("Exec_FindChild stripped-gate negative control: detected as expected")

    if args.artifact_dir is not None:
        args.artifact_dir.mkdir(parents=True, exist_ok=True)
        build_and_run(args.artifact_dir)
    else:
        with tempfile.TemporaryDirectory(prefix="p4-find-child-port-irq-") as temp:
            build_and_run(Path(temp))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
