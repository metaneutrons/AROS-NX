/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: The kernel's own I2C: one controller at a time, over the shared
          transport in ../i2c/p4i2c_hw.c.
*/

/*
 * The kernel needs I2C before Exec, for the D1001's panel power sequence
 * through its port expander, and for the boot-time touch diagnostics.
 * Everything after boot goes through the hidd.i2c bus driver
 * (i2c-esp32p4.hidd), which links its own copy of the transport; the two
 * never drive a controller at the same time because the kernel's users run
 * before Exec or from the kernel touch drivers that the HIDD replaces.
 */

#include "../i2c/p4i2c_hw.c"

#include "kernel_intern.h"

static struct P4I2CPort krn_i2c;

uint64_t p4i2c_now(void)
{
    return krnTimerCount();
}

int krnP4I2CInit(unsigned int port, unsigned int sda_gpio,
                 unsigned int scl_gpio, unsigned long bus_hz)
{
    return p4i2c_init(&krn_i2c, port, sda_gpio, scl_gpio, bus_hz);
}

int krnP4I2CTransfer(unsigned int address,
                     const unsigned char *wbuf, unsigned int wlen,
                     unsigned char *rbuf, unsigned int rlen)
{
    return p4i2c_transfer(&krn_i2c, address, wbuf, wlen, rbuf, rlen);
}

int krnP4I2CProbe(unsigned int address)
{
    return p4i2c_probe(&krn_i2c, address);
}

/* The last transfer's raw interrupt and status words, for diagnosis only. */
void krnP4I2CLastStatus(unsigned long *raw, unsigned long *sr)
{
    if (raw)
        *raw = krn_i2c.last_raw;
    if (sr)
        *sr = krn_i2c.last_sr;
}
