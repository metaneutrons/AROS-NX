/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#include "hosted_rpc.h"

#define RPC_MAX_FIELD UINT32_C(0x1fffffff)

static int rpc_id_matches_type(uint32_t msg_type, uint32_t msg_id,
                               unsigned discovery)
{
    switch (msg_type) {
    case P4_HOSTED_RPC_TYPE_REQUEST:
        return (msg_id > 256u && msg_id < 350u) ||
               (discovery && msg_id == 350u);
    case P4_HOSTED_RPC_TYPE_RESPONSE:
        return (msg_id > 512u && msg_id < 606u) ||
               (discovery && msg_id == 606u);
    case P4_HOSTED_RPC_TYPE_EVENT:
        return msg_id > 768u && msg_id < 777u;
    default:
        return 0;
    }
}

/* Treat address arithmetic overflow as overlap. This also makes absurd
 * caller-supplied capacities fail closed before the encoder writes anything. */
static int ranges_overlap(const void *left, size_t left_size,
                          const void *right, size_t right_size)
{
    uintptr_t left_start, right_start, left_length, right_length;
    uintptr_t left_end, right_end;

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

    left_end = left_start + left_length;
    right_end = right_start + right_length;
    return left_start < right_end && right_start < left_end;
}

static size_t varint_size(uint64_t value)
{
    size_t size = 1;

    while (value >= UINT64_C(0x80)) {
        value >>= 7;
        ++size;
    }
    return size;
}

static void write_varint(uint8_t *bytes, size_t *offset, uint64_t value)
{
    while (value >= UINT64_C(0x80)) {
        bytes[*offset] = (uint8_t)((value & UINT64_C(0x7f)) | UINT64_C(0x80));
        ++*offset;
        value >>= 7;
    }
    bytes[*offset] = (uint8_t)value;
    ++*offset;
}

/* A volatile destination prevents compilers from replacing this loop with a
 * libc copy helper, which is unavailable during early module loading. */
static void copy_payload(uint8_t *destination, const uint8_t *source,
                         size_t size)
{
    volatile uint8_t *volatile_destination = destination;
    size_t i;

    for (i = 0; i < size; ++i)
        volatile_destination[i] = source[i];
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
                        uint8_t wire_type)
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
        /* Deprecated groups (3/4) and reserved wire types (6/7). */
        return 0;
    }
}

static int is_rpc_payload_field(uint32_t field, unsigned discovery)
{
    return (field > 256u && field < 350u) ||
           (field > 512u && field < 606u) ||
           (field > 768u && field < 777u) ||
           (discovery && (field == 350u || field == 606u));
}

static int rpc_decode(const uint8_t *bytes, size_t size,
                       struct p4_hosted_rpc *rpc, unsigned discovery)
{
    uint32_t msg_type = 0;
    uint32_t msg_id = 0;
    uint32_t payload_field = 0;
    uint32_t uid = 0;
    const uint8_t *payload = NULL;
    size_t payload_size = 0;
    size_t offset = 0;
    int seen_type = 0;
    int seen_id = 0;
    int seen_uid = 0;
    int seen_payload = 0;

    if (!bytes || !rpc || size == 0 || size > P4_HOSTED_RPC_MAX_SIZE ||
        ranges_overlap(bytes, size, rpc, sizeof *rpc))
        return 0;

    while (offset < size) {
        uint64_t key;
        uint64_t value;
        uint32_t field;
        uint8_t wire_type;

        if (!read_varint(bytes, size, &offset, &key) || key == 0)
            return 0;
        if ((key >> 3) > RPC_MAX_FIELD)
            return 0;
        field = (uint32_t)(key >> 3);
        wire_type = (uint8_t)(key & 7u);
        if (field == 0u ||
            (wire_type != 0u && wire_type != 1u &&
             wire_type != 2u && wire_type != 5u))
            return 0;

        if (field >= 1u && field <= 3u) {
            if (wire_type != 0u ||
                !read_varint(bytes, size, &offset, &value) ||
                value > UINT32_MAX)
                return 0;
            if (field == 1u) {
                if (seen_type)
                    return 0;
                seen_type = 1;
                msg_type = (uint32_t)value;
            } else if (field == 2u) {
                if (seen_id)
                    return 0;
                seen_id = 1;
                msg_id = (uint32_t)value;
            } else {
                if (seen_uid)
                    return 0;
                seen_uid = 1;
                uid = (uint32_t)value;
            }
            continue;
        }

        if (is_rpc_payload_field(field, discovery)) {
            if (wire_type != 2u || seen_payload ||
                !read_varint(bytes, size, &offset, &value) ||
                value > (uint64_t)(size - offset) ||
                value > P4_HOSTED_RPC_MAX_SIZE)
                return 0;
            seen_payload = 1;
            payload_field = field;
            payload = bytes + offset;
            payload_size = (size_t)value;
            offset += payload_size;
            continue;
        }

        if (!skip_unknown(bytes, size, &offset, wire_type))
            return 0;
    }

    if (!seen_type || !seen_id || !seen_payload ||
        !rpc_id_matches_type(msg_type, msg_id, discovery) ||
        (discovery && msg_id != 350u && msg_id != 606u) ||
        payload_field != msg_id ||
        (msg_type != P4_HOSTED_RPC_TYPE_EVENT && uid == 0))
        return 0;

    rpc->msg_type = msg_type;
    rpc->msg_id = msg_id;
    rpc->uid = uid;
    rpc->payload = payload;
    rpc->payload_size = payload_size;
    return 1;
}

static int rpc_encode(uint8_t *bytes, size_t capacity,
                       const struct p4_hosted_rpc *rpc, size_t *written,
                       unsigned discovery)
{
    const uint8_t *payload;
    size_t payload_size;
    size_t total;
    size_t offset = 0;
    uint64_t payload_key;

    if (!bytes || !rpc || !written ||
        (rpc->payload_size != 0 && !rpc->payload) ||
        rpc->payload_size > P4_HOSTED_RPC_MAX_SIZE ||
        !rpc_id_matches_type(rpc->msg_type, rpc->msg_id, discovery) ||
        (discovery && rpc->msg_id != 350u && rpc->msg_id != 606u) ||
        (rpc->msg_type != P4_HOSTED_RPC_TYPE_EVENT && rpc->uid == 0))
        return 0;

    payload = rpc->payload;
    payload_size = rpc->payload_size;
    payload_key = ((uint64_t)rpc->msg_id << 3) | UINT64_C(2);
    total = 1u + varint_size(rpc->msg_type) +
            1u + varint_size(rpc->msg_id) +
            varint_size(payload_key) + varint_size(payload_size) +
            payload_size;
    if (rpc->uid != 0)
        total += 1u + varint_size(rpc->uid);

    if (total > P4_HOSTED_RPC_MAX_SIZE || capacity < total ||
        ranges_overlap(bytes, capacity, rpc, sizeof *rpc) ||
        ranges_overlap(bytes, capacity, written, sizeof *written) ||
        ranges_overlap(bytes, capacity, payload, payload_size) ||
        ranges_overlap(written, sizeof *written, rpc, sizeof *rpc) ||
        ranges_overlap(rpc, sizeof *rpc, payload, payload_size) ||
        ranges_overlap(written, sizeof *written, payload, payload_size))
        return 0;

    write_varint(bytes, &offset, UINT64_C(8));
    write_varint(bytes, &offset, rpc->msg_type);
    write_varint(bytes, &offset, UINT64_C(16));
    write_varint(bytes, &offset, rpc->msg_id);
    if (rpc->uid != 0) {
        write_varint(bytes, &offset, UINT64_C(24));
        write_varint(bytes, &offset, rpc->uid);
    }
    write_varint(bytes, &offset, payload_key);
    write_varint(bytes, &offset, payload_size);
    copy_payload(bytes + offset, payload, payload_size);
    offset += payload_size;

    *written = offset;
    return 1;
}

int p4_hosted_rpc_decode(const uint8_t *bytes, size_t size,
                         struct p4_hosted_rpc *rpc)
{
    return rpc_decode(bytes, size, rpc, 0u);
}

int p4_hosted_rpc_encode(uint8_t *bytes, size_t capacity,
                         const struct p4_hosted_rpc *rpc, size_t *written)
{
    return rpc_encode(bytes, capacity, rpc, written, 0u);
}

int p4_hosted_rpc_decode_board_discovery(const uint8_t *bytes, size_t size,
                                         struct p4_hosted_rpc *rpc)
{
    return rpc_decode(bytes, size, rpc, 1u);
}

int p4_hosted_rpc_encode_board_discovery(uint8_t *bytes, size_t capacity,
    const struct p4_hosted_rpc *rpc, size_t *written)
{
    return rpc_encode(bytes, capacity, rpc, written, 1u);
}
