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

#include "../board/board.h"

/*
 * The complete exact-reference AROS trial.  It reproduces the combined state
 * in one name so a later diagnostic cannot accidentally omit one part of the
 * measured lifecycle.  It is not a working profile: repeated D1001 runs are
 * technically active but visually black.
 *
 * P4_B5_FULL_EXACT_TRIAL deliberately overrides the visual-gate helper's
 * historical non-burst selection.  It otherwise refuses a conflicting lane
 * rate instead of silently changing an experiment.
 */
#ifdef P4_B5_FULL_EXACT_TRIAL
# ifdef P4_DSI_LANE_MBPS
#  if P4_DSI_LANE_MBPS != 1000
#   error P4_B5_FULL_EXACT_TRIAL requires 1000-Mbit/s DSI lanes
#  endif
# else
#  define P4_DSI_LANE_MBPS       1000
# endif
# ifdef P4_DSI_NONBURST
#  undef P4_DSI_NONBURST
# endif
# define P4_DSI_FRAME_ACK
# define P4_B5_EXACT_PHY_CREATE
# define P4_B5_EXACT_BUS_CREATE
# define P4_B5_EXACT_DBI_CREATE
# define P4_B5_EARLY_DPI_CREATE
# define P4_B5_EARLY_GDMA_CREATE
# define P4_B5_EXACT_DPI_CREATE
# define P4_B5_SPLIT_AUTO_START
# define P4_B5_FULL_ATOMIC_START
# define P4_B5_ATOMIC_START
# define P4_B5_REF_BRG_IRQ
#endif

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
/* Same register, bits 7:0 - uart_struct.h calls it rxfifo_cnt, "Stores the
   byte number of valid data in Rx-FIFO". */
#define  P4_UART_RXFIFO_CNT_S   0
#define  P4_UART_RXFIFO_CNT_M   0xFF
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
/* components/soc/esp32p4/register/hw_ver1/soc/usb_serial_jtag_struct.h:
   serial_out_ep_data_avail, "1'b1: Indicate there is data in UART Rx FIFO".
   Reading P4_USJ_EP1 pops one byte from that FIFO. */
#define  P4_USJ_OUT_EP_DATA_AVAIL (1 << 2)
#define P4_USJ_EP1_DEPTH        64

/*
 * Which of the two the kernel debug output goes to. The board profile names
 * the USB peripheral when its board brings it out (both supported boards
 * do); UART0 needs an adapter on the board's pins, and is selected by
 * defining this to 0.
 */
#ifndef P4_CONSOLE_USB
#define P4_CONSOLE_USB          P4_BOARD_CONSOLE_USB
#endif

/*
 * Where the heap may go, which is not the same question as where the
 * image may be linked.
 *
 * The link ceiling exists because of who loads the image: the first stage
 * ROM loader has its own stack somewhere above, and a second stage
 * bootloader would be sitting at 0x4FF29ED0. Once this kernel is running
 * neither is true any more - the loader is finished and the stack is ours
 * - so at runtime the low window reaches much further up, stopping below
 * the variables the ROM keeps for itself from about 0x4FF3FFC8. Leaving
 * those alone costs nothing and keeps the ROM's own routines usable.
 *
 * The high window at 0x4FF40000 is whatever the L2 cache has not taken.
 * The cache is carved from the top of SRAM on this silicon and its size
 * is set by software - not by us, so far - which is why it is read from
 * the controller rather than assumed.
 */
#define P4_HEAP_LOW_END         0x4FF3FFC0UL
#define P4_HEAP_HIGH_BASE       0x4FF40000UL
#define P4_HEAP_HIGH_SPAN       0x80000UL

#define P4_CACHE_BASE           0x3FF10000UL
#define P4_L2_CACHESIZE_CONF    0x0278
#define  P4_L2_CACHESIZE_256    (1U << 0)
#define  P4_L2_CACHESIZE_512    (1U << 1)

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
#define P4_SOURCE_DW_GDMA       24

/*
 * Which line the tick is routed to. The number written into the matrix
 * map register is the CLIC line index itself, not an external-interrupt
 * index that the controller then offsets by sixteen - established by
 * enabling both candidates and seeing which one the hardware raised.
 * Anything from P4_CLIC_EXT_OFFSET up is available.
 */
#define P4_TIMER_LINE           20
#define P4_DSI_DMA_LINE         21

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
/* The rest of what arming one takes (ESP-IDF's timer_group_reg.h) */
#define  P4_TIMG_WDT_CONF_UPDATE (1U << 22)     /* write-only trigger */
#define  P4_TIMG_WDT_STG0(a)    ((uint32_t)(a) << 29)   /* 2 bits per stage */
#define  P4_TIMG_WDT_SYS_RST_LEN(n) ((uint32_t)(n) << 15)
#define  P4_TIMG_WDT_CPU_RST_LEN(n) ((uint32_t)(n) << 18)
#define  P4_TIMG_WDT_ACT_RESET_SYSTEM 3
#define P4_TIMG_WDTCONFIG1      0x004C          /* prescaler in bits 31:16 */
#define P4_TIMG_WDTCONFIG2      0x0050          /* stage 0 hold */
#define P4_TIMG_WDTFEED         0x0060

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
 * The ROM's reset-reason function, rtc_get_reset_reason(cpu): the same
 * answer the ROM prints in its banner, "rst:0x7 (HP_SYS_HP_WDT_RESET)".
 * Address from ESP-IDF's esp32p4.rom.ld.
 */
#define P4_ROM_GET_RESET_REASON 0x4FC00018UL

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

/*
 * Native SD/MMC host and the reTerminal D1001 socket.
 *
 * The ESP32-P4 contains a Synopsys DesignWare MMC host in high-power
 * peripheral group 0.  Slot 0 has dedicated IOMUX pins: D0..D3 on
 * GPIO39..42, CLK on GPIO43 and CMD on GPIO44.  The D1001 uses what would
 * otherwise be D4/D5 as a mechanical card-detect input and an external
 * power-enable output, so this board must never select the host's 8-bit
 * mode.
 */
#define P4_HPPERIPH0_BASE       0x50000000UL
#define P4_SDMMC_BASE           (P4_HPPERIPH0_BASE + 0x83000)

#define P4_GPIO_BASE            (P4_HPPERIPH1_BASE + 0x20000)
/* The low bank, pins 0..31.  The SD host needed only the high bank, so these
   arrived with the first pin below 32 this port had to drive. */
#define P4_GPIO_OUT             0x0004
#define P4_GPIO_OUT_W1TS        0x0008
#define P4_GPIO_OUT_W1TC       0x000C
#define P4_GPIO_ENABLE_W1TS     0x0024
#define P4_GPIO_ENABLE_W1TC     0x0028
#define P4_GPIO_IN              0x003C
#define P4_GPIO_OUT1_W1TS       0x0014
#define P4_GPIO_OUT1_W1TC       0x0018
#define P4_GPIO_ENABLE1_W1TS    0x0030
#define P4_GPIO_ENABLE1_W1TC    0x0034
#define P4_GPIO_IN1             0x0040

#define P4_IOMUX_BASE           (P4_HPPERIPH1_BASE + 0x21000)
#define P4_IOMUX_GPIO39         0x00A0
#define P4_IOMUX_GPIO40         0x00A4
#define P4_IOMUX_GPIO41         0x00A8
#define P4_IOMUX_GPIO42         0x00AC
#define P4_IOMUX_GPIO43         0x00B0
#define P4_IOMUX_GPIO44         0x00B4
#define P4_IOMUX_GPIO45         0x00B8
#define P4_IOMUX_GPIO46         0x00BC
#define  P4_IOMUX_FUN_PD        (1U << 7)
#define  P4_IOMUX_FUN_PU        (1U << 8)
#define  P4_IOMUX_FUN_IE        (1U << 9)
#define  P4_IOMUX_FUN_DRV_S     10
#define  P4_IOMUX_FUN_DRV_M     (3U << P4_IOMUX_FUN_DRV_S)
#define  P4_IOMUX_MCU_SEL_S     12
#define  P4_IOMUX_MCU_SEL_M     (7U << P4_IOMUX_MCU_SEL_S)
#define  P4_IOMUX_FUNC_SDMMC    0
#define  P4_IOMUX_FUNC_GPIO     1

/*
 * Any pin's IOMUX register, and the GPIO matrix.
 *
 * The eight offsets above were written out one by one when the SD host was
 * the only thing that needed them, and its pins happen to have a dedicated
 * IOMUX function.  I2C does not on this board: SCL and SDA sit on GPIO21 and
 * GPIO20, which reach the peripheral only through the matrix, so a general
 * form is needed.  The pattern the eight follow is 0x4 + n*4, which
 * P4_IOMUX_GPIO39 at 0xA0 confirms.
 *
 * The matrix is two arrays.  One entry per pin says which peripheral signal
 * drives it, one entry per signal says which pin it is read from, and a
 * bidirectional line like SDA needs both pointing at each other.
 */
#define P4_IOMUX_PIN(n)         (0x4 + (n) * 4)
#define P4_GPIO_FUNC_OUT_SEL(pin) (0x558 + (pin) * 4)
#define  P4_GPIO_OUT_SEL_MASK   0x1FFUL
#define  P4_GPIO_OEN_SEL        (1UL << 10)
/*
 * The out-select value that means "the GPIO output register drives this pin"
 * rather than a peripheral signal.  256 is outside the signal map on purpose,
 * and the field is nine bits wide precisely so it can hold it.
 */
#define  P4_GPIO_OUT_SEL_GPIO   256UL
#define P4_GPIO_FUNC_IN_SEL(sig)  (0x158 + (sig) * 4)
#define  P4_GPIO_IN_SEL_MASK    0x3FUL

/*
 * I2C1, the bus the D1001 puts its port expander, codec, IMU and clock on.
 *
 * Espressif's I2C is not a shift register with a status bit: it takes a list
 * of up to eight commands - start, write, read, stop - in COMD0..COMD7, with
 * the payload in a 32-byte FIFO, and runs the whole list on one trigger.  So
 * a transfer is built, started once and waited for once, which is why the
 * byte-level methods of AROS's i2c HIDD class cannot be implemented on it and
 * WriteRead can.
 */
#define P4_I2C1_BASE            (P4_HPPERIPH1_BASE + 0x5000)
#define P4_I2C_SCL_LOW_PERIOD   0x00
#define P4_I2C_CTR              0x04
#define   P4_I2C_SDA_FORCE_OUT  (1UL << 0)
#define   P4_I2C_SCL_FORCE_OUT  (1UL << 1)
#define   P4_I2C_MS_MODE        (1UL << 4)
#define   P4_I2C_TRANS_START    (1UL << 5)
#define   P4_I2C_TX_LSB_FIRST   (1UL << 6)
#define   P4_I2C_RX_LSB_FIRST   (1UL << 7)
#define   P4_I2C_CLK_EN         (1UL << 8)
#define   P4_I2C_ARBITRATION_EN (1UL << 9)
#define   P4_I2C_FSM_RST        (1UL << 10)
#define   P4_I2C_CONF_UPGATE    (1UL << 11)
#define P4_I2C_SR               0x08
#define   P4_I2C_RESP_REC       (1UL << 0)
#define   P4_I2C_ARB_LOST       (1UL << 3)
#define   P4_I2C_BUS_BUSY       (1UL << 4)
#define   P4_I2C_RXFIFO_CNT_S   8
#define   P4_I2C_RXFIFO_CNT_M   0x3FUL
#define P4_I2C_TO               0x0C
#define   P4_I2C_TIME_OUT_EN    (1UL << 5)
#define P4_I2C_FIFO_ST          0x14
#define P4_I2C_FIFO_CONF        0x18
#define   P4_I2C_NONFIFO_EN     (1UL << 10)
#define   P4_I2C_FIFO_ADDR_CFG_EN (1UL << 11)
#define   P4_I2C_RX_FIFO_RST    (1UL << 12)
#define   P4_I2C_TX_FIFO_RST    (1UL << 13)
#define   P4_I2C_FIFO_PRT_EN    (1UL << 14)
#define P4_I2C_DATA             0x1C
#define P4_I2C_INT_RAW          0x20
#define P4_I2C_INT_CLR          0x24
#define   P4_I2C_END_DETECT_INT (1UL << 3)
#define   P4_I2C_ARB_LOST_INT   (1UL << 5)
#define   P4_I2C_TRANS_COMPLETE_INT (1UL << 7)
#define   P4_I2C_TIME_OUT_INT   (1UL << 8)
#define   P4_I2C_NACK_INT       (1UL << 10)
#define P4_I2C_INT_ENA          0x28
#define P4_I2C_SDA_HOLD         0x30
#define P4_I2C_SDA_SAMPLE       0x34
#define P4_I2C_SCL_HIGH_PERIOD  0x38
#define   P4_I2C_SCL_WAIT_HIGH_S 9
#define P4_I2C_SCL_START_HOLD   0x40
#define P4_I2C_SCL_RSTART_SETUP 0x44
#define P4_I2C_SCL_STOP_HOLD    0x48
#define P4_I2C_SCL_STOP_SETUP   0x4C
#define P4_I2C_FILTER_CFG       0x50
#define P4_I2C_COMD(n)          (0x58 + (n) * 4)

/* The command opcodes, in bits 13:11 of a COMD entry. */
#define P4_I2C_CMD_RSTART       6
#define P4_I2C_CMD_WRITE        1
#define P4_I2C_CMD_READ         3
#define P4_I2C_CMD_STOP         2
#define P4_I2C_CMD_END          4
#define P4_I2C_CMD_OP_S         11
#define P4_I2C_CMD_ACK_VALUE    (1UL << 10)
#define P4_I2C_CMD_ACK_EXP      (1UL << 9)
#define P4_I2C_CMD_ACK_CHECK_EN (1UL << 8)

/*
 * Both controllers' gates, source selects, dividers and resets.
 *
 * The two are not laid out symmetrically: I2C0's enable and source select sit
 * in PERI_CLK_CTRL10 with its divider, while I2C1's enable and source select
 * are in the same register but its divider is in CTRL11.  So the tables below
 * carry a register per field rather than assuming an offset.
 *
 * I2C0's divider is bits [9:2] of CTRL10, after its source select (bit 0)
 * and enable (bit 1); ESP-IDF v6.0.1 hw_ver1 hp_sys_clkrst_struct.h,
 * reg_i2c0_clk_div_num.  Until 2026-10-04 it was written at [7:0], which at
 * 100 kHz (divider field 0) cleared the enable and at 10 kHz (field 3) set
 * the fast-RC source: the "controller 0 only answers at 10 kHz" record.
 */
#define P4_I2C0_BASE            (P4_HPPERIPH1_BASE + 0x4000)
#define P4_CLKRST_SOC_CLK_CTRL2 (P4_HP_SYS_CLKRST_BASE + 0x1C)
#define   P4_I2C0_APB_CLK_EN    (1UL << 12)
#define   P4_I2C1_APB_CLK_EN    (1UL << 13)
#define P4_CLKRST_PERI_CLK_CTRL10 (P4_HP_SYS_CLKRST_BASE + 0x40)
#define   P4_I2C0_CLK_SRC_SEL   (1UL << 0)    /* 0 XTAL, 1 fast RC */
#define   P4_I2C0_CLK_EN        (1UL << 1)
#define   P4_I2C1_CLK_SRC_SEL   (1UL << 26)
#define   P4_I2C1_CLK_EN        (1UL << 27)
#define   P4_I2C0_CLK_DIV_NUM_S 2
#define P4_CLKRST_PERI_CLK_CTRL11 (P4_HP_SYS_CLKRST_BASE + 0x44)
#define   P4_I2C1_CLK_DIV_NUM_S 0
#define   P4_I2C_CLK_DIV_NUM_M  0xFFUL
#define P4_CLKRST_HP_RST_EN1    (P4_HP_SYS_CLKRST_BASE + 0xC4)
#define   P4_RST_EN_I2C1        (1UL << 21)
#define   P4_RST_EN_I2C0        (1UL << 22)

/* The matrix signal indices, from the SoC's signal map. */
#define P4_SIG_I2C0_SCL         68
#define P4_SIG_I2C0_SDA         69
#define P4_SIG_I2C1_SCL         70
#define P4_SIG_I2C1_SDA         71

/* Board wiring and port-expander signals live in board/<name>.h. */
/*
 * LEDC, for the backlight, because a static level does not light it.
 *
 * Driving GPIO14 high with the expander's enable bit set left the panel dark
 * with every register in the path reading back asserted.  The reference puts a
 * 5 kHz PWM there, and a backlight driver whose dimming input needs a switching
 * signal rather than a DC level is exactly what that difference produces.
 *
 * The source is the crystal, again for the reason the I2C bus uses it: it is
 * the one clock no divider this port sets can move.  At 40 MHz, ten bits of
 * resolution and 5 kHz, one counter step is 40e6 / 5000 / 1024 = 7.8125 source
 * ticks, and the divider is that in Q8, so 2000 exactly.  The duty register is
 * Q4, so a duty value is written shifted left four.
 */
#define P4_LEDC_BASE            (P4_HPPERIPH1_BASE + 0x13000)
#define P4_LEDC_CH0_CONF0       0x000
#define   P4_LEDC_TIMER_SEL_MASK 0x3UL
#define   P4_LEDC_SIG_OUT_EN    (1UL << 2)
#define   P4_LEDC_IDLE_LV       (1UL << 3)
#define   P4_LEDC_PARA_UP       (1UL << 4)
#define P4_LEDC_CH0_HPOINT      0x004
#define P4_LEDC_CH0_DUTY        0x008
#define P4_LEDC_CH0_CONF1       0x00C
#define   P4_LEDC_DUTY_START    (1UL << 31)
#define P4_LEDC_TIMER0_CONF     0x0A0
#define   P4_LEDC_DUTY_RES_MASK 0x1FUL
#define   P4_LEDC_CLK_DIV_SHIFT 5
#define   P4_LEDC_CLK_DIV_MASK  (0x3FFFFUL << P4_LEDC_CLK_DIV_SHIFT)
#define   P4_LEDC_TIMER_PAUSE   (1UL << 23)
#define   P4_LEDC_TIMER_RST     (1UL << 24)
#define   P4_LEDC_TICK_SEL      (1UL << 25)
/*
 * The timer's own parameter-commit bit, which is not the channel's.  Writing
 * the divider and the resolution without it leaves the timer not counting, and
 * a channel whose timer does not count holds its output at whatever the
 * comparison gives with the counter at zero - which is high, and looks exactly
 * like a working full-brightness backlight that does not light anything.
 */
#define   P4_LEDC_TIMER_PARA_UP (1UL << 26)
#define P4_LEDC_CONF            0x170
#define   P4_LEDC_APB_CLK_SEL_MASK 0x3UL
#define   P4_LEDC_GLOBAL_CLK_EN (1UL << 31)

#define P4_CLKRST_SOC_CLK_CTRL3 (P4_HP_SYS_CLKRST_BASE + 0x20)
#define   P4_LEDC_APB_CLK_EN    (1UL << 0)
#define P4_CLKRST_PERI_CLK_CTRL22 (P4_HP_SYS_CLKRST_BASE + 0x9C)
#define   P4_LEDC_CLK_SRC_MASK  0x3UL      /* 0 XTAL, 1 fast RC, 2 PLL */
#define   P4_LEDC_CLK_EN        (1UL << 2)
#define   P4_RST_EN_LEDC        (1UL << 29)

#define P4_SIG_LEDC_CH0_OUT     126
#define P4_LEDC_BL_DUTY_RES     10
#define P4_LEDC_BL_CLK_DIV      2000
/*
 * Twenty per cent, deliberately low while the display path is under
 * development: a panel that is being driven wrongly should not also be
 * bright.  Overridable from the build so that a run looking for a first image
 * can raise it, because a faint image on an unlit panel and no image at all
 * look the same from across a desk.
 */
/*
 * How long B5 leaves the scanout running before it stops it, in seconds.
 *
 * Zero means indefinitely, which is what this phase did and what cost a
 * working board twice.  A CPU reset does not reset the GDMA, so a scanout left
 * running is still reading PSRAM over AXI when the next boot's bring-up
 * reconfigures the controller, and the board then needs the vendor firmware
 * flashed to recover - a power cycle does not do it.
 *
 * Long enough to look at the panel and photograph it, short enough that a
 * reset a minute later is safe.
 */
#ifndef P4_SCANOUT_SECS
#define P4_SCANOUT_SECS         60
#endif

/* A board profile may set its own fixed level (P4_BOARD_BACKLIGHT_PERCENT)
   until brightness becomes a runtime setting (ROADMAP D2, J5). */
#ifndef P4_LEDC_BL_PERCENT
#ifdef P4_BOARD_BACKLIGHT_PERCENT
#define P4_LEDC_BL_PERCENT      P4_BOARD_BACKLIGHT_PERCENT
#else
#define P4_LEDC_BL_PERCENT      20
#endif
#endif

#define P4_PCA9535_INPUT        0x00
#define P4_PCA9535_OUTPUT       0x02
#define P4_PCA9535_POLARITY     0x04
#define P4_PCA9535_CONFIG       0x06

#define P4_SD_D0_GPIO           39
#define P4_SD_D1_GPIO           40
#define P4_SD_D2_GPIO           41
#define P4_SD_D3_GPIO           42
#define P4_SD_CLK_GPIO          43
#define P4_SD_CMD_GPIO          44

/* SDMMC module gate and its low-speed PLL160M divider. */
#define P4_HP_SYS_CLKRST_BASE   (P4_HPPERIPH1_BASE + 0x26000)
#define P4_HP_SOC_CLK_CTRL1     0x0018
#define  P4_HP_SDMMC_CLK_EN     (1U << 14)
#define P4_HP_REF_CLK_CTRL2     0x002C
#define  P4_HP_REF_160M_CLK_EN  (1U << 0)
#define P4_HP_PERI_CLK_CTRL01   0x0034
#define  P4_HP_SDIO_HS_MODE     (1U << 22)
#define  P4_HP_SDIO_CLK_SRC     (1U << 23)
#define  P4_HP_SDIO_CLK_EN      (1U << 24)
#define P4_HP_PERI_CLK_CTRL02   0x0038
#define  P4_HP_SDIO_UPDATE      (1U << 8)
#define  P4_HP_SDIO_EDGE_L_S    9
#define  P4_HP_SDIO_EDGE_H_S    13
#define  P4_HP_SDIO_EDGE_N_S    17
#define  P4_HP_SDIO_SLF_EDGE_S  21
#define  P4_HP_SDIO_DRV_EDGE_S  23
#define  P4_HP_SDIO_SAM_EDGE_S  25
#define  P4_HP_SDIO_SLF_EN      (1U << 27)
#define  P4_HP_SDIO_DRV_EN      (1U << 28)
#define  P4_HP_SDIO_SAM_EN      (1U << 29)
#define  P4_HP_SDIO_FIELDS_M    0x3FFFFF00U

#define P4_LP_CLKRST_BASE       (P4_LPAON_BASE + 0x1000)


/*
 * MIPI-DSI: the PHY supply, the clocks, the host and the bridge.
 *
 * Revision matters here and the display contract says why.  Three fields the
 * reference implementation writes do not exist on ESP32-P4 revision 1.x and
 * are hw_ver3 additions: the PHY PLL reference source select, its divider, and
 * the bridge's own soft reset.  So on this board the PHY reference is fixed by
 * hardware rather than chosen, and the bridge is reset only through the system
 * reset register.  Writing the absent fields would be writing reserved bits.
 */
/*
 * The digital supply's active-mode setting.
 *
 * Reset leaves this at 20.  The board vendor's own firmware raises it to 26
 * before it brings PSRAM up, and ESP-IDF does not do this for the P4 by
 * itself - it is a board-level correction, not a chip-level one, so nothing
 * in IDF's PSRAM path reveals it.
 *
 * It matters here because the PMU is not reset by a CPU reset.  A boot that
 * follows the vendor firmware inherits 26 and PSRAM works; a boot from cold,
 * or after any firmware that leaves the default, gets 20 and the chip carries
 * no data at all.  That is the whole of the 23 August failure: the PSRAM
 * registers were correct throughout and the supply underneath them was not.
 *
 * P4_PSRAM_LOW_BIAS forces the default back, which reproduces the failure on
 * demand without needing a power cycle.
 */
/*
 * The digital regulator and the PVT block's system clock.
 *
 * Defined but not written.  ESP-IDF's PVT path sets PMU_DIG_DBIAS_INIT with
 * the comment "start calibration", enables PVT_SYS_CLK_EN before it, and hands
 * the supply to the PVT monitor afterwards - so these looked like the part of
 * pmu_init that could gate another analogue calibration.  Setting all three
 * ahead of the MSPI PLL calibration changed nothing, measured.  They are kept
 * here because the full PVT setup is the remaining candidate and will need
 * them; nothing in this port writes them today.
 */
#define P4_PMU_HP_REGULATOR0    (P4_PMU_BASE + 0x28)
#define   P4_PMU_DBIAS_SEL      (1UL << 14)
#define   P4_PMU_DBIAS_INIT     (1UL << 15)   /* write-triggered */
#define   P4_PVT_SYS_CLK_EN     (1UL << 25)   /* in SOC_CLK_CTRL1 */

#define P4_PMU_HP_ACTIVE_BIAS   (P4_PMU_BASE + 0x18)
#define   P4_PMU_DCM_VSET_SHIFT 18
#define   P4_PMU_DCM_VSET_MASK  (0x1FUL << P4_PMU_DCM_VSET_SHIFT)
#define   P4_PMU_DCM_VSET_RESET 20
#define   P4_PMU_DCM_VSET_PSRAM 26

#define P4_PMU_EXT_LDO_VO2      (P4_PMU_BASE + 0x1D0)
#define P4_PMU_EXT_LDO_VO2_ANA  (P4_PMU_BASE + 0x1D4)
#define P4_PMU_EXT_LDO_VO3      (P4_PMU_BASE + 0x1C0)
#define P4_PMU_EXT_LDO_VO3_ANA  (P4_PMU_BASE + 0x1C4)
#define   P4_LDO_FORCE_TIEH_SEL (1UL << 7)
#define   P4_LDO_XPD            (1UL << 8)
#define   P4_LDO_TIEH_SEL_SHIFT 9
#define   P4_LDO_TIEH_SEL_MASK  (0x7UL << P4_LDO_TIEH_SEL_SHIFT)
#define   P4_LDO_TIEH           (1UL << 14)
#define   P4_LDO_MUL_SHIFT      23
#define   P4_LDO_MUL_MASK       (0x7UL << P4_LDO_MUL_SHIFT)
#define   P4_LDO_EN_VDET        (1UL << 26)
#define   P4_LDO_DREF_SHIFT     28
#define   P4_LDO_DREF_MASK      (0xFUL << P4_LDO_DREF_SHIFT)
/* The exact uncalibrated fallback for 2500 mV; see the display contract. */
#define   P4_LDO_DREF_2V5       9
#define   P4_LDO_MUL_2V5        6

/* Read-only eFuse fields used by ESP-IDF to calibrate LDO channels 2 and 3. */
#define P4_EFUSE_BASE            0x5012D000UL
#define P4_EFUSE_RD_MAC_SYS_2    (P4_EFUSE_BASE + 0x4C)
#define   P4_EFUSE_BLK_MINOR_SHIFT 8
#define   P4_EFUSE_BLK_MINOR_MASK  (0x7UL << P4_EFUSE_BLK_MINOR_SHIFT)
#define   P4_EFUSE_BLK_MAJOR_SHIFT 11
#define   P4_EFUSE_BLK_MAJOR_MASK  (0x3UL << P4_EFUSE_BLK_MAJOR_SHIFT)
#define   P4_EFUSE_LDO2_DREF_SHIFT 28
#define   P4_EFUSE_LDO2_DREF_MASK (0xFUL << P4_EFUSE_LDO2_DREF_SHIFT)
#define P4_EFUSE_RD_MAC_SYS_3    (P4_EFUSE_BASE + 0x50)
#define   P4_EFUSE_LDO2_MUL_SHIFT 3
#define   P4_EFUSE_LDO2_MUL_MASK  (0x7UL << P4_EFUSE_LDO2_MUL_SHIFT)
#define   P4_EFUSE_LDO3_K_SHIFT  6
#define   P4_EFUSE_LDO3_K_MASK   (0xFFUL << P4_EFUSE_LDO3_K_SHIFT)
#define   P4_EFUSE_LDO3_VOS_SHIFT 14
#define   P4_EFUSE_LDO3_VOS_MASK (0x3FUL << P4_EFUSE_LDO3_VOS_SHIFT)
#define   P4_EFUSE_LDO3_C_SHIFT  20
#define   P4_EFUSE_LDO3_C_MASK   (0x3FUL << P4_EFUSE_LDO3_C_SHIFT)

#define P4_CLKRST_SOC_CLK_CTRL1 (P4_HP_SYS_CLKRST_BASE + 0x18)
#define   P4_DSI_SYS_CLK_EN     (1UL << 12)
#define P4_CLKRST_HP_RST_EN0    (P4_HP_SYS_CLKRST_BASE + 0xC0)
#define   P4_RST_EN_DSI_BRG     (1UL << 26)
#define P4_CLKRST_PERI_CLK_CTRL02 (P4_HP_SYS_CLKRST_BASE + 0x38)
#define   P4_DSI_DPHY_CLK_SRC_SHIFT 30
#define   P4_DSI_DPHY_CLK_SRC_MASK  (0x3UL << P4_DSI_DPHY_CLK_SRC_SHIFT)
#define P4_CLKRST_PERI_CLK_CTRL03 (P4_HP_SYS_CLKRST_BASE + 0x3C)
#define   P4_DSI_DPHY_CFG_CLK_EN    (1UL << 0)
#define   P4_DSI_DPHY_PLL_REFCLK_EN (1UL << 1)
#define   P4_DSI_DPICLK_SRC_SHIFT   5
#define   P4_DSI_DPICLK_SRC_MASK    (0x3UL << P4_DSI_DPICLK_SRC_SHIFT)
#define   P4_DSI_DPICLK_EN          (1UL << 7)
#define   P4_DSI_DPICLK_DIV_SHIFT   8
#define   P4_DSI_DPICLK_DIV_MASK    (0xFFUL << P4_DSI_DPICLK_DIV_SHIFT)

#define P4_DSI_HOST_BASE        (P4_HPPERIPH0_BASE + 0xA0000)
#define P4_DSI_BRG_BASE         (P4_HPPERIPH0_BASE + 0xA0800)
#define P4_DSI_PWR_UP           0x004
#define   P4_DSI_SHUTDOWNZ      (1UL << 0)
#define P4_DSI_MODE_CFG         0x034
#define   P4_DSI_CMD_VIDEO_MODE (1UL << 0)
#define P4_DSI_LPCLK_CTRL       0x094
#define   P4_DSI_TXREQUESTCLKHS (1UL << 0)
/*
 * The other half of what the reference calls the clock lane's automatic state.
 *
 * It sets auto_clklane_ctrl together with txrequestclkhs; this port set only
 * txrequestclkhs, which pins the clock lane in high speed permanently instead
 * of letting the host manage it.  Measured with only the one bit: the PLL
 * locks, the clock lane leaves stop state, and both data lanes stay in it -
 * the host never transmits.
 */
#define   P4_DSI_AUTO_CLKLANE   (1UL << 1)
#define P4_DSI_PHY_RSTZ         0x0A0
#define   P4_DSI_PHY_SHUTDOWNZ  (1UL << 0)
#define   P4_DSI_PHY_RSTZ_BIT   (1UL << 1)
#define   P4_DSI_PHY_ENABLECLK  (1UL << 2)
#define   P4_DSI_PHY_FORCEPLL   (1UL << 3)
#define P4_DSI_PHY_IF_CFG       0x0A4
#define   P4_DSI_N_LANES_MASK   0x3UL
#define   P4_DSI_STOP_WAIT_SHIFT 8
#define   P4_DSI_STOP_WAIT_MASK (0xFFUL << P4_DSI_STOP_WAIT_SHIFT)
#define P4_DSI_PHY_STATUS       0x0B0
#define   P4_DSI_PHY_LOCK       (1UL << 0)
#define   P4_DSI_PHY_DIRECTION  (1UL << 1)
#define   P4_DSI_STOPSTATE_CLK  (1UL << 2)
#define   P4_DSI_STOPSTATE_L0   (1UL << 4)
#define   P4_DSI_STOPSTATE_L1   (1UL << 7)
/* Live state of the host's DPI input and internal payload buffer.  The working
 * Vellum reference holds 0x00020001 after startup: command input empty and the
 * internal payload buffer full, without either DPI input-full indication. */
#define P4_DSI_VID_PKT_STATUS   0x168
#define   P4_DSI_DPI_CMD_EMPTY  (1UL << 0)
#define   P4_DSI_DPI_CMD_FULL   (1UL << 1)
#define   P4_DSI_DPI_PLD_EMPTY  (1UL << 2)
#define   P4_DSI_DPI_PLD_FULL   (1UL << 3)
#define   P4_DSI_BUF_PLD_EMPTY  (1UL << 16)
#define   P4_DSI_BUF_PLD_FULL   (1UL << 17)
#define P4_DSI_PHY_TST_CTRL0    0x0B4
#define   P4_DSI_TESTCLR        (1UL << 0)
#define   P4_DSI_TESTCLK        (1UL << 1)
#define P4_DSI_PHY_TST_CTRL1    0x0B8
#define   P4_DSI_TESTDIN_MASK   0xFFUL
#define   P4_DSI_TESTEN         (1UL << 16)
#define P4_DSI_BRG_CLK_EN       0x000
#define   P4_DSI_BRG_CLK_EN_BIT (1UL << 0)
#define P4_DSI_BRG_EN           0x004
#define P4_DSI_BRG_PIXEL_TYPE   0x018
#define   P4_DSI_BRG_RAW_TYPE_MASK  0xFUL
#define   P4_DSI_BRG_DPI_TYPE_SHIFT 4
#define   P4_DSI_BRG_DPI_TYPE_MASK  (0x3UL << P4_DSI_BRG_DPI_TYPE_SHIFT)
#define   P4_DSI_BRG_DATA_IN_TYPE   (1UL << 6)
/*
 * The bridge's pixel format, and zero is not the neutral value it looks like.
 *
 * raw_type 0 is RGB888.  A bridge left at its reset value therefore reads
 * three bytes per pixel out of a two-byte-per-pixel frame and hands the host
 * twenty-four bits where it is configured for sixteen, which overruns the
 * host's payload fifo - DPI_PLD_WR_ERR - while every register reads back
 * exactly as written.  The value was read out as a diagnostic for three
 * sessions before anyone asked what zero meant.
 *
 * On this revision, hw_ver1, the reference sets raw_type for both the input
 * and the output format and leaves dpi_config at the sub-configuration it is
 * given, which is zero.  hw_ver3 splits the two into raw_type and dpi_type;
 * do not carry that split back here.
 */
#define   P4_DSI_BRG_RAW_RGB888     0UL
#define   P4_DSI_BRG_RAW_RGB666     1UL
#define   P4_DSI_BRG_RAW_RGB565     2UL
#define P4_DSI_BRG_DPI_V_CFG0   0x030
#define P4_DSI_BRG_DPI_V_CFG1   0x034
#define P4_DSI_BRG_DPI_H_CFG0   0x038
#define P4_DSI_BRG_DPI_H_CFG1   0x03C
#define   P4_DSI_BRG_TOTAL_SHIFT 0
#define   P4_DSI_BRG_DISP_SHIFT  16
#define   P4_DSI_BRG_BANK_SHIFT  0
#define   P4_DSI_BRG_SYNC_SHIFT  16
#define P4_DSI_BRG_DPI_MISC_CFG 0x040
#define   P4_DSI_BRG_DPI_EN     (1UL << 0)
#define P4_DSI_BRG_DPI_CFG_UPD  0x044
#define   P4_DSI_BRG_CFG_UPDATE (1UL << 0)
#define P4_DSI_BRG_INT_RAW      0x058
/*
 * What the bridge says about itself, which this port never asked.
 *
 * RAW_BUF_DEPTH is the bridge's own fifo occupancy and is the only reading
 * that distinguishes a bridge that never starts a frame from one that starts
 * and starves - both of which present as a DMA that does not move.
 *
 * RSV_DPI_DATA is why the host can report a continuous payload error while
 * the DMA delivers nothing: on underflow the bridge does not stop, it sends
 * this reserved pixel value to the host instead.  Default 16383.
 */
#define P4_DSI_BRG_FIFO_STATUS  0x014
#define   P4_DSI_BRG_BUF_DEPTH_MASK 0x3FFFUL
#define P4_DSI_BRG_CREDIT_CTL   0x010
#define P4_DSI_BRG_BLOCK_INTVL  0x01C
#define P4_DSI_BRG_REQ_INTVL    0x020
#define P4_DSI_BRG_DPI_LCD_CTL  0x024
#define   P4_DSI_BRG_DPISHUTDN  (1UL << 0)
#define   P4_DSI_BRG_DPICOLORM  (1UL << 1)
#define   P4_DSI_BRG_DPIUPDATE  (1UL << 2)
#define P4_DSI_BRG_RSV_DPI_DATA 0x028
#define P4_DSI_BRG_INT_ENA      0x050
#define P4_DSI_BRG_INT_CLR      0x054
#define P4_DSI_BRG_BLK_RAW_NUM  0x068
#define   P4_DSI_BRG_BLK_RAW_MASK      0x003FFFFFUL
#define   P4_DSI_BRG_BLK_RAW_SET       (1UL << 31)
#define P4_DSI_BRG_HOST_CTRL    0x080
#define P4_DSI_BRG_MEM_CLK_CTRL 0x084

/*
 * The bridge settings B4 never wrote, and the reason its host reported
 * DPI_PLD_WR_ERR.
 *
 * B4 configured the bridge's timing, pixel type and flow control and nothing
 * else.  With the pixel feed on, the host then reported a payload write error
 * whether or not its own pattern generator was running, and the bridge never
 * underran - so the bridge was pushing pixels the host could not take.  The
 * reference sets six more things, and one of them is not a refinement: it
 * makes the DMA the flow controller, where this port had the bridge itself,
 * which is what "push without being asked" looks like in a register.
 *
 * Values are the reference's, for 800x1280 at sixteen bits:
 *
 *   raw_num_total    (800 * 1280 * 16 + 63) / 64 = 256000 sixty-four-bit words
 *   discard count    800, one line
 *   burst length     256
 *   empty threshold  1024 - 256
 *   multi-block      1, one DMA node carries the whole image
 */
#define P4_DSI_BRG_DMA_REQ_CFG  0x008
#define   P4_DSI_BRG_BURST_LEN_MASK   0xFFFUL
#define P4_DSI_BRG_RAW_NUM_CFG  0x00C
#define   P4_DSI_BRG_RAW_NUM_MASK     0x3FFFFFUL
#define   P4_DSI_BRG_UNALIGN_64BIT    (1UL << 22)
#define   P4_DSI_BRG_RAW_NUM_SET      (1UL << 31)
#define   P4_DSI_BRG_DISCARD_SHIFT    4
#define   P4_DSI_BRG_DISCARD_MASK     (0xFFFUL << P4_DSI_BRG_DISCARD_SHIFT)
#define P4_DSI_BRG_DMA_FRAME_INT 0x06C
#define   P4_DSI_BRG_MULTIBLK_EN      (1UL << 28)
#define P4_DSI_BRG_DMA_FLOW_CTRL 0x088
#define   P4_DSI_BRG_FLOW_DMA         0UL   /* the reference's choice */
#define   P4_DSI_BRG_FLOW_BRIDGE_SEL  1UL   /* the reset default, and B4's */
#define   P4_DSI_BRG_MULTIBLK_SHIFT   4
#define   P4_DSI_BRG_MULTIBLK_MASK    (0xFUL << P4_DSI_BRG_MULTIBLK_SHIFT)
#define P4_DSI_BRG_EMPTY_THRD   0x08C
#define   P4_DSI_BRG_EMPTY_MASK       0x7FFUL

/* Where the bridge takes its pixels from: one fixed address, written by DMA */
#define P4_DSI_BRG_MEM_BASE     0x50105000UL

/*
 * The DesignWare AXI DMA, which is what actually moves the frame.
 *
 * One channel, one link-list item, and the item carries the whole image - the
 * reference's own comment says it assumes exactly that.  Channel registers are
 * a flat block per channel starting at 0x100; only channel one is used here.
 */
/*
 * The DMA's own clocks and reset, which are outside its register block.
 *
 * Missed on the first attempt, and the failure was quiet in an instructive
 * way: the module's registers answered, the reset bit cleared, the channel
 * enable read back set - and the channel never loaded its descriptor, so the
 * source address stayed at zero.  Everything that could be read looked
 * configured; only the engine was not running.
 */
#define P4_GDMA_CPU_CLK_EN      (1UL << 13)  /* in SOC_CLK_CTRL0 */
#define P4_GDMA_SYS_CLK_EN      (1UL << 5)   /* in SOC_CLK_CTRL1 */
#define P4_RST_EN_GDMA          (1UL << 21)  /* in HP_RST_EN0 */
/*
 * The MSPI block's own reset is P4_RST_EN_DUAL_MSPI_AXI/_APB in psram.h, and
 * the bring-up already pulses both in the reference's order.  ESP-IDF has no
 * chip-level reset for this part - no 0x66/0x99, nothing - because after a
 * reset its bootloader runs the bring-up before any DMA exists.  This port
 * leaves a scanout running across a reset, which is a situation the reference
 * never has.
 */

#define P4_DMAC_BASE            0x50081000UL
/* ESP-IDF's CACHE_LL_L2MEM_NON_CACHE_ADDR(): internal SRAM is cached at its
   normal address and directly visible to DMA through this CPU alias. */
#define P4_L2MEM_NONCACHE_OFFSET 0x40000000UL
#define P4_DMAC_CFG             (P4_DMAC_BASE + 0x010)
#define   P4_DMAC_CFG_EN        (1UL << 0)
#define   P4_DMAC_INT_EN        (1UL << 1)
#define P4_DMAC_CHEN            (P4_DMAC_BASE + 0x018)
#define   P4_DMAC_CH1_EN        (1UL << 0)
#define   P4_DMAC_CH1_EN_WE     (1UL << 8)
#define P4_DMAC_RESET           (P4_DMAC_BASE + 0x058)
#define   P4_DMAC_RESET_BIT     (1UL << 0)

#define P4_DMAC_CH1             (P4_DMAC_BASE + 0x100)
#define P4_DMAC_CH_SAR          0x000
#define P4_DMAC_CH_DAR          0x008
#define P4_DMAC_CH_BLOCK_TS     0x010
#define P4_DMAC_CH_CTL0         0x018
#define P4_DMAC_CH_CTL1         0x01C
#define P4_DMAC_CH_CFG0         0x020
#define P4_DMAC_CH_CFG1         0x024
#define P4_DMAC_CH_LLP          0x028
#define   P4_DMAC_LLP_LMS_MEMORY (1UL << 0)

/* CFG0: how each side walks its blocks.  3 is link-list. */
#define   P4_DMAC_SRC_MULTBLK_SHIFT   0
#define   P4_DMAC_DST_MULTBLK_SHIFT   2
#define   P4_DMAC_MULTBLK_RELOAD      1UL
#define   P4_DMAC_MULTBLK_LIST        3UL

/* CFG1: direction, who controls flow, which peripheral, how deep to queue */
#define   P4_DMAC_TT_FC_SHIFT         0
#define   P4_DMAC_TT_FC_M2P_DMAC      1UL   /* memory to peripheral, DMA controls */
#define   P4_DMAC_HS_SEL_SRC          (1UL << 3)   /* set = software handshake */
#define   P4_DMAC_HS_SEL_DST          (1UL << 4)
#define   P4_DMAC_SRC_PER_SHIFT       7
#define   P4_DMAC_DST_PER_SHIFT       12
#define   P4_DMAC_PER_DSI             0UL
#define   P4_DMAC_CH_PRIOR_SHIFT      17
#define   P4_DMAC_SRC_OSR_SHIFT       23
#define   P4_DMAC_DST_OSR_SHIFT       27
/*
 * The channel's own account of why it stopped.
 *
 * INTSTATUS_ENABLE0 resets with these bits set, so the status is readable
 * without enabling anything, and it names the failure directly: a descriptor
 * that would not read, a descriptor the engine considered invalid, a decode
 * error on either side, or a channel that simply disabled or aborted itself.
 * None of that is visible in the source address, which is all this port had
 * been reading.
 */
#define   P4_DMAC_CH_INTSTATUS0       0x088
#define   P4_DMAC_CH_INTSTATUS1       0x08C
#define   P4_DMAC_CH_INTSTATUS_ENABLE0 0x080
#define   P4_DMAC_CH_INTSIGNAL_ENABLE0 0x090
#define   P4_DMAC_CH_INTCLEAR0        0x098
#define   P4_DMAC_CH_INTCLEAR1        0x09C
#define   P4_DMAC_IS_BLOCK_DONE       (1UL << 0)
#define   P4_DMAC_IS_DMA_DONE         (1UL << 1)
#define   P4_DMAC_IS_SRC_TRANSCOMP    (1UL << 3)
#define   P4_DMAC_IS_DST_TRANSCOMP    (1UL << 4)
#define   P4_DMAC_IS_SRC_DEC_ERR      (1UL << 5)
#define   P4_DMAC_IS_DST_DEC_ERR      (1UL << 6)
#define   P4_DMAC_IS_SRC_SLV_ERR      (1UL << 7)
#define   P4_DMAC_IS_DST_SLV_ERR      (1UL << 8)
#define   P4_DMAC_IS_LLI_RD_DEC_ERR   (1UL << 9)
#define   P4_DMAC_IS_LLI_WR_DEC_ERR   (1UL << 10)
#define   P4_DMAC_IS_LLI_RD_SLV_ERR   (1UL << 11)
#define   P4_DMAC_IS_LLI_WR_SLV_ERR   (1UL << 12)
#define   P4_DMAC_IS_LLI_INVALID      (1UL << 13)
#define   P4_DMAC_IS_MULTIBLK_ERR     (1UL << 14)
#define   P4_DMAC_IS_SLVIF_DEC_ERR    (1UL << 16)
#define   P4_DMAC_IS_WRONCHEN_ERR     (1UL << 19)
#define   P4_DMAC_IS_SUSPENDED        (1UL << 29)
#define   P4_DMAC_IS_DISABLED         (1UL << 30)
#define   P4_DMAC_IS_ABORTED          (1UL << 31)

/*
 * A link-list item: sixty-four bytes, sixty-four-byte aligned, and the field
 * order is the channel's own register order.
 */
#define P4_DMAC_LLI_SAR_LO      0x00
#define P4_DMAC_LLI_SAR_HI      0x04
#define P4_DMAC_LLI_DAR_LO      0x08
#define P4_DMAC_LLI_DAR_HI      0x0C
#define P4_DMAC_LLI_BLOCK_TS    0x10
#define P4_DMAC_LLI_LLP_LO      0x18
#define P4_DMAC_LLI_LLP_HI      0x1C
#define P4_DMAC_LLI_CTL_LO      0x20
#define P4_DMAC_LLI_CTL_HI      0x24
#define P4_DMAC_LLI_SIZE        0x40

/* CTL_LO: master ports, address stepping, transfer widths, burst sizes */
#define   P4_DMAC_SMS           (1UL << 0) /* 1 = memory master */
#define   P4_DMAC_DMS           (1UL << 2) /* 0 = MIPI DSI master */
#define   P4_DMAC_SINC_FIXED    (1UL << 4)   /* clear = increment */
#define   P4_DMAC_DINC_FIXED    (1UL << 6)
#define   P4_DMAC_SRC_WIDTH_SHIFT 8
#define   P4_DMAC_DST_WIDTH_SHIFT 11
#define   P4_DMAC_WIDTH_64      3UL
#define   P4_DMAC_SRC_MSIZE_SHIFT 14
#define   P4_DMAC_DST_MSIZE_SHIFT 18
#define   P4_DMAC_MSIZE_256     7UL
#define   P4_DMAC_MSIZE_512     8UL

/* CTL_HI: AXI burst lengths, and the two bits that make an item live */
#define   P4_DMAC_ARLEN_EN      (1UL << 6)
#define   P4_DMAC_ARLEN_SHIFT   7
#define   P4_DMAC_AWLEN_EN      (1UL << 15)
#define   P4_DMAC_AWLEN_SHIFT   16
#define   P4_DMAC_IOC_BLKTFR    (1UL << 26)
#define   P4_DMAC_LLI_LAST      (1UL << 30)
#define   P4_DMAC_LLI_VALID     (1UL << 31)
#define   P4_DMAC_AXI_BURST_LEN 16UL

/* The frame this port scans out: the panel's native size in RGB565. */
#define P4_FB_BYTES_PER_PIXEL   (P4_PANEL_BPP / 8)
#define P4_FB_BYTES             ((unsigned long)P4_PANEL_H_RES * P4_TX_V_RES \
                                 * P4_FB_BYTES_PER_PIXEL)
#define P4_FB_WORDS64           (P4_FB_BYTES / 8)

/* B5 owns one immutable DMA surface.  B6's handoff gate owns two complete
 * surfaces so the ISR can select the next source only at DMA frame-done;
 * neither surface is ever part of Exec's allocatable PSRAM. */
#ifdef P4_B6_DOUBLE_BUFFER
#define P4_FB_BUFFER_COUNT      2UL
#else
#define P4_FB_BUFFER_COUNT      1UL
#endif
#define P4_FB_RESERVE_BYTES     (P4_FB_BYTES * P4_FB_BUFFER_COUNT)

/*
 * Where the frame lives: the top of the PSRAM window, not the base.
 *
 * The base is where the module package is loaded and where exec's external
 * memory pool starts, so a frame written there is overwritten a few steps
 * later and the DMA then scans out module code.  Reserving the top keeps the
 * two apart until there is an allocator to ask.
 */
#define P4_FB_BASE              (P4_PSRAM_WINDOW_BASE + 0x02000000UL \
                                 - P4_FB_RESERVE_BYTES)
#ifdef P4_B6_DOUBLE_BUFFER
#define P4_FB_BACK_BASE         (P4_FB_BASE + P4_FB_BYTES)
#endif
#define   P4_DSI_BRG_UNDERRUN   (1UL << 0)
#define P4_DSI_BRG_DMA_FLOW_CTL 0x088
#define   P4_DSI_BRG_FLOW_BRIDGE (1UL << 0)
#define   P4_DSI_BRG_DSI_EN     (1UL << 0)

#define P4_DSI_CLKMGR_CFG       0x008
#define   P4_DSI_TX_ESC_DIV_MASK  0xFFUL
#define   P4_DSI_TO_CLK_DIV_SHIFT 8
#define   P4_DSI_TO_CLK_DIV_MASK  (0xFFUL << P4_DSI_TO_CLK_DIV_SHIFT)
#define P4_DSI_PCKHDL_CFG       0x02C
#define   P4_DSI_EOTP_TX_EN     (1UL << 0)
#define   P4_DSI_BTA_EN         (1UL << 2)
#define   P4_DSI_ECC_RX_EN      (1UL << 3)
#define   P4_DSI_CRC_RX_EN      (1UL << 4)
#define   P4_DSI_EOTP_TX_LP_EN  (1UL << 5)
#define P4_DSI_GEN_VCID         0x030
#define   P4_DSI_GEN_VCID_RX_MASK 0x3UL
#define P4_DSI_CMD_MODE_CFG     0x068
#define   P4_DSI_ACK_RQST_EN    (1UL << 1)
#define   P4_DSI_GEN_SW_0P_TX   (1UL << 8)
#define   P4_DSI_GEN_SW_1P_TX   (1UL << 9)
#define   P4_DSI_GEN_SW_2P_TX   (1UL << 10)
#define   P4_DSI_GEN_SR_0P_TX   (1UL << 11)
#define   P4_DSI_GEN_SR_1P_TX   (1UL << 12)
#define   P4_DSI_GEN_SR_2P_TX   (1UL << 13)
#define   P4_DSI_GEN_LW_TX      (1UL << 14)
/*
 * The DCS command types, which this port never set.
 *
 * Each of these bits chooses low-power escape mode for one packet type; clear
 * means high speed.  The GEN_ group above was set and the DCS_ group was not,
 * so every generic packet went out in low power as intended while every DCS
 * packet - which is what a panel's commands and its identity read actually are
 * - was asked for in high speed, on data lanes that never leave stop state.
 *
 * The reference sets all of them, GEN and DCS alike, to low power.
 */
#define   P4_DSI_DCS_SW_0P_TX   (1UL << 16)
#define   P4_DSI_DCS_SW_1P_TX   (1UL << 17)
#define   P4_DSI_DCS_SR_0P_TX   (1UL << 18)
#define   P4_DSI_DCS_LW_TX      (1UL << 19)
#define   P4_DSI_MAX_RD_PKT_SIZE (1UL << 24)
#define P4_DSI_GEN_HDR          0x06C
#define   P4_DSI_GEN_DT_MASK    0x3FUL
#define   P4_DSI_GEN_VC_SHIFT   6
#define   P4_DSI_GEN_WC_LSB_SHIFT 8
#define   P4_DSI_GEN_WC_MSB_SHIFT 16
#define P4_DSI_GEN_PLD_DATA     0x070
#define P4_DSI_CMD_PKT_STATUS   0x074
#define   P4_DSI_GEN_CMD_EMPTY  (1UL << 0)
#define   P4_DSI_GEN_CMD_FULL   (1UL << 1)
#define   P4_DSI_GEN_PLD_W_EMPTY (1UL << 2)
#define   P4_DSI_GEN_PLD_W_FULL (1UL << 3)
#define   P4_DSI_GEN_PLD_R_EMPTY (1UL << 4)
#define   P4_DSI_GEN_RD_CMD_BUSY (1UL << 6)
#define   P4_DSI_GEN_BUFF_CMD_EMPTY (1UL << 16)
#define   P4_DSI_GEN_BUFF_CMD_FULL  (1UL << 17)
#define   P4_DSI_GEN_BUFF_PLD_EMPTY (1UL << 18)
#define   P4_DSI_GEN_BUFF_PLD_FULL  (1UL << 19)
#define P4_DSI_TO_CNT_CFG       0x078
#define P4_DSI_HS_RD_TO_CNT     0x07C
#define P4_DSI_LP_RD_TO_CNT     0x080
#define P4_DSI_HS_WR_TO_CNT     0x084
#define P4_DSI_LP_WR_TO_CNT     0x088
#define P4_DSI_BTA_TO_CNT       0x08C
#define P4_DSI_PHY_TMR_LPCLK_CFG 0x098
#define   P4_DSI_CLKLP2HS_SHIFT 0
#define   P4_DSI_CLKHS2LP_SHIFT 16
#define P4_DSI_PHY_TMR_CFG      0x09C
#define   P4_DSI_LP2HS_SHIFT    0
#define   P4_DSI_HS2LP_SHIFT    16
#define P4_DSI_PHY_TMR_RD_CFG   0x0F4
#define P4_DSI_DPI_VCID         0x00C
#define P4_DSI_DPI_COLOR_CODING 0x010
/*
 * The host's three 16-bit colour codings.
 *
 * They carry the same pixel and differ in how its bits sit on the DPI bus, so
 * a mismatch between this and what the bridge lays down is a pixel with its
 * fields displaced - which a panel is free to reject outright.  The reference
 * uses configuration 1 and this port followed it, but the reference also drives
 * a bridge configured by the same code, and one of the two is what this port
 * has been guessing at.
 *
 * Worth trying against a measured symptom: with RGB565 the transmit side is
 * clean - no payload error over 25 seconds - and the panel still reports its
 * output disabled.  That is what a rejected pixel format looks like from here.
 */
#define   P4_DSI_COLOR_16BIT_C1 0
#define   P4_DSI_COLOR_16BIT_C2 1
#define   P4_DSI_COLOR_16BIT_C3 2
#ifndef P4_DSI_565_CFG
#define P4_DSI_565_CFG          1
#endif
#define   P4_DSI_COLOR_24BIT    5
#define P4_DSI_DPI_CFG_POL      0x014
#define P4_DSI_DPI_LP_CMD_TIM   0x018
#define P4_DSI_VID_MODE_CFG     0x038
#define   P4_DSI_VID_MODE_TYPE_MASK 0x3UL
/*
 * VID_MODE_CFG's video mode type, bits 1:0.
 *
 * 0 is non-burst with sync pulses, 1 non-burst with sync events, 2 burst.  The
 * reference uses burst, and burst asks the host to buffer a whole video packet
 * before it starts transmitting it - VID_PKT_SIZE pixels, which is one line
 * here.  Non-burst transmits synchronously with the pixel stream and needs far
 * less, which makes the two a test of whether the host is waiting for a buffer
 * it will never fill.
 */
#define   P4_DSI_VID_NONBURST_PULSES 0
#define   P4_DSI_VID_BURST_SYNC_PULSES 2
#define   P4_DSI_LP_VSA_EN      (1UL << 8)
#define   P4_DSI_LP_VBP_EN      (1UL << 9)
#define   P4_DSI_LP_VFP_EN      (1UL << 10)
#define   P4_DSI_LP_VACT_EN     (1UL << 11)
#define   P4_DSI_LP_HBP_EN      (1UL << 12)
#define   P4_DSI_LP_HFP_EN      (1UL << 13)
#define   P4_DSI_FRAME_BTA_ACK_EN (1UL << 14)
#define   P4_DSI_LP_CMD_EN      (1UL << 15)
/*
 * VID_MODE_CFG's low-power and acknowledge bits.
 *
 * B4 and the first B5 attempt cleared all of these, on the reasoning that
 * fewer moving parts is easier to diagnose.  The reference sets every one of
 * them - its disable_lp flag is left false for this panel - so that reasoning
 * traded a known-good configuration for a guess, and the host stalled with
 * DPI_PLD_WR_ERR.  Low-power transitions are what give the host somewhere to
 * go between lines; without them it has to carry high-speed continuously and a
 * timing calculation that is even slightly short overflows its FIFO.
 *
 * Positions verified against the register header, because the first attempt
 * put frame acknowledge at bit 11 - which is LP_VACT_EN, so it enabled a
 * low-power transition while believing it enabled an acknowledge.
 */
#define   P4_DSI_LP_VSA_EN      (1UL << 8)
#define   P4_DSI_LP_VBP_EN      (1UL << 9)
#define   P4_DSI_LP_VFP_EN      (1UL << 10)
#define   P4_DSI_LP_VACT_EN     (1UL << 11)
#define   P4_DSI_LP_HBP_EN      (1UL << 12)
#define   P4_DSI_LP_HFP_EN      (1UL << 13)
#define   P4_DSI_VID_FRAME_ACK_EN (1UL << 14)
#define   P4_DSI_LP_CMD_EN      (1UL << 15)
#define   P4_DSI_VID_LP_ALL     (P4_DSI_LP_VSA_EN | P4_DSI_LP_VBP_EN \
                                 | P4_DSI_LP_VFP_EN | P4_DSI_LP_VACT_EN \
                                 | P4_DSI_LP_HBP_EN | P4_DSI_LP_HFP_EN \
                                 | P4_DSI_LP_CMD_EN)
/*
 * The same without the two horizontal transitions.
 *
 * A return to low power costs phy_hs2lp_time plus phy_lp2hs_time, which is
 * 50 + 104 = 154 lane byte clocks with the reference's switch times.  At this
 * panel's timing the back porch is 47 of them and the front porch 94, so
 * neither period is long enough to leave high speed and come back.  The host
 * is documented to make that check itself, so this is a hypothesis to test and
 * not a defect established on paper.
 */
#define   P4_DSI_VID_LP_VERT    (P4_DSI_LP_VSA_EN | P4_DSI_LP_VBP_EN \
                                 | P4_DSI_LP_VFP_EN | P4_DSI_LP_VACT_EN \
                                 | P4_DSI_LP_CMD_EN)
#define   P4_DSI_VPG_EN         (1UL << 16)
#define   P4_DSI_VPG_MODE       (1UL << 20)
#define   P4_DSI_VPG_ORIENTATION (1UL << 24)
#define P4_DSI_VID_PKT_SIZE     0x03C
#define P4_DSI_VID_NUM_CHUNKS   0x040
#define P4_DSI_VID_NULL_SIZE    0x044
#define P4_DSI_VID_HSA_TIME     0x048
#define P4_DSI_VID_HBP_TIME     0x04C
#define P4_DSI_VID_HLINE_TIME   0x050
#define P4_DSI_VID_VSA_LINES    0x054
#define P4_DSI_VID_VBP_LINES    0x058
#define P4_DSI_VID_VFP_LINES    0x05C
#define P4_DSI_VID_VACTIVE_LINES 0x060

/*
 * The panel's native scan geometry and timing come from the board profile
 * (board/*.h). The notes below record how the D1001's values were found.
 * For the D1001 they are display/DISPLAY-CONTRACT.md's Set A - the set the
 * working reference actually writes, not the one its header declares.
 */
#define P4_PANEL_H_RES          P4_BOARD_PANEL_H_RES
#define P4_PANEL_V_RES          P4_BOARD_PANEL_V_RES
/*
 * One transmitted line is one panel line.  The earlier VMUL=2 measurement was
 * made while every RGB565 pixel had its two bytes swapped; after correcting
 * that independent fault, a D1001 run with VMUL=1 showed the complete clean
 * 800 x 1280 grid.  Keep the override only as a diagnostic.  Everything on
 * the transmit side derives from P4_TX_V_RES so framebuffer, bridge and host
 * line counts cannot drift apart.
 */
#ifndef P4_PANEL_VMUL
#define P4_PANEL_VMUL           1
#endif
#define P4_TX_V_RES             (P4_PANEL_V_RES * P4_PANEL_VMUL)

/*
 * What the software draws on. The D1001 panel is portrait and shown rotated
 * 90 degrees clockwise, so its logical width is the native height; a panel
 * with ROTATE 0 is used as it scans.
 */
#if P4_BOARD_PANEL_ROTATE == 90
#define P4_LOGICAL_W            P4_TX_V_RES
#define P4_LOGICAL_H            P4_PANEL_H_RES
#else
#define P4_LOGICAL_W            P4_PANEL_H_RES
#define P4_LOGICAL_H            P4_TX_V_RES
#endif

#define P4_PANEL_HSYNC          P4_BOARD_PANEL_HSYNC
#define P4_PANEL_HBP            P4_BOARD_PANEL_HBP
#define P4_PANEL_HFP            P4_BOARD_PANEL_HFP
#define P4_PANEL_VSYNC          P4_BOARD_PANEL_VSYNC
/*
 * D1001: 30.  The component's own header macro says 12, and this port followed
 * it for one round; the firmware that actually drives this board says 30, and
 * that is the one with evidence behind it.  See P4_PANEL_BPP for the general
 * point.  The 24-bit experiment's 12 is chosen in board/d1001.h.
 */
#define P4_PANEL_VBP            P4_BOARD_PANEL_VBP
/*
 * Overridable so the blanking can be varied as a test.
 *
 * The ghost lines measure at 34 panel lines and vsync 4 plus this 30 is 34,
 * which looked like a match.  It is not established: tripling this value did
 * not move the read rate, and the reason turned out to be that the rate
 * measurement was taken over 5 ms - shorter than a frame - so it measured the
 * active rate and could not see blanking at all.  Measured over 100 ms the
 * bridge draws 213 MB/s against the 210 the timing calls for, so it does pause
 * for vertical blanking and that whole line of reasoning was an artefact.
 */
#ifndef P4_PANEL_VFP
#define P4_PANEL_VFP            P4_BOARD_PANEL_VFP
#endif
/* D1001: 40 MHz, from the working firmware's D1001_LCD_DPI_CLOCK_MHZ. */
#ifndef P4_PANEL_DPI_MHZ
#define P4_PANEL_DPI_MHZ        P4_BOARD_PANEL_DPI_MHZ
#endif
/*
 * Two panel profiles, because the evidence for them is split.
 *
 * P4_PANEL_565 selects what the firmware that drives this board uses:
 * LCD_COLOR_FMT_RGB565, bits_per_pixel 16, 40 MHz, 1000 Mbit/s, vertical back
 * porch 30.  That configuration demonstrably works in that firmware.  In this
 * port it transmits - lanes out of stop state, no payload error - and the
 * panel stays black.
 *
 * The default is the 24-bit profile, which is the only configuration that has
 * ever put an image on this panel from this port: 80 MHz, 1500 Mbit/s, and the
 * vertical back porch of 12 that the component's header macro specifies.  The
 * image is doubled down the panel's height and banded, so the profile is not
 * right either; it is the one with something to work on.
 *
 * Neither is adopted as correct.  Recording both, with what each produces, is
 * the honest state: an unexplained difference between this port and a working
 * firmware on identical hardware is a finding, not a detail to smooth over.
 */
#ifdef P4_PANEL_24BIT
#define P4_PANEL_BPP            24
#else
#define P4_PANEL_BPP            16
#endif

/*
 * The DPI clock: PLL_F240M divided by six is exactly 40 MHz, and the source
 * selector for PLL_F240M is 1.
 */
#define P4_DSI_DPICLK_SRC_PLL240 1
/*
 * Derived rather than written twice.  The source is the 240 MHz PLL tap, and
 * the divider and the pixel clock have to agree: two constants that can drift
 * apart is how a timing calculation ends up describing a clock the hardware is
 * not running.  240 divides exactly by 80, 60, 48 and 40.
 */
#define P4_DSI_DPICLK_DIV       (240 / P4_PANEL_DPI_MHZ)
#if (240 % P4_PANEL_DPI_MHZ) != 0
#error "P4_PANEL_DPI_MHZ must divide the 240 MHz PLL tap exactly"
#endif

#define P4_DSI_INT_ST0          0x0BC
#define P4_DSI_INT_ST1          0x0C0

/* The DSI data types this port sends. */
#define P4_DSI_DT_DCS_SW_0P     0x05
#define P4_DSI_DT_DCS_SW_1P     0x15
#define P4_DSI_DT_DCS_READ_0    0x06
#define P4_DSI_DT_DCS_LW        0x39
#define P4_DSI_DT_SET_MAX_RET   0x37

/*
 * The DSI link, derived from one number.
 *
 * Everything here used to be written out separately and they disagreed: the
 * PLL dividers assumed a 40 MHz reference, the range selector said 1000
 * Mbit/s, and the horizontal scale factor said a 125 MHz byte clock, while the
 * hardware was running at half of that.  Correcting one of them in isolation
 * changed which symptom appeared.  So the rate is stated once and the rest is
 * computed, and changing it changes all of them together.
 *
 * The reference is 40 MHz, and that is now measured rather than assumed.
 *
 * ESP-IDF's name for the source it selects on pre-3.0 silicon,
 * MIPI_DSI_PHY_PLLREF_CLK_SRC_DEFAULT_LEGACY, is PLL_F20M, so 20 MHz was
 * tried: N=2 and M=50 would then be 500 Mbit/s, and the whole link was
 * re-derived for that - dividers, range selector 0x07, horizontal scale 25/16.
 * The result was the clock lane refusing to leave stop state, where with the
 * range selector for 1000 Mbit/s it leaves it.  The selector has to match the
 * rate the lanes actually run at, so the lanes run at 1000, so the reference
 * is 40 MHz.
 *
 * B3's values were therefore right and B3's reasoning was still wrong: it
 * took a locking PLL as evidence for the reference frequency, and a loop that
 * closes says nothing about the frequency it closed on.  What settled it was
 * changing the rate and watching a lane state change, which is a measurement.
 *
 * The dividers follow ESP-IDF's own search: f_vco = M/N * f_ref with M even
 * and f_ref/N between 5 and 40 MHz, taking the smallest N that makes M even.
 * The range selector is a lookup in the PHY's table rather than a formula, so
 * it is written per rate and the build fails for a rate with no entry.
 */
#ifndef P4_DSI_PLLREF_MHZ
#ifdef P4_BOARD_DSI_PLLREF_MHZ
#define P4_DSI_PLLREF_MHZ       P4_BOARD_DSI_PLLREF_MHZ
#else
#define P4_DSI_PLLREF_MHZ       40
#endif
#endif
/*
 * 2026-10-03, JC1060P470C: the 40 MHz conclusion above does not survive a
 * register comparison.  JTAG snapshots of both the vendor firmware and this
 * port show peri_clk_ctrl02.mipi_dsi_dphy_clk_src_sel = 0, which on pre-v3
 * silicon is PLL_F20M, and ref_clk_ctrl1.ref_20m_clk_div_num = 23, i.e.
 * 480 / 24 = 20 MHz; ESP-IDF computes N against 20 MHz there.  With N for
 * 40 MHz every lane runs at half the stated rate.  The JC1060P470C profile
 * therefore sets 20; the D1001 keeps 40 until its own display is re-measured
 * (ROADMAP D2 evidence, open point).
 */
#if P4_DSI_PLLREF_MHZ != 40 && P4_DSI_PLLREF_MHZ != 20
#error "P4_DSI_PLLREF_MHZ: PLL dividers are tabulated for 20 and 40 MHz only"
#endif

/* From the board profile; the rate is the one thing to change. */
#define P4_DSI_LANES            P4_BOARD_PANEL_LANES
/*
 * Overridable, so the profiles can be crossed.
 *
 * The two panel profiles differ in four things at once - colour depth, pixel
 * clock, lane rate and vertical back porch - and every test so far moved all
 * four together.  The 24-bit profile is accepted by the panel and overruns the
 * host's payload fifo; the 16-bit one is clean and the panel rejects it.  Which
 * of the four decides acceptance cannot be read off that, only from crossing
 * them one at a time.
 */
/*
 * 1500, and the lane rate is what the panel judges.
 *
 * Crossed one at a time against the panel's own answer to DCS 0x0A, taken
 * after the video handover:
 *
 *   RGB565  40 MHz  1000 Mbit/s   rejects        transmit side clean
 *   RGB565  80 MHz  1500 Mbit/s   accepts        DPI_PLD_WR_ERR
 *   RGB565  40 MHz  1500 Mbit/s   accepts        clean
 *
 * (2026-10-04: these rates were all half the stated value, because N was
 * computed for a 40 MHz PLL reference that is really 20 MHz; the D1001 now
 * runs its vendor's 1000 Mbit/s in burst mode against 20 MHz, without the
 * row-phase workaround.  The table is history.)
 *
 * So the lane rate decides acceptance and the pixel clock decides whether the
 * host can keep up.  1500 with 40 gets both, and it is the only combination
 * tried that does.  Note that this is not the vendor firmware's pairing - that
 * runs 1000 with 40, which this port cannot get the panel to accept, and why
 * remains unexplained.
 */
#ifndef P4_DSI_LANE_MBPS
#define P4_DSI_LANE_MBPS        P4_BOARD_PANEL_LANE_MBPS
#endif

#if P4_DSI_LANE_MBPS == 1500
/*
 * The vendor bus configuration for this panel is 1500 Mbit/s over two lanes.
 * N and M follow from ESP-IDF's own search - the first even M whose
 * ref * M / N lands on the target, walking N from 1 - which for a 40 MHz
 * reference gives N 4 and M 150 exactly.  The range code is the table entry
 * [1450,1500] in soc_mipi_dsi_phy_pll_ranges.
 */
#define P4_DSI_PLL_N            (P4_DSI_PLLREF_MHZ == 40 ? 4 : 2)
#define P4_DSI_PLL_M            150
#define P4_DSI_HS_FREQ_SEL      0x3C
#elif P4_DSI_LANE_MBPS == 1000
#define P4_DSI_PLL_N            (P4_DSI_PLLREF_MHZ == 40 ? 2 : 1)  /* ref * 50 / N */
#define P4_DSI_PLL_M            50
#define P4_DSI_HS_FREQ_SEL      0x2A    /* the [1000,1050) row */
#elif P4_DSI_LANE_MBPS == 750
/*
 * The JC1060P470C vendor rate.  ESP-IDF's search walks N from 1: N 1, 3 and
 * 6 give even M but miss 750 by 30 or 3.3 MHz, N 8 gives M 150 exactly
 * (f_ref/N = 5 MHz, the lowest the PHY allows).  Range code: the [750,800)
 * row of soc_mipi_dsi_phy_pll_ranges.
 */
#define P4_DSI_PLL_N            (P4_DSI_PLLREF_MHZ == 40 ? 8 : 4)  /* 20 MHz: N 4 */
#define P4_DSI_PLL_M            150
#define P4_DSI_HS_FREQ_SEL      0x19
#elif P4_DSI_LANE_MBPS == 500
#define P4_DSI_PLL_N            (P4_DSI_PLLREF_MHZ == 40 ? 4 : 2)  /* ref * 50 / N */
#define P4_DSI_PLL_M            50
#define P4_DSI_HS_FREQ_SEL      0x07    /* the [500,550) row */
#else
#error "no PHY frequency range recorded for this lane rate"
#endif

/*
 * Pixels to lane byte clocks.  The host counts horizontal time in byte clocks
 * and the panel's timing is in pixels, so every horizontal value crosses this:
 * x * lane_rate / (8 * dpi_clock), rounded.  At 500 Mbit/s that is 25/16, at
 * 1000 it is 25/8, and getting it from the rate is what keeps the two from
 * drifting apart again.
 */
#define P4_DSI_PX_TO_BYTECLK(x) \
    (((unsigned long)(x) * P4_DSI_LANE_MBPS + 4UL * P4_PANEL_DPI_NOMINAL_MHZ) \
     / (8UL * P4_PANEL_DPI_NOMINAL_MHZ))

/*
 * ESP-IDF times the host against the pixel clock the board asks for, not the
 * one the integer divider delivers, and shortens the bridge's line so both
 * sides still take the same time per line (mipi_dsi_hal_host_dpi_set_
 * horizontal_timing: bridge HFP += round(real/expected * htotal) - htotal).
 * A board whose vendor clock does not divide 240 MHz names it as
 * P4_BOARD_PANEL_DPI_NOMINAL_MHZ to get the same registers as the vendor
 * firmware; otherwise nominal and real are the same and nothing changes.
 */
#ifndef P4_PANEL_DPI_NOMINAL_MHZ
#ifdef P4_BOARD_PANEL_DPI_NOMINAL_MHZ
#define P4_PANEL_DPI_NOMINAL_MHZ P4_BOARD_PANEL_DPI_NOMINAL_MHZ
#else
#define P4_PANEL_DPI_NOMINAL_MHZ P4_PANEL_DPI_MHZ
#endif
#endif
#define P4_PANEL_HTOTAL         (P4_PANEL_H_RES + P4_PANEL_HSYNC \
                                 + P4_PANEL_HBP + P4_PANEL_HFP)
#define P4_BRG_HTOTAL           ((P4_PANEL_HTOTAL * P4_PANEL_DPI_MHZ \
                                  + P4_PANEL_DPI_NOMINAL_MHZ / 2) \
                                 / P4_PANEL_DPI_NOMINAL_MHZ)

/*
 * The crystal.  A SoC fact rather than a PSRAM one, which is where it lived
 * until I2C needed it: it is the one clock on this chip that no divider this
 * port sets can move, which is why the I2C bus is timed from it.
 */
#define P4_XTAL_HZ              40000000UL

/*
 * The two register accessors every file here uses.
 *
 * They lived in psram.h while PSRAM was the only thing reaching registers
 * this directly.  Moved here when the clock tree needed them, because a
 * CPU-clock file including a PSRAM header reads as a mistake even when it
 * compiles.
 */
#define P4_ALWAYS_INLINE __attribute__((always_inline)) static inline

P4_ALWAYS_INLINE void p4_w32(unsigned long a, unsigned long v)
{
    *(volatile unsigned long *)a = v;
}

P4_ALWAYS_INLINE unsigned long p4_r32(unsigned long a)
{
    return *(volatile unsigned long *)a;
}

/*
 * The SoC clock tree's root dividers.
 *
 * This port configured no CPU clock at all and inherited what the
 * second-stage bootloader left, which measured 90 MHz on 2026-08-23: the CPLL
 * runs at 360 and the CPU divider sits at four.  ESP-IDF's own comment names
 * the only three configurations the constraints allow, MEM_CLK at most 200 MHz
 * and APB_CLK at most 100:
 *
 *   CPLL     CPU_CLK      MEM_CLK      SYS_CLK      APB_CLK
 *   360  /1      360  /2      180  /1      180  /2       90
 *   360  /2      180  /1      180  /1      180  /2       90
 *   360  /4       90  /1       90  /1       90  /1       90
 *
 * Each stage divides the one before it, so the third row is what a boot
 * arrives in and the first is what this port wants.  Note APB ends at 90
 * either way, which is why nothing clocked from it has to be reconfigured.
 *
 * Upscaling has to move APB first and CPU last, and downscaling the reverse.
 * Otherwise an intermediate state exists in which APB or MEM is above its
 * limit while the CPU is already fast, and a peripheral access in that window
 * is a fault with no obvious cause.  ESP-IDF says the hardware may silently
 * correct an illegal divider without reflecting it in the register, which
 * would leave the real frequencies unknowable; that is the reason the order
 * matters rather than merely being tidy.
 */
#define P4_CLKRST_ROOT_CLK_CTRL0    (P4_HP_SYS_CLKRST_BASE + 0x4)
#define   P4_SOC_CLK_DIV_UPDATE     (1UL << 4)
#define   P4_CPU_CLK_DIV_NUM_SHIFT  5
#define   P4_CPU_CLK_DIV_NUM_MASK   (0xFFUL << P4_CPU_CLK_DIV_NUM_SHIFT)
#define   P4_CPU_CLK_DIV_NUMER_SHIFT 13
#define   P4_CPU_CLK_DIV_NUMER_MASK (0xFFUL << P4_CPU_CLK_DIV_NUMER_SHIFT)
#define   P4_CPU_CLK_DIV_DENOM_SHIFT 21
#define   P4_CPU_CLK_DIV_DENOM_MASK (0xFFUL << P4_CPU_CLK_DIV_DENOM_SHIFT)

#define P4_CLKRST_ROOT_CLK_CTRL1    (P4_HP_SYS_CLKRST_BASE + 0x8)
#define   P4_MEM_CLK_DIV_NUM_SHIFT  0
#define   P4_MEM_CLK_DIV_NUM_MASK   (0xFFUL << P4_MEM_CLK_DIV_NUM_SHIFT)
#define   P4_SYS_CLK_DIV_NUM_SHIFT  24
#define   P4_SYS_CLK_DIV_NUM_MASK   (0xFFUL << P4_SYS_CLK_DIV_NUM_SHIFT)

#define P4_CLKRST_ROOT_CLK_CTRL2    (P4_HP_SYS_CLKRST_BASE + 0xC)
#define   P4_APB_CLK_DIV_NUM_SHIFT  16
#define   P4_APB_CLK_DIV_NUM_MASK   (0xFFUL << P4_APB_CLK_DIV_NUM_SHIFT)

/* Which root the HP domain runs from: 0 XTAL, 1 CPLL, 2 the fast RC */
#define P4_LP_CLKRST_HP_CLK_CTRL_R  (P4_LP_CLKRST_BASE + 0x40)
#define   P4_HP_ROOT_SRC_MASK       0x3UL
#define   P4_HP_ROOT_SRC_XTAL       0
#define   P4_HP_ROOT_SRC_CPLL       1
#define P4_LP_SDMMC_RST_CTRL    0x004C
#define  P4_LP_SDMMC_RST_EN     (1U << 28)

/* D1001's SD I/O rail is ESP32-P4 LDO channel 4 at the 3.3-V bypass. */
#define P4_PMU_BASE             (P4_LPAON_BASE + 0x5000)
#define P4_PMU_LDO4_CTRL        0x01D8
#define P4_PMU_LDO4_ANA         0x01DC
#define  P4_PMU_LDO_FORCE_SW    (1U << 7)
#define  P4_PMU_LDO_XPD         (1U << 8)
#define  P4_PMU_LDO_TIEH_SEL_M  (7U << 9)
#define  P4_PMU_LDO_3V3         (1U << 14)
#define  P4_PMU_LDO_EN_VDET     (1U << 26)

#endif /* ESP32P4_HARDWARE_H */
