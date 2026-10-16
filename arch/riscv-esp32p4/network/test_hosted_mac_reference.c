/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
/* Host-only GetMAC interoperability oracle. The pinned generated protobuf-c
 * codec is linked only into this test, never into the native driver. */
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hosted_mac.h"
#include "esp_hosted_rpc.pb-c.h"

static unsigned checks;

#define CHECK(expression) do { \
    ++checks; \
    if (!(expression)) { \
        fprintf(stderr, "reference line %u: %s\n", \
                (unsigned)__LINE__, #expression); \
        exit(1); \
    } \
} while (0)

static int is_valid_mac(const uint8_t *mac, size_t size)
{
    size_t i;
    uint8_t aggregate = 0;

    if (size != P4_HOSTED_MAC_ADDRESS_SIZE || (mac[0] & 1u) != 0)
        return 0;
    for (i = 0; i < size; ++i)
        aggregate |= mac[i];
    return aggregate != 0;
}

static void check_request(uint32_t uid)
{
    Rpc request = RPC__INIT;
    RpcReqGetMacAddress body = RPC__REQ__GET_MAC_ADDRESS__INIT;
    uint8_t reference_bytes[128], native_bytes[128];
    size_t reference_size, native_size = 0;
    Rpc *unpacked;

    request.msg_type = RPC_TYPE__Req;
    request.msg_id = RPC_ID__Req_GetMACAddress;
    request.uid = uid;
    request.payload_case = RPC__PAYLOAD_REQ_GET_MAC_ADDRESS;
    request.req_get_mac_address = &body;

    reference_size = rpc__get_packed_size(&request);
    CHECK(reference_size <= sizeof reference_bytes);
    CHECK(rpc__pack(&request, reference_bytes) == reference_size);
    CHECK(p4_hosted_mac_request(uid, native_bytes, sizeof native_bytes,
                               &native_size));
    CHECK(native_size == reference_size);
    CHECK(memcmp(native_bytes, reference_bytes, native_size) == 0);

    unpacked = rpc__unpack(NULL, native_size, native_bytes);
    CHECK(unpacked != NULL);
    CHECK(unpacked->msg_type == RPC_TYPE__Req);
    CHECK(unpacked->msg_id == RPC_ID__Req_GetMACAddress);
    CHECK(unpacked->uid == uid);
    CHECK(unpacked->payload_case == RPC__PAYLOAD_REQ_GET_MAC_ADDRESS);
    CHECK(unpacked->req_get_mac_address->mode == 0);
    rpc__free_unpacked(unpacked, NULL);
}

static void check_response(uint32_t uid, int32_t status,
                           const uint8_t *mac, size_t mac_size)
{
    Rpc response = RPC__INIT;
    RpcRespGetMacAddress body = RPC__RESP__GET_MAC_ADDRESS__INIT;
    uint8_t body_bytes[128], reference_bytes[256], native_bytes[256];
    size_t body_size, reference_size, native_size = 0;
    struct p4_hosted_rpc decoded;
    struct p4_hosted_mac result;
    Rpc *unpacked;

    body.mac.data = (uint8_t *)mac;
    body.mac.len = mac_size;
    body.resp = status;
    response.msg_type = RPC_TYPE__Resp;
    response.msg_id = RPC_ID__Resp_GetMACAddress;
    response.uid = uid;
    response.payload_case = RPC__PAYLOAD_RESP_GET_MAC_ADDRESS;
    response.resp_get_mac_address = &body;

    body_size = protobuf_c_message_get_packed_size(&body.base);
    CHECK(body_size <= sizeof body_bytes);
    CHECK(protobuf_c_message_pack(&body.base, body_bytes) == body_size);
    reference_size = rpc__get_packed_size(&response);
    CHECK(reference_size <= sizeof reference_bytes);
    CHECK(rpc__pack(&response, reference_bytes) == reference_size);

    {
        struct p4_hosted_rpc input;
        input.msg_type = P4_HOSTED_RPC_TYPE_RESPONSE;
        input.msg_id = 513u;
        input.uid = uid;
        input.payload = body_bytes;
        input.payload_size = body_size;
        CHECK(p4_hosted_rpc_encode(native_bytes, sizeof native_bytes,
                                   &input, &native_size));
        CHECK(native_size == reference_size);
        CHECK(memcmp(native_bytes, reference_bytes, native_size) == 0);
    }

    CHECK(p4_hosted_rpc_decode(reference_bytes, reference_size, &decoded));
    CHECK(decoded.msg_type == P4_HOSTED_RPC_TYPE_RESPONSE);
    CHECK(decoded.msg_id == 513u && decoded.uid == uid);
    CHECK(decoded.payload_size == body_size);
    CHECK(body_size == 0 ||
          memcmp(decoded.payload, body_bytes, body_size) == 0);
    CHECK(p4_hosted_mac_response(&decoded, uid, &result));
    CHECK(result.status == status);
    CHECK(result.valid_mac ==
          ((status == 0 && is_valid_mac(mac, mac_size)) ? 1u : 0u));
    if (mac_size == P4_HOSTED_MAC_ADDRESS_SIZE) {
        CHECK(memcmp(result.mac, mac, mac_size) == 0);
    } else {
        static const uint8_t zero[P4_HOSTED_MAC_ADDRESS_SIZE] = { 0 };
        CHECK(memcmp(result.mac, zero, sizeof zero) == 0);
    }

    unpacked = rpc__unpack(NULL, native_size, native_bytes);
    CHECK(unpacked != NULL);
    CHECK(unpacked->msg_type == RPC_TYPE__Resp);
    CHECK(unpacked->msg_id == RPC_ID__Resp_GetMACAddress);
    CHECK(unpacked->uid == uid);
    CHECK(unpacked->payload_case == RPC__PAYLOAD_RESP_GET_MAC_ADDRESS);
    CHECK(unpacked->resp_get_mac_address->resp == status);
    CHECK(unpacked->resp_get_mac_address->mac.len == mac_size);
    if (mac_size != 0)
        CHECK(memcmp(unpacked->resp_get_mac_address->mac.data,
                     mac, mac_size) == 0);
    rpc__free_unpacked(unpacked, NULL);
}

int main(void)
{
    static const uint32_t uids[] = { 1u, 127u, 128u, 16383u, UINT32_MAX };
    static const int32_t statuses[] = {
        0, -1, INT32_MIN, INT32_MAX, 0x300a
    };
    static const uint8_t good_mac[P4_HOSTED_MAC_ADDRESS_SIZE] = {
        0x10, 0xbd, 0xa3, 0x9b, 0xbc, 0x34
    };
    static const uint8_t multicast_mac[P4_HOSTED_MAC_ADDRESS_SIZE] = {
        0x11, 0xbd, 0xa3, 0x9b, 0xbc, 0x34
    };
    static const uint8_t zero_mac[P4_HOSTED_MAC_ADDRESS_SIZE] = { 0 };
    uint8_t unusual_mac[7] = { 0x10, 1, 2, 3, 4, 5, 6 };
    size_t i, j;

    for (i = 0; i < sizeof uids / sizeof *uids; ++i) {
        check_request(uids[i]);
        for (j = 0; j < sizeof statuses / sizeof *statuses; ++j)
            check_response(uids[i], statuses[j], good_mac, sizeof good_mac);
        check_response(uids[i], 0, NULL, 0u);
        check_response(uids[i], 0, good_mac, 5u);
        check_response(uids[i], 0, unusual_mac, sizeof unusual_mac);
        check_response(uids[i], 0, multicast_mac, sizeof multicast_mac);
        check_response(uids[i], 0, zero_mac, sizeof zero_mac);
    }

    printf("PASS: pinned GetMAC protobuf interoperability, %u checks\n",
           checks);
    return 0;
}
