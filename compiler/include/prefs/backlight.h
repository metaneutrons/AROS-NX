#ifndef PREFS_BACKLIGHT_H
#define PREFS_BACKLIGHT_H

/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: Backlight prefs definitions
    Lang: english
*/

#ifndef EXEC_TYPES_H
#   include <exec/types.h>
#endif

#ifndef LIBRARIES_IFFPARSE_H
#   include <libraries/iffparse.h>
#endif

/*
 * ENV:Sys/backlight.prefs: FORM PREF with a PRHD and one BKLT chunk.
 * IPrefs applies it through the platform's backlight operations
 * (KATTR_BacklightOps, <aros/backlight.h>). Words are big-endian in the
 * file, as in every IFF prefs chunk.
 */
#define ID_BKLT MAKE_ID('B','K','L','T')

struct BacklightPrefs
{
    ULONG bp_Reserved[4];
    UWORD bp_Level;         /* percent of the full range */
    UWORD bp_Pad;
};

/* Readers raise a lower level to this: level 0 is dark, and a dark panel
   at every boot cannot be undone through an editor on it. */
#define BACKLIGHT_MIN_LEVEL 5

#endif /* PREFS_BACKLIGHT_H */
