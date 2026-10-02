#!/usr/bin/env python3
"""Run extracted Exec task-creation paths under host ASan/UBSan."""
from pathlib import Path
import subprocess
import tempfile


repo = Path(__file__).resolve().parents[4]
newadd_source = (repo / "rom/exec/newaddtask.c").read_text()
newcreate_source = (repo / "rom/exec/newcreatetaska.c").read_text()
exec_source = (repo / "rom/exec/exec_util.c").read_text()


def libfunc_body(source: str, signature: str) -> str:
    start = source.index(signature)
    body_start = source.index("AROS_LIBFUNC_INIT", start) + len("AROS_LIBFUNC_INIT")
    body_end = source.index("AROS_LIBFUNC_EXIT", body_start)
    return source[body_start:body_end]


newadd_body = libfunc_body(newadd_source, "AROS_LH4(APTR, NewAddTask,")
newcreate_body = libfunc_body(
    newcreate_source, "AROS_LH1(struct Task *, NewCreateTaskA,"
)
template_start = newcreate_source.index("struct newMemList\n")
template_end = newcreate_source.index("\n/****", template_start)
mem_template = newcreate_source[template_start:template_end]

etask_start = exec_source.index("BOOL\nExec_InitETask(")
etask_end = exec_source.index("    et->et_Parent = parent;", etask_start)
etask_prefix = (
    exec_source[etask_start:etask_end]
    + "    (void)parent;\n    return TRUE;\n}\n"
)

fixture = r'''
#include <inttypes.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint32_t ULONG;
typedef uint16_t UWORD;
typedef uint8_t UBYTE;
typedef int32_t LONG;
typedef intptr_t IPTR;
typedef void *APTR;
typedef char *STRPTR;
typedef const char *CONST_STRPTR;
typedef int BOOL;
typedef void *RAWARG;
typedef void (*RawFmtFunc)(void);

#define TRUE 1
#define FALSE 0
#define MEMF_PUBLIC 1u
#define MEMF_CLEAR 2u
#define TF_ETASK 0x40u
#define TF_STACKCHK 0x80u
#define TF_SWITCH 0x100u
#define TF_LAUNCH 0x200u
#define EXECF_CPUAffinity 0x01u
#define EXECF_StackSnoop 0x02u
#define __AROSEXEC_SMP__ 1
#define AROS_STACKSIZE 256u
#define SP_OFFSET 0u
#define AROS_ALIGN(value) (value)
#define EXEC_SPINLOCK_INIT(lock) (*(lock) = 0u)
#define IntETask(et) ((struct IntETask *)(et))
#define PrivExecBase(base) (base)
#define D(...) do { (void)0; } while (0);
#define DADDTASK(...) ((void)0)
#define ASSERT_VALID_PTR(ptr) ((void)0)
#define NEWLIST(list) InitList(list)
#define RAWFMTFUNC_STRING ((RawFmtFunc)0)

#define TAG_DONE 0u
#define TASKTAG_ERROR 1u
#define TASKTAG_PC 2u
#define TASKTAG_FINALPC 3u
#define TASKTAG_STACKSIZE 4u
#define TASKTAG_NAME 5u
#define TASKTAG_USERDATA 6u
#define TASKTAG_PRI 7u
#define TASKTAG_TASKMSGPORT 8u
#define TASKTAG_FLAGS 9u
#define TASKTAG_TCBEXTRASIZE 10u
#define TASKTAG_PRELAUNCHHOOK 11u
#define TASKERROR_OK 0u
#define TASKERROR_NOMEMORY 1u
#define NT_TASK 1u
#define TS_ADDED 2u
#define TS_RUN 3u
#define TEST_SIGBIT 5u

static unsigned checks;
#define CHECK(condition) do { \
    checks++; \
    if (!(condition)) { \
        fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); \
        exit(1); \
    } \
} while (0)

struct Node {
    struct Node *ln_Succ;
    struct Node *ln_Pred;
    UBYTE ln_Type;
    int8_t ln_Pri;
    char *ln_Name;
};

struct List {
    struct Node *lh_Head;
    struct Node *lh_Tail;
    struct Node *lh_TailPred;
};

struct MemEntry {
    union {
        ULONG meu_Reqs;
        APTR meu_Addr;
    } me_Un;
    IPTR me_Length;
};
#define me_Reqs me_Un.meu_Reqs
#define me_Addr me_Un.meu_Addr

struct MemList {
    struct Node ml_Node;
    UWORD ml_NumEntries;
    struct MemEntry ml_ME[1];
};

struct TagItem {
    ULONG ti_Tag;
    IPTR ti_Data;
};

struct Hook { ULONG dummy; };
struct ETask { ULONG dummy; };
struct IntETask {
    struct ETask pub;
    void *iet_CpuAffinity;
    ULONG iet_TaskLock;
};

struct Task {
    struct Node tc_Node;
    struct List tc_MemEntry;
    LONG tc_IDNestCnt;
    LONG tc_TDNestCnt;
    ULONG tc_State;
    ULONG tc_Flags;
    ULONG tc_SigWait;
    ULONG tc_SigRecvd;
    ULONG tc_SigExcept;
    ULONG tc_SigAlloc;
    APTR tc_TrapCode;
    APTR tc_ExceptCode;
    APTR tc_SPLower;
    APTR tc_SPUpper;
    APTR tc_SPReg;
    union { struct ETask *tc_ETask; } tc_UnionETask;
    APTR tc_UserData;
    ULONG tc_Switch;
    ULONG tc_Launch;
};

struct MsgPort {
    struct Node mp_Node;
    ULONG mp_SigBit;
    struct Task *mp_SigTask;
    struct List mp_MsgList;
};

struct ExecBase {
    ULONG IntFlags;
    ULONG TaskSigAlloc;
    APTR TaskTrapCode;
    APTR TaskExceptCode;
    APTR TaskExitCode;
};

static struct ExecBase execBase;
static struct ExecBase *SysBase = &execBase;
static struct Task parentTask;
static struct Task *currentTask;

struct Allocation {
    void *ptr;
    size_t size;
    int live;
};
static struct Allocation allocations[256];
static size_t allocation_count;
static size_t live_count;
static size_t free_count;

static void *AllocMem(size_t size, unsigned flags)
{
    void *ptr;
    CHECK(size != 0);
    CHECK(allocation_count < sizeof(allocations) / sizeof(allocations[0]));
    ptr = malloc(size);
    if (!ptr)
        return NULL;
    if (flags & MEMF_CLEAR)
        memset(ptr, 0, size);
    allocations[allocation_count].ptr = ptr;
    allocations[allocation_count].size = size;
    allocations[allocation_count].live = 1;
    allocation_count++;
    live_count++;
    return ptr;
}

static void FreeMem(void *ptr, size_t size)
{
    size_t i;
    if (!ptr) {
        CHECK(size == 0);
        return;
    }
    for (i = 0; i < allocation_count; i++) {
        if (allocations[i].ptr == ptr && allocations[i].live) {
            CHECK(allocations[i].size == size);
            allocations[i].live = 0;
            live_count--;
            free_count++;
            free(ptr);
            return;
        }
    }
    CHECK(0 && "free of unknown or already freed allocation");
}

static void InitList(struct List *list)
{
    list->lh_Head = (struct Node *)&list->lh_Tail;
    list->lh_Tail = NULL;
    list->lh_TailPred = (struct Node *)&list->lh_Head;
}

static void AddHead(struct List *list, struct Node *node)
{
    node->ln_Succ = list->lh_Head;
    node->ln_Pred = (struct Node *)&list->lh_Head;
    list->lh_Head->ln_Pred = node;
    list->lh_Head = node;
}

static void AddTail(struct List *list, struct Node *node)
{
    node->ln_Succ = (struct Node *)&list->lh_Tail;
    node->ln_Pred = list->lh_TailPred;
    list->lh_TailPred->ln_Succ = node;
    list->lh_TailPred = node;
}

static void Remove(struct Node *node)
{
    node->ln_Pred->ln_Succ = node->ln_Succ;
    node->ln_Succ->ln_Pred = node->ln_Pred;
}

static size_t memlist_size(UWORD count)
{
    return sizeof(struct MemList) - sizeof(struct MemEntry) +
           sizeof(struct MemEntry) * count;
}

static struct MemList *watchedPrimary;
static struct MsgPort **watchedPortSlot;
static int inspectFailedPrimary;
static int failedPrimaryInspected;
static int capturePrimary;
static size_t generatedNameEntriesFreed;

static struct MemList *NewAllocEntry(struct MemList *template, ULONG *flags)
{
    UWORD i;
    struct MemList *result;
    result = AllocMem(memlist_size(template->ml_NumEntries), MEMF_PUBLIC);
    if (!result) {
        if (flags)
            *flags = MEMF_PUBLIC;
        return NULL;
    }
    memset(&result->ml_Node, 0, sizeof(result->ml_Node));
    result->ml_NumEntries = template->ml_NumEntries;
    for (i = 0; i < template->ml_NumEntries; i++) {
        result->ml_ME[i].me_Reqs = template->ml_ME[i].me_Reqs;
        result->ml_ME[i].me_Length = template->ml_ME[i].me_Length;
        result->ml_ME[i].me_Addr = NULL;
        if (template->ml_ME[i].me_Length) {
            result->ml_ME[i].me_Addr = AllocMem(template->ml_ME[i].me_Length,
                                                 template->ml_ME[i].me_Reqs);
            if (!result->ml_ME[i].me_Addr) {
                while (i > 0) {
                    i--;
                    FreeMem(result->ml_ME[i].me_Addr,
                            result->ml_ME[i].me_Length);
                }
                FreeMem(result, memlist_size(result->ml_NumEntries));
                if (flags)
                    *flags = template->ml_ME[i].me_Reqs;
                return NULL;
            }
        }
    }
    if (flags)
        *flags = 0;
    if (capturePrimary) {
        watchedPrimary = result;
        capturePrimary = 0;
    }
    return result;
}

static void FreeEntry(struct MemList *entry)
{
    UWORD i;
    if (entry == watchedPrimary && inspectFailedPrimary) {
        struct Task *task = (struct Task *)entry->ml_ME[0].me_Addr;
        CHECK(watchedPortSlot && *watchedPortSlot == NULL);
        CHECK(task->tc_Node.ln_Name == NULL);
        CHECK(task->tc_MemEntry.lh_Head == &entry->ml_Node);
        CHECK(task->tc_MemEntry.lh_TailPred == &entry->ml_Node);
        CHECK(entry->ml_Node.ln_Succ ==
              (struct Node *)&task->tc_MemEntry.lh_Tail);
        failedPrimaryInspected++;
    }
    if (entry != watchedPrimary && entry->ml_NumEntries == 1 &&
        entry->ml_ME[0].me_Addr)
        generatedNameEntriesFreed++;
    for (i = 0; i < entry->ml_NumEntries; i++)
        FreeMem(entry->ml_ME[i].me_Addr, entry->ml_ME[i].me_Length);
    FreeMem(entry, memlist_size(entry->ml_NumEntries));
}

static int maskAllocationFails;
static int prepareContextFails;
static int signalAllocationFails;
static unsigned etaskCleanupCalls;
static unsigned launchCalls;
static unsigned publicationChecks;
static struct MsgPort **launchPortSlot;

static int KrnGetCPUNumber(void) { return 1; }
static void *KrnAllocCPUMask(void)
{
    return maskAllocationFails ? NULL : AllocMem(sizeof(uint32_t), MEMF_CLEAR);
}
static void KrnGetCPUMask(int cpu, void *mask)
{
    CHECK(cpu == 1);
    *(uint32_t *)mask = 1u << cpu;
}

static BOOL Exec_InitETask(struct Task *task, struct Task *parent,
                           struct ExecBase *base);
static BOOL InitETask(struct Task *task, struct Task *parent)
{
    return Exec_InitETask(task, parent, SysBase);
}
static void CleanupETask(struct Task *task)
{
    struct IntETask *et;
    etaskCleanupCalls++;
    if (!(task->tc_Flags & TF_ETASK) || !task->tc_UnionETask.tc_ETask)
        return;
    et = IntETask(task->tc_UnionETask.tc_ETask);
    if (et->iet_CpuAffinity)
        FreeMem(et->iet_CpuAffinity, sizeof(uint32_t));
    FreeMem(et, sizeof(*et));
    task->tc_UnionETask.tc_ETask = NULL;
    task->tc_Flags &= ~TF_ETASK;
}

static BOOL PrepareContext(struct Task *task, APTR initialPC, APTR finalPC,
                           const struct TagItem *tagList,
                           struct ExecBase *base)
{
    (void)task;
    (void)initialPC;
    (void)finalPC;
    (void)tagList;
    (void)base;
    return !prepareContextFails;
}

static IPTR LibGetTagData(ULONG tag, IPTR defaultValue,
                          const struct TagItem *tags)
{
    (void)tag;
    (void)tags;
    return defaultValue;
}

static struct TagItem *LibNextTagItem(struct TagItem **state)
{
    struct TagItem *tag;
    if (!state || !*state || (*state)->ti_Tag == TAG_DONE)
        return NULL;
    tag = *state;
    *state = tag + 1;
    return tag;
}

static LONG AllocTaskSignal(struct Task *task, LONG requested,
                            struct ExecBase *base)
{
    (void)task;
    (void)requested;
    (void)base;
    return signalAllocationFails ? -1 : (LONG)TEST_SIGBIT;
}

static void InitMsgPort(struct MsgPort *port)
{
    memset(port, 0, sizeof(*port));
    InitList(&port->mp_MsgList);
}

static void RawDoFmt(const char *format, RAWARG raw, RawFmtFunc output,
                     char *destination)
{
    const char *parentName = (const char *)(uintptr_t)*(IPTR *)raw;
    (void)output;
    CHECK(strcmp(format, "%s.subtask") == 0);
    snprintf(destination, strlen(parentName) + 10, "%s.subtask", parentName);
}

static void TaskLaunch(struct Task *parent, struct Task *task,
                       struct Hook *preLaunchHook)
{
    (void)parent;
    (void)preLaunchHook;
    launchCalls++;
    if (launchPortSlot) {
        CHECK(*launchPortSlot != NULL);
        CHECK((*launchPortSlot)->mp_SigTask == task);
        CHECK((*launchPortSlot)->mp_SigBit == TEST_SIGBIT);
        publicationChecks++;
    }
}

#define GET_THIS_TASK (currentTask)
'''

newadd_function = (
    "static APTR NewAddTask(struct Task *task, APTR initialPC, APTR finalPC,\n"
    "                       struct TagItem *tagList)\n{\n"
    + newadd_body
    + "\n}\n"
)
newcreate_function = (
    "static struct Task *NewCreateTaskA(struct TagItem *tags)\n{\n"
    + newcreate_body
    + "\n}\n"
)

main = r'''
static void resetCase(void)
{
    CHECK(live_count == 0);
    memset(&execBase, 0, sizeof(execBase));
    memset(&parentTask, 0, sizeof(parentTask));
    execBase.IntFlags = EXECF_CPUAffinity;
    execBase.TaskSigAlloc = 0xffffffffu;
    execBase.TaskExitCode = (APTR)(uintptr_t)0x1000u;
    SysBase = &execBase;
    parentTask.tc_Node.ln_Name = "parent";
    parentTask.tc_Node.ln_Pri = 10;
    parentTask.tc_State = TS_RUN;
    currentTask = &parentTask;
    maskAllocationFails = 0;
    prepareContextFails = 0;
    signalAllocationFails = 0;
    etaskCleanupCalls = 0;
    launchCalls = 0;
    publicationChecks = 0;
    launchPortSlot = NULL;
    watchedPrimary = NULL;
    watchedPortSlot = NULL;
    inspectFailedPrimary = 0;
    failedPrimaryInspected = 0;
    capturePrimary = 0;
    generatedNameEntriesFreed = 0;
}

static void initCallerTask(struct Task *task, struct MemList *callerList)
{
    memset(task, 0, sizeof(*task));
    memset(callerList, 0, sizeof(*callerList));
    callerList->ml_NumEntries = 0;
    InitList(&task->tc_MemEntry);
    AddTail(&task->tc_MemEntry, &callerList->ml_Node);
    task->tc_SPLower = (APTR)(uintptr_t)0x2000u;
    task->tc_SPUpper = (APTR)(uintptr_t)0x2100u;
    task->tc_SPReg = task->tc_SPUpper;
}

static void checkOnlyCallerList(struct Task *task, struct MemList *callerList)
{
    CHECK(task->tc_MemEntry.lh_Head == &callerList->ml_Node);
    CHECK(task->tc_MemEntry.lh_TailPred == &callerList->ml_Node);
    CHECK(callerList->ml_Node.ln_Succ ==
          (struct Node *)&task->tc_MemEntry.lh_Tail);
    CHECK(callerList->ml_Node.ln_Pred ==
          (struct Node *)&task->tc_MemEntry.lh_Head);
}

int main(void)
{
    struct Task task;
    struct MemList callerList;
    APTR result;
    struct MsgPort *port;
    ULONG error;
    struct TagItem maskFailureTags[] = {
        { TASKTAG_ERROR, (IPTR)&error },
        { TASKTAG_TASKMSGPORT, (IPTR)&port },
        { TAG_DONE, 0 }
    };
    struct TagItem successTags[] = {
        { TASKTAG_NAME, (IPTR)"named-child" },
        { TASKTAG_PC, (IPTR)0x3000u },
        { TASKTAG_ERROR, (IPTR)&error },
        { TASKTAG_TASKMSGPORT, (IPTR)&port },
        { TAG_DONE, 0 }
    };
    struct Task *createdTask;
    struct MemList *primary;

    /* Mask allocation failure frees only the generated-name entry. */
    resetCase();
    initCallerTask(&task, &callerList);
    maskAllocationFails = 1;
    result = NewAddTask(&task, NULL, NULL, NULL);
    CHECK(result == NULL);
    CHECK(task.tc_Node.ln_Name == NULL);
    checkOnlyCallerList(&task, &callerList);
    CHECK(generatedNameEntriesFreed == 1);
    CHECK(etaskCleanupCalls == 0);
    CHECK(launchCalls == 0);
    CHECK(live_count == 0);

    /* PrepareContext failure also restores the caller's name and list. */
    resetCase();
    initCallerTask(&task, &callerList);
    prepareContextFails = 1;
    result = NewAddTask(&task, NULL, NULL, NULL);
    CHECK(result == NULL);
    CHECK(task.tc_Node.ln_Name == NULL);
    checkOnlyCallerList(&task, &callerList);
    CHECK(generatedNameEntriesFreed == 1);
    CHECK(etaskCleanupCalls == 1);
    CHECK(launchCalls == 0);
    CHECK(live_count == 0);

    /* NewCreateTaskA clears an already-published port before freeing it. */
    resetCase();
    maskAllocationFails = 1;
    port = (struct MsgPort *)(uintptr_t)0xdeadbeefu;
    error = 0xffffffffu;
    watchedPortSlot = &port;
    capturePrimary = 1;
    inspectFailedPrimary = 1;
    result = NewCreateTaskA(maskFailureTags);
    CHECK(result == NULL);
    CHECK(port == NULL);
    CHECK(error == TASKERROR_NOMEMORY);
    CHECK(failedPrimaryInspected == 1);
    CHECK(generatedNameEntriesFreed == 1);
    CHECK(etaskCleanupCalls == 0);
    CHECK(launchCalls == 0);
    CHECK(live_count == 0);

    /* Successful launch still sees the port before the child can run. */
    resetCase();
    port = NULL;
    error = 0xffffffffu;
    launchPortSlot = &port;
    result = NewCreateTaskA(successTags);
    CHECK(result != NULL);
    CHECK(error == TASKERROR_OK);
    CHECK(port != NULL && port->mp_SigTask == result);
    CHECK(launchCalls == 1);
    CHECK(publicationChecks == 1);
    createdTask = result;
    primary = (struct MemList *)createdTask->tc_MemEntry.lh_Head;
    CHECK(primary->ml_NumEntries == 4);
    CHECK(strcmp(createdTask->tc_Node.ln_Name, "named-child") == 0);
    launchPortSlot = NULL;
    CleanupETask(createdTask);
    port = NULL;
    FreeEntry(primary);
    CHECK(live_count == 0);

    printf("PASS: %u task-creation failure/publication checks\n", checks);
    return 0;
}
'''

prelude = r'''
static BOOL Exec_InitETask(struct Task *task, struct Task *parent,
                           struct ExecBase *base);
'''

program = (
    fixture
    + prelude
    + mem_template
    + "\n"
    + etask_prefix
    + "\n"
    + r'''
static APTR NewAddTask(struct Task *task, APTR initialPC, APTR finalPC,
                       struct TagItem *tagList);
'''
    + newadd_function
    + newcreate_function
    + main
)

def compile_and_run(test_program: str, optimization: str = "") -> None:
    with tempfile.TemporaryDirectory(prefix="p4-task-create-failure-") as tmp:
        src = Path(tmp) / "test.c"
        binary = Path(tmp) / "test"
        src.write_text(test_program)
        command = [
            "clang",
            "-std=gnu11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-fsanitize=address,undefined",
            "-g",
        ]
        if optimization:
            command.append(optimization)
        command.extend([str(src), "-o", str(binary)])
        subprocess.run(command, check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    compile_and_run(program)
