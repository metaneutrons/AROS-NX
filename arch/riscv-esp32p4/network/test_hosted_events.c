/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hosted_events.h"

static unsigned long checks;

#define CHECK(expression) do { \
    ++checks; \
    if (!(expression)) { \
        fprintf(stderr, "line %u: %s\n", (unsigned)__LINE__, #expression); \
        exit(1); \
    } \
} while (0)

static const uint8_t fixture_init[] = {
    0x0a, 0x04, 0x00, 0x11, 0x22, 0xff
};
static const uint8_t fixture_init_duplicate[] = {
    0x0a, 0x03, 0x01, 0x02, 0x03,
    0x0a, 0x02, 0xaa, 0x00
};
static const uint8_t fixture_unknown_fields[] = {
    0x80, 0x01, 0x7f,                         /* varint */
    0x89, 0x01, 0x00, 0x01, 0x02, 0x03,
    0x04, 0x05, 0x06, 0x07,                   /* fixed64 */
    0x92, 0x01, 0x02, 0x58, 0x59,             /* length-delimited */
    0x9d, 0x01, 0xaa, 0xbb, 0xcc, 0xdd        /* fixed32 */
};
static const uint8_t fixture_heartbeat[] = {
    0x08, 0x04,
    0x08, 0xf9, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0x01
};
static const uint8_t fixture_ap_connected[] = {
    0x12, 0x06, 0x02, 0x11, 0x22, 0x33, 0x44, 0x55,
    0x18, 0x01, 0x18, 0x07,
    0x20, 0x00, 0x20, 0x02
};
static const uint8_t fixture_ap_disconnected[] = {
    0x12, 0x06, 0x02, 0x11, 0x22, 0x33, 0x44, 0x55,
    0x18, 0x03, 0x20, 0x01, 0x28, 0x09
};
static const uint8_t fixture_ap_error[] = {
    0x08, 0x01,
    0x08, 0xfb, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0x01,
    0x12, 0x06, 0x02, 0x11, 0x22, 0x33, 0x44, 0x55,
    0x18, 0x07, 0x20, 0x01, 0x28, 0x09
};
static const uint8_t fixture_scan_done[] = {
    0x12, 0x04, 0x08, 0x01, 0x10, 0x02,
    0x12, 0x06, 0x08, 0x03, 0x10, 0x05, 0x18, 0x09
};
static const uint8_t fixture_scan_empty_nested[] = { 0x12, 0x00 };
static const uint8_t fixture_scan_error[] = {
    0x08, 0x02,
    0x12, 0x06, 0x08, 0x01, 0x10, 0x05, 0x18, 0x09
};

static void set_rpc(struct p4_hosted_rpc *rpc, uint32_t msg_id,
                    const uint8_t *payload, size_t payload_size)
{
    rpc->msg_type = P4_HOSTED_RPC_TYPE_EVENT;
    rpc->msg_id = msg_id;
    rpc->uid = 0u;
    rpc->payload = payload;
    rpc->payload_size = payload_size;
}

static void expect_refusal(const struct p4_hosted_rpc *rpc)
{
    struct p4_hosted_aux_event result;
    uint8_t before[sizeof result];

    memset(&result, 0xa5, sizeof result);
    memcpy(before, &result, sizeof result);
    CHECK(!p4_hosted_aux_event_decode(rpc, &result));
    CHECK(memcmp(&result, before, sizeof result) == 0);
}

static void test_init_and_unknown_fields(void)
{
    struct p4_hosted_rpc rpc;
    struct p4_hosted_aux_event result;
    uint8_t owned_fixture[] = { 0x0a, 0x03, 0x40, 0x00, 0x7f };
    uint8_t max_payload[P4_HOSTED_AUX_EVENT_MAX_PAYLOAD];
    size_t index;

    set_rpc(&rpc, P4_HOSTED_AUX_EVENT_ESP_INIT, NULL, 0u);
    memset(&result, 0xa5, sizeof result);
    CHECK(p4_hosted_aux_event_decode(&rpc, &result));
    CHECK(result.msg_id == P4_HOSTED_AUX_EVENT_ESP_INIT);
    CHECK(result.status == 0);
    CHECK(result.data.init.size == 0u);
    CHECK(result.data.init.data[0] == 0u);
    CHECK(result.data.init.data[P4_HOSTED_AUX_EVENT_INIT_DATA_SIZE - 1u] == 0u);

    set_rpc(&rpc, P4_HOSTED_AUX_EVENT_ESP_INIT,
            fixture_init, sizeof fixture_init);
    CHECK(p4_hosted_aux_event_decode(&rpc, &result));
    CHECK(result.data.init.size == 4u);
    CHECK(result.data.init.data[0] == 0x00u);
    CHECK(result.data.init.data[1] == 0x11u);
    CHECK(result.data.init.data[2] == 0x22u);
    CHECK(result.data.init.data[3] == 0xffu);

    set_rpc(&rpc, P4_HOSTED_AUX_EVENT_ESP_INIT,
            fixture_init_duplicate, sizeof fixture_init_duplicate);
    CHECK(p4_hosted_aux_event_decode(&rpc, &result));
    CHECK(result.data.init.size == 2u);
    CHECK(result.data.init.data[0] == 0xaau);
    CHECK(result.data.init.data[1] == 0x00u);
    CHECK(result.data.init.data[2] == 0u);

    set_rpc(&rpc, P4_HOSTED_AUX_EVENT_ESP_INIT,
            fixture_unknown_fields, sizeof fixture_unknown_fields);
    CHECK(p4_hosted_aux_event_decode(&rpc, &result));
    CHECK(result.data.init.size == 0u);

    set_rpc(&rpc, P4_HOSTED_AUX_EVENT_ESP_INIT,
            owned_fixture, sizeof owned_fixture);
    CHECK(p4_hosted_aux_event_decode(&rpc, &result));
    owned_fixture[2] = 0xeeu;
    CHECK(result.data.init.data[0] == 0x40u);

    max_payload[0] = 0x0au;
    max_payload[1] = 0xfdu;
    max_payload[2] = 0x01u;
    for (index = 0u; index < sizeof max_payload - 3u; ++index)
        max_payload[index + 3u] = (uint8_t)(index ^ 0x5au);
    set_rpc(&rpc, P4_HOSTED_AUX_EVENT_ESP_INIT,
            max_payload, sizeof max_payload);
    CHECK(p4_hosted_aux_event_decode(&rpc, &result));
    CHECK(result.data.init.size == sizeof max_payload - 3u);
    for (index = 0u; index < result.data.init.size; ++index)
        CHECK(result.data.init.data[index] == max_payload[index + 3u]);
}

static void test_heartbeat(void)
{
    struct p4_hosted_rpc rpc;
    struct p4_hosted_aux_event result;

    set_rpc(&rpc, P4_HOSTED_AUX_EVENT_HEARTBEAT, NULL, 0u);
    CHECK(p4_hosted_aux_event_decode(&rpc, &result));
    CHECK(result.msg_id == P4_HOSTED_AUX_EVENT_HEARTBEAT);
    CHECK(result.status == 0);
    CHECK(result.data.heartbeat.value == 0);

    set_rpc(&rpc, P4_HOSTED_AUX_EVENT_HEARTBEAT,
            fixture_heartbeat, sizeof fixture_heartbeat);
    CHECK(p4_hosted_aux_event_decode(&rpc, &result));
    CHECK(result.data.heartbeat.value == -7);
}

static void test_ap_events(void)
{
    struct p4_hosted_rpc rpc;
    struct p4_hosted_aux_event result;
    static const uint8_t expected_mac[6] = {
        0x02, 0x11, 0x22, 0x33, 0x44, 0x55
    };
    static const uint8_t status_only[] = { 0x08, 0x01 };

    set_rpc(&rpc, P4_HOSTED_AUX_EVENT_AP_STA_CONNECTED,
            fixture_ap_connected, sizeof fixture_ap_connected);
    CHECK(p4_hosted_aux_event_decode(&rpc, &result));
    CHECK(result.status == 0);
    CHECK(memcmp(result.data.ap.mac, expected_mac, sizeof expected_mac) == 0);
    CHECK(result.data.ap.aid == 7u);
    CHECK(result.data.ap.is_mesh_child == 1u);
    CHECK(result.data.ap.reason == 0u);

    set_rpc(&rpc, P4_HOSTED_AUX_EVENT_AP_STA_DISCONNECTED,
            fixture_ap_disconnected, sizeof fixture_ap_disconnected);
    CHECK(p4_hosted_aux_event_decode(&rpc, &result));
    CHECK(result.status == 0);
    CHECK(memcmp(result.data.ap.mac, expected_mac, sizeof expected_mac) == 0);
    CHECK(result.data.ap.aid == 3u);
    CHECK(result.data.ap.is_mesh_child == 1u);
    CHECK(result.data.ap.reason == 9u);

    set_rpc(&rpc, P4_HOSTED_AUX_EVENT_AP_STA_DISCONNECTED,
            fixture_ap_error, sizeof fixture_ap_error);
    CHECK(p4_hosted_aux_event_decode(&rpc, &result));
    CHECK(result.status == -5);
    CHECK(result.data.ap.aid == 0u);
    CHECK(result.data.ap.is_mesh_child == 0u);
    CHECK(result.data.ap.reason == 0u);
    CHECK(result.data.ap.mac[0] == 0u);

    set_rpc(&rpc, P4_HOSTED_AUX_EVENT_AP_STA_CONNECTED,
            status_only, sizeof status_only);
    CHECK(p4_hosted_aux_event_decode(&rpc, &result));
    CHECK(result.status == 1);
    CHECK(result.data.ap.mac[0] == 0u);

    set_rpc(&rpc, P4_HOSTED_AUX_EVENT_AP_STA_CONNECTED, NULL, 0u);
    expect_refusal(&rpc);
}

static void test_scan_events(void)
{
    struct p4_hosted_rpc rpc;
    struct p4_hosted_aux_event result;
    static const uint8_t status_only[] = { 0x08, 0x01 };
    static const uint8_t malformed_nested[] = {
        0x08, 0x01, 0x12, 0x02, 0x08, 0x80
    };

    set_rpc(&rpc, P4_HOSTED_AUX_EVENT_STA_SCAN_DONE,
            fixture_scan_done, sizeof fixture_scan_done);
    CHECK(p4_hosted_aux_event_decode(&rpc, &result));
    CHECK(result.status == 0);
    CHECK(result.data.scan.status == 3u);
    CHECK(result.data.scan.number == 5u);
    CHECK(result.data.scan.scan_id == 9u);

    set_rpc(&rpc, P4_HOSTED_AUX_EVENT_STA_SCAN_DONE,
            fixture_scan_empty_nested, sizeof fixture_scan_empty_nested);
    CHECK(p4_hosted_aux_event_decode(&rpc, &result));
    CHECK(result.data.scan.status == 0u);
    CHECK(result.data.scan.number == 0u);
    CHECK(result.data.scan.scan_id == 0u);

    set_rpc(&rpc, P4_HOSTED_AUX_EVENT_STA_SCAN_DONE,
            fixture_scan_error, sizeof fixture_scan_error);
    CHECK(p4_hosted_aux_event_decode(&rpc, &result));
    CHECK(result.status == 2);
    CHECK(result.data.scan.status == 0u);
    CHECK(result.data.scan.number == 0u);
    CHECK(result.data.scan.scan_id == 0u);

    set_rpc(&rpc, P4_HOSTED_AUX_EVENT_STA_SCAN_DONE,
            status_only, sizeof status_only);
    CHECK(p4_hosted_aux_event_decode(&rpc, &result));
    CHECK(result.status == 1);
    CHECK(result.data.scan.number == 0u);

    set_rpc(&rpc, P4_HOSTED_AUX_EVENT_STA_SCAN_DONE,
            malformed_nested, sizeof malformed_nested);
    expect_refusal(&rpc);

    set_rpc(&rpc, P4_HOSTED_AUX_EVENT_STA_SCAN_DONE, NULL, 0u);
    expect_refusal(&rpc);
}

static void test_envelope_refusals(void)
{
    struct p4_hosted_rpc rpc;

    set_rpc(&rpc, P4_HOSTED_AUX_EVENT_AP_STA_CONNECTED,
            fixture_ap_connected, sizeof fixture_ap_connected);
    rpc.msg_type = P4_HOSTED_RPC_TYPE_RESPONSE;
    expect_refusal(&rpc);
    rpc.msg_type = P4_HOSTED_RPC_TYPE_EVENT;
    rpc.uid = 1u;
    expect_refusal(&rpc);
    rpc.uid = 0u;
    rpc.msg_id = 775u;
    expect_refusal(&rpc);
    rpc.msg_id = 768u;
    expect_refusal(&rpc);
    rpc.msg_id = 777u;
    expect_refusal(&rpc);
    rpc.msg_id = P4_HOSTED_AUX_EVENT_AP_STA_CONNECTED;
    rpc.msg_type = P4_HOSTED_RPC_TYPE_REQUEST;
    expect_refusal(&rpc);
}

static void test_malformed_fields(void)
{
    struct p4_hosted_rpc rpc;
    static const uint8_t bad_init_wire[] = { 0x08, 0x01 };
    static const uint8_t bad_init_length[] = { 0x0a, 0xff, 0x01 };
    static const uint8_t bad_heartbeat_wire[] = { 0x0a, 0x00 };
    static const uint8_t bad_ap_mac_length[] = { 0x12, 0x05, 0, 1, 2, 3, 4 };
    static const uint8_t bad_ap_aid_wire[] = { 0x1a, 0x00 };
    static const uint8_t bad_ap_aid_overflow[] = {
        0x18, 0x80, 0x80, 0x80, 0x80, 0x10
    };
    static const uint8_t bad_ap_mesh_wire[] = { 0x22, 0x00 };
    static const uint8_t bad_ap_reason_wire[] = { 0x2a, 0x00 };
    static const uint8_t bad_scan_status_wire[] = { 0x0a, 0x00 };
    static const uint8_t bad_scan_nested_wire[] = {
        0x12, 0x02, 0x0a, 0x00
    };
    static const uint8_t bad_scan_overflow[] = {
        0x12, 0x06, 0x08, 0x80, 0x80, 0x80, 0x80, 0x10
    };
    static const uint8_t bad_int32_overflow[] = {
        0x08, 0x80, 0x80, 0x80, 0x80, 0x08
    };
    static const uint8_t bad_noncanonical_varint[] = { 0x08, 0x80, 0x00 };
    static const uint8_t bad_truncated_varint[] = { 0x08, 0x80 };
    static const uint8_t bad_group[] = { 0xc3, 0x01 };
    static const uint8_t bad_key_zero[] = { 0x00 };
    static const uint8_t bad_fixed64_skip[] = { 0x49, 0x00 };
    static const uint8_t bad_length_skip[] = { 0x52, 0x03, 0x00 };
    static const uint8_t bad_unknown_varint[] = { 0x50, 0x80 };
    static const uint8_t bad_wire_type[] = { 0x4f };

#define REFUSE(id, bytes) do { \
    set_rpc(&rpc, (id), (bytes), sizeof (bytes)); \
    expect_refusal(&rpc); \
} while (0)

    REFUSE(P4_HOSTED_AUX_EVENT_ESP_INIT, bad_init_wire);
    REFUSE(P4_HOSTED_AUX_EVENT_ESP_INIT, bad_init_length);
    REFUSE(P4_HOSTED_AUX_EVENT_HEARTBEAT, bad_heartbeat_wire);
    REFUSE(P4_HOSTED_AUX_EVENT_AP_STA_CONNECTED, bad_ap_mac_length);
    REFUSE(P4_HOSTED_AUX_EVENT_AP_STA_CONNECTED, bad_ap_aid_wire);
    REFUSE(P4_HOSTED_AUX_EVENT_AP_STA_CONNECTED, bad_ap_aid_overflow);
    REFUSE(P4_HOSTED_AUX_EVENT_AP_STA_CONNECTED, bad_ap_mesh_wire);
    REFUSE(P4_HOSTED_AUX_EVENT_AP_STA_DISCONNECTED, bad_ap_reason_wire);
    REFUSE(P4_HOSTED_AUX_EVENT_AP_STA_CONNECTED, bad_int32_overflow);
    REFUSE(P4_HOSTED_AUX_EVENT_STA_SCAN_DONE, bad_scan_status_wire);
    REFUSE(P4_HOSTED_AUX_EVENT_STA_SCAN_DONE, bad_scan_nested_wire);
    REFUSE(P4_HOSTED_AUX_EVENT_STA_SCAN_DONE, bad_scan_overflow);
    REFUSE(P4_HOSTED_AUX_EVENT_STA_SCAN_DONE, bad_noncanonical_varint);
    REFUSE(P4_HOSTED_AUX_EVENT_STA_SCAN_DONE, bad_truncated_varint);
    REFUSE(P4_HOSTED_AUX_EVENT_HEARTBEAT, bad_group);
    REFUSE(P4_HOSTED_AUX_EVENT_HEARTBEAT, bad_key_zero);
    REFUSE(P4_HOSTED_AUX_EVENT_HEARTBEAT, bad_fixed64_skip);
    REFUSE(P4_HOSTED_AUX_EVENT_HEARTBEAT, bad_length_skip);
    REFUSE(P4_HOSTED_AUX_EVENT_HEARTBEAT, bad_unknown_varint);
    REFUSE(P4_HOSTED_AUX_EVENT_HEARTBEAT, bad_wire_type);

#undef REFUSE
}

static void test_span_validation(void)
{
    struct p4_hosted_rpc rpc;
    struct p4_hosted_aux_event result;
    uint8_t oversized[P4_HOSTED_AUX_EVENT_MAX_PAYLOAD + 1u] = { 0 };
    uint8_t before[sizeof result];
    union {
        struct p4_hosted_rpc rpc;
        struct p4_hosted_aux_event result;
    } rpc_result_overlap;
    uint8_t rpc_overlap_before[sizeof rpc_result_overlap];
    union {
        struct p4_hosted_aux_event result;
        uint8_t bytes[sizeof(struct p4_hosted_aux_event)];
    } payload_result_overlap;
    uint8_t overlap_before[sizeof payload_result_overlap];

    memset(&result, 0x3c, sizeof result);
    memcpy(before, &result, sizeof result);
    set_rpc(&rpc, P4_HOSTED_AUX_EVENT_HEARTBEAT, oversized,
            sizeof oversized);
    CHECK(!p4_hosted_aux_event_decode(&rpc, &result));
    CHECK(memcmp(&result, before, sizeof result) == 0);

    set_rpc(&rpc, P4_HOSTED_AUX_EVENT_HEARTBEAT, oversized, 1u);
    rpc.payload = NULL;
    expect_refusal(&rpc);

    set_rpc(&rpc, P4_HOSTED_AUX_EVENT_HEARTBEAT, oversized, 4u);
    rpc.payload = (const uint8_t *)(uintptr_t)(UINTPTR_MAX - 1u);
    expect_refusal(&rpc);

    set_rpc(&rpc, P4_HOSTED_AUX_EVENT_HEARTBEAT, NULL, 0u);
    rpc.payload = (const uint8_t *)&rpc;
    rpc.payload_size = 1u;
    expect_refusal(&rpc);

    set_rpc(&rpc, P4_HOSTED_AUX_EVENT_HEARTBEAT,
            fixture_heartbeat, sizeof fixture_heartbeat);
    memset(&rpc_result_overlap, 0x6d, sizeof rpc_result_overlap);
    set_rpc(&rpc_result_overlap.rpc, P4_HOSTED_AUX_EVENT_HEARTBEAT,
            fixture_heartbeat, sizeof fixture_heartbeat);
    memcpy(rpc_overlap_before, &rpc_result_overlap,
           sizeof rpc_overlap_before);
    CHECK(!p4_hosted_aux_event_decode(&rpc_result_overlap.rpc,
                                     &rpc_result_overlap.result));
    CHECK(memcmp(&rpc_result_overlap, rpc_overlap_before,
                 sizeof rpc_overlap_before) == 0);

    memset(&payload_result_overlap, 0x7e, sizeof payload_result_overlap);
    payload_result_overlap.bytes[0] = 0x08u;
    payload_result_overlap.bytes[1] = 0x01u;
    memcpy(overlap_before, &payload_result_overlap,
           sizeof overlap_before);
    set_rpc(&rpc, P4_HOSTED_AUX_EVENT_HEARTBEAT,
            payload_result_overlap.bytes, 2u);
    CHECK(!p4_hosted_aux_event_decode(&rpc,
                                     &payload_result_overlap.result));
    CHECK(memcmp(&payload_result_overlap, overlap_before,
                 sizeof overlap_before) == 0);

    CHECK(!p4_hosted_aux_event_decode(NULL, &result));
    CHECK(!p4_hosted_aux_event_decode(&rpc, NULL));
}

int main(void)
{
    test_init_and_unknown_fields();
    test_heartbeat();
    test_ap_events();
    test_scan_events();
    test_envelope_refusals();
    test_malformed_fields();
    test_span_validation();
    printf("hosted auxiliary event checks: %lu\n", checks);
    return 0;
}
