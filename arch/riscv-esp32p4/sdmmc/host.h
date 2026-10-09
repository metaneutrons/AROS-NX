/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#ifndef ESP32P4_SDMMC_HOST_H
#define ESP32P4_SDMMC_HOST_H

#include <exec/libraries.h>
#include <exec/semaphores.h>

#define P4_SDMMC_HOST_NAME "esp32p4sdmmc.resource"
#define P4_SDMMC_HOST_ABI 3
#define P4_SDMMC_HOST_WAIT_US 1000000UL

/* Private port ABI, shared by separately loaded SD and radio clients.
 * Permanent resident: OpenResource is NOT a lifetime pin. All mutable host
 * fields, clocks, commands, DMA and status must be owned by this semaphore.
 * A transaction may contain several commands (CMD18 through CMD12).
 * Only task context; recursive acquisition by the owning task is supported.
 * Blocking acquisition requires scheduling and interrupts enabled, with no
 * Exec spin lock held. timeout0 may be used under Forbid (OpenDevice); it
 * never yields. No hardware transaction may run with interrupts disabled. */
struct P4SDMMCHost
{
    struct Library library;
    struct ExecBase *sysbase;
    ULONG abi;
    ULONG size;
    BOOL (*acquire)(struct P4SDMMCHost *, ULONG timeout_us);
    void (*release)(struct P4SDMMCHost *);
    void (*invalidate)(struct P4SDMMCHost *);
    ULONG (*now_us)(void);
    /* Caller must own lock. reset invalidates BOTH slots; it is not card
     * re-enumeration. set_clock preserves an active peer's source/divider. */
    BOOL (*reset)(struct P4SDMMCHost *);
    BOOL (*set_clock)(struct P4SDMMCHost *, unsigned slot, ULONG requested,
                      BOOL low_power, ULONG *actual);
    BOOL (*fifo_reset)(struct P4SDMMCHost *);
    BOOL (*dma_reset)(struct P4SDMMCHost *);
    BOOL (*dma_prepare)(struct P4SDMMCHost *, UBYTE *, ULONG, BOOL writing);
    BOOL (*dma_ready)(struct P4SDMMCHost *, ULONG);
    BOOL (*dma_complete)(struct P4SDMMCHost *, UBYTE *, ULONG);
    void (*dma_inspect)(struct P4SDMMCHost *);

    struct SignalSemaphore lock;
    ULONG generation; /* Full host reset invalidates all slot clients. */
    unsigned host_div;
    ULONG slot_clock[2];
    /* One permanent shared descriptor chain and bounce destination. Only
     * the owner may inspect these, even after an unsuccessful command. */
    APTR dma_desc;
    UBYTE *bounce;
    UBYTE *dma_data;
    ULONG dma_length;
    BOOL dma_writing;
    BOOL dma_faulted; /* Unknown hardware ownership: refuse new transfers. */
};

#endif
