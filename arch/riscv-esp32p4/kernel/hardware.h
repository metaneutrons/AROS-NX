/*
    Copyright (c) 2026, The AROS Development Team. All rights reserved.
    $Id$

    Desc: ESP32-P4 register blocks used by kernel.resource.

    Addresses are those of ESP-IDF's components/soc/esp32p4; the ones
    named here are identical in the hw_ver1 and hw_ver3 register trees.
    Kept private to the kernel until a second subsystem needs them, at
    which point they move to an exported include.
*/

#ifndef ESP32P4_HARDWARE_H
#define ESP32P4_HARDWARE_H

/* High-power peripheral group 1 */
#define P4_HPPERIPH1_BASE       0x500C0000UL

#define P4_UART0_BASE           (P4_HPPERIPH1_BASE + 0xA000)

/*
 * UART. The transmit FIFO is 128 bytes deep and its fill level is the
 * only thing the console needs: the first stage loader leaves UART0
 * configured and running at the rate it printed its own messages with,
 * so nothing here reprograms the divisor or the pin matrix.
 */
#define P4_UART_FIFO            0x0000  /* write pushes one byte        */
#define P4_UART_STATUS          0x001C
#define  P4_UART_TXFIFO_CNT_S   16
#define  P4_UART_TXFIFO_CNT_M   0xFF
#define P4_UART_FIFO_DEPTH      128

#define P4_USJ_BASE             (P4_HPPERIPH1_BASE + 0x12000)

/*
 * USB serial/JTAG. This is the peripheral behind the board's USB-C
 * socket: the ESP32-P4 enumerates as Espressif "USB JTAG_serial debug
 * unit", 303a:1001, and a host sees a CDC device without any bridge chip
 * in between. Output goes into the 64 byte IN endpoint buffer and is
 * handed to the host by writing WR_DONE; DATA_FREE reads back as zero
 * from then until the host has collected it, which is also how an
 * unattached host is recognised.
 */
#define P4_USJ_EP1              0x0000  /* write pushes one byte        */
#define P4_USJ_EP1_CONF         0x0004
#define  P4_USJ_WR_DONE         (1 << 0)
#define  P4_USJ_IN_EP_DATA_FREE (1 << 1)
#define P4_USJ_EP1_DEPTH        64

/*
 * Which of the two the kernel debug output goes to. The USB peripheral
 * is the default because it is what the reTerminal D1001 brings out;
 * UART0 needs an adapter on the board's pins, and is selected by
 * defining this to 0.
 */
#ifndef P4_CONSOLE_USB
#define P4_CONSOLE_USB          1
#endif

/*
 * Internal SRAM, as the address map sees it. How much of this the kernel
 * may actually use is smaller and not a constant: the L2 cache is carved
 * out of the low end (128, 256 or 512 KB, set at startup), and on
 * silicon older than revision 3 the remainder is split into two disjoint
 * windows. See the memory milestone in the port README.
 */
#define P4_SRAM_BASE            0x4FF00000UL
#define P4_SRAM_END             0x4FFC0000UL

/* External PSRAM window. 64 MB of address space; how much answers
   depends on what the board populated and on MSPI bring-up. */
#define P4_PSRAM_BASE           0x48000000UL
#define P4_PSRAM_END            0x4C000000UL

#endif /* ESP32P4_HARDWARE_H */
