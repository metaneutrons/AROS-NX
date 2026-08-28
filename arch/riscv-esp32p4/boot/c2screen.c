/*
 * ESP32-P4 C2 one-shot Intuition acceptance resident.
 *
 * Priority 8 is deliberate: Intuition installs the display callback at 15,
 * fbgfx inserts the D1001 display driver at 9, this test consumes both at 8,
 * and dosboot follows at -50.  The screen and windows intentionally stay
 * open for the lifetime of the boot so a human observer can inspect them.
 */

#include <exec/types.h>
#include <exec/libraries.h>
#include <exec/tasks.h>
#include <graphics/gfx.h>
#include <graphics/gfxbase.h>
#include <graphics/layers.h>
#include <graphics/modeid.h>
#include <graphics/rastport.h>
#include <graphics/rpattr.h>
#include <intuition/intuition.h>
#include <intuition/intuitionbase.h>
#include <intuition/screens.h>
#include <utility/tagitem.h>
#include <aros/debug.h>
#include <aros/symbolsets.h>

#include <proto/exec.h>
#include <proto/graphics.h>
#include <proto/intuition.h>

#include "c2screen_intern.h"
#include LC_LIBDEFS_FILE

static struct Screen *c2_screen;
static struct Window *c2_back;
static struct Window *c2_front;
struct ExecBase *SysBase;
struct GfxBase *GfxBase;
struct IntuitionBase *IntuitionBase;

static void c2_fill(struct RastPort *rp, ULONG color,
                    LONG x0, LONG y0, LONG x1, LONG y1)
{
    struct TagItem tags[] =
    {
        {RPTAG_PenMode, FALSE},
        {RPTAG_FgColor, color},
        {TAG_DONE, 0}
    };

    SetRPAttrsA(rp, tags);
    RectFill(rp, x0, y0, x1, y1);
}

static void c2_text(struct RastPort *rp, ULONG color,
                    LONG x, LONG y, CONST_STRPTR text, ULONG len)
{
    struct TagItem tags[] =
    {
        {RPTAG_PenMode, FALSE},
        {RPTAG_DrMd, JAM1},
        {RPTAG_FgColor, color},
        {TAG_DONE, 0}
    };

    SetRPAttrsA(rp, tags);
    Move(rp, x, y);
    Text(rp, text, len);
}

static void c2_draw_back(void)
{
    c2_fill(c2_back->RPort, 0xFF244878, 8, 12,
            c2_back->GZZWidth - 9, c2_back->GZZHeight - 13);
    c2_text(c2_back->RPort, 0xFFFFFFFF, 36, 70,
            "C2 BACK: TEXT THROUGH INTUITION",
            sizeof("C2 BACK: TEXT THROUGH INTUITION") - 1);
    c2_text(c2_back->RPort, 0xFF80FFFF, 36, 102,
            "THE FRONT WINDOW MUST CLIP THIS LINE",
            sizeof("THE FRONT WINDOW MUST CLIP THIS LINE") - 1);
    c2_fill(c2_back->RPort, 0xFF40D080, 36, 132, 760, 172);
}

static void c2_draw_front(void)
{
    c2_fill(c2_front->RPort, 0xFF701838, 8, 12,
            c2_front->GZZWidth - 9, c2_front->GZZHeight - 13);
    c2_text(c2_front->RPort, 0xFFFFFFFF, 34, 68,
            "C2 FRONT: OVERLAP IS LIVE",
            sizeof("C2 FRONT: OVERLAP IS LIVE") - 1);
    c2_fill(c2_front->RPort, 0xFFFFB020, 34, 100, 620, 140);
    c2_text(c2_front->RPort, 0xFF101010, 48, 126,
            "SCROLL TARGET >>>",
            sizeof("SCROLL TARGET >>>") - 1);
    c2_text(c2_front->RPort, 0xFFFFFFFF, 34, 200,
            "REFRESH PASS - DAMAGE REDRAWN",
            sizeof("REFRESH PASS - DAMAGE REDRAWN") - 1);
}

static BOOL c2_refresh_after_depth(struct Window *window, BOOL front,
                                   CONST_STRPTR stage)
{
    struct IntuiMessage *message;
    struct Layer *layer = window->RPort->Layer;
    BOOL got_message = FALSE;
    ULONG settle;

    /*
     * WindowToBack()/WindowToFront() enqueue asynchronous Intuition actions.
     * Wait for the layer damage they produce instead of blocking forever on
     * IDCMP: depth changes are allowed to expose a simple-refresh layer
     * without guaranteeing a fresh message when one is already pending.
     */
    for (settle = 0; settle < 60; settle++)
    {
        WaitTOF();
        if (layer->Flags & LAYERREFRESH)
            break;
    }

    while ((message = (struct IntuiMessage *)GetMsg(window->UserPort)))
    {
        if (message->Class == IDCMP_REFRESHWINDOW)
            got_message = TRUE;
        ReplyMsg((struct Message *)message);
    }

    bug("[ESP32P4/C2] %s settled after %lu frame(s): "
        "layer-flags=%04x idcmp=%ld\n",
        stage, (unsigned long)(settle + 1), layer->Flags,
        (long)got_message);

    if (!(layer->Flags & LAYERREFRESH))
    {
        bug("[ESP32P4/C2] FAIL: %s produced no refresh damage\n", stage);
        return FALSE;
    }

    BeginRefresh(window);
    if (front)
        c2_draw_front();
    else
        c2_draw_back();
    EndRefresh(window, TRUE);

    /* A message may race the layer-state observation; acknowledge it now. */
    while ((message = (struct IntuiMessage *)GetMsg(window->UserPort)))
    {
        if (message->Class == IDCMP_REFRESHWINDOW)
        {
            got_message = TRUE;
        }
        ReplyMsg((struct Message *)message);
    }

    bug("[ESP32P4/C2] %s damage redrawn; idcmp=%ld\n",
        stage, (long)got_message);
    return TRUE;
}

static void c2_refresh_task(void)
{
    bug("[ESP32P4/C2] refresh worker started after multitasking\n");

    WindowToBack(c2_front);
    if (!c2_refresh_after_depth(c2_back, FALSE, "back-window expose"))
        return;

    WindowToFront(c2_front);
    if (!c2_refresh_after_depth(c2_front, TRUE, "front-window expose"))
        return;

    RefreshWindowFrame(c2_back);
    RefreshWindowFrame(c2_front);
    ScrollWindowRaster(c2_front, 32, 0, 34, 100, 620, 170);

    bug("[ESP32P4/C2] PASS: screen=%p back=%p front=%p; "
        "IDCMP refresh+overlap+scroll completed\n",
        c2_screen, c2_back, c2_front);
}

static int C2Screen_Init(LIBBASETYPEPTR C2ScreenBase)
{
    struct Library *LayersBase;
    struct TagItem mode_tags[] =
    {
        {BIDTAG_NominalWidth, 1280},
        {BIDTAG_NominalHeight, 800},
        {BIDTAG_Depth, 16},
        {TAG_DONE, 0}
    };
    struct TagItem screen_tags[] =
    {
        {SA_DisplayID, INVALID_ID},
        {SA_Width, 1280},
        {SA_Height, 800},
        {SA_Depth, 16},
        {SA_Type, CUSTOMSCREEN},
        {SA_Title, (IPTR)"AROS ESP32-P4 C2 Intuition"},
        {TAG_DONE, 0}
    };
    struct TagItem back_tags[] =
    {
        {WA_CustomScreen, 0},
        {WA_Left, 120},
        {WA_Top, 110},
        {WA_Width, 850},
        {WA_Height, 500},
        {WA_Title, (IPTR)"C2 BACK WINDOW - LAYERS + REFRESH"},
        {WA_DragBar, TRUE},
        {WA_DepthGadget, TRUE},
        {WA_SimpleRefresh, TRUE},
        {WA_RMBTrap, TRUE},
        {WA_IDCMP, IDCMP_REFRESHWINDOW},
        {TAG_DONE, 0}
    };
    struct TagItem front_tags[] =
    {
        {WA_CustomScreen, 0},
        {WA_Left, 430},
        {WA_Top, 270},
        {WA_Width, 700},
        {WA_Height, 390},
        {WA_Title, (IPTR)"C2 FRONT WINDOW - OVERLAP + SCROLL"},
        {WA_DragBar, TRUE},
        {WA_DepthGadget, TRUE},
        {WA_SimpleRefresh, TRUE},
        {WA_RMBTrap, TRUE},
        {WA_IDCMP, IDCMP_REFRESHWINDOW},
        {TAG_DONE, 0}
    };
    struct TagItem task_tags[] =
    {
        {TASKTAG_NAME, (IPTR)"ESP32-P4 C2 refresh gate"},
        {TASKTAG_PRI, 5},
        {TASKTAG_PC, (IPTR)c2_refresh_task},
        {TASKTAG_STACKSIZE, 32768},
        {TAG_DONE, 0}
    };
    ULONG mode;

    SysBase = C2ScreenBase->c2s_SysBase;
    bug("[ESP32P4/C2] priority 8: one-shot Intuition gate entered\n");

    GfxBase = (struct GfxBase *)OpenLibrary("graphics.library", 0);
    LayersBase = OpenLibrary("layers.library", 0);
    IntuitionBase = (struct IntuitionBase *)OpenLibrary("intuition.library", 0);
    bug("[ESP32P4/C2] opens: graphics=%p layers=%p intuition=%p\n",
        GfxBase, LayersBase, IntuitionBase);
    if (!GfxBase || !LayersBase || !IntuitionBase)
    {
        bug("[ESP32P4/C2] FAIL: library/input skeleton startup blocked\n");
        return FALSE;
    }

    mode = BestModeIDA(mode_tags);
    bug("[ESP32P4/C2] BestModeID=%08lx\n", (unsigned long)mode);
    if (mode == INVALID_ID)
    {
        bug("[ESP32P4/C2] FAIL: display driver has no 1280x800x16 mode\n");
        return FALSE;
    }

    screen_tags[0].ti_Data = mode;
    c2_screen = OpenScreenTagList(NULL, screen_tags);
    bug("[ESP32P4/C2] OpenScreen=%p\n", c2_screen);
    if (!c2_screen)
    {
        bug("[ESP32P4/C2] FAIL: OpenScreenTags returned NULL\n");
        return FALSE;
    }

    c2_fill(&c2_screen->RastPort, 0xFF182030,
            0, 0, c2_screen->Width - 1, c2_screen->Height - 1);
    c2_fill(&c2_screen->RastPort, 0xFF00C8FF, 24, 24, 119, 119);
    c2_fill(&c2_screen->RastPort, 0xFFFF4060,
            c2_screen->Width - 120, 24, c2_screen->Width - 25, 119);
    c2_fill(&c2_screen->RastPort, 0xFF60E080,
            24, c2_screen->Height - 120, 119, c2_screen->Height - 25);
    c2_fill(&c2_screen->RastPort, 0xFFFFD040,
            c2_screen->Width - 120, c2_screen->Height - 120,
            c2_screen->Width - 25, c2_screen->Height - 25);

    back_tags[0].ti_Data = (IPTR)c2_screen;
    c2_back = OpenWindowTagList(NULL, back_tags);
    bug("[ESP32P4/C2] OpenWindow(back)=%p\n", c2_back);
    if (!c2_back)
    {
        bug("[ESP32P4/C2] FAIL: first window did not open\n");
        return FALSE;
    }

    c2_draw_back();

    front_tags[0].ti_Data = (IPTR)c2_screen;
    c2_front = OpenWindowTagList(NULL, front_tags);
    bug("[ESP32P4/C2] OpenWindow(front)=%p\n", c2_front);
    if (!c2_front)
    {
        bug("[ESP32P4/C2] FAIL: overlapping window did not open\n");
        return FALSE;
    }

    c2_draw_front();

    if (!NewCreateTaskA(task_tags))
    {
        bug("[ESP32P4/C2] FAIL: refresh worker creation failed\n");
        return FALSE;
    }
    bug("[ESP32P4/C2] refresh worker queued at priority 5\n");
    return TRUE;
}

ADD2INITLIB(C2Screen_Init, 0)
