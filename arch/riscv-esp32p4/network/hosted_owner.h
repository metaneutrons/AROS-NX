#ifndef P4_HOSTED_OWNER_H
#define P4_HOSTED_OWNER_H

/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */

#include "hosted_pending.h"
#include "hosted_rx.h"

#define P4_HOSTED_OWNER_MAX_EXCHANGE_US UINT32_C(10000000)
#define P4_HOSTED_OWNER_MAX_ACQUIRE_US UINT32_C(20000)
#define P4_HOSTED_OWNER_MAX_SLICE_US UINT32_C(250000)
#define P4_HOSTED_OWNER_PARTIAL_TIMEOUT_US UINT32_C(2000000)
#define P4_HOSTED_OWNER_MAX_POLLS 500u

enum p4_hosted_owner_error {
    P4_HOSTED_OWNER_OK = 0,
    P4_HOSTED_OWNER_PREFLIGHT,
    P4_HOSTED_OWNER_STOPPED,
    P4_HOSTED_OWNER_TIMEOUT,
    P4_HOSTED_OWNER_ACQUIRE,
    P4_HOSTED_OWNER_BUDGET,
    P4_HOSTED_OWNER_SESSION,
    P4_HOSTED_OWNER_TX,
    P4_HOSTED_OWNER_RX,
    P4_HOSTED_OWNER_PROTOCOL,
    P4_HOSTED_OWNER_DISPATCH
};

struct p4_hosted_owner_ops {
    void *context;
    /* Return exactly 1 after acquiring, 0 when busy, otherwise failure.
     * remaining_us is already capped at P4_HOSTED_OWNER_MAX_ACQUIRE_US. */
    int (*acquire)(void *context, uint32_t remaining_us);
    void (*release)(void *context);
    uint32_t (*now_us)(void *context);
    /* Called only outside shared-host ownership, including busy/idle waits. */
    void (*wait)(void *context);
    /* Called after acquisition with min(logical remaining,250ms). The adapter
     * configures the actual host/motor deadline; return exactly 1 on success. */
    int (*set_budget)(void *context, uint32_t slice_us);
    /* Return exactly 1 to request cancellation, otherwise 0. */
    int (*stopping)(void *context);
    /* A successful callback has consumed or copied the borrowed data. */
    int (*dispatch_event)(void *context,
                          const struct p4_hosted_inbox_item *item);
    int (*dispatch_frame)(void *context, const uint8_t *bytes, size_t size);
    /* Existing adapters; owner wraps their session/deadline checks so every
     * operation also observes the owner deadline and current acquisition. */
    const struct p4_hosted_rx_ops *rx;
    const struct p4_hosted_tx_ops *tx;
};

struct p4_hosted_owner_result {
    enum p4_hosted_owner_error error;
    enum p4_hosted_pending_error pending_error;
    enum p4_hosted_rx_error rx_error;
    uint32_t uid;
    uint32_t request_id;
    uint32_t elapsed_us;
    unsigned submitted;
    unsigned matched;
    unsigned batches;
    unsigned events;
    unsigned frames;
    unsigned unmatched;
    unsigned idle;
};

/* Persistent state is caller-owned and must live off small task stacks.
 * It must be zero-initialized only once. `rx` may first be populated by
 * p4_hosted_rx_take_open(); p4_hosted_owner_ids_init() deliberately does not
 * clear or reinitialize it. All operations are exclusive and serialized.
 * Adapter and dispatch callbacks must not re-enter or mutate owner/FIFO/IDs,
 * nor release shared-host ownership. */
struct p4_hosted_owner {
    struct p4_hosted_rx rx;
    struct p4_hosted_request_ids ids;
    struct p4_hosted_pending_result pending;
    struct p4_hosted_rx_result rx_result;
    struct p4_hosted_inbox_item inbox_scratch;
    struct p4_hosted_inbox_item reply;
    uint8_t request[P4_HOSTED_PENDING_MAX_REQUEST_SIZE];
    uint8_t wire[P4_HOSTED_PENDING_BUFFER_MAX];
    uint8_t frame[P4_HOSTED_ETHERNET_FRAME_MAX];
    uint32_t generation;
    uint32_t partial_started_us;
    uint32_t pending_uid;
    uint32_t pending_request_id;
    unsigned initialized;
    unsigned poisoned;
    unsigned partial_active;
    unsigned pending_active;
};

/* After the caller has accepted a fresh OPEN and handed its preserved suffix
 * to owner->rx, bind the persistent UID ledger. The owner RX must already be
 * initialized for generation. now_us is from the same monotonic clock used by
 * ops; when OPEN preserved a partial stream, pass its first-fragment timestamp
 * (or a conservative earlier OPEN start), not the later handoff time.
 * Repeated same-generation initialization is refused; the ledger is not
 * reset and its UID history is retained across retired epochs. */
int p4_hosted_owner_ids_init(struct p4_hosted_owner *owner,
                             uint32_t generation, uint32_t now_us);

/* Send one already-encoded request matching the active owner->ids reservation,
 * then receive until its exact UID/response ID is found or the single absolute
 * deadline expires. timeout_us must be 1..10s. Each host acquisition is at
 * most20ms; each acquired SDIO slice is bounded by min(remaining,250ms).
 * Events and Ethernet frames are drained and dispatched only after release.
 * The matching complete response is copied to owner->reply. Return success
 * means envelope identity only; the caller still validates remote status and
 * operation-specific data. Timeout, consumed transport/protocol failure or
 * dispatch failure retires only this matching generation; no retry/reset or
 * UID reuse is performed. All input, owner, FIFO, adapter and result ranges
 * must be valid, overflow-safe and disjoint. Preflight refusal changes none. */
int p4_hosted_owner_exchange(struct p4_hosted_owner *owner,
    struct p4_hosted_fifo *fifo, const struct p4_hosted_owner_ops *ops,
    uint32_t generation, unsigned checksum_enabled,
    const uint8_t *request, size_t request_size, uint32_t timeout_us,
    struct p4_hosted_owner_result *result);

/* One idle acquisition/poll. It never submits TX and returns success when a
 * bounded idle poll completed (result.idle is set when no RX work arrived).
 * slice_budget_us is 1..250ms and is per acquisition, not a renewable RPC
 * deadline. A partial SERIAL stream retains its first-fragment timestamp and
 * is terminal after2s across calls. Existing queued items are drained before
 * acquisition. All dispatch callbacks run outside shared-host ownership. */
int p4_hosted_owner_step(struct p4_hosted_owner *owner,
    struct p4_hosted_fifo *fifo, const struct p4_hosted_owner_ops *ops,
    uint32_t generation, unsigned checksum_enabled,
    uint32_t slice_budget_us, struct p4_hosted_owner_result *result);

/* Stop/cancel is terminal: wipe RX and private scratch, close the exact active
 * UID without reusing it, and poison matching FIFO/session state. Does not
 * acquire hardware, reset the peer, or restart the owner. */
int p4_hosted_owner_cancel(struct p4_hosted_owner *owner,
    struct p4_hosted_fifo *fifo, uint32_t generation);

#endif
