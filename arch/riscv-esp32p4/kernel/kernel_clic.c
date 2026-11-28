/*
    Copyright (c) 2026, The AROS Development Team. All rights reserved.

    Desc: The core local interrupt controller.

    mtvec is left in direct mode, so every interrupt arrives at the one
    trap entry and the line number is read out of mcause rather than
    dispatched through a vector table in mtvt. That keeps one entry point
    to reason about; selective hardware vectoring is available per line
    later if a latency case ever needs it.
*/

#include <inttypes.h>

#include "hardware.h"
#include "kernel_intern.h"

static inline void clic_wr(uint32_t off, uint32_t val)
{
    *(volatile uint32_t *)(P4_CLIC_BASE + off) = val;
}

static inline uint32_t clic_rd(uint32_t off)
{
    return *(volatile uint32_t *)(P4_CLIC_BASE + off);
}

static inline void ctrl_wr(unsigned int line, uint32_t val)
{
    *(volatile uint32_t *)P4_CLIC_CTRL(line) = val;
}

static inline uint32_t ctrl_rd(unsigned int line)
{
    return *(volatile uint32_t *)P4_CLIC_CTRL(line);
}

/*
 * The level as the hardware wants it: the top NLBITS of an eight bit
 * field, with the bits below them set. Matches what the threshold is
 * compared against.
 */
static inline uint32_t clic_level(unsigned int level)
{
    unsigned int shift = 8 - P4_CLIC_NLBITS;

    return ((level << shift) | ((1U << shift) - 1)) & 0xFF;
}

void krnCLICInit(void)
{
    unsigned int line;
    uint32_t cfg;

    cfg = clic_rd(P4_CLIC_INT_CONFIG);
    cfg &= ~(P4_CLIC_NLBITS_M << P4_CLIC_NLBITS_S);
    cfg |= (P4_CLIC_NLBITS & P4_CLIC_NLBITS_M) << P4_CLIC_NLBITS_S;
    clic_wr(P4_CLIC_INT_CONFIG, cfg);

    /* Mask nothing: a line is silent because it is disabled, not because
       the threshold is holding it back. */
    clic_wr(P4_CLIC_INT_THRESH, 0);

    /* Start from nothing enabled and nothing pending */
    for (line = 0; line < P4_CLIC_LINES; line++)
        ctrl_wr(line, 0);
}

void krnCLICEnable(unsigned int line, int edge)
{
    uint32_t v;

    if (line >= P4_CLIC_LINES)
        return;

    v = clic_level(P4_CLIC_LEVEL_DEFAULT) << P4_CLIC_INT_CTL_S;
    v |= (edge ? P4_CLIC_INT_TRIG_EDGE : P4_CLIC_INT_TRIG_LEVEL)
            << P4_CLIC_INT_TRIG_S;
    v |= P4_CLIC_INT_IE;
    ctrl_wr(line, v);
}

void krnCLICDisable(unsigned int line)
{
    if (line < P4_CLIC_LINES)
        ctrl_wr(line, 0);
}

/* Raising a line by hand, for a bring-up check and later for IPIs */
void krnCLICPend(unsigned int line)
{
    if (line < P4_CLIC_LINES)
        ctrl_wr(line, ctrl_rd(line) | P4_CLIC_INT_IP);
}

void krnCLICClear(unsigned int line)
{
    if (line < P4_CLIC_LINES)
        ctrl_wr(line, ctrl_rd(line) & ~P4_CLIC_INT_IP);
}

int krnCLICPending(unsigned int line)
{
    if (line >= P4_CLIC_LINES)
        return 0;
    return (ctrl_rd(line) & P4_CLIC_INT_IP) ? 1 : 0;
}
