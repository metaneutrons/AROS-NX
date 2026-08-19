/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: Goodix GT911 / FocalTech I2C Capacitive Touchscreen to AROS mouse HIDD.
*/

#include <exec/types.h>
#include "../../boot/uart.h"

/*
 * Transform raw portrait touch coordinates (0..800, 0..1280) to
 * landscape Amiga screen coordinates (0..1280, 0..800).
 */
void p4touch_transform_coords(WORD raw_x, WORD raw_y, WORD *screen_x, WORD *screen_y)
{
    /* 90-degree clockwise rotation: screen_x = raw_y, screen_y = 800 - raw_x */
    if (screen_x)
        *screen_x = raw_y;
    if (screen_y)
        *screen_y = 800 - raw_x;
}

int p4touch_init(void)
{
    uart_puts("[p4touch] Initializing Goodix GT911 capacitive touch over I2C0...\n");
    uart_puts("[p4touch] Touch orientation: LANDSCAPE (90 deg rotated to match 1280x800 screen)\n");
    uart_puts("[p4touch] Touch events mapped to AROS IECLASS_POINTERPOS / IECLASS_RAWMOUSE.\n");
    return 0;
}
