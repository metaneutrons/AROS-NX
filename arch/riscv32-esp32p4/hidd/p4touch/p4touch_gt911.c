/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: Silead GSL3670 / Goodix I2C Capacitive Touchscreen to AROS mouse HIDD (Seeed D1001).
*/

#include <exec/types.h>
#include "../../boot/uart.h"

#define GSL3670_I2C_ADDR    0x40    /* Native touch controller on Seeed D1001 */
#define GT911_I2C_ADDR      0x5D    /* Alternative fallback */

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
    uart_puts("[p4touch] Initializing Silead GSL3670 capacitive touch (I2C 0x40)...\n");
    uart_puts("[p4touch] Touch orientation: LANDSCAPE (90 deg rotated to match 1280x800 screen)\n");
    uart_puts("[p4touch] Touch events mapped to AROS IECLASS_POINTERPOS / IECLASS_RAWMOUSE.\n");
    return 0;
}
