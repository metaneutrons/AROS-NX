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
#define P4_HP_SYS_CLKRST_BASE       0x500E6000UL

/* Clock and reset control for the pair */
#define P4_CLKRST_SOC_CLK_CTRL0     (P4_HP_SYS_CLKRST_BASE + 0x14)
#define   P4_PSRAM_SYS_CLK_EN       (1UL << 31)
#define P4_CLKRST_PERI_CLK_CTRL00   (P4_HP_SYS_CLKRST_BASE + 0x30)
#define   P4_PSRAM_PLL_CLK_EN       (1UL << 14)
#define   P4_PSRAM_CORE_CLK_EN      (1UL << 15)
#define P4_CLKRST_PERI_CLK_CTRL01   (P4_HP_SYS_CLKRST_BASE + 0x34)
#define   P4_PSRAM_CLK_SRC_SHIFT    12
#define   P4_PSRAM_CLK_SRC_MASK     (3UL << P4_PSRAM_CLK_SRC_SHIFT)
#define     P4_PSRAM_CLK_SRC_XTAL   0   /* 40 MHz, always running */
#define     P4_PSRAM_CLK_SRC_MPLL   1   /* wants the MPLL brought up first */
#define     P4_PSRAM_CLK_SRC_SPLL   2
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
#define P4_PSRAM_PAGE_SIZE_2048     3

/*
 * The data strobe. A double-transfer-rate read has no reference edge
 * without it, so the controller starts a transaction that never finishes -
 * which is a hang rather than an error, and the reason this is not
 * optional. Two pins, one per half of the 16-bit bus.
 *
 * The drive strength of the other eighteen pins is deliberately left at
 * whatever reset chose. It is a signal integrity question, and at 20 MHz
 * on a board that runs this part at 200 MHz there is margin to spare; it
 * belongs with the calibration when the clock goes up.
 */
#define P4_IOMUX_MSPI_PIN_BASE      (0x500C0000UL + 0x21200)
#define P4_IOMUX_PSRAM_DQS_0        (P4_IOMUX_MSPI_PIN_BASE + 0x3C)
#define P4_IOMUX_PSRAM_DQS_1        (P4_IOMUX_MSPI_PIN_BASE + 0x68)
#define   P4_IOMUX_DQS_XPD          (1UL << 0)

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
#define P4_PSRAM_CS_MASK            (1UL << 1)

/* Mode register access on the AP hex part */
#define P4_PSRAM_REG_READ           0x4040
#define P4_PSRAM_REG_WRITE          0xC0C0
#define P4_PSRAM_RD_REG_DUMMY_SLOW  (2 * (5 - 1))
#define P4_PSRAM_MR1_VENDOR_MASK    0x1F
#define P4_PSRAM_VENDOR_AP          0x0D

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

static inline void p4_w32(unsigned long a, unsigned long v)
{
    *(volatile unsigned long *)a = v;
}

static inline unsigned long p4_r32(unsigned long a)
{
    return *(volatile unsigned long *)a;
}

unsigned long krnPSRAMClockUp(unsigned long target_hz);
void krnPSRAMConfigure(void);
int krnPSRAMIdentify(unsigned char *vendor, unsigned char *density);

#endif /* ESP32P4_PSRAM_H */
