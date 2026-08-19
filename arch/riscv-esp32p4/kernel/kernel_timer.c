/*
    Copyright (c) 2026, The AROS Development Team. All rights reserved.

    Desc: The periodic tick, and the only clock there is.

    SYSTIMER comparator 0 runs in period mode, so it reloads itself and a
    tick costs one acknowledgement rather than a new deadline computed per
    interrupt. Its interrupt is level triggered, which means clearing the
    pending bit in the CLIC is not enough on its own: the peripheral has
    to be told as well, or the line is still asserted when the handler
    returns and the trap is taken again immediately.
*/

#include <inttypes.h>

#include "hardware.h"
#include "kernel_intern.h"

volatile unsigned long __esp32p4_ticks;

static inline void st_wr(uint32_t off, uint32_t val)
{
    *(volatile uint32_t *)(P4_SYSTIMER_BASE + off) = val;
}

static inline uint32_t st_rd(uint32_t off)
{
    return *(volatile uint32_t *)(P4_SYSTIMER_BASE + off);
}

/*
 * The counter is 52 bits across two registers, so it cannot be read in
 * one go. Asking for a snapshot latches both halves together; without
 * that, a carry between the two reads would produce a value that never
 * existed.
 */
uint64_t krnTimerCount(void)
{
    unsigned int spins = 1000;
    uint32_t hi, lo;

    st_wr(P4_ST_UNIT0_OP, P4_ST_UNIT0_UPDATE);
    while (spins-- && !(st_rd(P4_ST_UNIT0_OP) & P4_ST_UNIT0_VALID))
        ;

    hi = st_rd(P4_ST_UNIT0_VALUE_HI);
    lo = st_rd(P4_ST_UNIT0_VALUE_LO);

    return ((uint64_t)hi << 32) | lo;
}

void krnTimerAck(void)
{
    st_wr(P4_ST_INT_CLR, P4_ST_TARGET0_INT);
}

void krnTimerInit(void)
{
    uint32_t conf;

    /* Counter first, so that reading it works even if the alarm does not */
    conf = st_rd(P4_ST_CONF);
    st_wr(P4_ST_CONF, conf | P4_ST_CLK_EN | P4_ST_UNIT0_WORK_EN);

    /* Comparator 0, reloading every tick */
    st_wr(P4_ST_TARGET0_CONF,
          P4_ST_TARGET0_PERIODIC | (P4_TICK_PERIOD & P4_ST_TARGET0_PERIOD_M));
    st_wr(P4_ST_COMP0_LOAD, P4_ST_COMP0_LOAD_BIT);

    conf = st_rd(P4_ST_CONF);
    st_wr(P4_ST_CONF, conf | P4_ST_TARGET0_WORK_EN);

    st_wr(P4_ST_INT_CLR, P4_ST_TARGET0_INT);
    st_wr(P4_ST_INT_ENA, P4_ST_TARGET0_INT);

    /* Carry the source to a core line, and let the CLIC through */
    *(volatile uint32_t *)P4_INTMTX_MAP(P4_SOURCE_SYSTIMER_T0) = P4_TIMER_LINE;
    krnCLICEnable(P4_TIMER_LINE, 0);
}

/* Counts of the tick, for anything that wants to wait without a scheduler */
unsigned long krnTimerTicks(void)
{
    return __esp32p4_ticks;
}

/*
 * Bounded on purpose. Waiting on a tick that never arrives would leave
 * the machine mute, and a bring-up path must not depend on a mechanism
 * that has not been shown to work yet: if the tick is not running this
 * falls back to spinning, the caller keeps going, and the report gets a
 * chance to say that the tick is not running. Returns whether the wait
 * was actually served by the timer.
 */
int krnTimerWait(unsigned long ticks)
{
    unsigned long until = __esp32p4_ticks + ticks;
    unsigned long guard = 0;

    while ((long)(__esp32p4_ticks - until) < 0)
    {
        /* No wfi here: with the tick dead there would be nothing to wake
           it, and this has to be able to time out. */
        if (++guard > 200000000UL)
            return 0;
    }

    return 1;
}
