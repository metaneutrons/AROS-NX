#!/usr/bin/env python3
"""Compile and exercise the actual SMP ChildOrphan function body."""

from __future__ import annotations

import hashlib
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile


def extract_child_orphan(source: str) -> str:
    signature = "AROS_LH1(ULONG, ChildOrphan,"
    start = source.find(signature)
    if start < 0:
        raise RuntimeError("ChildOrphan definition not found")
    marker = "\n{\n    AROS_LIBFUNC_INIT"
    opening = source.find(marker, start)
    if opening < 0:
        raise RuntimeError("ChildOrphan function body not found")
    opening += 1

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
                return source[opening : index + 1]
        index += 1
    raise RuntimeError("ChildOrphan closing brace not found")


HARNESS_PREFIX = r"""
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define __AROSEXEC_SMP__ 1
typedef uint32_t ULONG;
typedef int BOOL;
typedef int spinlock_t;
#define TRUE 1
#define FALSE 0
#define TF_ETASK UINT32_C(8)
#define CHILD_NOTNEW UINT32_C(1)
#define CHILD_NOTFOUND UINT32_C(2)
#define SPINLOCK_MODE_WRITE 1u
#define AROS_LIBFUNC_INIT
#define AROS_LIBFUNC_EXIT
#define GET_THIS_TASK mock_current_task
#define Forbid() mock_forbid()
#define Permit() mock_permit()
#define GetETask(task) mock_get_etask((struct Task *)(task))
#define IntETask(et) ((struct IntETask *)(et))
#define Remove(node) mock_remove((struct Node *)(node))
#define EXEC_SPINLOCK_LOCK(lock, hook, mode) \
    mock_lock((spinlock_t *)(lock), (hook), (mode))
#define EXEC_SPINLOCK_UNLOCK(lock) mock_unlock((spinlock_t *)(lock))
#define ForeachNode(list, node) \
    for ((node) = mock_foreach_first((struct List *)(list)); \
         ((struct Node *)(node))->ln_Succ != NULL; \
         (node) = (struct ETask *)((struct Node *)(node))->ln_Succ)
#define ForeachNodeSafe(list, current, next) \
    for ((current) = mock_foreach_first((struct List *)(list)), \
         (next) = (struct Node *)((struct Node *)(current))->ln_Succ; \
         (next) != NULL; \
         (current) = (struct ETask *)(next), \
         (next) = (struct Node *)((struct Node *)(current))->ln_Succ)
#define FindChild(tid) forbidden_find_child((ULONG)(tid))
#define ExpungeETask(et) forbidden_expunge((struct ETask *)(et))

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
struct MinList
{
    struct Node *mlh_Head;
    struct Node *mlh_Tail;
    struct Node *mlh_TailPred;
};
struct Message { struct Node mn_Node; };
struct MsgPort
{
    struct List mp_MsgList;
    spinlock_t mp_SpinLock;
};
struct Task;
struct ETask
{
    struct Message et_Message;
    struct Task *et_Parent;
    ULONG et_UniqueID;
    struct MinList et_Children;
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

static unsigned int checks;
static unsigned int forbid_depth;
static unsigned int forbid_calls;
static unsigned int permit_calls;
static unsigned int lock_depth;
static unsigned int lock_calls;
static unsigned int unlock_calls;
static unsigned int get_etask_calls;
static unsigned int remove_calls;
static unsigned int find_child_calls;
static unsigned int expunge_calls;
static int failed;
static struct Task *mock_current_task;
static struct ETask *expected_current_etask;
static struct List *expected_active_list;
static spinlock_t *expected_task_lock;
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

static void init_min_list(struct MinList *list)
{
    init_list((struct List *)list);
}

static void add_tail(struct List *list, struct Node *node)
{
    node->ln_Succ = (struct Node *)&list->lh_Tail;
    node->ln_Pred = list->lh_TailPred;
    list->lh_TailPred->ln_Succ = node;
    list->lh_TailPred = node;
}

static struct ETask *mock_foreach_first(struct List *list)
{
    CHECK(forbid_depth != 0u);
    CHECK(lock_depth == 1u);
    CHECK(list == expected_active_list);
    return (struct ETask *)list->lh_Head;
}

static unsigned int list_count(const struct List *list)
{
    const struct Node *node = list->lh_Head;
    unsigned int count = 0u;
    while (node != (const struct Node *)&list->lh_Tail && count < 64u)
    {
        ++count;
        node = node->ln_Succ;
    }
    CHECK(count < 64u);
    return count;
}

static int list_contains(const struct List *list, const struct Node *wanted)
{
    const struct Node *node = list->lh_Head;
    unsigned int count = 0u;
    while (node != (const struct Node *)&list->lh_Tail && count < 64u)
    {
        if (node == wanted)
            return 1;
        ++count;
        node = node->ln_Succ;
    }
    CHECK(count < 64u);
    return 0;
}

static int list_well_formed(const struct List *list)
{
    const struct Node *node = list->lh_Head;
    const struct Node *previous = (const struct Node *)&list->lh_Head;
    unsigned int count = 0u;
    while (node != NULL && node->ln_Succ != NULL && count < 64u)
    {
        if (node->ln_Pred != previous || node->ln_Succ->ln_Pred != node)
            return 0;
        previous = node;
        node = node->ln_Succ;
        ++count;
    }
    if (count >= 64u || node != (const struct Node *)&list->lh_Tail)
        return 0;
    return list->lh_TailPred == previous &&
           ((const struct Node *)&list->lh_Tail)->ln_Pred == previous;
}

static ULONG list_id_at(const struct List *list, unsigned int wanted)
{
    const struct Node *node = list->lh_Head;
    unsigned int index = 0u;
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

static void mock_forbid(void)
{
    ++forbid_depth;
    ++forbid_calls;
}

static void mock_permit(void)
{
    CHECK(forbid_depth != 0u);
    if (forbid_depth != 0u)
        --forbid_depth;
    ++permit_calls;
}

static void mock_lock(spinlock_t *lock, void *hook, unsigned int mode)
{
    CHECK(forbid_depth != 0u);
    CHECK(lock_depth == 0u);
    CHECK(lock == expected_task_lock);
    CHECK(hook == NULL);
    CHECK(mode == SPINLOCK_MODE_WRITE);
    held_lock = lock;
    ++lock_depth;
    ++lock_calls;
}

static void mock_unlock(spinlock_t *lock)
{
    CHECK(lock_depth == 1u);
    CHECK(lock == expected_task_lock && lock == held_lock);
    --lock_depth;
    held_lock = NULL;
    ++unlock_calls;
}

static struct ETask *mock_get_etask(struct Task *task)
{
    ++get_etask_calls;
    CHECK(task != NULL);
    if (task == NULL || (task->tc_Flags & TF_ETASK) == 0u)
        return NULL;
    return task->tc_UnionETask.tc_ETask;
}

static struct Node *mock_remove(struct Node *node)
{
    struct ETask *et = (struct ETask *)node;
    CHECK(forbid_depth != 0u);
    CHECK(lock_depth == 1u && held_lock == expected_task_lock);
    CHECK(list_contains(expected_active_list, node));
    CHECK(et->et_Parent == mock_current_task); /* Remove precedes clear. */
    node->ln_Pred->ln_Succ = node->ln_Succ;
    node->ln_Succ->ln_Pred = node->ln_Pred;
    ++remove_calls;
    return node;
}

struct ETask *forbidden_find_child(ULONG id)
{
    (void)id;
    ++find_child_calls;
    return NULL;
}

void forbidden_expunge(struct ETask *et)
{
    (void)et;
    ++expunge_calls;
}

static void reset_observation(void)
{
    forbid_depth = 0u;
    forbid_calls = 0u;
    permit_calls = 0u;
    lock_depth = 0u;
    lock_calls = 0u;
    unlock_calls = 0u;
    get_etask_calls = 0u;
    remove_calls = 0u;
    find_child_calls = 0u;
    expunge_calls = 0u;
    held_lock = NULL;
}

ULONG ChildOrphan(ULONG tid, struct ExecBase *SysBase __attribute__((unused)))
"""


HARNESS_SUFFIX = r"""

struct Fixture
{
    struct Task owner_task;
    struct IntETask owner;
    struct IntETask active[4];
    struct ETask dead[2];
};

static void fixture_init(struct Fixture *fixture,
                         const ULONG *active_ids,
                         unsigned int active_count)
{
    unsigned int index;
    memset(fixture, 0, sizeof(*fixture));
    fixture->owner_task.tc_Flags = TF_ETASK;
    fixture->owner_task.tc_UnionETask.tc_ETask = &fixture->owner.iet_ETask;
    init_min_list(&fixture->owner.iet_ETask.et_Children);
    init_list(&fixture->owner.iet_ETask.et_TaskMsgPort.mp_MsgList);
    expected_current_etask = &fixture->owner.iet_ETask;
    expected_active_list = (struct List *)&fixture->owner.iet_ETask.et_Children;
    expected_task_lock = &fixture->owner.iet_TaskLock;
    mock_current_task = &fixture->owner_task;

    for (index = 0u; index < active_count; ++index)
    {
        struct ETask *child = &fixture->active[index].iet_ETask;
        child->et_UniqueID = active_ids[index];
        child->et_Parent = &fixture->owner_task;
        init_min_list(&child->et_Children);
        init_list(&child->et_TaskMsgPort.mp_MsgList);
        add_tail(expected_active_list, (struct Node *)child);
    }
    for (index = 0u; index < 2u; ++index)
    {
        fixture->dead[index].et_UniqueID = UINT32_C(900) + index;
        fixture->dead[index].et_Parent = &fixture->owner_task;
        add_tail(&fixture->owner.iet_ETask.et_TaskMsgPort.mp_MsgList,
                 (struct Node *)&fixture->dead[index]);
    }
}

static void assert_balanced(void)
{
    CHECK(forbid_depth == 0u);
    CHECK(lock_depth == 0u);
    CHECK(held_lock == NULL);
    CHECK(forbid_calls == permit_calls);
    CHECK(lock_calls == unlock_calls);
    CHECK(find_child_calls == 0u);
    CHECK(expunge_calls == 0u);
}

static void snapshot_dead(const struct Fixture *fixture,
                          struct List *header,
                          struct ETask dead_snapshot[2])
{
    *header = fixture->owner.iet_ETask.et_TaskMsgPort.mp_MsgList;
    memcpy(dead_snapshot, fixture->dead, sizeof(fixture->dead));
}

static void check_dead_unchanged(const struct Fixture *fixture,
                                 const struct List *header,
                                 const struct ETask dead_snapshot[2])
{
    const struct List *list =
        &fixture->owner.iet_ETask.et_TaskMsgPort.mp_MsgList;
    CHECK(list->lh_Head == header->lh_Head);
    CHECK(list->lh_Tail == header->lh_Tail);
    CHECK(list->lh_TailPred == header->lh_TailPred);
    CHECK(memcmp(dead_snapshot, fixture->dead,
                 sizeof(fixture->dead)) == 0);
    CHECK(list_well_formed(list));
    CHECK(list_count(list) == 2u);
    CHECK(list_id_at(list, 0u) == 900u);
    CHECK(list_id_at(list, 1u) == 901u);
}

static void test_null_and_non_etask_current(void)
{
    struct Fixture fixture;
    const ULONG ids[] = {10u};
    struct Task ordinary;

    fixture_init(&fixture, ids, 1u);
    reset_observation();
    mock_current_task = NULL;
    CHECK(ChildOrphan(10u, NULL) == 0u);
    CHECK(get_etask_calls == 0u && lock_calls == 0u && remove_calls == 0u);
    assert_balanced();
    CHECK(list_count(expected_active_list) == 1u);

    memset(&ordinary, 0, sizeof(ordinary));
    ordinary.tc_UnionETask.tc_other = (void *)(uintptr_t)1u;
    reset_observation();
    mock_current_task = &ordinary;
    CHECK(ChildOrphan(10u, NULL) == CHILD_NOTNEW);
    CHECK(get_etask_calls == 1u && lock_calls == 0u && remove_calls == 0u);
    assert_balanced();

    reset_observation();
    fixture.owner_task.tc_UnionETask.tc_ETask = NULL;
    mock_current_task = &fixture.owner_task;
    CHECK(ChildOrphan(10u, NULL) == CHILD_NOTNEW);
    CHECK(get_etask_calls == 1u && lock_calls == 0u && remove_calls == 0u);
    assert_balanced();
}

static void test_orphan_all_removes_before_parent_clear(void)
{
    struct Fixture fixture;
    const ULONG ids[] = {10u, 20u, 30u, 40u};
    struct List dead_header;
    struct ETask dead_snapshot[2];
    unsigned int index;
    fixture_init(&fixture, ids, 4u);
    snapshot_dead(&fixture, &dead_header, dead_snapshot);
    reset_observation();

    CHECK(ChildOrphan(0u, NULL) == 0u);
    CHECK(remove_calls == 4u);
    CHECK(list_count(expected_active_list) == 0u);
    CHECK(list_well_formed(expected_active_list));
    for (index = 0u; index < 4u; ++index)
    {
        struct ETask *child = &fixture.active[index].iet_ETask;
        CHECK(child->et_Parent == NULL);
        CHECK(!list_contains(expected_active_list, (struct Node *)child));
    }
    check_dead_unchanged(&fixture, &dead_header, dead_snapshot);
    assert_balanced();

    /* Empty wildcard orphaning remains a successful no-op. */
    reset_observation();
    CHECK(ChildOrphan(0u, NULL) == 0u);
    CHECK(remove_calls == 0u && list_count(expected_active_list) == 0u);
    check_dead_unchanged(&fixture, &dead_header, dead_snapshot);
    assert_balanced();
}

static void test_specific_head_middle_tail(void)
{
    struct Fixture fixture;
    const ULONG ids[] = {11u, 22u, 33u, 44u};
    struct List dead_header;
    struct ETask dead_snapshot[2];
    fixture_init(&fixture, ids, 4u);
    snapshot_dead(&fixture, &dead_header, dead_snapshot);

    reset_observation();
    CHECK(ChildOrphan(11u, NULL) == 0u);
    CHECK(remove_calls == 1u);
    CHECK(fixture.active[0].iet_ETask.et_Parent == NULL);
    CHECK(!list_contains(expected_active_list,
                         (struct Node *)&fixture.active[0].iet_ETask));
    CHECK(list_count(expected_active_list) == 3u);
    CHECK(list_id_at(expected_active_list, 0u) == 22u);
    CHECK(list_id_at(expected_active_list, 1u) == 33u);
    CHECK(list_id_at(expected_active_list, 2u) == 44u);
    CHECK(list_well_formed(expected_active_list));
    check_dead_unchanged(&fixture, &dead_header, dead_snapshot);
    assert_balanced();

    reset_observation();
    CHECK(ChildOrphan(33u, NULL) == 0u);
    CHECK(remove_calls == 1u);
    CHECK(fixture.active[2].iet_ETask.et_Parent == NULL);
    CHECK(!list_contains(expected_active_list,
                         (struct Node *)&fixture.active[2].iet_ETask));
    CHECK(list_count(expected_active_list) == 2u);
    CHECK(list_id_at(expected_active_list, 0u) == 22u);
    CHECK(list_id_at(expected_active_list, 1u) == 44u);
    CHECK(fixture.active[1].iet_ETask.et_Parent == &fixture.owner_task);
    CHECK(fixture.active[3].iet_ETask.et_Parent == &fixture.owner_task);
    CHECK(list_well_formed(expected_active_list));
    check_dead_unchanged(&fixture, &dead_header, dead_snapshot);
    assert_balanced();

    reset_observation();
    CHECK(ChildOrphan(44u, NULL) == 0u);
    CHECK(remove_calls == 1u);
    CHECK(fixture.active[3].iet_ETask.et_Parent == NULL);
    CHECK(!list_contains(expected_active_list,
                         (struct Node *)&fixture.active[3].iet_ETask));
    CHECK(list_count(expected_active_list) == 1u);
    CHECK(list_id_at(expected_active_list, 0u) == 22u);
    CHECK(fixture.active[1].iet_ETask.et_Parent == &fixture.owner_task);
    CHECK(list_well_formed(expected_active_list));
    check_dead_unchanged(&fixture, &dead_header, dead_snapshot);
    assert_balanced();
}

static void test_dead_result_and_missing_ids_are_not_found(void)
{
    struct Fixture fixture;
    const ULONG ids[] = {17u, 29u};
    struct List dead_header;
    struct ETask dead_snapshot[2];
    fixture_init(&fixture, ids, 2u);
    snapshot_dead(&fixture, &dead_header, dead_snapshot);

    reset_observation();
    CHECK(ChildOrphan(900u, NULL) == CHILD_NOTFOUND);
    CHECK(remove_calls == 0u);
    CHECK(list_count(expected_active_list) == 2u);
    CHECK(fixture.active[0].iet_ETask.et_Parent == &fixture.owner_task);
    CHECK(fixture.active[1].iet_ETask.et_Parent == &fixture.owner_task);
    check_dead_unchanged(&fixture, &dead_header, dead_snapshot);
    assert_balanced();

    reset_observation();
    CHECK(ChildOrphan(777u, NULL) == CHILD_NOTFOUND);
    CHECK(remove_calls == 0u);
    CHECK(list_count(expected_active_list) == 2u);
    check_dead_unchanged(&fixture, &dead_header, dead_snapshot);
    assert_balanced();
}

int main(void)
{
    test_null_and_non_etask_current();
    test_orphan_all_removes_before_parent_clear();
    test_specific_head_middle_tail();
    test_dead_result_and_missing_ids_are_not_found();
    if (failed)
        return 1;
    printf("p4-child-orphan-test: PASS (%u checks)\n", checks);
    return 0;
}
"""


def main() -> int:
    repo = Path(__file__).resolve().parents[4]
    source_path = repo / "rom/exec/childorphan.c"
    source = source_path.read_text(encoding="utf-8")
    function = extract_child_orphan(source)
    function_hash = hashlib.sha256(function.encode("utf-8")).hexdigest()
    test_hash = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
    compiler = shutil.which(os.environ.get("CC", "clang"))
    if compiler is None:
        raise SystemExit("clang was not found; set CC to a Clang executable")

    with tempfile.TemporaryDirectory(prefix="p4-child-orphan-") as temp:
        temp_path = Path(temp)
        c_path = temp_path / "child_orphan_fixture.c"
        exe_path = temp_path / "child_orphan_fixture"
        c_path.write_text(
            HARNESS_PREFIX + function + HARNESS_SUFFIX, encoding="utf-8"
        )
        command = [
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
            str(c_path),
            "-o",
            str(exe_path),
        ]
        print("ChildOrphan body SHA-256:", function_hash)
        print("ChildOrphan body bytes:", len(function.encode("utf-8")))
        print("Fixture SHA-256:", test_hash)
        print("Compile:", shlex.join(command))
        subprocess.run(command, check=True)
        subprocess.run([str(exe_path)], check=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
