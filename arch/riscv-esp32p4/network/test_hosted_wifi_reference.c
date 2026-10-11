/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
/* Host-only semantic oracle against the pinned generated protobuf-c codec. */
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hosted_wifi.h"
#include "esp_hosted_rpc.pb-c.h"
#include <runtime-wifi-profile.h>

static unsigned checks;

#define CHECK(expression) do { \
    ++checks; \
    if (!(expression)) { \
        fprintf(stderr, "WiFi reference line %u: %s\n", \
                (unsigned)__LINE__, #expression); \
        exit(1); \
    } \
} while (0)

#define WIFI_CONFIG_FIELDS(X) \
    X(static_rx_buf_num) \
    X(dynamic_rx_buf_num) \
    X(tx_buf_type) \
    X(static_tx_buf_num) \
    X(dynamic_tx_buf_num) \
    X(cache_tx_buf_num) \
    X(csi_enable) \
    X(ampdu_rx_enable) \
    X(ampdu_tx_enable) \
    X(amsdu_tx_enable) \
    X(nvs_enable) \
    X(nano_enable) \
    X(rx_ba_win) \
    X(wifi_task_core_id) \
    X(beacon_max_len) \
    X(mgmt_sbuf_num) \
    X(feature_caps) \
    X(sta_disconnected_pm) \
    X(espnow_max_encrypt_num) \
    X(magic)

static int read_varint(const uint8_t *bytes, size_t size, size_t *offset,
                       uint64_t *value)
{
    uint64_t parsed = 0;
    size_t cursor = *offset;
    unsigned index;

    for (index = 0; index < 10u; ++index) {
        uint8_t byte;

        if (cursor >= size)
            return 0;
        byte = bytes[cursor++];
        if (index == 9u && (byte & 0xfeu) != 0)
            return 0;
        parsed |= (uint64_t)(byte & 0x7fu) << (index * 7u);
        if ((byte & 0x80u) == 0) {
            *offset = cursor;
            *value = parsed;
            return 1;
        }
    }
    return 0;
}

static void copy_reference_config(WifiInitConfig *reference,
                                  const struct p4_hosted_wifi_config *source)
{
#define COPY_CONFIG_FIELD(field) reference->field = source->field;
    WIFI_CONFIG_FIELDS(COPY_CONFIG_FIELD)
#undef COPY_CONFIG_FIELD
}

static void compare_config(const WifiInitConfig *actual,
                           const struct p4_hosted_wifi_config *expected)
{
#define CHECK_CONFIG_FIELD(field) \
    CHECK((uint64_t)actual->field == (uint64_t)expected->field);
    WIFI_CONFIG_FIELDS(CHECK_CONFIG_FIELD)
#undef CHECK_CONFIG_FIELD
}

static void fill_mixed_config(struct p4_hosted_wifi_config *config)
{
    memset(config, 0, sizeof *config);
    config->static_rx_buf_num = 0;
    config->dynamic_rx_buf_num = 5;
    config->tx_buf_type = -1;
    config->static_tx_buf_num = 4;
    config->dynamic_tx_buf_num = 0;
    config->cache_tx_buf_num = -7;
    config->csi_enable = 1;
    config->ampdu_rx_enable = 0;
    config->ampdu_tx_enable = 1;
    config->amsdu_tx_enable = -2;
    config->nvs_enable = 0;
    config->nano_enable = 1;
    config->rx_ba_win = 6;
    config->wifi_task_core_id = -1;
    config->beacon_max_len = 65535;
    config->mgmt_sbuf_num = INT32_MIN;
    config->feature_caps = UINT64_C(0xfedcba9876543210);
    config->sta_disconnected_pm = 1u;
    config->espnow_max_encrypt_num = -123456789;
    config->magic = INT32_C(0x23456789);
}

static void check_init_field_stream(const struct p4_hosted_rpc *rpc,
                                    const struct p4_hosted_wifi_config *config)
{
    const uint64_t expected[20] = {
        (uint64_t)(int64_t)config->static_rx_buf_num,
        (uint64_t)(int64_t)config->dynamic_rx_buf_num,
        (uint64_t)(int64_t)config->tx_buf_type,
        (uint64_t)(int64_t)config->static_tx_buf_num,
        (uint64_t)(int64_t)config->dynamic_tx_buf_num,
        (uint64_t)(int64_t)config->cache_tx_buf_num,
        (uint64_t)(int64_t)config->csi_enable,
        (uint64_t)(int64_t)config->ampdu_rx_enable,
        (uint64_t)(int64_t)config->ampdu_tx_enable,
        (uint64_t)(int64_t)config->amsdu_tx_enable,
        (uint64_t)(int64_t)config->nvs_enable,
        (uint64_t)(int64_t)config->nano_enable,
        (uint64_t)(int64_t)config->rx_ba_win,
        (uint64_t)(int64_t)config->wifi_task_core_id,
        (uint64_t)(int64_t)config->beacon_max_len,
        (uint64_t)(int64_t)config->mgmt_sbuf_num,
        config->feature_caps,
        (uint64_t)config->sta_disconnected_pm,
        (uint64_t)(int64_t)config->espnow_max_encrypt_num,
        (uint64_t)(int64_t)config->magic
    };
    size_t offset = 0;
    uint64_t key, length, value;
    unsigned index;

    CHECK(rpc->payload_size != 0u);
    CHECK(read_varint(rpc->payload, rpc->payload_size, &offset, &key));
    CHECK(key == 10u); /* Rpc_Req_WifiInit.cfg */
    CHECK(read_varint(rpc->payload, rpc->payload_size, &offset, &length));
    CHECK(length == rpc->payload_size - offset);
    for (index = 0; index < 20u; ++index) {
        CHECK(read_varint(rpc->payload, rpc->payload_size, &offset, &key));
        CHECK(key == ((uint64_t)(index + 1u) << 3));
        CHECK(read_varint(rpc->payload, rpc->payload_size, &offset, &value));
        CHECK(value == expected[index]);
    }
    CHECK(offset == rpc->payload_size);
}

static void check_init_rpc(const uint8_t *bytes, size_t size, uint32_t uid,
                           const struct p4_hosted_wifi_config *expected)
{
    struct p4_hosted_rpc envelope;
    Rpc *unpacked;

    CHECK(p4_hosted_rpc_decode(bytes, size, &envelope));
    CHECK(envelope.msg_type == P4_HOSTED_RPC_TYPE_REQUEST);
    CHECK(envelope.msg_id == 278u);
    CHECK(envelope.uid == uid);
    check_init_field_stream(&envelope, expected);

    unpacked = rpc__unpack(NULL, size, bytes);
    CHECK(unpacked != NULL);
    CHECK(unpacked->msg_type == RPC_TYPE__Req);
    CHECK(unpacked->msg_id == RPC_ID__Req_WifiInit);
    CHECK(unpacked->uid == uid);
    CHECK(unpacked->payload_case == RPC__PAYLOAD_REQ_WIFI_INIT);
    CHECK(unpacked->req_wifi_init != NULL);
    CHECK(unpacked->req_wifi_init->cfg != NULL);
    compare_config(unpacked->req_wifi_init->cfg, expected);
    rpc__free_unpacked(unpacked, NULL);
}

static void check_init_request(const struct p4_hosted_wifi_config *config,
                               uint32_t uid)
{
    struct p4_hosted_wifi_config snapshot = *config;
    WifiInitConfig reference_config = WIFI_INIT_CONFIG__INIT;
    RpcReqWifiInit request_body = RPC__REQ__WIFI_INIT__INIT;
    Rpc request = RPC__INIT;
    Rpc *reference_unpacked;
    uint8_t native_bytes[512], reference_bytes[512];
    size_t native_size = 0, reference_size;

    copy_reference_config(&reference_config, config);
    request_body.cfg = &reference_config;
    request.msg_type = RPC_TYPE__Req;
    request.msg_id = RPC_ID__Req_WifiInit;
    request.uid = uid;
    request.payload_case = RPC__PAYLOAD_REQ_WIFI_INIT;
    request.req_wifi_init = &request_body;

    reference_size = rpc__get_packed_size(&request);
    CHECK(reference_size <= sizeof reference_bytes);
    CHECK(rpc__pack(&request, reference_bytes) == reference_size);
    CHECK(p4_hosted_wifi_init_request(uid, config, native_bytes,
                                      sizeof native_bytes, &native_size));
    CHECK(native_size <= sizeof native_bytes);
    CHECK(memcmp(config, &snapshot, sizeof snapshot) == 0);

    /* Compare decoded semantics: the native encoder explicitly writes zero
     * config fields, while proto3 protobuf-c omits those defaults. */
    check_init_rpc(native_bytes, native_size, uid, config);
    reference_unpacked = rpc__unpack(NULL, reference_size, reference_bytes);
    CHECK(reference_unpacked != NULL);
    CHECK(reference_unpacked->msg_type == RPC_TYPE__Req);
    CHECK(reference_unpacked->msg_id == RPC_ID__Req_WifiInit);
    CHECK(reference_unpacked->uid == uid);
    CHECK(reference_unpacked->payload_case == RPC__PAYLOAD_REQ_WIFI_INIT);
    CHECK(reference_unpacked->req_wifi_init != NULL);
    CHECK(reference_unpacked->req_wifi_init->cfg != NULL);
    compare_config(reference_unpacked->req_wifi_init->cfg, config);
    rpc__free_unpacked(reference_unpacked, NULL);
}

static void check_request_envelope(const uint8_t *bytes, size_t size,
                                   uint32_t request_id, uint32_t uid,
                                   Rpc__PayloadCase payload_case)
{
    struct p4_hosted_rpc envelope;
    Rpc *unpacked;

    CHECK(p4_hosted_rpc_decode(bytes, size, &envelope));
    CHECK(envelope.msg_type == P4_HOSTED_RPC_TYPE_REQUEST);
    CHECK(envelope.msg_id == request_id);
    CHECK(envelope.uid == uid);
    unpacked = rpc__unpack(NULL, size, bytes);
    CHECK(unpacked != NULL);
    CHECK(unpacked->msg_type == RPC_TYPE__Req);
    CHECK(unpacked->msg_id == (RpcId)request_id);
    CHECK(unpacked->uid == uid);
    CHECK(unpacked->payload_case == payload_case);
    rpc__free_unpacked(unpacked, NULL);
}

static void check_set_mode_request(uint32_t uid)
{
    RpcReqSetMode request_body = RPC__REQ__SET_MODE__INIT;
    Rpc request = RPC__INIT;
    Rpc *unpacked;
    struct p4_hosted_rpc envelope;
    uint8_t bytes[128], reference[128];
    size_t size = 0, reference_size;

    request_body.mode = 1; /* ESP_WIFI_MODE_STA */
    request.msg_type = RPC_TYPE__Req;
    request.msg_id = RPC_ID__Req_SetWifiMode;
    request.uid = uid;
    request.payload_case = RPC__PAYLOAD_REQ_SET_WIFI_MODE;
    request.req_set_wifi_mode = &request_body;
    reference_size = rpc__get_packed_size(&request);
    CHECK(reference_size <= sizeof reference);
    CHECK(rpc__pack(&request, reference) == reference_size);

    CHECK(p4_hosted_wifi_set_mode_request(uid, 1, bytes, sizeof bytes, &size));
    CHECK(size == reference_size);
    CHECK(memcmp(bytes, reference, size) == 0);
    CHECK(p4_hosted_rpc_decode(bytes, size, &envelope));
    CHECK(envelope.msg_type == P4_HOSTED_RPC_TYPE_REQUEST);
    CHECK(envelope.msg_id == 260u && envelope.uid == uid);
    unpacked = rpc__unpack(NULL, size, bytes);
    CHECK(unpacked != NULL);
    CHECK(unpacked->msg_type == RPC_TYPE__Req);
    CHECK(unpacked->msg_id == RPC_ID__Req_SetWifiMode);
    CHECK(unpacked->uid == uid);
    CHECK(unpacked->payload_case == RPC__PAYLOAD_REQ_SET_WIFI_MODE);
    CHECK(unpacked->req_set_wifi_mode != NULL);
    CHECK(unpacked->req_set_wifi_mode->mode == 1);
    rpc__free_unpacked(unpacked, NULL);

    unpacked = rpc__unpack(NULL, reference_size, reference);
    CHECK(unpacked != NULL);
    CHECK(unpacked->msg_id == RPC_ID__Req_SetWifiMode);
    CHECK(unpacked->uid == uid);
    CHECK(unpacked->payload_case == RPC__PAYLOAD_REQ_SET_WIFI_MODE);
    CHECK(unpacked->req_set_wifi_mode->mode == 1);
    rpc__free_unpacked(unpacked, NULL);
}

static void check_simple_request(uint32_t request_id, uint32_t uid,
                                 Rpc__PayloadCase payload_case)
{
    Rpc request = RPC__INIT;
    RpcReqWifiDeinit deinit = RPC__REQ__WIFI_DEINIT__INIT;
    RpcReqWifiStart start = RPC__REQ__WIFI_START__INIT;
    RpcReqWifiStop stop = RPC__REQ__WIFI_STOP__INIT;
    uint8_t bytes[128];
    uint8_t reference[128];
    size_t size = 0, reference_size;

    request.msg_type = RPC_TYPE__Req;
    request.msg_id = (RpcId)request_id;
    request.uid = uid;
    request.payload_case = payload_case;
    switch (request_id) {
    case 279u: request.req_wifi_deinit = &deinit; break;
    case 280u: request.req_wifi_start = &start; break;
    case 281u: request.req_wifi_stop = &stop; break;
    default: CHECK(0); return;
    }
    reference_size = rpc__get_packed_size(&request);
    CHECK(reference_size <= sizeof reference);
    CHECK(rpc__pack(&request, reference) == reference_size);

    CHECK(p4_hosted_wifi_simple_request(request_id, uid, bytes, sizeof bytes,
                                        &size));
    CHECK(size == reference_size);
    CHECK(memcmp(bytes, reference, size) == 0);
    check_request_envelope(bytes, size, request_id, uid, payload_case);
}

static Rpc__PayloadCase response_payload_case(uint32_t request_id)
{
    switch (request_id) {
    case 260u: return RPC__PAYLOAD_RESP_SET_WIFI_MODE;
    case 278u: return RPC__PAYLOAD_RESP_WIFI_INIT;
    case 279u: return RPC__PAYLOAD_RESP_WIFI_DEINIT;
    case 280u: return RPC__PAYLOAD_RESP_WIFI_START;
    case 281u: return RPC__PAYLOAD_RESP_WIFI_STOP;
    default: return RPC__PAYLOAD__NOT_SET;
    }
}

static size_t pack_reference_status(uint32_t request_id, uint32_t uid,
                                    int32_t status, uint8_t *bytes,
                                    size_t capacity)
{
    Rpc response = RPC__INIT;
    RpcRespSetMode set_mode = RPC__RESP__SET_MODE__INIT;
    RpcRespWifiInit wifi_init = RPC__RESP__WIFI_INIT__INIT;
    RpcRespWifiDeinit wifi_deinit = RPC__RESP__WIFI_DEINIT__INIT;
    RpcRespWifiStart wifi_start = RPC__RESP__WIFI_START__INIT;
    RpcRespWifiStop wifi_stop = RPC__RESP__WIFI_STOP__INIT;
    size_t size;

    response.msg_type = RPC_TYPE__Resp;
    response.msg_id = (RpcId)(request_id + 256u);
    response.uid = uid;
    switch (request_id) {
    case 260u:
        set_mode.resp = status;
        response.payload_case = RPC__PAYLOAD_RESP_SET_WIFI_MODE;
        response.resp_set_wifi_mode = &set_mode;
        break;
    case 278u:
        wifi_init.resp = status;
        response.payload_case = RPC__PAYLOAD_RESP_WIFI_INIT;
        response.resp_wifi_init = &wifi_init;
        break;
    case 279u:
        wifi_deinit.resp = status;
        response.payload_case = RPC__PAYLOAD_RESP_WIFI_DEINIT;
        response.resp_wifi_deinit = &wifi_deinit;
        break;
    case 280u:
        wifi_start.resp = status;
        response.payload_case = RPC__PAYLOAD_RESP_WIFI_START;
        response.resp_wifi_start = &wifi_start;
        break;
    case 281u:
        wifi_stop.resp = status;
        response.payload_case = RPC__PAYLOAD_RESP_WIFI_STOP;
        response.resp_wifi_stop = &wifi_stop;
        break;
    default:
        return 0;
    }
    size = rpc__get_packed_size(&response);
    if (size > capacity)
        return 0;
    return rpc__pack(&response, bytes);
}

static void check_unpacked_status(const Rpc *response, uint32_t request_id,
                                 int32_t expected)
{
    CHECK(response->payload_case == response_payload_case(request_id));
    switch (request_id) {
    case 260u: CHECK(response->resp_set_wifi_mode->resp == expected); break;
    case 278u: CHECK(response->resp_wifi_init->resp == expected); break;
    case 279u: CHECK(response->resp_wifi_deinit->resp == expected); break;
    case 280u: CHECK(response->resp_wifi_start->resp == expected); break;
    case 281u: CHECK(response->resp_wifi_stop->resp == expected); break;
    default: CHECK(0); break;
    }
}

static void check_status_response(uint32_t request_id, uint32_t uid,
                                  int32_t expected)
{
    uint8_t bytes[128];
    size_t size;
    struct p4_hosted_rpc decoded;
    Rpc *unpacked;
    int32_t actual = INT32_C(0x1234567);

    size = pack_reference_status(request_id, uid, expected,
                                  bytes, sizeof bytes);
    CHECK(size != 0u);
    CHECK(p4_hosted_rpc_decode(bytes, size, &decoded));
    CHECK(decoded.msg_type == P4_HOSTED_RPC_TYPE_RESPONSE);
    CHECK(decoded.msg_id == request_id + 256u);
    CHECK(decoded.uid == uid);
    CHECK(p4_hosted_wifi_status_response(&decoded, request_id, uid, &actual));
    CHECK(actual == expected);
    if (expected == 0)
        CHECK(decoded.payload_size == 0u); /* proto3 omitted status */
    else
        CHECK(decoded.payload_size != 0u);

    unpacked = rpc__unpack(NULL, size, bytes);
    CHECK(unpacked != NULL);
    CHECK(unpacked->msg_type == RPC_TYPE__Resp);
    CHECK(unpacked->msg_id == (RpcId)(request_id + 256u));
    CHECK(unpacked->uid == uid);
    check_unpacked_status(unpacked, request_id, expected);
    rpc__free_unpacked(unpacked, NULL);

    actual = INT32_C(0x13572468);
    CHECK(!p4_hosted_wifi_status_response(&decoded, request_id,
                                          uid + 1u, &actual));
    CHECK(actual == INT32_C(0x13572468));
    actual = INT32_C(0x24681357);
    CHECK(!p4_hosted_wifi_status_response(&decoded,
                                          request_id == 260u ? 278u : 260u,
                                          uid, &actual));
    CHECK(actual == INT32_C(0x24681357));
}

int main(void)
{
    static const uint32_t request_ids[] = { 260u, 278u, 279u, 280u, 281u };
    static const int32_t statuses[] = {
        0, 7, -1, INT32_MIN, INT32_MAX
    };
    struct p4_hosted_wifi_config zero_config;
    struct p4_hosted_wifi_config mixed_config;
    uint32_t uid = UINT32_C(0xfedcba98);
    size_t request_index, status_index;

    memset(&zero_config, 0, sizeof zero_config);
    fill_mixed_config(&mixed_config);
    check_init_request(&zero_config, uid);
    check_init_request(&mixed_config, uid + 1u);
    /* IDF5.4.3 CONFIG_FEATURE_WPA3_SAE_BIT is bit0. This proves the
     * host requests SAE; it does not prove the installed peer compiled it. */
    CHECK((wifi_reference_profile.feature_caps & UINT64_C(1)) != 0u);
    CHECK(wifi_reference_profile.feature_caps == UINT64_C(129));
    CHECK(wifi_reference_profile.nvs_enable == 0);
    check_init_request(&wifi_reference_profile, uid + 2u);
    check_set_mode_request(uid);
    check_simple_request(279u, uid, RPC__PAYLOAD_REQ_WIFI_DEINIT);
    check_simple_request(280u, uid, RPC__PAYLOAD_REQ_WIFI_START);
    check_simple_request(281u, uid, RPC__PAYLOAD_REQ_WIFI_STOP);

    for (request_index = 0;
         request_index < sizeof request_ids / sizeof request_ids[0];
         ++request_index)
        for (status_index = 0;
             status_index < sizeof statuses / sizeof statuses[0];
             ++status_index)
            check_status_response(request_ids[request_index], uid,
                                 statuses[status_index]);

    printf("PASS: pinned WiFi protobuf interoperability, %u checks\n", checks);
    return 0;
}
