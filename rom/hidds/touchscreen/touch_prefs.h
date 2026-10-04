/*
 * Touchscreen preferences: the text format shared by touchscreen.hidd
 * (reads it at start and when it changes) and the Touchscreen preferences
 * editor (writes it). Independent of AROS so the host tests can run it.
 *
 * ENV:Sys/touchscreen.prefs, one "key=value" per line, '#' starts a
 * comment:
 *
 *   version=1
 *   calibration.<name>=x_min,x_max,y_min,y_max,flags
 *   mode=tap|direct
 *   hold_ms=400  slop_px=8  release_polls=2
 *   tapdrag_ms=350  tapdrag_px=32  two_finger_right=1
 *
 * <name> is the touchscreen's name (aHidd_TouchScreen_Name, on a board
 * the board's name), because raw ranges belong to one controller and one
 * panel and a boot volume may move between machines; other names' lines
 * are kept but ignored. flags is a set of the letters s (swap X/Y),
 * x (mirror X), y (mirror Y), or "-". Unknown keys and malformed values
 * are ignored, so the file can grow without breaking an older reader.
 */
#ifndef TOUCHSCREEN_PREFS_H
#define TOUCHSCREEN_PREFS_H

#include <stddef.h>
#include <stdint.h>

#include "touch_policy.h"

#define TOUCH_PREFS_PATH     "ENV:Sys/touchscreen.prefs"
#define TOUCH_PREFS_ENVARC   "ENVARC:Sys/touchscreen.prefs"
#define TOUCH_PREFS_VERSION  1

/* Same bit values as vHidd_TouchCal_* in <hidd/touchscreen.h>. */
#define TOUCH_PREFS_SWAP_XY  (1U << 0)
#define TOUCH_PREFS_MIRROR_X (1U << 1)
#define TOUCH_PREFS_MIRROR_Y (1U << 2)

/* Which fields a parse found. */
#define TOUCH_PREFS_HAVE_CAL       (1U << 0)
#define TOUCH_PREFS_HAVE_MODE      (1U << 1)
#define TOUCH_PREFS_HAVE_PARAMS    (1U << 2)

struct TouchCal
{
    uint32_t x_min, x_max, y_min, y_max, flags;
};

struct TouchPrefs
{
    unsigned int have;          /* TOUCH_PREFS_HAVE_* */
    struct TouchCal cal;        /* for the name passed to the parser */
    struct TouchParams params;
    unsigned int mode;          /* TOUCH_TAP or TOUCH_DIRECT */
};

/*
 * Parse text (not necessarily NUL-terminated) for the named screen. Starts
 * from *prefs as passed in, so callers preset defaults and get back only
 * what the file overrides; have collects what was present.
 */
void touch_prefs_parse(struct TouchPrefs *prefs, const char *text,
                         size_t length, const char *name);

/*
 * Write prefs as text for the named screen. Calibration lines of other
 * names found in keep/keep_length (the previous file, may be NULL) are
 * carried over, so one volume can hold the calibration of every machine it
 * boots. Returns the length written, or 0 if out is too small. Output is
 * NUL-terminated.
 */
size_t touch_prefs_format(const struct TouchPrefs *prefs,
                            const char *name, const char *keep,
                            size_t keep_length, char *out, size_t size);

#endif
