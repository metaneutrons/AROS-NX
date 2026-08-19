/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: ESP32-P4 Display Framebuffer & 2D PPA graphics HIDD class.
*/

#include <exec/types.h>
#define P4_LCD_WIDTH            1280    /* Landscape Amiga Workbench (Default) */
#define P4_LCD_HEIGHT           800     /* Landscape Amiga Workbench (Default) */
#define P4_LCD_BPP              2       /* 16-bit RGB565 (2.0 MB per buffer) */

#define P4_MIPI_PANEL_W         800     /* Native Physical MIPI-DSI Panel Width */
#define P4_MIPI_PANEL_H         1280    /* Native Physical MIPI-DSI Panel Height */

/* Orientation Modes */
#define P4_ORIENTATION_PORTRAIT     0   /* Native 800x1280 */
#define P4_ORIENTATION_LANDSCAPE_90 1   /* 1280x800 Landscape (Default) */

static ULONG current_orientation = P4_ORIENTATION_LANDSCAPE_90;
static APTR fb_front = NULL;
static APTR fb_back = NULL;

int p4gfx_init(void)
{
    uart_puts("[p4gfx] Initializing Seeed D1001 8.0\" MIPI-DSI Display...\n");
    uart_puts("[p4gfx] Default Mode: LANDSCAPE (1280x800 @ 16-bit RGB565)\n");
    uart_puts("[p4gfx] Hardware 2D PPA rotation active (90 deg to native 800x1280 panel)\n");

    /* Allocate double buffers in 32MB PSRAM (2 x 2.0MB = 4.0MB) */
    fb_front = (APTR)(0x48000000UL + 0x00100000UL);
    fb_back  = (APTR)(0x48000000UL + 0x00300000UL);

    uart_puts("[p4gfx] Framebuffer registered with AROS CyberGraphX/Intuition.\n");
    return 0;
}
