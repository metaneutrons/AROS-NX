#ifndef P4_HOSTED_MAC_H
#define P4_HOSTED_MAC_H

/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */

#include "hosted_rpc.h"

#define P4_HOSTED_MAC_ADDRESS_SIZE 6u

/* Local bound for the nested Rpc_Resp_GetMacAddress protobuf body. */
#define P4_HOSTED_MAC_MAX_BODY_SIZE 256u

struct p4_hosted_mac {
    int32_t status;
    uint8_t mac[P4_HOSTED_MAC_ADDRESS_SIZE];
    unsigned valid_mac;
};

/* Encode a canonical Rpc request for GetMACAddress in STA mode (mode 0).
 * The proto3-default mode produces an empty nested body. Refusal leaves the
 * output bytes and *written unchanged. */
int p4_hosted_mac_request(uint32_t uid, uint8_t *bytes, size_t capacity,
                          size_t *written);

/* Decode one response for a previously issued GetMACAddress request.
 * Requires response type, ID 513, a nonzero exact UID match and a nested body
 * no larger than P4_HOSTED_MAC_MAX_BODY_SIZE. Unknown scalar/fixed/bytes
 * fields are skipped; groups, malformed fields and duplicate known fields
 * are rejected. A syntactically valid error response is returned with its
 * signed status and valid_mac == 0. valid_mac is set only for a zero status
 * and an actual six-byte, nonzero unicast address. Refusal leaves *result
 * unchanged. result must not overlap rpc or its nested payload. */
int p4_hosted_mac_response(const struct p4_hosted_rpc *rpc,
                           uint32_t expected_uid,
                           struct p4_hosted_mac *result);

#endif
