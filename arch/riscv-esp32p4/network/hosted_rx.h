/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#ifndef P4_HOSTED_RX_H
#define P4_HOSTED_RX_H
#include "hosted_open.h"
#include "hosted_epoch_reassembly.h"
#include "hosted_inbox.h"
#include "hosted_frames.h"

enum p4_hosted_rx_error {
    P4_HOSTED_RX_OK = 0,
    P4_HOSTED_RX_SESSION,
    P4_HOSTED_RX_BUDGET,
    P4_HOSTED_RX_REGISTER,
    P4_HOSTED_RX_PEER_FAULT,
    P4_HOSTED_RX_COUNTER,
    P4_HOSTED_RX_CAPACITY,
    P4_HOSTED_RX_TRANSFER,
    P4_HOSTED_RX_PROTOCOL,
    P4_HOSTED_RX_QUEUE_FULL,
    P4_HOSTED_RX_FRAME_QUEUE_FULL
};
struct p4_hosted_rx_ops {
    void *context;
    int (*session_ok)(void *context, uint32_t generation);
    /* Includes the aggregate partial-message deadline, not just this poll's
     * bus budget. An expired partial message retires the matching session. */
    int (*budget_ok)(void *context);
    int (*read_word)(void *context, uint32_t address, uint32_t *word);
    int (*write_word)(void *context, uint32_t address, uint32_t word);
    int (*read_fifo)(void *context, const struct p4_hosted_fifo_plan *plan,
                     uint8_t *buffer);
};
struct p4_hosted_rx {
    uint32_t generation;
    unsigned initialized, poisoned;
    struct p4_hosted_reassembly assembly;
    struct p4_hosted_inbox inbox;
    struct p4_hosted_frames frames;
};
/* RX state is about27KB, not suitable for an8KB worker stack. Allocate the
 * persistent state,4KB inbox-pop item and1518-byte Ethernet-pop buffer outside
 * shared-host ownership, off small task stacks. Poll/pop are serialized by
 * the same lifecycle owner; releasing the controller mutex does not make
 * either queue concurrent. */
struct p4_hosted_rx_result {
    enum p4_hosted_rx_error error;
    unsigned batches, messages, frames, partial;
    uint32_t logical_bytes;
};
/* Zero-initialize once; start only at a known fresh transport epoch. A new
 * host acquisition is not a new peer epoch. Same-generation restart refuses.
 * State is retained between polls and exclusively serialized with FIFO/TX. */
int p4_hosted_rx_start(struct p4_hosted_rx *state, uint32_t generation);
/* Transfer the already-consumed raw suffix from a successful preserving OPEN
 * before buffer reuse/config encoding. No register/FIFO I/O or counter advance.
 * Initializer and serial suffix are revalidated; copied RPC events/responses
 * or partial serial assembly become owned by this RX state. The current OPEN
 * preserve gate accepts SERIAL/0 suffixes only. Same-generation repeated
 * handoff refuses;
 * caller zero-initializes RX only once, not to replay a consumed batch.
 * OPEN cleanup must not have begun. Caller independently established peer
 * freshness; host generation and this handoff do not prove a C6 reset.
 * All four ranges must be disjoint. Local preflight/start refusal leaves
 * state/OPEN/result/buffer unchanged. Consumed malformed bytes poison matching
 * FIFO/RX after acceptance. No dispatch/association/correlation acceptance. */
int p4_hosted_rx_take_open(struct p4_hosted_rx *state,
    struct p4_hosted_open_result *opened, uint32_t generation,
    const uint8_t *buffer, size_t capacity, struct p4_hosted_rx_result *result);
/* Exactly one bounded interrupt/FIFO batch; an idle poll returns immediately.
 * No TX, waiting, yield, dispatch, allocation or hidden reset. Caller owns the
 * bus for this call only, and releases it before popping/dispatching inbox
 * items. Callbacks must not release ownership/re-enter/mutate these objects.
 * All five ranges must be disjoint. Preflight refusal changes nothing.
 * SERIAL events/responses are copied into the RPC inbox; partial serial
 * assembly survives idle calls and interleaved STA frames. Complete STA/0
 * Ethernet frames from later RX packets are copied into `state->frames` and
 * counted separately in result.frames; result.messages remains the
 * RPC-envelope count. The pinned
 * slave emits STA handles with flags, sequence, packet type/reserved byte and
 * throttle zero. Since this receiver has no flow-control action for nonzero
 * throttle, nonzero values fail closed. The fixed four-frame queue accepts
 * 14..1518 bytes as local bounds, not a negotiated peer guarantee. Pop frame
 * data from state->frames only after releasing shared-host ownership, under
 * the same serialized lifecycle owner. Responses retain UID+ID for the
 * higher-level matcher; reception is not RPC/association success. Unknown,
 * AP and unsupported profiles fail closed. Ambiguous ACK/transfer, malformed
 * data, either queue overflow or expired partial message poison only matching-
 * generation objects. Replacement state is never altered by a stale
 * completion. No retry of consumed/ambiguous bytes. */
int p4_hosted_rx_poll(struct p4_hosted_rx *state,
    struct p4_hosted_fifo *fifo, uint32_t generation,
    const struct p4_hosted_rx_ops *ops, unsigned checksum_enabled,
    uint8_t *buffer, size_t capacity, struct p4_hosted_rx_result *result);
/* Cancellation of a partial stream is terminal for this transport epoch.
 * Caller also retires its matching request IDs; this layer does not own them. */
int p4_hosted_rx_invalidate(struct p4_hosted_rx *state,
    struct p4_hosted_fifo *fifo, uint32_t generation);
#endif
