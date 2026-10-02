/*
    Copyright (c) 2026, The AROS Development Team. All rights reserved.

    Private per-hart runtime state for the ESP32-P4.

    These slots are a diagnostic foundation only. A hart owns its slot while
    running; another hart may inspect it only after an explicit publication
    protocol or after the owner has stopped. Preparation is performed by hart
    0 while hart 1 is held in reset. Nothing in this interface makes Exec SMP
    safe or changes the single-hart scheduler.
*/

#ifndef P4_CPU_LOCAL_H
#define P4_CPU_LOCAL_H

#include <stddef.h>
#include <stdint.h>

#define P4_CPU_LOCAL_HART_COUNT 2U
#define P4_CPU_LOCAL_SLOT_BYTES 64U

struct ExecBase;
struct KernelBase;
struct Task;
struct ExceptionContext;

/*
 * Test harnesses may replace only this CSR read by defining
 * P4_CPU_LOCAL_READ_MHARTID as a function name before including this file.
 */
#ifndef P4_CPU_LOCAL_READ_MHARTID
#if !defined(__riscv)
#error "Define P4_CPU_LOCAL_READ_MHARTID when building off RISC-V"
#endif
static inline __attribute__((always_inline)) uint32_t
p4_cpu_local_read_mhartid(void)
{
    uint32_t hart_id;

    __asm__ volatile("csrr %0, mhartid" : "=r"(hart_id));
    return hart_id;
}
#define P4_CPU_LOCAL_READ_MHARTID p4_cpu_local_read_mhartid
#endif

struct p4_cpu_local
{
    uint32_t hart_id;
    struct Task *current_task;
    struct ExecBase *sys_base;
    struct KernelBase *kernel_base;
    uint32_t trap_depth;
    uint32_t schedule_flags;
    int32_t id_nest;
    int32_t td_nest;
    uint32_t idle_count;
    uint32_t ticks;
    struct Task *fpu_owner;
} __attribute__((aligned(P4_CPU_LOCAL_SLOT_BYTES)));

_Static_assert(sizeof(struct p4_cpu_local) == P4_CPU_LOCAL_SLOT_BYTES,
               "one CPU-local state must occupy one 64-byte slot");
_Static_assert(__alignof__(struct p4_cpu_local) >= P4_CPU_LOCAL_SLOT_BYTES,
               "CPU-local slots must be 64-byte aligned");
_Static_assert(offsetof(struct p4_cpu_local, hart_id) == 0,
               "hart id must lead the CPU-local slot");

extern struct p4_cpu_local __p4_cpu_locals[P4_CPU_LOCAL_HART_COUNT];

/* Invalid hart IDs return NULL; they never alias hart 0. */
struct p4_cpu_local *p4_cpu_local_for_hart(uint32_t hart_id);
struct p4_cpu_local *p4_cpu_local_current(void);

/*
 * Reset is valid only from hart 0 before hart 1 is released. It returns zero
 * without changing state for an invalid slot, ID, or caller hart.
 */
int p4_cpu_local_reset(struct p4_cpu_local *local, uint32_t hart_id);

/* Owner-only helpers. Rejected operations leave the slot unchanged. */
int p4_cpu_local_trap_enter(struct p4_cpu_local *local, uint32_t hart_id);
int p4_cpu_local_trap_leave(struct p4_cpu_local *local, uint32_t hart_id);

/* Caller must be hart 0 and must keep hart 1 reset-held for this call. */
int krnP4CpuLocalPrepare(void);

#endif /* P4_CPU_LOCAL_H */
