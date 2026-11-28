#ifndef P4_HOSTED_REQUEST_IDS_H
#define P4_HOSTED_REQUEST_IDS_H

/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */

#include <stdint.h>

/* Persistent, caller-owned identity ledger for one hosted RPC peer. The
 * generation is a host-side transport token, not evidence that the C6 was
 * physically reset or that its firmware started fresh. All calls require
 * caller-exclusive serialization; this ledger is not atomic or thread-safe. */
struct p4_hosted_request_ids {
    uint32_t initialized;
    uint32_t retired;
    uint32_t generation;
    uint32_t next_uid;
    uint32_t active_uid;
    uint32_t active_request_id;
    uint32_t exhausted;
};

/* Initialize a zero-initialized ledger, or rebind an explicitly retired
 * ledger to a different nonzero host generation. Rebind preserves next_uid
 * and exhaustion for the lifetime of this object; it never proves freshness
 * of the peer. Same-generation restart and active-ledger rebind are refused.
 * The caller must retain this object across epochs and must not reconstruct or
 * zero it while the peer may retain state. */
int p4_hosted_request_ids_init(struct p4_hosted_request_ids *ids,
                               uint32_t generation);

/* Reserve the one outstanding request. request_id must be nonzero and leave
 * room for the corresponding response ID (+256). UID is consumed immediately
 * on success and is never reused, even if later encoding or transmission
 * fails. UINT32_MAX may be reserved once; the ledger then remains exhausted
 * across every later generation. Refusal leaves both ledger and output UID
 * unchanged. The output pointer must not overlap the ledger. */
int p4_hosted_request_ids_reserve(struct p4_hosted_request_ids *ids,
                                  uint32_t generation,
                                  uint32_t request_id, uint32_t *uid);

/* Finish only the exact active (generation, UID, request ID) transaction.
 * Stale or mismatched completions do not change the ledger. */
int p4_hosted_request_ids_finish(struct p4_hosted_request_ids *ids,
                                 uint32_t generation, uint32_t uid,
                                 uint32_t request_id);

/* Retire a matching live generation and close its active reservation. A
 * stale-generation call is refused without mutation. A later init may bind a
 * different generation, preserving UID history and permanent exhaustion. */
int p4_hosted_request_ids_invalidate(struct p4_hosted_request_ids *ids,
                                     uint32_t generation);

#endif
