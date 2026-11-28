/*
    Copyright (c) 2026, The AROS Development Team. All rights reserved.
    $Id$

    Desc: Stop a display scanout the previous firmware left running.

    A CPU reset does not reset the GDMA, as it does not reset the PMU or the
    clock dividers. Firmware that drives the panel from a framebuffer in
    PSRAM - this port's own display driver, or the vendor's - leaves the
    channel walking that framebuffer over AXI after a reset, while the PSRAM
    bring-up reconfigures the MSPI controller underneath it. That hangs the
    bring-up, reproducibly, and before this existed only the vendor firmware
    or a power cycle recovered the board.

    Called before the PSRAM is touched, and written to be safe when nothing
    was running: the module clocks are enabled first so the registers answer
    at all, and every write is idempotent.
*/

#include <inttypes.h>

#include "hardware.h"
#include "kernel_intern.h"

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

void krnP4ScanoutQuiesce(void)
{
    /*
     * DMA first: it is the part still driving AXI reads against PSRAM. The
     * GDMA's reset is released before its registers are written, because
     * after a cold boot the block may be held in reset and this has to be
     * safe on the first boot as well as on a reset with scanout running.
     */
    p4_w32(P4_CLKRST_SOC_CLK_CTRL0,
           p4_r32(P4_CLKRST_SOC_CLK_CTRL0) | P4_GDMA_CPU_CLK_EN);
    p4_w32(P4_CLKRST_SOC_CLK_CTRL1,
           p4_r32(P4_CLKRST_SOC_CLK_CTRL1) | P4_GDMA_SYS_CLK_EN);
    p4_w32(P4_CLKRST_HP_RST_EN0,
           p4_r32(P4_CLKRST_HP_RST_EN0) & ~P4_RST_EN_GDMA);

    /* Stop completion delivery before stopping the channel, otherwise a
       frame ending in this window can re-arm the reader being stopped. */
    krnCLICDisable(P4_DSI_DMA_LINE);
    ch_wr(P4_DMAC_CH_INTSIGNAL_ENABLE0, 0);
    p4_w32(P4_DMAC_CFG, p4_r32(P4_DMAC_CFG) & ~P4_DMAC_INT_EN);

    /* Channel off, with the write-enable the register demands */
    p4_w32(P4_DMAC_CHEN, P4_DMAC_CH1_EN_WE);

    /* Then the whole controller through its reset, the only way to be sure
       of a channel that was in the middle of a burst */
    p4_w32(P4_CLKRST_HP_RST_EN0,
           p4_r32(P4_CLKRST_HP_RST_EN0) | P4_RST_EN_GDMA);
    p4_w32(P4_CLKRST_HP_RST_EN0,
           p4_r32(P4_CLKRST_HP_RST_EN0) & ~P4_RST_EN_GDMA);

    /*
     * The producer on the other side of that channel as well. On revision
     * one the DSI bridge has no usable internal soft reset; ESP-IDF's panel
     * teardown disables the DPI clock and the bridge before the chip-level
     * DSI reset. Enable the DSI system clock and release the block reset
     * first, or the bridge registers do not answer.
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
