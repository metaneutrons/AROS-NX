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

#include <aros/config.h>

#include "hardware.h"
#include "kernel_intern.h"
#include "tls.h"

#define SPIN_LIMIT      100000

/* Set once before Exec can schedule callers. Runtime debug must never wait
 * for a USB reader: callers include input publication and locked redraws.
 * Saturated output is deliberately lossy; the early boot capture policy is
 * unchanged. This is a single-hart transition, not a console lock. */
static volatile unsigned int console_runtime;
static volatile unsigned long console_dropped;

void krnP4ConsoleRuntime(void)
{
#if !defined(P4_CONSOLE_WAIT)
    console_runtime = 1;
#endif
}

/* Back to waiting output: for a report from a hart that is stuck, and
   around the second hart's start, whose report must not be dropped. */
void krnP4ConsoleBlocking(void)
{
    console_runtime = 0;
}

/*
 * The backends below write one character of a finished line each;
 * krnP4PutC() at the end of this file assembles the lines of both harts.
 * The host test (tests/console_nonblocking_test.py) builds the backends
 * alone, as krnP4PutC() itself, with P4_CONSOLE_BACKEND_ONLY.
 */
#if !defined(P4_CONSOLE_BACKEND_ONLY)
#define CONSOLE_RAW_PUTC console_raw_putc
static void console_raw_putc(char c);
static void console_flush_self(void);
#else
#define CONSOLE_RAW_PUTC krnP4PutC
#endif

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

    if (console_runtime)
        return;

    /* Wait for the host to collect it, so the next byte has somewhere to
       go. If no host ever does, give up and keep going. */
    while (spins--)
    {
        if (mmio_rd(P4_USJ_BASE, P4_USJ_EP1_CONF) & P4_USJ_IN_EP_DATA_FREE)
            return;
    }
}

/*
 * Waits for the host rather than dropping. During bring-up the important
 * output is the earliest, and the host is typically not attached yet when
 * it is produced - so dropping means the one sequence worth reading is
 * the one that cannot be read. Waiting means a listener attaching a
 * second later still gets it from the beginning.
 *
 * Still bounded, so a board with nothing attached boots rather than
 * stopping to talk to itself; the bound is long enough for a host to
 * finish enumerating and short enough not to look like a hang.
 */
#define ATTACH_SPINS    2000000

void CONSOLE_RAW_PUTC(char c)
{
    unsigned int spins;
    unsigned int limit = console_runtime ? 1 : ATTACH_SPINS;

    for (spins = 0; spins < limit; spins++)
    {
        if (mmio_rd(P4_USJ_BASE, P4_USJ_EP1_CONF) & P4_USJ_IN_EP_DATA_FREE)
            break;
    }
    if (spins == limit)
    {
        console_dropped++;
        return;
    }

    mmio_wr(P4_USJ_BASE, P4_USJ_EP1, (uint32_t)(unsigned char)c);

    if (++usj_pending >= P4_USJ_EP1_DEPTH || c == '\n')
        usj_flush();
}

/*
 * One character from the host, or -1 if it has sent none.
 *
 * Never waits.  A caller polling this has to be able to do something else,
 * and the one caller that matters - econsole's Raw_Read() - reschedules
 * between attempts.
 *
 * A pending transmit is flushed first.  Without that, a prompt written
 * without a trailing newline sits in the endpoint buffer unsent while this
 * waits for a reply to it, which is a deadlock made entirely of politeness.
 */
int krnP4GetC(void)
{
#if !defined(P4_CONSOLE_BACKEND_ONLY)
    console_flush_self();
#endif
    if (usj_pending)
        usj_flush();

    if (!(mmio_rd(P4_USJ_BASE, P4_USJ_EP1_CONF) & P4_USJ_OUT_EP_DATA_AVAIL))
        return -1;

    return (int)(mmio_rd(P4_USJ_BASE, P4_USJ_EP1) & 0xFF);
}

#else /* UART0 */

void CONSOLE_RAW_PUTC(char c)
{
    unsigned int spins = console_runtime ? 1 : SPIN_LIMIT;

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
    console_dropped++;
}

int krnP4GetC(void)
{
    if (((mmio_rd(P4_UART0_BASE, P4_UART_STATUS) >> P4_UART_RXFIFO_CNT_S)
            & P4_UART_RXFIFO_CNT_M) == 0)
        return -1;

    return (int)(mmio_rd(P4_UART0_BASE, P4_UART_FIFO) & 0xFF);
}

#endif

#if !defined(P4_CONSOLE_BACKEND_ONLY)
/*
 * Both harts print, mostly a character at a time through RawPutChar().
 * Each hart assembles its line in a buffer of its own, and a finished
 * line goes out whole under a lock, so lines from the two harts do not
 * mix and the USB writer's state has one user at a time. Interrupts are
 * masked while a hart touches its buffer or holds the lock: an interrupt
 * that prints would otherwise find its own hart holding it.
 *
 * The lock records its holder, so a fault taken inside the output, whose
 * report comes back here, does not wait for itself. The wait is bounded
 * like every wait in this file: past it the line goes out regardless, as
 * a mixed line is the lesser loss against a hart stuck on the console.
 */
#define CONSOLE_LINE        128
#define CONSOLE_LOCK_SPINS  20000000

static char console_line[P4_TLS_HARTS][CONSOLE_LINE];
static unsigned int console_len[P4_TLS_HARTS];
static volatile uint32_t console_holder;    /* hart + 1, or 0 */

static inline unsigned int console_hart(void)
{
    unsigned long hart;

    asm volatile("csrr %0, mhartid" : "=r"(hart));
    return (unsigned int)hart & (P4_TLS_HARTS - 1);
}

/* Interrupts are masked by the caller */
static void console_emit(unsigned int hart)
{
    uint32_t me = hart + 1;
    uint32_t free = 0;
    unsigned int spins = 0;
    int locked = 0;
    unsigned int i;

    if (console_holder != me)
    {
        for (;;)
        {
            free = 0;
            if (__atomic_compare_exchange_n(&console_holder, &free, me, 0,
                                            __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
            {
                locked = 1;
                break;
            }
            if (++spins == CONSOLE_LOCK_SPINS)
                break;
        }
    }

    for (i = 0; i < console_len[hart]; i++)
        console_raw_putc(console_line[hart][i]);
    console_len[hart] = 0;

    if (locked)
        __atomic_store_n(&console_holder, 0, __ATOMIC_RELEASE);
}

/* What this hart has of an unfinished line, before it waits for input */
static void console_flush_self(void)
{
    unsigned long s = p4_tls_mask();
    unsigned int hart = console_hart();

    if (console_len[hart])
        console_emit(hart);
    p4_tls_unmask(s);
}

void krnP4PutC(char c)
{
    unsigned long s = p4_tls_mask();
    unsigned int hart = console_hart();

    console_line[hart][console_len[hart]++] = c;
    if (c == '\n' || console_len[hart] == CONSOLE_LINE)
        console_emit(hart);
    p4_tls_unmask(s);
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

/*
 * Resident and node priorities are signed bytes, and printing them through
 * krnP4PutDec() turns -120 into 4294967176 - a number that says nothing and
 * cannot be compared against the priority written in a .conf file.  The
 * negation is done on the unsigned value so that INT32_MIN has no special
 * case.
 */
void krnP4PutDecS(int32_t val)
{
    if (val < 0)
    {
        krnP4PutStr("-");
        krnP4PutDec(-(uint32_t)val);
    }
    else
        krnP4PutDec((uint32_t)val);
}
