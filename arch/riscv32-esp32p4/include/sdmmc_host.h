/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: ESP32-P4 SD/MMC Host Controller (SDMMC) Driver Header
          for AROS Trackdisk / SCSI Block Device Subsystem.
*/

#ifndef RISCV32_SDMMC_HOST_H
#define RISCV32_SDMMC_HOST_H

#include "types.h"

/* ESP32-P4 SDMMC Register Base (Slot 0 / Slot 1) */
#define ESP32P4_SDMMC_BASE              0x500C2000UL

#define SDMMC_CTRL                      0x0000
#define SDMMC_PWREN                     0x0004
#define SDMMC_CLKDIV                    0x0008
#define SDMMC_CLKSRC                    0x000C
#define SDMMC_CLKENA                    0x0010
#define SDMMC_TMOUT                     0x0014
#define SDMMC_CTYPE                     0x0018
#define SDMMC_BLKSIZ                    0x001C
#define SDMMC_BYTCNT                    0x0020
#define SDMMC_INTMASK                   0x0024
#define SDMMC_CMDARG                    0x0028
#define SDMMC_CMD                       0x002C
#define SDMMC_RESP0                     0x0030
#define SDMMC_RESP1                     0x0034
#define SDMMC_RESP2                     0x0038
#define SDMMC_RESP3                     0x003C
#define SDMMC_MINTSTS                   0x0040
#define SDMMC_RINTSTS                   0x0044
#define SDMMC_STATUS                    0x0048
#define SDMMC_FIFOTH                    0x004C
#define SDMMC_CDETECT                   0x0050
#define SDMMC_WRTPRT                    0x0054
#define SDMMC_BMOD                      0x0080
#define SDMMC_PLDMND                    0x0084
#define SDMMC_DBADDR                    0x0088
#define SDMMC_IDSTS                     0x008C
#define SDMMC_IDINTEN                   0x0090
#define SDMMC_DATA                      0x0100

/* Standard SD Commands */
#define SD_CMD0_GO_IDLE_STATE           0
#define SD_CMD8_SEND_IF_COND            8
#define SD_CMD9_SEND_CSD                9
#define SD_CMD13_SEND_STATUS            13
#define SD_CMD16_SET_BLOCKLEN           16
#define SD_CMD17_READ_SINGLE_BLOCK      17
#define SD_CMD18_READ_MULTIPLE_BLOCK    18
#define SD_CMD24_WRITE_BLOCK            24
#define SD_CMD25_WRITE_MULTIPLE_BLOCK   25
#define SD_CMD55_APP_CMD                55
#define SD_ACMD41_SD_SEND_OP_COND       41
#define SD_ACMD6_SET_BUS_WIDTH          6

/* Internal DMA Descriptor */
struct sdmmc_idma_desc
{
    ULONG   des0;   /* Control / Status flags */
    ULONG   des1;   /* Buffer 1 size */
    ULONG   des2;   /* Buffer 1 physical address */
    ULONG   des3;   /* Next descriptor physical address */
};

/* SD Card Info Structure */
struct sd_card_info
{
    BOOL    initialized;
    BOOL    high_capacity;  /* SDHC/SDXC (Block addressing) */
    UWORD   rca;            /* Relative Card Address */
    ULONG   num_sectors;    /* Total 512-byte sectors */
    ULONG   capacity_mb;    /* Capacity in Megabytes */
};

/* Driver Interface */
int  sdmmc_host_init(void);
int  sdmmc_card_init(struct sd_card_info *info);
int  sdmmc_read_blocks(ULONG lba, ULONG count, UBYTE *dst_buf);
int  sdmmc_write_blocks(ULONG lba, ULONG count, const UBYTE *src_buf);

#endif /* RISCV32_SDMMC_HOST_H */
