#ifndef AROS_BACKLIGHT_H
#define AROS_BACKLIGHT_H

/*
 * Optional platform contract behind KATTR_BacklightOps.
 *
 * The platform owns the panel's backlight driver (PWM, enable lines, power
 * sequencing). A preferences editor or commodity reads and sets the level
 * through this table without linking against private kernel symbols and
 * without depending on the graphics driver.
 */

#include <exec/types.h>

#define KRN_BACKLIGHT_OPS_VERSION 1

struct KrnBacklightOps
{
    ULONG version;
    ULONG levels;           /* valid levels are 0 .. levels - 1 */
    ULONG default_level;    /* the board's build-time level */

    /* The level last set (or the default), whether or not the panel is lit
       yet. Callable from any context. */
    ULONG (*get_level)(VOID);

    /* Task context. Clamps to levels - 1. Before the display is up the
       level is stored and applied when the backlight comes on. Level 0 is
       dark; whether to allow it is the caller's decision. */
    BOOL (*set_level)(ULONG level);
};

#endif /* AROS_BACKLIGHT_H */
