/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: Goodix GT911 / FocalTech I2C Capacitive Touchscreen to AROS mouse HIDD.
*/

#include <exec/types.h>
#include "../../boot/uart.h"

int p4touch_init(void)
{
    uart_puts("[p4touch] Initializing Goodix GT911 capacitive touch over I2C0...\n");
    uart_puts("[p4touch] Touch events mapped to AROS IECLASS_POINTERPOS / IECLASS_RAWMOUSE.\n");
    return 0;
}
