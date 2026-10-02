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
#include <aros/framebuffer.h>
#include "psram.h"

#if defined(P4_B5_ROW_PHASE_COMPENSATION) \
    && defined(P4_B6_ROW_PHASE_WORKAROUND)
#error select either the B5 diagnostic compensation or the B6 workaround
#elif defined(P4_B6_ROW_PHASE_WORKAROUND)
#define P4_SCANOUT_ROW_PHASE_WORKAROUND P4_B6_ROW_PHASE_WORKAROUND
#elif defined(P4_B5_ROW_PHASE_COMPENSATION)
#define P4_SCANOUT_ROW_PHASE_WORKAROUND P4_B5_ROW_PHASE_COMPENSATION
#endif

#if defined(P4_SCANOUT_ROW_PHASE_WORKAROUND) \
    && P4_SCANOUT_ROW_PHASE_WORKAROUND >= P4_PANEL_H_RES
#error scanout row-phase workaround must be smaller than the panel width
#endif

#ifndef P4_B6_SWAP_FRAMES
#define P4_B6_SWAP_FRAMES 101
#endif

#if defined(P4_B6_DOUBLE_BUFFER) && P4_B6_SWAP_FRAMES < 1
#error P4_B6_SWAP_FRAMES must be at least one frame
#endif

#ifndef P4_B5_PHASE_CALIBRATION_BASE
#define P4_B5_PHASE_CALIBRATION_BASE 544
#endif

#ifndef P4_B5_PHASE_CALIBRATION_STEP
#define P4_B5_PHASE_CALIBRATION_STEP 8
#endif

#ifndef P4_B5_PHASE_CALIBRATION_WIDTH
#define P4_B5_PHASE_CALIBRATION_WIDTH 16
#endif

#if P4_B5_PHASE_CALIBRATION_STEP < 1
#error P4_B5_PHASE_CALIBRATION_STEP must be at least one pixel
#endif

#if P4_B5_PHASE_CALIBRATION_WIDTH < 1
#error P4_B5_PHASE_CALIBRATION_WIDTH must be at least one pixel
#endif

#if P4_B5_PHASE_CALIBRATION_BASE + 8 * P4_B5_PHASE_CALIBRATION_STEP \
        + P4_B5_PHASE_CALIBRATION_WIDTH > P4_PANEL_H_RES
#error phase calibration markers must fit inside one raw framebuffer row
#endif

#if defined(P4_B5_EARLY_GDMA_CREATE) && !defined(P4_B5_EARLY_DPI_CREATE)
#error P4_B5_EARLY_GDMA_CREATE requires P4_B5_EARLY_DPI_CREATE
#elif defined(P4_B5_EARLY_GDMA_CREATE) && defined(P4_B5_DMA_RELOAD)
#error P4_B5_EARLY_GDMA_CREATE requires the reference link-list path
#elif defined(P4_B5_EXACT_DPI_CREATE) && !defined(P4_B5_EARLY_DPI_CREATE)
#error P4_B5_EXACT_DPI_CREATE requires P4_B5_EARLY_DPI_CREATE
#endif

static inline void brg_wr(unsigned long off, unsigned long v)
{
    p4_w32(P4_DSI_BRG_BASE + off, v);
}

static inline unsigned long brg_rd(unsigned long off)
{
    return p4_r32(P4_DSI_BRG_BASE + off);
}

#ifdef P4_B5_EXACT_DPI_CREATE
static void brg_field(unsigned long off, unsigned long mask,
                      unsigned long value)
{
    brg_wr(off, (brg_rd(off) & ~mask) | (value & mask));
}
#endif

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
static volatile unsigned long scanout_active_fb;
static volatile unsigned long scanout_dma_swaps;
static volatile unsigned long scanout_pending_fb;
static volatile unsigned long scanout_dirty_submits;
static volatile unsigned long scanout_dirty_rejects;
/* Single-hart producer/IRQ ownership guard. The IRQ keeps scanning active
 * while task-context preparation owns the inactive/pending surface. */
static volatile unsigned long scanout_copy_busy;

static inline void lli_wr(unsigned long off, unsigned long v)
{
    *(volatile unsigned long *)((unsigned long)scanout_lli
                                + P4_L2MEM_NONCACHE_OFFSET + off) = v;
}

/*
 * Create the controller/channel object without starting a transfer.
 *
 * Vellum's exact IDF v6.0 path does this from esp_lcd_new_panel_dpi(), before
 * the JD9365 reset pulse and command table.  The descriptor is filled and the
 * channel is enabled only later from dpi_panel_init().  Ordinary AROS keeps
 * its historical all-at-start order; P4_B5_EARLY_GDMA_CREATE calls this early
 * and makes krnP4ScanoutDmaUp() retain the live controller state instead of
 * resetting it again.
 */
void krnP4ScanoutDmaCreate(void)
{
    unsigned long cfg1;

    p4_w32(P4_CLKRST_SOC_CLK_CTRL0,
           p4_r32(P4_CLKRST_SOC_CLK_CTRL0) | P4_GDMA_CPU_CLK_EN);
    p4_w32(P4_CLKRST_SOC_CLK_CTRL1,
           p4_r32(P4_CLKRST_SOC_CLK_CTRL1) | P4_GDMA_SYS_CLK_EN);

    p4_w32(P4_CLKRST_HP_RST_EN0,
           p4_r32(P4_CLKRST_HP_RST_EN0) | P4_RST_EN_GDMA);
    p4_w32(P4_CLKRST_HP_RST_EN0,
           p4_r32(P4_CLKRST_HP_RST_EN0) & ~P4_RST_EN_GDMA);

    p4_w32(P4_DMAC_RESET, P4_DMAC_RESET_BIT);
    while (p4_r32(P4_DMAC_RESET) & P4_DMAC_RESET_BIT)
        ;

    p4_w32(P4_DMAC_CFG, P4_DMAC_CFG_EN | P4_DMAC_INT_EN);
    p4_w32(P4_DMAC_CHEN, P4_DMAC_CH1_EN_WE);

    ch_wr(P4_DMAC_CH_CFG0,
          (P4_DMAC_MULTBLK_LIST << P4_DMAC_SRC_MULTBLK_SHIFT)
        | (P4_DMAC_MULTBLK_LIST << P4_DMAC_DST_MULTBLK_SHIFT));

    cfg1 = (P4_DMAC_TT_FC_M2P_DMAC << P4_DMAC_TT_FC_SHIFT)
         | (P4_DMAC_PER_DSI << P4_DMAC_SRC_PER_SHIFT)
         | (P4_DMAC_PER_DSI << P4_DMAC_DST_PER_SHIFT)
         | (1UL << P4_DMAC_CH_PRIOR_SHIFT)
         | (4UL << P4_DMAC_SRC_OSR_SHIFT)
         | (1UL << P4_DMAC_DST_OSR_SHIFT);
    ch_wr(P4_DMAC_CH_CFG1, cfg1);

    /* Match the already proven AROS final interrupt state while moving only
       its creation time.  This avoids conflating lifecycle with IRQ policy. */
    ch_wr(P4_DMAC_CH_INTSIGNAL_ENABLE0, 0);
    ch_wr(P4_DMAC_CH_INTCLEAR0, 0xFFFFFFFFUL);
    ch_wr(P4_DMAC_CH_INTSTATUS_ENABLE0, 0xFFFFFFFFUL);
    ch_wr(P4_DMAC_CH_INTSIGNAL_ENABLE0,
          P4_DMAC_IS_DMA_DONE | P4_DMAC_IS_LLI_INVALID);
    p4_w32(P4_INTMTX_MAP(P4_SOURCE_DW_GDMA), P4_DSI_DMA_LINE);
    krnCLICEnable(P4_DSI_DMA_LINE, 0);
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
static inline void px_raw(volatile unsigned char *at, unsigned long rgb)
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

static inline void px(volatile unsigned char *at, unsigned long rgb)
{
#ifdef P4_SCANOUT_ROW_PHASE_WORKAROUND
    /*
     * Explicit compatibility workaround.  Direct observation says physical X
     * currently consumes framebuffer (X + phase) modulo 800.  B5 uses this as
     * a diagnostic proof; B6 may use the separately named build switch while
     * B5R owns the uncompensated root fix.  Keeping the mapping here makes it
     * independently removable without changing the logical rotation API.
     */
    unsigned long row_bytes = P4_PANEL_H_RES * P4_FB_BYTES_PER_PIXEL;
    unsigned long offset = (unsigned long)at - P4_FB_BASE;
    unsigned long row_offset = offset - offset % row_bytes;
    unsigned long x = (offset % row_bytes) / P4_FB_BYTES_PER_PIXEL;

    x = (x + P4_SCANOUT_ROW_PHASE_WORKAROUND) % P4_PANEL_H_RES;
    at = (volatile unsigned char *)(P4_FB_BASE + row_offset
                                   + x * P4_FB_BYTES_PER_PIXEL);
#endif

    px_raw(at, rgb);
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
#ifndef P4_B5_EXACT_DPI_CREATE
    unsigned long v;
#endif

#ifdef P4_B5_EXACT_DPI_CREATE
    /* Exact volatile-field order emitted by Vellum's linked IDF-v6.0
       esp_lcd_new_panel_dpi() object.  RAW_NUM_SET is a write trigger, so the
       count and alignment must exist before that third write reloads the
       bridge's internal frame counter. */
#ifdef P4_BRG_RAW_DIV
    brg_field(P4_DSI_BRG_RAW_NUM_CFG, P4_DSI_BRG_RAW_NUM_MASK,
              P4_FB_WORDS64 / P4_BRG_RAW_DIV);
#else
    brg_field(P4_DSI_BRG_RAW_NUM_CFG, P4_DSI_BRG_RAW_NUM_MASK,
              P4_FB_WORDS64);
#endif
    brg_field(P4_DSI_BRG_RAW_NUM_CFG, P4_DSI_BRG_UNALIGN_64BIT, 0);
    brg_field(P4_DSI_BRG_RAW_NUM_CFG, P4_DSI_BRG_RAW_NUM_SET,
              P4_DSI_BRG_RAW_NUM_SET);

    brg_field(P4_DSI_BRG_DPI_MISC_CFG, P4_DSI_BRG_DISCARD_MASK,
              (unsigned long)P4_PANEL_H_RES << P4_DSI_BRG_DISCARD_SHIFT);

    /* hw_ver1 uses RAW_TYPE for both input and output.  IDF nevertheless
       writes input type, RGB/YUV selection, output type and DPI sub-config
       separately, in that order. */
#if P4_PANEL_BPP == 24
    brg_field(P4_DSI_BRG_PIXEL_TYPE, P4_DSI_BRG_RAW_TYPE_MASK,
              P4_DSI_BRG_RAW_RGB888);
#else
    brg_field(P4_DSI_BRG_PIXEL_TYPE, P4_DSI_BRG_RAW_TYPE_MASK,
              P4_DSI_BRG_RAW_RGB565);
#endif
    brg_field(P4_DSI_BRG_PIXEL_TYPE, P4_DSI_BRG_DATA_IN_TYPE, 0);
#if P4_PANEL_BPP == 24
    brg_field(P4_DSI_BRG_PIXEL_TYPE, P4_DSI_BRG_RAW_TYPE_MASK,
              P4_DSI_BRG_RAW_RGB888);
#else
    brg_field(P4_DSI_BRG_PIXEL_TYPE, P4_DSI_BRG_RAW_TYPE_MASK,
              P4_DSI_BRG_RAW_RGB565);
#endif
    brg_field(P4_DSI_BRG_PIXEL_TYPE, P4_DSI_BRG_DPI_TYPE_MASK, 0);

    brg_field(P4_DSI_BRG_DMA_FLOW_CTRL, 1UL, P4_DSI_BRG_FLOW_DMA);
    brg_field(P4_DSI_BRG_DMA_FLOW_CTRL, P4_DSI_BRG_MULTIBLK_MASK,
              1UL << P4_DSI_BRG_MULTIBLK_SHIFT);
    brg_field(P4_DSI_BRG_DMA_FRAME_INT, P4_DSI_BRG_MULTIBLK_EN, 0);
    brg_field(P4_DSI_BRG_DMA_REQ_CFG, P4_DSI_BRG_BURST_LEN_MASK, 256UL);
    brg_field(P4_DSI_BRG_EMPTY_THRD, P4_DSI_BRG_EMPTY_MASK,
              1024UL - 256UL);

#ifdef P4_B5_BLK_RAW_FRAME
    brg_wr(P4_DSI_BRG_BLK_RAW_NUM,
           (P4_FB_WORDS64 & P4_DSI_BRG_BLK_RAW_MASK)
           | P4_DSI_BRG_BLK_RAW_SET);
#endif

    brg_field(P4_DSI_BRG_EN, P4_DSI_BRG_DSI_EN, P4_DSI_BRG_DSI_EN);
    brg_field(P4_DSI_BRG_DPI_CFG_UPD, P4_DSI_BRG_CFG_UPDATE,
              P4_DSI_BRG_CFG_UPDATE);
#else

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
#endif

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
#ifdef P4_B5_EXACT_DPI_CREATE
    brg_field(P4_DSI_BRG_DPI_MISC_CFG, P4_DSI_BRG_DPI_EN,
              P4_DSI_BRG_DPI_EN);
#else
    brg_wr(P4_DSI_BRG_DPI_MISC_CFG,
           brg_rd(P4_DSI_BRG_DPI_MISC_CFG) | P4_DSI_BRG_DPI_EN);
#endif
#endif
#ifdef P4_B5_EXACT_DPI_CREATE
    brg_field(P4_DSI_BRG_DPI_CFG_UPD, P4_DSI_BRG_CFG_UPDATE,
              P4_DSI_BRG_CFG_UPDATE);
#else
    brg_wr(P4_DSI_BRG_DPI_CFG_UPD, P4_DSI_BRG_CFG_UPDATE);
#endif
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
    unsigned long ctl_lo, ctl_hi;

#ifndef P4_B5_EARLY_GDMA_CREATE
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
#endif

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
#ifndef P4_B5_EARLY_GDMA_CREATE
    ch_wr(P4_DMAC_CH_CFG0,
          (P4_DMAC_MULTBLK_LIST << P4_DMAC_SRC_MULTBLK_SHIFT)
        | (P4_DMAC_MULTBLK_LIST << P4_DMAC_DST_MULTBLK_SHIFT));
#endif
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
#ifndef P4_B5_EARLY_GDMA_CREATE
    ch_wr(P4_DMAC_CH_CFG1,
          (P4_DMAC_TT_FC_M2P_DMAC << P4_DMAC_TT_FC_SHIFT)
        | (P4_DMAC_PER_DSI << P4_DMAC_SRC_PER_SHIFT)
        | (P4_DMAC_PER_DSI << P4_DMAC_DST_PER_SHIFT)
        | (1UL << P4_DMAC_CH_PRIOR_SHIFT)
        | (4UL << P4_DMAC_SRC_OSR_SHIFT)
        | (1UL << P4_DMAC_DST_OSR_SHIFT));
#endif

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
    scanout_active_fb = P4_FB_BASE;
    scanout_dma_swaps = 0;
    scanout_pending_fb = 0;
    scanout_dirty_submits = 0;
    scanout_dirty_rejects = 0;
#ifndef P4_B5_EARLY_GDMA_CREATE
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
#ifdef P4_B6_DOUBLE_BUFFER
#if defined(P4_B6_DIRTY_GATE) || defined(P4_C1_FRAMEBUFFER_HIDD)
        if (!scanout_copy_busy && scanout_pending_fb != 0
            && scanout_pending_fb != scanout_active_fb)
        {
            scanout_active_fb = scanout_pending_fb;
            scanout_pending_fb = 0;
            scanout_dma_swaps++;
        }
#else
        if (scanout_dma_frames % P4_B6_SWAP_FRAMES == 0)
        {
            scanout_active_fb = scanout_active_fb == P4_FB_BASE
                              ? P4_FB_BACK_BASE : P4_FB_BASE;
            scanout_dma_swaps++;
        }
#endif
        lli_wr(P4_DMAC_LLI_SAR_LO, scanout_active_fb);
#endif
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
    out->active_fb    = scanout_active_fb;
    out->dma_swaps    = scanout_dma_swaps;
    out->pending_fb   = scanout_pending_fb;
    out->dirty_submits = scanout_dirty_submits;
    out->dirty_rejects = scanout_dirty_rejects;
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

#ifdef P4_B6_DOUBLE_BUFFER
/*
 * B6's public surface is 1280x800 landscape; the DMA surface remains the
 * panel-native 800x1280 portrait buffer.  This is the same independently
 * derived transform used by the working product firmware:
 *
 *     physical_index = (logical_width - 1 - x) * 800 + y
 *
 * It is a 90-degree clockwise rotation from logical to physical.  The row
 * workaround, when selected, is applied only after that rotation by px().
 */
static inline void b6_logical_px(unsigned long fb, unsigned long x,
                                 unsigned long y, unsigned long rgb)
{
    unsigned long physical_index =
        (P4_PANEL_V_RES - 1 - x) * P4_PANEL_H_RES + y;

    px((volatile unsigned char *)(fb
       + physical_index * P4_FB_BYTES_PER_PIXEL), rgb);
}

static void b6_logical_fill(unsigned long fb, unsigned long rgb)
{
    unsigned long y, x;

    for (y = 0; y < P4_PANEL_H_RES; y++)
        for (x = 0; x < P4_PANEL_V_RES; x++)
            b6_logical_px(fb, x, y, rgb);
}

static void b6_logical_rect(unsigned long fb, unsigned long x,
                            unsigned long y, unsigned long w,
                            unsigned long h, unsigned long rgb)
{
    unsigned long yy, xx;

    if (x >= P4_PANEL_V_RES || y >= P4_PANEL_H_RES)
        return;
    if (w > P4_PANEL_V_RES - x)
        w = P4_PANEL_V_RES - x;
    if (h > P4_PANEL_H_RES - y)
        h = P4_PANEL_H_RES - y;

    for (yy = y; yy < y + h; yy++)
        for (xx = x; xx < x + w; xx++)
            b6_logical_px(fb, xx, yy, rgb);
}

#ifdef P4_C1_FRAMEBUFFER_HIDD
void krnP4ScanoutC1Clear(void)
{
    b6_logical_fill(P4_FB_BASE, 0x000000UL);
    b6_logical_fill(P4_FB_BACK_BASE, 0x000000UL);
}
#endif

/*
 * Two immutable frames for the first B6 hardware gate.
 *
 * Front: four large logical landscape quadrants and a four-pixel black cross.
 * Back: a dark field with differently sized coloured blocks in all four
 * logical corners plus an asymmetric white/cyan centre marker.  The ISR swaps
 * their descriptor source every P4_B6_SWAP_FRAMES completions.  Since neither
 * live surface is modified, any split image is a handoff defect rather than a
 * cache/coherency ambiguity.
 */
void krnP4ScanoutB6Frames(void)
{
    unsigned long y, x;

#ifdef P4_B6_DIRTY_GATE
    /* Both sources begin with the same orientation witness.  The producer
       gate below changes only its central arena, so a moving rectangle cannot
       erase or fake any of the four logical-corner checks. */
    b6_logical_fill(P4_FB_BASE, 0x080808UL);
    b6_logical_fill(P4_FB_BACK_BASE, 0x080808UL);
    b6_logical_rect(P4_FB_BASE, 0, 0, 149, 73, 0xFF0000UL);
    b6_logical_rect(P4_FB_BASE, P4_PANEL_V_RES - 91, 0,
                    91, 127, 0x00FF00UL);
    b6_logical_rect(P4_FB_BASE, 0, P4_PANEL_H_RES - 113,
                    67, 113, 0x0000FFUL);
    b6_logical_rect(P4_FB_BASE, P4_PANEL_V_RES - 181,
                    P4_PANEL_H_RES - 47, 181, 47, 0xFFFF00UL);
    b6_logical_rect(P4_FB_BACK_BASE, 0, 0, 149, 73, 0xFF0000UL);
    b6_logical_rect(P4_FB_BACK_BASE, P4_PANEL_V_RES - 91, 0,
                    91, 127, 0x00FF00UL);
    b6_logical_rect(P4_FB_BACK_BASE, 0, P4_PANEL_H_RES - 113,
                    67, 113, 0x0000FFUL);
    b6_logical_rect(P4_FB_BACK_BASE, P4_PANEL_V_RES - 181,
                    P4_PANEL_H_RES - 47, 181, 47, 0xFFFF00UL);
    return;
#endif

    for (y = 0; y < P4_PANEL_H_RES; y++)
    {
        for (x = 0; x < P4_PANEL_V_RES; x++)
        {
            unsigned long rgb;

            if (x >= P4_PANEL_V_RES / 2 - 2
                && x < P4_PANEL_V_RES / 2 + 2)
                rgb = 0x000000UL;
            else if (y >= P4_PANEL_H_RES / 2 - 2
                     && y < P4_PANEL_H_RES / 2 + 2)
                rgb = 0x000000UL;
            else if (y < P4_PANEL_H_RES / 2)
                rgb = x < P4_PANEL_V_RES / 2
                    ? 0xFF0000UL : 0x00FF00UL;
            else
                rgb = x < P4_PANEL_V_RES / 2
                    ? 0x0000FFUL : 0xFFFF00UL;
            b6_logical_px(P4_FB_BASE, x, y, rgb);
        }
    }

    b6_logical_fill(P4_FB_BACK_BASE, 0x080808UL);
    b6_logical_rect(P4_FB_BACK_BASE, 0, 0, 149, 73, 0xFF0000UL);
    b6_logical_rect(P4_FB_BACK_BASE, P4_PANEL_V_RES - 91, 0,
                    91, 127, 0x00FF00UL);
    b6_logical_rect(P4_FB_BACK_BASE, 0, P4_PANEL_H_RES - 113,
                    67, 113, 0x0000FFUL);
    b6_logical_rect(P4_FB_BACK_BASE, P4_PANEL_V_RES - 181,
                    P4_PANEL_H_RES - 47, 181, 47, 0xFFFF00UL);
    b6_logical_rect(P4_FB_BACK_BASE, 449, 383, 383, 31, 0xFFFFFFUL);
    b6_logical_rect(P4_FB_BACK_BASE, 623, 277, 29, 247, 0x00FFFFUL);
}

/*
 * Publish exactly the physical cache ranges touched by one logical rectangle.
 * Rotation turns each logical X into one physical row and logical Y into a
 * contiguous run within that row.  The temporary +525 compatibility mapping
 * can wrap that run at the physical row edge, hence at most two writebacks per
 * affected row.  No byte outside the rectangle's cache lines is requested.
 */
static int b6_dirty_writeback(unsigned long fb, unsigned long x,
                              unsigned long y, unsigned long w,
                              unsigned long h)
{
    unsigned long xx;
    unsigned long mapped_y = y;

    if (!w || !h || x >= P4_PANEL_V_RES || y >= P4_PANEL_H_RES
        || w > P4_PANEL_V_RES - x || h > P4_PANEL_H_RES - y)
        return 0;

#ifdef P4_SCANOUT_ROW_PHASE_WORKAROUND
    mapped_y = (mapped_y + P4_SCANOUT_ROW_PHASE_WORKAROUND)
             % P4_PANEL_H_RES;
#endif

    for (xx = x; xx < x + w; xx++)
    {
        unsigned long physical_row = P4_PANEL_V_RES - 1 - xx;
        unsigned long first = h;
        unsigned long row = fb + physical_row * P4_PANEL_H_RES
                                  * P4_FB_BYTES_PER_PIXEL;

        if (first > P4_PANEL_H_RES - mapped_y)
            first = P4_PANEL_H_RES - mapped_y;
        krnP4CacheWritebackData(
            (void *)(row + mapped_y * P4_FB_BYTES_PER_PIXEL),
            first * P4_FB_BYTES_PER_PIXEL);
        if (first < h)
            krnP4CacheWritebackData((void *)row,
                (h - first) * P4_FB_BYTES_PER_PIXEL);
    }
    return 1;
}

#ifdef P4_C1_FRAMEBUFFER_HIDD
#include "framebuffer_rotate.h"
#ifdef P4_C1_COALESCE
static unsigned long c1_dirty_surface;
static unsigned long c1_dirty_x, c1_dirty_y, c1_dirty_right, c1_dirty_bottom;

static void c1_dirty_add(unsigned long surface, unsigned long x,
    unsigned long y, unsigned long w, unsigned long h)
{
    if (!c1_dirty_surface)
    {
        c1_dirty_x = x; c1_dirty_y = y;
        c1_dirty_right = x + w; c1_dirty_bottom = y + h;
        c1_dirty_surface = surface;
    }
    else
    {
        if (x < c1_dirty_x) c1_dirty_x = x;
        if (y < c1_dirty_y) c1_dirty_y = y;
        if (x + w > c1_dirty_right) c1_dirty_right = x + w;
        if (y + h > c1_dirty_bottom) c1_dirty_bottom = y + h;
    }
}

/* The last displayed dirty box is the only stale area of the old front.
 * Synchronize it once per subsequent frame, not once per UpdateRect. */
static int c1_sync_stale(unsigned long active, unsigned long target)
{
    unsigned long phase = 0;
    if (!c1_dirty_surface) return 1;
    if (c1_dirty_surface == target) return 1; /* pending: preserve earlier updates */
    if (c1_dirty_surface != active) return 0;
#ifdef P4_SCANOUT_ROW_PHASE_WORKAROUND
    phase = P4_SCANOUT_ROW_PHASE_WORKAROUND;
#endif
    p4_mirror_rect((volatile uint16_t *)target,
        (const volatile uint16_t *)active, P4_PANEL_V_RES, P4_PANEL_H_RES,
        phase, c1_dirty_x, c1_dirty_y,
        c1_dirty_right - c1_dirty_x, c1_dirty_bottom - c1_dirty_y);
    if (!b6_dirty_writeback(target, c1_dirty_x, c1_dirty_y,
        c1_dirty_right - c1_dirty_x, c1_dirty_bottom - c1_dirty_y)) return 0;
    c1_dirty_surface = 0;
    return 1;
}
#endif
#ifdef P4_C1_FULL_DIAGNOSTICS
struct C1PixelSummary
{
    unsigned long nonzero;
    unsigned long sum;
    unsigned long xor_value;
};

static unsigned int c1_full_diagnostics;

/* An order-independent census makes the rotated destination directly
 * comparable with its logical source.  It is deliberately bounded to the
 * first two full-screen submissions and is diagnostic evidence, not a data
 * integrity primitive. */
static void c1_logical_summary(const unsigned char *logical,
                               unsigned long pitch,
                               struct C1PixelSummary *out)
{
    unsigned long y, x;

    out->nonzero = 0;
    out->sum = 0;
    out->xor_value = 0;
    for (y = 0; y < P4_PANEL_H_RES; y++)
    {
        const unsigned char *src = logical + y * pitch;

        for (x = 0; x < P4_PANEL_V_RES; x++, src += 2)
        {
            unsigned long value = (unsigned long)src[0]
                                | ((unsigned long)src[1] << 8);

            if (value)
                out->nonzero++;
            out->sum += value;
            out->xor_value ^= value;
        }
    }
}

static void c1_physical_summary(unsigned long fb,
                                struct C1PixelSummary *out)
{
    const volatile unsigned char *src =
        (const volatile unsigned char *)fb;
    unsigned long i;

    out->nonzero = 0;
    out->sum = 0;
    out->xor_value = 0;
    for (i = 0; i < P4_FB_BYTES; i += 2)
    {
        unsigned long value = (unsigned long)src[i]
                            | ((unsigned long)src[i + 1] << 8);

        if (value)
            out->nonzero++;
        out->sum += value;
        out->xor_value ^= value;
    }
}

static unsigned int c1_logical_sample(const unsigned char *logical,
                                      unsigned long pitch,
                                      unsigned long x, unsigned long y)
{
    const unsigned char *src = logical + y * pitch + x * 2;

    return (unsigned int)src[0] | ((unsigned int)src[1] << 8);
}

static unsigned int c1_physical_sample(unsigned long fb,
                                       unsigned long x, unsigned long y)
{
    unsigned long physical_index =
        (P4_PANEL_V_RES - 1 - x) * P4_PANEL_H_RES + y;
    unsigned long physical_x = y;
    const volatile unsigned char *src;

#ifdef P4_SCANOUT_ROW_PHASE_WORKAROUND
    physical_x = (physical_x + P4_SCANOUT_ROW_PHASE_WORKAROUND)
               % P4_PANEL_H_RES;
#endif
    src = (const volatile unsigned char *)(fb
        + (physical_index - y + physical_x) * P4_FB_BYTES_PER_PIXEL);
    return (unsigned int)src[0] | ((unsigned int)src[1] << 8);
}

static void c1_put_summary(const char *name,
                           const struct C1PixelSummary *summary)
{
    krnP4PutStr(name);
    krnP4PutStr(" nz ");
    krnP4PutDec((uint32_t)summary->nonzero);
    krnP4PutStr(" sum ");
    krnP4PutHex32((uint32_t)summary->sum);
    krnP4PutStr(" xor ");
    krnP4PutHex32((uint32_t)summary->xor_value);
}

static void c1_report_full_update(const unsigned char *logical,
                                  unsigned long pitch,
                                  unsigned long front,
                                  unsigned long back)
{
    static const unsigned short points[][2] =
    {
        { 0, 0 }, { 1279, 0 }, { 0, 799 }, { 1279, 799 }, { 640, 400 }
    };
    struct C1PixelSummary source, front_summary, back_summary;
    unsigned int i;

    c1_logical_summary(logical, pitch, &source);
    c1_physical_summary(front, &front_summary);
    c1_physical_summary(back, &back_summary);
    krnP4PutStr("[c1diag] full ");
    krnP4PutDec(c1_full_diagnostics);
    krnP4PutStr(" source ");
    krnP4PutHex32((uint32_t)(unsigned long)logical);
    krnP4PutStr(" pitch ");
    krnP4PutDec((uint32_t)pitch);
    c1_put_summary(" src", &source);
    c1_put_summary(" front", &front_summary);
    c1_put_summary(" back", &back_summary);
    krnP4PutC('\n');
    krnP4PutStr("[c1diag] samples src/front/back");
    for (i = 0; i < sizeof(points) / sizeof(points[0]); i++)
    {
        unsigned long x = points[i][0];
        unsigned long y = points[i][1];

        krnP4PutStr(" ");
        krnP4PutHex32(c1_logical_sample(logical, pitch, x, y));
        krnP4PutStr("/");
        krnP4PutHex32(c1_physical_sample(front, x, y));
        krnP4PutStr("/");
        krnP4PutHex32(c1_physical_sample(back, x, y));
    }
    krnP4PutC('\n');
}

#endif /* P4_C1_FULL_DIAGNOSTICS */

/*
 * Copy one logical RGB565 rectangle into a physical portrait surface.
 * The logical bitmap remains ordinary, contiguous 1280x800 memory owned by
 * the HIDD.  Rotation and the temporary row-phase compatibility mapping are
 * below that interface, beside the DMA that consumes the result.
 */
static void c1_copy_rect(unsigned long fb, const unsigned char *logical,
                         unsigned long logical_pitch, unsigned long x,
                         unsigned long y, unsigned long w, unsigned long h)
{
    unsigned long phase = 0;
#ifdef P4_SCANOUT_ROW_PHASE_WORKAROUND
    phase = P4_SCANOUT_ROW_PHASE_WORKAROUND;
#endif
    p4_rotate_rect((volatile uint16_t *)fb, logical, logical_pitch,
        P4_PANEL_V_RES, P4_PANEL_H_RES, phase, x, y, w, h);
}

static int c1_wait_no_pending(unsigned long expected_active)
{
    unsigned long spins;

    /* More than ten nominal frame periods at the slowest supported CPU.
       Machine interrupts remain enabled while this task-context wait runs. */
    for (spins = 0; spins < 50000000UL; spins++)
    {
        if (!scanout_copy_busy && scanout_pending_fb == 0
            && (!expected_active || scanout_active_fb == expected_active))
            return 1;
        asm volatile("nop");
    }
    scanout_dirty_rejects++;
    return 0;
}

static BOOL c1_update_rect(CONST_APTR logical_ptr, ULONG logical_pitch,
                           LONG x, LONG y, LONG width, LONG height)
{
    const unsigned char *logical = (const unsigned char *)logical_ptr;
    unsigned long active, target;
#ifdef P4_C1_PROFILE
    static unsigned long profile_count;
    uint64_t start = krnTimerCount(), copied, cleaned, swapped, mirrored, finished;
#endif

    if (!logical || logical_pitch < P4_PANEL_V_RES * 2
        || x < 0 || y < 0 || width <= 0 || height <= 0
        || (unsigned long)x + (unsigned long)width > P4_PANEL_V_RES
        || (unsigned long)y + (unsigned long)height > P4_PANEL_H_RES)
    {
        scanout_dirty_rejects++;
        return FALSE;
    }

#ifdef P4_C1_COALESCE
    scanout_copy_busy = 1;
    asm volatile("fence rw, rw" ::: "memory");
#else
    if (!c1_wait_no_pending(0))
        return FALSE;
#endif

    active = scanout_active_fb;
    if (active == P4_FB_BASE)
        target = P4_FB_BACK_BASE;
    else if (active == P4_FB_BACK_BASE)
        target = P4_FB_BASE;
    else
    {
        scanout_dirty_rejects++;
        scanout_copy_busy = 0;
        return FALSE;
    }

#ifdef P4_C1_COALESCE
    /* A replacement covering the entire stale box needs no mirror copy. */
    if (c1_dirty_surface == active
        && (unsigned long)x <= c1_dirty_x && (unsigned long)y <= c1_dirty_y
        && (unsigned long)x + width >= c1_dirty_right
        && (unsigned long)y + height >= c1_dirty_bottom)
        c1_dirty_surface = 0;
    if ((scanout_pending_fb && scanout_pending_fb != target)
        || !c1_sync_stale(active, target))
    {
        scanout_dirty_rejects++;
        asm volatile("fence rw, rw" ::: "memory");
        scanout_copy_busy = 0;
        return FALSE;
    }
#endif

    c1_copy_rect(target, logical, logical_pitch, x, y, width, height);
#ifdef P4_C1_PROFILE
    copied = krnTimerCount();
#endif
    if (!b6_dirty_writeback(target, x, y, width, height))
    {
        scanout_dirty_rejects++;
        asm volatile("fence rw, rw" ::: "memory");
        scanout_copy_busy = 0;
        return FALSE;
    }

    scanout_dirty_submits++;
#ifdef P4_C1_COALESCE
    c1_dirty_add(target, x, y, width, height);
#endif
    asm volatile("fence rw, rw" ::: "memory");
    scanout_pending_fb = target;
#ifdef P4_C1_PROFILE
    cleaned = krnTimerCount();
#endif

#ifdef P4_C1_COALESCE
    asm volatile("fence rw, rw" ::: "memory");
    scanout_copy_busy = 0;
#ifdef P4_C1_PROFILE
    swapped = mirrored = cleaned;
    finished = krnTimerCount();
#endif
#else
    /* Once the prepared image is visible, apply the same bounded update to
       the now-inactive mirror.  Both surfaces therefore begin every later
       update in the same state without ever modifying the active one. */
    if (!c1_wait_no_pending(target))
        return FALSE;
#ifdef P4_C1_PROFILE
    swapped = krnTimerCount();
#endif
    c1_copy_rect(active, logical, logical_pitch, x, y, width, height);
#ifdef P4_C1_PROFILE
    mirrored = krnTimerCount();
#endif
    if (!b6_dirty_writeback(active, x, y, width, height))
    {
        scanout_dirty_rejects++;
        return FALSE;
    }
#endif /* P4_C1_COALESCE */
#ifdef P4_C1_PROFILE
    finished = krnTimerCount();
    profile_count++;
    if (profile_count <= 16 || (profile_count % 128) == 0
        || (width == P4_PANEL_V_RES && height == P4_PANEL_H_RES))
    {
        krnP4PutStr("[c1perf] n "); krnP4PutDec(profile_count);
        krnP4PutStr(" w "); krnP4PutDec(width);
        krnP4PutStr(" h "); krnP4PutDec(height);
#ifdef P4_C1_COALESCE
        krnP4PutStr(" us prepare(includes-stale-sync)/publish/queued-total ");
        krnP4PutDec((copied - start) / 16); krnP4PutC('/');
        krnP4PutDec((cleaned - copied) / 16); krnP4PutC('/');
        krnP4PutDec((finished - start) / 16); krnP4PutC('\n');
#else
        krnP4PutStr(" us copy/clean/wait/mirror/clean/total ");
        krnP4PutDec((copied - start) / 16); krnP4PutC('/');
        krnP4PutDec((cleaned - copied) / 16); krnP4PutC('/');
        krnP4PutDec((swapped - cleaned) / 16); krnP4PutC('/');
        krnP4PutDec((mirrored - swapped) / 16); krnP4PutC('/');
        krnP4PutDec((finished - mirrored) / 16); krnP4PutC('/');
        krnP4PutDec((finished - start) / 16); krnP4PutC('\n');
#endif
    }
#endif
#ifdef P4_C1_FULL_DIAGNOSTICS
    if ((unsigned long)x == 0 && (unsigned long)y == 0
        && (unsigned long)width == P4_PANEL_V_RES
        && (unsigned long)height == P4_PANEL_H_RES
        && c1_full_diagnostics < 2)
    {
        c1_full_diagnostics++;
        c1_wait_no_pending(target);
        c1_report_full_update(logical, logical_pitch,
                              P4_FB_BASE, P4_FB_BACK_BASE);
    }
#endif
    return TRUE;
}

static BOOL c1_clear(ULONG pixel)
{
    unsigned long active, target, y;

    if (!c1_wait_no_pending(0))
        return FALSE;
    active = scanout_active_fb;
    target = active == P4_FB_BASE ? P4_FB_BACK_BASE : P4_FB_BASE;
    if ((active != P4_FB_BASE && active != P4_FB_BACK_BASE))
        return FALSE;

    for (y = 0; y < P4_FB_BYTES / 2; y++)
        ((volatile uint16_t *)target)[y] = (uint16_t)pixel;
    krnP4CacheWritebackData((void *)target, P4_FB_BYTES);
    scanout_dirty_submits++;
    asm volatile("fence rw, rw" ::: "memory");
    scanout_pending_fb = target;
    if (!c1_wait_no_pending(target))
        return FALSE;
    for (y = 0; y < P4_FB_BYTES / 2; y++)
        ((volatile uint16_t *)active)[y] = (uint16_t)pixel;
    krnP4CacheWritebackData((void *)active, P4_FB_BYTES);
#ifdef P4_C1_COALESCE
    c1_dirty_surface = 0;
#endif
    return TRUE;
}

static BOOL c1_flush(VOID)
{
    return c1_wait_no_pending(0) ? TRUE : FALSE;
}

static VOID c1_get_stats(struct KrnFrameBufferStats *stats)
{
    if (!stats)
        return;
    stats->frames = scanout_dma_frames;
    stats->swaps = scanout_dma_swaps;
    stats->faults = scanout_dma_faults;
    stats->rejects = scanout_dirty_rejects;
    stats->active = scanout_active_fb;
    stats->pending = scanout_pending_fb;
}

static struct KrnFrameBufferOps c1_framebuffer_ops =
{
    KRN_FRAMEBUFFER_OPS_VERSION,
    P4_PANEL_V_RES,
    P4_PANEL_H_RES,
    16,
    2,
    P4_PANEL_V_RES * 2,
    P4_FB_BASE,
    P4_FB_BACK_BASE,
    P4_FB_BYTES,
    c1_update_rect,
    c1_clear,
    c1_get_stats,
    c1_flush
};

struct KrnFrameBufferOps *krnP4FrameBufferOps(void)
{
    return &c1_framebuffer_ops;
}
#endif /* P4_C1_FRAMEBUFFER_HIDD */

#ifdef P4_B6_DIRTY_GATE
/*
 * Produce one bounded logical dirty update into the source that is neither
 * active nor pending, clean only its rotated row ranges, then publish it.
 * With no pending source the ISR cannot change ownership while the CPU draws;
 * after publication the CPU never touches that surface until a later swap has
 * made it inactive again.
 */
unsigned long krnP4ScanoutB6DirtyStep(unsigned long second)
{
    static unsigned long old_x[2], old_y[2];
    static unsigned char old_valid[2];
    static const unsigned long colours[] =
    {
        0xFF0000UL, 0x00FF00UL, 0x0000FFUL,
        0xFFFF00UL, 0x00FFFFUL, 0xFF00FFUL, 0xFFFFFFUL,
    };
    const unsigned long arena_x = 192;
    const unsigned long arena_y = 144;
    const unsigned long arena_w = 896;
    const unsigned long arena_h = 512;
    const unsigned long rect_w = 127;
    const unsigned long rect_h = 73;
    unsigned long active, target, slot, x, y;

    if (scanout_pending_fb != 0)
    {
        scanout_dirty_rejects++;
        return 0;
    }

    active = scanout_active_fb;
    if (active == P4_FB_BASE)
    {
        target = P4_FB_BACK_BASE;
        slot = 1;
    }
    else if (active == P4_FB_BACK_BASE)
    {
        target = P4_FB_BASE;
        slot = 0;
    }
    else
    {
        scanout_dirty_rejects++;
        return 0;
    }

    x = arena_x + (second * 97 + 3) % (arena_w - rect_w + 1);
    y = arena_y + (second * 53 + 5) % (arena_h - rect_h + 1);

    if (old_valid[slot])
    {
        b6_logical_rect(target, old_x[slot], old_y[slot],
                        rect_w, rect_h, 0x080808UL);
        if (!b6_dirty_writeback(target, old_x[slot], old_y[slot],
                                rect_w, rect_h))
        {
            scanout_dirty_rejects++;
            return 0;
        }
    }

    b6_logical_rect(target, x, y, rect_w, rect_h,
                    colours[second % (sizeof(colours) / sizeof(colours[0]))]);
    if (!b6_dirty_writeback(target, x, y, rect_w, rect_h))
    {
        scanout_dirty_rejects++;
        return 0;
    }

    old_x[slot] = x;
    old_y[slot] = y;
    old_valid[slot] = 1;

    /* No pending request means frame-done cannot have changed active while
       the CPU was drawing.  Recheck before making the prepared surface owned
       by the ISR, then publish it with a release fence. */
    if (scanout_pending_fb != 0 || scanout_active_fb != active)
    {
        scanout_dirty_rejects++;
        return 0;
    }
    scanout_dirty_submits++;
    asm volatile("fence rw, rw" ::: "memory");
    scanout_pending_fb = target;
    return target;
}
#endif
#endif

/*
 * Immutable raw-source phase ruler.
 *
 * The nine horizontal bands test source phases BASE..BASE+8*STEP.  A band
 * writes a WIDTH-pixel coloured marker at its candidate source X without
 * applying the diagnostic compensation above.  The physical scanout maps
 * that marker to candidate-minus-actual-phase modulo 800: the band whose
 * marker splits across the left and right edges brackets the exact phase to
 * one STEP interval.  Full-width grey separators are invariant under a cyclic
 * row phase and make the bands countable.
 */
void krnP4ScanoutPhaseCalibration(void)
{
    static const unsigned long colours[9] = {
        0xFF0000UL,
        0x00FF00UL,
        0x0000FFUL,
        0xFFFF00UL,
        0x00FFFFUL,
        0xFF00FFUL,
        0xFFFFFFUL,
        0xFF8000UL,
        0x8000FFUL,
    };
    volatile unsigned char *fb = (volatile unsigned char *)P4_FB_BASE;
    unsigned long y, x;

    for (y = 0; y < P4_TX_V_RES; y++)
    {
        unsigned long band = y * 9 / P4_TX_V_RES;
        unsigned long candidate = P4_B5_PHASE_CALIBRATION_BASE
                                  + band * P4_B5_PHASE_CALIBRATION_STEP;
        unsigned long band_y = y * 9 % P4_TX_V_RES;
        volatile unsigned char *row = fb + y * P4_PANEL_H_RES
                                          * P4_FB_BYTES_PER_PIXEL;

        for (x = 0; x < P4_PANEL_H_RES; x++)
        {
            unsigned long rgb = 0x080808UL;

            if (band_y < 4 * 9)
                rgb = 0x404040UL;
            else if (x >= candidate
                     && x < candidate + P4_B5_PHASE_CALIBRATION_WIDTH)
                rgb = colours[band];
            px_raw(row + x * P4_FB_BYTES_PER_PIXEL, rgb);
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
#ifdef P4_B5_LIVE_UPDATE_GATE
    switch (second)
    {
        /* The preloaded coordinate card remains untouched for fifteen
           seconds.  Then change less than one percent of it, at deliberately
           unaligned coordinates, before attempting any full-frame write. */
        case 15:
            scanout_rect(337, 603, 127, 73, 0xFF00FFUL);
            phase = 11;
            break;
        case 30:
            krnP4ScanoutFill(0x00FF00UL);
            phase = 12;
            break;
        case 45:
            krnP4ScanoutFill(0x0000FFUL);
            phase = 13;
            break;
        default:
            break;
    }
#elif defined(P4_B5_VISUAL_GATE)
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
