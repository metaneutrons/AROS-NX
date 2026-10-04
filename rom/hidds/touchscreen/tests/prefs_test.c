#include "../touch_prefs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned int checks;

#define CHECK(condition) \
    do { \
        ++checks; \
        if (!(condition)) { \
            fprintf(stderr, "%s:%d: check failed: %s\n", \
                    __FILE__, __LINE__, #condition); \
            exit(EXIT_FAILURE); \
        } \
    } while (0)

static void defaults(struct TouchPrefs *p)
{
    memset(p, 0, sizeof(*p));
    touch_params_default(&p->params);
    p->mode = TOUCH_TAP;
}

static void test_parse(void)
{
    static const char text[] =
        "# comment\n"
        "version=1\n"
        "calibration.d1001=16,1638,15,874,y\n"
        "  calibration.jc1060p470c-v2 = 10,1023,16,599,-\r\n"
        "mode=direct\n"
        "hold_ms=500\n"
        "slop_px=12\n"
        "tapdrag_ms=0\n"
        "two_finger_right=0\n"
        "backlight=75\n"                     /* no longer a touch key */
        "unknown_key=3\n";
    struct TouchPrefs p;

    defaults(&p);
    touch_prefs_parse(&p, text, sizeof(text) - 1, "jc1060p470c-v2");
    CHECK(p.have == (TOUCH_PREFS_HAVE_CAL | TOUCH_PREFS_HAVE_MODE
                     | TOUCH_PREFS_HAVE_PARAMS));
    CHECK(p.cal.x_min == 10 && p.cal.x_max == 1023);
    CHECK(p.cal.y_min == 16 && p.cal.y_max == 599 && p.cal.flags == 0);
    CHECK(p.mode == TOUCH_DIRECT);
    CHECK(p.params.hold_ms == 500 && p.params.slop_px == 12);
    CHECK(p.params.tapdrag_ms == 0 && p.params.two_finger_right == 0);
    CHECK(p.params.tapdrag_px == TOUCH_TAPDRAG_PX);

    defaults(&p);
    touch_prefs_parse(&p, text, sizeof(text) - 1, "d1001");
    CHECK(p.cal.x_min == 16 && p.cal.x_max == 1638);
    CHECK(p.cal.flags == TOUCH_PREFS_MIRROR_Y);

    /* A name without a line keeps its default calibration. */
    defaults(&p);
    touch_prefs_parse(&p, text, sizeof(text) - 1, "other");
    CHECK(!(p.have & TOUCH_PREFS_HAVE_CAL));
}

static void test_malformed_ignored(void)
{
    static const char text[] =
        "calibration.b=100,10,0,599,-\n"      /* min >= max */
        "calibration.b=0,1023,0,599,q\n"      /* bad flag */
        "calibration.b=0,1023,0,599\n"        /* missing field */
        "calibration.b=0,1023,0,599,-,9\n"    /* extra field */
        "calibration.bb=0,1023,0,599,-\n"     /* other name, prefix */
        "hold_ms=20\n"                        /* below bound */
        "slop_px=x\n"
        "mode=fast\n"
        "release_polls=99999999999\n"
        "novalue\n"
        "=5\n";
    struct TouchPrefs p;

    defaults(&p);
    touch_prefs_parse(&p, text, sizeof(text) - 1, "b");
    CHECK(p.have == 0);
    CHECK(p.params.hold_ms == TOUCH_HOLD_MS);
    CHECK(p.mode == TOUCH_TAP);
}

static void test_round_trip_keeps_other_names(void)
{
    static const char old[] =
        "calibration.d1001=16,1638,15,874,y\n"
        "calibration.jc1060p470c-v2=0,1023,0,599,-\n"
        "backlight=20\n";                    /* dropped on rewrite */
    struct TouchPrefs p, q;
    char buf[1024];
    size_t n;

    defaults(&p);
    p.have = TOUCH_PREFS_HAVE_CAL | TOUCH_PREFS_HAVE_MODE
           | TOUCH_PREFS_HAVE_PARAMS;
    p.cal.x_min = 10; p.cal.x_max = 1023;
    p.cal.y_min = 16; p.cal.y_max = 599;
    p.cal.flags = TOUCH_PREFS_SWAP_XY | TOUCH_PREFS_MIRROR_X;
    p.params.tapdrag_ms = 300;
    n = touch_prefs_format(&p, "jc1060p470c-v2", old, sizeof(old) - 1,
                             buf, sizeof(buf));
    CHECK(n > 0 && n == strlen(buf));
    CHECK(strstr(buf, "calibration.d1001=16,1638,15,874,y\n") != NULL);
    CHECK(strstr(buf, "calibration.jc1060p470c-v2=10,1023,16,599,sx\n"));
    CHECK(strstr(buf, "calibration.jc1060p470c-v2=0,") == NULL);
    CHECK(strstr(buf, "backlight") == NULL);

    defaults(&q);
    touch_prefs_parse(&q, buf, n, "jc1060p470c-v2");
    CHECK(q.have == p.have);
    CHECK(memcmp(&q.cal, &p.cal, sizeof(q.cal)) == 0);
    CHECK(memcmp(&q.params, &p.params, sizeof(q.params)) == 0);
    CHECK(q.mode == p.mode);

    defaults(&q);
    touch_prefs_parse(&q, buf, n, "d1001");
    CHECK(q.cal.x_max == 1638 && q.cal.flags == TOUCH_PREFS_MIRROR_Y);

    /* Too small a buffer fails cleanly. */
    CHECK(touch_prefs_format(&p, "jc1060p470c-v2", NULL, 0, buf, 16) == 0);
    CHECK(buf[0] == 0);
}

int main(void)
{
    test_parse();
    test_malformed_ignored();
    test_round_trip_keeps_other_names();
    printf("prefs_test: %u checks passed\n", checks);
    return EXIT_SUCCESS;
}
