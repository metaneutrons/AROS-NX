/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#include "hosted_scan.h"

#define SCAN_MAX_FIELD UINT32_C(0x1fffffff)
#define SCAN_REQ_START 286u
#define SCAN_REQ_STOP 287u
#define SCAN_REQ_NUM 288u
#define SCAN_REQ_RECORDS 289u
#define SCAN_REQ_STA_AP_INFO 294u

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

static void copy_bytes(uint8_t *destination, const uint8_t *source,
                       size_t size)
{
    volatile uint8_t *volatile_destination = destination;
    size_t i;

    for (i = 0; i < size; ++i)
        volatile_destination[i] = source[i];
}

static void clear_bytes(uint8_t *destination, size_t size)
{
    volatile uint8_t *volatile_destination = destination;
    size_t i;

    for (i = 0; i < size; ++i)
        volatile_destination[i] = 0u;
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

static int encode_request(uint32_t request_id, uint32_t uid,
                          const uint8_t *payload, size_t payload_size,
                          uint8_t *bytes, size_t capacity, size_t *written)
{
    /* All supported request bodies fit in four bytes. Even with a
     * maximal UID their envelope fits comfortably here; a response-sized
     * scratch buffer would waste4KiB of the small runtime task stack. */
    uint8_t envelope[64];
    struct p4_hosted_rpc request;
    size_t encoded_size = 0u;

    if (!bytes || !written || uid == 0u ||
        !range_is_valid(bytes, capacity) ||
        !range_is_valid(written, sizeof *written) ||
        ranges_overlap(bytes, capacity, written, sizeof *written) ||
        payload_size > 4u ||
        (payload_size != 0u && (!payload ||
         !range_is_valid(payload, payload_size) ||
         ranges_overlap(bytes, capacity, payload, payload_size) ||
         ranges_overlap(written, sizeof *written, payload, payload_size))))
        return 0;

    request.msg_type = P4_HOSTED_RPC_TYPE_REQUEST;
    request.msg_id = request_id;
    request.uid = uid;
    request.payload = payload;
    request.payload_size = payload_size;
    if (!p4_hosted_rpc_encode(envelope, sizeof envelope, &request,
                              &encoded_size) || encoded_size > capacity)
        return 0;

    copy_bytes(bytes, envelope, encoded_size);
    *written = encoded_size;
    return 1;
}

int p4_hosted_scan_start_request(uint32_t uid, unsigned block,
                                 uint8_t *bytes, size_t capacity,
                                 size_t *written)
{
    /* ScanStart.config is the default NULL config. The block default is
     * omitted when false; config_set is explicitly field 3, value zero. */
    static const uint8_t body_nonblocking[] = { 0x18u, 0x00u };
    static const uint8_t body_blocking[] = { 0x10u, 0x01u, 0x18u, 0x00u };

    if (block > 1u)
        return 0;
    return encode_request(SCAN_REQ_START, uid,
                          block ? body_blocking : body_nonblocking,
                          block ? sizeof body_blocking :
                                  sizeof body_nonblocking,
                          bytes, capacity, written);
}

int p4_hosted_scan_stop_request(uint32_t uid, uint8_t *bytes,
                                size_t capacity, size_t *written)
{
    return encode_request(SCAN_REQ_STOP, uid, NULL, 0u,
                          bytes, capacity, written);
}

int p4_hosted_scan_num_request(uint32_t uid, uint8_t *bytes,
                               size_t capacity, size_t *written)
{
    return encode_request(SCAN_REQ_NUM, uid, NULL, 0u,
                          bytes, capacity, written);
}

int p4_hosted_scan_records_request(uint32_t uid, uint16_t number,
                                   uint8_t *bytes, size_t capacity,
                                   size_t *written)
{
    uint8_t body[2];
    size_t offset = 0u;

    if (number == 0u || number > P4_HOSTED_SCAN_MAX_RECORDS)
        return 0;
    body[offset++] = 0x08u;
    write_varint(body, &offset, number);
    return encode_request(SCAN_REQ_RECORDS, uid, body, offset,
                          bytes, capacity, written);
}

int p4_hosted_sta_ap_info_request(uint32_t uid, uint8_t *bytes,
                                  size_t capacity, size_t *written)
{
    return encode_request(SCAN_REQ_STA_AP_INFO, uid, NULL, 0u,
                          bytes, capacity, written);
}

static int read_varint(const uint8_t *bytes, size_t size, size_t *offset,
                       uint64_t *value)
{
    uint64_t parsed = 0u;
    size_t cursor = *offset;
    unsigned int i;

    for (i = 0; i < 10u; ++i) {
        uint8_t byte;

        if (cursor >= size)
            return 0;
        byte = bytes[cursor++];
        if (i == 9u && (byte & 0xfeu) != 0u)
            return 0;
        parsed |= (uint64_t)(byte & 0x7fu) << (i * 7u);
        if ((byte & 0x80u) == 0u) {
            if (i != 0u && (byte & 0x7fu) == 0u)
                return 0;
            *offset = cursor;
            *value = parsed;
            return 1;
        }
    }
    return 0;
}

static int next_field(const uint8_t *bytes, size_t size, size_t *offset,
                      uint32_t *field, unsigned int *wire_type)
{
    uint64_t key;

    if (!read_varint(bytes, size, offset, &key) || key == 0u ||
        (key >> 3) == 0u || (key >> 3) > SCAN_MAX_FIELD)
        return 0;
    *field = (uint32_t)(key >> 3);
    *wire_type = (unsigned int)(key & 7u);
    if (*wire_type != 0u && *wire_type != 1u &&
        *wire_type != 2u && *wire_type != 5u)
        return 0;
    return 1;
}

static int read_length_delimited(const uint8_t *bytes, size_t size,
                                 size_t *offset, const uint8_t **value,
                                 size_t *value_size)
{
    uint64_t length;

    if (!read_varint(bytes, size, offset, &length) || *offset > size ||
        length > (uint64_t)(size - *offset))
        return 0;
    *value = bytes + *offset;
    *value_size = (size_t)length;
    *offset += (size_t)length;
    return 1;
}

static int read_uint32(const uint8_t *bytes, size_t size, size_t *offset,
                       uint32_t *value)
{
    uint64_t parsed;

    if (!read_varint(bytes, size, offset, &parsed) || parsed > UINT32_MAX)
        return 0;
    *value = (uint32_t)parsed;
    return 1;
}

static int decode_int32(uint64_t value, int32_t *result)
{
    if (value <= (uint64_t)INT32_MAX) {
        *result = (int32_t)value;
        return 1;
    }

    /* int32 negatives use protobuf's 64-bit sign-extended varint form. */
    if (value >= UINT64_C(0xffffffff80000000)) {
        uint32_t low = (uint32_t)value;
        uint32_t distance = UINT32_MAX - low;
        *result = -1 - (int32_t)distance;
        return 1;
    }
    return 0;
}

static int read_int32(const uint8_t *bytes, size_t size, size_t *offset,
                      int32_t *value)
{
    uint64_t parsed;

    return read_varint(bytes, size, offset, &parsed) &&
           decode_int32(parsed, value);
}

static int skip_unknown(const uint8_t *bytes, size_t size, size_t *offset,
                        unsigned int wire_type)
{
    const uint8_t *ignored_bytes;
    size_t ignored_size;
    uint64_t ignored_value;

    switch (wire_type) {
    case 0u:
        return read_varint(bytes, size, offset, &ignored_value);
    case 1u:
        if (*offset > size || size - *offset < 8u)
            return 0;
        *offset += 8u;
        return 1;
    case 2u:
        return read_length_delimited(bytes, size, offset,
                                     &ignored_bytes, &ignored_size);
    case 5u:
        if (*offset > size || size - *offset < 4u)
            return 0;
        *offset += 4u;
        return 1;
    default:
        return 0;
    }
}

static int parse_status_body(const uint8_t *bytes, size_t size,
                             int32_t *status)
{
    size_t offset = 0u;
    int32_t parsed_status = 0;
    int seen_status = 0;

    while (offset < size) {
        uint32_t field;
        unsigned int wire_type;

        if (!next_field(bytes, size, &offset, &field, &wire_type))
            return 0;
        if (field == 1u) {
            if (wire_type != 0u || seen_status ||
                !read_int32(bytes, size, &offset, &parsed_status))
                return 0;
            seen_status = 1;
        } else if (!skip_unknown(bytes, size, &offset, wire_type)) {
            return 0;
        }
    }
    *status = parsed_status;
    return 1;
}

static int response_input_valid(const struct p4_hosted_rpc *rpc,
                                uint32_t expected_uid,
                                void *output, size_t output_size)
{
    if (!rpc || expected_uid == 0u || !output ||
        !range_is_valid(rpc, sizeof *rpc) ||
        !range_is_valid(output, output_size) ||
        ranges_overlap(rpc, sizeof *rpc, output, output_size))
        return 0;
    if (rpc->payload_size > P4_HOSTED_SCAN_MAX_BODY_SIZE ||
        (rpc->payload_size != 0u &&
         (!rpc->payload ||
          !range_is_valid(rpc->payload, rpc->payload_size))) ||
        ranges_overlap(rpc, sizeof *rpc, rpc->payload, rpc->payload_size) ||
        ranges_overlap(output, output_size,
                       rpc->payload, rpc->payload_size))
        return 0;
    return 1;
}

static int response_matches(const struct p4_hosted_rpc *rpc,
                            uint32_t request_id, uint32_t expected_uid)
{
    return rpc->msg_type == P4_HOSTED_RPC_TYPE_RESPONSE &&
           rpc->msg_id == request_id + 256u && rpc->uid == expected_uid;
}

int p4_hosted_scan_status_response(const struct p4_hosted_rpc *rpc,
                                   uint32_t expected_request_id,
                                   uint32_t expected_uid, int32_t *status)
{
    int32_t decoded_status = 0;

    if (expected_request_id != SCAN_REQ_START &&
        expected_request_id != SCAN_REQ_STOP)
        return 0;
    if (!response_input_valid(rpc, expected_uid, status,
                              status ? sizeof *status : 0u) ||
        !response_matches(rpc, expected_request_id, expected_uid) ||
        !parse_status_body(rpc->payload, rpc->payload_size,
                           &decoded_status))
        return 0;
    *status = decoded_status;
    return 1;
}

static int parse_num_body(const uint8_t *bytes, size_t size,
                          struct p4_hosted_scan_count *result)
{
    size_t offset = 0u;
    int32_t status = 0;
    int32_t number = 0;
    int seen_status = 0;
    int seen_number = 0;

    while (offset < size) {
        uint32_t field;
        unsigned int wire_type;

        if (!next_field(bytes, size, &offset, &field, &wire_type))
            return 0;
        if (field == 1u) {
            if (wire_type != 0u || seen_status ||
                !read_int32(bytes, size, &offset, &status))
                return 0;
            seen_status = 1;
        } else if (field == 2u) {
            if (wire_type != 0u || seen_number ||
                !read_int32(bytes, size, &offset, &number))
                return 0;
            seen_number = 1;
        } else if (!skip_unknown(bytes, size, &offset, wire_type)) {
            return 0;
        }
    }

    if (number < 0 || (uint32_t)number > UINT16_MAX)
        return 0;
    result->status = status;
    result->number = (uint16_t)number;
    return 1;
}

int p4_hosted_scan_num_response(const struct p4_hosted_rpc *rpc,
                                uint32_t expected_uid,
                                struct p4_hosted_scan_count *result)
{
    struct p4_hosted_scan_count decoded;

    if (!response_input_valid(rpc, expected_uid, result,
                              result ? sizeof *result : 0u))
        return 0;
    clear_bytes((uint8_t *)&decoded, sizeof decoded);
    if (!response_matches(rpc, SCAN_REQ_NUM, expected_uid) ||
        !parse_num_body(rpc->payload, rpc->payload_size, &decoded))
        return 0;
    copy_bytes((uint8_t *)result, (const uint8_t *)&decoded, sizeof decoded);
    return 1;
}

static int parse_country(const uint8_t *bytes, size_t size,
                         struct p4_hosted_scan_record *record)
{
    size_t offset = 0u;
    unsigned int seen = 0u;

    while (offset < size) {
        uint32_t field;
        unsigned int wire_type;
        unsigned int bit;

        if (!next_field(bytes, size, &offset, &field, &wire_type))
            return 0;
        if (field >= 1u && field <= 5u) {
            bit = 1u << (field - 1u);
            if ((seen & bit) != 0u)
                return 0;
            seen |= bit;
            if (field == 1u) {
                const uint8_t *cc;
                size_t cc_size;

                if (wire_type != 2u ||
                    !read_length_delimited(bytes, size, &offset,
                                           &cc, &cc_size) ||
                    cc_size > P4_HOSTED_SCAN_COUNTRY_CC_MAX_SIZE)
                    return 0;
                record->country_cc_size = cc_size;
                copy_bytes(record->country_cc, cc, cc_size);
            } else if (field == 2u) {
                if (wire_type != 0u ||
                    !read_uint32(bytes, size, &offset,
                                 &record->country_schan))
                    return 0;
            } else if (field == 3u) {
                if (wire_type != 0u ||
                    !read_uint32(bytes, size, &offset,
                                 &record->country_nchan))
                    return 0;
            } else if (field == 4u) {
                if (wire_type != 0u ||
                    !read_int32(bytes, size, &offset,
                                &record->country_max_tx_power))
                    return 0;
            } else {
                if (wire_type != 0u ||
                    !read_int32(bytes, size, &offset,
                                &record->country_policy))
                    return 0;
            }
        } else if (!skip_unknown(bytes, size, &offset, wire_type)) {
            return 0;
        }
    }
    return 1;
}

static int parse_he_info(const uint8_t *bytes, size_t size,
                         struct p4_hosted_scan_record *record)
{
    size_t offset = 0u;
    unsigned int seen = 0u;

    while (offset < size) {
        uint32_t field;
        unsigned int wire_type;
        unsigned int bit;

        if (!next_field(bytes, size, &offset, &field, &wire_type))
            return 0;
        if (field == 1u || field == 2u) {
            bit = 1u << (field - 1u);
            if ((seen & bit) != 0u || wire_type != 0u)
                return 0;
            seen |= bit;
            if (field == 1u) {
                if (!read_uint32(bytes, size, &offset,
                                 &record->he_bitmask))
                    return 0;
            } else if (!read_uint32(bytes, size, &offset,
                                    &record->he_bssid_index)) {
                return 0;
            }
        } else if (!skip_unknown(bytes, size, &offset, wire_type)) {
            return 0;
        }
    }
    return 1;
}

static int parse_record(const uint8_t *bytes, size_t size,
                        struct p4_hosted_scan_record *record)
{
    size_t offset = 0u;
    unsigned int seen = 0u;

    while (offset < size) {
        uint32_t field;
        unsigned int wire_type;
        unsigned int bit;

        if (!next_field(bytes, size, &offset, &field, &wire_type))
            return 0;
        if (field >= 1u && field <= 12u) {
            const uint8_t *submessage;
            size_t submessage_size;

            bit = 1u << (field - 1u);
            if ((seen & bit) != 0u)
                return 0;
            seen |= bit;
            if (field == 1u || field == 2u) {
                const uint8_t *value;
                size_t value_size;

                if (wire_type != 2u ||
                    !read_length_delimited(bytes, size, &offset,
                                           &value, &value_size))
                    return 0;
                if (field == 1u) {
                    if (value_size != P4_HOSTED_SCAN_BSSID_SIZE)
                        return 0;
                    copy_bytes(record->bssid, value, value_size);
                } else {
                    if (value_size > P4_HOSTED_SCAN_SSID_MAX_SIZE)
                        return 0;
                    /* Pinned slave RPC_RESP_COPY_STR includes the C-string
                     * terminator in its byte-field length for short SSIDs.
                     * Normalize exactly that last byte, not embedded NULs or
                     * arbitrary padding. SANA still rejects binary identities
                     * that cannot be represented by its C-string SSID tag. */
                    if (value_size && value[value_size - 1u] == 0u)
                        --value_size;
                    record->ssid_size = value_size;
                    copy_bytes(record->ssid, value, value_size);
                }
            } else if (field == 3u || field == 10u) {
                uint32_t *target = field == 3u ? &record->primary :
                                                &record->bitmask;
                if (wire_type != 0u ||
                    !read_uint32(bytes, size, &offset, target))
                    return 0;
            } else if (field >= 4u && field <= 9u) {
                int32_t *target;
                switch (field) {
                case 4u: target = &record->second; break;
                case 5u: target = &record->rssi; break;
                case 6u: target = &record->authmode; break;
                case 7u: target = &record->pairwise_cipher; break;
                case 8u: target = &record->group_cipher; break;
                default: target = &record->ant; break;
                }
                if (wire_type != 0u ||
                    !read_int32(bytes, size, &offset, target))
                    return 0;
            } else {
                if (wire_type != 2u ||
                    !read_length_delimited(bytes, size, &offset,
                                           &submessage,
                                           &submessage_size))
                    return 0;
                if (field == 11u) {
                    if (!parse_country(submessage, submessage_size, record))
                        return 0;
                } else if (!parse_he_info(submessage, submessage_size,
                                          record)) {
                    return 0;
                }
            }
        } else if (!skip_unknown(bytes, size, &offset, wire_type)) {
            return 0;
        }
    }
    return (seen & 1u) != 0u;
}

static int parse_sta_ap_info_body(const uint8_t *bytes, size_t size,
                                  struct p4_hosted_sta_ap_info *result)
{
    size_t offset = 0u;
    int seen_status = 0;
    int seen_ap = 0;

    while (offset < size) {
        uint32_t field;
        unsigned int wire_type;

        if (!next_field(bytes, size, &offset, &field, &wire_type))
            return 0;
        if (field == 1u) {
            if (wire_type != 0u || seen_status ||
                !read_int32(bytes, size, &offset, &result->status))
                return 0;
            seen_status = 1;
        } else if (field == 2u) {
            const uint8_t *record_bytes;
            size_t record_size;

            if (wire_type != 2u || seen_ap ||
                !read_length_delimited(bytes, size, &offset,
                                       &record_bytes, &record_size) ||
                !parse_record(record_bytes, record_size, &result->ap))
                return 0;
            seen_ap = 1;
        } else if (!skip_unknown(bytes, size, &offset, wire_type)) {
            return 0;
        }
    }

    if (result->status == 0 && !seen_ap)
        return 0;
    if (result->status != 0)
        clear_bytes((uint8_t *)&result->ap, sizeof result->ap);
    return 1;
}

static int parse_records_body(const uint8_t *bytes, size_t size,
                              uint16_t requested_capacity,
                              struct p4_hosted_scan_records *result)
{
    size_t offset = 0u;
    int32_t status = 0;
    int32_t number = 0;
    size_t record_count = 0u;
    int seen_status = 0;
    int seen_number = 0;

    /* This result is caller-private scratch until the full body is accepted. */
    clear_bytes((uint8_t *)result, sizeof *result);

    while (offset < size) {
        uint32_t field;
        unsigned int wire_type;

        if (!next_field(bytes, size, &offset, &field, &wire_type))
            return 0;
        if (field == 1u) {
            if (wire_type != 0u || seen_status ||
                !read_int32(bytes, size, &offset, &status))
                return 0;
            seen_status = 1;
        } else if (field == 2u) {
            if (wire_type != 0u || seen_number ||
                !read_int32(bytes, size, &offset, &number))
                return 0;
            seen_number = 1;
        } else if (field == 3u) {
            const uint8_t *record_bytes;
            size_t record_size;

            if (wire_type != 2u || record_count >= requested_capacity ||
                record_count >= P4_HOSTED_SCAN_MAX_RECORDS ||
                !read_length_delimited(bytes, size, &offset,
                                       &record_bytes, &record_size) ||
                !parse_record(record_bytes, record_size,
                              &result->records[record_count]))
                return 0;
            ++record_count;
        } else if (!skip_unknown(bytes, size, &offset, wire_type)) {
            return 0;
        }
    }

    if (number < 0 || (uint32_t)number > requested_capacity ||
        (uint32_t)number != record_count)
        return 0;
    result->status = status;
    result->number = (uint16_t)number;
    result->record_count = record_count;
    return 1;
}

int p4_hosted_scan_records_response(const struct p4_hosted_rpc *rpc,
                                   uint32_t expected_uid,
                                   uint16_t requested_capacity,
                                   struct p4_hosted_scan_records *result)
{
    struct p4_hosted_scan_records decoded;

    if (requested_capacity > P4_HOSTED_SCAN_MAX_RECORDS ||
        !response_input_valid(rpc, expected_uid, result,
                              result ? sizeof *result : 0u) ||
        !response_matches(rpc, SCAN_REQ_RECORDS, expected_uid) ||
        !parse_records_body(rpc->payload, rpc->payload_size,
                            requested_capacity, &decoded))
        return 0;
    copy_bytes((uint8_t *)result, (const uint8_t *)&decoded, sizeof decoded);
    return 1;
}

int p4_hosted_sta_ap_info_response(const struct p4_hosted_rpc *rpc,
                                   uint32_t expected_uid,
                                   struct p4_hosted_sta_ap_info *result)
{
    struct p4_hosted_sta_ap_info decoded;

    if (!response_input_valid(rpc, expected_uid, result,
                              result ? sizeof *result : 0u))
        return 0;
    clear_bytes((uint8_t *)&decoded, sizeof decoded);
    if (!response_matches(rpc, SCAN_REQ_STA_AP_INFO, expected_uid) ||
        !parse_sta_ap_info_body(rpc->payload, rpc->payload_size, &decoded))
        return 0;
    copy_bytes((uint8_t *)result, (const uint8_t *)&decoded, sizeof decoded);
    return 1;
}

