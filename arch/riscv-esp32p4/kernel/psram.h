#ifndef ESP32P4_PSRAM_H
#define ESP32P4_PSRAM_H
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

#endif /* ESP32P4_PSRAM_H */
