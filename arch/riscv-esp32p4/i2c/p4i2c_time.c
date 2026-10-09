/*
    Copyright (c) 2026, The AROS Development Team. All rights reserved.

    Desc: The SYSTIMER count the shared I2C transport bounds its waits with.
*/

/*
 * The transport only needs a monotonic 16 MHz count for its 50 ms bound.
 * The kernel supplies krnTimerCount(); a package module may not call it,
 * so this reads SYSTIMER directly, as sdcard.device does.
 */

#include <exec/types.h>

#include "../kernel/hardware.h"
#include "p4i2c_intern.h"

BOOL p4i2c_TimePrepare(void)
{
    return TRUE;
}

/*
 * The snapshot registers are shared by both harts and the kernel. Another
 * hart's snapshot between this one's request and its reads is harmless
 * (the counter only grows) unless the high half carries between the two
 * reads, which the second read of the high half shows: ask again then.
 */
uint64_t p4i2c_now(void)
{
    unsigned int tries = 8;
    ULONG high, low, again;

    do
    {
        unsigned int spins = 1000;

        p4_w32(P4_SYSTIMER_BASE + P4_ST_UNIT0_OP, P4_ST_UNIT0_UPDATE);
        __asm__ volatile("fence iorw, iorw" ::: "memory");
        while (spins-- != 0
               && !(p4_r32(P4_SYSTIMER_BASE + P4_ST_UNIT0_OP) &
                    P4_ST_UNIT0_VALID))
            ;
        high = p4_r32(P4_SYSTIMER_BASE + P4_ST_UNIT0_VALUE_HI);
        low = p4_r32(P4_SYSTIMER_BASE + P4_ST_UNIT0_VALUE_LO);
        again = p4_r32(P4_SYSTIMER_BASE + P4_ST_UNIT0_VALUE_HI);
    } while (again != high && --tries);

    return ((uint64_t)again << 32) | low;
}

