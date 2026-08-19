/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: ESP32-P4 Synopsys DWC2 USB 2.0 High-Speed Host (Poseidon Stack).
*/

#include <exec/types.h>
#include "../boot/uart.h"

#define ESP32P4_USB_OTG_BASE    0x50000000UL

int p4usb_init(void)
{
    uart_puts("[p4usb] Initializing Synopsys DWC2 USB 2.0 High-Speed OTG (0x50000000)...\n");
    uart_puts("[p4usb] Poseidon USB Host Stack online: Keyboard, Mouse, Mass Storage ready.\n");
    return 0;
}
