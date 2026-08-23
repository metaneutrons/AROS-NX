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
 * Which of the two the kernel debug output goes to. The USB peripheral
 * is the default because it is what the reTerminal D1001 brings out;
 * UART0 needs an adapter on the board's pins, and is selected by
 * defining this to 0.
 */
#ifndef P4_CONSOLE_USB
#define P4_CONSOLE_USB          1
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

#define P4_SD_D0_GPIO           39
#define P4_SD_D1_GPIO           40
#define P4_SD_D2_GPIO           41
#define P4_SD_D3_GPIO           42
#define P4_SD_CLK_GPIO          43
#define P4_SD_CMD_GPIO          44
#define P4_SD_DETECT_GPIO       45
#define P4_SD_POWER_GPIO        46

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
