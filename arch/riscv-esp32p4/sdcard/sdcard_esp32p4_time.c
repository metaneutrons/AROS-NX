/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Polled microsecond timebase for sdcard.device on ESP32-P4.
*/

#if !defined(P4_SD_TIME_TEST)
#include "sdcard_esp32p4_intern.h"
#include "timer.h"
#endif

#if defined(__AROSEXEC_SMP__)
#if !defined(P4_SD_TIME_TEST)
#include <aros/kernel.h>
#include <aros/p4timer.h>
#include <proto/kernel.h>
#endif
/* Initialized once before RegisterBus starts consumers; immutable thereafter. */
static const struct KrnP4TimerOps *p4time_ops;

BOOL p4sd_TimePrepare(struct SDCardBase *SDCardBase)
{
    IPTR value = (IPTR)KrnGetSystemAttr(KATTR_PlatformTimer);
    const struct KrnP4TimerOps *ops;
    if (value == 0 || value == (IPTR)-1)
        return FALSE;
    ops = (const struct KrnP4TimerOps *)value;
    if (ops->version != KRN_P4_TIMER_OPS_VERSION ||
        ops->size < sizeof(*ops) || ops->frequency != P4_SYSTIMER_HZ ||
        ops->read == NULL)
        return FALSE;
    p4time_ops = ops;
    return TRUE;
}

static UQUAD p4time_count(void)
{
    if (p4time_ops == NULL)
    {
        /* A mismatched module/kernel must never revert to ungated MMIO. */
#if defined(P4_SD_TIME_TEST)
        P4_SD_TIME_FATAL();
#else
        __asm__ volatile("csrci mstatus,8" ::: "memory");
        for (;;) __asm__ volatile("wfi" ::: "memory");
#endif
    }
    return p4time_ops->read();
}
#else

static inline ULONG p4time_read(IPTR address)
{
    return *(volatile ULONG *)address;
}

static inline void p4time_write(IPTR address, ULONG value)
{
    *(volatile ULONG *)address = value;
    __asm__ volatile("fence iorw, iorw" ::: "memory");
}

static UQUAD p4time_count(void)
{
    unsigned int spins = 1000;
    ULONG high, low;

    p4time_write(P4_SYSTIMER_BASE + P4_ST_UNIT0_OP, P4_ST_UNIT0_UPDATE);
    while (spins-- != 0 &&
           !(p4time_read(P4_SYSTIMER_BASE + P4_ST_UNIT0_OP) &
             P4_ST_UNIT0_VALID))
        ;

    high = p4time_read(P4_SYSTIMER_BASE + P4_ST_UNIT0_VALUE_HI);
    low = p4time_read(P4_SYSTIMER_BASE + P4_ST_UNIT0_VALUE_LO);
    return ((UQUAD)high << 32) | low;
}
#endif

ULONG sdcard_CurrentTime(void)
{
    return (ULONG)(p4time_count() / (P4_SYSTIMER_HZ / 1000000UL));
}

void sdcard_Udelay(ULONG usec)
{
    ULONG started = sdcard_CurrentTime();

    while ((ULONG)(sdcard_CurrentTime() - started) < usec)
        __asm__ volatile("nop");
}

void sdcard_WaitNano(ULONG ns, struct SDCardBase *SDCardBase)
{
    (void)SDCardBase;

    /* Nothing in the native host requires a sub-microsecond delay.  Round
       up so callers never receive less delay than they requested. */
    if (ns != 0)
        sdcard_Udelay((ns + 999) / 1000);
}
