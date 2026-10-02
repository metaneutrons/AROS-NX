#!/usr/bin/env python3
"""Host-compose the selected TaskResource cleanup callback and P4 consumer."""

from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[3]


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def extract_function(source: str, name: str) -> str:
    match = re.search(r"(?m)^[A-Za-z_][A-Za-z0-9_ *]+\b" + re.escape(name) + r"\s*\(", source)
    if not match:
        raise AssertionError(f"function not found: {name}")
    brace = source.find("{", match.end())
    if brace < 0:
        raise AssertionError(f"function body not found: {name}")

    depth = 0
    state = "code"
    escaped = False
    i = brace
    while i < len(source):
        ch = source[i]
        nxt = source[i + 1] if i + 1 < len(source) else ""
        if state == "code":
            if ch == "/" and nxt == "*":
                state = "block-comment"
                i += 1
            elif ch == "/" and nxt == "/":
                state = "line-comment"
                i += 1
            elif ch == '"':
                state = "string"
            elif ch == "'":
                state = "char"
            elif ch == "{":
                depth += 1
            elif ch == "}":
                depth -= 1
                if depth == 0:
                    return source[match.start() : i + 1]
        elif state == "block-comment":
            if ch == "*" and nxt == "/":
                state = "code"
                i += 1
        elif state == "line-comment":
            if ch == "\n":
                state = "code"
        elif state in ("string", "char"):
            if escaped:
                escaped = False
            elif ch == "\\":
                escaped = True
            elif (state == "string" and ch == '"') or (state == "char" and ch == "'"):
                state = "code"
        i += 1
    raise AssertionError(f"unterminated function body: {name}")


def source_contract_checks(paths: dict[str, Path]) -> int:
    checks = 0

    def require(condition: bool, description: str) -> None:
        nonlocal checks
        if not condition:
            raise AssertionError(description)
        checks += 1

    interface = paths["interface"].read_text()
    intern = paths["intern"].read_text()
    init = paths["init"].read_text()
    bootstrap_source = paths["bootstrap"].read_text()
    bootstrap = extract_function(bootstrap_source, "task_BootstrapInventory")
    commit = extract_function(bootstrap_source, "commit_inventory")
    platform = paths["platform"].read_text()
    execfuncs = paths["execfuncs"].read_text()
    newaddtask = paths["newaddtask"].read_text()
    cleanup = paths["consumer"].read_text()
    callback = extract_function(paths["support"].read_text(), "task_CleanupRemovedTask")

    library_at = interface.index("struct Library library;")
    contract_at = interface.index("ULONG contract;", library_at)
    callback_at = interface.index("BOOL (*cleanup)(struct Task *, APTR);", contract_at)
    prelaunch_at = interface.index("BOOL (*prelaunch)(struct Task *, APTR);", callback_at)
    require(library_at < contract_at < callback_at < prelaunch_at,
            "cleanup/prelaunch binding ABI members are reordered")

    selected_field = intern.index("struct taskresource_cleanup_binding trb_CleanupBinding;")
    later_field = intern.index("APTR                        trb_KernelBase;", selected_field)
    normal_field = intern.index("struct Library              trb_LibNode;")
    taskres_if = intern.rfind("#if defined(EXEC_PLATFORM_TASKRESOURCE_SERVICE_CLEANUP)", 0, selected_field)
    taskres_else = intern.index("#else", selected_field)
    taskres_endif = intern.index("#endif", normal_field)
    require(taskres_if < selected_field < taskres_else < normal_field < taskres_endif < later_field,
            "TaskResBase selected first-member binding or normal first member changed")

    init_semaphore = init.index("InitSemaphore(&TaskResBase->trb_Sem);")
    slot_alloc = init.index("AllocMem(sizeof(struct TaskStorageFreeSlot)")
    binding_contract = init.index("TaskResBase->trb_CleanupBinding.contract =")
    binding_callback = init.index("TaskResBase->trb_CleanupBinding.cleanup =")
    binding_prelaunch = init.index("TaskResBase->trb_CleanupBinding.prelaunch =")
    bootstrap_call = init.index("if (!task_BootstrapInventory(TaskResBase))")
    require(init_semaphore < slot_alloc < binding_contract < binding_callback < binding_prelaunch < bootstrap_call,
            "fallible setup/binding initialization no longer precedes base publication")
    count = commit.index("count_inventory(&count)")
    capacity_refusal = commit.index("count > transaction->capacity")
    first_copy = commit.index("take_list(&PrivExecBase(SysBase)->TaskRunning")
    last_copy = commit.index("take_list(&SysBase->TaskWait")
    release_publish = commit.index("__atomic_store_n")
    success = commit.index("return 1;", release_publish)
    require(count < capacity_refusal < first_copy < last_copy < release_publish < success and
            "__ATOMIC_RELEASE" in commit[release_publish:success],
            "selected final callback does not validate capacity, copy all four inventories, then release-publish")
    require("return 0;" in commit[:first_copy] and "return 2;" in commit[:first_copy] and
            "return 0;" not in commit[release_publish:success] and
            "return 2;" not in commit[release_publish:success],
            "final callback refusal/retry does not precede all mutations or publication")
    transaction_call = bootstrap.index("EXEC_PLATFORM_TASKRESOURCE_BOOTSTRAP_TRANSACTION(")
    final_disable = bootstrap.rfind("Disable();", 0, transaction_call)
    final_enable = bootstrap.index("Enable();", transaction_call)
    require(final_disable >= 0 and final_disable < transaction_call < final_enable and
            "commit_inventory, &transaction" in bootstrap[transaction_call:final_enable],
            "selected final callback is not inside the platform transaction's caller IRQ bracket")
    require("EXEC_PLATFORM_TASKRESOURCE_BOOTSTRAP_TRANSACTION" in platform and
            "krnP4TaskResourceBootstrapTransaction(SysBase" in platform,
            "platform does not route the final callback through the lifetime-gated transaction")
    failed_init_end = init.index("\n    }", bootstrap_call)
    failure_branch = init[bootstrap_call:failed_init_end]
    require("Remove((struct Node *)freeTaskStorageSlot)" in failure_branch and
            "FreeMem(freeTaskStorageSlot" in failure_branch and
            "return FALSE;" in failure_branch,
            "unpublished bootstrap failure no longer balances initial slot allocation")
    selected_end = init.index("#else", failed_init_end)
    require("return TRUE;" in init[failed_init_end:selected_end] and
            "return FALSE;" not in init[failed_init_end:selected_end],
            "selected initializer can fail after a successful bootstrap publication")
    vector_if = init.index("#if !defined(EXEC_PLATFORM_TASKRESOURCE_SERVICE_CLEANUP)", binding_prelaunch)
    vector_end = init.index("#endif /* Legacy vector interposition; selected P4 uses direct core callbacks. */", vector_if)
    require("SetFunction(" in init[vector_if:vector_end] and bootstrap_call > vector_end,
            "selected P4 still patches public vectors or legacy vector installation disappeared")

    exit_body = extract_function(init, "taskres_Exit")
    published_guard = exit_body.index("__ATOMIC_LOAD_N") if "__ATOMIC_LOAD_N" in exit_body else exit_body.index("__atomic_load_n")
    refuses = exit_body.index("return FALSE;", published_guard)
    restores = exit_body.index("SetFunction(", refuses)
    clears = exit_body.index("SysBase->lb_TaskResBase = NULL;", restores)
    require(published_guard < refuses < restores < clears,
            "published resource expunge does not refuse before vector/base teardown")

    remtask_start = execfuncs.index("AROS_LH1(void, RemTask,")
    remtask = execfuncs[remtask_start:]
    selected = remtask.index("#if defined(EXEC_PLATFORM_TASKRESOURCE_SERVICE_CLEANUP)")
    legacy = remtask.index("#else", selected)
    selected_branch = remtask[selected:legacy]
    legacy_branch = remtask[legacy:]
    require("AROS_CALL1(void, TaskResBase->trb_RemTask" in selected_branch and
            "findTask, A1" in selected_branch and "task_DetachEntry" not in selected_branch and
            "tc_UnionETask" not in selected_branch,
            "selected RemTask wrapper no longer forwards opaque identity before target access")
    require("task_DetachEntry(findTask, TaskResBase);" in legacy_branch and
            "findTask->tc_UnionETask.tc_ETask" in legacy_branch,
            "legacy/normal RemTask path was removed instead of retained")
    registration = extract_function(execfuncs, "task_RegisterNewTask")
    require("BOOL task_RegisterNewTask" in registration and
            "MEMF_PUBLIC | MEMF_CLEAR | MEMF_NO_EXPUNGE" in registration and
            "EXEC_SPINLOCK_LOCK(&TaskResBase->TaskListSpinLock" in registration and
            "CALLHOOKPKT" not in registration,
            "direct prelaunch registration no longer reports allocation success under the topology gate")
    launch = extract_function(newaddtask, "TaskLaunch")
    prelaunch_call = launch.index("if (!resource->prelaunch(task, resource))")
    refusal_fatal = launch.index("EXEC_PLATFORM_TASK_FATAL();", prelaunch_call)
    user_hook_call = launch.index("CALLHOOKPKT(plHook, task, 0);")
    ready_publish = launch.index("EXEC_PLATFORM_NEW_TASK_READY(task)")
    require(prelaunch_call < refusal_fatal < user_hook_call < ready_publish,
            "registration refusal no longer fails closed before user hook and Ready publication")

    claim_at = cleanup.index("krnP4TaskRecordClaimNextRetainedService(SysBase)")
    task_state_at = cleanup.index("task->tc_State", claim_at)
    resource_at = cleanup.index("&SysBase->lb_TaskResBase", task_state_at)
    etask_at = cleanup.index("et = GetETask(task);", resource_at)
    require(claim_at < task_state_at < resource_at < etask_at,
            "cleanup consumer target/resource access moved ahead of retained claim")

    clear = re.search(r"et->et_Reserved\[ETASK_RSVD_SLOTID\]\s*=\s*NULL\s*;", callback)
    free = re.search(r"FreeMem\(storage\s*,", callback)
    require(clear is not None and free is not None and clear.start() < free.start(),
            "TaskResource storage pointer is not cleared before freeing its allocation")
    assert clear is not None and free is not None
    clear_end = callback.find(";", clear.start()) + 1
    free_end = callback.find(";", free.start()) + 1
    bad_order = callback[: clear.start()] + callback[free.start() : free_end] + "\n" + callback[clear.start() : clear_end] + callback[free_end:]
    bad_clear = re.search(r"et->et_Reserved\[ETASK_RSVD_SLOTID\]\s*=\s*NULL\s*;", bad_order)
    bad_free = re.search(r"FreeMem\(storage\s*,", bad_order)
    if bad_clear is None or bad_free is None or bad_clear.start() < bad_free.start():
        raise AssertionError("clear-after-free negative control unexpectedly passed")
    checks += 1
    return checks


HARNESS = r'''#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EXEC_PLATFORM_TASKRESOURCE_SERVICE_CLEANUP 1
#define __AROSEXEC_SMP__ 1
#define __AROSPLATFORM_SMP__ 1
#define TRUE 1
#define FALSE 0
#define TS_REMOVED UINT32_C(0x00000001)
#define TS_INVALID UINT32_C(0x00000002)
#define TS_TOMBSTONED UINT32_C(0x00000004)
#define TF_ETASK UINT32_C(0x00000008)
#define ETASK_RSVD_SLOTID 1
#define __TS_FIRSTSLOT 0
#define TASKRESOURCE_CLEANUP_CONTRACT UINT32_C(0x50345233)
#define SPINLOCK_MODE_WRITE 1
#define P4_PUBLIC_SPIN_BAD_STATE 1
#define D(x) ((void)0)

typedef uint32_t ULONG;
typedef uintptr_t IPTR;
typedef void *APTR;
typedef int BOOL;

struct Node { struct Node *ln_Succ; struct Node *ln_Pred; };
struct List { struct Node *lh_Head; struct Node *lh_Tail; struct Node *lh_TailPred; };
struct MinList { struct Node *lh_Head; struct Node *lh_Tail; struct Node *lh_TailPred; };
struct Library { uintptr_t dummy; };
struct Task;
struct taskresource_cleanup_binding {
    struct Library library;
    ULONG contract;
    BOOL (*cleanup)(struct Task *, APTR);
    BOOL (*prelaunch)(struct Task *, APTR);
};
struct ETask { void *et_RegFrame; void *et_Reserved[2]; };
struct MemList { struct Node ml_Node; unsigned marker; };
struct Task {
    uint32_t tc_State;
    union { struct ETask *tc_ETask; } tc_UnionETask;
    uint32_t tc_Flags;
    struct List tc_MemEntry;
};
struct TaskListEntry { struct Node tle_Node; struct Task *tle_Task; struct List tle_HookTypes; };
struct TaskResBase {
    struct taskresource_cleanup_binding trb_CleanupBinding;
    struct List trb_TaskList;
    struct List trb_NewTasks;
    struct List trb_LockedLists;
    uintptr_t TaskListSpinLock;
};
struct p4_task_registry_record { struct Task *identity; };
struct ExecBase { struct Library *lb_TaskResBase; };

static unsigned checks;
#define CHECK(expression) do { \
    if (!(expression)) { \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #expression); \
        exit(EXIT_FAILURE); \
    } \
    ++checks; \
} while (0)

enum {
    EV_CLAIM = 1, EV_DISABLE, EV_GATE_LOCK, EV_GATE_UNLOCK, EV_ENABLE,
    EV_DETACHED_ENTRY_FREE, EV_GET_ETASK, EV_STORAGE_FREE, EV_CONTEXT,
    EV_ETASK_CLEANUP, EV_ENTRY0_FREE, EV_ENTRY1_FREE, EV_FINISH,
    EV_RECORD_FREE, EV_FATAL, EV_RETRY, EV_TRY_FINISH, EV_PRESSURE
};
static unsigned events[128];
static unsigned event_count;
static unsigned gate_depth;
static unsigned irq_depth;
static unsigned get_etask_calls;
static unsigned claim_calls;
static unsigned retry_calls;
static unsigned tryfinish_calls;
static unsigned finish_calls;
static unsigned fatal_calls;
static unsigned storage_free_calls;
static unsigned detached_entry_free_calls;
static unsigned free_entry_calls;
static unsigned context_calls;
static unsigned cleanup_etask_calls;
static int task_poisoned;
static struct Task *current_task;
static struct ETask *get_etask_result;
static IPTR storage_area[2];
static IPTR *current_storage;
static struct TaskListEntry *current_detached_entry;
static struct MemList *expected_entries[2];
static struct p4_task_registry_record cleanup_record;
static struct p4_task_registry_record *claim_result;
static struct p4_task_registry_record retry_record;
static struct p4_task_registry_record *retry_result;
static struct p4_task_registry_record *retry_owned_record;
static int try_finish_result;
static struct ExecBase exec_base;
static struct TaskResBase taskres;
static struct TaskListEntry taskres_entry;
static struct Task task_object;
static struct ETask etask_object;
static struct MemList mem_entries[2];

BOOL task_CleanupRemovedTask(struct Task *, APTR);
void task_DetachEntry(struct Task *, struct TaskResBase *);
int krnP4CleanupOneRetiredTask(struct ExecBase *);
static struct p4_task_registry_record *krnP4TaskRecordRetryPostFree(struct ExecBase *);
static BOOL krnP4TaskRecordTryFinishPostFree(struct p4_task_registry_record *, struct ExecBase *);

static void event(unsigned kind)
{
    CHECK(event_count < (unsigned)(sizeof(events) / sizeof(events[0])));
    events[event_count++] = kind;
}

static unsigned event_index(unsigned kind, unsigned occurrence)
{
    unsigned i;
    for (i = 0; i < event_count; ++i)
        if (events[i] == kind && occurrence-- == 0u)
            return i;
    return UINT_MAX;
}

static void list_init(struct List *list)
{
    list->lh_Head = (struct Node *)&list->lh_Tail;
    list->lh_Tail = NULL;
    list->lh_TailPred = (struct Node *)list;
}

static void list_add_tail(struct List *list, struct Node *node)
{
    struct Node *tail_sentinel = (struct Node *)&list->lh_Tail;
    node->ln_Succ = tail_sentinel;
    node->ln_Pred = list->lh_TailPred;
    list->lh_TailPred->ln_Succ = node;
    list->lh_TailPred = node;
}

static void list_remove(struct Node *node)
{
    node->ln_Pred->ln_Succ = node->ln_Succ;
    node->ln_Succ->ln_Pred = node->ln_Pred;
}

static struct Node *list_rem_head(struct List *list)
{
    struct Node *node = list->lh_Head;
    if (node->ln_Succ == NULL)
        return NULL;
    list_remove(node);
    return node;
}

#define NEWLIST(list) list_init((list))
#define RemHead(list) list_rem_head((list))
#define AddTail(list, node) list_add_tail((list), (node))
#define Remove(node) list_remove((struct Node *)(node))
#define IsListEmpty(list) ((list)->lh_Head == (struct Node *)&(list)->lh_Tail)
#define ForeachNodeSafe(list, entry, next) \
    for ((entry) = (struct TaskListEntry *)(list)->lh_Head; \
         (entry)->tle_Node.ln_Succ != NULL && \
             (((next) = (struct TaskListEntry *)(entry)->tle_Node.ln_Succ), 1); \
         (entry) = (next))

static void mock_disable(void)
{
    CHECK(gate_depth == 0u);
    CHECK(irq_depth == 0u);
    ++irq_depth;
    event(EV_DISABLE);
}
static void mock_enable(void)
{
    CHECK(gate_depth == 0u);
    CHECK(irq_depth == 1u);
    --irq_depth;
    event(EV_ENABLE);
}
static void mock_gate_lock(uintptr_t *lock, void *ctx, int mode)
{
    (void)ctx;
    CHECK(lock == &taskres.TaskListSpinLock);
    CHECK(mode == SPINLOCK_MODE_WRITE);
    CHECK(gate_depth == 0u && irq_depth == 1u);
    ++gate_depth;
    event(EV_GATE_LOCK);
}
static void mock_gate_unlock(uintptr_t *lock)
{
    CHECK(lock == &taskres.TaskListSpinLock);
    CHECK(gate_depth == 1u && irq_depth == 1u);
    --gate_depth;
    event(EV_GATE_UNLOCK);
}
#define Disable() mock_disable()
#define Enable() mock_enable()
#define EXEC_SPINLOCK_LOCK(lock, ctx, mode) mock_gate_lock((lock), (ctx), (mode))
#define EXEC_SPINLOCK_UNLOCK(lock) mock_gate_unlock((lock))

static struct ETask *GetETask(struct Task *task)
{
    CHECK(!task_poisoned);
    CHECK(task == current_task);
    ++get_etask_calls;
    event(EV_GET_ETASK);
    return get_etask_result;
}
static void KrnDeleteContext(void *context)
{
    CHECK(!task_poisoned);
    CHECK(get_etask_result != NULL && context == get_etask_result->et_RegFrame);
    ++context_calls;
    event(EV_CONTEXT);
}
static void CleanupETask(struct Task *task)
{
    CHECK(!task_poisoned);
    CHECK(task == current_task);
    ++cleanup_etask_calls;
    event(EV_ETASK_CLEANUP);
}
static void FreeMem(void *address, ULONG size)
{
    CHECK(gate_depth == 0u && irq_depth == 0u);
    if (address == (void *)current_detached_entry) {
        CHECK(!task_poisoned);
        CHECK(size == (ULONG)sizeof(*current_detached_entry));
        ++detached_entry_free_calls;
        event(EV_DETACHED_ENTRY_FREE);
        return;
    }
    if (address == (void *)current_storage) {
        CHECK(!task_poisoned);
        CHECK(get_etask_result != NULL);
        CHECK(get_etask_result->et_Reserved[ETASK_RSVD_SLOTID] == NULL);
        CHECK(size == (ULONG)(storage_area[__TS_FIRSTSLOT] * sizeof(*current_storage)));
        ++storage_free_calls;
        event(EV_STORAGE_FREE);
        return;
    }
    if (address == (void *)&cleanup_record) {
        CHECK(finish_calls == 1u || retry_owned_record == &cleanup_record);
        event(EV_RECORD_FREE);
        if (retry_owned_record == &cleanup_record)
            retry_owned_record = NULL;
        return;
    }
    if (address == (void *)retry_owned_record) {
        CHECK(retry_owned_record != NULL);
        retry_owned_record = NULL;
        event(EV_RECORD_FREE);
        return;
    }
    CHECK(0 && "unexpected FreeMem pointer");
}
static void FreeEntry(struct MemList *entry)
{
    CHECK(gate_depth == 0u && irq_depth == 0u);
    if (entry == expected_entries[0]) {
        CHECK(free_entry_calls == 0u);
        ++free_entry_calls;
        event(EV_ENTRY0_FREE);
    } else if (entry == expected_entries[1]) {
        CHECK(free_entry_calls == 1u);
        ++free_entry_calls;
        event(EV_ENTRY1_FREE);
    } else {
        CHECK(0 && "unexpected MemList entry");
    }
    /* The next operation may use only the detached housekeeper list. */
    task_poisoned = 1;
}
static struct p4_task_registry_record *
krnP4TaskRecordClaimNextRetainedService(struct ExecBase *base)
{
    CHECK(base == &exec_base);
    ++claim_calls;
    event(EV_CLAIM);
    return claim_result;
}
static struct p4_task_registry_record *krnP4TaskRecordRetryPostFree(struct ExecBase *base)
{
    struct p4_task_registry_record *result;
    CHECK(base == &exec_base);
    ++retry_calls;
    event(EV_RETRY);
    result = retry_result;
    retry_result = NULL;
    retry_owned_record = result;
    return result;
}
static BOOL krnP4TaskRecordTryFinishPostFree(
    struct p4_task_registry_record *record, struct ExecBase *base)
{
    CHECK(record == &cleanup_record && base == &exec_base);
    CHECK(gate_depth == 0u && irq_depth == 0u);
    CHECK(free_entry_calls == 2u);
    ++tryfinish_calls;
    event(EV_TRY_FINISH);
    if (try_finish_result) {
        ++finish_calls;
        event(EV_FINISH);
        return TRUE;
    }
    event(EV_PRESSURE);
    return FALSE;
}
static void krnP4PublicSpinFatal(unsigned reason)
{
    CHECK(reason == P4_PUBLIC_SPIN_BAD_STATE);
    ++fatal_calls;
    event(EV_FATAL);
}

static void reset_fixture(void)
{
    memset(events, 0, sizeof(events));
    event_count = 0u;
    gate_depth = 0u;
    irq_depth = 0u;
    get_etask_calls = 0u;
    claim_calls = 0u;
    retry_calls = 0u;
    tryfinish_calls = 0u;
    finish_calls = 0u;
    fatal_calls = 0u;
    storage_free_calls = 0u;
    detached_entry_free_calls = 0u;
    free_entry_calls = 0u;
    context_calls = 0u;
    cleanup_etask_calls = 0u;
    task_poisoned = 0;
    current_task = &task_object;
    get_etask_result = &etask_object;
    current_storage = storage_area;
    current_detached_entry = &taskres_entry;
    expected_entries[0] = &mem_entries[0];
    expected_entries[1] = &mem_entries[1];
    memset(&taskres, 0, sizeof(taskres));
    memset(&task_object, 0, sizeof(task_object));
    memset(&etask_object, 0, sizeof(etask_object));
    memset(&taskres_entry, 0, sizeof(taskres_entry));
    memset(mem_entries, 0, sizeof(mem_entries));
    memset(storage_area, 0, sizeof(storage_area));
    memset(&cleanup_record, 0, sizeof(cleanup_record));
    memset(&retry_record, 0, sizeof(retry_record));
    list_init(&taskres.trb_TaskList);
    list_init(&taskres.trb_NewTasks);
    list_init(&taskres.trb_LockedLists);
    list_init(&task_object.tc_MemEntry);
    task_object.tc_State = TS_REMOVED;
    task_object.tc_Flags = TF_ETASK;
    task_object.tc_UnionETask.tc_ETask = &etask_object;
    etask_object.et_RegFrame = (void *)(uintptr_t)0x1234u;
    storage_area[0] = 2u;
    etask_object.et_Reserved[ETASK_RSVD_SLOTID] = storage_area;
    taskres.trb_CleanupBinding.contract = TASKRESOURCE_CLEANUP_CONTRACT;
    taskres.trb_CleanupBinding.cleanup = task_CleanupRemovedTask;
    exec_base.lb_TaskResBase = (struct Library *)&taskres;
    taskres_entry.tle_Task = &task_object;
    list_add_tail(&taskres.trb_NewTasks, &taskres_entry.tle_Node);
    mem_entries[0].marker = 10u;
    mem_entries[1].marker = 20u;
    list_add_tail(&task_object.tc_MemEntry, &mem_entries[0].ml_Node);
    list_add_tail(&task_object.tc_MemEntry, &mem_entries[1].ml_Node);
    cleanup_record.identity = &task_object;
    claim_result = &cleanup_record;
    retry_result = NULL;
    retry_owned_record = NULL;
    try_finish_result = 1;
}

static void test_successful_order(void)
{
    int result;
    unsigned retry, claim, detached, get0, storage, get1, context, cleanup, free0, free1, tryfinish, finish, recfree;
    reset_fixture();
    result = krnP4CleanupOneRetiredTask(&exec_base);
    CHECK(result == 1);
    CHECK(claim_calls == 1u && finish_calls == 1u && fatal_calls == 0u);
    CHECK(retry_calls == 1u && tryfinish_calls == 1u);
    CHECK(detached_entry_free_calls == 1u && storage_free_calls == 1u);
    CHECK(get_etask_calls == 2u && context_calls == 1u && cleanup_etask_calls == 1u);
    CHECK(free_entry_calls == 2u && task_poisoned);
    CHECK(task_object.tc_UnionETask.tc_ETask == NULL);
    CHECK((task_object.tc_Flags & TF_ETASK) == 0u);
    CHECK(etask_object.et_Reserved[ETASK_RSVD_SLOTID] == NULL);
    CHECK(gate_depth == 0u && irq_depth == 0u);
    retry = event_index(EV_RETRY, 0u);
    claim = event_index(EV_CLAIM, 0u);
    detached = event_index(EV_DETACHED_ENTRY_FREE, 0u);
    get0 = event_index(EV_GET_ETASK, 0u);
    storage = event_index(EV_STORAGE_FREE, 0u);
    get1 = event_index(EV_GET_ETASK, 1u);
    context = event_index(EV_CONTEXT, 0u);
    cleanup = event_index(EV_ETASK_CLEANUP, 0u);
    free0 = event_index(EV_ENTRY0_FREE, 0u);
    free1 = event_index(EV_ENTRY1_FREE, 0u);
    finish = event_index(EV_FINISH, 0u);
    recfree = event_index(EV_RECORD_FREE, 0u);
    tryfinish = event_index(EV_TRY_FINISH, 0u);
    CHECK(retry < claim);
    CHECK(claim < detached && detached < get0 && get0 < storage && storage < get1);
    CHECK(get1 < context && context < cleanup && cleanup < free0 && free0 < free1);
    CHECK(free1 < tryfinish && tryfinish < finish && finish < recfree);
}

static void test_no_claim_no_target_or_resource_access(void)
{
    int result;
    reset_fixture();
    claim_result = NULL;
    current_task = (struct Task *)(uintptr_t)1u;
    get_etask_result = (struct ETask *)(uintptr_t)1u;
    exec_base.lb_TaskResBase = (struct Library *)(uintptr_t)1u;
    result = krnP4CleanupOneRetiredTask(&exec_base);
    CHECK(result == 0);
    CHECK(retry_calls == 1u && claim_calls == 1u && event_count == 2u);
    CHECK(events[0] == EV_RETRY && events[1] == EV_CLAIM);
    CHECK(get_etask_calls == 0u && fatal_calls == 0u && finish_calls == 0u);
    CHECK(detached_entry_free_calls == 0u && storage_free_calls == 0u && free_entry_calls == 0u);
}

static void test_resource_contract_fail_closed(int missing_callback)
{
    int result;
    reset_fixture();
    if (missing_callback)
        taskres.trb_CleanupBinding.cleanup = NULL;
    else
        taskres.trb_CleanupBinding.contract ^= 1u;
    result = krnP4CleanupOneRetiredTask(&exec_base);
    CHECK(result == 0);
    CHECK(claim_calls == 1u && fatal_calls == 1u);
    CHECK(get_etask_calls == 0u && context_calls == 0u && cleanup_etask_calls == 0u);
    CHECK(detached_entry_free_calls == 0u && storage_free_calls == 0u && finish_calls == 0u);
    CHECK(task_object.tc_UnionETask.tc_ETask == &etask_object);
}

static void test_invalid_slot_count_fails_closed(void)
{
    int result;
    reset_fixture();
    storage_area[0] = 0u;
    result = krnP4CleanupOneRetiredTask(&exec_base);
    CHECK(result == 0);
    CHECK(fatal_calls == 1u && finish_calls == 0u && free_entry_calls == 0u);
    CHECK(storage_free_calls == 0u);
    CHECK(etask_object.et_Reserved[ETASK_RSVD_SLOTID] == storage_area);
    CHECK(detached_entry_free_calls == 1u && get_etask_calls == 1u);
}

static void test_postfree_pressure_then_retry_release(void)
{
    unsigned target_calls;
    int result;
    reset_fixture();
    try_finish_result = 0;
    result = krnP4CleanupOneRetiredTask(&exec_base);
    CHECK(result == 1);
    CHECK(claim_calls == 1u && retry_calls == 1u && tryfinish_calls == 1u);
    CHECK(finish_calls == 0u && event_index(EV_PRESSURE, 0u) != UINT_MAX);
    CHECK(free_entry_calls == 2u && task_poisoned && finish_calls == 0u);
    CHECK(event_index(EV_RECORD_FREE, 0u) == UINT_MAX);
    target_calls = get_etask_calls + context_calls + cleanup_etask_calls + free_entry_calls;

    /* The next worker pass retires only the completion-owned record. Target
     * identity and TaskResource base are now deliberately inaccessible. */
    retry_result = &cleanup_record;
    current_task = (struct Task *)(uintptr_t)1u;
    get_etask_result = (struct ETask *)(uintptr_t)1u;
    exec_base.lb_TaskResBase = (struct Library *)(uintptr_t)1u;
    result = krnP4CleanupOneRetiredTask(&exec_base);
    CHECK(result == 1);
    CHECK(retry_calls == 2u && claim_calls == 1u && tryfinish_calls == 1u);
    CHECK(event_index(EV_RECORD_FREE, 0u) != UINT_MAX);
    CHECK(get_etask_calls + context_calls + cleanup_etask_calls + free_entry_calls == target_calls);
    CHECK(fatal_calls == 0u && gate_depth == 0u && irq_depth == 0u);
}

static void test_retry_record_without_claim(void)
{
    int result;
    reset_fixture();
    retry_record.identity = (struct Task *)(uintptr_t)1u;
    retry_result = &retry_record;
    claim_result = NULL;
    current_task = (struct Task *)(uintptr_t)1u;
    get_etask_result = (struct ETask *)(uintptr_t)1u;
    exec_base.lb_TaskResBase = (struct Library *)(uintptr_t)1u;
    result = krnP4CleanupOneRetiredTask(&exec_base);
    CHECK(result == 1);
    CHECK(retry_calls == 1u && claim_calls == 0u && get_etask_calls == 0u);
    CHECK(finish_calls == 0u && fatal_calls == 0u);
    CHECK(event_count == 2u && events[0] == EV_RETRY && events[1] == EV_RECORD_FREE);
}

static void test_callback_null_and_storage_branches(void)
{
    BOOL result;
    reset_fixture();
    result = task_CleanupRemovedTask(NULL, &taskres);
    CHECK(result == FALSE && get_etask_calls == 0u && detached_entry_free_calls == 0u);
    result = task_CleanupRemovedTask(&task_object, NULL);
    CHECK(result == FALSE && get_etask_calls == 0u && detached_entry_free_calls == 0u);

    reset_fixture();
    get_etask_result = NULL;
    result = task_CleanupRemovedTask(&task_object, &taskres);
    CHECK(result == TRUE && get_etask_calls == 1u && storage_free_calls == 0u);
    CHECK(detached_entry_free_calls == 1u);

    reset_fixture();
    etask_object.et_Reserved[ETASK_RSVD_SLOTID] = NULL;
    result = task_CleanupRemovedTask(&task_object, &taskres);
    CHECK(result == TRUE && get_etask_calls == 1u && storage_free_calls == 0u);
    CHECK(etask_object.et_Reserved[ETASK_RSVD_SLOTID] == NULL);

    reset_fixture();
    storage_area[0] = 0u;
    result = task_CleanupRemovedTask(&task_object, &taskres);
    CHECK(result == FALSE && storage_free_calls == 0u);
    CHECK(etask_object.et_Reserved[ETASK_RSVD_SLOTID] == storage_area);

    reset_fixture();
    storage_area[0] = (IPTR)UINT32_MAX;
    result = task_CleanupRemovedTask(&task_object, &taskres);
    CHECK(result == FALSE && storage_free_calls == 0u);
    CHECK(etask_object.et_Reserved[ETASK_RSVD_SLOTID] == storage_area);
}

int main(void)
{
    test_successful_order();
    test_no_claim_no_target_or_resource_access();
    test_resource_contract_fail_closed(0);
    test_resource_contract_fail_closed(1);
    test_invalid_slot_count_fails_closed();
    test_postfree_pressure_then_retry_release();
    test_retry_record_without_claim();
    test_callback_null_and_storage_branches();
    printf("runtime checks: %u\n", checks);
    return EXIT_SUCCESS;
}

/*__ACTUAL_TASK_DETACH__*/
/*__ACTUAL_TASKRESOURCE_CALLBACK__*/
/*__ACTUAL_P4_CLEANUP_CONSUMER__*/
'''


def make_paths(root: Path) -> dict[str, Path]:
    return {
        "interface": root / "rom/exec/taskresource_cleanup.h",
        "intern": root / "rom/task/task_intern.h",
        "init": root / "rom/task/task_init.c",
        "bootstrap": root / "rom/task/task_bootstrap.c",
        "execfuncs": root / "rom/task/task_execfuncs.c",
        "newaddtask": root / "rom/exec/newaddtask.c",
        "support": root / "rom/task/task_support.c",
        "consumer": root / "arch/riscv-esp32p4/exec/p4_task_cleanup.c",
        "platform": root / "arch/riscv-esp32p4/exec/exec_platform.h",
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--artifact-dir", type=Path)
    args = parser.parse_args()
    artifact_dir = args.artifact_dir or Path(tempfile.mkdtemp(prefix="p4-taskres-service-cleanup-author-"))
    artifact_dir.mkdir(parents=True, exist_ok=True)
    paths = make_paths(ROOT)
    source_checks = source_contract_checks(paths)

    support_text = paths["support"].read_text()
    cleanup_text = paths["consumer"].read_text()
    generated = HARNESS.replace(
        "/*__ACTUAL_TASK_DETACH__*/", extract_function(support_text, "task_DetachEntry")
    ).replace(
        "/*__ACTUAL_TASKRESOURCE_CALLBACK__*/", extract_function(support_text, "task_CleanupRemovedTask")
    ).replace(
        "/*__ACTUAL_P4_CLEANUP_CONSUMER__*/", extract_function(cleanup_text, "krnP4CleanupOneRetiredTask")
    )
    c_file = artifact_dir / "p4-taskres-service-cleanup-host.c"
    c_file.write_text(generated)
    clang = os.environ.get("CC", "clang")
    results = []
    for optimization in ("O1", "O2"):
        executable = artifact_dir / f"p4-taskres-service-cleanup-{optimization}-author-20261002"
        log = artifact_dir / f"p4-taskres-service-cleanup-{optimization}-author-20261002.log"
        command = [
            clang, "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wconversion",
            "-Wsign-conversion", "-Wshadow", "-Wstrict-prototypes",
            "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g",
            f"-{optimization}", str(c_file), "-o", str(executable),
        ]
        compiled = subprocess.run(command, text=True, capture_output=True, timeout=20)
        log.write_text("$ " + shlex.join(command) + "\n" + compiled.stdout + compiled.stderr)
        if compiled.returncode != 0:
            raise RuntimeError(f"{optimization} compile failed ({compiled.returncode}); see {log}")
        env = os.environ.copy()
        env["ASAN_OPTIONS"] = "detect_leaks=0:halt_on_error=1"
        env["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
        run = subprocess.run([str(executable)], text=True, capture_output=True, timeout=20, env=env)
        with log.open("a") as stream:
            stream.write("\n$ ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 " + shlex.quote(str(executable)) + "\n")
            stream.write(run.stdout + run.stderr)
        if run.returncode != 0:
            raise RuntimeError(f"{optimization} runtime failed ({run.returncode}); see {log}")
        results.append((optimization, executable, log, run.stdout.strip()))

    print(f"source-contract checks: {source_checks}")
    print(f"generated C: {c_file} sha256={sha256(c_file)} bytes={c_file.stat().st_size}")
    for optimization, executable, log, output in results:
        print(f"{optimization}: {output}; binary={executable} sha256={sha256(executable)} bytes={executable.stat().st_size}; log={log} sha256={sha256(log)}")
    for name, path in paths.items():
        print(f"source {name}: {path} sha256={sha256(path)} bytes={path.stat().st_size}")
    print(f"test driver: {Path(__file__).resolve()} sha256={sha256(Path(__file__).resolve())} bytes={Path(__file__).stat().st_size}")
    print(f"artifact directory: {artifact_dir.resolve()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
