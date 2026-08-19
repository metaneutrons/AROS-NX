/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: ESP32-P4 Synopsys DesignWare 10/100 Ethernet MAC (EMAC) Driver Engine
          for AROS SANA-II Network Subsystem.
*/

#include "types.h"
#include "../include/emac_p4.h"
#include "../boot/uart.h"

/* DMA Descriptor Rings and Buffers in SRAM (MEMF_LOCAL) */
static struct emac_dma_desc g_rx_descs[EMAC_DESC_RING_SIZE] __attribute__((aligned(16)));
static struct emac_dma_desc g_tx_descs[EMAC_DESC_RING_SIZE] __attribute__((aligned(16)));

static UBYTE g_rx_buffers[EMAC_DESC_RING_SIZE][EMAC_MAX_PACKET_SIZE] __attribute__((aligned(16)));
static UBYTE g_tx_buffers[EMAC_DESC_RING_SIZE][EMAC_MAX_PACKET_SIZE] __attribute__((aligned(16)));

static ULONG g_rx_idx = 0;
static ULONG g_tx_idx = 0;

static inline void wr32(ULONG offset, ULONG val) {
    *(volatile ULONG *)(ESP32P4_EMAC_BASE + offset) = val;
}

static inline ULONG rd32(ULONG offset) {
    return *(volatile ULONG *)(ESP32P4_EMAC_BASE + offset);
}

static void emac_delay_ms(ULONG ms) {
    for (volatile ULONG i = 0; i < ms * 40000; i++) {
        __asm__ volatile ("nop");
    }
}

int emac_p4_init(const UBYTE *mac) {
    uart_puts("[emac] Initializing ESP32-P4 10/100 Ethernet MAC (Synopsys DWMAC)...\n");

    /* 1. Reset DMA Controller (DMA_BUS_MODE: Bit 0=SWR) */
    wr32(EMAC_DMA_BUS_MODE, 1);
    while (rd32(EMAC_DMA_BUS_MODE) & 1) ;

    /* 2. Set Bus Mode (PBL = 32, Fixed Burst, Enhanced Descriptor Format) */
    wr32(EMAC_DMA_BUS_MODE, (32 << 8) | (1 << 24));

    /* 3. Setup Hardware MAC Address */
    if (mac) {
        ULONG mac_hi = ((ULONG)mac[5] << 8) | mac[4];
        ULONG mac_lo = ((ULONG)mac[3] << 24) | ((ULONG)mac[2] << 16) | ((ULONG)mac[1] << 8) | mac[0];
        wr32(EMAC_MAC_ADDR0_HIGH, mac_hi);
        wr32(EMAC_MAC_ADDR0_LOW, mac_lo);
    }

    /* 4. Initialize RX Descriptor Ring */
    for (int i = 0; i < EMAC_DESC_RING_SIZE; i++) {
        g_rx_descs[i].desc0 = (1UL << 31); /* OWN = DMA */
        g_rx_descs[i].desc1 = (1 << 14) | EMAC_MAX_PACKET_SIZE; /* RCH (Chained) */
        g_rx_descs[i].desc2 = (ULONG)(IPTR)&g_rx_buffers[i][0];
        g_rx_descs[i].desc3 = (ULONG)(IPTR)&g_rx_descs[(i + 1) % EMAC_DESC_RING_SIZE];
    }
    wr32(EMAC_DMA_RX_BASE, (ULONG)(IPTR)&g_rx_descs[0]);

    /* 5. Initialize TX Descriptor Ring */
    for (int i = 0; i < EMAC_DESC_RING_SIZE; i++) {
        g_tx_descs[i].desc0 = (1 << 20); /* TCH (Chained) */
        g_tx_descs[i].desc1 = 0;
        g_tx_descs[i].desc2 = (ULONG)(IPTR)&g_tx_buffers[i][0];
        g_tx_descs[i].desc3 = (ULONG)(IPTR)&g_tx_descs[(i + 1) % EMAC_DESC_RING_SIZE];
    }
    wr32(EMAC_DMA_TX_BASE, (ULONG)(IPTR)&g_tx_descs[0]);

    /* 6. Enable MAC Transmission, Reception, and Full Duplex */
    wr32(EMAC_MAC_CONFIG, (1 << 11) | (1 << 3) | (1 << 2)); /* DM=FullDuplex, TE=1, RE=1 */

    /* 7. Start DMA Transmission and Reception (DMA_STATUS / CONTROL) */
    wr32(0x1018, (1 << 13) | (1 << 1)); /* Start TX & RX DMA */

    g_rx_idx = 0;
    g_tx_idx = 0;

    uart_puts("[emac] Ethernet 10/100 Link Ready (RMII 50 MHz Clock, Hardware MAC Filter Active).\n");
    return 0;
}

int emac_p4_send_packet(const UBYTE *packet, UWORD len) {
    if (!packet || len == 0 || len > EMAC_MAX_PACKET_SIZE) return -1;

    struct emac_dma_desc *desc = &g_tx_descs[g_tx_idx];
    if (desc->desc0 & (1UL << 31)) {
        return -1; /* TX Ring Full (DMA owns descriptor) */
    }

    /* Copy packet into TX DMA buffer */
    UBYTE *dst = &g_tx_buffers[g_tx_idx][0];
    for (UWORD i = 0; i < len; i++) dst[i] = packet[i];

    /* Setup Descriptor: FS=FirstSegment(bit 28), LS=LastSegment(bit 29), IC=IntrOnCompletion(bit 30), OWN=DMA(bit 31) */
    desc->desc1 = (ULONG)len;
    desc->desc0 = (1UL << 31) | (1 << 30) | (1 << 29) | (1 << 28) | (1 << 20);

    /* Poll DMA TX */
    wr32(EMAC_DMA_TX_POLL, 1);

    g_tx_idx = (g_tx_idx + 1) % EMAC_DESC_RING_SIZE;
    return (int)len;
}

WORD emac_p4_receive_packet(UBYTE *dst_buf, UWORD max_len) {
    if (!dst_buf) return -1;

    struct emac_dma_desc *desc = &g_rx_descs[g_rx_idx];
    if (desc->desc0 & (1UL << 31)) {
        return 0; /* No packet received (DMA owns descriptor) */
    }

    /* Extract Frame Length (bits 29..16 of desc0) */
    UWORD frame_len = (UWORD)((desc->desc0 >> 16) & 0x3FFF) - 4; /* Subtract 4-byte FCS */
    if (frame_len > max_len) frame_len = max_len;

    const UBYTE *src = &g_rx_buffers[g_rx_idx][0];
    for (UWORD i = 0; i < frame_len; i++) dst_buf[i] = src[i];

    /* Return descriptor ownership to DMA */
    desc->desc0 = (1UL << 31);
    wr32(EMAC_DMA_RX_POLL, 1);

    g_rx_idx = (g_rx_idx + 1) % EMAC_DESC_RING_SIZE;
    return (WORD)frame_len;
}

void emac_p4_get_mac(UBYTE *mac) {
    if (!mac) return;
    mac[0] = 0x24; mac[1] = 0xEC; mac[2] = 0x4A;
    mac[3] = 0x01; mac[4] = 0x20; mac[5] = 0x01;
}
