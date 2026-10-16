/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hosted_wifi.h"

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

    CHECK(!p4_hosted_wifi_status_response(rpc, request_id, uid, &status));
    CHECK(status == before);
}

static void test_init_vectors(void)
{
    static const uint8_t all_zero_request[] = {
        0x08, 0x01, 0x10, 0x96, 0x02, 0x18, 0x01,
        0xb2, 0x11, 0x2f, 0x0a, 0x2d,
        0x08, 0x00, 0x10, 0x00, 0x18, 0x00, 0x20, 0x00,
        0x28, 0x00, 0x30, 0x00, 0x38, 0x00, 0x40, 0x00,
        0x48, 0x00, 0x50, 0x00, 0x58, 0x00, 0x60, 0x00,
        0x68, 0x00, 0x70, 0x00, 0x78, 0x00, 0x80, 0x01,
        0x00, 0x88, 0x01, 0x00, 0x90, 0x01, 0x00, 0x98,
        0x01, 0x00, 0xa0, 0x01, 0x00
    };
    static const uint8_t mixed_request[] = {
        0x08, 0x01, 0x10, 0x96, 0x02, 0x18, 0x01,
        0xb2, 0x11, 0x46, 0x0a, 0x44,
        0x08, 0x01, 0x10, 0x20, 0x18,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x01,
        0x20, 0x10, 0x28, 0x00, 0x30, 0x00, 0x38, 0x00,
        0x40, 0x01, 0x48, 0x01, 0x50, 0x00, 0x58, 0x00,
        0x60, 0x00, 0x68, 0x06, 0x70, 0x00, 0x78, 0xf0, 0x05,
        0x80, 0x01, 0x20, 0x88, 0x01,
        0x80, 0x80, 0x80, 0x80, 0x10,
        0x90, 0x01, 0x01, 0x98, 0x01, 0x07,
        0xa0, 0x01, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0x01
    };
    struct p4_hosted_wifi_config config;
    uint8_t bytes[128];
    size_t written = 0;

    memset(&config, 0, sizeof config);
    memset(bytes, 0xa5, sizeof bytes);
    CHECK(p4_hosted_wifi_init_request(1u, &config, bytes, sizeof bytes,
                                      &written));
    check_bytes(bytes, written, all_zero_request, sizeof all_zero_request);

    config.static_rx_buf_num = 1;
    config.dynamic_rx_buf_num = 32;
    config.tx_buf_type = -1;
    config.static_tx_buf_num = 16;
    config.ampdu_rx_enable = 1;
    config.ampdu_tx_enable = 1;
    config.rx_ba_win = 6;
    config.beacon_max_len = 752;
    config.mgmt_sbuf_num = 32;
    config.feature_caps = UINT64_C(0x100000000);
    config.sta_disconnected_pm = 1u;
    config.espnow_max_encrypt_num = 7;
    config.magic = -1;
    memset(bytes, 0xa5, sizeof bytes);
    written = 0;
    CHECK(p4_hosted_wifi_init_request(1u, &config, bytes, sizeof bytes,
                                      &written));
    check_bytes(bytes, written, mixed_request, sizeof mixed_request);
}

static void test_init_refusal_and_aliasing(void)
{
    struct p4_hosted_wifi_config config;
    uint8_t bytes[128], before[sizeof bytes];
    size_t written;

    memset(&config, 0, sizeof config);
    memset(bytes, 0x6d, sizeof bytes);
    memcpy(before, bytes, sizeof before);
    written = 0x1234u;

    CHECK(!p4_hosted_wifi_init_request(0u, &config, bytes, sizeof bytes,
                                       &written));
    CHECK(memcmp(bytes, before, sizeof bytes) == 0 && written == 0x1234u);
    CHECK(!p4_hosted_wifi_init_request(1u, &config, bytes, 9u, &written));
    CHECK(memcmp(bytes, before, sizeof bytes) == 0 && written == 0x1234u);
    CHECK(!p4_hosted_wifi_init_request(1u, &config, bytes, SIZE_MAX,
                                       &written));
    CHECK(memcmp(bytes, before, sizeof bytes) == 0 && written == 0x1234u);
    CHECK(!p4_hosted_wifi_init_request(1u, &config, NULL, 0u, &written));
    CHECK(!p4_hosted_wifi_init_request(1u, &config, bytes, sizeof bytes,
                                       NULL));

    {
        const struct p4_hosted_wifi_config *overflowing =
            (const struct p4_hosted_wifi_config *)(uintptr_t)(UINTPTR_MAX - 1u);
        CHECK(!p4_hosted_wifi_init_request(1u, overflowing, bytes,
                                           sizeof bytes, &written));
        CHECK(memcmp(bytes, before, sizeof bytes) == 0 && written == 0x1234u);
    }

    config.sta_disconnected_pm = 2u;
    CHECK(!p4_hosted_wifi_init_request(1u, &config, bytes, sizeof bytes,
                                       &written));
    CHECK(memcmp(bytes, before, sizeof bytes) == 0 && written == 0x1234u);
    config.sta_disconnected_pm = 0u;

    {
        union {
            struct p4_hosted_wifi_config config;
            uint8_t bytes[sizeof(struct p4_hosted_wifi_config)];
        } overlap;
        uint8_t snapshot[sizeof overlap.bytes];

        memset(&overlap, 0, sizeof overlap);
        memcpy(snapshot, overlap.bytes, sizeof snapshot);
        CHECK(!p4_hosted_wifi_init_request(1u, &overlap.config,
                                           overlap.bytes, sizeof overlap.bytes,
                                           &written));
        CHECK(memcmp(overlap.bytes, snapshot, sizeof snapshot) == 0);
    }

    {
        union {
            struct p4_hosted_wifi_config config;
            size_t written;
        } overlap;
        uint8_t other[128];

        memset(&overlap, 0, sizeof overlap);
        memset(other, 0x39, sizeof other);
        memcpy(before, other, sizeof before);
        CHECK(!p4_hosted_wifi_init_request(1u, &overlap.config, other,
                                           sizeof other, &overlap.written));
        CHECK(memcmp(other, before, sizeof other) == 0);
    }
}

static void test_simple_and_set_mode_requests(void)
{
    static const struct {
        uint32_t request_id;
        uint8_t bytes[10];
    } vectors[] = {
        { 279u, { 0x08, 0x01, 0x10, 0x97, 0x02,
                  0x18, 0x01, 0xba, 0x11, 0x00 } },
        { 280u, { 0x08, 0x01, 0x10, 0x98, 0x02,
                  0x18, 0x01, 0xc2, 0x11, 0x00 } },
        { 281u, { 0x08, 0x01, 0x10, 0x99, 0x02,
                  0x18, 0x01, 0xca, 0x11, 0x00 } }
    };
    static const uint8_t set_mode[] = {
        0x08, 0x01, 0x10, 0x84, 0x02, 0x18, 0x01,
        0xa2, 0x10, 0x02, 0x08, 0x01
    };
    uint8_t bytes[64];
    size_t written;
    size_t i;

    for (i = 0; i < sizeof vectors / sizeof vectors[0]; ++i) {
        memset(bytes, 0xa5, sizeof bytes);
        written = 0;
        CHECK(p4_hosted_wifi_simple_request(vectors[i].request_id, 1u,
                                            bytes, sizeof bytes, &written));
        check_bytes(bytes, written, vectors[i].bytes,
                    sizeof vectors[i].bytes);
    }

    written = 0;
    CHECK(p4_hosted_wifi_set_mode_request(1u, 1, bytes, sizeof bytes,
                                          &written));
    check_bytes(bytes, written, set_mode, sizeof set_mode);
}

static void test_request_refusal_and_aliasing(void)
{
    uint8_t bytes[32], before[sizeof bytes];
    size_t written;

    memset(bytes, 0x4b, sizeof bytes);
    memcpy(before, bytes, sizeof before);
    written = 0x7654u;

    CHECK(!p4_hosted_wifi_simple_request(278u, 1u, bytes, sizeof bytes,
                                         &written));
    CHECK(memcmp(bytes, before, sizeof bytes) == 0 && written == 0x7654u);
    CHECK(!p4_hosted_wifi_simple_request(282u, 1u, bytes, sizeof bytes,
                                         &written));
    CHECK(!p4_hosted_wifi_simple_request(279u, 0u, bytes, sizeof bytes,
                                         &written));
    CHECK(!p4_hosted_wifi_simple_request(279u, 1u, bytes, 8u, &written));
    CHECK(memcmp(bytes, before, sizeof bytes) == 0 && written == 0x7654u);
    CHECK(!p4_hosted_wifi_simple_request(279u, 1u, bytes, SIZE_MAX,
                                         &written));
    CHECK(memcmp(bytes, before, sizeof bytes) == 0 && written == 0x7654u);
    CHECK(!p4_hosted_wifi_simple_request(279u, 1u, NULL, sizeof bytes,
                                         &written));
    CHECK(!p4_hosted_wifi_simple_request(279u, 1u, bytes, sizeof bytes,
                                         NULL));
    CHECK(!p4_hosted_wifi_set_mode_request(1u, 0, bytes, sizeof bytes,
                                           &written));
    CHECK(!p4_hosted_wifi_set_mode_request(1u, 2, bytes, sizeof bytes,
                                           &written));
    CHECK(!p4_hosted_wifi_set_mode_request(0u, 1, bytes, sizeof bytes,
                                           &written));
    CHECK(!p4_hosted_wifi_set_mode_request(1u, 1, bytes, 11u, &written));
    CHECK(memcmp(bytes, before, sizeof bytes) == 0 && written == 0x7654u);

    {
        union {
            size_t alignment;
            uint8_t bytes[32];
        } overlap;
        uint8_t snapshot[sizeof overlap.bytes];

        memset(overlap.bytes, 0x91, sizeof overlap.bytes);
        written = *(size_t *)(void *)overlap.bytes;
        memcpy(snapshot, overlap.bytes, sizeof snapshot);
        CHECK(!p4_hosted_wifi_simple_request(279u, 1u, overlap.bytes,
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
    static const uint8_t int32_minimum[] = {
        0x08, 0x80, 0x80, 0x80, 0x80, 0xf8,
        0xff, 0xff, 0xff, 0xff, 0x01
    };
    static const uint8_t with_unknown_fields[] = {
        0x18, 0x7f,
        0x21, 1, 2, 3, 4, 5, 6, 7, 8,
        0x2a, 3, 0xaa, 0xbb, 0xcc,
        0x35, 0x11, 0x22, 0x33, 0x44,
        0x08, 0x00
    };
    struct p4_hosted_rpc rpc;
    int32_t status;

    set_response(&rpc, 278u, 9u, NULL, 0u);
    status = -1;
    CHECK(p4_hosted_wifi_status_response(&rpc, 278u, 9u, &status));
    CHECK(status == 0);

    set_response(&rpc, 278u, 9u, explicit_zero, sizeof explicit_zero);
    status = -1;
    CHECK(p4_hosted_wifi_status_response(&rpc, 278u, 9u, &status));
    CHECK(status == 0);

    set_response(&rpc, 278u, 9u, status_error, sizeof status_error);
    status = 0;
    CHECK(p4_hosted_wifi_status_response(&rpc, 278u, 9u, &status));
    CHECK(status == 0x3001);

    set_response(&rpc, 278u, 9u, negative_one, sizeof negative_one);
    status = 0;
    CHECK(p4_hosted_wifi_status_response(&rpc, 278u, 9u, &status));
    CHECK(status == -1);

    set_response(&rpc, 278u, 9u, int32_minimum, sizeof int32_minimum);
    status = 0;
    CHECK(p4_hosted_wifi_status_response(&rpc, 278u, 9u, &status));
    CHECK(status == INT32_MIN);

    set_response(&rpc, 278u, 9u, with_unknown_fields,
                 sizeof with_unknown_fields);
    status = -1;
    CHECK(p4_hosted_wifi_status_response(&rpc, 278u, 9u, &status));
    CHECK(status == 0);

    {
        static const uint32_t request_ids[] = { 279u, 280u, 281u, 260u };
        size_t i;

        for (i = 0; i < sizeof request_ids / sizeof request_ids[0]; ++i) {
            set_response(&rpc, request_ids[i], 9u, NULL, 0u);
            status = -1;
            CHECK(p4_hosted_wifi_status_response(&rpc, request_ids[i],
                                                  9u, &status));
            CHECK(status == 0);
        }
    }
}

static void test_status_limits_and_malformed_bodies(void)
{
    static const uint8_t zero_key[] = { 0x00 };
    static const uint8_t truncated_key[] = { 0x80 };
    static const uint8_t key_overflow[] = {
        0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x02
    };
    static const uint8_t field_number_overflow[] = {
        0x80, 0x80, 0x80, 0x80, 0x08
    };
    static const uint8_t wrong_status_wire[] = { 0x0a, 0x00 };
    static const uint8_t malformed_status[] = { 0x08, 0x80 };
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
    static const uint8_t duplicate_status[] = { 0x08, 0, 0x08, 0 };
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
    static const uint8_t trailing_bad_key[] = { 0x08, 0, 0x80 };
    static const uint8_t duplicate_known[] = { 0x08, 0, 0x08, 1 };
    uint8_t maximum[P4_HOSTED_WIFI_MAX_BODY_SIZE];
    struct p4_hosted_rpc rpc;
    size_t i;

#define REJECT(body_) do { \
    set_response(&rpc, 278u, 9u, (body_), sizeof (body_)); \
    expect_status_refusal(&rpc, 278u, 9u); \
} while (0)
    REJECT(zero_key);
    REJECT(truncated_key);
    REJECT(key_overflow);
    REJECT(field_number_overflow);
    REJECT(wrong_status_wire);
    REJECT(malformed_status);
    REJECT(status_varint_overflow);
    REJECT(invalid_int32_positive);
    REJECT(non_sign_extended_int32);
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
    REJECT(duplicate_known);
#undef REJECT

    for (i = 0; i < sizeof maximum; i += 2u) {
        maximum[i] = 0x18u;     /* Unknown field 3, varint. */
        maximum[i + 1u] = 0u;
    }
    set_response(&rpc, 278u, 9u, maximum, sizeof maximum);
    {
        int32_t status = -1;
        CHECK(p4_hosted_wifi_status_response(&rpc, 278u, 9u, &status));
        CHECK(status == 0);
    }

    set_response(&rpc, 278u, 9u, maximum,
                 P4_HOSTED_WIFI_MAX_BODY_SIZE + 1u);
    expect_status_refusal(&rpc, 278u, 9u);

    set_response(&rpc, 278u, 9u, NULL, 1u);
    expect_status_refusal(&rpc, 278u, 9u);
}

static void test_status_metadata_and_aliasing(void)
{
    static const uint8_t success[] = { 0x08, 0 };
    struct p4_hosted_rpc rpc;
    int32_t status;

    set_response(&rpc, 278u, 9u, success, sizeof success);
    expect_status_refusal(&rpc, 278u, 0u);
    expect_status_refusal(&rpc, 278u, 8u);
    expect_status_refusal(&rpc, 279u, 9u);
    expect_status_refusal(&rpc, 280u, 9u);
    expect_status_refusal(&rpc, 281u, 9u);
    expect_status_refusal(&rpc, 260u, 9u);
    expect_status_refusal(&rpc, 282u, 9u);

    rpc.msg_type = P4_HOSTED_RPC_TYPE_REQUEST;
    expect_status_refusal(&rpc, 278u, 9u);
    rpc.msg_type = P4_HOSTED_RPC_TYPE_EVENT;
    expect_status_refusal(&rpc, 278u, 9u);
    rpc.msg_type = P4_HOSTED_RPC_TYPE_RESPONSE;
    rpc.msg_id = 533u;
    expect_status_refusal(&rpc, 278u, 9u);
    rpc.msg_id = 535u;
    expect_status_refusal(&rpc, 278u, 9u);
    rpc.msg_id = 534u;
    rpc.uid = 0u;
    expect_status_refusal(&rpc, 278u, 9u);
    rpc.uid = 9u;

    status = -1;
    CHECK(!p4_hosted_wifi_status_response(&rpc, 278u, 9u, NULL));
    CHECK(status == -1);
    CHECK(!p4_hosted_wifi_status_response(NULL, 278u, 9u, &status));

    CHECK(!p4_hosted_wifi_status_response(&rpc, 278u, 9u,
                                          (int32_t *)(void *)&rpc));

    {
        const struct p4_hosted_rpc *overflowing =
            (const struct p4_hosted_rpc *)(uintptr_t)(UINTPTR_MAX - 1u);
        status = INT32_C(0x5a5a1234);
        CHECK(!p4_hosted_wifi_status_response(overflowing, 278u, 9u,
                                               &status));
        CHECK(status == INT32_C(0x5a5a1234));
    }

    rpc.payload = (const uint8_t *)(const void *)&status;
    rpc.payload_size = sizeof status;
    expect_status_refusal(&rpc, 278u, 9u);
}

int main(void)
{
    test_init_vectors();
    test_init_refusal_and_aliasing();
    test_simple_and_set_mode_requests();
    test_request_refusal_and_aliasing();
    test_status_responses();
    test_status_limits_and_malformed_bodies();
    test_status_metadata_and_aliasing();

    printf("hosted Wi-Fi codec: %lu checks passed\n", checks);
    return 0;
}
