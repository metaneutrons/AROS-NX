/*
    Copyright (c) 2026, The AROS Development Team. All rights reserved.

    Desc: What memory there is, and handing it to exec.

    Two regions on this silicon revision, and they are not equivalent.
    The low one, above this image, can hold code and data. The high one
    at 0x4FF40000 is data only - the instruction side does not reach it -
    and AROS's memory flags have no way to say so, so anything that loads
    code has to allocate from the low region on purpose. That is a real
    limitation of this platform rather than an oversight here, and it is
    why the low region keeps MEMF_KICK and the high one does not.

    PSRAM is a third region and is not here: the MSPI has not been
    brought up, so there is nothing to hand over yet.
*/

#include <exec/memory.h>
#include <exec/memheaderext.h>

#include <kernel_base.h>

#include "hardware.h"
#include "kernel_intern.h"

/* Filled in by the link script; array form so the name is the address */
extern char __kernel_end[];

struct MemHeader *__esp32p4_mh_low;
struct MemHeader *__esp32p4_mh_high;

static IPTR low_base, low_size, high_base, high_size;

/*
 * How much of the top of SRAM the L2 cache is holding. Read from the
 * controller: the size is a software decision, and the software that
 * made it was the ROM, not this kernel.
 */
static IPTR krnL2CacheSize(void)
{
    uint32_t conf = *(volatile uint32_t *)(P4_CACHE_BASE + P4_L2_CACHESIZE_CONF);

    if (conf & P4_L2_CACHESIZE_512)
        return 512 * 1024;
    if (conf & P4_L2_CACHESIZE_256)
        return 256 * 1024;
    return 128 * 1024;
}

void krnRAMInit(void)
{
    IPTR l2 = krnL2CacheSize();

    /* Immediately above this image, up to the ROM's own variables */
    low_base = ((IPTR)__kernel_end + 15) & ~(IPTR)15;
    low_size = (low_base < P4_HEAP_LOW_END) ? P4_HEAP_LOW_END - low_base : 0;

    /* And whatever the cache left of the high window */
    high_base = P4_HEAP_HIGH_BASE;
    high_size = (l2 < P4_HEAP_HIGH_SPAN) ? P4_HEAP_HIGH_SPAN - l2 : 0;

    if (low_size > sizeof(struct MemHeader) * 2)
    {
        __esp32p4_mh_low = (struct MemHeader *)low_base;
        krnCreateMemHeader("System Memory", 0, (APTR)low_base, low_size,
                           MEMF_FAST | MEMF_PUBLIC | MEMF_KICK | MEMF_LOCAL);
    }

    if (high_size > sizeof(struct MemHeader) * 2)
    {
        __esp32p4_mh_high = (struct MemHeader *)high_base;
        krnCreateMemHeader("Data Memory", -10, (APTR)high_base, high_size,
                           MEMF_FAST | MEMF_PUBLIC | MEMF_LOCAL);
    }
}

void krnRAMReport(void)
{
    krnP4PutStr("[kernel] l2     ");
    krnP4PutDec((uint32_t)(krnL2CacheSize() / 1024));
    krnP4PutStr(" KB cache at the top of SRAM\n");

    krnP4PutStr("[kernel] heap   ");
    krnP4PutHex32((uint32_t)low_base);
    krnP4PutStr(" - ");
    krnP4PutHex32((uint32_t)(low_base + low_size));
    krnP4PutStr("  ");
    krnP4PutDec((uint32_t)low_size);
    krnP4PutStr(" bytes, code and data");
    krnP4PutStr(__esp32p4_mh_low ? "\n" : " (NO HEADER)\n");

    krnP4PutStr("[kernel] heap   ");
    krnP4PutHex32((uint32_t)high_base);
    krnP4PutStr(" - ");
    krnP4PutHex32((uint32_t)(high_base + high_size));
    krnP4PutStr("  ");
    krnP4PutDec((uint32_t)high_size);
    krnP4PutStr(" bytes, data only");
    krnP4PutStr(__esp32p4_mh_high ? "\n" : " (NO HEADER)\n");

    krnP4PutStr("[kernel] total  ");
    krnP4PutDec((uint32_t)(low_size + high_size));
    krnP4PutStr(" bytes of internal SRAM available\n");
}
