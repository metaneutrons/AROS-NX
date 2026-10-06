/* Touchscreen: the four-target calibration window. */

#include <intuition/intuition.h>
#include <proto/exec.h>
#include <proto/graphics.h>
#include <proto/intuition.h>

#include <string.h>

#include "touchscreen.h"

#define TARGET_ARM 16
#define TARGET_BOX 5

static void draw_target(struct RastPort *rp, LONG x, LONG y, ULONG pen)
{
    SetAPen(rp, pen);
    Move(rp, x - TARGET_ARM, y);
    Draw(rp, x + TARGET_ARM, y);
    Move(rp, x, y - TARGET_ARM);
    Draw(rp, x, y + TARGET_ARM);
    Move(rp, x - TARGET_BOX, y - TARGET_BOX);
    Draw(rp, x + TARGET_BOX, y - TARGET_BOX);
    Draw(rp, x + TARGET_BOX, y + TARGET_BOX);
    Draw(rp, x - TARGET_BOX, y + TARGET_BOX);
    Draw(rp, x - TARGET_BOX, y - TARGET_BOX);
}

static void draw_line(struct RastPort *rp, LONG width, LONG y,
                      CONST_STRPTR text)
{
    LONG length = strlen(text);

    Move(rp, (width - TextLength(rp, text, length)) / 2, y);
    Text(rp, text, length);
}

/* Targets in screen coordinates; points[] holds them in logical ones. */
static void draw_step(struct Window *win, const UWORD *pens,
                      const LONG *tx, const LONG *ty, int step)
{
    static const char *const lines[4] =
    {
        "Touch the centre of the cross, top left (1 of 4).",
        "Top right (2 of 4).",
        "Bottom right (3 of 4).",
        "Bottom left (4 of 4)."
    };
    struct RastPort *rp = win->RPort;
    LONG mid = win->Height / 2;
    LONG line = rp->TxHeight + 4;
    int i;

    SetAPen(rp, pens[BACKGROUNDPEN]);
    RectFill(rp, 0, 0, win->Width - 1, win->Height - 1);
    SetAPen(rp, pens[TEXTPEN]);
    SetDrMd(rp, JAM1);
    draw_line(rp, win->Width, mid - line, "Touchscreen calibration");
    draw_line(rp, win->Width, mid + rp->TxBaseline - rp->TxHeight / 2,
              lines[step]);
    draw_line(rp, win->Width, mid + line, "Press Esc to cancel.");
    for (i = 0; i < step; ++i)
        draw_target(win->RPort, tx[i], ty[i], pens[SHADOWPEN]);
    draw_target(win->RPort, tx[step], ty[step], pens[TEXTPEN]);
}

int Touch_Calibrate(struct Screen *screen, const struct TouchCal *active,
                    ULONG width, ULONG height, struct TouchCal *result)
{
    struct TouchCalPoint points[4];
    struct Window *win;
    struct DrawInfo *dri;
    LONG tx[4], ty[4];
    LONG inset_x, inset_y, sw, sh;
    int step = 0, outcome = -1, i;
    BOOL done = FALSE;

    if (!screen || width < 2 || height < 2)
        return -1;
    sw = screen->Width;
    sh = screen->Height;
    win = OpenWindowTags(NULL,
                         WA_CustomScreen, (IPTR)screen,
                         WA_Left, 0,
                         WA_Top, 0,
                         WA_Width, sw,
                         WA_Height, sh,
                         WA_Borderless, TRUE,
                         WA_Activate, TRUE,
                         WA_RMBTrap, TRUE,
                         WA_NoCareRefresh, TRUE,
                         WA_IDCMP, IDCMP_MOUSEBUTTONS | IDCMP_VANILLAKEY,
                         TAG_DONE);
    if (!win)
        return -1;
    dri = GetScreenDrawInfo(screen);
    if (!dri)
    {
        CloseWindow(win);
        return -1;
    }

    /* An eighth in from each edge: inside any plausible calibration, so the
       pointer is not clamped while the targets are touched. */
    inset_x = sw / 8;
    inset_y = sh / 8;
    tx[0] = inset_x;           ty[0] = inset_y;
    tx[1] = sw - 1 - inset_x;  ty[1] = inset_y;
    tx[2] = sw - 1 - inset_x;  ty[2] = sh - 1 - inset_y;
    tx[3] = inset_x;           ty[3] = sh - 1 - inset_y;
    for (i = 0; i < 4; ++i)
    {
        points[i].sx = tx[i] * (LONG)width / sw;
        points[i].sy = ty[i] * (LONG)height / sh;
    }

    draw_step(win, dri->dri_Pens, tx, ty, step);
    while (!done)
    {
        struct IntuiMessage *msg;

        WaitPort(win->UserPort);
        while (!done && (msg = (struct IntuiMessage *)GetMsg(win->UserPort)))
        {
            ULONG class = msg->Class;
            UWORD code = msg->Code;
            LONG x = msg->MouseX + win->LeftEdge;
            LONG y = msg->MouseY + win->TopEdge;

            ReplyMsg(&msg->ExecMessage);
            if (class == IDCMP_VANILLAKEY && code == 27)
                done = TRUE;
            else if (class == IDCMP_MOUSEBUTTONS && code == SELECTDOWN)
            {
                /* The pointer sits where the HIDD put the contact; undo
                   its mapping to get the raw value back. */
                touch_cal_unmap(active, width, height,
                                  x * (LONG)width / sw,
                                  y * (LONG)height / sh,
                                  &points[step].rx, &points[step].ry);
                if (++step == 4)
                {
                    outcome = touch_cal_fit(points, width, height, result);
                    done = TRUE;
                }
                else
                    draw_step(win, dri->dri_Pens, tx, ty, step);
            }
        }
    }

    FreeScreenDrawInfo(screen, dri);
    CloseWindow(win);
    return outcome;
}
