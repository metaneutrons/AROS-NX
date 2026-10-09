/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Polled microsecond timebase for sdcard.device on ESP32-P4.
*/

#include "sdcard_esp32p4_intern.h"
#include "timer.h"

static inline ULONG p4time_read(IPTR address)
{
    return *(volatile ULONG *)address;
}

static inline void p4time_write(IPTR address, ULONG value)
{
    *(volatile ULONG *)address = value;
    __asm__ volatile("fence iorw, iorw" ::: "memory");
}

/*
 * The snapshot registers are one set for both harts and for the kernel
 * (krnTimerCount() serializes its own use). Another hart's snapshot
 * between this one's request and its reads replaces both halves, which
 * is harmless on its own (the counter only grows, so the pair is a later
 * moment and still a valid "now") unless the high half carries between
 * the two reads. Reading the high half again after the low half shows
 * that: if the two highs differ, ask again.
 */
static UQUAD p4time_count(void)
{
    unsigned int tries = 8;
    ULONG high, low, again;

    do
    {
        unsigned int spins = 1000;

        p4time_write(P4_SYSTIMER_BASE + P4_ST_UNIT0_OP, P4_ST_UNIT0_UPDATE);
        while (spins-- != 0 &&
               !(p4time_read(P4_SYSTIMER_BASE + P4_ST_UNIT0_OP) &
                 P4_ST_UNIT0_VALID))
            ;

        high = p4time_read(P4_SYSTIMER_BASE + P4_ST_UNIT0_VALUE_HI);
        low = p4time_read(P4_SYSTIMER_BASE + P4_ST_UNIT0_VALUE_LO);
        again = p4time_read(P4_SYSTIMER_BASE + P4_ST_UNIT0_VALUE_HI);
    } while (again != high && --tries);

    return ((UQUAD)again << 32) | low;
}

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
