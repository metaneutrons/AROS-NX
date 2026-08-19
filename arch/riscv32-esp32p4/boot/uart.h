/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: ESP32-P4 UART0 early console definitions.
*/

#ifndef ESP32P4_UART_H
#define ESP32P4_UART_H

#include "types.h"

/* ESP32-P4 Hardware UART0 */
#define ESP32P4_UART0_BASE              0x500C0000UL
#define UART_FIFO_REG(base)             ((volatile ULONG *)((base) + 0x00))
#define UART_STATUS_REG(base)           ((volatile ULONG *)((base) + 0x1C))

/* ESP32-P4 Hardware USB-Serial/JTAG Controller (Built-in USB-C CDC-ACM) */
#define ESP32P4_USB_SERIAL_JTAG_BASE    0x5009B000UL
#define USJ_EP1_REG(base)               ((volatile ULONG *)((base) + 0x00))
#define USJ_EP1_CONF_REG(base)          ((volatile ULONG *)((base) + 0x04))
#define USJ_EP1_DATA_FREE               (1 << 1)
#define USJ_EP1_WR_DONE                 (1 << 0)

void uart_init(ULONG baudrate);
void uart_putc(char c);
void uart_puts(const char *s);
void usj_putc(char c);
void usj_flush(void);

#endif /* ESP32P4_UART_H */
