/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
/* Independent OTA codec oracle against the pinned generated protobuf-c RPC. */
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hosted_ota.h"
#include "esp_hosted_rpc.pb-c.h"

static unsigned checks;
static unsigned requests_compared;
static unsigned responses_compared;
static unsigned unknown_responses;
static unsigned mismatch_probes;

#define CHECK(expression) do { \
    ++checks; \
    if (!(expression)) { \
        fprintf(stderr, "OTA reference line %u: %s\n", \
                (unsigned)__LINE__, #expression); \
        exit(1); \
    } \
} while (0)

enum ota_operation {
    OTA_BEGIN,
    OTA_WRITE,
    OTA_END
};

static uint32_t request_id(enum ota_operation operation)
{
    switch (operation) {
    case OTA_BEGIN:
        return (uint32_t)RPC_ID__Req_OTABegin;
    case OTA_WRITE:
        return (uint32_t)RPC_ID__Req_OTAWrite;
    case OTA_END:
        return (uint32_t)RPC_ID__Req_OTAEnd;
    }
    return 0u;
}

static uint32_t response_id(enum ota_operation operation)
{
    switch (operation) {
    case OTA_BEGIN:
        return (uint32_t)RPC_ID__Resp_OTABegin;
    case OTA_WRITE:
        return (uint32_t)RPC_ID__Resp_OTAWrite;
    case OTA_END:
        return (uint32_t)RPC_ID__Resp_OTAEnd;
    }
    return 0u;
}

static Rpc__PayloadCase request_payload_case(enum ota_operation operation)
{
    switch (operation) {
    case OTA_BEGIN:
        return RPC__PAYLOAD_REQ_OTA_BEGIN;
    case OTA_WRITE:
        return RPC__PAYLOAD_REQ_OTA_WRITE;
    case OTA_END:
        return RPC__PAYLOAD_REQ_OTA_END;
    }
    return RPC__PAYLOAD__NOT_SET;
}

static Rpc__PayloadCase response_payload_case(enum ota_operation operation)
{
    switch (operation) {
    case OTA_BEGIN:
        return RPC__PAYLOAD_RESP_OTA_BEGIN;
    case OTA_WRITE:
        return RPC__PAYLOAD_RESP_OTA_WRITE;
    case OTA_END:
        return RPC__PAYLOAD_RESP_OTA_END;
    }
    return RPC__PAYLOAD__NOT_SET;
}

static int encode_native_request(enum ota_operation operation, uint32_t uid,
                                 const uint8_t *data, size_t data_size,
                                 uint8_t *bytes, size_t capacity,
                                 size_t *written)
{
    switch (operation) {
    case OTA_BEGIN:
        return p4_hosted_ota_begin_request(uid, bytes, capacity, written);
    case OTA_WRITE:
        return p4_hosted_ota_write_request(uid, data, data_size, bytes,
                                            capacity, written);
    case OTA_END:
        return p4_hosted_ota_end_request(uid, bytes, capacity, written);
    }
    return 0;
}

static void check_native_request_semantics(const uint8_t *bytes, size_t size,
                                           enum ota_operation operation,
                                           uint32_t uid, const uint8_t *data,
                                           size_t data_size)
{
    Rpc *decoded = rpc__unpack(NULL, size, bytes);

    CHECK(decoded != NULL);
    CHECK(decoded->msg_type == RPC_TYPE__Req);
    CHECK((uint32_t)decoded->msg_id == request_id(operation));
    CHECK(decoded->uid == uid);
    CHECK(decoded->payload_case == request_payload_case(operation));

    switch (operation) {
    case OTA_BEGIN:
        CHECK(decoded->req_ota_begin != NULL);
        CHECK(decoded->req_ota_begin->base.n_unknown_fields == 0u);
        break;
    case OTA_WRITE:
        CHECK(decoded->req_ota_write != NULL);
        CHECK(decoded->req_ota_write->ota_data.len == data_size);
        CHECK(data_size == 0u ||
              memcmp(decoded->req_ota_write->ota_data.data,
                     data, data_size) == 0);
        break;
    case OTA_END:
        CHECK(decoded->req_ota_end != NULL);
        CHECK(decoded->req_ota_end->base.n_unknown_fields == 0u);
        break;
    }

    rpc__free_unpacked(decoded, NULL);
}

static void check_request_pair(enum ota_operation operation, uint32_t uid,
                               const uint8_t *data, size_t data_size)
{
    Rpc request = RPC__INIT;
    RpcReqOTABegin begin_body = RPC__REQ__OTABEGIN__INIT;
    RpcReqOTAWrite write_body = RPC__REQ__OTAWRITE__INIT;
    RpcReqOTAEnd end_body = RPC__REQ__OTAEND__INIT;
    const ProtobufCMessage *body;
    uint8_t native_bytes[P4_HOSTED_OTA_MAX_REQUEST_SIZE];
    uint8_t reference_bytes[P4_HOSTED_OTA_MAX_REQUEST_SIZE];
    size_t native_size = 0u;
    size_t reference_size;

    request.msg_type = RPC_TYPE__Req;
    request.msg_id = (RpcId)request_id(operation);
    request.uid = uid;
    switch (operation) {
    case OTA_BEGIN:
        request.payload_case = RPC__PAYLOAD_REQ_OTA_BEGIN;
        request.req_ota_begin = &begin_body;
        body = &begin_body.base;
        break;
    case OTA_WRITE:
        write_body.ota_data.data = (uint8_t *)(uintptr_t)data;
        write_body.ota_data.len = data_size;
        request.payload_case = RPC__PAYLOAD_REQ_OTA_WRITE;
        request.req_ota_write = &write_body;
        body = &write_body.base;
        break;
    case OTA_END:
        request.payload_case = RPC__PAYLOAD_REQ_OTA_END;
        request.req_ota_end = &end_body;
        body = &end_body.base;
        break;
    default:
        CHECK(0);
        return;
    }

    reference_size = rpc__get_packed_size(&request);
    CHECK(reference_size <= sizeof reference_bytes);
    CHECK(reference_size <= P4_HOSTED_OTA_MAX_REQUEST_SIZE);
    CHECK(protobuf_c_message_get_packed_size(body) <= reference_size);
    CHECK(rpc__pack(&request, reference_bytes) == reference_size);

    CHECK(encode_native_request(operation, uid, data, data_size,
                                native_bytes, sizeof native_bytes,
                                &native_size));
    CHECK(native_size == reference_size);
    /* The oracle is the independent pinned rpc__pack byte stream. */
    CHECK(memcmp(native_bytes, reference_bytes, reference_size) == 0);
    check_native_request_semantics(native_bytes, native_size, operation,
                                   uid, data, data_size);
    ++requests_compared;
}

static int32_t unpacked_status(const Rpc *rpc, enum ota_operation operation)
{
    switch (operation) {
    case OTA_BEGIN:
        CHECK(rpc->resp_ota_begin != NULL);
        return rpc->resp_ota_begin->resp;
    case OTA_WRITE:
        CHECK(rpc->resp_ota_write != NULL);
        return rpc->resp_ota_write->resp;
    case OTA_END:
        CHECK(rpc->resp_ota_end != NULL);
        return rpc->resp_ota_end->resp;
    }
    CHECK(0);
    return 0;
}

static void expect_native_mismatch(const struct p4_hosted_rpc *rpc,
                                   uint32_t expected_request_id,
                                   uint32_t expected_uid)
{
    int32_t status = INT32_C(0x5a5a1234);

    CHECK(!p4_hosted_ota_status_response(rpc, expected_request_id,
                                         expected_uid, &status));
    CHECK(status == INT32_C(0x5a5a1234));
    ++mismatch_probes;
}

static void check_status_pair(enum ota_operation operation, uint32_t uid,
                              int32_t expected_status, int add_unknown)
{
    static uint8_t unknown_value[] = { 0x81u, 0x01u };
    ProtobufCMessageUnknownField unknown = {
        7u, PROTOBUF_C_WIRE_TYPE_VARINT,
        sizeof unknown_value, unknown_value
    };
    Rpc response = RPC__INIT;
    RpcRespOTABegin begin_body = RPC__RESP__OTABEGIN__INIT;
    RpcRespOTAWrite write_body = RPC__RESP__OTAWRITE__INIT;
    RpcRespOTAEnd end_body = RPC__RESP__OTAEND__INIT;
    ProtobufCMessage *body;
    uint8_t reference_bytes[256];
    size_t reference_size;
    struct p4_hosted_rpc native_envelope;
    Rpc *reference_decoded;
    int32_t actual_status = INT32_C(0x12345678);

    response.msg_type = RPC_TYPE__Resp;
    response.msg_id = (RpcId)response_id(operation);
    response.uid = uid;
    switch (operation) {
    case OTA_BEGIN:
        begin_body.resp = expected_status;
        response.payload_case = RPC__PAYLOAD_RESP_OTA_BEGIN;
        response.resp_ota_begin = &begin_body;
        body = &begin_body.base;
        break;
    case OTA_WRITE:
        write_body.resp = expected_status;
        response.payload_case = RPC__PAYLOAD_RESP_OTA_WRITE;
        response.resp_ota_write = &write_body;
        body = &write_body.base;
        break;
    case OTA_END:
        end_body.resp = expected_status;
        response.payload_case = RPC__PAYLOAD_RESP_OTA_END;
        response.resp_ota_end = &end_body;
        body = &end_body.base;
        break;
    default:
        CHECK(0);
        return;
    }
    if (add_unknown) {
        body->n_unknown_fields = 1u;
        body->unknown_fields = &unknown;
    }

    reference_size = rpc__get_packed_size(&response);
    CHECK(reference_size <= sizeof reference_bytes);
    CHECK(rpc__pack(&response, reference_bytes) == reference_size);
    reference_decoded = rpc__unpack(NULL, reference_size, reference_bytes);
    CHECK(reference_decoded != NULL);
    CHECK(reference_decoded->msg_type == RPC_TYPE__Resp);
    CHECK((uint32_t)reference_decoded->msg_id == response_id(operation));
    CHECK(reference_decoded->uid == uid);
    CHECK(reference_decoded->payload_case == response_payload_case(operation));
    CHECK(unpacked_status(reference_decoded, operation) == expected_status);
    if (add_unknown) {
        ProtobufCMessage *decoded_body;
        switch (operation) {
        case OTA_BEGIN:
            decoded_body = &reference_decoded->resp_ota_begin->base;
            break;
        case OTA_WRITE:
            decoded_body = &reference_decoded->resp_ota_write->base;
            break;
        case OTA_END:
            decoded_body = &reference_decoded->resp_ota_end->base;
            break;
        default:
            CHECK(0);
            decoded_body = NULL;
            break;
        }
        CHECK(decoded_body->n_unknown_fields == 1u);
        CHECK(decoded_body->unknown_fields[0].tag == 7u);
        CHECK(decoded_body->unknown_fields[0].wire_type ==
              PROTOBUF_C_WIRE_TYPE_VARINT);
    }
    rpc__free_unpacked(reference_decoded, NULL);

    CHECK(p4_hosted_rpc_decode(reference_bytes, reference_size,
                               &native_envelope));
    CHECK(native_envelope.msg_type == P4_HOSTED_RPC_TYPE_RESPONSE);
    CHECK(native_envelope.msg_id == response_id(operation));
    CHECK(native_envelope.uid == uid);
    CHECK(p4_hosted_ota_status_response(&native_envelope,
                                        request_id(operation), uid,
                                        &actual_status));
    CHECK(actual_status == expected_status);
    if (add_unknown)
        ++unknown_responses;
    else
        ++responses_compared;
}

static void test_request_vectors(void)
{
    static const uint32_t uids[] = {
        1u, 127u, 128u, 16383u, 16384u, UINT32_MAX
    };
    static const size_t chunk_sizes[] = { 1u, 127u, 128u, 238u };
    uint8_t data[P4_HOSTED_OTA_MAX_WRITE_SIZE];
    size_t uid_index, size_index, i;

    for (i = 0u; i < sizeof data; ++i)
        data[i] = (uint8_t)(i * 29u + 0x53u);

    for (uid_index = 0u; uid_index < sizeof uids / sizeof uids[0];
         ++uid_index) {
        check_request_pair(OTA_BEGIN, uids[uid_index], NULL, 0u);
        check_request_pair(OTA_END, uids[uid_index], NULL, 0u);
        for (size_index = 0u;
             size_index < sizeof chunk_sizes / sizeof chunk_sizes[0];
             ++size_index) {
            size_t chunk_size = chunk_sizes[size_index];
            check_request_pair(OTA_WRITE, uids[uid_index], data, chunk_size);
        }
    }
}

static void test_status_vectors(void)
{
    static const uint32_t uids[] = {
        1u, 127u, 128u, 16383u, 16384u, UINT32_MAX
    };
    static const int32_t statuses[] = {
        0, -1, INT32_MIN, INT32_MAX
    };
    size_t uid_index, status_index;
    enum ota_operation operation;

    for (uid_index = 0u; uid_index < sizeof uids / sizeof uids[0];
         ++uid_index) {
        for (operation = OTA_BEGIN; operation <= OTA_END; ++operation) {
            for (status_index = 0u;
                 status_index < sizeof statuses / sizeof statuses[0];
                 ++status_index) {
                check_status_pair(operation, uids[uid_index],
                                  statuses[status_index], 0);
            }
        }
    }

    /* An unknown field in the original generated schema remains skippable. */
    check_status_pair(OTA_WRITE, UINT32_MAX, 0x3456, 1);
}

static void test_response_mismatches(void)
{
    Rpc response = RPC__INIT;
    RpcRespOTAWrite body = RPC__RESP__OTAWRITE__INIT;
    uint8_t bytes[128];
    size_t size;
    struct p4_hosted_rpc decoded, bad;

    body.resp = 0;
    response.msg_type = RPC_TYPE__Resp;
    response.msg_id = RPC_ID__Resp_OTAWrite;
    response.uid = UINT32_MAX;
    response.payload_case = RPC__PAYLOAD_RESP_OTA_WRITE;
    response.resp_ota_write = &body;
    size = rpc__get_packed_size(&response);
    CHECK(size <= sizeof bytes);
    CHECK(rpc__pack(&response, bytes) == size);
    CHECK(p4_hosted_rpc_decode(bytes, size, &decoded));
    {
        int32_t status = -1;
        CHECK(p4_hosted_ota_status_response(&decoded,
                                             RPC_ID__Req_OTAWrite,
                                             UINT32_MAX, &status));
        CHECK(status == 0);
    }

    bad = decoded;
    bad.msg_type = P4_HOSTED_RPC_TYPE_REQUEST;
    expect_native_mismatch(&bad, RPC_ID__Req_OTAWrite, UINT32_MAX);
    bad = decoded;
    bad.msg_id = RPC_ID__Resp_OTABegin;
    expect_native_mismatch(&bad, RPC_ID__Req_OTAWrite, UINT32_MAX);
    bad = decoded;
    bad.uid = UINT32_MAX - 1u;
    expect_native_mismatch(&bad, RPC_ID__Req_OTAWrite, UINT32_MAX);
    expect_native_mismatch(&decoded, RPC_ID__Req_OTAWrite, UINT32_MAX - 1u);
    expect_native_mismatch(&decoded, RPC_ID__Req_OTABegin, UINT32_MAX);
    expect_native_mismatch(&decoded, 271u, UINT32_MAX);
}

int main(void)
{
    test_request_vectors();
    test_status_vectors();
    test_response_mismatches();

    printf("PASS: pinned OTA protobuf-c oracle: %u checks, %u requests, "
           "%u responses, %u unknown-field responses, %u mismatch probes\n",
           checks, requests_compared, responses_compared,
           unknown_responses, mismatch_probes);
    return 0;
}
