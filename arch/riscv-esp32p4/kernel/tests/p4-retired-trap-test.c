/*
 * Host fixture for the production retired-trap and no-save discard helpers.
 * The outer routing block is extracted verbatim from kernel_traps.c by the
 * documented test command. Queue, registry, runtime and CPU-dispatch effects
 * are deterministic mocks.
 */
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sanitizer/asan_interface.h>

#define P4_RETIRED_TRAP_TEST 1
#define P4_DISCARD_TEST 1
#define __AROSEXEC_SMP__ 1

#define TS_INVALID 0u
#define TS_RUN 1u
#define TS_READY 2u
#define TS_WAIT 3u
#define TS_REMOVED 4u
#define TS_TOMBSTONED 5u
#define SPINLOCK_MODE_WRITE 1u
#define MSTATUS_MIE UINT32_C(0x00000008)
#define P4_PUBLIC_SPIN_BAD_STATE 1
#define TRAP_RESCHEDULE 1
#define TRAP_SYSCALL 2
#define CTX_REG_A7 14u
#define csr_read(reg) (mock_mstatus)
#define EXEC_SPINLOCK_LOCK(lock, owner, mode) mock_task_lock((lock), (mode))
#define EXEC_SPINLOCK_UNLOCK(lock) mock_task_unlock((lock))
#define GetETask(task) mock_get_etask((task))
#define GetIntETask(task) mock_get_int_etask((task))

struct ExecBase
{
    unsigned token;
};

struct ExceptionContext
{
    uintptr_t x[29];
    uintptr_t pc;
    uintptr_t sr;
    uintptr_t Flags;
    void *fpuContext;
    void *vecContext;
};

struct ETask
{
    struct ExceptionContext *et_RegFrame;
};

struct IntETask
{
    struct ETask iet_ETask;
    unsigned iet_CpuNumber;
};

struct Task
{
    unsigned tc_State;
    int tc_SpinLock;
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
    struct p4_exec_runtime *runtime_result;
    struct Task *task;
    struct IntETask *iet;
    struct ExceptionContext *old_context;
    unsigned char *old_fpu;
    struct ExceptionContext *entry_context;
    struct ExceptionContext dispatch_context;
    unsigned hart;
    unsigned running_refs;
    unsigned lock_depth;
    unsigned event_sequence;
    unsigned last_task_unlock;
    unsigned leave_event;
    unsigned dispatch_event;
    unsigned task_state_reads;
    unsigned etask_accesses;
    unsigned intetask_accesses;
    unsigned detached_calls;
    unsigned service_owner_calls;
    unsigned leave_calls;
    unsigned dispatch_calls;
    unsigned generic_exit_calls;
    unsigned generic_syscall_calls;
    unsigned mock_ipi_latched;
    unsigned fatal_code;
    unsigned service_owner_hart;
    int scheduler_detached;
    int service_pending;
    int on_ready;
    int on_wait;
    int on_running;
    int task_poisoned;
    int task_memory_poisoned;
    int old_context_poisoned;
    int old_fpu_poisoned;
    int iet_poisoned;
    int entry_context_poisoned;
    int retire_after_livecheck;
};

static struct Fixture *active;
static jmp_buf fatal_env;
static int fatal_armed;
static unsigned mock_mstatus;
static const char *stage = "fixture";
static unsigned checks;

static struct ETask *mock_get_etask(struct Task *task);
static struct IntETask *mock_get_int_etask(struct Task *task);

#define CHECK(condition) do { \
    ++checks; \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: check failed: %s (stage=%s)\n", \
                __FILE__, __LINE__, #condition, stage); \
        exit(1); \
    } \
} while (0)

static struct p4_exec_runtime *p4_exec_runtime_current(
    struct ExecBase *sys_base)
{
    stage = "runtime lookup";
    return sys_base == &active->sys_base ? active->runtime_result : NULL;
}

static void mock_task_lock(int *lock, unsigned mode)
{
    stage = "Task lock";
    CHECK(lock == &active->task->tc_SpinLock);
    CHECK(mode == SPINLOCK_MODE_WRITE);
    CHECK(active->lock_depth == 0);
    active->lock_depth = 1;
}

static void mock_task_unlock(int *lock)
{
    stage = "Task unlock";
    CHECK(lock == &active->task->tc_SpinLock);
    CHECK(active->lock_depth == 1);
    if (active->local.current_task == NULL)
        CHECK(active->local.fpu_owner == NULL);
    active->last_task_unlock = ++active->event_sequence;
    active->lock_depth = 0;
}

static __attribute__((unused)) struct ETask *mock_get_etask(struct Task *task)
{
    ++active->etask_accesses;
    return task != NULL && task->test_iet != NULL ?
        &task->test_iet->iet_ETask : NULL;
}

static __attribute__((unused)) struct IntETask *mock_get_int_etask(
    struct Task *task)
{
    ++active->intetask_accesses;
    return task != NULL ? task->test_iet : NULL;
}

static int p4_exec_runtime_set_current_task(
    struct ExecBase *sys_base, struct Task *task)
{
    stage = "clear current task";
    CHECK(sys_base == &active->sys_base);
    CHECK(active->lock_depth == 1);
    CHECK(active->local.current_task == active->task);
    CHECK(active->local.fpu_owner == active->task);
    active->local.current_task = task;
    return 1;
}

static int krnP4TaskDetachedLocked(struct Task *task,
                                  struct ExecBase *sys_base)
{
    stage = "detached proof";
    CHECK(task == active->task);
    CHECK(sys_base == &active->sys_base);
    CHECK(active->lock_depth == 1);
    ++active->detached_calls;
    return active->scheduler_detached && !active->on_ready &&
        !active->on_wait && !active->on_running;
}

static int krnP4TaskRecordServiceOwnerRunning(
    struct Task *task, unsigned hart, struct ExecBase *sys_base)
{
    stage = "service owner proof";
    CHECK(task == active->task);
    CHECK(sys_base == &active->sys_base);
    CHECK(active->lock_depth == 1);
    CHECK(active->detached_calls == 1);
    ++active->service_owner_calls;
    return active->service_pending && active->running_refs == 1 &&
        hart == active->hart && hart == active->service_owner_hart;
}

static void krnP4TaskRecordLeaveRunning(
    struct Task *identity, unsigned hart, struct ExecBase *sys_base)
{
    stage = "RUN release";
    CHECK(identity == active->task);
    CHECK(sys_base == &active->sys_base);
    CHECK(hart == active->hart);
    CHECK(active->lock_depth == 0);
    CHECK(active->local.current_task == NULL);
    CHECK(active->local.fpu_owner == NULL);
    CHECK(active->runtime.wait_pending == NULL);
    CHECK(active->running_refs == 1);
    CHECK(active->detached_calls == 1);
    CHECK(active->service_owner_calls == 1);
    CHECK(active->last_task_unlock != 0);
    active->leave_event = ++active->event_sequence;
    CHECK(active->leave_event > active->last_task_unlock);
    --active->running_refs;
    ++active->leave_calls;
    active->task_poisoned = 1;
    active->task_memory_poisoned = 1;
    __asan_poison_memory_region(identity, sizeof(*identity));
}

static void krnP4PublicSpinFatal(int code) __attribute__((noreturn));
static void krnP4PublicSpinFatal(int code)
{
    active->fatal_code = (unsigned)code;
    if (fatal_armed)
        longjmp(fatal_env, 1);
    fprintf(stderr, "unexpected production fatal: %s\n", stage);
    abort();
}

static void cpu_Dispatch(struct ExceptionContext *ctx);
static int krnP4RetiredTrap(struct ExceptionContext *, struct ExecBase *);
static void krnP4DiscardCurrent(struct ExecBase *);

#include "../p4_retired_trap.c"
#include "../p4_discard.c"

static void cpu_Dispatch(struct ExceptionContext *ctx)
{
    stage = "new frame dispatch";
    CHECK(ctx == active->entry_context);
    CHECK(active->lock_depth == 0);
    CHECK(active->running_refs == 0);
    CHECK(active->task_poisoned);
    CHECK(active->local.current_task == NULL);
    CHECK(active->local.fpu_owner == NULL);
    CHECK(active->runtime.wait_pending == NULL);
    CHECK(active->leave_event > active->last_task_unlock);
    active->dispatch_event = ++active->event_sequence;
    CHECK(active->dispatch_event > active->leave_event);
    if (active->entry_context_poisoned)
    {
        __asan_unpoison_memory_region(ctx, sizeof(*ctx));
        active->entry_context_poisoned = 0;
    }
    memcpy(ctx, &active->dispatch_context, sizeof(*ctx));
    ++active->dispatch_calls;
}

static void core_ExitInterrupt(struct ExceptionContext *ctx)
{
    stage = "generic interrupt exit";
    CHECK(active->lock_depth == 0);
    CHECK(ctx == active->entry_context);
    ++active->generic_exit_calls;
}

static void core_SysCall(int syscall, struct ExceptionContext *ctx)
{
    stage = "generic syscall";
    CHECK(active->lock_depth == 0);
    CHECK(ctx == active->entry_context);
    CHECK(syscall == (int)ctx->x[CTX_REG_A7]);
    ++active->generic_syscall_calls;
    if (active->retire_after_livecheck)
    {
        /* Model remote removal after the trap helper's live check. Its poke
         * cannot run until this trap returns, so the next trap is latched. */
        CHECK(active->task->tc_State == TS_RUN);
        CHECK(active->running_refs == 1);
        CHECK(active->service_pending == 0);
        mock_task_lock(&active->task->tc_SpinLock, SPINLOCK_MODE_WRITE);
        active->task->tc_State = TS_REMOVED;
        mock_task_unlock(&active->task->tc_SpinLock);
        active->on_ready = 0;
        active->on_wait = 0;
        active->on_running = 0;
        active->scheduler_detached = 1;
        active->service_pending = 1;
        active->service_owner_hart = active->hart;
        active->mock_ipi_latched = 1;
        active->retire_after_livecheck = 0;
    }
}

/* This include is generated verbatim from kernel_traps.c's outer routing
 * branch. It supplies SysBase, ctx, action, and local in this scope. */
static void run_extracted_outer_route(
    struct ExceptionContext *ctx, struct ExecBase *SysBase, int action)
{
    struct p4_cpu_local *local = &active->local;
#include "p4-retired-trap-route.inc"
}

static void deliver_latched_reschedule(struct Fixture *fixture)
{
    stage = "latched mock IPI delivery";
    CHECK(fixture->mock_ipi_latched == 1);
    fixture->mock_ipi_latched = 0;
    run_extracted_outer_route(fixture->entry_context, &fixture->sys_base,
                              TRAP_RESCHEDULE);
}

static struct Fixture *fixture_new(unsigned hart, unsigned task_state,
                                   int with_current_task)
{
    struct Fixture *fixture = calloc(1, sizeof(*fixture));
    CHECK(fixture != NULL);
    fixture->task = calloc(1, sizeof(*fixture->task));
    fixture->iet = calloc(1, sizeof(*fixture->iet));
    fixture->old_context = calloc(1, sizeof(*fixture->old_context));
    fixture->old_fpu = calloc(1, 144);
    fixture->entry_context = calloc(1, sizeof(*fixture->entry_context));
    CHECK(fixture->task != NULL && fixture->iet != NULL &&
          fixture->old_context != NULL && fixture->old_fpu != NULL &&
          fixture->entry_context != NULL);

    fixture->hart = hart;
    fixture->service_owner_hart = hart;
    fixture->runtime.hart_id = hart;
    fixture->runtime.cpu_local = &fixture->local;
    fixture->runtime_result = &fixture->runtime;
    fixture->local.trap_depth = 1;
    fixture->local.id_nest = 5; /* Deliberately Forbid'd. */
    fixture->task->tc_State = task_state;
    fixture->task->test_iet = fixture->iet;
    fixture->iet->iet_CpuNumber = hart;
    fixture->iet->iet_ETask.et_RegFrame = fixture->old_context;
    memset(fixture->old_context, 0xa5, sizeof(*fixture->old_context));
    fixture->old_context->fpuContext = fixture->old_fpu;
    memset(fixture->old_fpu, 0x6d, 144);
    memset(fixture->entry_context, 0x19, sizeof(*fixture->entry_context));
    fixture->entry_context->pc = UINT32_C(0x11112222);
    fixture->entry_context->x[CTX_REG_A7] = UINT32_C(0x77);
    memset(&fixture->dispatch_context, 0x3c,
           sizeof(fixture->dispatch_context));
    fixture->dispatch_context.pc = UINT32_C(0x98765432);
    fixture->dispatch_context.x[CTX_REG_A7] = UINT32_C(0x55);

    if (with_current_task)
    {
        fixture->local.current_task = fixture->task;
        fixture->local.fpu_owner = fixture->task;
        fixture->running_refs = 1;
    }
    active = fixture;
    mock_mstatus = 0;
    stage = "fixture initialized";
    return fixture;
}

static void fixture_finish(struct Fixture *fixture)
{
    if (fixture->task_memory_poisoned)
    {
        __asan_unpoison_memory_region(fixture->task, sizeof(*fixture->task));
        fixture->task_memory_poisoned = 0;
    }
    if (fixture->iet_poisoned)
    {
        __asan_unpoison_memory_region(fixture->iet, sizeof(*fixture->iet));
        fixture->iet_poisoned = 0;
    }
    if (fixture->old_context_poisoned)
    {
        __asan_unpoison_memory_region(fixture->old_context,
                                      sizeof(*fixture->old_context));
        fixture->old_context_poisoned = 0;
    }
    if (fixture->old_fpu_poisoned)
    {
        __asan_unpoison_memory_region(fixture->old_fpu, 144);
        fixture->old_fpu_poisoned = 0;
    }
    if (fixture->entry_context_poisoned)
    {
        __asan_unpoison_memory_region(fixture->entry_context,
                                      sizeof(*fixture->entry_context));
        fixture->entry_context_poisoned = 0;
    }
    CHECK(fixture->lock_depth == 0);
    free(fixture->entry_context);
    free(fixture->old_fpu);
    free(fixture->old_context);
    free(fixture->iet);
    free(fixture->task);
    active = NULL;
    free(fixture);
}

static void poison_retired_task_images(struct Fixture *fixture)
{
    __asan_poison_memory_region(fixture->iet, sizeof(*fixture->iet));
    fixture->iet_poisoned = 1;
    __asan_poison_memory_region(fixture->old_context,
                                sizeof(*fixture->old_context));
    fixture->old_context_poisoned = 1;
    __asan_poison_memory_region(fixture->old_fpu, 144);
    fixture->old_fpu_poisoned = 1;
    __asan_poison_memory_region(fixture->entry_context,
                                sizeof(*fixture->entry_context));
    fixture->entry_context_poisoned = 1;
}

static void unpoison_task_images(struct Fixture *fixture)
{
    if (fixture->iet_poisoned)
    {
        __asan_unpoison_memory_region(fixture->iet, sizeof(*fixture->iet));
        fixture->iet_poisoned = 0;
    }
    if (fixture->old_context_poisoned)
    {
        __asan_unpoison_memory_region(fixture->old_context,
                                      sizeof(*fixture->old_context));
        fixture->old_context_poisoned = 0;
    }
    if (fixture->old_fpu_poisoned)
    {
        __asan_unpoison_memory_region(fixture->old_fpu, 144);
        fixture->old_fpu_poisoned = 0;
    }
}

static void expect_fatal_direct(struct Fixture *fixture,
                                struct ExceptionContext *ctx,
                                struct ExecBase *sys_base)
{
    fatal_armed = 1;
    if (setjmp(fatal_env) == 0)
    {
        (void)krnP4RetiredTrap(ctx, sys_base);
        fprintf(stderr, "expected retired-trap fatal at %s\n", stage);
        exit(1);
    }
    fatal_armed = 0;
    CHECK(fixture->fatal_code == P4_PUBLIC_SPIN_BAD_STATE);
    CHECK(fixture->dispatch_calls == 0);
    CHECK(fixture->leave_calls == 0);
    CHECK(fixture->generic_exit_calls == 0);
    CHECK(fixture->generic_syscall_calls == 0);
    fixture->lock_depth = 0; /* Production fatal does not return/unlock. */
}

static void test_removed_trap_discards_then_installs_new_frame(void)
{
    const unsigned states[] = { TS_REMOVED, TS_INVALID, TS_TOMBSTONED };
    unsigned hart, state_index;

    for (hart = 0; hart < 2; ++hart)
    {
        for (state_index = 0;
             state_index < sizeof(states) / sizeof(states[0]);
             ++state_index)
        {
            struct Fixture *fixture = fixture_new(hart, states[state_index], 1);
            struct ExceptionContext expected_frame = fixture->dispatch_context;
            struct ExceptionContext old_saved;
            unsigned char old_fpu[144];
            unsigned state = states[state_index];
            int action = ((hart + state_index) & 1u) ?
                TRAP_RESCHEDULE : TRAP_SYSCALL;

            fixture->scheduler_detached = 1;
            fixture->service_pending = 1;
            fixture->service_owner_hart = hart;
            fixture->runtime.wait_pending = fixture->task;
            fixture->on_ready = 0;
            fixture->on_wait = 0;
            fixture->on_running = 0;
            old_saved = *fixture->old_context;
            memcpy(old_fpu, fixture->old_fpu, sizeof(old_fpu));
            poison_retired_task_images(fixture);

            run_extracted_outer_route(fixture->entry_context,
                                      &fixture->sys_base, action);

            if (fixture->task_memory_poisoned)
            {
                __asan_unpoison_memory_region(fixture->task,
                                              sizeof(*fixture->task));
                fixture->task_memory_poisoned = 0;
            }
            unpoison_task_images(fixture);
            CHECK(fixture->entry_context_poisoned == 0);
            CHECK(fixture->dispatch_calls == 1);
            CHECK(fixture->dispatch_event > fixture->leave_event);
            CHECK(memcmp(fixture->entry_context, &expected_frame,
                         sizeof(expected_frame)) == 0);
            CHECK(fixture->dispatch_context.pc != UINT32_C(0x11112222));
            CHECK(fixture->task->tc_State == state);
            CHECK(memcmp(fixture->old_context, &old_saved,
                         sizeof(old_saved)) == 0);
            CHECK(memcmp(fixture->old_fpu, old_fpu, sizeof(old_fpu)) == 0);
            CHECK(fixture->etask_accesses == 0);
            CHECK(fixture->intetask_accesses == 0);
            CHECK(fixture->detached_calls == 1);
            CHECK(fixture->service_owner_calls == 1);
            CHECK(fixture->leave_calls == 1);
            CHECK(fixture->running_refs == 0);
            CHECK(fixture->local.current_task == NULL);
            CHECK(fixture->local.fpu_owner == NULL);
            CHECK(fixture->runtime.wait_pending == NULL);
            CHECK(fixture->lock_depth == 0);
            CHECK(fixture->generic_exit_calls == 0);
            CHECK(fixture->generic_syscall_calls == 0);
            CHECK(fixture->task_poisoned);
            fixture_finish(fixture);
        }
    }
}

static void test_live_and_empty_owner_fall_through(void)
{
    struct Fixture *fixture = fixture_new(0, TS_RUN, 1);
    struct ExceptionContext original = *fixture->entry_context;

    run_extracted_outer_route(fixture->entry_context, &fixture->sys_base,
                              TRAP_SYSCALL);
    CHECK(fixture->dispatch_calls == 0);
    CHECK(fixture->leave_calls == 0);
    CHECK(fixture->generic_syscall_calls == 1);
    CHECK(fixture->generic_exit_calls == 0);
    CHECK(fixture->local.current_task == fixture->task);
    CHECK(fixture->running_refs == 1);
    CHECK(memcmp(fixture->entry_context, &original, sizeof(original)) == 0);
    fixture_finish(fixture);

    fixture = fixture_new(0, TS_RUN, 1);
    run_extracted_outer_route(fixture->entry_context, &fixture->sys_base,
                              TRAP_RESCHEDULE);
    CHECK(fixture->dispatch_calls == 0);
    CHECK(fixture->leave_calls == 0);
    CHECK(fixture->generic_syscall_calls == 0);
    CHECK(fixture->generic_exit_calls == 1);
    CHECK(fixture->local.current_task == fixture->task);
    fixture_finish(fixture);

    fixture = fixture_new(1, TS_REMOVED, 0);
    original = *fixture->entry_context;
    run_extracted_outer_route(fixture->entry_context, &fixture->sys_base,
                              TRAP_SYSCALL);
    CHECK(fixture->dispatch_calls == 0);
    CHECK(fixture->leave_calls == 0);
    CHECK(fixture->generic_syscall_calls == 1);
    CHECK(fixture->generic_exit_calls == 0);
    CHECK(fixture->local.current_task == NULL);
    CHECK(fixture->entry_context != NULL);
    CHECK(memcmp(fixture->entry_context, &original, sizeof(original)) == 0);
    fixture_finish(fixture);

    fixture = fixture_new(1, TS_REMOVED, 0);
    run_extracted_outer_route(fixture->entry_context, &fixture->sys_base,
                              TRAP_RESCHEDULE);
    CHECK(fixture->dispatch_calls == 0);
    CHECK(fixture->generic_syscall_calls == 0);
    CHECK(fixture->generic_exit_calls == 1);
    CHECK(fixture->local.current_task == NULL);
    fixture_finish(fixture);
}

static void test_nested_route_is_skipped(void)
{
    struct Fixture *fixture = fixture_new(0, TS_REMOVED, 1);
    fixture->local.trap_depth = 2;
    run_extracted_outer_route(fixture->entry_context, &fixture->sys_base,
                              TRAP_SYSCALL);
    CHECK(fixture->task->tc_State == TS_REMOVED);
    CHECK(fixture->dispatch_calls == 0);
    CHECK(fixture->detached_calls == 0);
    CHECK(fixture->service_owner_calls == 0);
    CHECK(fixture->leave_calls == 0);
    CHECK(fixture->generic_exit_calls == 0);
    CHECK(fixture->generic_syscall_calls == 0);
    CHECK(fixture->lock_depth == 0);
    fixture_finish(fixture);
}

static void test_retirement_after_live_check_waits_for_latched_trap(void)
{
    struct Fixture *fixture = fixture_new(1, TS_RUN, 1);
    struct ExceptionContext old_saved = *fixture->old_context;
    unsigned char old_fpu[144];

    fixture->retire_after_livecheck = 1;
    memcpy(old_fpu, fixture->old_fpu, sizeof(old_fpu));

    /* First route sees live state and falls through to the old syscall. The
     * mock remote retire then latches its reschedule while RUN keeps storage
     * alive. No early discard is allowed in this already-selected branch. */
    run_extracted_outer_route(fixture->entry_context, &fixture->sys_base,
                              TRAP_SYSCALL);
    CHECK(fixture->generic_syscall_calls == 1);
    CHECK(fixture->generic_exit_calls == 0);
    CHECK(fixture->dispatch_calls == 0);
    CHECK(fixture->leave_calls == 0);
    CHECK(fixture->task->tc_State == TS_REMOVED);
    CHECK(fixture->running_refs == 1);
    CHECK(fixture->local.current_task == fixture->task);
    CHECK(fixture->mock_ipi_latched == 1);

    /* The next outer trap observes retirement before generic scheduling and
     * must take the no-save discard/new-frame path instead. */
    __asan_poison_memory_region(fixture->iet, sizeof(*fixture->iet));
    fixture->iet_poisoned = 1;
    __asan_poison_memory_region(fixture->old_context,
                                sizeof(*fixture->old_context));
    fixture->old_context_poisoned = 1;
    __asan_poison_memory_region(fixture->old_fpu, 144);
    fixture->old_fpu_poisoned = 1;
    __asan_poison_memory_region(fixture->entry_context,
                                sizeof(*fixture->entry_context));
    fixture->entry_context_poisoned = 1;
    deliver_latched_reschedule(fixture);

    if (fixture->task_memory_poisoned)
    {
        __asan_unpoison_memory_region(fixture->task, sizeof(*fixture->task));
        fixture->task_memory_poisoned = 0;
    }
    unpoison_task_images(fixture);
    CHECK(fixture->entry_context_poisoned == 0);
    CHECK(fixture->dispatch_calls == 1);
    CHECK(fixture->leave_calls == 1);
    CHECK(fixture->dispatch_event > fixture->leave_event);
    CHECK(fixture->generic_syscall_calls == 1);
    CHECK(fixture->generic_exit_calls == 0);
    CHECK(fixture->running_refs == 0);
    CHECK(fixture->local.current_task == NULL);
    CHECK(fixture->local.fpu_owner == NULL);
    CHECK(fixture->runtime.wait_pending == NULL);
    CHECK(fixture->etask_accesses == 0);
    CHECK(fixture->intetask_accesses == 0);
    CHECK(memcmp(fixture->old_context, &old_saved, sizeof(old_saved)) == 0);
    CHECK(memcmp(fixture->old_fpu, old_fpu, sizeof(old_fpu)) == 0);
    fixture_finish(fixture);
}

static void test_retired_handoff_requires_service_proofs(void)
{
    unsigned failure;
    for (failure = 0; failure < 2; ++failure)
    {
        struct Fixture *fixture = fixture_new(1, TS_TOMBSTONED, 1);
        struct Task *old_current = fixture->local.current_task;
        struct Task *old_wait = fixture->runtime.wait_pending;
        fixture->scheduler_detached = failure == 0;
        fixture->service_pending = failure != 0;
        fixture->service_owner_hart = 1;
        fixture->runtime.wait_pending = fixture->task;
        old_wait = fixture->runtime.wait_pending;

        fatal_armed = 1;
        if (setjmp(fatal_env) == 0)
        {
            run_extracted_outer_route(fixture->entry_context,
                                      &fixture->sys_base, TRAP_SYSCALL);
            fprintf(stderr, "expected refusal for retired handoff %u\n", failure);
            exit(1);
        }
        fatal_armed = 0;
        CHECK(fixture->fatal_code == P4_PUBLIC_SPIN_BAD_STATE);
        CHECK(fixture->dispatch_calls == 0);
        CHECK(fixture->generic_exit_calls == 0);
        CHECK(fixture->generic_syscall_calls == 0);
        CHECK(fixture->leave_calls == 0);
        CHECK(fixture->running_refs == 1);
        CHECK(fixture->local.current_task == old_current);
        CHECK(fixture->local.fpu_owner == fixture->task);
        CHECK(fixture->runtime.wait_pending == old_wait);
        CHECK(fixture->task->tc_State == TS_TOMBSTONED);
        CHECK(fixture->detached_calls == 1);
        CHECK(fixture->service_owner_calls == (failure == 0 ? 1u : 0u));
        fixture->lock_depth = 0;
        fixture_finish(fixture);
    }
}

static void test_helper_invalid_inputs_fail_closed(void)
{
    struct Fixture *fixture;

#define BAD_CASE(mutation, context) do { \
    fixture = fixture_new(0, TS_REMOVED, 1); \
    mutation; \
    expect_fatal_direct(fixture, (context), &fixture->sys_base); \
    CHECK(fixture->detached_calls == 0); \
    CHECK(fixture->lock_depth == 0); \
    fixture_finish(fixture); \
} while (0)

    BAD_CASE(fixture->runtime_result = NULL, fixture->entry_context);
    BAD_CASE(fixture->runtime.hart_id = 2, fixture->entry_context);
    BAD_CASE(fixture->runtime.cpu_local = NULL, fixture->entry_context);
    BAD_CASE(fixture->local.trap_depth = 0, fixture->entry_context);
    BAD_CASE(fixture->local.trap_depth = 2, fixture->entry_context);
    BAD_CASE(mock_mstatus = MSTATUS_MIE, fixture->entry_context);
    BAD_CASE((void)0, NULL);
#undef BAD_CASE
}

int main(void)
{
    test_removed_trap_discards_then_installs_new_frame();
    test_live_and_empty_owner_fall_through();
    test_nested_route_is_skipped();
    test_retirement_after_live_check_waits_for_latched_trap();
    test_retired_handoff_requires_service_proofs();
    test_helper_invalid_inputs_fail_closed();
    printf("p4-retired-trap-test: %u checks passed\n", checks);
    return 0;
}
