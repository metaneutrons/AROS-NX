/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#include "hosted_ota.h"

#define OTA_REQ_BEGIN 272u
#define OTA_REQ_WRITE 273u
#define OTA_REQ_END 274u
#define OTA_MAX_FIELD UINT32_C(0x1fffffff)

static int range_is_valid(const void *pointer, size_t size)
{
    uintptr_t start, length;

    if (size == 0u)
        return 1;
    if (!pointer)
        return 0;
    start = (uintptr_t)pointer;
    length = (uintptr_t)size;
    if ((size_t)length != size || length > UINTPTR_MAX - start)
        return 0;
    return 1;
}

/* Overflowing or unrepresentable ranges are treated as overlapping. */
static int ranges_overlap(const void *left, size_t left_size,
                          const void *right, size_t right_size)
{
    uintptr_t left_start, right_start, left_length, right_length;

    if (left_size == 0u || right_size == 0u)
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

static size_t varint_size(uint64_t value)
{
    size_t size = 1u;

    while (value >= UINT64_C(0x80)) {
        value >>= 7;
        ++size;
    }
    return size;
}

static size_t request_envelope_size(uint32_t request_id, uint32_t uid,
                                    size_t payload_size)
{
    uint64_t payload_key = ((uint64_t)request_id << 3) | UINT64_C(2);

    return 2u + 1u + varint_size(request_id) +
           1u + varint_size(uid) + varint_size(payload_key) +
           varint_size(payload_size) + payload_size;
}

static void write_varint(uint8_t *bytes, size_t *offset, uint64_t value)
{
    while (value >= UINT64_C(0x80)) {
        bytes[*offset] = (uint8_t)((value & UINT64_C(0x7f)) |
                                   UINT64_C(0x80));
        ++*offset;
        value >>= 7;
    }
    bytes[*offset] = (uint8_t)value;
    ++*offset;
}

/* Keep the module freestanding: do not introduce a libc copy dependency. */
static void copy_bytes(uint8_t *destination, const uint8_t *source,
                       size_t size)
{
    volatile uint8_t *volatile_destination = destination;
    size_t i;

    for (i = 0u; i < size; ++i)
        volatile_destination[i] = source[i];
}

static int encode_request(uint32_t request_id, uint32_t uid,
                          const uint8_t *payload, size_t payload_size,
                          const void *input, size_t input_size,
                          uint8_t *bytes, size_t capacity, size_t *written)
{
    struct p4_hosted_rpc request;

    if (!bytes || !written || uid == 0u ||
        request_envelope_size(request_id, uid, payload_size) >
            P4_HOSTED_OTA_MAX_REQUEST_SIZE ||
        ranges_overlap(bytes, capacity, written, sizeof *written) ||
        (input_size != 0u &&
         (!range_is_valid(input, input_size) ||
          ranges_overlap(bytes, capacity, input, input_size) ||
          ranges_overlap(written, sizeof *written, input, input_size))))
        return 0;

    request.msg_type = P4_HOSTED_RPC_TYPE_REQUEST;
    request.msg_id = request_id;
    request.uid = uid;
    request.payload = payload;
    request.payload_size = payload_size;
    return p4_hosted_rpc_encode(bytes, capacity, &request, written);
}

int p4_hosted_ota_begin_request(uint32_t uid, uint8_t *bytes,
                                size_t capacity, size_t *written)
{
    return encode_request(OTA_REQ_BEGIN, uid, NULL, 0u, NULL, 0u,
                          bytes, capacity, written);
}

int p4_hosted_ota_write_request(uint32_t uid, const uint8_t *data,
                                size_t data_size, uint8_t *bytes,
                                size_t capacity, size_t *written)
{
    uint8_t body[1u + 2u + P4_HOSTED_OTA_MAX_WRITE_SIZE];
    size_t offset = 0u;
    uint64_t key = (UINT64_C(1) << 3) | UINT64_C(2);

    if (data_size == 0u || data_size > P4_HOSTED_OTA_MAX_WRITE_SIZE ||
        !range_is_valid(data, data_size))
        return 0;

    write_varint(body, &offset, key);
    write_varint(body, &offset, (uint64_t)data_size);
    copy_bytes(body + offset, data, data_size);
    offset += data_size;

    return encode_request(OTA_REQ_WRITE, uid, body, offset,
                          data, data_size, bytes, capacity, written);
}

int p4_hosted_ota_end_request(uint32_t uid, uint8_t *bytes,
                              size_t capacity, size_t *written)
{
    return encode_request(OTA_REQ_END, uid, NULL, 0u, NULL, 0u,
                          bytes, capacity, written);
}

static int read_varint(const uint8_t *bytes, size_t size, size_t *offset,
                       uint64_t *value)
{
    uint64_t parsed = 0u;
    size_t cursor = *offset;
    unsigned int i;

    for (i = 0u; i < 10u; ++i) {
        uint8_t byte;

        if (cursor >= size)
            return 0;
        byte = bytes[cursor++];
        if (i == 9u && (byte & 0xfeu) != 0u)
            return 0;
        parsed |= (uint64_t)(byte & 0x7fu) << (i * 7u);
        if ((byte & 0x80u) == 0u) {
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
    case 0u:
        return read_varint(bytes, size, offset, &length);
    case 1u:
        if (*offset > size || size - *offset < 8u)
            return 0;
        *offset += 8u;
        return 1;
    case 2u:
        if (!read_varint(bytes, size, offset, &length) || *offset > size ||
            length > (uint64_t)(size - *offset))
            return 0;
        *offset += (size_t)length;
        return 1;
    case 5u:
        if (*offset > size || size - *offset < 4u)
            return 0;
        *offset += 4u;
        return 1;
    default:
        /* Deprecated groups and reserved wire types are refused. */
        return 0;
    }
}

static int decode_int32(uint64_t value, int32_t *result)
{
    if (value <= (uint64_t)INT32_MAX) {
        *result = (int32_t)value;
        return 1;
    }

    /* Protobuf int32 negative values are sign-extended to 64 bits. */
    if (value >= UINT64_C(0xffffffff80000000)) {
        uint32_t low = (uint32_t)value;
        uint32_t distance = UINT32_MAX - low;
        *result = -1 - (int32_t)distance;
        return 1;
    }
    return 0;
}

static int supported_request_id(uint32_t request_id)
{
    return request_id == OTA_REQ_BEGIN || request_id == OTA_REQ_WRITE ||
           request_id == OTA_REQ_END;
}

int p4_hosted_ota_request_is_canonical(const uint8_t *bytes, size_t size)
{
    struct p4_hosted_rpc rpc;
    uint8_t canonical[P4_HOSTED_OTA_MAX_REQUEST_SIZE];
    size_t written = 0u, offset = 0u, i;
    uint64_t length;
    int encoded;

    if (!bytes || size == 0u || size > sizeof canonical ||
        !range_is_valid(bytes, size) ||
        !p4_hosted_rpc_decode(bytes, size, &rpc) ||
        rpc.msg_type != P4_HOSTED_RPC_TYPE_REQUEST || rpc.uid == 0u ||
        !supported_request_id(rpc.msg_id))
        return 0;

    if (rpc.msg_id == OTA_REQ_WRITE) {
        if (rpc.payload_size < 3u || rpc.payload[0] != 10u)
            return 0;
        offset = 1u;
        if (!read_varint(rpc.payload, rpc.payload_size, &offset, &length) ||
            length == 0u || length > P4_HOSTED_OTA_MAX_WRITE_SIZE ||
            length != rpc.payload_size - offset)
            return 0;
        encoded = p4_hosted_ota_write_request(rpc.uid, rpc.payload + offset,
            (size_t)length, canonical, sizeof canonical, &written);
    } else {
        if (rpc.payload_size != 0u)
            return 0;
        encoded = rpc.msg_id == OTA_REQ_BEGIN
            ? p4_hosted_ota_begin_request(rpc.uid, canonical,
                sizeof canonical, &written)
            : p4_hosted_ota_end_request(rpc.uid, canonical,
                sizeof canonical, &written);
    }
    if (!encoded || written != size)
        return 0;
    for (i = 0u; i < size; ++i)
        if (canonical[i] != bytes[i])
            return 0;
    return 1;
}

int p4_hosted_ota_status_response(const struct p4_hosted_rpc *rpc,
                                  uint32_t expected_request_id,
                                  uint32_t expected_uid, int32_t *status)
{
    int32_t decoded_status = 0;
    size_t offset = 0u;
    int seen_status = 0;
    uint32_t response_id;

    if (!rpc || !status || !range_is_valid(rpc, sizeof *rpc) ||
        !range_is_valid(status, sizeof *status) ||
        ranges_overlap(rpc, sizeof *rpc, status, sizeof *status) ||
        expected_uid == 0u || !supported_request_id(expected_request_id))
        return 0;

    response_id = expected_request_id + 256u;
    if (rpc->msg_type != P4_HOSTED_RPC_TYPE_RESPONSE ||
        rpc->msg_id != response_id || rpc->uid != expected_uid ||
        rpc->payload_size > P4_HOSTED_OTA_MAX_RESPONSE_BODY_SIZE ||
        (rpc->payload_size != 0u &&
         (!range_is_valid(rpc->payload, rpc->payload_size) ||
          ranges_overlap(rpc->payload, rpc->payload_size,
                         status, sizeof *status))))
        return 0;

    while (offset < rpc->payload_size) {
        uint64_t key, field_number;
        unsigned int wire_type;

        if (!read_varint(rpc->payload, rpc->payload_size, &offset, &key) ||
            key == 0u)
            return 0;
        field_number = key >> 3;
        if (field_number == 0u || field_number > OTA_MAX_FIELD)
            return 0;
        wire_type = (unsigned int)(key & 7u);

        if (field_number == 1u) {
            uint64_t value;

            if (wire_type != 0u || seen_status ||
                !read_varint(rpc->payload, rpc->payload_size,
                             &offset, &value) ||
                !decode_int32(value, &decoded_status))
                return 0;
            seen_status = 1;
            continue;
        }

        if (!skip_unknown(rpc->payload, rpc->payload_size,
                          &offset, wire_type))
            return 0;
    }

    *status = decoded_status;
    return 1;
}
