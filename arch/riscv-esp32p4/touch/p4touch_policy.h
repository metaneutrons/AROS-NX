/* Touch-to-mouse policy, independent of the controller and AROS callbacks. */
#ifndef ESP32P4_TOUCH_POLICY_H
#define ESP32P4_TOUCH_POLICY_H

#include <stdint.h>

#define P4_TOUCH_HOLD_MS 400U
#define P4_TOUCH_SLOP_PX 8
#define P4_TOUCH_RELEASE_POLLS 2U

enum P4TouchMode { P4_TOUCH_TAP, P4_TOUCH_DIRECT };
enum P4TouchAction { P4_TOUCH_MOTION, P4_TOUCH_PRESS, P4_TOUCH_RELEASE };
enum P4TouchButton { P4_TOUCH_NONE, P4_TOUCH_LEFT, P4_TOUCH_RIGHT };

struct P4TouchPolicy
{
    uint32_t started_ms, lifted_ms;
    int16_t origin_x, origin_y, x, y;
    unsigned int mode, active, moved, button, zero_polls;
};

typedef void (*P4TouchEmit)(void *context, unsigned int action,
                           unsigned int button, int16_t x, int16_t y);

void p4touch_policy_init(struct P4TouchPolicy *policy, unsigned int mode);
void p4touch_policy_step(struct P4TouchPolicy *policy, unsigned int contacts,
                         int16_t x, int16_t y, uint32_t now_ms,
                         P4TouchEmit emit, void *context);
/* Errors and shutdown release a held button without synthesizing a tap. */
void p4touch_policy_cancel(struct P4TouchPolicy *policy,
                           P4TouchEmit emit, void *context);

#endif
