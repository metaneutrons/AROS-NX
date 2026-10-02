/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Synchronous, polling-only ESP32-P4 DesignWare MMC backend.  CMD18 uses
    bounded receive-only IDMAC; the backend deliberately contains no
    transmit-data path.
*/

#define DEBUG 0
#include <aros/debug.h>

#include <hardware/mmc.h>
#include <hardware/sdhc.h>
#include <proto/exec.h>
#include <proto/utility.h>

#include <string.h>

#include "sdcard_esp32p4_intern.h"
#include "sdcard_unit.h"
#include "timer.h"

static BOOL p4sd_reset_fifo(struct sdcard_Bus *bus);

static BOOL p4sd_fault_injected;
static ULONG p4sd_cmd12_confirmations;

#if P4_SDCARD_TRACE
/* The FIFO observations below are the measurement for the word-repeat fault.
   They are bounded, because a 128-sector cell would otherwise emit one line
   per command and per drain. */
static ULONG p4sd_fifo_trace_budget = 24;
#endif

/* ESP32-P4 uses a 64-byte IDMAC descriptor.  The SDMMC peripheral can DMA
   directly to PSRAM, but descriptors must be cache-line aligned and fully
   visible before DBADDR is armed.  The generic driver serializes requests,
   so one bounded static chain is sufficient and avoids adding an allocator or
   a write-capable DMA path. */
struct p4sd_dma_desc
{
    ULONG control;
    ULONG size;
    APTR buffer;
    struct p4sd_dma_desc *next;
    ULONG reserved[12];
};

#define P4SD_DESC_LAST                  (1UL << 2)
#define P4SD_DESC_FIRST                 (1UL << 3)
#define P4SD_DESC_CHAIN                 (1UL << 4)
#define P4SD_DESC_OWN                   (1UL << 31)

static struct p4sd_dma_desc p4sd_dma_desc[P4SD_DMA_DESC_COUNT]
    __attribute__((aligned(64)));

/* ESP-IDF moves every SDMMC data byte with the IDMAC and never reads the
   data FIFO from software.  The measurement of 2026-08-22 showed why: a CPU
   read of P4SD_BUFFIFO does not pop, so 128 reads returned the same word 128
   times while STATUS.FIFO_COUNT never dropped and TBBCNT never moved.  Every
   data read therefore goes through the IDMAC now, including the eight-byte
   CMD51 that card identification depends on.  Such a short destination is
   neither cache-line aligned nor a whole number of lines, so it lands here
   first and is copied afterwards, which is also how IDF handles a buffer its
   DMA cannot use directly.

   Sized for the largest transfer the backend accepts rather than for one
   sector, because the caller that actually needs it is a filesystem.  FAT's
   cache reads 32 sectors at a time into a buffer it allocated with AllocMem,
   which is 32-byte aligned on this target and so unusable as a DMA
   destination; with a 512-byte bounce that request was refused, and FAT
   retried it 134,101 times in one boot.  Splitting the request would work
   too, but it would mean issuing several commands where the card expects one,
   and the memory this costs is not scarce: the buffer lives in the module's
   .bss, which the loader places in the 32 MB external window, so it is 64 KiB
   of PSRAM and nothing of the 230 KB internal heap.  It is also already the
   proven DMA destination - every unaligned read since A1 has landed here. */
static UBYTE p4sd_bounce[P4SD_MAX_DATA_LEN] __attribute__((aligned(64)));

static inline ULONG p4sd_reg(ULONG offset);

#if P4_SDCARD_TRACE
#define P4SD_TRACE_SAMPLES 12
struct p4sd_trace_sample
{
    const char *event;
    ULONG rintsts;
    ULONG mintsts;
    ULONG idsts;
    ULONG status;
    ULONG tcbcnt;
    ULONG tbbcnt;
    ULONG dscaddr;
    ULONG bufaddr;
    ULONG desc0;
    ULONG desc1;
    ULONG block0;
    ULONG block1;
};

struct p4sd_trace
{
    BOOL active;
    ULONG count;
    UBYTE *data;
    struct p4sd_trace_sample sample[P4SD_TRACE_SAMPLES];
};

static struct p4sd_trace p4sd_trace;

static void p4sd_trace_take(struct p4sd_trace *trace, const char *event)
{
    struct p4sd_trace_sample *sample;

    if (!trace->active || trace->count == P4SD_TRACE_SAMPLES)
        return;

    sample = &trace->sample[trace->count++];
    sample->event = event;
    sample->rintsts = p4sd_reg(P4SD_RINTSTS);
    sample->mintsts = p4sd_reg(P4SD_MINTSTS);
    sample->idsts = p4sd_reg(P4SD_IDSTS);
    sample->status = p4sd_reg(P4SD_STATUS);
    sample->tcbcnt = p4sd_reg(P4SD_TCBCNT);
    sample->tbbcnt = p4sd_reg(P4SD_TBBCNT);
    sample->dscaddr = p4sd_reg(P4SD_DSCADDR);
    sample->bufaddr = p4sd_reg(P4SD_BUFADDR);
    sample->desc0 = p4sd_dma_desc[0].control;
    sample->desc1 = p4sd_dma_desc[1].control;
    sample->block0 = trace->data ? *(ULONG *)(IPTR)trace->data : 0;
    sample->block1 = trace->data ?
        *(ULONG *)(IPTR)(trace->data + 512) : 0;
}

static void p4sd_trace_dump(struct p4sd_trace *trace,
                            struct sdcard_Bus *bus)
{
    ULONG index;

    if (!trace->active)
        return;

    bug("[P4SD%02u] CMD18 LBA %u controller trace (%u samples)\n",
        bus->sdcb_BusNum, P4SD_TRACE_LBA, trace->count);
    for (index = 0; index < trace->count; ++index)
    {
        const struct p4sd_trace_sample *sample = &trace->sample[index];

        bug("[P4SD%02u] trace %-7s ri=%08x mi=%08x id=%08x st=%08x ciu=%u biu=%u dsc=%08x buf=%08x d0=%08x d1=%08x b0=%08x b1=%08x\n",
            bus->sdcb_BusNum, sample->event, sample->rintsts,
            sample->mintsts, sample->idsts, sample->status,
            sample->tcbcnt, sample->tbbcnt, sample->dscaddr,
            sample->bufaddr, sample->desc0, sample->desc1,
            sample->block0, sample->block1);
    }
    trace->active = FALSE;
}
#endif

/* CacheClearE() on this port is deliberately only an ordering fence.  The
   P4 SDMMC IDMAC is non-coherent with PSRAM, so use the documented ROM range
   operations for ownership handoff.  Both ROM addresses are invariant for
   the base and ECO5 P4 ROMs. */
#define P4_ROM_CACHE_INVALIDATE_ADDR    0x4FC003E4UL
#define P4_ROM_CACHE_WRITEBACK_ADDR     0x4FC003F4UL
#define P4_CACHE_MAP_L1_DCACHE          (1UL << 4)
#define P4_CACHE_MAP_L2                 (1UL << 5)

typedef int (*p4sd_cache_range_t)(ULONG map, ULONG address, ULONG length);

static void p4sd_cache_writeback(APTR address, ULONG length)
{
    p4sd_cache_range_t writeback =
        (p4sd_cache_range_t)P4_ROM_CACHE_WRITEBACK_ADDR;

    if (length)
        writeback(P4_CACHE_MAP_L1_DCACHE | P4_CACHE_MAP_L2,
                  (ULONG)(IPTR)address, length);
    __asm__ volatile("fence rw, rw" ::: "memory");
}

static void p4sd_cache_invalidate(APTR address, ULONG length)
{
    p4sd_cache_range_t invalidate =
        (p4sd_cache_range_t)P4_ROM_CACHE_INVALIDATE_ADDR;

    if (length)
        invalidate(P4_CACHE_MAP_L1_DCACHE | P4_CACHE_MAP_L2,
                   (ULONG)(IPTR)address, length);
    __asm__ volatile("fence rw, rw" ::: "memory");
}

static inline ULONG p4sd_read(IPTR address)
{
    return *(volatile ULONG *)address;
}

static inline void p4sd_write(IPTR address, ULONG value)
{
    *(volatile ULONG *)address = value;
    __asm__ volatile("fence iorw, iorw" ::: "memory");
}

static inline ULONG p4sd_reg(ULONG offset)
{
    return p4sd_read(P4_SDMMC_BASE + offset);
}

static inline void p4sd_set(ULONG offset, ULONG value)
{
    p4sd_write(P4_SDMMC_BASE + offset, value);
}

static BOOL p4sd_timed_out(ULONG started, ULONG limit)
{
    return (ULONG)(sdcard_CurrentTime() - started) >= limit;
}

static BOOL p4sd_wait_clear(ULONG offset, ULONG mask, ULONG timeout)
{
    ULONG started = sdcard_CurrentTime();

    while (p4sd_reg(offset) & mask)
        if (p4sd_timed_out(started, timeout))
            return FALSE;

    return TRUE;
}

static void p4sd_iomux(unsigned int gpio, unsigned int function,
                       BOOL input, unsigned int drive)
{
    IPTR address = P4_IOMUX_BASE + P4_IOMUX_GPIO39 +
                   (gpio - P4_SD_D0_GPIO) * 4;
    ULONG value = p4sd_read(address);

    value &= ~(P4_IOMUX_FUN_PD | P4_IOMUX_FUN_PU |
               P4_IOMUX_FUN_DRV_M | P4_IOMUX_MCU_SEL_M);
    value |= (drive << P4_IOMUX_FUN_DRV_S) & P4_IOMUX_FUN_DRV_M;
    value |= (function << P4_IOMUX_MCU_SEL_S) & P4_IOMUX_MCU_SEL_M;
    if (input)
        value |= P4_IOMUX_FUN_IE;
    else
        value &= ~P4_IOMUX_FUN_IE;
    p4sd_write(address, value);
}

static void p4sd_gpio_output(unsigned int gpio, BOOL high)
{
    ULONG bit = 1UL << (gpio - 32);

    /* Establish the level before connecting the output driver. */
    p4sd_write(P4_GPIO_BASE + (high ? P4_GPIO_OUT1_W1TS
                                   : P4_GPIO_OUT1_W1TC), bit);
    p4sd_iomux(gpio, P4_IOMUX_FUNC_GPIO, FALSE, 1);
    p4sd_write(P4_GPIO_BASE + P4_GPIO_ENABLE1_W1TS, bit);
}

static BOOL p4sd_card_present(void)
{
    ULONG bit = 1UL << (P4_BOARD_SD_DETECT_GPIO - 32);

    p4sd_iomux(P4_BOARD_SD_DETECT_GPIO, P4_IOMUX_FUNC_GPIO, TRUE, 0);
    p4sd_write(P4_GPIO_BASE + P4_GPIO_ENABLE1_W1TC, bit);
    return (p4sd_read(P4_GPIO_BASE + P4_GPIO_IN1) & bit) == 0;
}

static void p4sd_configure_pins(unsigned int drive)
{
    unsigned int gpio;

    /* Native slot 0 is GPIO39..44.  GPIO45/46 must remain CD/power. */
    for (gpio = P4_SD_D0_GPIO; gpio <= P4_SD_CMD_GPIO; ++gpio)
        p4sd_iomux(gpio, P4_IOMUX_FUNC_SDMMC, TRUE, drive);
}

static void p4sd_host_clock_enable(unsigned int divider)
{
    ULONG value;

    value = p4sd_read(P4_HP_SYS_CLKRST_BASE + P4_HP_SOC_CLK_CTRL1);
    p4sd_write(P4_HP_SYS_CLKRST_BASE + P4_HP_SOC_CLK_CTRL1,
               value | P4_HP_SDMMC_CLK_EN);

    value = p4sd_read(P4_HP_SYS_CLKRST_BASE + P4_HP_REF_CLK_CTRL2);
    p4sd_write(P4_HP_SYS_CLKRST_BASE + P4_HP_REF_CLK_CTRL2,
               value | P4_HP_REF_160M_CLK_EN);

    value = p4sd_read(P4_HP_SYS_CLKRST_BASE + P4_HP_PERI_CLK_CTRL01);
    value &= ~(P4_HP_SDIO_HS_MODE | P4_HP_SDIO_CLK_SRC);
    if (divider == 1)
        value |= P4_HP_SDIO_HS_MODE;
    value |= P4_HP_SDIO_CLK_EN;
    p4sd_write(P4_HP_SYS_CLKRST_BASE + P4_HP_PERI_CLK_CTRL01, value);

    value = p4sd_read(P4_HP_SYS_CLKRST_BASE + P4_HP_PERI_CLK_CTRL02);
    value &= ~P4_HP_SDIO_FIELDS_M;
    if (divider > 1)
    {
        value |= ((divider - 1) << P4_HP_SDIO_EDGE_L_S) |
                 ((divider / 2 - 1) << P4_HP_SDIO_EDGE_H_S) |
                 ((divider - 1) << P4_HP_SDIO_EDGE_N_S);
    }
    value |= (1UL << P4_HP_SDIO_DRV_EDGE_S) |
             P4_HP_SDIO_SLF_EN | P4_HP_SDIO_DRV_EN |
             P4_HP_SDIO_SAM_EN;
    p4sd_write(P4_HP_SYS_CLKRST_BASE + P4_HP_PERI_CLK_CTRL02,
               value | P4_HP_SDIO_UPDATE);
    p4sd_write(P4_HP_SYS_CLKRST_BASE + P4_HP_PERI_CLK_CTRL02, value);
    sdcard_Udelay(10);
}

static void p4sd_module_reset(void)
{
    ULONG value = p4sd_read(P4_LP_CLKRST_BASE + P4_LP_SDMMC_RST_CTRL);

    p4sd_write(P4_LP_CLKRST_BASE + P4_LP_SDMMC_RST_CTRL,
               value | P4_LP_SDMMC_RST_EN);
    p4sd_write(P4_LP_CLKRST_BASE + P4_LP_SDMMC_RST_CTRL,
               value & ~P4_LP_SDMMC_RST_EN);
    sdcard_Udelay(10);
}

static BOOL p4sd_clock_update(void)
{
    ULONG command = P4SD_CMD_UPDATE_CLK | P4SD_CMD_WAIT_DATA |
                    P4SD_CMD_USE_HOLD | P4SD_CMD_START;

    if (!p4sd_wait_clear(P4SD_CMD, P4SD_CMD_START, P4SD_CMD_TIMEOUT_US))
        return FALSE;
    p4sd_set(P4SD_RINTSTS, 0xFFFFFFFFUL);
    p4sd_set(P4SD_CMDARG, 0);
    p4sd_set(P4SD_CMD, command);
    if (!p4sd_wait_clear(P4SD_CMD, P4SD_CMD_START, P4SD_CMD_TIMEOUT_US))
        return FALSE;
    if (p4sd_reg(P4SD_RINTSTS) & P4SD_INT_HLE)
        return FALSE;
    p4sd_set(P4SD_RINTSTS, 0xFFFFFFFFUL);
    return TRUE;
}

static BOOL p4sd_program_clock(ULONG requested, struct sdcard_Bus *bus)
{
    struct p4sd_private *priv = P4SD_PRIV(bus);
    ULONG host_div, card_div, actual;

    if (requested > P4SD_CLOCK_MAX_HZ)
        requested = P4SD_CLOCK_MAX_HZ;
    if (requested < P4SD_CLOCK_MIN_HZ)
        requested = P4SD_CLOCK_MIN_HZ;

    if (requested == P4SD_CLOCK_MIN_HZ)
    {
        /* IDF's conservative identification setting: 160M/10/(2*20). */
        host_div = 10;
        card_div = 20;
    }
    else
    {
        host_div = P4SD_CLOCK_SOURCE_HZ / requested;
        if (host_div < 1)
            host_div = 1;
        if (host_div <= 15)
        {
            if ((P4SD_CLOCK_SOURCE_HZ / host_div) > requested)
                ++host_div;
            card_div = 0;
        }
        else
        {
            host_div = 2;
            card_div = (P4SD_CLOCK_SOURCE_HZ / host_div) /
                       (2 * requested);
            if (card_div < 1)
                card_div = 1;
            if ((P4SD_CLOCK_SOURCE_HZ / host_div / (2 * card_div)) >
                requested)
                ++card_div;
            if (card_div > 255)
                card_div = 255;
        }
    }

    p4sd_set(P4SD_CLKENA, 0);
    if (!p4sd_clock_update())
        return FALSE;

    p4sd_set(P4SD_CLKDIV, card_div & 0xFF);
    p4sd_set(P4SD_CLKSRC, 0);
    p4sd_host_clock_enable(host_div);
    if (!p4sd_clock_update())
        return FALSE;

    p4sd_set(P4SD_CLKENA, 1UL | (1UL << 16));
    if (!p4sd_clock_update())
        return FALSE;

    actual = P4SD_CLOCK_SOURCE_HZ / host_div;
    if (card_div != 0)
        actual /= 2 * card_div;
    priv->clock_hz = actual;
    return TRUE;
}

static BOOL p4sd_host_init(struct sdcard_Bus *bus)
{
    struct p4sd_private *priv = P4SD_PRIV(bus);

    p4sd_host_clock_enable(10);
    p4sd_module_reset();

    p4sd_set(P4SD_CTRL, p4sd_reg(P4SD_CTRL) | P4SD_CTRL_RESET_ALL);
    if (!p4sd_wait_clear(P4SD_CTRL, P4SD_CTRL_RESET_ALL,
                         P4SD_CMD_TIMEOUT_US))
        return FALSE;

    p4sd_set(P4SD_CTRL, 0);       /* no global IRQ, DMA or IDMAC */
    p4sd_set(P4SD_BMOD, 0);
    p4sd_set(P4SD_IDINTEN, 0);
    p4sd_set(P4SD_IDSTS, 0xFFFFFFFFUL);
    p4sd_set(P4SD_INTMASK, 0);
    p4sd_set(P4SD_RINTSTS, 0xFFFFFFFFUL);
    p4sd_set(P4SD_RST_N, 1);
    p4sd_set(P4SD_CTYPE, 0);      /* slot 0, one data bit, never eight */
    p4sd_set(P4SD_TMOUT, 0xFFFFFFFFUL);
    /* With a 32-bit FIFO port, an RX watermark of 127 releases exactly one
       512-byte sector at a time.  Keep that safe PIO threshold and select an
       eight-beat IDMAC burst; BMOD.PBL mirrors this FIFOTH field on P4. */
    p4sd_set(P4SD_FIFOTH, P4SD_FIFOTH_RX_WMARK |
             P4SD_FIFOTH_DMA_MSIZE_8);
    p4sd_set(P4SD_CARDTHRCTL, 0);
    p4sd_set(P4SD_CLK_EDGE_SEL, 0);

    priv->host_ready = TRUE;
    if (!p4sd_program_clock(P4SD_CLOCK_MIN_HZ, bus))
    {
        priv->host_ready = FALSE;
        return FALSE;
    }
    return TRUE;
}

/* Generic SDHCI SetClock is linked into sdcard.device even though this bus
   never calls it.  Supply its architecture hook so that object can link. */
ULONG FNAME_SDCBUS(GetClockDiv)(ULONG speed, struct sdcard_Bus *bus)
{
    (void)speed;
    (void)bus;
    return 0;
}

void FNAME_P4SDBUS(SoftReset)(UBYTE mask, struct sdcard_Bus *bus)
{
    struct p4sd_private *priv = P4SD_PRIV(bus);

    /* The generic SDHCI reset bits do not map one-to-one onto DesignWare.
       Never turn a CMD/DATA recovery into an unannounced full reset: the
       latter loses clocks, width and card selection. */
    if (mask & SDHCI_RESET_ALL)
    {
        bug("[P4SD%02u] full reset requires card re-enumeration\n",
            bus->sdcb_BusNum);
        priv->host_ready = FALSE;

        p4sd_module_reset();
        if (!p4sd_host_init(bus))
        {
            bug("[P4SD%02u] full-reset host initialization failed\n",
                bus->sdcb_BusNum);
            return;
        }

        /* p4sd_host_init sets host_ready while programming the controller,
           but no unit/card state is valid until a new enumeration. */
        priv->host_ready = FALSE;
        return;
    }

    if (!(mask & (SDHCI_RESET_CMD | SDHCI_RESET_DATA)))
        return;

    p4sd_set(P4SD_RINTSTS, 0xFFFFFFFFUL);
    if (!p4sd_reset_fifo(bus))
    {
        priv->host_ready = FALSE;
        return;
    }
}

void FNAME_P4SDBUS(SetClock)(ULONG speed, struct sdcard_Bus *bus)
{
    if (!P4SD_PRIV(bus)->host_ready)
        return;
    if (!p4sd_program_clock(speed, bus))
        bug("[P4SD%02u] failed to change card clock\n", bus->sdcb_BusNum);
}

void FNAME_P4SDBUS(SetPowerLevel)(ULONG levels, BOOL lowest,
                                  struct sdcard_Bus *bus)
{
    struct p4sd_private *priv = P4SD_PRIV(bus);
    ULONG control;

    (void)levels;
    (void)lowest;
    if (priv->powered)
        return;

    /* D1001 board sequence: external switch low, LDO4 at the 3.3-V
       bypass, 100-ms settling delay, then external switch high. */
    p4sd_gpio_output(P4_BOARD_SD_POWER_GPIO, FALSE);

    control = p4sd_read(P4_PMU_BASE + P4_PMU_LDO4_CTRL);
    control &= ~(P4_PMU_LDO_XPD | P4_PMU_LDO_TIEH_SEL_M);
    control |= P4_PMU_LDO_FORCE_SW | P4_PMU_LDO_3V3;
    p4sd_write(P4_PMU_BASE + P4_PMU_LDO4_CTRL, control);
    p4sd_write(P4_PMU_BASE + P4_PMU_LDO4_ANA,
               p4sd_read(P4_PMU_BASE + P4_PMU_LDO4_ANA) |
               P4_PMU_LDO_EN_VDET);
    p4sd_write(P4_PMU_BASE + P4_PMU_LDO4_CTRL,
               control | P4_PMU_LDO_XPD);

    sdcard_Udelay(P4SD_POWER_DELAY_US);
    p4sd_gpio_output(P4_BOARD_SD_POWER_GPIO, TRUE);
    sdcard_Udelay(P4SD_POWER_DELAY_US);
    priv->powered = TRUE;
}

void FNAME_P4SDBUS(SetBusWidth)(UBYTE width, struct sdcard_Bus *bus)
{
    ULONG ctype = p4sd_reg(P4SD_CTYPE);

    /* GPIO45/46 occupy the native D4/D5 pins: eight-bit mode is forbidden. */
    ctype &= ~(P4SD_CTYPE_4BIT_SLOT0 | P4SD_CTYPE_8BIT_SLOT0);
    if (width == 4)
        ctype |= P4SD_CTYPE_4BIT_SLOT0;
    else if (width != 1)
    {
        bug("[P4SD%02u] refusing unsupported %u-bit bus width\n",
            bus->sdcb_BusNum, width);
        return;
    }
    p4sd_set(P4SD_CTYPE, ctype);
}

static void p4sd_store_fifo_word(UBYTE *data, ULONG length, ULONG *done,
                                 ULONG word)
{
    unsigned int byte;

    for (byte = 0; byte < 4 && *done < length; ++byte)
    {
        data[*done] = (UBYTE)(word >> (byte * 8));
        ++*done;
    }
}

static void p4sd_drain_read_fifo(UBYTE *data, ULONG length, ULONG *done)
{
    ULONG status = p4sd_reg(P4SD_STATUS);
    ULONG available = (status >> P4SD_STATUS_FIFO_COUNT_S) &
                       P4SD_STATUS_FIFO_COUNT_M;

#if P4_SDCARD_TRACE
    /* The reported fill level is the value under suspicion: a read side that
       believes a nearly empty FIFO is full pops the same word repeatedly.
       Record what it claimed against how much the card had actually
       delivered, which is what TCBCNT counts. */
    if (p4sd_fifo_trace_budget != 0)
    {
        --p4sd_fifo_trace_budget;
        bug("[P4SD] drain: claims %u words, empty=%u full=%u, ciu=%u "
            "biu=%u, copied %u of %u\n", (unsigned)available,
            (status & P4SD_STATUS_FIFO_EMPTY) ? 1u : 0u,
            (status & P4SD_STATUS_FIFO_FULL) ? 1u : 0u,
            (unsigned)p4sd_reg(P4SD_TCBCNT), (unsigned)p4sd_reg(P4SD_TBBCNT),
            (unsigned)*done, (unsigned)length);
    }
#endif

    while (available-- != 0 && *done < length)
        p4sd_store_fifo_word(data, length, done,
                             p4sd_reg(P4SD_BUFFIFO));
}

static BOOL p4sd_reset_fifo(struct sdcard_Bus *bus)
{
#if P4_SDCARD_TRACE
    ULONG status;
#endif

    p4sd_set(P4SD_CTRL, p4sd_reg(P4SD_CTRL) | P4SD_CTRL_FIFO_RESET);
    if (!p4sd_wait_clear(P4SD_CTRL, P4SD_CTRL_FIFO_RESET,
                         P4SD_CMD_TIMEOUT_US))
    {
        bug("[P4SD%02u] FIFO reset timed out\n", bus->sdcb_BusNum);
        return FALSE;
    }

    /* The cleared CTRL bit is not the end of the reset.  See
       P4SD_FIFO_RESET_SETTLE_US for why the pointers need longer, and what
       reading them too early produced. */
    sdcard_Udelay(P4SD_FIFO_RESET_SETTLE_US);

#if P4_SDCARD_TRACE
    /* Observe rather than enforce.  The caller in p4sd_dma_complete_read()
       resets while an open-ended CMD18 may still be streaming, so an
       immediately refilling FIFO is legitimate there and must not fail. */
    status = p4sd_reg(P4SD_STATUS);
    if (p4sd_fifo_trace_budget != 0)
    {
        --p4sd_fifo_trace_budget;
        bug("[P4SD%02u] fifo reset settled: status=%08x count=%u empty=%u "
            "full=%u\n", bus->sdcb_BusNum, status,
            (unsigned)((status >> P4SD_STATUS_FIFO_COUNT_S) &
                       P4SD_STATUS_FIFO_COUNT_M),
            (status & P4SD_STATUS_FIFO_EMPTY) ? 1u : 0u,
            (status & P4SD_STATUS_FIFO_FULL) ? 1u : 0u);
    }
#endif
    return TRUE;
}

static BOOL p4sd_reset_dma(struct sdcard_Bus *bus)
{
    p4sd_set(P4SD_BMOD, p4sd_reg(P4SD_BMOD) | P4SD_BMOD_SWR);
    if (!p4sd_wait_clear(P4SD_BMOD, P4SD_BMOD_SWR,
                         P4SD_CMD_TIMEOUT_US))
    {
        bug("[P4SD%02u] IDMAC reset timed out\n", bus->sdcb_BusNum);
        return FALSE;
    }

    p4sd_set(P4SD_CTRL, p4sd_reg(P4SD_CTRL) | P4SD_CTRL_DMA_RESET);
    if (!p4sd_wait_clear(P4SD_CTRL, P4SD_CTRL_DMA_RESET,
                         P4SD_CMD_TIMEOUT_US))
    {
        bug("[P4SD%02u] DMA interface reset timed out\n", bus->sdcb_BusNum);
        return FALSE;
    }

    p4sd_set(P4SD_IDSTS, 0xFFFFFFFFUL);
    p4sd_set(P4SD_CTRL, p4sd_reg(P4SD_CTRL) &
             ~(P4SD_CTRL_DMA_ENABLE | P4SD_CTRL_USE_INTERNAL_DMA));
    return TRUE;
}

static BOOL p4sd_dma_prepare_read(UBYTE *data, ULONG length,
                                  struct sdcard_Bus *bus)
{
    ULONG index, offset = 0;
    ULONG descriptors = (length + P4SD_DMA_DESC_BYTES - 1) /
                        P4SD_DMA_DESC_BYTES;

    if (descriptors == 0 || descriptors > P4SD_DMA_DESC_COUNT)
        return FALSE;

    memset(p4sd_dma_desc, 0, sizeof(p4sd_dma_desc));
    for (index = 0; index < descriptors; ++index)
    {
        ULONG bytes = length - offset;
        struct p4sd_dma_desc *desc = &p4sd_dma_desc[index];

        if (bytes > P4SD_DMA_DESC_BYTES)
            bytes = P4SD_DMA_DESC_BYTES;
        desc->control = P4SD_DESC_CHAIN | P4SD_DESC_OWN;
        if (index == 0)
            desc->control |= P4SD_DESC_FIRST;
        if (index + 1 == descriptors)
            desc->control |= P4SD_DESC_LAST;
        desc->size = bytes;
        desc->buffer = data + offset;
        desc->next = (index + 1 == descriptors) ? NULL :
                     &p4sd_dma_desc[index + 1];
        offset += bytes;
    }

    /* P4 SDMMC explicitly supports PSRAM DMA.  Make descriptor ownership and
       the complete destination range visible before DBADDR is armed.  The
       ROM range operations work on whole cache lines, so round up: the
       caller guarantees either a line-aligned destination of a whole number
       of lines, or the bounce buffer, which has room to spare. */
    p4sd_cache_writeback(p4sd_dma_desc, sizeof(p4sd_dma_desc));
    p4sd_cache_writeback(data, (length + 63UL) & ~63UL);

    if (!p4sd_reset_dma(bus))
        return FALSE;
    p4sd_set(P4SD_DBADDR, (ULONG)(IPTR)p4sd_dma_desc);
    p4sd_set(P4SD_IDINTEN, 0);
    /* The P4 needs both halves of the DesignWare DMA gate: the controller
       DMA interface/internal-DMAC bits and BMOD's enable/fixed-burst bits. */
    p4sd_set(P4SD_CTRL, p4sd_reg(P4SD_CTRL) | P4SD_CTRL_DMA_ENABLE |
             P4SD_CTRL_USE_INTERNAL_DMA);
    p4sd_set(P4SD_BMOD, P4SD_BMOD_DE | P4SD_BMOD_FB | P4SD_BMOD_PBL_8);
    p4sd_set(P4SD_PLDMND, 1);
    return TRUE;
}

static BOOL p4sd_dma_complete_read(UBYTE *data, ULONG length,
                                   struct sdcard_Bus *bus)
{
    ULONG index;

    /* IDMAC owns and clears descriptor words.  Do not inspect a stale cached
       OWN bit after DTO. */
    p4sd_cache_invalidate(p4sd_dma_desc, sizeof(p4sd_dma_desc));
    if (p4sd_reg(P4SD_IDSTS) & P4SD_IDSTS_ERRORS)
        return FALSE;
    for (index = 0; index < P4SD_DMA_DESC_COUNT; ++index)
    {
        if (p4sd_dma_desc[index].control & P4SD_DESC_OWN)
        {
            bug("[P4SD%02u] IDMAC retained descriptor %u\n",
                bus->sdcb_BusNum, index);
            return FALSE;
        }
        if (p4sd_dma_desc[index].control & P4SD_DESC_LAST)
            break;
    }
    if (index == P4SD_DMA_DESC_COUNT)
        return FALSE;

    p4sd_set(P4SD_BMOD, 0);
    /* Leave the controller out of internal-DMA mode between transfers.
       p4sd_dma_prepare_read() selects it and nothing used to clear it, so
       after the first CMD18 every later command ran with CTRL.dma_enable and
       CTRL.use_internal_dma still set. */
    p4sd_set(P4SD_CTRL, p4sd_reg(P4SD_CTRL) &
             ~(P4SD_CTRL_DMA_ENABLE | P4SD_CTRL_USE_INTERNAL_DMA));
    /* The generic owner next sends physical CMD12.  Discard a prefetched
       FIFO tail before that stop command, after all requested bytes reached
       completed descriptors. */
    if (!p4sd_reset_fifo(bus))
        return FALSE;
    p4sd_cache_invalidate(data, (length + 63UL) & ~63UL);
    return TRUE;
}

static BOOL p4sd_dma_read_ready(ULONG length)
{
    ULONG descriptors = (length + P4SD_DMA_DESC_BYTES - 1) /
                        P4SD_DMA_DESC_BYTES;

    if (descriptors == 0 || descriptors > P4SD_DMA_DESC_COUNT)
        return FALSE;
    p4sd_cache_invalidate(p4sd_dma_desc,
                          descriptors * sizeof(p4sd_dma_desc[0]));
    return !(p4sd_dma_desc[descriptors - 1].control & P4SD_DESC_OWN);
}

static void p4sd_dma_dump(ULONG length, struct sdcard_Bus *bus)
{
    ULONG index;

    (void)length;
    p4sd_cache_invalidate(p4sd_dma_desc, sizeof(p4sd_dma_desc));
    for (index = 0; index < P4SD_DMA_DESC_COUNT; ++index)
        bug("[P4SD%02u] IDMAC desc %u ctrl=%08x size=%08x buf=%p next=%p\n",
            bus->sdcb_BusNum, index, p4sd_dma_desc[index].control,
            p4sd_dma_desc[index].size, p4sd_dma_desc[index].buffer,
            p4sd_dma_desc[index].next);
}

static ULONG p4sd_card_rca(struct sdcard_Bus *bus)
{
    if (bus->sdcb_BusUnits == NULL || bus->sdcb_BusUnits->sdcbu_Units == NULL)
        return 0;

    return bus->sdcb_BusUnits->sdcbu_Units[0].sdcu_CardRCA;
}

/* CMD12 is only a successful recovery when the card subsequently reports
   TRAN and ready-for-data.  The same check protects the successful generic
   stop path and the backend's synchronous error recovery. */
static BOOL p4sd_confirm_tran(struct sdcard_Bus *bus)
{
    ULONG rca = p4sd_card_rca(bus);
    struct TagItem status_tags[] =
    {
        {SDCARD_TAG_CMD,         MMC_CMD_SEND_STATUS},
        {SDCARD_TAG_ARG,         rca << 16},
        {SDCARD_TAG_RSPTYPE,     MMC_RSP_R1},
        {SDCARD_TAG_RSP,         0},
        {TAG_DONE,               0}
    };
    ULONG status;

    if (rca == 0)
    {
        bug("[P4SD%02u] cannot confirm TRAN without an RCA\n",
            bus->sdcb_BusNum);
        return FALSE;
    }

    if (FNAME_P4SDBUS(SendCmd)(status_tags, bus) == (ULONG)-1 ||
        FNAME_P4SDBUS(WaitCmd)(SDHCI_PS_CMD_INHIBIT |
                                SDHCI_PS_DATA_INHIBIT,
                                P4SD_CMD_TIMEOUT_US, bus) == (ULONG)-1)
        return FALSE;

    status = status_tags[3].ti_Data;
    if ((status & MMC_STATUS_STATE_MASK) != MMC_STATUS_STATE_TRAN ||
        !(status & MMC_STATUS_RDY_FOR_DATA))
    {
        bug("[P4SD%02u] CMD13 state is not TRAN/ready: %08x\n",
            bus->sdcb_BusNum, status);
        return FALSE;
    }

    return TRUE;
}

static BOOL p4sd_recover_multiblock(struct sdcard_Bus *bus)
{
    struct p4sd_private *priv = P4SD_PRIV(bus);
    struct TagItem stop_tags[] =
    {
        {SDCARD_TAG_CMD,         MMC_CMD_STOP_TRANSMISSION},
        {SDCARD_TAG_ARG,         0},
        {SDCARD_TAG_RSPTYPE,     MMC_RSP_R1b},
        {SDCARD_TAG_RSP,         0},
        {TAG_DONE,               0}
    };
    BOOL stopped;

    /* A starvation/CRC failure can leave IDMAC/FIFO state and the DesignWare
       data machine active.  Drop the controller-side state before asking the
       card to stop; otherwise CMD12 can never complete on the P4. */
    p4sd_set(P4SD_RINTSTS, 0xFFFFFFFFUL);
    if (!p4sd_reset_dma(bus) || !p4sd_reset_fifo(bus))
        goto failed;

    /* SendCmd(CMD12) uses the controller STOP/ABORT encoding and confirms
       TRAN through CMD13 before it reports success. */
    stopped = FNAME_P4SDBUS(SendCmd)(stop_tags, bus) != (ULONG)-1 &&
              FNAME_P4SDBUS(WaitCmd)(SDHCI_PS_CMD_INHIBIT |
                                      SDHCI_PS_DATA_INHIBIT,
                                      P4SD_CMD_TIMEOUT_US, bus) != (ULONG)-1;

    if (stopped && p4sd_reset_dma(bus) && p4sd_reset_fifo(bus))
    {
        bug("[P4SD%02u] CMD18 recovery completed with CMD12/CMD13\n",
            bus->sdcb_BusNum);
        return TRUE;
    }

failed:
    /* Do not issue another request after an unconfirmed stop.  A full reset
       would require controller reprogramming and card re-enumeration, so the
       conservative recovery is to leave this bus unavailable until that is
       explicitly done by a later cold-start path. */
    bug("[P4SD%02u] multi-block recovery failed; disabling host\n",
        bus->sdcb_BusNum);
#if P4_SDCARD_TRACE
    p4sd_trace_take(&p4sd_trace, "recover");
    p4sd_trace_dump(&p4sd_trace, bus);
#endif
    priv->host_ready = FALSE;
    return FALSE;
}

static void p4sd_copy_response(struct TagItem *response, ULONG rsp_type)
{
    if (response == NULL || !(rsp_type & MMC_RSP_PRESENT))
        return;

    if (rsp_type & MMC_RSP_136)
    {
        ULONG *rsp = (ULONG *)(IPTR)response->ti_Data;

        if (rsp != NULL)
        {
            /* Generic Rsp136Unpack expects the most significant word first. */
            rsp[0] = p4sd_reg(P4SD_RESP3);
            rsp[1] = p4sd_reg(P4SD_RESP2);
            rsp[2] = p4sd_reg(P4SD_RESP1);
            rsp[3] = p4sd_reg(P4SD_RESP0);
        }
    }
    else
        response->ti_Data = p4sd_reg(P4SD_RESP0);
}

static BOOL p4sd_media_mutating_command(ULONG command)
{
    /* SD/MMC commands which can alter user media or its write-protection
       metadata.  Several are absent from AROS' current public mmc.h, so keep
       the numeric protocol values here as part of the read-only boundary. */
    switch (command)
    {
        case 20: /* WRITE_DAT_UNTIL_STOP */
        case 24: /* WRITE_SINGLE_BLOCK */
        case 25: /* WRITE_MULTIPLE_BLOCK */
        case 26: /* PROGRAM_CID */
        case 27: /* PROGRAM_CSD */
        case 28: /* SET_WRITE_PROT */
        case 29: /* CLR_WRITE_PROT */
        case 32: /* SD ERASE_WR_BLK_START */
        case 33: /* SD ERASE_WR_BLK_END */
        case 35: /* MMC ERASE_GROUP_START */
        case 36: /* MMC ERASE_GROUP_END */
        case 38: /* ERASE */
        case 42: /* LOCK_UNLOCK */
        case 56: /* GEN_CMD can select a card write operation */
            return TRUE;
        default:
            return FALSE;
    }
}

enum p4sd_request_state
{
    P4SD_SENDING_CMD,
    P4SD_SENDING_DATA,
    P4SD_BUSY,
    P4SD_IDLE
};

ULONG FNAME_P4SDBUS(SendCmd)(struct TagItem *tags, struct sdcard_Bus *bus)
{
    ULONG command_index = GetTagData(SDCARD_TAG_CMD, 0, tags);
    ULONG argument = GetTagData(SDCARD_TAG_ARG, 0, tags);
    ULONG rsp_type = GetTagData(SDCARD_TAG_RSPTYPE, MMC_RSP_NONE, tags);
    UBYTE *data = (UBYTE *)(IPTR)GetTagData(SDCARD_TAG_DATA, 0, tags);
    ULONG data_len = GetTagData(SDCARD_TAG_DATALEN, 0, tags);
    ULONG data_flags = GetTagData(SDCARD_TAG_DATAFLAGS, 0, tags);
    struct TagItem *response = FindTagItem(SDCARD_TAG_RSP, tags);
    ULONG command = command_index & 0x3F;
    struct p4sd_private *priv = P4SD_PRIV(bus);
    ULONG started, transferred = 0;
    enum p4sd_request_state state = P4SD_SENDING_CMD;
    BOOL busy_response = (rsp_type & MMC_RSP_BUSY) != 0;
    BOOL multi_read = command_index == MMC_CMD_READ_MULTIPLE_BLOCK;
    BOOL dma_read = FALSE;
    BOOL bounce_read = FALSE;
    UBYTE *receive = data;
    ULONG decided_raw = 0;
    BOOL command_started = FALSE;
    ULONG timeout = data_len ? P4SD_DATA_TIMEOUT_US : P4SD_CMD_TIMEOUT_US;
#if P4_SDCARD_TRACE
    ULONG trace_rintsts = ~0UL, trace_idsts = ~0UL;
    ULONG trace_tcbcnt = ~0UL, trace_tbbcnt = ~0UL;
#endif

    if (!priv->host_ready)
        return (ULONG)-1;

    /* Do not let an out-of-range command alias a permitted six-bit command
       in the hardware encoding (in particular, a denied write command). */
    if (command_index > 0x3F)
    {
        bug("[P4SD%02u] refusing invalid CMD%u\n",
            bus->sdcb_BusNum, command_index);
        return (ULONG)-1;
    }

    if (p4sd_media_mutating_command(command_index))
    {
        bug("[P4SD%02u] refusing media-mutating CMD%u\n",
            bus->sdcb_BusNum, command_index);
        return (ULONG)-1;
    }

    if (data_len != 0)
    {
        ULONG block_size;

        /* Hard read-only boundary.  There is no FIFO transmit code below,
           and no command can set the controller's R/W bit. */
        if (data == NULL || data_len > P4SD_MAX_DATA_LEN ||
            !(data_flags & MMC_DATA_READ) ||
            (data_flags & (MMC_DATA_WRITE | MMC_DATA_STREAM)))
        {
            bug("[P4SD%02u] refusing CMD%u data flags %08x len %u\n",
                bus->sdcb_BusNum, command_index, data_flags, data_len);
            return (ULONG)-1;
        }

        if (!p4sd_reset_fifo(bus))
            return (ULONG)-1;

        block_size = data_len > 512 ? 512 : data_len;
        if (data_len % block_size != 0)
            return (ULONG)-1;

        if (multi_read && (data_len < 2 * 512 || data_len % 512 != 0))
        {
            bug("[P4SD%02u] refusing malformed CMD18 length %u\n",
                bus->sdcb_BusNum, data_len);
            return (ULONG)-1;
        }
        p4sd_set(P4SD_BLKSIZ, block_size);
        p4sd_set(P4SD_BYTCNT, data_len);

        /* The cache maintenance around the descriptor chain works on whole
           64-byte lines.  A destination that does not start on one, or does
           not cover a whole number of them, goes through the aligned bounce
           buffer if it fits there. */
        if ((((IPTR)data & 63) != 0 || (data_len & 63) != 0))
        {
            if (data_len > sizeof(p4sd_bounce))
            {
                /* Unreachable while the bounce is P4SD_MAX_DATA_LEN and the
                   length check above enforces the same bound; kept because
                   the two constants could drift apart, and because a silent
                   overrun here would be a memory corruption rather than a
                   refused read. */
                bug("[P4SD%02u] refusing unaligned CMD%u receive of %u bytes "
                    "at %p\n", bus->sdcb_BusNum, command_index, data_len,
                    data);
                return (ULONG)-1;
            }
            bounce_read = TRUE;
            receive = p4sd_bounce;
        }

#if P4_SDCARD_TRACE
        if (multi_read && argument == P4SD_TRACE_LBA && data_len == 2 * 512)
        {
            memset(&p4sd_trace, 0, sizeof(p4sd_trace));
            p4sd_trace.active = TRUE;
            p4sd_trace.data = receive;
        }
#endif
        if (!p4sd_dma_prepare_read(receive, data_len, bus))
        {
            bug("[P4SD%02u] cannot prepare CMD%u IDMAC receive\n",
                bus->sdcb_BusNum, command_index);
            return (ULONG)-1;
        }
        dma_read = TRUE;
        command |= P4SD_CMD_DATA;
    }

    if (rsp_type & MMC_RSP_PRESENT)
        command |= P4SD_CMD_RESP;
    if (rsp_type & MMC_RSP_136)
        command |= P4SD_CMD_RESP_LONG;
    if (rsp_type & MMC_RSP_CRC)
        command |= P4SD_CMD_RESP_CRC;

    /* Match Espressif's native-host command encoding.  CMD0 sends the
       initial clocks without wait_complete; CMD12 is a stop/abort and must
       be issuable while the data state machine is active. */
    if (command_index == MMC_CMD_GO_IDLE_STATE)
        command |= P4SD_CMD_SEND_INIT;
    else if (command_index == MMC_CMD_STOP_TRANSMISSION)
        command |= P4SD_CMD_STOP_ABORT;
    else
        command |= P4SD_CMD_WAIT_DATA;

    command |= P4SD_CMD_USE_HOLD | P4SD_CMD_START;
    if (!p4sd_wait_clear(P4SD_CMD, P4SD_CMD_START, P4SD_CMD_TIMEOUT_US))
        return (ULONG)-1;

    p4sd_set(P4SD_RINTSTS, 0xFFFFFFFFUL);
    p4sd_set(P4SD_CMDARG, argument);
    p4sd_set(P4SD_CMD, command); /* never P4SD_CMD_WRITE */
    command_started = TRUE;
#if P4_SDCARD_TRACE
    p4sd_trace_take(&p4sd_trace, "start");
#endif
    started = sdcard_CurrentTime();

    while (!p4sd_timed_out(started, timeout))
    {
        ULONG raw = p4sd_reg(P4SD_RINTSTS);
        ULONG idstatus = dma_read ? p4sd_reg(P4SD_IDSTS) : 0;

        decided_raw = raw;

        if (!dma_read && raw)
            p4sd_set(P4SD_RINTSTS, raw);

#if P4_SDCARD_TRACE
        if (p4sd_trace.active &&
            (raw != trace_rintsts || idstatus != trace_idsts ||
             p4sd_reg(P4SD_TCBCNT) != trace_tcbcnt ||
             p4sd_reg(P4SD_TBBCNT) != trace_tbbcnt))
        {
            p4sd_trace_take(&p4sd_trace, "state");
            trace_rintsts = raw;
            trace_idsts = idstatus;
            trace_tcbcnt = p4sd_reg(P4SD_TCBCNT);
            trace_tbbcnt = p4sd_reg(P4SD_TBBCNT);
        }
#endif

        /* This exercises the exact error branch without a media write. */
        /* A data error can only be acted on once the request has reached
           SENDING_DATA, so injecting one earlier does nothing: the bit is
           checked under `state == P4SD_SENDING_DATA` and the local raw value
           is rebuilt on the next iteration.  It appeared to work only while
           CMD_DONE happened to be set in the same iteration that injected
           it, which changed when the core's timing changed.  Hold the data
           case back until the state is right; the other two act immediately
           and are state-independent. */
        if (multi_read && !p4sd_fault_injected &&
            P4_SDCARD_FAULT_INJECT != P4SD_FAULT_NONE &&
            (P4_SDCARD_FAULT_INJECT != P4SD_FAULT_DATA_CRC ||
             state == P4SD_SENDING_DATA))
        {
            p4sd_fault_injected = TRUE;
            /* Name the injected mode.  The failure line below prints the
               real RINTSTS, which cannot show a software-injected bit, so
               without this the three fault cases are indistinguishable in a
               log and the gate's recovery evidence would be unverifiable. */
            bug("[P4SD%02u] injecting fault mode %u in state %u\n",
                bus->sdcb_BusNum, (unsigned)P4_SDCARD_FAULT_INJECT,
                (unsigned)state);
            if (P4_SDCARD_FAULT_INJECT == P4SD_FAULT_CMD_CRC)
                raw |= P4SD_INT_RCRC;
            else if (P4_SDCARD_FAULT_INJECT == P4SD_FAULT_DATA_CRC)
                raw |= P4SD_INT_DCRC;
            else
                break;
            decided_raw = raw;
        }

        /* This is the v5.4.2 SENDING_CMD -> SENDING_DATA transition. */
        if (state == P4SD_SENDING_CMD && (raw & P4SD_INT_CMD_ERRORS))
            goto failed;
        if (state == P4SD_SENDING_CMD && (raw & P4SD_INT_CMD_DONE))
        {
            p4sd_copy_response(response, rsp_type);
            state = data_len ? P4SD_SENDING_DATA : P4SD_IDLE;
        }

        /* The documented, bounded fallback completes at the final receive
           descriptor and lets generic sdcard.device send physical CMD12.
           The rejected v5.4.2 DATA_OVER/auto-stop experiment is retained in
           the roadmap rather than left active on the development board. */
        if (dma_read && state == P4SD_SENDING_DATA &&
            p4sd_dma_read_ready(data_len))
        {
            if (!p4sd_dma_complete_read(receive, data_len, bus))
                goto failed;
            if (bounce_read)
                memcpy(data, receive, data_len);
            return 0;
        }

        if (!dma_read && data_len && (raw & P4SD_INT_RXDR))
            p4sd_drain_read_fifo(data, data_len, &transferred);
        /* CMD51's eight-byte SCR response can reach the FIFO only with its
           terminal DATA_OVER indication.  The old PIO path drained that
           tail; retain it while CMD18 itself uses IDMAC. */
        if (!dma_read && data_len && (raw & P4SD_INT_DATA_OVER))
            p4sd_drain_read_fifo(data, data_len, &transferred);

        if (state == P4SD_SENDING_DATA)
        {
            if ((raw & P4SD_INT_DATA_ERRORS) ||
                (idstatus & P4SD_IDSTS_ERRORS))
                goto failed;
            if (dma_read && (idstatus & (P4SD_IDSTS_TI | P4SD_IDSTS_RI |
                                         P4SD_IDSTS_NI)))
                state = P4SD_BUSY;
            if (raw & (P4SD_INT_SBE | P4SD_INT_DATA_OVER))
                state = P4SD_IDLE;
            else if (!dma_read && (raw & P4SD_INT_DATA_OVER))
                state = (transferred == data_len) ? P4SD_IDLE : P4SD_SENDING_DATA;
        }
        if (state == P4SD_BUSY && (raw & P4SD_INT_DATA_OVER))
            state = P4SD_IDLE;

        if (state != P4SD_IDLE)
            continue;

        if (dma_read && !p4sd_dma_complete_read(receive, data_len, bus))
            goto failed;
        if (dma_read && bounce_read)
            memcpy(data, receive, data_len);
        if (!dma_read && data_len && transferred != data_len)
            goto failed;

        if (busy_response)
        {
            ULONG busy_started = sdcard_CurrentTime();

            while (p4sd_reg(P4SD_STATUS) & P4SD_STATUS_DATA_BUSY)
                if (p4sd_timed_out(busy_started, P4SD_BUSY_TIMEOUT_US))
                    goto failed;
        }

        if (command_index == MMC_CMD_STOP_TRANSMISSION &&
            !p4sd_confirm_tran(bus))
            goto failed;
        return 0;
    }

failed:
    /* State and the decision-time raw value, not just the register.  A
       software-injected error bit never appears in RINTSTS, so without these
       two the three fault-injection modes produce identical failure lines and
       the gate's recovery evidence cannot say which branch fired. */
    bug("[P4SD%02u] CMD%u failed in state %u: decided on raw=%08x, "
        "rintsts=%08x idmac=%08x status=%08x bytes=%u/%u\n",
        bus->sdcb_BusNum, command_index, (unsigned)state, decided_raw,
        p4sd_reg(P4SD_RINTSTS),
        dma_read ? p4sd_reg(P4SD_IDSTS) : 0, p4sd_reg(P4SD_STATUS),
        transferred, data_len);
    if (dma_read)
        p4sd_dma_dump(data_len, bus);
    if (multi_read && command_started)
        (void)p4sd_recover_multiblock(bus);
    else if (data_len && !p4sd_reset_fifo(bus))
        priv->host_ready = FALSE;
    else if (command_index == MMC_CMD_STOP_TRANSMISSION)
        priv->host_ready = FALSE;
    return (ULONG)-1;
}

/* SendCmd completes the whole transaction synchronously. */
ULONG FNAME_P4SDBUS(WaitCmd)(ULONG mask, ULONG timeout,
                             struct sdcard_Bus *bus)
{
    (void)mask;
    (void)timeout;
    (void)bus;
    return 0;
}

ULONG FNAME_P4SDBUS(FinishCmd)(struct TagItem *tags,
                               struct sdcard_Bus *bus)
{
    (void)tags;
    (void)bus;
    return 0;
}

ULONG FNAME_P4SDBUS(FinishData)(struct TagItem *tags,
                                struct sdcard_Bus *bus)
{
    (void)tags;
    (void)bus;
    return 0;
}

void FNAME_P4SD(BusInit)(struct sdcard_Bus *bus)
{
    FNAME_P4SDBUS(SetPowerLevel)(bus->sdcb_Power, FALSE, bus);
    /* IDF starts strongly while identifying the card. */
    p4sd_configure_pins(3);

    if (!p4sd_host_init(bus))
    {
        bug("[P4SD%02u] host clock/reset initialization failed\n",
            bus->sdcb_BusNum);
        return;
    }

    FNAME_P4SDBUS(SetBusWidth)(1, bus);
    bug("[P4SD%02u] slot 0 polling IDMAC receive, 1-bit/400 kHz, read-only\n",
        bus->sdcb_BusNum);
}

void FNAME_P4SD(BusPostIRQInit)(struct sdcard_Bus *bus)
{
    struct sdcard_Unit *unit;

    if (!P4SD_PRIV(bus)->host_ready)
        return;
    if (!p4sd_card_present())
    {
        bug("[P4SD%02u] GPIO45 high: no card present\n",
            bus->sdcb_BusNum);
        return;
    }

    if (!FNAME_SDCBUS(RegisterUnit)(bus))
    {
        bug("[P4SD%02u] card enumeration failed\n", bus->sdcb_BusNum);
        return;
    }

    /* Seeed lowers all six signal pads after mount to avoid overshoot. */
    p4sd_configure_pins(1);

    /* Keep both layers honest: generic writes fail before SendCmd, while
       SendCmd independently rejects every non-read data transaction. */
    unit = (&bus->sdcb_BusUnits->sdcbu_Units)[0];
    if (unit != NULL)
        unit->sdcu_Flags |= AF_Card_WriteProtect;
}
