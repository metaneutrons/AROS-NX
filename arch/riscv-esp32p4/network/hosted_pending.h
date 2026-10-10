#ifndef P4_HOSTED_PENDING_H
#define P4_HOSTED_PENDING_H

/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */

#include <stddef.h>
#include <stdint.h>

#include "hosted_inbox.h"
#include "hosted_request_ids.h"
#include "hosted_tx.h"
#include "hosted_init.h"

#define P4_HOSTED_PENDING_MAX_REQUEST_SIZE 256u
#define P4_HOSTED_PENDING_BUFFER_MIN 512u
#define P4_HOSTED_PENDING_BUFFER_MAX 4096u

enum p4_hosted_pending_error {
    P4_HOSTED_PENDING_ERROR_NONE = 0,
    P4_HOSTED_PENDING_ERROR_ENCODE,
    P4_HOSTED_PENDING_ERROR_TX,
    P4_HOSTED_PENDING_ERROR_SESSION
};

struct p4_hosted_pending_result {
    enum p4_hosted_pending_error error;
    struct p4_hosted_tx_result tx;
    uint32_t request_id;
    uint32_t uid;
    size_t request_size;
    size_t serial_size;
    size_t wire_size;
    unsigned submitted;
};

enum p4_hosted_pending_correlation {
    P4_HOSTED_PENDING_CORRELATION_REFUSED = 0,
    P4_HOSTED_PENDING_CORRELATION_EVENT,
    P4_HOSTED_PENDING_CORRELATION_UNMATCHED,
    P4_HOSTED_PENDING_CORRELATION_MATCHED
};

/* Encode and submit exactly one allowlisted, already-reserved RPC request.
 * This performs no RX, wait beyond the existing bounded TX credit polls,
 * dispatch, allocation, UID reservation, reset, or deadline creation. The
 * caller must exclusively serialize access, hold controller ownership and
 * supply the existing aggregate deadline through tx_ops. The caller must
 * independently establish a fresh peer epoch before initializing FIFO and
 * request-ID state; a host generation alone is not evidence of peer reset.
 *
 * Requests are limited to IDs 257, 260, 278..284, and 286..289. The request
 * bytes must decode to the exact active UID/request ID and generation. All
 * six complete object ranges (FIFO, IDs, TX ops, request, full TX-buffer
 * capacity, and result) must be valid, overflow-safe and pairwise disjoint.
 * Buffer capacity is 512..4096 bytes and checksum_enabled is 0 or 1.
 * Preflight refusal leaves every object and callback state unchanged. Once
 * accepted, a terminal failure consumes the already-reserved UID and closes
 * only that exact reservation; a matching poisoned/lost FIFO epoch retires
 * that identity ledger. Successful submission leaves the reservation active
 * for later correlation. `submitted` means the existing TX gate reported one
 * complete successful frame; it alone does not mean the current reservation
 * remains active or that the remote operation succeeded. The helper wipes its
 * private serial staging buffer after every accepted call; the caller must
 * wipe its own request and TX buffer when no longer needed. The result is
 * caller-owned scratch and should be kept off small stacks.
 *
 * This transport allowlist does not imply credential-bearing deployment,
 * association success, or hardware acceptance. IDs 282..284 remain gated by
 * the independently documented credential/privacy and firmware-compatibility
 * prerequisites; this API neither clears those gates nor claims to transmit
 * credentials or perform association.
 */
int p4_hosted_pending_submit(struct p4_hosted_fifo *fifo,
                             struct p4_hosted_request_ids *ids,
                             uint32_t generation,
                             const struct p4_hosted_tx_ops *tx_ops,
                             unsigned checksum_enabled,
                             const uint8_t *request, size_t request_size,
                             uint8_t *txbuffer, size_t capacity,
                             struct p4_hosted_pending_result *result);

/* Correlate one already-owned inbox item with the caller's persistent ledger.
 * Call only after successful submit and while the caller's deadline/session
 * is still authoritative; the caller tracks submitted/deadline state, timeout
 * and cancellation because this ledger does not. Caller-exclusive
 * serialization is required. The ledger and complete item ranges must be
 * disjoint and valid. Events 769..776 (including UID zero) return EVENT
 * without ledger mutation. Valid responses 513..605 require a nonzero UID;
 * exact active UID plus request_id+256 finishes the reservation and returns
 * MATCHED. Other valid responses return UNMATCHED unchanged. MATCHED means
 * envelope identity only, not remote operation success, association success,
 * status validation, or dispatch. */
enum p4_hosted_pending_correlation
p4_hosted_pending_correlate(struct p4_hosted_request_ids *ids,
                            uint32_t generation,
                            const struct p4_hosted_inbox_item *item);

/* Private read-only extension path, not ordinary WLAN admission. Requires
 * an exact reserved350 identity, supported C6 INIT and canonical version1/
 * supplied nonzero nonce body. INIT is a seventh disjoint input range. Same
 * bounds, preflight atomicity, terminal UID consumption and no-retry contract
 * as normal submit. Caller must independently bind INIT to the fresh live
 * peer, enforce the exclusive maintenance lease/deadline, arm matching RX and
 * serialize this call under controller ownership. This helper does NOT prove
 * those facts, issue OTA, authorize credentials or grant write permission. */
int p4_hosted_pending_submit_board_discovery(struct p4_hosted_fifo *fifo,
    struct p4_hosted_request_ids *ids, uint32_t generation,
    const struct p4_hosted_tx_ops *tx_ops, unsigned checksum_enabled,
    const uint8_t *request, size_t request_size, uint8_t *txbuffer,
    size_t capacity, struct p4_hosted_pending_result *result,
    const struct p4_hosted_init *init, uint32_t nonce);

/* Call only after successful submit and while the original deadline/session
 * remain authoritative; an active reservation alone does not prove TX. Caller
 * exclusively serializes correlation and independently tracks submission.
 * Exact active350 ledger required. Only606 can match/finish it; ordinary
 * events/responses may be classified for draining but cannot finish350.
 * Matching consumes UID before semantic inventory/status validation. A match
 * is not success or freshness; caller validates nonce/body and lease/session
 * before publishing. Replays/stale generations cannot finish another UID. */
enum p4_hosted_pending_correlation
p4_hosted_pending_correlate_board_discovery(struct p4_hosted_request_ids *ids,
    uint32_t generation, const struct p4_hosted_inbox_item *item);

#endif
