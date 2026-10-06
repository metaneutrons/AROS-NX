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

