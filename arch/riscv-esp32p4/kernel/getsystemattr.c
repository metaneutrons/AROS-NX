/* ESP32-P4 system attributes exported through kernel.resource. */

#include <aros/cacheops.h>
#include <aros/framebuffer.h>
#include <aros/kernel.h>
#include <exec/execbase.h>

#include <kernel_base.h>

#include "hardware.h"
#include "kernel_intern.h"
#include "psram.h"

#include <proto/kernel.h>

AROS_LH1(intptr_t, KrnGetSystemAttr,
    AROS_LHA(uint32_t, id, D0),
    struct KernelBase *, KernelBase, 29, Kernel)
{
    AROS_LIBFUNC_INIT

    switch (id)
    {
    case KATTR_Architecture:
        return (intptr_t)"esp32p4-riscv";

    case KATTR_CacheOps:
        return (intptr_t)&__p4_cache_ops;

    case KATTR_ClockSource:
        return KernelBase->kb_ClockSource
             ? (intptr_t)KernelBase->kb_ClockSource : -1;


    default:
        return -1;
    }

    AROS_LIBFUNC_EXIT
}
