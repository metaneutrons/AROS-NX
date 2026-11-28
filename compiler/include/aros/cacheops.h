#ifndef AROS_CACHEOPS_H
#define AROS_CACHEOPS_H

/*
 * Optional platform contract behind KATTR_CacheOps.
 *
 * On a platform whose bus masters (SD host, display DMA) do not see the CPU
 * caches, and whose cache maintenance is a shared hardware unit that two
 * CPUs must not drive at once, the platform owns that maintenance. A driver
 * calls it through this table instead of driving the unit itself, so that
 * its calls and the kernel's are serialized across CPUs.
 *
 * Both calls round the range out to whole cache lines. Callable from task
 * and interrupt context.
 */

#include <exec/types.h>

#define KRN_CACHE_OPS_VERSION 1

struct KrnCacheOps
{
    ULONG version;

    /* Push the dirty lines of the range to memory and leave them valid: for
       a buffer or descriptor that a bus master is about to read. */
    VOID (*writeback_range)(APTR address, ULONG length);

    /* Drop the range's lines without writing them back: for a buffer or
       descriptor that a bus master has just written. */
    VOID (*invalidate_range)(APTR address, ULONG length);
};

#endif /* AROS_CACHEOPS_H */
