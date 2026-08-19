/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: ESP32-P4 I2S Audio Controller (Everest ES8311 DAC) Header
          for AROS AHI Audio Device Subsystem.
*/

#ifndef RISCV32_P4AUDIO_I2S_H
#define RISCV32_P4AUDIO_I2S_H

#include "types.h"

/* ESP32-P4 I2S0 Register Base */
#define ESP32P4_I2S0_BASE               0x500A2000UL

#define I2S_TX_CONF_REG                 0x0020
#define I2S_TX_CONF1_REG                0x0024
#define I2S_TX_CLKM_CONF_REG            0x0028
#define I2S_TX_CLKM_DIV_CONF_REG        0x002C
#define I2S_TX_TDM_CTRL_REG             0x0050
#define I2S_TX_SLOT_CONF_REG            0x0054
#define I2S_TX_PCM2PDM_CONF_REG         0x0058
#define I2S_TX_PCM2PDM_CONF1_REG        0x005C

/* Audio DMA Ping-Pong Buffer Size */
#define P4AUDIO_DMA_BUFFER_SAMPLES      1024
#define P4AUDIO_DMA_BUFFER_BYTES        (P4AUDIO_DMA_BUFFER_SAMPLES * 4) /* 16-bit Stereo = 4 bytes/sample */

/* Audio Formats */
#define P4AUDIO_FMT_16BIT_STEREO        0
#define P4AUDIO_FMT_32BIT_STEREO        1

/* Callback function type for AHI mixer feeding */
typedef void (*p4audio_render_callback_t)(WORD *stereo_out_buf, ULONG sample_count, void *user_data);

/* Driver Interface */
int  p4audio_i2s_init(ULONG sample_rate);
void p4audio_i2s_start(p4audio_render_callback_t cb, void *user_data);
void p4audio_i2s_stop(void);
void p4audio_i2s_set_volume(UBYTE vol_0_to_100);

#endif /* RISCV32_P4AUDIO_I2S_H */
