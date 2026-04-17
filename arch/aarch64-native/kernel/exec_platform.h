/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: exec_platform.h for AArch64 native.
          TLS accessed via TPIDR_EL1 system register.
*/
#ifndef __EXEC_PLATFORM_H
#define __EXEC_PLATFORM_H

#include <aros/config.h>

#include "tls.h"

#define SCHEDQUANTUM_VALUE      4

#define EXEC_REMTASK_NEEDSSWITCH

/* RemTask self-removal: switch away from the dying task via SVC */
#define krnSysCallSwitch() \
    __asm__ volatile("svc #2" ::: "x30", "memory")  /* SC_SWITCH = 2 */
#define krnSysCallReschedTask(task, state) \
    do { (task)->tc_State = (state); Remove(&(task)->tc_Node); } while(0)

struct Exec_PlatformData
{
    /* No platform-specific data by default */
};

/* TLS access via TPIDR_EL1 */
#define __GET_TLS() \
    ({ tls_t *__tls; __asm__ volatile("mrs %0, tpidr_el1" : "=r"(__tls)); __tls; })

#define IDNESTCOUNT_INC     do { __GET_TLS()->IDNestCnt++; } while(0)
#define IDNESTCOUNT_DEC     do { __GET_TLS()->IDNestCnt--; } while(0)
#define TDNESTCOUNT_INC     do { __GET_TLS()->TDNestCnt++; } while(0)
#define TDNESTCOUNT_DEC     do { __GET_TLS()->TDNestCnt--; } while(0)

#define IDNESTCOUNT_GET     ({ __GET_TLS()->IDNestCnt; })
#define IDNESTCOUNT_SET(val) do { __GET_TLS()->IDNestCnt = (val); } while(0)
#define TDNESTCOUNT_GET     ({ __GET_TLS()->TDNestCnt; })
#define TDNESTCOUNT_SET(val) do { __GET_TLS()->TDNestCnt = (val); } while(0)

/*
 * Schedule flag operations.
 *
 * When AROS_NO_ATOMIC_OPERATIONS is defined (e.g. in intserver_vblank.c,
 * kernel_scheduler.c — code that runs with interrupts disabled or from
 * within interrupt handlers), use simple non-atomic operations.
 * Otherwise use AROS_ATOMIC_OR/AND which, if no arch-specific atomics
 * exist, fall back to Disable()/Enable() — which is unsafe from IRQ context.
 */
#if defined(AROS_NO_ATOMIC_OPERATIONS)

#define FLAG_SCHEDQUANTUM_CLEAR  do { __GET_TLS()->ScheduleFlags &= ~TLSSF_Quantum; } while(0)
#define FLAG_SCHEDQUANTUM_SET    do { __GET_TLS()->ScheduleFlags |= TLSSF_Quantum; } while(0)
#define FLAG_SCHEDSWITCH_CLEAR   do { __GET_TLS()->ScheduleFlags &= ~TLSSF_Switch; } while(0)
#define FLAG_SCHEDSWITCH_SET     do { __GET_TLS()->ScheduleFlags |= TLSSF_Switch; } while(0)
#define FLAG_SCHEDDISPATCH_CLEAR do { __GET_TLS()->ScheduleFlags &= ~TLSSF_Dispatch; } while(0)
#define FLAG_SCHEDDISPATCH_SET   do { __GET_TLS()->ScheduleFlags |= TLSSF_Dispatch; } while(0)

#else /* !AROS_NO_ATOMIC_OPERATIONS */

#include <aros/atomic.h>

#define FLAG_SCHEDQUANTUM_CLEAR  do { AROS_ATOMIC_AND(__GET_TLS()->ScheduleFlags, ~TLSSF_Quantum); } while(0)
#define FLAG_SCHEDQUANTUM_SET    do { AROS_ATOMIC_OR(__GET_TLS()->ScheduleFlags, TLSSF_Quantum); } while(0)
#define FLAG_SCHEDSWITCH_CLEAR   do { AROS_ATOMIC_AND(__GET_TLS()->ScheduleFlags, ~TLSSF_Switch); } while(0)
#define FLAG_SCHEDSWITCH_SET     do { AROS_ATOMIC_OR(__GET_TLS()->ScheduleFlags, TLSSF_Switch); } while(0)
#define FLAG_SCHEDDISPATCH_CLEAR do { AROS_ATOMIC_AND(__GET_TLS()->ScheduleFlags, ~TLSSF_Dispatch); } while(0)
#define FLAG_SCHEDDISPATCH_SET   do { AROS_ATOMIC_OR(__GET_TLS()->ScheduleFlags, TLSSF_Dispatch); } while(0)

#endif /* !AROS_NO_ATOMIC_OPERATIONS */

#define FLAG_SCHEDQUANTUM_ISSET  ({ (__GET_TLS()->ScheduleFlags & TLSSF_Quantum) ? TRUE : FALSE; })
#define FLAG_SCHEDSWITCH_ISSET   ({ (__GET_TLS()->ScheduleFlags & TLSSF_Switch) ? TRUE : FALSE; })
#define FLAG_SCHEDDISPATCH_ISSET ({ (__GET_TLS()->ScheduleFlags & TLSSF_Dispatch) ? TRUE : FALSE; })

#define GET_THIS_TASK           TLS_GET(ThisTask)
#define SET_THIS_TASK(x)        do { TLS_SET(ThisTask,(x)); SysBase->ThisTask = (x); } while(0)

#define SCHEDQUANTUM_SET(val)   (SysBase->Quantum=(val))
#define SCHEDQUANTUM_GET        (SysBase->Quantum)
#define SCHEDELAPSED_SET(val)   (SysBase->Elapsed=(val))
#define SCHEDELAPSED_GET        (SysBase->Elapsed)

#endif /* __EXEC_PLATFORM_H */
