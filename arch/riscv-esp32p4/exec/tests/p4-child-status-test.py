#!/usr/bin/env python3
"""Compile and exercise the actual ChildStatus function body."""

from __future__ import annotations

import hashlib
import argparse
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile


REPO = Path(__file__).resolve().parents[4]
SOURCE_PATH = REPO / "rom/exec/childstatus.c"


def extract_child_status(source: str) -> str:
    signature = "AROS_LH1(ULONG, ChildStatus,"
    start = source.find(signature)
    if start < 0:
        raise RuntimeError("ChildStatus definition not found")
    marker = "\n{\n    AROS_LIBFUNC_INIT"
    opening = source.find(marker, start)
    if opening < 0:
        raise RuntimeError("ChildStatus function body not found")
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
    raise RuntimeError("ChildStatus closing brace not found")


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

typedef uint32_t ULONG;
typedef uint32_t spinlock_t;
#define TF_ETASK UINT32_C(8)
#define CHILD_NOTNEW UINT32_C(1)
#define CHILD_NOTFOUND UINT32_C(2)
#define CHILD_ACTIVE UINT32_C(3)
#define CHILD_EXITED UINT32_C(4)
#define SPINLOCK_MODE_READ 2u
#define AROS_LH1(type, name, arg1, base_type, base, lvo, library) \
    type name(arg1, base_type base __attribute__((unused)))
#define AROS_LHA(type, name, reg) type name
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
#define IntETask(et) ((struct IntETask *)(et))
#define EXEC_SPINLOCK_LOCK(lock, hook, mode) \
    mock_lock((spinlock_t *)(lock), (hook), (mode))
#define EXEC_SPINLOCK_UNLOCK(lock) mock_unlock((spinlock_t *)(lock))
#define ForeachNode(list, node) \
    for ((node) = (struct ETask *)((struct List *)(list))->lh_Head; \
         mock_foreach_continue((struct List *)(list), (node)); \
         (node) = (struct ETask *)((struct Node *)(node))->ln_Succ)

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
    struct List et_Children;
    struct MsgPort et_TaskMsgPort;
};
struct IntETask
{
    struct ETask iet_ETask;
    spinlock_t iet_TaskLock;
};
struct Task
{
    ULONG tc_Flags;
    union { struct ETask *tc_ETask; void *tc_other; } tc_UnionETask;
};
struct ExecBase { int unused; };

enum event_kind
{
    EV_FORBID = 1,
    EV_ACTIVE_LOCK,
    EV_ACTIVE_UNLOCK,
    EV_RESULT_LOCK,
    EV_RESULT_UNLOCK,
    EV_DISABLE,
    EV_ENABLE,
    EV_PERMIT
};

static unsigned int checks;
static unsigned int forbid_depth;
static unsigned int forbid_calls;
static unsigned int permit_calls;
static unsigned int irq_depth;
static unsigned int irq_entry_depth;
static unsigned int disable_calls;
static unsigned int enable_calls;
static unsigned int lock_depth;
static unsigned int lock_calls;
static unsigned int unlock_calls;
static unsigned int active_scan_probes;
static unsigned int result_scan_probes;
static unsigned int event_count;
static unsigned int events[16];
static int failed;
static struct Task *mock_current_task;
static struct List *expected_active_list;
static struct List *expected_result_list;
static spinlock_t *expected_active_lock;
static spinlock_t *expected_result_lock;
static spinlock_t *held_lock;

#define CHECK(condition) do { \
    ++checks; \
    if (!(condition)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", \
                __FILE__, __LINE__, #condition); \
        failed = 1; \
    } \
} while (0)

static void add_event(unsigned int event)
{
    CHECK(event_count < sizeof(events) / sizeof(events[0]));
    if (event_count < sizeof(events) / sizeof(events[0]))
        events[event_count++] = event;
}

static void init_list(struct List *list)
{
    list->lh_Head = (struct Node *)&list->lh_Tail;
    list->lh_Tail = NULL;
    list->lh_TailPred = (struct Node *)&list->lh_Head;
}

static void add_tail(struct List *list, struct ETask *child)
{
    struct Node *node = (struct Node *)child;
    node->ln_Succ = (struct Node *)&list->lh_Tail;
    node->ln_Pred = list->lh_TailPred;
    list->lh_TailPred->ln_Succ = node;
    list->lh_TailPred = node;
}

static void mock_forbid(void)
{
    CHECK(forbid_depth == 0u);
    ++forbid_depth;
    ++forbid_calls;
    add_event(EV_FORBID);
}

static void mock_permit(void)
{
    CHECK(forbid_depth == 1u);
    CHECK(lock_depth == 0u);
    if (forbid_depth == 1u)
        --forbid_depth;
    ++permit_calls;
    add_event(EV_PERMIT);
}

static void __attribute__((unused)) mock_disable(void)
{
    ++irq_depth;
    ++disable_calls;
    add_event(EV_DISABLE);
}

static void __attribute__((unused)) mock_enable(void)
{
    CHECK(irq_depth > irq_entry_depth);
    CHECK(lock_depth == 0u);
    if (irq_depth > irq_entry_depth)
        --irq_depth;
    ++enable_calls;
    add_event(EV_ENABLE);
}

static void __attribute__((unused)) mock_noop_irq(void) { }

static void __attribute__((unused)) mock_lock(
    spinlock_t *lock,
    void *hook,
    unsigned int mode)
{
    CHECK(forbid_depth == 1u);
    CHECK(lock_depth == 0u);
    CHECK(hook == NULL);
    CHECK(mode == SPINLOCK_MODE_READ);
    if (lock == expected_result_lock)
        CHECK(irq_depth > irq_entry_depth);
    if (lock == expected_active_lock)
        add_event(EV_ACTIVE_LOCK);
    else if (lock == expected_result_lock)
        add_event(EV_RESULT_LOCK);
    else
        CHECK(0 && "unexpected ChildStatus spinlock");
    held_lock = lock;
    ++lock_depth;
    ++lock_calls;
}

static void __attribute__((unused)) mock_unlock(spinlock_t *lock)
{
    CHECK(lock_depth == 1u);
    CHECK(lock == held_lock);
    if (lock == expected_result_lock)
        CHECK(irq_depth > irq_entry_depth);
    if (lock == expected_active_lock)
        add_event(EV_ACTIVE_UNLOCK);
    else if (lock == expected_result_lock)
        add_event(EV_RESULT_UNLOCK);
    else
        CHECK(0 && "unexpected ChildStatus unlock");
    if (lock_depth == 1u)
        --lock_depth;
    held_lock = NULL;
    ++unlock_calls;
}

static int mock_foreach_continue(struct List *list, struct ETask *child)
{
    struct Node *node = (struct Node *)child;
    CHECK(forbid_depth == 1u);
    if (list == expected_active_list)
    {
        ++active_scan_probes;
#if P4_TEST_SMP
        CHECK(lock_depth == 1u && held_lock == expected_active_lock);
#else
        CHECK(lock_depth == 0u);
#endif
    }
    else if (list == expected_result_list)
    {
        ++result_scan_probes;
#if P4_TEST_SMP
        CHECK(lock_depth == 1u && held_lock == expected_result_lock);
        CHECK(irq_depth > irq_entry_depth);
#else
        CHECK(lock_depth == 0u);
#endif
    }
    else
        CHECK(0 && "unexpected ChildStatus list traversal");
    CHECK(node != NULL);
    return node != NULL && node->ln_Succ != NULL;
}

static void setup_owner(
    struct IntETask *owner,
    struct Task *task)
{
    memset(owner, 0, sizeof(*owner));
    memset(task, 0, sizeof(*task));
    init_list(&owner->iet_ETask.et_Children);
    init_list(&owner->iet_ETask.et_TaskMsgPort.mp_MsgList);
    task->tc_Flags = TF_ETASK;
    task->tc_UnionETask.tc_ETask = &owner->iet_ETask;
    mock_current_task = task;
    expected_active_list = &owner->iet_ETask.et_Children;
    expected_result_list = &owner->iet_ETask.et_TaskMsgPort.mp_MsgList;
    expected_active_lock = &owner->iet_TaskLock;
    expected_result_lock = &owner->iet_ETask.et_TaskMsgPort.mp_SpinLock;
}

static void reset_observation(void)
{
    forbid_depth = 0;
    forbid_calls = 0;
    permit_calls = 0;
    irq_depth = 0u;
    irq_entry_depth = 0u;
    disable_calls = 0u;
    enable_calls = 0u;
    lock_depth = 0;
    lock_calls = 0;
    unlock_calls = 0;
    active_scan_probes = 0;
    result_scan_probes = 0;
    event_count = 0;
    held_lock = NULL;
}

static void check_query_balanced(void)
{
    CHECK(forbid_depth == 0u);
    CHECK(lock_depth == 0u);
    CHECK(held_lock == NULL);
    CHECK(irq_depth == irq_entry_depth);
    CHECK(disable_calls == enable_calls);
    CHECK(forbid_calls == 1u);
    CHECK(permit_calls == 1u);
    CHECK(active_scan_probes > 0u);
    CHECK(result_scan_probes > 0u);
#if P4_TEST_SMP
    {
        static const unsigned int expected[] = {
            EV_FORBID, EV_ACTIVE_LOCK, EV_ACTIVE_UNLOCK,
            EV_DISABLE, EV_RESULT_LOCK, EV_RESULT_UNLOCK,
            EV_ENABLE, EV_PERMIT
        };
        unsigned int index;
        CHECK(lock_calls == 2u && unlock_calls == 2u);
        CHECK(event_count == sizeof(expected) / sizeof(expected[0]));
        for (index = 0; index < sizeof(expected) / sizeof(expected[0]); ++index)
            CHECK(events[index] == expected[index]);
    }
#else
    CHECK(lock_calls == 0u && unlock_calls == 0u);
    CHECK(event_count == 2u);
    CHECK(events[0] == EV_FORBID && events[1] == EV_PERMIT);
#endif
}

static void check_no_exec_locking(void)
{
    CHECK(forbid_depth == 0u && forbid_calls == 0u && permit_calls == 0u);
    CHECK(lock_depth == 0u && lock_calls == 0u && unlock_calls == 0u);
    CHECK(irq_depth == irq_entry_depth && disable_calls == enable_calls);
    CHECK(held_lock == NULL && event_count == 0u);
}

ULONG ChildStatus(ULONG tid, struct ExecBase *SysBase __attribute__((unused)));

static ULONG query(ULONG tid)
{
    ULONG status;
    reset_observation();
    status = ChildStatus(tid, NULL);
    check_query_balanced();
    return status;
}

static ULONG query_with_initial_irq_depth(ULONG tid, unsigned int depth)
{
    ULONG status;
    reset_observation();
    irq_depth = depth;
    irq_entry_depth = depth;
    status = ChildStatus(tid, NULL);
    check_query_balanced();
    return status;
}

static unsigned int list_count(const struct List *list)
{
    const struct Node *node = list->lh_Head;
    unsigned int count = 0;
    while (node != (const struct Node *)&list->lh_Tail && count < 16u)
    {
        ++count;
        node = node->ln_Succ;
    }
    CHECK(count < 16u);
    return count;
}

ULONG ChildStatus(ULONG tid, struct ExecBase *SysBase __attribute__((unused)))
"""


HARNESS_SUFFIX = r"""

static void test_null_current_and_non_etask(void)
{
    struct Task ordinary = {0};
    reset_observation();
    mock_current_task = NULL;
    CHECK(ChildStatus(10u, NULL) == CHILD_NOTFOUND);
    check_no_exec_locking();

    reset_observation();
    mock_current_task = &ordinary;
    CHECK(ChildStatus(10u, NULL) == CHILD_NOTNEW);
    check_no_exec_locking();
}

static void test_active_head_middle_tail_and_miss(void)
{
    struct IntETask owner;
    struct Task task;
    struct ETask active[3];

    setup_owner(&owner, &task);
    memset(active, 0, sizeof(active));
    active[0].et_UniqueID = 11u;
    active[1].et_UniqueID = 22u;
    active[2].et_UniqueID = 33u;
    add_tail(&owner.iet_ETask.et_Children, &active[0]);
    add_tail(&owner.iet_ETask.et_Children, &active[1]);
    add_tail(&owner.iet_ETask.et_Children, &active[2]);

    CHECK(query(11u) == CHILD_ACTIVE);
    CHECK(query(22u) == CHILD_ACTIVE);
    CHECK(query(33u) == CHILD_ACTIVE);
    CHECK(query(99u) == CHILD_NOTFOUND);
    CHECK(list_count(&owner.iet_ETask.et_Children) == 3u);
    CHECK(list_count(&owner.iet_ETask.et_TaskMsgPort.mp_MsgList) == 0u);
    CHECK(active[0].et_UniqueID == 11u && active[1].et_UniqueID == 22u &&
          active[2].et_UniqueID == 33u);
}

static void test_retained_dead_match_and_dead_override(void)
{
    struct IntETask owner;
    struct Task task;
    struct ETask active[3];
    struct ETask dead;

    setup_owner(&owner, &task);
    memset(active, 0, sizeof(active));
    memset(&dead, 0, sizeof(dead));
    active[0].et_UniqueID = 11u;
    active[1].et_UniqueID = 22u;
    active[2].et_UniqueID = 33u;
    dead.et_UniqueID = 44u;
    add_tail(&owner.iet_ETask.et_Children, &active[0]);
    add_tail(&owner.iet_ETask.et_Children, &active[1]);
    add_tail(&owner.iet_ETask.et_Children, &active[2]);
    add_tail(&owner.iet_ETask.et_TaskMsgPort.mp_MsgList, &dead);

    CHECK(query(44u) == CHILD_EXITED);
    CHECK(list_count(&owner.iet_ETask.et_TaskMsgPort.mp_MsgList) == 1u);
    CHECK(dead.et_UniqueID == 44u);

    /* The historical result-list scan runs second and overrides ACTIVE. */
    dead.et_UniqueID = 22u;
    CHECK(query(22u) == CHILD_EXITED);
    CHECK(list_count(&owner.iet_ETask.et_Children) == 3u);
    CHECK(list_count(&owner.iet_ETask.et_TaskMsgPort.mp_MsgList) == 1u);
    CHECK(dead.et_UniqueID == 22u);
}

static void test_empty_lists(void)
{
    struct IntETask owner;
    struct Task task;
    setup_owner(&owner, &task);
    CHECK(query(123u) == CHILD_NOTFOUND);
}

static void test_port_gate_preserves_pre_disabled_caller(void)
{
    struct IntETask owner;
    struct Task task;

    setup_owner(&owner, &task);
    CHECK(query_with_initial_irq_depth(123u, 1u) == CHILD_NOTFOUND);
#if P4_TEST_SMP
    CHECK(disable_calls == 1u && enable_calls == 1u);
    CHECK(irq_depth == 1u);
#else
    CHECK(disable_calls == 0u && enable_calls == 0u);
    CHECK(irq_depth == 1u);
#endif
}

int main(void)
{
    test_null_current_and_non_etask();
    test_active_head_middle_tail_and_miss();
    test_retained_dead_match_and_dead_override();
    test_empty_lists();
    test_port_gate_preserves_pre_disabled_caller();
    if (failed)
        return 1;
#if P4_TEST_SMP
    printf("p4-child-status-test SMP: PASS (%u checks)\n", checks);
#else
    printf("p4-child-status-test non-SMP: PASS (%u checks)\n", checks);
#endif
    return 0;
}
"""


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--artifact-dir", type=Path)
    args = parser.parse_args()
    source = SOURCE_PATH.read_text(encoding="utf-8")
    body = extract_child_status(source)
    source_hash = hashlib.sha256(source.encode("utf-8")).hexdigest()
    body_hash = hashlib.sha256(body.encode("utf-8")).hexdigest()
    fixture_hash = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
    compiler = shutil.which(os.environ.get("CC", "clang"))
    if compiler is None:
        raise SystemExit("clang was not found; set CC to a Clang executable")
    run_env = os.environ.copy()
    run_env["ASAN_OPTIONS"] = "detect_leaks=0:halt_on_error=1"
    run_env["UBSAN_OPTIONS"] = "halt_on_error=1"

    common_flags = [
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
    def build_and_run(temp_path: Path) -> None:
        print("ChildStatus source SHA-256:", source_hash)
        print("ChildStatus body SHA-256:", body_hash)
        print("ChildStatus body bytes:", len(body.encode("utf-8")))
        print("Fixture SHA-256:", fixture_hash)
        for mode, mode_name in ((1, "SMP"), (0, "non-SMP")):
            c_path = temp_path / f"child_status_{mode_name.lower()}.c"
            exe_path = temp_path / f"child_status_{mode_name.lower()}"
            c_path.write_text(
                HARNESS_PREFIX + body + HARNESS_SUFFIX, encoding="utf-8"
            )
            command = common_flags + [
                f"-DP4_TEST_SMP={mode}",
                str(c_path),
                "-o",
                str(exe_path),
            ]
            print(f"Compile ({mode_name}):", shlex.join(command))
            subprocess.run(command, check=True, timeout=30)
            subprocess.run([str(exe_path)], check=True, timeout=20, env=run_env)
            if mode:
                negative_path = temp_path / "child_status_smp_no_irq_gate"
                negative_command = common_flags + [
                    "-DP4_TEST_SMP=1",
                    "-DP4_TEST_STRIP_PORT_IRQ_GATE=1",
                    str(c_path),
                    "-o",
                    str(negative_path),
                ]
                print("Compile (SMP stripped IRQ gate):", shlex.join(negative_command))
                subprocess.run(negative_command, check=True, timeout=30)
                negative = subprocess.run(
                    [str(negative_path)], capture_output=True, text=True, timeout=20,
                    env=run_env,
                )
                negative_text = negative.stdout + negative.stderr
                (temp_path / "child_status_negative-control.log").write_text(
                    negative_text, encoding="utf-8"
                )
                if negative.returncode == 0 or "irq_depth > irq_entry_depth" not in negative_text:
                    raise RuntimeError("stripped ChildStatus IRQ gate was not detected")
                print("ChildStatus stripped-gate negative control: detected as expected")

    if args.artifact_dir is not None:
        args.artifact_dir.mkdir(parents=True, exist_ok=True)
        build_and_run(args.artifact_dir)
    else:
        with tempfile.TemporaryDirectory(prefix="p4-child-status-") as temp:
            build_and_run(Path(temp))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
