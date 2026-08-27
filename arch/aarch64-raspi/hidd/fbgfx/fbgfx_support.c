/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: Linear framebuffer gfx HIDD. The bootstrap brings the VideoCore
          framebuffer up and hands its geometry to the kernel; this driver
          wraps that surface, querying it via KrnGetSystemAttr. It touches no
          VideoCore register itself - the one board-specific thing left is the
          pixel byte order below, which the firmware fixes and no attribute
          reports.
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

BOOL initFBGfxHW(struct HWData *data)
{
    struct KernelBase *KernelBase = OpenResource("kernel.resource");
#ifdef P4_C1_FRAMEBUFFER_HIDD
    struct KrnFrameBufferOps *ops = KernelBase
        ? (struct KrnFrameBufferOps *)KrnGetSystemAttr(KATTR_FrameBufferOps)
        : NULL;

    if (!ops || ops == (APTR)-1
        || ops->version != KRN_FRAMEBUFFER_OPS_VERSION
        || !ops->update_rect || ops->width != 1280 || ops->height != 800
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
#else
    IPTR fb = KernelBase ? (IPTR)KrnGetSystemAttr(KATTR_FrameBuffer) : 0;

    /* KrnGetSystemAttr() answers -1 for anything it does not know, so a
       kernel without the framebuffer attributes hands back a pointer that
       is not NULL but is not memory either. */
    if (fb == 0 || fb == (IPTR)-1)
    {
        D(bug("[FBGfx] HwInit: framebuffer not available\n"));
        return FALSE;
    }

#if !defined(DEBUGDISPLAY)
    /*
     * Detach the bootstrap's framebuffer console before handing this surface
     * on. 0x03 to RawPutChar drops ARMI_PutChar (krnPutC in
     * arch/aarch64-native/kernel/kernel_debug.c), leaving debug output on the
     * serial line; without it every bug() keeps drawing characters into the
     * screen Workbench is using.
     */
    RawPutChar(0x03);
#endif

    data->framebuffer  = (APTR)fb;
    data->width        = KrnGetSystemAttr(KATTR_FrameBufferWidth);
    data->height       = KrnGetSystemAttr(KATTR_FrameBufferHeight);
    data->bytesperline = KrnGetSystemAttr(KATTR_FrameBufferPitch);
    data->depth        = 32;
    data->bitsperpixel = 32;
    data->bytesperpixel = 4;

    /*
     * The VideoCore surface displays byte order R,G,B,x (see fb.c put()), so
     * the pixel masks are red@0-7, green@8-15, blue@16-23. AROS's MAP_COLCOMP
     * shift convention is (mask << shift) == 0xFF000000, i.e. shift = 24 minus
     * the mask's bit position - NOT the bit position itself.
     */
    data->redmask   = 0x000000FF; data->redshift   = 24;
    data->greenmask = 0x0000FF00; data->greenshift = 16;
    data->bluemask  = 0x00FF0000; data->blueshift  = 8;
    data->palettewidth = 8;
    data->fbsize = data->height * data->bytesperline;

    D(bug("[FBGfx] HwInit: %ux%ux%u linear FB @ 0x%p, pitch %u\n",
          data->width, data->height, data->depth,
          data->framebuffer, data->bytesperline));

    ClearBuffer(data);
    return TRUE;
#endif
}

/* Copy the (possibly partial) bitmap buffer to the visible framebuffer. */
void fbDoRefreshArea(struct HWData *hwdata, struct FBGfxBitMapData *data,
                       LONG x1, LONG y1, LONG x2, LONG y2)
{
#ifdef P4_C1_FRAMEBUFFER_HIDD
    LONG sx, sy;

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
    if (x2 > x1 && y2 > y1
        && !hwdata->ops->update_rect(data->VideoData, data->bytesperline,
                                     sx, sy, x2 - x1, y2 - y1))
        bug("[FBGfx/C1] dirty update rejected: %ld,%ld %ldx%ld\n",
            sx, sy, x2 - x1, y2 - y1);
#else
    UBYTE *src, *dst;
    ULONG srcmod, dstmod;
    LONG y, w, h, sx, sy;

    x1 += data->xoffset; y1 += data->yoffset;
    x2 += data->xoffset; y2 += data->yoffset;

    if ((x1 >= data->disp_width) || (x2 < 1) ||
        (y1 >= data->disp_height) || (y2 < 1))
        return;
    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 > data->disp_width)  x2 = data->disp_width;
    if (y2 > data->disp_height) y2 = data->disp_height;

    w = x2 - x1;
    h = y2 - y1;
    sx = x1 - data->xoffset;
    sy = y1 - data->yoffset;
    w *= data->bytesperpix;

    srcmod = data->bytesperline;
    dstmod = hwdata->bytesperline;
    src = data->VideoData + sy * data->bytesperline + sx * data->bytesperpix;
    dst = (UBYTE *)hwdata->framebuffer + y1 * hwdata->bytesperline + x1 * hwdata->bytesperpixel;

    if ((srcmod != dstmod) || (srcmod != (ULONG)w))
    {
        for (y = 0; y < h; y++)
        {
            CopyMem(src, dst, w);
            src += srcmod;
            dst += dstmod;
        }
    }
    else
    {
        CopyMem(src, dst, w * h);
    }
#endif
}

/* Truecolor only: no hardware palette to load. */
void DACLoad(struct FBGfx_staticdata *xsd, UBYTE *DAC, unsigned char first, int num)
{
    (void)xsd; (void)DAC; (void)first; (void)num;
}

void ClearBuffer(struct HWData *data)
{
#ifdef P4_C1_FRAMEBUFFER_HIDD
    if (data->ops && data->ops->clear && !data->ops->clear(0))
        bug("[FBGfx/C1] clear rejected\n");
#else
    if (data->framebuffer)
        memset(data->framebuffer, 0, data->height * data->bytesperline);
#endif
}
