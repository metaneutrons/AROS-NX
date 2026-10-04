/*
    Copyright (c) 2026, The AROS Development Team. All rights reserved.

    Desc: The SYSTIMER count the shared I2C transport bounds its waits with.
*/

/*
 * The transport only needs a monotonic 16 MHz count for its 50 ms bound.
 * The kernel supplies krnTimerCount(); a package module may not call it.
 * As in sdcard.device: an SMP kernel publishes the count through
 * KATTR_PlatformTimer and a module must use that, because SYSTIMER's
 * update/valid handshake is not safe to run from two harts; a single-hart
 * build reads SYSTIMER directly.
 */

#include <exec/types.h>

#include "../kernel/hardware.h"
#include "p4i2c_intern.h"

#if defined(__AROSEXEC_SMP__)
#include <aros/kernel.h>
#include <aros/p4timer.h>
#include <proto/exec.h>
#include <proto/kernel.h>

/* Set once at init, before any bus object exists. */
static const struct KrnP4TimerOps *p4i2c_timer;

BOOL p4i2c_TimePrepare(void)
{
    APTR KernelBase = OpenResource("kernel.resource");
    IPTR value;
    const struct KrnP4TimerOps *ops;

    if (!KernelBase)
        return FALSE;
    value = (IPTR)KrnGetSystemAttr(KATTR_PlatformTimer);
    if (value == 0 || value == (IPTR)-1)
        return FALSE;
    ops = (const struct KrnP4TimerOps *)value;
    if (ops->version != KRN_P4_TIMER_OPS_VERSION
        || ops->size < sizeof(*ops) || ops->frequency != P4_SYSTIMER_HZ
        || ops->read == NULL)
        return FALSE;
    p4i2c_timer = ops;
    return TRUE;
}

uint64_t p4i2c_now(void)
{
    /* Init refuses to load without the ops, so no bus object can get
       here with p4i2c_timer unset. */
    return p4i2c_timer->read();
}

#else

BOOL p4i2c_TimePrepare(void)
{
    return TRUE;
}

uint64_t p4i2c_now(void)
{
    unsigned int spins = 1000;
    ULONG high, low;

    p4_w32(P4_SYSTIMER_BASE + P4_ST_UNIT0_OP, P4_ST_UNIT0_UPDATE);
    __asm__ volatile("fence iorw, iorw" ::: "memory");
    while (spins-- != 0
           && !(p4_r32(P4_SYSTIMER_BASE + P4_ST_UNIT0_OP) & P4_ST_UNIT0_VALID))
        ;
    high = p4_r32(P4_SYSTIMER_BASE + P4_ST_UNIT0_VALUE_HI);
    low = p4_r32(P4_SYSTIMER_BASE + P4_ST_UNIT0_VALUE_LO);
    return ((uint64_t)high << 32) | low;
}

#endif
