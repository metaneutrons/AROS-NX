/*
 * Composed host test of the actual P4 switch, callback escape, and self-remove
 * bodies. Registry retirement and queue publication remain explicit boundary
 * mocks; the host jump buffer is not the RV32 GCC builtin ABI.
 */
#include <assert.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sanitizer/asan_interface.h>

#define P4_SWITCH_TEST 1
#define P4_CALLBACK_ESCAPE_TEST 1
#define P4_SELF_REMOVE_TEST 1

typedef uintptr_t ULONG;
typedef void *APTR;
struct ExecBase { unsigned identity; };
typedef void (*VOID_FUNC)(struct ExecBase *);

#define RISCV_REGSAVE_CNT 29
#define ECF_FPU UINT32_C(1)
#define TS_INVALID 0u
#define TS_RUN 1u
#define TS_READY 2u
#define TS_WAIT 3u
#define TS_REMOVED 4u
#define TS_TOMBSTONED 5u
#define TF_SWITCH UINT32_C(0x10)
#define MSTATUS_MIE UINT32_C(8)
#define MSTATUS_FS UINT32_C(0x6000)
#define MSTATUS_FS_INITIAL UINT32_C(0x2000)
#define MSTATUS_FS_CLEAN UINT32_C(0x4000)
#define MSTATUS_FS_DIRTY UINT32_C(0x6000)
#define P4_PUBLIC_SPIN_BAD_STATE 1
#define SPINLOCK_MODE_WRITE 1
#define AROS_UFC1NR(ret, fn, args) fn args
#define AROS_UFCA(type, value, reg) (value)

struct ExceptionContext
{
    union
    {
        ULONG x[RISCV_REGSAVE_CNT];
        struct { ULONG ra; ULONG sp; };
    };
    ULONG pc;
    ULONG sr;
    ULONG Flags;
    APTR fpuContext;
    APTR vecContext;
};

#include "../p4_trap_frame.h"
#include "../../exec/p4_signal_protocol.h"
#include "../p4_callback_escape.h"
#include "../../exec/p4_self_remove.h"

struct ETask
{
    struct ExceptionContext *et_RegFrame;
};

struct IntETask
{
    struct ETask iet_ETask;
    unsigned int iet_CpuNumber;
};

struct Task
{
    unsigned int tc_State;
    unsigned int tc_Flags;
    int tc_SpinLock;
    uintptr_t tc_SPLower;
    uintptr_t tc_SPUpper;
    APTR tc_SPReg;
    int tc_IDNestCnt;
    VOID_FUNC tc_Switch;
    struct IntETask *test_iet;
};

struct p4_cpu_local
{
    unsigned int trap_depth;
    struct Task *current_task;
    struct Task *fpu_owner;
    int id_nest;
};

struct p4_exec_runtime
{
    unsigned int hart_id;
    struct p4_cpu_local *cpu_local;
    struct Task *wait_pending;
};

enum pin_mode
{
    PIN_MODE_NORMAL,
    PIN_MODE_REMOTE_BEFORE_PIN,
    PIN_MODE_REMOTE_AFTER_PIN
};

enum lock_role
{
    LOCK_ROLE_INITIAL,
    LOCK_ROLE_SELF_REMOVE,
    LOCK_ROLE_REMOTE_RETIRE,
    LOCK_ROLE_EPILOGUE,
    LOCK_ROLE_OTHER
};

enum event_kind
{
    EV_LOCK = 1,
    EV_UNLOCK,
    EV_QUEUE_COMMIT,
    EV_QUEUE_PUBLISH,
    EV_HOOK_ENTER,
    EV_DISABLE,
    EV_PIN,
    EV_RETIRE,
    EV_UNPIN,
    EV_OWNER_PROOF,
    EV_TRANSFER,
    EV_HOOK_TAIL,
    EV_CLEAR_CURRENT,
    EV_RUN_RELEASE,
    EV_POKE,
    EV_FATAL
};

struct Fixture
{
    struct ExecBase sys_base;
    struct p4_cpu_local local;
    struct p4_exec_runtime runtime;
    struct Task *task;
    struct IntETask *iet;
    struct ExceptionContext *saved;
    struct ExceptionContext regs;
    int *task_lock_address;
    unsigned int hart;
    unsigned int lock_depth;
    unsigned int queue_lock_depth;
    unsigned int total_locks;
    unsigned int total_unlocks;
    unsigned int epilogue_locks;
    unsigned int epilogue_unlocks;
    unsigned int remote_retire_locks;
    unsigned int remote_retire_unlocks;
    unsigned int queue_commits;
    unsigned int queue_publications;
    unsigned int hook_entries;
    unsigned int hook_tails;
    unsigned int pin_calls;
    unsigned int pin_successes;
    unsigned int unpin_calls;
    unsigned int retire_calls;
    unsigned int remote_retire_calls;
    unsigned int owner_proof_calls;
    unsigned int transfer_calls;
    unsigned int dispatch_calls;
    unsigned int leave_calls;
    unsigned int poke_calls;
    unsigned int set_current_calls;
    unsigned int pin_count;
    unsigned int running_refs;
    unsigned int event_count;
    unsigned int events[64];
    unsigned int fatal_calls;
    unsigned int fatal_reason;
    unsigned int mode;
    unsigned int lock_role;
    int task_on_running;
    int task_on_ready;
    int service_retired;
    int task_poisoned;
    int iet_poisoned;
    int frame_poisoned;
    int fatal_armed;
};

static struct Fixture *active;
static jmp_buf fatal_env;
static unsigned long mock_mstatus;
static const char *stage = "setup";

static struct p4_exec_runtime *p4_exec_runtime_current(
    struct ExecBase *sys_base);
static int p4_exec_runtime_set_current_task(
    struct ExecBase *sys_base, struct Task *task);
static struct ETask *mock_get_etask(struct Task *task);
static struct IntETask *mock_get_int_etask(struct Task *task);
static void mock_task_lock(int *lock);
static void mock_task_unlock(int *lock);

#define GetETask(task) mock_get_etask((task))
#define GetIntETask(task) mock_get_int_etask((task))
#define GET_THIS_TASK (active->local.current_task)
#define TDNESTCOUNT_SET(value) (active->local.id_nest = (int)(value))
#define csr_read(reg) (mock_mstatus)
#define EXEC_SPINLOCK_LOCK(lock, owner, mode) mock_task_lock((lock))
#define EXEC_SPINLOCK_UNLOCK(lock) mock_task_unlock((lock))

static unsigned int checks;

#define CHECK(condition) do { \
    ++checks; \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d [%s]: check failed: %s\n", \
                __FILE__, __LINE__, stage, #condition); \
        exit(1); \
    } \
} while (0)

static void record_event(unsigned int event)
{
    CHECK(active != NULL);
    CHECK(active->event_count < sizeof(active->events) /
                                sizeof(active->events[0]));
    if (active->event_count < sizeof(active->events) /
                              sizeof(active->events[0]))
        active->events[active->event_count++] = event;
}

static void fatal(unsigned int code) __attribute__((noreturn));
static void fatal(unsigned int code)
{
    ++active->fatal_calls;
    active->fatal_reason = code;
    record_event(EV_FATAL);
    if (active->fatal_armed)
        longjmp(fatal_env, 1);
    fprintf(stderr, "unexpected production fatal at %s: code=%u hart=%u "
                    "depth=%u mie=%lu current=%p task=%p state=%u "
                    "running=%u ready=%d pin=%u\n",
            stage, code, active->runtime.hart_id,
            active->local.trap_depth, mock_mstatus & MSTATUS_MIE,
            (void *)active->local.current_task, (void *)active->task,
            active->task_poisoned ? 999u : active->task->tc_State,
            active->running_refs, active->task_on_ready, active->pin_count);
    abort();
}

void KrnCli(void)
{
    mock_mstatus &= ~((unsigned long)MSTATUS_MIE);
}

static void Disable(void)
{
    record_event(EV_DISABLE);
    mock_mstatus &= ~((unsigned long)MSTATUS_MIE);
}

static void KrnDispatch(void)
{
    ++active->dispatch_calls;
    fatal(P4_PUBLIC_SPIN_BAD_STATE);
}

static struct p4_exec_runtime *p4_exec_runtime_current(
    struct ExecBase *sys_base)
{
    stage = "runtime lookup";
    CHECK(sys_base == &active->sys_base);
    return &active->runtime;
}

static int p4_exec_runtime_set_current_task(
    struct ExecBase *sys_base, struct Task *task)
{
    stage = "clear current";
    CHECK(sys_base == &active->sys_base);
    CHECK(active->lock_depth == 1u);
    CHECK(active->local.current_task == active->task);
    CHECK(active->local.fpu_owner == active->task);
    CHECK(task == NULL);
    active->local.current_task = NULL;
    ++active->set_current_calls;
    record_event(EV_CLEAR_CURRENT);
    return 1;
}

static struct ETask *mock_get_etask(struct Task *task)
{
    stage = "GetETask";
    CHECK(task == active->task);
    CHECK(!active->task_poisoned && !active->iet_poisoned);
    return task->test_iet ? &task->test_iet->iet_ETask : NULL;
}

static struct IntETask *mock_get_int_etask(struct Task *task)
{
    stage = "GetIntETask";
    CHECK(task == active->task);
    CHECK(!active->task_poisoned && !active->iet_poisoned);
    return task->test_iet;
}

static void mock_task_lock(int *lock)
{
    if (active->lock_role == LOCK_ROLE_OTHER && active->hook_entries != 0u)
        active->lock_role = LOCK_ROLE_EPILOGUE;
    CHECK(lock == active->task_lock_address);
    CHECK(!active->task_poisoned);
    CHECK(active->lock_depth == 0u);
    ++active->total_locks;
    active->lock_depth = 1u;
    if (active->lock_role == LOCK_ROLE_EPILOGUE)
    {
        ++active->epilogue_locks;
        CHECK(active->queue_lock_depth == 0u);
    }
    else if (active->lock_role == LOCK_ROLE_REMOTE_RETIRE)
        ++active->remote_retire_locks;
    record_event(EV_LOCK);
}

static void mock_task_unlock(int *lock)
{
    CHECK(lock == active->task_lock_address);
    CHECK(!active->task_poisoned);
    CHECK(active->lock_depth == 1u);
    --active->lock_depth;
    ++active->total_unlocks;
    if (active->lock_role == LOCK_ROLE_EPILOGUE)
    {
        ++active->epilogue_unlocks;
        CHECK(active->local.current_task == NULL);
        CHECK(active->local.fpu_owner == NULL);
    }
    else if (active->lock_role == LOCK_ROLE_REMOTE_RETIRE)
        ++active->remote_retire_unlocks;
    record_event(EV_UNLOCK);
    if (active->lock_role == LOCK_ROLE_INITIAL)
        active->lock_role = LOCK_ROLE_OTHER;
}

static void p4_signal_adapter_lock_disabled(struct Task *task)
{
    stage = "self-remove Task lock";
    CHECK(task == active->task);
    CHECK((mock_mstatus & MSTATUS_MIE) == 0ul);
    CHECK(active->pin_count == 1u);
    if (active->mode == PIN_MODE_REMOTE_AFTER_PIN)
    {
        /* Remote retirement wins after the ordinary pin but before this
         * caller obtains the Task lock. RUN still owns the identity. */
        active->lock_role = LOCK_ROLE_REMOTE_RETIRE;
        mock_task_lock(&task->tc_SpinLock);
        CHECK(task->tc_State == TS_READY && active->task_on_ready);
        task->tc_State = TS_REMOVED;
        active->task_on_ready = 0;
        active->service_retired = 1;
        ++active->remote_retire_calls;
        mock_task_unlock(&task->tc_SpinLock);
        active->lock_role = LOCK_ROLE_OTHER;
    }
    active->lock_role = LOCK_ROLE_SELF_REMOVE;
    mock_task_lock(&task->tc_SpinLock);
}

static void p4_signal_adapter_unlock_disabled(struct Task *task)
{
    stage = "self-remove Task unlock";
    CHECK(task == active->task);
    CHECK(active->lock_depth == 1u);
    active->lock_role = LOCK_ROLE_OTHER;
    mock_task_unlock(&task->tc_SpinLock);
}

static void p4_signal_adapter_fail_closed(void) __attribute__((noreturn));
static void p4_signal_adapter_fail_closed(void)
{
    fatal(P4_PUBLIC_SPIN_BAD_STATE);
}

static int krnP4SignalTaskPin(struct Task *task, struct ExecBase *sys_base)
{
    stage = "ordinary pin";
    CHECK(sys_base == &active->sys_base);
    CHECK(task == active->task);
    CHECK(active->lock_depth == 0u);
    CHECK((mock_mstatus & MSTATUS_MIE) == 0ul);
    ++active->pin_calls;
    record_event(EV_PIN);
    if (active->mode == PIN_MODE_REMOTE_BEFORE_PIN)
    {
        /* Deterministic boundary interleaving: a service retirement is
         * already committed when the attempted ordinary pin reaches gate. */
        active->lock_role = LOCK_ROLE_REMOTE_RETIRE;
        mock_task_lock(&task->tc_SpinLock);
        CHECK(task->tc_State == TS_READY && active->task_on_ready);
        task->tc_State = TS_REMOVED;
        active->task_on_ready = 0;
        active->service_retired = 1;
        ++active->remote_retire_calls;
        mock_task_unlock(&task->tc_SpinLock);
        active->lock_role = LOCK_ROLE_OTHER;
        return 0;
    }
    if (active->service_retired)
        return 0;
    ++active->pin_count;
    ++active->pin_successes;
    return 1;
}

static void krnP4SignalTaskUnpin(struct Task *task, struct ExecBase *sys_base)
{
    stage = "ordinary unpin";
    CHECK(sys_base == &active->sys_base);
    CHECK(task == active->task);
    CHECK(active->pin_count == 1u);
    CHECK(active->lock_depth == 0u);
    --active->pin_count;
    ++active->unpin_calls;
    record_event(EV_UNPIN);
}

static int krnP4TaskRetireLocked(struct Task *task, struct ExecBase *sys_base)
{
    stage = "service retirement";
    CHECK(sys_base == &active->sys_base);
    CHECK(task == active->task);
    CHECK(active->lock_depth == 1u);
    CHECK(active->pin_count == 1u);
    ++active->retire_calls;
    record_event(EV_RETIRE);
    if (task->tc_State != TS_READY || !active->task_on_ready)
        return 0;
    task->tc_State = TS_REMOVED;
    active->task_on_ready = 0;
    active->service_retired = 1;
    return 1;
}

static int krnP4TaskRecordServiceOwnerRunning(
    struct Task *identity, unsigned int hart, struct ExecBase *sys_base)
{
    stage = "service owner proof";
    /* Identity is compared as an opaque key; the mock must not read it. */
    CHECK(sys_base == &active->sys_base);
    CHECK(identity == active->task);
    CHECK(hart == active->hart);
    CHECK(active->lock_depth == 0u);
    CHECK(active->pin_count == 0u);
    ++active->owner_proof_calls;
    record_event(EV_OWNER_PROOF);
    return active->service_retired && active->running_refs == 1u;
}

static struct p4_signal_effect p4_signal_adapter_switch_commit_locked(
    struct Task *task, struct ExecBase *sys_base)
{
    struct p4_signal_effect effect;
    stage = "switch signal commit";
    CHECK(task == active->task && sys_base == &active->sys_base);
    CHECK(active->lock_depth == 1u);
    CHECK(task->tc_State == TS_RUN && active->task_on_running);
    memset(&effect, 0, sizeof(effect));
    effect.status = P4_SIGNAL_STATUS_OK;
    effect.queue_transition = P4_SIGNAL_QUEUE_RUNNING_TO_READY;
    effect.request_reschedule = 1u;
    effect.reschedule_hart = active->hart;
    ++active->queue_commits;
    record_event(EV_QUEUE_COMMIT);
    return effect;
}

static int p4_signal_adapter_apply_queue_locked(
    struct Task *task, struct ExecBase *sys_base,
    struct p4_signal_effect *effect)
{
    stage = "switch queue publication";
    CHECK(task == active->task && sys_base == &active->sys_base);
    CHECK(effect != NULL && effect->status == P4_SIGNAL_STATUS_OK);
    CHECK(effect->queue_transition == P4_SIGNAL_QUEUE_RUNNING_TO_READY);
    CHECK(active->lock_depth == 1u);
    CHECK(active->task_on_running && active->running_refs == 1u);
    CHECK(active->queue_lock_depth == 0u);
    active->queue_lock_depth = 1u;
    active->task_on_running = 0;
    active->task_on_ready = 1;
    task->tc_State = TS_READY;
    ++active->queue_publications;
    record_event(EV_QUEUE_PUBLISH);
    active->queue_lock_depth = 0u;
    return 1;
}

static void p4_signal_adapter_poke_after_unlock(
    const struct p4_signal_effect *effect, struct ExecBase *sys_base)
{
    stage = "post-release poke";
    CHECK(effect != NULL && sys_base == &active->sys_base);
    CHECK(active->lock_depth == 0u && active->pin_count == 0u);
    CHECK(active->running_refs == 0u && active->local.current_task == NULL);
    CHECK(active->local.fpu_owner == NULL);
    if (active->task_poisoned)
    {
        CHECK(__asan_address_is_poisoned(active->task));
        CHECK(active->iet_poisoned && active->frame_poisoned);
        CHECK(__asan_address_is_poisoned(active->iet));
        CHECK(__asan_address_is_poisoned(active->saved));
    }
    CHECK(effect->status == P4_SIGNAL_STATUS_OK);
    if (effect->request_reschedule)
        ++active->poke_calls;
    record_event(EV_POKE);
}

static void krnP4TaskRecordLeaveRunning(
    struct Task *identity, unsigned int hart, struct ExecBase *sys_base)
{
    stage = "RUN release";
    CHECK(identity == active->task); /* opaque pointer comparison only */
    CHECK(hart == active->hart && sys_base == &active->sys_base);
    CHECK(active->lock_depth == 0u && active->pin_count == 0u);
    CHECK(active->running_refs == 1u);
    CHECK(active->local.current_task == NULL);
    CHECK(active->local.fpu_owner == NULL);
    CHECK(active->set_current_calls == 1u);
    --active->running_refs;
    ++active->leave_calls;
    record_event(EV_RUN_RELEASE);
    if (active->service_retired)
    {
        __asan_poison_memory_region(active->task, sizeof(*active->task));
        __asan_poison_memory_region(active->iet, sizeof(*active->iet));
        __asan_poison_memory_region(active->saved, sizeof(*active->saved));
        active->task_poisoned = 1;
        active->iet_poisoned = 1;
        active->frame_poisoned = 1;
    }
}

static void krnP4DiscardCurrent(struct ExecBase *sys_base)
{
    (void)sys_base;
    CHECK(0 && "the composed callback path must not enter discard");
}

static void krnP4PublicSpinFatal(int code) __attribute__((noreturn));
static void krnP4PublicSpinFatal(int code)
{
    fatal((unsigned int)code);
}

/* On this host only, substitute native setjmp storage/calls for the
 * compiler-owned RV32 builtin representation. QEMU qualification is separate. */
typedef jmp_buf p4_host_jmp_buf;
static void p4_host_longjmp(void *buffer, int value) __attribute__((noreturn));
static void p4_host_longjmp(void *buffer, int value)
{
    CHECK(buffer != NULL && value == 1);
    CHECK(active->lock_depth == 0u && active->pin_count == 0u);
    CHECK(active->running_refs == 1u);
    ++active->transfer_calls;
    record_event(EV_TRANSFER);
    longjmp(*(p4_host_jmp_buf *)buffer, value);
}
#define intptr_t p4_host_jmp_buf
#define __builtin_setjmp(buffer) \
    setjmp(*(p4_host_jmp_buf *)(void *)(buffer))
#define __builtin_longjmp(buffer, value) \
    p4_host_longjmp((void *)(buffer), (value))

#include "../p4_switch.c"
#include "../p4_callback_escape.c"
#include "../../exec/p4_self_remove.c"

#undef __builtin_longjmp
#undef __builtin_setjmp
#undef intptr_t

static void (*volatile remove_self_indirect)(
    struct Task *, struct ExecBase *) = Exec_P4RemoveSelf;

static void switch_hook_self_remove(struct ExecBase *sys_base)
{
    stage = "TF_SWITCH self-remove callback";
    CHECK(sys_base == &active->sys_base);
    CHECK(active->lock_depth == 0u && active->running_refs == 1u);
    CHECK(active->queue_lock_depth == 0u);
    CHECK(active->local.current_task == active->task);
    CHECK(active->local.fpu_owner == active->task);
    CHECK(p4_callback_slots[active->hart] != NULL);
    ++active->hook_entries;
    record_event(EV_HOOK_ENTER);
    remove_self_indirect(active->task, sys_base);
    /* An indirect non-noreturn type leaves an observable forbidden tail. */
    ++active->hook_tails;
    active->lock_role = LOCK_ROLE_EPILOGUE;
    record_event(EV_HOOK_TAIL);
}

static void switch_hook_ordinary(struct ExecBase *sys_base)
{
    stage = "ordinary TF_SWITCH callback";
    CHECK(sys_base == &active->sys_base);
    CHECK(active->lock_depth == 0u && active->running_refs == 1u);
    CHECK(active->queue_lock_depth == 0u);
    CHECK(p4_callback_slots[active->hart] != NULL);
    ++active->hook_entries;
    ++active->hook_tails;
    active->lock_role = LOCK_ROLE_EPILOGUE;
    record_event(EV_HOOK_ENTER);
    record_event(EV_HOOK_TAIL);
}

static struct Fixture *fixture_create(unsigned int hart, enum pin_mode mode)
{
    struct Fixture *fixture = calloc(1u, sizeof(*fixture));
    unsigned int index;
    CHECK(fixture != NULL);
    fixture->task = calloc(1u, sizeof(*fixture->task));
    fixture->iet = calloc(1u, sizeof(*fixture->iet));
    fixture->saved = calloc(1u, sizeof(*fixture->saved));
    CHECK(fixture->task != NULL && fixture->iet != NULL &&
          fixture->saved != NULL);
    fixture->hart = hart;
    fixture->mode = (unsigned int)mode;
    fixture->runtime.hart_id = hart;
    fixture->runtime.cpu_local = &fixture->local;
    fixture->local.trap_depth = 1u;
    fixture->local.id_nest = -1;
    fixture->local.current_task = fixture->task;
    fixture->local.fpu_owner = fixture->task;
    fixture->running_refs = 1u;
    fixture->task_on_running = 1;
    fixture->task->tc_State = TS_RUN;
    fixture->task->tc_Flags = TF_SWITCH;
    fixture->task->tc_SpinLock = 0;
    fixture->task_lock_address = &fixture->task->tc_SpinLock;
    fixture->task->tc_SPLower = UINT32_C(0x1000);
    fixture->task->tc_SPUpper = UINT32_C(0x3000);
    fixture->task->tc_IDNestCnt = 4;
    fixture->task->test_iet = fixture->iet;
    fixture->task->tc_Switch = switch_hook_self_remove;
    fixture->iet->iet_CpuNumber = hart;
    fixture->iet->iet_ETask.et_RegFrame = fixture->saved;
    fixture->regs.pc = UINT32_C(0x4000);
    fixture->regs.sr = MSTATUS_FS_INITIAL;
    for (index = 0; index < RISCV_REGSAVE_CNT; ++index)
        fixture->regs.x[index] = UINT32_C(0x1000) + index;
    fixture->regs.sp = UINT32_C(0x2000);
    active = fixture;
    stage = "fixture ready";
    mock_mstatus = 0ul;
    return fixture;
}

static void fixture_destroy(struct Fixture *fixture)
{
    if (fixture->task_poisoned)
        __asan_unpoison_memory_region(fixture->task, sizeof(*fixture->task));
    if (fixture->iet_poisoned)
        __asan_unpoison_memory_region(fixture->iet, sizeof(*fixture->iet));
    if (fixture->frame_poisoned)
        __asan_unpoison_memory_region(fixture->saved, sizeof(*fixture->saved));
    CHECK(fixture->lock_depth == 0u);
    CHECK(fixture->pin_count == 0u);
    CHECK(fixture->running_refs == 0u);
    CHECK(fixture->task_poisoned == fixture->service_retired);
    free(fixture->saved);
    free(fixture->iet);
    free(fixture->task);
    active = NULL;
    free(fixture);
}

static void assert_epilogue_once(struct Fixture *fixture)
{
    size_t clear_position = SIZE_MAX;
    size_t last_unlock_position = SIZE_MAX;
    size_t release_position = SIZE_MAX;
    size_t poke_position = SIZE_MAX;
    size_t index;
    for (index = 0; index < fixture->event_count; ++index)
    {
        if (fixture->events[index] == EV_CLEAR_CURRENT)
            clear_position = index;
        if (fixture->events[index] == EV_UNLOCK)
            last_unlock_position = index;
        if (fixture->events[index] == EV_RUN_RELEASE)
            release_position = index;
        if (fixture->events[index] == EV_POKE)
            poke_position = index;
    }
    CHECK(fixture->epilogue_locks == 1u);
    CHECK(fixture->epilogue_unlocks == 1u);
    CHECK(fixture->leave_calls == 1u);
    CHECK(fixture->set_current_calls == 1u);
    CHECK(fixture->local.current_task == NULL);
    CHECK(fixture->local.fpu_owner == NULL);
    CHECK(fixture->lock_depth == 0u);
    CHECK(fixture->running_refs == 0u);
    CHECK(fixture->dispatch_calls == 0u);
    CHECK(p4_callback_slots[fixture->hart] == NULL);
    CHECK(clear_position < last_unlock_position);
    CHECK(last_unlock_position < release_position);
    CHECK(release_position < poke_position);
}

static void run_switch(enum pin_mode mode, unsigned int hart, int retire)
{
    struct Fixture *fixture = fixture_create(hart, mode);
    fixture->task->tc_Switch = retire ? switch_hook_self_remove :
                                      switch_hook_ordinary;
    stage = "production krnP4Switch";
    krnP4Switch(&fixture->regs, &fixture->sys_base);
    CHECK(fixture->queue_commits == 1u);
    CHECK(fixture->queue_publications == 1u);
    CHECK(fixture->hook_entries == 1u);
    CHECK(fixture->hook_tails == (retire ? 0u : 1u));
    CHECK(fixture->total_locks == fixture->total_unlocks);
    CHECK(fixture->poke_calls == 1u);
    CHECK(fixture->fatal_calls == 0u);
    CHECK(fixture->queue_lock_depth == 0u);
    CHECK(fixture->pin_count == 0u);
    CHECK(p4_callback_slots[hart] == NULL);
    assert_epilogue_once(fixture);

    if (mode == PIN_MODE_NORMAL && retire)
    {
        CHECK(fixture->pin_calls == 1u && fixture->pin_successes == 1u);
        CHECK(fixture->unpin_calls == 1u);
        CHECK(fixture->retire_calls == 1u);
        CHECK(fixture->remote_retire_calls == 0u);
        CHECK(fixture->owner_proof_calls == 0u);
        CHECK(fixture->transfer_calls == 1u);
        CHECK(fixture->local.id_nest == -1);
        CHECK(fixture->remote_retire_locks == 0u &&
              fixture->remote_retire_unlocks == 0u);
        CHECK(fixture->total_locks == 3u && fixture->total_unlocks == 3u);
        CHECK(fixture->service_retired && fixture->task_poisoned);
    }
    else if (mode == PIN_MODE_REMOTE_BEFORE_PIN)
    {
        CHECK(fixture->pin_calls == 1u && fixture->pin_successes == 0u);
        CHECK(fixture->unpin_calls == 0u);
        CHECK(fixture->retire_calls == 0u);
        CHECK(fixture->remote_retire_calls == 1u);
        CHECK(fixture->owner_proof_calls == 1u);
        CHECK(fixture->transfer_calls == 1u);
        CHECK(fixture->remote_retire_locks == 1u &&
              fixture->remote_retire_unlocks == 1u);
        CHECK(fixture->total_locks == 3u && fixture->total_unlocks == 3u);
        CHECK(fixture->service_retired && fixture->task_poisoned);
    }
    else if (mode == PIN_MODE_REMOTE_AFTER_PIN)
    {
        CHECK(fixture->pin_calls == 1u && fixture->pin_successes == 1u);
        CHECK(fixture->unpin_calls == 1u);
        CHECK(fixture->retire_calls == 1u);
        CHECK(fixture->remote_retire_calls == 1u);
        CHECK(fixture->owner_proof_calls == 1u);
        CHECK(fixture->transfer_calls == 1u);
        CHECK(fixture->remote_retire_locks == 1u &&
              fixture->remote_retire_unlocks == 1u);
        CHECK(fixture->total_locks == 4u && fixture->total_unlocks == 4u);
        CHECK(fixture->service_retired && fixture->task_poisoned);
    }
    else
    {
        CHECK(!retire);
        CHECK(fixture->pin_calls == 0u && fixture->retire_calls == 0u);
        CHECK(fixture->transfer_calls == 0u);
        CHECK(fixture->total_locks == 2u && fixture->total_unlocks == 2u);
        CHECK(!fixture->service_retired && !fixture->task_poisoned);
        CHECK(fixture->task->tc_State == TS_READY && fixture->task_on_ready);
    }
    fixture_destroy(fixture);
}

static void test_nested_self_remove_refuses_before_pin(void)
{
    struct Fixture *fixture = fixture_create(0u, PIN_MODE_NORMAL);
    volatile int jumped;
    fixture->local.trap_depth = 2u;
    fixture->fatal_armed = 1;
    jumped = setjmp(fatal_env);
    if (jumped == 0)
        Exec_P4RemoveSelf(fixture->task, &fixture->sys_base);
    CHECK(jumped != 0);
    CHECK(fixture->fatal_calls == 1u);
    CHECK(fixture->pin_calls == 0u);
    CHECK(fixture->pin_successes == 0u);
    CHECK(fixture->retire_calls == 0u);
    CHECK(fixture->service_retired == 0);
    CHECK(fixture->lock_depth == 0u);
    CHECK(fixture->running_refs == 1u);
    CHECK(p4_callback_slots[0] == NULL);
    fixture->running_refs = 0u; /* fixture did not execute a switch epilogue */
    fixture_destroy(fixture);
}

int main(void)
{
    run_switch(PIN_MODE_NORMAL, 0u, 1);
    run_switch(PIN_MODE_REMOTE_BEFORE_PIN, 1u, 1);
    run_switch(PIN_MODE_REMOTE_AFTER_PIN, 0u, 1);
    run_switch(PIN_MODE_NORMAL, 1u, 0);
    test_nested_self_remove_refuses_before_pin();
    printf("p4-callback-switch-composed-test: %u checks passed\n", checks);
    return 0;
}
