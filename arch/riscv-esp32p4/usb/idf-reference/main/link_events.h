/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
 * Fixed-size, allocation-free counters for read-only USB link events.
 */
#ifndef HS_LINK_EVENTS_H
#define HS_LINK_EVENTS_H

#include <stdatomic.h>
#include <stdint.h>

#define HS_LINK_EVENT_COUNT 9u

enum hs_link_event_id {
    HS_LINK_EVENT_INVALID = 0,
    HS_LINK_EVENT_BUS_RESET = 1,
    HS_LINK_EVENT_UNPLUG = 2,
    HS_LINK_EVENT_SOF = 3,
    HS_LINK_EVENT_SUSPEND = 4,
    HS_LINK_EVENT_RESUME = 5,
    HS_LINK_EVENT_SETUP = 6,
    HS_LINK_EVENT_XFER = 7,
    HS_LINK_EVENT_FUNC_CALL = 8
};

/* The native ESP32-P4 RV32 target and supported host test targets use
 * lock-free 32-bit unsigned atomics. Keep this helper free of libatomic calls.
 */
_Static_assert(ATOMIC_INT_LOCK_FREE == 2,
               "USB link event counters require lock-free int atomics");
_Static_assert(sizeof(unsigned) == 4,
               "USB link event counters require 32-bit unsigned");
_Static_assert(sizeof(uint32_t) == sizeof(unsigned),
               "USB link event counters require 32-bit uint32_t");

typedef struct hs_link_events {
    _Atomic uint32_t counts[HS_LINK_EVENT_COUNT];
    _Atomic uint32_t ignored;
} hs_link_events_t;

typedef struct hs_link_counts {
    uint32_t event[HS_LINK_EVENT_COUNT];
    uint32_t ignored;
} hs_link_counts_t;

/* Initialize before publishing the state to any event-producing context. */
void hs_link_events_init(hs_link_events_t *state);

/* Count event IDs 1..8 from rhport 1. Invalid IDs and all other ports
 * increment ignored. Null state is ignored.
 */
void hs_link_events_record(hs_link_events_t *state, uint8_t rhport,
                           uint32_t event_id);

/* Loads each counter independently with relaxed ordering. The result is not a
 * coherent time-point snapshot; unsigned counters wrap modulo UINT32_MAX+1.
 */
void hs_link_events_snapshot(const hs_link_events_t *state,
                             hs_link_counts_t *out);

#endif /* HS_LINK_EVENTS_H */
