/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: Backlight preferences: the panel's brightness, set through the
          platform's backlight operations.
*/

/*********************************************************************************************/

#include "global.h"

#include <aros/backlight.h>
#include <aros/kernel.h>
#include <prefs/prefhdr.h>
#include <prefs/backlight.h>
#include <proto/kernel.h>

#define DEBUG 0
#include <aros/debug.h>

/*********************************************************************************************/

static LONG stopchunks[] =
{
    ID_PREF, ID_BKLT
};

/*********************************************************************************************/

/* A machine without a controllable backlight has no operations; the file
   is then ignored. */
static void SetBacklight(UWORD percent)
{
    APTR KernelBase = OpenResource("kernel.resource");
    struct KrnBacklightOps *ops;

    if (!KernelBase)
        return;
    ops = (struct KrnBacklightOps *)KrnGetSystemAttr(KATTR_BacklightOps);
    if (!ops || ops == (APTR)-1 || ops->version != KRN_BACKLIGHT_OPS_VERSION
        || ops->levels < 2)
        return;

    if (percent < BACKLIGHT_MIN_LEVEL)
        percent = BACKLIGHT_MIN_LEVEL;
    if (percent > 100)
        percent = 100;
    ops->set_level(((ULONG)percent * (ops->levels - 1) + 50) / 100);

    D(bug("[IPrefs] backlight %u%%\n", percent));
}

/*********************************************************************************************/

void BacklightPrefs_Handler(STRPTR filename)
{
    struct IFFHandle *iff;

    D(bug("In IPrefs:BacklightPrefs_Handler\n"));

    if ((iff = CreateIFF(filename, stopchunks, 1)))
    {
        while (ParseIFF(iff, IFFPARSE_SCAN) == 0)
        {
            struct ContextNode *cn = CurrentChunk(iff);
            struct BacklightPrefs *prefs;

            if (cn->cn_ID != ID_BKLT)
                continue;
            prefs = LoadChunk(iff, sizeof(struct BacklightPrefs), MEMF_ANY);
            if (prefs)
            {
                SetBacklight(AROS_BE2WORD(prefs->bp_Level));
                FreeVec(prefs);
            }
        }
        KillIFF(iff);
    }
}

/*********************************************************************************************/
