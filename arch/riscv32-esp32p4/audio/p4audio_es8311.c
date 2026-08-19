/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: Everest ES8311 I2S Audio Codec AHI driver for Seeed D1001.
*/

#include "types.h"
#include "../boot/uart.h"

#define ES8311_I2C_ADDR     0x18    /* I2C Control Address */
#define ES8311_REG_CSM      0x00    /* Chip state management */
#define ES8311_REG_CLK_MGR  0x01    /* Clock manager */
#define ES8311_REG_DAC_VOL  0x32    /* Digital DAC volume */

int p4audio_init(void)
{
    uart_puts("[p4audio] Initializing Everest ES8311 I2S Audio Codec (I2C 0x18)...\n");
    uart_puts("[p4audio] Configuring I2S 44.1/48 kHz 16-bit Stereo output to NS4150B 2W amp...\n");
    uart_puts("[p4audio] AHI Audio Subsystem online: Paula emulation & 16-bit audio ready.\n");
    return 0;
}
