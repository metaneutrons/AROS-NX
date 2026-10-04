#include "../touch_policy.h"
#include "../touch_coords.h"

/* A D1001-like controller domain (GSL3670), as a fixed example. */
#define TEST_X_MIN 16U
#define TEST_X_MAX 1638U
#define TEST_Y_MIN 15U
#define TEST_Y_MAX 874U

#include <stdio.h>
#include <stdlib.h>

struct Event
{
    unsigned int action;
    unsigned int button;
    int16_t x;
    int16_t y;
};

struct EventLog
{
    struct Event events[16];
    size_t count;
};

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

static void record_event(void *context, unsigned int action,
                         unsigned int button, int16_t x, int16_t y)
{
    struct EventLog *log = context;

    CHECK(log->count < sizeof(log->events) / sizeof(log->events[0]));
    log->events[log->count++] = (struct Event){action, button, x, y};
}

static void check_event(const struct EventLog *log, size_t index,
                        unsigned int action, unsigned int button,
                        int16_t x, int16_t y)
{
    CHECK(index < log->count);
    CHECK(log->events[index].action == action);
    CHECK(log->events[index].button == button);
    CHECK(log->events[index].x == x);
    CHECK(log->events[index].y == y);
}

static size_t count_event(const struct EventLog *log, unsigned int action,
                          unsigned int button)
{
    size_t count = 0, i;

    for (i = 0; i < log->count; ++i)
        if (log->events[i].action == action
            && log->events[i].button == button)
            ++count;
    return count;
}

static void test_motion_over_slop_never_clicks(void)
{
    struct TouchPolicy policy;
    struct EventLog log = {0};

    touch_policy_init(&policy, TOUCH_TAP);
    touch_policy_step(&policy, 1, 100, 100, 10, record_event, &log);
    touch_policy_step(&policy, 1, 109, 100, 20, record_event, &log);
    touch_policy_step(&policy, 1, 109, 100, 1000, record_event, &log);
    touch_policy_step(&policy, 0, 109, 100, 1010, record_event, &log);
    touch_policy_step(&policy, 0, 109, 100, 1020, record_event, &log);

    CHECK(log.count == 2);
    CHECK(count_event(&log, TOUCH_PRESS, TOUCH_LEFT) == 0);
    CHECK(count_event(&log, TOUCH_RELEASE, TOUCH_LEFT) == 0);
}

static void test_radial_motion_over_slop_never_clicks(void)
{
    struct TouchPolicy policy;
    struct EventLog log = {0};

    touch_policy_init(&policy, TOUCH_TAP);
    touch_policy_step(&policy, 1, 40, 40, 0, record_event, &log);
    touch_policy_step(&policy, 1, 46, 46, 20, record_event, &log);
    touch_policy_step(&policy, 0, 46, 46, 30, record_event, &log);
    touch_policy_step(&policy, 0, 46, 46, 40, record_event, &log);

    CHECK(log.count == 2);
    CHECK(count_event(&log, TOUCH_PRESS, TOUCH_LEFT) == 0);
}

static void test_short_tap_uses_first_empty_report_time(void)
{
    struct TouchPolicy policy;
    struct EventLog log = {0};

    touch_policy_init(&policy, TOUCH_TAP);
    touch_policy_step(&policy, 1, 12, 34, 100, record_event, &log);
    touch_policy_step(&policy, 1, 12, 34, 200, record_event, &log);
    touch_policy_step(&policy, 0, 12, 34, 500, record_event, &log);
    CHECK(log.count == 3);
    touch_policy_step(&policy, 0, 12, 34, 900, record_event, &log);

    CHECK(log.count == 3);
    check_event(&log, 0, TOUCH_MOTION, TOUCH_NONE, 12, 34);
    check_event(&log, 1, TOUCH_PRESS, TOUCH_LEFT, 12, 34);
    check_event(&log, 2, TOUCH_RELEASE, TOUCH_LEFT, 12, 34);
}

static void test_jitter_at_eight_pixels_clicks(void)
{
    struct TouchPolicy policy;
    struct EventLog log = {0};

    touch_policy_init(&policy, TOUCH_TAP);
    touch_policy_step(&policy, 1, 20, 30, 0, record_event, &log);
    touch_policy_step(&policy, 1, 28, 30, 50, record_event, &log);
    touch_policy_step(&policy, 0, 28, 30, 60, record_event, &log);
    touch_policy_step(&policy, 0, 28, 30, 70, record_event, &log);

    CHECK(log.count == 4);
    check_event(&log, 0, TOUCH_MOTION, TOUCH_NONE, 20, 30);
    check_event(&log, 1, TOUCH_MOTION, TOUCH_NONE, 28, 30);
    check_event(&log, 2, TOUCH_PRESS, TOUCH_LEFT, 28, 30);
    check_event(&log, 3, TOUCH_RELEASE, TOUCH_LEFT, 28, 30);
}

static void test_stationary_hold_starts_drag_at_threshold(void)
{
    struct TouchPolicy policy;
    struct EventLog log = {0};

    touch_policy_init(&policy, TOUCH_TAP);
    touch_policy_step(&policy, 1, 5, 6, 1000, record_event, &log);
    touch_policy_step(&policy, 1, 5, 6, 1399, record_event, &log);
    CHECK(log.count == 1);
    touch_policy_step(&policy, 1, 5, 6, 1400, record_event, &log);
    CHECK(log.count == 2);
    check_event(&log, 1, TOUCH_PRESS, TOUCH_LEFT, 5, 6);
    touch_policy_step(&policy, 0, 5, 6, 1410, record_event, &log);
    CHECK(log.count == 2);
    touch_policy_step(&policy, 0, 5, 6, 1420, record_event, &log);

    CHECK(log.count == 3);
    check_event(&log, 2, TOUCH_RELEASE, TOUCH_LEFT, 5, 6);
}

static void test_two_fingers_select_right_without_left(void)
{
    struct TouchPolicy policy;
    struct EventLog log = {0};

    touch_policy_init(&policy, TOUCH_TAP);
    touch_policy_step(&policy, 2, 70, 80, 0, record_event, &log);
    touch_policy_step(&policy, 1, 70, 80, 30, record_event, &log);
    touch_policy_step(&policy, 0, 70, 80, 60, record_event, &log);
    CHECK(log.count == 2);
    touch_policy_step(&policy, 0, 70, 80, 90, record_event, &log);

    CHECK(log.count == 3);
    check_event(&log, 0, TOUCH_MOTION, TOUCH_NONE, 70, 80);
    check_event(&log, 1, TOUCH_PRESS, TOUCH_RIGHT, 70, 80);
    check_event(&log, 2, TOUCH_RELEASE, TOUCH_RIGHT, 70, 80);
    CHECK(count_event(&log, TOUCH_PRESS, TOUCH_LEFT) == 0);
}

static void test_drag_promotes_to_latched_right_button(void)
{
    struct TouchPolicy policy;
    struct EventLog log = {0};

    touch_policy_init(&policy, TOUCH_TAP);
    touch_policy_step(&policy, 1, 4, 9, 100, record_event, &log);
    touch_policy_step(&policy, 1, 4, 9, 500, record_event, &log);
    touch_policy_step(&policy, 2, 4, 9, 510, record_event, &log);
    touch_policy_step(&policy, 1, 4, 9, 520, record_event, &log);
    touch_policy_step(&policy, 0, 4, 9, 530, record_event, &log);
    touch_policy_step(&policy, 0, 4, 9, 540, record_event, &log);

    CHECK(log.count == 5);
    check_event(&log, 0, TOUCH_MOTION, TOUCH_NONE, 4, 9);
    check_event(&log, 1, TOUCH_PRESS, TOUCH_LEFT, 4, 9);
    check_event(&log, 2, TOUCH_RELEASE, TOUCH_LEFT, 4, 9);
    check_event(&log, 3, TOUCH_PRESS, TOUCH_RIGHT, 4, 9);
    check_event(&log, 4, TOUCH_RELEASE, TOUCH_RIGHT, 4, 9);
    CHECK(count_event(&log, TOUCH_PRESS, TOUCH_LEFT) == 1);
}

static void test_cancel_tap_and_drag(void)
{
    struct TouchPolicy policy;
    struct EventLog tap_log = {0};
    struct EventLog drag_log = {0};

    touch_policy_init(&policy, TOUCH_TAP);
    touch_policy_step(&policy, 1, 1, 2, 10, record_event, &tap_log);
    touch_policy_cancel(&policy, record_event, &tap_log);
    touch_policy_step(&policy, 0, 1, 2, 20, record_event, &tap_log);
    CHECK(tap_log.count == 1);
    CHECK(count_event(&tap_log, TOUCH_PRESS, TOUCH_LEFT) == 0);

    touch_policy_init(&policy, TOUCH_TAP);
    touch_policy_step(&policy, 1, 3, 4, 100, record_event, &drag_log);
    touch_policy_step(&policy, 1, 3, 4, 500, record_event, &drag_log);
    touch_policy_cancel(&policy, record_event, &drag_log);
    touch_policy_cancel(&policy, record_event, &drag_log);
    CHECK(drag_log.count == 3);
    check_event(&drag_log, 1, TOUCH_PRESS, TOUCH_LEFT, 3, 4);
    check_event(&drag_log, 2, TOUCH_RELEASE, TOUCH_LEFT, 3, 4);
}

static void test_direct_mode_press_move_release(void)
{
    struct TouchPolicy policy;
    struct EventLog log = {0};

    touch_policy_init(&policy, TOUCH_DIRECT);
    touch_policy_step(&policy, 1, 10, 20, 0, record_event, &log);
    touch_policy_step(&policy, 1, 19, 20, 10, record_event, &log);
    touch_policy_step(&policy, 0, 19, 20, 20, record_event, &log);
    CHECK(log.count == 3);
    touch_policy_step(&policy, 0, 19, 20, 30, record_event, &log);

    CHECK(log.count == 4);
    check_event(&log, 0, TOUCH_MOTION, TOUCH_NONE, 10, 20);
    check_event(&log, 1, TOUCH_PRESS, TOUCH_LEFT, 10, 20);
    check_event(&log, 2, TOUCH_MOTION, TOUCH_NONE, 19, 20);
    check_event(&log, 3, TOUCH_RELEASE, TOUCH_LEFT, 19, 20);
}

static void test_direct_mode_right_latch(void)
{
    struct TouchPolicy policy;
    struct EventLog log = {0};

    touch_policy_init(&policy, TOUCH_DIRECT);
    touch_policy_step(&policy, 1, 7, 8, 0, record_event, &log);
    touch_policy_step(&policy, 2, 7, 8, 10, record_event, &log);
    touch_policy_step(&policy, 1, 7, 8, 20, record_event, &log);
    touch_policy_step(&policy, 0, 7, 8, 30, record_event, &log);
    touch_policy_step(&policy, 0, 7, 8, 40, record_event, &log);

    CHECK(log.count == 5);
    check_event(&log, 0, TOUCH_MOTION, TOUCH_NONE, 7, 8);
    check_event(&log, 1, TOUCH_PRESS, TOUCH_LEFT, 7, 8);
    check_event(&log, 2, TOUCH_RELEASE, TOUCH_LEFT, 7, 8);
    check_event(&log, 3, TOUCH_PRESS, TOUCH_RIGHT, 7, 8);
    check_event(&log, 4, TOUCH_RELEASE, TOUCH_RIGHT, 7, 8);
    CHECK(count_event(&log, TOUCH_PRESS, TOUCH_RIGHT) == 1);
}

static void test_tap_deadline_across_uint32_wrap(void)
{
    struct TouchPolicy policy;
    struct EventLog log = {0};
    uint32_t start = UINT32_MAX - 300U;

    touch_policy_init(&policy, TOUCH_TAP);
    touch_policy_step(&policy, 1, 50, 60, start, record_event, &log);
    touch_policy_step(&policy, 0, 50, 60, 99U, record_event, &log);
    touch_policy_step(&policy, 0, 50, 60, 500U, record_event, &log);

    CHECK(log.count == 3);
    check_event(&log, 1, TOUCH_PRESS, TOUCH_LEFT, 50, 60);
    check_event(&log, 2, TOUCH_RELEASE, TOUCH_LEFT, 50, 60);
}

static void test_hold_deadline_across_uint32_wrap(void)
{
    struct TouchPolicy policy;
    struct EventLog log = {0};
    uint32_t start = UINT32_MAX - 199U;

    touch_policy_init(&policy, TOUCH_TAP);
    touch_policy_step(&policy, 1, 11, 12, start, record_event, &log);
    touch_policy_step(&policy, 1, 11, 12, 199U, record_event, &log);
    CHECK(log.count == 1);
    touch_policy_step(&policy, 1, 11, 12, 200U, record_event, &log);
    CHECK(log.count == 2);
    check_event(&log, 1, TOUCH_PRESS, TOUCH_LEFT, 11, 12);
    touch_policy_step(&policy, 0, 11, 12, 201U, record_event, &log);
    touch_policy_step(&policy, 0, 11, 12, 202U, record_event, &log);
    CHECK(log.count == 3);
    check_event(&log, 2, TOUCH_RELEASE, TOUCH_LEFT, 11, 12);
}

static void test_single_empty_report_preserves_direct_press(void)
{
    struct TouchPolicy policy;
    struct EventLog log = {0};

    touch_policy_init(&policy, TOUCH_DIRECT);
    touch_policy_step(&policy, 1, 15, 16, 100, record_event, &log);
    touch_policy_step(&policy, 0, 15, 16, 150, record_event, &log);
    CHECK(log.count == 2);
    touch_policy_step(&policy, 1, 15, 16, 160, record_event, &log);
    CHECK(log.count == 2);
    touch_policy_step(&policy, 0, 15, 16, 170, record_event, &log);
    CHECK(log.count == 2);
    touch_policy_step(&policy, 0, 15, 16, 180, record_event, &log);

    CHECK(log.count == 3);
    check_event(&log, 0, TOUCH_MOTION, TOUCH_NONE, 15, 16);
    check_event(&log, 1, TOUCH_PRESS, TOUCH_LEFT, 15, 16);
    check_event(&log, 2, TOUCH_RELEASE, TOUCH_LEFT, 15, 16);
}

static void test_fast_double_tap(void)
{
    struct TouchPolicy policy;
    struct EventLog log = {0};

    touch_policy_init(&policy, TOUCH_TAP);
    touch_policy_step(&policy, 1, 50, 60, 0, record_event, &log);
    touch_policy_step(&policy, 0, 50, 60, 60, record_event, &log);
    /* Only one empty sample between taps: previously merged into one. */
    touch_policy_step(&policy, 1, 50, 60, 100, record_event, &log);
    touch_policy_step(&policy, 0, 50, 60, 160, record_event, &log);
    CHECK(count_event(&log, TOUCH_PRESS, TOUCH_LEFT) == 2);
    CHECK(count_event(&log, TOUCH_RELEASE, TOUCH_LEFT) == 2);
    CHECK(policy.active == 0 && policy.button == TOUCH_NONE);
    check_event(&log, 1, TOUCH_PRESS, TOUCH_LEFT, 50, 60);
    check_event(&log, 2, TOUCH_RELEASE, TOUCH_LEFT, 50, 60);
    check_event(&log, 4, TOUCH_PRESS, TOUCH_LEFT, 50, 60);
    check_event(&log, 5, TOUCH_RELEASE, TOUCH_LEFT, 50, 60);
}

static void test_double_tap_and_drag(void)
{
    struct TouchPolicy policy;
    struct EventLog log = {0};

    touch_policy_init(&policy, TOUCH_TAP);
    touch_policy_step(&policy, 1, 100, 100, 0, record_event, &log);
    touch_policy_step(&policy, 0, 100, 100, 80, record_event, &log);
    /* Second contact 150 ms later, 3 px away: left goes down at once. */
    touch_policy_step(&policy, 1, 102, 101, 230, record_event, &log);
    CHECK(policy.button == TOUCH_LEFT);
    check_event(&log, 4, TOUCH_PRESS, TOUCH_LEFT, 102, 101);
    touch_policy_step(&policy, 1, 160, 150, 280, record_event, &log);
    touch_policy_step(&policy, 1, 200, 200, 330, record_event, &log);
    CHECK(policy.button == TOUCH_LEFT);
    /* A held drag debounces its release like any held button. */
    touch_policy_step(&policy, 0, 200, 200, 380, record_event, &log);
    CHECK(policy.button == TOUCH_LEFT);
    touch_policy_step(&policy, 0, 200, 200, 430, record_event, &log);
    CHECK(policy.active == 0 && policy.button == TOUCH_NONE);
    check_event(&log, log.count - 1, TOUCH_RELEASE, TOUCH_LEFT,
                200, 200);
    CHECK(count_event(&log, TOUCH_PRESS, TOUCH_LEFT) == 2);
    CHECK(count_event(&log, TOUCH_RELEASE, TOUCH_LEFT) == 2);
    /* The drag does not arm another one. */
    CHECK(!policy.tap_valid);
}

static void test_late_or_distant_second_tap_is_plain(void)
{
    struct TouchPolicy policy;
    struct EventLog log = {0};

    touch_policy_init(&policy, TOUCH_TAP);
    touch_policy_step(&policy, 1, 100, 100, 0, record_event, &log);
    touch_policy_step(&policy, 0, 100, 100, 80, record_event, &log);
    /* Too late: no press at touch-down. */
    touch_policy_step(&policy, 1, 100, 100, 80 + TOUCH_TAPDRAG_MS + 1,
                        record_event, &log);
    CHECK(policy.button == TOUCH_NONE);
    touch_policy_step(&policy, 0, 100, 100, 500, record_event, &log);
    /* That was a tap again; now a contact too far away. */
    touch_policy_step(&policy, 1, 100 + TOUCH_TAPDRAG_PX + 1, 100, 550,
                        record_event, &log);
    CHECK(policy.button == TOUCH_NONE);
    touch_policy_step(&policy, 0, 100 + TOUCH_TAPDRAG_PX + 1, 100, 600,
                        record_event, &log);
    CHECK(count_event(&log, TOUCH_PRESS, TOUCH_LEFT) == 3);
    CHECK(count_event(&log, TOUCH_RELEASE, TOUCH_LEFT) == 3);
}

static void test_runtime_params(void)
{
    struct TouchPolicy policy;
    struct EventLog log = {0};
    struct TouchParams params;

    touch_policy_init(&policy, TOUCH_TAP);
    touch_params_default(&params);
    CHECK(params.hold_ms == TOUCH_HOLD_MS && params.two_finger_right);
    params.hold_ms = 200;
    params.two_finger_right = 0;
    params.tapdrag_ms = 0;
    touch_policy_set_params(&policy, &params);
    /* Applies at the next contact: hold now starts the drag at 200 ms. */
    touch_policy_step(&policy, 1, 10, 10, 0, record_event, &log);
    touch_policy_step(&policy, 1, 10, 10, 199, record_event, &log);
    CHECK(policy.button == TOUCH_NONE);
    touch_policy_step(&policy, 1, 10, 10, 200, record_event, &log);
    CHECK(policy.button == TOUCH_LEFT);
    /* A second finger no longer selects the right button. */
    touch_policy_step(&policy, 2, 12, 10, 250, record_event, &log);
    CHECK(policy.button == TOUCH_LEFT);
    touch_policy_step(&policy, 0, 12, 10, 300, record_event, &log);
    touch_policy_step(&policy, 0, 12, 10, 350, record_event, &log);
    CHECK(policy.button == TOUCH_NONE);
    CHECK(count_event(&log, TOUCH_PRESS, TOUCH_RIGHT) == 0);
    /* With tapdrag_ms 0 a tap followed by a quick touch stays plain. */
    touch_policy_step(&policy, 1, 50, 50, 1000, record_event, &log);
    touch_policy_step(&policy, 0, 50, 50, 1050, record_event, &log);
    touch_policy_step(&policy, 1, 50, 50, 1100, record_event, &log);
    CHECK(policy.button == TOUCH_NONE);
}

static void test_runtime_mode(void)
{
    struct TouchPolicy policy;
    struct EventLog log = {0};

    touch_policy_init(&policy, TOUCH_TAP);
    touch_policy_step(&policy, 1, 10, 10, 0, record_event, &log);
    /* Switching during a contact waits for the next one. */
    touch_policy_set_mode(&policy, TOUCH_DIRECT);
    CHECK(policy.mode == TOUCH_TAP);
    touch_policy_step(&policy, 0, 10, 10, 50, record_event, &log);
    touch_policy_step(&policy, 0, 10, 10, 100, record_event, &log);
    CHECK(count_event(&log, TOUCH_PRESS, TOUCH_LEFT) == 1);
    /* The tap just made must not turn the next touch into a tap-drag. */
    log.count = 0;
    touch_policy_step(&policy, 1, 10, 10, 150, record_event, &log);
    CHECK(policy.mode == TOUCH_DIRECT);
    CHECK(policy.tapdrag == 0);
    CHECK(policy.button == TOUCH_LEFT);
    touch_policy_step(&policy, 0, 10, 10, 200, record_event, &log);
    touch_policy_step(&policy, 0, 10, 10, 250, record_event, &log);
    CHECK(policy.button == TOUCH_NONE);
    /* And back: a direct contact presses nothing until the hold time. */
    touch_policy_set_mode(&policy, TOUCH_TAP);
    touch_policy_step(&policy, 1, 10, 10, 1000, record_event, &log);
    CHECK(policy.mode == TOUCH_TAP);
    CHECK(policy.button == TOUCH_NONE);
    touch_policy_cancel(&policy, record_event, &log);
}

static void test_coordinates(void)
{
    uint32_t raw;
    int16_t previous_x = 0, previous_y = 799;

    CHECK(touch_coordinate(16, 16, 1638, 1280, 0) == 0);
    CHECK(touch_coordinate(1638, 16, 1638, 1280, 0) == 1279);
    CHECK(touch_coordinate(15, 15, 874, 800, 1) == 799);
    CHECK(touch_coordinate(874, 15, 874, 800, 1) == 0);
    CHECK(touch_coordinate(0, 15, 874, 800, 1) == 799);
    CHECK(touch_coordinate(UINT32_MAX, 15, 874, 800, 1) == 0);
    CHECK(touch_coordinate(UINT32_MAX, 0, 65535, 32768, 0) == 32767);
    CHECK(touch_coordinate(12, 16, 16, 800, 0) == 0);
    CHECK(touch_coordinate(12, 16, 15, 800, 0) == 0);
    CHECK(touch_coordinate(12, 0, 15, 0, 0) == 0);
    CHECK(touch_coordinate(12, 0, 15, 65535, 0) == 0);
    CHECK(touch_coordinate(12, 0, 15, 1, 1) == 0);
    for (raw = 0; raw < 1800; ++raw)
    {
        int16_t x = touch_coordinate(raw, TEST_X_MIN,
                                       TEST_X_MAX, 1280, 0);
        int16_t y = touch_coordinate(raw, TEST_Y_MIN,
                                       TEST_Y_MAX, 800, 1);
        CHECK(x >= previous_x && x <= 1279);
        CHECK(y <= previous_y && y >= 0);
        previous_x = x;
        previous_y = y;
    }
}

int main(void)
{
    test_coordinates();
    test_motion_over_slop_never_clicks();
    test_radial_motion_over_slop_never_clicks();
    test_short_tap_uses_first_empty_report_time();
    test_jitter_at_eight_pixels_clicks();
    test_stationary_hold_starts_drag_at_threshold();
    test_two_fingers_select_right_without_left();
    test_drag_promotes_to_latched_right_button();
    test_cancel_tap_and_drag();
    test_direct_mode_press_move_release();
    test_direct_mode_right_latch();
    test_tap_deadline_across_uint32_wrap();
    test_hold_deadline_across_uint32_wrap();
    test_single_empty_report_preserves_direct_press();
    test_fast_double_tap();
    test_double_tap_and_drag();
    test_late_or_distant_second_tap_is_plain();
    test_runtime_params();
    test_runtime_mode();

    printf("policy_test: %u checks passed\n", checks);
    return EXIT_SUCCESS;
}
