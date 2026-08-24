/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: A frame from PSRAM to the panel: bridge, DMA and one link-list item.
*/

/*
 * Why this exists rather than B4's pattern generator.
 *
 * B4 asked the DSI host to generate its own colour bars, which is a
 * DesignWare feature the vendor's reference never uses on this SoC.  The host
 * reported no error, no counter said whether it emitted anything, and the
 * panel stayed dark.  Turning the bridge's pixel feed on then produced
 * DPI_PLD_WR_ERR whether or not the generator was running, while the bridge
 * never underran - so the bridge was pushing pixels the host could not accept,
 * and it was doing so because B4 had left it as its own flow controller.
 *
 * The reference's path is the one that works on this hardware: a frame in
 * PSRAM, the DMA as flow controller, and the bridge asking for pixels rather
 * than pushing them.  Everything here follows it, with the values read out of
 * esp_lcd_panel_dpi.c and recorded in DISPLAY-CONTRACT.md.
 *
 * One link-list item carries the whole frame.  That is not a simplification of
 * the reference; its own comment says it assumes exactly that.  The item's
 * next-pointer is made to point at itself so the transfer repeats without an
 * interrupt to restart it, because scanout has to be continuous and this port
 * has no DMA interrupt handler yet.
 */

#include <inttypes.h>
#include <exec/types.h>

#include "hardware.h"
#include "kernel_intern.h"
#include "psram.h"

static inline void brg_wr(unsigned long off, unsigned long v)
{
    p4_w32(P4_DSI_BRG_BASE + off, v);
}

static inline unsigned long brg_rd(unsigned long off)
{
    return p4_r32(P4_DSI_BRG_BASE + off);
}

static inline void ch_wr(unsigned long off, unsigned long v)
{
    p4_w32(P4_DMAC_CH1 + off, v);
}

static inline unsigned long ch_rd(unsigned long off)
{
    return p4_r32(P4_DMAC_CH1 + off);
}

/*
 * The link-list item lives in internal SRAM, not in the frame it describes.
 *
 * The DMA fetches it over AXI, so it has to be somewhere the DMA can read
 * coherently.  Internal SRAM needs no cache maintenance; PSRAM would, and a
 * descriptor read through a stale cache line is a fault that looks like
 * anything else.  Sixty-four byte alignment is the hardware's requirement.
 */
static unsigned char scanout_lli[P4_DMAC_LLI_SIZE]
    __attribute__((aligned(64)));

static inline void lli_wr(unsigned long off, unsigned long v)
{
    *(volatile unsigned long *)((unsigned long)scanout_lli + off) = v;
}

/*
 * Fill the frame with one colour, so that a picture on the panel is
 * unambiguous: a wrong pixel format or a wrong stride cannot produce a flat
 * field of the colour that was asked for.
 */
void krnP4ScanoutFill(unsigned short rgb565)
{
    volatile unsigned long *p = (volatile unsigned long *)P4_FB_BASE;
    unsigned long pair = ((unsigned long)rgb565 << 16) | rgb565;
    unsigned long i;

    for (i = 0; i < P4_FB_BYTES / 4; i++)
        p[i] = pair;
}

/*
 * The bridge, configured the way the reference configures it.
 *
 * Order matters at the end: the timing and counts are staged, DPI_CFG_UPD
 * commits them, and only then is the pixel feed allowed to run.
 */
void krnP4ScanoutBridgeUp(void)
{
    unsigned long v;

    /* How much data one frame is, in sixty-four-bit words, and a reload of
       the internal counter from it */
    v = (P4_FB_WORDS64 & P4_DSI_BRG_RAW_NUM_MASK) | P4_DSI_BRG_RAW_NUM_SET;
    brg_wr(P4_DSI_BRG_RAW_NUM_CFG, v);

    /* AXI burst length towards memory */
    v = brg_rd(P4_DSI_BRG_DMA_REQ_CFG) & ~P4_DSI_BRG_BURST_LEN_MASK;
    brg_wr(P4_DSI_BRG_DMA_REQ_CFG, v | 256UL);

    /* How much to throw away after an underrun: one line */
    v = brg_rd(P4_DSI_BRG_DPI_MISC_CFG) & ~P4_DSI_BRG_DISCARD_MASK;
    v |= ((unsigned long)P4_PANEL_H_RES << P4_DSI_BRG_DISCARD_SHIFT)
         & P4_DSI_BRG_DISCARD_MASK;
    brg_wr(P4_DSI_BRG_DPI_MISC_CFG, v);

    /* The DMA is the flow controller, not the bridge.  This is the register
       B4 had the other way round. */
    v = brg_rd(P4_DSI_BRG_DMA_FLOW_CTRL);
    v &= ~(1UL | P4_DSI_BRG_MULTIBLK_MASK);
    v |= P4_DSI_BRG_FLOW_DMA
         | ((1UL << P4_DSI_BRG_MULTIBLK_SHIFT) & P4_DSI_BRG_MULTIBLK_MASK);
    brg_wr(P4_DSI_BRG_DMA_FLOW_CTRL, v);

    /* One block per frame, so no multi-block interval handling */
    brg_wr(P4_DSI_BRG_DMA_FRAME_INT,
           brg_rd(P4_DSI_BRG_DMA_FRAME_INT) & ~P4_DSI_BRG_MULTIBLK_EN);

    /* When to ask for more data */
    v = brg_rd(P4_DSI_BRG_EMPTY_THRD) & ~P4_DSI_BRG_EMPTY_MASK;
    brg_wr(P4_DSI_BRG_EMPTY_THRD, v | (1024UL - 256UL));

    brg_wr(P4_DSI_BRG_DPI_CFG_UPD, P4_DSI_BRG_CFG_UPDATE);
}

/*
 * Let the bridge pull pixels.  Separate from the configuration above because
 * the DMA has to be armed first: a feed enabled against a channel that is not
 * running is an underrun by construction.
 */
void krnP4ScanoutFeedOn(void)
{
    brg_wr(P4_DSI_BRG_DPI_MISC_CFG,
           brg_rd(P4_DSI_BRG_DPI_MISC_CFG) | P4_DSI_BRG_DPI_EN);
    brg_wr(P4_DSI_BRG_DPI_CFG_UPD, P4_DSI_BRG_CFG_UPDATE);
}

/*
 * The DMA channel and its one item.
 *
 * Source is the frame in PSRAM, incrementing.  Destination is the bridge's
 * single memory port, fixed - it is a FIFO, not a buffer.  Both sides move
 * sixty-four bits at a time, which is what makes the frame a whole number of
 * items and what the bridge's raw_num_total counts.
 */
void krnP4ScanoutDmaUp(void)
{
    unsigned long ctl_lo, ctl_hi, cfg1;

    /*
     * The module's clocks and system reset first.  They are not in the DMA's
     * own register block, and without them the block still answers reads and
     * still accepts a channel enable - it simply never fetches a descriptor.
     */
    p4_w32(P4_CLKRST_SOC_CLK_CTRL0,
           p4_r32(P4_CLKRST_SOC_CLK_CTRL0) | P4_GDMA_CPU_CLK_EN);
    p4_w32(P4_CLKRST_SOC_CLK_CTRL1,
           p4_r32(P4_CLKRST_SOC_CLK_CTRL1) | P4_GDMA_SYS_CLK_EN);

    p4_w32(P4_CLKRST_HP_RST_EN0,
           p4_r32(P4_CLKRST_HP_RST_EN0) | P4_RST_EN_GDMA);
    p4_w32(P4_CLKRST_HP_RST_EN0,
           p4_r32(P4_CLKRST_HP_RST_EN0) & ~P4_RST_EN_GDMA);

    /* Then the controller's own soft reset */
    p4_w32(P4_DMAC_RESET, P4_DMAC_RESET_BIT);
    while (p4_r32(P4_DMAC_RESET) & P4_DMAC_RESET_BIT)
        ;

    p4_w32(P4_DMAC_CFG, P4_DMAC_CFG_EN);
    p4_w32(P4_DMAC_CHEN, P4_DMAC_CH1_EN_WE);

    ctl_lo = (P4_DMAC_WIDTH_64 << P4_DMAC_SRC_WIDTH_SHIFT)
           | (P4_DMAC_WIDTH_64 << P4_DMAC_DST_WIDTH_SHIFT)
           | (P4_DMAC_MSIZE_512 << P4_DMAC_SRC_MSIZE_SHIFT)
           | (P4_DMAC_MSIZE_256 << P4_DMAC_DST_MSIZE_SHIFT)
           | P4_DMAC_DINC_FIXED;          /* source increments, sink does not */

    ctl_hi = P4_DMAC_ARLEN_EN
           | (P4_DMAC_AXI_BURST_LEN << P4_DMAC_ARLEN_SHIFT)
           | P4_DMAC_AWLEN_EN
           | (P4_DMAC_AXI_BURST_LEN << P4_DMAC_AWLEN_SHIFT)
           | P4_DMAC_LLI_VALID;
    /*
     * Deliberately not P4_DMAC_LLI_LAST.  The reference marks its single item
     * last and restarts the channel from a transfer-done interrupt; this port
     * has no such handler, so the item points at itself instead and the
     * hardware repeats it.  Scanout has to be continuous either way.
     */

    lli_wr(P4_DMAC_LLI_SAR_LO, P4_FB_BASE);
    lli_wr(P4_DMAC_LLI_SAR_HI, 0);
    lli_wr(P4_DMAC_LLI_DAR_LO, P4_DSI_BRG_MEM_BASE);
    lli_wr(P4_DMAC_LLI_DAR_HI, 0);
    lli_wr(P4_DMAC_LLI_BLOCK_TS, P4_FB_WORDS64 - 1);
    lli_wr(P4_DMAC_LLI_LLP_LO, (unsigned long)scanout_lli);
    lli_wr(P4_DMAC_LLI_LLP_HI, 0);
    lli_wr(P4_DMAC_LLI_CTL_LO, ctl_lo);
    lli_wr(P4_DMAC_LLI_CTL_HI, ctl_hi);

    /*
     * Push the descriptor out of the cache before the channel is told where it
     * is.  The DMA reads it over AXI and does not see the CPU's caches, so a
     * descriptor still sitting in a dirty line is a descriptor of zeroes as far
     * as the engine is concerned - which reads back as a channel that is
     * enabled and never starts.
     */
    krnP4CacheWriteback();

    /* Both sides walk a link list */
    ch_wr(P4_DMAC_CH_CFG0,
          (P4_DMAC_MULTBLK_LIST << P4_DMAC_SRC_MULTBLK_SHIFT)
        | (P4_DMAC_MULTBLK_LIST << P4_DMAC_DST_MULTBLK_SHIFT));

    /*
     * Memory to peripheral with the DMA in charge, hardware handshake on both
     * sides, the DSI as the peripheral, and the outstanding-request depths the
     * reference uses - five reads against two writes, because PSRAM latency is
     * the thing that has to be hidden.
     */
    cfg1 = (P4_DMAC_TT_FC_M2P_DMAC << P4_DMAC_TT_FC_SHIFT)
         | (P4_DMAC_PER_DSI << P4_DMAC_SRC_PER_SHIFT)
         | (P4_DMAC_PER_DSI << P4_DMAC_DST_PER_SHIFT)
         | (1UL << P4_DMAC_CH_PRIOR_SHIFT)
         | (4UL << P4_DMAC_SRC_OSR_SHIFT)
         | (1UL << P4_DMAC_DST_OSR_SHIFT);
    ch_wr(P4_DMAC_CH_CFG1, cfg1);

    /* Where the list starts */
    ch_wr(P4_DMAC_CH_LLP, (unsigned long)scanout_lli);

    /* And run */
    p4_w32(P4_DMAC_CHEN, P4_DMAC_CH1_EN | P4_DMAC_CH1_EN_WE);
}

/*
 * What the host actually holds, read back rather than remembered.
 *
 * Comparing written values against the reference has run out: every one of
 * them matches and the host still refuses payload.  What has never been
 * checked is whether the writes took.  MODE_CFG is the one that decides
 * everything else - this port clears CMD_VIDEO_MODE to enter video mode, and a
 * host still in command mode has no video path at all, which would present as
 * exactly the payload write error into a full FIFO that is being seen.
 *
 * The same reasoning found the DMA descriptor sitting in a dirty cache line:
 * every register said configured, and the one thing not read back was the one
 * thing wrong.
 */
void krnP4HostState(struct P4HostState *out)
{
    out->pwr_up     = p4_r32(P4_DSI_HOST_BASE + P4_DSI_PWR_UP);
    out->mode_cfg   = p4_r32(P4_DSI_HOST_BASE + P4_DSI_MODE_CFG);
    out->vid_mode   = p4_r32(P4_DSI_HOST_BASE + P4_DSI_VID_MODE_CFG);
    out->lpclk      = p4_r32(P4_DSI_HOST_BASE + P4_DSI_LPCLK_CTRL);
    out->phy_status = p4_r32(P4_DSI_HOST_BASE + P4_DSI_PHY_STATUS);
    out->pkt_size   = p4_r32(P4_DSI_HOST_BASE + P4_DSI_VID_PKT_SIZE);
    out->hsa        = p4_r32(P4_DSI_HOST_BASE + P4_DSI_VID_HSA_TIME);
    out->hbp        = p4_r32(P4_DSI_HOST_BASE + P4_DSI_VID_HBP_TIME);
    out->hline      = p4_r32(P4_DSI_HOST_BASE + P4_DSI_VID_HLINE_TIME);
    out->vactive    = p4_r32(P4_DSI_HOST_BASE + P4_DSI_VID_VACTIVE_LINES);
    out->colour     = p4_r32(P4_DSI_HOST_BASE + P4_DSI_DPI_COLOR_CODING);
}

void krnP4ScanoutState(struct P4ScanoutState *out)
{
    out->lli          = (unsigned long)scanout_lli;
    out->chen         = p4_r32(P4_DMAC_CHEN);
    out->ch_cfg1      = ch_rd(P4_DMAC_CH_CFG1);
    out->ch_llp       = ch_rd(P4_DMAC_CH_LLP);
    out->ch_sar       = ch_rd(P4_DMAC_CH_SAR);
    out->brg_flow     = brg_rd(P4_DSI_BRG_DMA_FLOW_CTRL);
    out->brg_raw_num  = brg_rd(P4_DSI_BRG_RAW_NUM_CFG);
    out->brg_misc     = brg_rd(P4_DSI_BRG_DPI_MISC_CFG);
    out->brg_int      = brg_rd(P4_DSI_BRG_INT_RAW);
    out->words64      = P4_FB_WORDS64;
    out->fb_base      = P4_FB_BASE;

    /*
     * The bridge's own DPI timing, read back.  Its calculation and its field
     * positions have both been checked against the reference and match; what
     * has not been checked is whether the writes landed, and that is the only
     * remaining thing this port has ever got wrong twice.
     */
    out->brg_v_cfg0   = brg_rd(P4_DSI_BRG_DPI_V_CFG0);
    out->brg_v_cfg1   = brg_rd(P4_DSI_BRG_DPI_V_CFG1);
    out->brg_h_cfg0   = brg_rd(P4_DSI_BRG_DPI_H_CFG0);
    out->brg_h_cfg1   = brg_rd(P4_DSI_BRG_DPI_H_CFG1);
    out->brg_en       = brg_rd(P4_DSI_BRG_EN);
    out->brg_pixel    = brg_rd(P4_DSI_BRG_PIXEL_TYPE);
}
