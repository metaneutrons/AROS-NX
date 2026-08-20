/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: External PSRAM bring-up, first stage: the controller's clock.
*/

#include <inttypes.h>
#include <exec/types.h>

#include "hardware.h"
#include "kernel_intern.h"
#include "psram.h"

/*
 * Why this is P4_SRAMCODE: with ldscript-xip.lds the image's code is
 * fetched through the cache that MSPI serves, and reconfiguring MSPI while
 * executing from it is a way to stop executing anything. Everything this
 * file touches has to be reachable without that cache.
 */

/*
 * Take the pair of MSPI controllers out of reset with their clocks on, and
 * set the bus clock as close to the requested rate as the divider allows.
 *
 * XTAL rather than the MPLL as the source, deliberately. The MPLL has to
 * be brought up and told a frequency before it can be selected, and at the
 * clocks it makes possible the read timing has to be calibrated per board;
 * XTAL is running before this code does, and 40 MHz divided down needs no
 * calibration at all. The cost is the ceiling: 20 MHz here against 200 MHz
 * there, a tenth of the bandwidth and ten times the latency. That is the
 * trade this stage takes on purpose, and the divider is the only thing
 * that has to change when the MPLL and the calibration arrive.
 *
 * Returns the rate actually set, or 0 if the controller did not answer.
 */
P4_SRAMCODE unsigned long krnPSRAMClockUp(unsigned long target_hz)
{
    unsigned long div, n, clkval, readback;

    /* Module clocks first: the registers below do not answer without them */
    p4_w32(P4_CLKRST_SOC_CLK_CTRL0,
           p4_r32(P4_CLKRST_SOC_CLK_CTRL0) | P4_PSRAM_SYS_CLK_EN);
    p4_w32(P4_CLKRST_PERI_CLK_CTRL00,
           p4_r32(P4_CLKRST_PERI_CLK_CTRL00) | P4_PSRAM_PLL_CLK_EN
                                             | P4_PSRAM_CORE_CLK_EN);

    /* Reset both halves, AXI outermost, and release in the reverse order */
    p4_w32(P4_CLKRST_HP_RST_EN0,
           p4_r32(P4_CLKRST_HP_RST_EN0) | P4_RST_EN_DUAL_MSPI_AXI
                                        | P4_RST_EN_DUAL_MSPI_APB);
    p4_w32(P4_CLKRST_HP_RST_EN0,
           p4_r32(P4_CLKRST_HP_RST_EN0) & ~P4_RST_EN_DUAL_MSPI_APB);
    p4_w32(P4_CLKRST_HP_RST_EN0,
           p4_r32(P4_CLKRST_HP_RST_EN0) & ~P4_RST_EN_DUAL_MSPI_AXI);

    p4_w32(P4_CLKRST_PERI_CLK_CTRL01,
           (p4_r32(P4_CLKRST_PERI_CLK_CTRL01) & ~P4_PSRAM_CLK_SRC_MASK)
           | ((unsigned long)P4_PSRAM_CLK_SRC_XTAL << P4_PSRAM_CLK_SRC_SHIFT));

    /* Round the divider up, so the bus never runs faster than asked */
    if (target_hz == 0)
        target_hz = 20000000UL;
    div = (P4_XTAL_HZ + target_hz - 1) / target_hz;
    if (div < 1)
        div = 1;
    if (div > 64)
        div = 64;

    if (div == 1)
    {
        clkval = P4_SCLK_EQU_SYSCLK;
    }
    else
    {
        n = div - 1;
        clkval = (n << P4_SCLKCNT_N_SHIFT)
               | ((div / 2 - 1) << P4_SCLKCNT_H_SHIFT)
               | (n << P4_SCLKCNT_L_SHIFT);
    }

    p4_w32(P4_MSPI2_SRAM_CLK, clkval);
    p4_w32(P4_MSPI3_CLOCK, clkval);

    /*
     * The read-back is the test. An unclocked or still-reset controller
     * does not keep what was written to it, so a value that comes back
     * unchanged says the two steps above took effect - which is the whole
     * claim this stage makes.
     */
    readback = p4_r32(P4_MSPI2_SRAM_CLK);
    if (readback != clkval)
        return 0;

    return P4_XTAL_HZ / div;
}
