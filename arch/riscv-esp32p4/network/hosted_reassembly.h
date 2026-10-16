/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#ifndef HOSTED_REASSEMBLY_H
#define HOSTED_REASSEMBLY_H

#include <stddef.h>
#include <stdint.h>

#include "hosted_serial.h"
#include "hosted_wire.h"

/* Local receive limits. The serial envelope cap is 12 bytes plus the
 * hosted_serial codec's 4096-byte protobuf policy. */
#define HOSTED_REASSEMBLY_BUFFER_SIZE HOSTED_SERIAL_MESSAGE_MAX
#define HOSTED_REASSEMBLY_FRAGMENT_SIZE 1500u
#define HOSTED_REASSEMBLY_FRAGMENT_LIMIT 16u

enum hosted_reassembly_result {
    HOSTED_REASSEMBLY_REJECTED = 0,
    HOSTED_REASSEMBLY_PENDING = 1,
    HOSTED_REASSEMBLY_COMPLETE = 2,
    HOSTED_REASSEMBLY_IGNORED = 3
};

/* Caller-owned, allocation-free state for one SERIAL interface-number-0 RX
 * stream. Sequence numbers are checked only while a fragmented message is
 * active; no sequence baseline is inferred while idle. */
struct hosted_reassembly_state {
    uint8_t bytes[HOSTED_REASSEMBLY_BUFFER_SIZE];
    size_t length;
    uint32_t timeout_ms;
    uint32_t first_tick;
    uint16_t expected_sequence;
    uint8_t fragment_count;
    uint8_t active;
};

/* Returns zero without changing state unless timeout_ms is in [1, 2^31).
 * The timeout is measured from the first fragment, not extended per fragment.
 * The caller supplies monotonic millisecond ticks and must reset stale state
 * after a disconnect or an observation gap of 2^31 milliseconds or more. */
int hosted_reassembly_init(struct hosted_reassembly_state *state,
                           uint32_t timeout_ms);

/* Drops a partial message and invalidates any borrowed completed output. The
 * configured timeout is preserved; bytes are not scrubbed. */
void hosted_reassembly_reset(struct hosted_reassembly_state *state);

/* Feed a decoded frame. Non-SERIAL frames and SERIAL frames on another
 * interface number return IGNORED without disturbing a pending message.
 * SERIAL/0 payloads must be 1..1500 bytes, with only MORE_FRAGMENT permitted.
 * throttle and packet_type are per-frame/unconstrained here. Every fragment
 * of an active message must have the next uint16 sequence, including wrap.
 * A timeout, malformed fragment, sequence error, over-limit message or bad
 * final TLV returns REJECTED and discards the partial message. The frame that
 * discovers a timeout is refused rather than reconsidered as a new message.
 *
 * On PENDING, REJECTED and IGNORED, output is unchanged. On COMPLETE, output
 * borrows protobuf bytes from state->bytes and remains valid until the next
 * feed or reset. The input frame object, its payload range, output object and
 * state object must be pairwise disjoint. Aliasing is refused before any
 * state or output mutation. NULL arguments are also refused without mutation.
 * The caller serializes access to each state. */
enum hosted_reassembly_result hosted_reassembly_feed(
    struct hosted_reassembly_state *state,
    const struct p4_hosted_frame *frame,
    uint32_t now_ms,
    struct hosted_serial_message *output);

#endif
