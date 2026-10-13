#!/usr/bin/env python3
"""Exercise the actual kernel console queue/framing integration on a host."""

import os
import pathlib
import shlex
import subprocess
import tempfile


KERNEL = pathlib.Path(__file__).resolve().parents[1]
SOURCE = (KERNEL / "kernel_console.c").read_text()
CC = shlex.split(os.environ.get("CC", "cc"))


def extract_console_source():
    prefix_config_start = SOURCE.index("#ifndef P4_CONSOLE_RECORD_PREFIX")
    prefix_config_end = SOURCE.index("/* Runtime mode", prefix_config_start)
    prefix_config = SOURCE[prefix_config_start:prefix_config_end]

    marker = "#define SPIN_LIMIT"
    start = SOURCE.index(marker)
    source = SOURCE[start:]

    mmio_start = source.index("static inline uint32_t mmio_rd(")
    mmio_wr_start = source.index("static inline void mmio_wr(", mmio_start)
    mmio_end = source.index("\n}\n", mmio_wr_start) + len("\n}\n")
    source = source[:mmio_start] + source[mmio_end:]

    hart_start = source.index("static inline unsigned int console_hart(void)\n{")
    hart_end = source.index("\n}\n", hart_start) + len("\n}")
    hart_body = source[hart_start:hart_end]
    if 'asm volatile("csrr %0, mhartid"' not in hart_body:
        raise AssertionError("console_hart no longer contains the expected hart CSR")
    source = (source[:hart_start]
              + "static inline unsigned int console_hart(void)\n{\n"
                "    return p4_test_hart;\n}"
              + source[hart_end:])
    return prefix_config + "\n\n" + source


CONSOLE_SOURCE = extract_console_source()

PREAMBLE = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "console_ring.h"

#define P4_TLS_HARTS 2u
#define P4_USJ_BASE 0x1000u
#define P4_USJ_EP1 0x0000u
#define P4_USJ_EP1_CONF 0x0004u
#define P4_USJ_WR_DONE (1u << 0)
#define P4_USJ_IN_EP_DATA_FREE (1u << 1)
#define P4_USJ_OUT_EP_DATA_AVAIL (1u << 2)
#define P4_USJ_EP1_DEPTH 64u
#define P4_UART0_BASE 0x2000u
#define P4_UART_FIFO 0x0000u
#define P4_UART_STATUS 0x001cu
#define P4_UART_TXFIFO_CNT_S 16
#define P4_UART_TXFIFO_CNT_M 0xffu
#define P4_UART_RXFIFO_CNT_S 0
#define P4_UART_RXFIFO_CNT_M 0xffu
#define P4_UART_FIFO_DEPTH 128u

static unsigned int p4_test_hart;
static unsigned int p4_tls_mask_calls, p4_tls_unmask_calls;

static unsigned long p4_tls_mask(void);
static void p4_tls_unmask(unsigned long state);
static uint32_t mmio_rd(uint32_t base, uint32_t off);
static void mmio_wr(uint32_t base, uint32_t off, uint32_t value);
'''

MOCKS_AND_TESTS = r'''
#ifndef P4_CONSOLE_RECORD_PREFIX
#error The actual source default was not included in this host fixture
#endif
#if defined(P4_CONSOLE_TEST_DEFAULT) && P4_CONSOLE_RECORD_PREFIX != 0
#error P4_CONSOLE_RECORD_PREFIX must default to zero
#endif

#define MAX_USB_PACKETS 512u
#define MAX_USB_PACKET_BYTES 64u
#define MAX_CAPTURE_BYTES 32768u

static unsigned long p4_tls_mask(void)
{
    ++p4_tls_mask_calls;
    return 0x5a5au;
}

static void p4_tls_unmask(unsigned long state)
{
    assert(state == 0x5a5au);
    ++p4_tls_unmask_calls;
}

#if P4_CONSOLE_USB
static uint32_t usb_conf;
static unsigned char usb_rx_byte;
static unsigned int usb_rx_available;
static unsigned int usb_auto_release;
static unsigned char usb_current_packet[MAX_USB_PACKET_BYTES];
static size_t usb_current_length;
static unsigned char usb_packets[MAX_USB_PACKETS][MAX_USB_PACKET_BYTES];
static size_t usb_packet_lengths[MAX_USB_PACKETS];
static size_t usb_packet_count;
static size_t usb_endpoint_writes, usb_done_writes;
static size_t mmio_read_count, mmio_write_count;
static unsigned int usb_blocking_callback;

static uint32_t mmio_rd(uint32_t base, uint32_t off)
{
    assert(base == P4_USJ_BASE);
    ++mmio_read_count;
    if (off == P4_USJ_EP1_CONF)
        return usb_conf | (usb_rx_available ? P4_USJ_OUT_EP_DATA_AVAIL : 0u);
    if (off == P4_USJ_EP1)
        return usb_rx_byte;
    abort();
}

static void mmio_wr(uint32_t base, uint32_t off, uint32_t value)
{
    assert(base == P4_USJ_BASE);
    ++mmio_write_count;
    if (off == P4_USJ_EP1)
    {
        assert(usb_current_length < sizeof(usb_current_packet));
        usb_current_packet[usb_current_length++] = (unsigned char)value;
        ++usb_endpoint_writes;
        if (usb_blocking_callback)
        {
            usb_blocking_callback = 0;
            krnP4ConsoleBlocking();
        }
        return;
    }
    if (off == P4_USJ_EP1_CONF && value == P4_USJ_WR_DONE)
    {
        assert(usb_current_length > 0u);
        assert(usb_packet_count < MAX_USB_PACKETS);
        memcpy(usb_packets[usb_packet_count], usb_current_packet,
               usb_current_length);
        usb_packet_lengths[usb_packet_count] = usb_current_length;
        ++usb_packet_count;
        usb_current_length = 0;
        ++usb_done_writes;
        if (usb_auto_release)
            usb_conf |= P4_USJ_IN_EP_DATA_FREE;
        else
            usb_conf &= ~P4_USJ_IN_EP_DATA_FREE;
        return;
    }
    abort();
}
#else
static unsigned int uart_used, uart_rx_available;
static unsigned char uart_rx_byte;
static unsigned char uart_output[MAX_CAPTURE_BYTES];
static size_t uart_output_length, uart_fifo_writes;
static size_t mmio_read_count, mmio_write_count;

static uint32_t mmio_rd(uint32_t base, uint32_t off)
{
    assert(base == P4_UART0_BASE && off == P4_UART_STATUS);
    ++mmio_read_count;
    return ((uart_used & P4_UART_TXFIFO_CNT_M) << P4_UART_TXFIFO_CNT_S)
         | ((uart_rx_available ? 1u : 0u) << P4_UART_RXFIFO_CNT_S);
}

static void mmio_wr(uint32_t base, uint32_t off, uint32_t value)
{
    assert(base == P4_UART0_BASE && off == P4_UART_FIFO);
    assert(uart_output_length < sizeof(uart_output));
    ++mmio_write_count;
    ++uart_fifo_writes;
    uart_output[uart_output_length++] = (unsigned char)value;
}
#endif

static void reset_console(void)
{
    memset(console_line, 0, sizeof(console_line));
    memset(console_len, 0, sizeof(console_len));
    memset(&console_queue, 0, sizeof(console_queue));
    memset(console_frame, 0, sizeof(console_frame));
    console_frame_len = 0;
    console_frame_pos = 0;
    console_reported_drops = 0;
    console_last_overflow = 0;
    console_holder = 0;
    console_runtime = 0;
    console_dropped = 0;
    __atomic_store_n(&console_sender_ready, 0u, __ATOMIC_RELAXED);
#if P4_CONSOLE_USB
    usj_pending = 0;
    usj_wait_remaining = ATTACH_SPINS;
#endif
    p4_test_hart = 0;
    p4_tls_mask_calls = 0;
    p4_tls_unmask_calls = 0;
}

static void assert_tls_balanced(void)
{
    assert(p4_tls_mask_calls == p4_tls_unmask_calls);
}

static void sender_ready_runtime(void)
{
    krnP4ConsoleSenderReady();
    krnP4ConsoleRuntime();
    assert(__atomic_load_n(&console_sender_ready, __ATOMIC_ACQUIRE) == 1u);
    assert(__atomic_load_n(&console_runtime, __ATOMIC_ACQUIRE) == 1u);
}

#if P4_CONSOLE_USB
static void reset_mmio(void)
{
    usb_conf = P4_USJ_IN_EP_DATA_FREE;
    usb_rx_byte = 0;
    usb_rx_available = 0;
    usb_auto_release = 0;
    memset(usb_current_packet, 0, sizeof(usb_current_packet));
    memset(usb_packets, 0, sizeof(usb_packets));
    memset(usb_packet_lengths, 0, sizeof(usb_packet_lengths));
    usb_current_length = 0;
    usb_packet_count = 0;
    usb_endpoint_writes = 0;
    usb_done_writes = 0;
    mmio_read_count = 0;
    mmio_write_count = 0;
    usb_blocking_callback = 0;
}

static size_t collect_usb(unsigned char *output, size_t capacity)
{
    size_t i, length = 0;

    for (i = 0; i < usb_packet_count; ++i)
    {
        assert(usb_packet_lengths[i] <= MAX_USB_PACKET_BYTES);
        assert(length + usb_packet_lengths[i] <= capacity);
        memcpy(output + length, usb_packets[i], usb_packet_lengths[i]);
        length += usb_packet_lengths[i];
    }
    return length;
}

static size_t drain_usb_until_idle(void)
{
    unsigned int i;
    size_t total = 0;

    for (i = 0; i < 1024u; ++i)
    {
        unsigned int sent;
        usb_conf |= P4_USJ_IN_EP_DATA_FREE;
        sent = krnP4ConsoleDrain(256u);
        total += sent;
        if (sent == 0u)
            break;
    }
    assert(i < 1024u);
    assert(usb_current_length == 0u);
    return total;
}

static void assert_packets_for_frame(size_t frame_length)
{
    size_t i;
    size_t expected_count = (frame_length + MAX_USB_PACKET_BYTES - 1u)
                          / MAX_USB_PACKET_BYTES;

    assert(usb_packet_count == expected_count);
    for (i = 0; i < usb_packet_count; ++i)
    {
        size_t expected = frame_length - i * MAX_USB_PACKET_BYTES;
        if (expected > MAX_USB_PACKET_BYTES)
            expected = MAX_USB_PACKET_BYTES;
        assert(usb_packet_lengths[i] == expected);
    }
}

static void test_enqueue_blocked_retention_and_late_drain(void)
{
    unsigned char actual[MAX_CAPTURE_BYTES];
    const char *payload = "late USB record\n";
    char expected[256];
    size_t expected_length, actual_length;

    reset_console();
    reset_mmio();
    sender_ready_runtime();
    krnP4PutStr(payload);
    assert(console_queue.count == 1u);
    assert(mmio_write_count == 0u);

    usb_conf &= ~P4_USJ_IN_EP_DATA_FREE;
    assert(krnP4ConsoleDrain(256u) == 0u);
    assert(console_frame_pos == 0u && console_frame_len > 0u);
    assert(console_queue.count == 0u);
    assert(mmio_write_count == 0u);
    assert(krnP4ConsoleDrain(256u) == 0u);
    assert(console_frame_pos == 0u && console_frame_len > 0u);
    assert(usb_packet_count == 0u);

    expected_length = (size_t)snprintf(expected, sizeof(expected),
        "[p4log seq=00000000 hart=00000000 len=00000010] %s", payload);
    usb_conf |= P4_USJ_IN_EP_DATA_FREE;
    assert(drain_usb_until_idle() == expected_length);
    actual_length = collect_usb(actual, sizeof(actual));
    assert(actual_length == expected_length);
    assert(memcmp(actual, expected, expected_length) == 0);
    assert_packets_for_frame(expected_length);
    assert_tls_balanced();
}

static void test_usb_packet_pressure_exact_framing(void)
{
    char payload[P4_CONSOLE_RECORD_BYTES + 1u];
    char expected[256];
    unsigned char actual[MAX_CAPTURE_BYTES];
    size_t expected_length, actual_length, sent;
    unsigned int i;

    reset_console();
    reset_mmio();
    sender_ready_runtime();
    for (i = 0; i < P4_CONSOLE_RECORD_BYTES; ++i)
        payload[i] = (char)('A' + i % 26u);
    payload[P4_CONSOLE_RECORD_BYTES] = '\0';
    krnP4PutStr(payload);
    assert(console_queue.count == 1u);
    assert(mmio_write_count == 0u);

    expected_length = (size_t)snprintf(expected, sizeof(expected),
        "[p4log seq=00000000 hart=00000000 len=00000080] %s\n", payload);
    sent = krnP4ConsoleDrain(256u);
    assert(sent == MAX_USB_PACKET_BYTES);
    assert(usb_packet_count == 1u && usb_packet_lengths[0] == 64u);
    assert(console_frame_pos == 64u && console_frame_len == expected_length);
    assert((usb_conf & P4_USJ_IN_EP_DATA_FREE) == 0u);
    assert(krnP4ConsoleDrain(256u) == 0u);
    assert(usb_packet_count == 1u && console_frame_pos == 64u);

    usb_conf |= P4_USJ_IN_EP_DATA_FREE;
    assert(drain_usb_until_idle() == expected_length - 64u);
    actual_length = collect_usb(actual, sizeof(actual));
    assert(actual_length == expected_length);
    assert(memcmp(actual, expected, expected_length) == 0);
    assert_packets_for_frame(expected_length);
    assert(usb_endpoint_writes == expected_length);
    assert(usb_done_writes == usb_packet_count);
    assert(console_frame_pos == 0u && console_frame_len == 0u);
    assert_tls_balanced();
}

static void test_full_queue_overflow_is_reported_without_new_producer(void)
{
    char line[16];
    char expected[MAX_CAPTURE_BYTES];
    unsigned char actual[MAX_CAPTURE_BYTES];
    size_t expected_length = 0, actual_length;
    unsigned int i;

    reset_console();
    reset_mmio();
    sender_ready_runtime();
    for (i = 0; i < P4_CONSOLE_RING_SLOTS + 1u; ++i)
    {
        (void)snprintf(line, sizeof(line), "L%02u\n", i);
        krnP4PutStr(line);
    }
    assert(console_queue.count == P4_CONSOLE_RING_SLOTS);
    assert(p4_console_ring_dropped(&console_queue) == 1u);
    assert(mmio_write_count == 0u);

    expected_length += (size_t)snprintf(expected + expected_length,
        sizeof(expected) - expected_length,
        "[p4log overflow records=00000001]\n");
    for (i = 0; i < P4_CONSOLE_RING_SLOTS; ++i)
    {
        expected_length += (size_t)snprintf(expected + expected_length,
            sizeof(expected) - expected_length,
            "[p4log seq=%08x hart=00000000 len=00000004] L%02u\n",
            i, i);
    }

    /* No more records are produced after the overflow. The drain itself
       must notice the counter and emit the marker. */
    assert(drain_usb_until_idle() > 0u);
    actual_length = collect_usb(actual, sizeof(actual));
    assert(actual_length == expected_length);
    assert(memcmp(actual, expected, expected_length) == 0);
    actual[actual_length] = '\0';
    assert(strstr((const char *)actual, "[p4log overflow records=00000001]\n")
           != NULL);
    assert(console_reported_drops == 1u);
    assert(p4_console_ring_dropped(&console_queue) == 1u);
    assert_tls_balanced();
}

static void test_rx_prompt_is_queued_without_tx_write(void)
{
    const char *expected = "[p4log seq=00000000 hart=00000000 len=00000001] >\n";
    unsigned char actual[256];
    size_t actual_length;

    reset_console();
    reset_mmio();
    sender_ready_runtime();
    krnP4PutC('>');
    assert(mmio_write_count == 0u);

    usb_rx_byte = 'k';
    usb_rx_available = 1u;
    assert(krnP4GetC() == 'k');
    assert(mmio_write_count == 0u);
    assert(console_queue.count == 1u);
    assert(console_len[0] == 0u);

    assert(drain_usb_until_idle() == strlen(expected));
    actual_length = collect_usb(actual, sizeof(actual));
    assert(actual_length == strlen(expected));
    assert(memcmp(actual, expected, actual_length) == 0);
    assert_tls_balanced();
}

static void test_blocking_disables_drain_and_keeps_bounded_early_path(void)
{
    unsigned int reads_before;

    reset_console();
    reset_mmio();
    sender_ready_runtime();
    krnP4PutStr("retained\n");
    assert(console_queue.count == 1u && mmio_write_count == 0u);

    krnP4ConsoleBlocking();
    assert(__atomic_load_n(&console_runtime, __ATOMIC_ACQUIRE) == 0u);
    assert(krnP4ConsoleDrain(256u) == 0u);
    assert(console_queue.count == 1u && mmio_write_count == 0u);

    usb_conf &= ~P4_USJ_IN_EP_DATA_FREE;
    usj_wait_remaining = 3u;
    reads_before = (unsigned int)mmio_read_count;
    krnP4PutStr("B\n");
    assert(console_queue.count == 1u);
    assert(p4_console_ring_dropped(&console_queue) == 0u);
    assert(console_dropped == 2u);
    assert(usj_wait_remaining == 0u);
    assert(mmio_read_count - reads_before == 4u);
    assert(mmio_write_count == 0u);
    assert_tls_balanced();
}

static void test_usb_mode_change_during_write_never_waits(void)
{
    char payload[P4_CONSOLE_RECORD_BYTES + 1u];
    char expected[MAX_CAPTURE_BYTES];
    unsigned char actual[MAX_CAPTURE_BYTES];
    size_t expected_length = 0, actual_length;
    size_t reads_before;
    unsigned int i, sent;

    reset_console();
    reset_mmio();
    sender_ready_runtime();
    for (i = 0; i < P4_CONSOLE_RECORD_BYTES; ++i)
        payload[i] = 'M';
    payload[P4_CONSOLE_RECORD_BYTES] = '\0';
    krnP4PutStr(payload);
    krnP4PutStr("second\n");
    assert(console_queue.count == 2u);

    expected_length += (size_t)snprintf(expected + expected_length,
        sizeof(expected) - expected_length,
        "[p4log seq=00000000 hart=00000000 len=00000080] %s\n", payload);
    expected_length += (size_t)snprintf(expected + expected_length,
        sizeof(expected) - expected_length,
        "[p4log seq=00000001 hart=00000000 len=00000007] second\n");

    usb_blocking_callback = 1u;
    reads_before = mmio_read_count;
    sent = krnP4ConsoleDrain(256u);
    assert(sent == MAX_USB_PACKET_BYTES);
    assert(__atomic_load_n(&console_runtime, __ATOMIC_ACQUIRE) == 0u);
    assert(usb_blocking_callback == 0u);
    assert(console_queue.count == 1u);
    assert(console_frame_pos == MAX_USB_PACKET_BYTES);
    assert(console_frame_len == 177u);
    assert(usb_packet_count == 1u && usb_packet_lengths[0] == 64u);
    assert(mmio_read_count - reads_before == 65u);
    assert(usj_wait_remaining == ATTACH_SPINS);
    assert(console_holder == 0u);

    actual_length = collect_usb(actual, sizeof(actual));
    assert(actual_length == MAX_USB_PACKET_BYTES);
    assert(memcmp(actual, expected, actual_length) == 0);

    /* Resume the sender. The in-flight suffix and the untouched queued
       record must both emerge once, in order. */
    krnP4ConsoleRuntime();
    assert(drain_usb_until_idle() == expected_length - MAX_USB_PACKET_BYTES);
    actual_length = collect_usb(actual, sizeof(actual));
    assert(actual_length == expected_length);
    assert(memcmp(actual, expected, expected_length) == 0);
    assert(console_queue.count == 0u);
    assert(console_frame_pos == 0u && console_frame_len == 0u);
    assert_tls_balanced();
}

static void run_usb_tests(void)
{
    test_enqueue_blocked_retention_and_late_drain();
    test_usb_packet_pressure_exact_framing();
    test_full_queue_overflow_is_reported_without_new_producer();
    test_rx_prompt_is_queued_without_tx_write();
    test_blocking_disables_drain_and_keeps_bounded_early_path();
    test_usb_mode_change_during_write_never_waits();
}
#else
static void reset_mmio(void)
{
    uart_used = 0;
    uart_rx_available = 0;
    uart_rx_byte = 0;
    memset(uart_output, 0, sizeof(uart_output));
    uart_output_length = 0;
    uart_fifo_writes = 0;
    mmio_read_count = 0;
    mmio_write_count = 0;
}

static void test_uart_late_drain(void)
{
    const char *payload = "UART queued\n";
    const char *expected = "[p4log seq=00000000 hart=00000000 len=0000000c] UART queued\n";
    size_t expected_length = strlen(expected);

    reset_console();
    reset_mmio();
    sender_ready_runtime();
    krnP4PutStr(payload);
    assert(console_queue.count == 1u);
    assert(mmio_write_count == 0u);

    assert(krnP4ConsoleDrain(256u) == expected_length);
    assert(uart_output_length == expected_length);
    assert(memcmp(uart_output, expected, expected_length) == 0);
    assert(uart_fifo_writes == expected_length);
    assert(console_queue.count == 0u);
    assert_tls_balanced();
}

static void run_uart_tests(void)
{
    test_uart_late_drain();
}
#endif

static size_t drain_and_collect(unsigned char *output, size_t capacity)
{
#if P4_CONSOLE_USB
    (void)drain_usb_until_idle();
    return collect_usb(output, capacity);
#else
    unsigned int i;
    unsigned int sent;

    for (i = 0; i < 1024u; ++i)
    {
        sent = krnP4ConsoleDrain(256u);
        if (sent == 0u)
            break;
    }
    assert(i < 1024u);
    assert(uart_output_length <= capacity);
    memcpy(output, uart_output, uart_output_length);
    return uart_output_length;
#endif
}

static void test_unprefixed_short_prompt(void)
{
    const char *payload = "ready>";
    unsigned char actual[256];
    size_t actual_length;

    reset_console();
    reset_mmio();
    sender_ready_runtime();
    krnP4PutStr(payload);
    assert(console_queue.count == 0u && console_len[0] == strlen(payload));
    assert(krnP4GetC() == -1);
    assert(console_queue.count == 1u && console_len[0] == 0u);
    assert(mmio_write_count == 0u);

    actual_length = drain_and_collect(actual, sizeof(actual));
    assert(actual_length == strlen(payload));
    assert(memcmp(actual, payload, actual_length) == 0);
    assert_tls_balanced();
}

static void test_unprefixed_chunked_record(void)
{
    char payload[2u * P4_CONSOLE_RECORD_BYTES + 2u];
    unsigned char actual[MAX_CAPTURE_BYTES];
    size_t actual_length;
    unsigned int i;

    reset_console();
    reset_mmio();
    sender_ready_runtime();
    for (i = 0; i < 2u * P4_CONSOLE_RECORD_BYTES; ++i)
        payload[i] = (char)('a' + i % 26u);
    payload[2u * P4_CONSOLE_RECORD_BYTES] = '\n';
    payload[2u * P4_CONSOLE_RECORD_BYTES + 1u] = '\0';
    krnP4PutStr(payload);
    assert(console_queue.count == 3u);
    assert(mmio_write_count == 0u);

    actual_length = drain_and_collect(actual, sizeof(actual));
    assert(actual_length == strlen(payload));
    assert(memcmp(actual, payload, actual_length) == 0);
    assert(console_queue.count == 0u);
    assert_tls_balanced();
}

static void test_unprefixed_record_resumes_without_added_bytes(void)
{
    char payload[P4_CONSOLE_RECORD_BYTES + 1u];
    unsigned char actual[MAX_CAPTURE_BYTES];
    size_t actual_length;
    unsigned int i;

    reset_console();
    reset_mmio();
    sender_ready_runtime();
    for (i = 0; i < P4_CONSOLE_RECORD_BYTES; ++i)
        payload[i] = 'R';
    payload[P4_CONSOLE_RECORD_BYTES] = '\0';
    krnP4PutStr(payload);
    assert(console_queue.count == 1u);

#if P4_CONSOLE_USB
    assert(krnP4ConsoleDrain(256u) == MAX_USB_PACKET_BYTES);
    assert(console_frame_pos == MAX_USB_PACKET_BYTES);
    assert(console_frame_len == P4_CONSOLE_RECORD_BYTES);
    assert(usb_packet_count == 1u && usb_packet_lengths[0] == 64u);
    assert(krnP4ConsoleDrain(256u) == 0u);
    assert(console_frame_pos == MAX_USB_PACKET_BYTES);
    usb_conf |= P4_USJ_IN_EP_DATA_FREE;
    assert(drain_usb_until_idle() == P4_CONSOLE_RECORD_BYTES - 64u);
#else
    assert(krnP4ConsoleDrain(64u) == 64u);
    assert(console_frame_pos == 64u);
    assert(console_frame_len == P4_CONSOLE_RECORD_BYTES);
    assert(krnP4ConsoleDrain(64u) == 64u);
#endif

    actual_length = drain_and_collect(actual, sizeof(actual));
    assert(actual_length == P4_CONSOLE_RECORD_BYTES);
    assert(memcmp(actual, payload, actual_length) == 0);
    assert(console_queue.count == 0u);
    assert(console_frame_pos == 0u && console_frame_len == 0u);
    assert_tls_balanced();
}

static void test_overflow_marker_stays_unconditional(void)
{
    char line[16];
    char expected[MAX_CAPTURE_BYTES];
    unsigned char actual[MAX_CAPTURE_BYTES];
    size_t expected_length = 0, actual_length;
    unsigned int i;

    reset_console();
    reset_mmio();
    sender_ready_runtime();
    for (i = 0; i < P4_CONSOLE_RING_SLOTS + 1u; ++i)
    {
        (void)snprintf(line, sizeof(line), "L%02u\n", i);
        krnP4PutStr(line);
    }
    assert(console_queue.count == P4_CONSOLE_RING_SLOTS);
    assert(p4_console_ring_dropped(&console_queue) == 1u);

    expected_length += (size_t)snprintf(expected + expected_length,
        sizeof(expected) - expected_length,
        "[p4log overflow records=00000001]\n");
    for (i = 0; i < P4_CONSOLE_RING_SLOTS; ++i)
    {
        expected_length += (size_t)snprintf(expected + expected_length,
            sizeof(expected) - expected_length, "L%02u\n", i);
    }

    actual_length = drain_and_collect(actual, sizeof(actual));
    assert(actual_length == expected_length);
    assert(memcmp(actual, expected, expected_length) == 0);
    assert(actual_length >= sizeof("[p4log overflow records=00000001]\n") - 1u);
    assert(memcmp(actual, "[p4log overflow records=00000001]\n",
                  sizeof("[p4log overflow records=00000001]\n") - 1u) == 0);
    assert(console_reported_drops == 1u);
    assert(p4_console_ring_dropped(&console_queue) == 1u);
    assert_tls_balanced();
}

static void run_unprefixed_tests(void)
{
    test_unprefixed_short_prompt();
    test_unprefixed_chunked_record();
    test_unprefixed_record_resumes_without_added_bytes();
    test_overflow_marker_stays_unconditional();
}

static void test_drain_lock_contention(void)
{
    const char *expected = "[p4log seq=00000000 hart=00000000 len=00000007] locked\n";
    unsigned int reads_before;

    reset_console();
    reset_mmio();
    sender_ready_runtime();
    krnP4PutStr("locked\n");
    assert(console_queue.count == 1u);

    __atomic_store_n(&console_holder, 2u, __ATOMIC_RELEASE);
    reads_before = (unsigned int)mmio_read_count;
    assert(krnP4ConsoleDrain(256u) == 0u);
    assert(console_queue.count == 1u);
    assert(console_frame_pos == 0u && console_frame_len == 0u);
    assert(mmio_read_count == reads_before && mmio_write_count == 0u);
    assert(__atomic_load_n(&console_holder, __ATOMIC_ACQUIRE) == 2u);
    __atomic_store_n(&console_holder, 0u, __ATOMIC_RELEASE);

#if P4_CONSOLE_USB
    {
        unsigned char actual[256];
        size_t actual_length;
        usb_auto_release = 1u;
        assert(drain_usb_until_idle() == strlen(expected));
        actual_length = collect_usb(actual, sizeof(actual));
        assert(actual_length == strlen(expected));
        assert(memcmp(actual, expected, actual_length) == 0);
    }
#else
    assert(krnP4ConsoleDrain(256u) == strlen(expected));
    assert(uart_output_length == strlen(expected));
    assert(memcmp(uart_output, expected, uart_output_length) == 0);
#endif
    assert(console_queue.count == 0u);
    assert_tls_balanced();
}

static void test_sender_failure_uses_bounded_direct_fallback(void)
{
    unsigned int reads_before;

    reset_console();
    reset_mmio();
    sender_ready_runtime();
    krnP4PutStr("already queued\n");
    assert(console_queue.count == 1u);
    krnP4ConsoleSenderFailed();
    assert(__atomic_load_n(&console_sender_ready, __ATOMIC_ACQUIRE) == 0u);

    reads_before = (unsigned int)mmio_read_count;
    assert(krnP4ConsoleDrain(256u) == 0u);
    assert(console_queue.count == 1u);
    assert(mmio_read_count == reads_before && mmio_write_count == 0u);

#if P4_CONSOLE_USB
    usb_conf &= ~P4_USJ_IN_EP_DATA_FREE;
#else
    uart_used = P4_UART_FIFO_DEPTH;
#endif
    reads_before = (unsigned int)mmio_read_count;
    krnP4PutStr("F\n");
    assert(mmio_read_count - reads_before == 2u);
    assert(mmio_write_count == 0u);
    assert(console_dropped == 2u);
    assert(p4_console_ring_dropped(&console_queue) == 0u);
    assert(console_queue.count == 1u);
    assert(console_len[0] == 0u);
    assert_tls_balanced();
}

static void test_caller_budget_is_capped_at_256(void)
{
    char payload[P4_CONSOLE_RECORD_BYTES + 1u];
    char expected[MAX_CAPTURE_BYTES];
    unsigned char actual[MAX_CAPTURE_BYTES];
    size_t expected_length = 0, actual_length;
    unsigned int i, sent;

    reset_console();
    reset_mmio();
    sender_ready_runtime();
    for (i = 0; i < P4_CONSOLE_RECORD_BYTES; ++i)
        payload[i] = 'Q';
    payload[P4_CONSOLE_RECORD_BYTES] = '\0';
    krnP4PutStr(payload);
    krnP4PutStr(payload);
    assert(console_queue.count == 2u);

    expected_length += (size_t)snprintf(expected + expected_length,
        sizeof(expected) - expected_length,
        "[p4log seq=00000000 hart=00000000 len=00000080] %s\n", payload);
    expected_length += (size_t)snprintf(expected + expected_length,
        sizeof(expected) - expected_length,
        "[p4log seq=00000001 hart=00000000 len=00000080] %s\n", payload);
    assert(expected_length > 256u);

#if P4_CONSOLE_USB
    usb_auto_release = 1u;
#endif
    sent = krnP4ConsoleDrain(1000u);
    assert(sent == 256u);
    assert(console_queue.count == 0u);
#if P4_CONSOLE_USB
    actual_length = collect_usb(actual, sizeof(actual));
#else
    actual_length = uart_output_length;
    memcpy(actual, uart_output, actual_length);
#endif
    assert(actual_length == 256u);
    assert(memcmp(actual, expected, actual_length) == 0);

    sent = krnP4ConsoleDrain(1000u);
    assert(sent == expected_length - 256u);
#if P4_CONSOLE_USB
    actual_length = collect_usb(actual, sizeof(actual));
#else
    actual_length = uart_output_length;
    memcpy(actual, uart_output, actual_length);
#endif
    assert(actual_length == expected_length);
    assert(memcmp(actual, expected, expected_length) == 0);
    assert(console_frame_pos == 0u && console_frame_len == 0u);
    assert_tls_balanced();
}

int main(void)
{
    if (P4_CONSOLE_RECORD_PREFIX)
    {
#if P4_CONSOLE_USB
        run_usb_tests();
#else
        run_uart_tests();
#endif
        test_drain_lock_contention();
        test_sender_failure_uses_bounded_direct_fallback();
        test_caller_budget_is_capped_at_256();
    }
    else
        run_unprefixed_tests();

#if P4_CONSOLE_USB
    puts(P4_CONSOLE_RECORD_PREFIX
         ? "USB framed console queue integration passed"
         : "USB unprefixed console queue integration passed");
#else
    puts(P4_CONSOLE_RECORD_PREFIX
         ? "UART framed console queue integration passed"
         : "UART unprefixed console queue integration passed");
#endif
    return 0;
}
'''


def compile_and_run(tmpdir, backend, optimization, prefix_mode):
    mode_name = "default" if prefix_mode is None else str(prefix_mode)
    source = pathlib.Path(tmpdir) / (
        f"console_queue_{backend}_o{optimization}_prefix{mode_name}.c"
    )
    executable = pathlib.Path(tmpdir) / (
        f"console_queue_{backend}_o{optimization}_prefix{mode_name}"
    )
    source.write_text(PREAMBLE + CONSOLE_SOURCE + MOCKS_AND_TESTS)
    command = [
        *CC,
        "-std=c11",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-Wpedantic",
        "-O" + str(optimization),
        "-fno-omit-frame-pointer",
        "-fsanitize=address,undefined",
        "-DP4_CONSOLE_USB=" + ("1" if backend == "usb" else "0"),
        "-I",
        str(KERNEL),
        str(source),
        "-o",
        str(executable),
    ]
    if prefix_mode is not None:
        command.insert(command.index("-I"),
                       "-DP4_CONSOLE_RECORD_PREFIX=" + str(prefix_mode))
    else:
        command.insert(command.index("-I"), "-DP4_CONSOLE_TEST_DEFAULT=1")
    if backend == "uart":
        # The normal source keeps console_flush_self in this build even
        # though krnP4GetC only uses it in the USB backend.
        command.insert(command.index("-O" + str(optimization)),
                       "-Wno-unused-function")
    subprocess.run(command, check=True)
    subprocess.run([str(executable)], check=True)


def main():
    with tempfile.TemporaryDirectory(prefix="p4-console-queue-") as tmpdir:
        for prefix_mode in (None, 0, 1):
            for backend in ("usb", "uart"):
                for optimization in (0, 2):
                    compile_and_run(tmpdir, backend, optimization, prefix_mode)


if __name__ == "__main__":
    main()
