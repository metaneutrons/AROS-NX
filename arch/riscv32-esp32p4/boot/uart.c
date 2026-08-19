/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: ESP32-P4 UART0 + USB-Serial/JTAG early debug console driver.
          Mirrors debug output to both physical UART0 and built-in USB-C CDC-ACM.
*/

#include "uart.h"

void uart_init(ULONG baudrate)
{
    /* UART0 and USB-Serial/JTAG are pre-initialized by the ESP-IDF 2nd stage bootloader */
    (void)baudrate;
}

void usj_putc(char c)
{
    /* Check if USB-Serial/JTAG FIFO has space */
    if (*USJ_EP1_CONF_REG(ESP32P4_USB_SERIAL_JTAG_BASE) & USJ_EP1_DATA_FREE)
    {
        *USJ_EP1_REG(ESP32P4_USB_SERIAL_JTAG_BASE) = (ULONG)(UBYTE)c;
    }
}

void usj_flush(void)
{
    *USJ_EP1_CONF_REG(ESP32P4_USB_SERIAL_JTAG_BASE) |= USJ_EP1_WR_DONE;
}

void uart_putc(char c)
{
    if (c == '\n')
    {
        uart_putc('\r');
        usj_putc('\r');
    }

    /* Output to physical UART0 (wait until TX FIFO has room) */
    while (((*UART_STATUS_REG(ESP32P4_UART0_BASE) >> 16) & 0xFF) >= 127)
        ;

    *UART_FIFO_REG(ESP32P4_UART0_BASE) = (ULONG)(UBYTE)c;

    /* Output to USB-Serial/JTAG (USB-C port on Mac) */
    usj_putc(c);

    if (c == '\n')
        usj_flush();
}

void uart_puts(const char *s)
{
    if (!s)
        return;
    while (*s)
        uart_putc(*s++);
}

void uart_puthex(ULONG val)
{
    static const char hex[] = "0123456789ABCDEF";
    uart_puts("0x");
    for (int i = 28; i >= 0; i -= 4) {
        uart_putc(hex[(val >> i) & 0x0F]);
    }
}
