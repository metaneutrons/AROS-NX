/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#include "hosted_epoch_reassembly.h"

static int valid_range(const void *p, size_t n)
{
    return p && n && (uintmax_t)n <= UINTPTR_MAX - (uintptr_t)p;
}
static int separate(const void *a, size_t na, const void *b, size_t nb)
{
    uintptr_t pa = (uintptr_t)a, pb = (uintptr_t)b;
    return valid_range(a, na) && valid_range(b, nb) &&
        (pa + na <= pb || pb + nb <= pa);
}
static void clear_bytes(struct p4_hosted_reassembly *state)
{
    volatile uint8_t *p = state->bytes;
    size_t i;
    for (i = 0; i < sizeof(state->bytes); ++i) p[i] = 0;
    state->used = 0;
    state->complete = 0;
    state->fragments = 0;
}
static enum p4_hosted_reassembly_result malformed(
    struct p4_hosted_reassembly *state)
{
    clear_bytes(state);
    state->poisoned = 1;
    return P4_HOSTED_REASSEMBLY_PROTOCOL;
}
int p4_hosted_reassembly_start(struct p4_hosted_reassembly *state,
                               uint32_t generation)
{
    if (!valid_range(state, sizeof(*state)) || !generation ||
        (state->initialized && state->generation == generation)) return 0;
    clear_bytes(state);
    state->generation = generation;
    state->sequence = 0;
    state->initialized = 1;
    state->poisoned = 0;
    return 1;
}
enum p4_hosted_reassembly_result p4_hosted_reassembly_feed(
    struct p4_hosted_reassembly *state, uint32_t generation,
    const struct p4_hosted_frame *frame)
{
    struct hosted_serial_message decoded;
    size_t i;
    volatile uint8_t *out;
    const volatile uint8_t *in;
    if (!separate(state, sizeof(*state), frame, sizeof(*frame)))
        return P4_HOSTED_REASSEMBLY_REFUSED;
    if (!separate(state, sizeof(*state), frame->payload, frame->payload_size) ||
        !separate(frame, sizeof(*frame), frame->payload, frame->payload_size) ||
        !generation || state->initialized != 1 ||
        state->generation != generation || state->poisoned || state->complete)
        return P4_HOSTED_REASSEMBLY_REFUSED;
    if (frame->interface_type != P4_HOSTED_SERIAL ||
        frame->interface_number != 0 || frame->packet_type != 0 ||
        frame->throttle != 0 || (frame->flags & ~P4_HOSTED_MORE_FRAGMENT) ||
        frame->payload_size > P4_HOSTED_REASSEMBLY_FRAGMENT_MAX ||
        state->used > sizeof(state->bytes) ||
        frame->payload_size > sizeof(state->bytes) - state->used ||
        state->fragments >= P4_HOSTED_REASSEMBLY_MAX_FRAGMENTS ||
        (state->fragments == P4_HOSTED_REASSEMBLY_MAX_FRAGMENTS - 1u &&
         (frame->flags & P4_HOSTED_MORE_FRAGMENT)) ||
        (state->fragments && state->sequence != frame->sequence))
        return malformed(state);
    state->sequence = (uint16_t)(frame->sequence + 1u);
    out = state->bytes + state->used;
    in = frame->payload;
    for (i = 0; i < frame->payload_size; ++i) out[i] = in[i];
    state->used += frame->payload_size;
    ++state->fragments;
    if (frame->flags & P4_HOSTED_MORE_FRAGMENT)
        return P4_HOSTED_REASSEMBLY_PARTIAL;
    if (!hosted_serial_decode(state->bytes, state->used, &decoded))
        return malformed(state);
    state->complete = 1;
    return P4_HOSTED_REASSEMBLY_COMPLETE;
}
int p4_hosted_reassembly_view(const struct p4_hosted_reassembly *state,
    uint32_t generation, struct hosted_serial_message *message)
{
    if (!separate(state, sizeof(*state), message, sizeof(*message)) ||
        !generation || state->initialized != 1 ||
        state->generation != generation || state->poisoned ||
        state->complete != 1 || state->used > sizeof(state->bytes)) return 0;
    return hosted_serial_decode(state->bytes, state->used, message);
}
int p4_hosted_reassembly_release(struct p4_hosted_reassembly *state,
                                 uint32_t generation)
{
    if (!valid_range(state, sizeof(*state)) || !generation ||
        state->initialized != 1 || state->generation != generation ||
        state->poisoned || state->complete != 1) return 0;
    clear_bytes(state);
    return 1;
}
int p4_hosted_reassembly_abort(struct p4_hosted_reassembly *state,
                               uint32_t generation)
{
    if (!valid_range(state, sizeof(*state)) || !generation ||
        state->initialized != 1 || state->generation != generation) return 0;
    (void)malformed(state);
    return 1;
}
