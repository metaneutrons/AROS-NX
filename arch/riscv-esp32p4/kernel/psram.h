#ifndef ESP32P4_PSRAM_H
#define ESP32P4_PSRAM_H

#include <inttypes.h>
/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: MSPI and external PSRAM registers on the ESP32-P4.

    The register names and offsets here are the ones ESP-IDF's own headers
    carry for this part, read as the documentation they are rather than
    copied as code: nothing below is an ESP-IDF source file, and the
    sequence that uses them is this port's own. Where a value needed
    explaining, the explanation is here instead of a reference to a
    revision of a manual nobody has to hand.

    These are the pre-revision-3 definitions. ESP-IDF keeps two sets, and
    treats a build for one as incompatible with the other; this board is
    v1.3, so it is the hw_ver1 layout throughout. Anything here is suspect
    on v3 silicon.
*/

/* Peripheral bases. MSPI2 drives the PSRAM data path, MSPI3 its mode
   registers - two controllers on one bus, which is why so much below is
   done twice. */
#define P4_MSPI2_BASE               0x5008E000UL
#define P4_MSPI3_BASE               0x5008F000UL

/*
 * The MPLL, which is the bus clock's only usable source.
 *
 * Taking the bus clock off XTAL instead looked like a way to skip this
 * whole block, and it is not: ESP-IDF's slowest configuration still runs
 * the MPLL at 400 MHz and divides that by twenty. The divider sits behind
 * the PLL, and the controller's own clock enable is named for it. What a
 * low bus clock saves is the read timing calibration, not the PLL.
 *
 * The PLL's own dividers are not memory mapped. They sit on an internal
 * configuration bus that the chip calls regi2c, reached through a master
 * in the low power peripheral block; a write there is four fields in one
 * register and a poll on a busy bit. The registers for that master and
 * the field layout of the MPLL's three configuration bytes follow.
 */
#define P4_LPPERI_BASE              0x50120000UL
#define P4_I2C_ANA_MST_BASE         0x50124000UL

/* Power and clock gate for the PLL itself */
#define P4_PMU_RF_PWC               (P4_PMU_BASE + 0x15C)
#define   P4_PMU_MSPI_PHY_XPD       (1UL << 24)
#define P4_LP_CLKRST_HP_CLK_CTRL    (P4_LP_CLKRST_BASE + 0x40)
#define   P4_HP_MPLL_500M_CLK_EN    (1UL << 28)

/* The PLL calibrates itself; this is the handshake for it */
#define P4_CLKRST_ANA_PLL_CTRL0     (P4_HP_SYS_CLKRST_BASE + 0xBC)
#define   P4_MSPI_CAL_END           (1UL << 8)
#define   P4_MSPI_CAL_STOP          (1UL << 9)

/*
 * The 160 MHz reference clock, which is what the configuration bus master
 * below is told to run from. Reset enables it and something before this
 * code runs turns it off again, so it has to be turned back on: the
 * reference ESP-IDF build has it on, an AROS boot through the same
 * bootloader has it off, and that was the only difference left between a
 * working PSRAM bring-up and a hanging one.
 */
#define P4_CLKRST_REF_CLK_CTRL2     (P4_HP_SYS_CLKRST_BASE + 0x2C)
#define   P4_REF_160M_CLK_EN        (1UL << 0)

/* The regi2c master. Its clock gate is in the low power peripheral block,
   its own registers are separate, and it needs the 160 MHz source
   selected before it will move a byte. */
#define P4_LPPERI_CLK_EN            (P4_LPPERI_BASE + 0x0)
#define   P4_CK_EN_LP_I2CMST        (1UL << 27)
#define P4_I2C_ANA_MST_I2C0_CTRL    (P4_I2C_ANA_MST_BASE + 0x00)
#define P4_I2C_ANA_MST_ANA_CONF1    (P4_I2C_ANA_MST_BASE + 0x1C)
#define P4_I2C_ANA_MST_ANA_CONF2    (P4_I2C_ANA_MST_BASE + 0x20)
#define P4_I2C_ANA_MST_CLK160M      (P4_I2C_ANA_MST_BASE + 0x34)
#define   P4_CLK_I2C_MST_SEL_160M   (1UL << 0)
#define   P4_I2C_ANA_CONF_MASK      0x00FFFFFFUL
/* One bit per slave block, and only one may be selected at a time */
#define   P4_REGI2C_MPLL_MST_SEL    (1UL << 9)
/* The control register: slave, register, data, direction, busy */
#define   P4_REGI2C_SLAVE_SHIFT     0
#define   P4_REGI2C_ADDR_SHIFT      8
#define   P4_REGI2C_DATA_SHIFT      16
#define   P4_REGI2C_WRITE           (1UL << 24)
#define   P4_REGI2C_BUSY            (1UL << 25)

/* The MPLL's three configuration bytes on that bus */
#define P4_REGI2C_MPLL              0x63
#define   P4_MPLL_IR_CAL_RSTB_REG   1
#define     P4_MPLL_IR_CAL_RSTB     (1U << 5)
#define   P4_MPLL_DIV_REG           2
#define     P4_MPLL_REF_DIV_SHIFT   0    /* three bits */
#define     P4_MPLL_DIV_SHIFT       3    /* five bits  */
#define   P4_MPLL_DHREF_REG         3
#define     P4_MPLL_DHREF_SHIFT     4    /* two bits, both set */

/*
 * 400 MHz is what ESP-IDF uses for every PSRAM speed except 80 and 250
 * MHz, and 400 divides evenly by 20, 40 and 200. Running the PLL at the
 * same rate for the slow bring-up and the fast bus means the step up to
 * 200 MHz is a divider and a calibration, not another PLL change.
 *
 * MPLL = XTAL * (div + 1) / (ref_div + 1), with ref_div fixed at one:
 * 40 MHz * 20 / 2 = 400 MHz, so div is 19.
 */
#define P4_PSRAM_MPLL_HZ            400000000UL

/* Clock and reset control for the pair */
#define P4_CLKRST_SOC_CLK_CTRL0     (P4_HP_SYS_CLKRST_BASE + 0x14)
#define   P4_PSRAM_SYS_CLK_EN       (1UL << 31)
#define P4_CLKRST_PERI_CLK_CTRL00   (P4_HP_SYS_CLKRST_BASE + 0x30)
#define   P4_PSRAM_CLK_SRC_SHIFT    12
#define   P4_PSRAM_CLK_SRC_MASK     (3UL << P4_PSRAM_CLK_SRC_SHIFT)
#define     P4_PSRAM_CLK_SRC_XTAL   0   /* 40 MHz, always running */
#define     P4_PSRAM_CLK_SRC_MPLL   1   /* wants the MPLL brought up first */
#define     P4_PSRAM_CLK_SRC_SPLL   2
#define     P4_PSRAM_CLK_SRC_CPLL   3
#define   P4_PSRAM_PLL_CLK_EN       (1UL << 14)
#define   P4_PSRAM_CORE_CLK_EN      (1UL << 15)
/* The controller core's own divider off the selected source, held as
   value minus one. Reset leaves it at divide by one, which is what the
   bus clock counters below then divide further; nothing here changes it. */
#define   P4_PSRAM_CORE_CLK_DIV_SH  16
#define   P4_PSRAM_CORE_CLK_DIV_M   (0xFFUL << P4_PSRAM_CORE_CLK_DIV_SH)
#define P4_CLKRST_HP_RST_EN0        (P4_HP_SYS_CLKRST_BASE + 0xC0)
#define   P4_RST_EN_DUAL_MSPI_AXI   (1UL << 23)
#define   P4_RST_EN_DUAL_MSPI_APB   (1UL << 25)

/*
 * The bus clock is a divider off the selected source, and it is the only
 * thing that separates a 20 MHz bring-up from a 200 MHz one. Both
 * controllers carry the same three counters in different registers: N is
 * the period, H the half point, L the low time, each held as value-1.
 */
#define P4_MSPI2_SRAM_CLK           (P4_MSPI2_BASE + 0x50)
#define P4_MSPI3_CLOCK              (P4_MSPI3_BASE + 0x14)
#define   P4_SCLKCNT_L_SHIFT        0
#define   P4_SCLKCNT_H_SHIFT        8
#define   P4_SCLKCNT_N_SHIFT        16
#define   P4_SCLK_EQU_SYSCLK        (1UL << 31)  /* divider of one */

/*
 * Chip select. Only MSPI3 has these bits; on MSPI2 the AXI path picks the
 * device elsewhere. Reset disables chip select 1, which is the one the
 * PSRAM sits on, and the mask ROM's transaction call does not clear it -
 * so a transaction runs to completion with the chip never selected and
 * every read comes back as a floating bus. This is not a detail of the
 * chip; it is the difference between talking to it and not.
 */
#define P4_MSPI3_MISC               (P4_MSPI3_BASE + 0x34)
#define   P4_MISC_CS0_DIS           (1UL << 0)
#define   P4_MISC_CS1_DIS           (1UL << 1)

/*
 * The analogue side of the bus. All of it lives in MSPI2's register block,
 * including MSPI3's DLL, which is why there is only one base here.
 */
#define P4_MSPI_SMEM_ECC_CTRL       (P4_MSPI2_BASE + 0x174)
#define   P4_SMEM_PAGE_SIZE_SHIFT   18          /* 0:256 1:512 2:1024 3:2048 */
#define   P4_SMEM_PAGE_SIZE_MASK    (3UL << P4_SMEM_PAGE_SIZE_SHIFT)
#define P4_MSPI_TIMING_CALI         (P4_MSPI2_BASE + 0x180)   /* MSPI3's DLL */
#define P4_MSPI_SMEM_TIMING_CALI    (P4_MSPI2_BASE + 0x190)   /* MSPI2's DLL */
#define   P4_DLL_TIMING_CALI        (1UL << 5)
#define P4_MSPI_SMEM_AC             (P4_MSPI2_BASE + 0x1A0)
#define   P4_SMEM_CS_SETUP          (1UL << 0)
#define   P4_SMEM_CS_HOLD           (1UL << 1)
#define   P4_SMEM_CS_SETUP_TIME_SH  2
#define   P4_SMEM_CS_SETUP_TIME_M   (0x1FUL << P4_SMEM_CS_SETUP_TIME_SH)
#define   P4_SMEM_CS_HOLD_TIME_SH   7
#define   P4_SMEM_CS_HOLD_TIME_M    (0x1FUL << P4_SMEM_CS_HOLD_TIME_SH)
#define   P4_SMEM_CS_HOLD_DELAY_SH  25
#define   P4_SMEM_CS_HOLD_DELAY_M   (0x3FUL << P4_SMEM_CS_HOLD_DELAY_SH)
#define   P4_SMEM_SPLIT_TRANS_EN    (1UL << 31)

/*
 * Timings for the AP hex part on this board. Setup, hold and hold delay do
 * not depend on the clock; the read and write latencies do, and are the
 * values the mode registers are told about. 2 and 2 cover everything at or
 * below 80 MHz, 4 and 1 belong to 200 MHz and 6 and 3 to 250, which only
 * v3 silicon reaches.
 */
#define P4_PSRAM_CS_SETUP_TIME      4
#define P4_PSRAM_CS_HOLD_TIME       4
#define P4_PSRAM_CS_HOLD_DELAY      3
#define P4_PSRAM_RD_LATENCY_SLOW    2
#define P4_PSRAM_WR_LATENCY_SLOW    2
#define P4_PSRAM_RD_LATENCY_FAST    4
#define P4_PSRAM_WR_LATENCY_FAST    1
#define P4_PSRAM_PAGE_SIZE_2048     3

/*
 * The data strobe. A double-transfer-rate read has no reference edge
 * without it, so the controller starts a transaction that never finishes -
 * which is a hang rather than an error, and the reason this is not
 * optional. Two pins, one per half of the 16-bit bus.
 *
 * The drive strength of all twenty pins has to be raised, and leaving it
 * at what reset chose was a mistake: reset selects zero, the weakest of
 * four settings, and at that setting the chip does not answer at all. The
 * earlier reading of this as a signal integrity question with margin to
 * spare at 20 MHz confused two different things. Integrity is about the
 * shape of an edge that arrives; this is about whether the driver moves
 * the line far enough to be seen. ESP-IDF raises all twenty to two before
 * its first transaction and does not vary it with the clock, so two it is
 * here as well.
 */
#define P4_IOMUX_MSPI_PIN_BASE      (0x500C0000UL + 0x21200)
#define P4_IOMUX_PSRAM_DQS_0        (P4_IOMUX_MSPI_PIN_BASE + 0x3C)
#define P4_IOMUX_PSRAM_DQS_1        (P4_IOMUX_MSPI_PIN_BASE + 0x68)
#define   P4_IOMUX_DQS_XPD          (1UL << 0)
#define P4_PSRAM_PIN_DRV            2

/*
 * Every PSRAM pin's control register, as an offset from the block above
 * and the position of its two drive strength bits. The eighteen data and
 * control pins carry the field at bit 12 and the two strobes at bit 15,
 * which is the only reason this is a table of pairs rather than a range.
 * In order: D, Q, WP, HOLD, DQ4 to DQ15, DQS0, DQS1, CK, CS.
 */
#define P4_PSRAM_PIN_DRV_TABLE                                  \
    { 0x1C, 12 }, { 0x20, 12 }, { 0x24, 12 }, { 0x28, 12 },     \
    { 0x2C, 12 }, { 0x30, 12 }, { 0x34, 12 }, { 0x38, 12 },     \
    { 0x48, 12 }, { 0x4C, 12 }, { 0x50, 12 }, { 0x54, 12 },     \
    { 0x58, 12 }, { 0x5C, 12 }, { 0x60, 12 }, { 0x64, 12 },     \
    { 0x3C, 15 }, { 0x68, 15 }, { 0x40, 12 }, { 0x44, 12 }

/*
 * Talking to the chip itself, as opposed to the controller in front of it,
 * goes through three functions in the part's own mask ROM. Their addresses
 * are fixed and published; calling them saves writing an MSPI transaction
 * engine, and code in the ROM is the chip's own firmware rather than
 * anybody's source.
 */
#define P4_ROM_SPI_CMD_CONFIG       0x4FC00108UL
#define P4_ROM_SPI_CMD_START        0x4FC0010CUL
#define P4_ROM_SPI_SET_OP_MODE      0x4FC00110UL

#define P4_MSPI_ID_DATA             2   /* the data path */
#define P4_MSPI_ID_REG              3   /* the mode registers */
#define P4_ROM_OPI_DTR_MODE         7   /* octal, double transfer rate */
/*
 * Which chip select the ROM's transaction call asserts. ESP-IDF passes
 * 1 << 1 and that is what the hardware wants: measured against a working
 * reference in one session, a mask of 2 reads real data and a mask of 1
 * reads a floating bus. The ROM header's wording ("0 for cs0, 1 for cs1")
 * invites the other reading; it is wrong, and this port followed it once.
 */
#define P4_PSRAM_CS_INDEX           (1UL << 1)

/* Mode register and memory access on the AP hex part. The three dummy
   lengths differ from each other and all three depend on the clock; these
   are the values for 80 MHz and below. */
#define P4_PSRAM_REG_READ           0x4040
#define P4_PSRAM_REG_WRITE          0xC0C0
#define P4_PSRAM_SYNC_READ          0x0000
#define P4_PSRAM_SYNC_WRITE         0x8080
#define P4_PSRAM_RD_REG_DUMMY_SLOW  (2 * (5 - 1))
#define P4_PSRAM_RD_DUMMY_SLOW      (2 * (10 - 1))
#define P4_PSRAM_WR_DUMMY_SLOW      (2 * (5 - 1))
/*
 * And the set for 200 MHz.  The read latency in cycles is the mode register
 * value doubled plus six, so 2 gives 10 and 4 gives 14, and the dummy length
 * is twice that less one because a DTR transfer moves two bits per cycle.
 * The two have to agree: a controller that stops driving dummy cycles before
 * the chip starts driving data reads whatever the bus is floating at.
 *
 * A longer-than-necessary latency is harmless at a lower clock, which is what
 * makes the whole bring-up sequence able to run at 20 MHz with the fast set
 * already programmed.  The reverse is not true, which is why the set is
 * chosen from the target and not raised afterwards.
 */
#define P4_PSRAM_RD_REG_DUMMY_FAST  (2 * (7 - 1))
#define P4_PSRAM_RD_DUMMY_FAST      (2 * (14 - 1))
#define P4_PSRAM_WR_DUMMY_FAST      (2 * (7 - 1))
#define P4_PSRAM_TEST_PATTERN       0x5A6B7C8DUL
#define P4_PSRAM_MR1_VENDOR_MASK    0x1F
#define P4_PSRAM_VENDOR_AP          0x0D
/*
 * Mode register 2's low three bits, as sizes. The codes are not in order
 * and 2 and 4 are not used, which is why this is a table rather than a
 * shift.
 */
#define P4_PSRAM_MR2_DENSITY_MASK   0x07
#define P4_PSRAM_SIZE_TABLE         { 0, 4, 0, 8, 0, 16, 64, 32 }   /* MB */

/*
 * The window at 0x48000000 and the table that puts the chip in it.
 *
 * The PSRAM has its own address translation table, separate from the
 * flash's, in MSPI2's register block: one register selects an entry, the
 * next carries its contents. Pages are 64 KB and there is no other size on
 * this part, so an entry's contents are a physical page number plus two
 * flags, and the entry's index is a virtual page number counted from the
 * bottom of the window.
 *
 * Nothing here disables the cache. ESP-IDF's own mapping code does not
 * either, and it must not: this port executes from flash, and the flash
 * path runs through the cache that would be turned off. The per-range
 * cache bus enables that older parts need do not exist here; on this part
 * they are no-ops.
 */
/*
 * The memory-mapped path, which is a separate matter from the transactions
 * above.
 *
 * Those go through MSPI3 and are how the chip is configured. A load or
 * store to the window is served by MSPI2 on its own, filling a cache line,
 * and it has to be told what a read and a write look like: which command,
 * how long the address is, how many dummy cycles, that the bus is octal
 * for command and address and sixteen bits wide for data, and that it may
 * answer AXI requests at all. Until the last of those is set, a store to
 * the window is a bus error however correct the translation table is.
 */
#define P4_MSPI2_CTRL1              (P4_MSPI2_BASE + 0x0C)
#define   P4_MEM_AR_SPLICE_EN       (1UL << 25)
#define   P4_MEM_AW_SPLICE_EN       (1UL << 26)
#define P4_MSPI2_CACHE_FCTRL        (P4_MSPI2_BASE + 0x3C)
#define   P4_MEM_AXI_REQ_EN         (1UL << 0)
#define   P4_CLOSE_AXI_INF_EN       (1UL << 31)
#define P4_MSPI2_CACHE_SCTRL        (P4_MSPI2_BASE + 0x40)
#define   P4_CACHE_USR_SADDR_4BYTE  (1UL << 0)
#define   P4_USR_WR_SRAM_DUMMY      (1UL << 3)
#define   P4_USR_RD_SRAM_DUMMY      (1UL << 4)
#define   P4_CACHE_SRAM_USR_RCMD    (1UL << 5)
#define   P4_SRAM_RDUMMY_SHIFT      6       /* six bits, value minus one */
#define   P4_SRAM_RDUMMY_MASK       (0x3FUL << P4_SRAM_RDUMMY_SHIFT)
#define   P4_SRAM_ADDR_BITLEN_SHIFT 14      /* six bits, value minus one */
#define   P4_SRAM_ADDR_BITLEN_MASK  (0x3FUL << P4_SRAM_ADDR_BITLEN_SHIFT)
#define   P4_CACHE_SRAM_USR_WCMD    (1UL << 20)
#define   P4_SRAM_OCT               (1UL << 21)
#define   P4_SRAM_WDUMMY_SHIFT      22      /* six bits, value minus one */
#define   P4_SRAM_WDUMMY_MASK       (0x3FUL << P4_SRAM_WDUMMY_SHIFT)
#define P4_MSPI2_SRAM_CMD           (P4_MSPI2_BASE + 0x44)
#define   P4_MEM_SDIN_OCT           (1UL << 18)
#define   P4_MEM_SDOUT_OCT          (1UL << 19)
#define   P4_MEM_SADDR_OCT          (1UL << 20)
#define   P4_MEM_SCMD_OCT           (1UL << 21)
#define   P4_MEM_SDUMMY_WOUT        (1UL << 23)
#define   P4_MEM_SDIN_HEX           (1UL << 26)
#define   P4_MEM_SDOUT_HEX          (1UL << 27)
#define P4_MSPI2_SRAM_DRD_CMD       (P4_MSPI2_BASE + 0x48)
#define P4_MSPI2_SRAM_DWR_CMD       (P4_MSPI2_BASE + 0x4C)
#define   P4_SRAM_CMD_VALUE_MASK    0xFFFFUL
#define   P4_SRAM_CMD_BITLEN_SHIFT  28      /* four bits, value minus one */
#define   P4_SRAM_CMD_BITLEN_MASK   (0xFUL << P4_SRAM_CMD_BITLEN_SHIFT)
#define P4_MSPI2_SMEM_DDR           (P4_MSPI2_BASE + 0xD8)
#define P4_MSPI3_DDR                (P4_MSPI3_BASE + 0xD4)
#define   P4_DDR_EN                 (1UL << 0)
#define   P4_DDR_VAR_DUMMY          (1UL << 1)
#define   P4_DDR_RDAT_SWP           (1UL << 2)
#define   P4_DDR_WDAT_SWP           (1UL << 3)

#define P4_PSRAM_ADDR_BITLEN        32

#define P4_MMU_PSRAM_INDEX          (P4_MSPI2_BASE + 0x380)
#define P4_MMU_PSRAM_CONTENT        (P4_MSPI2_BASE + 0x37C)
#define P4_MMU_PAGE_SHIFT           16      /* 64 KB, the only size */
#define P4_MMU_ENTRIES              1024    /* 64 MB of window */
#define   P4_MMU_ACCESS_PSRAM       (1UL << 10)
#define   P4_MMU_VALID              (1UL << 11)

#define P4_PSRAM_WINDOW_BASE        0x48000000UL

struct p4_rom_spi_cmd
{
    uint16_t cmd;
    uint16_t cmd_bitlen;
    uint32_t *addr;
    uint32_t addr_bitlen;
    uint32_t *tx_data;
    uint32_t tx_data_bitlen;
    uint32_t *rx_data;
    uint32_t rx_data_bitlen;
    uint32_t dummy_bitlen;
};

#define P4_XTAL_HZ                  40000000UL

/*
 * always_inline, not merely inline. These are called from P4_SRAMCODE
 * functions, and at -Os the compiler is entitled to emit one out-of-line
 * copy of a static inline and call it from everywhere - which it does, and
 * that copy lands in .flash.text. Any such call inside a window where the
 * flash cache is off would hang with no output. Nothing in the PSRAM path
 * turns the flash cache off today, so this was latent rather than broken;
 * kernel_flash.c does turn it off, and the promise P4_SRAMCODE makes has
 * to actually hold before something relies on it.
 */


int krnPSRAMMPLLUp(void);
unsigned long krnPSRAMMPLLState(void);
unsigned long krnPSRAMClockUp(unsigned long target_hz);
unsigned long krnPSRAMClockSet(unsigned long target_hz);
void krnPSRAMConfigure(void);
void krnPSRAMModeInit(void);
int krnPSRAMIdentify(unsigned char *vendor, unsigned char *density);
int krnPSRAMRoundTrip(uint32_t *back);

/*
 * What a bring-up found out, for the caller to report. size is zero if the
 * chip did not answer, in which case the other fields say how far it got.
 */
/*
 * The MSPI pin IOMUX, which is where the sampling is tuned.
 *
 * Two knobs.  The DQS phase shifts the strobe against the clock in four
 * fixed steps, 67.5, 78.75, 90 and 101.25 degrees.  The delay lines shift
 * individual pins in sixteen steps each, and the two directions - strobe
 * later, or data later - together make one axis of relative delay from -15
 * to +15 with zero in the middle.
 *
 * The register block and every field in it are identical between ESP32-P4
 * hardware versions 1 and 3; only debug GPIO helper macros differ, which is
 * worth stating because the DSI bridge registers are not identical and this
 * port has to pick a set for that one.
 */
#define P4_MSPI_IOMUX_BASE          (P4_IOMUX_BASE + 0x200)
/* d, q, wp, hold, dq4..dq7 */
#define P4_MSPI_PSRAM_GRP0(n)       (P4_MSPI_IOMUX_BASE + 0x1C + (n) * 4)
#define P4_MSPI_PSRAM_GRP0_COUNT    8
#define P4_MSPI_PSRAM_DQS0          (P4_MSPI_IOMUX_BASE + 0x3C)
/* ck, cs, dq8..dq15 */
#define P4_MSPI_PSRAM_GRP1(n)       (P4_MSPI_IOMUX_BASE + 0x40 + (n) * 4)
#define P4_MSPI_PSRAM_GRP1_COUNT    10
#define P4_MSPI_PSRAM_DQS1          (P4_MSPI_IOMUX_BASE + 0x68)

#define P4_PSRAM_PIN_DLC_SHIFT      4
#define P4_PSRAM_PIN_DLC_MASK       (0xFUL << P4_PSRAM_PIN_DLC_SHIFT)
#define P4_PSRAM_DQS_PHASE_SHIFT    1
#define P4_PSRAM_DQS_PHASE_MASK     (0x3UL << P4_PSRAM_DQS_PHASE_SHIFT)
#define P4_PSRAM_DQS_DLY90_SHIFT    7
#define P4_PSRAM_DQS_DLY90_MASK     (0xFUL << P4_PSRAM_DQS_DLY90_SHIFT)
#define P4_PSRAM_DQS_DLY270_SHIFT   17
#define P4_PSRAM_DQS_DLY270_MASK    (0xFUL << P4_PSRAM_DQS_DLY270_SHIFT)

#define P4_PSRAM_PHASE_COUNT        4       /* 67.5, 78.75, 90, 101.25 */
#define P4_PSRAM_DELAY_COUNT        31      /* -15 .. +15 relative */
#define P4_PSRAM_TUNE_WORDS         32      /* 128 bytes, the reference block */
#define P4_PSRAM_TUNE_ADDR          0x80    /* scratch; exec does not exist yet */
#define P4_PSRAM_FIFO_WORDS         16      /* 64 bytes per transaction */

/*
 * How many times each delay-line candidate is read before it counts as
 * passing.  A phase is a coarse choice and one read separates the four; a
 * delay-line step is a margin question, and a candidate that passes once and
 * fails on the hundredth read is exactly the one that must not be chosen.
 * ESP-IDF uses 1 and 100 for the same reason.
 */
#define P4_PSRAM_PHASE_TRIES        1
#define P4_PSRAM_DELAY_TRIES        100

/*
 * The bus clock to ask for.  20 MHz unless the build says otherwise, because
 * a rate becomes a default only after it has been shown to hold across cold
 * and warm boots, not when it works once.
 */
#ifndef P4_PSRAM_MHZ
#define P4_PSRAM_MHZ                20
#endif
#define P4_PSRAM_TARGET_HZ          (P4_PSRAM_MHZ * 1000000UL)

struct P4PSRAMTuning
{
    unsigned char tuned;            /* both stages found a window */
    unsigned char phase;            /* the phase index chosen */
    unsigned char phase_pass;       /* bit n set: phase n passed */
    unsigned char phase_window;     /* length of the run it was taken from */
    unsigned char delay_index;      /* the delay-line candidate chosen */
    unsigned char delay_window;     /* length of the run it sits in the middle of */
    unsigned char data_delay;       /* the pair the chosen index means */
    unsigned char dqs_delay;
    unsigned long delay_pass;       /* bit n set: candidate n passed every try */
};

struct P4PSRAMInfo
{
    unsigned long clock_hz;
    unsigned long size;
    unsigned char vendor;
    unsigned char density;
    unsigned char mpll_up;
    unsigned char round_trip;
    unsigned char fast_requested;   /* the caller asked for the tuned clock */
    unsigned char fell_back;        /* it was asked for and did not hold */
    struct P4PSRAMTuning tuning;
};

int krnPSRAMBringUp(struct P4PSRAMInfo *info, unsigned long target_hz);
void krnPSRAMAxiConfigure(void);
void krnPSRAMMap(unsigned long size);
int krnPSRAMVerify(unsigned long size, unsigned long *failed_at);

/* Used by the tuning, which lives in psram_tuning.c */
unsigned long krnPSRAMClockUp(unsigned long target_hz);
void krnPSRAMBlockWrite(uint32_t addr, const uint32_t *words, uint32_t count);
void krnPSRAMBlockRead(uint32_t addr, uint32_t *words, uint32_t count);
void krnPSRAMTuneReference(uint32_t *words, uint32_t count);
int krnPSRAMTune(struct P4PSRAMTuning *out, unsigned long fast_hz);
void krnPSRAMTuningClear(void);

#endif /* ESP32P4_PSRAM_H */
