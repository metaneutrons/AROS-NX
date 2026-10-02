#include "p4touch_policy.h"

void p4touch_policy_init(struct P4TouchPolicy *p, unsigned int mode)
{
    *p = (struct P4TouchPolicy){0};
    p->mode = mode;
}

static void p4touch_button(struct P4TouchPolicy *p, unsigned int button,
                           P4TouchEmit emit, void *context)
{
    if (p->button == button)
        return;
    if (p->button != P4_TOUCH_NONE)
        emit(context, P4_TOUCH_RELEASE, p->button, p->x, p->y);
    p->button = button;
    if (button != P4_TOUCH_NONE)
        emit(context, P4_TOUCH_PRESS, button, p->x, p->y);
}

void p4touch_policy_cancel(struct P4TouchPolicy *p,
                           P4TouchEmit emit, void *context)
{
    p4touch_button(p, P4_TOUCH_NONE, emit, context);
    p->active = p->moved = p->zero_polls = 0;
}

void p4touch_policy_step(struct P4TouchPolicy *p, unsigned int contacts,
                         int16_t x, int16_t y, uint32_t now_ms,
                         P4TouchEmit emit, void *context)
{
    if (!contacts)
    {
        if (!p->active)
            return;
        if (!p->zero_polls)
            p->lifted_ms = now_ms;
        /* A short tap has no held button to protect from an empty-frame
         * glitch. Commit it on lift so a following tap cannot be swallowed
         * by drag-release debounce. Held left/right buttons still debounce. */
        if (++p->zero_polls < P4_TOUCH_RELEASE_POLLS
            && !(p->mode == P4_TOUCH_TAP && p->button == P4_TOUCH_NONE))
            return;
        /* Use the first empty report's time, not the debounce delay. */
        if (p->mode == P4_TOUCH_TAP && !p->moved
            && p->button == P4_TOUCH_NONE
            && (uint32_t)(p->lifted_ms - p->started_ms) <= P4_TOUCH_HOLD_MS)
        {
            p4touch_button(p, P4_TOUCH_LEFT, emit, context);
        }
        p4touch_policy_cancel(p, emit, context);
        return;
    }

    if (!p->active)
    {
        p->started_ms = now_ms;
        p->origin_x = x;
        p->origin_y = y;
        p->active = 1;
        p->moved = 0;
        emit(context, P4_TOUCH_MOTION, P4_TOUCH_NONE, x, y);
    }
    else if (x != p->x || y != p->y)
        emit(context, P4_TOUCH_MOTION, P4_TOUCH_NONE, x, y);
    p->x = x;
    p->y = y;
    p->zero_polls = 0;

    {
        int32_t dx = (int32_t)x - p->origin_x;
        int32_t dy = (int32_t)y - p->origin_y;

        /* Bound axes first: avoids squaring arbitrary int16 coordinates. */
        if (dx < -P4_TOUCH_SLOP_PX || dx > P4_TOUCH_SLOP_PX
            || dy < -P4_TOUCH_SLOP_PX || dy > P4_TOUCH_SLOP_PX
            || dx * dx + dy * dy > P4_TOUCH_SLOP_PX * P4_TOUCH_SLOP_PX)
            p->moved = 1;
    }

    /* Button2 remains latched through sequential finger lift. */
    if (contacts > 1 || p->button == P4_TOUCH_RIGHT)
        p4touch_button(p, P4_TOUCH_RIGHT, emit, context);
    else if (p->mode == P4_TOUCH_DIRECT
             || (!p->moved
                 && (uint32_t)(now_ms - p->started_ms) >= P4_TOUCH_HOLD_MS))
        p4touch_button(p, P4_TOUCH_LEFT, emit, context);
}
