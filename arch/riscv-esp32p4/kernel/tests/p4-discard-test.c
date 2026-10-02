/*
 * Extracted-source fixture for p4_discard.c. The production helper is compiled
 * verbatim with only its platform includes replaced by small mock bindings.
 * Build with Clang ASan/UBSan; see the paired evidence report.
 */
#define P4_DISCARD_TEST 1

#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sanitizer/asan_interface.h>

#define CHECK(condition) do { \
    ++checks; \
    if (!(condition)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        return 0; \
    } \
} while (0)

enum
{
    TS_INVALID = 0,
    TS_READY = 1,
    TS_WAIT = 2,
    TS_RUN = 3,
    TS_REMOVED = 4,
    TS_TOMBSTONED = 5
};

#define MSTATUS_MIE UINT32_C(0x8)
#define SPINLOCK_MODE_WRITE 1u
#define P4_PUBLIC_SPIN_BAD_STATE 7
#define csr_read(reg) fixture_mstatus

struct ExecBase
{
    unsigned token;
};

typedef struct
{
    int held;
} spinlock_t;

struct Task
{
    spinlock_t tc_SpinLock;
    unsigned int tc_State;
};

struct p4_cpu_local
{
    unsigned int trap_depth;
    struct Task *current_task;
    void *fpu_owner;
};

struct p4_exec_runtime
{
    unsigned int hart_id;
    struct p4_cpu_local *cpu_local;
    struct Task *wait_pending;
};

static unsigned int fixture_mstatus;
static struct ExecBase fixture_sysbase;
static struct Task fixture_task;
static struct Task fixture_other_task;
static struct p4_cpu_local fixture_cpu_local;
static struct p4_exec_runtime fixture_runtime;
static struct p4_exec_runtime *fixture_runtime_result;
static jmp_buf fatal_jump;
static int expect_fatal;
static int fatal_status;
static unsigned int lock_calls;
static unsigned int unlock_calls;
static unsigned int detached_calls;
static unsigned int service_owner_calls;
static unsigned int set_current_calls;
static unsigned int leave_calls;
static unsigned int leave_hart;
static int fixture_detached;
static int fixture_service_owner;
static int event_order;
static int unlock_event;
static int leave_event;
static unsigned int checks;

static void fixture_lock(spinlock_t *lock, void *hook, unsigned int mode)
{
    (void)hook;
    if (lock != &fixture_task.tc_SpinLock || mode != SPINLOCK_MODE_WRITE ||
        lock->held)
        abort();
    ++lock_calls;
    lock->held = 1;
}

static void fixture_unlock(spinlock_t *lock)
{
    if (lock != &fixture_task.tc_SpinLock || !lock->held)
        abort();
    lock->held = 0;
    ++unlock_calls;
    unlock_event = ++event_order;
}

#define EXEC_SPINLOCK_LOCK(lock, hook, mode) \
    fixture_lock((lock), (hook), (mode))
#define EXEC_SPINLOCK_UNLOCK(lock) fixture_unlock((lock))

struct p4_exec_runtime *p4_exec_runtime_current(struct ExecBase *sys_base)
{
    if (sys_base != &fixture_sysbase)
        return NULL;
    return fixture_runtime_result;
}

int p4_exec_runtime_set_current_task(struct ExecBase *sys_base,
                                     struct Task *task)
{
    ++set_current_calls;
    if (sys_base != &fixture_sysbase || fixture_runtime_result == NULL ||
        fixture_runtime_result->cpu_local == NULL)
        return 0;
    fixture_runtime_result->cpu_local->current_task = task;
    return 1;
}

void krnP4PublicSpinFatal(int status) __attribute__((noreturn));
void krnP4PublicSpinFatal(int status)
{
    fatal_status = status;
    if (expect_fatal)
        longjmp(fatal_jump, 1);
    abort();
}

int krnP4TaskDetachedLocked(struct Task *task, struct ExecBase *sys_base)
{
    if (task != &fixture_task || sys_base != &fixture_sysbase ||
        !fixture_task.tc_SpinLock.held ||
        (fixture_mstatus & MSTATUS_MIE) != 0)
        abort();
    ++detached_calls;
    return fixture_detached;
}

int krnP4TaskRecordServiceOwnerRunning(struct Task *task, unsigned int hart,
                                       struct ExecBase *sys_base)
{
    if (task != &fixture_task || sys_base != &fixture_sysbase ||
        hart != fixture_runtime.hart_id || !fixture_task.tc_SpinLock.held ||
        (fixture_mstatus & MSTATUS_MIE) != 0)
        abort();
    ++service_owner_calls;
    return fixture_service_owner;
}

void krnP4TaskRecordLeaveRunning(struct Task *task, unsigned int hart,
                                 struct ExecBase *sys_base)
{
    if (task != &fixture_task || sys_base != &fixture_sysbase ||
        hart != fixture_runtime.hart_id || fixture_task.tc_SpinLock.held ||
        fixture_runtime.cpu_local->current_task != NULL ||
        fixture_runtime.wait_pending != NULL ||
        fixture_runtime.cpu_local->fpu_owner != NULL ||
        unlock_event == 0)
        abort();

    ++leave_calls;
    leave_hart = hart;
    leave_event = ++event_order;
    /* Any later access by the extracted helper becomes an ASan failure. */
    __asan_poison_memory_region(&fixture_task, sizeof(fixture_task));
}

void krnP4DiscardCurrent(struct ExecBase *);
#include "../p4_discard.c"

static void reset_fixture(unsigned int hart, unsigned int state,
                          struct Task *wait_pending)
{
    memset(&fixture_task, 0, sizeof(fixture_task));
    memset(&fixture_other_task, 0, sizeof(fixture_other_task));
    memset(&fixture_cpu_local, 0, sizeof(fixture_cpu_local));
    memset(&fixture_runtime, 0, sizeof(fixture_runtime));
    fixture_mstatus = 0;
    fixture_runtime.hart_id = hart;
    fixture_runtime.cpu_local = &fixture_cpu_local;
    fixture_runtime.wait_pending = wait_pending;
    fixture_cpu_local.trap_depth = 1;
    fixture_cpu_local.current_task = &fixture_task;
    fixture_cpu_local.fpu_owner = &fixture_task;
    fixture_task.tc_State = state;
    fixture_runtime_result = &fixture_runtime;
    expect_fatal = 0;
    fatal_status = 0;
    lock_calls = 0;
    unlock_calls = 0;
    detached_calls = 0;
    service_owner_calls = 0;
    set_current_calls = 0;
    leave_calls = 0;
    leave_hart = UINT32_MAX;
    fixture_detached = 1;
    fixture_service_owner = 1;
    event_order = 0;
    unlock_event = 0;
    leave_event = 0;
}

static int expect_refusal(const char *name, unsigned int expected_locks,
                          unsigned int expected_detached,
                          unsigned int expected_service_owner)
{
    unsigned int old_state = fixture_task.tc_State;
    struct Task *old_current = fixture_cpu_local.current_task;
    struct Task *old_pending = fixture_runtime.wait_pending;
    void *old_fpu_owner = fixture_cpu_local.fpu_owner;
    unsigned int old_hart = fixture_runtime.hart_id;
    struct p4_cpu_local *old_cpu_local = fixture_runtime.cpu_local;
    unsigned int old_lock_calls = lock_calls;
    unsigned int old_unlock_calls = unlock_calls;
    unsigned int old_detached_calls = detached_calls;
    unsigned int old_service_owner_calls = service_owner_calls;
    unsigned int old_set_current_calls = set_current_calls;
    unsigned int old_leave_calls = leave_calls;
    int old_detached = fixture_detached;
    int old_service_owner = fixture_service_owner;

    ++checks;
    expect_fatal = 1;
    if (setjmp(fatal_jump) == 0)
    {
        krnP4DiscardCurrent(&fixture_sysbase);
        fprintf(stderr, "FAIL expected refusal: %s\n", name);
        return 0;
    }
    expect_fatal = 0;

    if (fatal_status != P4_PUBLIC_SPIN_BAD_STATE ||
        fixture_task.tc_State != old_state ||
        fixture_cpu_local.current_task != old_current ||
        fixture_runtime.wait_pending != old_pending ||
        fixture_cpu_local.fpu_owner != old_fpu_owner ||
        fixture_runtime.hart_id != old_hart ||
        fixture_runtime.cpu_local != old_cpu_local ||
        lock_calls - old_lock_calls != expected_locks ||
        unlock_calls - old_unlock_calls != 0 ||
        detached_calls - old_detached_calls != expected_detached ||
        service_owner_calls - old_service_owner_calls != expected_service_owner ||
        set_current_calls - old_set_current_calls != 0 ||
        leave_calls - old_leave_calls != 0 ||
        fixture_detached != old_detached ||
        fixture_service_owner != old_service_owner)
    {
        fprintf(stderr, "FAIL refusal mutated state or called wrong hooks: %s\n",
                name);
        return 0;
    }

    /* Fatal is non-returning in production; clear the mock lock after catch. */
    fixture_task.tc_SpinLock.held = 0;
    return 1;
}

static int test_precondition_refusals(void)
{
    reset_fixture(0, TS_REMOVED, NULL);
    fixture_runtime_result = NULL;
    CHECK(expect_refusal("runtime missing", 0, 0, 0));

    reset_fixture(0, TS_REMOVED, NULL);
    fixture_runtime.hart_id = 2;
    CHECK(expect_refusal("hart out of range", 0, 0, 0));

    reset_fixture(0, TS_REMOVED, NULL);
    fixture_runtime.cpu_local = NULL;
    CHECK(expect_refusal("cpu-local missing", 0, 0, 0));

    reset_fixture(0, TS_REMOVED, NULL);
    fixture_cpu_local.trap_depth = 0;
    CHECK(expect_refusal("not in outer trap", 0, 0, 0));

    reset_fixture(1, TS_REMOVED, NULL);
    fixture_cpu_local.trap_depth = 2;
    CHECK(expect_refusal("nested trap", 0, 0, 0));

    reset_fixture(0, TS_REMOVED, NULL);
    fixture_mstatus = MSTATUS_MIE;
    CHECK(expect_refusal("MIE enabled", 0, 0, 0));

    reset_fixture(0, TS_REMOVED, NULL);
    fixture_cpu_local.current_task = NULL;
    CHECK(expect_refusal("no current task", 0, 0, 0));
    return 1;
}

static int test_task_and_ownership_refusals(void)
{
    reset_fixture(0, TS_READY, NULL);
    CHECK(expect_refusal("ready state", 1, 0, 0));

    reset_fixture(0, TS_REMOVED, &fixture_other_task);
    CHECK(expect_refusal("foreign wait token", 1, 0, 0));

    reset_fixture(1, TS_INVALID, NULL);
    fixture_detached = 0;
    CHECK(expect_refusal("still linked", 1, 1, 0));

    reset_fixture(0, TS_TOMBSTONED, &fixture_task);
    fixture_service_owner = 0;
    CHECK(expect_refusal("service owner missing", 1, 1, 1));
    return 1;
}

static int test_success(unsigned int hart, unsigned int state,
                        struct Task *wait_pending)
{
    unsigned int old_state;
    reset_fixture(hart, state, wait_pending);
    old_state = fixture_task.tc_State;
    krnP4DiscardCurrent(&fixture_sysbase);

    /* Unpoison only after the production helper returned. A later read inside
     * krnP4DiscardCurrent would already have failed under ASan. */
    __asan_unpoison_memory_region(&fixture_task, sizeof(fixture_task));

    CHECK(fatal_status == 0);
    CHECK(lock_calls == 1);
    CHECK(unlock_calls == 1);
    CHECK(detached_calls == 1);
    CHECK(service_owner_calls == 1);
    CHECK(set_current_calls == 1);
    CHECK(leave_calls == 1);
    CHECK(leave_hart == hart);
    CHECK(unlock_event != 0 && leave_event > unlock_event);
    CHECK(fixture_task.tc_State == old_state);
    CHECK(fixture_runtime.wait_pending == NULL);
    CHECK(fixture_cpu_local.current_task == NULL);
    CHECK(fixture_cpu_local.fpu_owner == NULL);
    CHECK(!fixture_task.tc_SpinLock.held);
    return 1;
}

int main(void)
{
    if (!test_precondition_refusals() ||
        !test_task_and_ownership_refusals() ||
        !test_success(0, TS_REMOVED, &fixture_task) ||
        !test_success(1, TS_INVALID, NULL) ||
        !test_success(1, TS_TOMBSTONED, &fixture_task))
        return 1;

    printf("P4 discard helper: %u checks passed\n", checks);
    return 0;
}
