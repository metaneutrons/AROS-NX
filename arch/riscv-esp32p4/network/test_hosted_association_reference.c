/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
/* Independent semantic oracle against the pinned generated protobuf-c RPC. */
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hosted_association.h"
#include "esp_hosted_rpc.pb-c.h"

static unsigned checks;

#define CHECK(expression) do { \
    ++checks; \
    if (!(expression)) { \
        fprintf(stderr, "association reference line %u: %s\n", \
                (unsigned)__LINE__, #expression); \
        exit(1); \
    } \
} while (0)

#define TEST_UID UINT32_C(0x1234)
#define TEST_WIRE_CAPACITY 4096u

static size_t pack_rpc(Rpc *rpc, uint8_t *bytes, size_t capacity)
{
    size_t size = rpc__get_packed_size(rpc);

    if (size > capacity)
        return 0u;
    CHECK(rpc__pack(rpc, bytes) == size);
    return size;
}

static Rpc *unpack_rpc(const uint8_t *bytes, size_t size, uint32_t type,
                       uint32_t id, uint32_t uid,
                       Rpc__PayloadCase payload_case)
{
    Rpc *rpc = rpc__unpack(NULL, size, bytes);

    CHECK(rpc != NULL);
    CHECK(rpc->msg_type == (RpcType)type);
    CHECK(rpc->msg_id == (RpcId)id);
    CHECK(rpc->uid == uid);
    CHECK(rpc->payload_case == payload_case);
    return rpc;
}

struct config_fixture {
    struct p4_hosted_sta_config config;
    uint8_t ssid[P4_HOSTED_ASSOCIATION_SSID_SIZE];
    uint8_t password[P4_HOSTED_ASSOCIATION_PASSWORD_SIZE];
    uint8_t bssid[P4_HOSTED_ASSOCIATION_BSSID_SIZE];
    uint8_t sae_id[P4_HOSTED_ASSOCIATION_SAE_ID_SIZE];
};

static void fill_pattern(uint8_t *bytes, size_t size, uint8_t seed)
{
    size_t index;

    for (index = 0; index < size; ++index)
        bytes[index] = (uint8_t)(seed + (uint8_t)(index * 37u));
    if (size > 2u)
        bytes[2] = 0u;
    if (size > 7u)
        bytes[7] = 0xffu;
}

static void init_config_fixture(struct config_fixture *fixture, unsigned kind)
{
    memset(fixture, 0, sizeof *fixture);
    if (kind == 1u) {
        /* Synthetic arbitrary bytes only; no actual SSID or credentials. */
        fill_pattern(fixture->ssid, sizeof fixture->ssid, 0x21u);
        fill_pattern(fixture->password, sizeof fixture->password, 0x83u);
        fill_pattern(fixture->bssid, sizeof fixture->bssid, 0x35u);
        fill_pattern(fixture->sae_id, sizeof fixture->sae_id, 0xc1u);
        fixture->config.ssid = fixture->ssid;
        fixture->config.ssid_length = 9u;
        fixture->config.password = fixture->password;
        fixture->config.password_length = 17u;
        fixture->config.scan_method = INT32_MIN;
        fixture->config.bssid_set = 1u;
        fixture->config.bssid = fixture->bssid;
        fixture->config.bssid_length = sizeof fixture->bssid;
        fixture->config.channel = UINT32_MAX;
        fixture->config.listen_interval = UINT32_C(0x80000001);
        fixture->config.sort_method = -1234567;
        fixture->config.threshold_rssi = INT32_MIN;
        fixture->config.threshold_authmode = -1;
        fixture->config.pmf_capable = 1u;
        fixture->config.pmf_required = 1u;
        fixture->config.bitmask = UINT32_C(0xfedcba98);
        fixture->config.sae_pwe_h2e = INT32_MAX;
        fixture->config.failure_retry_cnt = UINT32_MAX;
        fixture->config.he_bitmask = UINT32_C(0x80000001);
        fixture->config.sae_h2e_identifier = fixture->sae_id;
        fixture->config.sae_h2e_identifier_length = 11u;
    } else if (kind == 2u || kind == 3u) {
        /* Maximum schema byte lengths, still below the envelope bound. */
        fill_pattern(fixture->ssid, sizeof fixture->ssid, 0x11u);
        fill_pattern(fixture->password, sizeof fixture->password, 0x61u);
        fill_pattern(fixture->bssid, sizeof fixture->bssid, 0x91u);
        fill_pattern(fixture->sae_id, sizeof fixture->sae_id, 0xd1u);
        fixture->config.ssid = fixture->ssid;
        fixture->config.ssid_length = sizeof fixture->ssid;
        fixture->config.password = fixture->password;
        fixture->config.password_length = sizeof fixture->password;
        fixture->config.bssid_set = 1u;
        fixture->config.bssid = fixture->bssid;
        fixture->config.bssid_length = sizeof fixture->bssid;
        fixture->config.sae_h2e_identifier = fixture->sae_id;
        fixture->config.sae_h2e_identifier_length = sizeof fixture->sae_id;
        if (kind == 3u) {
            /* Simultaneous worst-case field widths; all values synthetic. */
            fixture->config.scan_method = INT32_MIN;
            fixture->config.bssid_set = 1u;
            fixture->config.channel = UINT32_MAX;
            fixture->config.listen_interval = UINT32_MAX;
            fixture->config.sort_method = INT32_MIN;
            fixture->config.threshold_rssi = INT32_MIN;
            fixture->config.threshold_authmode = INT32_MIN;
            fixture->config.pmf_capable = 1u;
            fixture->config.pmf_required = 1u;
            fixture->config.bitmask = UINT32_MAX;
            fixture->config.sae_pwe_h2e = INT32_MIN;
            fixture->config.failure_retry_cnt = UINT32_MAX;
            fixture->config.he_bitmask = UINT32_MAX;
        }
    }
}

static void compare_sta(const WifiStaConfig *actual,
                        const struct p4_hosted_sta_config *expected)
{
    CHECK(actual != NULL);
    CHECK(actual->ssid.len == expected->ssid_length);
    CHECK(actual->password.len == expected->password_length);
    if (expected->ssid_length != 0u)
        CHECK(memcmp(actual->ssid.data, expected->ssid,
                     expected->ssid_length) == 0);
    if (expected->password_length != 0u)
        CHECK(memcmp(actual->password.data, expected->password,
                     expected->password_length) == 0);
    CHECK(actual->scan_method == expected->scan_method);
    CHECK(actual->bssid_set == (protobuf_c_boolean)expected->bssid_set);
    CHECK(actual->bssid.len == expected->bssid_length);
    if (expected->bssid_length != 0u)
        CHECK(memcmp(actual->bssid.data, expected->bssid,
                     expected->bssid_length) == 0);
    CHECK(actual->channel == expected->channel);
    CHECK(actual->listen_interval == expected->listen_interval);
    CHECK(actual->sort_method == expected->sort_method);
    CHECK(actual->threshold != NULL);
    CHECK(actual->threshold->rssi == expected->threshold_rssi);
    CHECK(actual->threshold->authmode == expected->threshold_authmode);
    CHECK(actual->pmf_cfg != NULL);
    CHECK(actual->pmf_cfg->capable ==
          (protobuf_c_boolean)expected->pmf_capable);
    CHECK(actual->pmf_cfg->required ==
          (protobuf_c_boolean)expected->pmf_required);
    CHECK(actual->bitmask == expected->bitmask);
    CHECK(actual->sae_pwe_h2e == expected->sae_pwe_h2e);
    CHECK(actual->failure_retry_cnt == expected->failure_retry_cnt);
    CHECK(actual->he_bitmask == expected->he_bitmask);
    CHECK(actual->sae_h2e_identifier.len ==
          expected->sae_h2e_identifier_length);
    if (expected->sae_h2e_identifier_length != 0u)
        CHECK(memcmp(actual->sae_h2e_identifier.data,
                     expected->sae_h2e_identifier,
                     expected->sae_h2e_identifier_length) == 0);
}

static size_t pack_set_config(uint32_t uid,
                              const struct p4_hosted_sta_config *config,
                              uint8_t *bytes, size_t capacity)
{
    WifiScanThreshold threshold = WIFI_SCAN_THRESHOLD__INIT;
    WifiPmfConfig pmf = WIFI_PMF_CONFIG__INIT;
    WifiStaConfig sta = WIFI_STA_CONFIG__INIT;
    WifiConfig cfg = WIFI_CONFIG__INIT;
    RpcReqWifiSetConfig body = RPC__REQ__WIFI_SET_CONFIG__INIT;
    Rpc request = RPC__INIT;

    sta.ssid.data = (uint8_t *)config->ssid;
    sta.ssid.len = config->ssid_length;
    sta.password.data = (uint8_t *)config->password;
    sta.password.len = config->password_length;
    sta.scan_method = config->scan_method;
    sta.bssid_set = (protobuf_c_boolean)config->bssid_set;
    sta.bssid.data = (uint8_t *)config->bssid;
    sta.bssid.len = config->bssid_length;
    sta.channel = config->channel;
    sta.listen_interval = config->listen_interval;
    sta.sort_method = config->sort_method;
    threshold.rssi = config->threshold_rssi;
    threshold.authmode = config->threshold_authmode;
    sta.threshold = &threshold;
    pmf.capable = (protobuf_c_boolean)config->pmf_capable;
    pmf.required = (protobuf_c_boolean)config->pmf_required;
    sta.pmf_cfg = &pmf;
    sta.bitmask = config->bitmask;
    sta.sae_pwe_h2e = config->sae_pwe_h2e;
    sta.failure_retry_cnt = config->failure_retry_cnt;
    sta.he_bitmask = config->he_bitmask;
    sta.sae_h2e_identifier.data = (uint8_t *)config->sae_h2e_identifier;
    sta.sae_h2e_identifier.len = config->sae_h2e_identifier_length;
    cfg.u_case = WIFI_CONFIG__U_STA;
    cfg.sta = &sta;
    body.iface = 0; /* WIFI_IF_STA */
    body.cfg = &cfg;
    request.msg_type = RPC_TYPE__Req;
    request.msg_id = RPC_ID__Req_WifiSetConfig;
    request.uid = uid;
    request.payload_case = RPC__PAYLOAD_REQ_WIFI_SET_CONFIG;
    request.req_wifi_set_config = &body;
    return pack_rpc(&request, bytes, capacity);
}

static void check_set_config_case(unsigned kind)
{
    struct config_fixture fixture;
    struct p4_hosted_rpc envelope;
    Rpc *decoded;
    uint8_t native[TEST_WIRE_CAPACITY], reference[TEST_WIRE_CAPACITY];
    size_t native_size = 0u, reference_size;
    uint32_t uid = kind == 3u ? UINT32_MAX : TEST_UID;

    init_config_fixture(&fixture, kind);
    reference_size = pack_set_config(uid, &fixture.config, reference,
                                     sizeof reference);
    CHECK(reference_size != 0u);
    if (kind == 3u) {
        uint8_t before[TEST_WIRE_CAPACITY];
        size_t written = 0x6b6bu;

        printf("max_schema_set_config_request_size: %zu bytes\n",
               reference_size);
        CHECK(reference_size == 258u);
        memset(native, 0xa9, sizeof native);
        memcpy(before, native, sizeof before);
        native_size = written;
        CHECK(!p4_hosted_association_set_config_request(uid,
                &fixture.config, native, sizeof native, &native_size));
        CHECK(memcmp(native, before, sizeof native) == 0);
        CHECK(native_size == written);
        CHECK(!p4_hosted_association_set_config_request(uid,
                &fixture.config, native,
                P4_HOSTED_ASSOCIATION_MAX_REQUEST_SIZE, &native_size));
        CHECK(memcmp(native, before, sizeof native) == 0);
        CHECK(native_size == written);
        return;
    }
    CHECK(p4_hosted_association_set_config_request(uid,
            &fixture.config, native, sizeof native, &native_size));
    CHECK(native_size <= P4_HOSTED_ASSOCIATION_MAX_REQUEST_SIZE);
    CHECK(native_size == reference_size);
    CHECK(memcmp(native, reference, native_size) == 0);
    CHECK(p4_hosted_rpc_decode(native, native_size, &envelope));
    CHECK(envelope.msg_type == P4_HOSTED_RPC_TYPE_REQUEST);
    CHECK(envelope.msg_id == 284u);
    CHECK(envelope.uid == uid);
    decoded = unpack_rpc(native, native_size, 1u, 284u, uid,
                         RPC__PAYLOAD_REQ_WIFI_SET_CONFIG);
    CHECK(decoded->req_wifi_set_config != NULL);
    CHECK(decoded->req_wifi_set_config->iface == 0);
    CHECK(decoded->req_wifi_set_config->cfg != NULL);
    CHECK(decoded->req_wifi_set_config->cfg->u_case == WIFI_CONFIG__U_STA);
    compare_sta(decoded->req_wifi_set_config->cfg->sta, &fixture.config);
    rpc__free_unpacked(decoded, NULL);

    /* Decode the independently generated reference form as well. */
    decoded = unpack_rpc(reference, reference_size, 1u, 284u, uid,
                         RPC__PAYLOAD_REQ_WIFI_SET_CONFIG);
    CHECK(decoded->req_wifi_set_config->cfg->u_case == WIFI_CONFIG__U_STA);
    compare_sta(decoded->req_wifi_set_config->cfg->sta, &fixture.config);
    rpc__free_unpacked(decoded, NULL);
}

static void test_set_config_requests(void)
{
    struct config_fixture fixture;
    uint8_t bytes[TEST_WIRE_CAPACITY], before[TEST_WIRE_CAPACITY];
    uint8_t undersized[TEST_WIRE_CAPACITY];
    size_t written, before_written, full_size;
    uint8_t too_long[ P4_HOSTED_ASSOCIATION_PASSWORD_SIZE + 1u ];
    struct p4_hosted_sta_config bad;

    check_set_config_case(0u); /* all proto3 scalars/byte fields default */
    check_set_config_case(1u); /* all typed fields and signed negatives */
    check_set_config_case(2u); /* maximum byte fields */
    check_set_config_case(3u); /* max bytes + int32/uint32 + max UID */

    init_config_fixture(&fixture, 1u);
    full_size = pack_set_config(TEST_UID, &fixture.config, undersized,
                                sizeof undersized);
    CHECK(full_size > 1u);
    memset(bytes, 0xa7, sizeof bytes);
    memcpy(before, bytes, sizeof before);
    written = 0x5a5au;
    before_written = written;
    CHECK(!p4_hosted_association_set_config_request(TEST_UID,
            &fixture.config, bytes, full_size - 1u, &written));
    CHECK(memcmp(bytes, before, sizeof bytes) == 0);
    CHECK(written == before_written);

    memset(too_long, 0x6d, sizeof too_long);
    bad = fixture.config;
    bad.ssid = too_long;
    bad.ssid_length = P4_HOSTED_ASSOCIATION_SSID_SIZE + 1u;
    CHECK(!p4_hosted_association_set_config_request(TEST_UID, &bad,
            bytes, sizeof bytes, &written));
    CHECK(memcmp(bytes, before, sizeof bytes) == 0);
    CHECK(written == before_written);
    bad = fixture.config;
    bad.password = too_long;
    bad.password_length = P4_HOSTED_ASSOCIATION_PASSWORD_SIZE + 1u;
    CHECK(!p4_hosted_association_set_config_request(TEST_UID, &bad,
            bytes, sizeof bytes, &written));
    CHECK(memcmp(bytes, before, sizeof bytes) == 0);
    CHECK(written == before_written);
    bad = fixture.config;
    bad.bssid_length = P4_HOSTED_ASSOCIATION_BSSID_SIZE + 1u;
    CHECK(!p4_hosted_association_set_config_request(TEST_UID, &bad,
            bytes, sizeof bytes, &written));
    CHECK(memcmp(bytes, before, sizeof bytes) == 0);
    CHECK(written == before_written);
    bad = fixture.config;
    bad.sae_h2e_identifier = too_long;
    bad.sae_h2e_identifier_length =
        P4_HOSTED_ASSOCIATION_SAE_ID_SIZE + 1u;
    CHECK(!p4_hosted_association_set_config_request(TEST_UID, &bad,
            bytes, sizeof bytes, &written));
    CHECK(memcmp(bytes, before, sizeof bytes) == 0);
    CHECK(written == before_written);

    bad = fixture.config;
    bad.ssid = NULL;
    CHECK(!p4_hosted_association_set_config_request(TEST_UID, &bad,
            bytes, sizeof bytes, &written));
    CHECK(memcmp(bytes, before, sizeof bytes) == 0);
    CHECK(written == before_written);
    CHECK(!p4_hosted_association_set_config_request(0u, &fixture.config,
            bytes, sizeof bytes, &written));
    CHECK(memcmp(bytes, before, sizeof bytes) == 0);
    CHECK(written == before_written);
    CHECK(!p4_hosted_association_set_config_request(TEST_UID, NULL,
            bytes, sizeof bytes, &written));
    CHECK(memcmp(bytes, before, sizeof bytes) == 0);
    CHECK(written == before_written);
    CHECK(!p4_hosted_association_set_config_request(TEST_UID, &fixture.config,
            bytes, sizeof bytes, NULL));
    CHECK(memcmp(bytes, before, sizeof bytes) == 0);
}

static void test_empty_association_requests(void)
{
    RpcReqWifiConnect connect = RPC__REQ__WIFI_CONNECT__INIT;
    RpcReqWifiDisconnect disconnect = RPC__REQ__WIFI_DISCONNECT__INIT;
    Rpc request = RPC__INIT;
    Rpc *decoded;
    uint8_t native[128], reference[128];
    size_t native_size, reference_size;
    uint32_t id;

    for (id = 282u; id <= 283u; ++id) {
        request.msg_type = RPC_TYPE__Req;
        request.msg_id = (RpcId)id;
        request.uid = TEST_UID;
        if (id == 282u) {
            request.payload_case = RPC__PAYLOAD_REQ_WIFI_CONNECT;
            request.req_wifi_connect = &connect;
        } else {
            request.payload_case = RPC__PAYLOAD_REQ_WIFI_DISCONNECT;
            request.req_wifi_disconnect = &disconnect;
        }
        reference_size = pack_rpc(&request, reference, sizeof reference);
        CHECK(reference_size != 0u);
        native_size = 0u;
        if (id == 282u)
            CHECK(p4_hosted_association_connect_request(TEST_UID, native,
                    sizeof native, &native_size));
        else
            CHECK(p4_hosted_association_disconnect_request(TEST_UID, native,
                    sizeof native, &native_size));
        CHECK(native_size == reference_size);
        CHECK(memcmp(native, reference, native_size) == 0);
        decoded = unpack_rpc(native, native_size, 1u, id, TEST_UID,
                id == 282u ? RPC__PAYLOAD_REQ_WIFI_CONNECT :
                             RPC__PAYLOAD_REQ_WIFI_DISCONNECT);
        rpc__free_unpacked(decoded, NULL);
    }
}

static size_t pack_status_response(uint32_t request_id, uint32_t uid,
                                   int32_t status, uint8_t *bytes,
                                   size_t capacity)
{
    RpcRespWifiSetConfig set_config = RPC__RESP__WIFI_SET_CONFIG__INIT;
    RpcRespWifiConnect connect = RPC__RESP__WIFI_CONNECT__INIT;
    RpcRespWifiDisconnect disconnect = RPC__RESP__WIFI_DISCONNECT__INIT;
    Rpc response = RPC__INIT;

    response.msg_type = RPC_TYPE__Resp;
    response.msg_id = (RpcId)(request_id + 256u);
    response.uid = uid;
    if (request_id == 284u) {
        set_config.resp = status;
        response.payload_case = RPC__PAYLOAD_RESP_WIFI_SET_CONFIG;
        response.resp_wifi_set_config = &set_config;
    } else if (request_id == 282u) {
        connect.resp = status;
        response.payload_case = RPC__PAYLOAD_RESP_WIFI_CONNECT;
        response.resp_wifi_connect = &connect;
    } else {
        CHECK(request_id == 283u);
        disconnect.resp = status;
        response.payload_case = RPC__PAYLOAD_RESP_WIFI_DISCONNECT;
        response.resp_wifi_disconnect = &disconnect;
    }
    return pack_rpc(&response, bytes, capacity);
}

static void check_status(uint32_t request_id, int32_t expected)
{
    uint8_t bytes[128];
    size_t size;
    int32_t status = INT32_C(0x13572468);
    Rpc__PayloadCase payload_case;
    Rpc *reference;
    struct p4_hosted_rpc envelope;

    size = pack_status_response(request_id, TEST_UID, expected,
                                bytes, sizeof bytes);
    CHECK(size != 0u);
    payload_case = request_id == 284u ? RPC__PAYLOAD_RESP_WIFI_SET_CONFIG :
        request_id == 282u ? RPC__PAYLOAD_RESP_WIFI_CONNECT :
                             RPC__PAYLOAD_RESP_WIFI_DISCONNECT;
    reference = unpack_rpc(bytes, size, 2u, request_id + 256u, TEST_UID,
                           payload_case);
    CHECK(p4_hosted_rpc_decode(bytes, size, &envelope));
    CHECK(p4_hosted_association_status_response(&envelope, request_id,
            TEST_UID, &status));
    CHECK(status == expected);
    if (request_id == 284u)
        CHECK(reference->resp_wifi_set_config->resp == expected);
    else if (request_id == 282u)
        CHECK(reference->resp_wifi_connect->resp == expected);
    else
        CHECK(reference->resp_wifi_disconnect->resp == expected);
    rpc__free_unpacked(reference, NULL);
}

static size_t encode_raw_envelope(uint32_t type, uint32_t id, uint32_t uid,
                                  const uint8_t *payload, size_t payload_size,
                                  uint8_t *bytes, size_t capacity)
{
    struct p4_hosted_rpc rpc;
    size_t written = 0u;

    rpc.msg_type = type;
    rpc.msg_id = id;
    rpc.uid = uid;
    rpc.payload = payload;
    rpc.payload_size = payload_size;
    if (!p4_hosted_rpc_encode(bytes, capacity, &rpc, &written))
        return 0u;
    return written;
}

static void check_raw_status(const uint8_t *payload, size_t payload_size,
                             int accepted, int32_t expected)
{
    uint8_t wire[TEST_WIRE_CAPACITY];
    size_t wire_size;
    struct p4_hosted_rpc envelope;
    int32_t status = INT32_C(0x24681357);

    wire_size = encode_raw_envelope(P4_HOSTED_RPC_TYPE_RESPONSE, 538u,
            TEST_UID, payload, payload_size, wire, sizeof wire);
    CHECK(wire_size != 0u);
    CHECK(p4_hosted_rpc_decode(wire, wire_size, &envelope));
    if (accepted) {
        CHECK(p4_hosted_association_status_response(&envelope, 282u,
                TEST_UID, &status));
        CHECK(status == expected);
    } else {
        CHECK(!p4_hosted_association_status_response(&envelope, 282u,
                TEST_UID, &status));
        CHECK(status == INT32_C(0x24681357));
    }
}

static void test_status_responses(void)
{
    static const int32_t statuses[] = { INT32_MIN, -1, 0, 1, INT32_MAX };
    static const uint8_t unknown_field[] = { 0x08u, 0x01u, 0x10u, 0x07u };
    static const uint8_t duplicate_status[] = {
        0x08u, 0x01u, 0x08u, 0x02u
    };
    static const uint8_t wrong_wire[] = { 0x0au, 0x00u };
    static const uint8_t truncated_varint[] = { 0x08u, 0x80u };
    uint8_t bytes[TEST_WIRE_CAPACITY];
    uint8_t oversized[ P4_HOSTED_ASSOCIATION_MAX_BODY_SIZE + 1u ];
    size_t size, index;
    uint32_t request_id;
    struct p4_hosted_rpc envelope, wrong;
    int32_t output, before;

    for (request_id = 282u; request_id <= 284u; ++request_id)
        for (index = 0; index < sizeof statuses / sizeof statuses[0]; ++index)
            check_status(request_id, statuses[index]);

    check_raw_status(unknown_field, sizeof unknown_field, 1, 1);
    check_raw_status(duplicate_status, sizeof duplicate_status, 0, 0);
    check_raw_status(wrong_wire, sizeof wrong_wire, 0, 0);
    check_raw_status(truncated_varint, sizeof truncated_varint, 0, 0);
    memset(oversized, 0, sizeof oversized);
    /* Repeated bounded unknown varints make an otherwise valid oversized body. */
    for (index = 0; index < sizeof oversized; ++index)
        oversized[index] = 0x10u;
    check_raw_status(oversized, sizeof oversized, 0, 0);

    size = pack_status_response(282u, TEST_UID, 0, bytes, sizeof bytes);
    CHECK(size != 0u);
    CHECK(!p4_hosted_rpc_decode(bytes, size - 1u, &envelope));
    CHECK(p4_hosted_rpc_decode(bytes, size, &envelope));
    output = INT32_C(0x76543210);
    before = output;
    wrong = envelope;
    wrong.msg_type = P4_HOSTED_RPC_TYPE_REQUEST;
    CHECK(!p4_hosted_association_status_response(&wrong, 282u,
            TEST_UID, &output));
    CHECK(output == before);
    wrong = envelope;
    wrong.msg_id = 539u;
    CHECK(!p4_hosted_association_status_response(&wrong, 282u,
            TEST_UID, &output));
    CHECK(output == before);
    wrong = envelope;
    wrong.uid = TEST_UID + 1u;
    CHECK(!p4_hosted_association_status_response(&wrong, 282u,
            TEST_UID, &output));
    CHECK(output == before);
    CHECK(!p4_hosted_association_status_response(&envelope, 281u,
            TEST_UID, &output));
    CHECK(output == before);
    CHECK(!p4_hosted_association_status_response(&envelope, 282u,
            0u, &output));
    CHECK(output == before);
}

static void fill_event_bytes(uint8_t *ssid, uint8_t *bssid, uint8_t seed)
{
    fill_pattern(ssid, P4_HOSTED_ASSOCIATION_SSID_SIZE, seed);
    fill_pattern(bssid, P4_HOSTED_ASSOCIATION_BSSID_SIZE,
                 (uint8_t)(seed + 0x43u));
}

static size_t pack_connected_event(uint32_t ssid_len, int32_t status,
                                   uint8_t *bytes, size_t capacity)
{
    uint8_t ssid[P4_HOSTED_ASSOCIATION_SSID_SIZE];
    uint8_t bssid[P4_HOSTED_ASSOCIATION_BSSID_SIZE];
    WifiEventStaConnected data = WIFI_EVENT_STA_CONNECTED__INIT;
    RpcEventStaConnected event = RPC__EVENT__STA_CONNECTED__INIT;
    Rpc rpc = RPC__INIT;

    fill_event_bytes(ssid, bssid, 0x25u);
    data.ssid.data = ssid;
    data.ssid.len = sizeof ssid;
    data.ssid_len = ssid_len;
    data.bssid.data = bssid;
    data.bssid.len = sizeof bssid;
    data.channel = UINT32_MAX;
    data.authmode = INT32_MIN;
    data.aid = -17;
    event.resp = status;
    event.sta_connected = &data;
    rpc.msg_type = RPC_TYPE__Event;
    rpc.msg_id = RPC_ID__Event_StaConnected;
    rpc.payload_case = RPC__PAYLOAD_EVENT_STA_CONNECTED;
    rpc.event_sta_connected = &event;
    return pack_rpc(&rpc, bytes, capacity);
}

static size_t pack_disconnected_event(uint32_t ssid_len, int32_t status,
                                      uint8_t *bytes, size_t capacity)
{
    uint8_t ssid[P4_HOSTED_ASSOCIATION_SSID_SIZE];
    uint8_t bssid[P4_HOSTED_ASSOCIATION_BSSID_SIZE];
    WifiEventStaDisconnected data = WIFI_EVENT_STA_DISCONNECTED__INIT;
    RpcEventStaDisconnected event = RPC__EVENT__STA_DISCONNECTED__INIT;
    Rpc rpc = RPC__INIT;

    fill_event_bytes(ssid, bssid, 0x62u);
    data.ssid.data = ssid;
    data.ssid.len = sizeof ssid;
    data.ssid_len = ssid_len;
    data.bssid.data = bssid;
    data.bssid.len = sizeof bssid;
    data.reason = UINT32_MAX;
    data.rssi = INT32_MIN;
    event.resp = status;
    event.sta_disconnected = &data;
    rpc.msg_type = RPC_TYPE__Event;
    rpc.msg_id = RPC_ID__Event_StaDisconnected;
    rpc.payload_case = RPC__PAYLOAD_EVENT_STA_DISCONNECTED;
    rpc.event_sta_disconnected = &event;
    return pack_rpc(&rpc, bytes, capacity);
}

static size_t pack_status_only_event(uint32_t id, int32_t status,
                                     uint8_t *bytes, size_t capacity)
{
    RpcEventStaConnected connected = RPC__EVENT__STA_CONNECTED__INIT;
    RpcEventStaDisconnected disconnected = RPC__EVENT__STA_DISCONNECTED__INIT;
    Rpc rpc = RPC__INIT;

    rpc.msg_type = RPC_TYPE__Event;
    rpc.msg_id = (RpcId)id;
    if (id == 775u) {
        connected.resp = status;
        rpc.payload_case = RPC__PAYLOAD_EVENT_STA_CONNECTED;
        rpc.event_sta_connected = &connected;
    } else {
        CHECK(id == 776u);
        disconnected.resp = status;
        rpc.payload_case = RPC__PAYLOAD_EVENT_STA_DISCONNECTED;
        rpc.event_sta_disconnected = &disconnected;
    }
    return pack_rpc(&rpc, bytes, capacity);
}

static void test_connected_event(void)
{
    uint8_t bytes[512];
    size_t size;
    Rpc *reference;
    struct p4_hosted_rpc envelope;
    struct p4_hosted_sta_connected_event result, before;
    uint8_t expected_ssid[P4_HOSTED_ASSOCIATION_SSID_SIZE];
    uint8_t expected_bssid[P4_HOSTED_ASSOCIATION_BSSID_SIZE];

    fill_event_bytes(expected_ssid, expected_bssid, 0x25u);
    size = pack_connected_event(P4_HOSTED_ASSOCIATION_SSID_SIZE, 0,
                                bytes, sizeof bytes);
    CHECK(size != 0u);
    reference = unpack_rpc(bytes, size, 3u, 775u, 0u,
                           RPC__PAYLOAD_EVENT_STA_CONNECTED);
    CHECK(reference->event_sta_connected->sta_connected != NULL);
    CHECK(reference->event_sta_connected->sta_connected->ssid.len ==
          P4_HOSTED_ASSOCIATION_SSID_SIZE);
    CHECK(reference->event_sta_connected->sta_connected->ssid_len ==
          P4_HOSTED_ASSOCIATION_SSID_SIZE);
    CHECK(p4_hosted_rpc_decode(bytes, size, &envelope));
    CHECK(envelope.msg_type == P4_HOSTED_RPC_TYPE_EVENT);
    CHECK(envelope.msg_id == 775u);
    CHECK(envelope.uid == 0u);
    memset(&result, 0xa5, sizeof result);
    CHECK(p4_hosted_association_connected_event(&envelope, &result));
    CHECK(result.status == 0);
    CHECK(result.ssid_length == P4_HOSTED_ASSOCIATION_SSID_SIZE);
    CHECK(memcmp(result.ssid, expected_ssid, sizeof expected_ssid) == 0);
    CHECK(memcmp(result.bssid, expected_bssid, sizeof expected_bssid) == 0);
    CHECK(result.channel == UINT32_MAX);
    CHECK(result.authmode == INT32_MIN);
    CHECK(result.aid == -17);

    /* The typed event owns fixed-size arrays, not the borrowed RPC storage. */
    memset(bytes, 0xa5, size);
    rpc__free_unpacked(reference, NULL);
    CHECK(result.ssid_length == P4_HOSTED_ASSOCIATION_SSID_SIZE);
    CHECK(memcmp(result.ssid, expected_ssid, sizeof expected_ssid) == 0);
    CHECK(memcmp(result.bssid, expected_bssid, sizeof expected_bssid) == 0);

    size = pack_connected_event(0u, 0, bytes, sizeof bytes);
    CHECK(size != 0u);
    CHECK(p4_hosted_rpc_decode(bytes, size, &envelope));
    CHECK(p4_hosted_association_connected_event(&envelope, &result));
    CHECK(result.ssid_length == 0u);
    CHECK(memcmp(result.ssid, expected_ssid, sizeof expected_ssid) == 0);

    /* Remote event errors may omit nested STA data; the value output remains
     * deterministic and contains no stale bytes from its previous value. */
    size = pack_connected_event(0u, -7, bytes, sizeof bytes);
    CHECK(size != 0u);
    CHECK(p4_hosted_rpc_decode(bytes, size, &envelope));
    memset(&result, 0x5c, sizeof result);
    CHECK(p4_hosted_association_connected_event(&envelope, &result));
    CHECK(result.status == -7);
    CHECK(result.ssid_length == 0u);
    CHECK(result.channel == 0u);
    CHECK(result.authmode == 0);
    CHECK(result.aid == 0);
    memset(&before, 0, sizeof before);
    CHECK(memcmp(result.ssid, before.ssid, sizeof result.ssid) == 0);
    CHECK(memcmp(result.bssid, before.bssid, sizeof result.bssid) == 0);

    size = pack_status_only_event(775u, -9, bytes, sizeof bytes);
    CHECK(size != 0u);
    CHECK(p4_hosted_rpc_decode(bytes, size, &envelope));
    CHECK(p4_hosted_association_connected_event(&envelope, &result));
    CHECK(result.status == -9);
    CHECK(result.ssid_length == 0u);
    CHECK(memcmp(result.ssid, before.ssid, sizeof result.ssid) == 0);
    CHECK(memcmp(result.bssid, before.bssid, sizeof result.bssid) == 0);
}

static void test_disconnected_event(void)
{
    uint8_t bytes[512];
    size_t size;
    Rpc *reference;
    struct p4_hosted_rpc envelope;
    struct p4_hosted_sta_disconnected_event result;
    uint8_t expected_ssid[P4_HOSTED_ASSOCIATION_SSID_SIZE];
    uint8_t expected_bssid[P4_HOSTED_ASSOCIATION_BSSID_SIZE];

    fill_event_bytes(expected_ssid, expected_bssid, 0x62u);
    size = pack_disconnected_event(P4_HOSTED_ASSOCIATION_SSID_SIZE, 0,
                                   bytes, sizeof bytes);
    CHECK(size != 0u);
    reference = unpack_rpc(bytes, size, 3u, 776u, 0u,
                           RPC__PAYLOAD_EVENT_STA_DISCONNECTED);
    CHECK(reference->event_sta_disconnected->sta_disconnected != NULL);
    CHECK(reference->event_sta_disconnected->sta_disconnected->ssid.len ==
          P4_HOSTED_ASSOCIATION_SSID_SIZE);
    CHECK(p4_hosted_rpc_decode(bytes, size, &envelope));
    CHECK(envelope.msg_type == P4_HOSTED_RPC_TYPE_EVENT);
    CHECK(envelope.msg_id == 776u);
    CHECK(envelope.uid == 0u);
    CHECK(p4_hosted_association_disconnected_event(&envelope, &result));
    CHECK(result.status == 0);
    CHECK(result.ssid_length == P4_HOSTED_ASSOCIATION_SSID_SIZE);
    CHECK(memcmp(result.ssid, expected_ssid, sizeof expected_ssid) == 0);
    CHECK(memcmp(result.bssid, expected_bssid, sizeof expected_bssid) == 0);
    CHECK(result.reason == UINT32_MAX);
    CHECK(result.rssi == INT32_MIN);
    memset(bytes, 0x96, size);
    rpc__free_unpacked(reference, NULL);
    CHECK(memcmp(result.ssid, expected_ssid, sizeof expected_ssid) == 0);
    CHECK(memcmp(result.bssid, expected_bssid, sizeof expected_bssid) == 0);

    size = pack_disconnected_event(0u, -19, bytes, sizeof bytes);
    CHECK(size != 0u);
    CHECK(p4_hosted_rpc_decode(bytes, size, &envelope));
    memset(&result, 0xff, sizeof result);
    CHECK(p4_hosted_association_disconnected_event(&envelope, &result));
    CHECK(result.status == -19);
    CHECK(result.ssid_length == 0u);
    CHECK(result.reason == 0u);
    CHECK(result.rssi == 0);
    {
        uint8_t zero_ssid[P4_HOSTED_ASSOCIATION_SSID_SIZE] = { 0 };
        uint8_t zero_bssid[P4_HOSTED_ASSOCIATION_BSSID_SIZE] = { 0 };
        CHECK(memcmp(result.ssid, zero_ssid, sizeof zero_ssid) == 0);
        CHECK(memcmp(result.bssid, zero_bssid, sizeof zero_bssid) == 0);
    }

    size = pack_status_only_event(776u, -21, bytes, sizeof bytes);
    CHECK(size != 0u);
    CHECK(p4_hosted_rpc_decode(bytes, size, &envelope));
    CHECK(p4_hosted_association_disconnected_event(&envelope, &result));
    CHECK(result.status == -21);
    CHECK(result.ssid_length == 0u);
    CHECK(result.reason == 0u);
    CHECK(result.rssi == 0);
}

static size_t put_varint(uint8_t *bytes, size_t capacity, size_t offset,
                         uint64_t value)
{
    while (value >= 0x80u) {
        if (offset >= capacity)
            return SIZE_MAX;
        bytes[offset++] = (uint8_t)(value | 0x80u);
        value >>= 7;
    }
    if (offset >= capacity)
        return SIZE_MAX;
    bytes[offset++] = (uint8_t)value;
    return offset;
}

static size_t put_key(uint8_t *bytes, size_t capacity, size_t offset,
                      uint32_t field, uint32_t wire_type)
{
    return put_varint(bytes, capacity, offset,
                      ((uint64_t)field << 3) | wire_type);
}

static size_t put_bytes_field(uint8_t *bytes, size_t capacity, size_t offset,
                              uint32_t field, const uint8_t *value,
                              size_t value_size)
{
    offset = put_key(bytes, capacity, offset, field, 2u);
    if (offset == SIZE_MAX)
        return SIZE_MAX;
    offset = put_varint(bytes, capacity, offset, value_size);
    if (offset == SIZE_MAX || value_size > capacity - offset)
        return SIZE_MAX;
    memcpy(bytes + offset, value, value_size);
    return offset + value_size;
}

static size_t make_raw_event(uint32_t id, int32_t status,
                             const uint8_t *ssid, size_t ssid_field_size,
                             uint32_t ssid_len, const uint8_t *bssid,
                             size_t bssid_field_size, int duplicate_ssid_len,
                             int truncate_nested, uint8_t *wire,
                             size_t wire_capacity)
{
    uint8_t nested[256], event_body[320];
    size_t nested_size = 0u, event_size = 0u;

    CHECK(id == 775u || id == 776u);

    nested_size = put_bytes_field(nested, sizeof nested, nested_size, 1u,
                                  ssid, ssid_field_size);
    if (nested_size == SIZE_MAX)
        return 0u;
    nested_size = put_varint(nested, sizeof nested,
            put_key(nested, sizeof nested, nested_size, 2u, 0u), ssid_len);
    if (nested_size == SIZE_MAX)
        return 0u;
    if (duplicate_ssid_len) {
        size_t duplicate = put_key(nested, sizeof nested, nested_size, 2u, 0u);
        nested_size = put_varint(nested, sizeof nested, duplicate, ssid_len);
        if (nested_size == SIZE_MAX)
            return 0u;
    }
    nested_size = put_bytes_field(nested, sizeof nested, nested_size, 3u,
                                  bssid, bssid_field_size);
    if (nested_size == SIZE_MAX)
        return 0u;
    /* Unknown nested fields are legal and must not disturb known fields. */
    nested_size = put_varint(nested, sizeof nested,
            put_key(nested, sizeof nested, nested_size, 20u, 0u), 77u);
    if (nested_size == SIZE_MAX)
        return 0u;
    if (truncate_nested) {
        /* Corrupt SSID's length so it crosses the enclosing message boundary. */
        if (nested_size < 2u)
            return 0u;
        nested[1] = 0x7fu;
    }
    event_size = put_varint(event_body, sizeof event_body,
            put_key(event_body, sizeof event_body, event_size, 1u, 0u),
            (uint64_t)(int64_t)status);
    if (event_size == SIZE_MAX)
        return 0u;
    event_size = put_bytes_field(event_body, sizeof event_body,
            event_size, 2u, nested, nested_size);
    if (event_size == SIZE_MAX)
        return 0u;

    /* p4_hosted_rpc_encode supplies the outer payload oneof tag itself. */
    return encode_raw_envelope(P4_HOSTED_RPC_TYPE_EVENT, id, 0u,
            event_body, event_size, wire, wire_capacity);
}

static void test_event_refusals(void)
{
    uint8_t wire[TEST_WIRE_CAPACITY];
    uint8_t ssid[P4_HOSTED_ASSOCIATION_SSID_SIZE];
    uint8_t bssid[P4_HOSTED_ASSOCIATION_BSSID_SIZE];
    uint8_t short_ssid[P4_HOSTED_ASSOCIATION_SSID_SIZE - 1u];
    uint8_t short_bssid[P4_HOSTED_ASSOCIATION_BSSID_SIZE - 1u];
    size_t size;
    struct p4_hosted_rpc envelope, wrong;
    struct p4_hosted_sta_connected_event connected, connected_before;
    struct p4_hosted_sta_disconnected_event disconnected, disconnected_before;

    fill_event_bytes(ssid, bssid, 0x73u);
    fill_pattern(short_ssid, sizeof short_ssid, 0x19u);
    fill_pattern(short_bssid, sizeof short_bssid, 0xa1u);
    memset(&connected, 0x5a, sizeof connected);
    connected_before = connected;
    size = make_raw_event(775u, 0, ssid, sizeof short_ssid,
            sizeof short_ssid, bssid, sizeof bssid, 0, 0,
            wire, sizeof wire);
    CHECK(size != 0u);
    CHECK(p4_hosted_rpc_decode(wire, size, &envelope));
    CHECK(!p4_hosted_association_connected_event(&envelope, &connected));
    CHECK(memcmp(&connected, &connected_before, sizeof connected) == 0);

    size = make_raw_event(776u, 0, ssid, sizeof ssid,
            P4_HOSTED_ASSOCIATION_SSID_SIZE + 1u,
            bssid, sizeof bssid, 0, 0, wire, sizeof wire);
    CHECK(size != 0u);
    CHECK(p4_hosted_rpc_decode(wire, size, &envelope));
    memset(&disconnected, 0x5a, sizeof disconnected);
    disconnected_before = disconnected;
    CHECK(!p4_hosted_association_disconnected_event(&envelope,
            &disconnected));
    CHECK(memcmp(&disconnected, &disconnected_before,
                 sizeof disconnected) == 0);

    size = make_raw_event(775u, 0, ssid, sizeof ssid,
            P4_HOSTED_ASSOCIATION_SSID_SIZE, short_bssid, sizeof short_bssid,
            0, 0, wire, sizeof wire);
    CHECK(size != 0u);
    CHECK(p4_hosted_rpc_decode(wire, size, &envelope));
    CHECK(!p4_hosted_association_connected_event(&envelope, &connected));
    CHECK(memcmp(&connected, &connected_before, sizeof connected) == 0);

    size = make_raw_event(776u, 0, ssid, sizeof ssid,
            P4_HOSTED_ASSOCIATION_SSID_SIZE, bssid, sizeof bssid,
            1, 0, wire, sizeof wire);
    CHECK(size != 0u);
    CHECK(p4_hosted_rpc_decode(wire, size, &envelope));
    CHECK(!p4_hosted_association_disconnected_event(&envelope,
            &disconnected));
    CHECK(memcmp(&disconnected, &disconnected_before,
                 sizeof disconnected) == 0);

    size = make_raw_event(775u, 0, ssid, sizeof ssid,
            P4_HOSTED_ASSOCIATION_SSID_SIZE, bssid, sizeof bssid,
            0, 1, wire, sizeof wire);
    CHECK(size != 0u);
    CHECK(p4_hosted_rpc_decode(wire, size, &envelope));
    CHECK(!p4_hosted_association_connected_event(&envelope, &connected));
    CHECK(memcmp(&connected, &connected_before, sizeof connected) == 0);

    /* A bounded unknown field in the nested STA event is ignored. */
    size = make_raw_event(775u, 0, ssid, sizeof ssid,
            7u, bssid, sizeof bssid, 0, 0, wire, sizeof wire);
    CHECK(size != 0u);
    CHECK(p4_hosted_rpc_decode(wire, size, &envelope));
    CHECK(p4_hosted_association_connected_event(&envelope, &connected));
    CHECK(connected.ssid_length == 7u);
    CHECK(memcmp(connected.ssid, ssid, sizeof ssid) == 0);
    connected_before = connected;

    /* Response/event envelope correlation remains strict. */
    size = pack_connected_event(32u, 0, wire, sizeof wire);
    CHECK(size != 0u);
    CHECK(p4_hosted_rpc_decode(wire, size, &envelope));
    wrong = envelope;
    wrong.msg_id = 776u;
    CHECK(!p4_hosted_association_connected_event(&wrong, &connected));
    CHECK(memcmp(&connected, &connected_before, sizeof connected) == 0);
    wrong = envelope;
    wrong.msg_type = P4_HOSTED_RPC_TYPE_RESPONSE;
    CHECK(!p4_hosted_association_connected_event(&wrong, &connected));
    CHECK(memcmp(&connected, &connected_before, sizeof connected) == 0);
    wrong = envelope;
    wrong.uid = 1u;
    CHECK(!p4_hosted_association_connected_event(&wrong, &connected));
    CHECK(memcmp(&connected, &connected_before, sizeof connected) == 0);
}

/* Use the pinned generated producer, not a second hand-written encoder. */
static void test_wifi_lifecycle_event(void)
{
    static const int32_t ids[] = {0, 2, 3, 127, INT32_MIN, INT32_MAX};
    static const int32_t statuses[] = {0, 7, -19};
    size_t i, j;
    for (i = 0; i < sizeof(ids) / sizeof(ids[0]); ++i) {
        for (j = 0; j < sizeof(statuses) / sizeof(statuses[0]); ++j) {
            Rpc rpc = RPC__INIT;
            RpcEventWifiEventNoArgs event = RPC__EVENT__WIFI_EVENT_NO_ARGS__INIT;
            struct p4_hosted_rpc envelope;
            struct p4_hosted_wifi_event decoded;
            uint8_t bytes[TEST_WIRE_CAPACITY];
            size_t size;
            event.resp = statuses[j];
            event.event_id = ids[i];
            rpc.msg_type = RPC_TYPE__Event;
            rpc.msg_id = RPC_ID__Event_WifiEventNoArgs;
            rpc.uid = 0;
            rpc.payload_case = RPC__PAYLOAD_EVENT_WIFI_EVENT_NO_ARGS;
            rpc.event_wifi_event_no_args = &event;
            size = pack_rpc(&rpc, bytes, sizeof(bytes));
            CHECK(size != 0);
            CHECK(p4_hosted_rpc_decode(bytes, size, &envelope));
            CHECK(p4_hosted_association_wifi_event(&envelope, &decoded));
            CHECK(decoded.status == statuses[j]);
            CHECK(decoded.event_id == (statuses[j] ? 0 : ids[i]));
        }
    }
}

int main(void)
{
    test_set_config_requests();
    test_empty_association_requests();
    test_status_responses();
    test_connected_event();
    test_disconnected_event();
    test_event_refusals();
    test_wifi_lifecycle_event();
    printf("hosted_association_reference: %u semantic checks passed\n", checks);
    return 0;
}
