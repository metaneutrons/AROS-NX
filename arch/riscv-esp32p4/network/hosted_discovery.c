/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#include "hosted_discovery.h"
#include "hosted_rpc.h"

#define DISCOVERY_BODY_MAX (BOARD_DISCOVERY_WIRE_MAX + 32u)

static int overlaps(const void *a, size_t n, const void *b, size_t m)
{
    uintptr_t x = (uintptr_t)a, y = (uintptr_t)b;
    if (!n || !m) return 0;
    if (!a || !b || (uintptr_t)n != n || (uintptr_t)m != m ||
        n > UINTPTR_MAX - x || m > UINTPTR_MAX - y) return 1;
    return x < y + m && y < x + n;
}

static void put_varint(uint8_t *bytes, size_t *offset, uint32_t value)
{
    while (value >= 128u) {
        bytes[(*offset)++] = (uint8_t)(value | 128u);
        value >>= 7;
    }
    bytes[(*offset)++] = (uint8_t)value;
}

static int get_varint(const uint8_t *bytes, size_t size, size_t *offset,
                       uint64_t *value)
{
    uint64_t v = 0;
    unsigned i;
    for (i = 0; i < 10u; ++i) {
        uint8_t byte;
        if (*offset >= size) return 0;
        byte = bytes[(*offset)++];
        if (i == 9u && (byte & 0xfeu)) return 0;
        v |= (uint64_t)(byte & 127u) << (7u * i);
        if (!(byte & 128u)) { *value = v; return 1; }
    }
    return 0;
}

static int skip_field(const uint8_t *bytes, size_t size, size_t *offset,
                       unsigned type)
{
    uint64_t length;
    size_t n;
    switch (type) {
    case 0: return get_varint(bytes, size, offset, &length);
    case 1: n = 8u; break;
    case 2:
        if (!get_varint(bytes, size, offset, &length) ||
            length > size - *offset) return 0;
        n = (size_t)length; break;
    case 5: n = 4u; break;
    default: return 0;
    }
    if (n > size - *offset) return 0;
    *offset += n;
    return 1;
}

int p4_hosted_discovery_request(const struct p4_hosted_init *init,
    uint32_t uid, uint32_t nonce, uint8_t *bytes, size_t capacity,
    size_t *written)
{
    uint8_t body[8];
    size_t offset = 0;
    struct p4_hosted_rpc request;
    if (!init || !bytes || !written || !uid || !nonce ||
        overlaps(bytes, capacity, init, sizeof(*init)) ||
        overlaps(written, sizeof(*written), init, sizeof(*init)) ||
        !p4_hosted_init_has_board_discovery(init)) return 0;
    body[offset++] = 8u;
    body[offset++] = BOARD_DISCOVERY_WIRE_VERSION;
    body[offset++] = 16u;
    put_varint(body, &offset, nonce);
    request.msg_type = P4_HOSTED_RPC_TYPE_REQUEST;
    request.msg_id = P4_HOSTED_DISCOVERY_REQUEST_ID;
    request.uid = uid;
    request.payload = body;
    request.payload_size = offset;
    return p4_hosted_rpc_encode_board_discovery(bytes, capacity, &request, written);
}

int p4_hosted_discovery_response(const uint8_t *bytes, size_t size,
    uint32_t uid, uint32_t nonce, struct p4_hosted_discovery_result *result)
{
    struct p4_hosted_rpc rpc;
    struct p4_hosted_discovery_result parsed = {0};
    const uint8_t *snapshot = NULL;
    size_t snapshot_size = 0, offset = 0;
    unsigned seen_status = 0, seen_snapshot = 0;
    if (!bytes || !result || !uid || !nonce ||
        overlaps(bytes, size, result, sizeof(*result)) ||
        !p4_hosted_rpc_decode_board_discovery(bytes, size, &rpc) ||
        rpc.msg_type != P4_HOSTED_RPC_TYPE_RESPONSE ||
        rpc.msg_id != P4_HOSTED_DISCOVERY_RESPONSE_ID || rpc.uid != uid ||
        rpc.payload_size > DISCOVERY_BODY_MAX) return 0;
    while (offset < rpc.payload_size) {
        uint64_t key, value;
        uint32_t field;
        unsigned type;
        if (!get_varint(rpc.payload, rpc.payload_size, &offset, &key) ||
            !key || (key >> 3) > UINT32_C(0x1fffffff)) return 0;
        field = (uint32_t)(key >> 3);
        type = (unsigned)(key & 7u);
        if (!field) return 0;
        if (field == 1u) {
            if (seen_status || type != 0u ||
                !get_varint(rpc.payload, rpc.payload_size, &offset, &value))
                return 0;
            /* int32 uses either a nonnegative value or sign extension. */
            if (value > INT32_MAX && value < UINT64_C(0xffffffff80000000))
                return 0;
            parsed.status = value <= INT32_MAX ? (int32_t)value :
                -1 - (int32_t)(UINT64_MAX - value);
            seen_status = 1;
        } else if (field == 2u) {
            if (seen_snapshot || type != 2u ||
                !get_varint(rpc.payload, rpc.payload_size, &offset, &value) ||
                value > BOARD_DISCOVERY_WIRE_MAX ||
                value > rpc.payload_size - offset) return 0;
            snapshot = rpc.payload + offset;
            snapshot_size = (size_t)value;
            offset += snapshot_size;
            seen_snapshot = 1;
        } else if (!skip_field(rpc.payload, rpc.payload_size, &offset, type))
            return 0;
    }
    if (!parsed.status) {
        if (!seen_snapshot || !board_discovery_decode(snapshot, snapshot_size,
            nonce, &parsed.snapshot)) return 0;
    } else if (snapshot_size) return 0;
    *result = parsed;
    return 1;
}
