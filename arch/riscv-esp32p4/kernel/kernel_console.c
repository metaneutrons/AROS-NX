/*
    Copyright (c) 2026, The AROS Development Team. All rights reserved.

    Desc: Early UART0 debug console for the esp32p4-riscv target.

    There is no firmware console call to borrow here, so output goes
    straight at the UART. The first stage ROM loader has already brought
    UART0 up to print its own messages, and it is left exactly as it was
    found: reprogramming the divisor before the clock tree is understood
    would lose the one channel that can report what went wrong.
*/

#include <inttypes.h>

#include "hardware.h"
#include "kernel_intern.h"

static inline uint32_t uart_rd(uint32_t off)
{
    return *(volatile uint32_t *)(P4_UART0_BASE + off);
}

static inline void uart_wr(uint32_t off, uint32_t val)
{
    *(volatile uint32_t *)(P4_UART0_BASE + off) = val;
}

/*
 * Wait for room in the transmit FIFO. Bounded: a UART whose FIFO never
 * drains - unclocked, or held in reset - must not take the boot with it,
 * and a dropped character is the lesser loss.
 */
void krnP4PutC(char c)
{
    unsigned int spins = 100000;

    while (spins--)
    {
        uint32_t used = (uart_rd(P4_UART_STATUS) >> P4_UART_TXFIFO_CNT_S)
                      & P4_UART_TXFIFO_CNT_M;

        if (used < P4_UART_FIFO_DEPTH)
        {
            uart_wr(P4_UART_FIFO, (uint32_t)(unsigned char)c);
            return;
        }
    }
}

void krnP4PutStr(const char *s)
{
    while (*s)
        krnP4PutC(*s++);
}

void krnP4PutHex32(uint32_t val)
{
    static const char hexchars[] = "0123456789abcdef";
    char buf[11];
    int i;

    buf[0] = '0';
    buf[1] = 'x';
    for (i = 0; i < 8; i++)
        buf[2 + i] = hexchars[(val >> (28 - i * 4)) & 0xF];
    buf[10] = '\0';

    krnP4PutStr(buf);
}

void krnP4PutDec(uint32_t val)
{
    char buf[11];
    int i = 10;

    buf[i] = '\0';
    do
    {
        buf[--i] = '0' + (val % 10);
        val /= 10;
    } while (val && i > 0);

    krnP4PutStr(&buf[i]);
}
