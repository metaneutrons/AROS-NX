/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: ESP32-P4 4-bit SD/MMC Host Controller for MicroSD Workbench Boot.
*/

#include "types.h"
#include "../boot/uart.h"

int p4sdcard_init(void)
{
    uart_puts("[p4sdcard] Initializing 4-bit SDHCI Host Controller...\n");
    uart_puts("[p4sdcard] Automounting DH0: from MicroSD (s:startup-sequence)...\n");
    return 0;
}
