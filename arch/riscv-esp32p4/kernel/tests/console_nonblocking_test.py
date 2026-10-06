#!/usr/bin/env python3
"""Test actual kernel_console.c USB/UART writers against counted mock MMIO."""
import pathlib
import subprocess
import tempfile


kernel = pathlib.Path(__file__).resolve().parents[1]
source = (kernel / "kernel_console.c").read_text()
start = source.index("#define SPIN_LIMIT")
mmio_start = source.index("static inline uint32_t mmio_rd", start)
backend_start = source.index("#if P4_CONSOLE_USB", mmio_start)
console = source[start:mmio_start] + source[backend_start:]

fixture = r'''
#define P4_CONSOLE_BACKEND_ONLY 1
#include <assert.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define P4_USJ_BASE 0x1000U
#define P4_USJ_EP1 0x0000U
#define P4_USJ_EP1_CONF 0x0004U
#define P4_USJ_WR_DONE (1U << 0)
#define P4_USJ_IN_EP_DATA_FREE (1U << 1)
#define P4_USJ_OUT_EP_DATA_AVAIL (1U << 2)
#define P4_USJ_EP1_DEPTH 64U
#define P4_UART0_BASE 0x2000U
#define P4_UART_FIFO 0x0000U
#define P4_UART_STATUS 0x001cU
#define P4_UART_TXFIFO_CNT_S 16
#define P4_UART_TXFIFO_CNT_M 0xffU
#define P4_UART_RXFIFO_CNT_S 0
#define P4_UART_RXFIFO_CNT_M 0xffU
#define P4_UART_FIFO_DEPTH 128U

#if P4_CONSOLE_USB
static uint32_t usb_conf, usb_rx_byte;
static unsigned long conf_reads, endpoint_reads, endpoint_writes;
static unsigned long wr_done_writes, tx_count;
static unsigned char tx_bytes[256];

static uint32_t mmio_rd(uint32_t base, uint32_t off)
{
    assert(base == P4_USJ_BASE);
    if (off == P4_USJ_EP1_CONF)
    {
        conf_reads++;
        return usb_conf;
    }
    if (off == P4_USJ_EP1)
    {
        endpoint_reads++;
        return usb_rx_byte;
    }
    abort();
}

static void mmio_wr(uint32_t base, uint32_t off, uint32_t value)
{
    assert(base == P4_USJ_BASE);
    if (off == P4_USJ_EP1)
    {
        assert(tx_count < sizeof(tx_bytes));
        tx_bytes[tx_count++] = (unsigned char)value;
        endpoint_writes++;
        return;
    }
    if (off == P4_USJ_EP1_CONF && value == P4_USJ_WR_DONE)
    {
        wr_done_writes++;
        /* Simulate a packet that the host has not consumed. */
        usb_conf &= ~P4_USJ_IN_EP_DATA_FREE;
        return;
    }
    abort();
}
#else
static uint32_t uart_used, uart_rx_count, uart_rx_byte;
static unsigned long status_reads, fifo_reads, fifo_writes, tx_count;
static unsigned char tx_bytes[256];

static uint32_t mmio_rd(uint32_t base, uint32_t off)
{
    assert(base == P4_UART0_BASE);
    if (off == P4_UART_STATUS)
    {
        status_reads++;
        return ((uart_used & P4_UART_TXFIFO_CNT_M) << P4_UART_TXFIFO_CNT_S)
             | ((uart_rx_count & P4_UART_RXFIFO_CNT_M)
                << P4_UART_RXFIFO_CNT_S);
    }
    if (off == P4_UART_FIFO)
    {
        fifo_reads++;
        return uart_rx_byte;
    }
    abort();
}

static void mmio_wr(uint32_t base, uint32_t off, uint32_t value)
{
    assert(base == P4_UART0_BASE && off == P4_UART_FIFO);
    assert(tx_count < sizeof(tx_bytes));
    tx_bytes[tx_count++] = (unsigned char)value;
    fifo_writes++;
}
#endif
'''

usb_tests = r'''
static void reset_usb(void)
{
    console_runtime = 0;
    console_dropped = 0;
    usj_pending = 0;
    usb_conf = usb_rx_byte = 0;
    conf_reads = endpoint_reads = endpoint_writes = wr_done_writes = 0;
    tx_count = 0;
    memset(tx_bytes, 0, sizeof(tx_bytes));
}

static void test_usb_early_bounds(void)
{
    unsigned int i;

    reset_usb();
    krnP4PutC('X');
    assert(conf_reads == 2000000UL);
    assert(endpoint_writes == 0 && wr_done_writes == 0);
    assert(console_dropped == 1 && usj_pending == 0);

    reset_usb();
    usb_conf = P4_USJ_IN_EP_DATA_FREE;
    krnP4PutC('\n');
    assert(conf_reads == 1UL + 100000UL);
    assert(endpoint_writes == 1 && tx_bytes[0] == '\n');
    assert(wr_done_writes == 1 && usj_pending == 0);
    assert(console_dropped == 0);

    reset_usb();
    usb_conf = P4_USJ_IN_EP_DATA_FREE;
    for (i = 0; i < P4_USJ_EP1_DEPTH; i++)
        krnP4PutC('a');
    assert(conf_reads == P4_USJ_EP1_DEPTH + 100000UL);
    assert(endpoint_writes == P4_USJ_EP1_DEPTH);
    assert(wr_done_writes == 1 && usj_pending == 0);
    assert(console_dropped == 0);
}

static void test_usb_runtime(void)
{
    static const char saturated[] = "output while the host is not reading";
    unsigned long reads_before, drops_before, writes_before, done_before;
    unsigned int i;

    reset_usb();
    krnP4ConsoleRuntime();
    usb_conf = P4_USJ_IN_EP_DATA_FREE;

    /* The 64th byte hands off once; runtime flush does not wait for the host. */
    for (i = 0; i < P4_USJ_EP1_DEPTH - 1; i++)
        krnP4PutC('a');
    assert(conf_reads == P4_USJ_EP1_DEPTH - 1);
    assert(wr_done_writes == 0 && usj_pending == P4_USJ_EP1_DEPTH - 1);
    krnP4PutC('b');
    assert(conf_reads == P4_USJ_EP1_DEPTH);
    assert(endpoint_writes == P4_USJ_EP1_DEPTH);
    assert(wr_done_writes == 1 && usj_pending == 0);
    assert(console_dropped == 0);

    /* Each saturated character gets one status read, then is dropped. */
    reads_before = conf_reads;
    drops_before = console_dropped;
    writes_before = endpoint_writes;
    done_before = wr_done_writes;
    for (i = 0; i < sizeof(saturated) - 1; i++)
        krnP4PutC(saturated[i]);
    assert(conf_reads - reads_before == sizeof(saturated) - 1);
    assert(console_dropped - drops_before == sizeof(saturated) - 1);
    assert(endpoint_writes == writes_before && wr_done_writes == done_before);
    assert(usj_pending == 0);

    /* DATA_FREE recovery permits output again without a stuck pending count. */
    usb_conf = P4_USJ_IN_EP_DATA_FREE;
    reads_before = conf_reads;
    writes_before = endpoint_writes;
    done_before = wr_done_writes;
    krnP4PutC('R');
    krnP4PutC('\n');
    assert(conf_reads - reads_before == 2);
    assert(endpoint_writes - writes_before == 2);
    assert(tx_bytes[tx_count - 2] == 'R' && tx_bytes[tx_count - 1] == '\n');
    assert(wr_done_writes - done_before == 1 && usj_pending == 0);

    /* GetC flushes a pending prompt without waiting, then consumes input. */
    usb_conf = P4_USJ_IN_EP_DATA_FREE;
    krnP4PutC('>');
    assert(usj_pending == 1);
    usb_conf |= P4_USJ_OUT_EP_DATA_AVAIL;
    usb_rx_byte = 'k';
    reads_before = conf_reads;
    done_before = wr_done_writes;
    assert(krnP4GetC() == 'k');
    assert(conf_reads - reads_before == 1);
    assert(endpoint_reads == 1);
    assert(wr_done_writes - done_before == 1 && usj_pending == 0);

    usb_conf &= ~P4_USJ_OUT_EP_DATA_AVAIL;
    reads_before = conf_reads;
    assert(krnP4GetC() == -1);
    assert(conf_reads - reads_before == 1);
    printf("USB console passed: early bounds, runtime poll/drop, flush/input and recovery\n");
}
'''

uart_tests = r'''
static void reset_uart(void)
{
    console_runtime = 0;
    console_dropped = 0;
    uart_used = P4_UART_FIFO_DEPTH;
    uart_rx_count = uart_rx_byte = 0;
    status_reads = fifo_reads = fifo_writes = 0;
    tx_count = 0;
    memset(tx_bytes, 0, sizeof(tx_bytes));
}

static void test_uart_early_bounds(void)
{
    reset_uart();
    krnP4PutC('X');
    assert(status_reads == 100000UL);
    assert(fifo_writes == 0 && console_dropped == 1);

    uart_used = P4_UART_FIFO_DEPTH - 1;
    status_reads = 0;
    krnP4PutC('E');
    assert(status_reads == 1 && fifo_writes == 1 && tx_bytes[0] == 'E');
}

static void test_uart_runtime(void)
{
    static const char saturated[] = "runtime UART output";
    unsigned long reads_before, drops_before, writes_before;
    unsigned int i;

    reset_uart();
    krnP4ConsoleRuntime();
    reads_before = status_reads;
    drops_before = console_dropped;
    for (i = 0; i < sizeof(saturated) - 1; i++)
        krnP4PutC(saturated[i]);
    assert(status_reads - reads_before == sizeof(saturated) - 1);
    assert(console_dropped - drops_before == sizeof(saturated) - 1);
    assert(fifo_writes == 0);

    uart_used = P4_UART_FIFO_DEPTH - 1;
    writes_before = fifo_writes;
    krnP4PutC('R');
    assert(status_reads - reads_before == sizeof(saturated));
    assert(fifo_writes - writes_before == 1 && tx_bytes[0] == 'R');
    assert(console_dropped - drops_before == sizeof(saturated) - 1);

    uart_rx_count = 1;
    uart_rx_byte = 'k';
    assert(krnP4GetC() == 'k');
    assert(fifo_reads == 1);
    printf("UART console passed: early bounds, runtime poll/drop and recovery\n");
}
'''

for usb in (1, 0):
    tests = usb_tests if usb else uart_tests
    main = ("int main(void) { test_usb_early_bounds(); test_usb_runtime(); return 0; }"
            if usb else
            "int main(void) { test_uart_early_bounds(); test_uart_runtime(); return 0; }")
    with tempfile.TemporaryDirectory(prefix="p4-console-test-") as temporary:
        binary = str(pathlib.Path(temporary) / "test")
        subprocess.run(
            ["clang", "-std=gnu99", "-O1", "-g", "-Wall", "-Wextra",
             "-Werror", "-Wno-unused-function", "-fsanitize=address,undefined",
             "-fno-omit-frame-pointer", f"-DP4_CONSOLE_USB={usb}",
             "-x", "c", "-", "-o", binary],
            input=fixture + console + tests + main,
            text=True,
            check=True,
        )
        subprocess.run([binary], check=True)
