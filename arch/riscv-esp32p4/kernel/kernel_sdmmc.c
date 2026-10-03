/*
    Copyright (c) 2026, The AROS Development Team. All rights reserved.
    $Id$

    Desc: Read-only ESP32-P4 native SD/MMC bring-up for the D1001.
*/

#include "kernel_intern.h"
#include "hardware.h"

/* Synopsys DesignWare MMC register offsets. */
#define SD_CTRL             0x0000
#define  SD_CTRL_RESET_ALL  0x00000007U
#define SD_CLKDIV           0x0008
#define SD_CLKSRC           0x000C
#define SD_CLKENA           0x0010
#define SD_TMOUT            0x0014
#define SD_CTYPE            0x0018
#define SD_BLKSIZ           0x001C
#define SD_BYTCNT           0x0020
#define SD_INTMASK          0x0024
#define SD_CMDARG           0x0028
#define SD_CMD              0x002C
#define  SD_CMD_RESP        (1U << 6)
#define  SD_CMD_RESP_LONG   (1U << 7)
#define  SD_CMD_RESP_CRC    (1U << 8)
#define  SD_CMD_DATA        (1U << 9)
#define  SD_CMD_WAIT_DATA   (1U << 13)
#define  SD_CMD_SEND_INIT   (1U << 15)
#define  SD_CMD_UPDATE_CLK  (1U << 21)
#define  SD_CMD_USE_HOLD    (1U << 29)
#define  SD_CMD_START       (1U << 31)
#define SD_RESP0            0x0030
#define SD_RINTSTS          0x0044
#define  SD_INT_CMD_DONE    (1U << 2)
#define  SD_INT_DATA_OVER   (1U << 3)
#define  SD_INT_RXDR        (1U << 5)
#define  SD_INT_RESP_ERR    (1U << 1)
#define  SD_INT_RCRC        (1U << 6)
#define  SD_INT_DCRC        (1U << 7)
#define  SD_INT_RTO         (1U << 8)
#define  SD_INT_DTO         (1U << 9)
#define  SD_INT_HTO         (1U << 10)
#define  SD_INT_FRUN        (1U << 11)
#define  SD_INT_HLE         (1U << 12)
#define  SD_INT_SBE         (1U << 13)
#define  SD_INT_EBE         (1U << 15)
#define  SD_INT_CMD_ERRORS  (SD_INT_RESP_ERR | SD_INT_RCRC | SD_INT_RTO | \
                             SD_INT_HLE)
#define  SD_INT_DATA_ERRORS (SD_INT_DCRC | SD_INT_DTO | SD_INT_HTO | \
                             SD_INT_FRUN | SD_INT_SBE | SD_INT_EBE)
#define SD_STATUS           0x0048
#define  SD_STATUS_BUSY     (1U << 9)
#define  SD_STATUS_FIFO_S   17
#define  SD_STATUS_FIFO_M   0x1FFFU
#define SD_FIFOTH           0x004C
#define SD_RST_N            0x0078
#define SD_BMOD             0x0080
#define SD_IDSTS            0x008C
#define SD_IDINTEN          0x0090
#define SD_CARDTHRCTL       0x0100
#define SD_BUFFIFO          0x0200
#define SD_CLK_EDGE_SEL     0x0800

#define SD_CMD_TIMEOUT_US   200000UL
#define SD_DATA_TIMEOUT_US  500000UL
#define SD_POWER_DELAY_US   100000UL
#define SD_ACMD41_LIMIT_US  1000000UL

#define SD_OCR_BUSY         (1U << 31)
#define SD_OCR_CCS          (1U << 30)

static uint32_t sd_sector0[128] P4_SRAMDATA __attribute__((aligned(4)));

static inline uint32_t sd_read(unsigned long addr)
{
    return *(volatile uint32_t *)addr;
}

static inline void sd_write(unsigned long addr, uint32_t value)
{
    *(volatile uint32_t *)addr = value;
    __asm__ volatile("fence iorw, iorw" ::: "memory");
}

static inline uint32_t sd_reg(unsigned long offset)
{
    return sd_read(P4_SDMMC_BASE + offset);
}

static inline void sd_set(unsigned long offset, uint32_t value)
{
    sd_write(P4_SDMMC_BASE + offset, value);
}

static uint64_t sd_deadline(unsigned long usec)
{
    return krnTimerCount() + (uint64_t)usec *
           (P4_SYSTIMER_HZ / 1000000UL);
}

static int sd_expired(uint64_t deadline)
{
    return (int64_t)(krnTimerCount() - deadline) >= 0;
}

static void sd_delay(unsigned long usec)
{
    uint64_t deadline = sd_deadline(usec);

    while (!sd_expired(deadline))
        __asm__ volatile("nop");
}

static int sd_wait_clear(unsigned long offset, uint32_t mask,
                         unsigned long usec)
{
    uint64_t deadline = sd_deadline(usec);

    while (sd_reg(offset) & mask)
    {
        if (sd_expired(deadline))
            return 0;
    }
    return 1;
}

static void sd_iomux(unsigned int gpio, unsigned int function,
                     int input, unsigned int drive)
{
    unsigned long addr = P4_IOMUX_BASE + P4_IOMUX_GPIO39 +
                         (gpio - P4_SD_D0_GPIO) * 4;
    uint32_t value = sd_read(addr);

    value &= ~(P4_IOMUX_FUN_PD | P4_IOMUX_FUN_PU |
               P4_IOMUX_FUN_DRV_M | P4_IOMUX_MCU_SEL_M);
    value |= (drive << P4_IOMUX_FUN_DRV_S) & P4_IOMUX_FUN_DRV_M;
    value |= (function << P4_IOMUX_MCU_SEL_S) & P4_IOMUX_MCU_SEL_M;
    if (input)
        value |= P4_IOMUX_FUN_IE;
    else
        value &= ~P4_IOMUX_FUN_IE;
    sd_write(addr, value);
}

/* Without a card-detect contact, identification decides. */
static int sd_card_present(void)
{
#if P4_BOARD_SD_HAS_DETECT
    uint32_t bit = 1U << (P4_BOARD_SD_DETECT_GPIO - 32);

    sd_iomux(P4_BOARD_SD_DETECT_GPIO, P4_IOMUX_FUNC_GPIO, 1, 0);
    sd_write(P4_GPIO_BASE + P4_GPIO_ENABLE1_W1TC, bit);
    return (sd_read(P4_GPIO_BASE + P4_GPIO_IN1) & bit) == 0;
#else
    return 1;
#endif
}

#if P4_BOARD_SD_HAS_POWER_GPIO
static void sd_gpio_output(unsigned int gpio, int high)
{
    uint32_t bit = 1U << (gpio - 32);

    /* Select the inactive level before connecting the output driver. */
    sd_write(P4_GPIO_BASE + (high ? P4_GPIO_OUT1_W1TS
                                 : P4_GPIO_OUT1_W1TC), bit);
    sd_iomux(gpio, P4_IOMUX_FUNC_GPIO, 0, 1);
    sd_write(P4_GPIO_BASE + P4_GPIO_ENABLE1_W1TS, bit);
}
#endif

static void sd_power_on(void)
{
    uint32_t ctrl;

#if P4_BOARD_SD_HAS_POWER_GPIO
    /* Hold the external switch off while its upstream 3.3-V rail starts. */
    sd_gpio_output(P4_BOARD_SD_POWER_GPIO, 0);
#endif

    ctrl = sd_read(P4_PMU_BASE + P4_PMU_LDO4_CTRL);
    ctrl &= ~(P4_PMU_LDO_XPD | P4_PMU_LDO_TIEH_SEL_M);
    ctrl |= P4_PMU_LDO_FORCE_SW | P4_PMU_LDO_3V3;
    sd_write(P4_PMU_BASE + P4_PMU_LDO4_CTRL, ctrl);
    sd_write(P4_PMU_BASE + P4_PMU_LDO4_ANA,
             sd_read(P4_PMU_BASE + P4_PMU_LDO4_ANA) |
             P4_PMU_LDO_EN_VDET);
    sd_write(P4_PMU_BASE + P4_PMU_LDO4_CTRL, ctrl | P4_PMU_LDO_XPD);

    /* Seeed's own board init gives the external switch a full 100-ms
       low pulse before applying card power. */
    sd_delay(SD_POWER_DELAY_US);
#if P4_BOARD_SD_HAS_POWER_GPIO
    sd_gpio_output(P4_BOARD_SD_POWER_GPIO, 1);
    sd_delay(SD_POWER_DELAY_US);
#endif
}

static void sd_configure_pins(void)
{
    unsigned int gpio;

    /* IDF starts at drive strength 3; the board BSP lowers it after mount. */
    for (gpio = P4_SD_D0_GPIO; gpio <= P4_SD_CMD_GPIO; ++gpio)
        sd_iomux(gpio, P4_IOMUX_FUNC_SDMMC, 1, 3);
}

static void sd_host_clock(void)
{
    uint32_t value;

    value = sd_read(P4_HP_SYS_CLKRST_BASE + P4_HP_SOC_CLK_CTRL1);
    sd_write(P4_HP_SYS_CLKRST_BASE + P4_HP_SOC_CLK_CTRL1,
             value | P4_HP_SDMMC_CLK_EN);

    value = sd_read(P4_HP_SYS_CLKRST_BASE + P4_HP_REF_CLK_CTRL2);
    sd_write(P4_HP_SYS_CLKRST_BASE + P4_HP_REF_CLK_CTRL2,
             value | P4_HP_REF_160M_CLK_EN);

    value = sd_read(P4_HP_SYS_CLKRST_BASE + P4_HP_PERI_CLK_CTRL01);
    value &= ~(P4_HP_SDIO_HS_MODE | P4_HP_SDIO_CLK_SRC);
    value |= P4_HP_SDIO_CLK_EN;       /* PLL160M, low-speed path */
    sd_write(P4_HP_SYS_CLKRST_BASE + P4_HP_PERI_CLK_CTRL01, value);

    /* 160 MHz / 10.  Card divider 20 below yields exactly 400 kHz. */
    value = sd_read(P4_HP_SYS_CLKRST_BASE + P4_HP_PERI_CLK_CTRL02);
    value &= ~P4_HP_SDIO_FIELDS_M;
    value |= (9U << P4_HP_SDIO_EDGE_L_S) |
             (4U << P4_HP_SDIO_EDGE_H_S) |
             (9U << P4_HP_SDIO_EDGE_N_S) |
             (1U << P4_HP_SDIO_DRV_EDGE_S) |
             P4_HP_SDIO_SLF_EN | P4_HP_SDIO_DRV_EN |
             P4_HP_SDIO_SAM_EN;
    sd_write(P4_HP_SYS_CLKRST_BASE + P4_HP_PERI_CLK_CTRL02,
             value | P4_HP_SDIO_UPDATE);
    sd_write(P4_HP_SYS_CLKRST_BASE + P4_HP_PERI_CLK_CTRL02, value);

    value = sd_read(P4_LP_CLKRST_BASE + P4_LP_SDMMC_RST_CTRL);
    sd_write(P4_LP_CLKRST_BASE + P4_LP_SDMMC_RST_CTRL,
             value | P4_LP_SDMMC_RST_EN);
    sd_write(P4_LP_CLKRST_BASE + P4_LP_SDMMC_RST_CTRL,
             value & ~P4_LP_SDMMC_RST_EN);
    sd_delay(10);
}

static int sd_clock_update(void)
{
    uint32_t command = SD_CMD_UPDATE_CLK | SD_CMD_WAIT_DATA |
                       SD_CMD_USE_HOLD | SD_CMD_START;

    if (!sd_wait_clear(SD_CMD, SD_CMD_START, SD_CMD_TIMEOUT_US))
        return 0;
    sd_set(SD_RINTSTS, 0xFFFFFFFFU);
    sd_set(SD_CMDARG, 0);
    sd_set(SD_CMD, command);
    if (!sd_wait_clear(SD_CMD, SD_CMD_START, SD_CMD_TIMEOUT_US))
        return 0;
    if (sd_reg(SD_RINTSTS) & SD_INT_HLE)
        return 0;
    sd_set(SD_RINTSTS, 0xFFFFFFFFU);
    return 1;
}

static int sd_host_init(void)
{
    sd_host_clock();

    sd_set(SD_CTRL, sd_reg(SD_CTRL) | SD_CTRL_RESET_ALL);
    if (!sd_wait_clear(SD_CTRL, SD_CTRL_RESET_ALL, SD_CMD_TIMEOUT_US))
        return 0;

    sd_set(SD_CTRL, 0);       /* polling, no IRQ and no DMA */
    sd_set(SD_BMOD, 0);
    sd_set(SD_IDINTEN, 0);
    sd_set(SD_IDSTS, 0xFFFFFFFFU);
    sd_set(SD_INTMASK, 0);
    sd_set(SD_RINTSTS, 0xFFFFFFFFU);
    sd_set(SD_RST_N, 1);
    sd_set(SD_CTYPE, 0);      /* slot 0, one data bit */
    sd_set(SD_TMOUT, 0xFFFFFFFFU);
    sd_set(SD_FIFOTH, 15U << 16);
    sd_set(SD_CARDTHRCTL, 0);
    sd_set(SD_CLK_EDGE_SEL, 0);

    sd_set(SD_CLKENA, 0);
    if (!sd_clock_update())
        return 0;
    sd_set(SD_CLKDIV, 20);
    sd_set(SD_CLKSRC, 0);
    if (!sd_clock_update())
        return 0;
    sd_set(SD_CLKENA, 1U | (1U << 16));
    return sd_clock_update();
}

/*
 * Execute one SD command.  There is intentionally no write-data path: a
 * caller can request either no data or exactly one 512-byte read.  This
 * makes the bring-up probe incapable of changing the card's media even if
 * a future call site passes the wrong opcode.
 */
static int sd_command(unsigned int index, uint32_t argument, int response,
                      int response_long, int response_crc,
                      uint32_t out_response[4], uint32_t *read_words,
                      uint32_t *raw_out)
{
    uint32_t command = index & 0x3FU;
    uint32_t words = 0;
    int command_done = 0;
    uint64_t deadline;

    if (response)
        command |= SD_CMD_RESP;
    if (response_long)
        command |= SD_CMD_RESP_LONG;
    if (response_crc)
        command |= SD_CMD_RESP_CRC;
    if (read_words)
        command |= SD_CMD_DATA;
    if (index == 0)
        command |= SD_CMD_SEND_INIT;
    else
        command |= SD_CMD_WAIT_DATA;
    command |= SD_CMD_USE_HOLD | SD_CMD_START;

    if (!sd_wait_clear(SD_CMD, SD_CMD_START, SD_CMD_TIMEOUT_US))
        return 0;

    if (read_words)
    {
        sd_set(SD_CTRL, sd_reg(SD_CTRL) | (1U << 1));
        if (!sd_wait_clear(SD_CTRL, 1U << 1, SD_CMD_TIMEOUT_US))
            return 0;
        sd_set(SD_BLKSIZ, 512);
        sd_set(SD_BYTCNT, 512);
    }

    sd_set(SD_RINTSTS, 0xFFFFFFFFU);
    sd_set(SD_CMDARG, argument);
    sd_set(SD_CMD, command);
    deadline = sd_deadline(read_words ? SD_DATA_TIMEOUT_US :
                                      SD_CMD_TIMEOUT_US);

    for (;;)
    {
        uint32_t raw = sd_reg(SD_RINTSTS);
        uint32_t errors = SD_INT_CMD_ERRORS;

        if (read_words)
            errors |= SD_INT_DATA_ERRORS;
        if (raw & errors)
        {
            if (raw_out)
                *raw_out = raw;
            sd_set(SD_RINTSTS, raw);
            return 0;
        }

        if (read_words)
        {
            uint32_t available = (sd_reg(SD_STATUS) >> SD_STATUS_FIFO_S) &
                                 SD_STATUS_FIFO_M;

            while (available && words < 128)
            {
                read_words[words++] = sd_reg(SD_BUFFIFO);
                --available;
            }
            if (raw & SD_INT_RXDR)
                sd_set(SD_RINTSTS, SD_INT_RXDR);
        }

        if (raw & SD_INT_CMD_DONE)
        {
            unsigned int i;

            command_done = 1;
            if (out_response)
                for (i = 0; i < (response_long ? 4U : 1U); ++i)
                    out_response[i] = sd_reg(SD_RESP0 + i * 4);
            sd_set(SD_RINTSTS, SD_INT_CMD_DONE);
            if (!read_words)
                return 1;
        }

        if (read_words && (raw & SD_INT_DATA_OVER))
        {
            uint32_t available = (sd_reg(SD_STATUS) >> SD_STATUS_FIFO_S) &
                                 SD_STATUS_FIFO_M;

            while (available && words < 128)
            {
                read_words[words++] = sd_reg(SD_BUFFIFO);
                --available;
            }
            sd_set(SD_RINTSTS, SD_INT_DATA_OVER);
            if (raw_out)
                *raw_out = raw;
            return command_done && words == 128;
        }

        if (sd_expired(deadline))
        {
            if (raw_out)
                *raw_out = raw;
            return 0;
        }
    }
}

static int sd_wait_not_busy(unsigned long usec)
{
    uint64_t deadline = sd_deadline(usec);

    while (sd_reg(SD_STATUS) & SD_STATUS_BUSY)
        if (sd_expired(deadline))
            return 0;
    return 1;
}

static uint32_t sd_le32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void sd_report_sector(void)
{
    const unsigned char *sector = (const unsigned char *)sd_sector0;
    unsigned int i;

    krnP4PutStr("[sdmmc]  sector 0 words ");
    for (i = 0; i < 4; ++i)
    {
        if (i)
            krnP4PutStr(" ");
        krnP4PutHex32(sd_sector0[i]);
    }
    krnP4PutStr("\n[sdmmc]  signature ");
    krnP4PutStr(sector[510] == 0x55 && sector[511] == 0xAA ?
                "55aa\n" : "absent\n");

    for (i = 0; i < 4; ++i)
    {
        const unsigned char *part = sector + 446 + i * 16;
        uint32_t lba = sd_le32(part + 8);
        uint32_t count = sd_le32(part + 12);

        if (part[4] == 0 && count == 0)
            continue;
        krnP4PutStr("[sdmmc]  partition ");
        krnP4PutDec(i);
        krnP4PutStr(" type ");
        krnP4PutHex32(part[4]);
        krnP4PutStr(" lba ");
        krnP4PutDec(lba);
        krnP4PutStr(" sectors ");
        krnP4PutDec(count);
        krnP4PutStr("\n");
    }
}

void krnP4SDMMCProbe(void)
{
    uint32_t response[4] = { 0, 0, 0, 0 };
    uint32_t raw = 0;
    uint32_t ocr = 0;
    uint32_t rca;
    uint64_t acmd_deadline;
    int cmd8;

#if P4_BOARD_SD_HAS_POWER_GPIO
    krnP4PutStr("[sdmmc]  read-only probe: power LDO4/GPIO46\n");
#else
    krnP4PutStr("[sdmmc]  read-only probe: power LDO4\n");
#endif
    sd_power_on();

#if P4_BOARD_SD_HAS_DETECT
    krnP4PutStr("[sdmmc]  detect GPIO45 is ");
#else
    krnP4PutStr("[sdmmc]  card detect is ");
#endif
    if (!sd_card_present())
    {
        krnP4PutStr("high (no card)\n");
#ifndef P4_SDMMC_IGNORE_CD
        return;
#else
        /*
         * Diagnostic only: every command below is read-only and bounded.
         * Continuing lets bring-up distinguish a failed mechanical CD
         * contact from a missing or electrically unreachable card.
         */
        krnP4PutStr("[sdmmc]  ignoring card-detect for read-only probe\n");
#endif
    }
    else
#if P4_BOARD_SD_HAS_DETECT
        krnP4PutStr("low (card inserted)\n");
#else
        krnP4PutStr("not wired; trying the card\n");
#endif

    sd_configure_pins();
    if (!sd_host_init())
    {
        krnP4PutStr("[sdmmc]  host clock/reset failed\n");
        return;
    }
    krnP4PutStr("[sdmmc]  slot 0, 1-bit, 400 kHz, LDO4 3.3 V\n");

    if (!sd_command(0, 0, 0, 0, 0, NULL, NULL, &raw))
    {
        krnP4PutStr("[sdmmc]  CMD0 failed, raw ");
        krnP4PutHex32(raw);
        krnP4PutStr("\n");
        return;
    }

    raw = 0;
    cmd8 = sd_command(8, 0x1AA, 1, 0, 1, response, NULL, &raw) &&
           ((response[0] & 0xFFF) == 0x1AA);
    krnP4PutStr(cmd8 ? "[sdmmc]  CMD8 accepted\n" :
                       "[sdmmc]  CMD8 absent; trying legacy voltage range\n");

    acmd_deadline = sd_deadline(SD_ACMD41_LIMIT_US);
    do
    {
        if (!sd_command(55, 0, 1, 0, 1, response, NULL, &raw) ||
            !sd_command(41, (cmd8 ? SD_OCR_CCS : 0) | 0x00FF8000U,
                        1, 0, 0, response, NULL, &raw))
        {
            krnP4PutStr("[sdmmc]  ACMD41 failed, raw ");
            krnP4PutHex32(raw);
            krnP4PutStr("\n");
            return;
        }
        ocr = response[0];
        if (ocr & SD_OCR_BUSY)
            break;
        sd_delay(10000);
    } while (!sd_expired(acmd_deadline));

    if (!(ocr & SD_OCR_BUSY))
    {
        krnP4PutStr("[sdmmc]  card stayed busy, OCR ");
        krnP4PutHex32(ocr);
        krnP4PutStr("\n");
        return;
    }
    krnP4PutStr("[sdmmc]  OCR ");
    krnP4PutHex32(ocr);
    krnP4PutStr((ocr & SD_OCR_CCS) ? " (SDHC/SDXC)\n" : " (SDSC)\n");

    if (!sd_command(2, 0, 1, 1, 1, response, NULL, &raw))
    {
        krnP4PutStr("[sdmmc]  CMD2 failed\n");
        return;
    }
    krnP4PutStr("[sdmmc]  CID ");
    krnP4PutHex32(response[3]);
    krnP4PutStr(" ");
    krnP4PutHex32(response[2]);
    krnP4PutStr(" ");
    krnP4PutHex32(response[1]);
    krnP4PutStr(" ");
    krnP4PutHex32(response[0]);
    krnP4PutStr("\n");

    if (!sd_command(3, 0, 1, 0, 1, response, NULL, &raw))
    {
        krnP4PutStr("[sdmmc]  CMD3 failed\n");
        return;
    }
    rca = response[0] >> 16;
    krnP4PutStr("[sdmmc]  RCA ");
    krnP4PutHex32(rca);
    krnP4PutStr("\n");

    if (!sd_command(9, rca << 16, 1, 1, 1, response, NULL, &raw))
    {
        krnP4PutStr("[sdmmc]  CMD9 failed\n");
        return;
    }
    krnP4PutStr("[sdmmc]  CSD ");
    krnP4PutHex32(response[3]);
    krnP4PutStr(" ");
    krnP4PutHex32(response[2]);
    krnP4PutStr(" ");
    krnP4PutHex32(response[1]);
    krnP4PutStr(" ");
    krnP4PutHex32(response[0]);
    krnP4PutStr("\n");

    if (!sd_command(7, rca << 16, 1, 0, 1, response, NULL, &raw) ||
        !sd_wait_not_busy(SD_DATA_TIMEOUT_US))
    {
        krnP4PutStr("[sdmmc]  CMD7/select failed\n");
        return;
    }

    if (!(ocr & SD_OCR_CCS) &&
        !sd_command(16, 512, 1, 0, 1, response, NULL, &raw))
    {
        krnP4PutStr("[sdmmc]  CMD16 failed\n");
        return;
    }

    raw = 0;
    if (!sd_command(17, 0, 1, 0, 1, response, sd_sector0, &raw))
    {
        krnP4PutStr("[sdmmc]  CMD17/read sector 0 failed, raw ");
        krnP4PutHex32(raw);
        krnP4PutStr(" status ");
        krnP4PutHex32(sd_reg(SD_STATUS));
        krnP4PutStr("\n");
        return;
    }

    sd_report_sector();
    krnP4PutStr("[sdmmc]  read-only probe complete; no media write issued\n");
}
