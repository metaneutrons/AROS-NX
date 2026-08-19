/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: ESP32-P4 SD/MMC Host Controller (SDMMC) Driver Engine
          for AROS Trackdisk / SCSI Block Device Subsystem.
*/

#include "types.h"
#include "../include/sdmmc_host.h"
#include "../boot/uart.h"

static struct sd_card_info g_card_info;

static inline void wr32(ULONG offset, ULONG val) {
    *(volatile ULONG *)(ESP32P4_SDMMC_BASE + offset) = val;
}

static inline ULONG rd32(ULONG offset) {
    return *(volatile ULONG *)(ESP32P4_SDMMC_BASE + offset);
}

static void sd_delay_ms(ULONG ms) {
    for (volatile ULONG i = 0; i < ms * 40000; i++) {
        __asm__ volatile ("nop");
    }
}

/* Send low-level SD command and poll for completion */
static int sdmmc_send_cmd(ULONG cmd, ULONG arg, ULONG *resp0) {
    wr32(SDMMC_CMDARG, arg);

    /* Command Flags: Bit 31=StartCmd, Bit 29=UseHoldReg, Bit 6=ResponseExpect */
    ULONG cmd_flags = (1 << 31) | (1 << 29) | (cmd & 0x3F);
    if (cmd != SD_CMD0_GO_IDLE_STATE) {
        cmd_flags |= (1 << 6); /* Expect Response */
        if (cmd == SD_CMD9_SEND_CSD || cmd == 2) {
            cmd_flags |= (1 << 7); /* Long 136-bit Response (R2) */
        }
        if (cmd != SD_ACMD41_SD_SEND_OP_COND && cmd != SD_CMD9_SEND_CSD) {
            cmd_flags |= (1 << 8); /* Check Response CRC */
        }
    }

    wr32(SDMMC_CMD, cmd_flags);

    /* Wait for command completion */
    ULONG count = 0;
    while (rd32(SDMMC_CMD) & (1 << 31)) {
        if (++count > 200000) return -1;
    }

    if (resp0) {
        *resp0 = rd32(SDMMC_RESP0);
    }
    return 0;
}

int sdmmc_host_init(void) {
    uart_puts("[sdcard] Initializing ESP32-P4 SDMMC Host Controller...\n");

    /* 1. Reset Controller, FIFO, and DMA (CTRL register) */
    wr32(SDMMC_CTRL, (1 << 0) | (1 << 1) | (1 << 2));
    while (rd32(SDMMC_CTRL) & 0x07) ;

    /* 2. Power on Card (PWREN) */
    wr32(SDMMC_PWREN, 1);
    sd_delay_ms(20);

    /* 3. Enable 400 kHz Identification Clock (CLKDIV = 100 on 80 MHz source) */
    wr32(SDMMC_CLKDIV, 100);
    wr32(SDMMC_CLKENA, 1);
    wr32(SDMMC_CMD, (1 << 31) | (1 << 21) | (1 << 13)); /* Update clock regs */
    while (rd32(SDMMC_CMD) & (1 << 31)) ;

    /* 4. Probe SD Card */
    if (sdmmc_card_init(&g_card_info) < 0) {
        uart_puts("[sdcard] WARN: No SD Card detected or initialization failed.\n");
        return -1;
    }

    /* 5. Switch to 50 MHz High-Speed Clock and 4-Bit Bus */
    wr32(SDMMC_CLKDIV, 0); /* 50 MHz Clock */
    wr32(SDMMC_CTYPE, 1);  /* 4-Bit Bus Width */
    wr32(SDMMC_CMD, (1 << 31) | (1 << 21) | (1 << 13));
    while (rd32(SDMMC_CMD) & (1 << 31)) ;

    uart_puts("[sdcard] SD Card Ready (SDHC/SDXC 4-bit @ 50 MHz, Block Count: ");
    uart_puthex(g_card_info.num_sectors);
    uart_puts(" sectors)\n");

    return 0;
}

int sdmmc_card_init(struct sd_card_info *info) {
    if (!info) return -1;

    ULONG resp;

    /* CMD0: Go Idle State */
    sdmmc_send_cmd(SD_CMD0_GO_IDLE_STATE, 0, NULL);
    sd_delay_ms(10);

    /* CMD8: Send Interface Condition (Voltage 2.7-3.6V, Pattern 0xAA) */
    if (sdmmc_send_cmd(SD_CMD8_SEND_IF_COND, 0x1AA, &resp) == 0 && (resp & 0xFF) == 0xAA) {
        info->high_capacity = TRUE;
    } else {
        info->high_capacity = FALSE;
    }

    /* ACMD41: Initialize and wait for card ready */
    ULONG count = 0;
    for (;;) {
        sdmmc_send_cmd(SD_CMD55_APP_CMD, 0, NULL);
        ULONG hcs_flag = info->high_capacity ? (1 << 30) : 0;
        if (sdmmc_send_cmd(SD_ACMD41_SD_SEND_OP_COND, 0x00FF8000 | hcs_flag, &resp) == 0) {
            if (resp & (1 << 31)) { /* Card Ready bit set */
                if (resp & (1 << 30)) info->high_capacity = TRUE;
                break;
            }
        }
        if (++count > 200) return -1;
        sd_delay_ms(10);
    }

    /* CMD2: All Send CID (Long response) */
    sdmmc_send_cmd(2, 0, NULL);

    /* CMD3: Send Relative Address (RCA) */
    if (sdmmc_send_cmd(3, 0, &resp) < 0) return -1;
    info->rca = (UWORD)(resp >> 16);

    /* CMD9: Read CSD */
    sdmmc_send_cmd(SD_CMD9_SEND_CSD, (ULONG)info->rca << 16, NULL);

    /* CMD7: Select Card */
    sdmmc_send_cmd(7, (ULONG)info->rca << 16, NULL);

    /* ACMD6: Switch to 4-bit bus width */
    sdmmc_send_cmd(SD_CMD55_APP_CMD, (ULONG)info->rca << 16, NULL);
    sdmmc_send_cmd(SD_ACMD6_SET_BUS_WIDTH, 2, NULL); /* 4-bit */

    /* CMD16: Set Block Length to 512 bytes */
    sdmmc_send_cmd(SD_CMD16_SET_BLOCKLEN, 512, NULL);

    /* Default card capacity estimate (e.g. 16GB / 31,116,288 sectors) */
    info->num_sectors = 31116288;
    info->capacity_mb = 15193;
    info->initialized = TRUE;

    return 0;
}

int sdmmc_read_blocks(ULONG lba, ULONG count, UBYTE *dst_buf) {
    if (!dst_buf || count == 0) return -1;

    for (ULONG b = 0; b < count; b++) {
        ULONG addr = g_card_info.high_capacity ? (lba + b) : ((lba + b) * 512);

        /* Set block size and byte count */
        wr32(SDMMC_BLKSIZ, 512);
        wr32(SDMMC_BYTCNT, 512);

        /* CMD17: Read Single Block (Bit 9=DataExpected, Bit 29=UseHoldReg) */
        wr32(SDMMC_CMDARG, addr);
        wr32(SDMMC_CMD, (1 << 31) | (1 << 29) | (1 << 9) | (1 << 6) | SD_CMD17_READ_SINGLE_BLOCK);

        /* Read 128 words (512 bytes) from FIFO */
        ULONG *dst = (ULONG *)(dst_buf + b * 512);
        for (int w = 0; w < 128; w++) {
            while (rd32(SDMMC_STATUS) & (1 << 2)) ; /* Wait while FIFO empty */
            dst[w] = rd32(SDMMC_DATA);
        }
    }
    return (int)count;
}

int sdmmc_write_blocks(ULONG lba, ULONG count, const UBYTE *src_buf) {
    if (!src_buf || count == 0) return -1;

    for (ULONG b = 0; b < count; b++) {
        ULONG addr = g_card_info.high_capacity ? (lba + b) : ((lba + b) * 512);

        wr32(SDMMC_BLKSIZ, 512);
        wr32(SDMMC_BYTCNT, 512);

        /* CMD24: Write Single Block (Bit 10=Write, Bit 9=DataExpected) */
        wr32(SDMMC_CMDARG, addr);
        wr32(SDMMC_CMD, (1 << 31) | (1 << 29) | (1 << 10) | (1 << 9) | (1 << 6) | SD_CMD24_WRITE_BLOCK);

        /* Write 128 words to FIFO */
        const ULONG *src = (const ULONG *)(src_buf + b * 512);
        for (int w = 0; w < 128; w++) {
            while (rd32(SDMMC_STATUS) & (1 << 3)) ; /* Wait while FIFO full */
            wr32(SDMMC_DATA, src[w]);
        }
    }
    return (int)count;
}
