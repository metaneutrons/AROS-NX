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
    CHECK(actual_size == expected_size);
    CHECK(memcmp(actual, expected, expected_size) == 0);
}

static void set_response(struct p4_hosted_rpc *rpc, uint32_t request_id,
                         uint32_t uid, const uint8_t *body, size_t body_size)
{
    rpc->msg_type = P4_HOSTED_RPC_TYPE_RESPONSE;
    rpc->msg_id = request_id + 256u;
    rpc->uid = uid;
    rpc->payload = body;
    rpc->payload_size = body_size;
}

static void expect_status_refusal(const struct p4_hosted_rpc *rpc,
                                  uint32_t request_id, uint32_t uid)
{
    int32_t status = INT32_C(0x5a5a1234);
    int32_t before = status;

    CHECK(!p4_hosted_scan_status_response(rpc, request_id, uid, &status));
    CHECK(status == before);
}

static void expect_num_refusal(const struct p4_hosted_rpc *rpc, uint32_t uid)
{
    struct p4_hosted_scan_count result, before;

    memset(&result, 0x6d, sizeof result);
    before = result;
    CHECK(!p4_hosted_scan_num_response(rpc, uid, &result));
    CHECK(memcmp(&result, &before, sizeof result) == 0);
}

static void expect_records_refusal(const struct p4_hosted_rpc *rpc,
                                   uint32_t uid, uint16_t capacity)
{
    struct p4_hosted_scan_records result, before;

    memset(&result, 0xa7, sizeof result);
    before = result;
    if (p4_hosted_scan_records_response(rpc, uid, capacity, &result)) {
        fprintf(stderr, "accepted malformed records body (%lu bytes, cap %u)\n",
                (unsigned long)rpc->payload_size, (unsigned)capacity);
        exit(1);
    }
    ++checks;
    CHECK(memcmp(&result, &before, sizeof result) == 0);
}

static void test_request_vectors(void)
{
    static const uint8_t start[] = {
        0x08, 0x01, 0x10, 0x9e, 0x02, 0x18, 0x07,
        0xf2, 0x11, 0x02, 0x18, 0x00
    };
    static const uint8_t start_blocking[] = {
        0x08, 0x01, 0x10, 0x9e, 0x02, 0x18, 0x07,
        0xf2, 0x11, 0x04, 0x10, 0x01, 0x18, 0x00
    };
    static const uint8_t stop[] = {
        0x08, 0x01, 0x10, 0x9f, 0x02, 0x18, 0x07,
        0xfa, 0x11, 0x00
    };
    static const uint8_t get_num[] = {
        0x08, 0x01, 0x10, 0xa0, 0x02, 0x18, 0x07,
        0x82, 0x12, 0x00
    };
    static const uint8_t get_records[] = {
        0x08, 0x01, 0x10, 0xa1, 0x02, 0x18, 0x07,
        0x8a, 0x12, 0x02, 0x08, 0x08
    };
    uint8_t bytes[64];
    size_t written = 0u;
    struct p4_hosted_rpc rpc;

    memset(bytes, 0xa5, sizeof bytes);
    CHECK(p4_hosted_scan_start_request(7u, 0u, bytes, sizeof bytes,
                                       &written));
    check_bytes(bytes, written, start, sizeof start);
    CHECK(p4_hosted_rpc_decode(bytes, written, &rpc));
    CHECK(rpc.msg_type == P4_HOSTED_RPC_TYPE_REQUEST && rpc.msg_id == 286u &&
          rpc.uid == 7u && rpc.payload_size == 2u &&
          rpc.payload[0] == 0x18u && rpc.payload[1] == 0x00u);

    CHECK(p4_hosted_scan_start_request(7u, 1u, bytes, sizeof bytes,
                                       &written));
    check_bytes(bytes, written, start_blocking, sizeof start_blocking);
    CHECK(p4_hosted_rpc_decode(bytes, written, &rpc));
    CHECK(rpc.msg_id == 286u && rpc.payload_size == 4u &&
          rpc.payload[0] == 0x10u && rpc.payload[1] == 0x01u &&
          rpc.payload[2] == 0x18u && rpc.payload[3] == 0x00u);

    written = 0u;
    CHECK(p4_hosted_scan_stop_request(7u, bytes, sizeof bytes, &written));
    check_bytes(bytes, written, stop, sizeof stop);
    CHECK(p4_hosted_rpc_decode(bytes, written, &rpc));
    CHECK(rpc.msg_id == 287u && rpc.payload_size == 0u);

    written = 0u;
    CHECK(p4_hosted_scan_num_request(7u, bytes, sizeof bytes, &written));
    check_bytes(bytes, written, get_num, sizeof get_num);
    CHECK(p4_hosted_rpc_decode(bytes, written, &rpc));
    CHECK(rpc.msg_id == 288u && rpc.payload_size == 0u);

    written = 0u;
    CHECK(p4_hosted_scan_records_request(7u, 8u, bytes, sizeof bytes,
                                         &written));
    check_bytes(bytes, written, get_records, sizeof get_records);
    CHECK(p4_hosted_rpc_decode(bytes, written, &rpc));
    CHECK(rpc.msg_id == 289u && rpc.payload_size == 2u &&
          rpc.payload[0] == 0x08u && rpc.payload[1] == 0x08u);
}

static void test_request_refusal_and_aliasing(void)
{
    uint8_t bytes[64], before[sizeof bytes];
    size_t written;

    memset(bytes, 0x43, sizeof bytes);
    memcpy(before, bytes, sizeof bytes);
    written = 0x7654u;
    CHECK(!p4_hosted_scan_start_request(0u, 0u, bytes, sizeof bytes,
                                        &written));
    CHECK(memcmp(bytes, before, sizeof bytes) == 0 && written == 0x7654u);
    CHECK(!p4_hosted_scan_stop_request(0u, bytes, sizeof bytes, &written));
    CHECK(!p4_hosted_scan_num_request(0u, bytes, sizeof bytes, &written));
    CHECK(!p4_hosted_scan_records_request(0u, 1u, bytes, sizeof bytes,
                                          &written));
    CHECK(!p4_hosted_scan_records_request(1u, 0u, bytes, sizeof bytes,
                                          &written));
    CHECK(!p4_hosted_scan_records_request(1u, 9u, bytes, sizeof bytes,
                                          &written));
    CHECK(memcmp(bytes, before, sizeof bytes) == 0 && written == 0x7654u);

    CHECK(!p4_hosted_scan_start_request(1u, 0u, bytes, 11u, &written));
    CHECK(memcmp(bytes, before, sizeof bytes) == 0 && written == 0x7654u);
    CHECK(!p4_hosted_scan_stop_request(1u, bytes, 9u, &written));
    CHECK(memcmp(bytes, before, sizeof bytes) == 0 && written == 0x7654u);
    CHECK(!p4_hosted_scan_num_request(1u, bytes, SIZE_MAX, &written));
    CHECK(memcmp(bytes, before, sizeof bytes) == 0 && written == 0x7654u);
    CHECK(!p4_hosted_scan_records_request(1u, 1u, bytes, 11u, &written));
    CHECK(memcmp(bytes, before, sizeof bytes) == 0 && written == 0x7654u);
    CHECK(!p4_hosted_scan_start_request(1u, 0u, NULL, sizeof bytes,
                                        &written));
    CHECK(!p4_hosted_scan_start_request(1u, 0u, bytes, sizeof bytes, NULL));
    CHECK(!p4_hosted_scan_start_request(1u, 2u, bytes, sizeof bytes,
                                        &written));
    {
        size_t *overflow_written =
            (size_t *)(uintptr_t)(UINTPTR_MAX - 1u);
        CHECK(!p4_hosted_scan_start_request(1u, 0u, bytes, sizeof bytes,
                                             overflow_written));
        CHECK(memcmp(bytes, before, sizeof bytes) == 0 && written == 0x7654u);
    }

    {
        uint8_t *overflow = (uint8_t *)(uintptr_t)(UINTPTR_MAX - 1u);
        CHECK(!p4_hosted_scan_start_request(1u, 0u, overflow, 12u,
                                             &written));
        CHECK(written == 0x7654u);
    }

    {
        union {
            size_t alignment;
            uint8_t bytes[64];
        } overlap;
        uint8_t snapshot[sizeof overlap.bytes];

        memset(overlap.bytes, 0x91, sizeof overlap.bytes);
        memcpy(snapshot, overlap.bytes, sizeof snapshot);
        CHECK(!p4_hosted_scan_stop_request(1u, overlap.bytes,
                                            sizeof overlap.bytes,
                                            (size_t *)(void *)overlap.bytes));
        CHECK(memcmp(overlap.bytes, snapshot, sizeof snapshot) == 0);
    }
}

static void test_status_responses(void)
{
    static const uint8_t explicit_zero[] = { 0x08, 0x00 };
    static const uint8_t status_error[] = { 0x08, 0x81, 0x60 };
    static const uint8_t negative_one[] = {
        0x08, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0x01
    };
    static const uint8_t with_unknowns[] = {
        0x18, 0x7f,
        0x21, 1, 2, 3, 4, 5, 6, 7, 8,
        0x2a, 3, 0xaa, 0xbb, 0xcc,
        0x35, 0x11, 0x22, 0x33, 0x44,
        0x08, 0x00
    };
    struct p4_hosted_rpc rpc;
    int32_t status;

    set_response(&rpc, 286u, 9u, NULL, 0u);
    status = -1;
    CHECK(p4_hosted_scan_status_response(&rpc, 286u, 9u, &status));
    CHECK(status == 0);

    set_response(&rpc, 287u, 9u, explicit_zero, sizeof explicit_zero);
    status = -1;
    CHECK(p4_hosted_scan_status_response(&rpc, 287u, 9u, &status));
    CHECK(status == 0);

    set_response(&rpc, 286u, 9u, status_error, sizeof status_error);
    status = 0;
    CHECK(p4_hosted_scan_status_response(&rpc, 286u, 9u, &status));
    CHECK(status == 0x3001);

    set_response(&rpc, 287u, 9u, negative_one, sizeof negative_one);
    status = 0;
    CHECK(p4_hosted_scan_status_response(&rpc, 287u, 9u, &status));
    CHECK(status == -1);

    set_response(&rpc, 286u, 9u, with_unknowns, sizeof with_unknowns);
    status = -1;
    CHECK(p4_hosted_scan_status_response(&rpc, 286u, 9u, &status));
    CHECK(status == 0);
}

static void test_status_refusals(void)
{
    static const uint8_t duplicate[] = { 0x08, 0x00, 0x08, 0x00 };
    static const uint8_t wrong_wire[] = { 0x0a, 0x00 };
    static const uint8_t truncated[] = { 0x08, 0x80 };
    static const uint8_t overflow[] = {
        0x08, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0x02
    };
    static const uint8_t overlong[] = { 0x08, 0x80, 0x00 };
    static const uint8_t bad_positive_int32[] = {
        0x08, 0x80, 0x80, 0x80, 0x80, 0x08
    };
    static const uint8_t group[] = { 0x1b, 0x1c };
    struct p4_hosted_rpc rpc;
    int32_t status = 0x12345678;

    set_response(&rpc, 286u, 9u, duplicate, sizeof duplicate);
    expect_status_refusal(&rpc, 286u, 9u);
    set_response(&rpc, 286u, 9u, wrong_wire, sizeof wrong_wire);
    expect_status_refusal(&rpc, 286u, 9u);
    set_response(&rpc, 286u, 9u, truncated, sizeof truncated);
    expect_status_refusal(&rpc, 286u, 9u);
    set_response(&rpc, 286u, 9u, overflow, sizeof overflow);
    expect_status_refusal(&rpc, 286u, 9u);
    set_response(&rpc, 286u, 9u, overlong, sizeof overlong);
    expect_status_refusal(&rpc, 286u, 9u);
    set_response(&rpc, 286u, 9u, bad_positive_int32,
                 sizeof bad_positive_int32);
    expect_status_refusal(&rpc, 286u, 9u);
    set_response(&rpc, 286u, 9u, group, sizeof group);
    expect_status_refusal(&rpc, 286u, 9u);

    set_response(&rpc, 286u, 9u, NULL, 0u);
    CHECK(!p4_hosted_scan_status_response(&rpc, 288u, 9u, &status));
    CHECK(status == 0x12345678);
    CHECK(!p4_hosted_scan_status_response(&rpc, 286u, 0u, &status));
    CHECK(!p4_hosted_scan_status_response(&rpc, 286u, 10u, &status));
    rpc.msg_type = P4_HOSTED_RPC_TYPE_REQUEST;
    expect_status_refusal(&rpc, 286u, 9u);
    rpc.msg_type = P4_HOSTED_RPC_TYPE_RESPONSE;
    rpc.msg_id = 543u;
    expect_status_refusal(&rpc, 286u, 9u);

    set_response(&rpc, 286u, 9u, NULL, 0u);
    rpc.payload_size = P4_HOSTED_SCAN_MAX_BODY_SIZE + 1u;
    expect_status_refusal(&rpc, 286u, 9u);
    rpc.payload = (const uint8_t *)(uintptr_t)(UINTPTR_MAX - 1u);
    rpc.payload_size = 4u;
    expect_status_refusal(&rpc, 286u, 9u);

    {
        const struct p4_hosted_rpc *overflowing =
            (const struct p4_hosted_rpc *)(uintptr_t)(UINTPTR_MAX - 1u);
        CHECK(!p4_hosted_scan_status_response(overflowing, 286u, 9u,
                                              &status));
        CHECK(status == 0x12345678);
    }

    {
        union {
            struct p4_hosted_rpc rpc;
            int32_t status;
        } overlap;
        memset(&overlap, 0, sizeof overlap);
        overlap.rpc.msg_type = P4_HOSTED_RPC_TYPE_RESPONSE;
        overlap.rpc.msg_id = 542u;
        overlap.rpc.uid = 9u;
        CHECK(!p4_hosted_scan_status_response(&overlap.rpc, 286u, 9u,
                                              &overlap.status));
    }
    {
        union {
            int32_t status;
            uint8_t body[sizeof(int32_t)];
        } overlap;
        uint8_t before[sizeof overlap.body];

        memset(&overlap, 0, sizeof overlap);
        overlap.body[0] = 0x08u;
        overlap.body[1] = 0x00u;
        memcpy(before, overlap.body, sizeof before);
        set_response(&rpc, 286u, 9u, overlap.body, 2u);
        CHECK(!p4_hosted_scan_status_response(&rpc, 286u, 9u,
                                              &overlap.status));
        CHECK(memcmp(overlap.body, before, sizeof before) == 0);
    }
}

static void test_num_responses(void)
{
    static const uint8_t count_two[] = { 0x10, 0x02 };
    static const uint8_t status_and_count[] = { 0x08, 0x81, 0x60,
                                                 0x10, 0x03 };
    static const uint8_t decoded_envelope[] = {
        0x08, 0x02, 0x10, 0xa0, 0x04, 0x18, 0x07,
        0x82, 0x22, 0x02, 0x10, 0x02
    };
    struct p4_hosted_rpc rpc;
    struct p4_hosted_scan_count result;

    set_response(&rpc, 288u, 9u, NULL, 0u);
    result.status = -1;
    result.number = 77u;
    CHECK(p4_hosted_scan_num_response(&rpc, 9u, &result));
    CHECK(result.status == 0 && result.number == 0u);
    {
        const uint8_t all_zero[sizeof result] = { 0 };
        CHECK(memcmp(&result, all_zero, sizeof result) == 0);
    }

    set_response(&rpc, 288u, 9u, count_two, sizeof count_two);
    CHECK(p4_hosted_scan_num_response(&rpc, 9u, &result));
    CHECK(result.status == 0 && result.number == 2u);
    set_response(&rpc, 288u, 9u, status_and_count,
                 sizeof status_and_count);
    CHECK(p4_hosted_scan_num_response(&rpc, 9u, &result));
    CHECK(result.status == 0x3001 && result.number == 3u);

    CHECK(p4_hosted_rpc_decode(decoded_envelope, sizeof decoded_envelope,
                               &rpc));
    CHECK(rpc.msg_type == P4_HOSTED_RPC_TYPE_RESPONSE && rpc.msg_id == 544u &&
          rpc.uid == 7u);
    CHECK(p4_hosted_scan_num_response(&rpc, 7u, &result));
    CHECK(result.number == 2u);

}

static void test_num_refusals(void)
{
    static const uint8_t duplicate_count[] = {
        0x10, 0x01, 0x10, 0x01
    };
    static const uint8_t negative_count[] = {
        0x10, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0x01
    };
    static const uint8_t count_too_big[] = { 0x10, 0x80, 0x80, 0x04 };
    static const uint8_t overlong_count[] = { 0x10, 0x81, 0x00 };
    static const uint8_t duplicate_status[] = {
        0x08, 0x00, 0x08, 0x00
    };
    struct p4_hosted_rpc rpc;

    set_response(&rpc, 288u, 9u, duplicate_count, sizeof duplicate_count);
    expect_num_refusal(&rpc, 9u);
    set_response(&rpc, 288u, 9u, negative_count, sizeof negative_count);
    expect_num_refusal(&rpc, 9u);
    set_response(&rpc, 288u, 9u, count_too_big, sizeof count_too_big);
    expect_num_refusal(&rpc, 9u);
    set_response(&rpc, 288u, 9u, overlong_count, sizeof overlong_count);
    expect_num_refusal(&rpc, 9u);
    set_response(&rpc, 288u, 9u, duplicate_status, sizeof duplicate_status);
    expect_num_refusal(&rpc, 9u);
    set_response(&rpc, 287u, 9u, NULL, 0u);
    expect_num_refusal(&rpc, 9u);
    set_response(&rpc, 288u, 9u, NULL, 0u);
    expect_num_refusal(&rpc, 8u);

    {
        union {
            struct p4_hosted_scan_count result;
            uint8_t body[sizeof(struct p4_hosted_scan_count)];
        } overlap;
        uint8_t snapshot[sizeof overlap.body];

        memset(&overlap, 0, sizeof overlap);
        overlap.body[0] = 0x10u;
        overlap.body[1] = 0x00u;
        memcpy(snapshot, overlap.body, sizeof snapshot);
        set_response(&rpc, 288u, 9u, overlap.body, 2u);
        CHECK(!p4_hosted_scan_num_response(&rpc, 9u, &overlap.result));
        CHECK(memcmp(overlap.body, snapshot, sizeof snapshot) == 0);
    }
}

static const uint8_t one_record_response[] = {
    0x08, 0x00, 0x10, 0x01, 0x1a, 0x4d,
    0x0a, 0x06, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff,
    0x12, 0x03, 0x6e, 0x65, 0x74,
    0x18, 0x24,
    0x20, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x01,
    0x28, 0xd6, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x01,
    0x30, 0x03, 0x38, 0x04, 0x40, 0x05,
    0x48, 0xfe, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x01,
    0x50, 0xff, 0x03,
    0x5a, 0x0c, 0x0a, 0x02, 0x55, 0x53, 0x10, 0x01,
    0x18, 0x0e, 0x20, 0x14, 0x28, 0x01,
    0x62, 0x04, 0x08, 0x20, 0x10, 0x07
};

static void test_records_literal_vector(void)
{
    struct p4_hosted_rpc rpc;
    struct p4_hosted_scan_records result;

    set_response(&rpc, 289u, 7u, one_record_response,
                 sizeof one_record_response);
    memset(&result, 0, sizeof result);
    CHECK(p4_hosted_scan_records_response(&rpc, 7u, 1u, &result));
    CHECK(result.status == 0 && result.number == 1u &&
          result.record_count == 1u);
    CHECK(result.records[0].bssid[0] == 0xaau &&
          result.records[0].bssid[5] == 0xffu);
    CHECK(result.records[0].ssid_size == 3u &&
          result.records[0].ssid[0] == 'n' &&
          result.records[0].ssid[2] == 't');
    CHECK(result.records[0].primary == 36u && result.records[0].second == -1);
    CHECK(result.records[0].rssi == -42 && result.records[0].authmode == 3 &&
          result.records[0].pairwise_cipher == 4 &&
          result.records[0].group_cipher == 5 && result.records[0].ant == -2);
    CHECK(result.records[0].bitmask == 511u);
    CHECK(result.records[0].country_cc_size == 2u &&
          result.records[0].country_cc[0] == 'U' &&
          result.records[0].country_cc[1] == 'S');
    CHECK(result.records[0].country_schan == 1u &&
          result.records[0].country_nchan == 14u &&
          result.records[0].country_max_tx_power == 20 &&
          result.records[0].country_policy == 1);
    CHECK(result.records[0].he_bitmask == 32u &&
          result.records[0].he_bssid_index == 7u);
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

static size_t make_simple_record(uint8_t *bytes, size_t ssid_size,
                                 uint8_t seed)
{
    size_t size = 0u, i;

    append_byte(bytes, &size, 0x0au);
    append_byte(bytes, &size, 0x06u);
    for (i = 0; i < 6u; ++i)
        append_byte(bytes, &size, (uint8_t)(seed + i));
    append_byte(bytes, &size, 0x12u);
    append_varint(bytes, &size, ssid_size);
    for (i = 0; i < ssid_size; ++i)
        append_byte(bytes, &size, (uint8_t)(seed ^ (uint8_t)i));
    return size;
}

static size_t make_records_body(uint8_t *body, unsigned int count,
                                size_t ssid_size)
{
    uint8_t record[64];
    size_t body_size = 0u;
    size_t record_size;
    unsigned int i;

    append_byte(body, &body_size, 0x10u);
    append_varint(body, &body_size, count);
    for (i = 0; i < count; ++i) {
        record_size = make_simple_record(record, ssid_size,
                                         (uint8_t)(0x20u + i * 8u));
        append_byte(body, &body_size, 0x1au);
        append_varint(body, &body_size, record_size);
        for (size_t j = 0; j < record_size; ++j)
            append_byte(body, &body_size, record[j]);
    }
    return body_size;
}

static void test_records_capacity_and_binary_lengths(void)
{
    uint8_t body[1024];
    size_t body_size;
    struct p4_hosted_rpc rpc;
    struct p4_hosted_scan_records result;
    unsigned int i;

    body_size = make_records_body(body, 8u, 32u);
    set_response(&rpc, 289u, 19u, body, body_size);
    memset(&result, 0, sizeof result);
    CHECK(p4_hosted_scan_records_response(&rpc, 19u, 8u, &result));
    CHECK(result.number == 8u && result.record_count == 8u);
    for (i = 0; i < 8u; ++i) {
        CHECK(result.records[i].ssid_size == 32u);
        CHECK(result.records[i].bssid[0] == (uint8_t)(0x20u + i * 8u));
        CHECK(result.records[i].ssid[31] ==
              (uint8_t)((0x20u + i * 8u) ^ 31u));
    }

    body_size = make_records_body(body, 8u, 0u);
    set_response(&rpc, 289u, 19u, body, body_size);
    CHECK(p4_hosted_scan_records_response(&rpc, 19u, 8u, &result));
    CHECK(result.number == 8u && result.records[7].ssid_size == 0u);

    body_size = make_records_body(body, 9u, 0u);
    set_response(&rpc, 289u, 19u, body, body_size);
    expect_records_refusal(&rpc, 19u, 8u);
    CHECK(!p4_hosted_scan_records_response(&rpc, 19u, 9u, &result));

    {
        uint8_t record[64];
        size_t record_size = make_simple_record(record, 2u, 0u);
        size_t offset = 0u;

        body[offset++] = 0x10u;
        body[offset++] = 0x01u;
        body[offset++] = 0x1au;
        body[offset++] = (uint8_t)record_size;
        for (i = 0; i < record_size; ++i)
            body[offset++] = record[i];
        set_response(&rpc, 289u, 19u, body, offset);
        CHECK(p4_hosted_scan_records_response(&rpc, 19u, 1u, &result));
        CHECK(result.records[0].ssid_size == 2u &&
              result.records[0].ssid[0] == 0u &&
              result.records[0].ssid[1] == 1u);
    }
}

static void test_records_zero_and_unknowns(void)
{
    static const uint8_t zero_records[] = { 0x08, 0x00, 0x10, 0x00 };
    static const uint8_t unknowns[] = {
        0x20, 0x7f,
        0x21, 1, 2, 3, 4, 5, 6, 7, 8,
        0x2a, 3, 0xaa, 0xbb, 0xcc,
        0x35, 0x11, 0x22, 0x33, 0x44,
        0x08, 0x00, 0x10, 0x00
    };
    struct p4_hosted_rpc rpc;
    struct p4_hosted_scan_records result;

    set_response(&rpc, 289u, 7u, NULL, 0u);
    memset(&result, 0xa5, sizeof result);
    CHECK(p4_hosted_scan_records_response(&rpc, 7u, 0u, &result));
    CHECK(result.status == 0 && result.number == 0u &&
          result.record_count == 0u);
    {
        const uint8_t all_zero[sizeof result] = { 0 };
        CHECK(memcmp(&result, all_zero, sizeof result) == 0);
    }

    set_response(&rpc, 289u, 7u, zero_records, sizeof zero_records);
    CHECK(p4_hosted_scan_records_response(&rpc, 7u, 1u, &result));
    CHECK(result.number == 0u && result.record_count == 0u);

    set_response(&rpc, 289u, 7u, unknowns, sizeof unknowns);
    CHECK(p4_hosted_scan_records_response(&rpc, 7u, 0u, &result));
    CHECK(result.number == 0u && result.record_count == 0u);
}

static void test_records_malformed_fields(void)
{
    static const uint8_t no_number[] = { 0x1a, 0x08,
                                          0x0a, 0x06, 1, 2, 3, 4, 5, 6 };
    static const uint8_t number_mismatch[] = { 0x10, 0x02, 0x1a, 0x08,
                                                0x0a, 0x06, 1, 2, 3, 4, 5, 6 };
    static const uint8_t duplicate_number[] = { 0x10, 0x01, 0x10, 0x01 };
    static const uint8_t negative_number[] = {
        0x10, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0x01
    };
    static const uint8_t wrong_number_wire[] = { 0x12, 0x00 };
    static const uint8_t overlong_number[] = { 0x10, 0x81, 0x00 };
    static const uint8_t duplicate_status[] = { 0x08, 0x00, 0x08, 0x00 };
    static const uint8_t no_bssid[] = { 0x10, 0x01, 0x1a, 0x02, 0x12, 0x00 };
    static const uint8_t short_bssid[] = {
        0x10, 0x01, 0x1a, 0x09, 0x0a, 0x05, 1, 2, 3, 4, 5
    };
    static const uint8_t duplicate_bssid[] = {
        0x10, 0x01, 0x1a, 0x10,
        0x0a, 0x06, 1, 2, 3, 4, 5, 6,
        0x0a, 0x06, 1, 2, 3, 4, 5, 6
    };
    static const uint8_t long_ssid[] = {
        0x10, 0x01, 0x1a, 0x2b,
        0x0a, 0x06, 1, 2, 3, 4, 5, 6,
        0x12, 0x21,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    };
    static const uint8_t bad_known_wire[] = {
        0x10, 0x01, 0x1a, 0x0a, 0x0a, 0x06, 1, 2, 3, 4, 5, 6,
        0x1a, 0x00
    };
    static const uint8_t record_group[] = {
        0x10, 0x01, 0x1a, 0x0a, 0x0b, 0x0c,
        0x0a, 0x06, 1, 2, 3, 4, 5, 6
    };
    static const struct {
        const uint8_t *body;
        size_t size;
    } malformed[] = {
        { no_number, sizeof no_number },
        { number_mismatch, sizeof number_mismatch },
        { duplicate_number, sizeof duplicate_number },
        { negative_number, sizeof negative_number },
        { wrong_number_wire, sizeof wrong_number_wire },
        { overlong_number, sizeof overlong_number },
        { duplicate_status, sizeof duplicate_status },
        { no_bssid, sizeof no_bssid },
        { short_bssid, sizeof short_bssid },
        { duplicate_bssid, sizeof duplicate_bssid },
        { long_ssid, sizeof long_ssid },
        { bad_known_wire, sizeof bad_known_wire },
        { record_group, sizeof record_group }
    };
    struct p4_hosted_rpc rpc;
    size_t i;

    for (i = 0; i < sizeof malformed / sizeof malformed[0]; ++i) {
        set_response(&rpc, 289u, 7u, malformed[i].body, malformed[i].size);
        expect_records_refusal(&rpc, 7u, 8u);
    }
}

static void test_nested_malformed_and_preserved_fields(void)
{
    static const uint8_t duplicate_country_field[] = {
        0x10, 0x01, 0x1a, 0x13,
        0x0a, 0x06, 1, 2, 3, 4, 5, 6,
        0x5a, 0x05, 0x0a, 0x01, 'U', 0x0a, 0x00
    };
    static const uint8_t country_cc_too_long[] = {
        0x10, 0x01, 0x1a, 0x10,
        0x0a, 0x06, 1, 2, 3, 4, 5, 6,
        0x5a, 0x06, 0x0a, 0x04, 'U', 'S', 'A', 'X'
    };
    static const uint8_t duplicate_country_child[] = {
        0x10, 0x01, 0x1a, 0x11,
        0x0a, 0x06, 1, 2, 3, 4, 5, 6,
        0x5a, 0x07, 0x10, 0x01, 0x10, 0x02, 0x18, 0x01
    };
    static const uint8_t duplicate_country_parent[] = {
        0x10, 0x01, 0x1a, 0x0c,
        0x0a, 0x06, 1, 2, 3, 4, 5, 6,
        0x5a, 0x00, 0x5a, 0x00
    };
    static const uint8_t country_cc_three[] = {
        0x10, 0x01, 0x1a, 0x0f,
        0x0a, 0x06, 1, 2, 3, 4, 5, 6,
        0x5a, 0x05, 0x0a, 0x03, 'U', 'S', 'A'
    };
    static const uint8_t country_bad_wire[] = {
        0x10, 0x01, 0x1a, 0x0b,
        0x0a, 0x06, 1, 2, 3, 4, 5, 6,
        0x5a, 0x01, 0x08
    };
    static const uint8_t he_duplicate[] = {
        0x10, 0x01, 0x1a, 0x11,
        0x0a, 0x06, 1, 2, 3, 4, 5, 6,
        0x62, 0x07, 0x08, 0x01, 0x08, 0x02, 0x10, 0x03
    };
    static const uint8_t he_bad_uint32[] = {
        0x10, 0x01, 0x1a, 0x10,
        0x0a, 0x06, 1, 2, 3, 4, 5, 6,
        0x62, 0x06, 0x08, 0x80, 0x80, 0x80, 0x80, 0x10
    };
    static const uint8_t unknowns_inside_ap[] = {
        0x10, 0x01, 0x1a, 0x1f,
        0x0a, 0x06, 1, 2, 3, 4, 5, 6,
        0x78, 0x01, 0x81, 0x01, 1, 2, 3, 4, 5, 6, 7, 8,
        0x8a, 0x01, 0x02, 0xaa, 0xbb,
        0x95, 0x01, 1, 2, 3, 4
    };
    static const struct {
        const uint8_t *body;
        size_t size;
    } malformed[] = {
        { duplicate_country_field, sizeof duplicate_country_field },
        { country_cc_too_long, sizeof country_cc_too_long },
        { duplicate_country_child, sizeof duplicate_country_child },
        { duplicate_country_parent, sizeof duplicate_country_parent },
        { country_bad_wire, sizeof country_bad_wire },
        { he_duplicate, sizeof he_duplicate },
        { he_bad_uint32, sizeof he_bad_uint32 }
    };
    struct p4_hosted_rpc rpc;
    struct p4_hosted_scan_records result;
    size_t i;

    for (i = 0; i < sizeof malformed / sizeof malformed[0]; ++i) {
        set_response(&rpc, 289u, 7u, malformed[i].body, malformed[i].size);
        expect_records_refusal(&rpc, 7u, 8u);
    }

    set_response(&rpc, 289u, 7u, country_cc_three,
                 sizeof country_cc_three);
    CHECK(p4_hosted_scan_records_response(&rpc, 7u, 1u, &result));
    CHECK(result.records[0].country_cc_size == 3u &&
          result.records[0].country_cc[0] == 'U' &&
          result.records[0].country_cc[2] == 'A');

    set_response(&rpc, 289u, 7u, unknowns_inside_ap,
                 sizeof unknowns_inside_ap);
    CHECK(p4_hosted_scan_records_response(&rpc, 7u, 1u, &result));
    CHECK(result.record_count == 1u && result.records[0].primary == 0u);
}

static void test_records_response_limits_and_aliasing(void)
{
    uint8_t body[P4_HOSTED_SCAN_MAX_BODY_SIZE + 1u];
    struct p4_hosted_rpc rpc;
    struct p4_hosted_scan_records result;

    /* Two-byte unknown fields fill the exact local body bound. */
    for (size_t i = 0; i < P4_HOSTED_SCAN_MAX_BODY_SIZE; i += 2u) {
        body[i] = 0x78u;
        body[i + 1u] = 0x00u;
    }
    set_response(&rpc, 289u, 7u, body, P4_HOSTED_SCAN_MAX_BODY_SIZE);
    CHECK(p4_hosted_scan_records_response(&rpc, 7u, 0u, &result));
    body[P4_HOSTED_SCAN_MAX_BODY_SIZE] = 0u;
    set_response(&rpc, 289u, 7u, body, sizeof body);
    expect_records_refusal(&rpc, 7u, 8u);

    set_response(&rpc, 288u, 7u, NULL, 0u);
    expect_records_refusal(&rpc, 7u, 8u);
    set_response(&rpc, 289u, 7u, NULL, 0u);
    expect_records_refusal(&rpc, 8u, 8u);
    rpc.msg_type = P4_HOSTED_RPC_TYPE_REQUEST;
    expect_records_refusal(&rpc, 7u, 8u);

    {
        const struct p4_hosted_rpc *overflowing =
            (const struct p4_hosted_rpc *)(uintptr_t)(UINTPTR_MAX - 1u);
        CHECK(!p4_hosted_scan_records_response(overflowing, 7u, 8u,
                                                &result));
    }
    {
        set_response(&rpc, 289u, 7u,
                     (const uint8_t *)(uintptr_t)(UINTPTR_MAX - 1u), 4u);
        expect_records_refusal(&rpc, 7u, 8u);
    }
    {
        struct p4_hosted_scan_records *overflowing =
            (struct p4_hosted_scan_records *)(uintptr_t)(UINTPTR_MAX - 1u);
        set_response(&rpc, 289u, 7u, NULL, 0u);
        CHECK(!p4_hosted_scan_records_response(&rpc, 7u, 8u, overflowing));
    }
    {
        union {
            struct p4_hosted_rpc rpc;
            struct p4_hosted_scan_records result;
        } overlap;
        memset(&overlap, 0, sizeof overlap);
        overlap.rpc.msg_type = P4_HOSTED_RPC_TYPE_RESPONSE;
        overlap.rpc.msg_id = 545u;
        overlap.rpc.uid = 7u;
        CHECK(!p4_hosted_scan_records_response(&overlap.rpc, 7u, 8u,
                                               &overlap.result));
    }
    {
        union {
            struct p4_hosted_scan_records result;
            uint8_t body[sizeof(struct p4_hosted_scan_records)];
        } overlap;
        memset(&overlap, 0, sizeof overlap);
        set_response(&rpc, 289u, 7u, overlap.body, 2u);
        CHECK(!p4_hosted_scan_records_response(&rpc, 7u, 8u,
                                               &overlap.result));
    }
}

static void test_count_and_status_aliasing(void)
{
    struct p4_hosted_rpc rpc;

    set_response(&rpc, 288u, 7u, NULL, 0u);
    CHECK(!p4_hosted_scan_num_response(&rpc, 7u, NULL));
    {
        struct p4_hosted_scan_count *overflowing =
            (struct p4_hosted_scan_count *)(uintptr_t)(UINTPTR_MAX - 1u);
        CHECK(!p4_hosted_scan_num_response(&rpc, 7u, overflowing));
    }
    {
        union {
            struct p4_hosted_rpc rpc;
            struct p4_hosted_scan_count count;
        } overlap;
        memset(&overlap, 0, sizeof overlap);
        overlap.rpc.msg_type = P4_HOSTED_RPC_TYPE_RESPONSE;
        overlap.rpc.msg_id = 544u;
        overlap.rpc.uid = 7u;
        CHECK(!p4_hosted_scan_num_response(&overlap.rpc, 7u,
                                           &overlap.count));
    }
}

int main(void)
{
    test_request_vectors();
    test_request_refusal_and_aliasing();
    test_status_responses();
    test_status_refusals();
    test_num_responses();
    test_num_refusals();
    test_records_literal_vector();
    test_records_capacity_and_binary_lengths();
    test_records_zero_and_unknowns();
    test_records_malformed_fields();
    test_nested_malformed_and_preserved_fields();
    test_records_response_limits_and_aliasing();
    test_count_and_status_aliasing();
    printf("hosted_scan: %lu checks\n", checks);
    return 0;
}
