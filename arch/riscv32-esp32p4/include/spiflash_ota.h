/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: ESP32-P4 Hardware SPI Flash & Dual-Bank OTA Manager Header
          for AROS Firmware Upgrade CLI and GUI Subsystems.
*/

#ifndef RISCV32_SPIFLASH_OTA_H
#define RISCV32_SPIFLASH_OTA_H

#include "types.h"

/* ESP32-P4 SPI0/SPI1 NOR Flash Controller Base */
#define ESP32P4_SPI0_BASE               0x50080000UL
#define ESP32P4_SPI1_BASE               0x50081000UL

/* Flash Controller Registers */
#define SPI_CMD_REG                     0x0000
#define SPI_ADDR_REG                    0x0004
#define SPI_CTRL_REG                    0x0008
#define SPI_CTRL1_REG                   0x000C
#define SPI_STATUS_REG                  0x0010
#define SPI_USER_REG                    0x0018
#define SPI_USER1_REG                   0x001C
#define SPI_USER2_REG                   0x0020
#define SPI_W0_REG                      0x0058

/* Standard SPI NOR Flash Commands */
#define SPI_FLASH_CMD_WREN              0x06    /* Write Enable */
#define SPI_FLASH_CMD_WRDI              0x04    /* Write Disable */
#define SPI_FLASH_CMD_RDSR              0x05    /* Read Status Register */
#define SPI_FLASH_CMD_SE                0x20    /* Sector Erase (4 KB) */
#define SPI_FLASH_CMD_BE_32K            0x52    /* Block Erase (32 KB) */
#define SPI_FLASH_CMD_BE_64K            0xD8    /* Block Erase (64 KB) */
#define SPI_FLASH_CMD_PP                0x02    /* Page Program (256 B) */
#define SPI_FLASH_CMD_READ              0x03    /* Read Data */
#define SPI_FLASH_CMD_RDID              0x9F    /* Read JEDEC ID */

#define SPI_FLASH_SECTOR_SIZE           4096
#define SPI_FLASH_PAGE_SIZE             256

/* OTA Partition Offsets & Sizes */
#define OTA_DATA_PARTITION_OFFSET       0x00019000UL
#define OTA_DATA_PARTITION_SIZE         0x00002000UL    /* 8 KB (Two 4KB sectors) */

#define OTA_SLOT_A_OFFSET               0x00020000UL    /* aros_a (Slot 0) */
#define OTA_SLOT_A_SIZE                 0x00800000UL    /* 8 MB */

#define OTA_SLOT_B_OFFSET               0x00820000UL    /* aros_b (Slot 1) */
#define OTA_SLOT_B_SIZE                 0x00800000UL    /* 8 MB */

#define OTA_SLOT_INVALID                -1
#define OTA_SLOT_A                      0
#define OTA_SLOT_B                      1

/* ESP-IDF Compatible OTA Selection Data Structure (32 bytes) */
struct esp_ota_select_entry
{
    ULONG   ota_seq;        /* Monotonically increasing sequence number */
    UBYTE   seq_label[20];  /* Label / description */
    ULONG   ota_state;      /* State: 0=VALID, 1=NEW, 2=PENDING_VERIFY */
    ULONG   crc;            /* CRC32 of ota_seq (bits 0..31) */
};

/* Driver Interface */
int  spiflash_init(void);
int  spiflash_read(ULONG flash_offset, void *dst, ULONG len);
int  spiflash_erase_sector(ULONG flash_offset);
int  spiflash_write(ULONG flash_offset, const void *src, ULONG len);

int  ota_get_running_slot(void);
int  ota_get_target_slot(void);
ULONG ota_get_slot_offset(int slot);
int  ota_write_target_slot(ULONG slot_offset, const void *data, ULONG len);
int  ota_activate_target_slot(void);
int  ota_reboot(void);

#endif /* RISCV32_SPIFLASH_OTA_H */
