#include "p4_semaphore_cancel_retry.h"
#include <stddef.h>

static int p4_cancel_retry_valid(const struct p4_semaphore_cancel_retry *state)
{
    return state != NULL &&
        (uintptr_t)state % _Alignof(struct p4_semaphore_cancel_retry) == 0 &&
        state->active <= 1u && state->waiting <= 1u &&
        (state->active == 0 || state->waiting == 1u);
}

int p4_semaphore_cancel_retry_begin(struct p4_semaphore_cancel_retry *state)
{
    if (!p4_cancel_retry_valid(state))
        return -1;
    if (state->active != 0)
        return 0;
    state->active = 1u;
    state->waiting = 1u;
    return 1;
}

int p4_semaphore_cancel_retry_end(struct p4_semaphore_cancel_retry *state,
    uint32_t blocked)
{
    if (!p4_cancel_retry_valid(state) || blocked > 1u)
        return -1;
    if (state->active == 0)
        return 0;
    state->active = 0;
    state->waiting = blocked;
    return 1;
}

int p4_semaphore_cancel_retry_event(const struct p4_semaphore_cancel_retry *state)
{
    if (!p4_cancel_retry_valid(state))
        return -1;
    return (int)state->waiting;
}
