/*
 * Copyright (c) 2026, The AROS Development Team. All rights reserved.
 *
 * ESP32-P4 Exec platform mappings. Normal builds include the generic header
 * unchanged; the transitional Giant build (P4_GIANT) has its own.
 */

#ifndef P4_EXEC_PLATFORM_H
#define P4_EXEC_PLATFORM_H

#include <aros/config.h>

#if defined(P4_GIANT)

/*
 * Transitional Giant Exec SMP (kernel/p4_giant.h): the normal Exec ABI with
 * per-hart ThisTask, nesting counts and scheduler flags, and Forbid() and
 * Disable() made exclusive across harts by two global locks. SysBase's own
 * IDNestCnt/TDNestCnt/AttnResched/Elapsed fields are not used; ThisTask is
 * kept as hart 0's task for code that reads it directly.
 */
#include <exec/execbase.h>
#include "../kernel/p4_giant.h"

#define SCHEDQUANTUM_VALUE      4

struct Exec_PlatformData
{
    /* No ExecBase layout change. */
};

#define IDNESTCOUNT_INC         P4_GIANT_SET(id_nest, P4_GIANT_GET(id_nest) + 1)
#define IDNESTCOUNT_DEC         P4_GIANT_SET(id_nest, P4_GIANT_GET(id_nest) - 1)
#define TDNESTCOUNT_INC         P4_GIANT_SET(td_nest, P4_GIANT_GET(td_nest) + 1)
#define TDNESTCOUNT_DEC         P4_GIANT_SET(td_nest, P4_GIANT_GET(td_nest) - 1)
#define IDNESTCOUNT_GET         P4_GIANT_GET(id_nest)
#define IDNESTCOUNT_SET(val)    P4_GIANT_SET(id_nest, (val))
#define TDNESTCOUNT_GET         P4_GIANT_GET(td_nest)
#define TDNESTCOUNT_SET(val)    P4_GIANT_SET(td_nest, (val))

#define FLAG_SCHEDQUANTUM_CLEAR     P4_GIANT_SCHED_CLEAR(P4_GIANT_QUANTUM)
#define FLAG_SCHEDQUANTUM_SET       P4_GIANT_SCHED_SET(P4_GIANT_QUANTUM)
#define FLAG_SCHEDSWITCH_CLEAR      P4_GIANT_SCHED_CLEAR(P4_GIANT_SWITCH)
#define FLAG_SCHEDSWITCH_SET        P4_GIANT_SCHED_SET(P4_GIANT_SWITCH)
#define FLAG_SCHEDDISPATCH_CLEAR    P4_GIANT_SCHED_CLEAR(P4_GIANT_DISPATCH)
#define FLAG_SCHEDDISPATCH_SET      P4_GIANT_SCHED_SET(P4_GIANT_DISPATCH)
#define FLAG_SCHEDQUANTUM_ISSET     P4_GIANT_SCHED_ISSET(P4_GIANT_QUANTUM)
#define FLAG_SCHEDSWITCH_ISSET      P4_GIANT_SCHED_ISSET(P4_GIANT_SWITCH)
#define FLAG_SCHEDDISPATCH_ISSET    P4_GIANT_SCHED_ISSET(P4_GIANT_DISPATCH)

#define SCHEDQUANTUM_SET(val)   (SysBase->Quantum = (val))
#define SCHEDQUANTUM_GET        (SysBase->Quantum)
#define SCHEDELAPSED_SET(val)   P4_GIANT_SET(elapsed, (val))
#define SCHEDELAPSED_GET        P4_GIANT_GET(elapsed)

#define GET_THIS_TASK           P4_GIANT_GET(current)
#define SET_THIS_TASK(x)        p4_giant_set_current(SysBase, (x))

/* Soft interrupts are dispatched on hart 0 only. */
#define EXEC_PLATFORM_SOFTINT_PENDING() \
    (p4_giant_hart() == 0 && (SysBase->SysFlags & SFF_SoftInt))

/* Hooks in rom/exec, all called with D held unless noted. */
#define EXEC_PLATFORM_TASK_READY(task)      p4_giant_task_ready(task)
/* RemTask() of another task, under Disable(): 1 if it runs on the other
   hart; it is then marked removed and the eviction requested. The wait
   for the eviction runs after Enable(), Forbid() still held. */
#define EXEC_PLATFORM_REMOVE_RUNNING(task)  p4_giant_evict_request(task)
#define EXEC_PLATFORM_REMOVE_WAIT(task)     p4_giant_evict_wait(task)
#define EXEC_PLATFORM_FIND_RUNNING(name)    p4_giant_running_elsewhere(name)

#if defined(P4_GIANT_MIGRATE)
/* Every task created from now on may run on either hart. */
#define EXEC_PLATFORM_TASK_FLAGS_INIT(task) ((task)->tc_Flags |= P4_TF_HART1_OK)
#endif

#else

#include "../../../rom/exec/exec_platform.h"

#endif /* P4_GIANT */

#endif /* P4_EXEC_PLATFORM_H */
