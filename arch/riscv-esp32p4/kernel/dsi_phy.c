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
 * The fixed reference is 40 MHz, and it is now measured rather than assumed -
 * see the derivation in hardware.h.  B3 assumed it and treated a locking PLL
 * as evidence, which it is not: a loop that closes says nothing about the
 * frequency it closed on.  What settled it was configuring the whole link for
 * 20 MHz instead, watching the clock lane stop leaving stop state, and putting
 * it back.
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

static void brg_wr(unsigned long off, unsigned long v)
{
    p4_w32(P4_DSI_BRG_BASE + off, v);
}

static unsigned long brg_rd(unsigned long off)
{
    return p4_r32(P4_DSI_BRG_BASE + off);
}

/*
 * PHY_STATUS at each point where the link's direction can change.
 *
 * This file prints nothing by design and returns state to its callers
 * instead, so the readings accumulate here and the probe reports them.  The
 * question they answer is which step leaves phy_direction set: one reading
 * taken after the fact is consistent with any of the read, the command
 * sequence or the video handover having done it.
 */
unsigned long krnP4DsiPhyTrace[P4_DSI_TRACE_MAX];
unsigned int  krnP4DsiPhyTraceCount;

static void dsi_trace(void)
{
    if (krnP4DsiPhyTraceCount < P4_DSI_TRACE_MAX)
        krnP4DsiPhyTrace[krnP4DsiPhyTraceCount++] = dsi_rd(P4_DSI_PHY_STATUS);
}

/* Match ESP-IDF ldo_ll_voltage_to_dref_mul() exactly for LDO unit 2.  The
 * calibration constants are signed eFuse fields; using the nominal 9/6 pair
 * on every die changes the real PHY rail even though both settings are called
 * "2.5 V" at the API boundary. */
static void dsi_ldo_2v5_params(unsigned char *dref, unsigned char *mul)
{
    unsigned long sys2 = p4_r32(P4_EFUSE_RD_MAC_SYS_2);
    unsigned long sys3 = p4_r32(P4_EFUSE_RD_MAC_SYS_3);
    unsigned int blk_version;
    unsigned int efuse_k = 0;
    unsigned int efuse_vos = 0;
    unsigned int efuse_c = 0;
    int k_1000 = 1000;
    int vos_1000 = 0;
    int c_1000 = 1000;
    int min_diff = 400000000;
    unsigned int d;
    unsigned int m;

    blk_version = ((sys2 & P4_EFUSE_BLK_MAJOR_MASK)
                   >> P4_EFUSE_BLK_MAJOR_SHIFT) * 100;
    blk_version += (sys2 & P4_EFUSE_BLK_MINOR_MASK)
                   >> P4_EFUSE_BLK_MINOR_SHIFT;

    *dref = P4_LDO_DREF_2V5;
    *mul = P4_LDO_MUL_2V5;
    if (blk_version < 1)
        return;

    efuse_k = (sys3 & P4_EFUSE_LDO3_K_MASK) >> P4_EFUSE_LDO3_K_SHIFT;
    efuse_vos = (sys3 & P4_EFUSE_LDO3_VOS_MASK)
                >> P4_EFUSE_LDO3_VOS_SHIFT;
    efuse_c = (sys3 & P4_EFUSE_LDO3_C_MASK) >> P4_EFUSE_LDO3_C_SHIFT;

    if (efuse_k)
        k_1000 = (efuse_k & 0x80) ? 975 - (int)(efuse_k & 0x7f)
                                  : 975 + (int)efuse_k;
    if (efuse_vos)
        vos_1000 = (efuse_vos & 0x20) ? -3 - (int)(efuse_vos & 0x1f)
                                      : (int)efuse_vos - 3;
    if (efuse_c)
        c_1000 = (efuse_c & 0x20) ? 990 - (int)(efuse_c & 0x1f)
                                  : 990 + (int)efuse_c;

    for (d = 0; d < 16; d++) {
        int vref_20 = d < 9 ? 10 + (int)d : 20 + ((int)d - 9) * 2;

        for (m = 0; m < 8; m++) {
            int vout = (vref_20 * k_1000 + 20 * vos_1000)
                       * (4000 + (int)m * c_1000);
            int diff = 2500 * 80000 - vout;

            if (diff < 0)
                diff = -diff;
            if (diff < min_diff) {
                min_diff = diff;
                *dref = d;
                *mul = m;
            }
        }
    }
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
    unsigned char dref;
    unsigned char mul;

    dsi_ldo_2v5_params(&dref, &mul);

    v = p4_r32(P4_PMU_EXT_LDO_VO3);
    v |= P4_LDO_FORCE_TIEH_SEL;             /* software owns it, not eFuse */
    v &= ~P4_LDO_TIEH_SEL_MASK;             /* selection 0: use tieh */
    v &= ~P4_LDO_TIEH;                      /* tieh 0: Vref * mul, not 3V3 */
    p4_w32(P4_PMU_EXT_LDO_VO3, v);

    v = p4_r32(P4_PMU_EXT_LDO_VO3_ANA);
    v &= ~(P4_LDO_DREF_MASK | P4_LDO_MUL_MASK);
    v |= (unsigned long)dref << P4_LDO_DREF_SHIFT;
    v |= (unsigned long)mul << P4_LDO_MUL_SHIFT;
    v |= P4_LDO_EN_VDET;                    /* IDF ripple suppression */
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
 * reset means anything.  ESP-IDF then pulses the digital reset, enables the
 * clock lane, forces the PLL on and only then writes the internal PLL
 * registers through the test interface.  Keeping reset asserted during those
 * writes is observably a different programming sequence even if the PLL later
 * reports lock.
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
    /*
     * The PLL dividers and the range selector all come from the lane rate,
     * which is stated once in hardware.h.  See the derivation there: the
     * reference is 20 MHz and not a choice on this silicon, and this file used
     * to carry values for three different rates at once.
     */
    out->pll_n = P4_DSI_PLL_N;
    out->pll_m = P4_DSI_PLL_M;
    out->hs_freq_sel = P4_DSI_HS_FREQ_SEL;

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

    /*
     * Lane count and the stop-wait time, then host and PHY out of shutdown.
     *
     * The stop-wait time is how long the host holds the lanes in stop state
     * after one transmission before it may begin the next.  This port left it
     * at its reset value of zero, which the reference never does - it sets
     * 0x3F - and a host that is allowed no settling time between transmissions
     * is a plausible reading of a host whose data lanes never leave stop
     * state at all.  It is the last value in this sequence that differed.
     */
    v = dsi_rd(P4_DSI_PHY_IF_CFG);
    v &= ~(P4_DSI_N_LANES_MASK | P4_DSI_STOP_WAIT_MASK);
    v |= (unsigned long)(P4_DSI_LANES - 1);
    v |= (0x3FUL << P4_DSI_STOP_WAIT_SHIFT) & P4_DSI_STOP_WAIT_MASK;
    dsi_wr(P4_DSI_PHY_IF_CFG, v);

    dsi_set(P4_DSI_PWR_UP, P4_DSI_SHUTDOWNZ);
    dsi_set(P4_DSI_PHY_RSTZ, P4_DSI_PHY_SHUTDOWNZ);

    /*
     * Match mipi_dsi_hal_init() before configure_phy_pll(): pulse the digital
     * reset, then enable the clock lane and force the PLL before using the
     * test interface.  The interface is cleared first because a stale address
     * would make the first write land somewhere else.
     */
    dsi_clr(P4_DSI_PHY_RSTZ, P4_DSI_PHY_RSTZ_BIT);
    dsi_set(P4_DSI_PHY_RSTZ, P4_DSI_PHY_RSTZ_BIT);
    dsi_set(P4_DSI_PHY_RSTZ, P4_DSI_PHY_ENABLECLK | P4_DSI_PHY_FORCEPLL);

    dsi_wr(P4_DSI_PHY_TST_CTRL0, P4_DSI_TESTCLR);
    dsi_wr(P4_DSI_PHY_TST_CTRL0, 0);

    dsi_phy_write(0x44, (unsigned char)(out->hs_freq_sel << 1));
    dsi_phy_write(0x19, 0x30);
    dsi_phy_write(0x17, (unsigned char)(out->pll_n - 1));
    dsi_phy_write(0x18, (unsigned char)((out->pll_m - 1) & 0x1F));
    dsi_phy_write(0x18, (unsigned char)(0x80 | (((out->pll_m - 1) >> 5) & 0x0F)));

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
 * Command mode.
 *
 * Everything from here on is the host controller rather than the PHY, and it
 * has to be in place before a single command goes out: the escape clock is
 * what low-power commands are actually clocked by, and a host with the wrong
 * divider sends them at the wrong rate and gets no acknowledgement.
 *
 * The byte clock is the lane rate over eight, 125 MHz here.  The two dividers
 * bring it to the escape clock's 18 MHz and the timeout clock's 10 MHz, and
 * both are the reference's targets rather than anything derived.
 *
 * The timeout counters are set to zero, which disables the timeout mechanism.
 * That is the reference's choice and it is worth naming rather than copying
 * silently: it means a command that never completes waits forever in the
 * hardware, so the bound has to be in this file instead, which is what the
 * loops below are.
 */
int krnP4DsiCmdModeUp(void)
{
    unsigned long byte_clk_mhz = P4_DSI_LANE_MBPS / 8;

    /* Command mode, with the clock lane held in low power.

       This is the exact v6.0 tree Vellum's project_description.json names.
       A later local v6.0.1 tree changed esp_lcd_new_dsi_bus() to select AUTO
       here; P4_B5_IDF_AUTO_CMD_CLOCK reproduced that version difference and
       was hardware-negative, but it is not a Vellum parity setting. */
    dsi_set(P4_DSI_MODE_CFG, P4_DSI_CMD_VIDEO_MODE);
#ifdef P4_B5_IDF_AUTO_CMD_CLOCK
    dsi_set(P4_DSI_LPCLK_CTRL,
            P4_DSI_TXREQUESTCLKHS | P4_DSI_AUTO_CLKLANE);
#else
    dsi_clr(P4_DSI_LPCLK_CTRL,
            P4_DSI_TXREQUESTCLKHS | P4_DSI_AUTO_CLKLANE);
#endif

    /* The four lane-transition times, undocumented and carried over as they
       are; see the display contract's unresolved list. */
    dsi_wr(P4_DSI_PHY_TMR_CFG,
           (104UL << P4_DSI_LP2HS_SHIFT) | (50UL << P4_DSI_HS2LP_SHIFT));
    dsi_wr(P4_DSI_PHY_TMR_LPCLK_CFG,
           (128UL << P4_DSI_CLKLP2HS_SHIFT) | (46UL << P4_DSI_CLKHS2LP_SHIFT));

    /* Receive checking on, and an end-of-transmission packet after each
       high-speed burst. */
    dsi_wr(P4_DSI_PCKHDL_CFG, P4_DSI_CRC_RX_EN | P4_DSI_ECC_RX_EN
                            | P4_DSI_EOTP_TX_EN);

    /* Escape and timeout clocks, rounded the same way the reference rounds
       them: 125/18 is 7 and 125/10 is 13. */
    dsi_wr(P4_DSI_CLKMGR_CFG,
           ((byte_clk_mhz * 2 / 18 + 1) / 2)
           | (((byte_clk_mhz * 2 / 10 + 1) / 2) << P4_DSI_TO_CLK_DIV_SHIFT));

    /* Timeouts disabled, as above. */
    dsi_wr(P4_DSI_TO_CNT_CFG, 0);
    dsi_wr(P4_DSI_HS_RD_TO_CNT, 0);
    dsi_wr(P4_DSI_LP_RD_TO_CNT, 0);
    dsi_wr(P4_DSI_HS_WR_TO_CNT, 0);
    dsi_wr(P4_DSI_LP_WR_TO_CNT, 0);
    dsi_wr(P4_DSI_BTA_TO_CNT, 0);

    dsi_wr(P4_DSI_PHY_TMR_RD_CFG, 6000);

    {
        unsigned long v = dsi_rd(P4_DSI_PHY_IF_CFG);

        v &= ~P4_DSI_STOP_WAIT_MASK;
        v |= 0x3FUL << P4_DSI_STOP_WAIT_SHIFT;
        dsi_wr(P4_DSI_PHY_IF_CFG, v);
    }

    /*
     * Every command type goes out in low power.
     *
     * High speed would be faster and is wrong here: the panel is being
     * configured, not fed pixels, and its controller accepts configuration in
     * low-power mode.  Sending configuration at high speed also requires the
     * clock lane in high speed, which is the state this function has just
     * deliberately left.
     */
    /*
     * Every command type in low power, and an acknowledge request.
     *
     * The DCS group was missing here, and DCS is what a panel is actually
     * spoken to in: the JD9365 initialisation sequence, the display-on, the
     * identity read.  With bits 16 to 19 clear those packets were asked for in
     * high speed while the data lanes sat in stop state, which is a plausible
     * reading of five reads that never answered and a sequence that produced
     * no panel-side evidence of any kind.
     *
     * ACK_RQST_EN asks the peripheral to acknowledge every command, which is
     * a bus turnaround per command.  The reference sets it, and this port did
     * too on that basis, but the two are not in the same position: the
     * reference reads and reports the acknowledgements, and this port has no
     * handler for them at all, so the bit buys the risk of a turnaround that
     * never completes without buying the diagnosis it exists for.
     *
     * Measured under P4_DSI_ACK_REQUEST: with it set, PHY_STATUS after the
     * jd9365 sequence reads 0x15af in some runs and 0x15bd in others - a
     * turnaround still in progress, non-deterministically - and 0x15af then
     * survives the whole handover to video mode unchanged, so the host is
     * still receiving when it should be sending pixels.
     */
    dsi_wr(P4_DSI_CMD_MODE_CFG,
           P4_DSI_GEN_SW_0P_TX | P4_DSI_GEN_SW_1P_TX | P4_DSI_GEN_SW_2P_TX
         | P4_DSI_GEN_SR_0P_TX | P4_DSI_GEN_SR_1P_TX | P4_DSI_GEN_SR_2P_TX
         | P4_DSI_GEN_LW_TX
         | P4_DSI_DCS_SW_0P_TX | P4_DSI_DCS_SW_1P_TX
         | P4_DSI_DCS_SR_0P_TX | P4_DSI_DCS_LW_TX
         | P4_DSI_MAX_RD_PKT_SIZE
#if defined(P4_DSI_ACK_REQUEST) || defined(P4_B5_IDF_CMD_STATE)
         | P4_DSI_ACK_RQST_EN
#endif
           );

    dsi_trace();                /* 0: configured, nothing sent yet */

    return P4_DSI_OK;
}

/* Wait, bounded, for a bit in the packet-status register to clear. */
static int dsi_wait_clear(unsigned long bits)
{
    uint64_t deadline = krnTimerCount() + P4_SYSTIMER_HZ / 50;   /* 20 ms */

    while (dsi_rd(P4_DSI_CMD_PKT_STATUS) & bits)
        if (krnTimerCount() > deadline)
            return P4_DSI_CMD_BUSY;
    return P4_DSI_OK;
}

/* The packet header, which is what actually starts a transmission. */
static int dsi_send_header(unsigned char dt, unsigned char lsb,
                           unsigned char msb)
{
    int r = dsi_wait_clear(P4_DSI_GEN_CMD_FULL);

    if (r != P4_DSI_OK)
        return r;

    dsi_wr(P4_DSI_GEN_HDR,
           ((unsigned long)dt & P4_DSI_GEN_DT_MASK)
           | (0UL << P4_DSI_GEN_VC_SHIFT)               /* virtual channel 0 */
           | ((unsigned long)lsb << P4_DSI_GEN_WC_LSB_SHIFT)
           | ((unsigned long)msb << P4_DSI_GEN_WC_MSB_SHIFT));
    return P4_DSI_OK;
}

/*
 * One DCS write.
 *
 * Three packet shapes, chosen by how much there is to send, and the choice is
 * the protocol's rather than an optimisation: a command with no parameter is a
 * short write with none, one parameter is a short write with one, and anything
 * more is a long write whose payload goes through the FIFO first and whose
 * header carries the byte count.
 */
int krnP4DsiDcsWrite(unsigned char cmd, const unsigned char *param,
                     unsigned int param_bytes)
{
    unsigned int total = 1 + param_bytes;
    int r;

    if (total > 2)
    {
        unsigned long word = cmd;
        unsigned int i, in_word = 1;

        for (i = 0; i < param_bytes; ++i)
        {
            word |= (unsigned long)param[i] << (8 * in_word);
            if (++in_word == 4)
            {
                r = dsi_wait_clear(P4_DSI_GEN_PLD_W_FULL);
                if (r != P4_DSI_OK)
                    return r;
                dsi_wr(P4_DSI_GEN_PLD_DATA, word);
                word = 0;
                in_word = 0;
            }
        }
        if (in_word)
        {
            r = dsi_wait_clear(P4_DSI_GEN_PLD_W_FULL);
            if (r != P4_DSI_OK)
                return r;
            dsi_wr(P4_DSI_GEN_PLD_DATA, word);
        }

        r = dsi_send_header(P4_DSI_DT_DCS_LW,
                            (unsigned char)(total & 0xFF),
                            (unsigned char)(total >> 8));
    }
    else if (total == 2)
        r = dsi_send_header(P4_DSI_DT_DCS_SW_1P, cmd, param[0]);
    else
        r = dsi_send_header(P4_DSI_DT_DCS_SW_0P, cmd, 0);

    return r;
}

/*
 * One DCS read.
 *
 * The panel is told how many bytes it may return, the host is told to expect a
 * bus turnaround, and only then does the read command go out.  Without the
 * maximum-return-size packet the controller may return more than the host has
 * room for; without the turnaround enable the host never listens.
 *
 * Returns P4_DSI_CMD_NO_REPLY if nothing arrives, which for this panel is a
 * possible and documented outcome rather than a failure: no source states that
 * the JD9365 answers a 0x04 identity read, so the caller is told what happened
 * and decides.
 */
int krnP4DsiDcsRead(unsigned char cmd, unsigned char *out, unsigned int want)
{
    unsigned int got = 0;
    int r;

    r = dsi_send_header(P4_DSI_DT_SET_MAX_RET, (unsigned char)(want & 0xFF),
                        (unsigned char)(want >> 8));
    if (r != P4_DSI_OK)
        return r;

    dsi_set(P4_DSI_MODE_CFG, P4_DSI_CMD_VIDEO_MODE);
    dsi_set(P4_DSI_PCKHDL_CFG, P4_DSI_BTA_EN);
    dsi_wr(P4_DSI_GEN_VCID, dsi_rd(P4_DSI_GEN_VCID) & ~P4_DSI_GEN_VCID_RX_MASK);

    r = dsi_send_header(P4_DSI_DT_DCS_READ_0, cmd, 0);
    if (r != P4_DSI_OK)
    {
        dsi_clr(P4_DSI_PCKHDL_CFG, P4_DSI_BTA_EN);
        return r;
    }

    r = dsi_wait_clear(P4_DSI_GEN_RD_CMD_BUSY);
    if (r != P4_DSI_OK)
    {
        dsi_clr(P4_DSI_PCKHDL_CFG, P4_DSI_BTA_EN);
        return r;
    }

    /* The read FIFO going non-empty is the reply arriving. */
    {
        uint64_t deadline = krnTimerCount() + P4_SYSTIMER_HZ / 50;

        while (dsi_rd(P4_DSI_CMD_PKT_STATUS) & P4_DSI_GEN_PLD_R_EMPTY)
            if (krnTimerCount() > deadline)
            {
                dsi_clr(P4_DSI_PCKHDL_CFG, P4_DSI_BTA_EN);
                return P4_DSI_CMD_NO_REPLY;
            }
    }

    /*
     * Drain the FIFO completely, keeping only what was asked for.
     *
     * The condition here used to include got < want, which stops as soon as
     * the caller's buffer is full and leaves the rest in the FIFO.  The FIFO
     * hands out whole thirty-two bit words, so a three-byte identity read
     * leaves one byte behind - and the next read then finds a FIFO that is
     * already non-empty and returns that leftover instead of waiting for its
     * own reply.  That is why the identity read answered and every read after
     * it did not.
     *
     * The reference drains unconditionally and discards the excess, which is
     * what this now does.
     */
    while (!(dsi_rd(P4_DSI_CMD_PKT_STATUS) & P4_DSI_GEN_PLD_R_EMPTY))
    {
        unsigned long word = dsi_rd(P4_DSI_GEN_PLD_DATA);
        unsigned int i;

        for (i = 0; i < 4; ++i)
            if (got < want)
                out[got++] = (unsigned char)((word >> (8 * i)) & 0xFF);
    }

    /*
     * Turn receiving off again, and this is not tidiness.
     *
     * BTA is what turns the link around so the peripheral may answer, and it
     * was switched on here and never off.  PHY_STATUS then reads with
     * PHY_DIRECTION set - the host sitting in receive - and a host in receive
     * does not transmit, which is why the video scanout stalled with a full
     * payload FIFO after any read had been attempted.  One missing clear tied
     * the two open defects together.
     */
#ifndef P4_B5_IDF_CMD_STATE
    dsi_clr(P4_DSI_PCKHDL_CFG, P4_DSI_BTA_EN);
#endif

    return got ? (int)got : P4_DSI_CMD_NO_REPLY;
}

/*
 * What the host thinks happened, for when a read produces nothing.
 *
 * The packet-status register says whether the command was accepted and whether
 * the host is still waiting for a reply; the two interrupt-status registers
 * carry the protocol errors, and a bus turnaround that was not acknowledged
 * shows up there rather than as a timeout.  Reading them is the difference
 * between knowing which of the two ends is wrong and guessing.
 */
void krnP4DsiCmdStatus(unsigned long *pkt, unsigned long *int0,
                       unsigned long *int1)
{
    if (pkt)
        *pkt = dsi_rd(P4_DSI_CMD_PKT_STATUS);
    if (int0)
        *int0 = dsi_rd(P4_DSI_INT_ST0);
    if (int1)
        *int1 = dsi_rd(P4_DSI_INT_ST1);
}

unsigned long krnP4DsiVideoStatus(void)
{
    return dsi_rd(P4_DSI_VID_PKT_STATUS);
}

/*
 * The panel's own initialisation, in the working reference's order.
 *
 * No software reset, and that is a correction rather than an omission.  The
 * first version sent one, on the reasoning that the Espressif panel driver's
 * reset function does so and that a register reset is not the same thing as a
 * pin reset.  But that function is never called on this board: the board layer
 * pulses the reset line through the port expander itself and then goes
 * straight to the panel's init, so the working path contains no software reset
 * at all.  Sending one put the controller back into reset a few milliseconds
 * before it was asked to identify itself, and it did not answer.  B2's
 * hardware pulse, with its 120 ms tail, is the reset this sequence follows.
 *
 * The identity read comes first, before anything is configured, because that
 * is where the reference puts it and because a value read after the page
 * unlock would be answering a different question.  Its result is returned
 * separately from the initialisation's: no source states what this panel
 * replies, so a value is a board fact and a silence is not a failure of the
 * sequence.
 */
int krnP4DsiPanelInit(unsigned char *id, int *id_result)
{
    unsigned int i;
    int r;

    if (id && id_result)
        *id_result = krnP4DsiDcsRead(0x04, id, 3);

    dsi_trace();                /* 1: after the only read on this path */

    {
        static const unsigned char page_user = 0x00;
        static const unsigned char madctl = 0x00;   /* RGB order, no mirror */
        /* The vendor driver derives this from bits_per_pixel: 16 gives 0x55,
           18 gives 0x66 and 24 gives 0x77 */
        static const unsigned char colmod =
            (P4_PANEL_BPP == 24) ? 0x77 : ((P4_PANEL_BPP == 18) ? 0x66 : 0x55);
        static const unsigned char lanes = 0x01;    /* two data lanes */

        r = krnP4DsiDcsWrite(0xE0, &page_user, 1);
        if (r == P4_DSI_OK)
            r = krnP4DsiDcsWrite(0x36, &madctl, 1);
        if (r == P4_DSI_OK)
            r = krnP4DsiDcsWrite(0x3A, &colmod, 1);
        if (r == P4_DSI_OK)
            r = krnP4DsiDcsWrite(0x80, &lanes, 1);
        if (r != P4_DSI_OK)
            return r;
    }

    dsi_trace();                /* 2: after the four framing commands */

    for (i = 0; i < krnP4JD9365InitCount; ++i)
    {
        const struct P4JD9365Cmd *c = &krnP4JD9365Init[i];

        r = krnP4DsiDcsWrite(c->cmd, &c->param, c->param_bytes);
        if (r != P4_DSI_OK)
            return r;

        if (c->delay_ms)
            krnTimerWait((c->delay_ms * P4_TICK_HZ + 999) / 1000);
    }

#ifndef P4_B5_FULL_ATOMIC_START
    dsi_trace();                /* 3: after the whole vendor sequence */
#endif

#ifdef P4_B5_HOST_RESTART
    /*
     * Diagnostic boundary between the completed command stream and video.
     * CMD_PKT_STATUS is 0x50015 here: both external and buffered command and
     * payload paths are empty.  SHUTDOWNZ resets only the DSI host core; the
     * separately powered PHY remains locked and the already configured panel
     * remains awake.  Video timing is staged again by krnP4DsiPatternOn()
     * below, so a clean first frame after this pulse identifies otherwise
     * invisible command-engine state as the poisoned handover state.
     */
    dsi_clr(P4_DSI_PWR_UP, P4_DSI_SHUTDOWNZ);
    dsi_set(P4_DSI_PWR_UP, P4_DSI_SHUTDOWNZ);
#endif

#ifndef P4_SCANOUT_TEST
    return krnP4DsiPanelOn();
#else
    /* The working wrapper calls the underlying DPI panel's init here, which
       starts DMA, video mode and the bridge feed, and only then calls its
       disp_on_off entry point.  B5 therefore defers the valid parameterless
       0x29 until the same three producers are running. */
    return P4_DSI_OK;
#endif
}

/* The valid display-on transaction is deliberately separate from the vendor
 * table.  That table contains 0x29 with one parameter, but DCS defines only a
 * parameterless form; Vellum sends this second transaction after DPI init. */
int krnP4DsiPanelOn(void)
{
    int r = krnP4DsiDcsWrite(0x29, 0, 0);

    if (r != P4_DSI_OK)
        return r;

    krnTimerWait((20 * P4_TICK_HZ + 999) / 1000);
    dsi_trace();                /* after an already-running video stream */

    return P4_DSI_OK;
}

/*
 * The handover to video mode, separated from staging the timing for it.
 *
 * The order against the pixel source is the whole reason this is its own
 * function.  The reference enables the host's video mode after the DMA channel
 * is armed and only then turns on the bridge's DPI output; this port did all
 * three inside the staging call, so the host entered video mode against a DPI
 * input that had nothing behind it, latched a payload error and never
 * recovered - measured as PHY_STATUS 0x15b9 with both data lanes in stop state
 * and DPI_PLD_WR_ERR set.
 */
void krnP4DsiVideoOn(void)
{
    /*
     * Video mode, and the clock lane to high speed with it.  A pixel stream
     * needs the clock lane running; leaving it in low power is what made the
     * command phase safe and is exactly wrong here.
     */
    dsi_clr(P4_DSI_MODE_CFG, P4_DSI_CMD_VIDEO_MODE);

#ifndef P4_B5_ATOMIC_START
    dsi_trace();                /* 5: video mode, clock lane not yet requested */
#endif

    /*
     * Clock lane to the host's own control, and that is two bits.
     *
     * The reference sets auto_clklane_ctrl alongside txrequestclkhs; this port
     * set only the second, which pins the lane in high speed permanently
     * rather than letting the host manage it.  Measured with one bit only: the
     * PLL locks, the clock lane leaves stop state, and both data lanes stay in
     * it - PHY_STATUS 0x15b9, a host that never transmits while its FIFO
     * overflows.
     */
#ifdef P4_B5_SPLIT_AUTO_START
    /* Match the two distinct volatile bitfield writes emitted by Vellum's
       v6.0 esp_lcd_panel_dpi.c object: AUTO first, then HS request.  AROS's
       ordinary combined RMW has the same final value but skips the observable
       intermediate clock-lane FSM state. */
    dsi_set(P4_DSI_LPCLK_CTRL, P4_DSI_AUTO_CLKLANE);
    dsi_set(P4_DSI_LPCLK_CTRL, P4_DSI_TXREQUESTCLKHS);
#else
    dsi_set(P4_DSI_LPCLK_CTRL,
            P4_DSI_TXREQUESTCLKHS | P4_DSI_AUTO_CLKLANE);
#endif

#ifndef P4_B5_ATOMIC_START
    dsi_trace();                /* 6: clock lane in high speed */
#endif
}

/*
 * B4: the host's own test pattern.
 *
 * The point of using the internal generator rather than a framebuffer is that
 * it removes memory from the question.  If a pattern appears, then the panel,
 * the PHY, the command sequence, the DPI timing and the colour coding are all
 * right, and nothing about PSRAM, DMA or cache coherency has been involved.  If
 * it does not appear, the fault is in that list and not in a list twice as long.
 *
 * The horizontal timing is scaled and the vertical is not, which looks
 * asymmetric and is not.  Vertical timing is counted in lines and a line is a
 * line; horizontal timing is counted in the host's own byte-clock, so pixel
 * counts have to be converted by the ratio of the lane rate to the pixel rate.
 * That ratio is 1000 / (40 * 8) = 25/8 here, which is exact, so the conversion
 * is integer arithmetic and not a rounded approximation.
 *
 * The rounding compensation is the reference's and it earns its place: the four
 * scaled intervals do not necessarily sum to the scaled total, and the host
 * takes the total from a separate register.  Without the correction the line
 * time and the sum of its parts disagree by a byte-clock, which is a tear.
 */
int krnP4DsiPatternOn(struct P4DsiPattern *out)
{
    /* Pixels to host byte clocks, from the lane rate rather than a literal */
#define SCALE(x)    P4_DSI_PX_TO_BYTECLK(x)
    unsigned long hsa = SCALE(P4_PANEL_HSYNC);
    unsigned long hbp = SCALE(P4_PANEL_HBP);
    unsigned long hfp = SCALE(P4_PANEL_HFP);
    unsigned long act = SCALE(P4_PANEL_H_RES);
    unsigned long htotal = SCALE(P4_PANEL_H_RES + P4_PANEL_HSYNC
                                + P4_PANEL_HBP + P4_PANEL_HFP);
    long compensation = (long)htotal - (long)(hsa + hbp + act + hfp);
    unsigned long v;

    act = (unsigned long)((long)act + compensation);

    if (out)
    {
        out->hsa = hsa;
        out->hbp = hbp;
        out->hfp = hfp;
        out->hact = act;
        out->hline = act + hsa + hbp + hfp;
        out->frame_mhz = P4_PANEL_DPI_MHZ;
        out->htotal_px = P4_PANEL_H_RES + P4_PANEL_HSYNC + P4_PANEL_HBP
                         + P4_PANEL_HFP;
        out->vtotal_px = P4_PANEL_V_RES + P4_PANEL_VSYNC + P4_PANEL_VBP
                         + P4_PANEL_VFP;
    }

    /* The DPI clock: 240 MHz over six is exactly the 40 MHz the panel wants. */
    v = p4_r32(P4_CLKRST_PERI_CLK_CTRL03);
    v &= ~(P4_DSI_DPICLK_SRC_MASK | P4_DSI_DPICLK_DIV_MASK);
    v |= (unsigned long)P4_DSI_DPICLK_SRC_PLL240 << P4_DSI_DPICLK_SRC_SHIFT;
    v |= (unsigned long)(P4_DSI_DPICLK_DIV - 1) << P4_DSI_DPICLK_DIV_SHIFT;
    v |= P4_DSI_DPICLK_EN;
    p4_w32(P4_CLKRST_PERI_CLK_CTRL03, v);

    /*
     * The bridge, which the first version of this function left alone
     * entirely - and that is why the panel stayed dark.
     *
     * The host's pattern generator replaces the pixel *data* on the DPI
     * interface and not the timing.  On this SoC that interface is fed by the
     * DSI bridge, so a bridge that is neither configured nor enabled means the
     * host has no timing to hang a frame on, and nothing is transmitted at all.
     * Reading the register header rather than the driver hid this: a pattern
     * generator sounds self-contained and is not.
     *
     * The flow controller is the bridge rather than the DMA engine, and that
     * matters here more than anywhere else: with DMA as the controller the
     * bridge waits for data that no one is going to send.
     *
     * These timings are in pixels, unscaled, unlike the host's below.  The
     * bridge counts pixels because it is on the pixel side of the link.
     */
    /*
     * Leave the bridge register-clock force-on bit at its reset value.
     * ESP-IDF exposes this bit only as a diagnostic/bring-up override and its
     * DPI path never sets it.  The working Vellum scanout consequently reads
     * CLK_EN=0, whereas keeping it forced in AROS was the last measured static
     * bridge difference.  The reset-continuity teardown still forces the
     * register clock before touching a possibly gated bridge.
     */

    /*
     * P4_BRG_VDIV is a test of whether the bridge honours its active-line
     * count at all.
     *
     * Halving it stops the transfer dead - 0 bytes moved - rather than halving
     * the rate, so the bridge does read this count and will not run with an
     * active-line count that disagrees with raw_num_total.  That is what the
     * test established.  The suspicion that prompted it, that the bridge emits
     * pixels through vertical blanking, came from a 5 ms rate window inside a
     * 14.6 ms frame and did not survive a 100 ms one.
     */
    brg_wr(P4_DSI_BRG_DPI_V_CFG0,
#ifdef P4_BRG_VDIV
           ((unsigned long)(P4_TX_V_RES / P4_BRG_VDIV)
            << P4_DSI_BRG_DISP_SHIFT)
#else
           ((unsigned long)P4_TX_V_RES << P4_DSI_BRG_DISP_SHIFT)
#endif
           | ((unsigned long)(P4_TX_V_RES + P4_PANEL_VSYNC + P4_PANEL_VBP
                              + P4_PANEL_VFP) << P4_DSI_BRG_TOTAL_SHIFT));
    brg_wr(P4_DSI_BRG_DPI_V_CFG1,
           ((unsigned long)P4_PANEL_VSYNC << P4_DSI_BRG_SYNC_SHIFT)
           | ((unsigned long)(P4_PANEL_VBP
#ifdef P4_DSI_BRG_VBP_DELTA
                              + P4_DSI_BRG_VBP_DELTA
#endif
                              ) << P4_DSI_BRG_BANK_SHIFT));
    brg_wr(P4_DSI_BRG_DPI_H_CFG0,
           ((unsigned long)P4_PANEL_H_RES << P4_DSI_BRG_DISP_SHIFT)
           | ((unsigned long)(P4_PANEL_H_RES + P4_PANEL_HSYNC + P4_PANEL_HBP
                              + P4_PANEL_HFP
#ifdef P4_DSI_BRG_HLINE_DELTA
                              + P4_DSI_BRG_HLINE_DELTA
#endif
                             ) << P4_DSI_BRG_TOTAL_SHIFT));
    brg_wr(P4_DSI_BRG_DPI_H_CFG1,
           ((unsigned long)P4_PANEL_HSYNC << P4_DSI_BRG_SYNC_SHIFT)
           | ((unsigned long)P4_PANEL_HBP << P4_DSI_BRG_BANK_SHIFT));

    /* RGB565 in and out; both codes are zero, which is worth stating rather
       than leaving as an untouched register. */
    brg_wr(P4_DSI_BRG_PIXEL_TYPE, 0);

    brg_wr(P4_DSI_BRG_DMA_FLOW_CTL, P4_DSI_BRG_FLOW_BRIDGE);

    /*
     * The bridge is enabled and its pixel feed is not, and the two are
     * separate bits for a reason this stage found the hard way.
     *
     * Leaving the bridge off entirely left the panel dark.  Turning it fully
     * on, pixel feed included, produced DPI_PLD_WR_ERR in the host's second
     * interrupt-status register - a payload write error, which is an overflow:
     * the bridge was pushing pixels into a host that generates its own and
     * consumes none.  So `dsi_en` alone, which brings the link up, and
     * `dpi_en` clear, which stops the feed.  With a framebuffer in B5 it will
     * be the other way round and the generator will be off.
     */
    /*
     * The bridge's pixel feed, under a switch, to settle an architecture
     * question this phase cannot answer otherwise.
     *
     * Left clear, the bridge brings the link up but produces no pixels, and the
     * host's own pattern generator is supposed to supply them.  The panel is
     * lit and stays dark, and there is no counter anywhere that says whether
     * the generator emits anything at all - it is a DesignWare feature the
     * vendor reference never uses on this SoC.
     *
     * Set, the bridge asks for pixels it has no DMA to fetch, so it must
     * underrun.  That underrun is the measurement: it proves the pixel path
     * runs through the bridge and that the generator is not feeding it, which
     * is what decides whether B4 can be finished at all or whether the
     * framebuffer of B5 is the only way to put an image on this panel.
     */
#ifdef P4_PATTERN_BRIDGE_FEED
    brg_wr(P4_DSI_BRG_DPI_MISC_CFG, P4_DSI_BRG_DPI_EN);
#elif defined(P4_SCANOUT_TEST)
    /* B5 configures and enables the feed itself, after the DMA is armed */
#else
    brg_wr(P4_DSI_BRG_DPI_MISC_CFG, 0);
#endif
    /* B4 owns the bridge immediately.  B5 instead defers the first enable and
       commit until its DMA flow, raw count and thresholds are all staged, as
       esp_lcd_new_panel_dpi() does.  Enabling this partial bridge-flow state
       here and changing it live later leaves identical final registers but a
       different internal counter/FIFO start state. */
#ifndef P4_SCANOUT_TEST
    brg_wr(P4_DSI_BRG_EN, P4_DSI_BRG_DSI_EN);
    brg_wr(P4_DSI_BRG_DPI_CFG_UPD, P4_DSI_BRG_CFG_UPDATE);
#endif

    /* Virtual channel 0, RGB565, every sync signal active high. */
    dsi_wr(P4_DSI_DPI_VCID, 0);
#if P4_PANEL_BPP == 24
    dsi_wr(P4_DSI_DPI_COLOR_CODING, P4_DSI_COLOR_24BIT);
#else
    /* P4_DSI_565_CFG selects which of the three 16-bit codings; 1, 2 or 3 */
    dsi_wr(P4_DSI_DPI_COLOR_CODING,
           (unsigned long)(P4_DSI_565_CFG - 1));
#endif
    dsi_wr(P4_DSI_DPI_CFG_POL, 0);

    /*
     * Burst mode with sync pulses, and no low-power transitions anywhere in
     * the frame.  Low power between video periods saves energy and is a
     * complication this stage does not need: a pattern that only fails when
     * the link drops to low power and back would be a harder fault to read
     * than a pattern that never does.
     */
    /*
     * Burst with sync pulses, and for the scanout path every low-power
     * transition and the frame acknowledge as well - which is what the
     * reference does, its disable_lp flag being false for this panel.
     *
     * B4 cleared all of them deliberately, to keep the number of moving parts
     * down while diagnosing.  That traded a configuration known to work on
     * this hardware for one that does not, and the host stalled with
     * DPI_PLD_WR_ERR.  The transitions are what give the host somewhere to go
     * between lines; without them it carries high-speed continuously.
     *
     * Scoped to the scanout path rather than turned on everywhere, because
     * the acknowledge asks the panel to answer each frame and no read from
     * this panel has ever answered.
     */
#ifdef P4_SCANOUT_TEST
    /*
     * Low-power transitions yes, frame acknowledge no.
     *
     * Measured: with the acknowledge on, PHY_STATUS reads 0x15b9 - the clock
     * lane in high speed, the PLL locked, and both data lanes sitting in stop
     * state.  The host is not transmitting at all.  Frame acknowledge makes it
     * wait for the panel to answer every frame, and nothing this port has ever
     * read from this panel has answered, so the wait cannot end.
     *
     * The reference can afford it because its reads work.  Until the silent
     * read path is understood, asking for an acknowledgement is asking the
     * host to stop.
     */
#ifdef P4_DSI_NO_LP
#define P4_DSI_LP_BITS  0UL
#elif defined(P4_DSI_LP_VERT_ONLY)
#define P4_DSI_LP_BITS  P4_DSI_VID_LP_VERT
#else
#define P4_DSI_LP_BITS  P4_DSI_VID_LP_ALL
#endif

/*
 * Exact final command-read state seen in the working Vellum image.
 *
 * These two bits only permit/request turnaround for command transactions;
 * setting them after the last command cannot create an unhandled command
 * acknowledgement.  They are kept behind one diagnostic switch because the
 * normal AROS read path deliberately returns the host to transmit-only state.
 */
#ifdef P4_B5_REF_READ_STATE
    dsi_set(P4_DSI_PCKHDL_CFG, P4_DSI_BTA_EN);
    dsi_set(P4_DSI_CMD_MODE_CFG, P4_DSI_ACK_RQST_EN);
#endif

/*
 * Frame acknowledge, which the reference enables and this port does not.
 *
 * It makes the host request a bus turnaround after every frame and wait for
 * the panel to answer, which couples the two: without it the host and the
 * panel each run on their own clock and nothing keeps their frame boundaries
 * together.  The panel showing the framebuffer more than once while the DMA
 * reads it exactly once per panel frame is what an uncoupled pair looks like.
 *
 * The two earlier attempts at this bit are not evidence.  The first wrote bit
 * 11, which is LP_VACT_EN, so it enabled a low-power transition while being
 * recorded as a frame acknowledge.  The second ran with ACK_RQST_EN still set,
 * so the link was already turned around before the frame acknowledge could be
 * judged - and that is why it was recorded as stopping the host.
 *
 * Measured properly, on a link that is otherwise correct: the panel goes
 * black.  The registers still report a transmitting host, which they would for
 * the first frame; the panel does not answer the turnaround, so no second
 * frame follows.  The reference can enable this bit because its reads are part
 * of a driver that services them, and this port's panel evidently does not
 * acknowledge frames unasked.  Off by default, and the switch stays so the
 * measurement can be repeated rather than remembered.
 */
#ifdef P4_DSI_FRAME_ACK
#define P4_DSI_LP_SET   (P4_DSI_LP_BITS | P4_DSI_VID_FRAME_ACK_EN)
#else
#define P4_DSI_LP_SET   P4_DSI_LP_BITS
#endif
#ifdef P4_DSI_NONBURST
    dsi_wr(P4_DSI_VID_MODE_CFG,
           P4_DSI_VID_NONBURST_PULSES | P4_DSI_LP_SET);
#else
    dsi_wr(P4_DSI_VID_MODE_CFG,
           P4_DSI_VID_BURST_SYNC_PULSES | P4_DSI_LP_SET);
#endif
#else
    dsi_wr(P4_DSI_VID_MODE_CFG, P4_DSI_VID_BURST_SYNC_PULSES);
#endif
    dsi_wr(P4_DSI_DPI_LP_CMD_TIM, 0);

    /*
     * One packet per line, or several.
     *
     * The reference sends one - trunks_num 0 - which asks the host's payload
     * fifo to hold a whole line: 800 pixels, and at twenty-four bits that is
     * 2,400 bytes.  DPI_PLD_WR_ERR is that fifo overflowing, and it is
     * permanent at 80 MHz and intermittent at 60, which is the signature of a
     * buffer that is too small rather than a configuration that is wrong.
     * Splitting the line into chunks reduces what has to be held at once;
     * VID_PKT_SIZE times VID_NUM_CHUNKS has to equal the active width.
     */
#ifdef P4_DSI_CHUNKS
    dsi_wr(P4_DSI_VID_PKT_SIZE, P4_PANEL_H_RES / P4_DSI_CHUNKS);
    dsi_wr(P4_DSI_VID_NUM_CHUNKS, P4_DSI_CHUNKS);
#else
    dsi_wr(P4_DSI_VID_PKT_SIZE, P4_PANEL_H_RES);
    dsi_wr(P4_DSI_VID_NUM_CHUNKS, 0);
#endif
    dsi_wr(P4_DSI_VID_NULL_SIZE, 0);

    dsi_wr(P4_DSI_VID_HSA_TIME, hsa);
    dsi_wr(P4_DSI_VID_HBP_TIME, hbp);
#ifdef P4_DSI_HLINE_DELTA
    /*
     * UART-only rate-mismatch diagnostic.  Keep the bridge timing fixed and
     * move only the host's line period so the first-frame DPI FIFO overflow
     * can be tested for sensitivity to producer/consumer phase.  Never use a
     * delta as a display mode without a subsequent visual geometry gate.
     */
    dsi_wr(P4_DSI_VID_HLINE_TIME,
           (unsigned long)((long)(act + hsa + hbp + hfp)
                           + (long)P4_DSI_HLINE_DELTA));
#else
    dsi_wr(P4_DSI_VID_HLINE_TIME, act + hsa + hbp + hfp);
#endif
    dsi_wr(P4_DSI_VID_VSA_LINES, P4_PANEL_VSYNC);
    dsi_wr(P4_DSI_VID_VBP_LINES, P4_PANEL_VBP);
    dsi_wr(P4_DSI_VID_VFP_LINES, P4_PANEL_VFP);
    dsi_wr(P4_DSI_VID_VACTIVE_LINES, P4_TX_V_RES);

#ifndef P4_B5_FULL_ATOMIC_START
    dsi_trace();                /* 4: video registers staged, still command mode */
#endif

#ifndef P4_SCANOUT_TEST
    krnP4DsiVideoOn();
#endif

    /* Vertical colour bars: pattern mode 0, orientation 0. */
    /*
     * The generator and the bridge's pixel feed are alternatives, not layers.
     *
     * Measured: with the feed on and the generator on, the host reports a
     * payload write error and the bridge reports no underrun - so the bridge
     * is delivering pixels and the host cannot take them while generating its
     * own.  With the feed on and the generator off, the bridge's pixels are
     * the only ones on the link, which is the path the vendor reference uses
     * and the one B5's framebuffer has to feed.
     */
#if !defined(P4_PATTERN_BRIDGE_FEED) && !defined(P4_SCANOUT_TEST)
    dsi_set(P4_DSI_VID_MODE_CFG, P4_DSI_VPG_EN);
#endif
#ifdef P4_DSI_VPG
    /*
     * The host's own generator, as a diagnostic rather than a configuration.
     *
     * It produces its pattern from the same video timing the panel is being
     * driven with, but without the bridge and without the DMA.  That splits
     * the chain in half: if the panel shows the generator's bars twice down
     * its height, the doubling is in the host's timing or in the panel, and if
     * it shows them once, the doubling is in what feeds the host.
     *
     * Horizontal bars, because the doubling is along the line axis and
     * vertical bars would be invariant under it.
     */
    dsi_set(P4_DSI_VID_MODE_CFG, P4_DSI_VPG_EN | P4_DSI_VPG_ORIENTATION);
#endif

    dsi_trace();                /* 7: end of the handover */

    if (out)
        out->brg_en = brg_rd(P4_DSI_BRG_EN);

    return P4_DSI_OK;
#undef SCALE
}

/* Pattern off, back to command mode with the clock lane in low power. */
void krnP4DsiPatternOff(void)
{
    dsi_clr(P4_DSI_VID_MODE_CFG, P4_DSI_VPG_EN);
    dsi_clr(P4_DSI_LPCLK_CTRL, P4_DSI_TXREQUESTCLKHS | P4_DSI_AUTO_CLKLANE);
    dsi_set(P4_DSI_MODE_CFG, P4_DSI_CMD_VIDEO_MODE);
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
