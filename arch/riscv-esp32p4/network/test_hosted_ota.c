/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hosted_ota.h"

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

static void expect_request(const uint8_t *bytes, size_t size,
                           uint32_t request_id, uint32_t uid,
                           const uint8_t *body, size_t body_size)
{
    struct p4_hosted_rpc rpc;

    memset(&rpc, 0xa5, sizeof rpc);
    CHECK(p4_hosted_rpc_decode(bytes, size, &rpc));
    CHECK(rpc.msg_type == P4_HOSTED_RPC_TYPE_REQUEST);
    CHECK(rpc.msg_id == request_id);
    CHECK(rpc.uid == uid);
    CHECK(rpc.payload_size == body_size);
    if (body_size != 0u)
        CHECK(memcmp(rpc.payload, body, body_size) == 0);
}

static void set_response(struct p4_hosted_rpc *rpc, uint32_t request_id,
                         uint32_t uid, uint32_t type, const uint8_t *body,
                         size_t body_size)
{
    rpc->msg_type = type;
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

    CHECK(!p4_hosted_ota_status_response(rpc, request_id, uid, &status));
    CHECK(status == before);
}

static void test_request_vectors(void)
{
    static const uint8_t begin_uid1[] = {
        0x08, 0x01, 0x10, 0x90, 0x02, 0x18, 0x01,
        0x82, 0x11, 0x00
    };
    static const uint8_t write_uid1[] = {
        0x08, 0x01, 0x10, 0x91, 0x02, 0x18, 0x01,
        0x8a, 0x11, 0x03, 0x0a, 0x01, 0xa5
    };
    static const uint8_t end_uid1[] = {
        0x08, 0x01, 0x10, 0x92, 0x02, 0x18, 0x01,
        0x92, 0x11, 0x00
    };
    static const uint8_t write_body[] = { 0x0a, 0x01, 0xa5 };
    const uint8_t one_byte[] = { 0xa5 };
    uint8_t bytes[P4_HOSTED_OTA_MAX_REQUEST_SIZE];
    size_t written = 0u;

    memset(bytes, 0xa5, sizeof bytes);
    CHECK(p4_hosted_ota_begin_request(1u, bytes, sizeof bytes, &written));
    check_bytes(bytes, written, begin_uid1, sizeof begin_uid1);
    expect_request(bytes, written, 272u, 1u, NULL, 0u);

    memset(bytes, 0xa5, sizeof bytes);
    written = 0u;
    CHECK(p4_hosted_ota_write_request(1u, one_byte, sizeof one_byte,
                                      bytes, sizeof bytes, &written));
    check_bytes(bytes, written, write_uid1, sizeof write_uid1);
    expect_request(bytes, written, 273u, 1u,
                   write_body, sizeof write_body);

    memset(bytes, 0xa5, sizeof bytes);
    written = 0u;
    CHECK(p4_hosted_ota_end_request(1u, bytes, sizeof bytes, &written));
    check_bytes(bytes, written, end_uid1, sizeof end_uid1);
    expect_request(bytes, written, 274u, 1u, NULL, 0u);
}

static void test_maximum_request_and_input_ownership(void)
{
    uint8_t data[P4_HOSTED_OTA_MAX_WRITE_SIZE];
    uint8_t data_before[sizeof data];
    uint8_t bytes[P4_HOSTED_OTA_MAX_REQUEST_SIZE];
    struct p4_hosted_rpc rpc;
    size_t written = 0u;
    size_t i;

    for (i = 0u; i < sizeof data; ++i)
        data[i] = (uint8_t)(i * 31u + 7u);
    memcpy(data_before, data, sizeof data);
    memset(bytes, 0x6d, sizeof bytes);
    CHECK(p4_hosted_ota_write_request(UINT32_MAX, data, sizeof data,
                                     bytes, sizeof bytes, &written));
    CHECK(written == P4_HOSTED_OTA_MAX_REQUEST_SIZE);
    CHECK(p4_hosted_rpc_decode(bytes, written, &rpc));
    CHECK(rpc.msg_id == 273u && rpc.uid == UINT32_MAX);
    CHECK(rpc.payload_size == 1u + 2u + sizeof data);
    CHECK(rpc.payload[0] == 0x0au && rpc.payload[1] == 0xeeu &&
          rpc.payload[2] == 0x01u);
    CHECK(memcmp(rpc.payload + 3u, data, sizeof data) == 0);
    CHECK(memcmp(data, data_before, sizeof data) == 0);
    CHECK(P4_HOSTED_OTA_MAX_REQUEST_SIZE == 256u);
    CHECK(P4_HOSTED_OTA_MAX_WRITE_SIZE == 238u);
}

static void expect_request_refusal(uint32_t uid, const uint8_t *data,
                                   size_t data_size, uint8_t *bytes,
                                   size_t capacity, size_t *written,
                                   const uint8_t *before,
                                   size_t before_size, size_t written_before)
{
    CHECK(!p4_hosted_ota_write_request(uid, data, data_size, bytes,
                                       capacity, written));
    CHECK(memcmp(bytes, before, before_size) == 0);
    CHECK(*written == written_before);
}

static void test_request_refusal_and_aliasing(void)
{
    uint8_t source[P4_HOSTED_OTA_MAX_WRITE_SIZE + 1u];
    uint8_t bytes[P4_HOSTED_OTA_MAX_REQUEST_SIZE];
    uint8_t before[sizeof bytes];
    size_t written;
    size_t written_before;

    memset(source, 0x39, sizeof source);
    memset(bytes, 0x6d, sizeof bytes);
    memcpy(before, bytes, sizeof before);
    written = 0x1234u;
    written_before = written;

    expect_request_refusal(0u, source, 1u, bytes, sizeof bytes, &written,
                           before, sizeof before, written_before);
    expect_request_refusal(1u, source, 0u, bytes, sizeof bytes, &written,
                           before, sizeof before, written_before);
    expect_request_refusal(1u, source, sizeof source, bytes, sizeof bytes,
                           &written, before, sizeof before, written_before);
    expect_request_refusal(1u, source, 1u, bytes, 12u, &written,
                           before, sizeof before, written_before);
    expect_request_refusal(UINT32_MAX, source,
                           P4_HOSTED_OTA_MAX_WRITE_SIZE,
                           bytes, P4_HOSTED_OTA_MAX_REQUEST_SIZE - 1u,
                           &written, before, sizeof before, written_before);
    expect_request_refusal(1u, source, 1u, bytes, SIZE_MAX, &written,
                           before, sizeof before, written_before);

    CHECK(!p4_hosted_ota_write_request(1u, NULL, 1u, bytes, sizeof bytes,
                                       &written));
    CHECK(memcmp(bytes, before, sizeof bytes) == 0 && written == written_before);
    CHECK(!p4_hosted_ota_write_request(1u, source, 1u, NULL, sizeof bytes,
                                       &written));
    CHECK(!p4_hosted_ota_write_request(1u, source, 1u, bytes, sizeof bytes,
                                       NULL));

    {
        const uint8_t *bad_source =
            (const uint8_t *)(uintptr_t)(UINTPTR_MAX - 1u);
        CHECK(!p4_hosted_ota_write_request(1u, bad_source, 4u, bytes,
                                           sizeof bytes, &written));
        CHECK(memcmp(bytes, before, sizeof bytes) == 0 &&
              written == written_before);
    }

    CHECK(!p4_hosted_ota_write_request(1u, bytes, 1u, bytes, sizeof bytes,
                                       &written));
    CHECK(memcmp(bytes, before, sizeof bytes) == 0 && written == written_before);

    {
        union {
            size_t value;
            uint8_t bytes[sizeof(size_t)];
        } input_output;
        memset(&input_output, 0x6d, sizeof input_output);
        written = 0x1234u;
        CHECK(!p4_hosted_ota_write_request(
            1u, input_output.bytes, sizeof input_output.bytes,
            bytes, sizeof bytes, &input_output.value));
        CHECK(memcmp(bytes, before, sizeof bytes) == 0);
        CHECK(input_output.value == 0x6d6d6d6d6d6d6d6dULL ||
              input_output.value == (size_t)UINT32_C(0x6d6d6d6d));
        CHECK(written == 0x1234u);
    }

    {
        union {
            size_t alignment;
            uint8_t bytes[P4_HOSTED_OTA_MAX_REQUEST_SIZE];
        } overlapping;
        uint8_t overlap_before[sizeof overlapping.bytes];

        memset(&overlapping, 0x6d, sizeof overlapping);
        memcpy(overlap_before, overlapping.bytes, sizeof overlap_before);
        written = 0x9876u;
        CHECK(!p4_hosted_ota_begin_request(
            1u, overlapping.bytes, sizeof overlapping.bytes,
            &overlapping.alignment));
        CHECK(memcmp(overlapping.bytes, overlap_before,
                     sizeof overlap_before) == 0);
        CHECK(overlapping.alignment == (size_t)UINT32_C(0x6d6d6d6d) ||
              sizeof(size_t) == 8u);
        CHECK(written == 0x9876u);
    }

    written = 0x2468u;
    CHECK(!p4_hosted_ota_begin_request(0u, bytes, sizeof bytes, &written));
    CHECK(memcmp(bytes, before, sizeof bytes) == 0 && written == 0x2468u);
    CHECK(!p4_hosted_ota_begin_request(1u, bytes, 9u, &written));
    CHECK(memcmp(bytes, before, sizeof bytes) == 0 && written == 0x2468u);
    CHECK(!p4_hosted_ota_begin_request(1u, NULL, sizeof bytes, &written));
    CHECK(!p4_hosted_ota_begin_request(1u, bytes, sizeof bytes, NULL));

    written = 0x2468u;
    CHECK(!p4_hosted_ota_end_request(0u, bytes, sizeof bytes, &written));
    CHECK(memcmp(bytes, before, sizeof bytes) == 0 && written == 0x2468u);
    CHECK(!p4_hosted_ota_end_request(1u, bytes, 9u, &written));
    CHECK(memcmp(bytes, before, sizeof bytes) == 0 && written == 0x2468u);
}

static void expect_canonical_unchanged(const uint8_t *bytes, size_t size,
                                       int expected)
{
    uint8_t before[P4_HOSTED_OTA_MAX_REQUEST_SIZE + 1u];

    CHECK(bytes != NULL);
    CHECK(size <= sizeof before);
    memcpy(before, bytes, size);
    CHECK(p4_hosted_ota_request_is_canonical(bytes, size) == expected);
    CHECK(memcmp(bytes, before, size) == 0);
}

static size_t encode_test_rpc(uint32_t type, uint32_t msg_id, uint32_t uid,
                              const uint8_t *payload, size_t payload_size,
                              uint8_t *bytes, size_t capacity)
{
    struct p4_hosted_rpc rpc;
    size_t written = SIZE_MAX;

    rpc.msg_type = type;
    rpc.msg_id = msg_id;
    rpc.uid = uid;
    rpc.payload = payload;
    rpc.payload_size = payload_size;
    CHECK(p4_hosted_rpc_encode(bytes, capacity, &rpc, &written));
    return written;
}

static void test_canonical_valid_requests(void)
{
    static const uint32_t uids[] = {
        1u, 127u, 128u, 16383u, 16384u, 2097151u, 2097152u, UINT32_MAX
    };
    uint8_t data[P4_HOSTED_OTA_MAX_WRITE_SIZE];
    uint8_t bytes[P4_HOSTED_OTA_MAX_REQUEST_SIZE];
    size_t uid_index, data_size, i, written;

    for (i = 0u; i < sizeof data; ++i)
        data[i] = (uint8_t)(i * 73u + 19u);

    for (uid_index = 0u; uid_index < sizeof uids / sizeof uids[0];
         ++uid_index) {
        written = 0u;
        CHECK(p4_hosted_ota_begin_request(uids[uid_index], bytes,
                                          sizeof bytes, &written));
        expect_canonical_unchanged(bytes, written, 1);

        written = 0u;
        CHECK(p4_hosted_ota_end_request(uids[uid_index], bytes,
                                        sizeof bytes, &written));
        expect_canonical_unchanged(bytes, written, 1);

        for (data_size = 1u; data_size <= sizeof data; ++data_size) {
            written = 0u;
            CHECK(p4_hosted_ota_write_request(uids[uid_index], data,
                                               data_size, bytes,
                                               sizeof bytes, &written));
            CHECK(written >= 1u &&
                  written <= P4_HOSTED_OTA_MAX_REQUEST_SIZE);
            if (uid_index == sizeof uids / sizeof uids[0] - 1u &&
                data_size == sizeof data)
                CHECK(written == P4_HOSTED_OTA_MAX_REQUEST_SIZE);
            expect_canonical_unchanged(bytes, written, 1);
        }
    }
}

static void test_canonical_refusals(void)
{
    static const uint8_t begin_uid1[] = {
        0x08, 0x01, 0x10, 0x90, 0x02, 0x18, 0x01,
        0x82, 0x11, 0x00
    };
    static const uint8_t begin_with_body[] = {
        0x08, 0x01, 0x10, 0x90, 0x02, 0x18, 0x01,
        0x82, 0x11, 0x02, 0x18, 0x01
    };
    static const uint8_t begin_uid_zero[] = {
        0x08, 0x01, 0x10, 0x90, 0x02, 0x18, 0x00,
        0x82, 0x11, 0x00
    };
    static const uint8_t begin_uid_omitted[] = {
        0x08, 0x01, 0x10, 0x90, 0x02, 0x82, 0x11, 0x00
    };
    static const uint8_t begin_reordered[] = {
        0x08, 0x01, 0x18, 0x01, 0x10, 0x90, 0x02,
        0x82, 0x11, 0x00
    };
    static const uint8_t begin_duplicate_uid[] = {
        0x08, 0x01, 0x10, 0x90, 0x02, 0x18, 0x01, 0x18, 0x01,
        0x82, 0x11, 0x00
    };
    static const uint8_t begin_duplicate_id[] = {
        0x08, 0x01, 0x10, 0x90, 0x02, 0x10, 0x90, 0x02,
        0x18, 0x01, 0x82, 0x11, 0x00
    };
    static const uint8_t begin_and_end[] = {
        0x08, 0x01, 0x10, 0x90, 0x02, 0x18, 0x01,
        0x82, 0x11, 0x00, 0x92, 0x11, 0x00
    };
    static const uint8_t begin_unknown_envelope_field[] = {
        0x08, 0x01, 0x10, 0x90, 0x02, 0x18, 0x01,
        0x20, 0x01, 0x82, 0x11, 0x00
    };
    static const uint8_t begin_overlong_uid[] = {
        0x08, 0x01, 0x10, 0x90, 0x02, 0x18, 0x81, 0x00,
        0x82, 0x11, 0x00
    };
    static const uint8_t begin_overlong_id[] = {
        0x08, 0x01, 0x10, 0x90, 0x82, 0x00, 0x18, 0x01,
        0x82, 0x11, 0x00
    };
    static const uint8_t end_with_body[] = {
        0x08, 0x01, 0x10, 0x92, 0x02, 0x18, 0x01,
        0x92, 0x11, 0x02, 0x18, 0x01
    };
    static const uint8_t write_empty_data[] = { 0x0a, 0x00 };
    static const uint8_t write_wrong_field[] = { 0x12, 0x01, 0xa5 };
    static const uint8_t write_duplicate_data[] = {
        0x0a, 0x01, 0xa5, 0x0a, 0x01, 0x5a
    };
    static const uint8_t write_unknown_body_field[] = {
        0x0a, 0x01, 0xa5, 0x18, 0x01
    };
    static const uint8_t write_overlong_key[] = {
        0x8a, 0x00, 0x01, 0xa5
    };
    static const uint8_t write_overlong_length[] = {
        0x0a, 0x81, 0x00, 0xa5
    };
    static const uint8_t write_truncated_data[] = { 0x0a, 0x02, 0xa5 };
    static const uint8_t write_bad_trailing_field[] = {
        0x0a, 0x01, 0xa5, 0x18, 0x80
    };
    static const uint8_t write_trailing_declared_length[] = {
        0x0a, 0x01, 0xa5, 0x00
    };
    const uint8_t *bad_payloads[] = {
        write_empty_data, write_wrong_field, write_duplicate_data,
        write_unknown_body_field, write_overlong_key,
        write_overlong_length, write_truncated_data,
        write_bad_trailing_field, write_trailing_declared_length
    };
    const size_t bad_payload_sizes[] = {
        sizeof write_empty_data, sizeof write_wrong_field,
        sizeof write_duplicate_data, sizeof write_unknown_body_field,
        sizeof write_overlong_key, sizeof write_overlong_length,
        sizeof write_truncated_data, sizeof write_bad_trailing_field,
        sizeof write_trailing_declared_length
    };
    const uint8_t *bad_envelopes[] = {
        begin_with_body, begin_uid_zero, begin_uid_omitted, begin_reordered,
        begin_duplicate_uid, begin_duplicate_id, begin_and_end,
        begin_unknown_envelope_field,
        begin_overlong_uid, begin_overlong_id, end_with_body
    };
    const size_t bad_envelope_sizes[] = {
        sizeof begin_with_body, sizeof begin_uid_zero,
        sizeof begin_uid_omitted, sizeof begin_reordered,
        sizeof begin_duplicate_uid, sizeof begin_duplicate_id,
        sizeof begin_and_end, sizeof begin_unknown_envelope_field,
        sizeof begin_overlong_uid, sizeof begin_overlong_id,
        sizeof end_with_body
    };
    uint8_t bytes[P4_HOSTED_OTA_MAX_REQUEST_SIZE + 1u];
    uint8_t before[sizeof bytes];
    uint8_t payload[P4_HOSTED_OTA_MAX_WRITE_SIZE + 4u];
    size_t i, written;

    expect_canonical_unchanged(begin_uid1, sizeof begin_uid1, 1);
    for (i = 0u; i < sizeof bad_envelopes / sizeof bad_envelopes[0]; ++i)
        expect_canonical_unchanged(bad_envelopes[i], bad_envelope_sizes[i], 0);

    for (i = 0u; i < sizeof bad_payloads / sizeof bad_payloads[0]; ++i) {
        written = encode_test_rpc(P4_HOSTED_RPC_TYPE_REQUEST, 273u, 1u,
                                  bad_payloads[i], bad_payload_sizes[i],
                                  bytes, sizeof bytes);
        expect_canonical_unchanged(bytes, written, 0);
    }

    written = encode_test_rpc(P4_HOSTED_RPC_TYPE_REQUEST, 271u, 1u,
                              NULL, 0u, bytes, sizeof bytes);
    expect_canonical_unchanged(bytes, written, 0);
    written = encode_test_rpc(P4_HOSTED_RPC_TYPE_REQUEST, 275u, 1u,
                              NULL, 0u, bytes, sizeof bytes);
    expect_canonical_unchanged(bytes, written, 0);
    written = encode_test_rpc(P4_HOSTED_RPC_TYPE_REQUEST, 273u, 1u,
                              NULL, 0u, bytes, sizeof bytes);
    expect_canonical_unchanged(bytes, written, 0);
    written = encode_test_rpc(P4_HOSTED_RPC_TYPE_RESPONSE, 529u, 1u,
                              NULL, 0u, bytes, sizeof bytes);
    expect_canonical_unchanged(bytes, written, 0);
    written = encode_test_rpc(P4_HOSTED_RPC_TYPE_EVENT, 769u, 1u,
                              NULL, 0u, bytes, sizeof bytes);
    expect_canonical_unchanged(bytes, written, 0);

    memset(bytes, 0x6d, sizeof bytes);
    expect_canonical_unchanged(bytes, 1u, 0);
    expect_canonical_unchanged(bytes, 0u, 0);
    expect_canonical_unchanged(bytes, sizeof bytes, 0);
    CHECK(!p4_hosted_ota_request_is_canonical(NULL, 1u));
    CHECK(!p4_hosted_ota_request_is_canonical(NULL, 0u));
    memcpy(before, bytes, sizeof before);
    CHECK(!p4_hosted_ota_request_is_canonical(bytes, SIZE_MAX));
    CHECK(memcmp(bytes, before, sizeof before) == 0);
    {
        const uint8_t *bad_range =
            (const uint8_t *)(uintptr_t)(UINTPTR_MAX - 1u);
        CHECK(!p4_hosted_ota_request_is_canonical(bad_range, 4u));
    }

    /* A valid envelope with a declared write size beyond the local policy is
     * rejected even though the RPC decoder accepts its outer framing. */
    payload[0] = 0x0au;
    payload[1] = (uint8_t)(P4_HOSTED_OTA_MAX_WRITE_SIZE + 1u);
    payload[2] = 1u; /* Canonical varint239, not a truncated length. */
    memset(payload + 3u, 0xa5, P4_HOSTED_OTA_MAX_WRITE_SIZE + 1u);
    written = encode_test_rpc(P4_HOSTED_RPC_TYPE_REQUEST, 273u, 1u,
                              payload, P4_HOSTED_OTA_MAX_WRITE_SIZE + 4u,
                              bytes, sizeof bytes);
    expect_canonical_unchanged(bytes, written, 0);
}

static void test_status_positive_responses(void)
{
    static const uint8_t explicit_zero[] = { 0x08, 0x00 };
    static const uint8_t nonzero[] = { 0x08, 0x07 };
    static const uint8_t negative_one[] = {
        0x08, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0x01
    };
    static const uint8_t unknown_fields[] = {
        0x18, 0x01,                         /* Unknown varint. */
        0x21, 1, 2, 3, 4, 5, 6, 7, 8,     /* Unknown fixed64. */
        0x2a, 0x02, 0xa5, 0x5a,             /* Unknown bytes. */
        0x35, 1, 2, 3, 4,                   /* Unknown fixed32. */
        0x08, 0x07
    };
    const uint32_t request_ids[] = { 272u, 273u, 274u };
    struct p4_hosted_rpc rpc;
    int32_t status = -1;
    size_t i;

    for (i = 0u; i < sizeof request_ids / sizeof request_ids[0]; ++i) {
        set_response(&rpc, request_ids[i], 9u,
                     P4_HOSTED_RPC_TYPE_RESPONSE, NULL, 0u);
        status = INT32_C(0x12345678);
        CHECK(p4_hosted_ota_status_response(&rpc, request_ids[i], 9u,
                                            &status));
        CHECK(status == 0);
    }

    set_response(&rpc, 273u, 9u, P4_HOSTED_RPC_TYPE_RESPONSE,
                 explicit_zero, sizeof explicit_zero);
    status = -1;
    CHECK(p4_hosted_ota_status_response(&rpc, 273u, 9u, &status));
    CHECK(status == 0);

    set_response(&rpc, 272u, 9u, P4_HOSTED_RPC_TYPE_RESPONSE,
                 nonzero, sizeof nonzero);
    status = -1;
    CHECK(p4_hosted_ota_status_response(&rpc, 272u, 9u, &status));
    CHECK(status == 7);

    set_response(&rpc, 274u, 9u, P4_HOSTED_RPC_TYPE_RESPONSE,
                 negative_one, sizeof negative_one);
    status = 0;
    CHECK(p4_hosted_ota_status_response(&rpc, 274u, 9u, &status));
    CHECK(status == -1);

    set_response(&rpc, 273u, 9u, P4_HOSTED_RPC_TYPE_RESPONSE,
                 unknown_fields, sizeof unknown_fields);
    status = -1;
    CHECK(p4_hosted_ota_status_response(&rpc, 273u, 9u, &status));
    CHECK(status == 7);

    {
        uint8_t maximum[P4_HOSTED_OTA_MAX_RESPONSE_BODY_SIZE];
        for (i = 0u; i < sizeof maximum; i += 2u) {
            maximum[i] = 0x18u;
            maximum[i + 1u] = 0u;
        }
        set_response(&rpc, 273u, 9u, P4_HOSTED_RPC_TYPE_RESPONSE,
                     maximum, sizeof maximum);
        status = -1;
        CHECK(p4_hosted_ota_status_response(&rpc, 273u, 9u, &status));
        CHECK(status == 0);
    }
}

static void test_status_refusals(void)
{
    static const uint8_t zero_key[] = { 0x00 };
    static const uint8_t truncated_key[] = { 0x80 };
    static const uint8_t key_overflow[] = {
        0xff, 0xff, 0xff, 0xff, 0x10
    };
    static const uint8_t wrong_status_wire[] = { 0x0a, 0x00 };
    static const uint8_t truncated_status[] = { 0x08, 0x80 };
    static const uint8_t status_varint_overflow[] = {
        0x08, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0x02
    };
    static const uint8_t invalid_int32_positive[] = {
        0x08, 0x80, 0x80, 0x80, 0x80, 0x08
    };
    static const uint8_t non_sign_extended_int32[] = {
        0x08, 0xff, 0xff, 0xff, 0xff, 0x0f
    };
    static const uint8_t duplicate_status[] = { 0x08, 0x00, 0x08, 0x00 };
    static const uint8_t truncated_unknown_fixed64[] = { 0x21, 1, 2, 3 };
    static const uint8_t truncated_unknown_fixed32[] = { 0x25, 1, 2 };
    static const uint8_t truncated_unknown_length[] = { 0x2a, 3, 1, 2 };
    static const uint8_t unknown_group_start[] = { 0x33 };
    static const uint8_t unknown_group_end[] = { 0x34 };
    static const uint8_t unknown_reserved6[] = { 0x36 };
    static const uint8_t unknown_reserved7[] = { 0x37 };
    static const uint8_t trailing_bad_key[] = { 0x18, 0x00, 0x80 };
    const uint8_t *bad_bodies[] = {
        zero_key, truncated_key, key_overflow, wrong_status_wire,
        truncated_status, status_varint_overflow, invalid_int32_positive,
        non_sign_extended_int32, duplicate_status,
        truncated_unknown_fixed64, truncated_unknown_fixed32,
        truncated_unknown_length, unknown_group_start, unknown_group_end,
        unknown_reserved6, unknown_reserved7, trailing_bad_key
    };
    const size_t bad_sizes[] = {
        sizeof zero_key, sizeof truncated_key, sizeof key_overflow,
        sizeof wrong_status_wire, sizeof truncated_status,
        sizeof status_varint_overflow, sizeof invalid_int32_positive,
        sizeof non_sign_extended_int32, sizeof duplicate_status,
        sizeof truncated_unknown_fixed64, sizeof truncated_unknown_fixed32,
        sizeof truncated_unknown_length, sizeof unknown_group_start,
        sizeof unknown_group_end, sizeof unknown_reserved6,
        sizeof unknown_reserved7, sizeof trailing_bad_key
    };
    struct p4_hosted_rpc rpc;
    uint8_t oversized[P4_HOSTED_OTA_MAX_RESPONSE_BODY_SIZE + 1u];
    size_t i;

    for (i = 0u; i < sizeof bad_bodies / sizeof bad_bodies[0]; ++i) {
        set_response(&rpc, 273u, 9u, P4_HOSTED_RPC_TYPE_RESPONSE,
                     bad_bodies[i], bad_sizes[i]);
        expect_status_refusal(&rpc, 273u, 9u);
    }

    memset(oversized, 0x18, sizeof oversized);
    set_response(&rpc, 273u, 9u, P4_HOSTED_RPC_TYPE_RESPONSE,
                 oversized, sizeof oversized);
    expect_status_refusal(&rpc, 273u, 9u);
    set_response(&rpc, 273u, 9u, P4_HOSTED_RPC_TYPE_RESPONSE, NULL, 1u);
    expect_status_refusal(&rpc, 273u, 9u);

    set_response(&rpc, 273u, 0u, P4_HOSTED_RPC_TYPE_RESPONSE, NULL, 0u);
    expect_status_refusal(&rpc, 273u, 0u);
    expect_status_refusal(&rpc, 273u, 8u);
    expect_status_refusal(&rpc, 272u, 9u);
    expect_status_refusal(&rpc, 274u, 9u);
    expect_status_refusal(&rpc, 271u, 9u);
    expect_status_refusal(&rpc, 275u, 9u);

    set_response(&rpc, 273u, 9u, P4_HOSTED_RPC_TYPE_REQUEST, NULL, 0u);
    expect_status_refusal(&rpc, 273u, 9u);
    set_response(&rpc, 273u, 9u, P4_HOSTED_RPC_TYPE_EVENT, NULL, 0u);
    expect_status_refusal(&rpc, 273u, 9u);
    set_response(&rpc, 273u, 9u, P4_HOSTED_RPC_TYPE_RESPONSE, NULL, 0u);
    rpc.msg_id = 529u;
    rpc.uid = 0u;
    expect_status_refusal(&rpc, 273u, 9u);

    {
        int32_t status = INT32_C(0x5a5a1234);
        CHECK(!p4_hosted_ota_status_response(&rpc, 273u, 9u, NULL));
        CHECK(status == INT32_C(0x5a5a1234));
        CHECK(!p4_hosted_ota_status_response(NULL, 273u, 9u, &status));
        CHECK(status == INT32_C(0x5a5a1234));
        CHECK(!p4_hosted_ota_status_response(&rpc, 273u, 9u,
                                              (int32_t *)(void *)&rpc));
        CHECK(status == INT32_C(0x5a5a1234));
    }

    {
        const struct p4_hosted_rpc *bad_rpc =
            (const struct p4_hosted_rpc *)(uintptr_t)(UINTPTR_MAX - 1u);
        int32_t status = INT32_C(0x5a5a1234);
        CHECK(!p4_hosted_ota_status_response(bad_rpc, 273u, 9u, &status));
        CHECK(status == INT32_C(0x5a5a1234));
    }

    {
        union {
            int32_t aligned_status;
            uint8_t body[sizeof(int32_t)];
        } overlapping;
        uint8_t before[sizeof overlapping.body];

        memset(&overlapping, 0, sizeof overlapping);
        overlapping.body[0] = 0x08u;
        overlapping.body[1] = 0x00u;
        memcpy(before, overlapping.body, sizeof before);
        set_response(&rpc, 273u, 9u, P4_HOSTED_RPC_TYPE_RESPONSE,
                     overlapping.body, 2u);
        CHECK(!p4_hosted_ota_status_response(&rpc, 273u, 9u,
                                              &overlapping.aligned_status));
        CHECK(memcmp(overlapping.body, before, sizeof before) == 0);
    }
}

int main(void)
{
    test_request_vectors();
    test_maximum_request_and_input_ownership();
    test_request_refusal_and_aliasing();
    test_canonical_valid_requests();
    test_canonical_refusals();
    test_status_positive_responses();
    test_status_refusals();

    printf("C6 OTA codec: %lu checks passed\n", checks);
    return 0;
}
