/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: External PSRAM bring-up: the PLL, the controller and the chip.
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
 * Bring the MPLL up at 400 MHz, which is the bus clock's only usable
 * source.
 *
 * The PLL's dividers are not memory mapped; they sit on the internal
 * configuration bus described in psram.h, so most of this function is four
 * accesses to that bus with a bounded wait around each. Every wait here is
 * bounded on purpose: the failure this replaces was a hang, and a
 * bring-up step that can hang is not an improvement on one that reports
 * that it failed.
 *
 * Returns non-zero if the PLL reported its calibration done.
 */
/*
 * The clock-dependent parameters, chosen once from the target rate.
 *
 * These were compile-time constants while there was only one clock.  They
 * are variables now rather than a parameter threaded through six functions,
 * because every one of those functions runs before exec exists and is called
 * from exactly one place in one order; a second caller would be the bug, not
 * the shared state.  In SRAM, like the code that reads them.
 */
P4_SRAMDATA static uint32_t p4_rd_latency  = P4_PSRAM_RD_LATENCY_SLOW;
P4_SRAMDATA static uint32_t p4_wr_latency  = P4_PSRAM_WR_LATENCY_SLOW;
P4_SRAMDATA static uint32_t p4_rd_reg_dummy = P4_PSRAM_RD_REG_DUMMY_SLOW;
P4_SRAMDATA static uint32_t p4_rd_dummy    = P4_PSRAM_RD_DUMMY_SLOW;
P4_SRAMDATA static uint32_t p4_wr_dummy    = P4_PSRAM_WR_DUMMY_SLOW;

/*
 * Above 80 MHz the chip needs the longer latencies.  The threshold is the
 * boundary ESP-IDF draws between its own parameter sets, and 200 MHz is the
 * only rate above it this port asks for.
 */
P4_SRAMCODE static void p4_psram_select_params(unsigned long target_hz)
{
    if (target_hz > 80000000UL)
    {
        p4_rd_latency   = P4_PSRAM_RD_LATENCY_FAST;
        p4_wr_latency   = P4_PSRAM_WR_LATENCY_FAST;
        p4_rd_reg_dummy = P4_PSRAM_RD_REG_DUMMY_FAST;
        p4_rd_dummy     = P4_PSRAM_RD_DUMMY_FAST;
        p4_wr_dummy     = P4_PSRAM_WR_DUMMY_FAST;
    }
    else
    {
        p4_rd_latency   = P4_PSRAM_RD_LATENCY_SLOW;
        p4_wr_latency   = P4_PSRAM_WR_LATENCY_SLOW;
        p4_rd_reg_dummy = P4_PSRAM_RD_REG_DUMMY_SLOW;
        p4_rd_dummy     = P4_PSRAM_RD_DUMMY_SLOW;
        p4_wr_dummy     = P4_PSRAM_WR_DUMMY_SLOW;
    }
}

P4_SRAMCODE static int p4_regi2c_idle(void)
{
    int spin = 100000;

    while ((p4_r32(P4_I2C_ANA_MST_I2C0_CTRL) & P4_REGI2C_BUSY) && --spin)
        ;

    return spin != 0;
}

/* One slave block may be selected at a time, so both selection registers
   are cleared before the MPLL is named. */
P4_SRAMCODE static void p4_regi2c_select_mpll(void)
{
    p4_w32(P4_I2C_ANA_MST_ANA_CONF2,
           p4_r32(P4_I2C_ANA_MST_ANA_CONF2) & ~P4_I2C_ANA_CONF_MASK);
    p4_w32(P4_I2C_ANA_MST_ANA_CONF1,
           p4_r32(P4_I2C_ANA_MST_ANA_CONF1) & ~P4_I2C_ANA_CONF_MASK);
    p4_w32(P4_I2C_ANA_MST_ANA_CONF2,
           p4_r32(P4_I2C_ANA_MST_ANA_CONF2) | P4_REGI2C_MPLL_MST_SEL);
}

P4_SRAMCODE static int p4_regi2c_read(unsigned char reg, unsigned char *out)
{
    p4_regi2c_select_mpll();
    if (!p4_regi2c_idle())
        return 0;

    p4_w32(P4_I2C_ANA_MST_I2C0_CTRL,
           ((unsigned long)P4_REGI2C_MPLL << P4_REGI2C_SLAVE_SHIFT)
           | ((unsigned long)reg << P4_REGI2C_ADDR_SHIFT));

    if (!p4_regi2c_idle())
        return 0;

    *out = (unsigned char)((p4_r32(P4_I2C_ANA_MST_I2C0_CTRL)
                            >> P4_REGI2C_DATA_SHIFT) & 0xFF);
    return 1;
}

P4_SRAMCODE static int p4_regi2c_write(unsigned char reg, unsigned char val)
{
    p4_regi2c_select_mpll();
    if (!p4_regi2c_idle())
        return 0;

    p4_w32(P4_I2C_ANA_MST_I2C0_CTRL,
           ((unsigned long)P4_REGI2C_MPLL << P4_REGI2C_SLAVE_SHIFT)
           | ((unsigned long)reg << P4_REGI2C_ADDR_SHIFT)
           | ((unsigned long)val << P4_REGI2C_DATA_SHIFT)
           | P4_REGI2C_WRITE);

    return p4_regi2c_idle();
}

P4_SRAMDATA unsigned long p4_mpll_trace[4];
P4_SRAMDATA unsigned long p4_mpll_spins;
P4_SRAMDATA unsigned long p4_mpll_attempts;

P4_SRAMCODE static int p4_mpll_calibrate(void)
{
    unsigned char rstb, dhref;
    unsigned long div;
    int spin;


    /*
     * The analogue master's 160 MHz source is deliberately NOT selected.
     *
     * This file used to set I2C_ANA_MST_CLK160M bit 0, and that is the one
     * write in the whole MPLL sequence that ESP-IDF does not make anywhere for
     * this chip - the register's default is zero and IDF leaves it there.  The
     * register writes still succeed at 160 MHz, which is why the bus never
     * reported a fault; what did not work was the calibration that runs over
     * the same analogue path, and MSPI_CAL_END never appeared.
     */

    /*
     * Power the PLL down and back up, rather than only up.
     *
     * The measurement that forced this: on a cold-started chip the analogue
     * registers all read back exactly what was written, MSPI_CAL_STOP was
     * cleared as ESP-IDF clears it, and MSPI_CAL_END never appeared in a
     * million polls.  The calibration was not failing, it was never starting.
     *
     * The PLL arrives powered - the bootloader leaves PMU_MSPI_PHY_XPD set and
     * MSPI_CAL_STOP set with CAL_END clear - and setting a bit that is already
     * set does nothing, so a calibration that needs the block to come up has
     * nothing to come up from.  ESP-IDF never meets this state: its own
     * rtc_clk_mpll_disable clears exactly this bit, and its PSRAM path
     * acquires the MPLL through a reference count that powers it from zero.
     *
     * A boot that followed firmware which had already calibrated found CAL_END
     * set from that run and continued, which is why this only ever failed from
     * cold and why it looked like the chip rather than the clock.
     */
    /*
     * Reset the analogue peripheral I2C block, then release it and power it.
     *
     * This is the bit that was missing, and nothing in ESP-IDF's P4 sources
     * points at it: PERIF_I2C_RSTB defaults to reset-asserted, IDF releases it
     * only in the C5 and C61 bootloader ports, and the register reads and
     * writes over this bus work regardless - which is why every measurement
     * said the bus was fine while the calibration never began.
     *
     * A pulse rather than a plain release, and that is deliberate.  The
     * register survives a CPU reset, so a boot following firmware that already
     * released it would inherit a working block and prove nothing: the failure
     * this fixes only appears when the block arrives held down.  Asserting the
     * reset first puts it back into the state a cold boot leaves it in, so
     * every boot exercises the path that was broken instead of one that was
     * already working - and takes the proof out of needing a battery
     * disconnected behind a screwed-down panel.
     */
    p4_w32(P4_PMU_RF_PWC,
           p4_r32(P4_PMU_RF_PWC) & ~P4_PMU_PERIF_I2C_RSTB);

    {
        unsigned long start, now;

        asm volatile("csrr %0, mcycle" : "=r"(start));
        do
            asm volatile("csrr %0, mcycle" : "=r"(now));
        while ((unsigned long)(now - start) < 360000UL);
    }

    p4_w32(P4_PMU_RF_PWC,
           p4_r32(P4_PMU_RF_PWC) | P4_PMU_PERIF_I2C_RSTB
                                 | P4_PMU_XPD_PERIF_I2C);

    /*
     * And let the block come up before anything asks it to calibrate.
     *
     * Without this the calibration succeeded on most boots and not on all -
     * one run in four reported "the mpll did not calibrate" with the same
     * binary.  A marginal analogue block is worse than a broken one, because
     * it passes the test that was used to declare it fixed.  A millisecond
     * costs nothing here and the failure it removes costs the whole boot.
     */
    {
        unsigned long start, now;

        asm volatile("csrr %0, mcycle" : "=r"(start));
        do
            asm volatile("csrr %0, mcycle" : "=r"(now));
        while ((unsigned long)(now - start) < 360000UL);
    }

    p4_w32(P4_PMU_RF_PWC,
           p4_r32(P4_PMU_RF_PWC) & ~P4_PMU_MSPI_PHY_XPD);

    {
        unsigned long start, now;

        asm volatile("csrr %0, mcycle" : "=r"(start));
        do
            asm volatile("csrr %0, mcycle" : "=r"(now));
        while ((unsigned long)(now - start) < 36000UL);
    }

    p4_w32(P4_PMU_RF_PWC, p4_r32(P4_PMU_RF_PWC) | P4_PMU_MSPI_PHY_XPD);
    p4_w32(P4_LP_CLKRST_HP_CLK_CTRL,
           p4_r32(P4_LP_CLKRST_HP_CLK_CTRL) | P4_HP_MPLL_500M_CLK_EN);

    /*
     * Let the analogue block come up before its calibration is started.
     *
     * ESP-IDF has no explicit wait here, but it does not need one: it powers
     * the PLL in rtc_clk_mpll_enable and calibrates in a separate function,
     * with a mutex acquisition and two calls in between.  This file does both
     * back to back, and on a cold-started chip the calibration then never
     * completed - MSPI_CAL_END stayed clear while every register written over
     * the configuration bus read back correctly.  A boot after firmware that
     * had already calibrated found the bit set from that run and continued,
     * which is why this only ever failed from cold.
     *
     * Counted in CPU cycles from the counter CSR, because this is SRAM-resident
     * and may not reach a flash address.  360000 is a millisecond at the
     * fastest clock this port sets and four at the slowest.
     */
    {
        unsigned long start, now;

        asm volatile("csrr %0, mcycle" : "=r"(start));
        do
            asm volatile("csrr %0, mcycle" : "=r"(now));
        while ((unsigned long)(now - start) < 360000UL);
    }

    p4_mpll_trace[0] = p4_r32(P4_CLKRST_ANA_PLL_CTRL0);

    /* The calibration runs while the stop bit is clear, so it has to be
       cleared before the dividers are written, not after */
    p4_w32(P4_CLKRST_ANA_PLL_CTRL0,
           p4_r32(P4_CLKRST_ANA_PLL_CTRL0) & ~P4_MSPI_CAL_STOP);

    p4_mpll_trace[1] = p4_r32(P4_CLKRST_ANA_PLL_CTRL0);

    /*
     * Every step below returns a distinct reason, because "the mpll did not
     * calibrate" conflates two failures that need different answers: the
     * configuration bus not answering at all, and the bus answering while the
     * calibration never completes.  A run that could not tell them apart cost
     * a session's worth of guessing.
     */
    /* Reference level to its maximum first */
    if (!p4_regi2c_read(P4_MPLL_DHREF_REG, &dhref))
        return P4_MPLL_NO_BUS;
    if (!p4_regi2c_write(P4_MPLL_DHREF_REG,
                         dhref | (3 << P4_MPLL_DHREF_SHIFT)))
        return P4_MPLL_NO_BUS;

    /* Then the calibration reset, low and back high */
    if (!p4_regi2c_read(P4_MPLL_IR_CAL_RSTB_REG, &rstb))
        return P4_MPLL_NO_BUS;
    if (!p4_regi2c_write(P4_MPLL_IR_CAL_RSTB_REG,
                         rstb & (unsigned char)~P4_MPLL_IR_CAL_RSTB))
        return P4_MPLL_NO_BUS;

    /*
     * Hold the calibration reset low before releasing it.
     *
     * ESP-IDF issues these two writes back to back.  This does not fix the
     * calibration - it was tried for that and changed nothing - but a reset
     * that is a pulse should have a width, and each regi2c transaction here is
     * microseconds of bus traffic rather than a register write, so the cost is
     * already paid.  Kept as correctness, not as a fix.
     */
    {
        unsigned long start, now;

        asm volatile("csrr %0, mcycle" : "=r"(start));
        do
            asm volatile("csrr %0, mcycle" : "=r"(now));
        while ((unsigned long)(now - start) < 36000UL);
    }

    if (!p4_regi2c_write(P4_MPLL_IR_CAL_RSTB_REG,
                         rstb | P4_MPLL_IR_CAL_RSTB))
        return P4_MPLL_NO_BUS;

    /* And the multiplier. ref_div stays at one, so the PLL sees half of
       XTAL and the target divided by that, less one, is the field. */
    div = P4_PSRAM_MPLL_HZ / (P4_XTAL_HZ / 2) - 1;
    if (!p4_regi2c_write(P4_MPLL_DIV_REG,
                         (unsigned char)((div << P4_MPLL_DIV_SHIFT)
                                         | (1UL << P4_MPLL_REF_DIV_SHIFT))))
        return P4_MPLL_NO_BUS;

    p4_mpll_trace[2] = p4_r32(P4_CLKRST_ANA_PLL_CTRL0);

    spin = 1000000;
    while (!(p4_r32(P4_CLKRST_ANA_PLL_CTRL0) & P4_MSPI_CAL_END) && --spin)
        ;
    p4_mpll_spins = (unsigned long)(1000000 - spin);
    p4_mpll_trace[3] = p4_r32(P4_CLKRST_ANA_PLL_CTRL0);

    p4_w32(P4_CLKRST_ANA_PLL_CTRL0,
           p4_r32(P4_CLKRST_ANA_PLL_CTRL0) | P4_MSPI_CAL_STOP);

    return spin ? P4_MPLL_OK : P4_MPLL_NO_CAL_END;
}

/*
 * The calibration, retried until it takes.
 *
 * One attempt is not reliable.  With the analogue block released and given a
 * settling wait, the calibration completed on most boots and not on all - and
 * a first measurement of six consecutive successes does not refute a one-in-
 * four failure rate, it is what a one-in-four rate produces about a fifth of
 * the time.  Reasoning from that sample was a mistake.
 *
 * A longer wait would only move the rate, not remove it, so the answer is to
 * try again rather than to wait harder: each attempt asserts the block's reset
 * afresh, power-cycles the PLL and runs the sequence, which is the same work
 * a cold boot would do.  Attempts are counted and reported, because a bring-up
 * that quietly needs three tries is a bring-up that is still marginal and
 * should be visible as one.
 */
P4_SRAMCODE int krnPSRAMMPLLUp(void)
{
    int reason = P4_MPLL_NO_CAL_END;
    unsigned int attempt;

    /* The bus master's clock and source, once: they are not what fails. */
    p4_w32(P4_CLKRST_REF_CLK_CTRL2,
           p4_r32(P4_CLKRST_REF_CLK_CTRL2) | P4_REF_160M_CLK_EN);
    p4_w32(P4_LPPERI_CLK_EN, p4_r32(P4_LPPERI_CLK_EN) | P4_CK_EN_LP_I2CMST);

    for (attempt = 0; attempt < P4_MPLL_CAL_TRIES; ++attempt)
    {
        p4_mpll_attempts = attempt + 1;

        reason = p4_mpll_calibrate();
        if (reason == P4_MPLL_OK || reason == P4_MPLL_NO_BUS)
            break;
    }

    return reason;
}

/*
 * Read the PLL's three configuration bytes back off the configuration bus.
 *
 * A write there reports nothing, so this is the only way to tell a
 * configured PLL from a write that went somewhere else. Returns the three
 * bytes packed as rstb, div, dhref from the low byte up.
 */
P4_SRAMCODE unsigned long krnPSRAMMPLLState(void)
{
    unsigned char rstb = 0xFF, div = 0xFF, dhref = 0xFF;

    p4_regi2c_read(P4_MPLL_IR_CAL_RSTB_REG, &rstb);
    p4_regi2c_read(P4_MPLL_DIV_REG, &div);
    p4_regi2c_read(P4_MPLL_DHREF_REG, &dhref);

    return (unsigned long)rstb | ((unsigned long)div << 8)
           | ((unsigned long)dhref << 16);
}

/*
 * Take the pair of MSPI controllers out of reset with their clocks on, and
 * set the bus clock as close to the requested rate as the divider allows.
 *
 * The source is the MPLL, so krnPSRAMMPLLUp has to have run and reported
 * success first. The controller's core clock is the PLL's own rate,
 * undivided, and the counters written here divide that; a target that
 * divides 400 MHz evenly is therefore exact, and 20, 40, 50, 80, 100 and
 * 200 MHz all do.
 *
 * Returns the rate actually set, or 0 if the controller did not answer.
 */
/*
 * The divider alone, with no reset and nothing else touched.
 *
 * This exists because krnPSRAMClockUp() below is not a clock setter: it also
 * takes both controllers out of reset, which is exactly right the first time
 * and destroys everything the second.  Calling it again after the mode
 * registers were written left the controller at its reset defaults and the
 * next transaction never completed, which is a hang with no output at all.
 * The calibration changes the clock three times, so it needs this.
 *
 * ESP-IDF's own clock change is the same three register writes and no reset,
 * which is the confirmation that nothing else has to move with the divider.
 * The DLL in particular stays as krnPSRAMConfigure() left it.
 *
 * Returns the rate actually set, or 0 if the controller kept nothing.
 */
P4_SRAMCODE unsigned long krnPSRAMClockSet(unsigned long target_hz)
{
    unsigned long div, n, clkval;

    if (target_hz == 0)
        target_hz = 20000000UL;

    /* Round up, so the bus never runs faster than asked */
    div = (P4_PSRAM_MPLL_HZ + target_hz - 1) / target_hz;
    if (div < 1)
        div = 1;
    if (div > 256)              /* the counters are eight bits each */
        div = 256;

    if (div == 1)
        clkval = P4_SCLK_EQU_SYSCLK;
    else
    {
        n = div - 1;
        clkval = (n << P4_SCLKCNT_N_SHIFT)
               | ((div / 2 - 1) << P4_SCLKCNT_H_SHIFT)
               | (n << P4_SCLKCNT_L_SHIFT);
    }

    p4_w32(P4_MSPI2_SRAM_CLK, clkval);
    p4_w32(P4_MSPI3_CLOCK, clkval);

    if (p4_r32(P4_MSPI2_SRAM_CLK) != clkval)
        return 0;

    return P4_PSRAM_MPLL_HZ / div;
}

/*
 * The controller itself: module clocks, reset, clock source.  No divider.
 *
 * Split out from krnPSRAMClockUp because ESP-IDF's order is not the one this
 * file used to have.  esp_psram_impl_enable() brings the module up, then sets
 * the pin drive and the strobe, then the analogue timing, and only then the
 * divider - so the pins are configured before any clock counts down against
 * them.  Whether that matters at 20 MHz is not established; following the
 * sequence that is known to work costs nothing and removes the question.
 *
 * Returns 1 if the controller kept a write, 0 if it did not.
 */
P4_SRAMCODE static int krnPSRAMControllerUp(struct P4PSRAMEntry *entry)
{
    /* Module clocks first: the registers below do not answer without them */
    p4_w32(P4_CLKRST_SOC_CLK_CTRL0,
           p4_r32(P4_CLKRST_SOC_CLK_CTRL0) | P4_PSRAM_SYS_CLK_EN);
    p4_w32(P4_CLKRST_PERI_CLK_CTRL00,
           p4_r32(P4_CLKRST_PERI_CLK_CTRL00) | P4_PSRAM_PLL_CLK_EN
                                             | P4_PSRAM_CORE_CLK_EN);

    /*
     * Readable from here and cleared by the reset below, so this is the only
     * window in which the handover state of the controller itself exists.
     */
    if (entry)
    {
        entry->mspi2_sram_clk   = p4_r32(P4_MSPI2_SRAM_CLK);
        entry->mspi3_clock      = p4_r32(P4_MSPI3_CLOCK);
        entry->timing_cali      = p4_r32(P4_MSPI_TIMING_CALI);
        entry->smem_timing_cali = p4_r32(P4_MSPI_SMEM_TIMING_CALI);
        entry->smem_ac          = p4_r32(P4_MSPI_SMEM_AC);
    }

    /* Reset both halves, AXI outermost, and release in the reverse order */
    p4_w32(P4_CLKRST_HP_RST_EN0,
           p4_r32(P4_CLKRST_HP_RST_EN0) | P4_RST_EN_DUAL_MSPI_AXI
                                        | P4_RST_EN_DUAL_MSPI_APB);
    p4_w32(P4_CLKRST_HP_RST_EN0,
           p4_r32(P4_CLKRST_HP_RST_EN0) & ~P4_RST_EN_DUAL_MSPI_APB);
    p4_w32(P4_CLKRST_HP_RST_EN0,
           p4_r32(P4_CLKRST_HP_RST_EN0) & ~P4_RST_EN_DUAL_MSPI_AXI);

    p4_w32(P4_CLKRST_PERI_CLK_CTRL00,
           (p4_r32(P4_CLKRST_PERI_CLK_CTRL00) & ~P4_PSRAM_CLK_SRC_MASK)
           | ((unsigned long)P4_PSRAM_CLK_SRC_MPLL << P4_PSRAM_CLK_SRC_SHIFT));

    /*
     * A write and its read-back is the test for everything above.  An
     * unclocked or still-reset controller does not keep what was written to
     * it, so a value that comes back unchanged says the reset was released
     * and the module clocks are on, which is the whole claim this stage makes.
     * The divider itself is set later, in the reference's order.
     */
    return krnPSRAMClockSet(20000000UL) != 0;
}

/*
 * The old combined form, kept for the tuning and for any caller that wants
 * the controller and a clock in one step.  The bring-up no longer uses it,
 * because the steps between the two now have to run in between.
 */
P4_SRAMCODE unsigned long krnPSRAMClockUp(unsigned long target_hz)
{
    if (!krnPSRAMControllerUp(NULL))
        return 0;

    return krnPSRAMClockSet(target_hz);
}

/*
 * The analogue configuration: how long chip select is asserted around a
 * transfer, whether the controller may split a burst, how large a page is,
 * and the delay line.
 *
 * None of these values depend on the bus clock, which is why this stage
 * says nothing about speed. The two that do - the read and write latencies
 * - are not here: they are told to the chip through its mode registers,
 * and belong with that sequence.
 *
 * The DLL is enabled at every clock ESP-IDF supports, including the slowest,
 * so it is enabled here too rather than guessed about.
 */
/*
 * Raise every PSRAM pin's drive strength. Reset leaves them at zero, which
 * is not enough for the chip to see anything, so this is a precondition for
 * the bring-up rather than a tuning step.
 *
 * The table lives in SRAM, not in the read-only section, for the reason at
 * the top of this file: a P4_SRAMCODE function may not reach through the
 * cache that MSPI serves.
 */
P4_SRAMCODE static void p4_psram_pin_drive(unsigned long drv)
{
    P4_SRAMRODATA static const struct { unsigned short off; unsigned char sh; }
        pins[] = { P4_PSRAM_PIN_DRV_TABLE };
    unsigned int i;

    for (i = 0; i < sizeof(pins) / sizeof(pins[0]); i++)
    {
        unsigned long reg = P4_IOMUX_MSPI_PIN_BASE + pins[i].off;

        p4_w32(reg, (p4_r32(reg) & ~(3UL << pins[i].sh))
                    | ((drv & 3UL) << pins[i].sh));
    }
}

/*
 * Drive strength and the strobe.
 *
 * Without the strobe a DTR read never completes, and the controller waits
 * rather than complaining.  ESP-IDF does both of these immediately after the
 * module reset and before the divider, which is why they are their own step.
 */
P4_SRAMCODE static void krnPSRAMPinsUp(void)
{
    p4_psram_pin_drive(P4_PSRAM_PIN_DRV);

    p4_w32(P4_IOMUX_PSRAM_DQS_0,
           p4_r32(P4_IOMUX_PSRAM_DQS_0) | P4_IOMUX_DQS_XPD);
    p4_w32(P4_IOMUX_PSRAM_DQS_1,
           p4_r32(P4_IOMUX_PSRAM_DQS_1) | P4_IOMUX_DQS_XPD);
}

/*
 * Chip-select timing, split transactions and the page size.  None of these
 * depend on the bus clock, which is why this stage says nothing about speed.
 */
P4_SRAMCODE static void krnPSRAMAnalogUp(void)
{
    unsigned long ac;

    ac = p4_r32(P4_MSPI_SMEM_AC);
    ac &= ~(P4_SMEM_CS_SETUP_TIME_M | P4_SMEM_CS_HOLD_TIME_M
            | P4_SMEM_CS_HOLD_DELAY_M);
    ac |= P4_SMEM_CS_SETUP | P4_SMEM_CS_HOLD | P4_SMEM_SPLIT_TRANS_EN;
    /* each counter is held as value minus one */
    ac |= ((unsigned long)(P4_PSRAM_CS_SETUP_TIME - 1) << P4_SMEM_CS_SETUP_TIME_SH);
    ac |= ((unsigned long)(P4_PSRAM_CS_HOLD_TIME - 1) << P4_SMEM_CS_HOLD_TIME_SH);
    ac |= ((unsigned long)(P4_PSRAM_CS_HOLD_DELAY - 1) << P4_SMEM_CS_HOLD_DELAY_SH);
    p4_w32(P4_MSPI_SMEM_AC, ac);

    p4_w32(P4_MSPI_SMEM_ECC_CTRL,
           (p4_r32(P4_MSPI_SMEM_ECC_CTRL) & ~P4_SMEM_PAGE_SIZE_MASK)
           | ((unsigned long)P4_PSRAM_PAGE_SIZE_2048 << P4_SMEM_PAGE_SIZE_SHIFT));
}

/*
 * The delay line, for both halves of the block.
 *
 * Both bits are bit 5, one in the flash half's timing register and one in the
 * PSRAM half's, and ESP-IDF sets both at every clock it supports including
 * the slowest.  It sets them after the divider, which is the order used here.
 */
P4_SRAMCODE static void krnPSRAMDllUp(void)
{
    p4_w32(P4_MSPI_SMEM_TIMING_CALI,
           p4_r32(P4_MSPI_SMEM_TIMING_CALI) | P4_DLL_TIMING_CALI);
    p4_w32(P4_MSPI_TIMING_CALI,
           p4_r32(P4_MSPI_TIMING_CALI) | P4_DLL_TIMING_CALI);
}

/*
 * The three above in one call, for the retry path, where the clock already
 * stands and the order between them no longer means anything.
 */
P4_SRAMCODE static void krnPSRAMConfigure(void)
{
    krnPSRAMPinsUp();
    krnPSRAMAnalogUp();
    krnPSRAMDllUp();
}

/*
 * What the second-stage bootloader left behind, read before anything here
 * writes.  See the comment on struct P4PSRAMEntry for why this is worth
 * printing: it is the one input to the bring-up this port does not set.
 */
P4_SRAMCODE void krnPSRAMEntryRead(struct P4PSRAMEntry *out)
{
    out->soc_clk_ctrl0    = p4_r32(P4_CLKRST_SOC_CLK_CTRL0);
    out->peri_clk_ctrl00  = p4_r32(P4_CLKRST_PERI_CLK_CTRL00);
    out->hp_rst_en0       = p4_r32(P4_CLKRST_HP_RST_EN0);
    out->dqs_0            = p4_r32(P4_IOMUX_PSRAM_DQS_0);

    /*
     * The controller's own registers are not readable here.  The first
     * version of this function read them too and the boot stopped dead on
     * the first of them: without the module clock an MSPI register does not
     * answer, and the bus waits rather than faulting.  That is a measurement
     * in itself - the bootloader hands this port a PSRAM controller with its
     * clock off - so the four are read where they become readable instead,
     * in krnPSRAMControllerUp, after the clock and before the reset.
     */
    out->mspi2_sram_clk   = 0;
    out->mspi3_clock      = 0;
    out->timing_cali      = 0;
    out->smem_timing_cali = 0;
    out->smem_ac          = 0;
}

/*
 * Talking to the chip, as opposed to the controller in front of it.
 *
 * The transaction is done by three functions in the part's own mask ROM, at
 * fixed published addresses. Writing an MSPI transaction engine to send
 * sixteen bits of command and move two bytes would be work for its own
 * sake, and the ROM is always mapped, so calling it needs no cache.
 *
 * The operating mode is set before every transaction rather than once.
 * ESP-IDF does the same, and it is the difference between the second
 * transaction in a row answering and not answering.
 */
P4_SRAMCODE static void p4_psram_cmd(uint32_t cmd, uint32_t reg_addr,
                                     uint32_t dummy,
                                     uint32_t *tx, uint32_t tx_bits,
                                     uint32_t *rx, uint32_t rx_bits)
{
    void (*rom_set_op_mode)(int, int) =
        (void (*)(int, int))P4_ROM_SPI_SET_OP_MODE;
    void (*rom_cmd_config)(int, const struct p4_rom_spi_cmd *) =
        (void (*)(int, const struct p4_rom_spi_cmd *))P4_ROM_SPI_CMD_CONFIG;
    void (*rom_cmd_start)(int, unsigned char *, uint32_t, uint32_t, int) =
        (void (*)(int, unsigned char *, uint32_t, uint32_t, int))P4_ROM_SPI_CMD_START;

    struct p4_rom_spi_cmd c;
    uint32_t addr = reg_addr;

    c.cmd = (uint16_t)cmd;
    c.cmd_bitlen = 16;
    c.addr = &addr;
    c.addr_bitlen = 32;
    c.tx_data = tx;
    c.tx_data_bitlen = tx_bits;
    c.rx_data = rx;
    c.rx_data_bitlen = rx_bits;
    c.dummy_bitlen = dummy;

    rom_set_op_mode(P4_MSPI_ID_REG, P4_ROM_OPI_DTR_MODE);
    rom_cmd_config(P4_MSPI_ID_REG, &c);
    rom_cmd_start(P4_MSPI_ID_REG, (unsigned char *)rx, rx_bits / 8,
                  P4_PSRAM_CS_INDEX, 0);
}

/*
 * Mode registers come in pairs at even addresses, because the bus is
 * sixteen bits wide and a transfer moves both halves. Address 0 carries
 * mode register 0 in its low byte and mode register 1 in its high byte,
 * address 4 carries 4 and 5, address 8 carries 8. A read at an odd address
 * is not a way to reach the odd-numbered register.
 */
P4_SRAMCODE static void p4_psram_reg_read(uint32_t addr, uint32_t *pair)
{
    *pair = 0;
    p4_psram_cmd(P4_PSRAM_REG_READ, addr, p4_rd_reg_dummy,
                 NULL, 0, pair, 16);
}

P4_SRAMCODE static void p4_psram_reg_write(uint32_t addr, uint32_t pair)
{
    uint32_t v = pair;

    p4_psram_cmd(P4_PSRAM_REG_WRITE, addr, 0, &v, 16, NULL, 0);
}

/*
 * Configure the chip through its mode registers, which has to happen before
 * anything can be read from it.
 *
 * This is not an optimisation step. Mode register 8 selects the bus width,
 * and until it is told otherwise the part does not drive all sixteen lanes;
 * reading its identity first, which is what this file did at first, asks a
 * chip in one configuration a question in another and gets an answer that
 * looks like a floating bus. Each register is read, the fields this port
 * owns are replaced, and the rest is written back untouched.
 *
 * The latencies are the pair for 80 MHz and below. They are the only values
 * here that depend on the clock, which is why raising the clock later means
 * revisiting this function and not just the divider.
 */
P4_SRAMCODE void krnPSRAMModeInit(void)
{
    /*
     * Absolute values, not read-modify-write.
     *
     * ESP-IDF reads each register, replaces the fields it owns and writes
     * the rest back, which is the right thing to do when the read can be
     * trusted. Here it cannot: before the chip is configured a read returns
     * a floating bus, and writing 0xff back sets every reserved bit along
     * with a partial-array-refresh and refresh-rate setting nobody asked
     * for. Writing the power-on configuration outright is both simpler and
     * the only version that does not depend on the thing being fixed.
     *
     * mode register 0: drive strength 0, read latency 2, fixed latency
     * mode register 4: write latency 2, no partial array refresh
     * mode register 8: burst length 3, linear bursts, row crossing, x16
     *
     * The two latencies are the pair for 80 MHz and below and are the only
     * values here that depend on the clock.
     */
    p4_psram_reg_write(0, (p4_rd_latency << 2) | (1UL << 5));
    p4_psram_reg_write(4, p4_wr_latency << 5);
    p4_psram_reg_write(8, 3UL | (1UL << 3) | (1UL << 6));

    /*
     * Mode register 8 selects the bus width, and the part needs a moment to
     * change it internally before it will answer on the new one.  Counted in
     * CPU cycles from the counter CSR because this is SRAM-resident code and
     * may not reach a flash address: 36000 cycles is 100 us at the fastest
     * clock this port sets and 400 us at the slowest.
     */
    {
        unsigned long start, now;

        asm volatile("csrr %0, mcycle" : "=r"(start));
        do
            asm volatile("csrr %0, mcycle" : "=r"(now));
        while ((unsigned long)(now - start) < 36000UL);
    }
}

/*
 * Ask the chip who it is.
 *
 * Mode register 1 holds the vendor in its low five bits, mode register 2 the
 * density. 0x0d as the vendor means Espressif's AP part, which is what this
 * board carries; anything else means the sequence above configured a
 * controller with nothing on the other end.
 */
P4_SRAMCODE int krnPSRAMIdentify(unsigned char *vendor, unsigned char *density)
{
    uint32_t pair;
    unsigned char mr1, mr2;

    p4_psram_reg_read(0, &pair);
    mr1 = (unsigned char)((pair >> 8) & P4_PSRAM_MR1_VENDOR_MASK);

    p4_psram_reg_read(2, &pair);
    mr2 = (unsigned char)(pair & 0xFF);

    if (vendor)
        *vendor = mr1;
    if (density)
        *density = mr2;

    return (mr1 == P4_PSRAM_VENDOR_AP);
}

/*
 * Ask the chip who it is in every read latency it can be in.
 *
 * The identity read above uses whatever dummy length this port selected for
 * its own target clock.  That is correct once the chip has been told, and
 * wrong before it has: the mode registers survive a CPU reset, so the part
 * arrives in the latency the last firmware chose, and a read at any other
 * one samples outside the window and returns a floating bus.  A port that
 * only asks in its own latency therefore works after itself and after nothing
 * else - which is exactly the failure that cost 23 August.
 *
 * Eight candidates, two register reads each.  Returns the latency that
 * answered, or -1, and leaves the dummy length at the value that worked so
 * the caller can see it; the caller restores its own with
 * p4_psram_select_params.
 */
P4_SRAMCODE static int p4_psram_probe_latency(unsigned char *vendor,
                                              unsigned char *density)
{
    unsigned int lat;

    for (lat = 0; lat < P4_PSRAM_RD_LATENCIES; ++lat)
    {
        p4_rd_reg_dummy = P4_PSRAM_REG_DUMMY_FOR(lat);
        if (krnPSRAMIdentify(vendor, density))
            return (int)lat;
    }

    return -1;
}

/*
 * A block through the command path, in transaction-sized pieces.
 *
 * The controller's FIFO takes 64 bytes, so a 128-byte reference block is two
 * transactions.  These exist for the tuning, which needs to write a known
 * block at a clock it trusts and read it back at one it does not.
 */
P4_SRAMCODE void krnPSRAMBlockWrite(uint32_t addr, const uint32_t *words,
                                    uint32_t count)
{
    while (count)
    {
        uint32_t n = count > P4_PSRAM_FIFO_WORDS ? P4_PSRAM_FIFO_WORDS : count;

        p4_psram_cmd(P4_PSRAM_SYNC_WRITE, addr, p4_wr_dummy,
                     (uint32_t *)words, n * 32, NULL, 0);
        words += n;
        addr += n * 4;
        count -= n;
    }
}

P4_SRAMCODE void krnPSRAMBlockRead(uint32_t addr, uint32_t *words,
                                   uint32_t count)
{
    while (count)
    {
        uint32_t n = count > P4_PSRAM_FIFO_WORDS ? P4_PSRAM_FIFO_WORDS : count;

        p4_psram_cmd(P4_PSRAM_SYNC_READ, addr, p4_rd_dummy,
                     NULL, 0, words, n * 32);
        words += n;
        addr += n * 4;
        count -= n;
    }
}

/*
 * Write a word to the chip and read it back.
 *
 * This answers a different question from the identity read: not whether the
 * mode registers are being addressed correctly, but whether the controller,
 * the pins, the clock and the chip carry data at all. ESP-IDF makes the same
 * test, at the same address, with the same pattern, as its check for whether
 * a chip is connected.
 */
P4_SRAMCODE int krnPSRAMRoundTrip(uint32_t *back)
{
    uint32_t out = P4_PSRAM_TEST_PATTERN;
    uint32_t in = 0;

    p4_psram_cmd(P4_PSRAM_SYNC_WRITE, 0, p4_wr_dummy,
                 &out, 32, NULL, 0);
    p4_psram_cmd(P4_PSRAM_SYNC_READ, 0, p4_rd_dummy,
                 NULL, 0, &in, 32);

    if (back)
        *back = in;

    return in == P4_PSRAM_TEST_PATTERN;
}

/*
 * The same read, with the transaction started by hand.
 *
 * Everything the mask ROM programs has been measured to match a working
 * ESP-IDF run register for register, and the data still arrives as ones.
 * That leaves the possibility that the ROM's start call is not doing what
 * its name says on this part, so this does the last step directly: set the
 * user transaction bit, wait for the controller to clear it, and read the
 * data out of the controller's own buffer rather than out of a pointer the
 * ROM copied into.
 *
 * Returns the data word. The state machine's two status fields, sampled
 * before and after, go into *state; a controller that never left its idle
 * state would say so there.
 */
/*
 * The whole bring-up, in the order it has to happen.
 *
 * The order is not a matter of taste. The PLL has to be running before a
 * divider off it means anything; the pins have to be able to drive before
 * a command can be seen; the mode registers have to be written before a
 * read returns anything, because until then the chip is not driving all of
 * its data lines.
 *
 * Interrupts are the caller's business. This runs from SRAM and does not
 * print, so that the sequence is not interleaved with a console that lives
 * in flash.
 */
/*
 * Progress markers for a function that cannot call the console.
 *
 * A hang in the bring-up produces no output at all - the boot stops after the
 * clock report - and that has now happened twice.  Two obvious forms of this
 * instrument do not work here, and both were tried:
 *
 *   - calling krnP4PutStr, because this code is SRAM-resident and the console
 *     lives in flash on an XIP build;
 *   - passing a string, because the literal itself lands in flash .rodata and
 *     an SRAM-resident function may not reach it.
 *
 * check-sramtext.sh rejects both, correctly.  It was also read as a passing
 * build several times, because the error line does not match a grep for
 * " error", so stale images were flashed and their behaviour recorded as
 * measurement.  Filter on "error:".
 *
 * What is left is a single character, which needs no storage at all: it is an
 * immediate in the instruction stream.  The output mechanism is the same one
 * krnP4PutC uses - poll the endpoint's data-free bit, write the byte, mark the
 * transfer done - duplicated rather than shared, because making the console
 * SRAM-resident would move it into a 40 KB budget for the sake of a diagnostic.
 *
 * A hang therefore shows as a truncated run of digits, and the last one printed
 * is the last stage entered.  Silent unless P4_PSRAM_TRACE is defined.
 */
#ifdef P4_PSRAM_TRACE
P4_SRAMCODE static void psram_mark(char c)
{
    unsigned int spins = 400000;

    while (spins-- &&
           !(p4_r32(P4_USJ_BASE + P4_USJ_EP1_CONF) & P4_USJ_IN_EP_DATA_FREE))
        ;
    p4_w32(P4_USJ_BASE + P4_USJ_EP1, (unsigned long)(unsigned char)c);
    p4_w32(P4_USJ_BASE + P4_USJ_EP1_CONF, P4_USJ_WR_DONE);
}
#else
#define psram_mark(c)   do { } while (0)
#endif

P4_SRAMCODE int krnPSRAMBringUp(struct P4PSRAMInfo *info,
                                unsigned long target_hz)
{
    P4_SRAMRODATA static const unsigned char sizes[] = P4_PSRAM_SIZE_TABLE;
    unsigned char vendor = 0, density = 0;
    uint32_t back = 0;
    int fast;

    info->clock_hz = 0;
    info->size = 0;
    info->vendor = 0;
    info->density = 0;
    info->round_trip = 0;
    info->fell_back = 0;
    info->tuning.tuned = 0;

    info->connected = 0;
    info->probe_latency = -1;
    info->bias_found = 0;
    info->bias_set = 0;

    fast = target_hz > 80000000UL;
    info->fast_requested = (unsigned char)fast;

    /* Before anything here writes, so the print is the handover state */
    psram_mark('1');
    krnPSRAMEntryRead(&info->entry);

    /*
     * The supply is not raised here.  It is a machine-wide setting and it has
     * to be up before the CPU clock, which is set earlier than this - see
     * krnP4SupplyUp and the comment on it.  What this records is what that
     * step left, so a failure can be read against it.
     */
    info->bias_found = (unsigned char)((p4_r32(P4_PMU_HP_ACTIVE_BIAS)
                                        & P4_PMU_DCM_VSET_MASK)
                                       >> P4_PMU_DCM_VSET_SHIFT);
    info->bias_set   = info->bias_found;

    psram_mark('2');
    info->mpll_reason = (signed char)krnPSRAMMPLLUp();
    info->mpll_up = (info->mpll_reason == P4_MPLL_OK) ? 1 : 0;
    /*
     * A missing MSPI_CAL_END is fatal, and this was tested rather than assumed:
     * letting the bring-up continue without it hung the boot at the first
     * controller register, with no output at all.  The bit is a real signal -
     * the PLL does not run uncalibrated - so stopping here is what keeps a
     * failure diagnosable instead of silent.
     */
    info->mpll_state = krnPSRAMMPLLState();
    info->ana_pll_ctrl0 = p4_r32(P4_CLKRST_ANA_PLL_CTRL0);
    info->ana_trace[0] = p4_mpll_trace[0];
    info->ana_trace[1] = p4_mpll_trace[1];
    info->ana_trace[2] = p4_mpll_trace[2];
    info->ana_trace[3] = p4_mpll_trace[3];
    info->ana_spins = p4_mpll_spins;
    info->mpll_attempts = (unsigned char)p4_mpll_attempts;
    if (!info->mpll_up)
        return 0;

    /*
     * The parameter set comes from the target, and the whole device
     * configuration then runs at 20 MHz regardless.
     *
     * ESP-IDF configures the chip at the target rate with the sampling
     * untuned, which works and is one risk this port has no reason to take:
     * a longer latency is harmless at a lower clock, so the same registers
     * can be written slowly and only the divider raised afterwards.  If the
     * identity read fails, it fails at a clock where the answer means the
     * chip, and not the sampling.
     */
    psram_mark('3');
    p4_psram_select_params(target_hz);
    krnPSRAMTuningClear();

    /*
     * ESP-IDF's order, step for step: module up, pins, analogue timing,
     * divider, delay line.  This file used to set the divider immediately
     * after the module reset and everything else afterwards.
     */
    psram_mark('4');
    if (!krnPSRAMControllerUp(&info->entry))
        return 0;

    psram_mark('5');
    krnPSRAMPinsUp();
    krnPSRAMAnalogUp();

    info->clock_hz = krnPSRAMClockSet(20000000UL);
    if (!info->clock_hz)
        return 0;

    psram_mark('6');
    krnPSRAMDllUp();

    /*
     * Find the chip, tell it what this port wants, then confirm.
     *
     * The order matters and it is the lesson of 23 August.  The chip's mode
     * registers survive a CPU reset, so the part arrives in the latency and
     * width the last firmware chose rather than in a reset default.  Writing
     * the wanted configuration first and reading afterwards works only if the
     * arriving state happens to be the one this port assumes; when it is not,
     * every read returns a floating bus, the retry repeats the same wrong
     * assumption three times, and the boot reports an absent chip that is
     * sitting there working.  That is not a hypothesis: writing the vendor's
     * own firmware back brought the same chip up immediately, and this port
     * then worked on every boot after it.
     *
     * So the sweep comes first.  It asks in all eight latencies, and the one
     * that answers is both the way in and the measurement of what the chip
     * arrived in.  The mode-register write then lands, and the confirmation
     * runs in this port's own timing.
     *
     * The retry is kept for the case where the sweep finds nothing: a first
     * transaction after the reset may be lost in either direction, and three
     * bounded attempts cost microseconds against a failure that costs the
     * whole boot.
     */
    {
        unsigned int attempt;

        info->probe_latency = -1;

        for (attempt = 0; attempt < 3; ++attempt)
        {
            int lat;

            psram_mark('7');
            krnPSRAMConfigure();

            /*
             * Write blind first.  After a cold start the part is in its
             * power-on width, which is not the one this port reads in, and a
             * read at the wrong width does not fail - it does not return.  A
             * first version of this loop swept the latencies before writing
             * anything and hung the boot dead on a cold-started chip, with no
             * output at all where a diagnosis used to be.  The write needs no
             * dummy cycles and is therefore the one transaction that is safe
             * to send into an unknown state.
             */
            psram_mark('8');
            krnPSRAMModeInit();

            psram_mark('9');
            info->connected = krnPSRAMRoundTrip(&back) ? 1 : 0;

            psram_mark('a');
            if (krnPSRAMIdentify(&vendor, &density))
            {
                if (info->probe_latency < 0)
                    info->probe_latency = (signed char)p4_rd_latency;
                break;
            }

            /*
             * The write did not take, or the read is looking in the wrong
             * place.  Now the sweep is worth its risk: ask in each latency,
             * and if one answers, write again with this port's own timing and
             * confirm.  This is the path that recovers a part left configured
             * by other firmware.
             */
            psram_mark('b');
            lat = p4_psram_probe_latency(&vendor, &density);
            p4_psram_select_params(target_hz);

            if (lat >= 0)
            {
                if (info->probe_latency < 0)
                    info->probe_latency = (signed char)lat;

                psram_mark('c');
                krnPSRAMModeInit();
                psram_mark('d');
                info->connected = krnPSRAMRoundTrip(&back) ? 1 : 0;

                psram_mark('e');
                if (krnPSRAMIdentify(&vendor, &density))
                    break;
            }
        }
        psram_mark('f');
        info->identify_attempts = (unsigned char)(attempt + 1);

        if (attempt == 3)
        {
            info->vendor = vendor;
            info->density = density;
            info->round_trip = info->connected;
            return 0;
        }
    }

    psram_mark('g');
    info->vendor = vendor;
    info->density = density;
    info->round_trip = krnPSRAMRoundTrip(&back) ? 1 : 0;
    psram_mark('h');

    if (fast && info->round_trip)
    {
        if (krnPSRAMTune(&info->tuning, target_hz))
        {
            info->clock_hz = target_hz;
            /* The same question again, now at the tuned clock.  A window
               found on 128 bytes that cannot round-trip one word is not a
               window. */
            info->round_trip = krnPSRAMRoundTrip(&back) ? 1 : 0;
        }
        if (!info->tuning.tuned || !info->round_trip)
        {
            /*
             * Back to a rate that has never failed, with the sampling
             * neutral again.  The parameter set stays as it was: it is
             * correct at any lower clock, and rewriting the mode registers
             * here would add a failure path to the recovery path.
             */
            info->fell_back = 1;
            krnPSRAMTuningClear();
            info->clock_hz = krnPSRAMClockSet(20000000UL);
            info->round_trip = krnPSRAMRoundTrip(&back) ? 1 : 0;
        }
    }

    krnPSRAMAxiConfigure();
    info->size = (unsigned long)sizes[density & P4_PSRAM_MR2_DENSITY_MASK]
                 * 1024UL * 1024UL;

    return info->size != 0 && info->round_trip;
}

/*
 * Tell MSPI2 how to serve a load or a store to the window.
 *
 * Everything before this configures the chip. This configures the path the
 * cache uses to reach it: the two commands, the address length, the two
 * dummy lengths, octal command and address with sixteen bit data, double
 * transfer rate, and finally permission to answer AXI requests at all.
 * The last of those is why a store to a correctly mapped window is a bus
 * error until this has run.
 *
 * The dummy lengths are the pair for 80 MHz and below, like the latencies
 * in the mode registers, and have to change with them when the clock rises.
 */
P4_SRAMCODE void krnPSRAMAxiConfigure(void)
{
    unsigned long v;

    /* The commands the controller issues for a cache line */
    p4_w32(P4_MSPI2_SRAM_DWR_CMD,
           ((unsigned long)(16 - 1) << P4_SRAM_CMD_BITLEN_SHIFT)
           | (P4_PSRAM_SYNC_WRITE & P4_SRAM_CMD_VALUE_MASK));
    p4_w32(P4_MSPI2_SRAM_DRD_CMD,
           ((unsigned long)(16 - 1) << P4_SRAM_CMD_BITLEN_SHIFT)
           | (P4_PSRAM_SYNC_READ & P4_SRAM_CMD_VALUE_MASK));

    v = p4_r32(P4_MSPI2_CACHE_SCTRL);
    v &= ~(P4_SRAM_ADDR_BITLEN_MASK | P4_SRAM_RDUMMY_MASK | P4_SRAM_WDUMMY_MASK);
    v |= P4_CACHE_SRAM_USR_WCMD | P4_CACHE_SRAM_USR_RCMD;
    v |= P4_CACHE_USR_SADDR_4BYTE;
    v |= P4_USR_WR_SRAM_DUMMY | P4_USR_RD_SRAM_DUMMY;
    v |= P4_SRAM_OCT;
    v |= (unsigned long)(P4_PSRAM_ADDR_BITLEN - 1) << P4_SRAM_ADDR_BITLEN_SHIFT;
    v |= (unsigned long)(p4_rd_dummy - 1) << P4_SRAM_RDUMMY_SHIFT;
    v |= (unsigned long)(p4_wr_dummy - 1) << P4_SRAM_WDUMMY_SHIFT;
    p4_w32(P4_MSPI2_CACHE_SCTRL, v);

    /* Octal for command and address, sixteen bits for data */
    v = p4_r32(P4_MSPI2_SRAM_CMD);
    v |= P4_MEM_SCMD_OCT | P4_MEM_SADDR_OCT | P4_MEM_SDOUT_OCT | P4_MEM_SDIN_OCT;
    v |= P4_MEM_SDIN_HEX | P4_MEM_SDOUT_HEX;
    v |= P4_MEM_SDUMMY_WOUT;
    p4_w32(P4_MSPI2_SRAM_CMD, v);

    /* Double transfer rate, no byte swapping, variable dummy on both
       controllers as ESP-IDF sets it */
    v = p4_r32(P4_MSPI2_SMEM_DDR);
    v &= ~(P4_DDR_RDAT_SWP | P4_DDR_WDAT_SWP);
    v |= P4_DDR_EN | P4_DDR_VAR_DUMMY;
    p4_w32(P4_MSPI2_SMEM_DDR, v);
    p4_w32(P4_MSPI3_DDR, p4_r32(P4_MSPI3_DDR) | P4_DDR_VAR_DUMMY);

    /* Splice adjacent AXI bursts */
    p4_w32(P4_MSPI2_CTRL1,
           p4_r32(P4_MSPI2_CTRL1) | P4_MEM_AW_SPLICE_EN | P4_MEM_AR_SPLICE_EN);

    /* And last, because until now there was nothing to answer with */
    p4_w32(P4_MSPI2_CACHE_FCTRL,
           (p4_r32(P4_MSPI2_CACHE_FCTRL) & ~P4_CLOSE_AXI_INF_EN)
           | P4_MEM_AXI_REQ_EN);
}

/*
 * Put the chip in the window at 0x48000000.
 *
 * Physical page n goes to virtual page n, which is the only arrangement
 * that makes the window look like memory. The two functions below are the
 * exception to this file's rule about running from SRAM: they touch the
 * translation table and the window, not the controller's configuration, so
 * the cache they are fetched through is not the one they are changing.
 */
void krnPSRAMMap(unsigned long size)
{
    unsigned long pages = size >> P4_MMU_PAGE_SHIFT;
    unsigned long i;

    if (pages > P4_MMU_ENTRIES)
        pages = P4_MMU_ENTRIES;

    for (i = 0; i < pages; i++)
    {
        p4_w32(P4_MMU_PSRAM_INDEX, i);
        p4_w32(P4_MMU_PSRAM_CONTENT, i | P4_MMU_VALID | P4_MMU_ACCESS_PSRAM);
    }
}

/*
 * Check that the window really is that much memory.
 *
 * One word per megabyte, each holding something derived from its own
 * address, all written before any is read back. Spreading the writes over
 * the whole range and reading afterwards is what makes this a test of the
 * memory rather than of the cache: 32 MB of writes cannot sit in 128 KB of
 * cache, so every read has to go to the chip. A part that aliases, or a
 * table with the wrong page in it, shows up as a word carrying another
 * address's value.
 *
 * Returns non-zero if every word came back. On failure *failed_at is the
 * address that did not.
 */
int krnPSRAMVerify(unsigned long size, unsigned long *failed_at)
{
    const unsigned long step = 1024UL * 1024UL;
    volatile unsigned long *p;
    unsigned long a;

    for (a = 0; a < size; a += step)
    {
        p = (volatile unsigned long *)(P4_PSRAM_WINDOW_BASE + a);
        *p = (P4_PSRAM_WINDOW_BASE + a) ^ 0xA5A5A5A5UL;
    }

    for (a = 0; a < size; a += step)
    {
        p = (volatile unsigned long *)(P4_PSRAM_WINDOW_BASE + a);
        if (*p != ((P4_PSRAM_WINDOW_BASE + a) ^ 0xA5A5A5A5UL))
        {
            if (failed_at)
                *failed_at = P4_PSRAM_WINDOW_BASE + a;
            return 0;
        }
    }

    return 1;
}

/*
 * Where this stands.
 *
 * Two things were wrong in the earlier version of this file and both are
 * fixed above. The first was the clock source: taking the bus clock off
 * XTAL to avoid bringing the MPLL up rests on the idea that 20 MHz needs
 * no PLL, and ESP-IDF's own 20 MHz configuration runs the MPLL at 400 MHz
 * and divides by twenty. The divider sits behind the PLL. What a low bus
 * clock saves is the read timing calibration, and nothing else - which
 * makes the slow bring-up a smaller saving than it looked, and the step to
 * 200 MHz shorter, because the PLL will already be there.
 *
 * The second was an address. The source select field is in PERI_CLK_CTRL00
 * and this file wrote PERI_CLK_CTRL01, so the two bits went into the wrong
 * register. It went unnoticed because XTAL is encoded as zero and zero is
 * the reset value, so the bus ran off XTAL either way and the read-back
 * test on the divider still passed. Whatever bits 12 and 13 of CTRL01 are,
 * they were being cleared for no reason.
 *
 * What is still not done, in the order it has to happen: the mode register
 * writes (MR0 latency and drive, MR4 write latency, MR8 burst length and
 * bus width), which ESP-IDF performs before it reads anything back rather
 * than after; mapping the window at 0x48000000, which the bootloader
 * unmaps on its way out; handing the range to exec; and then the clock,
 * where 200 MHz needs the per-board read timing calibration and the pin
 * drive strength that this file deliberately leaves at reset.
 */
