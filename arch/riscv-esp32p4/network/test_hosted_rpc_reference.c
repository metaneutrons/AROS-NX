/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
/* Host-only interoperability oracle. The pinned generated protobuf-c codec
 * is linked only into this test, never copied into the native driver. */
#include "hosted_rpc.h"
#include "esp_hosted_rpc.pb-c.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(value) do { ++checks; if (!(value)) { \
    fprintf(stderr, "reference line %d: %s\n", __LINE__, #value); \
    exit(1); } } while (0)

static void check_message(Rpc *reference, const ProtobufCMessage *body)
{
    uint8_t body_bytes[128], reference_bytes[256], native_bytes[256];
    size_t body_size, reference_size, native_size = 0;
    struct p4_hosted_rpc input, decoded;
    Rpc *unpacked;

    body_size = protobuf_c_message_get_packed_size(body);
    CHECK(body_size <= sizeof body_bytes);
    CHECK(protobuf_c_message_pack(body, body_bytes) == body_size);
    reference_size = rpc__get_packed_size(reference);
    CHECK(reference_size <= sizeof reference_bytes);
    CHECK(rpc__pack(reference, reference_bytes) == reference_size);
    input.msg_type = (uint32_t)reference->msg_type;
    input.msg_id = (uint32_t)reference->msg_id;
    input.uid = reference->uid;
    input.payload = body_bytes;
    input.payload_size = body_size;

    CHECK(p4_hosted_rpc_encode(native_bytes, sizeof native_bytes,
                               &input, &native_size));
    CHECK(native_size == reference_size);
    CHECK(memcmp(native_bytes, reference_bytes, native_size) == 0);
    CHECK(p4_hosted_rpc_decode(reference_bytes, reference_size, &decoded));
    CHECK(decoded.msg_type == input.msg_type && decoded.msg_id == input.msg_id);
    CHECK(decoded.uid == input.uid && decoded.payload_size == body_size);
    CHECK(body_size == 0 || memcmp(decoded.payload, body_bytes, body_size) == 0);
    unpacked = rpc__unpack(NULL, native_size, native_bytes);
    CHECK(unpacked != NULL);
    CHECK(unpacked->msg_type == reference->msg_type);
    CHECK(unpacked->msg_id == reference->msg_id && unpacked->uid == reference->uid);
    CHECK(unpacked->payload_case == reference->payload_case);
    rpc__free_unpacked(unpacked, NULL);
}

int main(void)
{
    static const uint32_t uids[] = {1, 127, 128, 16383, 16384, UINT32_MAX};
    static const int32_t modes[] = {0, 1, -1};
    static const int32_t statuses[] = {0, -1, 0x300a};
    uint8_t mac[] = {0x10, 0xbd, 0xa3, 0x9b, 0xbc, 0x34};
    uint8_t event_data[] = {0x00, 0x80, 0xff};
    size_t uid, value, event_length;

    for (uid = 0; uid < sizeof uids / sizeof *uids; ++uid) {
        for (value = 0; value < sizeof modes / sizeof *modes; ++value) {
            Rpc request = RPC__INIT;
            RpcReqGetMacAddress body = RPC__REQ__GET_MAC_ADDRESS__INIT;
            body.mode = modes[value];
            request.msg_type = RPC_TYPE__Req;
            request.msg_id = RPC_ID__Req_GetMACAddress;
            request.uid = uids[uid];
            request.payload_case = RPC__PAYLOAD_REQ_GET_MAC_ADDRESS;
            request.req_get_mac_address = &body;
            check_message(&request, &body.base);
        }
        for (value = 0; value < sizeof statuses / sizeof *statuses; ++value) {
            Rpc response = RPC__INIT;
            RpcRespGetMacAddress body = RPC__RESP__GET_MAC_ADDRESS__INIT;
            body.mac.data = mac;
            body.mac.len = sizeof mac;
            body.resp = statuses[value];
            response.msg_type = RPC_TYPE__Resp;
            response.msg_id = RPC_ID__Resp_GetMACAddress;
            response.uid = uids[uid];
            response.payload_case = RPC__PAYLOAD_RESP_GET_MAC_ADDRESS;
            response.resp_get_mac_address = &body;
            check_message(&response, &body.base);
        }
    }
    for (event_length = 0; event_length <= sizeof event_data; ++event_length) {
        Rpc event = RPC__INIT;
        RpcEventESPInit body = RPC__EVENT__ESPINIT__INIT;
        body.init_data.data = event_data;
        body.init_data.len = event_length;
        event.msg_type = RPC_TYPE__Event;
        event.msg_id = RPC_ID__Event_ESPInit;
        event.payload_case = RPC__PAYLOAD_EVENT_ESP_INIT;
        event.event_esp_init = &body;
        check_message(&event, &body.base);
    }
    printf("PASS: pinned RPC interoperability, %u checks, 40 messages\n", checks);
    return 0;
}
