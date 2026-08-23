/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: MIPI-DSI PHY supply, clocks and PLL up to lock and lane stop state.
*/

/*
 * B3, first stage.  The last step before this one that touches the panel is
 * B2's reset pulse; this is the first that touches the data path.
 *
 * The whole register sequence was derived from ESP-IDF before any of it was
 * written, and it is recorded in display/DISPLAY-CONTRACT.md with this board's
 * parameters already substituted.  That file is the reference for why each
 * value is what it is; this one only carries out the sequence.
 *
 * Three things the reference implementation does are deliberately absent.  On
 * ESP32-P4 revision 1.x the PHY PLL reference source select, its divider and
 * the bridge's own soft reset do not exist as registers - all three are
 * hw_ver3 additions - so the reference is fixed by hardware rather than
 * chosen, and the bridge is reset only through the system reset register.
 * Writing the absent fields would be writing reserved bits, and the fact that
 * the reference driver writes them is a property of the revision it was
 * written against.
 *
 * Which leaves one assumption this stage exists to test: that the fixed
 * reference is the 40 MHz crystal.  N=2 and M=50 give exactly 1000 Mbit/s per
 * lane from 40 MHz, and nothing else in reach gives an exact answer, so a PLL
 * that locks is evidence for the assumption and a PLL that does not is the
 * first place to look.
 *
 * Every wait here is bounded by the system timer and every failure returns a
 * named reason, because a bring-up step that can hang is worse than one that
 * reports that it failed.
 */

#include <inttypes.h>
#include <exec/types.h>

#include "hardware.h"
#include "kernel_intern.h"

static unsigned long dsi_rd(unsigned long off)
{
    return p4_r32(P4_DSI_HOST_BASE + off);
}

static void dsi_wr(unsigned long off, unsigned long v)
{
    p4_w32(P4_DSI_HOST_BASE + off, v);
}

static void dsi_set(unsigned long off, unsigned long bits)
{
    dsi_wr(off, dsi_rd(off) | bits);
}

static void dsi_clr(unsigned long off, unsigned long bits)
{
    dsi_wr(off, dsi_rd(off) & ~bits);
}

/*
 * The PHY supply.
 *
 * Channel 3 in the board's terms is LDO unit 2, which is the register pair at
 * PMU + 0x1c0.  Two mappings sit between the name and the register and both
 * are in ESP-IDF rather than in any datasheet: channel to unit is minus one,
 * and unit to register index goes through a four-entry table.  Getting either
 * wrong powers a different rail, which is the reason the contract writes the
 * derivation out.
 *
 * The order is: take ownership from eFuse so software decides, select the
 * regulated output rather than the 3.3 V rail, set the reference and
 * multiplier, and only then enable.  Enabling before the voltage is set would
 * bring the rail up at whatever the reset values happen to encode.
 */
void krnP4DsiLdoUp(void)
{
    unsigned long v;

    v = p4_r32(P4_PMU_EXT_LDO_VO3);
    v |= P4_LDO_FORCE_TIEH_SEL;             /* software owns it, not eFuse */
    v &= ~P4_LDO_TIEH_SEL_MASK;             /* selection 0: use tieh */
    v &= ~P4_LDO_TIEH;                      /* tieh 0: Vref * mul, not 3V3 */
    p4_w32(P4_PMU_EXT_LDO_VO3, v);

    v = p4_r32(P4_PMU_EXT_LDO_VO3_ANA);
    v &= ~(P4_LDO_DREF_MASK | P4_LDO_MUL_MASK);
    v |= (unsigned long)P4_LDO_DREF_2V5 << P4_LDO_DREF_SHIFT;
    v |= (unsigned long)P4_LDO_MUL_2V5 << P4_LDO_MUL_SHIFT;
    p4_w32(P4_PMU_EXT_LDO_VO3_ANA, v);

    p4_w32(P4_PMU_EXT_LDO_VO3, p4_r32(P4_PMU_EXT_LDO_VO3) | P4_LDO_XPD);

    /* Let the rail settle before the PHY is asked to lock to it.  One tick is
       10 ms, which is an order of magnitude more than an on-chip LDO needs and
       costs nothing here. */
    krnTimerWait(1);
}

/*
 * One PHY register, through the DesignWare test interface.
 *
 * The PLL is not memory mapped.  An address is presented on the test data
 * bus and latched by a falling edge of the test clock, then a value is
 * presented and latched by a rising edge.  Both halves keep the test enable
 * asserted for the address and drop it for the value, which is what
 * distinguishes them; that asymmetry is the protocol and not a mistake.
 */
static void dsi_phy_write(unsigned char reg, unsigned char val)
{
    /* Address phase: test enable high, address on the bus. */
    dsi_wr(P4_DSI_PHY_TST_CTRL0, 0);
    dsi_wr(P4_DSI_PHY_TST_CTRL1, P4_DSI_TESTEN | (unsigned long)reg);
    dsi_wr(P4_DSI_PHY_TST_CTRL0, P4_DSI_TESTCLK);
    dsi_wr(P4_DSI_PHY_TST_CTRL0, 0);

    /* Value phase: test enable low, value on the bus. */
    dsi_wr(P4_DSI_PHY_TST_CTRL1, (unsigned long)val);
    dsi_wr(P4_DSI_PHY_TST_CTRL0, P4_DSI_TESTCLK);
    dsi_wr(P4_DSI_PHY_TST_CTRL0, 0);
}

/*
 * Clocks, host, PHY and PLL, in the order the reference uses.
 *
 * The order is not decorative.  The PHY has to be out of shutdown before its
 * reset means anything, the clock lane has to be enabled before the PLL is
 * forced on, and the PLL registers have to be written while the PHY is held
 * in reset - releasing reset is what makes it act on them.
 */
int krnP4DsiPhyUp(struct P4DsiState *out)
{
    uint64_t deadline;
    unsigned long v;

    out->ldo_reg = 0;
    out->ldo_ana = 0;
    out->status = 0;
    out->locked = 0;
    out->lanes_stopped = 0;
    out->pll_n = 2;
    out->pll_m = 50;
    out->hs_freq_sel = 0x2A;

    krnP4DsiLdoUp();
    out->ldo_reg = p4_r32(P4_PMU_EXT_LDO_VO3);
    out->ldo_ana = p4_r32(P4_PMU_EXT_LDO_VO3_ANA);

    /* System clock for the DSI block, then the bridge out of reset. */
    p4_w32(P4_CLKRST_SOC_CLK_CTRL1,
           p4_r32(P4_CLKRST_SOC_CLK_CTRL1) | P4_DSI_SYS_CLK_EN);
    p4_w32(P4_CLKRST_HP_RST_EN0,
           p4_r32(P4_CLKRST_HP_RST_EN0) | P4_RST_EN_DSI_BRG);
    p4_w32(P4_CLKRST_HP_RST_EN0,
           p4_r32(P4_CLKRST_HP_RST_EN0) & ~P4_RST_EN_DSI_BRG);

    /*
     * The PHY's configuration clock and its PLL reference.  The source select
     * for the configuration clock exists on this revision; the one for the PLL
     * reference does not, so the reference is whatever the hardware fixes and
     * only its gate is ours to open.
     */
    v = p4_r32(P4_CLKRST_PERI_CLK_CTRL02);
    v &= ~P4_DSI_DPHY_CLK_SRC_MASK;         /* 0: the 20 MHz PLL tap */
    p4_w32(P4_CLKRST_PERI_CLK_CTRL02, v);

    p4_w32(P4_CLKRST_PERI_CLK_CTRL03,
           p4_r32(P4_CLKRST_PERI_CLK_CTRL03) | P4_DSI_DPHY_CFG_CLK_EN
                                             | P4_DSI_DPHY_PLL_REFCLK_EN);

    /* Lane count, then host and PHY out of shutdown. */
    v = dsi_rd(P4_DSI_PHY_IF_CFG);
    v &= ~P4_DSI_N_LANES_MASK;
    v |= (unsigned long)(P4_DSI_LANES - 1);
    dsi_wr(P4_DSI_PHY_IF_CFG, v);

    dsi_set(P4_DSI_PWR_UP, P4_DSI_SHUTDOWNZ);
    dsi_set(P4_DSI_PHY_RSTZ, P4_DSI_PHY_SHUTDOWNZ);

    /*
     * Held in reset while the PLL is programmed, then released.  The test
     * interface is cleared first, because a stale address left in it would
     * make the first write land somewhere else.
     */
    dsi_clr(P4_DSI_PHY_RSTZ, P4_DSI_PHY_RSTZ_BIT);
    dsi_wr(P4_DSI_PHY_TST_CTRL0, P4_DSI_TESTCLR);
    dsi_wr(P4_DSI_PHY_TST_CTRL0, 0);

    dsi_phy_write(0x44, (unsigned char)(out->hs_freq_sel << 1));
    dsi_phy_write(0x19, 0x30);
    dsi_phy_write(0x17, (unsigned char)(out->pll_n - 1));
    dsi_phy_write(0x18, (unsigned char)((out->pll_m - 1) & 0x1F));
    dsi_phy_write(0x18, (unsigned char)(0x80 | (((out->pll_m - 1) >> 5) & 0x0F)));

    dsi_set(P4_DSI_PHY_RSTZ, P4_DSI_PHY_RSTZ_BIT);
    dsi_set(P4_DSI_PHY_RSTZ, P4_DSI_PHY_ENABLECLK | P4_DSI_PHY_FORCEPLL);

    /*
     * Lock, then stop state, each with its own bound so a failure says which
     * one it was.  10 ms is far more than either takes; the reference waits in
     * one-millisecond steps with no bound at all, which is exactly the shape
     * this port does not permit.
     */
    deadline = krnTimerCount() + P4_SYSTIMER_HZ / 100;
    while (!(dsi_rd(P4_DSI_PHY_STATUS) & P4_DSI_PHY_LOCK))
        if (krnTimerCount() > deadline)
        {
            out->status = dsi_rd(P4_DSI_PHY_STATUS);
            return P4_DSI_NO_LOCK;
        }
    out->locked = 1;

    deadline = krnTimerCount() + P4_SYSTIMER_HZ / 100;
    for (;;)
    {
        unsigned long want = P4_DSI_STOPSTATE_CLK | P4_DSI_STOPSTATE_L0;

        if (P4_DSI_LANES > 1)
            want |= P4_DSI_STOPSTATE_L1;

        out->status = dsi_rd(P4_DSI_PHY_STATUS);
        if ((out->status & want) == want)
            break;
        if (krnTimerCount() > deadline)
            return P4_DSI_NO_STOPSTATE;
    }
    out->lanes_stopped = 1;

    return P4_DSI_OK;
}

/*
 * Everything off again, in the reverse order.
 *
 * Called on failure and safe at any point.  The LDO goes last because the PHY
 * should stop being clocked before its supply is removed rather than after.
 */
void krnP4DsiPhyDown(void)
{
    dsi_clr(P4_DSI_PHY_RSTZ, P4_DSI_PHY_ENABLECLK | P4_DSI_PHY_FORCEPLL);
    dsi_clr(P4_DSI_PHY_RSTZ, P4_DSI_PHY_RSTZ_BIT);
    dsi_clr(P4_DSI_PHY_RSTZ, P4_DSI_PHY_SHUTDOWNZ);
    dsi_clr(P4_DSI_PWR_UP, P4_DSI_SHUTDOWNZ);

    p4_w32(P4_CLKRST_PERI_CLK_CTRL03,
           p4_r32(P4_CLKRST_PERI_CLK_CTRL03) & ~(P4_DSI_DPHY_CFG_CLK_EN
                                               | P4_DSI_DPHY_PLL_REFCLK_EN));

    p4_w32(P4_PMU_EXT_LDO_VO3, p4_r32(P4_PMU_EXT_LDO_VO3) & ~P4_LDO_XPD);
}
