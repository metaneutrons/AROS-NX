#!/usr/bin/env python3
"""Compile the actual P4 TaskResource bootstrap transaction and guard bodies."""

from __future__ import annotations

import argparse
import hashlib
from pathlib import Path
import re
import subprocess


ROOT = Path(__file__).resolve().parents[3]
EVIDENCE = Path(
    "/Volumes/Dev/Source/Amiga/AROS-ESP32-build/evidence/"
    "smp-priority-2026-10-01"
)
SOURCES = {
    "bootstrap": ROOT / "rom/task/task_bootstrap.c",
    "init": ROOT / "rom/task/task_init.c",
    "newaddtask": ROOT / "rom/exec/newaddtask.c",
    "pins": ROOT / "arch/riscv-esp32p4/kernel/p4_task_pins.c",
    "registry_h": ROOT / "arch/riscv-esp32p4/kernel/p4_task_registry.h",
    "guard": ROOT / "arch/riscv-esp32p4/kernel/p4_taskresource_bootstrap.c",
    "platform": ROOT / "arch/riscv-esp32p4/exec/exec_platform.h",
    "task_mk": ROOT / "rom/task/mmakefile.src",
    "kernel_mk": ROOT / "arch/riscv-esp32p4/kernel/mmakefile.src",
}


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


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


def actual_bodies() -> tuple[dict[str, str], dict[str, str]]:
    sources = {name: path.read_text() for name, path in SOURCES.items()}
    boot = sources["bootstrap"]
    pins = sources["pins"]
    guard_source = sources["guard"]
    newadd = sources["newaddtask"]
    bodies = {
        "count_list": extract_function(
            boot, r"(?m)^static BOOL count_list\s*\(", "count_list"
        ),
        "count_inventory": extract_function(
            boot, r"(?m)^static BOOL count_inventory\s*\(", "count_inventory"
        ),
        "take_list": extract_function(
            boot, r"(?m)^static void take_list\s*\(", "take_list"
        ),
        "free_pool": extract_function(
            boot, r"(?m)^static void free_pool\s*\(", "free_pool"
        ),
        "commit_inventory": extract_function(
            boot, r"(?m)^static int commit_inventory\s*\(", "commit_inventory"
        ),
        "task_BootstrapInventory": extract_function(
            boot,
            r"(?m)^BOOL task_BootstrapInventory\s*\(",
            "task_BootstrapInventory",
        ),
        "bootstrap_hardware_isolated": extract_function(
            guard_source,
            r"(?m)^static int bootstrap_hardware_isolated\s*\(",
            "bootstrap_hardware_isolated",
        ),
        "krnP4TaskResourceBootstrapIsolated": extract_function(
            guard_source,
            r"(?m)^int krnP4TaskResourceBootstrapIsolated\s*\(",
            "krnP4TaskResourceBootstrapIsolated",
        ),
        "krnP4TaskResourceBootstrapTransaction": extract_function(
            guard_source,
            r"(?m)^int krnP4TaskResourceBootstrapTransaction\s*\(",
            "krnP4TaskResourceBootstrapTransaction",
        ),
        "p4_taskresource_bootstrap_quiescent_locked": extract_function(
            pins,
            r"(?m)^static int p4_taskresource_bootstrap_quiescent_locked\s*\(",
            "p4_taskresource_bootstrap_quiescent_locked",
        ),
        "krnP4TaskResourceBootstrapQuiescent": extract_function(
            pins,
            r"(?m)^int krnP4TaskResourceBootstrapQuiescent\s*\(",
            "krnP4TaskResourceBootstrapQuiescent",
        ),
        "krnP4TaskResourceBootstrapApply": extract_function(
            pins,
            r"(?m)^int krnP4TaskResourceBootstrapApply\s*\(",
            "krnP4TaskResourceBootstrapApply",
        ),
        "krnP4TaskRecordPublishPinned": extract_function(
            pins,
            r"(?m)^int krnP4TaskRecordPublishPinned\s*\(",
            "krnP4TaskRecordPublishPinned",
        ),
        "TaskLaunch": extract_function(
            newadd, r"(?m)^static void TaskLaunch\s*\(", "TaskLaunch"
        ),
    }
    guard = bodies["bootstrap_hardware_isolated"]
    barrier = '__asm__ volatile("fence iorw,iorw" ::: "memory");'
    if guard.count(barrier) != 1:
        raise RuntimeError("expected one target-only RISC-V fence in actual guard")
    bodies["bootstrap_hardware_isolated"] = guard.replace(barrier, "mock_fence();")
    commit = bodies["commit_inventory"]
    if commit.count("__atomic_store_n(") != 1:
        raise RuntimeError("expected one release-publish in actual inventory callback")
    bodies["commit_inventory"] = commit.replace(
        "__atomic_store_n(", "mock_atomic_store_n("
    )
    return sources, bodies


HARNESS = r"""
#define _DEFAULT_SOURCE 1
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif

typedef uint32_t ULONG;
typedef uint8_t UBYTE;
typedef int BOOL;
typedef void *APTR;
typedef uint32_t uint32;
#define TRUE 1
#define FALSE 0
#define MEMF_PUBLIC UINT32_C(0x00000001)
#define MEMF_CLEAR UINT32_C(0x00010000)
#define MEMF_NO_EXPUNGE UINT32_C(0x00020000)
#define TS_RUN UINT8_C(1)
#define TS_SPIN UINT8_C(2)
#define TS_READY UINT8_C(3)
#define TS_WAIT UINT8_C(4)
#define TS_ADDED UINT8_C(5)
#define TS_INVALID UINT8_C(6)
#define TASKRES_BOOT_LIMIT 20000UL
#define TASKRES_BOOT_ATTEMPTS 4U
#define P4_TASK_LIFETIME_CLAIMED UINT32_C(1)
#define P4_PUBLIC_SPIN_BAD_STATE 1u
#define TASKRESOURCE_CLEANUP_CONTRACT UINT32_C(0x50345233)
#define EXEC_PLATFORM_TASKRESOURCE_SERVICE_CLEANUP 1
#define MSTATUS_MIE UINT32_C(8)
#define P4_SECONDARY_RESET_REG UINT32_C(0x500e60c0)
#define P4_SECONDARY_RESET_BIT UINT32_C(0x100)
#define P4_SECONDARY_CLOCK_REG UINT32_C(0x500e6014)
#define P4_SECONDARY_CLOCK_BIT UINT32_C(0x10)
#define P4_SECONDARY_BOOT_REG UINT32_C(0x50110164)
#define P4_EXEC_RUNTIME_STAGE_FINALIZED 4
#define P4_EXEC_RUNTIME_STAGE_PREPARED 2

struct Node { struct Node *ln_Succ, *ln_Pred; };
struct List { struct Node *lh_Head, *lh_Tail, *lh_TailPred; };
struct Task { struct Node tc_Node; uint8_t tc_State; };
struct Library { uint32_t dummy; };
struct taskresource_cleanup_binding {
    struct Library library;
    uint32_t contract;
    int (*cleanup)(struct Task *, APTR);
    BOOL (*prelaunch)(struct Task *, APTR);
};
struct ExecBase {
    struct List TaskRunning, TaskSpinning, TaskReady, TaskWait;
    struct Library *lb_TaskResBase;
};
struct TaskListEntry { struct Node tle_Node; struct Task *tle_Task; struct List tle_HookTypes; };
struct TaskResBase { struct List trb_TaskList; };
struct taskres_boot_transaction {
    struct TaskResBase *resource;
    struct List *pool;
    ULONG capacity;
};
struct p4_task_wait_scope { uint32_t active; };
struct p4_task_lifetime { uint32_t flags, notification_pins; };
struct p4_task_registry_record {
    struct p4_task_registry_record *next;
    struct Task *identity;
    uint32_t service_pending, service_queued, service_inflight, service_postfree;
    uint32_t service_generation;
    uint32_t semaphore_pending, semaphore_pending_generation;
    uint32_t semaphore_inflight, semaphore_inflight_generation;
    uint32_t worker_protected;
    struct Task *construction_creator;
    uint32_t construction_children;
    uintptr_t creation_frame;
    struct p4_task_wait_scope wait_scope;
    struct p4_task_lifetime lifetime;
};
struct p4_task_registry { struct p4_task_registry_record *head; uint32_t count; };
struct p4_cpu_local {
    struct Task *current_task;
    int id_nest;
    uint8_t hart_id;
    void *sys_base, *kernel_base;
};
struct p4_exec_runtime {
    uint8_t hart_id;
    int stage;
    int initialized;
    struct p4_cpu_local *cpu_local;
};
struct Hook { unsigned unused; };

static struct ExecBase exec_base;
static struct ExecBase *SysBase = &exec_base;
static struct TaskResBase resource_base;
static struct p4_task_registry p4_task_records;
static struct p4_exec_runtime primary_runtime, secondary_runtime;
static struct p4_cpu_local primary_local, secondary_local;
static struct Task current_task;
static uint32_t mock_mstatus = MSTATUS_MIE;
static uint32_t reset_reg = P4_SECONDARY_RESET_BIT;
static uint32_t clock_reg = 0;
static uint32_t boot_reg = 0;
static unsigned cpu_count = 1;
static unsigned irq_depth, gate_depth;
static unsigned allocations, frees, enables, disables, fences, outer_enables;
static unsigned gate_locks, gate_unlocks, validations, fatals;
static unsigned transaction_callback_calls, probe_callback_calls;
static unsigned entry_hook_initializations, inventory_pool_removals;
static unsigned resource_list_insertions, base_publications;
static unsigned mock_registry_valid = 1;
static long fail_alloc_on;
static int add_on_first_enable, add_on_each_first_enable;
static int move_ready_to_wait;
static struct Task tasks[32];
static unsigned task_count;
static struct Task *launch_task;
static struct Task *late_task;
static struct Task *moved_task;
static struct TaskResBase *launch_resource;
static int run_bootstrap_during_ready;
static int bootstrap_during_ready_result;
static unsigned ready_calls;

#define PrivExecBase(base) (base)
#define NEWLIST(list) mock_new_list(list)
#define AddTail(list, node) mock_add_tail((list), (node))
#define RemHead(list) mock_rem_head(list)
#define Remove(node) mock_remove(node)
#define ForeachNode(list, item) \
    for ((item) = (void *)(list)->lh_Head; \
         (item) != NULL && (struct Node *)(item) != (struct Node *)&(list)->lh_Tail; \
         (item) = (void *)((struct Node *)(item))->ln_Succ)
#define Disable() mock_disable()
#define Enable() mock_enable()
#define csr_read(csr) mock_read_mstatus()
#define mstatus 0

static void check_failed(const char *expression, int line)
{
    fprintf(stderr, "CHECK line %d failed: %s\n", line, expression);
    exit(1);
}
#define CHECK(test) do { ++checks; if (!(test)) check_failed(#test, __LINE__); } while (0)
static unsigned checks;

static void mock_new_list(struct List *list)
{
    if (gate_depth != 0) {
        CHECK(gate_depth == 1 && irq_depth >= 2);
        ++entry_hook_initializations;
    }
    list->lh_Head = (struct Node *)&list->lh_Tail;
    list->lh_Tail = NULL;
    list->lh_TailPred = (struct Node *)&list->lh_Head;
}
static void mock_add_tail(struct List *list, struct Node *node)
{
    if (list == &resource_base.trb_TaskList) {
        CHECK(gate_depth == 1 && irq_depth >= 2);
        ++resource_list_insertions;
    }
    struct Node *tail = (struct Node *)&list->lh_Tail;
    node->ln_Succ = tail;
    node->ln_Pred = list->lh_TailPred;
    list->lh_TailPred->ln_Succ = node;
    list->lh_TailPred = node;
}
static struct Node *mock_rem_head(struct List *list)
{
    if (gate_depth != 0) {
        CHECK(gate_depth == 1 && irq_depth >= 2);
        ++inventory_pool_removals;
    }
    struct Node *node = list->lh_Head;
    struct Node *tail = (struct Node *)&list->lh_Tail;
    if (node == tail)
        return NULL;
    list->lh_Head = node->ln_Succ;
    node->ln_Succ->ln_Pred = (struct Node *)&list->lh_Head;
    if (node->ln_Succ == tail)
        list->lh_TailPred = (struct Node *)&list->lh_Head;
    node->ln_Succ = node->ln_Pred = NULL;
    return node;
}
static void mock_remove(struct Node *node)
{
    node->ln_Pred->ln_Succ = node->ln_Succ;
    node->ln_Succ->ln_Pred = node->ln_Pred;
    node->ln_Succ = node->ln_Pred = NULL;
}
static unsigned list_count(const struct List *list)
{
    const struct Node *tail = (const struct Node *)&list->lh_Tail;
    const struct Node *node = list->lh_Head;
    unsigned count = 0;
    while (node != tail) { CHECK(node != NULL); ++count; node = node->ln_Succ; }
    return count;
}
static void mock_disable(void)
{
    ++disables;
    ++irq_depth;
    mock_mstatus &= ~MSTATUS_MIE;
}
static void mock_enable(void)
{
    CHECK(irq_depth != 0);
    CHECK(gate_depth == 0);
    --irq_depth;
    ++enables;
    if (irq_depth == 0) {
        ++outer_enables;
        mock_mstatus |= MSTATUS_MIE;
        if (add_on_first_enable && outer_enables == 1) {
            add_on_first_enable = 0;
            if (move_ready_to_wait) {
                struct Node *node = SysBase->TaskReady.lh_Head;
                struct Node *tail = (struct Node *)&SysBase->TaskReady.lh_Tail;
                CHECK(node != tail);
                moved_task = (struct Task *)((char *)node - offsetof(struct Task, tc_Node));
                mock_remove(node);
                moved_task->tc_State = TS_WAIT;
                mock_add_tail(&SysBase->TaskWait, node);
                move_ready_to_wait = 0;
            }
            CHECK(task_count < sizeof(tasks) / sizeof(tasks[0]));
            late_task = &tasks[task_count++];
            late_task->tc_State = TS_READY;
            mock_add_tail(&SysBase->TaskReady, &late_task->tc_Node);
        } else if (add_on_each_first_enable && (outer_enables & 1u) != 0) {
            CHECK(task_count < sizeof(tasks) / sizeof(tasks[0]));
            late_task = &tasks[task_count++];
            late_task->tc_State = TS_READY;
            mock_add_tail(&SysBase->TaskReady, &late_task->tc_Node);
        }
    }
}
static uint32_t mock_read_mstatus(void) { return mock_mstatus; }
static void mock_fence(void) { CHECK(irq_depth != 0); ++fences; }
static uint32_t p4_r32(uint32_t address)
{
    if (address == P4_SECONDARY_RESET_REG) return reset_reg;
    if (address == P4_SECONDARY_CLOCK_REG) return clock_reg;
    if (address == P4_SECONDARY_BOOT_REG) return boot_reg;
    CHECK(0); return 0;
}
static unsigned krnP4GetCPUCount(void) { return cpu_count; }
static struct p4_exec_runtime *p4_exec_runtime_current(struct ExecBase *base)
{ CHECK(base == SysBase); return &primary_runtime; }
static struct p4_exec_runtime *p4_exec_runtime_for_hart(unsigned hart)
{ return hart == 1 ? &secondary_runtime : NULL; }
static void p4_task_pins_lock(struct ExecBase *base)
{
    CHECK(base == SysBase && gate_depth == 0);
    mock_disable();
    ++gate_locks; ++gate_depth;
}
static void p4_task_pins_unlock(struct ExecBase *base)
{
    CHECK(base == SysBase && gate_depth == 1);
    ++gate_unlocks; --gate_depth;
    mock_enable();
}
static int p4_task_registry_validate_locked(struct p4_task_registry *registry)
{
    CHECK(registry == &p4_task_records && gate_depth == 1);
    ++validations;
    return mock_registry_valid != 0;
}
static int (*mock_validation_keepalive)(struct p4_task_registry *) =
    p4_task_registry_validate_locked;
static int p4_task_registry_register_locked(struct p4_task_registry *registry,
    struct p4_task_registry_record *record, struct Task *task)
{
    CHECK(registry == &p4_task_records && gate_depth == 1);
    if (record->identity != NULL || record->next != NULL || task == NULL)
        return 0;
    record->identity = task;
    record->next = registry->head;
    registry->head = record;
    ++registry->count;
    return 1;
}
static int p4_task_registry_pin_locked(struct p4_task_registry *registry,
    struct Task *task)
{
    struct p4_task_registry_record *record;
    CHECK(registry == &p4_task_records && gate_depth == 1);
    for (record = registry->head; record != NULL; record = record->next) {
        if (record->identity == task) {
            ++record->lifetime.notification_pins;
            return 1;
        }
    }
    return 0;
}
void krnP4PublicSpinFatal(unsigned reason) { (void)reason; ++fatals; }
void *AllocMem(size_t size, ULONG flags)
{
    CHECK(irq_depth == 0 && gate_depth == 0);
    CHECK((flags & (MEMF_PUBLIC | MEMF_CLEAR)) == (MEMF_PUBLIC | MEMF_CLEAR));
    ++allocations;
    if (fail_alloc_on > 0 && (long)allocations == fail_alloc_on)
        return NULL;
    return calloc(1, size);
}
void FreeMem(void *pointer, size_t size)
{
    CHECK(irq_depth == 0 && gate_depth == 0 && pointer != NULL && size != 0);
    ++frees;
    free(pointer);
}
static void mock_atomic_store_n(struct Library **address,
    struct Library *value, int order)
{
    CHECK(address == &SysBase->lb_TaskResBase);
    CHECK(order == __ATOMIC_RELEASE);
    CHECK(gate_depth == 1 && irq_depth >= 2);
    ++base_publications;
    *address = value;
}
static int mock_new_task_ready(struct Task *task);
static void mock_task_fatal(void) { ++fatals; }
static void mock_user_hook(struct Hook *hook, struct Task *task, unsigned arg)
{ (void)hook; (void)task; (void)arg; ++checks; }
#define EXEC_PLATFORM_TASKRESOURCE_BOOTSTRAP_ISOLATED() \
    krnP4TaskResourceBootstrapIsolated(SysBase)
#define EXEC_PLATFORM_TASKRESOURCE_BOOTSTRAP_TRANSACTION(callback, context) \
    krnP4TaskResourceBootstrapTransaction(SysBase, (callback), (context))
#define EXEC_PLATFORM_NEW_TASK_READY(task) mock_new_task_ready(task)
#define EXEC_PLATFORM_TASK_FATAL() mock_task_fatal()
#define DADDTASK(...) ((void)0)
#define CALLHOOKPKT(h,t,a) mock_user_hook((h),(t),(a))

int krnP4TaskResourceBootstrapIsolated(struct ExecBase *);
int krnP4TaskResourceBootstrapTransaction(struct ExecBase *, int (*)(void *), void *);
int krnP4TaskResourceBootstrapQuiescent(struct ExecBase *);
int krnP4TaskResourceBootstrapApply(int (*)(void *), void *, struct ExecBase *);
int krnP4TaskRecordPublishPinned(struct Task *, struct p4_task_registry_record *, struct ExecBase *);
BOOL task_BootstrapInventory(struct TaskResBase *);

@PINNED_PUBLISH@
@QUIESCENT_LOCKED@
@QUIESCENT@
@BOOTSTRAP_HARDWARE_ISOLATED@
@GUARD@
@BOOTSTRAP_APPLY@
@BOOTSTRAP_TRANSACTION@
@COUNT_LIST@
@COUNT_INVENTORY@
@TAKE_LIST@
@FREE_POOL@
@COMMIT_INVENTORY@
static int counted_commit_inventory(void *opaque)
{
    ++transaction_callback_calls;
    return commit_inventory(opaque);
}
@BOOTSTRAP@
@TASK_LAUNCH@

static void reset_fixture(void)
{
    memset(&exec_base, 0, sizeof(exec_base));
    memset(&resource_base, 0, sizeof(resource_base));
    memset(&p4_task_records, 0, sizeof(p4_task_records));
    memset(&primary_runtime, 0, sizeof(primary_runtime));
    memset(&secondary_runtime, 0, sizeof(secondary_runtime));
    memset(&primary_local, 0, sizeof(primary_local));
    memset(&secondary_local, 0, sizeof(secondary_local));
    memset(tasks, 0, sizeof(tasks));
    mock_new_list(&SysBase->TaskRunning); mock_new_list(&SysBase->TaskSpinning);
    mock_new_list(&SysBase->TaskReady); mock_new_list(&SysBase->TaskWait);
    mock_new_list(&resource_base.trb_TaskList);
    primary_runtime.hart_id = 0; primary_runtime.stage = P4_EXEC_RUNTIME_STAGE_FINALIZED;
    primary_runtime.cpu_local = &primary_local;
    primary_local.current_task = &current_task; primary_local.id_nest = 0; primary_local.hart_id = 0;
    secondary_runtime.hart_id = 1; secondary_runtime.stage = P4_EXEC_RUNTIME_STAGE_PREPARED;
    secondary_runtime.initialized = 1; secondary_runtime.cpu_local = &secondary_local;
    secondary_local.hart_id = 1;
    mock_mstatus = MSTATUS_MIE; reset_reg = P4_SECONDARY_RESET_BIT;
    clock_reg = boot_reg = 0; cpu_count = 1;
    irq_depth = gate_depth = allocations = frees = enables = disables = fences = outer_enables = 0;
    gate_locks = gate_unlocks = validations = fatals = 0;
    transaction_callback_calls = probe_callback_calls = 0;
    entry_hook_initializations = inventory_pool_removals = 0;
    resource_list_insertions = base_publications = 0;
    mock_registry_valid = 1; fail_alloc_on = 0;
    add_on_first_enable = add_on_each_first_enable = 0; task_count = 0;
    move_ready_to_wait = 0;
    late_task = moved_task = launch_task = NULL; launch_resource = &resource_base;
    run_bootstrap_during_ready = 0; bootstrap_during_ready_result = -1;
    ready_calls = 0;
}
static struct Task *new_task(uint8_t state, struct List *list)
{
    struct Task *task;
    CHECK(task_count < sizeof(tasks) / sizeof(tasks[0]));
    task = &tasks[task_count++]; task->tc_State = state;
    mock_add_tail(list, &task->tc_Node); return task;
}
static void add_four_inventory_tasks(void)
{
    (void)new_task(TS_RUN, &SysBase->TaskRunning);
    (void)new_task(TS_SPIN, &SysBase->TaskSpinning);
    (void)new_task(TS_READY, &SysBase->TaskReady);
    (void)new_task(TS_WAIT, &SysBase->TaskWait);
}
static void check_inventory_entries(unsigned expected)
{
    struct Node *node = resource_base.trb_TaskList.lh_Head;
    struct Node *tail = (struct Node *)&resource_base.trb_TaskList.lh_Tail;
    unsigned count = 0;
    while (node != tail) {
        struct TaskListEntry *entry = (struct TaskListEntry *)node;
        CHECK(node->ln_Pred != NULL && node->ln_Succ != NULL);
        CHECK(entry->tle_Task != NULL);
        CHECK(entry->tle_HookTypes.lh_Head == (struct Node *)&entry->tle_HookTypes.lh_Tail);
        ++count; node = node->ln_Succ;
    }
    CHECK(count == expected);
}
static int mock_new_task_ready(struct Task *task)
{
    CHECK(task != NULL); ++ready_calls;
    if (run_bootstrap_during_ready) {
        CHECK(task == launch_task && SysBase->lb_TaskResBase == NULL);
        bootstrap_during_ready_result = task_BootstrapInventory(launch_resource);
        CHECK(bootstrap_during_ready_result == FALSE);
        CHECK(SysBase->lb_TaskResBase == NULL);
        /* The ready primitive publishes only after the competing snapshot
         * was refused; it does not inspect the task in the bootstrap helper. */
        task->tc_State = TS_READY;
        mock_add_tail(&SysBase->TaskReady, &task->tc_Node);
    }
    return 1;
}
static void test_capacity_growth_retries_transaction(void)
{
    reset_fixture(); add_four_inventory_tasks();
    /* The first count is followed by a new task before commit. The callback
     * must return the capacity-retry result without mutating, then commit the
     * expanded inventory on the next pass. */
    add_on_first_enable = 1;
    move_ready_to_wait = 1;
    CHECK(task_BootstrapInventory(&resource_base) == TRUE);
    CHECK(SysBase->lb_TaskResBase == (struct Library *)&resource_base);
    CHECK(allocations == 5 && frees == 0 && enables == disables);
    CHECK(transaction_callback_calls == 2);
    CHECK(entry_hook_initializations == 5 && inventory_pool_removals == 5);
    CHECK(resource_list_insertions == 5 && base_publications == 1);
    CHECK(list_count(&resource_base.trb_TaskList) == 5);
    CHECK(moved_task != NULL && moved_task->tc_State == TS_WAIT);
    CHECK(list_count(&SysBase->TaskWait) == 2 && list_count(&SysBase->TaskReady) == 1);
    check_inventory_entries(5);
    CHECK(fences == 4 && gate_locks == gate_unlocks && gate_depth == 0 && irq_depth == 0);
}
static void test_failure_atomicity(void)
{
    reset_fixture();
    CHECK(task_BootstrapInventory(&resource_base) == FALSE); /* zero total is invalid */
    CHECK(allocations == 0 && frees == 0 && SysBase->lb_TaskResBase == NULL);
    reset_fixture(); (void)new_task(TS_READY, &SysBase->TaskReady);
    (void)new_task(TS_RUN, &SysBase->TaskRunning);
    fail_alloc_on = 2;
    CHECK(task_BootstrapInventory(&resource_base) == FALSE);
    CHECK(allocations == 2 && frees == 1 && SysBase->lb_TaskResBase == NULL);
    CHECK(list_count(&SysBase->TaskReady) == 1 && list_count(&SysBase->TaskRunning) == 1);
    CHECK(list_count(&resource_base.trb_TaskList) == 0 && irq_depth == 0 && gate_depth == 0);
}
static void test_invalid_inventory_and_existing_base(void)
{
    struct Task *task;
    reset_fixture(); task = new_task(TS_READY, &SysBase->TaskReady);
    task->tc_State = TS_WAIT;
    CHECK(task_BootstrapInventory(&resource_base) == FALSE);
    CHECK(allocations == 0 && SysBase->lb_TaskResBase == NULL && list_count(&resource_base.trb_TaskList) == 0);
    reset_fixture(); task = new_task(TS_READY, &SysBase->TaskReady);
    task->tc_Node.ln_Pred = NULL; /* malformed reciprocal list linkage */
    CHECK(task_BootstrapInventory(&resource_base) == FALSE);
    CHECK(allocations == 0 && SysBase->lb_TaskResBase == NULL);
    reset_fixture(); add_four_inventory_tasks();
    SysBase->lb_TaskResBase = (struct Library *)&resource_base;
    CHECK(task_BootstrapInventory(&resource_base) == FALSE);
    CHECK(allocations == 0 && list_count(&resource_base.trb_TaskList) == 0 && fences == 0);
    CHECK(irq_depth == 0 && gate_depth == 0);
}
static void test_bounded_retry_exhaustion(void)
{
    reset_fixture(); (void)new_task(TS_READY, &SysBase->TaskReady);
    add_on_each_first_enable = 1;
    CHECK(task_BootstrapInventory(&resource_base) == FALSE);
    CHECK(allocations == 4 && frees == 4 && SysBase->lb_TaskResBase == NULL);
    CHECK(transaction_callback_calls == 4 && base_publications == 0);
    CHECK(entry_hook_initializations == 0 && inventory_pool_removals == 0);
    CHECK(resource_list_insertions == 0);
    CHECK(list_count(&resource_base.trb_TaskList) == 0);
    CHECK(list_count(&SysBase->TaskReady) == 5);
    CHECK(enables == disables && irq_depth == 0 && gate_depth == 0);
}
static int probe_transaction(void *context)
{
    CHECK(context == &resource_base);
    CHECK(gate_depth == 1 && irq_depth >= 2);
    ++probe_callback_calls;
    return 1;
}
static void test_transaction_refuses_before_callback(void)
{
    struct p4_task_registry_record ordinary_pin;
    reset_fixture();
    reset_reg = 0;
    mock_disable();
    CHECK(krnP4TaskResourceBootstrapTransaction(
        SysBase, probe_transaction, &resource_base) == 0);
    CHECK(probe_callback_calls == 0 && gate_locks == 0 && gate_unlocks == 0);
    mock_enable();
    CHECK(irq_depth == 0 && gate_depth == 0);

    reset_fixture();
    memset(&ordinary_pin, 0, sizeof(ordinary_pin));
    ordinary_pin.lifetime.notification_pins = 1;
    p4_task_records.head = &ordinary_pin;
    p4_task_records.count = 1;
    mock_disable();
    CHECK(krnP4TaskResourceBootstrapTransaction(
        SysBase, probe_transaction, &resource_base) == 0);
    CHECK(probe_callback_calls == 0 && validations == 1);
    CHECK(gate_locks == 1 && gate_unlocks == 1 && gate_depth == 0);
    mock_enable();
    CHECK(irq_depth == 0 && gate_depth == 0);
}
static void test_registry_quiescence_binding(void)
{
    struct p4_task_registry_record record;
    struct p4_task_registry_record before;
    long page_size = sysconf(_SC_PAGESIZE);
    void *protected_page;
    reset_fixture();
    memset(&record, 0, sizeof(record));
    protected_page = mmap(NULL, (size_t)page_size, PROT_NONE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(protected_page != MAP_FAILED);
    record.identity = (struct Task *)protected_page;
    record.lifetime.notification_pins = 1; /* ordinary construction/reference pin */
    before = record;
    p4_task_records.head = &record; p4_task_records.count = 1;
    mock_disable();
    CHECK(krnP4TaskResourceBootstrapQuiescent(SysBase) == 0);
    mock_enable();
    CHECK(gate_locks == 1 && gate_unlocks == 1 && validations == 1);
    CHECK(memcmp(&record, &before, sizeof(record)) == 0);
    CHECK(munmap(protected_page, (size_t)page_size) == 0);

    reset_fixture(); memset(&record, 0, sizeof(record));
    protected_page = mmap(NULL, (size_t)page_size, PROT_NONE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(protected_page != MAP_FAILED);
    record.identity = (struct Task *)protected_page;
    record.worker_protected = 1; record.wait_scope.active = 1;
    record.semaphore_pending = 2; record.semaphore_inflight = 3;
    record.lifetime.notification_pins = 7;
    p4_task_records.head = &record; p4_task_records.count = 1;
    mock_disable(); CHECK(krnP4TaskResourceBootstrapQuiescent(SysBase) == 1); mock_enable();
    CHECK(gate_locks == gate_unlocks && gate_locks == 1);
    record.lifetime.notification_pins = 6;
    mock_disable(); CHECK(krnP4TaskResourceBootstrapQuiescent(SysBase) == 0); mock_enable();
    record.lifetime.notification_pins = 7;
    record.construction_children = 1;
    mock_disable(); CHECK(krnP4TaskResourceBootstrapQuiescent(SysBase) == 0); mock_enable();
    record.construction_children = 0; record.construction_creator = (struct Task *)protected_page;
    mock_disable(); CHECK(krnP4TaskResourceBootstrapQuiescent(SysBase) == 0); mock_enable();
    record.construction_creator = NULL; record.creation_frame = 1;
    mock_disable(); CHECK(krnP4TaskResourceBootstrapQuiescent(SysBase) == 0); mock_enable();
    CHECK(munmap(protected_page, (size_t)page_size) == 0);

    reset_fixture(); memset(&record, 0, sizeof(record));
    protected_page = mmap(NULL, (size_t)page_size, PROT_NONE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(protected_page != MAP_FAILED);
    record.identity = (struct Task *)protected_page;
    record.lifetime.flags = P4_TASK_LIFETIME_CLAIMED;
    before = record;
    p4_task_records.head = &record; p4_task_records.count = 1;
    mock_disable(); CHECK(krnP4TaskResourceBootstrapQuiescent(SysBase) == 0); mock_enable();
    CHECK(memcmp(&record, &before, sizeof(record)) == 0);
    CHECK(munmap(protected_page, (size_t)page_size) == 0);

    reset_fixture(); memset(&record, 0, sizeof(record));
    protected_page = mmap(NULL, (size_t)page_size, PROT_NONE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(protected_page != MAP_FAILED);
    record.identity = (struct Task *)protected_page;
    p4_task_records.head = &record; p4_task_records.count = 1;
    mock_registry_valid = 0;
    mock_disable(); CHECK(krnP4TaskResourceBootstrapQuiescent(SysBase) == 0); mock_enable();
    CHECK(validations == 1 && gate_locks == gate_unlocks && gate_locks == 1);
    CHECK(munmap(protected_page, (size_t)page_size) == 0);
}
static void test_guard_hardware_matrix(void)
{
    reset_fixture();
    mock_disable(); CHECK(krnP4TaskResourceBootstrapIsolated(SysBase) == 1); mock_enable();
    CHECK(fences == 1 && irq_depth == 0);
    reset_fixture(); secondary_runtime.initialized = 0;
    mock_disable(); CHECK(krnP4TaskResourceBootstrapIsolated(SysBase) == 0); mock_enable();
    CHECK(fences == 0);
    reset_fixture(); reset_reg = 0;
    mock_disable(); CHECK(krnP4TaskResourceBootstrapIsolated(SysBase) == 0); mock_enable();
    reset_fixture(); clock_reg = P4_SECONDARY_CLOCK_BIT;
    mock_disable(); CHECK(krnP4TaskResourceBootstrapIsolated(SysBase) == 0); mock_enable();
    reset_fixture(); boot_reg = 1;
    mock_disable(); CHECK(krnP4TaskResourceBootstrapIsolated(SysBase) == 0); mock_enable();
    reset_fixture(); cpu_count = 2;
    mock_disable(); CHECK(krnP4TaskResourceBootstrapIsolated(SysBase) == 0); mock_enable();
    reset_fixture(); primary_local.id_nest = -1;
    mock_disable(); CHECK(krnP4TaskResourceBootstrapIsolated(SysBase) == 0); mock_enable();
    CHECK(irq_depth == 0 && gate_depth == 0);
}
static void test_actual_newaddtask_interleaving(void)
{
    struct p4_task_registry_record record;
    reset_fixture(); add_four_inventory_tasks();
    memset(&record, 0, sizeof(record));
    launch_task = &tasks[task_count++]; launch_task->tc_State = TS_ADDED;
    CHECK(krnP4TaskRecordPublishPinned(launch_task, &record, SysBase) == 1);
    CHECK(record.identity == launch_task && record.lifetime.notification_pins == 1);
    run_bootstrap_during_ready = 1;
    TaskLaunch(NULL, launch_task, NULL);
    CHECK(bootstrap_during_ready_result == FALSE && ready_calls == 1 && fatals == 0);
    CHECK(SysBase->lb_TaskResBase == NULL && launch_task->tc_State == TS_READY);
    CHECK(list_count(&SysBase->TaskReady) == 2 && list_count(&resource_base.trb_TaskList) == 0);
    CHECK(record.lifetime.notification_pins == 1 && irq_depth == 0 && gate_depth == 0);
}
static void test_source_ordering_and_build_route(void)
{
    const char *newadd = @NEWADD_LITERAL@;
    const char *task_mk = @TASK_MK_LITERAL@;
    const char *kernel_mk = @KERNEL_MK_LITERAL@;
    const char *platform = @PLATFORM_LITERAL@;
    const char *init = @INIT_LITERAL@;
    const char *bootstrap = @BOOTSTRAP_LITERAL@;
    const char *record_create = "EXEC_PLATFORM_TASK_RECORD_CREATE(task)";
    const char *record_release = "EXEC_PLATFORM_TASK_RECORD_RELEASE(task)";
    const char *launch = "TaskLaunch(parent, task, plHook);";
    const char *added = "task->tc_State = TS_ADDED;";
    const char *copy = "take_list(&PrivExecBase";
    const char *store = "__atomic_store_n";
    CHECK(strstr(newadd, added) != NULL && strstr(newadd, record_create) != NULL);
    CHECK(strstr(newadd, launch) != NULL && strstr(newadd, record_release) != NULL);
    CHECK(strstr(newadd, added) < strstr(newadd, record_create));
    CHECK(strstr(newadd, record_create) < strstr(newadd, launch));
    CHECK(strstr(newadd, launch) < strstr(newadd, record_release));
    CHECK(strstr(task_mk, "task_bootstrap") != NULL);
    CHECK(strstr(kernel_mk, "p4_taskresource_bootstrap") != NULL);
    CHECK(strstr(platform, "EXEC_PLATFORM_TASKRESOURCE_BOOTSTRAP_ISOLATED") != NULL);
    CHECK(strstr(platform, "EXEC_PLATFORM_TASKRESOURCE_BOOTSTRAP_TRANSACTION") != NULL);
    CHECK(strstr(platform, "krnP4TaskResourceBootstrapTransaction(SysBase") != NULL);
    CHECK(strstr(init, "task_BootstrapInventory(TaskResBase)") != NULL);
    CHECK(strstr(init, "Remove((struct Node *)freeTaskStorageSlot)") != NULL);
    CHECK(strstr(init, "FreeMem(freeTaskStorageSlot") != NULL);
    CHECK(strstr(bootstrap, "__ATOMIC_RELEASE") != NULL);
    CHECK(strstr(bootstrap, store) != NULL && strstr(bootstrap, copy) != NULL);
    CHECK(strstr(bootstrap, copy) < strstr(bootstrap, store));
    CHECK(strstr(bootstrap, "static int commit_inventory(void *opaque)") != NULL);
    CHECK(strstr(bootstrap, "EXEC_PLATFORM_TASKRESOURCE_BOOTSTRAP_TRANSACTION(") != NULL);
    CHECK(strstr(bootstrap, "commit_inventory, &transaction") != NULL);
}
int main(void)
{
    (void)mock_validation_keepalive;
    test_actual_newaddtask_interleaving();
    test_capacity_growth_retries_transaction();
    test_failure_atomicity();
    test_invalid_inventory_and_existing_base();
    test_bounded_retry_exhaustion();
    test_transaction_refuses_before_callback();
    test_registry_quiescence_binding();
    test_guard_hardware_matrix();
    test_source_ordering_and_build_route();
    printf("taskres-bootstrap: %u checks passed\n", checks);
    return 0;
}
"""


def c_string(path: Path) -> str:
    return '"' + path.read_text().replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n") + '"'


def build_harness(
    bodies: dict[str, str],
    quiescent_locked_override: str | None = None,
) -> str:
    bootstrap_body = bodies["task_BootstrapInventory"]
    callback_site = "commit_inventory, &transaction"
    if bootstrap_body.count(callback_site) != 1:
        raise RuntimeError("expected one final inventory callback site")
    bootstrap_body = bootstrap_body.replace(
        callback_site, "counted_commit_inventory, &transaction"
    )
    replacements = {
        "@PINNED_PUBLISH@": bodies["krnP4TaskRecordPublishPinned"],
        "@QUIESCENT_LOCKED@": bodies["p4_taskresource_bootstrap_quiescent_locked"],
        "@QUIESCENT@": bodies["krnP4TaskResourceBootstrapQuiescent"],
        "@BOOTSTRAP_HARDWARE_ISOLATED@": bodies["bootstrap_hardware_isolated"],
        "@GUARD@": bodies["krnP4TaskResourceBootstrapIsolated"],
        "@BOOTSTRAP_APPLY@": bodies["krnP4TaskResourceBootstrapApply"],
        "@BOOTSTRAP_TRANSACTION@": bodies["krnP4TaskResourceBootstrapTransaction"],
        "@COUNT_LIST@": bodies["count_list"],
        "@COUNT_INVENTORY@": bodies["count_inventory"],
        "@TAKE_LIST@": bodies["take_list"],
        "@FREE_POOL@": bodies["free_pool"],
        "@COMMIT_INVENTORY@": bodies["commit_inventory"],
        "@BOOTSTRAP@": bootstrap_body,
        "@TASK_LAUNCH@": bodies["TaskLaunch"],
    }
    text = HARNESS
    if quiescent_locked_override is not None:
        replacements["@QUIESCENT_LOCKED@"] = quiescent_locked_override
    for marker, body in replacements.items():
        text = text.replace(marker, body)
    for key, marker in (
        ("newaddtask", "@NEWADD_LITERAL@"),
        ("task_mk", "@TASK_MK_LITERAL@"),
        ("kernel_mk", "@KERNEL_MK_LITERAL@"),
        ("platform", "@PLATFORM_LITERAL@"),
        ("init", "@INIT_LITERAL@"),
        ("bootstrap", "@BOOTSTRAP_LITERAL@"),
    ):
        text = text.replace(marker, c_string(SOURCES[key]))
    return text


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--artifact-dir", type=Path, required=True)
    args = parser.parse_args()
    args.artifact_dir.mkdir(parents=True, exist_ok=True)
    sources, bodies = actual_bodies()
    generated = args.artifact_dir / "taskres-bootstrap-generated.c"
    generated.write_text(build_harness(bodies))
    print("sources", flush=True)
    for name, path in SOURCES.items():
        print(name, digest(path), path, flush=True)
    print("generated", digest(generated), generated, flush=True)
    for optimization in ("O1", "O2"):
        binary = args.artifact_dir / f"p4-taskres-bootstrap-author-{optimization}"
        command = [
            "clang", "-std=c11", "-" + optimization, "-Wall", "-Wextra", "-Werror",
            "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
            "-fno-omit-frame-pointer", str(generated), "-o", str(binary),
        ]
        print("compile", " ".join(command), flush=True)
        subprocess.run(command, check=True, timeout=20)
        subprocess.run([str(binary)], check=True, timeout=20)
        print("artifact", binary.stat().st_size, digest(binary), binary, flush=True)
    # Prove the interleaving assertion detects the original unsafe policy:
    # a NULL observer load, then bootstrap at the Ready boundary, with the
    # constructor's pinned record present in the registry.
    mutant_body = (
        "static int p4_taskresource_bootstrap_quiescent_locked(void) "
        "{ return 1; }"
    )
    mutant_source = args.artifact_dir / "taskres-bootstrap-no-quiescence-mutant.c"
    mutant_source.write_text(build_harness(bodies, mutant_body))
    mutant_binary = args.artifact_dir / "p4-taskres-bootstrap-negative-O2"
    mutant_command = [
        "clang", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
        "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
        "-fno-omit-frame-pointer", str(mutant_source), "-o", str(mutant_binary),
    ]
    print("negative-compile", " ".join(mutant_command), flush=True)
    subprocess.run(mutant_command, check=True, timeout=20)
    negative = subprocess.run(
        [str(mutant_binary)], check=False, timeout=20,
        capture_output=True, text=True,
    )
    if negative.returncode != 1 or "bootstrap_during_ready_result == FALSE" not in negative.stderr:
        raise AssertionError(
            f"quiescence negative control did not hit the intended interleaving assertion: "
            f"exit={negative.returncode}, stderr={negative.stderr!r}"
        )
    print("negative-control: expected interleaving CHECK exit 1", flush=True)
    print("negative artifact", mutant_binary.stat().st_size, digest(mutant_binary), mutant_binary, flush=True)
    print("limits: actual bootstrap/commit callback, hardware guard, lifetime-gated Apply/quiescence and TaskLaunch bodies; registry validator, gate primitive, scheduler Ready, list/allocator/IRQ/hardware/runtime primitives are host mocks", flush=True)


if __name__ == "__main__":
    main()
