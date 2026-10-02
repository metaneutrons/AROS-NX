/*
 * Host fixture for the production P4 SMP context handoff.
 *
 * Compile with ASan/UBSan. Queue membership, runtime lookup, the public lock,
 * and platform notifications are deterministic mocks; the switch function
 * and portable signal/wait state machine are the production sources.
 */
#include <assert.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sanitizer/asan_interface.h>

#define P4_SWITCH_TEST 1
#define P4_DISCARD_TEST 1

/* The host fixture uses a pointer-width word for APTR-compatible SP casts;
 * production itself is separately compiled for RV32. */
typedef uintptr_t ULONG;
typedef void *APTR;
struct ExecBase;
typedef void (*VOID_FUNC)(struct ExecBase *);

#define RISCV_REGSAVE_CNT 29
#define ECF_FPU 0x0001u
#define TS_INVALID 0u
#define TS_RUN 1u
#define TS_READY 2u
#define TS_WAIT 3u
#define TS_REMOVED 4u
#define TS_TOMBSTONED 5u
#define TF_SWITCH 0x10u
#define MSTATUS_MIE 0x00000008u
#define MSTATUS_FS 0x00006000u
#define MSTATUS_FS_INITIAL 0x00002000u
#define MSTATUS_FS_CLEAN 0x00004000u
#define MSTATUS_FS_DIRTY 0x00006000u
#define P4_PUBLIC_SPIN_BAD_STATE 1
#define SPINLOCK_MODE_WRITE 1
#define AROS_UFC1NR(ret, fn, args) fn args
#define AROS_UFCA(type, value, reg) (value)

struct ExecBase { unsigned unused; };
int krnP4RunTaskCallback(void (*invoke)(void *), void *argument,
                         struct ExecBase *sys_base);

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

struct ETask
{
    struct ExceptionContext *et_RegFrame;
};

struct IntETask
{
    struct ETask iet_ETask;
    unsigned long iet_CpuNumber;
};

struct Task
{
    unsigned tc_State;
    unsigned tc_Flags;
    int tc_SpinLock;
    uintptr_t tc_SPLower;
    uintptr_t tc_SPUpper;
    APTR tc_SPReg;
    int tc_IDNestCnt;
    uint32_t tc_SigRecvd;
    uint32_t tc_SigWait;
    uint32_t tc_SigExcept;
    VOID_FUNC tc_Switch;
    struct IntETask *test_iet;
};

struct p4_cpu_local
{
    unsigned trap_depth;
    struct Task *current_task;
    struct Task *fpu_owner;
    int id_nest;
};

struct p4_exec_runtime
{
    unsigned hart_id;
    struct p4_cpu_local *cpu_local;
    struct Task *wait_pending;
};

struct Fixture
{
    struct ExecBase sys_base;
    struct p4_cpu_local local;
    struct p4_exec_runtime runtime;
    struct Task *task;
    struct IntETask *iet;
    struct ExceptionContext *saved;
    void *saved_fpu_allocation;
    void *regs_fpu_allocation;
    struct ExceptionContext regs;
    struct p4_signal_effect last_effect;
    unsigned running_refs;
    unsigned lock_depth;
    unsigned queue_apply_calls;
    unsigned callback_calls;
    unsigned callback_wrapper_calls;
    unsigned poke_calls;
    unsigned leave_calls;
    unsigned queue_publications;
    unsigned callback_signal_calls;
    unsigned callback_retire_calls;
    unsigned reject_admission_count;
    unsigned callback_mode;
    unsigned get_etask_calls;
    unsigned get_int_etask_calls;
    unsigned detached_calls;
    unsigned service_owner_calls;
    unsigned event_sequence;
    unsigned last_task_unlock_sequence;
    unsigned leave_sequence;
    unsigned fatal_code;
    unsigned hart;
    unsigned service_owner_hart;
    int scheduler_detached;
    int service_pending;
    int expect_discard_path;
    int task_poisoned;
    int task_memory_poisoned;
    int saved_memory_poisoned;
    int iet_memory_poisoned;
    int saved_fpu_memory_poisoned;
    int on_running;
    int on_ready;
    int on_wait;
    int context_checked_before_commit;
    int admission_after_switch;
};

#define active fixture_active
static struct Fixture *active;
static jmp_buf fatal_env;
static int fatal_armed;
static unsigned long mock_mstatus;
static const char *stage = "fixture";

enum
{
    CALLBACK_NONE = 0,
    CALLBACK_SIGNAL,
    CALLBACK_RETIRE
};

#define GetETask(task) mock_get_etask((task))
#define GetIntETask(task) mock_get_int_etask((task))
#define csr_read(csr) (mock_mstatus)
#define EXEC_SPINLOCK_LOCK(lock, owner, mode) mock_task_lock((lock))
#define EXEC_SPINLOCK_UNLOCK(lock) mock_task_unlock((lock))

static struct ETask *mock_get_etask(struct Task *task);
static struct IntETask *mock_get_int_etask(struct Task *task);
static void mock_task_lock(int *lock);
static void mock_task_unlock(int *lock);

static unsigned checks;
#define CHECK(condition) do { \
    ++checks; \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: check failed: %s\n", \
                __FILE__, __LINE__, #condition); \
        exit(1); \
    } \
} while (0)

static void fail_closed(unsigned code) __attribute__((noreturn));
static void fail_closed(unsigned code)
{
    active->fatal_code = code;
    if (fatal_armed)
        longjmp(fatal_env, 1);
    fprintf(stderr, "unexpected production fatal at stage: %s\n", stage);
    abort();
}

void KrnCli(void)
{
    mock_mstatus &= ~MSTATUS_MIE;
}

static struct p4_exec_runtime *p4_exec_runtime_current(
    struct ExecBase *sys_base)
{
    stage = "runtime current";
    return sys_base == &active->sys_base ? &active->runtime : NULL;
}

static int p4_exec_runtime_set_current_task(
    struct ExecBase *sys_base,
    struct Task *task)
{
    stage = "set current";
    CHECK(sys_base == &active->sys_base);
    CHECK(active->lock_depth == 1);
    CHECK(active->local.current_task == active->task);
    CHECK(active->local.fpu_owner == active->task);
    active->local.current_task = task;
    return 1;
}

static struct ETask *mock_get_etask(struct Task *task)
{
    if (active != NULL)
        ++active->get_etask_calls;
    return task != NULL && task->test_iet != NULL ?
        &task->test_iet->iet_ETask : NULL;
}

static struct IntETask *mock_get_int_etask(struct Task *task)
{
    if (active != NULL)
        ++active->get_int_etask_calls;
    return task != NULL ? task->test_iet : NULL;
}

static void mock_task_lock(int *lock)
{
    stage = "task lock";
    CHECK(lock == &active->task->tc_SpinLock);
    CHECK(active->lock_depth == 0);
    active->lock_depth = 1;
}

static void mock_task_unlock(int *lock)
{
    stage = "task unlock";
    CHECK(lock == &active->task->tc_SpinLock);
    CHECK(active->lock_depth == 1);
    if (active->local.current_task == NULL)
        CHECK(active->local.fpu_owner == NULL);
    active->last_task_unlock_sequence = ++active->event_sequence;
    active->lock_depth = 0;
}

static int task_may_be_admitted_on_other_hart(struct Task *task)
{
    stage = "admission";
    CHECK(task == active->task);
    return active->running_refs == 0 && active->on_ready &&
        task->tc_State == TS_READY &&
        active->local.current_task != task;
}

static struct p4_signal_effect p4_signal_adapter_switch_commit_locked(
    struct Task *task,
    struct ExecBase *sys_base)
{
    stage = "signal commit";
    struct p4_signal_task_state state;
    void *pending;
    struct p4_signal_effect effect;
    unsigned i;

    CHECK(task == active->task);
    CHECK(sys_base == &active->sys_base);
    CHECK(active->lock_depth == 1);
    CHECK(task->tc_State == TS_RUN);
    CHECK(active->saved->pc == active->regs.pc);
    CHECK(active->saved->sr == active->regs.sr ||
          active->saved->sr ==
              ((active->regs.sr & ~MSTATUS_FS) | MSTATUS_FS_CLEAN));
    CHECK((uintptr_t)task->tc_SPReg == (uintptr_t)active->regs.sp);
    for (i = 0; i < RISCV_REGSAVE_CNT; ++i)
        CHECK(active->saved->x[i] == active->regs.x[i]);
    if (p4_trap_fpu_state_is_dirty(active->regs.sr))
    {
        CHECK((active->saved->Flags & ECF_FPU) != 0);
        CHECK(active->saved->sr ==
              ((active->regs.sr & ~MSTATUS_FS) | MSTATUS_FS_CLEAN));
        CHECK(memcmp(active->saved->fpuContext, active->regs.fpuContext,
                     P4_TRAP_FPU_SPILL_BYTES) == 0);
    }
    active->context_checked_before_commit = 1;

    state.sig_received = task->tc_SigRecvd;
    state.sig_wait = task->tc_SigWait;
    state.sig_except = task->tc_SigExcept;
    state.owner_hart = active->runtime.hart_id;
    state.flags = (uint8_t)task->tc_Flags;
    state.exception_flag_mask = 0;
    state.owner = P4_SIGNAL_OWNER_RUNNING;
    pending = active->runtime.wait_pending;
    /* This runs after the saved image assertions above: token consumption
     * must be ordered after context save. */
    if (pending == task)
        CHECK(active->context_checked_before_commit);
    effect = p4_signal_switch_commit_locked(&state, &pending, task);
    if (effect.status == P4_SIGNAL_STATUS_OK)
    {
        task->tc_SigRecvd = state.sig_received;
        task->tc_SigWait = state.sig_wait;
        task->tc_SigExcept = state.sig_except;
        active->runtime.wait_pending = pending;
    }
    active->last_effect = effect;
    return effect;
}

static int p4_signal_adapter_apply_queue_locked(
    struct Task *task,
    struct ExecBase *sys_base,
    struct p4_signal_effect *effect)
{
    unsigned i;

    stage = "queue apply";
    CHECK(task == active->task);
    CHECK(sys_base == &active->sys_base);
    CHECK(active->lock_depth == 1);
    CHECK(effect != NULL);
    CHECK(effect->status == P4_SIGNAL_STATUS_OK);
    CHECK(effect->queue_transition != P4_SIGNAL_QUEUE_NONE);
    CHECK(active->runtime.wait_pending == NULL);
    CHECK(active->saved->pc == active->regs.pc);
    CHECK(active->saved->sr == active->regs.sr ||
          active->saved->sr ==
              ((active->regs.sr & ~MSTATUS_FS) | MSTATUS_FS_CLEAN));
    CHECK((uintptr_t)task->tc_SPReg == (uintptr_t)active->regs.sp);
    for (i = 0; i < RISCV_REGSAVE_CNT; ++i)
        CHECK(active->saved->x[i] == active->regs.x[i]);
    if (p4_trap_fpu_state_is_dirty(active->regs.sr))
    {
        CHECK((active->saved->Flags & ECF_FPU) != 0);
        CHECK(active->saved->sr ==
              ((active->regs.sr & ~MSTATUS_FS) | MSTATUS_FS_CLEAN));
        CHECK(memcmp(active->saved->fpuContext, active->regs.fpuContext,
                     P4_TRAP_FPU_SPILL_BYTES) == 0);
    }

    ++active->queue_apply_calls;
    ++active->queue_publications;
    if (effect->queue_transition == P4_SIGNAL_QUEUE_RUNNING_TO_READY)
    {
        CHECK(task->tc_State == TS_RUN && active->on_running);
        CHECK(active->running_refs == 1);
        active->on_running = 0;
        active->on_ready = 1;
        task->tc_State = TS_READY;
        active->local.current_task = task;
        active->admission_after_switch =
            task_may_be_admitted_on_other_hart(task);
        CHECK(!active->admission_after_switch);
        active->reject_admission_count++;
        effect->request_reschedule = 1;
        effect->reschedule_hart = active->runtime.hart_id;
    }
    else if (effect->queue_transition == P4_SIGNAL_QUEUE_RUNNING_TO_WAIT)
    {
        CHECK(task->tc_State == TS_RUN && active->on_running);
        CHECK(active->running_refs == 1);
        active->on_running = 0;
        active->on_wait = 1;
        task->tc_State = TS_WAIT;
        active->local.current_task = task;
        active->admission_after_switch =
            task_may_be_admitted_on_other_hart(task);
        CHECK(!active->admission_after_switch);
        active->reject_admission_count++;
    }
    else
    {
        CHECK(0 && "unexpected switch queue transition");
    }
    return 1;
}

static void switch_callback(struct ExecBase *sys_base)
{
    stage = "switch callback";
    struct Task *task = active->task;

    CHECK(sys_base == &active->sys_base);
    CHECK(active->lock_depth == 0);
    CHECK(task->tc_State == TS_READY);
    CHECK(active->on_ready && !active->on_running && !active->on_wait);
    CHECK((task->tc_Flags & TF_SWITCH) != 0);
    CHECK(active->running_refs == 1);
    CHECK(task_may_be_admitted_on_other_hart(task) == 0);
    CHECK(active->queue_publications == 1);
    ++active->callback_calls;

    if (active->callback_mode == CALLBACK_SIGNAL)
    {
        /* Model a reentrant Signal() that serializes on the Task lock. */
        mock_task_lock(&task->tc_SpinLock);
        task->tc_SigRecvd |= UINT32_C(1) << 5;
        mock_task_unlock(&task->tc_SpinLock);
        ++active->callback_signal_calls;
    }
    else if (active->callback_mode == CALLBACK_RETIRE)
    {
        /* The real remover may retire a READY Task in this callback. The
         * retained RUN reference keeps the storage alive until leave. */
        mock_task_lock(&task->tc_SpinLock);
        CHECK(task->tc_State == TS_READY);
        task->tc_State = TS_REMOVED;
        active->on_ready = 0;
        active->scheduler_detached = 1;
        active->service_pending = 1;
        mock_task_unlock(&task->tc_SpinLock);
        ++active->callback_retire_calls;
    }
}

/* Boundary mock for the actual scoped nonlocal wrapper. Its implementation
 * and escape semantics are exercised in p4-callback-escape-test.c. */
int krnP4RunTaskCallback(void (*invoke)(void *), void *argument,
                         struct ExecBase *sys_base)
{
    unsigned callbacks_before = active->callback_calls;

    CHECK(invoke != NULL);
    CHECK(sys_base == &active->sys_base);
    CHECK(active->lock_depth == 0);
    CHECK(active->running_refs == 1);
    ++active->callback_wrapper_calls;
    invoke(argument);
    /* Callback/return hooks are permitted to enable interrupts. The switch
     * owner restores the trap's masked state before resuming its epilogue. */
    KrnCli();
    return active->callback_calls == callbacks_before ? 0 : 1;
}

static void krnP4TaskRecordLeaveRunning(
    struct Task *identity,
    unsigned hart,
    struct ExecBase *sys_base)
{
    stage = "leave running";
    CHECK(identity == active->task);
    CHECK(hart == active->hart);
    CHECK(sys_base == &active->sys_base);
    CHECK(active->lock_depth == 0);
    CHECK(active->local.current_task == NULL);
    CHECK(active->local.fpu_owner == NULL);
    CHECK(active->running_refs == 1);
    CHECK(active->last_task_unlock_sequence != 0);
    active->leave_sequence = ++active->event_sequence;
    CHECK(active->leave_sequence > active->last_task_unlock_sequence);
    if (active->expect_discard_path)
    {
        CHECK(active->detached_calls == 1);
        CHECK(active->service_owner_calls == 1);
    }
    else
    {
        CHECK(active->detached_calls == 0);
        CHECK(active->service_owner_calls == 0);
    }
    --active->running_refs;
    ++active->leave_calls;
    active->task_poisoned = 1;
    active->task_memory_poisoned = 1;
    __asan_poison_memory_region(identity, sizeof(*identity));
}

/* These two production discard prerequisites are deterministic host mocks.
 * The actual krnP4DiscardCurrent implementation is compiled below. */
static int krnP4TaskDetachedLocked(struct Task *task,
                                   struct ExecBase *sys_base)
{
    stage = "discard queue absence";
    CHECK(task == active->task);
    CHECK(sys_base == &active->sys_base);
    CHECK(active->lock_depth == 1);
    ++active->detached_calls;
    return active->scheduler_detached && !active->on_ready &&
        !active->on_wait && !active->on_running;
}

static int krnP4TaskRecordServiceOwnerRunning(struct Task *task,
                                              unsigned hart,
                                              struct ExecBase *sys_base)
{
    stage = "discard service owner";
    CHECK(task == active->task);
    CHECK(sys_base == &active->sys_base);
    CHECK(active->lock_depth == 1);
    CHECK(active->detached_calls == 1);
    ++active->service_owner_calls;
    return active->service_pending && active->running_refs == 1 &&
        active->service_owner_hart == hart && hart == active->hart;
}

static void p4_signal_adapter_poke_after_unlock(
    const struct p4_signal_effect *effect,
    struct ExecBase *sys_base)
{
    stage = "poke";
    CHECK(effect != NULL);
    CHECK(sys_base == &active->sys_base);
    CHECK(active->lock_depth == 0);
    CHECK(active->task_poisoned);
    CHECK(active->running_refs == 0);
    CHECK(effect->status == P4_SIGNAL_STATUS_OK);
    CHECK(effect->queue_transition != P4_SIGNAL_QUEUE_NONE);
    if (effect->request_reschedule)
    {
        ++active->poke_calls;
        active->last_effect = *effect;
    }
}

static void krnP4PublicSpinFatal(int code) __attribute__((noreturn));
static void krnP4PublicSpinFatal(int code)
{
    fail_closed((unsigned)code);
}

void krnP4DiscardCurrent(struct ExecBase *);
#include "../p4_switch.c"
#undef active
#include "../../exec/p4_signal_protocol.c"
#define active fixture_active
#include "../p4_discard.c"

static struct Fixture *fixture_new(unsigned hart)
{
    struct Fixture *fixture = calloc(1, sizeof(*fixture));
    unsigned i;

    CHECK(fixture != NULL);
    fixture->task = calloc(1, sizeof(*fixture->task));
    fixture->iet = calloc(1, sizeof(*fixture->iet));
    fixture->saved = calloc(1, sizeof(*fixture->saved));
    CHECK(fixture->task != NULL && fixture->iet != NULL &&
          fixture->saved != NULL);
    fixture->hart = hart;
    fixture->service_owner_hart = hart;
    fixture->runtime.hart_id = hart;
    fixture->runtime.cpu_local = &fixture->local;
    fixture->local.trap_depth = 1;
    fixture->local.id_nest = -1;
    fixture->local.current_task = fixture->task;
    fixture->local.fpu_owner = fixture->task;
    fixture->running_refs = 1;
    fixture->on_running = 1;
    fixture->task->tc_State = TS_RUN;
    fixture->task->tc_SPReg = (APTR)(uintptr_t)UINT32_C(0xdead0000);
    fixture->task->tc_IDNestCnt = 7;
    fixture->task->tc_SpinLock = 0;
    fixture->task->tc_SPLower = UINT32_C(0x1000);
    fixture->task->tc_SPUpper = UINT32_C(0x3000);
    fixture->task->test_iet = fixture->iet;
    fixture->iet->iet_CpuNumber = hart;
    fixture->iet->iet_ETask.et_RegFrame = fixture->saved;
    fixture->regs.sp = UINT32_C(0x2000);
    fixture->regs.pc = UINT32_C(0x4000);
    fixture->regs.sr = MSTATUS_FS_INITIAL;
    for (i = 0; i < RISCV_REGSAVE_CNT; ++i)
    {
        fixture->regs.x[i] = UINT32_C(0x10101000) + i * UINT32_C(0x101);
        fixture->saved->x[i] = UINT32_C(0xbad00000) + i;
    }
    fixture->regs.sp = UINT32_C(0x2000);
    fixture->saved->pc = UINT32_C(0xbad10000);
    fixture->saved->sr = UINT32_C(0xbad20000);
    fixture->saved->Flags = 0;
    active = fixture;
    stage = "fixture ready";
    mock_mstatus = 0;
    return fixture;
}

static void fixture_finish(struct Fixture *fixture)
{
    if (fixture->task_memory_poisoned)
    {
        __asan_unpoison_memory_region(fixture->task, sizeof(*fixture->task));
        fixture->task_memory_poisoned = 0;
    }
    if (fixture->saved_memory_poisoned)
    {
        __asan_unpoison_memory_region(fixture->saved, sizeof(*fixture->saved));
        fixture->saved_memory_poisoned = 0;
    }
    if (fixture->iet_memory_poisoned)
    {
        __asan_unpoison_memory_region(fixture->iet, sizeof(*fixture->iet));
        fixture->iet_memory_poisoned = 0;
    }
    if (fixture->saved_fpu_memory_poisoned)
    {
        __asan_unpoison_memory_region(fixture->saved_fpu_allocation, 144);
        fixture->saved_fpu_memory_poisoned = 0;
    }
    CHECK(fixture->lock_depth == 0);
    free(fixture->saved_fpu_allocation);
    free(fixture->regs_fpu_allocation);
    free(fixture->saved);
    free(fixture->iet);
    free(fixture->task);
    active = NULL;
    free(fixture);
}

static void run_switch(struct Fixture *fixture)
{
    fatal_armed = 0;
    krnP4Switch(&fixture->regs, &fixture->sys_base);
    if (fixture->task_memory_poisoned)
    {
        __asan_unpoison_memory_region(fixture->task, sizeof(*fixture->task));
        fixture->task_memory_poisoned = 0;
    }
}

static void check_clean_finish(struct Fixture *fixture)
{
    CHECK(fixture->task_poisoned);
    CHECK(fixture->leave_calls == 1);
    CHECK(fixture->running_refs == 0);
    CHECK(fixture->local.current_task == NULL);
    CHECK(fixture->local.fpu_owner == NULL);
    CHECK(fixture->lock_depth == 0);
    CHECK(fixture->poke_calls == 1);
}

static void test_harts_context_fpu_and_callback(void)
{
    unsigned hart;

    for (hart = 0; hart < 2; ++hart)
    {
        struct Fixture *fixture = fixture_new(hart);
        unsigned i;

        fixture->regs.sr = MSTATUS_FS_DIRTY | UINT32_C(0x003);
        fixture->saved_fpu_allocation = aligned_alloc(16, 144);
        fixture->regs_fpu_allocation = aligned_alloc(16, 144);
        fixture->saved->fpuContext = fixture->saved_fpu_allocation;
        fixture->regs.fpuContext = fixture->regs_fpu_allocation;
        CHECK(fixture->saved->fpuContext != NULL &&
              fixture->regs.fpuContext != NULL);
        memset(fixture->saved->fpuContext, 0xa5,
               P4_TRAP_FPU_SPILL_BYTES + 4);
        for (i = 0; i < P4_TRAP_FPU_SPILL_BYTES; ++i)
            ((unsigned char *)fixture->regs.fpuContext)[i] =
                (unsigned char)(i * 13u + hart);
        fixture->task->tc_Flags = TF_SWITCH;
        fixture->task->tc_Switch = switch_callback;
        fixture->callback_mode = (hart == 0) ? CALLBACK_SIGNAL : CALLBACK_RETIRE;
        run_switch(fixture);

        CHECK(fixture->queue_apply_calls == 1);
        CHECK(fixture->queue_publications == 1);
        CHECK(fixture->reject_admission_count == 1);
        CHECK(fixture->callback_calls == 1);
        CHECK(fixture->callback_wrapper_calls == 1);
        CHECK(fixture->runtime.hart_id == hart);
        CHECK(fixture->task_poisoned);
        CHECK(fixture->leave_calls == 1);
        CHECK(fixture->poke_calls == 1);
        CHECK(fixture->saved->Flags & ECF_FPU);
        CHECK(fixture->saved->sr ==
              ((fixture->regs.sr & ~MSTATUS_FS) | MSTATUS_FS_CLEAN));
        CHECK(memcmp(fixture->saved->fpuContext, fixture->regs.fpuContext,
                     P4_TRAP_FPU_SPILL_BYTES) == 0);
        for (i = 0; i < RISCV_REGSAVE_CNT; ++i)
            CHECK(fixture->saved->x[i] == fixture->regs.x[i]);
        CHECK(fixture->saved->pc == fixture->regs.pc);
        CHECK((uintptr_t)fixture->task->tc_SPReg ==
              (uintptr_t)fixture->regs.sp);
        CHECK(fixture->task->tc_IDNestCnt == fixture->local.id_nest);
        if (hart == 0)
        {
            CHECK(fixture->callback_signal_calls == 1);
            CHECK(fixture->task->tc_State == TS_READY);
            CHECK(fixture->on_ready && !fixture->on_running &&
                  !fixture->on_wait);
        }
        else
        {
            CHECK(fixture->callback_retire_calls == 1);
            CHECK(fixture->task->tc_State == TS_REMOVED);
            CHECK(!fixture->on_ready && !fixture->on_running &&
                  !fixture->on_wait);
        }
        check_clean_finish(fixture);
        fixture_finish(fixture);
    }
}

static void test_wait_token_after_context_save(void)
{
    struct Fixture *fixture = fixture_new(0);
    fixture->runtime.wait_pending = fixture->task;
    fixture->task->tc_SigWait = UINT32_C(1) << 4;
    run_switch(fixture);
    CHECK(fixture->runtime.wait_pending == NULL);
    CHECK(fixture->queue_apply_calls == 1);
    CHECK(fixture->task->tc_State == TS_WAIT);
    CHECK(fixture->on_wait && !fixture->on_running && !fixture->on_ready);
    CHECK(fixture->queue_publications == 1);
    CHECK(fixture->poke_calls == 0);
    CHECK(fixture->leave_calls == 1);
    CHECK(fixture->task_poisoned);
    CHECK(fixture->local.current_task == NULL);
    fixture_finish(fixture);

    fixture = fixture_new(1);
    fixture->runtime.wait_pending = fixture->task;
    fixture->task->tc_SigWait = UINT32_C(1) << 4;
    fixture->task->tc_SigRecvd = UINT32_C(1) << 4;
    run_switch(fixture);
    CHECK(fixture->runtime.wait_pending == NULL);
    CHECK(fixture->queue_apply_calls == 1);
    CHECK(fixture->task->tc_State == TS_READY);
    CHECK(fixture->task->tc_SigRecvd == (UINT32_C(1) << 4));
    CHECK(fixture->poke_calls == 1);
    fixture_finish(fixture);
}

static void test_clean_fpu_metadata_and_no_hook_unless_ready(void)
{
    struct Fixture *fixture = fixture_new(0);

    fixture->saved->Flags = ECF_FPU;
    fixture->saved_fpu_allocation = aligned_alloc(16, 144);
    fixture->saved->fpuContext = fixture->saved_fpu_allocation;
    memset(fixture->saved->fpuContext, 0x5a, 144);
    fixture->regs.sr = MSTATUS_FS_INITIAL;
    fixture->task->tc_Flags = TF_SWITCH;
    fixture->task->tc_Switch = switch_callback;
    fixture->callback_mode = CALLBACK_SIGNAL;
    run_switch(fixture);
    CHECK(fixture->saved->Flags & ECF_FPU);
    CHECK(fixture->saved->sr == MSTATUS_FS_INITIAL);
    for (unsigned i = 0; i < P4_TRAP_FPU_SPILL_BYTES; ++i)
        CHECK(((unsigned char *)fixture->saved->fpuContext)[i] == 0x5a);
    CHECK(fixture->callback_calls == 1);
    CHECK(fixture->callback_wrapper_calls == 1);
    CHECK(fixture->callback_signal_calls == 1);
    CHECK(fixture->queue_apply_calls == 1);
    CHECK(fixture->on_ready && !fixture->on_running && !fixture->on_wait);
    fixture_finish(fixture);

    fixture = fixture_new(1);
    fixture->runtime.wait_pending = fixture->task;
    fixture->task->tc_SigWait = UINT32_C(1) << 3;
    fixture->task->tc_Flags = TF_SWITCH;
    fixture->task->tc_Switch = switch_callback;
    run_switch(fixture);
    CHECK(fixture->task->tc_State == TS_WAIT);
    CHECK(fixture->on_wait && !fixture->on_running && !fixture->on_ready);
    CHECK(fixture->callback_calls == 0);
    CHECK(fixture->callback_wrapper_calls == 0);
    CHECK(fixture->queue_apply_calls == 1);
    fixture_finish(fixture);
}

static void expect_fatal_without_queue(struct Fixture *fixture)
{
    int jumped;

    fatal_armed = 1;
    jumped = setjmp(fatal_env);
    if (jumped == 0)
        krnP4Switch(&fixture->regs, &fixture->sys_base);
    fatal_armed = 0;
    CHECK(jumped != 0);
    CHECK(fixture->fatal_code == P4_PUBLIC_SPIN_BAD_STATE);
    CHECK(fixture->queue_apply_calls == 0);
    CHECK(fixture->queue_publications == 0);
    CHECK(fixture->callback_calls == 0);
    CHECK(fixture->poke_calls == 0);
    /* Fatal is a terminal production path. The harness returns via longjmp
     * only to inspect publication counters, then discards lock instrumentation. */
    fixture->lock_depth = 0;
    if (fixture->task_memory_poisoned)
    {
        __asan_unpoison_memory_region(fixture->task, sizeof(*fixture->task));
        fixture->task_memory_poisoned = 0;
    }
}

static void test_null_switch_arguments_fail_closed(void)
{
    struct Fixture *fixture = fixture_new(0);
    int jumped;

    fatal_armed = 1;
    jumped = setjmp(fatal_env);
    if (jumped == 0)
        krnP4Switch(NULL, &fixture->sys_base);
    fatal_armed = 0;
    CHECK(jumped != 0);
    CHECK(fixture->queue_apply_calls == 0);
    CHECK(fixture->queue_publications == 0);
    fixture_finish(fixture);

    fixture = fixture_new(1);
    fatal_armed = 1;
    jumped = setjmp(fatal_env);
    if (jumped == 0)
        krnP4Switch(&fixture->regs, NULL);
    fatal_armed = 0;
    CHECK(jumped != 0);
    CHECK(fixture->queue_apply_calls == 0);
    CHECK(fixture->queue_publications == 0);
    fixture_finish(fixture);
}

static void test_fail_closed_runtime_state_and_context(void)
{
    struct Fixture *fixture;

#define BAD_CASE(statement) do { \
    fixture = fixture_new(0); \
    statement; \
    expect_fatal_without_queue(fixture); \
    fixture_finish(fixture); \
} while (0)

    BAD_CASE(fixture->runtime.hart_id = 2);
    BAD_CASE(fixture->runtime.cpu_local = NULL);
    BAD_CASE(fixture->local.trap_depth = 0);
    BAD_CASE(mock_mstatus = MSTATUS_MIE);
    BAD_CASE(fixture->local.current_task = NULL);
    BAD_CASE(fixture->task->tc_State = TS_READY);
    BAD_CASE(fixture->task->test_iet = NULL);
    BAD_CASE(fixture->iet->iet_CpuNumber = 1);
    BAD_CASE(fixture->iet->iet_ETask.et_RegFrame = NULL);
    BAD_CASE(fixture->regs.sp = 0);
    BAD_CASE(fixture->regs.sp = UINT32_C(0x1000));
    BAD_CASE(fixture->regs.sp = UINT32_C(0x3008));
    BAD_CASE(fixture->regs.sp = UINT32_C(0x2004));
    BAD_CASE(fixture->regs.pc = 0);
    BAD_CASE(fixture->regs.pc = UINT32_C(0x4001));
    BAD_CASE(fixture->local.id_nest = -2);
    BAD_CASE(fixture->local.id_nest = 128);

    BAD_CASE(fixture->regs.sr = MSTATUS_FS_DIRTY);
    BAD_CASE(fixture->regs.sr = MSTATUS_FS_DIRTY; \
             fixture->saved_fpu_allocation = aligned_alloc(16, 144); \
             fixture->saved->fpuContext = fixture->saved_fpu_allocation);
    BAD_CASE(fixture->regs.sr = MSTATUS_FS_DIRTY; \
             fixture->saved_fpu_allocation = aligned_alloc(16, 144); \
             fixture->saved->fpuContext = fixture->saved_fpu_allocation; \
             fixture->regs_fpu_allocation = aligned_alloc(16, 144); \
             fixture->regs.fpuContext = fixture->regs_fpu_allocation; \
             fixture->saved->fpuContext = \
                 (unsigned char *)fixture->saved->fpuContext + 1);
    BAD_CASE(fixture->regs.sr = MSTATUS_FS_DIRTY; \
             fixture->saved_fpu_allocation = aligned_alloc(16, 144); \
             fixture->saved->fpuContext = fixture->saved_fpu_allocation; \
             fixture->regs_fpu_allocation = aligned_alloc(16, 144); \
             fixture->regs.fpuContext = fixture->regs_fpu_allocation; \
             fixture->regs.fpuContext = \
                 (unsigned char *)fixture->regs.fpuContext + 1);
    BAD_CASE(fixture->saved->Flags = ECF_FPU);
    BAD_CASE(fixture->saved->Flags = ECF_FPU; \
             fixture->saved_fpu_allocation = aligned_alloc(16, 144); \
             fixture->saved->fpuContext = fixture->saved_fpu_allocation; \
             fixture->saved->fpuContext = \
                 (unsigned char *)fixture->saved->fpuContext + 1);
#undef BAD_CASE
}

static void test_bad_pending_token_fails_after_save_before_publication(void)
{
    struct Fixture *fixture = fixture_new(0);
    struct Task foreign = { 0 };
    fixture->runtime.wait_pending = &foreign;
    expect_fatal_without_queue(fixture);
    CHECK(fixture->saved->pc == fixture->regs.pc);
    CHECK(fixture->runtime.wait_pending == &foreign);
    fixture_finish(fixture);
}

static void test_retired_switch_discards_without_saving(void)
{
    const unsigned states[] = { TS_REMOVED, TS_INVALID, TS_TOMBSTONED };
    unsigned hart, state_index;

    for (hart = 0; hart < 2; ++hart)
    {
        for (state_index = 0;
             state_index < sizeof(states) / sizeof(states[0]);
             ++state_index)
        {
            struct Fixture *fixture = fixture_new(hart);
            struct ExceptionContext saved_before;
            struct ExceptionContext regs_before;
            unsigned char fpu_before[144];
            unsigned state = states[state_index];

            fixture->task->tc_State = state;
            fixture->task->tc_SPReg = (APTR)(uintptr_t)UINT32_C(0xdead1230);
            fixture->task->tc_IDNestCnt = 37;
            fixture->task->tc_Flags = TF_SWITCH;
            fixture->task->tc_Switch = switch_callback;
            fixture->runtime.wait_pending = fixture->task;
            fixture->on_running = 0;
            fixture->on_ready = 0;
            fixture->on_wait = 0;
            fixture->scheduler_detached = 1;
            fixture->service_pending = 1;
            fixture->service_owner_hart = hart;
            fixture->expect_discard_path = 1;

            fixture->saved_fpu_allocation = aligned_alloc(16, 144);
            CHECK(fixture->saved_fpu_allocation != NULL);
            memset(fixture->saved_fpu_allocation, (int)(0x50u + hart), 144);
            fixture->saved->Flags = ECF_FPU;
            fixture->saved->fpuContext = fixture->saved_fpu_allocation;
            fixture->regs.sr = MSTATUS_FS_DIRTY;
            fixture->regs.fpuContext = fixture->saved_fpu_allocation;
            saved_before = *fixture->saved;
            regs_before = fixture->regs;
            memcpy(fpu_before, fixture->saved_fpu_allocation,
                   sizeof(fpu_before));

            /* Any attempt to read ETask, the saved frame, or its FPU image
             * on the removed fast path must fail under ASan. */
            __asan_poison_memory_region(fixture->iet, sizeof(*fixture->iet));
            fixture->iet_memory_poisoned = 1;
            __asan_poison_memory_region(fixture->saved, sizeof(*fixture->saved));
            fixture->saved_memory_poisoned = 1;
            __asan_poison_memory_region(fixture->saved_fpu_allocation, 144);
            fixture->saved_fpu_memory_poisoned = 1;

            run_switch(fixture);

            __asan_unpoison_memory_region(fixture->iet, sizeof(*fixture->iet));
            fixture->iet_memory_poisoned = 0;
            __asan_unpoison_memory_region(fixture->saved, sizeof(*fixture->saved));
            fixture->saved_memory_poisoned = 0;
            __asan_unpoison_memory_region(fixture->saved_fpu_allocation, 144);
            fixture->saved_fpu_memory_poisoned = 0;

            CHECK(fixture->get_etask_calls == 0);
            CHECK(fixture->get_int_etask_calls == 0);
            CHECK(fixture->queue_apply_calls == 0);
            CHECK(fixture->queue_publications == 0);
            CHECK(fixture->callback_calls == 0);
            CHECK(fixture->callback_wrapper_calls == 0);
            CHECK(fixture->callback_signal_calls == 0);
            CHECK(fixture->callback_retire_calls == 0);
            CHECK(fixture->poke_calls == 0);
            CHECK(fixture->detached_calls == 1);
            CHECK(fixture->service_owner_calls == 1);
            CHECK(fixture->task->tc_State == state);
            CHECK((uintptr_t)fixture->task->tc_SPReg == UINT32_C(0xdead1230));
            CHECK(fixture->task->tc_IDNestCnt == 37);
            CHECK(fixture->task_poisoned);
            CHECK(fixture->leave_calls == 1);
            CHECK(fixture->leave_sequence >
                  fixture->last_task_unlock_sequence);
            CHECK(fixture->running_refs == 0);
            CHECK(fixture->runtime.wait_pending == NULL);
            CHECK(fixture->local.current_task == NULL);
            CHECK(fixture->local.fpu_owner == NULL);
            CHECK(fixture->lock_depth == 0);
            CHECK(memcmp(fixture->saved, &saved_before,
                         sizeof(saved_before)) == 0);
            CHECK(memcmp(&fixture->regs, &regs_before,
                         sizeof(regs_before)) == 0);
            CHECK(memcmp(fixture->saved_fpu_allocation, fpu_before,
                         sizeof(fpu_before)) == 0);
            fixture_finish(fixture);
        }
    }
}

static void test_retired_switch_requires_detached_service_owner(void)
{
    const unsigned states[] = { TS_REMOVED, TS_INVALID, TS_TOMBSTONED };
    unsigned hart, state_index, failure;

    for (hart = 0; hart < 2; ++hart)
    {
        for (state_index = 0;
             state_index < sizeof(states) / sizeof(states[0]);
             ++state_index)
        {
            for (failure = 0; failure < 2; ++failure)
            {
                struct Fixture *fixture = fixture_new(hart);
                unsigned state = states[state_index];
                unsigned int old_state;
                struct Task *old_current;
                struct Task *old_wait_pending;

                fixture->task->tc_State = state;
                fixture->runtime.wait_pending = fixture->task;
                fixture->on_running = 0;
                fixture->on_ready = 0;
                fixture->on_wait = 0;
                fixture->scheduler_detached = 1;
                fixture->service_pending = 1;
                fixture->service_owner_hart = hart;
                if (failure == 0)
                    fixture->service_pending = 0;
                else
                {
                    fixture->scheduler_detached = 0;
                    fixture->on_wait = 1;
                }

                old_state = fixture->task->tc_State;
                old_current = fixture->local.current_task;
                old_wait_pending = fixture->runtime.wait_pending;
                expect_fatal_without_queue(fixture);

                CHECK(fixture->task->tc_State == old_state);
                CHECK(fixture->local.current_task == old_current);
                CHECK(fixture->local.fpu_owner == fixture->task);
                CHECK(fixture->runtime.wait_pending == old_wait_pending);
                CHECK(fixture->running_refs == 1);
                CHECK(fixture->leave_calls == 0);
                CHECK(!fixture->task_poisoned);
                CHECK(fixture->detached_calls == 1);
                CHECK(fixture->service_owner_calls ==
                      (failure == 0 ? 1u : 0u));
                CHECK(fixture->get_etask_calls == 0);
                CHECK(fixture->get_int_etask_calls == 0);
                fixture_finish(fixture);
            }
        }
    }
}

int main(void)
{
    test_harts_context_fpu_and_callback();
    test_wait_token_after_context_save();
    test_clean_fpu_metadata_and_no_hook_unless_ready();
    test_fail_closed_runtime_state_and_context();
    test_bad_pending_token_fails_after_save_before_publication();
    test_null_switch_arguments_fail_closed();
    test_retired_switch_discards_without_saving();
    test_retired_switch_requires_detached_service_owner();
    printf("p4-switch-test: %u checks passed\n", checks);
    return 0;
}
