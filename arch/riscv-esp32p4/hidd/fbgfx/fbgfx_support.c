/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: ESP32-P4 framebuffer gfx HIDD, hardware side. The surface and
          its geometry come from kernel.resource (KATTR_FrameBufferOps);
          this driver touches no display register itself.
*/

#define DEBUG 0
#include <aros/debug.h>
#include <aros/framebuffer.h>
#include <aros/kernel.h>
#include <proto/exec.h>

/* kernel.resource is a resource, not a library: open it explicitly rather
   than letting the module's autoinit try to OpenLibrary() it. */
#define __NOLIBBASE__
#include <proto/kernel.h>
#include <string.h>

#include "fbgfx_intern.h"
#include "fbgfx_hidd.h"

static ULONG c1_refresh_count;

BOOL initFBGfxHW(struct HWData *data)
{
    struct KernelBase *KernelBase = OpenResource("kernel.resource");
    struct KrnFrameBufferOps *ops = KernelBase
        ? (struct KrnFrameBufferOps *)KrnGetSystemAttr(KATTR_FrameBufferOps)
        : NULL;

    if (!ops || ops == (APTR)-1
        || ops->version != KRN_FRAMEBUFFER_OPS_VERSION
        || !ops->update_rect || !ops->get_stats
        || !ops->width || !ops->height
        || ops->width > 4096 || ops->height > 4096
        || ops->logical_pitch < ops->width * 2
        || ops->depth != 16 || ops->bytes_per_pixel != 2)
    {
        bug("[FBGfx/C1] framebuffer operation table missing or invalid\n");
        return FALSE;
    }

    data->ops = ops;
    data->framebuffer = (APTR)ops->physical_front;
    data->width = ops->width;
    data->height = ops->height;
    data->bytesperline = ops->logical_pitch;
    data->depth = 16;
    data->bitsperpixel = 16;
    data->bytesperpixel = 2;
    data->redmask = 0x0000F800; data->redshift = 16;
    data->greenmask = 0x000007E0; data->greenshift = 21;
    data->bluemask = 0x0000001F; data->blueshift = 27;
    data->palettewidth = 8;
    data->fbsize = ops->physical_size * 2;

    bug("[FBGfx/C1] logical %lux%lux%lu RGB565, pitch %lu; physical %p/%p\n",
        (unsigned long)data->width, (unsigned long)data->height,
        (unsigned long)data->depth, (unsigned long)data->bytesperline,
        (APTR)ops->physical_front, (APTR)ops->physical_back);
    return TRUE;
}

/* Copy the (possibly partial) bitmap buffer to the visible framebuffer. */
void fbDoRefreshArea(struct HWData *hwdata, struct FBGfxBitMapData *data,
                       LONG x1, LONG y1, LONG x2, LONG y2)
{
    LONG sx, sy;
    BOOL accepted;
    ULONG sample;
    struct KrnFrameBufferStats stats;

    x1 += data->xoffset; y1 += data->yoffset;
    x2 += data->xoffset; y2 += data->yoffset;
    if ((x1 >= data->disp_width) || (x2 < 1)
        || (y1 >= data->disp_height) || (y2 < 1))
        return;
    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 > data->disp_width) x2 = data->disp_width;
    if (y2 > data->disp_height) y2 = data->disp_height;
    sx = x1 - data->xoffset;
    sy = y1 - data->yoffset;
    if (x2 <= x1 || y2 <= y1)
        return;

    accepted = hwdata->ops->update_rect(data->VideoData,
                                        data->bytesperline,
                                        sx, sy, x2 - x1, y2 - y1);
    sample = c1_refresh_count++;
    if (!accepted)
        bug("[FBGfx/C1] dirty update rejected: %ld,%ld %ldx%ld\n",
            sx, sy, x2 - x1, y2 - y1);
    if (sample < 16 || (sample & 127) == 127)
    {
        memset(&stats, 0, sizeof(stats));
        hwdata->ops->get_stats(&stats);
        bug("[FBGfx/C1] update %lu %ld,%ld %ldx%ld %s; "
            "src=%p pitch=%lu bpp=%lu bm=%lux%lu offs=%ld,%ld; "
            "frames=%lu swaps=%lu faults=%lu rejects=%lu "
            "active=%p pending=%p\n",
            (unsigned long)sample + 1, sx, sy, x2 - x1, y2 - y1,
            accepted ? "accepted" : "REJECTED",
            data->VideoData, (unsigned long)data->bytesperline,
            (unsigned long)data->bytesperpix,
            (unsigned long)data->width, (unsigned long)data->height,
            (long)data->xoffset, (long)data->yoffset,
            (unsigned long)stats.frames, (unsigned long)stats.swaps,
            (unsigned long)stats.faults, (unsigned long)stats.rejects,
            (APTR)stats.active, (APTR)stats.pending);
    }
}

/* Truecolor only: no hardware palette to load. */
void DACLoad(struct FBGfx_staticdata *xsd, UBYTE *DAC, unsigned char first, int num)
{
    (void)xsd; (void)DAC; (void)first; (void)num;
}

void ClearBuffer(struct HWData *data)
{
    if (data->ops && data->ops->clear && !data->ops->clear(0))
        bug("[FBGfx/C1] clear rejected\n");
}
