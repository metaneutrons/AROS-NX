/*
 * Host fixture for selected production P4 signal-adapter bodies.
 * The Python driver extracts the current error/load/flush/raise functions and
 * eligible-hart helper; runtime records, CPU masks and Task/ETask are mocked.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <p4_signal_protocol.h>

#define P4_EXEC_RUNTIME_HART_COUNT 2u
#define TS_INVALID   0u
#define TS_RUN       1u
#define TS_READY     2u
#define TS_WAIT      3u
#define TS_REMOVED   4u
#define TS_TOMBSTONED 5u
#define TF_ETASK     UINT8_C(0x01)
#define TF_EXCEPT    UINT8_C(0x80)

typedef uint32_t ULONG;
typedef uintptr_t IPTR;

struct ETask { unsigned int unused; };

struct IntETask
{
    struct ETask public_task;
    IPTR iet_CpuNumber;
    uint32_t affinity;
    const uint32_t *iet_CpuAffinity;
};

struct Task
{
    ULONG tc_State;
    uint32_t tc_SigRecvd;
    uint32_t tc_SigWait;
    uint32_t tc_SigExcept;
    uint8_t tc_Flags;
    struct { struct ETask *tc_ETask; } tc_UnionETask;
};

struct ExecBase { unsigned int unused; };

struct p4_cpu_local { struct Task *current_task; };
struct p4_exec_runtime
{
    uint32_t hart_id;
    int valid;
    struct p4_cpu_local *cpu_local;
    struct Task *wait_pending;
};

struct p4_exec_runtime __p4_exec_runtimes[P4_EXEC_RUNTIME_HART_COUNT];

#define IntETask(etask) ((struct IntETask *)(etask))
#define GetIntETask(task) \
    (((task)->tc_Flags & TF_ETASK) ? \
        IntETask((task)->tc_UnionETask.tc_ETask) : NULL)

static struct ExecBase test_sys_base;
static struct p4_exec_runtime *test_current_runtime;
static unsigned int test_cpu_count;
static unsigned int checks;

#define CHECK(condition) do { \
    ++checks; \
    if (!(condition)) { \
        fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); \
        return 0; \
    } \
} while (0)

static struct p4_exec_runtime *p4_exec_runtime_current(
    struct ExecBase *sys_base)
{
    if (sys_base != &test_sys_base || test_current_runtime == NULL ||
        !test_current_runtime->valid)
        return NULL;
    return test_current_runtime;
}

unsigned int krnP4GetCPUCount(void)
{
    return test_cpu_count;
}

int krnP4CPUInMask(uint32_t number, const void *mask)
{
    const uint32_t *bits = (const uint32_t *)mask;
    return bits != NULL && number < 32u &&
        ((*bits & (UINT32_C(1) << number)) != 0);
}

/* EXTRACTED_ADAPTER_LOAD */

/* EXTRACTED_ELIGIBLE_HART */

/* EXTRACTED_ADAPTER_RAISE */

static void init_runtime(unsigned int caller_hart)
{
    static struct p4_cpu_local cpu_local[2];
    memset(__p4_exec_runtimes, 0, sizeof(__p4_exec_runtimes));
    memset(cpu_local, 0, sizeof(cpu_local));
    test_current_runtime = &__p4_exec_runtimes[caller_hart];
    test_current_runtime->hart_id = caller_hart;
    test_current_runtime->valid = 1;
    test_current_runtime->cpu_local = &cpu_local[caller_hart];
    test_cpu_count = 2;
}

static void init_task(struct Task *task, struct IntETask *etask,
                      ULONG state, IPTR owner_hart, uint32_t affinity)
{
    memset(task, 0, sizeof(*task));
    memset(etask, 0, sizeof(*etask));
    task->tc_State = state;
    task->tc_Flags = TF_ETASK;
    task->tc_UnionETask.tc_ETask = &etask->public_task;
    etask->iet_CpuNumber = owner_hart;
    etask->affinity = affinity;
    etask->iet_CpuAffinity = &etask->affinity;
}

static int test_ready_eligible_hart0(void)
{
    struct Task task;
    struct IntETask etask;
    struct p4_signal_effect effect;

    init_runtime(0);
    init_task(&task, &etask, TS_READY, 0, UINT32_C(1));
    effect = p4_signal_adapter_raise_locked(
        &task, &test_sys_base, UINT32_C(0x01));
    CHECK(effect.status == P4_SIGNAL_STATUS_OK);
    CHECK(effect.queue_transition == P4_SIGNAL_QUEUE_NONE);
    CHECK(effect.request_reschedule == 1);
    CHECK(effect.reschedule_hart == 0);
    CHECK(effect.remote_reschedule == 0);
    CHECK(task.tc_SigRecvd == UINT32_C(0x01));
    return 1;
}

static int test_ready_eligible_hart1(void)
{
    struct Task task;
    struct IntETask etask;
    struct p4_signal_effect effect;

    init_runtime(0);
    init_task(&task, &etask, TS_READY, 0, UINT32_C(2));
    effect = p4_signal_adapter_raise_locked(
        &task, &test_sys_base, UINT32_C(0x02));
    CHECK(effect.status == P4_SIGNAL_STATUS_OK);
    CHECK(effect.request_reschedule == 1);
    CHECK(effect.reschedule_hart == 1);
    CHECK(effect.remote_reschedule == 1);
    CHECK(task.tc_SigRecvd == UINT32_C(0x02));
    return 1;
}

static int test_ready_invalid_affinity_does_not_flush(void)
{
    struct Task task;
    struct IntETask etask;
    struct p4_signal_effect effect;

    init_runtime(0);
    init_task(&task, &etask, TS_READY, 0, 0);
    task.tc_SigRecvd = UINT32_C(0x20);
    task.tc_SigExcept = UINT32_C(0x40);
    task.tc_Flags |= UINT8_C(0x10);
    effect = p4_signal_adapter_raise_locked(
        &task, &test_sys_base, UINT32_C(0x02));
    CHECK(effect.status == P4_SIGNAL_STATUS_INVALID_STATE);
    CHECK(task.tc_SigRecvd == UINT32_C(0x20));
    CHECK(task.tc_SigExcept == UINT32_C(0x40));
    CHECK(task.tc_Flags == (TF_ETASK | UINT8_C(0x10)));
    return 1;
}

static int test_foreign_running_token_rejected_before_mutation(void)
{
    struct Task task, other;
    struct IntETask etask;
    struct p4_signal_effect effect;

    init_runtime(0);
    init_task(&task, &etask, TS_RUN, 1, UINT32_C(3));
    task.tc_SigWait = UINT32_C(0x04);
    __p4_exec_runtimes[1].wait_pending = &other;
    effect = p4_signal_adapter_raise_locked(
        &task, &test_sys_base, UINT32_C(0x04));
    CHECK(effect.status == P4_SIGNAL_STATUS_PENDING_OWNER_MISMATCH);
    CHECK(task.tc_SigRecvd == 0);
    CHECK(task.tc_SigWait == UINT32_C(0x04));
    CHECK(task.tc_Flags == TF_ETASK);
    CHECK(__p4_exec_runtimes[1].wait_pending == &other);
    return 1;
}

static int test_running_armed_signal_requests_owner_hart(void)
{
    struct Task task;
    struct IntETask etask;
    struct p4_signal_effect effect;

    init_runtime(0);
    init_task(&task, &etask, TS_RUN, 1, UINT32_C(3));
    task.tc_SigWait = UINT32_C(0x08);
    __p4_exec_runtimes[1].wait_pending = &task;
    effect = p4_signal_adapter_raise_locked(
        &task, &test_sys_base, UINT32_C(0x08));
    CHECK(effect.status == P4_SIGNAL_STATUS_OK);
    CHECK(effect.request_reschedule == 1);
    CHECK(effect.reschedule_hart == 1);
    CHECK(effect.remote_reschedule == 1);
    CHECK(task.tc_SigRecvd == UINT32_C(0x08));
    return 1;
}

static int test_running_stale_wait_mask_does_not_reschedule(void)
{
    struct Task task;
    struct IntETask etask;
    struct p4_signal_effect effect;

    init_runtime(0);
    init_task(&task, &etask, TS_RUN, 0, UINT32_C(1));
    task.tc_SigWait = UINT32_C(0x10);
    effect = p4_signal_adapter_raise_locked(
        &task, &test_sys_base, UINT32_C(0x10));
    CHECK(effect.status == P4_SIGNAL_STATUS_OK);
    CHECK(effect.request_reschedule == 0);
    CHECK(task.tc_SigRecvd == UINT32_C(0x10));
    return 1;
}

static int test_running_exception_reschedules_without_wait_token(void)
{
    struct Task task;
    struct IntETask etask;
    struct p4_signal_effect effect;

    init_runtime(0);
    init_task(&task, &etask, TS_RUN, 1, UINT32_C(3));
    task.tc_SigExcept = UINT32_C(0x20);
    effect = p4_signal_adapter_raise_locked(
        &task, &test_sys_base, UINT32_C(0x20));
    CHECK(effect.status == P4_SIGNAL_STATUS_OK);
    CHECK(effect.request_reschedule == 1);
    CHECK(effect.reschedule_hart == 1);
    CHECK(effect.remote_reschedule == 1);
    CHECK((task.tc_Flags & TF_EXCEPT) != 0);
    CHECK(task.tc_SigRecvd == UINT32_C(0x20));
    return 1;
}

static int test_invalid_runtime_does_not_mutate(void)
{
    struct Task task;
    struct IntETask etask;
    struct p4_signal_effect effect;

    init_runtime(0);
    init_task(&task, &etask, TS_READY, 0, UINT32_C(1));
    test_current_runtime->valid = 0;
    effect = p4_signal_adapter_raise_locked(
        &task, &test_sys_base, UINT32_C(0x40));
    CHECK(effect.status == P4_SIGNAL_STATUS_INVALID_ARGUMENT);
    CHECK(task.tc_SigRecvd == 0);
    CHECK(task.tc_Flags == TF_ETASK);
    return 1;
}

int main(void)
{
    if (!test_ready_eligible_hart0() ||
        !test_ready_eligible_hart1() ||
        !test_ready_invalid_affinity_does_not_flush() ||
        !test_foreign_running_token_rejected_before_mutation() ||
        !test_running_armed_signal_requests_owner_hart() ||
        !test_running_stale_wait_mask_does_not_reschedule() ||
        !test_running_exception_reschedules_without_wait_token() ||
        !test_invalid_runtime_does_not_mutate())
        return 1;

    printf("P4 signal adapter checks: %u passed\n", checks);
    return 0;
}
