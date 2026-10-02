/*
 * Copyright (c) 2026, The AROS Development Team. All rights reserved.
 *
 * Exec runtime state layered over the ESP32-P4 CPU-local foundation.
 *
 * This interface is preparatory. It does not start hart 1 or make the
 * scheduler, traps, task lists, timers, FPU handling, or Exec locks SMP-safe.
 */

#ifndef P4_CPU_LOCAL_RUNTIME_H
#define P4_CPU_LOCAL_RUNTIME_H

#include <stdint.h>

#include "cpu_local.h"

#define P4_EXEC_RUNTIME_HART_COUNT P4_CPU_LOCAL_HART_COUNT
#define P4_EXEC_RUNTIME_CACHELINE_BYTES 64U
#define P4_EXEC_RUNTIME_DEFAULT_QUANTUM 4U

/* These are deliberately disjoint; the public Exec masks overlap by field. */
#define P4_EXEC_SCHEDULE_QUANTUM  (UINT32_C(1) << 0)
#define P4_EXEC_SCHEDULE_SWITCH   (UINT32_C(1) << 1)
#define P4_EXEC_SCHEDULE_DISPATCH (UINT32_C(1) << 2)

enum p4_exec_runtime_stage
{
    P4_EXEC_RUNTIME_STAGE_EMPTY = 0,
    P4_EXEC_RUNTIME_STAGE_PREPARED = 1,
    P4_EXEC_RUNTIME_STAGE_SYSBASE_BOUND = 2,
    P4_EXEC_RUNTIME_STAGE_FINALIZED = 3
};

struct p4_exec_runtime
{
    uint32_t hart_id;
    uint32_t initialized;
    struct p4_cpu_local *cpu_local;
    uint32_t stage;
    uint32_t quantum;
    uint32_t elapsed;
    uint32_t dispatch_count;
    /* Only the owning hart arms this token under the task-state lock.
     * The scheduler consumes it after saving that task's CPU context,
     * before publishing WAIT/READY. It is not a runnable-state flag. */
    struct Task *wait_pending;
    uint8_t reserved[24];
} __attribute__((aligned(P4_EXEC_RUNTIME_CACHELINE_BYTES)));

_Static_assert(sizeof(struct p4_exec_runtime) ==
                   P4_EXEC_RUNTIME_CACHELINE_BYTES,
               "each P4 Exec runtime record must occupy one cache line");
_Static_assert(__alignof__(struct p4_exec_runtime) >=
                   P4_EXEC_RUNTIME_CACHELINE_BYTES,
               "P4 Exec runtime records must be cache-line aligned");

struct ExecBase;
struct KernelBase;

extern struct p4_exec_runtime
    __p4_exec_runtimes[P4_EXEC_RUNTIME_HART_COUNT];

/* Invalid IDs return NULL; no hart aliases hart 0. */
struct p4_exec_runtime *p4_exec_runtime_for_hart(uint32_t hart_id);

/*
 * Return the initialized runtime for the executing hart only when its
 * CPU-local SysBase matches the caller's A6 SysBase argument. The function
 * reads mhartid directly through the bounds-checked CPU-local accessor; it
 * does not use tp or change the A6 calling convention.
 */
struct p4_exec_runtime *p4_exec_runtime_current(struct ExecBase *sys_base);

/*
 * Set only the caller hart's current-task pointer. The scheduler remains
 * responsible for TaskRunning-list transitions under its qualified lock.
 */
int p4_exec_runtime_set_current_task(struct ExecBase *sys_base,
                                     struct Task *task);

/*
 * Initialize both runtime slots from hart 0 after krnP4CpuLocalPrepare(),
 * while hart 1 remains held in reset. No ExecBase or KernelBase is required.
 * The primary slot is bound to SysBase separately, before Exec platform
 * macros first run. This does not release hart 1.
 */
int krnP4ExecRuntimeBootstrapPrepare(void);

/*
 * Bind hart 0's newly allocated SysBase exactly once. Call immediately after
 * assigning SysBase and before any platform macro that reads runtime state.
 * Repeated calls, including the same pointer, are rejected without mutation.
 * Keep local MIE clear throughout SysBase publication and this transition;
 * timer traps must not observe a new SysBase with an unbound runtime.
 */
int krnP4ExecRuntimeBootstrapBind(struct ExecBase *sys_base);

/*
 * Backward-compatible one-stage preparation for callers that already have
 * both bases. Hart 0 must still have fresh CPU-local/runtime slots.
 */
int krnP4ExecRuntimePrepare(struct ExecBase *sys_base,
                            struct KernelBase *kernel_base);

/*
 * Attach KernelBase after SysBase-bound runtime use. This only attaches the
 * base and advances the explicit stage; current-task, nesting, quantum,
 * elapsed, dispatch and schedule-flag fields are left intact. Hart 1 can bind
 * only after hart 0 has finalized. The same completed binding is idempotent.
 * The owner must mask local MIE during this multi-field stage transition;
 * hart 1 performs it before enabling its interrupts. Primary publication to
 * hart 1 also requires the launch controller's explicit release protocol.
 */
int krnP4ExecRuntimeFinalize(struct ExecBase *sys_base,
                             struct KernelBase *kernel_base);

/* Legacy name for krnP4ExecRuntimeFinalize(). */
int krnP4ExecRuntimeBind(struct ExecBase *sys_base,
                         struct KernelBase *kernel_base);

#endif /* P4_CPU_LOCAL_RUNTIME_H */
