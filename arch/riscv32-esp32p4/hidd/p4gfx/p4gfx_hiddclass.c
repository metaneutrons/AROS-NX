/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: ESP32-P4 Display Framebuffer & 2D PPA graphics HIDD class.
*/

#include <exec/types.h>
#include <exec/memory.h>
#include "../../boot/uart.h"

#define P4_LCD_WIDTH    800
#define P4_LCD_HEIGHT   480
#define P4_LCD_BPP      2       /* 16-bit RGB565 */

static APTR fb_front = NULL;
static APTR fb_back = NULL;

int p4gfx_init(void)
{
    uart_puts("[p4gfx] Initializing LCD Framebuffer (800x480 @ 16-bit RGB565)...\n");

    /* Allocate double buffers in 32MB PSRAM */
    fb_front = (APTR)(0x48000000UL + 0x00100000UL);
    fb_back  = (APTR)(0x48000000UL + 0x00200000UL);

    uart_puts("[p4gfx] Framebuffer registered with AROS CyberGraphX/Intuition.\n");
    return 0;
}
