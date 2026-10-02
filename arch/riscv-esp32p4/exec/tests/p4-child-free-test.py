#!/usr/bin/env python3
"""Compile and exercise the actual SMP ChildFree function body."""

from __future__ import annotations

import hashlib
import argparse
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile


def extract_child_free(source: str) -> str:
    signature = "AROS_LH1(void, ChildFree,"
    start = source.find(signature)
    if start < 0:
        raise RuntimeError("ChildFree definition not found")
    marker = "\n{\n    AROS_LIBFUNC_INIT"
    opening = source.find(marker, start)
    if opening < 0:
        raise RuntimeError("ChildFree function body not found")
    opening += 1

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
    raise RuntimeError("ChildFree closing brace not found")


HARNESS_PREFIX = r"""
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define __AROSEXEC_SMP__ 1
typedef uint32_t ULONG;
typedef int spinlock_t;
#define TF_ETASK UINT32_C(8)
#define SPINLOCK_MODE_WRITE 1u
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
#define GetETask(task) mock_get_etask((struct Task *)(task))
#define Remove(node) mock_remove((struct Node *)(node))
#define EXEC_SPINLOCK_LOCK(lock, hook, mode) \
    mock_lock((spinlock_t *)(lock), (hook), (mode))
#define EXEC_SPINLOCK_UNLOCK(lock) mock_unlock((spinlock_t *)(lock))
#define ForeachNode(list, node) \
    for ((node) = (struct ETask *)((struct List *)(list))->lh_Head; \
         ((struct Node *)(node))->ln_Succ != NULL; \
         (node) = (struct ETask *)((struct Node *)(node))->ln_Succ)
#define FindChild(tid) forbidden_find_child((ULONG)(tid))

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
struct Message { struct Node mn_Node; };
struct MsgPort
{
    struct List mp_MsgList;
    spinlock_t mp_SpinLock;
};
struct ETask
{
    struct Message et_Message;
    ULONG et_UniqueID;
    struct MsgPort et_TaskMsgPort;
};
struct Task
{
    ULONG tc_Flags;
    union { struct ETask *tc_ETask; void *tc_other; } tc_UnionETask;
};
struct ExecBase { int unused; };

static unsigned int checks;
static unsigned int forbid_depth;
static unsigned int irq_depth;
static unsigned int irq_entry_depth;
static unsigned int disable_calls;
static unsigned int enable_calls;
static unsigned int lock_depth;
static unsigned int get_etask_calls;
static unsigned int expunge_calls;
static unsigned int find_child_calls;
static unsigned int remove_calls;
static int failed;
static struct Task *mock_current_task;
static struct ETask *active_child;
static struct ETask *expected_expunge;
static ULONG expected_expunge_id;
static spinlock_t *expected_port_lock;
static spinlock_t *held_lock;

#define CHECK(condition) do { \
    ++checks; \
    if (!(condition)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        failed = 1; \
    } \
} while (0)

static void init_list(struct List *list)
{
    list->lh_Head = (struct Node *)&list->lh_Tail;
    list->lh_Tail = NULL;
    list->lh_TailPred = (struct Node *)list;
}

static void add_tail(struct List *list, struct Node *node)
{
    node->ln_Succ = (struct Node *)&list->lh_Tail;
    node->ln_Pred = list->lh_TailPred;
    list->lh_TailPred->ln_Succ = node;
    list->lh_TailPred = node;
}

static struct Node *mock_remove(struct Node *node)
{
    CHECK(forbid_depth != 0);
    CHECK(lock_depth == 1);
    CHECK(held_lock == expected_port_lock);
    CHECK(node != NULL && node->ln_Pred != NULL && node->ln_Succ != NULL);
    if (node != NULL && node->ln_Pred != NULL && node->ln_Succ != NULL)
    {
        node->ln_Pred->ln_Succ = node->ln_Succ;
        node->ln_Succ->ln_Pred = node->ln_Pred;
    }
    ++remove_calls;
    return node;
}

static void mock_forbid(void) { ++forbid_depth; }
static void mock_permit(void)
{
    CHECK(forbid_depth != 0);
    if (forbid_depth != 0)
        --forbid_depth;
}

static void __attribute__((unused)) mock_noop_irq(void) { }

static void __attribute__((unused)) mock_disable(void)
{
    ++disable_calls;
    ++irq_depth;
}

static void __attribute__((unused)) mock_enable(void)
{
    CHECK(irq_depth > irq_entry_depth);
    CHECK(lock_depth == 0u);
    if (irq_depth > irq_entry_depth)
        --irq_depth;
    ++enable_calls;
}

static void mock_lock(spinlock_t *lock, void *hook, unsigned int mode)
{
    CHECK(forbid_depth != 0);
    CHECK(irq_depth > irq_entry_depth);
    CHECK(lock_depth == 0);
    CHECK(lock == expected_port_lock);
    CHECK(hook == NULL);
    CHECK(mode == SPINLOCK_MODE_WRITE);
    held_lock = lock;
    ++lock_depth;
}

static void mock_unlock(spinlock_t *lock)
{
    CHECK(irq_depth > irq_entry_depth);
    CHECK(lock_depth == 1);
    CHECK(lock == held_lock && lock == expected_port_lock);
    --lock_depth;
    held_lock = NULL;
}

static struct ETask *mock_get_etask(struct Task *task)
{
    ++get_etask_calls;
    CHECK(task != NULL);
    if (task == NULL || (task->tc_Flags & TF_ETASK) == 0)
        return NULL;
    return task->tc_UnionETask.tc_ETask;
}

struct ETask *forbidden_find_child(ULONG tid)
{
    (void)tid;
    ++find_child_calls;
    return active_child;
}

static void ExpungeETask(struct ETask *et)
{
    CHECK(et != NULL);
    CHECK(forbid_depth == 0);
    CHECK(lock_depth == 0);
    CHECK(held_lock == NULL);
    CHECK(irq_depth == irq_entry_depth);
    CHECK(et == expected_expunge);
    if (et != NULL)
        CHECK(et->et_UniqueID == expected_expunge_id);
    ++expunge_calls;
    free(et); /* ASan poisons the result; any post-expunge dereference fails. */
}

static unsigned int list_count(const struct List *list)
{
    const struct Node *node = list->lh_Head;
    unsigned int count = 0;
    while (node != (const struct Node *)&list->lh_Tail && count < 64u)
    {
        ++count;
        node = node->ln_Succ;
    }
    CHECK(count < 64u);
    return count;
}

static ULONG list_id_at(const struct List *list, unsigned int wanted)
{
    const struct Node *node = list->lh_Head;
    unsigned int index = 0;
    while (node != (const struct Node *)&list->lh_Tail && index < 64u)
    {
        if (index == wanted)
            return ((const struct ETask *)node)->et_UniqueID;
        ++index;
        node = node->ln_Succ;
    }
    CHECK(0);
    return UINT32_MAX;
}

static struct ETask *new_result(ULONG id)
{
    struct ETask *et = (struct ETask *)calloc(1u, sizeof(*et));
    if (et != NULL)
        et->et_UniqueID = id;
    return et;
}

static void reset_observation(void)
{
    get_etask_calls = 0;
    expunge_calls = 0;
    find_child_calls = 0;
    remove_calls = 0;
    irq_depth = 0u;
    irq_entry_depth = 0u;
    disable_calls = 0u;
    enable_calls = 0u;
    lock_depth = 0u;
    held_lock = NULL;
    expected_expunge = NULL;
    expected_expunge_id = 0;
}

void ChildFree(ULONG tid, struct ExecBase *SysBase __attribute__((unused)))
"""


HARNESS_SUFFIX = r"""

static void check_balanced(void)
{
    CHECK(forbid_depth == 0);
    CHECK(irq_depth == irq_entry_depth);
    CHECK(disable_calls == enable_calls);
    CHECK(lock_depth == 0);
    CHECK(held_lock == NULL);
}

static void test_null_and_non_etask(void)
{
    struct Task ordinary = {0};
    reset_observation();
    mock_current_task = NULL;
    ChildFree(7u, NULL);
    CHECK(get_etask_calls == 0u);
    CHECK(expunge_calls == 0u && remove_calls == 0u);
    check_balanced();

    reset_observation();
    mock_current_task = &ordinary;
    ChildFree(7u, NULL);
    CHECK(get_etask_calls == 1u);
    CHECK(expunge_calls == 0u && remove_calls == 0u);
    check_balanced();
}

static void test_null_etask_pointer(void)
{
    struct Task task = {0};
    task.tc_Flags = TF_ETASK;
    task.tc_UnionETask.tc_ETask = NULL;
    reset_observation();
    mock_current_task = &task;
    ChildFree(9u, NULL);
    CHECK(get_etask_calls == 1u);
    CHECK(expunge_calls == 0u && remove_calls == 0u);
    check_balanced();
}

static void test_active_and_missing_ids_ignored(void)
{
    struct Task task = {0};
    struct ETask owner = {0};
    struct ETask *active = new_result(101u);
    CHECK(active != NULL);
    if (active == NULL)
        return;
    init_list(&owner.et_TaskMsgPort.mp_MsgList);
    task.tc_Flags = TF_ETASK;
    task.tc_UnionETask.tc_ETask = &owner;
    active_child = active;
    expected_port_lock = &owner.et_TaskMsgPort.mp_SpinLock;
    mock_current_task = &task;

    reset_observation();
    ChildFree(101u, NULL);
    CHECK(list_count(&owner.et_TaskMsgPort.mp_MsgList) == 0u);
    CHECK(expunge_calls == 0u && remove_calls == 0u);
    CHECK(find_child_calls == 0u);
    check_balanced();

    /* An already-disabled caller remains disabled after the local port gate. */
    reset_observation();
    irq_depth = 1u;
    irq_entry_depth = 1u;
    expected_port_lock = &owner.et_TaskMsgPort.mp_SpinLock;
    mock_current_task = &task;
    ChildFree(999u, NULL);
    CHECK(irq_depth == 1u);
    CHECK(disable_calls == 1u && enable_calls == 1u);
    CHECK(expunge_calls == 0u && remove_calls == 0u);
    check_balanced();

    reset_observation();
    ChildFree(999u, NULL);
    CHECK(expunge_calls == 0u && remove_calls == 0u);
    CHECK(find_child_calls == 0u);
    check_balanced();
    free(active);
    active_child = NULL;
}

static void test_dead_result_head_middle_tail(void)
{
    struct Task task = {0};
    struct ETask owner = {0};
    struct ETask *first = new_result(11u);
    struct ETask *middle = new_result(22u);
    struct ETask *last = new_result(33u);
    struct ETask *tail = new_result(44u);
    CHECK(first != NULL && middle != NULL && last != NULL && tail != NULL);
    if (first == NULL || middle == NULL || last == NULL || tail == NULL)
    {
        free(first);
        free(middle);
        free(last);
        free(tail);
        return;
    }

    init_list(&owner.et_TaskMsgPort.mp_MsgList);
    add_tail(&owner.et_TaskMsgPort.mp_MsgList, (struct Node *)first);
    add_tail(&owner.et_TaskMsgPort.mp_MsgList, (struct Node *)middle);
    add_tail(&owner.et_TaskMsgPort.mp_MsgList, (struct Node *)last);
    add_tail(&owner.et_TaskMsgPort.mp_MsgList, (struct Node *)tail);
    task.tc_Flags = TF_ETASK;
    task.tc_UnionETask.tc_ETask = &owner;
    expected_port_lock = &owner.et_TaskMsgPort.mp_SpinLock;
    mock_current_task = &task;

    reset_observation();
    expected_expunge = middle;
    expected_expunge_id = 22u;
    ChildFree(22u, NULL);
    CHECK(expunge_calls == 1u && remove_calls == 1u);
    CHECK(list_count(&owner.et_TaskMsgPort.mp_MsgList) == 3u);
    CHECK(list_id_at(&owner.et_TaskMsgPort.mp_MsgList, 0u) == 11u);
    CHECK(list_id_at(&owner.et_TaskMsgPort.mp_MsgList, 1u) == 33u);
    CHECK(list_id_at(&owner.et_TaskMsgPort.mp_MsgList, 2u) == 44u);
    CHECK(find_child_calls == 0u);
    check_balanced();

    reset_observation();
    expected_expunge = first;
    expected_expunge_id = 11u;
    ChildFree(11u, NULL);
    CHECK(expunge_calls == 1u && remove_calls == 1u);
    CHECK(list_count(&owner.et_TaskMsgPort.mp_MsgList) == 2u);
    CHECK(list_id_at(&owner.et_TaskMsgPort.mp_MsgList, 0u) == 33u);
    CHECK(list_id_at(&owner.et_TaskMsgPort.mp_MsgList, 1u) == 44u);
    check_balanced();

    reset_observation();
    expected_expunge = tail;
    expected_expunge_id = 44u;
    ChildFree(44u, NULL);
    CHECK(expunge_calls == 1u && remove_calls == 1u);
    CHECK(list_count(&owner.et_TaskMsgPort.mp_MsgList) == 1u);
    CHECK(list_id_at(&owner.et_TaskMsgPort.mp_MsgList, 0u) == 33u);
    CHECK(find_child_calls == 0u);
    check_balanced();

    free(last);
}

int main(void)
{
    test_null_and_non_etask();
    test_null_etask_pointer();
    test_active_and_missing_ids_ignored();
    test_dead_result_head_middle_tail();
    if (failed)
        return 1;
    printf("p4-child-free-test: PASS (%u checks)\n", checks);
    return 0;
}
"""


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--artifact-dir", type=Path)
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[4]
    source_path = repo / "rom/exec/childfree.c"
    source = source_path.read_text(encoding="utf-8")
    function = extract_child_free(source)
    function_hash = hashlib.sha256(function.encode("utf-8")).hexdigest()
    test_hash = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
    compiler = shutil.which(os.environ.get("CC", "clang"))
    if compiler is None:
        raise SystemExit("clang was not found; set CC to a Clang executable")
    run_env = os.environ.copy()
    run_env["ASAN_OPTIONS"] = "detect_leaks=0:halt_on_error=1"
    run_env["UBSAN_OPTIONS"] = "halt_on_error=1"

    command = [
        compiler,
        "-std=c11",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-Wconversion",
        "-Wsign-conversion",
        "-fsanitize=address,undefined",
        "-fno-omit-frame-pointer",
        "-O1",
    ]
    def build_and_run(temp_path: Path) -> None:
        c_path = temp_path / "child_free_fixture.c"
        exe_path = temp_path / "child_free_fixture"
        c_path.write_text(
            HARNESS_PREFIX + function + HARNESS_SUFFIX, encoding="utf-8"
        )
        compile_command = command + [str(c_path), "-o", str(exe_path)]
        print("ChildFree body SHA-256:", function_hash)
        print("ChildFree body bytes:", len(function.encode("utf-8")))
        print("Fixture SHA-256:", test_hash)
        print("Compile:", shlex.join(compile_command))
        subprocess.run(compile_command, check=True, timeout=30)
        subprocess.run([str(exe_path)], check=True, timeout=20, env=run_env)

        negative_path = temp_path / "child_free_fixture_no_irq_gate"
        negative_command = command + [
            "-DP4_TEST_STRIP_PORT_IRQ_GATE=1",
            str(c_path),
            "-o",
            str(negative_path),
        ]
        print("Compile (stripped IRQ gate):", shlex.join(negative_command))
        subprocess.run(negative_command, check=True, timeout=30)
        negative = subprocess.run(
            [str(negative_path)], capture_output=True, text=True, timeout=20,
            env=run_env,
        )
        negative_text = negative.stdout + negative.stderr
        (temp_path / "child_free_negative-control.log").write_text(
            negative_text, encoding="utf-8"
        )
        if negative.returncode == 0 or "irq_depth > irq_entry_depth" not in negative_text:
            raise RuntimeError("stripped Disable/Enable negative control was not detected")
        print("ChildFree stripped-gate negative control: detected as expected")

    if args.artifact_dir is not None:
        args.artifact_dir.mkdir(parents=True, exist_ok=True)
        build_and_run(args.artifact_dir)
    else:
        with tempfile.TemporaryDirectory(prefix="p4-child-free-") as temp:
            build_and_run(Path(temp))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
