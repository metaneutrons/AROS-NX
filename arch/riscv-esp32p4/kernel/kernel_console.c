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

/* Set once before Exec can schedule callers. Runtime debug must never wait
 * for a USB reader: callers include input publication and locked redraws.
 * Saturated output is deliberately lossy; the early boot capture policy is
 * unchanged. This is a single-hart transition, not a console lock. */
static volatile unsigned int console_runtime;
static volatile unsigned long console_dropped;

void krnP4ConsoleRuntime(void)
{
    console_runtime = 1;
}

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

void krnP4PutC(char c)
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
    if (usj_pending)
        usj_flush();

    if (!(mmio_rd(P4_USJ_BASE, P4_USJ_EP1_CONF) & P4_USJ_OUT_EP_DATA_AVAIL))
        return -1;

    return (int)(mmio_rd(P4_USJ_BASE, P4_USJ_EP1) & 0xFF);
}

#else /* UART0 */

void krnP4PutC(char c)
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

#if defined(P4_E3_FPU_TEST) && (P4_E3_FPU_TEST == 1) \
    || (defined(P4_E3_RUNTIME_PREPARE) && (P4_E3_RUNTIME_PREPARE == 1))
/*
 * Probe diagnostics are stricter than the ordinary console: the caller
 * holds Forbid(), and a success result means the complete line was handed
 * off. The 36-million-mcycle threshold is the configured cycle budget;
 * converting it to wall time assumes a 360 MHz core clock, which is not
 * calibrated by this code. Unsigned subtraction handles 32-bit wraparound.
 * The poll limit is a fallback if mcycle is stopped or not advancing.
 */
#define P4_DIAGNOSTIC_LINE_MAX       512U
#define P4_DIAGNOSTIC_TIMEOUT_CYCLES 36000000U
#define P4_DIAGNOSTIC_POLL_LIMIT     65536U

static inline uint32_t p4_diagnostic_mcycle(void)
{
    uint32_t cycles;

    __asm__ volatile("csrr %0, mcycle" : "=r"(cycles));
    return cycles;
}

static int p4_diagnostic_before_deadline(uint32_t start,
                                        unsigned int *polls_left)
{
    if (!*polls_left)
        return 0;
    (*polls_left)--;
    return (uint32_t)(p4_diagnostic_mcycle() - start) <
           P4_DIAGNOSTIC_TIMEOUT_CYCLES;
}

#if P4_CONSOLE_USB
static int p4_diagnostic_wait_usb_free(uint32_t start,
                                       unsigned int *polls_left)
{
    for (;;)
    {
        if (mmio_rd(P4_USJ_BASE, P4_USJ_EP1_CONF) &
            P4_USJ_IN_EP_DATA_FREE)
            return p4_diagnostic_before_deadline(start, polls_left);

        if (!p4_diagnostic_before_deadline(start, polls_left))
            return 0;
    }
}
#else
/* ESP-IDF P4 UART_FSM_STATUS_REG offset and ST_UTX_OUT bits [7:4]. */
#define P4_DIAGNOSTIC_UART_FSM_STATUS 0x0070U
#define P4_DIAGNOSTIC_UART_ST_UTX_OUT_M (0x0fU << 4)

static int p4_diagnostic_wait_uart_space(uint32_t start,
                                         unsigned int *polls_left)
{
    for (;;)
    {
        uint32_t used = (mmio_rd(P4_UART0_BASE, P4_UART_STATUS)
                            >> P4_UART_TXFIFO_CNT_S) & P4_UART_TXFIFO_CNT_M;

        if (used < P4_UART_FIFO_DEPTH)
            return p4_diagnostic_before_deadline(start, polls_left);

        if (!p4_diagnostic_before_deadline(start, polls_left))
            return 0;
    }
}

static int p4_diagnostic_wait_uart_empty(uint32_t start,
                                         unsigned int *polls_left)
{
    for (;;)
    {
        uint32_t used = (mmio_rd(P4_UART0_BASE, P4_UART_STATUS)
                            >> P4_UART_TXFIFO_CNT_S) & P4_UART_TXFIFO_CNT_M;

        if (used == 0)
        {
            uint32_t fsm = mmio_rd(P4_UART0_BASE,
                                   P4_DIAGNOSTIC_UART_FSM_STATUS);

            if ((fsm & P4_DIAGNOSTIC_UART_ST_UTX_OUT_M) == 0)
                return p4_diagnostic_before_deadline(start, polls_left);
        }
        if (!p4_diagnostic_before_deadline(start, polls_left))
            return 0;
    }
}
#endif

/*
 * Write one bounded diagnostic line. A missing trailing newline is added;
 * embedded newlines and overlong input are rejected before touching MMIO.
 * USB flushes each 64-byte packet and the final newline, then waits until
 * DATA_FREE confirms the host has collected it. UART waits for both an empty
 * TX FIFO and an idle transmit FSM. A timeout may have transmitted a prefix,
 * but never qualifies a complete diagnostic line.
 */
int krnP4DiagnosticLine(const char *line)
{
    uint32_t start;
    unsigned int polls_left = P4_DIAGNOSTIC_POLL_LIMIT;
    unsigned int length;
    unsigned int total;
    unsigned int i;
    int append_newline;

    if (!line)
        return 0;

    for (length = 0;
         length <= P4_DIAGNOSTIC_LINE_MAX && line[length] != '\0';
         length++)
        ;
    if (length > P4_DIAGNOSTIC_LINE_MAX)
        return 0;

    for (i = 0; i < length; i++)
    {
        if (line[i] == '\n' && i + 1U != length)
            return 0;
    }

    append_newline = !length || line[length - 1U] != '\n';
    if (append_newline && length == P4_DIAGNOSTIC_LINE_MAX)
        return 0;
    total = length + (unsigned int)append_newline;

    start = p4_diagnostic_mcycle();

#if P4_CONSOLE_USB
    /* Delimit even a previously committed partial packet before this line. */
    if (!p4_diagnostic_wait_usb_free(start, &polls_left))
        return 0;
    mmio_wr(P4_USJ_BASE, P4_USJ_EP1, (uint32_t)'\n');
    usj_pending++;
    mmio_wr(P4_USJ_BASE, P4_USJ_EP1_CONF, P4_USJ_WR_DONE);
    usj_pending = 0;
    if (!p4_diagnostic_wait_usb_free(start, &polls_left))
        return 0;

    for (i = 0; i < total; i++)
    {
        char c = (i < length) ? line[i] : '\n';

        if (!p4_diagnostic_wait_usb_free(start, &polls_left))
            return 0;
        mmio_wr(P4_USJ_BASE, P4_USJ_EP1, (uint32_t)(unsigned char)c);
        usj_pending++;

        if (usj_pending >= P4_USJ_EP1_DEPTH || c == '\n')
        {
            mmio_wr(P4_USJ_BASE, P4_USJ_EP1_CONF, P4_USJ_WR_DONE);
            usj_pending = 0;
            if (!p4_diagnostic_wait_usb_free(start, &polls_left))
                return 0;
        }
        else if (!p4_diagnostic_before_deadline(start, &polls_left))
            return 0;
    }
#else
    /* A prior UART fragment may already be in the hardware FIFO. */
    if (!p4_diagnostic_wait_uart_space(start, &polls_left))
        return 0;
    mmio_wr(P4_UART0_BASE, P4_UART_FIFO, (uint32_t)'\n');
    if (!p4_diagnostic_before_deadline(start, &polls_left) ||
        !p4_diagnostic_wait_uart_empty(start, &polls_left))
        return 0;

    for (i = 0; i < total; i++)
    {
        char c = (i < length) ? line[i] : '\n';

        if (!p4_diagnostic_wait_uart_space(start, &polls_left))
            return 0;
        mmio_wr(P4_UART0_BASE, P4_UART_FIFO, (uint32_t)(unsigned char)c);
        if (!p4_diagnostic_before_deadline(start, &polls_left))
            return 0;
    }

    /* The UART has no packet commit bit; wait until queued bytes are sent. */
    return p4_diagnostic_wait_uart_empty(start, &polls_left);
#endif

    return 1;
}
#endif /* P4 E3 diagnostic-line users */

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
