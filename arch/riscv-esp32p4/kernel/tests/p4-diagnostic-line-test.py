#!/usr/bin/env python3
"""Exercise the production P4 diagnostic-line writer against mock MMIO."""

import pathlib
import re
import subprocess
import tempfile


kernel = pathlib.Path(__file__).resolve().parents[1]
source = (kernel / "kernel_console.c").read_text()
start = source.index("#if defined(P4_E3_FPU_TEST) && (P4_E3_FPU_TEST == 1)")
end_marker = "#endif /* P4 E3 diagnostic-line users */"
end = source.index(end_marker, start) + len(end_marker)
production = source[start:end]
production, replacements = re.subn(
    r"static inline uint32_t p4_diagnostic_mcycle\(void\)\s*\{.*?\n\}",
    "static inline uint32_t p4_diagnostic_mcycle(void)\n"
    "{ return mock_mcycle(); }",
    production,
    count=1,
    flags=re.S,
)
assert replacements == 1

fixture = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define P4_USJ_BASE 0x1000U
#define P4_USJ_EP1 0x0000U
#define P4_USJ_EP1_CONF 0x0004U
#define P4_USJ_WR_DONE (1U << 0)
#define P4_USJ_IN_EP_DATA_FREE (1U << 1)
#define P4_USJ_EP1_DEPTH 64U
#define P4_UART0_BASE 0x2000U
#define P4_UART_FIFO 0x0000U
#define P4_UART_STATUS 0x001cU
#define P4_UART_FSM_STATUS 0x0070U
#define P4_UART_ST_UTX_OUT_M (0x0fU << 4)
#define P4_UART_TXFIFO_CNT_S 16
#define P4_UART_TXFIFO_CNT_M 0xffU
#define P4_UART_FIFO_DEPTH 128U

static uint32_t cycle_value, cycle_step;
static uint32_t mock_mcycle(void)
{
    uint32_t value = cycle_value;
    cycle_value += cycle_step;
    return value;
}

#if P4_CONSOLE_USB
static unsigned int usj_pending;
static uint32_t usb_conf;
static int usb_reader;
static unsigned int usb_release_after, usb_polls_after_done;
static unsigned int usb_stop_after_done;
static unsigned int usb_conf_reads;
static unsigned int usb_done_count, usb_packet_sizes[16];
static unsigned int usb_packet_start, usb_tx_count;
static unsigned char usb_tx[1024];

static uint32_t mmio_rd(uint32_t base, uint32_t off)
{
    assert(base == P4_USJ_BASE && off == P4_USJ_EP1_CONF);
    usb_conf_reads++;
    if (!(usb_conf & P4_USJ_IN_EP_DATA_FREE) && usb_reader &&
        (!usb_stop_after_done || usb_done_count < usb_stop_after_done))
    {
        usb_polls_after_done++;
        if (usb_polls_after_done >= usb_release_after)
            usb_conf |= P4_USJ_IN_EP_DATA_FREE;
    }
    return usb_conf;
}

static void mmio_wr(uint32_t base, uint32_t off, uint32_t value)
{
    assert(base == P4_USJ_BASE);
    if (off == P4_USJ_EP1)
    {
        assert(usb_tx_count < sizeof(usb_tx));
        usb_tx[usb_tx_count++] = (unsigned char)value;
        return;
    }
    assert(off == P4_USJ_EP1_CONF && value == P4_USJ_WR_DONE);
    assert(usb_done_count < sizeof(usb_packet_sizes) /
                                  sizeof(usb_packet_sizes[0]));
    usb_packet_sizes[usb_done_count++] = usb_tx_count - usb_packet_start;
    usb_packet_start = usb_tx_count;
    usb_conf &= ~P4_USJ_IN_EP_DATA_FREE;
    usb_polls_after_done = 0;
}

static void reset_console(void)
{
    usj_pending = 0;
    usb_conf = P4_USJ_IN_EP_DATA_FREE;
    usb_reader = 1;
    usb_release_after = 1;
    usb_polls_after_done = usb_done_count = usb_packet_start = 0;
    usb_stop_after_done = 0;
    usb_conf_reads = 0;
    usb_tx_count = 0;
    cycle_value = 0;
    cycle_step = 10;
    memset(usb_packet_sizes, 0, sizeof(usb_packet_sizes));
    memset(usb_tx, 0, sizeof(usb_tx));
}
#else
static unsigned int uart_used, uart_tx_count, uart_status_reads;
static int uart_drains;
static uint32_t uart_fsm_status;
static unsigned int uart_fsm_busy_reads, uart_fsm_empty_reads;
static int uart_fsm_stuck;
static unsigned char uart_tx[1024];

static uint32_t mmio_rd(uint32_t base, uint32_t off)
{
    assert(base == P4_UART0_BASE);
    if (off == P4_UART_STATUS)
    {
        uint32_t used = uart_used;
        uart_status_reads++;
        if (uart_drains && uart_used)
            uart_used--;
        return (used & P4_UART_TXFIFO_CNT_M) << P4_UART_TXFIFO_CNT_S;
    }
    assert(off == P4_UART_FSM_STATUS);
    if (uart_used == 0)
        uart_fsm_empty_reads++;
    {
        uint32_t status = uart_fsm_status;
        if (!uart_fsm_stuck && uart_fsm_busy_reads)
        {
            uart_fsm_busy_reads--;
            if (!uart_fsm_busy_reads)
                uart_fsm_status = 0;
        }
        return status;
    }
}

static void mmio_wr(uint32_t base, uint32_t off, uint32_t value)
{
    assert(base == P4_UART0_BASE && off == P4_UART_FIFO);
    assert(uart_tx_count < sizeof(uart_tx));
    uart_tx[uart_tx_count++] = (unsigned char)value;
    uart_used++;
    uart_fsm_status = 1U << 4;
    uart_fsm_busy_reads = 2;
}

static void reset_console(void)
{
    uart_used = uart_tx_count = uart_status_reads = 0;
    uart_drains = 1;
    uart_fsm_status = uart_fsm_busy_reads = uart_fsm_empty_reads = 0;
    uart_fsm_stuck = 0;
    cycle_value = 0;
    cycle_step = 10;
    memset(uart_tx, 0, sizeof(uart_tx));
}
#endif
'''

usb_tests = r'''
static void test_usb_success_and_boundaries(void)
{
    char line[131];
    unsigned int i;

    reset_console();
    assert(krnP4DiagnosticLine("ok"));
    assert(usb_tx_count == 4 && memcmp(usb_tx, "\nok\n", 4) == 0);
    assert(usb_done_count == 2 && usb_packet_sizes[0] == 1 &&
           usb_packet_sizes[1] == 3);
    assert(usj_pending == 0);

    reset_console();
    for (i = 0; i < 129; i++)
        line[i] = 'a' + (i % 26);
    line[129] = '\n';
    line[130] = '\0';
    assert(krnP4DiagnosticLine(line));
    assert(usb_tx_count == 131 && usb_tx[0] == '\n' &&
           memcmp(usb_tx + 1, line, 130) == 0);
    assert(usb_done_count == 4);
    assert(usb_packet_sizes[0] == 1 && usb_packet_sizes[1] == 64 &&
           usb_packet_sizes[2] == 64 && usb_packet_sizes[3] == 2);

    reset_console();
    cycle_value = 0xfffffff0U;
    assert(krnP4DiagnosticLine("wrap\n"));
    assert(usb_tx_count == 6 && usb_done_count == 2);

    reset_console();
    {
        char max_line[513];
        memset(max_line, 'm', 511);
        max_line[511] = '\n';
        max_line[512] = '\0';
        assert(krnP4DiagnosticLine(max_line));
        assert(usb_tx_count == 513 && usb_done_count == 9);
        for (i = 1; i < usb_done_count; i++)
            assert(usb_packet_sizes[i] == 64);
    }
}

static void test_usb_prior_pending_and_validation(void)
{
    char too_long[514];

    reset_console();
    usb_tx[0] = 'x';
    usb_tx[1] = 'y';
    usb_tx_count = 2;
    usj_pending = 2;
    assert(krnP4DiagnosticLine("line\n"));
    assert(usb_done_count == 2);
    assert(usb_packet_sizes[0] == 3 && usb_packet_sizes[1] == 5);
    assert(memcmp(usb_tx, "xy\nline\n", 8) == 0);

    /* Already-sent unterminated output also gets an explicit delimiter. */
    reset_console();
    memcpy(usb_tx, "sent", 4);
    usb_tx_count = usb_packet_start = 4;
    usb_done_count = 1;
    usb_packet_sizes[0] = 4;
    assert(krnP4DiagnosticLine("line\n"));
    assert(memcmp(usb_tx, "sent\nline\n", 10) == 0);
    assert(usb_done_count == 3 && usb_packet_sizes[1] == 1 &&
           usb_packet_sizes[2] == 5);

    reset_console();
    memset(too_long, 'z', sizeof(too_long));
    too_long[513] = '\0';
    assert(!krnP4DiagnosticLine(NULL));
    assert(!krnP4DiagnosticLine("bad\nline"));
    assert(!krnP4DiagnosticLine(too_long));
    assert(usb_tx_count == 0 && usb_done_count == 0 && usj_pending == 0);

    reset_console();
    memset(too_long, 'q', 512);
    too_long[512] = '\0';
    assert(!krnP4DiagnosticLine(too_long));
    assert(usb_tx_count == 0 && usb_done_count == 0);
}

static void test_usb_timeout_does_not_qualify_line(void)
{
    char line[101];
    unsigned int i;

    reset_console();
    for (i = 0; i < 100; i++)
        line[i] = 'p';
    line[100] = '\0';
    usb_reader = 1;
    cycle_step = 200000U;
    usb_stop_after_done = 2;
    assert(!krnP4DiagnosticLine(line));
    assert(usb_tx_count == 65 && usb_done_count == 2);
    assert(usb_packet_sizes[0] == 1 && usb_packet_sizes[1] == 64 &&
           usj_pending == 0);
    assert(usb_tx_count < 102);

    reset_console();
    usb_conf = 0;
    usb_reader = 0;
    cycle_step = 9000000U;
    assert(!krnP4DiagnosticLine("PASS counters\n"));
    assert(usb_tx_count == 0 && usb_done_count == 0);

    reset_console();
    usb_conf = 0;
    usb_reader = 0;
    cycle_step = 0;
    assert(!krnP4DiagnosticLine("stopped cycle\n"));
    assert(usb_tx_count == 0 && usb_conf_reads <=
           P4_DIAGNOSTIC_POLL_LIMIT + 1U);
}

int main(void)
{
    test_usb_success_and_boundaries();
    test_usb_prior_pending_and_validation();
    test_usb_timeout_does_not_qualify_line();
    puts("USB diagnostic-line writer passed");
    return 0;
}
'''

uart_tests = r'''
static void test_uart_success_wrap_and_validation(void)
{
    char line[131];
    char too_long[513];
    unsigned int i;

    reset_console();
    assert(krnP4DiagnosticLine("ok"));
    assert(uart_tx_count == 4 && memcmp(uart_tx, "\nok\n", 4) == 0);
    assert(uart_used == 0 && uart_fsm_empty_reads > 0);

    reset_console();
    cycle_value = 0xfffffff0U;
    for (i = 0; i < 129; i++)
        line[i] = 'A';
    line[129] = '\n';
    line[130] = '\0';
    assert(krnP4DiagnosticLine(line));
    assert(uart_tx_count == 131 && uart_tx[0] == '\n' &&
           memcmp(uart_tx + 1, line, 130) == 0);
    assert(uart_used == 0);

    reset_console();
    memset(too_long, 'x', sizeof(too_long));
    too_long[512] = '\0';
    assert(!krnP4DiagnosticLine(too_long));
    assert(uart_tx_count == 0);
}

static void test_uart_timeout(void)
{
    reset_console();
    uart_used = P4_UART_FIFO_DEPTH;
    uart_drains = 0;
    cycle_step = 9000000U;
    assert(!krnP4DiagnosticLine("no room\n"));
    assert(uart_tx_count == 0 && uart_used == P4_UART_FIFO_DEPTH);

    reset_console();
    uart_drains = 0;
    cycle_step = 1000000U;
    {
        char line[200];
        memset(line, 'u', 199);
        line[199] = '\0';
        assert(!krnP4DiagnosticLine(line));
        assert(uart_tx_count == 1 && uart_tx[0] == '\n');
    }

    reset_console();
    uart_fsm_stuck = 1;
    cycle_step = 2000000U;
    assert(!krnP4DiagnosticLine("wait for shifter\n"));
    assert(uart_tx_count == 1 && uart_tx[0] == '\n');
    assert(uart_used == 0 && uart_fsm_empty_reads > 0);
    assert((uart_fsm_status & P4_UART_ST_UTX_OUT_M) != 0);

    reset_console();
    uart_fsm_stuck = 1;
    cycle_step = 0;
    assert(!krnP4DiagnosticLine("stopped cycle\n"));
    assert(uart_tx_count == 1 && uart_used == 0 &&
           uart_status_reads <= P4_DIAGNOSTIC_POLL_LIMIT + 2U);
}

int main(void)
{
    test_uart_success_wrap_and_validation();
    test_uart_timeout();
    puts("UART diagnostic-line writer passed");
    return 0;
}
'''

for usb in (1, 0):
    tests = usb_tests if usb else uart_tests
    with tempfile.TemporaryDirectory(prefix="p4-diagnostic-line-") as temp:
        binary = str(pathlib.Path(temp) / "test")
        subprocess.run(
            ["clang", "-std=gnu99", "-O1", "-g", "-Wall", "-Wextra",
             "-Werror", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
             "-DP4_E3_FPU_TEST=1", f"-DP4_CONSOLE_USB={usb}",
             "-x", "c", "-", "-o", binary],
            input=fixture + production + tests,
            text=True,
            check=True,
        )
        subprocess.run([binary], check=True)
