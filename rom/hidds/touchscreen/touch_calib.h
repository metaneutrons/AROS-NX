/*
 * Touch calibration from four touched targets (ROADMAP J5), independent of
 * AROS so the host tests can run it.
 *
 * The editor never talks to the controller. While it calibrates, the HIDD
 * maps with a known calibration (the board default); the editor turns each
 * pointer position back into the raw value that produced it and fits a new
 * calibration to the four targets. Axis swap and mirroring follow from the
 * direction in which the raw values grow, so a panel that is mounted the
 * other way round calibrates without editing flags.
 */
#ifndef TOUCHSCREEN_CALIB_H
#define TOUCHSCREEN_CALIB_H

#include <stdint.h>

#include "touch_prefs.h"

struct TouchCalPoint
{
    int32_t sx, sy;         /* target centre on the logical screen */
    uint32_t rx, ry;        /* raw contact, controller axes */
};

enum
{
    TOUCH_CAL_OK,
    TOUCH_CAL_SPREAD,    /* touches too close together */
    TOUCH_CAL_AXES,      /* touches do not form a rectangle */
    TOUCH_CAL_RANGE      /* fitted bounds outside 0..65535 */
};

/* Fewer raw units than this between opposite targets is no measurement. */
#define TOUCH_CAL_MIN_SPREAD 16

/*
 * The raw contact that cal maps to logical (x, y), within one raw step;
 * the inverse of the HIDD's mapping (touch_map in touch_worker.c).
 */
void touch_cal_unmap(const struct TouchCal *cal, uint32_t width,
                       uint32_t height, int32_t x, int32_t y,
                       uint32_t *raw_x, uint32_t *raw_y);

/*
 * Fit from points[0..3]: top left, top right, bottom right, bottom left.
 * Returns TOUCH_CAL_OK and fills *out, otherwise leaves *out alone.
 */
int touch_cal_fit(const struct TouchCalPoint points[4], uint32_t width,
                    uint32_t height, struct TouchCal *out);

#endif
