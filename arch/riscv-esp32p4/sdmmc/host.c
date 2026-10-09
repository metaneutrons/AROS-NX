/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#include <aros/debug.h>
#include <aros/symbolsets.h>
#include <proto/exec.h>
#include "host.h"
#include "clock.h"
#include "dma.h"
#include "command.h"
#include LC_LIBDEFS_FILE

struct ExecBase *SysBase;

/* Same P4 SYSTIMER timebase as the SD backend. Double-read high to reject
 * a carry across the low read.
 * Both the peripheral poll and retry count are bounded. No MMIO at init. */
static ULONG p4host_now(void)
{
    volatile ULONG *op = (volatile ULONG *)0x500e2004UL;
    volatile ULONG *hi = (volatile ULONG *)0x500e2040UL;
    volatile ULONG *lo = (volatile ULONG *)0x500e2044UL;
    ULONG high, low, again;
    unsigned tries = 8;
    do
    {
        unsigned spins = 1000;
        *op = 1UL << 30;
        __asm__ volatile("fence iorw, iorw" ::: "memory");
        while (spins-- && !(*op & (1UL << 29)))
            ;
        high = *hi;
        low = *lo;
        again = *hi;
    } while (high != again && --tries);
    return (ULONG)((((UQUAD)again << 32) | low) / 16);
}

static BOOL p4host_acquire(struct P4SDMMCHost *host, ULONG timeout_us)
{
    ULONG started;
    unsigned attempts = 100000;
    if (!host || timeout_us > P4_SDMMC_HOST_WAIT_US)
        return FALSE;
    if (AttemptSemaphore(&host->lock))
        return TRUE;
    if (!timeout_us)
        return FALSE;
    started = host->now_us();
    do
    {
        /* AttemptSemaphore owns only its internal short Exec lock. The
         * scheduler and watchdog can run while another client owns hardware. */
        Reschedule();
        if ((ULONG)(host->now_us() - started) >= timeout_us)
            return FALSE;
        if (AttemptSemaphore(&host->lock))
            return TRUE;
    } while (--attempts);
    /* A stale/faulted timer must not make ownership acquisition unbounded.
     * This is an additional refusal bound, not a calibrated time duration. */
    return FALSE;
}

static void p4host_release(struct P4SDMMCHost *host)
{
    if (host && host->lock.ss_Owner == FindTask(NULL))
        ReleaseSemaphore(&host->lock);
    else
        bug("[P4SDMMC] refusing release by non-owner\n");
}

static void p4host_invalidate(struct P4SDMMCHost *host)
{
    if (!host || host->lock.ss_Owner != FindTask(NULL))
    {
        bug("[P4SDMMC] refusing invalidation by non-owner\n");
        return;
    }
    /* A slot must compare its generation while owning the host; zero is
     * reserved for a client which has not initialized its slot yet. */
    if (++host->generation == 0)
        ++host->generation;
}

static int P4SDMMC_Init(LIBBASETYPEPTR P4SDMMCBase)
{
    SysBase = P4SDMMCBase->sysbase;
    InitSemaphore(&P4SDMMCBase->lock);
    P4SDMMCBase->generation = 1;
    P4SDMMCBase->abi = P4_SDMMC_HOST_ABI;
    P4SDMMCBase->size = sizeof(*P4SDMMCBase);
    P4SDMMCBase->acquire = p4host_acquire;
    P4SDMMCBase->release = p4host_release;
    P4SDMMCBase->invalidate = p4host_invalidate;
    P4SDMMCBase->now_us = p4host_now;
    P4SDMMCBase->reset = p4host_reset;
    P4SDMMCBase->set_clock = p4host_set_clock;
    P4SDMMCBase->host_div = 0;
    P4SDMMCBase->slot_clock[0] = P4SDMMCBase->slot_clock[1] = 0;
    P4SDMMCBase->fifo_reset = p4host_fifo_reset;
    P4SDMMCBase->dma_reset = p4host_dma_reset;
    P4SDMMCBase->dma_prepare = p4host_dma_prepare;
    P4SDMMCBase->dma_ready = p4host_dma_ready;
    P4SDMMCBase->dma_complete = p4host_dma_complete;
    P4SDMMCBase->dma_inspect = p4host_dma_inspect;
    P4SDMMCBase->command = p4host_command;
    p4host_dma_init(P4SDMMCBase);
    return TRUE;
}

ADD2INITLIB(P4SDMMC_Init, 0)
