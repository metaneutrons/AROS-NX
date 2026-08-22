/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Synchronous, polling-only PIO backend for the ESP32-P4 DesignWare MMC
    controller.  The backend deliberately contains no transmit-data path.
*/

#define DEBUG 0
#include <aros/debug.h>

#include <hardware/mmc.h>
#include <proto/utility.h>

#include "sdcard_esp32p4_intern.h"
#include "sdcard_unit.h"
#include "timer.h"

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
    ULONG bit = 1UL << (P4_SD_DETECT_GPIO - 32);

    p4sd_iomux(P4_SD_DETECT_GPIO, P4_IOMUX_FUNC_GPIO, TRUE, 0);
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
    p4sd_set(P4SD_FIFOTH, 15UL << 16);
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
    (void)mask;

    p4sd_set(P4SD_CTRL, p4sd_reg(P4SD_CTRL) | P4SD_CTRL_RESET_ALL);
    if (!p4sd_wait_clear(P4SD_CTRL, P4SD_CTRL_RESET_ALL,
                         P4SD_CMD_TIMEOUT_US))
    {
        bug("[P4SD%02u] controller reset timed out\n", bus->sdcb_BusNum);
        P4SD_PRIV(bus)->host_ready = FALSE;
        return;
    }

    p4sd_set(P4SD_CTRL, 0);
    p4sd_set(P4SD_BMOD, 0);
    p4sd_set(P4SD_IDINTEN, 0);
    p4sd_set(P4SD_INTMASK, 0);
    p4sd_set(P4SD_RINTSTS, 0xFFFFFFFFUL);
    p4sd_set(P4SD_CTYPE, 0);
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
    p4sd_gpio_output(P4_SD_POWER_GPIO, FALSE);

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
    p4sd_gpio_output(P4_SD_POWER_GPIO, TRUE);
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
    ULONG available = (p4sd_reg(P4SD_STATUS) >>
                       P4SD_STATUS_FIFO_COUNT_S) &
                       P4SD_STATUS_FIFO_COUNT_M;

    while (available-- != 0 && *done < length)
        p4sd_store_fifo_word(data, length, done,
                             p4sd_reg(P4SD_BUFFIFO));
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
    ULONG started, transferred = 0;
    BOOL command_done = FALSE;
    BOOL data_done = data_len == 0;
    BOOL busy_response = (rsp_type & MMC_RSP_BUSY) != 0;
    ULONG timeout = data_len ? P4SD_DATA_TIMEOUT_US : P4SD_CMD_TIMEOUT_US;

    if (!P4SD_PRIV(bus)->host_ready)
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

    /* A failed CMD18 needs a proven CMD12/data-state recovery sequence.
       Keep the controlled read-only first stage fail-safe by refusing to
       enter multi-block transfer state at all. */
    if (command_index == MMC_CMD_READ_MULTIPLE_BLOCK)
    {
        bug("[P4SD%02u] refusing unvalidated multi-block CMD18\n",
            bus->sdcb_BusNum);
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

        p4sd_set(P4SD_CTRL, p4sd_reg(P4SD_CTRL) | P4SD_CTRL_FIFO_RESET);
        if (!p4sd_wait_clear(P4SD_CTRL, P4SD_CTRL_FIFO_RESET,
                             P4SD_CMD_TIMEOUT_US))
            return (ULONG)-1;

        block_size = data_len > 512 ? 512 : data_len;
        if (data_len % block_size != 0)
            return (ULONG)-1;
        p4sd_set(P4SD_BLKSIZ, block_size);
        p4sd_set(P4SD_BYTCNT, data_len);
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
    p4sd_set(P4SD_CMD, command); /* no AUTO_STOP and never P4SD_CMD_WRITE */
    started = sdcard_CurrentTime();

    while (!p4sd_timed_out(started, timeout))
    {
        ULONG raw = p4sd_reg(P4SD_RINTSTS);
        ULONG errors = P4SD_INT_CMD_ERRORS;

        if (data_len)
            errors |= P4SD_INT_DATA_ERRORS;
        if (raw & errors)
        {
            bug("[P4SD%02u] CMD%u failed: raw=%08x status=%08x\n",
                bus->sdcb_BusNum, command_index, raw,
                p4sd_reg(P4SD_STATUS));
            p4sd_set(P4SD_RINTSTS, raw);
            if (data_len)
                p4sd_set(P4SD_CTRL,
                         p4sd_reg(P4SD_CTRL) | P4SD_CTRL_FIFO_RESET);
            return (ULONG)-1;
        }

        if (data_len)
            p4sd_drain_read_fifo(data, data_len, &transferred);

        if ((raw & P4SD_INT_CMD_DONE) && !command_done)
        {
            p4sd_copy_response(response, rsp_type);
            command_done = TRUE;
            p4sd_set(P4SD_RINTSTS, P4SD_INT_CMD_DONE);
        }

        if (data_len && (raw & P4SD_INT_RXDR))
            p4sd_set(P4SD_RINTSTS, P4SD_INT_RXDR);

        if (data_len && (raw & P4SD_INT_DATA_OVER))
        {
            p4sd_drain_read_fifo(data, data_len, &transferred);
            data_done = transferred == data_len;
            p4sd_set(P4SD_RINTSTS, P4SD_INT_DATA_OVER);
        }

        if (command_done && data_done)
        {
            if (busy_response)
            {
                ULONG busy_started = sdcard_CurrentTime();

                while (p4sd_reg(P4SD_STATUS) & P4SD_STATUS_DATA_BUSY)
                    if (p4sd_timed_out(busy_started,
                                       P4SD_BUSY_TIMEOUT_US))
                        return (ULONG)-1;
            }
            return 0;
        }
    }

    bug("[P4SD%02u] CMD%u timed out: status=%08x bytes=%u/%u\n",
        bus->sdcb_BusNum, command_index, p4sd_reg(P4SD_STATUS),
        transferred, data_len);
    if (data_len)
        p4sd_set(P4SD_CTRL, p4sd_reg(P4SD_CTRL) | P4SD_CTRL_FIFO_RESET);
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
    bug("[P4SD%02u] slot 0 polling PIO, 1-bit/400 kHz, read-only\n",
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
