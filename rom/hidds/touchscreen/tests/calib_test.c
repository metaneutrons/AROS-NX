#include "../touch_calib.h"
#include "../touch_coords.h"

#include <stdio.h>
#include <stdlib.h>

static unsigned int checks;

#define CHECK(expression) do { \
    ++checks; \
    if (!(expression)) { \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, \
                #expression); \
        exit(EXIT_FAILURE); \
    } \
} while (0)

/* What the HIDD does with a raw contact (touch_map). */
static void hidd_map(const struct TouchCal *cal, uint32_t w, uint32_t h,
                     uint32_t raw_x, uint32_t raw_y, int32_t *x, int32_t *y)
{
    if (cal->flags & TOUCH_PREFS_SWAP_XY)
    {
        uint32_t t = raw_x;

        raw_x = raw_y;
        raw_y = t;
    }
    *x = touch_coordinate(raw_x, (uint16_t)cal->x_min,
                            (uint16_t)cal->x_max, (uint16_t)w,
                            (cal->flags & TOUCH_PREFS_MIRROR_X) != 0);
    *y = touch_coordinate(raw_y, (uint16_t)cal->y_min,
                            (uint16_t)cal->y_max, (uint16_t)h,
                            (cal->flags & TOUCH_PREFS_MIRROR_Y) != 0);
}

static int close_to(uint32_t a, uint32_t b, uint32_t tolerance)
{
    return a > b ? a - b <= tolerance : b - a <= tolerance;
}

/*
 * The panel really behaves like truth; the HIDD maps with active. The user
 * touches each target exactly (plus jitter); the editor sees the pointer
 * position and unmaps it with active. The fit must find truth again.
 */
static void run_calibration(const struct TouchCal *truth,
                            const struct TouchCal *active,
                            uint32_t w, uint32_t h, int jitter,
                            uint32_t tolerance)
{
    struct TouchCalPoint points[4];
    struct TouchCal fitted;
    int32_t inset_x = (int32_t)w / 8, inset_y = (int32_t)h / 8;
    int32_t tx[4], ty[4];
    int i;

    tx[0] = inset_x;                   ty[0] = inset_y;
    tx[1] = (int32_t)w - 1 - inset_x;  ty[1] = inset_y;
    tx[2] = (int32_t)w - 1 - inset_x;  ty[2] = (int32_t)h - 1 - inset_y;
    tx[3] = inset_x;                   ty[3] = (int32_t)h - 1 - inset_y;
    for (i = 0; i < 4; ++i)
    {
        uint32_t raw_x, raw_y;
        int32_t px, py;
        int32_t jx = (i & 1) ? jitter : -jitter;
        int32_t jy = (i & 2) ? -jitter : jitter;

        touch_cal_unmap(truth, w, h, tx[i] + jx, ty[i] + jy,
                          &raw_x, &raw_y);
        hidd_map(active, w, h, raw_x, raw_y, &px, &py);
        points[i].sx = tx[i];
        points[i].sy = ty[i];
        touch_cal_unmap(active, w, h, px, py, &points[i].rx,
                          &points[i].ry);
    }
    CHECK(touch_cal_fit(points, w, h, &fitted) == TOUCH_CAL_OK);
    CHECK(fitted.flags == truth->flags);
    CHECK(close_to(fitted.x_min, truth->x_min, tolerance));
    CHECK(close_to(fitted.x_max, truth->x_max, tolerance));
    CHECK(close_to(fitted.y_min, truth->y_min, tolerance));
    CHECK(close_to(fitted.y_max, truth->y_max, tolerance));
}

static void test_unmap_inverts_map(void)
{
    static const struct TouchCal cals[] =
    {
        {10, 1023, 16, 599, 0},
        {15, 1638, 16, 874, TOUCH_PREFS_MIRROR_Y},
        {0, 1100, 30, 700, TOUCH_PREFS_SWAP_XY | TOUCH_PREFS_MIRROR_X},
    };
    unsigned int c;

    for (c = 0; c < sizeof(cals) / sizeof(cals[0]); ++c)
    {
        int32_t x, y;

        for (y = 0; y < 600; y += 7)
            for (x = 0; x < 1024; x += 5)
            {
                uint32_t rx, ry;
                int32_t mx, my;

                touch_cal_unmap(&cals[c], 1024, 600, x, y, &rx, &ry);
                hidd_map(&cals[c], 1024, 600, rx, ry, &mx, &my);
                /* Exact where the axis has a raw value per pixel; with
                   fewer (the JC1060's 1013 for 1024) some pixels have no
                   raw value of their own and come back as a neighbour. */
                CHECK(cals[c].x_max - cals[c].x_min >= 1023
                      ? mx == x : mx - x <= 1 && x - mx <= 1);
                CHECK(cals[c].y_max - cals[c].y_min >= 599
                      ? my == y : my - y <= 1 && y - my <= 1);
            }
    }
}

static void test_fit_recovers_calibration(void)
{
    /* JC1060: GT911 in panel pixels, the measured default. */
    struct TouchCal jc_default = {10, 1023, 16, 599, 0};
    struct TouchCal jc_shifted = {4, 1019, 22, 597, 0};
    /* D1001: GSL3670 raw domain, mirrored Y. */
    struct TouchCal d_default = {15, 1638, 16, 874, TOUCH_PREFS_MIRROR_Y};
    struct TouchCal d_true = {30, 1610, 20, 860, TOUCH_PREFS_MIRROR_Y};
    /* A panel mounted the other way round, and one with swapped axes. */
    struct TouchCal flipped = {10, 1023, 16, 599,
                                 TOUCH_PREFS_MIRROR_X
                                 | TOUCH_PREFS_MIRROR_Y};
    struct TouchCal swapped = {16, 599, 10, 1023, TOUCH_PREFS_SWAP_XY};
    struct TouchCal identity = {0, 1023, 0, 599, 0};

    run_calibration(&jc_default, &jc_default, 1024, 600, 0, 1);
    run_calibration(&jc_shifted, &jc_default, 1024, 600, 0, 2);
    run_calibration(&jc_shifted, &jc_default, 1024, 600, 2, 4);
    run_calibration(&d_true, &d_default, 1280, 800, 0, 3);
    run_calibration(&d_true, &d_default, 1280, 800, 3, 8);
    run_calibration(&flipped, &jc_default, 1024, 600, 0, 2);
    /* The swapped panel needs a calibration that does not clamp its
       raw Y beyond 599 while the targets are touched. */
    swapped.y_max = 1023;
    swapped.x_max = 599;
    run_calibration(&swapped, &identity, 1024, 600, 0, 3);
}

static void test_fit_rejects_bad_input(void)
{
    struct TouchCalPoint same[4] =
    {
        {128, 75, 500, 300}, {895, 75, 501, 300},
        {895, 524, 500, 301}, {128, 524, 500, 300}
    };
    /* Only the top two exchanged: no orientation explains it. (Top right
       and bottom left exchanged would be a valid swap of the axes.) */
    struct TouchCalPoint crossed[4] =
    {
        {128, 75, 906, 82}, {895, 75, 138, 82},
        {895, 524, 906, 527}, {128, 524, 138, 527}
    };
    struct TouchCal out = {1, 2, 3, 4, 5};

    CHECK(touch_cal_fit(same, 1024, 600, &out) == TOUCH_CAL_AXES
          || touch_cal_fit(same, 1024, 600, &out) == TOUCH_CAL_SPREAD);
    CHECK(touch_cal_fit(crossed, 1024, 600, &out) == TOUCH_CAL_AXES);
    CHECK(out.x_min == 1 && out.flags == 5);
}

int main(void)
{
    test_unmap_inverts_map();
    test_fit_recovers_calibration();
    test_fit_rejects_bad_input();
    printf("calib_test: %u checks passed\n", checks);
    return EXIT_SUCCESS;
}
