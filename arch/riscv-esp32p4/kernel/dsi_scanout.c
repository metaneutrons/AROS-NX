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
 * the reference; its own comment says it assumes exactly that.  The item is
 * marked last, and the DMA-complete interrupt restores the VALID bit and
 * re-arms it exactly as Espressif's dpi driver does.  Automatic register reload
 * removed the frame boundary and let the linear framebuffer start part-way
 * through a panel line, which folded the four framebuffer corners into pairs
 * at the middle of the panel's short edges.
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

/* The DesignWare engine reads this over AXI and clears VALID when it consumes
 * the item.  The CPU discards its cached view once during setup and thereafter
 * rewrites VALID only through the P4's non-cacheable internal-SRAM alias. */
static unsigned char scanout_lli[P4_DMAC_LLI_SIZE]
    __attribute__((aligned(64)));
static unsigned long scanout_lli_ctl_hi;
static volatile unsigned long scanout_dma_frames;
static volatile unsigned long scanout_dma_faults;

static inline void lli_wr(unsigned long off, unsigned long v)
{
    *(volatile unsigned long *)((unsigned long)scanout_lli
                                + P4_L2MEM_NONCACHE_OFFSET + off) = v;
}

/*
 * Fill the frame with one colour, so that a picture on the panel is
 * unambiguous: a wrong pixel format or a wrong stride cannot produce a flat
 * field of the colour that was asked for.
 *
 * Three bytes per pixel do not divide into a 32-bit word, but four pixels are
 * twelve bytes, which is three words.  Writing that repeating triple is what
 * keeps this a word-wide loop over two megabytes rather than a byte one.
 */
/*
 * One pixel, written in whatever format the panel is being driven with.
 *
 * The patterns below are written once against this rather than three times
 * against two formats: the port has already changed pixel depth twice, and
 * each pattern that carries its own packing is another place for the two to
 * disagree.  The argument is 0xRRGGBB regardless; RGB565 quantises it.
 */
static inline void px(volatile unsigned char *at, unsigned long rgb)
{
#if P4_PANEL_BPP == 24
    at[0] = (unsigned char)(rgb & 0xFF);
    at[1] = (unsigned char)((rgb >> 8) & 0xFF);
    at[2] = (unsigned char)((rgb >> 16) & 0xFF);
#else
    unsigned int v = (unsigned int)(((rgb >> 19) & 0x1F) << 11)
                   | (unsigned int)(((rgb >> 10) & 0x3F) << 5)
                   | (unsigned int)((rgb >> 3) & 0x1F);

    /*
     * Native little-endian RGB565, as used by Espressif's D1001 BSP.
     *
     * The complete primary/pair-colour card makes this bit-exact rather than
     * an inference from thin grid lines.  High-byte-first changed requested
     * R/G/B into the values 0x00f8/0xe007/0x1f00 as read by the bridge, which
     * arrived as blue/red/green; yellow/cyan/magenta changed to
     * magenta/yellow/cyan in the same way.  White remained white, which is why
     * geometry could be clean while the byte order was still wrong.
     */
    at[0] = (unsigned char)(v & 0xFF);
    at[1] = (unsigned char)(v >> 8);
#endif
}

void krnP4ScanoutFill(unsigned long rgb)
{
    volatile unsigned char *fb = (volatile unsigned char *)P4_FB_BASE;
    unsigned long i;

    for (i = 0; i < (unsigned long)P4_PANEL_H_RES * P4_TX_V_RES; i++)
        px(fb + i * P4_FB_BYTES_PER_PIXEL, rgb);
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

    /*
     * The pixel format first, because zero means RGB888.
     *
     * This register was read out as a diagnostic and never written, so the
     * bridge fetched three bytes per pixel from a two-byte-per-pixel frame and
     * fed the host twenty-four bits per pixel where DPI_COLOR_CODING says
     * sixteen.  A host receiving half again as much data as it is configured
     * for overruns its payload fifo, which is the DPI_PLD_WR_ERR this phase
     * reported from its first run.
     */
    v = brg_rd(P4_DSI_BRG_PIXEL_TYPE);
    v &= ~(P4_DSI_BRG_RAW_TYPE_MASK | P4_DSI_BRG_DPI_TYPE_MASK
           | P4_DSI_BRG_DATA_IN_TYPE);
#if P4_PANEL_BPP == 24
    v |= P4_DSI_BRG_RAW_RGB888;
#else
    v |= P4_DSI_BRG_RAW_RGB565;
#endif
    brg_wr(P4_DSI_BRG_PIXEL_TYPE, v);

    /* How much data one frame is, in sixty-four-bit words, and a reload of
       the internal counter from it */
    /*
     * A scale test, not a configuration.
     *
     * The panel shows the framebuffer twice down its height while the DMA has
     * been measured reading it once per panel frame - 218 MB/s against the
     * 210 the timing calls for, where reading it twice would be 420.  So the
     * bridge reads one frame and emits two.  If this counter is what makes it
     * restart, halving it gives four copies and doubling it gives one; if the
     * count of copies does not follow it, the restart is somewhere else.
     */
#ifdef P4_BRG_RAW_DIV
    v = ((P4_FB_WORDS64 / P4_BRG_RAW_DIV) & P4_DSI_BRG_RAW_NUM_MASK)
        | P4_DSI_BRG_RAW_NUM_SET;
#else
    v = (P4_FB_WORDS64 & P4_DSI_BRG_RAW_NUM_MASK) | P4_DSI_BRG_RAW_NUM_SET;
#endif
    brg_wr(P4_DSI_BRG_RAW_NUM_CFG, v);

#ifdef P4_B5_BLK_RAW_FRAME
    /* Revision-one resets this nominally multi-block-only count to a complete
       720x1280 RGB565 frame.  Test whether it still clips a one-block transfer
       by explicitly giving it this panel's complete 800x1280 frame. */
    brg_wr(P4_DSI_BRG_BLK_RAW_NUM,
           (P4_FB_WORDS64 & P4_DSI_BRG_BLK_RAW_MASK)
           | P4_DSI_BRG_BLK_RAW_SET);
#endif

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

    /* This is B5's first bridge enable and commit.  All DMA-facing fields are
       now complete, matching esp_lcd_new_panel_dpi(); feed remains off until
       the channel and host video path are running. */
    brg_wr(P4_DSI_BRG_EN, P4_DSI_BRG_DSI_EN);
    brg_wr(P4_DSI_BRG_DPI_CFG_UPD, P4_DSI_BRG_CFG_UPDATE);

#ifdef P4_B5_PRESTAGE_FEED
    /*
     * Stage DPI_EN without committing it.  The ordinary path has to perform
     * a bridge read, a bridge write and then the update after host video is
     * already live.  If the host loses the first DPI synchronization edge in
     * that interval, preparing the shadow value here lets the later start be
     * one update write.  A revision-one bridge that does not shadow DPI_EN
     * will expose that immediately as an early underrun; the bounded B5
     * oracle treats that as a rejection, not as a usable mode.
     */
    brg_wr(P4_DSI_BRG_DPI_MISC_CFG,
           brg_rd(P4_DSI_BRG_DPI_MISC_CFG) | P4_DSI_BRG_DPI_EN);
#endif
}

/*
 * Let the bridge pull pixels.  Separate from the configuration above because
 * the DMA has to be armed first: a feed enabled against a channel that is not
 * running is an underrun by construction.
 */
void krnP4ScanoutFeedOn(void)
{
#ifndef P4_B5_PRESTAGE_FEED
    brg_wr(P4_DSI_BRG_DPI_MISC_CFG,
           brg_rd(P4_DSI_BRG_DPI_MISC_CFG) | P4_DSI_BRG_DPI_EN);
#endif
    brg_wr(P4_DSI_BRG_DPI_CFG_UPD, P4_DSI_BRG_CFG_UPDATE);
#ifdef P4_B5_REF_BRG_IRQ
    /* esp_lcd_panel_dpi.c performs this immediately after the feed commit.
       It should affect only interrupt delivery, not the bridge/host
       handshake, but it is the last measured static bridge-register
       difference from the working Vellum state.  The peripheral route is not
       enabled in AROS; RAW status remains our polling oracle. */
    brg_wr(P4_DSI_BRG_INT_ENA, brg_rd(P4_DSI_BRG_INT_ENA) | 1UL);
#endif
}

/*
 * The DMA channel and its one-frame item.
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

    p4_w32(P4_DMAC_CFG, P4_DMAC_CFG_EN | P4_DMAC_INT_EN);
    p4_w32(P4_DMAC_CHEN, P4_DMAC_CH1_EN_WE);

    /* ESP-IDF routes a PSRAM source through GDMA master 1 (memory) and the
       bridge destination through master 0 (MIPI DSI).  Leaving SMS at its
       reset value silently puts both sides on master 0: bytes still move, but
       not over the topology used and validated by the reference driver. */
    ctl_lo = P4_DMAC_SMS
           | (P4_DMAC_WIDTH_64 << P4_DMAC_SRC_WIDTH_SHIFT)
           | (P4_DMAC_WIDTH_64 << P4_DMAC_DST_WIDTH_SHIFT)
           | (P4_DMAC_MSIZE_512 << P4_DMAC_SRC_MSIZE_SHIFT)
           | (P4_DMAC_MSIZE_256 << P4_DMAC_DST_MSIZE_SHIFT)
           | P4_DMAC_DINC_FIXED;          /* source increments, sink does not */

    ctl_hi = P4_DMAC_ARLEN_EN
           | (P4_DMAC_AXI_BURST_LEN << P4_DMAC_ARLEN_SHIFT)
           | P4_DMAC_AWLEN_EN
           | (P4_DMAC_AXI_BURST_LEN << P4_DMAC_AWLEN_SHIFT);
#ifndef P4_B5_DMA_RELOAD
    ctl_hi |= P4_DMAC_LLI_LAST | P4_DMAC_LLI_VALID;
    scanout_lli_ctl_hi = ctl_hi;

    /* Match dw_gdma_new_link_list(): discard any cached view once, then touch
       the item only through the non-cacheable alias.  The LLP itself keeps the
       normal address because that is the address the DMA master consumes. */
    krnP4CacheSyncData(scanout_lli, sizeof(scanout_lli));
    lli_wr(P4_DMAC_LLI_SAR_LO, P4_FB_BASE);
    lli_wr(P4_DMAC_LLI_SAR_HI, 0);
    lli_wr(P4_DMAC_LLI_DAR_LO, P4_DSI_BRG_MEM_BASE);
    lli_wr(P4_DMAC_LLI_DAR_HI, 0);
    lli_wr(P4_DMAC_LLI_BLOCK_TS, P4_FB_WORDS64 - 1);
    /* Even a last item with a null next pointer carries the link-list master
       selector.  IDF selects master 1 because the descriptor lives in L2MEM. */
    lli_wr(P4_DMAC_LLI_LLP_LO, P4_DMAC_LLP_LMS_MEMORY);
    lli_wr(P4_DMAC_LLI_LLP_HI, 0);
    lli_wr(P4_DMAC_LLI_CTL_LO, ctl_lo);
    lli_wr(P4_DMAC_LLI_CTL_HI, ctl_hi);

    /*
     * The frame itself still needs the cache written back: the DMA reads it
     * over AXI and does not see the CPU's caches, so a frame in dirty lines is
     * a frame of whatever memory held before.  The descriptor instead remains
     * reachable through the non-cacheable internal-SRAM alias above.
     */
    krnP4CacheWriteback();

    /* Both sides take their next block from the one-item list. */
    ch_wr(P4_DMAC_CH_CFG0,
          (P4_DMAC_MULTBLK_LIST << P4_DMAC_SRC_MULTBLK_SHIFT)
        | (P4_DMAC_MULTBLK_LIST << P4_DMAC_DST_MULTBLK_SHIFT));
#else
    /* Diagnostic only: keep the producer continuous across a framebuffer
       boundary.  This exact register-reload form previously proved capable
       of sustained transport but lost the bridge/panel frame phase, so it may
       localize the present host stop and must not be accepted as geometry. */
    ch_wr(P4_DMAC_CH_SAR, P4_FB_BASE);
    ch_wr(P4_DMAC_CH_SAR + 4, 0);
    ch_wr(P4_DMAC_CH_DAR, P4_DSI_BRG_MEM_BASE);
    ch_wr(P4_DMAC_CH_DAR + 4, 0);
    ch_wr(P4_DMAC_CH_BLOCK_TS, P4_FB_WORDS64 - 1);
    ch_wr(P4_DMAC_CH_CTL0, ctl_lo);
    ch_wr(P4_DMAC_CH_CTL1, ctl_hi);
    krnP4CacheWriteback();
    ch_wr(P4_DMAC_CH_CFG0,
          (P4_DMAC_MULTBLK_RELOAD << P4_DMAC_SRC_MULTBLK_SHIFT)
        | (P4_DMAC_MULTBLK_RELOAD << P4_DMAC_DST_MULTBLK_SHIFT));
#endif

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

#ifndef P4_B5_DMA_RELOAD
    ch_wr(P4_DMAC_CH_LLP,
          (unsigned long)scanout_lli | P4_DMAC_LLP_LMS_MEMORY);
    ch_wr(P4_DMAC_CH_LLP + 4, 0);
#else
    ch_wr(P4_DMAC_CH_LLP, 0);
    ch_wr(P4_DMAC_CH_LLP + 4, 0);
#endif

    scanout_dma_frames = 0;
    scanout_dma_faults = 0;
    ch_wr(P4_DMAC_CH_INTCLEAR0, 0xFFFFFFFFUL);
#ifndef P4_B5_DMA_RELOAD
    ch_wr(P4_DMAC_CH_INTSTATUS_ENABLE0, 0xFFFFFFFFUL);
    ch_wr(P4_DMAC_CH_INTSIGNAL_ENABLE0,
          P4_DMAC_IS_DMA_DONE | P4_DMAC_IS_LLI_INVALID);
    p4_w32(P4_INTMTX_MAP(P4_SOURCE_DW_GDMA), P4_DSI_DMA_LINE);
    krnCLICEnable(P4_DSI_DMA_LINE, 0);
#else
    ch_wr(P4_DMAC_CH_INTSTATUS_ENABLE0, 0);
    ch_wr(P4_DMAC_CH_INTSIGNAL_ENABLE0, 0);
#endif

    /* And run */
    p4_w32(P4_DMAC_CHEN, P4_DMAC_CH1_EN | P4_DMAC_CH1_EN_WE);
}

/* Espressif's mipi_dsi_dma_trans_done_cb(), reduced to the one fixed frame and
 * one fixed item this port owns.  Clear the level source before re-arming, and
 * do not restart a transfer that ended with a real DMA fault. */
void krnP4ScanoutDmaInterrupt(void)
{
    unsigned long status = ch_rd(P4_DMAC_CH_INTSTATUS0);
    unsigned long faults = status
        & ~(P4_DMAC_IS_BLOCK_DONE | P4_DMAC_IS_DMA_DONE
            | P4_DMAC_IS_SRC_TRANSCOMP | P4_DMAC_IS_DST_TRANSCOMP
            | P4_DMAC_IS_DISABLED);

    ch_wr(P4_DMAC_CH_INTCLEAR0, status);
    scanout_dma_faults |= faults;

    if ((status & P4_DMAC_IS_DMA_DONE) && !faults)
    {
        scanout_dma_frames++;
        lli_wr(P4_DMAC_LLI_CTL_HI, scanout_lli_ctl_hi);
        ch_wr(P4_DMAC_CH_LLP,
              (unsigned long)scanout_lli | P4_DMAC_LLP_LMS_MEMORY);
        ch_wr(P4_DMAC_CH_LLP + 4, 0);
        p4_w32(P4_DMAC_CHEN, P4_DMAC_CH1_EN | P4_DMAC_CH1_EN_WE);
    }
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

    /*
     * The bridge's own pixel clock, read back rather than assumed.
     *
     * The bridge generates the DPI timing the host synchronises to, and needs
     * this clock to do it.  A gate left closed presents as a host that takes
     * pixels and never begins a frame, which no other register distinguishes.
     */
    out->dpi_clk    = p4_r32(P4_CLKRST_PERI_CLK_CTRL03);
}

void krnP4ScanoutState(struct P4ScanoutState *out)
{
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
    out->ch_int0      = ch_rd(P4_DMAC_CH_INTSTATUS0);
    out->ch_int1      = ch_rd(P4_DMAC_CH_INTSTATUS1);
    out->brg_depth    = brg_rd(P4_DSI_BRG_FIFO_STATUS)
                        & P4_DSI_BRG_BUF_DEPTH_MASK;
    out->dma_frames   = scanout_dma_frames;
    out->dma_faults   = scanout_dma_faults;
}

/*
 * The bridge registers this port configures nothing in, read back once.
 *
 * Not a diagnostic for its own sake.  The pixel format sat in exactly this
 * category - never written, printed as a curiosity, and wrong - so the rest of
 * the block is worth one look before anything else is guessed at.
 */
void krnP4ScanoutBridgeRest(struct P4BridgeRest *out)
{
    out->credit_ctl   = brg_rd(P4_DSI_BRG_CREDIT_CTL);
    out->block_intvl  = brg_rd(P4_DSI_BRG_BLOCK_INTVL);
    out->req_intvl    = brg_rd(P4_DSI_BRG_REQ_INTVL);
    out->lcd_ctl      = brg_rd(P4_DSI_BRG_DPI_LCD_CTL);
    out->rsv_dpi_data = brg_rd(P4_DSI_BRG_RSV_DPI_DATA);
    out->int_ena      = brg_rd(P4_DSI_BRG_INT_ENA);
    out->blk_raw_num  = brg_rd(P4_DSI_BRG_BLK_RAW_NUM);
    out->host_ctrl    = brg_rd(P4_DSI_BRG_HOST_CTRL);
    out->mem_clk_ctrl = brg_rd(P4_DSI_BRG_MEM_CLK_CTRL);
    out->dma_req_cfg  = brg_rd(P4_DSI_BRG_DMA_REQ_CFG);
}

/*
 * One sample of the bridge's occupancy and its raw interrupt.
 *
 * Cheap enough to call in a tight loop, which is the point: a fifo that fills
 * and drains is a bridge doing its job, a fifo pinned at one value is a bridge
 * that has stopped, and a fifo at zero with no underrun raised is a bridge
 * that never started a frame.  One reading cannot separate those.
 */
void krnP4ScanoutSample(unsigned long *depth, unsigned long *int_raw)
{
    if (depth)
        *depth = brg_rd(P4_DSI_BRG_FIFO_STATUS) & P4_DSI_BRG_BUF_DEPTH_MASK;
    if (int_raw)
        *int_raw = brg_rd(P4_DSI_BRG_INT_RAW);
}

/*
 * A frame that reports on its own transmission.
 *
 * A flat colour proves only that something arrived.  It cannot distinguish a
 * frame that is intact from one that is short by a few lines, offset by half a
 * line, or has had lines replaced by the bridge's underflow substitute - all
 * of which are live possibilities here, and all of which look like "banding"
 * from across a desk.
 *
 * So: eight horizontal bars in known colours, a two-pixel white border, and a
 * one-pixel white tick every 100 pixels along the top.  The bars say which
 * lines arrived and in what order; the border says whether the geometry and
 * the stride are right, because a stride error turns a rectangle into a
 * diagonal; the ticks give a horizontal scale to read an offset against.  The
 * bar colours are the three primaries and their pairs, so the channel order
 * can be read off directly rather than inferred.
 */
void krnP4ScanoutTestCard(void)
{
    static const unsigned long bars[8] =
    {
        0xFF0000,   /* as written to memory: byte 0 is the low one */
        0x00FF00,
        0x0000FF,
        0xFFFF00,
        0x00FFFF,
        0xFF00FF,
        0xFFFFFF,
        0x404040,
    };
    volatile unsigned char *fb = (volatile unsigned char *)P4_FB_BASE;
    unsigned long y, x;

    /*
     * The bars occupy the top half only, and the bottom half is left black.
     * Distinct halves prove that the native-height frame arrives once and in
     * the expected order; the earlier apparent repetition was observed while
     * RGB565 byte order made the diagnostic patterns misleading.
     */
    for (y = 0; y < P4_TX_V_RES; y++)
    {
        unsigned long c = (y < P4_PANEL_V_RES / 2)
                          ? bars[(y * 16 / P4_PANEL_V_RES) & 7]
                          : 0x000000;
        volatile unsigned char *row = fb + y * P4_PANEL_H_RES
                                          * P4_FB_BYTES_PER_PIXEL;

        for (x = 0; x < P4_PANEL_H_RES; x++)
        {
            unsigned long v = c;

            /* two-pixel border all the way round */
            if (y < 2 || y >= P4_PANEL_V_RES - 2
                || x < 2 || x >= P4_PANEL_H_RES - 2)
                v = 0xFFFFFF;
            /* a tick every hundred pixels, eight lines tall, under the border */
            else if (y < 10 && (x % 100) == 0)
                v = 0xFFFFFF;

            px(row + x * P4_FB_BYTES_PER_PIXEL, v);
        }
    }
}

static void scanout_rect(unsigned long x, unsigned long y,
                         unsigned long w, unsigned long h,
                         unsigned long rgb)
{
    volatile unsigned char *fb = (volatile unsigned char *)P4_FB_BASE;
    unsigned long yy, xx;

    for (yy = y; yy < y + h; yy++)
    {
        volatile unsigned char *row = fb + yy * P4_PANEL_H_RES
                                          * P4_FB_BYTES_PER_PIXEL;

        for (xx = x; xx < x + w; xx++)
            px(row + xx * P4_FB_BYTES_PER_PIXEL, rgb);
    }
}

/*
 * Coverage whose dimensions are hostile to accidental alignment.
 *
 * Twenty-five by twenty pixel checker cells tile the 800x1280 framebuffer
 * exactly.  A 25-pixel RGB565 run is 50 bytes, so the pattern still crosses
 * 64-byte cache-line boundaries without leaving a partial edge cell that can
 * masquerade as a coherency defect.
 */
static void scanout_checker(void)
{
    volatile unsigned char *fb = (volatile unsigned char *)P4_FB_BASE;
    unsigned long y, x;

    for (y = 0; y < P4_TX_V_RES; y++)
    {
        volatile unsigned char *row = fb + y * P4_PANEL_H_RES
                                          * P4_FB_BYTES_PER_PIXEL;

        for (x = 0; x < P4_PANEL_H_RES; x++)
            px(row + x * P4_FB_BYTES_PER_PIXEL,
               (((x / 25) ^ (y / 20)) & 1) ? 0xFFFFFFUL : 0x000000UL);
    }
}

/* Four asymmetric corner blocks, isolated from the line test so an observer
 * cannot confuse line intersections with the intended physical corners. */
static void scanout_corner_blocks(void)
{
    krnP4ScanoutFill(0x000000UL);
    scanout_rect(0, 0, 37, 53, 0xFF0000UL);
    scanout_rect(P4_PANEL_H_RES - 61, 0, 61, 43, 0x00FF00UL);
    scanout_rect(0, P4_TX_V_RES - 67, 47, 67, 0x0000FFUL);
    scanout_rect(P4_PANEL_H_RES - 73, P4_TX_V_RES - 31,
                 73, 31, 0xFFFF00UL);
}

static void scanout_one_pixel_lines(void)
{
    volatile unsigned char *fb = (volatile unsigned char *)P4_FB_BASE;
    unsigned long y, x;

    krnP4ScanoutFill(0x000000UL);

    for (y = 0; y < P4_TX_V_RES; y++)
    {
        volatile unsigned char *row = fb + y * P4_PANEL_H_RES
                                          * P4_FB_BYTES_PER_PIXEL;

        px(row + 173 * P4_FB_BYTES_PER_PIXEL, 0xFFFFFFUL);
        px(row + 599 * P4_FB_BYTES_PER_PIXEL, 0xFF00FFUL);
    }
    for (x = 0; x < P4_PANEL_H_RES; x++)
    {
        px(fb + (311 * P4_PANEL_H_RES + x) * P4_FB_BYTES_PER_PIXEL,
           0xFFFFFFUL);
        px(fb + (997 * P4_PANEL_H_RES + x) * P4_FB_BYTES_PER_PIXEL,
           0x00FFFFUL);
    }
}

static void scanout_corners_and_lines(void)
{
    volatile unsigned char *fb = (volatile unsigned char *)P4_FB_BASE;
    unsigned long y, x;

    scanout_corner_blocks();
    for (y = 0; y < P4_TX_V_RES; y++)
    {
        volatile unsigned char *row = fb + y * P4_PANEL_H_RES
                                          * P4_FB_BYTES_PER_PIXEL;

        px(row + 173 * P4_FB_BYTES_PER_PIXEL, 0xFFFFFFUL);
        px(row + 599 * P4_FB_BYTES_PER_PIXEL, 0xFF00FFUL);
    }
    for (x = 0; x < P4_PANEL_H_RES; x++)
    {
        px(fb + (311 * P4_PANEL_H_RES + x) * P4_FB_BYTES_PER_PIXEL,
           0xFFFFFFUL);
        px(fb + (997 * P4_PANEL_H_RES + x) * P4_FB_BYTES_PER_PIXEL,
           0x00FFFFUL);
    }
}

/* Large asymmetric quadrants expose coordinate folding or an unexpected
 * origin that a periodic checker cannot.  A four-pixel black cross keeps the
 * quadrant boundaries unambiguous without relying on the panel bezel. */
void krnP4ScanoutCoordinatePattern(void)
{
    volatile unsigned char *fb = (volatile unsigned char *)P4_FB_BASE;
    unsigned long y, x;

    for (y = 0; y < P4_TX_V_RES; y++)
    {
        volatile unsigned char *row = fb + y * P4_PANEL_H_RES
                                          * P4_FB_BYTES_PER_PIXEL;

        for (x = 0; x < P4_PANEL_H_RES; x++)
        {
            unsigned long rgb;

            if (x >= P4_PANEL_H_RES / 2 - 2
                && x < P4_PANEL_H_RES / 2 + 2)
                rgb = 0x000000UL;
            else if (y >= P4_TX_V_RES / 2 - 2
                     && y < P4_TX_V_RES / 2 + 2)
                rgb = 0x000000UL;
            else if (y < P4_TX_V_RES / 2)
                rgb = x < P4_PANEL_H_RES / 2 ? 0xFF0000UL : 0x00FF00UL;
            else
                rgb = x < P4_PANEL_H_RES / 2 ? 0x0000FFUL : 0xFFFF00UL;
            px(row + x * P4_FB_BYTES_PER_PIXEL, rgb);
        }
    }
}

/*
 * One deterministic second of the B5 coherency instrument.
 *
 * Returns a phase number only when the framebuffer changed.  The caller logs
 * it alongside the DMA/bridge/host sample, so the UART proves that each image
 * was written back rather than merely compiled in.  The dirty rectangles are
 * 127 by 73 pixels at odd, changing offsets; neither their ends nor their row
 * strides share a cache-line boundary.  Each step erases the old rectangle
 * and writes a differently coloured new one before a complete cache
 * writeback.  A later range operation may replace that conservative flush,
 * but it cannot weaken this gate silently.
 */
unsigned long krnP4ScanoutCoherencyStep(unsigned long second)
{
    unsigned long phase = 0;

#ifdef P4_B5_STATIC_PRELOAD
    /* Keep every framebuffer byte unchanged after DMA starts. */
    (void)second;
    return 0;
#else
#ifdef P4_B5_VISUAL_GATE
    switch (second)
    {
        case 0:  krnP4ScanoutFill(0xFF0000UL); phase = 1; break;
        case 3:  krnP4ScanoutFill(0x00FF00UL); phase = 2; break;
        case 6:  krnP4ScanoutFill(0x0000FFUL); phase = 3; break;
        case 9:  krnP4ScanoutFill(0xFFFFFFUL); phase = 4; break;
        case 12: krnP4ScanoutFill(0x000000UL); phase = 5; break;
        case 15: scanout_checker(); phase = 6; break;
        case 20: krnP4ScanoutCoordinatePattern(); phase = 7; break;
        case 30: scanout_corner_blocks(); phase = 8; break;
        case 40: scanout_one_pixel_lines(); phase = 9; break;
        case 50: scanout_corners_and_lines(); phase = 10; break;
        default: break;
    }
#else
    switch (second)
    {
        case 0:  krnP4ScanoutFill(0xFF0000UL); phase = 1; break;
        case 2:  krnP4ScanoutFill(0x00FF00UL); phase = 2; break;
        case 4:  krnP4ScanoutFill(0x0000FFUL); phase = 3; break;
        case 6:  krnP4ScanoutFill(0xFFFFFFUL); phase = 4; break;
        case 8:  krnP4ScanoutFill(0x000000UL); phase = 5; break;
        case 10: scanout_checker(); phase = 6; break;
        case 14: scanout_corners_and_lines(); phase = 7; break;
        case 18: krnP4ScanoutFill(0x000000UL); phase = 8; break;
#ifndef P4_B5_CONCURRENT_STRESS
        case 42: scanout_corners_and_lines(); phase = 9; break;
#endif
        default:
            if (second >= 19
#ifndef P4_B5_CONCURRENT_STRESS
                && second < 42
#endif
               )
            {
                unsigned long n = second - 19;
                unsigned long x = (n * 97 + 3) % (P4_PANEL_H_RES - 127 + 1);
                unsigned long y = (n * 173 + 5) % (P4_TX_V_RES - 73 + 1);
                static const unsigned long colours[6] =
                {
                    0xFF0000UL, 0x00FF00UL, 0x0000FFUL,
                    0xFFFF00UL, 0x00FFFFUL, 0xFF00FFUL,
                };

                if (n)
                {
                    unsigned long old = n - 1;
                    unsigned long ox = (old * 97 + 3)
                                     % (P4_PANEL_H_RES - 127 + 1);
                    unsigned long oy = (old * 173 + 5)
                                     % (P4_TX_V_RES - 73 + 1);

                    scanout_rect(ox, oy, 127, 73, 0x000000UL);
                }
                scanout_rect(x, y, 127, 73, colours[n % 6]);
                phase = 8;
            }
            break;
    }
#endif

    if (phase)
        krnP4CacheWriteback();
    return phase;
#endif
}

/*
 * A cross, which answers one question and nothing else.
 *
 * Two facts have to be reconciled: the panel shows the framebuffer twice down
 * its height, and the DMA reads it exactly once per panel frame.  Together
 * those force each transmitted line to carry half the pixels it should, which
 * would shear the image - every line displaced from the one above by half a
 * width - and shear is what a single vertical line makes visible and a field
 * of colour cannot.
 *
 * So: one vertical line at the middle column, one horizontal line at the
 * middle row, black everywhere else.  A vertical line that arrives vertical
 * rules shear out and leaves genuine line doubling; one that arrives as a
 * diagonal confirms it and gives the displacement from its slope.  The
 * horizontal line counts the copies at the same time.
 */
void krnP4ScanoutCross(void)
{
    volatile unsigned char *fb = (volatile unsigned char *)P4_FB_BASE;
    unsigned long y, x;

    for (y = 0; y < P4_TX_V_RES; y++)
    {
        volatile unsigned char *row = fb + y * P4_PANEL_H_RES
                                          * P4_FB_BYTES_PER_PIXEL;

        for (x = 0; x < P4_PANEL_H_RES; x++)
        {
            unsigned char v = 0;

            if (x >= P4_PANEL_H_RES / 2 && x < P4_PANEL_H_RES / 2 + 4)
                v = 0xFF;                       /* the vertical line */
            else if (y >= P4_PANEL_V_RES / 2 && y < P4_PANEL_V_RES / 2 + 4)
                v = 0xFF;                       /* the horizontal one */

            px(row + x * P4_FB_BYTES_PER_PIXEL,
               v ? 0xFFFFFFUL : 0UL);
        }
    }
}

/*
 * A grid at a known pitch, so the panel can be counted rather than described.
 *
 * Every earlier pattern has been read qualitatively - "twice", "banded",
 * "offset" - and every reading of it has been an interpretation.  A grid is
 * arithmetic: 800 columns at a pitch of 100 is eight vertical lines and 1280
 * rows is thirteen horizontal ones, and any other count is a ratio that says
 * directly what is happening.  Sixteen verticals means each transmitted line
 * carries half the pixels the panel expects; twenty-six horizontals means
 * twice as many lines arrive as there should be.
 *
 * Every fifth line is drawn double width so a long count can be checked
 * against a short one.
 */
void krnP4ScanoutGrid(void)
{
    volatile unsigned char *fb = (volatile unsigned char *)P4_FB_BASE;
    unsigned long y, x;

    for (y = 0; y < P4_TX_V_RES; y++)
    {
        volatile unsigned char *row = fb + y * P4_PANEL_H_RES
                                          * P4_FB_BYTES_PER_PIXEL;
        /* the row pitch is in panel rows, so it scales with the multiplier;
           the column pitch does not, because columns are not doubled */
        unsigned long ry = y % (100 * P4_PANEL_VMUL);
        int y_line = (ry < 2 * P4_PANEL_VMUL)
                  || (((y / (100 * P4_PANEL_VMUL)) % 5) == 0
                      && ry < 4 * P4_PANEL_VMUL);

        for (x = 0; x < P4_PANEL_H_RES; x++)
        {
            unsigned long rx = x % 100;
            int x_line = (rx < 2) || (((x / 100) % 5) == 0 && rx < 4);
            unsigned char r = 0, g = 0, b = 0;

            if (y_line)
                g = 0xFF;               /* rows in one channel */
            if (x_line)
                r = 0xFF;               /* columns in another */

            px(row + x * P4_FB_BYTES_PER_PIXEL,
               ((unsigned long)r << 16) | ((unsigned long)g << 8) | b);
        }
    }
}

/*
 * Three single lines, and nothing else.
 *
 * Every grid line arrives with a second, fainter copy 34 panel lines away, and
 * it is on the panel rather than in the camera exposure.  A grid cannot say
 * whether that is one echo or a train of them, because at a pitch of 100 the
 * third echo of one line lands where the next line's second echo does.
 *
 * So: one line at row 100, one at 500, one at 900, four rows tall, white.
 * Three lines on the panel means one echo each; more than three means a train,
 * and the count gives its decay.  Even spacing between the echoes says the
 * offset is fixed; growing spacing says it accumulates.  All three at the same
 * offset says it is a property of the transfer, not of position in the frame.
 */
void krnP4ScanoutThreeLines(void)
{
    volatile unsigned char *fb = (volatile unsigned char *)P4_FB_BASE;
    unsigned long y, x;

    for (y = 0; y < P4_TX_V_RES; y++)
    {
        volatile unsigned char *row = fb + y * P4_PANEL_H_RES
                                          * P4_FB_BYTES_PER_PIXEL;
        int on = (y >= 100 * P4_PANEL_VMUL && y < 100 * P4_PANEL_VMUL + 4 * P4_PANEL_VMUL)
              || (y >= 500 * P4_PANEL_VMUL && y < 500 * P4_PANEL_VMUL + 4 * P4_PANEL_VMUL)
              || (y >= 900 * P4_PANEL_VMUL && y < 900 * P4_PANEL_VMUL + 4 * P4_PANEL_VMUL);

        for (x = 0; x < P4_PANEL_H_RES; x++)
            px(row + x * P4_FB_BYTES_PER_PIXEL, on ? 0xFFFFFFUL : 0UL);
    }
}

/*
 * A pattern that tests one hypothesis and nothing else.
 *
 * Measured: framebuffer row y arrives on panel row y/2, and the whole frame
 * arrives a second time 640 panel rows lower.  Two sent rows therefore make one
 * panel row, which is what a panel expecting 1600 pixels per line and being
 * given 800 would do - the first sent row fills the left half, the next fills
 * the right.  Full-width white rows cannot distinguish that from any other
 * halving, because both halves look the same.
 *
 * So: even framebuffer rows get a bar in their left half only, odd rows get one
 * in their right half only, at three widely separated places.  If two sent rows
 * are being joined, each pair reassembles into one continuous panel row and the
 * panel shows three unbroken lines.  If they are not, the halves stay separate
 * and the panel shows short bars alternating left and right.
 *
 * The three groups are at different rows so a position-dependent effect can be
 * told from a uniform one, and the left and right bars differ in length so the
 * two halves cannot be confused with each other.
 */
void krnP4ScanoutHalves(void)
{
    volatile unsigned char *fb = (volatile unsigned char *)P4_FB_BASE;
    unsigned long y, x;

    for (y = 0; y < P4_TX_V_RES; y++)
    {
        volatile unsigned char *row = fb + y * P4_PANEL_H_RES
                                          * P4_FB_BYTES_PER_PIXEL;
        int band = (y >= 100 && y < 108)
                || (y >= 500 && y < 508)
                || (y >= 900 && y < 908);
        int right = (y & 1);

        for (x = 0; x < P4_PANEL_H_RES; x++)
        {
            unsigned long v = 0;

            if (band)
            {
                /* left half gets x 0..299, right half x 500..799: a gap in the
                   middle of each so a join is visible as a gap, not a guess */
                if (!right && x < 300)
                    v = 0xFFFFFFUL;
                else if (right && x >= 500)
                    v = 0xFFFFFFUL;
            }
            px(row + x * P4_FB_BYTES_PER_PIXEL, v);
        }
    }
}

/*
 * Stop anything the previous boot left reading memory.
 *
 * A CPU reset does not reset the GDMA, exactly as it does not reset the PMU or
 * the clock dividers - which this port already knew and had recorded about
 * those two.  B5 leaves the scanout running deliberately, so after a reset the
 * channel is still walking the framebuffer over AXI while the PSRAM bring-up
 * reconfigures the MSPI controller underneath it.  That hangs, reproducibly:
 * three resets in a row stopped at the same point, immediately after the clock
 * report and before any PSRAM output.
 *
 * Before the bounded MSPI and ordered DSI recovery existed, restoring that
 * state required the vendor firmware and sometimes a power cycle.  This path
 * now has to recover both consumers of the old framebuffer, not merely keep a
 * normal shutdown tidy.
 *
 * Called before the PSRAM is touched, and written to be safe when nothing was
 * running: the module clocks are enabled first so the registers answer at all,
 * and every write is idempotent.
 */
void krnP4ScanoutQuiesce(void)
{
    /*
     * DMA first: it is the part still driving AXI reads against PSRAM.  The
     * GDMA's reset is released before its registers are written, because after
     * a cold boot the block may be held in reset and this function has to be
     * safe on the first boot as well as on a reset with scanout still running.
     * The bridge is stopped separately below, after the memory reader can no
     * longer race its teardown.
     */
    p4_w32(P4_CLKRST_SOC_CLK_CTRL0,
           p4_r32(P4_CLKRST_SOC_CLK_CTRL0) | P4_GDMA_CPU_CLK_EN);
    p4_w32(P4_CLKRST_SOC_CLK_CTRL1,
           p4_r32(P4_CLKRST_SOC_CLK_CTRL1) | P4_GDMA_SYS_CLK_EN);

    /* Out of reset first, so the writes below reach a block that answers */
    p4_w32(P4_CLKRST_HP_RST_EN0,
           p4_r32(P4_CLKRST_HP_RST_EN0) & ~P4_RST_EN_GDMA);

    /* Stop completion delivery before stopping the channel, otherwise a frame
       ending in this window can re-arm the reader we are trying to quiesce. */
    krnCLICDisable(P4_DSI_DMA_LINE);
    ch_wr(P4_DMAC_CH_INTSIGNAL_ENABLE0, 0);
    p4_w32(P4_DMAC_CFG, p4_r32(P4_DMAC_CFG) & ~P4_DMAC_INT_EN);

    /* Channel off, with the write-enable the register demands */
    p4_w32(P4_DMAC_CHEN, P4_DMAC_CH1_EN_WE);

    /* Then the whole controller through its reset, which is the only way to be
       sure of a channel that was in the middle of a burst */
    p4_w32(P4_CLKRST_HP_RST_EN0,
           p4_r32(P4_CLKRST_HP_RST_EN0) | P4_RST_EN_GDMA);
    p4_w32(P4_CLKRST_HP_RST_EN0,
           p4_r32(P4_CLKRST_HP_RST_EN0) & ~P4_RST_EN_GDMA);

    /*
     * Stop the producer on the other side of that channel as well.
     *
     * On revision one the bridge has no usable internal soft reset; the local
     * ESP-IDF says so explicitly.  Its panel teardown instead disables the
     * DPI clock and bridge before the chip-level DSI reset is pulsed by the
     * next bus initialisation.  A reset caught in active scanout used to skip
     * that teardown here: the DMA was reset while the bridge and host remained
     * in video mode, and the next boot could fill the bridge FIFO without the
     * host ever consuming it.
     *
     * The two conditions that made an earlier bridge access hang are made
     * explicit first: enable the DSI system clock and release the block reset.
     * Force the bridge register clock on, stop its pixel clock, disable the
     * bridge, and only then reset the complete host/bridge block.  Release the
     * reset again because this function is also used by the bounded B5
     * shutdown, whose next operation still accesses the host registers.
     */
    p4_w32(P4_CLKRST_SOC_CLK_CTRL1,
           p4_r32(P4_CLKRST_SOC_CLK_CTRL1) | P4_DSI_SYS_CLK_EN);
    p4_w32(P4_CLKRST_HP_RST_EN0,
           p4_r32(P4_CLKRST_HP_RST_EN0) & ~P4_RST_EN_DSI_BRG);

    brg_wr(P4_DSI_BRG_CLK_EN, P4_DSI_BRG_CLK_EN_BIT);
    p4_w32(P4_CLKRST_PERI_CLK_CTRL03,
           p4_r32(P4_CLKRST_PERI_CLK_CTRL03) & ~P4_DSI_DPICLK_EN);
    brg_wr(P4_DSI_BRG_EN,
           brg_rd(P4_DSI_BRG_EN) & ~P4_DSI_BRG_DSI_EN);

    p4_w32(P4_CLKRST_HP_RST_EN0,
           p4_r32(P4_CLKRST_HP_RST_EN0) | P4_RST_EN_DSI_BRG);
    (void)p4_r32(P4_CLKRST_HP_RST_EN0);
    p4_w32(P4_CLKRST_HP_RST_EN0,
           p4_r32(P4_CLKRST_HP_RST_EN0) & ~P4_RST_EN_DSI_BRG);
}

/*
 * Three bands, each with 0xFF in a different byte of the pixel.
 *
 * A flat field written with 0xFF in byte 0 came back yellow, and yellow needs
 * two channels at full - so the panel is not reading the bytes the way this
 * code lays them down.  Which byte reaches which channel cannot be inferred
 * from one colour; it can be read off three.
 *
 * Top third: byte 0 only.  Middle: byte 1 only.  Bottom: byte 2 only.  The
 * three colours that appear, in order, are the channel order in memory.  If a
 * band shows two channels rather than one, the pixel stride itself is wrong and
 * the bands will also be striped, which distinguishes a channel-order question
 * from a stride question in the same look.
 *
 * Written byte-wise on purpose rather than through px(), because px() is the
 * thing under test.
 */
void krnP4ScanoutBands(void)
{
    volatile unsigned char *fb = (volatile unsigned char *)P4_FB_BASE;
    unsigned long y, x;

    for (y = 0; y < P4_TX_V_RES; y++)
    {
        volatile unsigned char *row = fb + y * P4_PANEL_H_RES
                                          * P4_FB_BYTES_PER_PIXEL;
        unsigned int which = (unsigned int)(y * 3 / P4_TX_V_RES);   /* 0,1,2 */

        for (x = 0; x < P4_PANEL_H_RES; x++)
        {
            volatile unsigned char *at = row + x * P4_FB_BYTES_PER_PIXEL;
            unsigned int b;

            for (b = 0; b < P4_FB_BYTES_PER_PIXEL; b++)
                at[b] = (b == which) ? 0xFF : 0x00;
        }
    }
}
