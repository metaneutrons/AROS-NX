/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#ifndef P4_HOSTED_DISCOVERY_H
#define P4_HOSTED_DISCOVERY_H
#include "hosted_init.h"
#include "../boardtool/discovery_wire.h"

#define P4_HOSTED_DISCOVERY_REQUEST_ID 350u
#define P4_HOSTED_DISCOVERY_RESPONSE_ID 606u

struct p4_hosted_discovery_result {
    int32_t status;
    struct board_discovery_snapshot snapshot;
};

/* Codec only, no transport or permission to update. Requires a C6 INIT with
 * explicit supported capability; uid/nonce must be nonzero. Caller must bind
 * INIT, UID and nonce to its live owner/peer generation and maintenance lease.
 * No request is issued by this function. Output/written/init must not overlap;
 * refusal leaves outputs unchanged, including undersized/overflowing ranges. */
int p4_hosted_discovery_request(const struct p4_hosted_init *init,
    uint32_t uid, uint32_t nonce, uint8_t *bytes, size_t capacity,
    size_t *written);

/* Decode a complete private response correlated by ID/type/UID and nonce.
 * Success status0 requires a valid canonical inventory. Nonzero status permits
 * no inventory (absent/empty bytes) and publishes a zero snapshot. Unknown
 * bounded protobuf fields are skipped; duplicates/groups/malformed bodies
 * refuse. Caller must separately check live epoch/lease BEFORE publishing.
 * No authentication, installed-table provenance or write authority is implied.
 * Refusal leaves result unchanged; it must not overlap the input bytes. Keep
 * result off small stacks; decoding has a bounded local inventory scratch. */
int p4_hosted_discovery_response(const uint8_t *bytes, size_t size,
    uint32_t uid, uint32_t nonce, struct p4_hosted_discovery_result *result);
#endif
