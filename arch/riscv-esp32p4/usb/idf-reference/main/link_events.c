/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#include "link_events.h"

void hs_link_events_init(hs_link_events_t *state)
{
    if (!state)
        return;

    for (uint32_t i = 0; i < HS_LINK_EVENT_COUNT; ++i)
        atomic_init(&state->counts[i], 0);
    atomic_init(&state->ignored, 0);
}

void hs_link_events_record(hs_link_events_t *state, uint8_t rhport,
                          uint32_t event_id)
{
    if (!state)
        return;

    if (rhport != 1 || event_id == HS_LINK_EVENT_INVALID ||
        event_id >= HS_LINK_EVENT_COUNT) {
        atomic_fetch_add_explicit(&state->ignored, 1, memory_order_relaxed);
        return;
    }

    atomic_fetch_add_explicit(&state->counts[event_id], 1,
                              memory_order_relaxed);
}

void hs_link_events_snapshot(const hs_link_events_t *state,
                             hs_link_counts_t *out)
{
    if (!state || !out)
        return;

    for (uint32_t i = 0; i < HS_LINK_EVENT_COUNT; ++i)
        out->event[i] = atomic_load_explicit(&state->counts[i],
                                             memory_order_relaxed);
    out->ignored = atomic_load_explicit(&state->ignored, memory_order_relaxed);
}
