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
/* Internal resource-only recovery hook. Caller has completed physical reset,
 * disabled DMA and settled FIFO before calling. Never a substitute for reset.
 * Scrub sticky TX staging and write zeros through the serialized cache API;
 * failure must leave the controller faulted, not advertise idle. */
BOOL p4host_dma_scrub_idle(struct P4SDMMCHost *host);
/* All calls except initialization require task ownership. Caller memory is
 * never exposed to DMA: TX is copied at prepare, RX only at successful
 * complete. DMA-ready is NOT card-side protocol success. The command engine
 * must check command/data status before completion; the qualified CMD18
 * exception retains the outer transaction through physical CMD12/CMD13.
 * ABI8: TX input must not overlap the permanent bounce. Preparation owns all
 * TX staging; completion/recovery wipes it only after proven DMA quiescence.
 * The command engine keeps RX in bounce until its final error resample. */
BOOL p4host_fifo_reset(struct P4SDMMCHost *host);
/* ABI8 successful DMA reset includes FIFO reset/settle and TX scrub. Failure
 * preserves the sticky residue and faults the host until physical recovery. */
BOOL p4host_dma_reset(struct P4SDMMCHost *host);
BOOL p4host_dma_prepare(struct P4SDMMCHost *host, UBYTE *data,
                       ULONG length, BOOL writing);
BOOL p4host_dma_ready(struct P4SDMMCHost *host, ULONG length);
BOOL p4host_dma_complete(struct P4SDMMCHost *host, UBYTE *data, ULONG length);
void p4host_dma_inspect(struct P4SDMMCHost *host);
#endif
