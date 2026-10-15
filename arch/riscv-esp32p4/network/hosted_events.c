/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#include "hosted_events.h"

#define AUX_EVENT_MAX_FIELD UINT32_C(0x1fffffff)

struct reader {
    const uint8_t *bytes;
    size_t size;
    size_t offset;
};

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

/* Invalid or overflowing ranges are conservatively treated as overlapping. */
static int ranges_overlap(const void *left, size_t left_size,
                          const void *right, size_t right_size)
{
    uintptr_t left_start, right_start, left_length, right_length;

    if (left_size == 0u || right_size == 0u)
        return 0;
    if (!range_is_valid(left, left_size) ||
        !range_is_valid(right, right_size))
        return 1;
    left_start = (uintptr_t)left;
    right_start = (uintptr_t)right;
    left_length = (uintptr_t)left_size;
    right_length = (uintptr_t)right_size;
    return left_start < right_start + right_length &&
           right_start < left_start + left_length;
}

static void clear_bytes(uint8_t *bytes, size_t size)
{
    size_t index;

    for (index = 0u; index < size; ++index)
        bytes[index] = 0u;
}

static int copy_bytes(uint8_t *destination, const uint8_t *source,
                      size_t size)
{
    size_t index;

    if (size != 0u && (!destination || !source))
        return 0;
    for (index = 0u; index < size; ++index)
        destination[index] = source[index];
    return 1;
}

static void copy_result(struct p4_hosted_aux_event *destination,
                        const struct p4_hosted_aux_event *source)
{
    volatile uint8_t *destination_bytes =
        (volatile uint8_t *)(void *)destination;
    const volatile uint8_t *source_bytes =
        (const volatile uint8_t *)(const void *)source;
    size_t index;

    for (index = 0u; index < sizeof *destination; ++index)
        destination_bytes[index] = source_bytes[index];
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

static int reader_varint(struct reader *reader, uint64_t *value)
{
    uint64_t parsed = 0u;
    size_t start;
    unsigned int index;

    if (!reader || !value || reader->offset > reader->size)
        return 0;
    start = reader->offset;
    for (index = 0u; index < 10u; ++index) {
        uint8_t byte;

        if (reader->offset >= reader->size)
            return 0;
        byte = reader->bytes[reader->offset++];
        if (index == 9u && (byte & 0xfeu) != 0u)
            return 0;
        parsed |= (uint64_t)(byte & 0x7fu) << (index * 7u);
        if ((byte & 0x80u) == 0u) {
            if (reader->offset - start != varint_size(parsed))
                return 0;
            *value = parsed;
            return 1;
        }
    }
    return 0;
}

static int reader_key(struct reader *reader, uint32_t *field,
                      unsigned int *wire_type)
{
    uint64_t key;

    if (!reader_varint(reader, &key) || key == 0u ||
        (key >> 3) == 0u || (key >> 3) > AUX_EVENT_MAX_FIELD)
        return 0;
    *field = (uint32_t)(key >> 3);
    *wire_type = (unsigned int)(key & 7u);
    return *wire_type == 0u || *wire_type == 1u || *wire_type == 2u ||
           *wire_type == 5u;
}

static int reader_uint32(struct reader *reader, uint32_t *value)
{
    uint64_t parsed;

    if (!reader_varint(reader, &parsed) || parsed > UINT32_MAX)
        return 0;
    *value = (uint32_t)parsed;
    return 1;
}

static int decode_int32_value(uint64_t value, int32_t *decoded)
{
    if (value <= (uint64_t)INT32_MAX) {
        *decoded = (int32_t)value;
        return 1;
    }
    if (value >= UINT64_C(0xffffffff80000000)) {
        uint32_t low = (uint32_t)value;
        uint32_t distance = UINT32_MAX - low;
        *decoded = -1 - (int32_t)distance;
        return 1;
    }
    return 0;
}

static int reader_int32(struct reader *reader, int32_t *value)
{
    uint64_t parsed;

    return reader_varint(reader, &parsed) && decode_int32_value(parsed, value);
}

static int reader_bytes(struct reader *reader, const uint8_t **bytes,
                        size_t *length)
{
    uint64_t parsed_length;

    if (reader->offset > reader->size ||
        !reader_varint(reader, &parsed_length) ||
        parsed_length > (uint64_t)(reader->size - reader->offset))
        return 0;
    *bytes = reader->bytes + reader->offset;
    *length = (size_t)parsed_length;
    reader->offset += *length;
    return 1;
}

static int reader_skip(struct reader *reader, unsigned int wire_type)
{
    uint64_t length;

    if (reader->offset > reader->size)
        return 0;
    switch (wire_type) {
    case 0u:
        return reader_varint(reader, &length);
    case 1u:
        if (reader->size - reader->offset < 8u)
            return 0;
        reader->offset += 8u;
        return 1;
    case 2u:
        if (!reader_varint(reader, &length) ||
            length > (uint64_t)(reader->size - reader->offset))
            return 0;
        reader->offset += (size_t)length;
        return 1;
    case 5u:
        if (reader->size - reader->offset < 4u)
            return 0;
        reader->offset += 4u;
        return 1;
    default:
        /* Deprecated groups and reserved wire types are unsupported. */
        return 0;
    }
}

static int rpc_input_is_valid(const struct p4_hosted_rpc *rpc,
                              const struct p4_hosted_aux_event *result)
{
    if (!rpc || !result ||
        !range_is_valid(rpc, sizeof *rpc) ||
        !range_is_valid(result, sizeof *result) ||
        ranges_overlap(rpc, sizeof *rpc, result, sizeof *result))
        return 0;
    if (rpc->payload_size > P4_HOSTED_AUX_EVENT_MAX_PAYLOAD ||
        (rpc->payload_size != 0u && !rpc->payload) ||
        !range_is_valid(rpc->payload, rpc->payload_size) ||
        ranges_overlap(rpc, sizeof *rpc, rpc->payload, rpc->payload_size) ||
        ranges_overlap(result, sizeof *result, rpc->payload,
                       rpc->payload_size))
        return 0;
    return 1;
}

static int parse_init(const uint8_t *bytes, size_t size,
                      struct p4_hosted_aux_event *result)
{
    struct reader reader = { bytes, size, 0u };

    while (reader.offset < reader.size) {
        uint32_t field;
        unsigned int wire_type;

        if (!reader_key(&reader, &field, &wire_type))
            return 0;
        if (field == 1u) {
            const uint8_t *value;
            size_t value_size;

            if (wire_type != 2u ||
                !reader_bytes(&reader, &value, &value_size) ||
                value_size > P4_HOSTED_AUX_EVENT_INIT_DATA_SIZE)
                return 0;
            clear_bytes(result->data.init.data,
                        sizeof result->data.init.data);
            if (!copy_bytes(result->data.init.data, value, value_size))
                return 0;
            result->data.init.size = value_size;
        } else if (!reader_skip(&reader, wire_type)) {
            return 0;
        }
    }
    return 1;
}

static int parse_heartbeat(const uint8_t *bytes, size_t size,
                           struct p4_hosted_aux_event *result)
{
    struct reader reader = { bytes, size, 0u };

    while (reader.offset < reader.size) {
        uint32_t field;
        unsigned int wire_type;

        if (!reader_key(&reader, &field, &wire_type))
            return 0;
        if (field == 1u) {
            if (wire_type != 0u ||
                !reader_int32(&reader, &result->data.heartbeat.value))
                return 0;
        } else if (!reader_skip(&reader, wire_type)) {
            return 0;
        }
    }
    return 1;
}

static int parse_ap_event(const uint8_t *bytes, size_t size,
                          uint32_t event_id,
                          struct p4_hosted_aux_event *result)
{
    struct reader reader = { bytes, size, 0u };
    int seen_mac = 0;

    while (reader.offset < reader.size) {
        uint32_t field;
        unsigned int wire_type;

        if (!reader_key(&reader, &field, &wire_type))
            return 0;
        switch (field) {
        case 1u:
            if (wire_type != 0u ||
                !reader_int32(&reader, &result->status))
                return 0;
            break;
        case 2u: {
            const uint8_t *value;
            size_t value_size;

            if (wire_type != 2u ||
                !reader_bytes(&reader, &value, &value_size) ||
                value_size != sizeof result->data.ap.mac ||
                !copy_bytes(result->data.ap.mac, value, value_size))
                return 0;
            seen_mac = 1;
            break;
        }
        case 3u:
            if (wire_type != 0u ||
                !reader_uint32(&reader, &result->data.ap.aid))
                return 0;
            break;
        case 4u: {
            uint64_t value;

            if (wire_type != 0u || !reader_varint(&reader, &value))
                return 0;
            result->data.ap.is_mesh_child = value != 0u;
            break;
        }
        case 5u:
            if (event_id == P4_HOSTED_AUX_EVENT_AP_STA_DISCONNECTED) {
                if (wire_type != 0u ||
                    !reader_uint32(&reader, &result->data.ap.reason))
                    return 0;
            } else if (!reader_skip(&reader, wire_type)) {
                return 0;
            }
            break;
        default:
            if (!reader_skip(&reader, wire_type))
                return 0;
            break;
        }
    }

    return result->status != 0 || seen_mac;
}

static int parse_scan_done(const uint8_t *bytes, size_t size,
                           struct p4_hosted_aux_event *result)
{
    struct reader reader = { bytes, size, 0u };

    while (reader.offset < reader.size) {
        uint32_t field;
        unsigned int wire_type;

        if (!reader_key(&reader, &field, &wire_type))
            return 0;
        switch (field) {
        case 1u:
            if (wire_type != 0u ||
                !reader_uint32(&reader, &result->data.scan.status))
                return 0;
            break;
        case 2u:
            if (wire_type != 0u ||
                !reader_uint32(&reader, &result->data.scan.number))
                return 0;
            break;
        case 3u:
            if (wire_type != 0u ||
                !reader_uint32(&reader, &result->data.scan.scan_id))
                return 0;
            break;
        default:
            if (!reader_skip(&reader, wire_type))
                return 0;
            break;
        }
    }
    return 1;
}

static int parse_scan_event(const uint8_t *bytes, size_t size,
                            struct p4_hosted_aux_event *result)
{
    struct reader reader = { bytes, size, 0u };
    int seen_scan_done = 0;

    while (reader.offset < reader.size) {
        uint32_t field;
        unsigned int wire_type;

        if (!reader_key(&reader, &field, &wire_type))
            return 0;
        switch (field) {
        case 1u:
            if (wire_type != 0u ||
                !reader_int32(&reader, &result->status))
                return 0;
            break;
        case 2u: {
            const uint8_t *value;
            size_t value_size;

            if (wire_type != 2u ||
                !reader_bytes(&reader, &value, &value_size) ||
                !parse_scan_done(value, value_size, result))
                return 0;
            seen_scan_done = 1;
            break;
        }
        default:
            if (!reader_skip(&reader, wire_type))
                return 0;
            break;
        }
    }

    return result->status != 0 || seen_scan_done;
}

int p4_hosted_aux_event_decode(const struct p4_hosted_rpc *rpc,
                               struct p4_hosted_aux_event *result)
{
    struct p4_hosted_aux_event decoded;
    int parsed;

    if (!rpc_input_is_valid(rpc, result) ||
        rpc->msg_type != P4_HOSTED_RPC_TYPE_EVENT || rpc->uid != 0u)
        return 0;

    clear_bytes((uint8_t *)&decoded, sizeof decoded);
    decoded.msg_id = rpc->msg_id;
    switch (rpc->msg_id) {
    case P4_HOSTED_AUX_EVENT_ESP_INIT:
        parsed = parse_init(rpc->payload, rpc->payload_size, &decoded);
        break;
    case P4_HOSTED_AUX_EVENT_HEARTBEAT:
        parsed = parse_heartbeat(rpc->payload, rpc->payload_size, &decoded);
        break;
    case P4_HOSTED_AUX_EVENT_AP_STA_CONNECTED:
    case P4_HOSTED_AUX_EVENT_AP_STA_DISCONNECTED:
        parsed = parse_ap_event(rpc->payload, rpc->payload_size,
                                rpc->msg_id, &decoded);
        break;
    case P4_HOSTED_AUX_EVENT_STA_SCAN_DONE:
        parsed = parse_scan_event(rpc->payload, rpc->payload_size, &decoded);
        break;
    default:
        return 0;
    }
    if (!parsed)
        return 0;

    if (decoded.status != 0)
        clear_bytes((uint8_t *)&decoded.data, sizeof decoded.data);
    copy_result(result, &decoded);
    return 1;
}
