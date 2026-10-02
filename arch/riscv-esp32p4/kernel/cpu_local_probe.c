/*
 * Copyright (c) 2026, The AROS Development Team. All rights reserved.
 *
 * Diagnostic owner-only CPU state. Sentinel pointers below are identities,
 * never objects to dereference. No Exec, scheduler, timer or FPU calls here.
 * Both harts have interrupts masked during this stage. A peer may inspect
 * the finished state only after acquiring the E2 completion publication.
 */
#include <stdint.h>
#include "cpu_local.h"
#include "cpu_local_probe.h"
#define SRAM __attribute__((section(".sramtext.secondary_mailbox"), noinline))
#define N 8192u

SRAM int krnP4CpuLocalExercise(void)
{
    struct p4_cpu_local *local = p4_cpu_local_current();
    uint32_t h, i, status;
    __asm__ volatile("csrr %0,mstatus" : "=r"(status));
    if (!local || (status & 8u)) return 0;
    h = local->hart_id;
    if (h > 1 || p4_cpu_local_for_hart(2) ||
        p4_cpu_local_for_hart(UINT32_MAX) || local->trap_depth ||
        local->current_task || local->fpu_owner || local->sys_base ||
        local->kernel_base || local->schedule_flags || local->ticks ||
        local->idle_count || local->id_nest != -1 || local->td_nest != -1)
        return 0;
    if (p4_cpu_local_trap_leave(local, h) ||
        p4_cpu_local_trap_enter(local, h ^ 1u) || local->trap_depth)
        return 0;
    local->trap_depth = UINT32_MAX;
    if (p4_cpu_local_trap_enter(local, h) || local->trap_depth != UINT32_MAX)
        return 0;
    local->trap_depth = 0;
    local->current_task = (struct Task *)(uintptr_t)local;
    local->fpu_owner = (struct Task *)(uintptr_t)&local->ticks;
    local->sys_base = (struct ExecBase *)(uintptr_t)&local->idle_count;
    local->kernel_base = (struct KernelBase *)(uintptr_t)&local->schedule_flags;
    local->id_nest = (int8_t)h;
    local->td_nest = (int8_t)(-1 - (int)h);
    for (i = 0; i < N; ++i) {
        if (!p4_cpu_local_trap_enter(local, h) || local->trap_depth != 1 ||
            !p4_cpu_local_trap_enter(local, h) || local->trap_depth != 2 ||
            !p4_cpu_local_trap_leave(local, h) || local->trap_depth != 1 ||
            !p4_cpu_local_trap_leave(local, h) || local->trap_depth)
            return 0;
        local->schedule_flags = 0x51000000u | (h << 16) | i;
        ++local->ticks;
        ++local->idle_count;
        __asm__ volatile("" ::: "memory");
    }
    return p4_cpu_local_current() == local;
}

int krnP4CpuLocalCheck(void)
{
    uint32_t h;
    /* Caller has acquired the worker completion ticket before this read. */
    for (h = 0; h < 2; ++h) {
        struct p4_cpu_local *local = p4_cpu_local_for_hart(h);
        if (!local || local->hart_id != h || local->trap_depth ||
            local->current_task != (struct Task *)(uintptr_t)local ||
            local->fpu_owner != (struct Task *)(uintptr_t)&local->ticks ||
            local->sys_base != (struct ExecBase *)(uintptr_t)&local->idle_count ||
            local->kernel_base != (struct KernelBase *)(uintptr_t)&local->schedule_flags ||
            local->schedule_flags != (0x51000000u | (h << 16) | (N - 1)) ||
            local->ticks != N || local->idle_count != N ||
            local->id_nest != (int8_t)h || local->td_nest != -1 - (int)h)
            return 0;
    }
    return p4_cpu_local_for_hart(0) != p4_cpu_local_for_hart(1);
}
