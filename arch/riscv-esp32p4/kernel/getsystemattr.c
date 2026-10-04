/* ESP32-P4 system attributes exported through kernel.resource. */

#include <aros/framebuffer.h>
#include <aros/kernel.h>
#include <aros/touchscreen.h>
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

    case KATTR_ClockSource:
        return KernelBase->kb_ClockSource
             ? (intptr_t)KernelBase->kb_ClockSource : -1;

#ifdef P4_C1_FRAMEBUFFER_HIDD
    case KATTR_FrameBuffer:
        return (intptr_t)P4_FB_BASE;
    case KATTR_FrameBufferWidth:
        return (intptr_t)P4_PANEL_H_RES;
    case KATTR_FrameBufferHeight:
        return (intptr_t)P4_TX_V_RES;
    case KATTR_FrameBufferDepth:
        return 16;
    case KATTR_FrameBufferPitch:
        return (intptr_t)(P4_PANEL_H_RES * P4_FB_BYTES_PER_PIXEL);
    case KATTR_FrameBufferOps:
        return (intptr_t)krnP4FrameBufferOps();
    case KATTR_BacklightOps:
        return (intptr_t)krnP4BacklightOps();
#endif

#ifdef P4_C4_TOUCH_HIDD
    case KATTR_TouchScreenOps:
#if defined(P4_BOARD_TOUCH_GT911)
        return (intptr_t)krnP4GT911TouchScreenOps();
#else
        return (intptr_t)krnP4GSLTouchScreenOps();
#endif
#endif

    default:
        return -1;
    }

    AROS_LIBFUNC_EXIT
}
