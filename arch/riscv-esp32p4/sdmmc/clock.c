/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#include <proto/exec.h>
#include "clock.h"
#include "clock_plan.h"
#include "registers.h"

#define P4HOST_CLOCK_TIMEOUT_US 250000UL
#define P4HOST_POLL_BOUND 1000000UL

static ULONG clock_read(IPTR address)
{
    return *(volatile ULONG *)address;
}

static void clock_write(IPTR address, ULONG value)
{
    *(volatile ULONG *)address = value;
    __asm__ volatile("fence iorw, iorw" ::: "memory");
}

static BOOL clock_owned(struct P4SDMMCHost *host)
{
    return host && host->lock.ss_Owner == FindTask(NULL);
}

static BOOL clock_wait_clear(struct P4SDMMCHost *host, ULONG offset,
                               ULONG mask)
{
    ULONG started = host->now_us();
    ULONG polls = P4HOST_POLL_BOUND;
    for (;;)
    {
        if (!p4host_budget_ok(host))
            return FALSE;
        if (!(clock_read(P4_SDMMC_BASE + offset) & mask))
            return TRUE;
        if (!--polls ||
            (ULONG)(host->now_us() - started) >= P4HOST_CLOCK_TIMEOUT_US)
            return FALSE;
    }
}

static BOOL clock_delay(struct P4SDMMCHost *host, ULONG usec)
{
    ULONG started = host->now_us();
    ULONG polls = P4HOST_POLL_BOUND;
    while ((ULONG)(host->now_us() - started) < usec)
        if (!--polls || !p4host_budget_ok(host))
            return FALSE;
    return p4host_budget_ok(host);
}

static BOOL clock_source(struct P4SDMMCHost *host, unsigned divider)
{
    ULONG value;
    if (divider < 1 || divider > 16 || !p4host_budget_ok(host))
        return FALSE;
    value = clock_read(P4_HP_SYS_CLKRST_BASE + P4_HP_SOC_CLK_CTRL1);
    clock_write(P4_HP_SYS_CLKRST_BASE + P4_HP_SOC_CLK_CTRL1,
                 value | P4_HP_SDMMC_CLK_EN);
    value = clock_read(P4_HP_SYS_CLKRST_BASE + P4_HP_REF_CLK_CTRL2);
    clock_write(P4_HP_SYS_CLKRST_BASE + P4_HP_REF_CLK_CTRL2,
                 value | P4_HP_REF_160M_CLK_EN);
    value = clock_read(P4_HP_SYS_CLKRST_BASE + P4_HP_PERI_CLK_CTRL01);
    value &= ~(P4_HP_SDIO_HS_MODE | P4_HP_SDIO_CLK_SRC);
    if (divider == 1)
        value |= P4_HP_SDIO_HS_MODE;
    clock_write(P4_HP_SYS_CLKRST_BASE + P4_HP_PERI_CLK_CTRL01,
                 value | P4_HP_SDIO_CLK_EN);
    value = clock_read(P4_HP_SYS_CLKRST_BASE + P4_HP_PERI_CLK_CTRL02);
    value &= ~P4_HP_SDIO_FIELDS_M;
    if (divider > 1)
        value |= ((divider - 1) << P4_HP_SDIO_EDGE_L_S) |
                 ((divider / 2 - 1) << P4_HP_SDIO_EDGE_H_S) |
                 ((divider - 1) << P4_HP_SDIO_EDGE_N_S);
    value |= (1UL << P4_HP_SDIO_DRV_EDGE_S) | P4_HP_SDIO_SLF_EN |
             P4_HP_SDIO_DRV_EN | P4_HP_SDIO_SAM_EN;
    clock_write(P4_HP_SYS_CLKRST_BASE + P4_HP_PERI_CLK_CTRL02,
                 value | P4_HP_SDIO_UPDATE);
    clock_write(P4_HP_SYS_CLKRST_BASE + P4_HP_PERI_CLK_CTRL02, value);
    if (!clock_delay(host, 10))
        return FALSE;
    host->host_div = divider;
    return TRUE;
}

static BOOL clock_update(struct P4SDMMCHost *host, unsigned slot)
{
    ULONG command = P4SD_CMD_UPDATE_CLK | P4SD_CMD_WAIT_DATA |
                    P4SD_CMD_USE_HOLD | P4SD_CMD_START |
                    ((ULONG)slot << P4SD_CMD_SLOT_S);
    if (!clock_wait_clear(host, P4SD_CMD, P4SD_CMD_START))
        return FALSE;
    clock_write(P4_SDMMC_BASE + P4SD_RINTSTS, P4SD_INT_CMD_DATA_MASK);
    clock_write(P4_SDMMC_BASE + P4SD_CMDARG, 0);
    clock_write(P4_SDMMC_BASE + P4SD_CMD, command);
    if (!clock_wait_clear(host, P4SD_CMD, P4SD_CMD_START))
        return FALSE;
    if (clock_read(P4_SDMMC_BASE + P4SD_RINTSTS) & P4SD_INT_HLE)
        return FALSE;
    clock_write(P4_SDMMC_BASE + P4SD_RINTSTS, P4SD_INT_CMD_DATA_MASK);
    return TRUE;
}

/* A failed transition cannot leave a peer believing its clock is valid.
 * Request both output clocks disabled; a stuck CMD can prevent latching
 * that request. Generation invalidation forces clients to refuse new IO.
 * No reset or successful recovery is implied by this fail-closed path. */
static void clock_fail(struct P4SDMMCHost *host)
{
    ULONG value = clock_read(P4_SDMMC_BASE + P4SD_CLKENA);
    clock_write(P4_SDMMC_BASE + P4SD_CLKENA, value & ~3UL);
    host->slot_clock[0] = host->slot_clock[1] = 0;
    host->host_div = 0;
    host->dma_faulted = TRUE;
    host->invalidate(host);
}

BOOL p4host_reset(struct P4SDMMCHost *host)
{
    ULONG value;
    if (!clock_owned(host) || !p4host_budget_ok(host))
        return FALSE;
    host->initialization_started = TRUE;
    host->invalidate(host);
    host->slot_clock[0] = host->slot_clock[1] = 0;
    host->host_div = 0;
    host->dma_faulted = TRUE;
    /* On a cold boot the peripheral bus may still be gated. Establish the
     * bus clock before accessing its registers, but do not yet change the
     * host-wide source/divider of any retained card-clock output. */
    value = clock_read(P4_HP_SYS_CLKRST_BASE + P4_HP_SOC_CLK_CTRL1);
    clock_write(P4_HP_SYS_CLKRST_BASE + P4_HP_SOC_CLK_CTRL1,
                 value | P4_HP_SDMMC_CLK_EN);
    value = clock_read(P4_SDMMC_BASE + P4SD_CLKENA);
    clock_write(P4_SDMMC_BASE + P4SD_CLKENA, value & ~3UL);
    if (!clock_source(host, 10))
    {
        clock_fail(host);
        return FALSE;
    }
    value = clock_read(P4_LP_CLKRST_BASE + P4_LP_SDMMC_RST_CTRL);
    clock_write(P4_LP_CLKRST_BASE + P4_LP_SDMMC_RST_CTRL,
                 value | P4_LP_SDMMC_RST_EN);
    clock_write(P4_LP_CLKRST_BASE + P4_LP_SDMMC_RST_CTRL,
                 value & ~P4_LP_SDMMC_RST_EN);
    if (!clock_delay(host, 10))
    {
        clock_fail(host);
        return FALSE;
    }
    /* Controller-wide programming belongs to the resource, not a slot.
     * reset is explicitly destructive to both generations. A joining slot
     * must reuse this initialized host rather than calling reset again. */
    clock_write(P4_SDMMC_BASE + P4SD_CTRL,
                 clock_read(P4_SDMMC_BASE + P4SD_CTRL) | P4SD_CTRL_RESET_ALL);
    if (!clock_wait_clear(host, P4SD_CTRL, P4SD_CTRL_RESET_ALL))
    {
        clock_fail(host);
        return FALSE;
    }
    clock_write(P4_SDMMC_BASE + P4SD_CTRL, 0);
    clock_write(P4_SDMMC_BASE + P4SD_BMOD, 0);
    clock_write(P4_SDMMC_BASE + P4SD_IDINTEN, 0);
    clock_write(P4_SDMMC_BASE + P4SD_IDSTS, 0xFFFFFFFFUL);
    clock_write(P4_SDMMC_BASE + P4SD_INTMASK, 0);
    clock_write(P4_SDMMC_BASE + P4SD_RINTSTS, P4SD_INT_CMD_DATA_MASK);
    clock_write(P4_SDMMC_BASE + P4SD_RST_N, 0);
    clock_write(P4_SDMMC_BASE + P4SD_CTYPE, 0);
    clock_write(P4_SDMMC_BASE + P4SD_TMOUT, 0xFFFFFFFFUL);
    clock_write(P4_SDMMC_BASE + P4SD_FIFOTH,
                 P4SD_FIFOTH_RX_WMARK | P4SD_FIFOTH_DMA_MSIZE_8);
    clock_write(P4_SDMMC_BASE + P4SD_CARDTHRCTL, 0);
    clock_write(P4_SDMMC_BASE + P4SD_CLK_EDGE_SEL, 0);
    if (!p4host_budget_ok(host))
    {
        clock_fail(host);
        return FALSE;
    }
    host->dma_data = NULL;
    host->dma_length = 0;
    host->dma_writing = FALSE;
    host->dma_faulted = FALSE;
    return TRUE;
}

BOOL p4host_set_clock(struct P4SDMMCHost *host, unsigned slot,
                       ULONG requested, BOOL low_power, ULONG *actual)
{
    struct p4sdmmc_clock_plan plan;
    ULONG value, bit;
    BOOL peer;
    if (!clock_owned(host) || !p4host_budget_ok(host) || slot > 1 || !actual ||
        host->dma_length || host->dma_faulted ||
        host->host_div < 1 || host->host_div > 16 ||
        (low_power != FALSE && low_power != TRUE))
        return FALSE;
    peer = host->slot_clock[slot ^ 1] != 0;
    if (!p4sdmmc_clock_plan(requested, host->host_div, peer, &plan))
        return FALSE;
    if (!clock_wait_clear(host, P4SD_CMD, P4SD_CMD_START) ||
        !clock_wait_clear(host, P4SD_STATUS, P4SD_STATUS_DATA_BUSY))
        goto failed;
    bit = (ULONG)1 << slot;
    value = clock_read(P4_SDMMC_BASE + P4SD_CLKENA);
    clock_write(P4_SDMMC_BASE + P4SD_CLKENA, value & ~bit);
    if (!clock_update(host, slot))
        goto failed;
    value = clock_read(P4_SDMMC_BASE + P4SD_CLKDIV);
    value = (value & ~(0xffUL << (slot * 8))) |
            ((ULONG)plan.card_div << (slot * 8));
    clock_write(P4_SDMMC_BASE + P4SD_CLKDIV, value);
    value = clock_read(P4_SDMMC_BASE + P4SD_CLKSRC);
    value = (value & ~(3UL << (slot * 2))) |
            ((ULONG)slot << (slot * 2));
    clock_write(P4_SDMMC_BASE + P4SD_CLKSRC, value);
    /* The source is host-wide, unlike CLKSRC's per-card divider mux.
     * Never rewrite it while an initialized peer may depend on it. */
    if (!peer && !clock_source(host, plan.host_div))
        goto failed;
    if (!clock_update(host, slot))
        goto failed;
    value = clock_read(P4_SDMMC_BASE + P4SD_CLKENA);
    value = (value | bit) & ~(bit << 16);
    if (low_power)
        value |= bit << 16;
    clock_write(P4_SDMMC_BASE + P4SD_CLKENA, value);
    if (!clock_update(host, slot))
        goto failed;
    if (!p4host_budget_ok(host))
        goto failed;
    host->slot_clock[slot] = plan.actual_hz;
    *actual = plan.actual_hz;
    return TRUE;
failed:
    clock_fail(host);
    return FALSE;
}
