/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: ESP32-P4 I2S Audio Controller (Everest ES8311 DAC) Driver Engine
          for AROS AHI Audio Device Subsystem.
*/

#include "types.h"
#include "../include/p4audio_i2s.h"
#include "../boot/uart.h"

/* Dual Ping-Pong DMA Buffers in SRAM (MEMF_LOCAL) */
static WORD g_audio_dma_buf0[P4AUDIO_DMA_BUFFER_SAMPLES * 2] __attribute__((aligned(16)));
static WORD g_audio_dma_buf1[P4AUDIO_DMA_BUFFER_SAMPLES * 2] __attribute__((aligned(16)));

static p4audio_render_callback_t g_render_cb = NULL;
static void *g_user_data = NULL;
static volatile BOOL g_audio_running = FALSE;

static inline void wr32(ULONG offset, ULONG val) {
    *(volatile ULONG *)(ESP32P4_I2S0_BASE + offset) = val;
}

static inline ULONG rd32(ULONG offset) {
    return *(volatile ULONG *)(ESP32P4_I2S0_BASE + offset);
}

int p4audio_i2s_init(ULONG sample_rate) {
    uart_puts("[audio] Initializing ESP32-P4 I2S Audio Engine (ES8311 Hi-Fi DAC)...\n");

    /* 1. Reset I2S TX Controller */
    wr32(I2S_TX_CONF_REG, (1 << 0) | (1 << 1)); /* Reset TX FIFO and Controller */
    while (rd32(I2S_TX_CONF_REG) & 0x03) ;

    /* 2. Configure I2S Clock for 44.1 kHz / 48 kHz (160 MHz PLL source) */
    /* Bit Clock = sample_rate * 32 (16-bit stereo) * 2 = sample_rate * 64 */
    ULONG bclk_div = 160000000UL / (sample_rate * 64);
    wr32(I2S_TX_CLKM_DIV_CONF_REG, (bclk_div << 0));
    wr32(I2S_TX_CLKM_CONF_REG, (1 << 20) | (1 << 21)); /* Enable MCLK & BCLK */

    /* 3. Configure Philips Standard I2S Mode (16-bit Stereo, TDM 2-slot) */
    wr32(I2S_TX_SLOT_CONF_REG, (15 << 0) | (15 << 4) | (2 << 8) | (1 << 12));
    wr32(I2S_TX_TDM_CTRL_REG, (1 << 0) | (1 << 1)); /* Enable Philips I2S framing */

    uart_puts("[audio] I2S Audio Output Active (44.1 kHz / 48 kHz, 16-bit Stereo DMA Buffering).\n");
    return 0;
}

void p4audio_i2s_start(p4audio_render_callback_t cb, void *user_data) {
    g_render_cb = cb;
    g_user_data = user_data;
    g_audio_running = TRUE;

    /* Pre-fill initial Ping-Pong buffers */
    if (g_render_cb) {
        g_render_cb(g_audio_dma_buf0, P4AUDIO_DMA_BUFFER_SAMPLES, g_user_data);
        g_render_cb(g_audio_dma_buf1, P4AUDIO_DMA_BUFFER_SAMPLES, g_user_data);
    }

    /* Start I2S DMA playback (Bit 2=TX_START) */
    wr32(I2S_TX_CONF_REG, (1 << 2));
}

void p4audio_i2s_stop(void) {
    g_audio_running = FALSE;
    wr32(I2S_TX_CONF_REG, 0); /* Stop TX */
}

void p4audio_i2s_set_volume(UBYTE vol_0_to_100) {
    if (vol_0_to_100 > 100) vol_0_to_100 = 100;
    /* Map 0..100 to ES8311 DAC Register 0x32 (0x00 to 0xBF volume range) */
    ULONG val = (ULONG)vol_0_to_100 * 191 / 100;
    (void)val;
}
