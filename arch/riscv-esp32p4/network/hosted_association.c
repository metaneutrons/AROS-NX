/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#include "hosted_association.h"

#define ASSOCIATION_MAX_FIELD UINT32_C(0x1fffffff)
#define ASSOCIATION_REQ_CONNECT 282u
#define ASSOCIATION_REQ_DISCONNECT 283u
#define ASSOCIATION_REQ_SET_CONFIG 284u
#define ASSOCIATION_EVENT_CONNECTED 775u
#define ASSOCIATION_EVENT_DISCONNECTED 776u
#define ASSOCIATION_SCRATCH_SIZE \
    (4u * P4_HOSTED_ASSOCIATION_MAX_REQUEST_SIZE)

struct writer {
    uint8_t *bytes;
    size_t capacity;
    size_t offset;
};

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

/* Invalid or overflowing ranges are treated as overlapping. */
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

static size_t varint_size(uint64_t value)
{
    size_t size = 1u;

    while (value >= UINT64_C(0x80)) {
        value >>= 7;
        ++size;
    }
    return size;
}

static int writer_varint(struct writer *writer, uint64_t value)
{
    if (!writer || writer->offset > writer->capacity)
        return 0;
    while (value >= UINT64_C(0x80)) {
        if (writer->offset >= writer->capacity)
            return 0;
        writer->bytes[writer->offset++] =
            (uint8_t)((value & UINT64_C(0x7f)) | UINT64_C(0x80));
        value >>= 7;
    }
    if (writer->offset >= writer->capacity)
        return 0;
    writer->bytes[writer->offset++] = (uint8_t)value;
    return 1;
}

static int writer_key(struct writer *writer, uint32_t field,
                      unsigned int wire_type)
{
    uint64_t key;

    if (field == 0u || field > ASSOCIATION_MAX_FIELD || wire_type > 5u)
        return 0;
    key = ((uint64_t)field << 3) | (uint64_t)wire_type;
    return writer_varint(writer, key);
}

static uint64_t int32_wire_value(int32_t value)
{
    return (uint64_t)(int64_t)value;
}

static int writer_int32_field(struct writer *writer, uint32_t field,
                              int32_t value)
{
    if (value == 0)
        return 1;
    return writer_key(writer, field, 0u) &&
           writer_varint(writer, int32_wire_value(value));
}

static int writer_uint32_field(struct writer *writer, uint32_t field,
                               uint32_t value)
{
    if (value == 0u)
        return 1;
    return writer_key(writer, field, 0u) &&
           writer_varint(writer, (uint64_t)value);
}

static int writer_bool_field(struct writer *writer, uint32_t field,
                             unsigned int value)
{
    if (value == 0u)
        return 1;
    return writer_key(writer, field, 0u) && writer_varint(writer, 1u);
}

/* Volatile stores keep credential copies from being optimized away. */
static void wipe_bytes(uint8_t *bytes, size_t size)
{
    volatile uint8_t *destination = bytes;
    size_t index;

    for (index = 0u; index < size; ++index)
        destination[index] = 0u;
}

static int copy_bytes(uint8_t *destination, const uint8_t *source,
                      size_t size)
{
    volatile uint8_t *volatile_destination = destination;
    size_t index;

    if (size != 0u && (!destination || !source))
        return 0;
    for (index = 0u; index < size; ++index)
        volatile_destination[index] = source[index];
    return 1;
}

static int writer_bytes_field(struct writer *writer, uint32_t field,
                              const uint8_t *value, size_t value_size)
{
    if (value_size == 0u)
        return 1;
    if (!value || !writer_key(writer, field, 2u) ||
        !writer_varint(writer, (uint64_t)value_size) ||
        value_size > writer->capacity - writer->offset)
        return 0;
    if (!copy_bytes(writer->bytes + writer->offset, value, value_size))
        return 0;
    writer->offset += value_size;
    return 1;
}

static size_t int32_field_size(uint32_t field, int32_t value)
{
    if (value == 0)
        return 0u;
    return varint_size(((uint64_t)field << 3)) +
           varint_size(int32_wire_value(value));
}

static size_t bool_field_size(uint32_t field, unsigned int value)
{
    return value == 0u ? 0u : varint_size((uint64_t)field << 3) + 1u;
}

static int writer_message_field(struct writer *writer, uint32_t field,
                                const uint8_t *message, size_t message_size)
{
    if (message_size != 0u && !message)
        return 0;
    if (!writer_key(writer, field, 2u) ||
        !writer_varint(writer, (uint64_t)message_size) ||
        message_size > writer->capacity - writer->offset)
        return 0;
    if (!copy_bytes(writer->bytes + writer->offset, message, message_size))
        return 0;
    writer->offset += message_size;
    return 1;
}

static int outputs_are_valid(uint32_t uid, uint8_t *bytes, size_t capacity,
                             size_t *written)
{
    if (uid == 0u || !bytes || !written ||
        !range_is_valid(bytes, capacity) ||
        !range_is_valid(written, sizeof *written) ||
        ranges_overlap(bytes, capacity, written, sizeof *written))
        return 0;
    return 1;
}

static int input_disjoint_from_outputs(const void *input, size_t input_size,
                                       uint8_t *bytes, size_t capacity,
                                       size_t *written)
{
    return !ranges_overlap(input, input_size, bytes, capacity) &&
           !ranges_overlap(input, input_size, written,
                           written ? sizeof *written : 0u);
}

static void copy_to_output(uint8_t *destination, const uint8_t *source,
                           size_t size)
{
    (void)copy_bytes(destination, source, size);
}

int p4_hosted_association_set_config_request(
    uint32_t uid, const struct p4_hosted_sta_config *config,
    uint8_t *bytes, size_t capacity, size_t *written)
{
    uint8_t scratch[ASSOCIATION_SCRATCH_SIZE];
    uint8_t *sta_bytes = scratch;
    uint8_t *wifi_config_bytes = scratch +
        P4_HOSTED_ASSOCIATION_MAX_REQUEST_SIZE;
    uint8_t *request_body = scratch +
        2u * P4_HOSTED_ASSOCIATION_MAX_REQUEST_SIZE;
    uint8_t *rpc_bytes = scratch +
        3u * P4_HOSTED_ASSOCIATION_MAX_REQUEST_SIZE;
    struct writer sta, wifi_config, request_writer;
    struct p4_hosted_rpc request;
    size_t threshold_size, pmf_size, rpc_size = 0u;
    int ok = 0;

    if (!outputs_are_valid(uid, bytes, capacity, written) || !config ||
        !range_is_valid(config, sizeof *config) ||
        !input_disjoint_from_outputs(config, sizeof *config, bytes,
                                     capacity, written))
        return 0;

    if (config->ssid_length > P4_HOSTED_ASSOCIATION_SSID_SIZE ||
        config->password_length > P4_HOSTED_ASSOCIATION_PASSWORD_SIZE ||
        config->sae_h2e_identifier_length >
            P4_HOSTED_ASSOCIATION_SAE_ID_SIZE ||
        config->bssid_set > 1u || config->pmf_capable > 1u ||
        config->pmf_required > 1u ||
        (config->bssid_set != 0u &&
         (config->bssid_length != P4_HOSTED_ASSOCIATION_BSSID_SIZE ||
          !config->bssid)) ||
        (config->bssid_set == 0u &&
         (config->bssid_length != 0u || config->bssid != NULL)))
        return 0;

    if (!range_is_valid(config->ssid, config->ssid_length) ||
        !range_is_valid(config->password, config->password_length) ||
        !range_is_valid(config->bssid, config->bssid_length) ||
        !range_is_valid(config->sae_h2e_identifier,
                        config->sae_h2e_identifier_length) ||
        !input_disjoint_from_outputs(config->ssid, config->ssid_length,
                                     bytes, capacity, written) ||
        !input_disjoint_from_outputs(config->password,
                                     config->password_length,
                                     bytes, capacity, written) ||
        !input_disjoint_from_outputs(config->bssid, config->bssid_length,
                                     bytes, capacity, written) ||
        !input_disjoint_from_outputs(config->sae_h2e_identifier,
                                     config->sae_h2e_identifier_length,
                                     bytes, capacity, written))
        return 0;

    /* Every path after this point clears every transient secret copy. */
    {
        volatile uint8_t *clear = scratch;
        size_t index;
        for (index = 0u; index < sizeof scratch; ++index)
            clear[index] = 0u;
    }

    threshold_size = int32_field_size(1u, config->threshold_rssi) +
                     int32_field_size(2u, config->threshold_authmode);
    pmf_size = bool_field_size(1u, config->pmf_capable) +
               bool_field_size(2u, config->pmf_required);
    if (threshold_size > P4_HOSTED_ASSOCIATION_MAX_BODY_SIZE ||
        pmf_size > P4_HOSTED_ASSOCIATION_MAX_BODY_SIZE)
        goto cleanup;

    sta.bytes = sta_bytes;
    sta.capacity = P4_HOSTED_ASSOCIATION_MAX_BODY_SIZE;
    sta.offset = 0u;
    if (!writer_bytes_field(&sta, 1u, config->ssid, config->ssid_length) ||
        !writer_bytes_field(&sta, 2u, config->password,
                            config->password_length) ||
        !writer_int32_field(&sta, 3u, config->scan_method) ||
        !writer_bool_field(&sta, 4u, config->bssid_set) ||
        !writer_bytes_field(&sta, 5u, config->bssid,
                            config->bssid_length) ||
        !writer_uint32_field(&sta, 6u, config->channel) ||
        !writer_uint32_field(&sta, 7u, config->listen_interval) ||
        !writer_int32_field(&sta, 8u, config->sort_method) ||
        !writer_key(&sta, 9u, 2u) ||
        !writer_varint(&sta, (uint64_t)threshold_size) ||
        !writer_int32_field(&sta, 1u, config->threshold_rssi) ||
        !writer_int32_field(&sta, 2u, config->threshold_authmode) ||
        !writer_key(&sta, 10u, 2u) ||
        !writer_varint(&sta, (uint64_t)pmf_size) ||
        !writer_bool_field(&sta, 1u, config->pmf_capable) ||
        !writer_bool_field(&sta, 2u, config->pmf_required) ||
        !writer_uint32_field(&sta, 11u, config->bitmask) ||
        !writer_int32_field(&sta, 12u, config->sae_pwe_h2e) ||
        !writer_uint32_field(&sta, 13u, config->failure_retry_cnt) ||
        !writer_uint32_field(&sta, 14u, config->he_bitmask) ||
        !writer_bytes_field(&sta, 15u, config->sae_h2e_identifier,
                            config->sae_h2e_identifier_length))
        goto cleanup;

    wifi_config.bytes = wifi_config_bytes;
    wifi_config.capacity = P4_HOSTED_ASSOCIATION_MAX_BODY_SIZE;
    wifi_config.offset = 0u;
    if (!writer_message_field(&wifi_config, 2u, sta_bytes, sta.offset))
        goto cleanup;

    request_writer.bytes = request_body;
    request_writer.capacity = P4_HOSTED_ASSOCIATION_MAX_BODY_SIZE;
    request_writer.offset = 0u;
    if (!writer_message_field(&request_writer, 2u, wifi_config_bytes,
                              wifi_config.offset))
        goto cleanup;

    request.msg_type = P4_HOSTED_RPC_TYPE_REQUEST;
    request.msg_id = ASSOCIATION_REQ_SET_CONFIG;
    request.uid = uid;
    request.payload = request_body;
    request.payload_size = request_writer.offset;
    if (!p4_hosted_rpc_encode(rpc_bytes,
                              P4_HOSTED_ASSOCIATION_MAX_REQUEST_SIZE,
                              &request, &rpc_size) ||
        rpc_size > P4_HOSTED_ASSOCIATION_MAX_REQUEST_SIZE ||
        rpc_size > capacity)
        goto cleanup;

    copy_to_output(bytes, rpc_bytes, rpc_size);
    *written = rpc_size;
    ok = 1;

cleanup:
    wipe_bytes(scratch, sizeof scratch);
    return ok;
}

static int encode_empty_request(uint32_t request_id, uint32_t uid,
                                uint8_t *bytes, size_t capacity,
                                size_t *written)
{
    uint8_t scratch[P4_HOSTED_ASSOCIATION_MAX_REQUEST_SIZE];
    struct p4_hosted_rpc request;
    size_t encoded_size = 0u;
    int ok = 0;

    if ((request_id != ASSOCIATION_REQ_CONNECT &&
         request_id != ASSOCIATION_REQ_DISCONNECT) ||
        !outputs_are_valid(uid, bytes, capacity, written))
        return 0;

    request.msg_type = P4_HOSTED_RPC_TYPE_REQUEST;
    request.msg_id = request_id;
    request.uid = uid;
    request.payload = NULL;
    request.payload_size = 0u;
    if (p4_hosted_rpc_encode(scratch, sizeof scratch, &request,
                             &encoded_size) &&
        encoded_size <= capacity &&
        encoded_size <= P4_HOSTED_ASSOCIATION_MAX_REQUEST_SIZE) {
        copy_to_output(bytes, scratch, encoded_size);
        *written = encoded_size;
        ok = 1;
    }
    wipe_bytes(scratch, sizeof scratch);
    return ok;
}

int p4_hosted_association_connect_request(uint32_t uid, uint8_t *bytes,
                                         size_t capacity, size_t *written)
{
    return encode_empty_request(ASSOCIATION_REQ_CONNECT, uid, bytes,
                                capacity, written);
}

int p4_hosted_association_disconnect_request(uint32_t uid, uint8_t *bytes,
                                            size_t capacity, size_t *written)
{
    return encode_empty_request(ASSOCIATION_REQ_DISCONNECT, uid, bytes,
                                capacity, written);
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
        (key >> 3) == 0u || (key >> 3) > ASSOCIATION_MAX_FIELD)
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

    return reader_varint(reader, &parsed) &&
           decode_int32_value(parsed, value);
}

static int reader_bytes(struct reader *reader, const uint8_t **bytes,
                        size_t *length)
{
    uint64_t parsed_length;

    if (!reader_varint(reader, &parsed_length) ||
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
        /* Groups and reserved wire types are unsupported. */
        return 0;
    }
}

static int rpc_input_is_valid(const struct p4_hosted_rpc *rpc,
                              const void *result, size_t result_size)
{
    if (!rpc || !result ||
        !range_is_valid(rpc, sizeof *rpc) ||
        !range_is_valid(result, result_size) ||
        ranges_overlap(rpc, sizeof *rpc, result, result_size))
        return 0;
    if (rpc->payload_size > P4_HOSTED_ASSOCIATION_MAX_BODY_SIZE ||
        (rpc->payload_size != 0u && !rpc->payload) ||
        !range_is_valid(rpc->payload, rpc->payload_size) ||
        ranges_overlap(rpc, sizeof *rpc, rpc->payload, rpc->payload_size) ||
        ranges_overlap(result, result_size, rpc->payload, rpc->payload_size))
        return 0;
    return 1;
}

static int supported_association_request(uint32_t request_id)
{
    return request_id == ASSOCIATION_REQ_CONNECT ||
           request_id == ASSOCIATION_REQ_DISCONNECT ||
           request_id == ASSOCIATION_REQ_SET_CONFIG;
}

int p4_hosted_association_status_response(const struct p4_hosted_rpc *rpc,
                                          uint32_t expected_request_id,
                                          uint32_t expected_uid,
                                          int32_t *status)
{
    struct reader reader;
    int32_t decoded_status = 0;
    int seen_status = 0;

    if (!status || !range_is_valid(status, sizeof *status) ||
        !rpc_input_is_valid(rpc, status, sizeof *status) ||
        expected_uid == 0u ||
        !supported_association_request(expected_request_id) ||
        rpc->msg_type != P4_HOSTED_RPC_TYPE_RESPONSE ||
        rpc->msg_id != expected_request_id + 256u || rpc->uid != expected_uid)
        return 0;

    reader.bytes = rpc->payload;
    reader.size = rpc->payload_size;
    reader.offset = 0u;
    while (reader.offset < reader.size) {
        uint32_t field;
        unsigned int wire_type;

        if (!reader_key(&reader, &field, &wire_type))
            return 0;
        if (field == 1u) {
            if (wire_type != 0u || seen_status ||
                !reader_int32(&reader, &decoded_status))
                return 0;
            seen_status = 1;
        } else if (!reader_skip(&reader, wire_type)) {
            return 0;
        }
    }

    *status = decoded_status;
    return 1;
}

static int parse_event_wrapper(const struct p4_hosted_rpc *rpc,
                               uint32_t event_id, int32_t *status,
                               const uint8_t **event_data,
                               size_t *event_data_size)
{
    struct reader reader;
    int seen_status = 0;
    int seen_data = 0;

    if (!rpc || !status || !event_data || !event_data_size ||
        rpc->msg_type != P4_HOSTED_RPC_TYPE_EVENT || rpc->msg_id != event_id ||
        rpc->uid != 0u)
        return 0;
    reader.bytes = rpc->payload;
    reader.size = rpc->payload_size;
    reader.offset = 0u;
    *status = 0;
    *event_data = NULL;
    *event_data_size = 0u;

    while (reader.offset < reader.size) {
        uint32_t field;
        unsigned int wire_type;

        if (!reader_key(&reader, &field, &wire_type))
            return 0;
        if (field == 1u) {
            if (wire_type != 0u || seen_status ||
                !reader_int32(&reader, status))
                return 0;
            seen_status = 1;
        } else if (field == 2u) {
            if (wire_type != 2u || seen_data ||
                !reader_bytes(&reader, event_data, event_data_size))
                return 0;
            seen_data = 1;
        } else if (!reader_skip(&reader, wire_type)) {
            return 0;
        }
    }

    return *status != 0 || seen_data;
}

static int parse_connected_body(const uint8_t *bytes, size_t size,
                                struct p4_hosted_sta_connected_event *result)
{
    struct reader reader = { bytes, size, 0u };
    int seen_ssid = 0, seen_ssid_length = 0, seen_bssid = 0;
    int seen_channel = 0, seen_authmode = 0, seen_aid = 0;

    while (reader.offset < reader.size) {
        uint32_t field;
        unsigned int wire_type;

        if (!reader_key(&reader, &field, &wire_type))
            return 0;
        switch (field) {
        case 1u: {
            const uint8_t *value;
            size_t value_size;
            if (wire_type != 2u || seen_ssid ||
                !reader_bytes(&reader, &value, &value_size) ||
                value_size != P4_HOSTED_ASSOCIATION_SSID_SIZE ||
                !copy_bytes(result->ssid, value, value_size))
                return 0;
            seen_ssid = 1;
            break;
        }
        case 2u:
            if (wire_type != 0u || seen_ssid_length ||
                !reader_uint32(&reader, &result->ssid_length) ||
                result->ssid_length > P4_HOSTED_ASSOCIATION_SSID_SIZE)
                return 0;
            seen_ssid_length = 1;
            break;
        case 3u: {
            const uint8_t *value;
            size_t value_size;
            if (wire_type != 2u || seen_bssid ||
                !reader_bytes(&reader, &value, &value_size) ||
                value_size != P4_HOSTED_ASSOCIATION_BSSID_SIZE ||
                !copy_bytes(result->bssid, value, value_size))
                return 0;
            seen_bssid = 1;
            break;
        }
        case 4u:
            if (wire_type != 0u || seen_channel ||
                !reader_uint32(&reader, &result->channel))
                return 0;
            seen_channel = 1;
            break;
        case 5u:
            if (wire_type != 0u || seen_authmode ||
                !reader_int32(&reader, &result->authmode))
                return 0;
            seen_authmode = 1;
            break;
        case 6u:
            if (wire_type != 0u || seen_aid ||
                !reader_int32(&reader, &result->aid))
                return 0;
            seen_aid = 1;
            break;
        default:
            if (!reader_skip(&reader, wire_type))
                return 0;
            break;
        }
    }

    return seen_ssid && seen_bssid;
}

static int parse_disconnected_body(
    const uint8_t *bytes, size_t size,
    struct p4_hosted_sta_disconnected_event *result)
{
    struct reader reader = { bytes, size, 0u };
    int seen_ssid = 0, seen_ssid_length = 0, seen_bssid = 0;
    int seen_reason = 0, seen_rssi = 0;

    while (reader.offset < reader.size) {
        uint32_t field;
        unsigned int wire_type;

        if (!reader_key(&reader, &field, &wire_type))
            return 0;
        switch (field) {
        case 1u: {
            const uint8_t *value;
            size_t value_size;
            if (wire_type != 2u || seen_ssid ||
                !reader_bytes(&reader, &value, &value_size) ||
                value_size != P4_HOSTED_ASSOCIATION_SSID_SIZE ||
                !copy_bytes(result->ssid, value, value_size))
                return 0;
            seen_ssid = 1;
            break;
        }
        case 2u:
            if (wire_type != 0u || seen_ssid_length ||
                !reader_uint32(&reader, &result->ssid_length) ||
                result->ssid_length > P4_HOSTED_ASSOCIATION_SSID_SIZE)
                return 0;
            seen_ssid_length = 1;
            break;
        case 3u: {
            const uint8_t *value;
            size_t value_size;
            if (wire_type != 2u || seen_bssid ||
                !reader_bytes(&reader, &value, &value_size) ||
                value_size != P4_HOSTED_ASSOCIATION_BSSID_SIZE ||
                !copy_bytes(result->bssid, value, value_size))
                return 0;
            seen_bssid = 1;
            break;
        }
        case 4u:
            if (wire_type != 0u || seen_reason ||
                !reader_uint32(&reader, &result->reason))
                return 0;
            seen_reason = 1;
            break;
        case 5u:
            if (wire_type != 0u || seen_rssi ||
                !reader_int32(&reader, &result->rssi))
                return 0;
            seen_rssi = 1;
            break;
        default:
            if (!reader_skip(&reader, wire_type))
                return 0;
            break;
        }
    }

    return seen_ssid && seen_bssid;
}

int p4_hosted_association_connected_event(
    const struct p4_hosted_rpc *rpc,
    struct p4_hosted_sta_connected_event *result)
{
    struct p4_hosted_sta_connected_event decoded = { 0 };
    const uint8_t *event_data;
    size_t event_data_size;

    if (!rpc_input_is_valid(rpc, result, sizeof *result))
        return 0;
    if (!parse_event_wrapper(rpc, ASSOCIATION_EVENT_CONNECTED,
                             &decoded.status, &event_data,
                             &event_data_size))
        return 0;
    if (event_data &&
        !parse_connected_body(event_data, event_data_size, &decoded))
        return 0;
    if (decoded.status != 0) {
        int32_t status = decoded.status;
        decoded = (struct p4_hosted_sta_connected_event){ 0 };
        decoded.status = status;
    }
    *result = decoded;
    return 1;
}

int p4_hosted_association_disconnected_event(
    const struct p4_hosted_rpc *rpc,
    struct p4_hosted_sta_disconnected_event *result)
{
    struct p4_hosted_sta_disconnected_event decoded = { 0 };
    const uint8_t *event_data;
    size_t event_data_size;

    if (!rpc_input_is_valid(rpc, result, sizeof *result))
        return 0;
    if (!parse_event_wrapper(rpc, ASSOCIATION_EVENT_DISCONNECTED,
                             &decoded.status, &event_data,
                             &event_data_size))
        return 0;
    if (event_data &&
        !parse_disconnected_body(event_data, event_data_size, &decoded))
        return 0;
    if (decoded.status != 0) {
        int32_t status = decoded.status;
        decoded = (struct p4_hosted_sta_disconnected_event){ 0 };
        decoded.status = status;
    }
    *result = decoded;
    return 1;
}
