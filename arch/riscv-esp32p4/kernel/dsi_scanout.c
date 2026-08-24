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

    at[0] = (unsigned char)(v & 0xFF);
    at[1] = (unsigned char)(v >> 8);
#endif
}

void krnP4ScanoutFill(unsigned long rgb)
{
    volatile unsigned char *fb = (volatile unsigned char *)P4_FB_BASE;
    unsigned long i;

    for (i = 0; i < (unsigned long)P4_PANEL_H_RES * P4_PANEL_V_RES; i++)
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
           | (P4_DMAC_AXI_BURST_LEN << P4_DMAC_AWLEN_SHIFT);

    /*
     * Automatic reload rather than a link list, and the engine's own report is
     * why.
     *
     * The list version put one item in SRAM whose next-pointer addressed
     * itself, so that the transfer would repeat without an interrupt to restart
     * it.  It ran once.  The channel then raised
     * SHADOWREG_OR_LLI_INVALID_ERR - bit 13 of CH_INTSTATUS0 - and the item
     * read back with its control word at 0x000f87c0: source address,
     * destination, block size and self-pointer all intact, and bit 31, the
     * valid bit this code had written, cleared.  The engine clears that bit
     * when it consumes an item, so an item pointing at itself is valid exactly
     * once and invalid every time after.  That is also why the reference marks
     * its single item last and re-arms the channel from a transfer-done
     * callback: with a list there is no other way.
     *
     * Reload needs neither.  The channel keeps its transfer parameters in its
     * own registers, restores them at the end of every block and starts the
     * next one, indefinitely, until the channel is disabled.  For a scanout
     * that is exactly the required behaviour, and it removes the descriptor,
     * its cache maintenance and the interrupt handler this port does not have.
     */
    ch_wr(P4_DMAC_CH_SAR, P4_FB_BASE);
    ch_wr(P4_DMAC_CH_SAR + 4, 0);
    ch_wr(P4_DMAC_CH_DAR, P4_DSI_BRG_MEM_BASE);
    ch_wr(P4_DMAC_CH_DAR + 4, 0);
    ch_wr(P4_DMAC_CH_BLOCK_TS, P4_FB_WORDS64 - 1);
    ch_wr(P4_DMAC_CH_CTL0, ctl_lo);
    ch_wr(P4_DMAC_CH_CTL1, ctl_hi);

    /*
     * The frame itself still needs the cache written back: the DMA reads it
     * over AXI and does not see the CPU's caches, so a frame in dirty lines is
     * a frame of whatever memory held before.  The descriptor no longer does,
     * because there is no descriptor.
     */
    krnP4CacheWriteback();

    /* Both sides reload their own configuration at the end of each block */
    ch_wr(P4_DMAC_CH_CFG0,
          (P4_DMAC_MULTBLK_RELOAD << P4_DMAC_SRC_MULTBLK_SHIFT)
        | (P4_DMAC_MULTBLK_RELOAD << P4_DMAC_DST_MULTBLK_SHIFT));

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

    /* No list to walk */
    ch_wr(P4_DMAC_CH_LLP, 0);
    ch_wr(P4_DMAC_CH_LLP + 4, 0);

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
     *
     * The panel shows the frame twice down its height and the DMA has been
     * measured reading the framebuffer once per panel frame, so those two
     * facts do not compose.  A frame whose halves differ separates them: two
     * banded regions means the buffer is being read twice after all, one
     * banded region above a black one means it is read once and the panel is
     * addressing its lines at half the expected pitch.
     */
    for (y = 0; y < P4_PANEL_V_RES; y++)
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

    for (y = 0; y < P4_PANEL_V_RES; y++)
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

    for (y = 0; y < P4_PANEL_V_RES; y++)
    {
        volatile unsigned char *row = fb + y * P4_PANEL_H_RES
                                          * P4_FB_BYTES_PER_PIXEL;
        unsigned long ry = y % 100;
        int y_line = (ry < 2) || (((y / 100) % 5) == 0 && ry < 4);

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
