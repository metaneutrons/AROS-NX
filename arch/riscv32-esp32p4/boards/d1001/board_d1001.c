/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: Seeed Studio reTerminal D1001 Board Profile (ESP32-P4).
*/

#include "types.h"
#include "../../include/board.h"
#include "../../boot/uart.h"

extern int p4gfx_init(void);
extern int p4touch_init(void);
extern int p4audio_init(void);
extern int p4camera_init(void);
extern int p4usb_init(void);
extern int p4sdcard_init(void);

static int d1001_init_board(void)
{
    uart_puts("[board] Activated Profile: Seeed Studio reTerminal D1001\n");
    uart_puts("[board] Display: 8.0\" MIPI-DSI (1280x800 Landscape @ 16-bit)\n");
    uart_puts("[board] Touch: Silead GSL3670 (I2C 0x40)\n");
    uart_puts("[board] RTC: NXP PCF8563T (I2C 0x51)\n");
    uart_puts("[board] Audio: Everest ES8311 DAC + ES7210 Mic (I2C 0x18/0x41)\n");
    uart_puts("[board] Camera: SmartSens SC2356 2MP MIPI-CSI (0x500A0000)\n");

    p4usb_init();
    p4sdcard_init();
    return 0;
}

static const struct ESP32P4_Board d1001_board_descriptor = {
    .name                = "d1001",
    .description         = "Seeed Studio reTerminal D1001 (8.0\" Touch HMI)",
    .display_width       = 1280,
    .display_height      = 800,
    .display_bpp         = 2,
    .default_orientation = 1,   /* Landscape 90 deg */
    .init_board          = d1001_init_board,
    .init_display        = p4gfx_init,
    .init_touch          = p4touch_init,
    .init_audio          = p4audio_init,
    .init_camera         = p4camera_init,
    .init_rtc            = NULL
};

const struct ESP32P4_Board *get_active_board(void)
{
    return &d1001_board_descriptor;
}
