#!/usr/bin/env python3
"""Compile the actual SMP Exec_CleanupETask body against a minimal list mock."""

from __future__ import annotations

import hashlib
import argparse
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import tempfile


def extract_cleanup(source: str) -> str:
    match = re.search(r"(?m)^void\s*\nExec_CleanupETask\s*\(", source)
    if match is None:
        raise RuntimeError("Exec_CleanupETask definition not found")
    opening = source.find("{", match.start())
    if opening < 0:
        raise RuntimeError("Exec_CleanupETask opening brace not found")

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
                return source[match.start() : index + 1]
        index += 1
    raise RuntimeError("Exec_CleanupETask closing brace not found")


HARNESS_PREFIX = r"""
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define __AROSEXEC_SMP__ 1
typedef uint32_t ULONG;
typedef uint16_t UWORD;
typedef uintptr_t IPTR;
typedef int BOOL;
typedef int spinlock_t;

#define TRUE 1
#define FALSE 0
#define TF_ETASK (1u << 0)
#define EXECF_CPUAffinity (1u << 2)
#define TASKAFFINITY_ANY ((IPTR)UINTPTR_MAX)
#define SPINLOCK_MODE_WRITE 1u
#define NT_TASK 1u
#define NT_PROCESS 13u
#define PRF_NOTIFYONDEATH (1u << 4)
#define D(...) do { } while (0);
#define PrivExecBase(base) ((struct IntExecBase *)(base))
#define IntETask(et) ((struct IntETask *)(et))
#define GetETask(task) mock_get_etask((struct Task *)(task))
#define ForeachNodeSafe(list, current, next)                         \
    for (*(void **)&(current) = (void *)((struct List *)(list))->lh_Head; \
         (*(void **)&(next) = (void *)((struct Node *)(current))->ln_Succ); \
         *(void **)&(current) = (void *)(next))
#define NEWLIST(list) init_list((struct List *)(list))
#define AddTail(list, node) add_tail((struct List *)(list), (struct Node *)(node))
#define RemHead(list) rem_head((struct List *)(list))
#define ADDTAIL(list, node) \
    add_tail((struct List *)(list), (struct Node *)(node))
#define REMOVE(node) ((void)Remove((struct Node *)(node)))
#define EXEC_SPINLOCK_LOCK(lock, hook, mode) \
    mock_lock((spinlock_t *)(lock), (mode))
#define EXEC_SPINLOCK_UNLOCK(lock) mock_unlock((spinlock_t *)(lock))
#ifdef P4_TEST_STRIP_PORT_IRQ_GATE
#define Disable() mock_noop_irq()
#define Enable() mock_noop_irq()
#else
#define Disable() mock_disable()
#define Enable() mock_enable()
#endif

struct Task;
struct ETask;
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
    struct Node *mlh_Head;
    struct Node *mlh_Tail;
    struct Node *mlh_TailPred;
};
struct Message
{
    struct Node mn_Node;
};
struct MsgPort
{
    struct List mp_MsgList;
    spinlock_t mp_SpinLock;
    struct Task *mp_SigTask;
    uint8_t mp_SigBit;
};
struct ETask
{
    struct Message et_Message;
    struct Task *et_Parent;
    ULONG et_UniqueID;
    struct MinList et_Children;
    UWORD et_TrapAlloc;
    UWORD et_TrapAble;
    ULONG et_Result1;
    void *et_Result2;
    struct MsgPort et_TaskMsgPort;
};
struct IntETask
{
    struct ETask iet_ETask;
    spinlock_t iet_TaskLock;
    void *iet_CpuAffinity;
};
struct Task
{
    struct Node tc_Node;
    ULONG tc_Flags;
    union
    {
        struct ETask *tc_ETask;
        void *tc_other;
    } tc_UnionETask;
};
struct Process
{
    struct Task pr_Task;
    ULONG pr_Flags;
};
struct ExecBase
{
    ULONG unused;
};
struct IntExecBase
{
    struct ExecBase pub;
    ULONG IntFlags;
};

static unsigned int checks;
static int failed;
static int forbid_depth;
static spinlock_t *lock_stack[16];
static unsigned int lock_depth;
static unsigned int expunge_count;
static unsigned int putmsg_count;
static unsigned int irq_depth;
static unsigned int irq_entry_depth;
static unsigned int disable_calls;
static unsigned int enable_calls;
static struct ETask *current_cleanup_etask;
static struct MsgPort *current_cleanup_port;
static struct MsgPort *expected_notification_port;
static struct ETask *expected_dead_results[4];
static unsigned int expected_dead_result_count;
static unsigned int expected_dead_results_seen;
static int check_current_children_empty_on_expunge;
static int current_children_empty_observed;

#define CHECK(expr) do {                                                \
    ++checks;                                                           \
    if (!(expr))                                                        \
    {                                                                   \
        fprintf(stderr, "check failed at harness line %d: %s\n",       \
                __LINE__, #expr);                                       \
        failed = 1;                                                     \
    }                                                                   \
} while (0)

static void init_list(struct List *list)
{
    list->lh_Head = (struct Node *)&list->lh_Tail;
    list->lh_Tail = NULL;
    list->lh_TailPred = (struct Node *)list;
}

static void init_min_list(struct MinList *list)
{
    struct List *as_list = (struct List *)list;
    init_list(as_list);
}

static void add_tail(struct List *list, struct Node *node)
{
    node->ln_Succ = (struct Node *)&list->lh_Tail;
    node->ln_Pred = list->lh_TailPred;
    list->lh_TailPred->ln_Succ = node;
    list->lh_TailPred = node;
}

static struct Node *Remove(struct Node *node)
{
    node->ln_Pred->ln_Succ = node->ln_Succ;
    node->ln_Succ->ln_Pred = node->ln_Pred;
    return node;
}

static struct Node *rem_head(struct List *list)
{
    struct Node *node = list->lh_Head;
    return node != NULL && node->ln_Succ != NULL ? Remove(node) : NULL;
}

static int list_empty(const struct List *list)
{
    return list->lh_Head == (const struct Node *)&list->lh_Tail &&
           list->lh_Tail == NULL &&
           list->lh_TailPred == (const struct Node *)list;
}

static int list_count(const struct List *list)
{
    const struct Node *node = list->lh_Head;
    int count = 0;
    while (node != NULL && node->ln_Succ != NULL && count < 32)
    {
        ++count;
        node = node->ln_Succ;
    }
    return count < 32 ? count : -1;
}

static int list_contains(const struct List *list, const struct Node *wanted)
{
    const struct Node *node = list->lh_Head;
    int count = 0;
    while (node != NULL && node->ln_Succ != NULL && count < 32)
    {
        if (node == wanted)
            return 1;
        ++count;
        node = node->ln_Succ;
    }
    return count < 32 ? 0 : -1;
}

static int lock_is_held(const spinlock_t *wanted)
{
    for (unsigned int index = 0; index < lock_depth; ++index)
        if (lock_stack[index] == wanted)
            return 1;
    return 0;
}

static int is_message_port_lock(const spinlock_t *lock)
{
    return (current_cleanup_port != NULL &&
            lock == &current_cleanup_port->mp_SpinLock) ||
           (expected_notification_port != NULL &&
            lock == &expected_notification_port->mp_SpinLock);
}

static int list_well_formed(const struct List *list)
{
    const struct Node *node = list->lh_Head;
    const struct Node *previous = (const struct Node *)&list->lh_Head;
    int count = 0;
    while (node != NULL && node->ln_Succ != NULL && count < 32)
    {
        if (node->ln_Pred != previous || node->ln_Succ->ln_Pred != node)
            return 0;
        previous = node;
        node = node->ln_Succ;
        ++count;
    }
    if (count >= 32 || node != (const struct Node *)&list->lh_Tail)
        return 0;
    return list->lh_TailPred == previous &&
           ((const struct Node *)&list->lh_Tail)->ln_Pred == previous;
}

static void mock_lock(spinlock_t *lock, unsigned int mode)
{
    CHECK(mode == SPINLOCK_MODE_WRITE);
    if (is_message_port_lock(lock))
        CHECK(irq_depth > irq_entry_depth);
    CHECK(lock_depth < sizeof(lock_stack) / sizeof(lock_stack[0]));
    if (lock_depth < sizeof(lock_stack) / sizeof(lock_stack[0]))
        lock_stack[lock_depth++] = lock;
}

static void mock_unlock(spinlock_t *lock)
{
    CHECK(lock_depth != 0);
    if (is_message_port_lock(lock))
        CHECK(irq_depth > irq_entry_depth);
    if (lock_depth != 0)
        CHECK(lock_stack[--lock_depth] == lock);
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

static void Forbid(void) { ++forbid_depth; }
static void Permit(void) { --forbid_depth; }
static void KrnFreeCPUMask(void *mask)
{
    CHECK(lock_depth == 0u && irq_depth == irq_entry_depth);
    free(mask);
}
static void FreeVec(void *memory)
{
    CHECK(lock_depth == 0u && irq_depth == irq_entry_depth);
    free(memory);
}
static void FreeMem(void *memory, size_t size)
{
    (void)size;
    CHECK(lock_depth == 0u && irq_depth == irq_entry_depth);
    free(memory);
}

static struct ETask *mock_get_etask(struct Task *task)
{
    if (task == NULL || (task->tc_Flags & TF_ETASK) == 0)
        return NULL;
    return task->tc_UnionETask.tc_ETask;
}

static struct IntETask *new_etask(void)
{
    struct IntETask *storage = calloc(1, sizeof(*storage));
    if (storage != NULL)
    {
        init_min_list(&storage->iet_ETask.et_Children);
        init_list(&storage->iet_ETask.et_TaskMsgPort.mp_MsgList);
    }
    return storage;
}

static void init_task(struct Task *task, struct ETask *et)
{
    memset(task, 0, sizeof(*task));
    task->tc_Flags = TF_ETASK;
    task->tc_UnionETask.tc_ETask = et;
}

static void ExpungeETask(struct ETask *et);
static void PutMsg(struct MsgPort *port, struct Message *message);
"""


HARNESS_SUFFIX = r"""
static void ExpungeETask(struct ETask *et)
{
    CHECK(et != NULL);
    if (current_cleanup_port != NULL)
    {
        CHECK(lock_depth == 0);
        CHECK(!lock_is_held(&current_cleanup_port->mp_SpinLock));
    }
    CHECK(irq_depth == irq_entry_depth);
    if (et == current_cleanup_etask && check_current_children_empty_on_expunge)
    {
        CHECK(list_empty((const struct List *)&et->et_Children));
        current_children_empty_observed = 1;
    }
    if (et != current_cleanup_etask && current_cleanup_port != NULL)
    {
        CHECK(list_well_formed(&current_cleanup_port->mp_MsgList));
        CHECK(list_empty(&current_cleanup_port->mp_MsgList));
        CHECK(list_contains(&current_cleanup_port->mp_MsgList,
                            (const struct Node *)et) == 0);
        CHECK(expected_dead_result_count <=
              sizeof(expected_dead_results) / sizeof(expected_dead_results[0]));
        for (unsigned int index = 0; index < expected_dead_result_count; ++index)
            CHECK(list_contains(&current_cleanup_port->mp_MsgList,
                                (const struct Node *)expected_dead_results[index]) == 0);
        ++expected_dead_results_seen;
    }
    ++expunge_count;
    FreeVec(et->et_Result2);
    FreeMem(et, sizeof(struct IntETask));
}

static void PutMsg(struct MsgPort *port, struct Message *message)
{
    CHECK(lock_depth == 0u && irq_depth == irq_entry_depth);
    expected_notification_port = port;
    Disable();
    mock_lock(&port->mp_SpinLock, SPINLOCK_MODE_WRITE);
    add_tail(&port->mp_MsgList, &message->mn_Node);
    mock_unlock(&port->mp_SpinLock);
    Enable();
    expected_notification_port = NULL;
    ++putmsg_count;
}

static void reset_irq_observation(unsigned int initial_depth)
{
    CHECK(lock_depth == 0u);
    irq_depth = initial_depth;
    irq_entry_depth = initial_depth;
    disable_calls = 0u;
    enable_calls = 0u;
}

static struct ETask *new_child(struct Task *parent)
{
    struct IntETask *storage = new_etask();
    if (storage != NULL)
        storage->iet_ETask.et_Parent = parent;
    return storage == NULL ? NULL : &storage->iet_ETask;
}

static void test_reparent_and_message_detach(void)
{
    struct IntExecBase sysbase = {{0}, 0};
    struct IntETask *parent_storage = new_etask();
    struct IntETask *current_storage = new_etask();
    struct ETask *dead1 = new_child(NULL);
    struct ETask *dead2 = new_child(NULL);
    struct ETask *child1 = new_child(NULL);
    struct ETask *child2 = new_child(NULL);
    struct Process current = {0};
    struct Task parent_task = {0};
    struct ETask *parent_et = &parent_storage->iet_ETask;
    struct ETask *current_et = &current_storage->iet_ETask;

    CHECK(parent_storage != NULL && current_storage != NULL && dead1 != NULL &&
          dead2 != NULL && child1 != NULL && child2 != NULL);
    if (failed)
        return;

    init_task(&parent_task, parent_et);
    init_task(&current.pr_Task, current_et);
    current.pr_Task.tc_Node.ln_Type = NT_PROCESS;
    current.pr_Flags = PRF_NOTIFYONDEATH;
    current_et->et_Parent = &parent_task;
    parent_et->et_TaskMsgPort.mp_SigTask = &parent_task;

    add_tail((struct List *)&parent_et->et_Children,
             (struct Node *)current_et);
    add_tail((struct List *)&current_et->et_Children, (struct Node *)child1);
    add_tail((struct List *)&current_et->et_Children, (struct Node *)child2);
    add_tail(&current_et->et_TaskMsgPort.mp_MsgList, (struct Node *)dead1);
    add_tail(&current_et->et_TaskMsgPort.mp_MsgList, (struct Node *)dead2);

    current_cleanup_etask = current_et;
    current_cleanup_port = &current_et->et_TaskMsgPort;
    expected_notification_port = &parent_et->et_TaskMsgPort;
    check_current_children_empty_on_expunge = 0;
    expunge_count = putmsg_count = 0;
    expected_dead_results[0] = dead1;
    expected_dead_results[1] = dead2;
    expected_dead_result_count = 2;
    expected_dead_results_seen = 0;
    reset_irq_observation(0u);
    Exec_CleanupETask(&current.pr_Task, (struct ExecBase *)&sysbase);

    CHECK(expunge_count == 2);
    CHECK(expected_dead_results_seen == 2);
    CHECK(putmsg_count == 1);
    CHECK(list_empty(&current_et->et_TaskMsgPort.mp_MsgList));
    CHECK(list_well_formed(&parent_et->et_TaskMsgPort.mp_MsgList));
    CHECK(list_contains(&parent_et->et_TaskMsgPort.mp_MsgList,
                        (struct Node *)current_et) == 1);
    CHECK(list_count((const struct List *)&current_et->et_Children) == 0);
    CHECK(list_well_formed((const struct List *)&current_et->et_Children));
    CHECK(list_count((const struct List *)&parent_et->et_Children) == 2);
    CHECK(list_well_formed((const struct List *)&parent_et->et_Children));
    CHECK(child1->et_Parent == &parent_task && child2->et_Parent == &parent_task);
    CHECK(list_contains((const struct List *)&parent_et->et_Children,
                        (const struct Node *)child1) == 1);
    CHECK(list_contains((const struct List *)&parent_et->et_Children,
                        (const struct Node *)child2) == 1);
    CHECK(list_contains((const struct List *)&parent_et->et_Children,
                        (const struct Node *)current_et) == 0);
    CHECK(forbid_depth == 0 && lock_depth == 0);
    CHECK(irq_depth == 0u && disable_calls == enable_calls);
    CHECK(disable_calls == 2u && enable_calls == 2u);

    Remove((struct Node *)current_et);
    Remove((struct Node *)child1);
    Remove((struct Node *)child2);
    FreeMem(current_et, sizeof(struct IntETask));
    FreeMem(child1, sizeof(struct IntETask));
    FreeMem(child2, sizeof(struct IntETask));
    FreeMem(parent_et, sizeof(struct IntETask));
}

static void test_orphan_detach_before_expunge(void)
{
    struct IntExecBase sysbase = {{0}, 0};
    struct IntETask *current_storage = new_etask();
    struct ETask *child1 = new_child(NULL);
    struct ETask *child2 = new_child(NULL);
    struct Task current = {0};
    struct ETask *current_et = &current_storage->iet_ETask;

    CHECK(current_storage != NULL && child1 != NULL && child2 != NULL);
    if (failed)
        return;
    init_task(&current, current_et);
    current_et->et_Parent = NULL;
    add_tail((struct List *)&current_et->et_Children, (struct Node *)child1);
    add_tail((struct List *)&current_et->et_Children, (struct Node *)child2);

    current_cleanup_etask = current_et;
    current_cleanup_port = &current_et->et_TaskMsgPort;
    expected_notification_port = NULL;
    check_current_children_empty_on_expunge = 1;
    current_children_empty_observed = 0;
    expunge_count = putmsg_count = 0;
    expected_dead_result_count = 0;
    expected_dead_results_seen = 0;
    /* The worker is itself entered with interrupts already masked. */
    reset_irq_observation(1u);
    Exec_CleanupETask(&current, (struct ExecBase *)&sysbase);

    CHECK(expunge_count == 1);
    CHECK(putmsg_count == 0);
    CHECK(current_children_empty_observed == 1);
    CHECK(child1->et_Parent == NULL && child2->et_Parent == NULL);
    CHECK(forbid_depth == 0 && lock_depth == 0);
    CHECK(irq_depth == 1u && disable_calls == enable_calls);
    CHECK(disable_calls == 1u && enable_calls == 1u);

    FreeMem(child1, sizeof(struct IntETask));
    FreeMem(child2, sizeof(struct IntETask));
}

int main(void)
{
    test_reparent_and_message_detach();
    test_orphan_detach_before_expunge();
    if (failed)
        return 1;
    printf("p4-etask-detach-test: PASS (%u checks)\n", checks);
    return 0;
}
"""


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--artifact-dir", type=Path)
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[4]
    source_path = repo / "rom/exec/exec_util.c"
    source = source_path.read_text(encoding="utf-8")
    function = extract_cleanup(source)
    function_hash = hashlib.sha256(function.encode("utf-8")).hexdigest()
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
        "-D__AROSEXEC_SMP__=1",
    ]
    def build_and_run(temp_path: Path) -> None:
        c_path = temp_path / "etask_detach_fixture.c"
        exe_path = temp_path / "etask_detach_fixture"
        c_path.write_text(
            HARNESS_PREFIX + function + HARNESS_SUFFIX, encoding="utf-8"
        )
        compile_command = command + [str(c_path), "-o", str(exe_path)]
        print("Extracted function SHA-256:", function_hash)
        print("Extracted function bytes:", len(function.encode("utf-8")))
        print("Compile:", shlex.join(compile_command))
        subprocess.run(compile_command, check=True, timeout=30)
        subprocess.run([str(exe_path)], check=True, timeout=20, env=run_env)

        negative_path = temp_path / "etask_detach_fixture_no_irq_gate"
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
        (temp_path / "etask_detach_negative-control.log").write_text(
            negative_text, encoding="utf-8"
        )
        if negative.returncode == 0 or "irq_depth > irq_entry_depth" not in negative_text:
            raise RuntimeError("stripped CleanupETask IRQ gate was not detected")
        print("Exec_CleanupETask stripped-gate negative control: detected as expected")

    if args.artifact_dir is not None:
        args.artifact_dir.mkdir(parents=True, exist_ok=True)
        build_and_run(args.artifact_dir)
    else:
        with tempfile.TemporaryDirectory(prefix="p4-etask-detach-") as temp:
            build_and_run(Path(temp))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
