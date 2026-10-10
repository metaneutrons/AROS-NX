/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Synchronous, polling-only ESP32-P4 DesignWare MMC backend.  CMD18 uses
    bounded receive-only IDMAC; the backend deliberately contains no
    transmit-data path.
*/

#define DEBUG 0
#include <aros/debug.h>

#include <aros/cacheops.h>
#include <aros/kernel.h>
#include <hardware/mmc.h>
#include <hardware/sdhc.h>
#include <proto/exec.h>
#include <proto/kernel.h>
#include <proto/utility.h>

#include <string.h>

#include "sdcard_esp32p4_intern.h"
#include "sdcard_unit.h"
#include "timer.h"

static BOOL p4sd_reset_fifo(struct sdcard_Bus *bus);

BOOL FNAME_P4SDBUS(BeginTransaction)(struct sdcard_Bus *bus, BOOL may_wait)
{
    struct p4sd_private *priv = P4SD_PRIV(bus);
    if (!priv->host ||
        !priv->host->acquire(priv->host,
                            may_wait ? P4_SDMMC_HOST_WAIT_US : 0))
        return FALSE;
    if (priv->host_generation != priv->host->generation)
        priv->host_ready = FALSE;
    return TRUE;
}

void FNAME_P4SDBUS(EndTransaction)(struct sdcard_Bus *bus)
{
    struct p4sd_private *priv = P4SD_PRIV(bus);
    priv->host->release(priv->host);
}

#if P4_SDCARD_FAULT_INJECT != P4SD_FAULT_NONE
static BOOL p4sd_fault_injected;
#endif
static ULONG p4sd_cmd12_confirmations;

#include "../sdmmc/dma.h"
#include "../sdmmc/command.h"
#define p4sd_dma_desc ((struct P4HostDMADesc *)P4SD_PRIV(bus)->host->dma_desc)
#define p4sd_bounce (P4SD_PRIV(bus)->host->bounce)
#define P4SD_DESC_OWN P4HOST_DESC_OWN

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

static void p4sd_trace_take(struct p4sd_trace *trace, const char *event,
                            struct sdcard_Bus *bus)
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

/* Cache handoffs belong to the permanent shared host resource. */

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
    if (offset == P4SD_RINTSTS)
        value &= P4SD_INT_CMD_DATA_MASK;
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

#if P4_BOARD_SD_HAS_POWER_GPIO
static void p4sd_gpio_output(unsigned int gpio, BOOL high)
{
    ULONG bit = 1UL << (gpio - 32);

    /* Establish the level before connecting the output driver. */
    p4sd_write(P4_GPIO_BASE + (high ? P4_GPIO_OUT1_W1TS
                                   : P4_GPIO_OUT1_W1TC), bit);
    p4sd_iomux(gpio, P4_IOMUX_FUNC_GPIO, FALSE, 1);
    p4sd_write(P4_GPIO_BASE + P4_GPIO_ENABLE1_W1TS, bit);
}
#endif

/*
 * A board without a card-detect contact reports a card; identification then
 * finds out whether one answers.
 */
static BOOL p4sd_card_present(void)
{
#if P4_BOARD_SD_HAS_DETECT
    ULONG bit = 1UL << (P4_BOARD_SD_DETECT_GPIO - 32);

    p4sd_iomux(P4_BOARD_SD_DETECT_GPIO, P4_IOMUX_FUNC_GPIO, TRUE, 0);
    p4sd_write(P4_GPIO_BASE + P4_GPIO_ENABLE1_W1TC, bit);
    return (p4sd_read(P4_GPIO_BASE + P4_GPIO_IN1) & bit) == 0;
#else
    return TRUE;
#endif
}

static void p4sd_configure_pins(unsigned int drive)
{
    unsigned int gpio;

    /* Native slot 0 is GPIO39..44.  On the D1001 GPIO45/46 stay CD/power. */
    for (gpio = P4_SD_D0_GPIO; gpio <= P4_SD_CMD_GPIO; ++gpio)
        p4sd_iomux(gpio, P4_IOMUX_FUNC_SDMMC, TRUE, drive);
}

static BOOL p4sd_module_reset(struct sdcard_Bus *bus)
{
    struct p4sd_private *priv = P4SD_PRIV(bus);
    return priv->host->reset(priv->host);
}

static BOOL p4sd_program_clock(ULONG requested, struct sdcard_Bus *bus)
{
    struct p4sd_private *priv = P4SD_PRIV(bus);
    ULONG actual;
    /* Storage keeps its qualified 25-MHz ceiling; radio negotiates its own. */
    if (requested > P4SD_CLOCK_MAX_HZ)
        requested = P4SD_CLOCK_MAX_HZ;
    if (!priv->host->set_clock(priv->host, 0, requested, TRUE, &actual))
    {
        priv->host_ready = FALSE;
        return FALSE;
    }
    priv->clock_hz = actual;
    return TRUE;
}

static BOOL p4sd_host_init(struct sdcard_Bus *bus)
{
    struct p4sd_private *priv = P4SD_PRIV(bus);

    priv->host_ready = FALSE;
    /* The first owner performs shared initialization. An already initialized
     * peer must retain its clocks, width and protocol state. A faulted host
     * requires an explicit full-reset/re-enumeration path, not slot joining. */
    if (!priv->host->initialization_started && !p4sd_module_reset(bus))
        return FALSE;
    if (priv->host->host_div < 1 || priv->host->host_div > 16 ||
        priv->host->dma_faulted || priv->host->dma_length)
        return FALSE;
    p4sd_set(P4SD_RST_N, p4sd_reg(P4SD_RST_N) | 1UL);
    p4sd_set(P4SD_CTYPE, p4sd_reg(P4SD_CTYPE) &
             ~(P4SD_CTYPE_4BIT_SLOT0 | P4SD_CTYPE_8BIT_SLOT0));

    priv->host_ready = TRUE;
    if (!p4sd_program_clock(P4SD_CLOCK_MIN_HZ, bus))
    {
        priv->host_ready = FALSE;
        return FALSE;
    }
    priv->host_generation = priv->host->generation;
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

static void p4sd_soft_reset_locked(UBYTE mask, struct sdcard_Bus *bus)
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

        p4sd_module_reset(bus);
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

    if (!priv->host_ready)
        return;

    p4sd_set(P4SD_RINTSTS, 0xFFFFFFFFUL);
    if (!p4sd_reset_fifo(bus))
    {
        priv->host_ready = FALSE;
        return;
    }
}

static void p4sd_set_clock_locked(ULONG speed, struct sdcard_Bus *bus)
{
    if (!P4SD_PRIV(bus)->host_ready)
        return;
    if (!p4sd_program_clock(speed, bus))
        bug("[P4SD%02u] failed to change card clock\n", bus->sdcb_BusNum);
}

static void p4sd_set_power_locked(ULONG levels, BOOL lowest,
                                  struct sdcard_Bus *bus)
{
    struct p4sd_private *priv = P4SD_PRIV(bus);
    ULONG control;

    (void)levels;
    (void)lowest;
    if (!priv->host_ready || priv->powered)
        return;

    /* D1001 board sequence: external switch low, LDO4 at the 3.3-V
       bypass, 100-ms settling delay, then external switch high. A board
       whose switch is hard-wired on only gets LDO4 and the settling time. */
#if P4_BOARD_SD_HAS_POWER_GPIO
    p4sd_gpio_output(P4_BOARD_SD_POWER_GPIO, FALSE);
#endif

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
#if P4_BOARD_SD_HAS_POWER_GPIO
    p4sd_gpio_output(P4_BOARD_SD_POWER_GPIO, TRUE);
    sdcard_Udelay(P4SD_POWER_DELAY_US);
#endif
    priv->powered = TRUE;
}

static void p4sd_set_width_locked(UBYTE width, struct sdcard_Bus *bus)
{
    ULONG ctype;

    if (!P4SD_PRIV(bus)->host_ready)
        return;
    ctype = p4sd_reg(P4SD_CTYPE);

    /* MicroSD has four data lines, and on the D1001 GPIO45/46 occupy the
       native D4/D5 pins: eight-bit mode is forbidden. */
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



static BOOL p4sd_reset_fifo(struct sdcard_Bus *bus)
{
    struct P4SDMMCHost *host = P4SD_PRIV(bus)->host;
    return host->fifo_reset(host);
}

static BOOL p4sd_reset_dma(struct sdcard_Bus *bus)
{
    struct P4SDMMCHost *host = P4SD_PRIV(bus)->host;
    return host->dma_reset(host);
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
    p4sd_trace_take(&p4sd_trace, "recover", bus);
    p4sd_trace_dump(&p4sd_trace, bus);
#endif
    priv->host_ready = FALSE;
    return FALSE;
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

#if P4_SDCARD_TRACE
/* Hook runs under the shared owner. Record a bounded set of changed states,
 * without issuing commands or changing controller/cache ownership. Payload
 * samples are CPU views only, not proof of DMA cache coherence. */
static void p4sd_command_observe(void *context,
                                 const struct P4HostCommand *request,
                                 enum P4HostCommandObservation event,
                                 const struct P4HostCommandResult *result)
{
    struct sdcard_Bus *bus = context;
    ULONG tcbcnt, tbbcnt;
    struct p4sd_trace_sample *last;
    (void)request;
    if (!p4sd_trace.active || p4sd_trace.count == P4SD_TRACE_SAMPLES)
        return;
    if (event == P4HOST_OBSERVE_START || event == P4HOST_OBSERVE_FINISH)
    {
        p4sd_trace_take(&p4sd_trace,
                         event == P4HOST_OBSERVE_START ? "start" : "finish", bus);
        return;
    }
    tcbcnt = p4sd_reg(P4SD_TCBCNT);
    tbbcnt = p4sd_reg(P4SD_TBBCNT);
    last = p4sd_trace.count ? &p4sd_trace.sample[p4sd_trace.count - 1] : NULL;
    if (!last || last->rintsts != result->raw || last->idsts != result->idstatus ||
        last->tcbcnt != tcbcnt || last->tbbcnt != tbbcnt)
        p4sd_trace_take(&p4sd_trace, "state", bus);
}
#endif

/* Every build uses the shared motor. Card policy, diagnostic selection,
 * CMD18 workaround permission and physical card recovery stay in this client. */
static ULONG p4sd_shared_command(struct sdcard_Bus *bus, ULONG index,
                                 ULONG argument, ULONG rsp_type,
                                 struct TagItem *response, UBYTE *data,
                                 ULONG data_len, ULONG data_flags)
{
    struct p4sd_private *priv = P4SD_PRIV(bus);
    struct P4HostCommand request;
    struct P4HostCommandResult result;

    memset(&request, 0, sizeof(request));
    request.slot = 0;
    request.generation = priv->host_generation;
    request.index = index;
    request.argument = argument;
    request.response_flags = rsp_type;
    /* A block read without DATA would leave the card protocol active while
     * the controller reports a completed no-data command. */
    if (!data_len && (index == MMC_CMD_READ_SINGLE_BLOCK ||
                      index == MMC_CMD_READ_MULTIPLE_BLOCK))
        return (ULONG)-1;
    /* This storage client configures 512-byte sectors. CMD17 transfers exactly
     * one of them; multi-block transfers require CMD18 and its stop path. */
    if (index == MMC_CMD_READ_SINGLE_BLOCK && data_len != 512)
        return (ULONG)-1;
    if (data_len)
    {
        if (!data || data_len > P4SD_MAX_DATA_LEN ||
            !(data_flags & MMC_DATA_READ) ||
            (data_flags & (MMC_DATA_WRITE | MMC_DATA_STREAM)))
            return (ULONG)-1;
        request.block_size = data_len > 512 ? 512 : data_len;
        if (data_len % request.block_size ||
            (index == MMC_CMD_READ_MULTIPLE_BLOCK &&
             (data_len < 1024 || data_len % 512)))
            return (ULONG)-1;
        request.data = data;
        request.length = data_len;
        /* Known storage-only workaround, never enable for CMD17/SCR/radio.
         * The generic outer transaction must still issue physical CMD12/13. */
        request.early_cmd18 = index == MMC_CMD_READ_MULTIPLE_BLOCK;
    }
#if P4_SDCARD_TRACE
    if (index == MMC_CMD_READ_MULTIPLE_BLOCK && argument == P4SD_TRACE_LBA &&
        data_len == 1024)
    {
        memset(&p4sd_trace, 0, sizeof(p4sd_trace));
        p4sd_trace.active = TRUE;
        p4sd_trace.data = priv->host->bounce;
    }
    if (p4sd_trace.active)
    {
        request.observe = p4sd_command_observe;
        request.observe_context = bus;
    }
#endif
#if P4_SDCARD_FAULT_INJECT != P4SD_FAULT_NONE
#if P4_SDCARD_FAULT_INJECT < P4SD_FAULT_CMD_CRC || P4_SDCARD_FAULT_INJECT > P4SD_FAULT_TIMEOUT
#error Unsupported SD fault injection mode
#endif
    if (index == MMC_CMD_READ_MULTIPLE_BLOCK && !p4sd_fault_injected)
        request.diagnostic_fault = (enum P4HostCommandFault)P4_SDCARD_FAULT_INJECT;
#endif
    /* Preserve legacy card-status policy: no generic R1 mask is added here. */
    if (!priv->host->command(priv->host, &request, &result))
    {
#if P4_SDCARD_FAULT_INJECT != P4SD_FAULT_NONE
        if (result.injected_fault != P4HOST_FAULT_NONE)
        {
            p4sd_fault_injected = TRUE;
            bug("[P4SD%02u] injecting fault mode %u in shared motor\n",
                bus->sdcb_BusNum, (unsigned)result.injected_fault);
        }
#endif
        bug("[P4SD%02u] shared CMD%u failed: status=%u started=%u "
            "raw=%08x idmac=%08x injected=%u\n", bus->sdcb_BusNum, index,
            (unsigned)result.status, (unsigned)result.started,
            result.raw, result.idstatus, (unsigned)result.injected_fault);
        if (priv->host->dma_faulted ||
            priv->host_generation != priv->host->generation)
            priv->host_ready = FALSE;
        else if (data_len && result.started)
        {
            /* DMA/FIFO reset is not card protocol recovery. A started read
             * needs a confirmed stop/TRAN before this transaction can end. */
            if (index == MMC_CMD_READ_MULTIPLE_BLOCK)
                (void)p4sd_recover_multiblock(bus);
            else if (!p4sd_confirm_tran(bus))
                priv->host_ready = FALSE;
        }
        else if (index == MMC_CMD_STOP_TRANSMISSION)
            priv->host_ready = FALSE;
#if P4_SDCARD_TRACE
        /* Dump collected samples even when a shared fault makes card recovery
         * unsafe. This does not read MMIO after ownership became uncertain. */
        p4sd_trace_dump(&p4sd_trace, bus);
#endif
        return (ULONG)-1;
    }
    if (index == MMC_CMD_STOP_TRANSMISSION && !p4sd_confirm_tran(bus))
    {
        priv->host_ready = FALSE;
#if P4_SDCARD_TRACE
        p4sd_trace_dump(&p4sd_trace, bus);
#endif
        return (ULONG)-1;
    }
#if P4_SDCARD_TRACE
    if (index == MMC_CMD_STOP_TRANSMISSION)
        p4sd_trace_dump(&p4sd_trace, bus);
#endif
    if (response && (rsp_type & MMC_RSP_PRESENT))
    {
        if (rsp_type & MMC_RSP_136)
        {
            ULONG *words = (ULONG *)(IPTR)response->ti_Data;
            if (words)
                memcpy(words, result.response, sizeof(result.response));
        }
        else
            response->ti_Data = result.response[0];
    }
    return 0;
}

static ULONG p4sd_send_cmd_locked(struct TagItem *tags, struct sdcard_Bus *bus)
{
    ULONG command_index = GetTagData(SDCARD_TAG_CMD, 0, tags);
    ULONG argument = GetTagData(SDCARD_TAG_ARG, 0, tags);
    ULONG rsp_type = GetTagData(SDCARD_TAG_RSPTYPE, MMC_RSP_NONE, tags);
    UBYTE *data = (UBYTE *)(IPTR)GetTagData(SDCARD_TAG_DATA, 0, tags);
    ULONG data_len = GetTagData(SDCARD_TAG_DATALEN, 0, tags);
    ULONG data_flags = GetTagData(SDCARD_TAG_DATAFLAGS, 0, tags);
    struct TagItem *response = FindTagItem(SDCARD_TAG_RSP, tags);
    struct p4sd_private *priv = P4SD_PRIV(bus);

    if (!priv->host_ready)
        return (ULONG)-1;
    /* Refuse before the engine encodes six bits: never alias a denied write. */
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
    /* Diagnostics annotate this same request; there is no alternate motor. */
    return p4sd_shared_command(bus, command_index, argument, rsp_type,
                               response, data, data_len, data_flags);
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

static void p4sd_bus_init_locked(struct sdcard_Bus *bus)
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

static void p4sd_bus_post_init_locked(struct sdcard_Bus *bus)
{
    struct sdcard_Unit *unit;

    if (!P4SD_PRIV(bus)->host_ready)
        return;
    if (!p4sd_card_present())
    {
        bug("[P4SD%02u] card-detect high: no card present\n",
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

/* Standalone vtable calls own the host too, but never wait: OpenDevice may
 * call them under Forbid. Generic chunk/enumeration hooks
 * hold one outer acquisition through all dependent calls. Recovery's CMD12
 * and status probes nest in the same task, never release the outer owner. */
void FNAME_P4SDBUS(SoftReset)(UBYTE mask, struct sdcard_Bus *bus)
{
    if (!FNAME_P4SDBUS(BeginTransaction)(bus, FALSE))
        return;
    p4sd_soft_reset_locked(mask, bus);
    FNAME_P4SDBUS(EndTransaction)(bus);
}

void FNAME_P4SDBUS(SetClock)(ULONG speed, struct sdcard_Bus *bus)
{
    if (!FNAME_P4SDBUS(BeginTransaction)(bus, FALSE))
        return;
    p4sd_set_clock_locked(speed, bus);
    FNAME_P4SDBUS(EndTransaction)(bus);
}

void FNAME_P4SDBUS(SetPowerLevel)(ULONG levels, BOOL lowest,
                               struct sdcard_Bus *bus)
{
    if (!FNAME_P4SDBUS(BeginTransaction)(bus, FALSE))
        return;
    p4sd_set_power_locked(levels, lowest, bus);
    FNAME_P4SDBUS(EndTransaction)(bus);
}

void FNAME_P4SDBUS(SetBusWidth)(UBYTE width, struct sdcard_Bus *bus)
{
    if (!FNAME_P4SDBUS(BeginTransaction)(bus, FALSE))
        return;
    p4sd_set_width_locked(width, bus);
    FNAME_P4SDBUS(EndTransaction)(bus);
}

ULONG FNAME_P4SDBUS(SendCmd)(struct TagItem *tags, struct sdcard_Bus *bus)
{
    ULONG result;
    if (!FNAME_P4SDBUS(BeginTransaction)(bus, FALSE))
        return (ULONG)-1;
    result = p4sd_send_cmd_locked(tags, bus);
    FNAME_P4SDBUS(EndTransaction)(bus);
    return result;
}

void FNAME_P4SD(BusInit)(struct sdcard_Bus *bus)
{
    /* BusTask boundary, unlike the leaf vtable calls below OpenDevice. */
    if (!FNAME_P4SDBUS(BeginTransaction)(bus, TRUE))
        return;
    p4sd_bus_init_locked(bus);
    FNAME_P4SDBUS(EndTransaction)(bus);
}

void FNAME_P4SD(BusPostIRQInit)(struct sdcard_Bus *bus)
{
    if (!FNAME_P4SDBUS(BeginTransaction)(bus, TRUE))
        return;
    p4sd_bus_post_init_locked(bus);
    FNAME_P4SDBUS(EndTransaction)(bus);
}
