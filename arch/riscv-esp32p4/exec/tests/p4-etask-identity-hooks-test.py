#!/usr/bin/env python3
"""Exercise actual Exec_InitETask/Exec_ExpungeETask with P4 identity hooks."""
from pathlib import Path
import hashlib
import os
import subprocess
import tempfile


REPO = Path(__file__).resolve().parents[4]
SOURCE = REPO / "rom/exec/exec_util.c"
source = SOURCE.read_text()


def extract_braced(text: str, signature: str) -> str:
    start = text.index(signature)
    opening = text.index("{", start)
    depth = 0
    state = "code"
    i = opening
    while i < len(text):
        c = text[i]
        n = text[i + 1] if i + 1 < len(text) else ""
        if state == "code":
            if c == "/" and n == "*":
                state = "block"
                i += 2
                continue
            if c == "/" and n == "/":
                state = "line"
                i += 2
                continue
            if c == '"':
                state = "string"
            elif c == "'":
                state = "char"
            elif c == "{":
                depth += 1
            elif c == "}":
                depth -= 1
                if depth == 0:
                    return text[start:i + 1]
        elif state == "block":
            if c == "*" and n == "/":
                state = "code"
                i += 2
                continue
        elif state == "line":
            if c == "\n":
                state = "code"
        elif state == "string":
            if c == "\\":
                i += 2
                continue
            if c == '"':
                state = "code"
        elif state == "char":
            if c == "\\":
                i += 2
                continue
            if c == "'":
                state = "code"
        i += 1
    raise ValueError(f"unterminated function after {signature!r}")


init_body = extract_braced(source, "Exec_InitETask(struct Task *task,")
expunge_body = extract_braced(source, "Exec_ExpungeETask(struct ETask *et,")
storage_body = extract_braced(source, "Exec_FreeETaskStorage(struct ETask *et,")
storage_body = storage_body.replace(
    "struct ExecBase *SysBase)",
    "struct ExecBase *SysBase __attribute__((unused)))",
    1,
)
source_hash = hashlib.sha256(source.encode()).hexdigest()

PRELUDE = r'''
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint32_t ULONG;
typedef int BOOL;
typedef void *APTR;
#define TRUE 1
#define FALSE 0
#define MEMF_PUBLIC 1u
#define MEMF_CLEAR 2u
#define TF_ETASK 0x0040u
#define EXECF_CPUAffinity 0x0001u
#define SIGB_CHILD 3u
#define SPINLOCK_MODE_WRITE 1u
#define P4_SRAMDATA
#define IntETask(et) ((struct IntETask *)(et))
#define PrivExecBase(base) (base)
#define GetETask(task) ((task)->tc_UnionETask.tc_ETask)
#define D(...) do { } while (0);
#define EXEC_PLATFORM_ETASK_RESERVE(et) identity_reserve((et))
#define EXEC_PLATFORM_ETASK_RELEASE(et) identity_release((et), SysBase)

struct Node {
    struct Node *ln_Succ;
    struct Node *ln_Pred;
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
struct Task {
    struct Node tc_Node;
    union { struct ETask *tc_ETask; } tc_UnionETask;
    uint32_t tc_Flags;
};
struct MsgPort {
    struct List mp_MsgList;
    uint32_t mp_SpinLock;
    struct Task *mp_SigTask;
    uint32_t mp_SigBit;
};
struct ETask {
    uint32_t et_UniqueID;
    struct Task *et_Parent;
    struct MinList et_Children;
    struct MsgPort et_TaskMsgPort;
    APTR et_TrapAlloc;
    uint32_t et_TrapAble;
    APTR et_Result2;
};
struct IntETask {
    struct ETask iet_ETask;
    uint32_t iet_TaskLock;
    APTR iet_CpuAffinity;
};
struct ExecBase {
    uint32_t ex_TaskID;
    uint32_t IntFlags;
    APTR TaskTrapAlloc;
};
void Exec_FreeETaskStorage(struct ETask *et, struct ExecBase *SysBase);

static unsigned checks;
static unsigned event_count;
static unsigned events[32];
static unsigned alloc_calls;
static unsigned free_calls;
static unsigned reserve_calls;
static unsigned release_calls;
static unsigned find_pid_calls;
static unsigned spin_init_calls;
static unsigned forbid_calls;
static unsigned permit_calls;
static int allocation_fails;
static int reserve_succeeds;
static int release_result = 1;
static int affinity_fails;
static int identity_live;
static uint32_t next_pid = 400u;
static struct Task *active_task;

enum { EVENT_RESERVE = 1u, EVENT_RELEASE, EVENT_FREE, EVENT_FREEVEC };

#define CHECK(condition) do { \
    ++checks; \
    if (!(condition)) { \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", \
                __FILE__, __LINE__, #condition); \
        exit(1); \
    } \
} while (0)

static void event(unsigned value)
{
    CHECK(event_count < sizeof(events) / sizeof(events[0]));
    events[event_count++] = value;
}

static void *AllocMem(size_t size, ULONG flags)
{
    CHECK(size == sizeof(struct IntETask));
    CHECK(flags == (MEMF_PUBLIC | MEMF_CLEAR));
    ++alloc_calls;
    if (allocation_fails)
        return NULL;
    return calloc(1u, size);
}

static void FreeMem(APTR memory, size_t size)
{
    CHECK(memory != NULL);
    CHECK(size == sizeof(struct IntETask));
    if (active_task != NULL)
    {
        CHECK(active_task->tc_UnionETask.tc_ETask == NULL);
        CHECK((active_task->tc_Flags & TF_ETASK) == 0u);
    }
    if (identity_live)
        CHECK(0 && "ETask freed before identity release");
    ++free_calls;
    event(EVENT_FREE);
    free(memory);
}

static void FreeVec(APTR memory)
{
    if (memory != NULL)
        free(memory);
    event(EVENT_FREEVEC);
}

static int identity_reserve(struct ETask *et)
{
    CHECK(et != NULL);
    CHECK(active_task != NULL);
    CHECK(active_task->tc_UnionETask.tc_ETask == et);
    CHECK((active_task->tc_Flags & TF_ETASK) == 0u);
    CHECK(!identity_live);
    ++reserve_calls;
    event(EVENT_RESERVE);
    if (!reserve_succeeds)
        return 0;
    et->et_UniqueID = next_pid++;
    identity_live = 1;
    return 1;
}

static int identity_release(struct ETask *et, struct ExecBase *base)
{
    CHECK(base != NULL);
    CHECK(et != NULL);
    CHECK(identity_live);
    CHECK(et->et_UniqueID != 0u);
    if (active_task != NULL)
    {
        CHECK(active_task->tc_UnionETask.tc_ETask == NULL);
        CHECK((active_task->tc_Flags & TF_ETASK) == 0u);
    }
    ++release_calls;
    event(EVENT_RELEASE);
    if (release_result)
        identity_live = 0;
    return release_result;
}

static int KrnGetCPUNumber(void) { return 1; }
static APTR KrnAllocCPUMask(void) { return affinity_fails ? NULL : malloc(4u); }
static void KrnGetCPUMask(int cpu, APTR mask) { CHECK(cpu == 1 && mask != NULL); }
static void EXEC_SPINLOCK_INIT(uint32_t *lock)
{
    CHECK(lock != NULL);
    CHECK(identity_live);
    CHECK(active_task != NULL);
    CHECK(active_task->tc_UnionETask.tc_ETask != NULL);
    ++spin_init_calls;
}
static void EXEC_SPINLOCK_LOCK(uint32_t *lock, void *hook, unsigned mode)
{
    CHECK(lock != NULL && hook == NULL && mode == SPINLOCK_MODE_WRITE);
}
static void EXEC_SPINLOCK_UNLOCK(uint32_t *lock) { CHECK(lock != NULL); }
struct ETask *FindTaskByPID(uint32_t pid)
{
    (void)pid;
    ++find_pid_calls;
    return NULL;
}
static void Forbid(void) { ++forbid_calls; }
static void Permit(void) { ++permit_calls; }
static void InitMsgPort(struct MsgPort *port)
{
    memset(port, 0, sizeof(*port));
}
static void NewMinList(struct MinList *list)
{
    list->mlh_Head = (struct Node *)&list->mlh_Tail;
    list->mlh_Tail = NULL;
    list->mlh_TailPred = (struct Node *)&list->mlh_Head;
}
static void AddHead(struct List *list, struct Node *node)
{
    CHECK(list != NULL && node != NULL);
}
static void stub_current_symbols(void)
{
    (void)FindTaskByPID(0u);
}
'''

TESTS = r'''
static void reset_state(void)
{
    event_count = 0;
    alloc_calls = 0;
    free_calls = 0;
    reserve_calls = 0;
    release_calls = 0;
    find_pid_calls = 0;
    spin_init_calls = 0;
    forbid_calls = 0;
    permit_calls = 0;
    allocation_fails = 0;
    reserve_succeeds = 1;
    release_result = 1;
    affinity_fails = 0;
    identity_live = 0;
    next_pid = 400u;
    active_task = NULL;
}

static void init_task(struct Task *task)
{
    memset(task, 0, sizeof(*task));
    task->tc_Node.ln_Name = (char *)"fixture-task";
}

static struct ExecBase make_base(void)
{
    struct ExecBase base = { 55u, 0u, NULL };
    return base;
}

static void test_alloc_failure(void)
{
    struct Task task;
    struct ExecBase base = make_base();
    reset_state();
    init_task(&task);
    allocation_fails = 1;
    active_task = &task;
    CHECK(!Exec_InitETask(&task, NULL, &base));
    CHECK(task.tc_UnionETask.tc_ETask == NULL);
    CHECK((task.tc_Flags & TF_ETASK) == 0u);
    CHECK(alloc_calls == 1u && reserve_calls == 0u && free_calls == 0u);
    CHECK(!identity_live && find_pid_calls == 0u);
}

static void test_reserve_failure_rolls_back(void)
{
    struct Task task;
    struct ExecBase base = make_base();
    reset_state();
    init_task(&task);
    reserve_succeeds = 0;
    active_task = &task;
    CHECK(!Exec_InitETask(&task, NULL, &base));
    CHECK(task.tc_UnionETask.tc_ETask == NULL);
    CHECK((task.tc_Flags & TF_ETASK) == 0u);
    CHECK(alloc_calls == 1u && reserve_calls == 1u);
    CHECK(release_calls == 0u && free_calls == 1u);
    CHECK(event_count == 2u && events[0] == EVENT_RESERVE &&
          events[1] == EVENT_FREE);
    CHECK(!identity_live && find_pid_calls == 0u);
}

static void test_affinity_failure_releases_once_before_free(void)
{
    struct Task task;
    struct ExecBase base = make_base();
    reset_state();
    init_task(&task);
    base.IntFlags = EXECF_CPUAffinity;
    affinity_fails = 1;
    active_task = &task;
    CHECK(!Exec_InitETask(&task, NULL, &base));
    CHECK(task.tc_UnionETask.tc_ETask == NULL);
    CHECK((task.tc_Flags & TF_ETASK) == 0u);
    CHECK(alloc_calls == 1u && reserve_calls == 1u &&
          release_calls == 1u && free_calls == 1u);
    CHECK(spin_init_calls == 1u);
    CHECK(event_count == 3u && events[0] == EVENT_RESERVE &&
          events[1] == EVENT_RELEASE && events[2] == EVENT_FREE);
    CHECK(!identity_live && find_pid_calls == 0u);
}

static void test_success_retains_pid_until_expunge(void)
{
    struct Task task;
    struct ExecBase base = make_base();
    struct ETask *et;
    uint32_t pid;
    reset_state();
    init_task(&task);
    active_task = &task;
    CHECK(Exec_InitETask(&task, NULL, &base));
    et = task.tc_UnionETask.tc_ETask;
    CHECK(et != NULL && (task.tc_Flags & TF_ETASK) != 0u);
    CHECK(et->et_UniqueID == 400u && identity_live);
    pid = et->et_UniqueID;
    CHECK(reserve_calls == 1u && release_calls == 0u);
    CHECK(find_pid_calls == 0u && base.ex_TaskID == 55u);
    CHECK(spin_init_calls == 1u);
    CHECK(forbid_calls == 1u && permit_calls == 1u);
    CHECK(event_count == 1u && events[0] == EVENT_RESERVE);

    /* The reservation remains live after Init returns, through the actual
     * Expunge entrypoint. Release runs before ETask storage is freed. */
    CHECK(identity_live && et->et_UniqueID == pid);
    active_task = NULL; /* Task may already be gone when child metadata dies. */
    Exec_ExpungeETask(et, &base);
    CHECK(release_calls == 1u && free_calls == 1u && !identity_live);
    CHECK(event_count == 4u && events[1] == EVENT_RELEASE &&
          events[2] == EVENT_FREEVEC && events[3] == EVENT_FREE);
    CHECK(find_pid_calls == 0u);
}

static void test_release_zero_defers_all_storage_free(void)
{
    struct Task task;
    struct ExecBase base = make_base();
    struct ETask *et;
    reset_state();
    init_task(&task);
    active_task = &task;
    CHECK(Exec_InitETask(&task, NULL, &base));
    et = task.tc_UnionETask.tc_ETask;
    CHECK(et != NULL && identity_live);
    active_task = NULL;

    release_result = 0;
    Exec_ExpungeETask(et, &base);
    CHECK(release_calls == 1u && identity_live);
    CHECK(free_calls == 0u && event_count == 2u);
    CHECK(events[0] == EVENT_RESERVE && events[1] == EVENT_RELEASE);

    release_result = 1;
    Exec_ExpungeETask(et, &base);
    CHECK(release_calls == 2u && !identity_live);
    CHECK(free_calls == 1u && event_count == 5u);
    CHECK(events[2] == EVENT_RELEASE && events[3] == EVENT_FREEVEC &&
          events[4] == EVENT_FREE);
    CHECK(find_pid_calls == 0u);
}

int main(void)
{
    test_alloc_failure();
    test_reserve_failure_rolls_back();
    test_affinity_failure_releases_once_before_free();
    test_success_retains_pid_until_expunge();
    test_release_zero_defers_all_storage_free();
    printf("PASS actual ETask identity hooks: %u checks; source=%s\n",
           checks, SOURCE_HASH);
    return 0;
}
'''

PRELUDE = PRELUDE.replace(
    "static void stub_current_symbols(void)\n{\n    (void)FindTaskByPID(0u);\n}\n",
    "",
)

with tempfile.TemporaryDirectory(prefix="p4-etask-id-hooks-") as temp:
    temp_path = Path(temp)
    generated = temp_path / "fixture.c"
    binary = temp_path / "fixture"
    generated.write_text(
        PRELUDE
        + "\n#define __AROSEXEC_SMP__ 1\n"
        + "#define SOURCE_HASH \"" + source_hash + "\"\n"
        + "BOOL\n" + init_body + "\n"
        + "void\n" + expunge_body + "\n"
        + "void\n" + storage_body + "\n"
        + TESTS
    )
    compiler = os.environ.get("CC", "clang")
    command = [
        compiler,
        "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
        "-Wconversion", "-Wpedantic", "-fsanitize=address,undefined",
        "-fno-omit-frame-pointer", str(generated), "-o", str(binary),
    ]
    subprocess.run(command, check=True)
    subprocess.run([str(binary)], check=True)
    print("exec_util.c sha256:", source_hash)
    print("compiler command:", " ".join(command))
