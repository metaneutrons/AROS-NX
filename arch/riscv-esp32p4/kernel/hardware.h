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
 * The system timer, and the matrix that carries its interrupt to a core.
 *
 * There is no mtime/mtimecmp on this chip, so the periodic tick has to
 * come from a peripheral. SYSTIMER is a 52-bit up counter with three
 * comparators; comparator 0 in period mode reloads itself, which is
 * exactly a tick and needs no arithmetic per interrupt.
 *
 * Its clock is the crystal through a fixed divider of 2.5. The crystal on
 * this board is 40 MHz, so the counter advances at 16 MHz and one tick of
 * 100 Hz is 160000 counts. The counter is also the only clock available
 * for measuring anything at this stage.
 *
 * The interrupt matrix maps a peripheral source to a core interrupt line:
 * one 6-bit register per source at base + source*4, holding the line
 * number. Line numbers there are 0 to 31 and appear on the CLIC 16 higher,
 * after the sixteen lines the core keeps for itself.
 */
#define P4_SYSTIMER_BASE        (P4_HPPERIPH1_BASE + 0x22000)
#define P4_ST_CONF              0x0000
#define  P4_ST_CLK_EN           (1U << 31)
#define  P4_ST_UNIT0_WORK_EN    (1U << 30)
#define  P4_ST_TARGET0_WORK_EN  (1U << 24)
#define P4_ST_UNIT0_OP          0x0004
#define  P4_ST_UNIT0_UPDATE     (1U << 30)
#define  P4_ST_UNIT0_VALID      (1U << 29)
#define P4_ST_TARGET0_CONF      0x0034
#define  P4_ST_TARGET0_PERIOD_M 0x03FFFFFFU
#define  P4_ST_TARGET0_PERIODIC (1U << 30)
#define P4_ST_UNIT0_VALUE_HI    0x0040
#define P4_ST_UNIT0_VALUE_LO    0x0044
#define P4_ST_COMP0_LOAD        0x0050
#define  P4_ST_COMP0_LOAD_BIT   (1U << 0)
#define P4_ST_INT_ENA           0x0064
#define P4_ST_INT_CLR           0x006C
#define  P4_ST_TARGET0_INT      (1U << 0)

#define P4_SYSTIMER_HZ          16000000UL
#define P4_TICK_HZ              100
#define P4_TICK_PERIOD          (P4_SYSTIMER_HZ / P4_TICK_HZ)

#define P4_INTMTX_CORE0_BASE    (P4_HPPERIPH1_BASE + 0x16000)
#define P4_INTMTX_MAP(source)   (P4_INTMTX_CORE0_BASE + (source) * 4)
#define P4_SOURCE_SYSTIMER_T0   53

/*
 * Which line the tick is routed to. The number written into the matrix
 * map register is the CLIC line index itself, not an external-interrupt
 * index that the controller then offsets by sixteen - established by
 * enabling both candidates and seeing which one the hardware raised.
 * Anything from P4_CLIC_EXT_OFFSET up is available.
 */
#define P4_TIMER_LINE           20

/*
 * The core local interrupt controller. Not a PLIC: this is a CLIC, and on
 * silicon before revision 3 it is a non-standard one - the threshold
 * lives in a memory mapped register rather than in the mintthresh CSR,
 * and mintstatus sits at CSR 0x346 instead of 0xFB1. ESP-IDF selects
 * between the two on the same revision switch that decides the SRAM
 * layout, so one boundary governs both.
 *
 * Each interrupt has one 32-bit control word: pending at bit 0, enable at
 * bit 8, hardware vectoring at bit 16, trigger at bits 18:17 and the
 * level at bits 31:24. With NLBITS at 3 only the top three of those eight
 * level bits are compared against the threshold.
 *
 * The register block is per core, the other core's copy sitting one
 * DUALCORE offset further on. Only core 0 runs so far.
 */
#define P4_CLIC_BASE            0x20800000UL
#define P4_CLIC_CTRL_BASE       0x20801000UL
#define P4_CLIC_DUALCORE_OFF    0x10000

#define P4_CLIC_INT_CONFIG      0x0000
#define  P4_CLIC_NLBITS_S       1
#define  P4_CLIC_NLBITS_M       0xF
#define P4_CLIC_INT_THRESH      0x0008
#define  P4_CLIC_THRESH_S       24

#define P4_CLIC_CTRL(i)         (P4_CLIC_CTRL_BASE + (i) * 4)
#define  P4_CLIC_INT_IP         (1U << 0)
#define  P4_CLIC_INT_IE         (1U << 8)
#define  P4_CLIC_INT_SHV        (1U << 16)
#define  P4_CLIC_INT_TRIG_S     17
#define  P4_CLIC_INT_TRIG_LEVEL 0
#define  P4_CLIC_INT_TRIG_EDGE  1
#define  P4_CLIC_INT_CTL_S      24

/* How many level bits are compared, and the level everything is given */
#define P4_CLIC_NLBITS          3
#define P4_CLIC_LEVEL_DEFAULT   1

/* The first sixteen lines are internal to the core; peripherals start here */
#define P4_CLIC_EXT_OFFSET      16
#define P4_CLIC_LINES           48

/*
 * Watchdogs. The first stage ROM loader arms these in "flashboot" mode
 * before it enters the image, so that something which never finishes
 * booting gets reset rather than hanging. Software is expected to turn
 * that off once it has booted - the note in ESP-IDF's lpwdt_ll.h says as
 * much - and until it does, a kernel that settles into wfi is restarted
 * about once a second.
 *
 * Two of the three are timer group main watchdogs, one per group; the
 * reset this board was observed taking, cause 0x07, is CORE_MWDT, so at
 * least one of them is armed. The third is in the always-on low power
 * domain, together with the super watchdog, which cannot be turned off
 * and is instead told to feed itself.
 *
 * All four registers are write protected by the same key.
 */
#define P4_TIMG0_BASE           (P4_HPPERIPH1_BASE + 0x2000)
#define P4_TIMG1_BASE           (P4_HPPERIPH1_BASE + 0x3000)
#define P4_TIMG_WDTCONFIG0      0x0048
#define  P4_TIMG_WDT_EN         (1U << 31)
#define  P4_TIMG_WDT_FLASHBOOT  (1U << 14)
#define P4_TIMG_WDTWPROTECT     0x0064

#define P4_LPAON_BASE           0x50110000UL
#define P4_LPWDT_BASE           (P4_LPAON_BASE + 0x6000)
#define P4_LPWDT_CONFIG0        0x0000
#define  P4_LPWDT_EN            (1U << 31)
#define  P4_LPWDT_FLASHBOOT     (1U << 12)
#define P4_LPWDT_WPROTECT       0x0018
#define P4_LPWDT_SWD_CONFIG     0x001C
#define  P4_LPWDT_SWD_AUTO_FEED (1U << 18)
#define P4_LPWDT_SWD_WPROTECT   0x0020

#define P4_WDT_WKEY             0x50D83AA1UL

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
