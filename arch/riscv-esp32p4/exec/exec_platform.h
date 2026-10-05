/*
    Copyright (c) 2026, The AROS Development Team. All rights reserved.

    Desc: exec platform definitions for the esp32p4-riscv target.
*/

#ifndef P4_EXEC_PLATFORM_H
#define P4_EXEC_PLATFORM_H

#include <aros/config.h>

#if !defined(__AROSEXEC_SMP__)
#error "esp32p4 builds only the smp variant (SMP.md, S6)"
#endif

#include <aros/types/spinlock_s.h>
#include <utility/hooks.h>

#include "tls.h"

#define SCHEDQUANTUM_VALUE      4

/* kernel.resource is linked into the same image. These run before
   KernelBase exists, so they are called directly and never use it. */
extern void Kernel_49_KrnSpinInit(spinlock_t *, void *);
#define EXEC_SPINLOCK_INIT(a) Kernel_49_KrnSpinInit((a), NULL)
extern spinlock_t *Kernel_52_KrnSpinLock(spinlock_t *, struct Hook *, ULONG, void *);
#define EXEC_SPINLOCK_LOCK(a,b,c) Kernel_52_KrnSpinLock((a), (b), (c), NULL)
extern void Kernel_53_KrnSpinUnLock(spinlock_t *, void *);
#define EXEC_SPINLOCK_UNLOCK(a) Kernel_53_KrnSpinUnLock((a), NULL)

/* Publishing a freshly built structure to lock-free readers. */
#define EXEC_MEMORY_BARRIER()   asm volatile("fence rw, rw" ::: "memory")

/* No syscall needed: the list changes happen under spinlocks. */
extern void Exec_ReschedTask(struct Task *, ULONG);
#define krnSysCallReschedTask(task, state) Exec_ReschedTask((task), (state))

/* Without this RemTask() and the service task change the scheduler lists
   without locks. */
#define EXEC_REMTASK_NEEDSSWITCH

/* RemTask()'s self-removal: detach and tombstone, then return, so that
   RemTask() can finish; KrnSwitch() would never come back. */
extern void Exec_SuicideSwitch(void);
#define krnSysCallSwitch() Exec_SuicideSwitch()

struct Exec_PlatformData
{
    /* No platform-specific data */
};

/*
 * Interrupt masking around scheduler lists and tc_SpinLock. There is one
 * interrupt level here: the inter-hart interrupt is an ordinary CLIC line,
 * so masking everything is what the arm ports' FIQ masking amounts to.
 * Both pairs nest through the returned previous state.
 */
#define EXEC_IRQFIQ_DISABLE()   ((unsigned int)p4_tls_mask())
#define EXEC_IRQFIQ_RESTORE(s)  p4_tls_unmask((unsigned long)(s))
#define EXEC_FIQ_DISABLE()      EXEC_IRQFIQ_DISABLE()
#define EXEC_FIQ_RESTORE(s)     EXEC_IRQFIQ_RESTORE(s)

/* Per-hart nesting counts and scheduler flags, see tls.h */
#define IDNESTCOUNT_INC             TLS_UPDATE(IDNestCnt, ++)
#define IDNESTCOUNT_DEC             TLS_UPDATE(IDNestCnt, --)
#define TDNESTCOUNT_INC             TLS_UPDATE(TDNestCnt, ++)
#define TDNESTCOUNT_DEC             TLS_UPDATE(TDNestCnt, --)
#define IDNESTCOUNT_GET             ((LONG)TLS_GET(IDNestCnt))
#define IDNESTCOUNT_SET(val)        TLS_SET(IDNestCnt, (val))
#define TDNESTCOUNT_GET             ((LONG)TLS_GET(TDNestCnt))
#define TDNESTCOUNT_SET(val)        TLS_SET(TDNestCnt, (val))

#define FLAG_SCHEDQUANTUM_CLEAR     TLS_UPDATE(ScheduleFlags, &= ~TLSSF_Quantum)
#define FLAG_SCHEDQUANTUM_SET       TLS_UPDATE(ScheduleFlags, |= TLSSF_Quantum)
#define FLAG_SCHEDSWITCH_CLEAR      TLS_UPDATE(ScheduleFlags, &= ~TLSSF_Switch)
#define FLAG_SCHEDSWITCH_SET        TLS_UPDATE(ScheduleFlags, |= TLSSF_Switch)
#define FLAG_SCHEDDISPATCH_CLEAR    TLS_UPDATE(ScheduleFlags, &= ~TLSSF_Dispatch)
#define FLAG_SCHEDDISPATCH_SET      TLS_UPDATE(ScheduleFlags, |= TLSSF_Dispatch)
#define FLAG_SCHEDQUANTUM_ISSET     ((BOOL)((TLS_GET(ScheduleFlags) & TLSSF_Quantum) != 0))
#define FLAG_SCHEDSWITCH_ISSET      ((BOOL)((TLS_GET(ScheduleFlags) & TLSSF_Switch) != 0))
#define FLAG_SCHEDDISPATCH_ISSET    ((BOOL)((TLS_GET(ScheduleFlags) & TLSSF_Dispatch) != 0))

/* TDNestCnt is per hart: this blocks the dispatch at interrupt exit
   without Forbid()/Permit(), whose Permit() could reschedule while a list
   lock is held. */
#define EXEC_BLOCK_DISPATCH_INC     TDNESTCOUNT_INC
#define EXEC_BLOCK_DISPATCH_DEC     TDNESTCOUNT_DEC

#define SCHEDQUANTUM_SET(val)       TLS_SET(Quantum, (val))
#define SCHEDQUANTUM_GET            TLS_GET(Quantum)
#define SCHEDELAPSED_SET(val)       TLS_SET(Elapsed, (val))
#define SCHEDELAPSED_GET            TLS_GET(Elapsed)

#define GET_THIS_TASK               TLS_GET(ThisTask)
/* A running task is also on TaskRunning; core_Switch() takes it off. */
#define SET_THIS_TASK(x) \
    do { \
        TLS_SET(ThisTask, (x)); \
        EXEC_SPINLOCK_LOCK(&PrivExecBase(SysBase)->TaskRunningSpinLock, \
                           NULL, SPINLOCK_MODE_WRITE); \
        AddHead(&PrivExecBase(SysBase)->TaskRunning, (struct Node *)(x)); \
        EXEC_SPINLOCK_UNLOCK(&PrivExecBase(SysBase)->TaskRunningSpinLock); \
    } while (0)

#endif /* P4_EXEC_PLATFORM_H */
