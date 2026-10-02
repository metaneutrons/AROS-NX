#!/usr/bin/env python3
"""Compile actual Exec_CleanupETask with actual P4 metadata owner hooks."""

from pathlib import Path
import hashlib
import os
import subprocess
import tempfile


REPO = Path(__file__).resolve().parents[4]
EXEC_DIR = REPO / "arch/riscv-esp32p4/exec"
UTIL_PATH = REPO / "rom/exec/exec_util.c"
IDENTITY_PATH = EXEC_DIR / "p4_etask_identity.c"
LIFETIME_PATH = EXEC_DIR / "p4_etask_lifetime.c"
BINDING_PATH = EXEC_DIR / "p4_etask_identity_binding.c"


def extract_function(source: str, signature: str) -> str:
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 0
    state = "code"
    index = opening
    while index < len(source):
        char = source[index]
        following = source[index + 1] if index + 1 < len(source) else ""
        if state == "code":
            if char == "/" and following == "*":
                state = "block"
                index += 2
                continue
            if char == "/" and following == "/":
                state = "line"
                index += 2
                continue
            if char == '"':
                state = "string"
            elif char == "'":
                state = "char"
            elif char == "{":
                depth += 1
            elif char == "}":
                depth -= 1
                if depth == 0:
                    return source[start:index + 1]
        elif state == "block":
            if char == "*" and following == "/":
                state = "code"
                index += 2
                continue
        elif state == "line":
            if char == "\n":
                state = "code"
        elif state == "string":
            if char == "\\":
                index += 2
                continue
            if char == '"':
                state = "code"
        elif state == "char":
            if char == "\\":
                index += 2
                continue
            if char == "'":
                state = "code"
        index += 1
    raise ValueError(f"unterminated function after {signature!r}")


util_source = UTIL_PATH.read_text()
cleanup = extract_function(util_source, "Exec_CleanupETask(struct Task *task,")
source_hashes = {
    "exec_util.c": hashlib.sha256(util_source.encode()).hexdigest(),
    "p4_etask_identity.c": hashlib.sha256(IDENTITY_PATH.read_bytes()).hexdigest(),
    "p4_etask_lifetime.c": hashlib.sha256(LIFETIME_PATH.read_bytes()).hexdigest(),
    "p4_etask_identity_binding.c": hashlib.sha256(BINDING_PATH.read_bytes()).hexdigest(),
}

PRELUDE = r'''
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sanitizer/asan_interface.h>

#define __AROSEXEC_SMP__ 1
#define P4_ETASK_IDENTITY_BINDING_TEST 1
#define P4_SRAMDATA
#define P4_PUBLIC_SPIN_BAD_STATE 7
#define MEMF_PUBLIC 1u
#define MEMF_CLEAR 2u
#define TF_ETASK 0x0040u
#define EXECF_CPUAffinity 0x0001u
#define TASKAFFINITY_ANY ((uintptr_t)UINTPTR_MAX)
#define NT_PROCESS 13u
#define PRF_NOTIFYONDEATH 0x0010u
#define SPINLOCK_MODE_WRITE 1u
#define TRUE 1
#define FALSE 0
#define D(...) do { } while (0);
#define IntETask(et) ((struct IntETask *)(et))
#define PrivExecBase(base) ((struct IntExecBase *)(base))
#define GetETask(task) ((task)->tc_UnionETask.tc_ETask)
#define EXEC_PLATFORM_ETASK_CLEANUP_BEGIN(et) \
    mock_cleanup_begin((et), SysBase)
#define EXEC_PLATFORM_ETASK_RESULT_RETAIN(et) \
    mock_result_retain((et), SysBase)
#define NEWLIST(list) init_list((struct List *)(list))
#define RemHead(list) rem_head((struct List *)(list))
#define AddTail(list, node) add_tail((struct List *)(list), (struct Node *)(node))
#define ADDTAIL(list, node) add_tail((struct List *)(list), (struct Node *)(node))
#define REMOVE(node) ((void)Remove((struct Node *)(node)))
#define ForeachNodeSafe(list, current, next) \
    for ((current) = (void *)((struct List *)(list))->lh_Head; \
         (current) != NULL && \
             ((struct Node *)(void *)(current))->ln_Succ != NULL && \
             (((next) = (void *)((struct Node *)(void *)(current))->ln_Succ), 1); \
         (current) = (void *)(next))
#define ForeachNode(list, current) \
    for ((current) = (void *)((struct List *)(list))->lh_Head; \
         (current) != NULL && \
             ((struct Node *)(void *)(current))->ln_Succ != NULL; \
         (current) = (void *)((struct Node *)(void *)(current))->ln_Succ)
#define EXEC_SPINLOCK_LOCK(lock, hook, mode) \
    mock_lock((void *)(lock), (hook), (mode))
#define EXEC_SPINLOCK_UNLOCK(lock) mock_unlock((void *)(lock))

typedef uint32_t ULONG;
typedef uint16_t UWORD;
typedef uintptr_t IPTR;
typedef int BOOL;
typedef void *APTR;
struct Task;
struct ETask;
struct ExecBase;
struct Node {
    struct Node *ln_Succ;
    struct Node *ln_Pred;
    uint8_t ln_Type;
    int8_t ln_Pri;
    char *ln_Name;
};
struct List {
    struct Node *lh_Head;
    struct Node *lh_Tail;
    struct Node *lh_TailPred;
};
struct MinList {
    struct Node *mlh_Head;
    struct Node *mlh_Tail;
    struct Node *mlh_TailPred;
};
struct Message { struct Node mn_Node; };
struct MsgPort {
    struct List mp_MsgList;
    uint32_t mp_SpinLock;
    struct Task *mp_SigTask;
    uint8_t mp_SigBit;
};
struct Task {
    struct Node tc_Node;
    uint32_t tc_Flags;
    union { struct ETask *tc_ETask; void *tc_other; } tc_UnionETask;
};
struct Process { struct Task pr_Task; uint32_t pr_Flags; };
struct ETask {
    struct Message et_Message;
    struct Task *et_Parent;
    uint32_t et_UniqueID;
    struct MinList et_Children;
    uint16_t et_TrapAlloc;
    uint16_t et_TrapAble;
    uint32_t et_Result1;
    void *et_Result2;
    struct MsgPort et_TaskMsgPort;
};
struct IntETask {
    struct ETask iet_ETask;
    uint32_t iet_TaskLock;
    void *iet_CpuAffinity;
};
struct ExecBase { uint32_t ex_TaskID; };
struct IntExecBase { struct ExecBase pub; uint32_t IntFlags; };

struct p4_etask_identity_record;
struct p4_etask_lifetime;
static void *AllocMem(size_t size, ULONG flags);
static void FreeMem(void *memory, size_t size);
static void mock_cleanup_begin(struct ETask *et, struct ExecBase *base);
static void mock_result_retain(struct ETask *et, struct ExecBase *base);
uint32_t krnP4ETaskGateLock(struct ExecBase *base);
void krnP4ETaskGateUnlock(uint32_t token);
void krnP4PublicSpinFatal(int code);

static unsigned checks;
static unsigned gate_depth;
static unsigned spin_depth;
static unsigned event_count;
static unsigned events[8];
static unsigned alloc_calls;
static unsigned free_calls;
static unsigned fatal_calls;
static unsigned forbid_depth;
static unsigned permit_calls;
static unsigned putmsg_calls;
static unsigned expunge_calls;
static unsigned storage_free_calls;
static struct ExecBase *active_base;
static struct ETask *poisoned_etask;
static uint32_t retained_pid;

enum { EVENT_BEGIN = 1u, EVENT_RETAIN, EVENT_PUTMSG, EVENT_PERMIT };

#define CHECK(condition) do { \
    ++checks; \
    if (!(condition)) { \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", \
                __FILE__, __LINE__, #condition); \
        exit(1); \
    } \
} while (0)

static void add_event(unsigned event_value)
{
    CHECK(event_count < sizeof(events) / sizeof(events[0]));
    events[event_count++] = event_value;
}

static void init_list(struct List *list)
{
    list->lh_Head = (struct Node *)&list->lh_Tail;
    list->lh_Tail = NULL;
    list->lh_TailPred = (struct Node *)&list->lh_Head;
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
    CHECK(node != NULL && node->ln_Pred != NULL && node->ln_Succ != NULL);
    node->ln_Pred->ln_Succ = node->ln_Succ;
    node->ln_Succ->ln_Pred = node->ln_Pred;
    return node;
}

static struct Node *rem_head(struct List *list)
{
    struct Node *node = list->lh_Head;
    return node != NULL && node->ln_Succ != NULL ? Remove(node) : NULL;
}

static void mock_lock(void *lock, void *hook, unsigned mode)
{
    CHECK(lock != NULL && hook == NULL && mode == SPINLOCK_MODE_WRITE);
    CHECK(event_count >= 1u && events[0] == EVENT_BEGIN);
    CHECK(spin_depth == 0u);
    ++spin_depth;
}

static void mock_unlock(void *lock)
{
    CHECK(lock != NULL && spin_depth == 1u);
    --spin_depth;
}

static void Forbid(void) { ++forbid_depth; }
static void Permit(void)
{
    CHECK(forbid_depth == 1u && spin_depth == 0u && gate_depth == 0u);
    CHECK(putmsg_calls == 1u);
    --forbid_depth;
    ++permit_calls;
    add_event(EVENT_PERMIT);
}
static void KrnFreeCPUMask(void *mask) { free(mask); }
static void ExpungeETask(struct ETask *et)
{
    (void)et;
    ++expunge_calls;
    CHECK(0 && "retained-result cleanup must not expunge the ETask");
}
static void PutMsg(struct MsgPort *port, struct Message *message);
'''

TAIL = r'''
static void *AllocMem(size_t size, ULONG flags)
{
    CHECK(gate_depth == 0u && spin_depth == 0u);
    CHECK(size == sizeof(struct p4_etask_owner_record));
    CHECK(flags == (MEMF_PUBLIC | MEMF_CLEAR));
    ++alloc_calls;
    return calloc(1u, size);
}

static void FreeMem(void *memory, size_t size)
{
    CHECK(gate_depth == 0u && spin_depth == 0u && memory != NULL);
    CHECK(size == sizeof(struct p4_etask_owner_record));
    ++free_calls;
    free(memory);
}

uint32_t krnP4ETaskGateLock(struct ExecBase *base)
{
    CHECK(base == active_base && gate_depth == 0u && spin_depth == 0u);
    gate_depth = 1u;
    return 0u;
}

void krnP4ETaskGateUnlock(uint32_t token)
{
    CHECK(token == 0u && gate_depth == 1u && spin_depth == 0u);
    gate_depth = 0u;
}

void krnP4PublicSpinFatal(int code)
{
    CHECK(gate_depth == 0u && code == P4_PUBLIC_SPIN_BAD_STATE);
    ++fatal_calls;
    CHECK(0 && "valid owner protocol unexpectedly failed closed");
}

void Exec_FreeETaskStorage(struct ETask *et, struct ExecBase *base)
{
    CHECK(et == poisoned_etask && base == active_base);
    ++storage_free_calls;
    CHECK(0 && "caller-owned immediate release must not invoke deferred helper");
}

static void mock_cleanup_begin(struct ETask *et, struct ExecBase *base)
{
    CHECK(et == poisoned_etask && base == active_base);
    CHECK(spin_depth == 0u && gate_depth == 0u);
    CHECK(identities.count == 1u);
    add_event(EVENT_BEGIN);
    krnP4ETaskMetadataBeginCleanup(et, base);
}

static void mock_result_retain(struct ETask *et, struct ExecBase *base)
{
    CHECK(et == poisoned_etask && base == active_base);
    CHECK(spin_depth == 0u && gate_depth == 0u);
    CHECK(event_count == 1u && events[0] == EVENT_BEGIN);
    add_event(EVENT_RETAIN);
    krnP4ETaskMetadataRetainResult(et, base);
}

static void PutMsg(struct MsgPort *port, struct Message *message)
{
    struct p4_etask_owner_record *owner;
    CHECK(port != NULL && message == (struct Message *)poisoned_etask);
    CHECK(spin_depth == 0u && gate_depth == 0u && forbid_depth == 1u);
    CHECK(event_count == 2u && events[0] == EVENT_BEGIN &&
          events[1] == EVENT_RETAIN);
    CHECK(identities.count == 1u && identities.head != NULL);
    owner = (struct p4_etask_owner_record *)identities.head;
    CHECK(owner->identity.identity == poisoned_etask);
    CHECK(owner->identity.pid == retained_pid);
    CHECK(owner->lifetime.initialized == 1u && owner->lifetime.task_ref == 0u);
    CHECK(owner->lifetime.result_ref == 1u && owner->lifetime.closing == 1u);
    CHECK(owner->lifetime.expunge == 0u && owner->lifetime.readers == 0u);
    ++putmsg_calls;
    add_event(EVENT_PUTMSG);
    memset(poisoned_etask, 0xdd, sizeof(*poisoned_etask));
    __asan_poison_memory_region(poisoned_etask, sizeof(*poisoned_etask));
}

static void test_cleanup_owner_transfer_before_visibility(void)
{
    struct IntExecBase int_base = { { 90u }, 0u };
    struct Process process;
    struct Task parent_task;
    struct IntETask *child_storage;
    struct IntETask *parent_storage;
    struct ETask *child;
    struct ETask *parent;
    unsigned char *bytes;
    size_t index;

    memset(&process, 0, sizeof(process));
    memset(&parent_task, 0, sizeof(parent_task));
    child_storage = calloc(1u, sizeof(*child_storage));
    parent_storage = calloc(1u, sizeof(*parent_storage));
    CHECK(child_storage != NULL && parent_storage != NULL);
    child = &child_storage->iet_ETask;
    parent = &parent_storage->iet_ETask;
    active_base = &int_base.pub;
    poisoned_etask = child;
    process.pr_Task.tc_Flags = TF_ETASK;
    process.pr_Task.tc_Node.ln_Type = NT_PROCESS;
    process.pr_Task.tc_UnionETask.tc_ETask = child;
    process.pr_Flags = PRF_NOTIFYONDEATH;
    parent_task.tc_Flags = TF_ETASK;
    parent_task.tc_UnionETask.tc_ETask = parent;
    child->et_Parent = &parent_task;
    init_list((struct List *)&child->et_Children);
    init_list((struct List *)&parent->et_Children);
    init_list(&child->et_TaskMsgPort.mp_MsgList);
    init_list(&parent->et_TaskMsgPort.mp_MsgList);
    add_tail((struct List *)&parent->et_Children, (struct Node *)child);

    CHECK(krnP4ETaskIdentityReserve(child, active_base));
    retained_pid = child->et_UniqueID;
    CHECK(retained_pid == 91u && identities.count == 1u);
    CHECK(((struct p4_etask_owner_record *)identities.head)->lifetime.task_ref == 1u);

    Exec_CleanupETask(&process.pr_Task, active_base);
    CHECK(event_count == 4u);
    CHECK(events[0] == EVENT_BEGIN && events[1] == EVENT_RETAIN &&
          events[2] == EVENT_PUTMSG && events[3] == EVENT_PERMIT);
    CHECK(putmsg_calls == 1u && permit_calls == 1u && expunge_calls == 0u);
    CHECK(forbid_depth == 0u && spin_depth == 0u && gate_depth == 0u);
    CHECK(identities.count == 1u && free_calls == 0u && fatal_calls == 0u);

    /* The actual CleanupETask body ran with ASan poison active after PutMsg.
     * No subsequent access to the published ETask is allowed. IdentityRelease
     * compares the pointer as an opaque key and must not dereference it. */
    CHECK(krnP4ETaskIdentityRelease(child, active_base) == 1);
    CHECK(identities.head == NULL && identities.count == 0u);
    CHECK(free_calls == 1u && fatal_calls == 0u && storage_free_calls == 0u);
    __asan_unpoison_memory_region(child, sizeof(*child));
    bytes = (unsigned char *)child;
    for (index = 0; index < sizeof(*child); ++index)
        CHECK(bytes[index] == 0xddu);
    free(child_storage);
    free(parent_storage);
}

int main(void)
{
    test_cleanup_owner_transfer_before_visibility();
    CHECK(event_count == 4u && identities.count == 0u);
    printf("PASS actual CleanupETask owner hooks: %u checks\n", checks);
    return 0;
}
'''

identity_source = IDENTITY_PATH.read_text()
lifetime_source = LIFETIME_PATH.read_text()
binding_source = BINDING_PATH.read_text()

with tempfile.TemporaryDirectory(prefix="p4-etask-owner-hooks-") as temp:
    temp_path = Path(temp)
    generated = temp_path / "fixture.c"
    binary = temp_path / "fixture"
    generated.write_text(
        PRELUDE
        + '\n#include "p4_etask_identity.h"\n'
        + '#include "p4_etask_lifetime.h"\n'
        + '#include "p4_etask_identity_binding.h"\n'
        + identity_source + "\n"
        + lifetime_source + "\n"
        + binding_source + "\n"
        + "void\n" + cleanup + "\n"
        + TAIL
    )
    compiler = os.environ.get("CC", "clang")
    command = [
        compiler,
        "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
        "-Wconversion", "-Wpedantic", "-fsanitize=address,undefined",
        "-fno-omit-frame-pointer", "-I", str(EXEC_DIR),
        str(generated), "-o", str(binary),
    ]
    subprocess.run(command, check=True)
    subprocess.run([str(binary)], check=True)
    print("source sha256:")
    for name, digest in source_hashes.items():
        print(f"  {name}: {digest}")
    print("compiler command:", " ".join(command))
