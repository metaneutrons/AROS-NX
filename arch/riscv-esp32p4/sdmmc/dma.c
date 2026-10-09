/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#include <aros/kernel.h>
#include <aros/cacheops.h>
#include <proto/exec.h>
#include <proto/kernel.h>
#include <string.h>
#include "dma.h"
#include "registers.h"

static struct P4HostDMADesc descriptors[P4HOST_DMA_COUNT]
    __attribute__((aligned(64)));
typedef char p4host_dma_descriptor_must_be_64_bytes[
    sizeof(struct P4HostDMADesc) == 64 ? 1 : -1];
static UBYTE bounce[P4HOST_DMA_MAX] __attribute__((aligned(64)));
static const struct KrnCacheOps *cache_ops;

static BOOL dma_owned(struct P4SDMMCHost *host)
{
    return host && host->lock.ss_Owner == FindTask(NULL);
}

static ULONG dma_read(ULONG offset)
{
    return *(volatile ULONG *)(IPTR)(P4_SDMMC_BASE + offset);
}

static void dma_write(ULONG offset, ULONG value)
{
    *(volatile ULONG *)(IPTR)(P4_SDMMC_BASE + offset) = value;
    __asm__ volatile("fence iorw, iorw" ::: "memory");
}

/* Both elapsed time and finite polls bound a stopped-timer failure. */
static BOOL dma_wait(struct P4SDMMCHost *host, ULONG offset, ULONG mask)
{
    ULONG started = host->now_us(), polls = 1000000UL;
    while (dma_read(offset) & mask)
        if (!--polls || (ULONG)(host->now_us() - started) >= 250000UL)
            return FALSE;
    return TRUE;
}

static BOOL dma_settle(struct P4SDMMCHost *host)
{
    ULONG started = host->now_us(), polls = 1000000UL;
    /* FIFO reset needs two system clocks and two card clocks. Preserve the
     * qualified 20-us margin at 400 kHz, including after CMD18 completion. */
    while ((ULONG)(host->now_us() - started) < 20UL)
        if (!--polls)
            return FALSE;
    return TRUE;
}

static void dma_writeback(APTR address, ULONG length)
{
    cache_ops->writeback_range(address, length);
    __asm__ volatile("fence rw, rw" ::: "memory");
}

static void dma_invalidate(APTR address, ULONG length)
{
    cache_ops->invalidate_range(address, length);
    __asm__ volatile("fence rw, rw" ::: "memory");
}

static BOOL dma_cache_available(void)
{
    struct Library *KernelBase = OpenResource("kernel.resource");
    IPTR ops = KernelBase ? (IPTR)KrnGetSystemAttr(KATTR_CacheOps) : 0;
    /* No direct ROM fallback: on SMP the kernel serializes the shared
     * cache-maintenance engine with graphics and other memory users. */
    cache_ops = (ops && ops != (IPTR)-1) ? (const struct KrnCacheOps *)ops : NULL;
    if (!cache_ops || cache_ops->version != KRN_CACHE_OPS_VERSION ||
        !cache_ops->writeback_range || !cache_ops->invalidate_range)
    {
        cache_ops = NULL;
        return FALSE;
    }
    return TRUE;
}

void p4host_dma_init(struct P4SDMMCHost *host)
{
    /* Resource priority16 precedes kernel.resource priority5. Resolve cache
     * operations only on the first owned transfer, after COLDSTART. */
    cache_ops = NULL;
    host->dma_desc = descriptors;
    host->bounce = bounce;
    host->dma_data = NULL;
    host->dma_length = 0;
    host->dma_writing = FALSE;
    host->dma_faulted = TRUE; /* Physical reset must establish idle first. */
}

BOOL p4host_fifo_reset(struct P4SDMMCHost *host)
{
    if (!dma_owned(host))
        return FALSE;
    if ((dma_read(P4SD_CTRL) & P4SD_CTRL_DMA_ENABLE) ||
        (dma_read(P4SD_BMOD) & P4SD_BMOD_DE))
        return FALSE;
    dma_write(P4SD_CTRL, dma_read(P4SD_CTRL) | P4SD_CTRL_FIFO_RESET);
    return dma_wait(host, P4SD_CTRL, P4SD_CTRL_FIFO_RESET) && dma_settle(host);
}

BOOL p4host_dma_reset(struct P4SDMMCHost *host)
{
    if (!dma_owned(host))
        return FALSE;
    dma_write(P4SD_BMOD, dma_read(P4SD_BMOD) | P4SD_BMOD_SWR);
    if (!dma_wait(host, P4SD_BMOD, P4SD_BMOD_SWR))
        goto failed;
    dma_write(P4SD_CTRL, dma_read(P4SD_CTRL) | P4SD_CTRL_DMA_RESET);
    if (!dma_wait(host, P4SD_CTRL, P4SD_CTRL_DMA_RESET))
        goto failed;
    dma_write(P4SD_IDSTS, 0xffffffffUL);
    dma_write(P4SD_CTRL, dma_read(P4SD_CTRL) &
              ~(P4SD_CTRL_DMA_ENABLE | P4SD_CTRL_USE_INTERNAL_DMA));
    host->dma_data = NULL;
    host->dma_length = 0;
    host->dma_writing = FALSE;
    host->dma_faulted = FALSE;
    return TRUE;
failed:
    /* Do not discard the possible bus-master destination or advertise idle
     * when a hardware reset has not completed. Both clients must re-enumerate. */
    host->dma_faulted = TRUE;
    host->invalidate(host);
    return FALSE;
}

BOOL p4host_dma_prepare(struct P4SDMMCHost *host, UBYTE *data,
                       ULONG length, BOOL writing)
{
    ULONG index, offset = 0, count;
    if (!dma_owned(host) || !data || !length ||
        length > P4HOST_DMA_MAX ||
        (IPTR)data > (IPTR)-1 - length ||
        (writing != FALSE && writing != TRUE) || host->dma_length ||
        host->dma_faulted)
        return FALSE;
    if (!cache_ops && !dma_cache_available())
        return FALSE;
    if (!p4host_dma_reset(host))
        return FALSE;
    /* All bus-master memory is permanent. A timed-out reset must not leave
     * DMA writing a client buffer which its caller may release on error.
     * This also keeps rounded cache maintenance away from client neighbours. */
    if (writing && data != bounce)
        memmove(bounce, data, length);
    count = (length + P4HOST_DMA_SEGMENT - 1) / P4HOST_DMA_SEGMENT;
    memset(descriptors, 0, sizeof(descriptors));
    for (index = 0; index < count; ++index)
    {
        ULONG bytes = length - offset;
        struct P4HostDMADesc *desc = &descriptors[index];
        if (bytes > P4HOST_DMA_SEGMENT)
            bytes = P4HOST_DMA_SEGMENT;
        desc->control = P4HOST_DESC_CHAIN | P4HOST_DESC_OWN;
        if (!index)
            desc->control |= P4HOST_DESC_FIRST;
        if (index + 1 == count)
            desc->control |= P4HOST_DESC_LAST;
        desc->size = bytes;
        desc->buffer = (ULONG)(IPTR)(bounce + offset);
        desc->next = index + 1 == count ? 0 :
                      (ULONG)(IPTR)&descriptors[index + 1];
        offset += bytes;
    }
    dma_writeback(descriptors, sizeof(descriptors));
    dma_writeback(bounce, (length + 63UL) & ~63UL);
    host->dma_data = data;
    host->dma_length = length;
    host->dma_writing = writing;
    dma_write(P4SD_DBADDR, (ULONG)(IPTR)descriptors);
    dma_write(P4SD_IDINTEN, 0);
    dma_write(P4SD_CTRL, dma_read(P4SD_CTRL) | P4SD_CTRL_DMA_ENABLE |
              P4SD_CTRL_USE_INTERNAL_DMA);
    dma_write(P4SD_BMOD, P4SD_BMOD_DE | P4SD_BMOD_FB | P4SD_BMOD_PBL_8);
    dma_write(P4SD_PLDMND, 1);
    return TRUE;
}

BOOL p4host_dma_ready(struct P4SDMMCHost *host, ULONG length)
{
    ULONG count;
    if (!dma_owned(host) || host->dma_faulted || !cache_ops || !length ||
        length > P4HOST_DMA_MAX || length != host->dma_length)
        return FALSE;
    count = (length + P4HOST_DMA_SEGMENT - 1) / P4HOST_DMA_SEGMENT;
    dma_invalidate(descriptors, count * sizeof(descriptors[0]));
    return !(descriptors[count - 1].control & P4HOST_DESC_OWN);
}

BOOL p4host_dma_complete(struct P4SDMMCHost *host, UBYTE *data, ULONG length)
{
    ULONG index, count;
    if (!dma_owned(host) || host->dma_faulted || !cache_ops || !length ||
        length > P4HOST_DMA_MAX || length != host->dma_length ||
        data != host->dma_data)
        return FALSE;
    count = (length + P4HOST_DMA_SEGMENT - 1) / P4HOST_DMA_SEGMENT;
    dma_invalidate(descriptors, sizeof(descriptors));
    if (dma_read(P4SD_IDSTS) & P4SD_IDSTS_ERRORS)
        return FALSE;
    for (index = 0; index < count; ++index)
        if ((descriptors[index].control & P4HOST_DESC_OWN) ||
            ((descriptors[index].control & P4HOST_DESC_LAST) != 0) !=
             (index + 1 == count))
            return FALSE;
    dma_write(P4SD_BMOD, 0);
    dma_write(P4SD_CTRL, dma_read(P4SD_CTRL) &
              ~(P4SD_CTRL_DMA_ENABLE | P4SD_CTRL_USE_INTERNAL_DMA));
    if (!p4host_fifo_reset(host))
        return FALSE;
    if (!host->dma_writing)
    {
        dma_invalidate(bounce, (length + 63UL) & ~63UL);
        if (data != bounce)
            memmove(data, bounce, length);
    }
    host->dma_data = NULL;
    host->dma_length = 0;
    host->dma_writing = FALSE;
    return TRUE;
}

void p4host_dma_inspect(struct P4SDMMCHost *host)
{
    if (dma_owned(host) && cache_ops && cache_ops->invalidate_range)
        dma_invalidate(descriptors, sizeof(descriptors));
}
