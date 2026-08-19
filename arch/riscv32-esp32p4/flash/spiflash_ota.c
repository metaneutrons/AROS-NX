/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: ESP32-P4 Hardware SPI Flash & Dual-Bank OTA Manager Engine
          for AROS Firmware Upgrade CLI and GUI Subsystems.
*/

#include "types.h"
#include "../include/spiflash_ota.h"
#include "../boot/uart.h"

static int g_running_slot = OTA_SLOT_A;

static inline void wr32(ULONG offset, ULONG val) {
    *(volatile ULONG *)(ESP32P4_SPI1_BASE + offset) = val;
}

static inline ULONG rd32(ULONG offset) {
    return *(volatile ULONG *)(ESP32P4_SPI1_BASE + offset);
}

/* CRC-32 (IEEE 802.3 Ethernet Polynomial 0xEDB88320) for OTA Entry Validation */
static ULONG ota_crc32(const UBYTE *data, ULONG len) {
    ULONG crc = 0xFFFFFFFFUL;
    for (ULONG i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            crc = (crc >> 1) ^ (0xEDB88320UL & (-(crc & 1)));
        }
    }
    return ~crc;
}

static void flash_delay_us(ULONG us) {
    for (volatile ULONG i = 0; i < us * 40; i++) {
        __asm__ volatile ("nop");
    }
}

/* Wait for Flash Write-In-Progress (WIP) to complete */
static int spiflash_wait_idle(void) {
    ULONG count = 0;
    for (;;) {
        /* Send RDSR (0x05) */
        wr32(SPI_USER_REG, (1 << 28) | (1 << 27)); /* Command + Read Data */
        wr32(SPI_USER2_REG, (7 << 28) | SPI_FLASH_CMD_RDSR); /* 8-bit cmd */
        wr32(SPI_CMD_REG, (1 << 18)); /* Start User Command */
        while (rd32(SPI_CMD_REG) & (1 << 18)) ;

        UBYTE status = (UBYTE)(rd32(SPI_W0_REG) & 0xFF);
        if (!(status & 0x01)) break; /* Bit 0 (WIP) cleared */

        if (++count > 2000000) return -1; /* Timeout */
        flash_delay_us(10);
    }
    return 0;
}

/* Send Write Enable Command (0x06) */
static int spiflash_write_enable(void) {
    if (spiflash_wait_idle() < 0) return -1;

    wr32(SPI_USER_REG, (1 << 28)); /* Command only */
    wr32(SPI_USER2_REG, (7 << 28) | SPI_FLASH_CMD_WREN);
    wr32(SPI_CMD_REG, (1 << 18));
    while (rd32(SPI_CMD_REG) & (1 << 18)) ;

    return 0;
}

int spiflash_init(void) {
    uart_puts("[spiflash] Initializing ESP32-P4 Hardware SPI Flash Controller...\n");

    /* Probe active slot: Check if PC is running in Slot A or Slot B */
    ULONG pc;
    __asm__ volatile ("auipc %0, 0" : "=r"(pc));
    if (pc >= (0x40000000UL + OTA_SLOT_B_OFFSET)) {
        g_running_slot = OTA_SLOT_B;
        uart_puts("[spiflash] Active Boot Slot: SLOT B (Offset: 0x00820000)\n");
    } else {
        g_running_slot = OTA_SLOT_A;
        uart_puts("[spiflash] Active Boot Slot: SLOT A (Offset: 0x00020000)\n");
    }

    return 0;
}

int spiflash_read(ULONG flash_offset, void *dst, ULONG len) {
    if (!dst || len == 0) return -1;
    if (spiflash_wait_idle() < 0) return -1;

    /* Read directly via Memory-Mapped Flash Window (0x40000000) */
    const UBYTE *src = (const UBYTE *)(0x40000000UL + flash_offset);
    UBYTE *d = (UBYTE *)dst;
    for (ULONG i = 0; i < len; i++) {
        d[i] = src[i];
    }
    return (int)len;
}

int spiflash_erase_sector(ULONG flash_offset) {
    /* Align to 4KB sector boundary */
    flash_offset &= ~(SPI_FLASH_SECTOR_SIZE - 1);

    if (spiflash_write_enable() < 0) return -1;

    /* Setup Sector Erase Command (0x20) with 24-bit Address */
    wr32(SPI_ADDR_REG, flash_offset << 8);
    wr32(SPI_USER_REG, (1 << 28) | (1 << 26)); /* Command + Address */
    wr32(SPI_USER1_REG, (23 << 26));            /* 24-bit Address */
    wr32(SPI_USER2_REG, (7 << 28) | SPI_FLASH_CMD_SE);
    wr32(SPI_CMD_REG, (1 << 18));
    while (rd32(SPI_CMD_REG) & (1 << 18)) ;

    return spiflash_wait_idle();
}

int spiflash_write(ULONG flash_offset, const void *src, ULONG len) {
    if (!src || len == 0) return -1;

    const UBYTE *s = (const UBYTE *)src;
    ULONG written = 0;

    while (written < len) {
        /* Compute bytes to write within current 256-byte page */
        ULONG page_offset = (flash_offset + written) % SPI_FLASH_PAGE_SIZE;
        ULONG chunk = SPI_FLASH_PAGE_SIZE - page_offset;
        if (chunk > (len - written)) chunk = (len - written);
        if (chunk > 64) chunk = 64; /* 64 bytes per hardware FIFO burst */

        if (spiflash_write_enable() < 0) return -1;

        /* Write data to FIFO registers (W0-W15) */
        ULONG addr = (flash_offset + written);
        wr32(SPI_ADDR_REG, addr << 8);

        ULONG words = (chunk + 3) / 4;
        for (ULONG w = 0; w < words; w++) {
            ULONG val = 0;
            for (int b = 0; b < 4; b++) {
                ULONG byte_idx = written + w * 4 + b;
                if (byte_idx < len) {
                    val |= ((ULONG)s[byte_idx]) << (b * 8);
                }
            }
            wr32(SPI_W0_REG + w * 4, val);
        }

        /* Setup Page Program Command (0x02) */
        wr32(SPI_USER_REG, (1 << 28) | (1 << 26) | (1 << 25)); /* Command + Addr + MOSI */
        wr32(SPI_USER1_REG, (23 << 26) | ((chunk * 8 - 1) << 17));
        wr32(SPI_USER2_REG, (7 << 28) | SPI_FLASH_CMD_PP);
        wr32(SPI_CMD_REG, (1 << 18));
        while (rd32(SPI_CMD_REG) & (1 << 18)) ;

        if (spiflash_wait_idle() < 0) return -1;

        written += chunk;
    }

    return (int)written;
}

int ota_get_running_slot(void) {
    return g_running_slot;
}

int ota_get_target_slot(void) {
    return (g_running_slot == OTA_SLOT_A) ? OTA_SLOT_B : OTA_SLOT_A;
}

ULONG ota_get_slot_offset(int slot) {
    return (slot == OTA_SLOT_B) ? OTA_SLOT_B_OFFSET : OTA_SLOT_A_OFFSET;
}

int ota_write_target_slot(ULONG slot_offset, const void *data, ULONG len) {
    ULONG target_base = ota_get_slot_offset(ota_get_target_slot());
    ULONG abs_offset = target_base + slot_offset;

    /* Auto-erase 4KB sectors as we cross boundaries */
    if ((abs_offset % SPI_FLASH_SECTOR_SIZE) == 0) {
        if (spiflash_erase_sector(abs_offset) < 0) {
            return -1;
        }
    }

    return spiflash_write(abs_offset, data, len);
}

int ota_activate_target_slot(void) {
    int target_slot = ota_get_target_slot();

    /* Read current otadata sector */
    struct esp_ota_select_entry entry;
    struct esp_ota_select_entry cur_entry;
    spiflash_read(OTA_DATA_PARTITION_OFFSET, &cur_entry, sizeof(cur_entry));

    ULONG next_seq = (cur_entry.ota_seq == 0xFFFFFFFFUL) ? 1 : (cur_entry.ota_seq + 1);

    entry.ota_seq = next_seq;
    entry.ota_state = 0; /* ESP_OTA_IMG_VALID */
    entry.crc = ota_crc32((const UBYTE *)&entry.ota_seq, sizeof(entry.ota_seq));

    for (int i = 0; i < 20; i++) entry.seq_label[i] = 0;
    const char *lbl = (target_slot == OTA_SLOT_B) ? "aros_b" : "aros_a";
    for (int i = 0; lbl[i]; i++) entry.seq_label[i] = lbl[i];

    /* Erase otadata sector and write updated entry */
    spiflash_erase_sector(OTA_DATA_PARTITION_OFFSET);
    spiflash_write(OTA_DATA_PARTITION_OFFSET, &entry, sizeof(entry));

    uart_puts("[ota] Switched active bootloader slot to: ");
    uart_puts(lbl);
    uart_puts(" (Sequence: ");
    uart_puthex(next_seq);
    uart_puts(")\n");

    return 0;
}

int ota_reboot(void) {
    uart_puts("[ota] System rebooting into new OTA slot...\n");
    flash_delay_us(100000);

    /* Trigger ESP32-P4 Software System Reset */
    *(volatile ULONG *)(0x500C0000UL + 0x0000) = (1 << 0);
    for (;;) ;
    return 0;
}
