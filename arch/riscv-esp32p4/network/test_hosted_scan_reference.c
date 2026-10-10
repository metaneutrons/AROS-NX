/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
/* Semantic oracle against the pinned generated protobuf-c schema/runtime. */
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hosted_scan.h"
#include "esp_hosted_rpc.pb-c.h"

static unsigned checks;

#define CHECK(expression) do { \
    ++checks; \
    if (!(expression)) { \
        fprintf(stderr, "scan reference line %u: %s\n", \
                (unsigned)__LINE__, #expression); \
        exit(1); \
    } \
} while (0)

#define TEST_UID UINT32_C(0x7a6b5c4d)
#define TEST_WIRE_CAPACITY 4096u

static size_t pack_rpc(Rpc *rpc, uint8_t *bytes, size_t capacity)
{
    size_t size = rpc__get_packed_size(rpc);

    if (size > capacity)
        return 0u;
    CHECK(rpc__pack(rpc, bytes) == size);
    return size;
}

static void check_outer(const uint8_t *bytes, size_t size, uint32_t type,
                        uint32_t id, uint32_t uid, size_t expected_body_size)
{
    struct p4_hosted_rpc envelope;

    CHECK(p4_hosted_rpc_decode(bytes, size, &envelope));
    CHECK(envelope.msg_type == type);
    CHECK(envelope.msg_id == id);
    CHECK(envelope.uid == uid);
    CHECK(envelope.payload_size == expected_body_size);
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

static void check_scan_start_request(unsigned block)
{
    RpcReqWifiScanStart body = RPC__REQ__WIFI_SCAN_START__INIT;
    Rpc request = RPC__INIT;
    Rpc *decoded;
    struct p4_hosted_rpc envelope;
    const uint8_t *expected_body;
    size_t expected_body_size;
    uint8_t native[128], reference[128];
    size_t native_size = 0u, reference_size;

    body.block = (protobuf_c_boolean)block;
    body.config_set = 0;
    request.msg_type = RPC_TYPE__Req;
    request.msg_id = RPC_ID__Req_WifiScanStart;
    request.uid = TEST_UID;
    request.payload_case = RPC__PAYLOAD_REQ_WIFI_SCAN_START;
    request.req_wifi_scan_start = &body;
    reference_size = pack_rpc(&request, reference, sizeof reference);
    CHECK(reference_size != 0u);

    CHECK(p4_hosted_scan_start_request(TEST_UID, block, native,
                                      sizeof native, &native_size));
    CHECK(native_size != 0u);
    CHECK(p4_hosted_rpc_decode(native, native_size, &envelope));
    CHECK(envelope.msg_type == P4_HOSTED_RPC_TYPE_REQUEST);
    CHECK(envelope.msg_id == 286u);
    CHECK(envelope.uid == TEST_UID);
    CHECK(envelope.payload_size == (block ? 4u : 2u));
    if (block) {
        static const uint8_t blocking_body[] = { 0x10u, 0x01u, 0x18u, 0x00u };
        expected_body = blocking_body;
        expected_body_size = sizeof blocking_body;
    } else {
        static const uint8_t nonblocking_body[] = { 0x18u, 0x00u };
        expected_body = nonblocking_body;
        expected_body_size = sizeof nonblocking_body;
    }
    CHECK(envelope.payload_size == expected_body_size);
    CHECK(memcmp(envelope.payload, expected_body, expected_body_size) == 0);

    decoded = unpack_rpc(native, native_size, 1u, 286u, TEST_UID,
                         RPC__PAYLOAD_REQ_WIFI_SCAN_START);
    CHECK(decoded->req_wifi_scan_start != NULL);
    CHECK(decoded->req_wifi_scan_start->config == NULL);
    CHECK(decoded->req_wifi_scan_start->block == (protobuf_c_boolean)block);
    CHECK(decoded->req_wifi_scan_start->config_set == 0);
    rpc__free_unpacked(decoded, NULL);

    /* protobuf-c omits proto3 zero fields. Decoding both encodings must still
     * yield the same NULL config, requested block mode and config_set value. */
    decoded = unpack_rpc(reference, reference_size, 1u, 286u, TEST_UID,
                         RPC__PAYLOAD_REQ_WIFI_SCAN_START);
    CHECK(decoded->req_wifi_scan_start != NULL);
    CHECK(decoded->req_wifi_scan_start->config == NULL);
    CHECK(decoded->req_wifi_scan_start->block == (protobuf_c_boolean)block);
    CHECK(decoded->req_wifi_scan_start->config_set == 0);
    rpc__free_unpacked(decoded, NULL);
}

static void check_empty_request(uint32_t id, uint32_t uid,
                                Rpc__PayloadCase payload_case)
{
    RpcReqWifiScanStop stop = RPC__REQ__WIFI_SCAN_STOP__INIT;
    RpcReqWifiScanGetApNum get_num = RPC__REQ__WIFI_SCAN_GET_AP_NUM__INIT;
    Rpc request = RPC__INIT;
    Rpc *decoded;
    uint8_t native[128], reference[128];
    size_t native_size = 0u, reference_size;

    request.msg_type = RPC_TYPE__Req;
    request.msg_id = (RpcId)id;
    request.uid = uid;
    request.payload_case = payload_case;
    if (id == 287u)
        request.req_wifi_scan_stop = &stop;
    else {
        CHECK(id == 288u);
        request.req_wifi_scan_get_ap_num = &get_num;
    }
    reference_size = pack_rpc(&request, reference, sizeof reference);
    CHECK(reference_size != 0u);

    if (id == 287u)
        CHECK(p4_hosted_scan_stop_request(uid, native, sizeof native,
                                          &native_size));
    else
        CHECK(p4_hosted_scan_num_request(uid, native, sizeof native,
                                         &native_size));
    CHECK(native_size == reference_size);
    CHECK(memcmp(native, reference, native_size) == 0);
    check_outer(native, native_size, P4_HOSTED_RPC_TYPE_REQUEST, id, uid, 0u);
    decoded = unpack_rpc(native, native_size, 1u, id, uid, payload_case);
    rpc__free_unpacked(decoded, NULL);
}

static void check_records_request(uint16_t number)
{
    RpcReqWifiScanGetApRecords body =
        RPC__REQ__WIFI_SCAN_GET_AP_RECORDS__INIT;
    Rpc request = RPC__INIT;
    Rpc *decoded;
    uint8_t native[128], reference[128];
    size_t native_size = 0u, reference_size;

    body.number = number;
    request.msg_type = RPC_TYPE__Req;
    request.msg_id = RPC_ID__Req_WifiScanGetApRecords;
    request.uid = TEST_UID;
    request.payload_case = RPC__PAYLOAD_REQ_WIFI_SCAN_GET_AP_RECORDS;
    request.req_wifi_scan_get_ap_records = &body;
    reference_size = pack_rpc(&request, reference, sizeof reference);
    CHECK(reference_size != 0u);
    CHECK(p4_hosted_scan_records_request(TEST_UID, number, native,
                                         sizeof native, &native_size));
    CHECK(native_size == reference_size);
    CHECK(memcmp(native, reference, native_size) == 0);
    check_outer(native, native_size, P4_HOSTED_RPC_TYPE_REQUEST, 289u,
                TEST_UID, 2u);
    decoded = unpack_rpc(native, native_size, 1u, 289u, TEST_UID,
                         RPC__PAYLOAD_REQ_WIFI_SCAN_GET_AP_RECORDS);
    CHECK(decoded->req_wifi_scan_get_ap_records != NULL);
    CHECK(decoded->req_wifi_scan_get_ap_records->number == (int32_t)number);
    rpc__free_unpacked(decoded, NULL);
}

static void test_requests(void)
{
    uint8_t bytes[128], before[128];
    size_t written, before_written;
    uint16_t number;

    check_scan_start_request(0u);
    check_scan_start_request(1u);
    check_empty_request(287u, TEST_UID,
                        RPC__PAYLOAD_REQ_WIFI_SCAN_STOP);
    check_empty_request(288u, TEST_UID,
                        RPC__PAYLOAD_REQ_WIFI_SCAN_GET_AP_NUM);
    for (number = 1u; number <= P4_HOSTED_SCAN_MAX_RECORDS; ++number)
        check_records_request(number);

    memset(bytes, 0xa7, sizeof bytes);
    memcpy(before, bytes, sizeof before);
    written = 0x1234u;
    before_written = written;
    CHECK(!p4_hosted_scan_start_request(TEST_UID, 2u, bytes, sizeof bytes,
                                        &written));
    CHECK(memcmp(bytes, before, sizeof bytes) == 0);
    CHECK(written == before_written);
    CHECK(!p4_hosted_scan_start_request(0u, 0u, bytes, sizeof bytes,
                                        &written));
    CHECK(memcmp(bytes, before, sizeof bytes) == 0);
    CHECK(written == before_written);
    CHECK(!p4_hosted_scan_records_request(TEST_UID, 0u, bytes, sizeof bytes,
                                          &written));
    CHECK(!p4_hosted_scan_records_request(TEST_UID,
            P4_HOSTED_SCAN_MAX_RECORDS + 1u, bytes, sizeof bytes, &written));
    CHECK(memcmp(bytes, before, sizeof bytes) == 0);
    CHECK(written == before_written);
}

static size_t pack_status_response(uint32_t request_id, uint32_t uid,
                                   int32_t status, uint8_t *bytes,
                                   size_t capacity)
{
    RpcRespWifiScanStart start = RPC__RESP__WIFI_SCAN_START__INIT;
    RpcRespWifiScanStop stop = RPC__RESP__WIFI_SCAN_STOP__INIT;
    Rpc response = RPC__INIT;

    response.msg_type = RPC_TYPE__Resp;
    response.msg_id = (RpcId)(request_id + 256u);
    response.uid = uid;
    if (request_id == 286u) {
        start.resp = status;
        response.payload_case = RPC__PAYLOAD_RESP_WIFI_SCAN_START;
        response.resp_wifi_scan_start = &start;
    } else {
        CHECK(request_id == 287u);
        stop.resp = status;
        response.payload_case = RPC__PAYLOAD_RESP_WIFI_SCAN_STOP;
        response.resp_wifi_scan_stop = &stop;
    }
    return pack_rpc(&response, bytes, capacity);
}

static int32_t reference_status(const Rpc *rpc, uint32_t request_id)
{
    switch (request_id) {
    case 286u: return rpc->resp_wifi_scan_start->resp;
    case 287u: return rpc->resp_wifi_scan_stop->resp;
    default: CHECK(0); return 0;
    }
}

static void check_status_response(uint32_t request_id, int32_t status)
{
    uint8_t bytes[128];
    size_t size;
    int32_t actual = INT32_C(0x13572468);
    struct p4_hosted_rpc envelope;
    Rpc__PayloadCase payload_case = request_id == 286u ?
        RPC__PAYLOAD_RESP_WIFI_SCAN_START : RPC__PAYLOAD_RESP_WIFI_SCAN_STOP;
    Rpc *reference;

    size = pack_status_response(request_id, TEST_UID, status,
                                 bytes, sizeof bytes);
    CHECK(size != 0u);
    reference = unpack_rpc(bytes, size, 2u, request_id + 256u, TEST_UID,
                           payload_case);
    CHECK(reference_status(reference, request_id) == status);
    CHECK(p4_hosted_rpc_decode(bytes, size, &envelope));
    CHECK(envelope.msg_type == P4_HOSTED_RPC_TYPE_RESPONSE);
    CHECK(envelope.msg_id == request_id + 256u);
    CHECK(envelope.uid == TEST_UID);
    CHECK(p4_hosted_scan_status_response(&envelope, request_id, TEST_UID,
                                         &actual));
    CHECK(actual == status);
    if (status == 0)
        CHECK(envelope.payload_size == 0u);
    rpc__free_unpacked(reference, NULL);

    if (status == INT32_MIN) {
        struct p4_hosted_rpc wrong = envelope;
        int32_t unchanged = INT32_C(0x24681357);
        wrong.msg_type = P4_HOSTED_RPC_TYPE_REQUEST;
        CHECK(!p4_hosted_scan_status_response(&wrong, request_id, TEST_UID,
                                              &unchanged));
        CHECK(unchanged == INT32_C(0x24681357));
        wrong = envelope;
        wrong.msg_id = request_id + 257u;
        CHECK(!p4_hosted_scan_status_response(&wrong, request_id, TEST_UID,
                                              &unchanged));
        wrong = envelope;
        wrong.uid = TEST_UID + 1u;
        CHECK(!p4_hosted_scan_status_response(&wrong, request_id, TEST_UID,
                                              &unchanged));
        CHECK(!p4_hosted_scan_status_response(&envelope, request_id + 1u,
                                              TEST_UID, &unchanged));
        CHECK(unchanged == INT32_C(0x24681357));
    }
}

static void test_status_responses(void)
{
    static const int32_t statuses[] = {
        INT32_MIN, -1, 0, 1, INT32_MAX
    };
    size_t index;
    uint32_t request_id;

    for (request_id = 286u; request_id <= 287u; ++request_id)
        for (index = 0; index < sizeof statuses / sizeof statuses[0]; ++index)
            check_status_response(request_id, statuses[index]);
}

static size_t pack_num_response(uint32_t uid, int32_t status, int32_t number,
                                uint8_t *bytes, size_t capacity)
{
    RpcRespWifiScanGetApNum body =
        RPC__RESP__WIFI_SCAN_GET_AP_NUM__INIT;
    Rpc response = RPC__INIT;

    body.resp = status;
    body.number = number;
    response.msg_type = RPC_TYPE__Resp;
    response.msg_id = (RpcId)544u;
    response.uid = uid;
    response.payload_case = RPC__PAYLOAD_RESP_WIFI_SCAN_GET_AP_NUM;
    response.resp_wifi_scan_get_ap_num = &body;
    return pack_rpc(&response, bytes, capacity);
}

static void check_num_response(int32_t status, int32_t number, int accepted)
{
    uint8_t bytes[128];
    size_t size;
    struct p4_hosted_rpc envelope;
    Rpc *reference;
    struct p4_hosted_scan_count actual;
    struct p4_hosted_scan_count before;

    size = pack_num_response(TEST_UID, status, number, bytes, sizeof bytes);
    CHECK(size != 0u);
    reference = unpack_rpc(bytes, size, 2u, 544u, TEST_UID,
                           RPC__PAYLOAD_RESP_WIFI_SCAN_GET_AP_NUM);
    CHECK(reference->resp_wifi_scan_get_ap_num != NULL);
    CHECK(reference->resp_wifi_scan_get_ap_num->resp == status);
    CHECK(reference->resp_wifi_scan_get_ap_num->number == number);
    CHECK(p4_hosted_rpc_decode(bytes, size, &envelope));
    CHECK(envelope.msg_type == P4_HOSTED_RPC_TYPE_RESPONSE);
    CHECK(envelope.msg_id == 544u);
    CHECK(envelope.uid == TEST_UID);
    memset(&actual, 0x69, sizeof actual);
    before = actual;
    if (accepted) {
        CHECK(p4_hosted_scan_num_response(&envelope, TEST_UID, &actual));
        CHECK(actual.status == status);
        CHECK(actual.number == (uint16_t)number);
    } else {
        CHECK(!p4_hosted_scan_num_response(&envelope, TEST_UID, &actual));
        CHECK(memcmp(&actual, &before, sizeof actual) == 0);
    }
    rpc__free_unpacked(reference, NULL);

    if (number == 13) {
        struct p4_hosted_rpc wrong = envelope;
        memset(&actual, 0x69, sizeof actual);
        before = actual;
        wrong.msg_id = 545u;
        CHECK(!p4_hosted_scan_num_response(&wrong, TEST_UID, &actual));
        CHECK(memcmp(&actual, &before, sizeof actual) == 0);
        wrong = envelope;
        wrong.uid = TEST_UID + 1u;
        CHECK(!p4_hosted_scan_num_response(&wrong, TEST_UID, &actual));
        wrong = envelope;
        wrong.msg_type = P4_HOSTED_RPC_TYPE_REQUEST;
        CHECK(!p4_hosted_scan_num_response(&wrong, TEST_UID, &actual));
        CHECK(memcmp(&actual, &before, sizeof actual) == 0);
    }
}

static void test_num_responses(void)
{
    check_num_response(0, 0, 1);
    check_num_response(INT32_MIN, 13, 1);
    check_num_response(7, 65535, 1);
    check_num_response(0, -1, 0);
    check_num_response(0, 65536, 0);
}

struct record_fixture {
    WifiApRecord record[P4_HOSTED_SCAN_MAX_RECORDS];
    WifiCountry country[P4_HOSTED_SCAN_MAX_RECORDS];
    WifiHeApInfo he[P4_HOSTED_SCAN_MAX_RECORDS];
    uint8_t bssid[P4_HOSTED_SCAN_MAX_RECORDS][P4_HOSTED_SCAN_BSSID_SIZE];
    uint8_t ssid[P4_HOSTED_SCAN_MAX_RECORDS][P4_HOSTED_SCAN_SSID_MAX_SIZE];
    uint8_t country_cc[P4_HOSTED_SCAN_MAX_RECORDS]
                      [P4_HOSTED_SCAN_COUNTRY_CC_MAX_SIZE];
    WifiApRecord *record_ptr[P4_HOSTED_SCAN_MAX_RECORDS];
};

static void init_record_fixture(struct record_fixture *fixture)
{
    size_t index, byte;

    memset(fixture, 0, sizeof *fixture);
    for (index = 0; index < P4_HOSTED_SCAN_MAX_RECORDS; ++index) {
        fixture->record[index] = (WifiApRecord)WIFI_AP_RECORD__INIT;
        fixture->country[index] = (WifiCountry)WIFI_COUNTRY__INIT;
        fixture->he[index] = (WifiHeApInfo)WIFI_HE_AP_INFO__INIT;
        fixture->record_ptr[index] = &fixture->record[index];
        for (byte = 0; byte < P4_HOSTED_SCAN_BSSID_SIZE; ++byte)
            fixture->bssid[index][byte] = (uint8_t)(index * 11u + byte);
        fixture->bssid[index][0] = 0u;
        fixture->bssid[index][2] = 0u;
        fixture->bssid[index][5] = 0xffu;
        fixture->record[index].bssid.len = P4_HOSTED_SCAN_BSSID_SIZE;
        fixture->record[index].bssid.data = fixture->bssid[index];

        if (index == 0u) {
            for (byte = 0; byte < P4_HOSTED_SCAN_SSID_MAX_SIZE; ++byte)
                fixture->ssid[index][byte] = (uint8_t)(byte * 7u + 1u);
            fixture->ssid[index][0] = 'A';
            fixture->ssid[index][3] = 0u;
            fixture->ssid[index][17] = 0u;
            fixture->ssid[index][31] = 0xffu;
            fixture->record[index].ssid.len =
                P4_HOSTED_SCAN_SSID_MAX_SIZE;
            fixture->record[index].ssid.data = fixture->ssid[index];
            fixture->record[index].primary = 196u;
            fixture->record[index].second = INT32_MIN;
            fixture->record[index].rssi = -97;
            fixture->record[index].authmode = INT32_MAX;
            fixture->record[index].pairwise_cipher = -7;
            fixture->record[index].group_cipher = 4;
            fixture->record[index].ant = -1;
            fixture->record[index].bitmask = UINT32_C(0x800000a5);

            fixture->country_cc[index][0] = 'U';
            fixture->country_cc[index][1] = 0u;
            fixture->country_cc[index][2] = 'S';
            fixture->country[index].cc.len =
                P4_HOSTED_SCAN_COUNTRY_CC_MAX_SIZE;
            fixture->country[index].cc.data = fixture->country_cc[index];
            fixture->country[index].schan = 1u;
            fixture->country[index].nchan = 14u;
            fixture->country[index].max_tx_power = -8;
            fixture->country[index].policy = INT32_MIN;
            fixture->record[index].country = &fixture->country[index];

            fixture->he[index].bitmask = UINT32_C(0xfedcba98);
            fixture->he[index].bssid_index = UINT32_C(0x80000003);
            fixture->record[index].he_ap = &fixture->he[index];
        } else if (index == 1u) {
            /* All proto3 scalar defaults and absent optional submessages. */
            fixture->record[index].ssid.len = 0u;
            fixture->record[index].ssid.data = NULL;
        } else {
            for (byte = 0; byte < index; ++byte)
                fixture->ssid[index][byte] = (uint8_t)(0xa0u + index + byte);
            fixture->record[index].ssid.len = index;
            fixture->record[index].ssid.data = fixture->ssid[index];
            fixture->record[index].primary = (uint32_t)(index + 1u);
            fixture->record[index].second = -(int32_t)index;
            fixture->record[index].rssi = -(int32_t)(40u + index);
            fixture->record[index].authmode = (int32_t)index;
            fixture->record[index].pairwise_cipher = -(int32_t)index;
            fixture->record[index].group_cipher = (int32_t)(index + 1u);
            fixture->record[index].ant = (int32_t)index;
            fixture->record[index].bitmask =
                UINT32_C(0x10203040) + (uint32_t)index;
            if (index == 2u) {
                /* Present empty nested messages preserve zero defaults. */
                fixture->record[index].country = &fixture->country[index];
                fixture->record[index].he_ap = &fixture->he[index];
            }
        }
    }
}

static size_t pack_records_response(uint32_t uid, int32_t status,
                                    int32_t number, size_t record_count,
                                    WifiApRecord **records, uint8_t *bytes,
                                    size_t capacity)
{
    RpcRespWifiScanGetApRecords body =
        RPC__RESP__WIFI_SCAN_GET_AP_RECORDS__INIT;
    Rpc response = RPC__INIT;

    body.resp = status;
    body.number = number;
    body.n_ap_records = record_count;
    body.ap_records = records;
    response.msg_type = RPC_TYPE__Resp;
    response.msg_id = (RpcId)545u;
    response.uid = uid;
    response.payload_case = RPC__PAYLOAD_RESP_WIFI_SCAN_GET_AP_RECORDS;
    response.resp_wifi_scan_get_ap_records = &body;
    return pack_rpc(&response, bytes, capacity);
}

static void check_owned_record(const struct p4_hosted_scan_record *actual,
                               const WifiApRecord *expected)
{
    CHECK(expected->bssid.len == P4_HOSTED_SCAN_BSSID_SIZE);
    CHECK(actual->ssid_size == expected->ssid.len);
    CHECK(memcmp(actual->bssid, expected->bssid.data,
                 P4_HOSTED_SCAN_BSSID_SIZE) == 0);
    if (expected->ssid.len != 0u)
        CHECK(memcmp(actual->ssid, expected->ssid.data,
                     expected->ssid.len) == 0);
    CHECK(actual->primary == expected->primary);
    CHECK(actual->second == expected->second);
    CHECK(actual->rssi == expected->rssi);
    CHECK(actual->authmode == expected->authmode);
    CHECK(actual->pairwise_cipher == expected->pairwise_cipher);
    CHECK(actual->group_cipher == expected->group_cipher);
    CHECK(actual->ant == expected->ant);
    CHECK(actual->bitmask == expected->bitmask);
    if (expected->country) {
        CHECK(actual->country_cc_size == expected->country->cc.len);
        if (expected->country->cc.len != 0u)
            CHECK(memcmp(actual->country_cc, expected->country->cc.data,
                         expected->country->cc.len) == 0);
        CHECK(actual->country_schan == expected->country->schan);
        CHECK(actual->country_nchan == expected->country->nchan);
        CHECK(actual->country_max_tx_power ==
              expected->country->max_tx_power);
        CHECK(actual->country_policy == expected->country->policy);
    } else {
        CHECK(actual->country_cc_size == 0u);
        CHECK(actual->country_schan == 0u);
        CHECK(actual->country_nchan == 0u);
        CHECK(actual->country_max_tx_power == 0);
        CHECK(actual->country_policy == 0);
    }
    if (expected->he_ap) {
        CHECK(actual->he_bitmask == expected->he_ap->bitmask);
        CHECK(actual->he_bssid_index == expected->he_ap->bssid_index);
    } else {
        CHECK(actual->he_bitmask == 0u);
        CHECK(actual->he_bssid_index == 0u);
    }
}

static void check_reference_record(const WifiApRecord *actual,
                                   const WifiApRecord *expected)
{
    CHECK(actual->bssid.len == expected->bssid.len);
    CHECK(actual->ssid.len == expected->ssid.len);
    CHECK(memcmp(actual->bssid.data, expected->bssid.data,
                 expected->bssid.len) == 0);
    if (expected->ssid.len != 0u)
        CHECK(memcmp(actual->ssid.data, expected->ssid.data,
                     expected->ssid.len) == 0);
    CHECK(actual->primary == expected->primary);
    CHECK(actual->second == expected->second);
    CHECK(actual->rssi == expected->rssi);
    CHECK(actual->authmode == expected->authmode);
    CHECK(actual->pairwise_cipher == expected->pairwise_cipher);
    CHECK(actual->group_cipher == expected->group_cipher);
    CHECK(actual->ant == expected->ant);
    CHECK(actual->bitmask == expected->bitmask);
    CHECK((actual->country == NULL) == (expected->country == NULL));
    if (expected->country) {
        CHECK(actual->country->cc.len == expected->country->cc.len);
        if (expected->country->cc.len != 0u)
            CHECK(memcmp(actual->country->cc.data,
                         expected->country->cc.data,
                         expected->country->cc.len) == 0);
        CHECK(actual->country->schan == expected->country->schan);
        CHECK(actual->country->nchan == expected->country->nchan);
        CHECK(actual->country->max_tx_power ==
              expected->country->max_tx_power);
        CHECK(actual->country->policy == expected->country->policy);
    }
    CHECK((actual->he_ap == NULL) == (expected->he_ap == NULL));
    if (expected->he_ap) {
        CHECK(actual->he_ap->bitmask == expected->he_ap->bitmask);
        CHECK(actual->he_ap->bssid_index == expected->he_ap->bssid_index);
    }
}

static void check_records_response(size_t count, int32_t status)
{
    struct record_fixture fixture;
    struct p4_hosted_scan_records actual;
    struct p4_hosted_rpc envelope;
    Rpc *reference;
    uint8_t bytes[TEST_WIRE_CAPACITY];
    size_t size, index;

    init_record_fixture(&fixture);
    size = pack_records_response(TEST_UID, status, (int32_t)count, count,
                                 fixture.record_ptr, bytes, sizeof bytes);
    CHECK(size != 0u);
    reference = unpack_rpc(bytes, size, 2u, 545u, TEST_UID,
                           RPC__PAYLOAD_RESP_WIFI_SCAN_GET_AP_RECORDS);
    CHECK(reference->resp_wifi_scan_get_ap_records != NULL);
    CHECK(reference->resp_wifi_scan_get_ap_records->resp == status);
    CHECK(reference->resp_wifi_scan_get_ap_records->number ==
          (int32_t)count);
    CHECK(reference->resp_wifi_scan_get_ap_records->n_ap_records == count);
    for (index = 0; index < count; ++index)
        check_reference_record(
            reference->resp_wifi_scan_get_ap_records->ap_records[index],
            &fixture.record[index]);

    CHECK(p4_hosted_rpc_decode(bytes, size, &envelope));
    CHECK(envelope.msg_type == P4_HOSTED_RPC_TYPE_RESPONSE);
    CHECK(envelope.msg_id == 545u);
    CHECK(envelope.uid == TEST_UID);
    memset(&actual, 0x6c, sizeof actual);
    CHECK(p4_hosted_scan_records_response(&envelope, TEST_UID,
            (uint16_t)count, &actual));
    CHECK(actual.status == status);
    CHECK(actual.number == (uint16_t)count);
    CHECK(actual.record_count == count);
    for (index = 0; index < count; ++index)
        check_owned_record(&actual.records[index], &fixture.record[index]);

    /* The output is a value object: destroying both the serialized response
     * and protobuf-c's decoded graph cannot invalidate any returned field. */
    memset(bytes, 0xa5, size);
    rpc__free_unpacked(reference, NULL);
    CHECK(actual.status == status);
    CHECK(actual.number == (uint16_t)count);
    CHECK(actual.record_count == count);
    for (index = 0; index < count; ++index)
        check_owned_record(&actual.records[index], &fixture.record[index]);
}

static void test_records_response_boundaries_and_ownership(void)
{
    check_records_response(0u, 0);
    check_records_response(1u, INT32_MIN);
    check_records_response(P4_HOSTED_SCAN_MAX_RECORDS, 0);
}

static void test_records_response_refusals(void)
{
    struct record_fixture fixture;
    struct p4_hosted_scan_records output, before;
    struct p4_hosted_rpc envelope, wrong;
    uint8_t bytes[TEST_WIRE_CAPACITY];
    size_t size;

    init_record_fixture(&fixture);
    size = pack_records_response(TEST_UID, 0, 1, 1u,
        fixture.record_ptr, bytes, sizeof bytes);
    CHECK(size != 0u);
    CHECK(p4_hosted_rpc_decode(bytes, size, &envelope));
    memset(&output, 0x5a, sizeof output);
    before = output;
    wrong = envelope;
    wrong.msg_id = 544u;
    CHECK(!p4_hosted_scan_records_response(&wrong, TEST_UID, 1u, &output));
    CHECK(memcmp(&output, &before, sizeof output) == 0);
    wrong = envelope;
    wrong.uid = TEST_UID + 1u;
    CHECK(!p4_hosted_scan_records_response(&wrong, TEST_UID, 1u, &output));
    CHECK(memcmp(&output, &before, sizeof output) == 0);
    wrong = envelope;
    wrong.msg_type = P4_HOSTED_RPC_TYPE_REQUEST;
    CHECK(!p4_hosted_scan_records_response(&wrong, TEST_UID, 1u, &output));
    CHECK(memcmp(&output, &before, sizeof output) == 0);
    CHECK(!p4_hosted_scan_records_response(&envelope, TEST_UID, 0u, &output));
    CHECK(memcmp(&output, &before, sizeof output) == 0);
    CHECK(!p4_hosted_scan_records_response(&envelope, TEST_UID,
            P4_HOSTED_SCAN_MAX_RECORDS + 1u, &output));
    CHECK(memcmp(&output, &before, sizeof output) == 0);

    size = pack_records_response(TEST_UID, 0, -1, 0u,
        fixture.record_ptr, bytes, sizeof bytes);
    CHECK(size != 0u);
    CHECK(p4_hosted_rpc_decode(bytes, size, &envelope));
    CHECK(!p4_hosted_scan_records_response(&envelope, TEST_UID,
            P4_HOSTED_SCAN_MAX_RECORDS, &output));
    CHECK(memcmp(&output, &before, sizeof output) == 0);

    size = pack_records_response(TEST_UID, 0, 9, 0u,
        fixture.record_ptr, bytes, sizeof bytes);
    CHECK(size != 0u);
    CHECK(p4_hosted_rpc_decode(bytes, size, &envelope));
    CHECK(!p4_hosted_scan_records_response(&envelope, TEST_UID,
            P4_HOSTED_SCAN_MAX_RECORDS, &output));
    CHECK(memcmp(&output, &before, sizeof output) == 0);

    size = pack_records_response(TEST_UID, 0, 2, 1u,
        fixture.record_ptr, bytes, sizeof bytes);
    CHECK(size != 0u);
    CHECK(p4_hosted_rpc_decode(bytes, size, &envelope));
    CHECK(!p4_hosted_scan_records_response(&envelope, TEST_UID,
            P4_HOSTED_SCAN_MAX_RECORDS, &output));
    CHECK(memcmp(&output, &before, sizeof output) == 0);
}

int main(void)
{
    test_requests();
    test_status_responses();
    test_num_responses();
    test_records_response_boundaries_and_ownership();
    test_records_response_refusals();
    printf("hosted_scan_reference: %u semantic checks passed\n", checks);
    return 0;
}
