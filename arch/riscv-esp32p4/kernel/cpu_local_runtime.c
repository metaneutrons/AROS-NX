/*
 * Copyright (c) 2026, The AROS Development Team. All rights reserved.
 *
 * Private per-hart Exec runtime state for the ESP32-P4.
 */

#include "cpu_local_runtime.h"

#define P4_EXEC_RUNTIME_SRAM \
    __attribute__((section(".sramtext.secondary_mailbox"), noinline))

/* Kept in internal SRAM with one cache line per hart. */
struct p4_exec_runtime __p4_exec_runtimes[P4_EXEC_RUNTIME_HART_COUNT]
    __attribute__((section(".bss.p4_exec_runtime"),
                   aligned(P4_EXEC_RUNTIME_CACHELINE_BYTES)));

_Static_assert(sizeof(__p4_exec_runtimes) ==
                   P4_EXEC_RUNTIME_HART_COUNT *
                       P4_EXEC_RUNTIME_CACHELINE_BYTES,
               "P4 Exec runtime records must be contiguous cache lines");

P4_EXEC_RUNTIME_SRAM
struct p4_exec_runtime *p4_exec_runtime_for_hart(uint32_t hart_id)
{
    if (hart_id >= P4_EXEC_RUNTIME_HART_COUNT)
        return NULL;

    return &__p4_exec_runtimes[hart_id];
}

P4_EXEC_RUNTIME_SRAM
struct p4_exec_runtime *p4_exec_runtime_current(struct ExecBase *sys_base)
{
    struct p4_cpu_local *local;
    struct p4_exec_runtime *runtime;
    uint32_t hart_id;

    if (sys_base == NULL)
        return NULL;

    hart_id = P4_CPU_LOCAL_READ_MHARTID();
    if (hart_id >= P4_EXEC_RUNTIME_HART_COUNT)
        return NULL;

    local = p4_cpu_local_current();
    if (local == NULL || local != p4_cpu_local_for_hart(hart_id) ||
        local->hart_id != hart_id || local->sys_base != sys_base)
        return NULL;

    runtime = p4_exec_runtime_for_hart(hart_id);
    if (runtime == NULL || runtime->initialized != 1U ||
        runtime->hart_id != hart_id || runtime->cpu_local != local)
        return NULL;

    if (runtime->stage == P4_EXEC_RUNTIME_STAGE_SYSBASE_BOUND)
    {
        /* Only hart 0 has a valid pre-KernelBase bootstrap state. */
        if (hart_id != 0 || local->kernel_base != NULL)
            return NULL;
    }
    else if (runtime->stage == P4_EXEC_RUNTIME_STAGE_FINALIZED)
    {
        if (local->kernel_base == NULL)
            return NULL;
    }
    else
        return NULL;

    return runtime;
}

P4_EXEC_RUNTIME_SRAM
int p4_exec_runtime_set_current_task(struct ExecBase *sys_base,
                                     struct Task *task)
{
    struct p4_exec_runtime *runtime = p4_exec_runtime_current(sys_base);

    if (runtime == NULL)
        return 0;

    runtime->cpu_local->current_task = task;
    return 1;
}

static P4_EXEC_RUNTIME_SRAM
int p4_exec_runtime_cpu_local_is_fresh(struct p4_cpu_local *local,
                                      uint32_t hart_id)
{
    return local != NULL && local == p4_cpu_local_for_hart(hart_id) &&
           local->hart_id == hart_id && local->current_task == NULL &&
           local->sys_base == NULL && local->kernel_base == NULL &&
           local->trap_depth == 0 && local->schedule_flags == 0 &&
           local->id_nest == -1 && local->td_nest == -1 &&
           local->idle_count == 0 && local->ticks == 0 &&
           local->fpu_owner == NULL;
}

static P4_EXEC_RUNTIME_SRAM
int p4_exec_runtime_is_fresh(struct p4_exec_runtime *runtime)
{
    uint32_t i;

    if (runtime == NULL || runtime->hart_id != 0 ||
        runtime->initialized != 0 ||
        runtime->stage != P4_EXEC_RUNTIME_STAGE_EMPTY ||
        runtime->cpu_local != NULL || runtime->quantum != 0 ||
        runtime->elapsed != 0 || runtime->dispatch_count != 0 ||
        runtime->wait_pending != NULL)
        return 0;

    for (i = 0; i < sizeof(runtime->reserved); ++i)
    {
        if (runtime->reserved[i] != 0)
            return 0;
    }

    return 1;
}

static P4_EXEC_RUNTIME_SRAM
void p4_exec_runtime_reset(struct p4_exec_runtime *runtime,
                           struct p4_cpu_local *local, uint32_t hart_id)
{
    uint32_t i;

    runtime->initialized = 0;
    runtime->stage = P4_EXEC_RUNTIME_STAGE_EMPTY;
    runtime->hart_id = hart_id;
    runtime->cpu_local = local;
    runtime->quantum = P4_EXEC_RUNTIME_DEFAULT_QUANTUM;
    runtime->elapsed = 0;
    runtime->dispatch_count = 0;
    runtime->wait_pending = NULL;
    for (i = 0; i < sizeof(runtime->reserved); ++i)
        runtime->reserved[i] = 0;
    runtime->stage = P4_EXEC_RUNTIME_STAGE_PREPARED;
    runtime->initialized = 1;
}

P4_EXEC_RUNTIME_SRAM
int krnP4ExecRuntimeBootstrapPrepare(void)
{
    struct p4_cpu_local *local0;
    struct p4_cpu_local *local1;
    struct p4_exec_runtime *runtime0;
    struct p4_exec_runtime *runtime1;

    if (P4_CPU_LOCAL_READ_MHARTID() != 0)
        return 0;

    local0 = p4_cpu_local_for_hart(0);
    local1 = p4_cpu_local_for_hart(1);
    runtime0 = p4_exec_runtime_for_hart(0);
    runtime1 = p4_exec_runtime_for_hart(1);

    /* Validate the full target set before mutating either record. */
    if (local0 == NULL || local1 == NULL || runtime0 == NULL ||
        runtime1 == NULL ||
        !p4_exec_runtime_cpu_local_is_fresh(local0, 0) ||
        !p4_exec_runtime_cpu_local_is_fresh(local1, 1) ||
        !p4_exec_runtime_is_fresh(runtime0) ||
        !p4_exec_runtime_is_fresh(runtime1))
        return 0;

    p4_exec_runtime_reset(runtime0, local0, 0);
    p4_exec_runtime_reset(runtime1, local1, 1);

    return 1;
}

P4_EXEC_RUNTIME_SRAM
int krnP4ExecRuntimeBootstrapBind(struct ExecBase *sys_base)
{
    struct p4_cpu_local *local0;
    struct p4_cpu_local *local1;
    struct p4_exec_runtime *runtime0;
    struct p4_exec_runtime *runtime1;

    if (sys_base == NULL || P4_CPU_LOCAL_READ_MHARTID() != 0)
        return 0;

    local0 = p4_cpu_local_for_hart(0);
    local1 = p4_cpu_local_for_hart(1);
    runtime0 = p4_exec_runtime_for_hart(0);
    runtime1 = p4_exec_runtime_for_hart(1);

    /* Validate the complete staged state before changing hart 0's binding. */
    if (local0 == NULL || local1 == NULL || runtime0 == NULL ||
        runtime1 == NULL || local0->hart_id != 0 || local1->hart_id != 1 ||
        local0->sys_base != NULL || local0->kernel_base != NULL ||
        local1->sys_base != NULL || local1->kernel_base != NULL ||
        runtime0->initialized != 1U ||
        runtime0->stage != P4_EXEC_RUNTIME_STAGE_PREPARED ||
        runtime0->hart_id != 0 || runtime0->cpu_local != local0 ||
        runtime1->initialized != 1U ||
        runtime1->stage != P4_EXEC_RUNTIME_STAGE_PREPARED ||
        runtime1->hart_id != 1 || runtime1->cpu_local != local1)
        return 0;

    local0->sys_base = sys_base;
    runtime0->stage = P4_EXEC_RUNTIME_STAGE_SYSBASE_BOUND;
    return 1;
}

P4_EXEC_RUNTIME_SRAM
int krnP4ExecRuntimePrepare(struct ExecBase *sys_base,
                            struct KernelBase *kernel_base)
{
    struct p4_cpu_local *local0;
    struct p4_cpu_local *local1;
    struct p4_exec_runtime *runtime0;
    struct p4_exec_runtime *runtime1;

    if (P4_CPU_LOCAL_READ_MHARTID() != 0 || sys_base == NULL ||
        kernel_base == NULL)
        return 0;

    local0 = p4_cpu_local_for_hart(0);
    local1 = p4_cpu_local_for_hart(1);
    runtime0 = p4_exec_runtime_for_hart(0);
    runtime1 = p4_exec_runtime_for_hart(1);

    /* Validate the full target set before mutating either record. */
    if (local0 == NULL || local1 == NULL || runtime0 == NULL ||
        runtime1 == NULL ||
        !p4_exec_runtime_cpu_local_is_fresh(local0, 0) ||
        !p4_exec_runtime_cpu_local_is_fresh(local1, 1) ||
        !p4_exec_runtime_is_fresh(runtime0) ||
        !p4_exec_runtime_is_fresh(runtime1))
        return 0;

    p4_exec_runtime_reset(runtime0, local0, 0);
    p4_exec_runtime_reset(runtime1, local1, 1);

    local0->sys_base = sys_base;
    local0->kernel_base = kernel_base;
    runtime0->stage = P4_EXEC_RUNTIME_STAGE_FINALIZED;

    return 1;
}

P4_EXEC_RUNTIME_SRAM
int krnP4ExecRuntimeFinalize(struct ExecBase *sys_base,
                             struct KernelBase *kernel_base)
{
    struct p4_cpu_local *local;
    struct p4_cpu_local *primary;
    struct p4_exec_runtime *runtime;
    struct p4_exec_runtime *primary_runtime;
    uint32_t hart_id;

    if (sys_base == NULL || kernel_base == NULL)
        return 0;

    hart_id = P4_CPU_LOCAL_READ_MHARTID();
    if (hart_id >= P4_EXEC_RUNTIME_HART_COUNT)
        return 0;

    local = p4_cpu_local_current();
    primary = p4_cpu_local_for_hart(0);
    runtime = p4_exec_runtime_for_hart(hart_id);
    primary_runtime = p4_exec_runtime_for_hart(0);
    if (local == NULL || primary == NULL || runtime == NULL ||
        primary_runtime == NULL || local != p4_cpu_local_for_hart(hart_id) ||
        local != runtime->cpu_local || runtime->initialized != 1U ||
        runtime->hart_id != hart_id || local->hart_id != hart_id ||
        primary != primary_runtime->cpu_local || primary->hart_id != 0 ||
        primary_runtime->initialized != 1U ||
        primary_runtime->hart_id != 0)
        return 0;

    if (hart_id == 0)
    {
        if (runtime->stage == P4_EXEC_RUNTIME_STAGE_SYSBASE_BOUND &&
            local->sys_base == sys_base && local->kernel_base == NULL)
        {
            local->kernel_base = kernel_base;
            runtime->stage = P4_EXEC_RUNTIME_STAGE_FINALIZED;
            return 1;
        }

        return runtime->stage == P4_EXEC_RUNTIME_STAGE_FINALIZED &&
               local->sys_base == sys_base &&
               local->kernel_base == kernel_base;
    }

    /* Hart 1 may bind only after the primary hart has completed finalization. */
    if (primary_runtime->stage != P4_EXEC_RUNTIME_STAGE_FINALIZED ||
        primary->sys_base != sys_base || primary->kernel_base != kernel_base)
        return 0;

    if (runtime->stage == P4_EXEC_RUNTIME_STAGE_PREPARED &&
        local->sys_base == NULL && local->kernel_base == NULL)
    {
        local->sys_base = sys_base;
        local->kernel_base = kernel_base;
        runtime->stage = P4_EXEC_RUNTIME_STAGE_FINALIZED;
        return 1;
    }

    return runtime->stage == P4_EXEC_RUNTIME_STAGE_FINALIZED &&
           local->sys_base == sys_base && local->kernel_base == kernel_base;
}

P4_EXEC_RUNTIME_SRAM
int krnP4ExecRuntimeBind(struct ExecBase *sys_base,
                         struct KernelBase *kernel_base)
{
    return krnP4ExecRuntimeFinalize(sys_base, kernel_base);
}
