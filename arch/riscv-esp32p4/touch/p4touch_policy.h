/* Touch-to-mouse policy, independent of the controller and AROS callbacks. */
#ifndef ESP32P4_TOUCH_POLICY_H
#define ESP32P4_TOUCH_POLICY_H

#include <stdint.h>

/* Default parameters; the HIDD may replace them at runtime (ROADMAP J5). */
#define P4_TOUCH_HOLD_MS 400U
#define P4_TOUCH_SLOP_PX 8
#define P4_TOUCH_RELEASE_POLLS 2U
/* Tap, then touch again within this time and radius: the second contact
   holds the left button (double-tap-and-drag, e.g. to drag a selection). */
#define P4_TOUCH_TAPDRAG_MS 350U
#define P4_TOUCH_TAPDRAG_PX 32

enum P4TouchMode { P4_TOUCH_TAP, P4_TOUCH_DIRECT };
enum P4TouchAction { P4_TOUCH_MOTION, P4_TOUCH_PRESS, P4_TOUCH_RELEASE };
enum P4TouchButton { P4_TOUCH_NONE, P4_TOUCH_LEFT, P4_TOUCH_RIGHT };

/* The tunable part of the policy, so a preferences editor can change it
   without rebuilding the HIDD.  tapdrag_ms 0 disables double-tap-and-drag;
   two_finger_right 0 treats extra fingers as the first one. */
struct P4TouchParams
{
    uint32_t hold_ms, release_polls, tapdrag_ms;
    int32_t slop_px, tapdrag_px;
    unsigned int two_finger_right;
};

struct P4TouchPolicy
{
    struct P4TouchParams params;
    uint32_t started_ms, lifted_ms, tap_ms;
    int16_t origin_x, origin_y, x, y, tap_x, tap_y;
    unsigned int mode, active, moved, button, zero_polls;
    unsigned int tap_valid, tapdrag;
    struct P4TouchParams pending;
    unsigned int have_pending;
};

typedef void (*P4TouchEmit)(void *context, unsigned int action,
                           unsigned int button, int16_t x, int16_t y);

void p4touch_params_default(struct P4TouchParams *params);
/* Resets the policy to idle with the default parameters. */
void p4touch_policy_init(struct P4TouchPolicy *policy, unsigned int mode);
/* Takes effect at the next contact; a gesture in progress keeps its own. */
void p4touch_policy_set_params(struct P4TouchPolicy *policy,
                               const struct P4TouchParams *params);
void p4touch_policy_step(struct P4TouchPolicy *policy, unsigned int contacts,
                         int16_t x, int16_t y, uint32_t now_ms,
                         P4TouchEmit emit, void *context);
/* Errors and shutdown release a held button without synthesizing a tap. */
void p4touch_policy_cancel(struct P4TouchPolicy *policy,
                           P4TouchEmit emit, void *context);

#endif
