/* Touch-to-mouse policy, independent of the controller and AROS callbacks. */
#ifndef TOUCHSCREEN_POLICY_H
#define TOUCHSCREEN_POLICY_H

#include <stdint.h>

/* Default parameters; the preferences replace them at runtime. */
#define TOUCH_HOLD_MS 400U
#define TOUCH_SLOP_PX 8
#define TOUCH_RELEASE_POLLS 2U
/* Tap, then touch again within this time and radius: the second contact
   holds the left button (double-tap-and-drag, e.g. to drag a selection). */
#define TOUCH_TAPDRAG_MS 350U
#define TOUCH_TAPDRAG_PX 32

enum TouchMode { TOUCH_TAP, TOUCH_DIRECT };
enum TouchAction { TOUCH_MOTION, TOUCH_PRESS, TOUCH_RELEASE };
enum TouchButton { TOUCH_NONE, TOUCH_LEFT, TOUCH_RIGHT };

/* The tunable part of the policy, so a preferences editor can change it
   without rebuilding the HIDD.  tapdrag_ms 0 disables double-tap-and-drag;
   two_finger_right 0 treats extra fingers as the first one. */
struct TouchParams
{
    uint32_t hold_ms, release_polls, tapdrag_ms;
    int32_t slop_px, tapdrag_px;
    unsigned int two_finger_right;
};

struct TouchPolicy
{
    struct TouchParams params;
    uint32_t started_ms, lifted_ms, tap_ms;
    int16_t origin_x, origin_y, x, y, tap_x, tap_y;
    unsigned int mode, active, moved, button, zero_polls;
    unsigned int tap_valid, tapdrag;
    struct TouchParams pending;
    unsigned int have_pending, pending_mode, have_pending_mode;
};

typedef void (*TouchEmit)(void *context, unsigned int action,
                           unsigned int button, int16_t x, int16_t y);

void touch_params_default(struct TouchParams *params);
/* Resets the policy to idle with the default parameters. */
void touch_policy_init(struct TouchPolicy *policy, unsigned int mode);
/* Takes effect at the next contact; a gesture in progress keeps its own. */
void touch_policy_set_params(struct TouchPolicy *policy,
                               const struct TouchParams *params);
/* TOUCH_TAP or TOUCH_DIRECT, also from the next contact on. */
void touch_policy_set_mode(struct TouchPolicy *policy, unsigned int mode);
void touch_policy_step(struct TouchPolicy *policy, unsigned int contacts,
                         int16_t x, int16_t y, uint32_t now_ms,
                         TouchEmit emit, void *context);
/* Errors and shutdown release a held button without synthesizing a tap. */
void touch_policy_cancel(struct TouchPolicy *policy,
                           TouchEmit emit, void *context);

#endif
