/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "hosted_mac.h"

static unsigned long checks;

#define CHECK(expression) do { \
    ++checks; \
    if (!(expression)) { \
        fprintf(stderr, "line %u: %s\n", (unsigned)__LINE__, #expression); \
        exit(1); \
    } \
} while (0)

static const uint8_t good_mac[P4_HOSTED_MAC_ADDRESS_SIZE] = {
    0x10, 0xbd, 0xa3, 0x9b, 0xbc, 0x34
};

static void set_response(struct p4_hosted_rpc *rpc, uint32_t uid,
                         const uint8_t *body, size_t body_size)
{
    rpc->msg_type = P4_HOSTED_RPC_TYPE_RESPONSE;
    rpc->msg_id = 513u;
    rpc->uid = uid;
    rpc->payload = body;
    rpc->payload_size = body_size;
}

static void check_mac(const struct p4_hosted_mac *result,
                      const uint8_t expected[P4_HOSTED_MAC_ADDRESS_SIZE])
{
    size_t i;
    for (i = 0; i < P4_HOSTED_MAC_ADDRESS_SIZE; ++i)
        CHECK(result->mac[i] == expected[i]);
}

static void expect_response_refusal(const struct p4_hosted_rpc *rpc,
                                    uint32_t expected_uid)
{
    struct p4_hosted_mac result;
    uint8_t before[sizeof result];

    memset(&result, 0xa5, sizeof result);
    memcpy(before, &result, sizeof result);
    CHECK(!p4_hosted_mac_response(rpc, expected_uid, &result));
    CHECK(memcmp(&result, before, sizeof result) == 0);
}

static void test_canonical_request(void)
{
    static const uint8_t expected[] = {
        0x08, 0x01,             /* request */
        0x10, 0x81, 0x02,      /* GetMACAddress (257) */
        0x18, 0x01,             /* UID 1 */
        0x8a, 0x10, 0x00       /* empty request body */
    };
    uint8_t bytes[32];
    size_t written = 0;

    memset(bytes, 0xa5, sizeof bytes);
    CHECK(p4_hosted_mac_request(1u, bytes, sizeof bytes, &written));
    CHECK(written == sizeof expected);
    CHECK(memcmp(bytes, expected, sizeof expected) == 0);
}

static void test_request_refusal_and_alias(void)
{
    uint8_t bytes[32], before[sizeof bytes];
    size_t written;

    memset(bytes, 0x6d, sizeof bytes);
    memcpy(before, bytes, sizeof bytes);
    written = 0x1234u;
    CHECK(!p4_hosted_mac_request(0u, bytes, sizeof bytes, &written));
    CHECK(memcmp(bytes, before, sizeof bytes) == 0);
    CHECK(written == 0x1234u);

    CHECK(!p4_hosted_mac_request(1u, bytes, 8u, &written));
    CHECK(memcmp(bytes, before, sizeof bytes) == 0);
    CHECK(written == 0x1234u);

    CHECK(!p4_hosted_mac_request(1u, bytes, SIZE_MAX, &written));
    CHECK(memcmp(bytes, before, sizeof bytes) == 0);
    CHECK(written == 0x1234u);
    CHECK(!p4_hosted_mac_request(1u, NULL, sizeof bytes, &written));
    CHECK(!p4_hosted_mac_request(1u, bytes, sizeof bytes, NULL));

    {
        union {
            size_t alignment;
            uint8_t bytes[32];
        } overlap;
        size_t i;

        for (i = 0; i < sizeof overlap.bytes; ++i)
            overlap.bytes[i] = 0x37;
        * (size_t *)(void *)overlap.bytes = 0x9876u;
        memcpy(before, overlap.bytes, sizeof before);
        CHECK(!p4_hosted_mac_request(1u, overlap.bytes,
                                     sizeof overlap.bytes,
                                     (size_t *)(void *)overlap.bytes));
        CHECK(memcmp(overlap.bytes, before, sizeof before) == 0);
    }
}

static void test_valid_and_default_responses(void)
{
    static const uint8_t mac_only[] = {
        0x0a, 0x06, 0x10, 0xbd, 0xa3, 0x9b, 0xbc, 0x34
    };
    static const uint8_t explicit_status_zero[] = {
        0x10, 0x00,
        0x0a, 0x06, 0x10, 0xbd, 0xa3, 0x9b, 0xbc, 0x34
    };
    static const uint8_t no_mac[] = { 0x10, 0x00 };
    struct p4_hosted_rpc rpc;
    struct p4_hosted_mac result;

    set_response(&rpc, 9u, mac_only, sizeof mac_only);
    CHECK(p4_hosted_mac_response(&rpc, 9u, &result));
    CHECK(result.status == 0);
    CHECK(result.valid_mac == 1u);
    check_mac(&result, good_mac);

    set_response(&rpc, 9u, explicit_status_zero, sizeof explicit_status_zero);
    CHECK(p4_hosted_mac_response(&rpc, 9u, &result));
    CHECK(result.status == 0);
    CHECK(result.valid_mac == 1u);
    check_mac(&result, good_mac);

    set_response(&rpc, 9u, no_mac, sizeof no_mac);
    CHECK(p4_hosted_mac_response(&rpc, 9u, &result));
    CHECK(result.status == 0);
    CHECK(result.valid_mac == 0u);
    {
        static const uint8_t zero[P4_HOSTED_MAC_ADDRESS_SIZE] = { 0 };
        check_mac(&result, zero);
    }

    set_response(&rpc, 9u, NULL, 0u);
    CHECK(p4_hosted_mac_response(&rpc, 9u, &result));
    CHECK(result.status == 0);
    CHECK(result.valid_mac == 0u);
}

static void test_status_values_and_mac_rules(void)
{
    static const uint8_t error_negative[] = {
        0x0a, 0x06, 0x10, 0xbd, 0xa3, 0x9b, 0xbc, 0x34,
        0x10, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x01
    };
    static const uint8_t error_positive[] = {
        0x10, 0x8a, 0x60,
        0x0a, 0x06, 0x10, 0xbd, 0xa3, 0x9b, 0xbc, 0x34
    };
    static const uint8_t zero_mac[] = {
        0x0a, 0x06, 0, 0, 0, 0, 0, 0
    };
    static const uint8_t multicast_mac[] = {
        0x0a, 0x06, 0x11, 0x02, 0x03, 0x04, 0x05, 0x06
    };
    static const uint8_t short_mac[] = {
        0x0a, 0x05, 0x10, 0xbd, 0xa3, 0x9b, 0xbc
    };
    static const uint8_t long_mac[] = {
        0x0a, 0x07, 0x10, 0xbd, 0xa3, 0x9b, 0xbc, 0x34, 0x00
    };
    struct p4_hosted_rpc rpc;
    struct p4_hosted_mac result;

    set_response(&rpc, 9u, error_negative, sizeof error_negative);
    CHECK(p4_hosted_mac_response(&rpc, 9u, &result));
    CHECK(result.status == -1);
    CHECK(result.valid_mac == 0u);
    check_mac(&result, good_mac);

    set_response(&rpc, 9u, error_positive, sizeof error_positive);
    CHECK(p4_hosted_mac_response(&rpc, 9u, &result));
    CHECK(result.status == 0x300a);
    CHECK(result.valid_mac == 0u);

    set_response(&rpc, 9u, zero_mac, sizeof zero_mac);
    CHECK(p4_hosted_mac_response(&rpc, 9u, &result));
    CHECK(result.status == 0 && result.valid_mac == 0u);

    set_response(&rpc, 9u, multicast_mac, sizeof multicast_mac);
    CHECK(p4_hosted_mac_response(&rpc, 9u, &result));
    CHECK(result.status == 0 && result.valid_mac == 0u);

    set_response(&rpc, 9u, short_mac, sizeof short_mac);
    CHECK(p4_hosted_mac_response(&rpc, 9u, &result));
    CHECK(result.status == 0 && result.valid_mac == 0u);
    {
        static const uint8_t zero[P4_HOSTED_MAC_ADDRESS_SIZE] = { 0 };
        check_mac(&result, zero);
    }

    set_response(&rpc, 9u, long_mac, sizeof long_mac);
    CHECK(p4_hosted_mac_response(&rpc, 9u, &result));
    CHECK(result.status == 0 && result.valid_mac == 0u);
}

static void test_unknown_field_skipping_and_body_limit(void)
{
    static const uint8_t with_unknowns[] = {
        0x18, 0x96, 0x01,             /* unknown varint */
        0x21, 1, 2, 3, 4, 5, 6, 7, 8,/* unknown fixed64 */
        0x2a, 3, 0xaa, 0xbb, 0xcc,   /* unknown length-delimited */
        0x35, 0x11, 0x22, 0x33, 0x44,/* unknown fixed32 */
        0x0a, 0x06, 0x10, 0xbd, 0xa3, 0x9b, 0xbc, 0x34
    };
    uint8_t maximum[P4_HOSTED_MAC_MAX_BODY_SIZE];
    struct p4_hosted_rpc rpc;
    struct p4_hosted_mac result;
    size_t i;

    set_response(&rpc, 9u, with_unknowns, sizeof with_unknowns);
    CHECK(p4_hosted_mac_response(&rpc, 9u, &result));
    CHECK(result.valid_mac == 1u);
    check_mac(&result, good_mac);

    for (i = 0; i < sizeof maximum; i += 2u) {
        maximum[i] = 0x18;     /* unknown field 3, varint */
        maximum[i + 1u] = 0;
    }
    set_response(&rpc, 9u, maximum, sizeof maximum);
    CHECK(p4_hosted_mac_response(&rpc, 9u, &result));
    CHECK(result.status == 0 && result.valid_mac == 0u);

    set_response(&rpc, 9u, maximum, sizeof maximum);
    rpc.payload_size = P4_HOSTED_MAC_MAX_BODY_SIZE + 1u;
    expect_response_refusal(&rpc, 9u);
}

static void test_malformed_bodies(void)
{
    static const uint8_t zero_key[] = { 0x00 };
    static const uint8_t truncated_key[] = { 0x80 };
    static const uint8_t key_overflow[] = {
        0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x02
    };
    static const uint8_t field_number_overflow[] = {
        0x80, 0x80, 0x80, 0x80, 0x10
    };
    static const uint8_t wrong_mac_wire[] = { 0x08, 0x00 };
    static const uint8_t wrong_status_wire[] = { 0x12, 0x00 };
    static const uint8_t truncated_mac_length[] = { 0x0a, 0x80 };
    static const uint8_t short_mac_data[] = { 0x0a, 0x06, 1, 2 };
    static const uint8_t malformed_status[] = { 0x10, 0x80 };
    static const uint8_t status_varint_overflow[] = {
        0x10, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x02
    };
    static const uint8_t invalid_int32_positive[] = {
        0x10, 0x80, 0x80, 0x80, 0x80, 0x08
    };
    static const uint8_t non_sign_extended_int32[] = {
        0x10, 0xff, 0xff, 0xff, 0xff, 0x0f
    };
    static const uint8_t duplicate_mac[] = {
        0x0a, 0x06, 0x10, 0xbd, 0xa3, 0x9b, 0xbc, 0x34,
        0x0a, 0x06, 0x10, 0xbd, 0xa3, 0x9b, 0xbc, 0x34
    };
    static const uint8_t duplicate_status[] = { 0x10, 0, 0x10, 0 };
    static const uint8_t truncated_unknown_fixed64[] = { 0x21, 1, 2, 3 };
    static const uint8_t truncated_unknown_fixed32[] = { 0x25, 1, 2 };
    static const uint8_t truncated_unknown_length[] = { 0x2a, 3, 1, 2 };
    static const uint8_t unknown_length_overflow[] = {
        0x2a, 0x80, 0x80, 0x80, 0x80, 0x10
    };
    static const uint8_t unknown_group_start[] = { 0x33 };
    static const uint8_t unknown_group_end[] = { 0x34 };
    static const uint8_t unknown_reserved6[] = { 0x36 };
    static const uint8_t unknown_reserved7[] = { 0x37 };
    static const uint8_t trailing_bad_key[] = { 0x10, 0, 0x80 };
    static const uint8_t bad_int32_negative_truncation[] = {
        0x10, 0xff, 0xff, 0xff, 0xff, 0x1f
    };
    struct p4_hosted_rpc rpc;

#define REJECT(body_) do { \
    set_response(&rpc, 9u, (body_), sizeof (body_)); \
    expect_response_refusal(&rpc, 9u); \
} while (0)
    REJECT(zero_key);
    REJECT(truncated_key);
    REJECT(key_overflow);
    REJECT(field_number_overflow);
    REJECT(wrong_mac_wire);
    REJECT(wrong_status_wire);
    REJECT(truncated_mac_length);
    REJECT(short_mac_data);
    REJECT(malformed_status);
    REJECT(status_varint_overflow);
    REJECT(invalid_int32_positive);
    REJECT(non_sign_extended_int32);
    REJECT(duplicate_mac);
    REJECT(duplicate_status);
    REJECT(truncated_unknown_fixed64);
    REJECT(truncated_unknown_fixed32);
    REJECT(truncated_unknown_length);
    REJECT(unknown_length_overflow);
    REJECT(unknown_group_start);
    REJECT(unknown_group_end);
    REJECT(unknown_reserved6);
    REJECT(unknown_reserved7);
    REJECT(trailing_bad_key);
    REJECT(bad_int32_negative_truncation);
#undef REJECT
}

static void test_metadata_mismatch_and_untouched_result(void)
{
    static const uint8_t good_body[] = {
        0x0a, 0x06, 0x10, 0xbd, 0xa3, 0x9b, 0xbc, 0x34
    };
    struct p4_hosted_rpc rpc;

    set_response(&rpc, 9u, good_body, sizeof good_body);
    expect_response_refusal(&rpc, 0u);
    expect_response_refusal(&rpc, 8u);

    rpc.msg_type = P4_HOSTED_RPC_TYPE_REQUEST;
    expect_response_refusal(&rpc, 9u);
    rpc.msg_type = P4_HOSTED_RPC_TYPE_EVENT;
    expect_response_refusal(&rpc, 9u);
    rpc.msg_type = P4_HOSTED_RPC_TYPE_RESPONSE;
    rpc.msg_id = 512u;
    expect_response_refusal(&rpc, 9u);
    rpc.msg_id = 514u;
    expect_response_refusal(&rpc, 9u);
    rpc.msg_id = 513u;
    rpc.uid = 0u;
    expect_response_refusal(&rpc, 9u);
    rpc.uid = 9u;
    rpc.payload = NULL;
    expect_response_refusal(&rpc, 9u);
    rpc.payload = good_body;
    rpc.payload_size = 1u;
    expect_response_refusal(&rpc, 9u);
}

static void test_response_alias_refusal(void)
{
    struct p4_hosted_rpc rpc;
    struct p4_hosted_mac result;
    uint8_t before[sizeof result];

    memset(&result, 0x5c, sizeof result);
    memcpy(before, &result, sizeof result);
    set_response(&rpc, 9u, (const uint8_t *)&result.status,
                 sizeof result.status);
    CHECK(!p4_hosted_mac_response(&rpc, 9u, &result));
    CHECK(memcmp(&result, before, sizeof result) == 0);

    {
        union {
            struct p4_hosted_rpc rpc;
            struct p4_hosted_mac result;
        } overlap;

        overlap.rpc.msg_type = P4_HOSTED_RPC_TYPE_RESPONSE;
        overlap.rpc.msg_id = 513u;
        overlap.rpc.uid = 9u;
        overlap.rpc.payload = NULL;
        overlap.rpc.payload_size = 0;
        overlap.result.status = 0x12345678;
        overlap.result.mac[0] = 0x55;
        overlap.result.mac[1] = 0x66;
        overlap.result.mac[2] = 0x77;
        overlap.result.mac[3] = 0x88;
        overlap.result.mac[4] = 0x99;
        overlap.result.mac[5] = 0xaa;
        overlap.result.valid_mac = 0xabcdefu;
        memcpy(before, &overlap.result, sizeof overlap.result);
        CHECK(!p4_hosted_mac_response(&overlap.rpc, 9u,
                                     &overlap.result));
        CHECK(memcmp(&overlap.result, before, sizeof overlap.result) == 0);
    }
}

int main(void)
{
    test_canonical_request();
    test_request_refusal_and_alias();
    test_valid_and_default_responses();
    test_status_values_and_mac_rules();
    test_unknown_field_skipping_and_body_limit();
    test_malformed_bodies();
    test_metadata_mismatch_and_untouched_result();
    test_response_alias_refusal();
    printf("PASS: GetMAC nested codec, %lu checks\n", checks);
    return 0;
}
