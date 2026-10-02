#!/usr/bin/env python3
"""Compile the actual NewCreateTaskA body against instrumented host stubs."""

from pathlib import Path
import subprocess
import tempfile


repo = Path(__file__).resolve().parents[4]
source = (repo / "rom/exec/newcreatetaska.c").read_text()


def libfunc_body(text: str, signature: str) -> str:
    start = text.index(signature)
    body_start = text.index("AROS_LIBFUNC_INIT", start) + len("AROS_LIBFUNC_INIT")
    body_end = text.index("AROS_LIBFUNC_EXIT", body_start)
    return text[body_start:body_end]


body = libfunc_body(source, "AROS_LH1(struct Task *, NewCreateTaskA,")
template_start = source.index("struct newMemList\n")
template_end = source.index("\n/****", template_start)
mem_template = source[template_start:template_end]

fixture = r'''
#define _DEFAULT_SOURCE 1
#include <inttypes.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

typedef uint32_t ULONG;
typedef uint16_t UWORD;
typedef uint8_t UBYTE;
typedef int32_t LONG;
typedef intptr_t IPTR;
typedef void *APTR;
typedef char *STRPTR;
typedef int BOOL;
typedef void *RAWARG;
typedef void (*RawFmtFunc)(void);

#define TRUE 1
#define FALSE 0
#define MEMF_PUBLIC UINT32_C(1)
#define MEMF_CLEAR UINT32_C(2)
#define TF_ETASK UINT32_C(0x40)
#define TF_STACKCHK UINT32_C(0x80)
#define EXECF_CPUAffinity UINT32_C(0x01)
#define EXECF_StackSnoop UINT32_C(0x02)
#define AROS_STACKSIZE UINT32_C(256)
#define SP_OFFSET UINT32_C(0)
#define AROS_ALIGN(value) (value)
#define IntETask(et) ((struct IntETask *)(et))
#define PrivExecBase(base) (base)
#define DADDTASK(...) ((void)0)
#define ASSERT_VALID_PTR(ptr) ((void)0)
#define NEWLIST(list) InitList(list)
#define RAWFMTFUNC_STRING ((RawFmtFunc)0)

#if TEST_SMP
#define __AROSEXEC_SMP__ 1
#define __AROSPLATFORM_SMP__ 1
#endif

#define TAG_DONE UINT32_C(0)
#define TASKTAG_ERROR UINT32_C(1)
#define TASKTAG_PC UINT32_C(2)
#define TASKTAG_FINALPC UINT32_C(3)
#define TASKTAG_STACKSIZE UINT32_C(4)
#define TASKTAG_NAME UINT32_C(5)
#define TASKTAG_USERDATA UINT32_C(6)
#define TASKTAG_PRI UINT32_C(7)
#define TASKTAG_TASKMSGPORT UINT32_C(8)
#define TASKTAG_FLAGS UINT32_C(9)
#define TASKTAG_TCBEXTRASIZE UINT32_C(10)
#define TASKERROR_OK UINT32_C(0)
#define TASKERROR_NOMEMORY UINT32_C(1)
#define NT_TASK UINT8_C(1)
#define TS_ADDED UINT32_C(2)
#define SPINLOCK_DIRTY UINT32_C(0xdeadbeef)
#define SPINLOCK_READY UINT32_C(0x51a7c0de)
#define TEST_SIGBIT 5

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
    union { ULONG meu_Reqs; APTR meu_Addr; } me_Un;
    IPTR me_Length;
};
#define me_Reqs me_Un.meu_Reqs
#define me_Addr me_Un.meu_Addr

struct MemList {
    struct Node ml_Node;
    UWORD ml_NumEntries;
    struct MemEntry ml_ME[1];
};

struct TagItem { ULONG ti_Tag; IPTR ti_Data; };
struct Hook { ULONG unused; };
struct ETask { ULONG unused; };
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
    ULONG tc_SpinLock;
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
static size_t page_size;

enum event {
    EV_NEW_ALLOC_ENTRY = 1,
    EV_SPIN_INIT,
    EV_ALLOC_TASK_SIGNAL,
    EV_NEW_ADD_TASK,
    EV_FREE_ENTRY
};

static enum event events[32];
static size_t event_count;
static unsigned spin_init_calls;
static unsigned alloc_signal_calls;
static unsigned newadd_calls;
static unsigned free_entry_calls;
static unsigned free_slot_checks;
static int fail_new_alloc_entry;
static int fail_signal_allocation;
static int fail_newadd_task;
static int poison_task_after_newadd;
static int task_page_poisoned;
static int expect_slot_clear_at_free;
static struct MsgPort **watched_slot;
static struct Task *allocated_task;
static void *allocated_task_page;
static struct MemList *primary_entry;

struct Allocation {
    void *ptr;
    size_t size;
    int is_mapping;
    int live;
};
static struct Allocation allocations[128];
static size_t allocation_count;
static size_t live_count;

static void record_event(enum event value)
{
    CHECK(event_count < sizeof(events) / sizeof(events[0]));
    events[event_count++] = value;
}

static size_t memlist_size(UWORD count)
{
    return sizeof(struct MemList) - sizeof(struct MemEntry) +
           sizeof(struct MemEntry) * (size_t)count;
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

static void *AllocMem(size_t size, ULONG flags)
{
    void *ptr;
    int is_mapping = 0;

    CHECK(size != 0U);
    CHECK(allocation_count < sizeof(allocations) / sizeof(allocations[0]));
    if (size == sizeof(struct Task))
    {
        ptr = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANON, -1, 0);
        if (ptr == MAP_FAILED)
            return NULL;
        is_mapping = 1;
        memset(ptr, 0, size);
        /* Deliberately violate the MEMF_CLEAR assumption for this field. */
        ((struct Task *)ptr)->tc_SpinLock = SPINLOCK_DIRTY;
        allocated_task = (struct Task *)ptr;
        allocated_task_page = ptr;
    }
    else
    {
        ptr = malloc(size);
        if (ptr == NULL)
            return NULL;
        if (flags & MEMF_CLEAR)
            memset(ptr, 0, size);
    }

    allocations[allocation_count].ptr = ptr;
    allocations[allocation_count].size = size;
    allocations[allocation_count].is_mapping = is_mapping;
    allocations[allocation_count].live = 1;
    allocation_count++;
    live_count++;
    return ptr;
}

static void FreeMem(void *ptr, size_t size)
{
    size_t i;

    if (ptr == NULL)
    {
        CHECK(size == 0U);
        return;
    }
    for (i = 0; i < allocation_count; ++i)
    {
        if (allocations[i].ptr == ptr && allocations[i].live)
        {
            CHECK(allocations[i].size == size);
            allocations[i].live = 0;
            live_count--;
            if (allocations[i].is_mapping)
            {
                if (task_page_poisoned)
                {
                    CHECK(mprotect(ptr, page_size,
                                   PROT_READ | PROT_WRITE) == 0);
                    task_page_poisoned = 0;
                }
                CHECK(munmap(ptr, page_size) == 0);
            }
            else
            {
                free(ptr);
            }
            return;
        }
    }
    CHECK(0 && "free of unknown or already freed allocation");
}

void EXEC_SPINLOCK_INIT_MOCK(ULONG *lock)
{
    CHECK(lock != NULL);
    CHECK(*lock == SPINLOCK_DIRTY);
    *lock = SPINLOCK_READY;
    spin_init_calls++;
    record_event(EV_SPIN_INIT);
}
#if TEST_SMP
#define EXEC_SPINLOCK_INIT(lock) EXEC_SPINLOCK_INIT_MOCK((ULONG *)(lock))
#endif

static struct MemList *NewAllocEntry(struct MemList *template, ULONG *flags)
{
    UWORD i;
    struct MemList *result;

    record_event(EV_NEW_ALLOC_ENTRY);
    if (fail_new_alloc_entry)
    {
        if (flags != NULL)
            *flags = MEMF_PUBLIC;
        return NULL;
    }
    result = AllocMem(memlist_size(template->ml_NumEntries), MEMF_PUBLIC);
    if (result == NULL)
        return NULL;
    memset(&result->ml_Node, 0, sizeof(result->ml_Node));
    result->ml_NumEntries = template->ml_NumEntries;
    for (i = 0; i < template->ml_NumEntries; ++i)
    {
        result->ml_ME[i].me_Reqs = template->ml_ME[i].me_Reqs;
        result->ml_ME[i].me_Length = template->ml_ME[i].me_Length;
        result->ml_ME[i].me_Addr = NULL;
        if (template->ml_ME[i].me_Length != 0)
        {
            result->ml_ME[i].me_Addr = AllocMem(
                (size_t)template->ml_ME[i].me_Length,
                template->ml_ME[i].me_Reqs);
            if (result->ml_ME[i].me_Addr == NULL)
            {
                while (i > 0)
                {
                    --i;
                    FreeMem(result->ml_ME[i].me_Addr,
                            (size_t)result->ml_ME[i].me_Length);
                }
                FreeMem(result, memlist_size(result->ml_NumEntries));
                return NULL;
            }
        }
    }
    if (flags != NULL)
        *flags = 0;
    primary_entry = result;
    return result;
}

static void FreeEntry(struct MemList *entry)
{
    UWORD i;

    record_event(EV_FREE_ENTRY);
    free_entry_calls++;
    if (expect_slot_clear_at_free)
    {
        CHECK(watched_slot != NULL);
        CHECK(*watched_slot == NULL);
        free_slot_checks++;
    }
    for (i = 0; i < entry->ml_NumEntries; ++i)
        FreeMem(entry->ml_ME[i].me_Addr,
                (size_t)entry->ml_ME[i].me_Length);
    FreeMem(entry, memlist_size(entry->ml_NumEntries));
}

static struct TagItem *LibNextTagItem(struct TagItem **state)
{
    struct TagItem *tag;
    if (state == NULL || *state == NULL || (*state)->ti_Tag == TAG_DONE)
        return NULL;
    tag = *state;
    *state = tag + 1;
    return tag;
}

static LONG AllocTaskSignal(struct Task *task, LONG requested,
                            struct ExecBase *base)
{
    CHECK(task == allocated_task);
    CHECK(base == SysBase);
    CHECK(requested == -1);
    CHECK(task->tc_SigAlloc == SysBase->TaskSigAlloc);
#if TEST_SMP
    CHECK(task->tc_SpinLock == SPINLOCK_READY);
    CHECK(spin_init_calls == 1U);
    CHECK(event_count >= 2U);
    CHECK(events[1] == EV_SPIN_INIT);
#else
    CHECK(task->tc_SpinLock == SPINLOCK_DIRTY);
    CHECK(spin_init_calls == 0U);
#endif
    CHECK(newadd_calls == 0U);
    alloc_signal_calls++;
    record_event(EV_ALLOC_TASK_SIGNAL);
    return fail_signal_allocation ? -1 : (LONG)TEST_SIGBIT;
}

static void InitMsgPort(struct MsgPort *port)
{
    memset(port, 0, sizeof(*port));
    InitList(&port->mp_MsgList);
}

static void restore_task_page(void)
{
    if (task_page_poisoned)
    {
        CHECK(mprotect(allocated_task_page, page_size,
                       PROT_READ | PROT_WRITE) == 0);
        task_page_poisoned = 0;
    }
}

static APTR NewAddTask(struct Task *task, APTR initialPC, APTR finalPC,
                       struct TagItem *tags)
{
    (void)initialPC;
    (void)finalPC;
    (void)tags;
    CHECK(task == allocated_task);
#if TEST_SMP
    CHECK(task->tc_SpinLock == SPINLOCK_READY);
    CHECK(spin_init_calls == 1U);
#else
    CHECK(task->tc_SpinLock == SPINLOCK_DIRTY);
    CHECK(spin_init_calls == 0U);
#endif
    newadd_calls++;
    record_event(EV_NEW_ADD_TASK);
    if (fail_newadd_task)
        return NULL;
    if (poison_task_after_newadd)
    {
        CHECK(mprotect(allocated_task_page, page_size, PROT_NONE) == 0);
        task_page_poisoned = 1;
    }
    return task;
}

#define DADDTASK(...) ((void)0)
'''

main = r'''
static void reset_case(void)
{
    CHECK(live_count == 0U);
    memset(&execBase, 0, sizeof(execBase));
    execBase.TaskSigAlloc = UINT32_C(0xffffffff);
    execBase.TaskExitCode = (APTR)(uintptr_t)UINT32_C(0x1000);
    SysBase = &execBase;
    event_count = 0;
    spin_init_calls = 0;
    alloc_signal_calls = 0;
    newadd_calls = 0;
    free_entry_calls = 0;
    free_slot_checks = 0;
    fail_new_alloc_entry = 0;
    fail_signal_allocation = 0;
    fail_newadd_task = 0;
    poison_task_after_newadd = 0;
    task_page_poisoned = 0;
    expect_slot_clear_at_free = 0;
    watched_slot = NULL;
    allocated_task = NULL;
    allocated_task_page = NULL;
    primary_entry = NULL;
    allocation_count = 0;
}

static int test_unnamed_no_port_success(void)
{
    ULONG error = UINT32_MAX;
    struct TagItem tags[] = {
        { TASKTAG_ERROR, (IPTR)&error },
        { TAG_DONE, 0 }
    };
    struct Task *result;

    reset_case();
    result = NewCreateTaskA(tags);
    CHECK(result != NULL);
    CHECK(error == TASKERROR_OK);
    CHECK(result->tc_Node.ln_Name == NULL);
    CHECK(primary_entry != NULL);
    CHECK(primary_entry->ml_NumEntries == 2U);
    CHECK(alloc_signal_calls == 0U);
#if TEST_SMP
    CHECK(spin_init_calls == 1U);
    CHECK(result->tc_SpinLock == SPINLOCK_READY);
#else
    CHECK(spin_init_calls == 0U);
    CHECK(result->tc_SpinLock == SPINLOCK_DIRTY);
#endif
    FreeEntry(primary_entry);
    CHECK(live_count == 0U);
    return 1;
}

static int test_named_no_port_success(void)
{
    ULONG error = UINT32_MAX;
    struct TagItem tags[] = {
        { TASKTAG_NAME, (IPTR)"portless-child" },
        { TASKTAG_ERROR, (IPTR)&error },
        { TAG_DONE, 0 }
    };
    struct Task *result;

    reset_case();
    result = NewCreateTaskA(tags);
    CHECK(result != NULL);
    CHECK(error == TASKERROR_OK);
    CHECK(alloc_signal_calls == 0U);
    CHECK(newadd_calls == 1U);
    CHECK(free_entry_calls == 0U);
#if TEST_SMP
    CHECK(spin_init_calls == 1U);
    CHECK(result->tc_SpinLock == SPINLOCK_READY);
#else
    CHECK(spin_init_calls == 0U);
    CHECK(result->tc_SpinLock == SPINLOCK_DIRTY);
#endif
    CHECK(strcmp(result->tc_Node.ln_Name, "portless-child") == 0);
    CHECK(primary_entry != NULL);
    FreeEntry(primary_entry);
    CHECK(live_count == 0U);
    return 1;
}

static int test_port_lock_initialized_before_signal(void)
{
    struct MsgPort *port = (struct MsgPort *)(uintptr_t)UINT32_C(0xdeadbeef);
    ULONG error = UINT32_MAX;
    struct TagItem tags[] = {
        { TASKTAG_ERROR, (IPTR)&error },
        { TASKTAG_TASKMSGPORT, (IPTR)&port },
        { TAG_DONE, 0 }
    };
    struct Task *result;

    reset_case();
    result = NewCreateTaskA(tags);
    CHECK(result != NULL);
    CHECK(error == TASKERROR_OK);
    CHECK(port != NULL);
    CHECK(port->mp_SigBit == (ULONG)TEST_SIGBIT);
    CHECK(port->mp_SigTask == result);
    CHECK(alloc_signal_calls == 1U);
    CHECK(newadd_calls == 1U);
    CHECK(event_count >= 3U);
#if TEST_SMP
    CHECK(events[1] == EV_SPIN_INIT);
    CHECK(events[2] == EV_ALLOC_TASK_SIGNAL);
    CHECK(spin_init_calls == 1U);
    CHECK(result->tc_SpinLock == SPINLOCK_READY);
#else
    CHECK(events[1] == EV_ALLOC_TASK_SIGNAL);
    CHECK(spin_init_calls == 0U);
    CHECK(result->tc_SpinLock == SPINLOCK_DIRTY);
#endif
    CHECK(primary_entry != NULL);
    FreeEntry(primary_entry);
    CHECK(live_count == 0U);
    return 1;
}

static int test_allocation_failure(void)
{
    struct MsgPort *port = (struct MsgPort *)(uintptr_t)UINT32_C(0xdeadbeef);
    ULONG error = UINT32_MAX;
    struct TagItem tags[] = {
        { TASKTAG_ERROR, (IPTR)&error },
        { TASKTAG_TASKMSGPORT, (IPTR)&port },
        { TAG_DONE, 0 }
    };
    struct Task *result;

    reset_case();
    fail_new_alloc_entry = 1;
    result = NewCreateTaskA(tags);
    CHECK(result == NULL);
    CHECK(port == NULL);
    CHECK(error == TASKERROR_NOMEMORY);
    CHECK(spin_init_calls == 0U);
    CHECK(alloc_signal_calls == 0U);
    CHECK(newadd_calls == 0U);
    CHECK(free_entry_calls == 0U);
    CHECK(live_count == 0U);
    return 1;
}

static int test_signal_allocation_failure(void)
{
    struct MsgPort *port = (struct MsgPort *)(uintptr_t)UINT32_C(0xdeadbeef);
    ULONG error = UINT32_MAX;
    struct TagItem tags[] = {
        { TASKTAG_ERROR, (IPTR)&error },
        { TASKTAG_TASKMSGPORT, (IPTR)&port },
        { TAG_DONE, 0 }
    };
    struct Task *result;

    reset_case();
    fail_signal_allocation = 1;
    result = NewCreateTaskA(tags);
    CHECK(result == NULL);
    CHECK(port == NULL);
    CHECK(error == TASKERROR_NOMEMORY);
    CHECK(alloc_signal_calls == 1U);
    CHECK(newadd_calls == 0U);
    CHECK(free_entry_calls == 1U);
#if TEST_SMP
    CHECK(spin_init_calls == 1U);
    CHECK(events[1] == EV_SPIN_INIT);
    CHECK(events[2] == EV_ALLOC_TASK_SIGNAL);
#else
    CHECK(spin_init_calls == 0U);
    CHECK(events[1] == EV_ALLOC_TASK_SIGNAL);
#endif
    CHECK(live_count == 0U);
    return 1;
}

static int test_newadd_failure_clears_slot_before_free(void)
{
    struct MsgPort *port = (struct MsgPort *)(uintptr_t)UINT32_C(0xdeadbeef);
    ULONG error = UINT32_MAX;
    struct TagItem tags[] = {
        { TASKTAG_ERROR, (IPTR)&error },
        { TASKTAG_TASKMSGPORT, (IPTR)&port },
        { TAG_DONE, 0 }
    };
    struct Task *result;

    reset_case();
    fail_newadd_task = 1;
    expect_slot_clear_at_free = 1;
    watched_slot = &port;
    result = NewCreateTaskA(tags);
    CHECK(result == NULL);
    CHECK(port == NULL);
    CHECK(error == TASKERROR_NOMEMORY);
    CHECK(alloc_signal_calls == 1U);
    CHECK(newadd_calls == 1U);
    CHECK(free_entry_calls == 1U);
    CHECK(free_slot_checks == 1U);
    CHECK(live_count == 0U);
    return 1;
}

static int test_newadd_success_poison_boundary(void)
{
    struct MsgPort *port = NULL;
    ULONG error = UINT32_MAX;
    struct TagItem tags[] = {
        { TASKTAG_NAME, (IPTR)"constructor-child" },
        { TASKTAG_ERROR, (IPTR)&error },
        { TASKTAG_TASKMSGPORT, (IPTR)&port },
        { TAG_DONE, 0 }
    };
    struct Task *result;

    reset_case();
    poison_task_after_newadd = 1;
    result = NewCreateTaskA(tags);
    CHECK(result == allocated_task);
    CHECK(task_page_poisoned);
    CHECK(error == TASKERROR_OK);
    CHECK(port != NULL && port->mp_SigTask == result);
    CHECK(newadd_calls == 1U);
    CHECK(live_count != 0U);
    /* Only now re-enable access for assertions and allocator cleanup. */
    restore_task_page();
#if TEST_SMP
    CHECK(result->tc_SpinLock == SPINLOCK_READY);
#else
    CHECK(result->tc_SpinLock == SPINLOCK_DIRTY);
#endif
    CHECK(strcmp(result->tc_Node.ln_Name, "constructor-child") == 0);
    CHECK(primary_entry != NULL);
    FreeEntry(primary_entry);
    CHECK(live_count == 0U);
    return 1;
}

int main(void)
{
    page_size = (size_t)sysconf(_SC_PAGESIZE);
    CHECK(page_size != 0U);
    CHECK(test_unnamed_no_port_success());
    CHECK(test_named_no_port_success());
    CHECK(test_port_lock_initialized_before_signal());
    CHECK(test_allocation_failure());
    CHECK(test_signal_allocation_failure());
    CHECK(test_newadd_failure_clears_slot_before_free());
    CHECK(test_newadd_success_poison_boundary());
#if TEST_SMP
    printf("PASS P4-SMP NewCreateTaskA lock fixture: %u checks\n", checks);
#else
    printf("PASS non-SMP NewCreateTaskA control fixture: %u checks\n", checks);
#endif
    return 0;
}
'''

newcreate_function = (
    "static struct Task *NewCreateTaskA(struct TagItem *tags)\n{\n"
    + body
    + "\n}\n"
)
program = fixture + "\n" + mem_template + "\n" + newcreate_function + main

with tempfile.TemporaryDirectory(prefix="p4-task-constructor-lock-") as tmp:
    temp = Path(tmp)
    for variant, smp in (("smp", 1), ("normal", 0)):
        src = temp / f"{variant}.c"
        binary = temp / variant
        src.write_text(program)
        subprocess.run(
            [
                "clang",
                "-std=gnu11",
                "-O1",
                "-g",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-Wconversion",
                "-Wshadow",
                # These are intentional AROS ABI narrowing/pointer-arithmetic
                # operations in the extracted body; host IPTR is pointer-sized.
                "-Wno-sign-conversion",
                "-Wno-implicit-int-conversion",
                "-Wno-shorten-64-to-32",
                "-fsanitize=address,undefined",
                "-fno-omit-frame-pointer",
                f"-DTEST_SMP={smp}",
                str(src),
                "-o",
                str(binary),
            ],
            check=True,
        )
        subprocess.run([str(binary)], check=True)
