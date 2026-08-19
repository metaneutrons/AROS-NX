/*
    Copyright (c) 2026, The AROS Development Team. All rights reserved.

    Desc: Early debug console for the esp32p4-riscv target.

    There is no firmware console call to borrow here, so output goes
    straight at the hardware. Two channels are implemented and one is
    compiled in (see P4_CONSOLE_USB in hardware.h):

      - the USB serial/JTAG peripheral, which is what a board with only a
        USB-C socket brings out, and

      - UART0, for boards that route it to pins. It is left exactly as
        the first stage ROM loader configured it to print its own
        messages: reprogramming the divisor before the clock tree is
        understood would lose the one channel that can report what went
        wrong.

    Both waits are bounded. A console that stops draining - an unclocked
    UART, or a USB host that never attached - must not take the boot with
    it, and a dropped character is the lesser loss.
*/

#include <inttypes.h>

#include "hardware.h"
#include "kernel_intern.h"

#define SPIN_LIMIT      100000

static inline uint32_t mmio_rd(uint32_t base, uint32_t off)
{
    return *(volatile uint32_t *)(base + off);
}

static inline void mmio_wr(uint32_t base, uint32_t off, uint32_t val)
{
    *(volatile uint32_t *)(base + off) = val;
}

#if P4_CONSOLE_USB

/*
 * Bytes accumulate in the endpoint buffer and are handed over on a
 * newline or when it fills. Flushing every byte would work but would cap
 * the console at one USB transaction per character.
 */
static unsigned int usj_pending;

static void usj_flush(void)
{
    unsigned int spins = SPIN_LIMIT;

    if (!usj_pending)
        return;

    mmio_wr(P4_USJ_BASE, P4_USJ_EP1_CONF, P4_USJ_WR_DONE);
    usj_pending = 0;

    /* Wait for the host to collect it, so the next byte has somewhere to
       go. If no host ever does, give up and keep going. */
    while (spins--)
    {
        if (mmio_rd(P4_USJ_BASE, P4_USJ_EP1_CONF) & P4_USJ_IN_EP_DATA_FREE)
            return;
    }
}

void krnP4PutC(char c)
{
    if (!(mmio_rd(P4_USJ_BASE, P4_USJ_EP1_CONF) & P4_USJ_IN_EP_DATA_FREE))
        return;

    mmio_wr(P4_USJ_BASE, P4_USJ_EP1, (uint32_t)(unsigned char)c);

    if (++usj_pending >= P4_USJ_EP1_DEPTH || c == '\n')
        usj_flush();
}

#else /* UART0 */

void krnP4PutC(char c)
{
    unsigned int spins = SPIN_LIMIT;

    while (spins--)
    {
        uint32_t used = (mmio_rd(P4_UART0_BASE, P4_UART_STATUS)
                            >> P4_UART_TXFIFO_CNT_S) & P4_UART_TXFIFO_CNT_M;

        if (used < P4_UART_FIFO_DEPTH)
        {
            mmio_wr(P4_UART0_BASE, P4_UART_FIFO, (uint32_t)(unsigned char)c);
            return;
        }
    }
}

#endif

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
