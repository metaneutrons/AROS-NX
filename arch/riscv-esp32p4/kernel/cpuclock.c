/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: Raise the CPU and memory clocks from what the bootloader left.
*/

/*
 * Why this exists.
 *
 * B1 set out to raise the PSRAM bus clock and found that the bus was never
 * the limit.  With the calibration working and the bus at 200 MHz, a
 * sequential read reached 20 MB/s; the same loop over internal SRAM, with no
 * external bus in the path at all, reached 25.  Both numbers are explained by
 * one measurement: mcycle against the 16 MHz system timer put the CPU at 90
 * MHz.  The CPLL was already at 360 and the CPU divider was sitting at four,
 * which is the state a boot arrives in when nothing configures it.
 *
 * So this is not an optimisation.  A display driver reads a framebuffer by
 * DMA and does not care about the CPU, but everything that composes into that
 * framebuffer does, and a fourfold CPU clock with a doubled memory clock is
 * the largest single change available to this port.
 *
 * The dividers move in the order ESP-IDF's own comment demands, and for the
 * reason it gives: between the first write and the last there are states in
 * which APB or MEM would be above its limit while the CPU is already fast,
 * and the hardware may silently correct an illegal divider without saying so
 * in the register.  Upscaling therefore slows the far end of the chain first
 * and speeds the CPU last, so every intermediate state is slower than both
 * endpoints rather than faster than one of them.
 */

#include <inttypes.h>
#include <exec/types.h>

#include "hardware.h"
#include "kernel_intern.h"

/*
 * Commit the divider registers and wait for the hardware to acknowledge.
 *
 * The update bit is write-to-trigger and reads back set until the change has
 * taken effect, so the wait is the hardware's own statement that it is done.
 * Bounded, because a bring-up step that can hang is worse than one that
 * reports failure, and this one runs before there is anything to report with.
 */
static int p4_clk_commit(void)
{
    unsigned int spin = 100000;

    p4_w32(P4_CLKRST_ROOT_CLK_CTRL0,
           p4_r32(P4_CLKRST_ROOT_CLK_CTRL0) | P4_SOC_CLK_DIV_UPDATE);

    while ((p4_r32(P4_CLKRST_ROOT_CLK_CTRL0) & P4_SOC_CLK_DIV_UPDATE) && --spin)
        ;

    return spin != 0;
}

static void p4_set_field(unsigned long reg, unsigned long mask,
                         unsigned int shift, unsigned long value)
{
    p4_w32(reg, (p4_r32(reg) & ~mask) | ((value << shift) & mask));
}

/* The dividers as the registers hold them, which is the value less one. */
void krnP4CPUClockRead(struct P4CPUClock *out)
{
    unsigned long c0 = p4_r32(P4_CLKRST_ROOT_CLK_CTRL0);
    unsigned long c1 = p4_r32(P4_CLKRST_ROOT_CLK_CTRL1);
    unsigned long c2 = p4_r32(P4_CLKRST_ROOT_CLK_CTRL2);

    out->source = (unsigned char)(p4_r32(P4_LP_CLKRST_HP_CLK_CTRL_R)
                                 & P4_HP_ROOT_SRC_MASK);
    out->cpu_div = (unsigned char)
        (((c0 & P4_CPU_CLK_DIV_NUM_MASK) >> P4_CPU_CLK_DIV_NUM_SHIFT) + 1);
    out->cpu_numerator = (unsigned char)
        ((c0 & P4_CPU_CLK_DIV_NUMER_MASK) >> P4_CPU_CLK_DIV_NUMER_SHIFT);
    out->cpu_denominator = (unsigned char)
        ((c0 & P4_CPU_CLK_DIV_DENOM_MASK) >> P4_CPU_CLK_DIV_DENOM_SHIFT);
    out->mem_div = (unsigned char)
        (((c1 & P4_MEM_CLK_DIV_NUM_MASK) >> P4_MEM_CLK_DIV_NUM_SHIFT) + 1);
    out->sys_div = (unsigned char)
        (((c1 & P4_SYS_CLK_DIV_NUM_MASK) >> P4_SYS_CLK_DIV_NUM_SHIFT) + 1);
    out->apb_div = (unsigned char)
        (((c2 & P4_APB_CLK_DIV_NUM_MASK) >> P4_APB_CLK_DIV_NUM_SHIFT) + 1);
}

/*
 * Ask for one of the three configurations the constraints allow.
 *
 * Only 360, 180 and 90 are accepted, and not because a fractional divider is
 * unavailable - the CPU divider has a numerator and a denominator - but
 * because any other pairing of MEM and APB dividers is a combination ESP-IDF
 * refuses to produce and this port has no way to validate.  An unsupported
 * request is refused rather than approximated.
 *
 * Returns non-zero if the registers hold what was asked for afterwards.  Does
 * not attempt to verify the resulting frequency; the caller does that by
 * measurement, which is the only check that means anything here.
 */
int krnP4CPUClockSet(unsigned int mhz)
{
    unsigned int cpu_div, mem_div, apb_div;
    struct P4CPUClock now;

    switch (mhz)
    {
    case 360: cpu_div = 1; mem_div = 2; apb_div = 2; break;
    case 180: cpu_div = 2; mem_div = 1; apb_div = 2; break;
    case 90:  cpu_div = 4; mem_div = 1; apb_div = 1; break;
    default:  return 0;
    }

    krnP4CPUClockRead(&now);

    /* Nothing here changes the root, so a board that is not on the CPLL is
       one this function cannot reason about. */
    if (now.source != P4_HP_ROOT_SRC_CPLL)
        return 0;

    if (now.cpu_div == cpu_div && now.mem_div == mem_div
        && now.apb_div == apb_div)
        return 1;

    if (cpu_div < now.cpu_div)
    {
        /* Upscaling: the far end of the chain first, the CPU last. */
        p4_set_field(P4_CLKRST_ROOT_CLK_CTRL2, P4_APB_CLK_DIV_NUM_MASK,
                     P4_APB_CLK_DIV_NUM_SHIFT, apb_div - 1);
        if (!p4_clk_commit())
            return 0;
        p4_set_field(P4_CLKRST_ROOT_CLK_CTRL1, P4_SYS_CLK_DIV_NUM_MASK,
                     P4_SYS_CLK_DIV_NUM_SHIFT, 0);
        if (!p4_clk_commit())
            return 0;
        p4_set_field(P4_CLKRST_ROOT_CLK_CTRL1, P4_MEM_CLK_DIV_NUM_MASK,
                     P4_MEM_CLK_DIV_NUM_SHIFT, mem_div - 1);
        if (!p4_clk_commit())
            return 0;
        p4_set_field(P4_CLKRST_ROOT_CLK_CTRL0, P4_CPU_CLK_DIV_NUM_MASK,
                     P4_CPU_CLK_DIV_NUM_SHIFT, cpu_div - 1);
        p4_set_field(P4_CLKRST_ROOT_CLK_CTRL0, P4_CPU_CLK_DIV_NUMER_MASK,
                     P4_CPU_CLK_DIV_NUMER_SHIFT, 0);
        p4_set_field(P4_CLKRST_ROOT_CLK_CTRL0, P4_CPU_CLK_DIV_DENOM_MASK,
                     P4_CPU_CLK_DIV_DENOM_SHIFT, 0);
        if (!p4_clk_commit())
            return 0;
    }
    else
    {
        /* Downscaling: the CPU first, the far end last. */
        p4_set_field(P4_CLKRST_ROOT_CLK_CTRL0, P4_CPU_CLK_DIV_NUM_MASK,
                     P4_CPU_CLK_DIV_NUM_SHIFT, cpu_div - 1);
        p4_set_field(P4_CLKRST_ROOT_CLK_CTRL0, P4_CPU_CLK_DIV_NUMER_MASK,
                     P4_CPU_CLK_DIV_NUMER_SHIFT, 0);
        p4_set_field(P4_CLKRST_ROOT_CLK_CTRL0, P4_CPU_CLK_DIV_DENOM_MASK,
                     P4_CPU_CLK_DIV_DENOM_SHIFT, 0);
        if (!p4_clk_commit())
            return 0;
        p4_set_field(P4_CLKRST_ROOT_CLK_CTRL1, P4_MEM_CLK_DIV_NUM_MASK,
                     P4_MEM_CLK_DIV_NUM_SHIFT, mem_div - 1);
        if (!p4_clk_commit())
            return 0;
        p4_set_field(P4_CLKRST_ROOT_CLK_CTRL1, P4_SYS_CLK_DIV_NUM_MASK,
                     P4_SYS_CLK_DIV_NUM_SHIFT, 0);
        if (!p4_clk_commit())
            return 0;
        p4_set_field(P4_CLKRST_ROOT_CLK_CTRL2, P4_APB_CLK_DIV_NUM_MASK,
                     P4_APB_CLK_DIV_NUM_SHIFT, apb_div - 1);
        if (!p4_clk_commit())
            return 0;
    }

    krnP4CPUClockRead(&now);
    return now.cpu_div == cpu_div && now.mem_div == mem_div
           && now.apb_div == apb_div;
}
