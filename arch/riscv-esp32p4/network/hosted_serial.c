/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#include "hosted_serial.h"

/* Freestanding BSP code must not import libc startup (.eh_frame is not
 * placed by the module loader). Volatile accesses prevent the compiler from
 * replacing these bounded byte operations with memcpy/memcmp calls. */
static void copy_bytes(uint8_t *destination, const uint8_t *source, size_t size)
{
    volatile uint8_t *out = destination;
    const volatile uint8_t *in = source;
    size_t index;
    for (index = 0; index < size; ++index)
        out[index] = in[index];
}

static int equal_bytes(const uint8_t *a, const uint8_t *b, size_t size)
{
    const volatile uint8_t *first = a;
    const volatile uint8_t *second = b;
    size_t index;
    for (index = 0; index < size; ++index)
        if (first[index] != second[index])
            return 0;
    return 1;
}

static const uint8_t endpoint_rpc_rsp[HOSTED_SERIAL_ENDPOINT_SIZE] = {
    'R', 'P', 'C', 'R', 's', 'p'
};
static const uint8_t endpoint_rpc_evt[HOSTED_SERIAL_ENDPOINT_SIZE] = {
    'R', 'P', 'C', 'E', 'v', 't'
};

/* Uses integer address differences rather than relational comparisons or
 * pointer addition across unrelated objects. Unrepresentable spans are
 * conservatively treated as overlapping. */
static int ranges_overlap(const void *a, size_t a_size,
                          const void *b, size_t b_size)
{
    uintptr_t a_start, b_start, a_length, b_length;

    if (a_size == 0 || b_size == 0)
        return 0;
#if SIZE_MAX > UINTPTR_MAX
    if ((uintmax_t)a_size > (uintmax_t)UINTPTR_MAX ||
        (uintmax_t)b_size > (uintmax_t)UINTPTR_MAX)
        return 1;
#endif

    a_start = (uintptr_t)a;
    b_start = (uintptr_t)b;
    a_length = (uintptr_t)a_size;
    b_length = (uintptr_t)b_size;
    if (a_start <= b_start)
        return b_start - a_start < a_length;
    return a_start - b_start < b_length;
}

static uint16_t get_le16(const uint8_t *bytes)
{
    return (uint16_t)((unsigned)bytes[0] | ((unsigned)bytes[1] << 8));
}

static void put_le16(uint8_t *bytes, size_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
}

static int decode_endpoint(const uint8_t *bytes,
                           enum hosted_serial_endpoint *endpoint)
{
    if (equal_bytes(bytes, endpoint_rpc_rsp, HOSTED_SERIAL_ENDPOINT_SIZE)) {
        *endpoint = HOSTED_SERIAL_ENDPOINT_RPCRSP;
        return 1;
    }
    if (equal_bytes(bytes, endpoint_rpc_evt, HOSTED_SERIAL_ENDPOINT_SIZE)) {
        *endpoint = HOSTED_SERIAL_ENDPOINT_RPCEVT;
        return 1;
    }
    return 0;
}

int hosted_serial_decode(const uint8_t *bytes, size_t available,
                         struct hosted_serial_message *message)
{
    struct hosted_serial_message candidate;
    size_t endpoint_value_offset, data_header_offset, payload_offset;
    size_t protobuf_size, wire_size;

    if (!bytes || !message ||
        ranges_overlap(bytes, available, message, sizeof *message) ||
        available < HOSTED_SERIAL_MESSAGE_OVERHEAD)
        return 0;

    if (bytes[0] != 1u ||
        get_le16(bytes + 1) != HOSTED_SERIAL_ENDPOINT_SIZE)
        return 0;
    endpoint_value_offset = HOSTED_SERIAL_TLV_HEADER_SIZE;
    if (!decode_endpoint(bytes + endpoint_value_offset, &candidate.endpoint))
        return 0;

    data_header_offset = endpoint_value_offset + HOSTED_SERIAL_ENDPOINT_SIZE;
    if (bytes[data_header_offset] != 2u)
        return 0;
    protobuf_size = get_le16(bytes + data_header_offset + 1u);
    if (protobuf_size == 0 || protobuf_size > HOSTED_SERIAL_PROTOBUF_MAX)
        return 0;

    payload_offset = data_header_offset + HOSTED_SERIAL_TLV_HEADER_SIZE;
    wire_size = payload_offset + protobuf_size;
    if (wire_size != available)
        return 0;

    candidate.protobuf = bytes + payload_offset;
    candidate.protobuf_size = protobuf_size;
    candidate.wire_size = wire_size;
    *message = candidate;
    return 1;
}

int hosted_serial_encode(uint8_t *bytes, size_t capacity,
                         const struct hosted_serial_message *message,
                         size_t *written)
{
    enum hosted_serial_endpoint endpoint;
    const uint8_t *endpoint_bytes;
    const uint8_t *protobuf;
    size_t protobuf_size, wire_size;

    if (!bytes || !message || !written)
        return 0;

    /* Snapshot all inputs before writing, and reject every alias that would
     * make a successful encode modify input state or metadata. */
    endpoint = message->endpoint;
    protobuf = message->protobuf;
    protobuf_size = message->protobuf_size;
    if (endpoint == HOSTED_SERIAL_ENDPOINT_RPCRSP)
        endpoint_bytes = endpoint_rpc_rsp;
    else if (endpoint == HOSTED_SERIAL_ENDPOINT_RPCEVT)
        endpoint_bytes = endpoint_rpc_evt;
    else
        return 0;

    if (!protobuf || protobuf_size == 0 ||
        protobuf_size > HOSTED_SERIAL_PROTOBUF_MAX)
        return 0;
    wire_size = HOSTED_SERIAL_MESSAGE_OVERHEAD + protobuf_size;
    if (capacity < wire_size ||
        ranges_overlap(bytes, wire_size, message, sizeof *message) ||
        ranges_overlap(bytes, wire_size, written, sizeof *written) ||
        ranges_overlap(bytes, wire_size, protobuf, protobuf_size) ||
        ranges_overlap(message, sizeof *message, protobuf, protobuf_size) ||
        ranges_overlap(written, sizeof *written, message, sizeof *message) ||
        ranges_overlap(written, sizeof *written, protobuf, protobuf_size))
        return 0;

    bytes[0] = 1u;
    put_le16(bytes + 1u, HOSTED_SERIAL_ENDPOINT_SIZE);
    copy_bytes(bytes + HOSTED_SERIAL_TLV_HEADER_SIZE, endpoint_bytes,
           HOSTED_SERIAL_ENDPOINT_SIZE);
    bytes[9] = 2u;
    put_le16(bytes + 10u, protobuf_size);
    copy_bytes(bytes + HOSTED_SERIAL_MESSAGE_OVERHEAD, protobuf, protobuf_size);
    *written = wire_size;
    return 1;
}
