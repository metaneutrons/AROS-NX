/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#ifndef P4_HOSTED_CONTROL_H
#define P4_HOSTED_CONTROL_H
#include "hosted_mac.h"
#include "hosted_open.h"
#include "hosted_tx.h"
#include "hosted_epoch_reassembly.h"
#include "hosted_request_ids.h"

#define P4_HOSTED_CONTROL_MAX_POLLS 100u
/* ScanStart may need to return only after a blocking scan has completed. A
 * 500-poll cap covers a nominal 20 ms caller tick (10 s); budget_ok remains
 * the authoritative aggregate deadline and is checked throughout. */
#define P4_HOSTED_CONTROL_MAX_SCAN_POLLS 500u
#define P4_HOSTED_CONTROL_MAX_RPC_SIZE 256u
#define P4_HOSTED_CONTROL_MAX_RESPONSE_SIZE HOSTED_SERIAL_PROTOBUF_MAX
enum p4_hosted_control_error {
    P4_HOSTED_CONTROL_OK, P4_HOSTED_CONTROL_ENCODE,
    P4_HOSTED_CONTROL_TX, P4_HOSTED_CONTROL_SESSION,
    P4_HOSTED_CONTROL_BUDGET, P4_HOSTED_CONTROL_REGISTER,
    P4_HOSTED_CONTROL_PEER_FAULT, P4_HOSTED_CONTROL_COUNTER,
    P4_HOSTED_CONTROL_CAPACITY, P4_HOSTED_CONTROL_RX,
    P4_HOSTED_CONTROL_PROTOCOL, P4_HOSTED_CONTROL_REMOTE,
    P4_HOSTED_CONTROL_TIMEOUT,
    /* Caller-side classification when exchange refuses without output. */
    P4_HOSTED_CONTROL_PREFLIGHT
};
struct p4_hosted_control_result {
    enum p4_hosted_control_error error;
    struct p4_hosted_tx_result tx;
    struct p4_hosted_mac mac;
    uint8_t response_body[P4_HOSTED_CONTROL_MAX_RESPONSE_SIZE];
    /* Caller-owned scratch; keep the full result off small kernel stacks. */
    struct p4_hosted_reassembly assembly;
    size_t response_size;
    uint32_t response_id, response_uid;
    uint32_t polls, batches, events, unmatched;
    unsigned matched, success;
};
/* One explicitly outstanding GetMAC(STA) request in a known fresh transport
 * session. The caller owns the controller and its aggregate deadline. This
 * ownership must serialize all ledger access; budget_ok is a deadline-only
 * check and must not replace the ledger or release controller ownership. This
 * gate neither opens the datapath nor resets counters, renews budgets or
 * retries a transmission. SERIAL/0 messages may span validated frames/batches;
 * the result owns partial bytes under the same generation and deadline.
 * Events are envelope-checked
 * and counted opaquely, not accepted as WLAN state. UID and response ID must
 * both match; unmatched valid responses are counted within the poll bound.
 * A transmitted UID is consumed even on timeout: never reuse it within this
 * epoch. Persistent UID allocation and request cancellation are caller work,
 * not implemented by this synchronous gate.
 * Full input-capacity/state/ops/result ranges must be pairwise disjoint;
 * preflight refusal changes none. Buffer capacity must be512..4096 bytes.
 * Consumed invalid protocol, ambiguous RX or session loss poison the ledger.
 * Terminal failure while partial wipes/poisons assembly and matching ledger.
 * A matching response followed by an unfinished partial in its committed batch
 * fails rather than leaving bytes for a subsequent RPC. The result owns all
 * bytes, borrows no receive pointer, and must be allocated off small stacks.
 */
int p4_hosted_control_get_mac(struct p4_hosted_fifo *state, uint32_t generation,
    const struct p4_hosted_open_ops *rx_ops,
    const struct p4_hosted_tx_ops *tx_ops, unsigned checksum_enabled,
    uint32_t uid, uint8_t *buffer, size_t capacity,
    struct p4_hosted_control_result *result);
/* Exchange one encoded RPC request on the already-open transport. Currently
 * allowlisted: GetMAC, SetMode, WiFiInit/Deinit/Start/Stop and ScanStart/Stop/
 * GetApNum/GetApRecords. Request and output ranges must also be disjoint. A
 * success means a correlated complete response envelope, NOT a successful
 * remote status: the caller must validate the owned response body with its
 * operation-specific codec. ScanStart alone has a 500-poll cap; all other
 * exchanges retain 100 polls. The caller's aggregate deadline remains
 * authoritative. Same no-retry/UID-consumption rules as above. Requests remain
 * at most256 bytes; responses are bounded by4096 protobuf bytes including
 * envelope overhead. No persistent UID allocator. */
int p4_hosted_control_exchange(struct p4_hosted_fifo *state, uint32_t generation,
    const struct p4_hosted_open_ops *rx_ops,
    const struct p4_hosted_tx_ops *tx_ops, unsigned checksum_enabled,
    const uint8_t *request, size_t request_size,
    uint8_t *buffer, size_t capacity, struct p4_hosted_control_result *result);
/* Persistent caller variant. Reserve an identity before encoding and keep the
 * same ledger for the lifetime of a possibly retained C6. The encoded request
 * must match the live reservation and generation exactly. All seven object
 * ranges must be disjoint. Wrapper preflight refusal changes nothing. Once
 * accepted, every terminal exchange outcome finishes the reservation without
 * rewinding its consumed UID, including lower-level preflight refusal. FIFO
 * poison or generation loss retires only the matching identity epoch. A
 * callback must not release controller ownership, re-enter this API or mutate
 * the request-ID ledger; a stale completion cannot
 * finish or invalidate a replacement ledger. This is still synchronous and
 * does not add idle RX/event delivery, retries or an association allowlist. */
int p4_hosted_control_exchange_reserved(struct p4_hosted_fifo *state,
    struct p4_hosted_request_ids *ids, uint32_t generation,
    const struct p4_hosted_open_ops *rx_ops,
    const struct p4_hosted_tx_ops *tx_ops, unsigned checksum_enabled,
    const uint8_t *request, size_t request_size, uint8_t *buffer,
    size_t capacity, struct p4_hosted_control_result *result);
#endif
