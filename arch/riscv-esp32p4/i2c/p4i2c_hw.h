/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: ESP32-P4 I2C master transport (p4i2c_hw.c), shared by the kernel
          and the hidd.i2c bus driver.
*/
#ifndef ESP32P4_I2C_HW_H
#define ESP32P4_I2C_HW_H

#include <stdint.h>

/*
 * Transport results.  A NACK and a timeout are different answers and a
 * caller has to be able to tell them apart: nothing at the address, against
 * something holding the line.
 */
#define P4_I2C_OK           0
#define P4_I2C_NACK         (-1)
#define P4_I2C_TIMEOUT      (-2)
#define P4_I2C_ARBLOST      (-3)
#define P4_I2C_STUCK        (-4)
#define P4_I2C_BUSY         (-5)
#define P4_I2C_TOOLONG      (-6)
#define P4_I2C_NOTREADY     (-7)

#define P4_I2C_MAX_WRITE    30      /* the TX FIFO also carries the address */
#define P4_I2C_MAX_READ     255     /* one hardware command byte count */

/* One controller.  Filled by p4i2c_init(); the caller owns the storage and
   serialises access. */
struct P4I2CPort
{
    unsigned long base;
    unsigned long div_reg;
    unsigned int  div_shift;
    unsigned int  ready;
    unsigned long last_raw, last_sr;    /* last transfer, for diagnosis */
};

/* SYSTIMER count (P4_SYSTIMER_HZ), supplied by whoever links the file. */
uint64_t p4i2c_now(void);

/* Port 0 or 1 on the given pads at bus_hz; non-zero on success.  A second
   call fully re-initialises the controller. */
int p4i2c_init(struct P4I2CPort *p, unsigned int port, unsigned int sda_gpio,
               unsigned int scl_gpio, unsigned long bus_hz);

/* Write wlen bytes, then read rlen after a repeated start, then stop.
   7-bit address.  P4_I2C_* result. */
int p4i2c_transfer(struct P4I2CPort *p, unsigned int address,
                   const unsigned char *wbuf, unsigned int wlen,
                   unsigned char *rbuf, unsigned int rlen);

/* Start, address, stop: does anything acknowledge? */
int p4i2c_probe(struct P4I2CPort *p, unsigned int address);

const char *p4i2c_result_name(int result);

#endif
