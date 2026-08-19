/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: ESP32-P4 UART0 early console definitions.
*/

#ifndef ESP32P4_UART_H
#define ESP32P4_UART_H

#include <exec/types.h>

#define ESP32P4_UART0_BASE      0x500C0000UL
#define UART_FIFO_REG(base)     ((volatile ULONG *)((base) + 0x00))
#define UART_STATUS_REG(base)   ((volatile ULONG *)((base) + 0x1C))

void uart_init(ULONG baudrate);
void uart_putc(char c);
void uart_puts(const char *s);

#endif /* ESP32P4_UART_H */
