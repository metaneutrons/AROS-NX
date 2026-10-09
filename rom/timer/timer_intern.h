#ifndef _TIMER_INTERN_H
#define _TIMER_INTERN_H

/*
    Copyright © 1995-2020, The AROS Development Team. All rights reserved.
    $Id$

    Desc: Internal information about the timer.device and HIDD's
    Lang: english
*/

#include <aros/config.h>

#include <exec/execbase.h>
#include <exec/lists.h>
#include <exec/interrupts.h>
#include <exec/io.h>
#include <exec/devices.h>
#include <exec/interrupts.h>
#include <devices/timer.h>
#include <dos/bptr.h>
#include <hardware/intbits.h>
#include <aros/asmcall.h>

/* Platform-specific portion */
#include <timer_platform.h>

#if defined(__AROSEXEC_SMP__)
#include <aros/types/spinlock_s.h>
#endif

/*
 * First two of these correspond to UNIT_MICROHZ and UNIT_VBLANK.
 * This is important.
 */
#define TL_MICROHZ	0
#define TL_VBLANK	1
#define TL_WAITVBL	2
#define NUM_LISTS	3

struct TimerBase
{
    /* Required by the system */
    struct Device	        tb_Device;		/* Our base						*/
    APTR		        tb_KernelBase;		/* kernel.resource base					*/

    /* Time counters */
    struct timeval	        tb_CurrentTime;	        /* Absolute system time. Can be set by TR_SETSYSTIME.	*/
    struct timeval	        tb_Elapsed;		/* Relative system time. Used for measuring intervals.	*/

    /* This is required for hardware-specific code */
    APTR		        tb_TimerIRQHandle;	/* Timer IRQ handle					*/
    struct Interrupt	        tb_VBlankInt;		/* Used by older implementations, needs to be removed	*/
    struct Interrupt	        tb_ResetHandler;	/* Stops interrupt generation before a reboot		*/

    /* Request queues */
    struct MinList	        tb_Lists[NUM_LISTS];

    /* EClock counter */
    UQUAD                       tb_ticks_total;	        /* Effective EClock value				*/
    ULONG                       tb_ticks_sec;		/* Fraction of second for CurrentTime in ticks		*/
    ULONG                       tb_ticks_elapsed;	/* Fraction of second for Elapsed in ticks		*/
    ULONG                       tb_prev_tick;		/* Hardware-specific					*/
    ULONG		        tb_eclock_rate;	        /* EClock frequency					*/

#ifdef USE_VBLANK_EMU
    struct timerequest          tb_vblank_timerequest;  /* VBlank emulation request				*/
#endif

    struct PlatformTimer        tb_Platform;		/* Platform-specific data				*/
#if defined(__AROSEXEC_SMP__)
    void                        *tb_ExecLockBase;
    void                        *tb_ListLock;
    void                        *tb_TimeLock;           /* tb_CurrentTime, tb_Elapsed, tb_ticks_total */
#endif
};

#define GetTimerBase(tb)	((struct TimerBase *)(tb))
#define GetDevice(tb)		((struct Device *)(tb))

BOOL common_BeginIO(struct timerequest *timereq, struct TimerBase *TimerBase);
void TimerProcessMicroHZ(struct TimerBase *TimerBase, struct ExecBase *SysBase, BOOL locked);
void TimerProcessVBlank(struct TimerBase *TimerBase, struct ExecBase *SysBase, BOOL locked);
#define handleMicroHZ(x,y) TimerProcessMicroHZ(x,y,FALSE)
#define handleVBlank(x,y) TimerProcessVBlank(x,y,FALSE)
void EClockUpdate(struct TimerBase *TimerBase);
void EClockSet(struct TimerBase *TimerBase);

/*
 * The current and elapsed time are two words each, and the VBlank server
 * advances them. Disable() keeps a reader on the same core from seeing half
 * an update; on an SMP system the server runs on another core, and a
 * reader saw the seconds from before a carry with the microseconds from
 * after it, a second backwards. So on SMP the time is written and read
 * under tb_TimeLock as well. The caller has interrupts masked: the server
 * runs with them masked, everybody else under Disable().
 *
 * tb_TimeLock is never held together with tb_ListLock: code that needs
 * both takes a copy of the time first (timer_GetTimes()), and replies go
 * out under tb_ListLock only, so that a reply handler may read the time.
 */
#if defined(__AROSEXEC_SMP__)
#include <proto/execlock.h>
#endif

static inline void timer_TimeLock(struct TimerBase *TimerBase)
{
#if defined(__AROSEXEC_SMP__)
    struct ExecLockBase *ExecLockBase = TimerBase->tb_ExecLockBase;

    if (ExecLockBase && TimerBase->tb_TimeLock)
        ObtainLock(TimerBase->tb_TimeLock, SPINLOCK_MODE_WRITE, 0);
#else
    (void)TimerBase;
#endif
}

static inline void timer_TimeUnlock(struct TimerBase *TimerBase)
{
#if defined(__AROSEXEC_SMP__)
    struct ExecLockBase *ExecLockBase = TimerBase->tb_ExecLockBase;

    if (ExecLockBase && TimerBase->tb_TimeLock)
        ReleaseLock(TimerBase->tb_TimeLock, 0);
#else
    (void)TimerBase;
#endif
}

/* The time as one consistent pair, the hardware queried first. Interrupts
   masked by the caller; either pointer may be NULL. */
static inline void timer_GetTimes(struct TimerBase *TimerBase,
                                  struct timeval *current,
                                  struct timeval *elapsed)
{
    timer_TimeLock(TimerBase);
    EClockUpdate(TimerBase);
    if (current)
        *current = TimerBase->tb_CurrentTime;
    if (elapsed)
        *elapsed = TimerBase->tb_Elapsed;
    timer_TimeUnlock(TimerBase);
}

/* Call exec VBlank vector, if present */
static inline void vblank_Cause(struct ExecBase *SysBase)
{
    struct IntVector *iv = &SysBase->IntVects[INTB_VERTB];

    if (iv->iv_Code)
        AROS_INTC2(iv->iv_Code, iv->iv_Data, INTF_VERTB);
}

#endif /* _TIMER_INTERN_H */
