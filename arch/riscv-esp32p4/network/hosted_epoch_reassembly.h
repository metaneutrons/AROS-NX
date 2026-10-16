/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#ifndef P4_HOSTED_EPOCH_REASSEMBLY_H
#define P4_HOSTED_EPOCH_REASSEMBLY_H
#include "hosted_wire.h"
#include "hosted_serial.h"

/* Defensive native policy, not a negotiated peer limit. */
#define P4_HOSTED_REASSEMBLY_MAX_FRAGMENTS 16u
/* Pinned slave serial_write_data splits at ETH_DATA_LEN (1500). */
#define P4_HOSTED_REASSEMBLY_FRAGMENT_MAX 1500u
enum p4_hosted_reassembly_result {
    P4_HOSTED_REASSEMBLY_REFUSED = 0,
    P4_HOSTED_REASSEMBLY_PARTIAL,
    P4_HOSTED_REASSEMBLY_COMPLETE,
    P4_HOSTED_REASSEMBLY_PROTOCOL
};
struct p4_hosted_reassembly {
    uint8_t bytes[HOSTED_SERIAL_MESSAGE_MAX];
    size_t used;
    uint32_t generation;
    uint16_t sequence;
    unsigned initialized, fragments, complete, poisoned;
};
/* Caller zero-initializes once and exclusively owns state. Start only at a
 * known fresh transport epoch: never clear a partial message in the same
 * epoch. No I/O, allocation, counter reset or deadline creation. */
int p4_hosted_reassembly_start(struct p4_hosted_reassembly *state,
                               uint32_t generation);
/* Feed one outer frame already validated by p4_hosted_decode. SERIAL/0 only,
 * packet_type/throttle0 and flags0/MORE. The pinned slave increments the
 * uint16 sequence for every fragment (including wrap); sequence is NOT
 * enforced across complete messages.
 * State/frame metadata/payload ranges must be disjoint and overflow-safe.
 * Stale generation, invalid ranges and outstanding COMPLETE refuse without
 * mutation. Invalid consumed fields, sequence change, oversized assembly or
 * malformed final serial TLV poison this state; caller must invalidate its
 * matching transport too. The aggregate caller deadline bounds partial waits.
 * Other interfaces must be routed separately, not fed to this assembler. */
enum p4_hosted_reassembly_result p4_hosted_reassembly_feed(
    struct p4_hosted_reassembly *state, uint32_t generation,
    const struct p4_hosted_frame *frame);
/* The successful view borrows the owned state bytes until release, abort or
 * a new epoch. No pointer into a transient DMA buffer escapes. Output must
 * be disjoint from the entire state and is unchanged on refusal. */
int p4_hosted_reassembly_view(const struct p4_hosted_reassembly *state,
    uint32_t generation, struct hosted_serial_message *message);
int p4_hosted_reassembly_release(struct p4_hosted_reassembly *state,
                                 uint32_t generation);
/* Use on cancellation, timeout or transport failure while partial. Abort
 * poisons only the matching epoch; it cannot reopen/retry a message. */
int p4_hosted_reassembly_abort(struct p4_hosted_reassembly *state,
                               uint32_t generation);
#endif
