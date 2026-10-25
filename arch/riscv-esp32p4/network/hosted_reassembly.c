/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#include "hosted_reassembly.h"

#include <string.h>

/* Avoid relational pointer comparisons and pointer addition across unrelated
 * objects. Unrepresentable or address-wrapping ranges conservatively overlap. */
static int ranges_overlap(const void *a, size_t a_size,
                          const void *b, size_t b_size)
{
    uintptr_t a_start, b_start, a_length, b_length;

    if (a_size == 0 || b_size == 0)
        return 0;
#if SIZE_MAX > UINTPTR_MAX
    if ((uintmax_t)a_size > (uintmax_t)UINTPTR_MAX ||
        (uintmax_t)b_size > (uintmax_t)UINTPTR_MAX)
        return 1;
#endif

    a_start = (uintptr_t)a;
    b_start = (uintptr_t)b;
    a_length = (uintptr_t)a_size;
    b_length = (uintptr_t)b_size;
    if (a_length > UINTPTR_MAX - a_start ||
        b_length > UINTPTR_MAX - b_start)
        return 1;
    if (a_start <= b_start)
        return b_start - a_start < a_length;
    return a_start - b_start < b_length;
}

static void clear_partial(struct hosted_reassembly_state *state)
{
    state->length = 0;
    state->first_tick = 0;
    state->expected_sequence = 0;
    state->fragment_count = 0;
    state->active = 0;
}

static int valid_timeout(uint32_t timeout_ms)
{
    return timeout_ms != 0 && timeout_ms < UINT32_C(0x80000000);
}

int hosted_reassembly_init(struct hosted_reassembly_state *state,
                           uint32_t timeout_ms)
{
    if (!state || !valid_timeout(timeout_ms))
        return 0;

    state->timeout_ms = timeout_ms;
    clear_partial(state);
    return 1;
}

void hosted_reassembly_reset(struct hosted_reassembly_state *state)
{
    if (state)
        clear_partial(state);
}

static int aliases_for_feed(const struct hosted_reassembly_state *state,
                            const struct p4_hosted_frame *frame,
                            const struct hosted_serial_message *output)
{
    if (ranges_overlap(state, sizeof *state, frame, sizeof *frame) ||
        ranges_overlap(state, sizeof *state, output, sizeof *output) ||
        ranges_overlap(frame, sizeof *frame, output, sizeof *output))
        return 1;

    if (frame->payload && frame->payload_size != 0 &&
        (ranges_overlap(frame->payload, frame->payload_size,
                        state, sizeof *state) ||
         ranges_overlap(frame->payload, frame->payload_size,
                        frame, sizeof *frame) ||
         ranges_overlap(frame->payload, frame->payload_size,
                        output, sizeof *output)))
        return 1;
    return 0;
}

static enum hosted_reassembly_result finish_message(
    struct hosted_reassembly_state *state,
    struct hosted_serial_message *output)
{
    struct hosted_serial_message candidate;

    if (!hosted_serial_decode(state->bytes, state->length, &candidate)) {
        clear_partial(state);
        return HOSTED_REASSEMBLY_REJECTED;
    }

    clear_partial(state);
    *output = candidate;
    return HOSTED_REASSEMBLY_COMPLETE;
}

enum hosted_reassembly_result hosted_reassembly_feed(
    struct hosted_reassembly_state *state,
    const struct p4_hosted_frame *frame,
    uint32_t now_ms,
    struct hosted_serial_message *output)
{
    size_t new_length;
    uint8_t new_fragment_count;
    int more_fragments;

    if (!state)
        return HOSTED_REASSEMBLY_REJECTED;
    if (!frame || !output)
        return HOSTED_REASSEMBLY_REJECTED;

    /* Frame/output/state overlap cannot be repaired without mutating an
     * aliased caller object, so it is refused with all bytes left untouched. */
    if (aliases_for_feed(state, frame, output))
        return HOSTED_REASSEMBLY_REJECTED;

    /* Other traffic can share the transport batch. It is not a serial
     * fragment and must not cancel this stream's partial message. */
    if (frame->interface_type != P4_HOSTED_SERIAL ||
        frame->interface_number != 0)
        return HOSTED_REASSEMBLY_IGNORED;

    if (!valid_timeout(state->timeout_ms)) {
        clear_partial(state);
        return HOSTED_REASSEMBLY_REJECTED;
    }

    if (!frame->payload || frame->payload_size == 0 ||
        frame->payload_size > HOSTED_REASSEMBLY_FRAGMENT_SIZE ||
        (frame->flags & (uint8_t)~P4_HOSTED_MORE_FRAGMENT) != 0) {
        clear_partial(state);
        return HOSTED_REASSEMBLY_REJECTED;
    }

    if (state->active > 1u ||
        (state->active &&
         (state->length == 0 ||
          state->length > HOSTED_REASSEMBLY_BUFFER_SIZE ||
          state->fragment_count == 0 ||
          state->fragment_count > HOSTED_REASSEMBLY_FRAGMENT_LIMIT))) {
        clear_partial(state);
        return HOSTED_REASSEMBLY_REJECTED;
    }

    if (state->active &&
        (uint32_t)(now_ms - state->first_tick) >= state->timeout_ms) {
        clear_partial(state);
        return HOSTED_REASSEMBLY_REJECTED;
    }

    if (state->active && frame->sequence != state->expected_sequence) {
        clear_partial(state);
        return HOSTED_REASSEMBLY_REJECTED;
    }

    more_fragments = (frame->flags & P4_HOSTED_MORE_FRAGMENT) != 0;
    new_fragment_count = state->active ?
        (uint8_t)(state->fragment_count + 1u) : 1u;
    if (new_fragment_count > HOSTED_REASSEMBLY_FRAGMENT_LIMIT ||
        (more_fragments &&
         new_fragment_count == HOSTED_REASSEMBLY_FRAGMENT_LIMIT)) {
        clear_partial(state);
        return HOSTED_REASSEMBLY_REJECTED;
    }

    if (state->active &&
        frame->payload_size > HOSTED_REASSEMBLY_BUFFER_SIZE - state->length) {
        clear_partial(state);
        return HOSTED_REASSEMBLY_REJECTED;
    }

    new_length = state->active ? state->length + frame->payload_size :
        frame->payload_size;
    if (!state->active)
        state->first_tick = now_ms;
    memcpy(state->bytes + (state->active ? state->length : 0u),
           frame->payload, frame->payload_size);
    state->length = new_length;
    state->fragment_count = new_fragment_count;
    state->expected_sequence = (uint16_t)(frame->sequence + 1u);

    if (more_fragments) {
        state->active = 1u;
        return HOSTED_REASSEMBLY_PENDING;
    }

    state->active = 0u;
    return finish_message(state, output);
}
