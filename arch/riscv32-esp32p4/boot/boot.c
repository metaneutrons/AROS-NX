/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: Main C kernel boot entry for ESP32-P4 (Seeed D1001).
*/

#include "types.h"
#include "board.h"
#include "uart.h"

/* Memory map constants for ESP32-P4 */
#define ESP32P4_SRAM_BASE       0x4FF00000UL
#define ESP32P4_SRAM_SIZE       (768 * 1024)        /* 768 KB */

#define ESP32P4_PSRAM_BASE      0x48000000UL
#define ESP32P4_PSRAM_SIZE      (32 * 1024 * 1024)  /* 32 MB */

extern void platform_init(void);

void c_boot(void)
{
    const struct ESP32P4_Board *board = get_active_board();

    uart_init(115200);
    uart_puts("\n========================================\n");
    uart_puts("   AROS Research Operating System\n");
    uart_puts("   Architecture: RISC-V 32-bit (RV32IMAFDC)\n");
    uart_puts("   Target: Espressif ESP32-P4 SoC\n");
    if (board && board->description)
    {
        uart_puts("   Board:  ");
        uart_puts(board->description);
        uart_puts("\n");
    }
    uart_puts("   Flash: 32MB Dual-Bank OTA | PSRAM: 32MB\n");
    uart_puts("========================================\n\n");

    uart_puts("[boot] Initializing platform timers and interrupt matrix...\n");
    platform_init();

    if (board && board->init_board)
        board->init_board();

    uart_puts("[boot] Configuring Amiga memory pools:\n");
    uart_puts("  - Internal SRAM: 768 KB (MEMF_LOCAL)\n");
    uart_puts("  - Octal PSRAM:   32 MB  (MEMF_PUBLIC | MEMF_FAST)\n");

    /* Scan and initialize all Kickstart ROM Resident drivers by priority */
    extern void exec_init_coldstart_residents(void);
    exec_init_coldstart_residents();

    uart_puts("[boot] Launching exec.library scheduler...\n");
}
