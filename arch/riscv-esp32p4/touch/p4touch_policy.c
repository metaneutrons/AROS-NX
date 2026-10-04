#include "p4touch_policy.h"

void p4touch_params_default(struct P4TouchParams *params)
{
    params->hold_ms = P4_TOUCH_HOLD_MS;
    params->release_polls = P4_TOUCH_RELEASE_POLLS;
    params->tapdrag_ms = P4_TOUCH_TAPDRAG_MS;
    params->slop_px = P4_TOUCH_SLOP_PX;
    params->tapdrag_px = P4_TOUCH_TAPDRAG_PX;
    params->two_finger_right = 1;
}

void p4touch_policy_init(struct P4TouchPolicy *p, unsigned int mode)
{
    *p = (struct P4TouchPolicy){0};
    p->mode = mode;
    p4touch_params_default(&p->params);
}

void p4touch_policy_set_params(struct P4TouchPolicy *p,
                               const struct P4TouchParams *params)
{
    p->pending = *params;
    p->have_pending = 1;
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
    p->active = p->moved = p->zero_polls = p->tapdrag = 0;
}

/* Is a new contact the second half of a double-tap-and-drag? */
static int p4touch_tapdrag_starts(const struct P4TouchPolicy *p,
                                  int16_t x, int16_t y, uint32_t now_ms)
{
    int32_t dx = (int32_t)x - p->tap_x;
    int32_t dy = (int32_t)y - p->tap_y;

    return p->mode == P4_TOUCH_TAP && p->tap_valid && p->params.tapdrag_ms
        && (uint32_t)(now_ms - p->tap_ms) <= p->params.tapdrag_ms
        && dx >= -p->params.tapdrag_px && dx <= p->params.tapdrag_px
        && dy >= -p->params.tapdrag_px && dy <= p->params.tapdrag_px;
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
        if (++p->zero_polls < p->params.release_polls
            && !(p->mode == P4_TOUCH_TAP
                 && (p->button == P4_TOUCH_NONE
                     || (p->tapdrag && !p->moved))))
            return;
        /* Use the first empty report's time, not the debounce delay. */
        if (p->mode == P4_TOUCH_TAP && !p->moved
            && p->button == P4_TOUCH_NONE
            && (uint32_t)(p->lifted_ms - p->started_ms) <= p->params.hold_ms)
        {
            p4touch_button(p, P4_TOUCH_LEFT, emit, context);
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
        p4touch_policy_cancel(p, emit, context);
        return;
    }

    if (!p->active)
    {
        if (p->have_pending)
        {
            p->params = p->pending;
            p->have_pending = 0;
        }
        p->started_ms = now_ms;
        p->origin_x = x;
        p->origin_y = y;
        p->active = 1;
        p->moved = 0;
        p->tapdrag = p4touch_tapdrag_starts(p, x, y, now_ms);
        p->tap_valid = 0;
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
        int32_t slop = p->params.slop_px;

        if (dx < -slop || dx > slop || dy < -slop || dy > slop
            || dx * dx + dy * dy > slop * slop)
            p->moved = 1;
    }

    /* Button2 remains latched through sequential finger lift. */
    if ((contacts > 1 && p->params.two_finger_right)
        || p->button == P4_TOUCH_RIGHT)
        p4touch_button(p, P4_TOUCH_RIGHT, emit, context);
    else if (p->mode == P4_TOUCH_DIRECT || p->tapdrag
             || (!p->moved
                 && (uint32_t)(now_ms - p->started_ms) >= p->params.hold_ms))
        p4touch_button(p, P4_TOUCH_LEFT, emit, context);
}
