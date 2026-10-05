/* Touch calibration from four targets; see touch_calib.h. */

#include "touch_calib.h"

static uint32_t cal_unmap_axis(int32_t s, uint32_t low, uint32_t high,
                               uint32_t size, int mirror)
{
    uint32_t u;

    if (size < 2 || high <= low)
        return low;
    if (s < 0)
        s = 0;
    if ((uint32_t)s > size - 1U)
        s = (int32_t)(size - 1U);
    u = mirror ? size - 1U - (uint32_t)s : (uint32_t)s;
    return low + (uint32_t)(((uint64_t)u * (high - low) + (size - 1U) / 2U)
                            / (size - 1U));
}

void touch_cal_unmap(const struct TouchCal *cal, uint32_t width,
                       uint32_t height, int32_t x, int32_t y,
                       uint32_t *raw_x, uint32_t *raw_y)
{
    /* Mapped values are post-swap: screen X comes from the raw axis that
       the swap put first. */
    uint32_t a = cal_unmap_axis(x, cal->x_min, cal->x_max, width,
                                (cal->flags & TOUCH_PREFS_MIRROR_X) != 0);
    uint32_t b = cal_unmap_axis(y, cal->y_min, cal->y_max, height,
                                (cal->flags & TOUCH_PREFS_MIRROR_Y) != 0);

    if (cal->flags & TOUCH_PREFS_SWAP_XY)
    {
        *raw_x = b;
        *raw_y = a;
    }
    else
    {
        *raw_x = a;
        *raw_y = b;
    }
}

static int64_t cal_abs(int64_t v)
{
    return v < 0 ? -v : v;
}

/* Rounded n / d for d > 0. */
static int64_t cal_div(int64_t n, int64_t d)
{
    return n >= 0 ? (n + d / 2) / d : -((-n + d / 2) / d);
}

/*
 * One axis from two sides. ra/rb are sums of the two raw values on the low
 * and the high screen side, sa/sb the sums of the two screen positions
 * (sums keep the half units of the averages exact).
 */
static int cal_fit_axis(int64_t ra, int64_t rb, int64_t sa, int64_t sb,
                        uint32_t size, uint32_t *low, uint32_t *high,
                        int *mirror)
{
    int64_t ua, ub, lo, hi, last = 2 * ((int64_t)size - 1);

    if (sb <= sa || size < 2)
        return TOUCH_CAL_SPREAD;
    if (cal_abs(rb - ra) < 2 * TOUCH_CAL_MIN_SPREAD)
        return TOUCH_CAL_SPREAD;
    *mirror = rb < ra;
    if (*mirror)
    {
        /* Work in the mirrored screen coordinate, where raw grows. */
        int64_t t = ra;

        ra = rb;
        rb = t;
        ua = last - sb;
        ub = last - sa;
    }
    else
    {
        ua = sa;
        ub = sb;
    }
    /* raw = lo + (rb - ra) / (ub - ua) * u, in halves on both sides */
    lo = cal_div(ra * (ub - ua) - (rb - ra) * ua, 2 * (ub - ua));
    hi = lo + cal_div((rb - ra) * last, 2 * (ub - ua));
    if (lo < 0)
        lo = 0;
    if (hi > 65535 || hi <= lo)
        return TOUCH_CAL_RANGE;
    *low = (uint32_t)lo;
    *high = (uint32_t)hi;
    return TOUCH_CAL_OK;
}

int touch_cal_fit(const struct TouchCalPoint p[4], uint32_t width,
                    uint32_t height, struct TouchCal *out)
{
    int64_t a[4], b[4];
    int64_t dx_x, dx_y, dy_x, dy_y;
    struct TouchCal cal;
    int swap, mirror_x, mirror_y, i, result;

    /* Which raw axis grows along screen X, which along screen Y. */
    dx_x = (int64_t)p[1].rx + p[2].rx - p[0].rx - p[3].rx;
    dx_y = (int64_t)p[1].ry + p[2].ry - p[0].ry - p[3].ry;
    dy_x = (int64_t)p[2].rx + p[3].rx - p[0].rx - p[1].rx;
    dy_y = (int64_t)p[2].ry + p[3].ry - p[0].ry - p[1].ry;
    swap = cal_abs(dx_y) > cal_abs(dx_x);
    if (swap ? cal_abs(dy_x) <= cal_abs(dy_y)
             : cal_abs(dy_y) <= cal_abs(dy_x))
        return TOUCH_CAL_AXES;

    for (i = 0; i < 4; ++i)
    {
        a[i] = swap ? p[i].ry : p[i].rx;
        b[i] = swap ? p[i].rx : p[i].ry;
    }
    /* Two touches on one side must agree better than the sides differ. */
    if (2 * (cal_abs(a[0] - a[3]) + cal_abs(a[1] - a[2]))
            >= cal_abs(a[1] + a[2] - a[0] - a[3])
        || 2 * (cal_abs(b[0] - b[1]) + cal_abs(b[3] - b[2]))
            >= cal_abs(b[2] + b[3] - b[0] - b[1]))
        return TOUCH_CAL_AXES;

    result = cal_fit_axis(a[0] + a[3], a[1] + a[2],
                          (int64_t)p[0].sx + p[3].sx,
                          (int64_t)p[1].sx + p[2].sx,
                          width, &cal.x_min, &cal.x_max, &mirror_x);
    if (result != TOUCH_CAL_OK)
        return result;
    result = cal_fit_axis(b[0] + b[1], b[3] + b[2],
                          (int64_t)p[0].sy + p[1].sy,
                          (int64_t)p[3].sy + p[2].sy,
                          height, &cal.y_min, &cal.y_max, &mirror_y);
    if (result != TOUCH_CAL_OK)
        return result;
    cal.flags = (swap ? TOUCH_PREFS_SWAP_XY : 0)
              | (mirror_x ? TOUCH_PREFS_MIRROR_X : 0)
              | (mirror_y ? TOUCH_PREFS_MIRROR_Y : 0);
    *out = cal;
    return TOUCH_CAL_OK;
}
