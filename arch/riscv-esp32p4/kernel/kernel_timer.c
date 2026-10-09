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
#include "tls.h"

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
 *
 * The snapshot registers are one set for both harts: a snapshot asked
 * for by one between the other's request and its two reads would give
 * that one a high half and a low half from different moments. With two
 * harts the sequence runs under a lock, with interrupts masked so that
 * an interrupt on the holder cannot wait for it.
 */
static volatile uint32_t snapshot_lock;

#ifdef P4_BOOT_TIMING
/* Diagnostic only. No absolute power-on origin is assumed: the bootloader
   may leave this counter disabled or running. Enable it before the first
   USB output, without enabling a comparator or interrupt early. */
static uint64_t boot_origin;
static uint32_t boot_valid;
static uint32_t boot_phase_ms[P4_BOOT_PHASES];
static uint32_t boot_failures;
static uint32_t boot_updates;

/* Unlike the general timer accessor, measurement failure is explicit and
   lock acquisition is a single try. A diagnostic must not wait for a hart. */
static int boot_snapshot(uint64_t *value)
{
    unsigned int spins;
    unsigned long s = p4_tls_mask();
    if (__atomic_exchange_n(&snapshot_lock, 1, __ATOMIC_ACQUIRE))
    {
        p4_tls_unmask(s);
        return 0;
    }
    st_wr(P4_ST_UNIT0_OP, P4_ST_UNIT0_UPDATE);
    for (spins = 0; spins < 1000; ++spins)
        if (st_rd(P4_ST_UNIT0_OP) & P4_ST_UNIT0_VALID)
            break;
    if (spins < 1000)
        *value = ((uint64_t)(st_rd(P4_ST_UNIT0_VALUE_HI) & 0xfffff) << 32)
               | st_rd(P4_ST_UNIT0_VALUE_LO);
    __atomic_store_n(&snapshot_lock, 0, __ATOMIC_RELEASE);
    p4_tls_unmask(s);
    return spins < 1000;
}

void krnP4BootTimingStart(void)
{
    unsigned int i;
    uint64_t next;
    /* Called once on hart0 before interrupts or the secondary are enabled. */
    for (i = 0; i < P4_BOOT_PHASES; ++i)
        boot_phase_ms[i] = UINT32_MAX;
    st_wr(P4_ST_CONF, st_rd(P4_ST_CONF) | P4_ST_CLK_EN | P4_ST_UNIT0_WORK_EN);
    if (boot_snapshot(&boot_origin) && boot_snapshot(&next)
        && next != boot_origin)
        boot_valid = 1;
    else
        boot_failures = 1;
}

static int boot_elapsed(uint32_t *ms)
{
    uint64_t now, delta;
    if (!boot_valid || !boot_snapshot(&now))
        return 0;
    delta = (now - boot_origin) & ((UINT64_C(1) << 52) - 1);
    delta /= P4_SYSTIMER_HZ / 1000;
    if (delta >= UINT32_MAX)
        return 0;
    *ms = (uint32_t)delta;
    return 1;
}

void krnP4BootTimingMark(enum P4BootPhase phase)
{
    uint32_t ms, missing = UINT32_MAX;
    if ((unsigned int)phase >= P4_BOOT_PHASES)
        return;
    if (!boot_elapsed(&ms))
    {
        __atomic_fetch_or(&boot_failures, 1U << (phase + 1), __ATOMIC_RELAXED);
        return;
    }
    __atomic_compare_exchange_n(&boot_phase_ms[phase], &missing, ms, 0,
                                __ATOMIC_RELEASE, __ATOMIC_RELAXED);
}

void krnP4BootTimingUpdate(void)
{
    uint32_t n;
    if (__atomic_load_n(&boot_updates, __ATOMIC_RELAXED) >= 256)
        return;
    n = __atomic_add_fetch(&boot_updates, 1, __ATOMIC_RELAXED);
    if (n == 1) krnP4BootTimingMark(P4_BOOT_UPDATE1);
    if (n == 128) krnP4BootTimingMark(P4_BOOT_UPDATE128);
    if (n == 256) krnP4BootTimingMark(P4_BOOT_UPDATE256);
}

void krnP4BootTimingReport(void)
{
    static unsigned int slot;
    uint32_t now, ms;
    /* Runtime USB output is lossy once the endpoint fills. Keep even the
       longest numeric representation within one 64-byte endpoint packet,
       rotating one retained phase per beat; do not add blocking flushes. */
    krnP4PutStr("[bt] now=");
    if (boot_elapsed(&now)) krnP4PutDec(now);
    else krnP4PutStr("INVALID");
    krnP4PutStr(" fail=");
    krnP4PutHex32(__atomic_load_n(&boot_failures, __ATOMIC_RELAXED));
    krnP4PutStr(" p="); krnP4PutDec(slot);
    ms = __atomic_load_n(&boot_phase_ms[slot], __ATOMIC_ACQUIRE);
    krnP4PutStr(" ms=");
    if (ms != UINT32_MAX) krnP4PutDec(ms);
    else krnP4PutStr("MISSING");
    krnP4PutC('\n');
    slot = (slot + 1) % P4_BOOT_PHASES;
}
#endif

uint64_t krnTimerCount(void)
{
    unsigned int spins = 1000;
    uint32_t hi, lo;
    unsigned long s = p4_tls_mask();

    while (__atomic_exchange_n(&snapshot_lock, 1, __ATOMIC_ACQUIRE))
        ;

    st_wr(P4_ST_UNIT0_OP, P4_ST_UNIT0_UPDATE);
    while (spins-- && !(st_rd(P4_ST_UNIT0_OP) & P4_ST_UNIT0_VALID))
        ;

    hi = st_rd(P4_ST_UNIT0_VALUE_HI);
    lo = st_rd(P4_ST_UNIT0_VALUE_LO);

    __atomic_store_n(&snapshot_lock, 0, __ATOMIC_RELEASE);
    p4_tls_unmask(s);

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
