/*
    Copyright (C) 1995-2026, The AROS Development Team. All rights reserved.

    Desc: ESP32-P4 framebuffer graphics HIDD.

    Derived from arch/aarch64-raspi/hidd/fbgfx. kernel.resource owns the
    panel, DSI and DMA and publishes the drawing surface through
    KATTR_FrameBufferOps; this driver draws there and has the kernel
    present the changed rectangles.
*/

#include <proto/exec.h>
#include <proto/graphics.h>
#include <proto/oop.h>

#include <exec/types.h>
#include <exec/lists.h>
#include <graphics/driver.h>
#include <graphics/gfx.h>
#include <graphics/gfxbase.h>
#include <graphics/modeid.h>
#include <graphics/rastport.h>
#include <graphics/rpattr.h>
#include <hidd/gfx.h>
#include <oop/oop.h>
#include <utility/utility.h>
#include <aros/framebuffer.h>
#include <aros/symbolsets.h>

#include "fbgfx_support.h"
#include "fbgfx_hidd.h"

#include LC_LIBDEFS_FILE

#define DEBUG 0
#include <aros/debug.h>

/*
 * The following two functions are candidates for inclusion into oop.library.
 * For slightly other implementation see incomplete Android-hosted graphics driver.
 */
static void FreeAttrBases(const STRPTR *iftable, OOP_AttrBase *bases, ULONG num)
{
    ULONG i;
    
    for (i = 0; i < num; i++)
    {
        if (bases[i])
            OOP_ReleaseAttrBase(iftable[i]);
    }
}

static BOOL GetAttrBases(const STRPTR *iftable, OOP_AttrBase *bases, ULONG num)
{
    ULONG i;

    for (i = 0; i < num; i++)
    {
        bases[i] = OOP_ObtainAttrBase(iftable[i]);
        if (!bases[i])
        {
            FreeAttrBases(iftable, bases, i);
            return FALSE;
        }
    }

    return TRUE;
}

/* These must stay in the same order as attrBases[] entries assignment in fbgfxclass.h */
static const STRPTR interfaces[ATTRBASES_NUM] =
{
    IID_Hidd_ChunkyBM,
    IID_Hidd_BitMap,
    IID_Hidd_Gfx,
    IID_Hidd_PixFmt,
    IID_Hidd_Sync,
    IID_Hidd,
    IID_Hidd_Display,
    IID_Hidd_DMEnum
};

static struct DisplayRange c1_ranges[2];

#if defined(P4_C1_GATE)
static BOOL FBGfx_C1Gate(struct FBGfx_staticdata *xsd,
                         struct GfxBase *GfxBase)
{
    struct BitMap *bitmap = NULL;
    struct RastPort rp;
    struct KrnFrameBufferStats stats;
    struct TagItem bmtags[2];
    UBYTE *pixels = NULL;
    PLANEPTR probe_raster;
    IPTR pitch = 0;
    ULONG mode;
    ULONG white_pixels = 0;
    ULONG line, x, y;
    unsigned int attempt;
    /* The card scales with the panel: 1280x800 on the D1001, 1024x600 on
       the JC1060P470C.  Corner blocks, plate and text keep their sizes. */
    const ULONG W = xsd->data.width, H = xsd->data.height;

    mode = BestModeID(BIDTAG_NominalWidth, W,
                      BIDTAG_NominalHeight, H,
                      BIDTAG_Depth, 16,
                      TAG_DONE);
    if (mode == INVALID_ID)
    {
        bug("[FBGfx/C1] queued driver replay did not expose a mode\n");
        return FALSE;
    }
    bug("[FBGfx/C1] AddDisplayDriver replay selected mode %08lx\n",
        (unsigned long)mode);

    bmtags[0].ti_Tag = BMATags_DisplayID;
    bmtags[0].ti_Data = mode;
    bmtags[1].ti_Tag = TAG_DONE;
    bmtags[1].ti_Data = 0;

    for (attempt = 0; attempt < 3; attempt++)
    {
        bitmap = AllocBitMap(W, H, 16,
                             BMF_DISPLAYABLE | BMF_CLEAR | BMF_CHECKVALUE,
                             (struct BitMap *)bmtags);
        if (!bitmap || !IS_HIDD_BM(bitmap))
        {
            bug("[FBGfx/C1] displayable bitmap allocation %u failed\n",
                attempt + 1);
            if (bitmap)
                FreeBitMap(bitmap);
            return FALSE;
        }
        if (attempt != 2)
        {
            FreeBitMap(bitmap);
            bitmap = NULL;
        }
    }
    bug("[FBGfx/C1] three %lux%lu RGB565 bitmap allocations passed\n",
        (unsigned long)W, (unsigned long)H);

    InitRastPort(&rp);
    rp.BitMap = bitmap;
    HIDD_Display_Show(xsd->vcfbdisplay, HIDD_BM_OBJ(bitmap), 0);

    SetRPAttrs(&rp, RPTAG_PenMode, FALSE,
               RPTAG_FgColor, 0xFFFF0000, TAG_DONE);
    RectFill(&rp, 32, 32, 191, 159);
    SetRPAttrs(&rp, RPTAG_FgColor, 0xFF00FF00, TAG_DONE);
    RectFill(&rp, W - 192, 32, W - 33, 159);
    SetRPAttrs(&rp, RPTAG_FgColor, 0xFF0000FF, TAG_DONE);
    RectFill(&rp, 32, H - 160, 191, H - 33);
    SetRPAttrs(&rp, RPTAG_FgColor, 0xFFFFFF00, TAG_DONE);
    RectFill(&rp, W - 192, H - 160, W - 33, H - 33);

    SetRPAttrs(&rp, RPTAG_FgColor, 0xFF00FFFF, TAG_DONE);
    Move(&rp, 32, 32);
    Draw(&rp, W - 33, H - 33);
    Move(&rp, W - 33, 32);
    Draw(&rp, 32, H - 33);
    /* The built-in font is only eight pixels high.  Give Text() a large,
       high-contrast centre plate instead of asking the observer to find one
       tiny line under the cyan diagonals. */
    SetRPAttrs(&rp, RPTAG_FgColor, 0xFF000000, TAG_DONE);
    RectFill(&rp, W / 2 - 192, H / 2 - 96, W / 2 + 191, H / 2 + 95);
    SetRPAttrs(&rp, RPTAG_DrMd, JAM1,
               RPTAG_FgColor, 0xFFFFFFFF, TAG_DONE);

    probe_raster = AllocRaster(128, 8);
    bug("[FBGfx/C1] MEMF_CHIP available=%lu largest=%lu; "
        "AllocRaster(128,8)=%p\n",
        (unsigned long)AvailMem(MEMF_CHIP),
        (unsigned long)AvailMem(MEMF_CHIP | MEMF_LARGEST),
        probe_raster);
    if (!probe_raster)
    {
        bug("[FBGfx/C1] AllocRaster failed; Text cannot render\n");
        return FALSE;
    }
    FreeRaster(probe_raster, 128, 8);

    for (line = 0; line < 6; line++)
    {
        Move(&rp, W / 2 - 128, H / 2 - 64 + line * 24);
        Text(&rp, "AROS ESP32-P4 C1", sizeof("AROS ESP32-P4 C1") - 1);
    }

    OOP_GetAttr(HIDD_BM_OBJ(bitmap),
                xsd->attrBases[0] + aoHidd_ChunkyBM_Buffer,
                (IPTR *)&pixels);
    OOP_GetAttr(HIDD_BM_OBJ(bitmap),
                xsd->attrBases[1] + aoHidd_BitMap_BytesPerRow, &pitch);
    if (pixels && pitch >= W * 2)
    {
        for (y = H / 2 - 96; y <= H / 2 + 95; y++)
            for (x = W / 2 - 192; x <= W / 2 + 191; x++)
            {
                UBYTE *pixel = pixels + y * pitch + x * 2;
                if (pixel[0] == 0xff && pixel[1] == 0xff)
                    white_pixels++;
            }
    }
    bug("[FBGfx/C1] Text produced %lu white RGB565 pixels\n",
        (unsigned long)white_pixels);
    if (white_pixels < 128)
    {
        bug("[FBGfx/C1] Text did not render into the logical bitmap\n");
        return FALSE;
    }

    UpdateBitMap(bitmap, 0, 0, W, H);
    bug("[FBGfx/C1] Show, RectFill, Draw, Text and full update submitted\n");

    if (!xsd->data.ops->flush || !xsd->data.ops->flush())
    {
        bug("[FBGfx/C1] final presentation timed out\n");
        return FALSE;
    }
    xsd->data.ops->get_stats(&stats);
    bug("[FBGfx/C1] frames=%lu swaps=%lu faults=%lu rejects=%lu "
        "active=%p pending=%p\n",
        (unsigned long)stats.frames, (unsigned long)stats.swaps,
        (unsigned long)stats.faults, (unsigned long)stats.rejects,
        (APTR)stats.active, (APTR)stats.pending);
    if (stats.faults || stats.rejects || stats.pending)
    {
        bug("[FBGfx/C1] scanout counters rejected the acceptance gate\n");
        return FALSE;
    }

    /* The gate bitmap intentionally remains visible and allocated.  C1 has
       no Intuition owner yet; C2 will replace this forced boot-mode path. */
    return TRUE;
}
#endif

static int FBGfx_Init(LIBBASETYPEPTR LIBBASE)
{
    struct FBGfx_staticdata *xsd = &LIBBASE->vsd;
    struct GfxBase *GfxBase;
    ULONG err;
    int res = FALSE;

    /*
     * Open graphics.library ourselves because we will close it
     * after adding the driver.
     * Autoinit code would close it only upon driver expunge.
     */
    GfxBase = (struct GfxBase *)TaggedOpenLibrary(TAGGEDOPEN_GRAPHICS);
    if (GfxBase)
    {
        if (initFBGfxHW(&xsd->data))
        {
            if (GetAttrBases(interfaces, xsd->attrBases, ATTRBASES_NUM))
            {
                xsd->basebm = OOP_FindClass(CLID_Hidd_BitMap);
                xsd->mid_Dispose = OOP_GetMethodID(IID_Root, moRoot_Dispose);
                D(bug("[FBGfx] BitMap class @ 0x%p\n", xsd->basebm));

                InitSemaphore(&xsd->framebufferlock);
                InitSemaphore(&xsd->HW_acc);

                D(bug("[FBGfx] Init: Everything OK, installing driver\n"));
                
                /*
                 * It is unknown (and no way to know) what hardware part this driver uses.
                 * In order to avoid conflicts with disk-based native-mode hardware
                 * drivers it needs to be removed from the system when some other driver
                 * is installed.
                 * This is done by graphics.library if DDRV_BootMode is set to TRUE.
                 */
#ifdef P4_C2_INTUITION_GATE
                c1_ranges[0].dr_Base = xsd->data.framebuffer;
                c1_ranges[0].dr_Size = xsd->data.fbsize;
                c1_ranges[1].dr_Base = NULL;
                c1_ranges[1].dr_Size = 0;
                bug("[ESP32P4/C2] priority 9: AddDisplayDriver begin; "
                    "Intuition priority 15 must already own the callback\n");
                err = AddDisplayDriver(xsd->fbgfxclass, NULL,
                                       DDRV_BootMode, TRUE,
                                       DDRV_HWRanges, (IPTR)c1_ranges,
                                       TAG_DONE);
                bug("[ESP32P4/C2] AddDisplayDriver result=%lu\n",
                    (unsigned long)err);
#else
                c1_ranges[0].dr_Base = xsd->data.framebuffer;
                c1_ranges[0].dr_Size = xsd->data.fbsize;
                c1_ranges[1].dr_Base = NULL;
                c1_ranges[1].dr_Size = 0;
                err = AddDisplayDriver(xsd->fbgfxclass, NULL,
                                       DDRV_BootMode, TRUE,
                                       DDRV_HWRanges, (IPTR)c1_ranges,
                                       TAG_DONE);
#ifdef P4_C3_GRAPHICAL_BOOT
                bug("[ESP32P4/C3] normal AddDisplayDriver result=%lu; "
                    "framebuffer=%p size=%lu\n",
                    (unsigned long)err, xsd->data.framebuffer,
                    (unsigned long)xsd->data.fbsize);
#endif
#endif

                D(bug("[FBGfx] AddDisplayDriver() result: %u\n", err));
                if (!err)
                {
#if defined(P4_C1_GATE)
                    if (!FBGfx_C1Gate(xsd, GfxBase))
                    {
                        bug("[FBGfx/C1] acceptance harness failed\n");
                        CloseLibrary(&GfxBase->LibNode);
                        return FALSE;
                    }
#endif
                    /* expunge protection */
                    LIBBASE->library.lib_OpenCnt = 1;
                    res = TRUE;
                }
            }
        }
        CloseLibrary(&GfxBase->LibNode);
    }
    else
    {
        D(bug("[FBGfx] Failed to open graphics.library!\n"));
    }
    return res;
}

ADD2INITLIB(FBGfx_Init, 0)
