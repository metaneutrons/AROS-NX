#ifndef TOUCHSCREEN_EDITOR_H
#define TOUCHSCREEN_EDITOR_H

#include <exec/types.h>
#include <intuition/screens.h>

#include "../../../rom/hidds/touchscreen/touch_calib.h"
#include "../../../rom/hidds/touchscreen/touch_prefs.h"

/*
 * Full-screen four-target calibration on screen. active is the calibration
 * the driver maps with while the targets are touched; width/height are the
 * driver's screen size. Returns TOUCH_CAL_OK with *result filled, another
 * TOUCH_CAL_* code if the touches do not fit, or -1 if the user cancelled
 * or the window could not open.
 */
int Touch_Calibrate(struct Screen *screen, const struct TouchCal *active,
                    ULONG width, ULONG height, struct TouchCal *result);

#endif
