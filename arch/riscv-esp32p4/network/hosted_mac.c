/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#include "hosted_mac.h"

#define MAC_MAX_FIELD UINT32_C(0x1fffffff)

static int ranges_overlap(const void *left, size_t left_size,
                          const void *right, size_t right_size)
{
    uintptr_t left_start, right_start, left_length, right_length;

    if (left_size == 0 || right_size == 0)
        return 0;
    if (!left || !right)
        return 1;

    left_start = (uintptr_t)left;
    right_start = (uintptr_t)right;
    left_length = (uintptr_t)left_size;
    right_length = (uintptr_t)right_size;
    if ((size_t)left_length != left_size ||
        (size_t)right_length != right_size ||
        left_length > UINTPTR_MAX - left_start ||
        right_length > UINTPTR_MAX - right_start)
        return 1;

    return left_start < right_start + right_length &&
           right_start < left_start + left_length;
}

static int read_varint(const uint8_t *bytes, size_t size, size_t *offset,
                       uint64_t *value)
{
    uint64_t parsed = 0;
    size_t cursor = *offset;
    unsigned int i;

    for (i = 0; i < 10u; ++i) {
        uint8_t byte;

        if (cursor >= size)
            return 0;
        byte = bytes[cursor++];
        if (i == 9u && (byte & 0xfeu) != 0)
            return 0;
        parsed |= (uint64_t)(byte & 0x7fu) << (i * 7u);
        if ((byte & 0x80u) == 0) {
            *offset = cursor;
            *value = parsed;
            return 1;
        }
    }
    return 0;
}

static int skip_unknown(const uint8_t *bytes, size_t size, size_t *offset,
                        unsigned int wire_type)
{
    uint64_t length;

    switch (wire_type) {
    case 0:
        return read_varint(bytes, size, offset, &length);
    case 1:
        if (size - *offset < 8u)
            return 0;
        *offset += 8u;
        return 1;
    case 2:
        if (!read_varint(bytes, size, offset, &length) ||
            length > (uint64_t)(size - *offset))
            return 0;
        *offset += (size_t)length;
        return 1;
    case 5:
        if (size - *offset < 4u)
            return 0;
        *offset += 4u;
        return 1;
    default:
        /* Protobuf groups and reserved wire types are not supported. */
        return 0;
    }
}

static int decode_int32(uint64_t value, int32_t *result)
{
    if (value <= (uint64_t)INT32_MAX) {
        *result = (int32_t)value;
        return 1;
    }

    /* A negative protobuf int32 is sign-extended to 64 bits. Reject values
     * that only become negative after truncation to 32 bits. */
    if (value >= UINT64_C(0xffffffff80000000)) {
        uint32_t low = (uint32_t)value;
        uint32_t distance = UINT32_MAX - low;
        *result = -1 - (int32_t)distance;
        return 1;
    }
    return 0;
}

static int is_valid_unicast_mac(const uint8_t mac[P4_HOSTED_MAC_ADDRESS_SIZE])
{
    unsigned int i;
    uint8_t aggregate = 0;

    if ((mac[0] & 1u) != 0)
        return 0;
    for (i = 0; i < P4_HOSTED_MAC_ADDRESS_SIZE; ++i)
        aggregate |= mac[i];
    return aggregate != 0;
}

int p4_hosted_mac_request(uint32_t uid, uint8_t *bytes, size_t capacity,
                          size_t *written)
{
    struct p4_hosted_rpc request;

    request.msg_type = P4_HOSTED_RPC_TYPE_REQUEST;
    request.msg_id = 257u;
    request.uid = uid;
    request.payload = NULL;
    request.payload_size = 0;

    return p4_hosted_rpc_encode(bytes, capacity, &request, written);
}

int p4_hosted_mac_response(const struct p4_hosted_rpc *rpc,
                           uint32_t expected_uid,
                           struct p4_hosted_mac *result)
{
    uint8_t mac[P4_HOSTED_MAC_ADDRESS_SIZE] = { 0 };
    int32_t status = 0;
    size_t offset = 0;
    uint8_t seen_mac = 0;
    uint8_t seen_status = 0;
    uint8_t mac_is_six_bytes = 0;

    if (!rpc || !result ||
        ranges_overlap(rpc, sizeof *rpc, result, sizeof *result) ||
        expected_uid == 0 ||
        rpc->msg_type != P4_HOSTED_RPC_TYPE_RESPONSE ||
        rpc->msg_id != 513u || rpc->uid != expected_uid ||
        rpc->payload_size > P4_HOSTED_MAC_MAX_BODY_SIZE ||
        (rpc->payload_size != 0 && !rpc->payload) ||
        ranges_overlap(rpc->payload, rpc->payload_size,
                       result, sizeof *result))
        return 0;

    while (offset < rpc->payload_size) {
        uint64_t key;
        uint64_t field_number;
        unsigned int wire_type;

        if (!read_varint(rpc->payload, rpc->payload_size, &offset, &key) ||
            key == 0)
            return 0;
        field_number = key >> 3;
        if (field_number == 0 || field_number > MAC_MAX_FIELD)
            return 0;
        wire_type = (unsigned int)(key & 7u);

        if (field_number == 1u) {
            uint64_t length;
            size_t i;

            if (wire_type != 2u || seen_mac ||
                !read_varint(rpc->payload, rpc->payload_size,
                             &offset, &length) ||
                length > (uint64_t)(rpc->payload_size - offset))
                return 0;
            seen_mac = 1;
            mac_is_six_bytes = (uint8_t)(length == P4_HOSTED_MAC_ADDRESS_SIZE);
            if (mac_is_six_bytes) {
                for (i = 0; i < P4_HOSTED_MAC_ADDRESS_SIZE; ++i)
                    mac[i] = rpc->payload[offset + i];
            }
            offset += (size_t)length;
            continue;
        }

        if (field_number == 2u) {
            uint64_t value;

            if (wire_type != 0u || seen_status ||
                !read_varint(rpc->payload, rpc->payload_size,
                             &offset, &value) ||
                !decode_int32(value, &status))
                return 0;
            seen_status = 1;
            continue;
        }

        if (!skip_unknown(rpc->payload, rpc->payload_size,
                          &offset, wire_type))
            return 0;
    }

    result->status = status;
    {
        unsigned int i;
        for (i = 0; i < P4_HOSTED_MAC_ADDRESS_SIZE; ++i)
            result->mac[i] = mac[i];
    }
    result->valid_mac = (status == 0 && mac_is_six_bytes &&
                         is_valid_unicast_mac(mac)) ? 1u : 0u;
    return 1;
}
