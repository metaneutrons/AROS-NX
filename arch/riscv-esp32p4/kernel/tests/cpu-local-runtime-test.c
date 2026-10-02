/*
 * Host regression for the production P4 Exec runtime and platform mappings.
 * Only the mhartid CSR read is mocked; the CPU-local and runtime sources are
 * compiled directly below.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint32_t test_hart_id;

static uint32_t test_read_mhartid(void)
{
    return test_hart_id;
}

#define P4_CPU_LOCAL_READ_MHARTID test_read_mhartid
#define __AROSEXEC_SMP__ 1
#define P4_E3_RUNTIME_READY 1
#include "../cpu_local.h"
#include "../cpu_local_runtime.h"
#include "../../exec/exec_platform.h"

#if defined(__APPLE__)
/* ELF section names are not accepted by Mach-O. */
#define section(name) used
#endif
#include "../cpu_local.c"
#include "../cpu_local_runtime.c"
#if defined(__APPLE__)
#undef section
#endif

struct Task
{
    uint32_t identity;
};

static unsigned long check_count;
static uint32_t sys_base_marker[2];
static uint32_t kernel_base_marker[2];
static struct Task tasks[2];

#define CHECK(condition) do { \
    ++check_count; \
    if (!(condition)) { \
        fprintf(stderr, "FAIL line %d: %s (mock hart %u)\n", \
                __LINE__, #condition, test_hart_id); \
        return 0; \
    } \
} while (0)

static void reset_runtime_state(void)
{
    memset(__p4_cpu_locals, 0, sizeof(__p4_cpu_locals));
    memset(__p4_exec_runtimes, 0, sizeof(__p4_exec_runtimes));
    test_hart_id = 0;
}

static int test_legacy_prepare_and_bind(void)
{
    struct p4_cpu_local local_before[P4_CPU_LOCAL_HART_COUNT];
    struct p4_exec_runtime runtime_before[P4_EXEC_RUNTIME_HART_COUNT];
    struct ExecBase *sys0 = (struct ExecBase *)&sys_base_marker[0];
    struct ExecBase *sys1 = (struct ExecBase *)&sys_base_marker[1];
    struct KernelBase *kernel0 = (struct KernelBase *)&kernel_base_marker[0];
    struct KernelBase *kernel1 = (struct KernelBase *)&kernel_base_marker[1];
    struct p4_exec_runtime *runtime0;
    struct p4_exec_runtime *runtime1;

    reset_runtime_state();

    CHECK(p4_exec_runtime_for_hart(0) == &__p4_exec_runtimes[0]);
    CHECK(p4_exec_runtime_for_hart(1) == &__p4_exec_runtimes[1]);
    CHECK(p4_exec_runtime_for_hart(2) == NULL);
    CHECK(p4_exec_runtime_for_hart(UINT32_MAX) == NULL);
    CHECK(p4_exec_runtime_current(sys0) == NULL);

    test_hart_id = 0;
    CHECK(krnP4CpuLocalPrepare());
    /* A pending token must never be silently erased by bootstrap. */
    __p4_exec_runtimes[1].wait_pending = &tasks[1];
    memcpy(runtime_before, __p4_exec_runtimes, sizeof(runtime_before));
    CHECK(!krnP4ExecRuntimeBootstrapPrepare());
    CHECK(memcmp(runtime_before, __p4_exec_runtimes,
                 sizeof(runtime_before)) == 0);
    __p4_exec_runtimes[1].wait_pending = NULL;
    memcpy(local_before, __p4_cpu_locals, sizeof(local_before));
    memcpy(runtime_before, __p4_exec_runtimes, sizeof(runtime_before));

    test_hart_id = 1;
    CHECK(!krnP4ExecRuntimePrepare(sys0, kernel0));
    CHECK(memcmp(local_before, __p4_cpu_locals, sizeof(local_before)) == 0);
    CHECK(memcmp(runtime_before, __p4_exec_runtimes,
                 sizeof(runtime_before)) == 0);

    test_hart_id = 0;
    CHECK(!krnP4ExecRuntimePrepare(NULL, kernel0));
    CHECK(!krnP4ExecRuntimePrepare(sys0, NULL));
    CHECK(memcmp(local_before, __p4_cpu_locals, sizeof(local_before)) == 0);
    CHECK(memcmp(runtime_before, __p4_exec_runtimes,
                 sizeof(runtime_before)) == 0);
    CHECK(krnP4ExecRuntimePrepare(sys0, kernel0));

    runtime0 = p4_exec_runtime_current(sys0);
    runtime1 = p4_exec_runtime_for_hart(1);
    CHECK(runtime0 == &__p4_exec_runtimes[0]);
    CHECK(runtime0->cpu_local == &__p4_cpu_locals[0]);
    CHECK(runtime0->quantum == P4_EXEC_RUNTIME_DEFAULT_QUANTUM);
    CHECK(runtime0->elapsed == 0);
    CHECK(runtime0->dispatch_count == 0);
    CHECK(runtime0->stage == P4_EXEC_RUNTIME_STAGE_FINALIZED);
    CHECK(runtime1->cpu_local == &__p4_cpu_locals[1]);
    CHECK(runtime1->quantum == P4_EXEC_RUNTIME_DEFAULT_QUANTUM);
    CHECK(runtime1->stage == P4_EXEC_RUNTIME_STAGE_PREPARED);
    CHECK(p4_exec_runtime_current(sys1) == NULL);

    test_hart_id = 1;
    CHECK(p4_exec_runtime_current(sys0) == NULL);
    CHECK(!krnP4ExecRuntimeBind(sys1, kernel0));
    CHECK(krnP4ExecRuntimeBind(sys0, kernel0));
    CHECK(p4_exec_runtime_current(sys0) == runtime1);
    CHECK(p4_exec_runtime_current(sys1) == NULL);
    CHECK(!krnP4ExecRuntimeBind(sys0, kernel1));

    test_hart_id = 0;
    CHECK(p4_exec_runtime_current(sys0) == runtime0);
    CHECK(!krnP4ExecRuntimeBind(sys1, kernel0));
    CHECK(krnP4ExecRuntimeBind(sys0, kernel0));

    test_hart_id = 2;
    CHECK(p4_exec_runtime_current(sys0) == NULL);
    CHECK(!krnP4ExecRuntimeBind(sys0, kernel0));
    test_hart_id = UINT32_MAX;
    CHECK(p4_exec_runtime_current(sys0) == NULL);

    test_hart_id = 0;
    return 1;
}

static int test_staged_bootstrap(void)
{
    struct p4_cpu_local local_before[P4_CPU_LOCAL_HART_COUNT];
    struct p4_exec_runtime runtime_before[P4_EXEC_RUNTIME_HART_COUNT];
    struct ExecBase *sys0 = (struct ExecBase *)&sys_base_marker[0];
    struct ExecBase *sys1 = (struct ExecBase *)&sys_base_marker[1];
    struct KernelBase *kernel0 = (struct KernelBase *)&kernel_base_marker[0];
    struct KernelBase *kernel1 = (struct KernelBase *)&kernel_base_marker[1];
    struct p4_exec_runtime *runtime0;
    struct p4_exec_runtime *runtime1;
    struct p4_cpu_local *local0;
    struct ExecBase *SysBase = sys0;

    reset_runtime_state();

    /* Neither bootstrap stage is allowed to repair unprepared CPU-local data. */
    memcpy(local_before, __p4_cpu_locals, sizeof(local_before));
    memcpy(runtime_before, __p4_exec_runtimes, sizeof(runtime_before));
    CHECK(!krnP4ExecRuntimeBootstrapPrepare());
    CHECK(!krnP4ExecRuntimeBootstrapBind(sys0));
    CHECK(memcmp(local_before, __p4_cpu_locals, sizeof(local_before)) == 0);
    CHECK(memcmp(runtime_before, __p4_exec_runtimes,
                 sizeof(runtime_before)) == 0);

    CHECK(krnP4CpuLocalPrepare());
    memcpy(local_before, __p4_cpu_locals, sizeof(local_before));
    memcpy(runtime_before, __p4_exec_runtimes, sizeof(runtime_before));

    test_hart_id = 1;
    CHECK(!krnP4ExecRuntimeBootstrapPrepare());
    CHECK(!krnP4ExecRuntimeBootstrapBind(sys0));
    CHECK(!krnP4ExecRuntimePrepare(sys0, kernel0));
    CHECK(memcmp(local_before, __p4_cpu_locals, sizeof(local_before)) == 0);
    CHECK(memcmp(runtime_before, __p4_exec_runtimes,
                 sizeof(runtime_before)) == 0);

    test_hart_id = 0;
    CHECK(!krnP4ExecRuntimeBootstrapBind(NULL));
    CHECK(krnP4ExecRuntimeBootstrapPrepare());
    runtime0 = p4_exec_runtime_for_hart(0);
    runtime1 = p4_exec_runtime_for_hart(1);
    local0 = p4_cpu_local_for_hart(0);
    CHECK(runtime0->initialized == 1U);
    CHECK(runtime1->initialized == 1U);
    CHECK(runtime0->wait_pending == NULL);
    CHECK(runtime1->wait_pending == NULL);
    CHECK(runtime0->stage == P4_EXEC_RUNTIME_STAGE_PREPARED);
    CHECK(runtime1->stage == P4_EXEC_RUNTIME_STAGE_PREPARED);
    CHECK(runtime0->cpu_local == local0);
    CHECK(local0->sys_base == NULL);
    CHECK(local0->kernel_base == NULL);
    CHECK(p4_cpu_local_for_hart(1)->sys_base == NULL);
    CHECK(p4_cpu_local_for_hart(1)->kernel_base == NULL);
    CHECK(p4_exec_runtime_current(sys0) == NULL);

    /* A second prepare is a rejected reset and leaves both slots untouched. */
    memcpy(local_before, __p4_cpu_locals, sizeof(local_before));
    memcpy(runtime_before, __p4_exec_runtimes, sizeof(runtime_before));
    CHECK(!krnP4ExecRuntimeBootstrapPrepare());
    CHECK(!krnP4ExecRuntimePrepare(sys0, kernel0));
    CHECK(memcmp(local_before, __p4_cpu_locals, sizeof(local_before)) == 0);
    CHECK(memcmp(runtime_before, __p4_exec_runtimes,
                 sizeof(runtime_before)) == 0);

    /* Hart 1 cannot finalize until the primary has attached KernelBase. */
    test_hart_id = 1;
    CHECK(!krnP4ExecRuntimeFinalize(sys0, kernel0));
    CHECK(p4_exec_runtime_current(sys0) == NULL);
    CHECK(memcmp(local_before, __p4_cpu_locals, sizeof(local_before)) == 0);
    CHECK(memcmp(runtime_before, __p4_exec_runtimes,
                 sizeof(runtime_before)) == 0);

    test_hart_id = 0;
    CHECK(krnP4ExecRuntimeBootstrapBind(sys0));
    CHECK(runtime0->stage == P4_EXEC_RUNTIME_STAGE_SYSBASE_BOUND);
    CHECK(local0->sys_base == sys0);
    CHECK(local0->kernel_base == NULL);
    CHECK(p4_exec_runtime_current(sys0) == runtime0);
    CHECK(p4_exec_runtime_current(sys1) == NULL);
    CHECK(!krnP4ExecRuntimeBootstrapBind(NULL));
    CHECK(!krnP4ExecRuntimeBootstrapBind(sys0));
    CHECK(!krnP4ExecRuntimeBootstrapBind(sys1));
    CHECK(!krnP4ExecRuntimeFinalize(sys1, kernel0));
    CHECK(!krnP4ExecRuntimeFinalize(sys0, NULL));
    CHECK(runtime0->stage == P4_EXEC_RUNTIME_STAGE_SYSBASE_BOUND);
    CHECK(local0->sys_base == sys0);
    CHECK(local0->kernel_base == NULL);

    /* Model PrepareExecBase platform-macro state before KernelBase exists. */
    SET_THIS_TASK(&tasks[0]);
    IDNESTCOUNT_SET(5);
    TDNESTCOUNT_SET(3);
    SCHEDQUANTUM_SET(11);
    SCHEDELAPSED_SET(7);
    FLAG_SCHEDSWITCH_SET;
    FLAG_SCHEDDISPATCH_SET;
    runtime0->dispatch_count = 9;
    runtime0->wait_pending = &tasks[0];
    CHECK(p4_exec_runtime_current(sys0) == runtime0);
    CHECK(local0->current_task == &tasks[0]);
    CHECK(local0->id_nest == 5);
    CHECK(local0->td_nest == 3);
    CHECK(runtime0->quantum == 11);
    CHECK(runtime0->elapsed == 7);
    CHECK(runtime0->dispatch_count == 9);
    CHECK(runtime0->wait_pending == &tasks[0]);
    CHECK(local0->schedule_flags ==
          (P4_EXEC_SCHEDULE_SWITCH | P4_EXEC_SCHEDULE_DISPATCH));

    /* Invalid hart-1 binding after SysBase but before KernelBase is inert. */
    memcpy(local_before, __p4_cpu_locals, sizeof(local_before));
    memcpy(runtime_before, __p4_exec_runtimes, sizeof(runtime_before));
    test_hart_id = 1;
    CHECK(!krnP4ExecRuntimeBind(sys0, kernel0));
    CHECK(memcmp(local_before, __p4_cpu_locals, sizeof(local_before)) == 0);
    CHECK(memcmp(runtime_before, __p4_exec_runtimes,
                 sizeof(runtime_before)) == 0);

    test_hart_id = 0;
    CHECK(krnP4ExecRuntimeFinalize(sys0, kernel0));
    CHECK(runtime0->stage == P4_EXEC_RUNTIME_STAGE_FINALIZED);
    CHECK(local0->kernel_base == kernel0);
    CHECK(local0->current_task == &tasks[0]);
    CHECK(local0->id_nest == 5);
    CHECK(local0->td_nest == 3);
    CHECK(runtime0->quantum == 11);
    CHECK(runtime0->elapsed == 7);
    CHECK(runtime0->dispatch_count == 9);
    CHECK(runtime0->wait_pending == &tasks[0]);
    CHECK(local0->schedule_flags ==
          (P4_EXEC_SCHEDULE_SWITCH | P4_EXEC_SCHEDULE_DISPATCH));
    CHECK(p4_exec_runtime_current(sys0) == runtime0);
    CHECK(krnP4ExecRuntimeFinalize(sys0, kernel0));
    CHECK(!krnP4ExecRuntimeFinalize(sys0, kernel1));

    test_hart_id = 1;
    CHECK(!krnP4ExecRuntimeFinalize(sys1, kernel0));
    CHECK(!krnP4ExecRuntimeFinalize(sys0, kernel1));
    CHECK(krnP4ExecRuntimeFinalize(sys0, kernel0));
    CHECK(p4_exec_runtime_current(sys0) == runtime1);
    CHECK(runtime1->stage == P4_EXEC_RUNTIME_STAGE_FINALIZED);
    CHECK(krnP4ExecRuntimeFinalize(sys0, kernel0));
    CHECK(!krnP4ExecRuntimeFinalize(sys1, kernel0));

    /* Bounds checks and all failed rebinds leave the finalized state intact. */
    test_hart_id = 2;
    CHECK(p4_exec_runtime_current(sys0) == NULL);
    CHECK(!krnP4ExecRuntimeFinalize(sys0, kernel0));
    test_hart_id = UINT32_MAX;
    CHECK(p4_exec_runtime_current(sys0) == NULL);
    CHECK(!krnP4ExecRuntimeBootstrapBind(sys0));

    return 1;
}

static int test_platform_mappings(void)
{
    struct ExecBase *SysBase = (struct ExecBase *)&sys_base_marker[0];
    struct p4_exec_runtime *runtime0 = p4_exec_runtime_current(SysBase);
    struct p4_cpu_local *local0 = runtime0->cpu_local;

    tasks[0].identity = 0x10;
    tasks[1].identity = 0x20;

    CHECK(SCHEDQUANTUM_VALUE == P4_EXEC_RUNTIME_DEFAULT_QUANTUM);
    CHECK(IDNESTCOUNT_GET == -1);
    CHECK(TDNESTCOUNT_GET == -1);
    CHECK(GET_THIS_TASK == NULL);

    IDNESTCOUNT_INC;
    TDNESTCOUNT_INC;
    CHECK(IDNESTCOUNT_GET == 0);
    CHECK(TDNESTCOUNT_GET == 0);
    IDNESTCOUNT_DEC;
    TDNESTCOUNT_SET(3);
    CHECK(IDNESTCOUNT_GET == -1);
    CHECK(TDNESTCOUNT_GET == 3);
    TDNESTCOUNT_DEC;
    CHECK(TDNESTCOUNT_GET == 2);

    SET_THIS_TASK(&tasks[0]);
    CHECK(GET_THIS_TASK == &tasks[0]);
    CHECK(!p4_exec_runtime_set_current_task(
        (struct ExecBase *)&sys_base_marker[1], &tasks[1]));
    CHECK(GET_THIS_TASK == &tasks[0]);
    SCHEDQUANTUM_SET(9);
    SCHEDELAPSED_SET(4);
    CHECK(SCHEDQUANTUM_GET == 9);
    CHECK(SCHEDELAPSED_GET == 4);

    FLAG_SCHEDQUANTUM_SET;
    FLAG_SCHEDSWITCH_SET;
    FLAG_SCHEDDISPATCH_SET;
    CHECK(FLAG_SCHEDQUANTUM_ISSET);
    CHECK(FLAG_SCHEDSWITCH_ISSET);
    CHECK(FLAG_SCHEDDISPATCH_ISSET);
    CHECK(local0->schedule_flags ==
          (P4_EXEC_SCHEDULE_QUANTUM | P4_EXEC_SCHEDULE_SWITCH |
           P4_EXEC_SCHEDULE_DISPATCH));
    FLAG_SCHEDSWITCH_CLEAR;
    CHECK(!FLAG_SCHEDSWITCH_ISSET);
    CHECK(FLAG_SCHEDQUANTUM_ISSET);
    CHECK(FLAG_SCHEDDISPATCH_ISSET);
    FLAG_SCHEDQUANTUM_CLEAR;
    FLAG_SCHEDDISPATCH_CLEAR;
    CHECK(local0->schedule_flags == 0);

    test_hart_id = 1;
    {
        struct ExecBase *SysBase1 = (struct ExecBase *)&sys_base_marker[0];
        struct p4_exec_runtime *runtime1 = p4_exec_runtime_current(SysBase1);

        CHECK(runtime1 != NULL);
        SCHEDQUANTUM_SET(13);
        SCHEDELAPSED_SET(7);
        SET_THIS_TASK(&tasks[1]);
        FLAG_SCHEDDISPATCH_SET;
        CHECK(SCHEDQUANTUM_GET == 13);
        CHECK(SCHEDELAPSED_GET == 7);
        CHECK(GET_THIS_TASK == &tasks[1]);
        CHECK(FLAG_SCHEDDISPATCH_ISSET);
        CHECK(runtime1->dispatch_count == 0);
    }

    test_hart_id = 0;
    CHECK(p4_exec_runtime_current(SysBase) == runtime0);
    CHECK(SCHEDQUANTUM_GET == 9);
    CHECK(SCHEDELAPSED_GET == 4);
    CHECK(GET_THIS_TASK == &tasks[0]);
    CHECK(local0->schedule_flags == 0);

    test_hart_id = 1;
    CHECK(p4_exec_runtime_for_hart(1)->cpu_local->schedule_flags ==
          P4_EXEC_SCHEDULE_DISPATCH);
    return 1;
}

int main(void)
{
    if (!test_legacy_prepare_and_bind() || !test_platform_mappings() ||
        !test_staged_bootstrap())
        return 1;

    printf("PASS cpu-local runtime host test (%lu checks)\n", check_count);
    return 0;
}
