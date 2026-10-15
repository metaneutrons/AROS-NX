#ifndef P4_HOSTED_RPC_H
#define P4_HOSTED_RPC_H

/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */

#include <stddef.h>
#include <stdint.h>

/* Local defensive limit for the complete protobuf Rpc envelope. */
#define P4_HOSTED_RPC_MAX_SIZE 4096u

enum p4_hosted_rpc_type {
    P4_HOSTED_RPC_TYPE_INVALID = 0,
    P4_HOSTED_RPC_TYPE_REQUEST = 1,
    P4_HOSTED_RPC_TYPE_RESPONSE = 2,
    P4_HOSTED_RPC_TYPE_EVENT = 3
};

/* Parsed envelope metadata. payload borrows bytes from the decoded input and
 * remains valid only while that input buffer remains valid. The payload is
 * the opaque nested message bytes; this codec does not interpret its fields. */
struct p4_hosted_rpc {
    uint32_t msg_type;
    uint32_t msg_id;
    uint32_t uid;
    const uint8_t *payload;
    size_t payload_size;
};

/* Decode exactly one bounded Rpc protobuf envelope. Requests use msg IDs
 * 257..349, responses 513..605, and events 769..776; the payload field number
 * must equal msg_id. These are class ranges, not a supported-operation
 * allowlist: interior undefined tags remain opaque and callers must reject
 * operations they do not implement. A missing UID is proto3's zero default and is accepted
 * only for events. Requests and responses require nonzero UIDs for native
 * correlation; callers must match both UID and expected response ID against
 * an outstanding transaction. Unknown protobuf fields with wire types 0, 1, 2, or 5 are
 * skipped; groups and malformed fields are rejected. Refusal leaves *rpc
 * unchanged. rpc must not overlap the input bytes. */
int p4_hosted_rpc_decode(const uint8_t *bytes, size_t size,
                         struct p4_hosted_rpc *rpc);

/* Encode canonical top-level fields in field-number order, always including
 * the oneof payload even when it is empty. Proto3's default event UID of zero
 * is omitted. payload may be NULL only when payload_size is zero. Refusal
 * leaves both output bytes and *written unchanged. The output buffer, rpc,
 * written, and nonempty payload must not overlap. */
int p4_hosted_rpc_encode(uint8_t *bytes, size_t capacity,
                         const struct p4_hosted_rpc *rpc, size_t *written);

/* Separate private extension codec. Accepts ONLY discovery request350 or
 * response606 with matching type/nonzero UID. It never widens the ordinary
 * RPC APIs or normal WLAN submission allowlist. Capability, owner epoch and
 * lease admission remain caller responsibilities; decoding is not admission.
 * Same bounds, alias refusal and unchanged-output contract as above. */
int p4_hosted_rpc_decode_board_discovery(const uint8_t *bytes, size_t size,
                                         struct p4_hosted_rpc *rpc);
int p4_hosted_rpc_encode_board_discovery(uint8_t *bytes, size_t capacity,
    const struct p4_hosted_rpc *rpc, size_t *written);

#endif
