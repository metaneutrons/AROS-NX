/*
    Copyright (c) 2026, The AROS Development Team. All rights reserved.

    Private per-hart runtime state for the ESP32-P4.
*/

#include "cpu_local.h"

#define P4_CPU_LOCAL_SRAM \
    __attribute__((section(".sramtext.secondary_mailbox"), noinline))

/*
 * The P4 linker scripts collect .bss* after the internal-SRAM location
 * counter is selected. Keep these canonical cached slots in that BSS region.
 */
struct p4_cpu_local __p4_cpu_locals[P4_CPU_LOCAL_HART_COUNT]
    __attribute__((section(".bss.p4_cpu_local"), aligned(P4_CPU_LOCAL_SLOT_BYTES)));

_Static_assert(sizeof(__p4_cpu_locals) ==
                   P4_CPU_LOCAL_HART_COUNT * P4_CPU_LOCAL_SLOT_BYTES,
               "CPU-local slots must be contiguous and independently aligned");

P4_CPU_LOCAL_SRAM
struct p4_cpu_local *p4_cpu_local_for_hart(uint32_t hart_id)
{
    if (hart_id >= P4_CPU_LOCAL_HART_COUNT)
        return NULL;

    return &__p4_cpu_locals[hart_id];
}

P4_CPU_LOCAL_SRAM
struct p4_cpu_local *p4_cpu_local_current(void)
{
    return p4_cpu_local_for_hart(P4_CPU_LOCAL_READ_MHARTID());
}

P4_CPU_LOCAL_SRAM
int p4_cpu_local_reset(struct p4_cpu_local *local, uint32_t hart_id)
{
    if (local == NULL || hart_id >= P4_CPU_LOCAL_HART_COUNT ||
        local != p4_cpu_local_for_hart(hart_id) ||
        P4_CPU_LOCAL_READ_MHARTID() != 0)
        return 0;

    local->hart_id = hart_id;
    local->current_task = NULL;
    local->sys_base = NULL;
    local->kernel_base = NULL;
    local->trap_depth = 0;
    local->schedule_flags = 0;
    local->id_nest = -1;
    local->td_nest = -1;
    local->idle_count = 0;
    local->ticks = 0;
    local->fpu_owner = NULL;

    return 1;
}

static P4_CPU_LOCAL_SRAM
int p4_cpu_local_is_owner(struct p4_cpu_local *local, uint32_t hart_id)
{
    return local != NULL && hart_id < P4_CPU_LOCAL_HART_COUNT &&
           local == p4_cpu_local_for_hart(hart_id) &&
           local->hart_id == hart_id &&
           P4_CPU_LOCAL_READ_MHARTID() == hart_id;
}

P4_CPU_LOCAL_SRAM
int p4_cpu_local_trap_enter(struct p4_cpu_local *local, uint32_t hart_id)
{
    if (!p4_cpu_local_is_owner(local, hart_id) ||
        local->trap_depth == UINT32_MAX)
        return 0;

    local->trap_depth++;
    return 1;
}

P4_CPU_LOCAL_SRAM
int p4_cpu_local_trap_leave(struct p4_cpu_local *local, uint32_t hart_id)
{
    if (!p4_cpu_local_is_owner(local, hart_id) || local->trap_depth == 0)
        return 0;

    local->trap_depth--;
    return 1;
}

P4_CPU_LOCAL_SRAM
int krnP4CpuLocalPrepare(void)
{
    uint32_t hart_id;

    if (P4_CPU_LOCAL_READ_MHARTID() != 0)
        return 0;

    /* Validate the complete target set before changing either slot. */
    for (hart_id = 0; hart_id < P4_CPU_LOCAL_HART_COUNT; hart_id++)
    {
        if (p4_cpu_local_for_hart(hart_id) != &__p4_cpu_locals[hart_id])
            return 0;
    }

    for (hart_id = 0; hart_id < P4_CPU_LOCAL_HART_COUNT; hart_id++)
    {
        if (!p4_cpu_local_reset(&__p4_cpu_locals[hart_id], hart_id))
            return 0;
    }

    return 1;
}
