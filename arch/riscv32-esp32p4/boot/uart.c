/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: ESP32-P4 UART0 early debug console driver.
*/

#include "uart.h"

void uart_init(ULONG baudrate)
{
    /* UART0 is initialized to 115200 baud by the ESP-IDF 2nd stage bootloader */
    (void)baudrate;
}

void uart_putc(char c)
{
    if (c == '\n')
        uart_putc('\r');

    /* Wait until FIFO is not full (Status bit 16:23 = TX FIFO count < 128) */
    while (((*UART_STATUS_REG(ESP32P4_UART0_BASE) >> 16) & 0xFF) >= 127)
        ;

    *UART_FIFO_REG(ESP32P4_UART0_BASE) = (ULONG)(UBYTE)c;
}

void uart_puts(const char *s)
{
    if (!s)
        return;
    while (*s)
        uart_putc(*s++);
}
