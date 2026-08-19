/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: ESP32-P4 Synopsys DesignWare 10/100 Ethernet MAC (EMAC) Header
          for AROS SANA-II Network Subsystem.
*/

#ifndef RISCV32_EMAC_P4_H
#define RISCV32_EMAC_P4_H

#include "types.h"

/* ESP32-P4 EMAC Register Base */
#define ESP32P4_EMAC_BASE               0x500C6000UL

/* MAC Core Registers */
#define EMAC_MAC_CONFIG                 0x0000
#define EMAC_MAC_FRAME_FILTER           0x0004
#define EMAC_MAC_HASH_HIGH              0x0008
#define EMAC_MAC_HASH_LOW               0x000C
#define EMAC_MAC_MII_ADDR               0x0010
#define EMAC_MAC_MII_DATA               0x0014
#define EMAC_MAC_ADDR0_HIGH             0x0040
#define EMAC_MAC_ADDR0_LOW              0x0044

/* DMA Controller Registers */
#define EMAC_DMA_BUS_MODE               0x1000
#define EMAC_DMA_TX_POLL                0x1004
#define EMAC_DMA_RX_POLL                0x1008
#define EMAC_DMA_RX_BASE                0x100C
#define EMAC_DMA_TX_BASE                0x1010
#define EMAC_DMA_STATUS                 0x1014
#define EMAC_DMA_INTR_ENA               0x101C

/* Enhanced DMA Descriptors (Ring Buffer) */
#define EMAC_DESC_RING_SIZE             8
#define EMAC_MAX_PACKET_SIZE            1536

struct emac_dma_desc
{
    volatile ULONG  desc0;  /* Status flags / Ownership bit (bit 31=OWN) */
    volatile ULONG  desc1;  /* Control / Buffer lengths */
    volatile ULONG  desc2;  /* Buffer 1 physical address */
    volatile ULONG  desc3;  /* Next descriptor physical address */
};

/* Driver Interface */
int  emac_p4_init(const UBYTE *mac_addr);
int  emac_p4_send_packet(const UBYTE *packet, UWORD len);
WORD emac_p4_receive_packet(UBYTE *dst_buf, UWORD max_len);
void emac_p4_get_mac(UBYTE *mac_addr);

#endif /* RISCV32_EMAC_P4_H */
