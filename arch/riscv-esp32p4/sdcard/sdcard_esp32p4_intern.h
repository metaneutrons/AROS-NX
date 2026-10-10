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
#include "../board/board.h"
#include "../sdmmc/host.h"

#define FNAME_P4SD(x)       ESP32P4SD__Device__ ## x
#define FNAME_P4SDBUS(x)    ESP32P4SD__SDBus__ ## x

struct p4sd_private
{
    ULONG clock_hz;
    BOOL powered;
    BOOL host_ready;
    struct P4SDMMCHost *host;
    ULONG host_generation;
    /* Host-owned protocol session, independent of the mounted unit's RCA.
     * Enumeration may replace this before same-media publication succeeds. */
    ULONG card_rca;
    BOOL recovery_budget; /* This client started the current owned budget. */
};

#define P4SD_PRIV(bus) ((struct p4sd_private *)(IPTR)(bus)->sdcb_Private)

#include "../sdmmc/registers.h"

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
#define P4SD_RECOVERY_BUDGET_US        10000000UL
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
BOOL FNAME_P4SDBUS(BeginTransaction)(struct sdcard_Bus *bus, BOOL may_wait);
void FNAME_P4SDBUS(EndTransaction)(struct sdcard_Bus *bus);
BOOL FNAME_P4SDBUS(RecoveryNeeded)(struct sdcard_Bus *bus);
BOOL FNAME_P4SDBUS(RecoveryPrepare)(struct sdcard_Bus *bus);
BOOL FNAME_P4SDBUS(RecoveryFinish)(struct sdcard_Bus *bus, BOOL success);
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
