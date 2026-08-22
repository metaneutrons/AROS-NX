/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Private definitions for the ESP32-P4 native SD/MMC controller on the
    reTerminal D1001.  This driver is deliberately polling-only and read-only;
    CMD18 receives through the controller's bounded IDMAC path.
*/

#ifndef SDCARD_ESP32P4_INTERN_H
#define SDCARD_ESP32P4_INTERN_H

#include <exec/types.h>
#include <utility/tagitem.h>

#include "sdcard_base.h"
#include "sdcard_bus.h"

#define FNAME_P4SD(x)       ESP32P4SD__Device__ ## x
#define FNAME_P4SDBUS(x)    ESP32P4SD__SDBus__ ## x

struct p4sd_private
{
    ULONG clock_hz;
    BOOL powered;
    BOOL host_ready;
};

#define P4SD_PRIV(bus) ((struct p4sd_private *)(IPTR)(bus)->sdcb_Private)

/* ESP32-P4 high-power peripheral windows. */
#define P4_HPPERIPH0_BASE              0x50000000UL
#define P4_HPPERIPH1_BASE              0x500C0000UL
#define P4_LPAON_BASE                  0x50110000UL

/* Synopsys DesignWare MMC, native slot 0. */
#define P4_SDMMC_BASE                  (P4_HPPERIPH0_BASE + 0x83000)
#define P4SD_CTRL                      0x0000
#define  P4SD_CTRL_RESET_ALL           0x00000007UL
#define  P4SD_CTRL_FIFO_RESET          (1UL << 1)
#define  P4SD_CTRL_DMA_RESET           (1UL << 2)
#define  P4SD_CTRL_DMA_ENABLE          (1UL << 5)
/* Bit 25, not 26.  ESP-IDF's own register description for this peripheral
   (components/soc/esp32p4/register/hw_ver1/soc/sdmmc_struct.h) places
   card_voltage_a on 16..19, card_voltage_b on 20..23, enable_od_pullup on 24,
   use_internal_dma on 25 and reserved3 from 26 up.  With bit 26 the internal
   DMAC was never selected on the controller side while BMOD.DE still ran the
   descriptor engine. */
#define  P4SD_CTRL_USE_INTERNAL_DMA    (1UL << 25)
#define P4SD_CLKDIV                    0x0008
#define P4SD_CLKSRC                    0x000C
#define P4SD_CLKENA                    0x0010
#define P4SD_TMOUT                     0x0014
#define P4SD_CTYPE                     0x0018
#define  P4SD_CTYPE_4BIT_SLOT0         (1UL << 0)
#define  P4SD_CTYPE_8BIT_SLOT0         (1UL << 16)
#define P4SD_BLKSIZ                    0x001C
#define P4SD_BYTCNT                    0x0020
#define P4SD_INTMASK                   0x0024
#define P4SD_CMDARG                    0x0028
#define P4SD_CMD                       0x002C
#define  P4SD_CMD_RESP                 (1UL << 6)
#define  P4SD_CMD_RESP_LONG            (1UL << 7)
#define  P4SD_CMD_RESP_CRC             (1UL << 8)
#define  P4SD_CMD_DATA                 (1UL << 9)
#define  P4SD_CMD_WRITE                (1UL << 10)
#define  P4SD_CMD_SEND_AUTO_STOP        (1UL << 12)
#define  P4SD_CMD_STOP_ABORT           (1UL << 14)
#define  P4SD_CMD_WAIT_DATA            (1UL << 13)
#define  P4SD_CMD_SEND_INIT            (1UL << 15)
#define  P4SD_CMD_UPDATE_CLK           (1UL << 21)
#define  P4SD_CMD_USE_HOLD             (1UL << 29)
#define  P4SD_CMD_START                (1UL << 31)
#define P4SD_RESP0                     0x0030
#define P4SD_RESP1                     0x0034
#define P4SD_RESP2                     0x0038
#define P4SD_RESP3                     0x003C
#define P4SD_MINTSTS                   0x0040
#define P4SD_RINTSTS                   0x0044
#define  P4SD_INT_RESP_ERR             (1UL << 1)
#define  P4SD_INT_CMD_DONE             (1UL << 2)
#define  P4SD_INT_DATA_OVER            (1UL << 3)
#define  P4SD_INT_RXDR                 (1UL << 5)
#define  P4SD_INT_RCRC                 (1UL << 6)
#define  P4SD_INT_DCRC                 (1UL << 7)
#define  P4SD_INT_RTO                  (1UL << 8)
#define  P4SD_INT_DTO                  (1UL << 9)
#define  P4SD_INT_HTO                  (1UL << 10)
#define  P4SD_INT_FRUN                 (1UL << 11)
#define  P4SD_INT_HLE                  (1UL << 12)
#define  P4SD_INT_SBE                  (1UL << 13)
#define  P4SD_INT_EBE                  (1UL << 15)
#define  P4SD_INT_CMD_ERRORS           (P4SD_INT_RESP_ERR | P4SD_INT_RCRC | \
                                        P4SD_INT_RTO | P4SD_INT_HLE)
#define  P4SD_INT_DATA_ERRORS          (P4SD_INT_DCRC | P4SD_INT_DTO | \
                                        P4SD_INT_HTO | P4SD_INT_FRUN | \
                                        P4SD_INT_SBE | P4SD_INT_EBE)
#define P4SD_STATUS                    0x0048
#define  P4SD_STATUS_FIFO_EMPTY        (1UL << 2)
#define  P4SD_STATUS_FIFO_FULL         (1UL << 3)
#define  P4SD_STATUS_DATA_BUSY         (1UL << 9)
#define  P4SD_STATUS_FIFO_COUNT_S      17
#define  P4SD_STATUS_FIFO_COUNT_M      0x1FFFUL
#define P4SD_FIFOTH                    0x004C
#define  P4SD_FIFOTH_RX_WMARK          (127UL << 16)
#define  P4SD_FIFOTH_DMA_MSIZE_8       (2UL << 28)
#define P4SD_RST_N                     0x0078
#define P4SD_TCBCNT                    0x005C
#define P4SD_TBBCNT                    0x0060
#define P4SD_BMOD                      0x0080
#define  P4SD_BMOD_SWR                 (1UL << 0)
#define  P4SD_BMOD_FB                  (1UL << 1)
#define  P4SD_BMOD_DE                  (1UL << 7)
#define  P4SD_BMOD_PBL_8               (2UL << 8)
#define P4SD_PLDMND                    0x0084
#define P4SD_DBADDR                    0x0088
#define P4SD_IDSTS                     0x008C
#define  P4SD_IDSTS_TI                 (1UL << 0)
#define  P4SD_IDSTS_RI                 (1UL << 1)
#define  P4SD_IDSTS_FBE                (1UL << 2)
#define  P4SD_IDSTS_DU                 (1UL << 4)
#define  P4SD_IDSTS_CES                (1UL << 5)
#define  P4SD_IDSTS_NI                 (1UL << 8)
#define  P4SD_IDSTS_ERRORS             (P4SD_IDSTS_FBE | P4SD_IDSTS_DU | \
                                         P4SD_IDSTS_CES)
#define P4SD_IDINTEN                   0x0090
#define  P4SD_IDINTEN_TI                (1UL << 0)
#define  P4SD_IDINTEN_RI                (1UL << 1)
#define  P4SD_IDINTEN_NI                (1UL << 8)
#define P4SD_DSCADDR                   0x0094
#define P4SD_BUFADDR                   0x0098
#define P4SD_CARDTHRCTL                0x0100
#define P4SD_BUFFIFO                   0x0200
#define P4SD_CLK_EDGE_SEL              0x0800

/* GPIO and IOMUX.  GPIO45 card-detect is active low. */
#define P4_GPIO_BASE                   (P4_HPPERIPH1_BASE + 0x20000)
#define P4_GPIO_OUT1_W1TS              0x0014
#define P4_GPIO_OUT1_W1TC              0x0018
#define P4_GPIO_ENABLE1_W1TS           0x0030
#define P4_GPIO_ENABLE1_W1TC           0x0034
#define P4_GPIO_IN1                    0x0040

#define P4_IOMUX_BASE                  (P4_HPPERIPH1_BASE + 0x21000)
#define P4_IOMUX_GPIO39                0x00A0
#define  P4_IOMUX_FUN_PD               (1UL << 7)
#define  P4_IOMUX_FUN_PU               (1UL << 8)
#define  P4_IOMUX_FUN_IE               (1UL << 9)
#define  P4_IOMUX_FUN_DRV_S            10
#define  P4_IOMUX_FUN_DRV_M            (3UL << P4_IOMUX_FUN_DRV_S)
#define  P4_IOMUX_MCU_SEL_S            12
#define  P4_IOMUX_MCU_SEL_M            (7UL << P4_IOMUX_MCU_SEL_S)
#define  P4_IOMUX_FUNC_SDMMC           0
#define  P4_IOMUX_FUNC_GPIO            1

#define P4_SD_D0_GPIO                  39
#define P4_SD_D3_GPIO                  42
#define P4_SD_CMD_GPIO                 44
#define P4_SD_DETECT_GPIO              45
#define P4_SD_POWER_GPIO               46

/* SDMMC module gate, PLL160M divider and reset. */
#define P4_HP_SYS_CLKRST_BASE          (P4_HPPERIPH1_BASE + 0x26000)
#define P4_HP_SOC_CLK_CTRL1            0x0018
#define  P4_HP_SDMMC_CLK_EN            (1UL << 14)
#define P4_HP_REF_CLK_CTRL2            0x002C
#define  P4_HP_REF_160M_CLK_EN         (1UL << 0)
#define P4_HP_PERI_CLK_CTRL01          0x0034
#define  P4_HP_SDIO_HS_MODE            (1UL << 22)
#define  P4_HP_SDIO_CLK_SRC            (1UL << 23)
#define  P4_HP_SDIO_CLK_EN             (1UL << 24)
#define P4_HP_PERI_CLK_CTRL02          0x0038
#define  P4_HP_SDIO_UPDATE             (1UL << 8)
#define  P4_HP_SDIO_EDGE_L_S           9
#define  P4_HP_SDIO_EDGE_H_S           13
#define  P4_HP_SDIO_EDGE_N_S           17
#define  P4_HP_SDIO_DRV_EDGE_S         23
#define  P4_HP_SDIO_SLF_EN             (1UL << 27)
#define  P4_HP_SDIO_DRV_EN             (1UL << 28)
#define  P4_HP_SDIO_SAM_EN             (1UL << 29)
#define  P4_HP_SDIO_FIELDS_M           0x3FFFFF00UL

#define P4_LP_CLKRST_BASE              (P4_LPAON_BASE + 0x1000)
#define P4_LP_SDMMC_RST_CTRL           0x004C
#define  P4_LP_SDMMC_RST_EN            (1UL << 28)

/* D1001 SD I/O power: ESP32-P4 LDO channel 4 plus external GPIO46. */
#define P4_PMU_BASE                    (P4_LPAON_BASE + 0x5000)
#define P4_PMU_LDO4_CTRL               0x01D8
#define P4_PMU_LDO4_ANA                0x01DC
#define  P4_PMU_LDO_FORCE_SW           (1UL << 7)
#define  P4_PMU_LDO_XPD                (1UL << 8)
#define  P4_PMU_LDO_TIEH_SEL_M         (7UL << 9)
#define  P4_PMU_LDO_3V3                (1UL << 14)
#define  P4_PMU_LDO_EN_VDET            (1UL << 26)

/* SYSTIMER is the polling timeout source: 16 ticks per microsecond. */
#define P4_SYSTIMER_BASE               (P4_HPPERIPH1_BASE + 0x22000)
#define P4_ST_UNIT0_OP                 0x0004
#define  P4_ST_UNIT0_UPDATE            (1UL << 30)
#define  P4_ST_UNIT0_VALID             (1UL << 29)
#define P4_ST_UNIT0_VALUE_HI           0x0040
#define P4_ST_UNIT0_VALUE_LO           0x0044
#define P4_SYSTIMER_HZ                 16000000UL

#define P4SD_CLOCK_SOURCE_HZ           160000000UL
#define P4SD_CLOCK_MIN_HZ              400000UL
#define P4SD_CLOCK_MAX_HZ              25000000UL

#define P4SD_CMD_TIMEOUT_US            250000UL
#define P4SD_DATA_TIMEOUT_US           5000000UL
#define P4SD_BUSY_TIMEOUT_US           1000000UL
#define P4SD_POWER_DELAY_US            100000UL
/*
 * A1 keeps transfers bounded to the generic sdcard.device chunk size.  The
 * controller remains polling and read-only; larger requests are split by the
 * generic layer before they reach this backend.
 */
#define P4SD_MAX_READ_BLOCKS           128UL
#define P4SD_MAX_DATA_LEN              (P4SD_MAX_READ_BLOCKS * 512UL)
/* The D1001 A1 fallback keeps one bounded descriptor chain while the exact
   v5.4.2 ring experiment remains recorded in ROADMAP.md. */
/* The self-clearing CTRL.fifo_reset bit is an AHB event.  The register
   description requires two system clocks plus a two-card-clock
   synchronisation before the FIFO pointers are valid again, which is 5 us at
   400 kHz.  Programming BLKSIZ, BYTCNT, CTRL, BMOD and the command start
   inside that window left STATUS reporting a full FIFO where the card had
   delivered almost nothing, and the read side then returned one real word
   followed by 127 copies of it.  Twenty microseconds is eight card clocks at
   the current probing frequency. */
#define P4SD_FIFO_RESET_SETTLE_US        20UL

#define P4SD_DMA_DESC_BYTES              512UL
#define P4SD_DMA_DESC_COUNT             (P4SD_MAX_DATA_LEN / P4SD_DMA_DESC_BYTES)

/* A build-only A1 diagnostic hook.  It is off unless passed to the sdcard
   module as P4_SDCARD_FAULT_INJECT=<mode>; it never enables writes. */
#define P4SD_FAULT_NONE                 0
#define P4SD_FAULT_CMD_CRC              1
#define P4SD_FAULT_DATA_CRC             2
#define P4SD_FAULT_TIMEOUT              3
#ifndef P4_SDCARD_FAULT_INJECT
#define P4_SDCARD_FAULT_INJECT          P4SD_FAULT_NONE
#endif

/* This is a diagnostic-only controller trace, not another transfer mode.
   It records the single known-bad CMD18 before emitting output so polling
   timing is unchanged.  The normal package leaves it off. */
#ifndef P4_SDCARD_TRACE
#define P4_SDCARD_TRACE                 0
#endif
#define P4SD_TRACE_LBA                  2048UL

void FNAME_P4SDBUS(SoftReset)(UBYTE mask, struct sdcard_Bus *bus);
void FNAME_P4SDBUS(SetClock)(ULONG speed, struct sdcard_Bus *bus);
void FNAME_P4SDBUS(SetPowerLevel)(ULONG levels, BOOL lowest,
                                  struct sdcard_Bus *bus);
ULONG FNAME_P4SDBUS(SendCmd)(struct TagItem *tags, struct sdcard_Bus *bus);
ULONG FNAME_P4SDBUS(WaitCmd)(ULONG mask, ULONG timeout,
                             struct sdcard_Bus *bus);
ULONG FNAME_P4SDBUS(FinishCmd)(struct TagItem *tags,
                               struct sdcard_Bus *bus);
ULONG FNAME_P4SDBUS(FinishData)(struct TagItem *tags,
                                struct sdcard_Bus *bus);
void FNAME_P4SDBUS(SetBusWidth)(UBYTE width, struct sdcard_Bus *bus);
void FNAME_P4SD(BusInit)(struct sdcard_Bus *bus);
void FNAME_P4SD(BusPostIRQInit)(struct sdcard_Bus *bus);

#endif /* SDCARD_ESP32P4_INTERN_H */
