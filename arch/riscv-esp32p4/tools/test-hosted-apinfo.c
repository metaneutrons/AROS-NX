/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hosted_scan.h"

static unsigned long checks;

#define CHECK(expression) do { \
    ++checks; \
    if (!(expression)) { \
        fprintf(stderr, "line %u: %s\n", (unsigned)__LINE__, #expression); \
        exit(1); \
    } \
} while (0)

static void check_bytes(const uint8_t *actual, size_t actual_size,
                        const uint8_t *expected, size_t expected_size)
{
    if (actual_size != expected_size ||
        memcmp(actual, expected, expected_size) != 0) {
        size_t i;
        fprintf(stderr, "actual (%lu):", (unsigned long)actual_size);
        for (i = 0; i < actual_size; ++i)
            fprintf(stderr, " %02x", actual[i]);
        fprintf(stderr, "\nexpected (%lu):", (unsigned long)expected_size);
        for (i = 0; i < expected_size; ++i)
            fprintf(stderr, " %02x", expected[i]);
        fprintf(stderr, "\n");
    }
    CHECK(actual_size == expected_size);
    CHECK(memcmp(actual, expected, expected_size) == 0);
}

static void append_byte(uint8_t *bytes, size_t *size, uint8_t value)
{
    bytes[(*size)++] = value;
}

static void append_varint(uint8_t *bytes, size_t *size, uint64_t value)
{
    while (value >= UINT64_C(0x80)) {
        append_byte(bytes, size,
                    (uint8_t)((value & UINT64_C(0x7f)) | UINT64_C(0x80)));
        value >>= 7;
    }
    append_byte(bytes, size, (uint8_t)value);
}

static void append_int32_field(uint8_t *bytes, size_t *size,
                               uint32_t field, int32_t value)
{
    append_varint(bytes, size, (uint64_t)field << 3);
    append_varint(bytes, size, (uint64_t)(int64_t)value);
}

static void append_bytes_field(uint8_t *bytes, size_t *size,
                               uint32_t field, const uint8_t *value,
                               size_t value_size)
{
    size_t i;

    append_varint(bytes, size, ((uint64_t)field << 3) | 2u);
    append_varint(bytes, size, value_size);
    for (i = 0; i < value_size; ++i)
        append_byte(bytes, size, value[i]);
}

static size_t make_record(uint8_t *bytes, const uint8_t bssid[6],
                          const uint8_t *ssid, size_t ssid_size)
{
    size_t size = 0u;

    append_bytes_field(bytes, &size, 1u, bssid, 6u);
    append_bytes_field(bytes, &size, 2u, ssid, ssid_size);
    append_int32_field(bytes, &size, 3u, 11);
    append_int32_field(bytes, &size, 5u, -37);
    append_int32_field(bytes, &size, 6u, 3);
    append_int32_field(bytes, &size, 7u, 4);
    append_int32_field(bytes, &size, 8u, 5);
    return size;
}

static size_t make_ap_info_body(uint8_t *body, int include_status,
                                int32_t status, const uint8_t *record,
                                size_t record_size)
{
    size_t size = 0u;

    if (include_status)
        append_int32_field(body, &size, 1u, status);
    if (record)
        append_bytes_field(body, &size, 2u, record, record_size);
    return size;
}

static void set_response(struct p4_hosted_rpc *rpc, uint32_t uid,
                         const uint8_t *body, size_t body_size)
{
    rpc->msg_type = P4_HOSTED_RPC_TYPE_RESPONSE;
    rpc->msg_id = 550u;
    rpc->uid = uid;
    rpc->payload = body;
    rpc->payload_size = body_size;
}

static void expect_refusal(const struct p4_hosted_rpc *rpc, uint32_t uid)
{
    struct p4_hosted_sta_ap_info result, before;

    memset(&result, 0xa7, sizeof result);
    before = result;
    CHECK(!p4_hosted_sta_ap_info_response(rpc, uid, &result));
    CHECK(memcmp(&result, &before, sizeof result) == 0);
}

static void check_zero_ap(const struct p4_hosted_scan_record *record)
{
    struct p4_hosted_scan_record zero;

    memset(&zero, 0, sizeof zero);
    CHECK(memcmp(record, &zero, sizeof zero) == 0);
}

static void test_empty_request(void)
{
    static const uint8_t uid_seven[] = {
        0x08, 0x01, 0x10, 0xa6, 0x02, 0x18, 0x07, 0xb2, 0x12, 0x00
    };
    static const uint8_t uid_max[] = {
        0x08, 0x01, 0x10, 0xa6, 0x02,
        0x18, 0xff, 0xff, 0xff, 0xff, 0x0f, 0xb2, 0x12, 0x00
    };
    uint8_t bytes[64], before[sizeof bytes];
    struct p4_hosted_rpc rpc;
    size_t written = 0u;

    memset(bytes, 0xa5, sizeof bytes);
    CHECK(p4_hosted_sta_ap_info_request(7u, bytes, sizeof bytes, &written));
    check_bytes(bytes, written, uid_seven, sizeof uid_seven);
    CHECK(p4_hosted_rpc_decode(bytes, written, &rpc));
    CHECK(rpc.msg_type == P4_HOSTED_RPC_TYPE_REQUEST && rpc.msg_id == 294u &&
          rpc.uid == 7u && rpc.payload_size == 0u);

    CHECK(p4_hosted_sta_ap_info_request(UINT32_MAX, bytes, sizeof bytes,
                                        &written));
    check_bytes(bytes, written, uid_max, sizeof uid_max);
    CHECK(p4_hosted_rpc_decode(bytes, written, &rpc));
    CHECK(rpc.msg_type == P4_HOSTED_RPC_TYPE_REQUEST && rpc.msg_id == 294u &&
          rpc.uid == UINT32_MAX && rpc.payload_size == 0u);

    memset(bytes, 0x43, sizeof bytes);
    memcpy(before, bytes, sizeof before);
    written = (size_t)0x7654u;
    CHECK(!p4_hosted_sta_ap_info_request(0u, bytes, sizeof bytes, &written));
    CHECK(!p4_hosted_sta_ap_info_request(1u, bytes, 9u, &written));
    CHECK(!p4_hosted_sta_ap_info_request(1u, NULL, sizeof bytes, &written));
    CHECK(!p4_hosted_sta_ap_info_request(1u, bytes, sizeof bytes, NULL));
    CHECK(!p4_hosted_sta_ap_info_request(1u, bytes, SIZE_MAX, &written));
    CHECK(memcmp(bytes, before, sizeof bytes) == 0 &&
          written == (size_t)0x7654u);

    {
        union {
            size_t alignment;
            uint8_t bytes[64];
        } overlap;
        uint8_t snapshot[sizeof overlap.bytes];

        memset(overlap.bytes, 0x91, sizeof overlap.bytes);
        memcpy(snapshot, overlap.bytes, sizeof snapshot);
        CHECK(!p4_hosted_sta_ap_info_request(1u, overlap.bytes,
                                              sizeof overlap.bytes,
                                              (size_t *)(void *)overlap.bytes));
        CHECK(memcmp(overlap.bytes, snapshot, sizeof snapshot) == 0);
    }
    {
        size_t *overflow_written =
            (size_t *)(uintptr_t)(UINTPTR_MAX - 1u);
        CHECK(!p4_hosted_sta_ap_info_request(1u, bytes, sizeof bytes,
                                              overflow_written));
        CHECK(memcmp(bytes, before, sizeof bytes) == 0 &&
              written == (size_t)0x7654u);
    }
    {
        uint8_t *overflow = (uint8_t *)(uintptr_t)(UINTPTR_MAX - 1u);
        CHECK(!p4_hosted_sta_ap_info_request(1u, overflow, 4u, &written));
        CHECK(written == (size_t)0x7654u);
    }
}

static void test_success_and_owned_binary_record(void)
{
    uint8_t body[256], record[128];
    uint8_t bssid[6] = { 0x02, 0x10, 0x20, 0x30, 0x40, 0x50 };
    uint8_t ssid[32];
    size_t record_size, body_size, i;
    struct p4_hosted_rpc rpc;
    struct p4_hosted_sta_ap_info result;

    for (i = 0; i < sizeof ssid; ++i)
        ssid[i] = (uint8_t)(0x60u + i);
    ssid[4] = 0u;
    record_size = make_record(record, bssid, ssid, sizeof ssid);
    body_size = make_ap_info_body(body, 1, 0, record, record_size);
    set_response(&rpc, UINT32_MAX, body, body_size);
    memset(&result, 0xa5, sizeof result);
    CHECK(p4_hosted_sta_ap_info_response(&rpc, UINT32_MAX, &result));
    CHECK(result.status == 0);
    CHECK(result.ap.ssid_size == sizeof ssid);
    CHECK(memcmp(result.ap.ssid, ssid, sizeof ssid) == 0);
    CHECK(memcmp(result.ap.bssid, bssid, sizeof bssid) == 0);
    CHECK(result.ap.primary == 11u && result.ap.rssi == -37);
    CHECK(result.ap.authmode == 3 && result.ap.pairwise_cipher == 4 &&
          result.ap.group_cipher == 5);

    memset(body, 0, sizeof body);
    memset(record, 0, sizeof record);
    CHECK(result.ap.ssid_size == sizeof ssid &&
          memcmp(result.ap.ssid, ssid, sizeof ssid) == 0);
    CHECK(memcmp(result.ap.bssid, bssid, sizeof bssid) == 0);

    /* The proto3 default status may be absent on a successful response. */
    /* Rebuild the valid record after the ownership check mutated its source. */
    record_size = make_record(record, bssid, ssid, sizeof ssid);
    body_size = make_ap_info_body(body, 0, 0, record, record_size);
    set_response(&rpc, 17u, body, body_size);
    CHECK(p4_hosted_sta_ap_info_response(&rpc, 17u, &result));
    CHECK(result.status == 0 && result.ap.ssid_size == sizeof ssid &&
          memcmp(result.ap.ssid, ssid, sizeof ssid) == 0);
}

static void test_error_status_and_discard(void)
{
    uint8_t body[256], record[128];
    size_t record_size, body_size;
    struct p4_hosted_rpc rpc;
    struct p4_hosted_sta_ap_info result;

    set_response(&rpc, 5u, NULL, 0u);
    CHECK(!p4_hosted_sta_ap_info_response(&rpc, 5u, &result));

    body[0] = 0x08u;
    body[1] = 0x8fu;
    body[2] = 0x60u;
    set_response(&rpc, 5u, body, 3u);
    memset(&result, 0xa5, sizeof result);
    CHECK(p4_hosted_sta_ap_info_response(&rpc, 5u, &result));
    CHECK(result.status == 0x300f);
    check_zero_ap(&result.ap);

    body[1] = 0x81u;
    set_response(&rpc, 6u, body, 3u);
    CHECK(p4_hosted_sta_ap_info_response(&rpc, 6u, &result));
    CHECK(result.status == 0x3001);
    check_zero_ap(&result.ap);

    body[0] = 0x08u;
    body[1] = 0xffu;
    body[2] = 0xffu;
    body[3] = 0xffu;
    body[4] = 0xffu;
    body[5] = 0xffu;
    body[6] = 0xffu;
    body[7] = 0xffu;
    body[8] = 0xffu;
    body[9] = 0xffu;
    body[10] = 0x01u;
    set_response(&rpc, 7u, body, 11u);
    CHECK(p4_hosted_sta_ap_info_response(&rpc, 7u, &result));
    CHECK(result.status == -1);
    check_zero_ap(&result.ap);

    {
        const uint8_t bssid[6] = { 1, 2, 3, 4, 5, 6 };
        const uint8_t ssid[] = { 'a', 0, 'b' };

        record_size = make_record(record, bssid, ssid, sizeof ssid);
        body_size = make_ap_info_body(body, 1, 0x300f, record, record_size);
        set_response(&rpc, 8u, body, body_size);
        CHECK(p4_hosted_sta_ap_info_response(&rpc, 8u, &result));
        CHECK(result.status == 0x300f);
        check_zero_ap(&result.ap);
    }
}

static void test_response_refusals(void)
{
    static const uint8_t status_zero[] = { 0x08, 0x00 };
    static const uint8_t duplicate_status[] = {
        0x08, 0x00, 0x08, 0x00
    };
    static const uint8_t wrong_status_wire[] = { 0x0a, 0x00 };
    static const uint8_t status_truncated[] = { 0x08, 0x80 };
    static const uint8_t short_bssid_record[] = {
        0x0a, 0x05, 1, 2, 3, 4, 5
    };
    static const uint8_t duplicate_bssid_record[] = {
        0x0a, 0x06, 1, 2, 3, 4, 5, 6,
        0x0a, 0x06, 1, 2, 3, 4, 5, 6
    };
    static const uint8_t malformed_nested[] = {
        0x12, 0x07, 0x0a, 0x05, 1, 2, 3, 4, 5
    };
    static const uint8_t error_with_malformed_ap[] = {
        0x08, 0x8f, 0x60, 0x12, 0x07, 0x0a, 0x05, 1, 2, 3, 4, 5
    };
    static const uint8_t duplicate_ap[] = {
        0x12, 0x08, 0x0a, 0x06, 1, 2, 3, 4, 5, 6,
        0x12, 0x08, 0x0a, 0x06, 1, 2, 3, 4, 5, 6
    };
    static const uint8_t duplicate_record_field[] = {
        0x12, 0x10,
        0x0a, 0x06, 1, 2, 3, 4, 5, 6,
        0x0a, 0x06, 1, 2, 3, 4, 5, 6
    };
    static const uint8_t bad_country_child[] = {
        0x12, 0x11,
        0x0a, 0x06, 1, 2, 3, 4, 5, 6,
        0x5a, 0x05, 0x0a, 0x03, 'U', 'S', 'A',
        0x5a, 0x00
    };
    static const uint8_t record_group[] = {
        0x12, 0x0a, 0x0b, 0x0c,
        0x0a, 0x06, 1, 2, 3, 4, 5, 6
    };
    static const uint8_t response_group[] = { 0x0b, 0x0c };
    struct p4_hosted_rpc rpc;
    size_t i;
    static const struct {
        const uint8_t *body;
        size_t size;
    } malformed[] = {
        { NULL, 0u },
        { status_zero, sizeof status_zero },
        { duplicate_status, sizeof duplicate_status },
        { wrong_status_wire, sizeof wrong_status_wire },
        { status_truncated, sizeof status_truncated },
        { malformed_nested, sizeof malformed_nested },
        { error_with_malformed_ap, sizeof error_with_malformed_ap },
        { duplicate_ap, sizeof duplicate_ap },
        { duplicate_record_field, sizeof duplicate_record_field },
        { bad_country_child, sizeof bad_country_child },
        { record_group, sizeof record_group },
        { response_group, sizeof response_group }
    };

    for (i = 0; i < sizeof malformed / sizeof malformed[0]; ++i) {
        set_response(&rpc, 9u, malformed[i].body, malformed[i].size);
        expect_refusal(&rpc, 9u);
    }

    set_response(&rpc, 9u, short_bssid_record,
                 sizeof short_bssid_record);
    expect_refusal(&rpc, 9u);
    set_response(&rpc, 9u, duplicate_bssid_record,
                 sizeof duplicate_bssid_record);
    expect_refusal(&rpc, 9u);
}

static void test_unknown_fields_and_limits(void)
{
    uint8_t body[P4_HOSTED_SCAN_MAX_BODY_SIZE + 1u];
    uint8_t record[128];
    const uint8_t bssid[6] = { 1, 2, 3, 4, 5, 6 };
    const uint8_t ssid[] = { 0x71u };
    size_t record_size, body_size, offset, i;
    struct p4_hosted_rpc rpc;
    struct p4_hosted_sta_ap_info result;

    record_size = make_record(record, bssid, ssid, sizeof ssid);
    /* Unknown top-level and nested fields using each supported wire shape. */
    append_varint(record, &record_size, 0x78u);
    append_varint(record, &record_size, 1u);
    append_byte(record, &record_size, 0x81u);
    append_byte(record, &record_size, 0x01u);
    append_byte(record, &record_size, 1u);
    append_byte(record, &record_size, 2u);
    append_byte(record, &record_size, 3u);
    append_byte(record, &record_size, 4u);
    append_byte(record, &record_size, 5u);
    append_byte(record, &record_size, 6u);
    append_byte(record, &record_size, 7u);
    append_byte(record, &record_size, 8u);
    append_byte(record, &record_size, 0x8au);
    append_byte(record, &record_size, 0x01u);
    append_varint(record, &record_size, 2u);
    append_byte(record, &record_size, 0xaau);
    append_byte(record, &record_size, 0xbbu);
    append_byte(record, &record_size, 0x95u);
    append_byte(record, &record_size, 0x01u);
    append_byte(record, &record_size, 1u);
    append_byte(record, &record_size, 2u);
    append_byte(record, &record_size, 3u);
    append_byte(record, &record_size, 4u);

    body_size = make_ap_info_body(body, 1, 0, record, record_size);
    append_int32_field(body, &body_size, 3u, 2);
    append_varint(body, &body_size, 0x21u);
    append_byte(body, &body_size, 1u);
    append_byte(body, &body_size, 2u);
    append_byte(body, &body_size, 3u);
    append_byte(body, &body_size, 4u);
    append_byte(body, &body_size, 5u);
    append_byte(body, &body_size, 6u);
    append_byte(body, &body_size, 7u);
    append_byte(body, &body_size, 8u);
    append_varint(body, &body_size, 0x2au);
    append_varint(body, &body_size, 2u);
    append_byte(body, &body_size, 0xaau);
    append_byte(body, &body_size, 0xbbu);
    append_varint(body, &body_size, 0x35u);
    append_byte(body, &body_size, 1u);
    append_byte(body, &body_size, 2u);
    append_byte(body, &body_size, 3u);
    append_byte(body, &body_size, 4u);
    set_response(&rpc, 10u, body, body_size);
    CHECK(p4_hosted_sta_ap_info_response(&rpc, 10u, &result));
    CHECK(result.status == 0 && result.ap.primary == 11u &&
          result.ap.ssid_size == sizeof ssid && result.ap.ssid[0] == ssid[0]);

    /* A bounded error body can contain ignorable unknown fields. */
    body[0] = 0x08u;
    body[1] = 0x8fu;
    body[2] = 0x60u;
    offset = 3u;
    for (i = 0u; i < 2044u; ++i) {
        body[offset++] = 0x78u;
        body[offset++] = 0u;
    }
    CHECK(offset == P4_HOSTED_SCAN_MAX_BODY_SIZE - 5u);
    body[offset++] = 0x7du;
    body[offset++] = 0u;
    body[offset++] = 0u;
    body[offset++] = 0u;
    body[offset++] = 0u;
    CHECK(offset == P4_HOSTED_SCAN_MAX_BODY_SIZE);
    set_response(&rpc, 11u, body, P4_HOSTED_SCAN_MAX_BODY_SIZE);
    CHECK(p4_hosted_sta_ap_info_response(&rpc, 11u, &result));
    CHECK(result.status == 0x300f);
    check_zero_ap(&result.ap);

    body[P4_HOSTED_SCAN_MAX_BODY_SIZE] = 0u;
    set_response(&rpc, 11u, body, sizeof body);
    expect_refusal(&rpc, 11u);
}

static void test_correlation_and_aliasing(void)
{
    uint8_t body[] = { 0x08, 0x8f, 0x60 };
    struct p4_hosted_rpc rpc;

    set_response(&rpc, 12u, body, sizeof body);
    expect_refusal(&rpc, 13u);
    rpc.msg_id = 549u;
    expect_refusal(&rpc, 12u);
    rpc.msg_id = 550u;
    rpc.msg_type = P4_HOSTED_RPC_TYPE_EVENT;
    expect_refusal(&rpc, 12u);
    rpc.msg_type = P4_HOSTED_RPC_TYPE_RESPONSE;
    CHECK(!p4_hosted_sta_ap_info_response(&rpc, 0u,
                                          (struct p4_hosted_sta_ap_info *)body));

    {
        struct p4_hosted_sta_ap_info result;
        set_response(&rpc, 12u, body, sizeof body);
        CHECK(!p4_hosted_sta_ap_info_response(&rpc, 12u, NULL));
        {
            struct p4_hosted_sta_ap_info *overflowing =
                (struct p4_hosted_sta_ap_info *)(uintptr_t)(UINTPTR_MAX - 1u);
            CHECK(!p4_hosted_sta_ap_info_response(&rpc, 12u, overflowing));
        }
        {
            const struct p4_hosted_rpc *overflowing =
                (const struct p4_hosted_rpc *)(uintptr_t)(UINTPTR_MAX - 1u);
            CHECK(!p4_hosted_sta_ap_info_response(overflowing, 12u, &result));
        }
        rpc.payload = (const uint8_t *)(uintptr_t)(UINTPTR_MAX - 1u);
        rpc.payload_size = 4u;
        expect_refusal(&rpc, 12u);
    }

    {
        union {
            struct p4_hosted_rpc rpc;
            struct p4_hosted_sta_ap_info result;
        } overlap;
        memset(&overlap, 0, sizeof overlap);
        overlap.rpc.msg_type = P4_HOSTED_RPC_TYPE_RESPONSE;
        overlap.rpc.msg_id = 550u;
        overlap.rpc.uid = 12u;
        {
            uint8_t before[sizeof overlap];
            memcpy(before, &overlap, sizeof before);
            CHECK(!p4_hosted_sta_ap_info_response(&overlap.rpc, 12u,
                                                   &overlap.result));
            CHECK(memcmp(&overlap, before, sizeof before) == 0);
        }
    }

    {
        union {
            struct p4_hosted_sta_ap_info result;
            uint8_t body[sizeof(struct p4_hosted_sta_ap_info)];
        } overlap;
        uint8_t before[sizeof overlap.body];

        memset(&overlap, 0x6d, sizeof overlap);
        memcpy(before, overlap.body, sizeof before);
        set_response(&rpc, 12u, overlap.body, sizeof overlap.body);
        CHECK(!p4_hosted_sta_ap_info_response(&rpc, 12u, &overlap.result));
        CHECK(memcmp(overlap.body, before, sizeof before) == 0);
    }
}

int main(void)
{
    test_empty_request();
    test_success_and_owned_binary_record();
    test_error_status_and_discard();
    test_response_refusals();
    test_unknown_fields_and_limits();
    test_correlation_and_aliasing();
    printf("hosted_apinfo: %lu checks\n", checks);
    return 0;
}
