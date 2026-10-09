/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#ifndef ESP32P4_SDMMC_DMA_H
#define ESP32P4_SDMMC_DMA_H
#include "host.h"

#define P4HOST_DMA_MAX 65536UL
#define P4HOST_DMA_SEGMENT 512UL
#define P4HOST_DMA_COUNT (P4HOST_DMA_MAX / P4HOST_DMA_SEGMENT)
#define P4HOST_DESC_LAST (1UL << 2)
#define P4HOST_DESC_FIRST (1UL << 3)
#define P4HOST_DESC_CHAIN (1UL << 4)
#define P4HOST_DESC_OWN (1UL << 31)

/* Hardware layout is always 64 bytes, including on a host-test LP64 ABI. */
struct P4HostDMADesc
{
    ULONG control, size, buffer, next;
    ULONG reserved[12];
};

void p4host_dma_init(struct P4SDMMCHost *host);
/* All calls except initialization require task ownership. Caller memory is
 * never exposed to DMA: TX is copied at prepare, RX only at successful
 * complete. DMA-ready is NOT card-side protocol success. The command engine
 * must check command/data status before completion; the qualified CMD18
 * exception retains the outer transaction through physical CMD12/CMD13. */
BOOL p4host_fifo_reset(struct P4SDMMCHost *host);
BOOL p4host_dma_reset(struct P4SDMMCHost *host);
BOOL p4host_dma_prepare(struct P4SDMMCHost *host, UBYTE *data,
                       ULONG length, BOOL writing);
BOOL p4host_dma_ready(struct P4SDMMCHost *host, ULONG length);
BOOL p4host_dma_complete(struct P4SDMMCHost *host, UBYTE *data, ULONG length);
void p4host_dma_inspect(struct P4SDMMCHost *host);
#endif
