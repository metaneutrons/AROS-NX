#include "touch_policy.h"

void touch_params_default(struct TouchParams *params)
{
    params->hold_ms = TOUCH_HOLD_MS;
    params->release_polls = TOUCH_RELEASE_POLLS;
    params->tapdrag_ms = TOUCH_TAPDRAG_MS;
    params->slop_px = TOUCH_SLOP_PX;
    params->tapdrag_px = TOUCH_TAPDRAG_PX;
    params->two_finger_right = 1;
}

void touch_policy_init(struct TouchPolicy *p, unsigned int mode)
{
    *p = (struct TouchPolicy){0};
    p->mode = mode;
    touch_params_default(&p->params);
}

void touch_policy_set_params(struct TouchPolicy *p,
                               const struct TouchParams *params)
{
    p->pending = *params;
    p->have_pending = 1;
}

void touch_policy_set_mode(struct TouchPolicy *p, unsigned int mode)
{
    p->pending_mode = mode;
    p->have_pending_mode = 1;
}

static void touch_button(struct TouchPolicy *p, unsigned int button,
                           TouchEmit emit, void *context)
{
    if (p->button == button)
        return;
    if (p->button != TOUCH_NONE)
        emit(context, TOUCH_RELEASE, p->button, p->x, p->y);
    p->button = button;
    if (button != TOUCH_NONE)
        emit(context, TOUCH_PRESS, button, p->x, p->y);
}

void touch_policy_cancel(struct TouchPolicy *p,
                           TouchEmit emit, void *context)
{
    touch_button(p, TOUCH_NONE, emit, context);
    p->active = p->moved = p->zero_polls = p->tapdrag = 0;
}

/* Is a new contact the second half of a double-tap-and-drag? */
static int touch_tapdrag_starts(const struct TouchPolicy *p,
                                  int16_t x, int16_t y, uint32_t now_ms)
{
    int32_t dx = (int32_t)x - p->tap_x;
    int32_t dy = (int32_t)y - p->tap_y;

    return p->mode == TOUCH_TAP && p->tap_valid && p->params.tapdrag_ms
        && (uint32_t)(now_ms - p->tap_ms) <= p->params.tapdrag_ms
        && dx >= -p->params.tapdrag_px && dx <= p->params.tapdrag_px
        && dy >= -p->params.tapdrag_px && dy <= p->params.tapdrag_px;
}

void touch_policy_step(struct TouchPolicy *p, unsigned int contacts,
                         int16_t x, int16_t y, uint32_t now_ms,
                         TouchEmit emit, void *context)
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
        if (++p->zero_polls < p->params.release_polls
            && !(p->mode == TOUCH_TAP
                 && (p->button == TOUCH_NONE
                     || (p->tapdrag && !p->moved))))
            return;
        /* Use the first empty report's time, not the debounce delay. */
        if (p->mode == TOUCH_TAP && !p->moved
            && p->button == TOUCH_NONE
            && (uint32_t)(p->lifted_ms - p->started_ms) <= p->params.hold_ms)
        {
            touch_button(p, TOUCH_LEFT, emit, context);
            /* A tap may start a double-tap-and-drag; the second half of
               one may not start another. */
            p->tap_valid = 1;
            p->tap_ms = p->lifted_ms;
            p->tap_x = p->x;
            p->tap_y = p->y;
        }
        else
            p->tap_valid = 0;
        if (p->tapdrag)
            p->tap_valid = 0;
        touch_policy_cancel(p, emit, context);
        return;
    }

    if (!p->active)
    {
        if (p->have_pending)
        {
            p->params = p->pending;
            p->have_pending = 0;
        }
        if (p->have_pending_mode)
        {
            /* A tap from the old mode does not start a tap-drag. */
            if (p->mode != p->pending_mode)
                p->tap_valid = 0;
            p->mode = p->pending_mode;
            p->have_pending_mode = 0;
        }
        p->started_ms = now_ms;
        p->origin_x = x;
        p->origin_y = y;
        p->active = 1;
        p->moved = 0;
        p->tapdrag = touch_tapdrag_starts(p, x, y, now_ms);
        p->tap_valid = 0;
        emit(context, TOUCH_MOTION, TOUCH_NONE, x, y);
    }
    else if (x != p->x || y != p->y)
        emit(context, TOUCH_MOTION, TOUCH_NONE, x, y);
    p->x = x;
    p->y = y;
    p->zero_polls = 0;

    {
        int32_t dx = (int32_t)x - p->origin_x;
        int32_t dy = (int32_t)y - p->origin_y;

        /* Bound axes first: avoids squaring arbitrary int16 coordinates. */
        int32_t slop = p->params.slop_px;

        if (dx < -slop || dx > slop || dy < -slop || dy > slop
            || dx * dx + dy * dy > slop * slop)
            p->moved = 1;
    }

    /* Button2 remains latched through sequential finger lift. */
    if ((contacts > 1 && p->params.two_finger_right)
        || p->button == TOUCH_RIGHT)
        touch_button(p, TOUCH_RIGHT, emit, context);
    else if (p->mode == TOUCH_DIRECT || p->tapdrag
             || (!p->moved
                 && (uint32_t)(now_ms - p->started_ms) >= p->params.hold_ms))
        touch_button(p, TOUCH_LEFT, emit, context);
}
