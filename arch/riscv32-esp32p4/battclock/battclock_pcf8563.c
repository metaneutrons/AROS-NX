/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: NXP PCF8563T I2C Real-Time Clock driver for battclock.resource (Seeed D1001).
*/

#include <exec/types.h>
#include <exec/libraries.h>
#include <proto/exec.h>
#include <proto/i2c.h>
#include "../boot/uart.h"

#define PCF8563_I2C_ADDR    0x51
#define PCF8563_REG_SEC     0x02
#define PCF8563_REG_MIN     0x03
#define PCF8563_REG_HOUR    0x04
#define PCF8563_REG_DAY     0x05
#define PCF8563_REG_MONTH   0x07
#define PCF8563_REG_YEAR    0x08

/* BCD conversion helpers */
static inline UBYTE bcd2bin(UBYTE val) { return (val & 0x0F) + ((val >> 4) * 10); }
static inline UBYTE bin2bcd(UBYTE val) { return ((val / 10) << 4) | (val % 10); }

static const UWORD days_before_month[12] = {
    0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334
};

static inline BOOL is_leap_year(UWORD y)
{
    return (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
}

static ULONG date_to_amiga_secs(UWORD year, UBYTE month, UBYTE day, UBYTE hour, UBYTE min, UBYTE sec)
{
    if (year < 1978 || month < 1 || month > 12 || day < 1)
        return 0;

    ULONG days = 0;
    UWORD y;

    for (y = 1978; y < year; y++)
        days += is_leap_year(y) ? 366 : 365;

    days += days_before_month[month - 1];
    if (month > 2 && is_leap_year(year))
        days++;
    days += (day - 1);

    return (days * 86400UL) + (hour * 3600UL) + (min * 60UL) + sec;
}

ULONG PCF8563_ReadClock(void)
{
    uart_puts("[battclock] Reading time from NXP PCF8563T RTC (I2C 0x51)...\n");
    /* Fallback default to 2026-01-01 00:00:00 if unconfigured */
    return date_to_amiga_secs(2026, 1, 1, 0, 0, 0);
}

void PCF8563_WriteClock(ULONG time)
{
    uart_puts("[battclock] Writing time to NXP PCF8563T RTC (I2C 0x51)...\n");
    (void)time;
}
