/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#ifndef P4_HOSTED_CONTROL_H
#define P4_HOSTED_CONTROL_H
#include "hosted_mac.h"
#include "hosted_open.h"
#include "hosted_tx.h"

#define P4_HOSTED_CONTROL_MAX_POLLS 100u
enum p4_hosted_control_error {
    P4_HOSTED_CONTROL_OK, P4_HOSTED_CONTROL_ENCODE,
    P4_HOSTED_CONTROL_TX, P4_HOSTED_CONTROL_SESSION,
    P4_HOSTED_CONTROL_BUDGET, P4_HOSTED_CONTROL_REGISTER,
    P4_HOSTED_CONTROL_PEER_FAULT, P4_HOSTED_CONTROL_COUNTER,
    P4_HOSTED_CONTROL_CAPACITY, P4_HOSTED_CONTROL_RX,
    P4_HOSTED_CONTROL_PROTOCOL, P4_HOSTED_CONTROL_REMOTE,
    P4_HOSTED_CONTROL_TIMEOUT
};
struct p4_hosted_control_result {
    enum p4_hosted_control_error error;
    struct p4_hosted_tx_result tx;
    struct p4_hosted_mac mac;
    uint32_t polls, batches, events, unmatched;
    unsigned matched, success;
};
/* One explicitly outstanding GetMAC(STA) request in a known fresh transport
 * session. The caller owns the controller and its aggregate deadline. This
 * gate neither opens the datapath nor resets counters, renews budgets or
 * retries a transmission. Complete SERIAL/0 frames are required; fragmented
 * messages are refused, not partially interpreted. Events are envelope-checked
 * and counted opaquely, not accepted as WLAN state. UID and response ID must
 * both match; unmatched valid responses are counted within the poll bound.
 * A transmitted UID is consumed even on timeout: never reuse it within this
 * epoch. Persistent UID allocation and request cancellation are caller work,
 * not implemented by this one-shot gate. The runtime closes after one call.
 * Full input-capacity/state/ops/result ranges must be pairwise disjoint;
 * preflight refusal changes none. Buffer capacity must be512..4096 bytes.
 * Consumed invalid protocol, ambiguous RX or session loss poison the ledger.
 * The result owns MAC bytes; it borrows no pointer into the receive buffer.
 */
int p4_hosted_control_get_mac(struct p4_hosted_fifo *state, uint32_t generation,
    const struct p4_hosted_open_ops *rx_ops,
    const struct p4_hosted_tx_ops *tx_ops, unsigned checksum_enabled,
    uint32_t uid, uint8_t *buffer, size_t capacity,
    struct p4_hosted_control_result *result);
#endif
