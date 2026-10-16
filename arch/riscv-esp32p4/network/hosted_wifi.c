/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#include "hosted_wifi.h"

#define WIFI_MAX_FIELD UINT32_C(0x1fffffff)
#define WIFI_REQ_SET_MODE 260u
#define WIFI_REQ_INIT 278u
#define WIFI_REQ_DEINIT 279u
#define WIFI_REQ_START 280u
#define WIFI_REQ_STOP 281u

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

static int range_is_valid(const void *pointer, size_t size)
{
    uintptr_t start, length;

    if (size == 0)
        return 1;
    if (!pointer)
        return 0;
    start = (uintptr_t)pointer;
    length = (uintptr_t)size;
    if ((size_t)length != size || length > UINTPTR_MAX - start)
        return 0;
    return 1;
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

static int add_size(size_t *total, size_t amount)
{
    if (amount > SIZE_MAX - *total)
        return 0;
    *total += amount;
    return 1;
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

static uint64_t int32_wire_value(int32_t value)
{
    return (uint64_t)(int64_t)value;
}

static int encode_request(uint32_t request_id, uint32_t uid,
                          const uint8_t *payload, size_t payload_size,
                          const void *input, size_t input_size,
                          uint8_t *bytes, size_t capacity, size_t *written)
{
    struct p4_hosted_rpc request;

    if (!bytes || !written || uid == 0 ||
        ranges_overlap(bytes, capacity, written, sizeof *written) ||
        (input_size != 0 &&
         (!input || ranges_overlap(bytes, capacity, input, input_size) ||
          ranges_overlap(written, sizeof *written, input, input_size))))
        return 0;

    request.msg_type = P4_HOSTED_RPC_TYPE_REQUEST;
    request.msg_id = request_id;
    request.uid = uid;
    request.payload = payload;
    request.payload_size = payload_size;
    return p4_hosted_rpc_encode(bytes, capacity, &request, written);
}

static void get_config_values(const struct p4_hosted_wifi_config *config,
                              uint64_t values[20])
{
    values[0] = int32_wire_value(config->static_rx_buf_num);
    values[1] = int32_wire_value(config->dynamic_rx_buf_num);
    values[2] = int32_wire_value(config->tx_buf_type);
    values[3] = int32_wire_value(config->static_tx_buf_num);
    values[4] = int32_wire_value(config->dynamic_tx_buf_num);
    values[5] = int32_wire_value(config->cache_tx_buf_num);
    values[6] = int32_wire_value(config->csi_enable);
    values[7] = int32_wire_value(config->ampdu_rx_enable);
    values[8] = int32_wire_value(config->ampdu_tx_enable);
    values[9] = int32_wire_value(config->amsdu_tx_enable);
    values[10] = int32_wire_value(config->nvs_enable);
    values[11] = int32_wire_value(config->nano_enable);
    values[12] = int32_wire_value(config->rx_ba_win);
    values[13] = int32_wire_value(config->wifi_task_core_id);
    values[14] = int32_wire_value(config->beacon_max_len);
    values[15] = int32_wire_value(config->mgmt_sbuf_num);
    values[16] = config->feature_caps;
    values[17] = (uint64_t)config->sta_disconnected_pm;
    values[18] = int32_wire_value(config->espnow_max_encrypt_num);
    values[19] = int32_wire_value(config->magic);
}

static int config_body_size(const uint64_t values[20], size_t *body_size)
{
    size_t total = 0;
    unsigned int i;

    for (i = 0; i < 20u; ++i) {
        uint64_t key = ((uint64_t)(i + 1u) << 3) | UINT64_C(0);
        if (!add_size(&total, varint_size(key)) ||
            !add_size(&total, varint_size(values[i])))
            return 0;
    }
    if (total > P4_HOSTED_WIFI_MAX_BODY_SIZE)
        return 0;
    *body_size = total;
    return 1;
}

int p4_hosted_wifi_init_request(uint32_t uid,
                                const struct p4_hosted_wifi_config *config,
                                uint8_t *bytes, size_t capacity,
                                size_t *written)
{
    uint64_t values[20];
    uint8_t payload[P4_HOSTED_WIFI_MAX_BODY_SIZE];
    size_t config_size, offset = 0;
    unsigned int i;

    if (!config || !range_is_valid(config, sizeof *config) ||
        ranges_overlap(config, sizeof *config, bytes, capacity) ||
        ranges_overlap(config, sizeof *config, written,
                       written ? sizeof *written : 0u))
        return 0;
    if (config->sta_disconnected_pm > 1u)
        return 0;

    get_config_values(config, values);
    if (!config_body_size(values, &config_size) ||
        config_size > P4_HOSTED_WIFI_MAX_BODY_SIZE - 1u -
                      varint_size((uint64_t)config_size))
        return 0;

    /* Rpc_Req_WifiInit.cfg is field 1, length-delimited. */
    payload[offset++] = 0x0au;
    write_varint(payload, &offset, (uint64_t)config_size);
    for (i = 0; i < 20u; ++i) {
        uint64_t key = ((uint64_t)(i + 1u) << 3) | UINT64_C(0);
        write_varint(payload, &offset, key);
        write_varint(payload, &offset, values[i]);
    }
    if (offset > P4_HOSTED_WIFI_MAX_BODY_SIZE)
        return 0;

    return encode_request(WIFI_REQ_INIT, uid, payload, offset,
                          config, sizeof *config, bytes, capacity, written);
}

int p4_hosted_wifi_simple_request(uint32_t request_id, uint32_t uid,
                                  uint8_t *bytes, size_t capacity,
                                  size_t *written)
{
    if (request_id != WIFI_REQ_DEINIT && request_id != WIFI_REQ_START &&
        request_id != WIFI_REQ_STOP)
        return 0;
    return encode_request(request_id, uid, NULL, 0u, NULL, 0u,
                          bytes, capacity, written);
}

int p4_hosted_wifi_set_mode_request(uint32_t uid, int32_t mode,
                                    uint8_t *bytes, size_t capacity,
                                    size_t *written)
{
    static const uint8_t sta_mode_body[] = { 0x08u, 0x01u };

    if (mode != 1)
        return 0;
    return encode_request(WIFI_REQ_SET_MODE, uid,
                          sta_mode_body, sizeof sta_mode_body,
                          NULL, 0u, bytes, capacity, written);
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
        if (*offset > size || size - *offset < 8u)
            return 0;
        *offset += 8u;
        return 1;
    case 2:
        if (!read_varint(bytes, size, offset, &length) || *offset > size ||
            length > (uint64_t)(size - *offset))
            return 0;
        *offset += (size_t)length;
        return 1;
    case 5:
        if (*offset > size || size - *offset < 4u)
            return 0;
        *offset += 4u;
        return 1;
    default:
        /* Deprecated groups and reserved wire types are rejected. */
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
    return request_id == WIFI_REQ_INIT || request_id == WIFI_REQ_DEINIT ||
           request_id == WIFI_REQ_START || request_id == WIFI_REQ_STOP ||
           request_id == WIFI_REQ_SET_MODE;
}

int p4_hosted_wifi_status_response(const struct p4_hosted_rpc *rpc,
                                   uint32_t expected_request_id,
                                   uint32_t expected_uid, int32_t *status)
{
    int32_t decoded_status = 0;
    size_t offset = 0;
    int seen_status = 0;
    uint32_t response_id;

    if (!rpc || !status ||
        ranges_overlap(rpc, sizeof *rpc, status, sizeof *status) ||
        expected_uid == 0 || !supported_request_id(expected_request_id))
        return 0;

    response_id = expected_request_id + 256u;
    if (rpc->msg_type != P4_HOSTED_RPC_TYPE_RESPONSE ||
        rpc->msg_id != response_id || rpc->uid != expected_uid ||
        rpc->payload_size > P4_HOSTED_WIFI_MAX_BODY_SIZE ||
        (rpc->payload_size != 0u && !rpc->payload) ||
        ranges_overlap(rpc->payload, rpc->payload_size,
                       status, sizeof *status))
        return 0;

    while (offset < rpc->payload_size) {
        uint64_t key, field_number;
        unsigned int wire_type;

        if (!read_varint(rpc->payload, rpc->payload_size, &offset, &key) ||
            key == 0)
            return 0;
        field_number = key >> 3;
        if (field_number == 0 || field_number > WIFI_MAX_FIELD)
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
