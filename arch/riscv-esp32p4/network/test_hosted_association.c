/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "hosted_association.h"

static unsigned long checks;

#define CHECK(expression) do { \
    ++checks; \
    if (!(expression)) { \
        fprintf(stderr, "line %u: %s\n", (unsigned)__LINE__, #expression); \
        exit(1); \
    } \
} while (0)

static const uint8_t fixture_ssid[] = {
    'f', 'i', 'x', 't', 'u', 'r', 'e'
};
static const uint8_t fixture_password[] = {
    0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78
};
static const uint8_t fixture_bssid[6] = {
    0x02, 0x11, 0x22, 0x33, 0x44, 0x55
};
static const uint8_t fixture_sae_id[] = { 'i', 'd' };

static void check_bytes(const uint8_t *actual, size_t actual_size,
                        const uint8_t *expected, size_t expected_size)
{
    CHECK(actual_size == expected_size);
    CHECK(memcmp(actual, expected, expected_size) == 0);
}

static int contains_bytes(const uint8_t *bytes, size_t size,
                          const uint8_t *sequence, size_t sequence_size)
{
    size_t offset;

    if (sequence_size > size)
        return 0;
    for (offset = 0u; offset <= size - sequence_size; ++offset) {
        if (memcmp(bytes + offset, sequence, sequence_size) == 0)
            return 1;
    }
    return 0;
}

static void fill_config(struct p4_hosted_sta_config *config)
{
    memset(config, 0, sizeof *config);
    config->ssid = fixture_ssid;
    config->ssid_length = sizeof fixture_ssid;
    config->password = fixture_password;
    config->password_length = sizeof fixture_password;
    config->bssid_set = 1u;
    config->bssid = fixture_bssid;
    config->bssid_length = sizeof fixture_bssid;
    config->threshold_rssi = -63;
    config->threshold_authmode = 3;
    config->pmf_capable = 1u;
    config->sae_h2e_identifier = fixture_sae_id;
    config->sae_h2e_identifier_length = sizeof fixture_sae_id;
}

static void set_rpc(struct p4_hosted_rpc *rpc, uint32_t type, uint32_t id,
                    uint32_t uid, const uint8_t *body, size_t body_size)
{
    rpc->msg_type = type;
    rpc->msg_id = id;
    rpc->uid = uid;
    rpc->payload = body;
    rpc->payload_size = body_size;
}

static void expect_status_refusal(const struct p4_hosted_rpc *rpc,
                                  uint32_t request_id, uint32_t uid)
{
    int32_t status = INT32_C(0x5a5a1234);
    int32_t before = status;

    CHECK(!p4_hosted_association_status_response(rpc, request_id, uid,
                                                  &status));
    CHECK(status == before);
}

static void expect_connected_refusal(const struct p4_hosted_rpc *rpc)
{
    struct p4_hosted_sta_connected_event result;
    struct p4_hosted_sta_connected_event before;

    memset(&result, 0xa5, sizeof result);
    memcpy(&before, &result, sizeof before);
    CHECK(!p4_hosted_association_connected_event(rpc, &result));
    CHECK(memcmp(&result, &before, sizeof result) == 0);
}

static void expect_disconnected_refusal(const struct p4_hosted_rpc *rpc)
{
    struct p4_hosted_sta_disconnected_event result;
    struct p4_hosted_sta_disconnected_event before;

    memset(&result, 0x5a, sizeof result);
    memcpy(&before, &result, sizeof before);
    CHECK(!p4_hosted_association_disconnected_event(rpc, &result));
    CHECK(memcmp(&result, &before, sizeof result) == 0);
}

static void test_literal_requests(void)
{
    static const uint8_t empty_set_config[] = {
        0x08, 0x01, 0x10, 0x9c, 0x02, 0x18, 0x01,
        0xe2, 0x11, 0x08, 0x12, 0x06, 0x12, 0x04,
        0x4a, 0x00, 0x52, 0x00
    };
    static const uint8_t empty_connect[] = {
        0x08, 0x01, 0x10, 0x9a, 0x02, 0x18, 0x01,
        0xd2, 0x11, 0x00
    };
    static const uint8_t empty_disconnect[] = {
        0x08, 0x01, 0x10, 0x9b, 0x02, 0x18, 0x01,
        0xda, 0x11, 0x00
    };
    struct p4_hosted_sta_config config;
    uint8_t bytes[P4_HOSTED_ASSOCIATION_MAX_REQUEST_SIZE];
    size_t written;

    memset(&config, 0, sizeof config);
    memset(bytes, 0xa5, sizeof bytes);
    written = 0u;
    CHECK(p4_hosted_association_set_config_request(1u, &config, bytes,
                                                    sizeof bytes, &written));
    check_bytes(bytes, written, empty_set_config, sizeof empty_set_config);

    memset(bytes, 0xa5, sizeof bytes);
    written = 0u;
    CHECK(p4_hosted_association_connect_request(1u, bytes, sizeof bytes,
                                                &written));
    check_bytes(bytes, written, empty_connect, sizeof empty_connect);

    memset(bytes, 0xa5, sizeof bytes);
    written = 0u;
    CHECK(p4_hosted_association_disconnect_request(1u, bytes, sizeof bytes,
                                                   &written));
    check_bytes(bytes, written, empty_disconnect, sizeof empty_disconnect);
}

static void test_full_config_and_bounds(void)
{
    struct p4_hosted_sta_config config;
    uint8_t ssid[P4_HOSTED_ASSOCIATION_SSID_SIZE];
    uint8_t password[P4_HOSTED_ASSOCIATION_PASSWORD_SIZE];
    uint8_t bssid[P4_HOSTED_ASSOCIATION_BSSID_SIZE];
    uint8_t sae_id[P4_HOSTED_ASSOCIATION_SAE_ID_SIZE];
    uint8_t bytes[P4_HOSTED_ASSOCIATION_MAX_REQUEST_SIZE];
    uint8_t before[sizeof bytes];
    struct p4_hosted_rpc rpc;
    size_t written;
    size_t max_case_size;
    size_t index;

    memset(&config, 0, sizeof config);
    for (index = 0u; index < sizeof ssid; ++index)
        ssid[index] = (uint8_t)index;
    for (index = 0u; index < sizeof password; ++index)
        password[index] = (uint8_t)(0x80u + (uint8_t)index);
    for (index = 0u; index < sizeof bssid; ++index)
        bssid[index] = (uint8_t)(0xd0u + (uint8_t)index);
    for (index = 0u; index < sizeof sae_id; ++index)
        sae_id[index] = (uint8_t)(0x40u + (uint8_t)index);

    config.ssid = ssid;
    config.ssid_length = sizeof ssid;
    config.password = password;
    config.password_length = sizeof password;
    config.scan_method = 1;
    config.bssid_set = 1u;
    config.bssid = bssid;
    config.bssid_length = sizeof bssid;
    config.channel = UINT32_MAX;
    config.listen_interval = UINT32_MAX;
    config.sort_method = 1;
    config.threshold_rssi = -127;
    config.threshold_authmode = 3;
    config.pmf_capable = 1u;
    config.pmf_required = 1u;
    config.bitmask = UINT32_MAX;
    config.sae_pwe_h2e = 1;
    config.failure_retry_cnt = UINT32_MAX;
    config.he_bitmask = UINT32_MAX;
    config.sae_h2e_identifier = sae_id;
    config.sae_h2e_identifier_length = sizeof sae_id;

    memset(bytes, 0xa5, sizeof bytes);
    written = 0u;
    CHECK(p4_hosted_association_set_config_request(UINT32_MAX, &config,
                                                    bytes, sizeof bytes,
                                                    &written));
    CHECK(written <= P4_HOSTED_ASSOCIATION_MAX_REQUEST_SIZE);
    CHECK(p4_hosted_rpc_decode(bytes, written, &rpc));
    CHECK(rpc.msg_type == P4_HOSTED_RPC_TYPE_REQUEST);
    CHECK(rpc.msg_id == 284u);
    CHECK(rpc.uid == UINT32_MAX);
    CHECK(rpc.payload_size != 0u);
    CHECK(contains_bytes(bytes, written, ssid, sizeof ssid));
    CHECK(contains_bytes(bytes, written, password, sizeof password));
    CHECK(contains_bytes(bytes, written, bssid, sizeof bssid));
    CHECK(contains_bytes(bytes, written, sae_id, sizeof sae_id));

    memset(&config, 0, sizeof config);
    config.ssid = ssid;
    config.ssid_length = sizeof ssid;
    config.password = password;
    config.password_length = sizeof password;
    config.bssid_set = 1u;
    config.bssid = bssid;
    config.bssid_length = sizeof bssid;
    config.threshold_rssi = -1;
    config.scan_method = -1;
    config.sort_method = -1;
    config.threshold_authmode = -1;
    config.sae_pwe_h2e = -1;
    config.sae_h2e_identifier = sae_id;
    config.sae_h2e_identifier_length = sizeof sae_id;
    written = 0u;
    CHECK(p4_hosted_association_set_config_request(UINT32_MAX, &config,
                                                    bytes, sizeof bytes,
                                                    &written));
    CHECK(written <= P4_HOSTED_ASSOCIATION_MAX_REQUEST_SIZE);
    max_case_size = written;
    memset(bytes, 0x6d, sizeof bytes);
    memcpy(before, bytes, sizeof bytes);
    written = 0x1234u;
    CHECK(!p4_hosted_association_set_config_request(UINT32_MAX, &config,
                                                     bytes, max_case_size - 1u,
                                                     &written));
    CHECK(memcmp(bytes, before, sizeof bytes) == 0);
    CHECK(written == 0x1234u);

    config.scan_method = INT32_MIN;
    config.sort_method = INT32_MIN;
    config.threshold_rssi = INT32_MIN;
    config.threshold_authmode = INT32_MIN;
    config.sae_pwe_h2e = INT32_MIN;
    config.channel = UINT32_MAX;
    config.listen_interval = UINT32_MAX;
    config.bitmask = UINT32_MAX;
    config.failure_retry_cnt = UINT32_MAX;
    config.he_bitmask = UINT32_MAX;
    config.pmf_capable = 1u;
    config.pmf_required = 1u;
    memset(bytes, 0x3c, sizeof bytes);
    memcpy(before, bytes, sizeof bytes);
    written = 0x7654u;
    CHECK(!p4_hosted_association_set_config_request(UINT32_MAX, &config,
                                                     bytes, sizeof bytes,
                                                     &written));
    CHECK(memcmp(bytes, before, sizeof bytes) == 0);
    CHECK(written == 0x7654u);
}

static void test_request_refusals_and_aliases(void)
{
    struct p4_hosted_sta_config config;
    struct p4_hosted_sta_config before_config;
    uint8_t bytes[256], before[sizeof bytes];
    uint8_t too_long[65];
    size_t written;
    uintptr_t invalid_address = UINTPTR_MAX - 1u;

    fill_config(&config);
    memset(bytes, 0x6d, sizeof bytes);
    memcpy(before, bytes, sizeof bytes);
    written = 0x1234u;
    CHECK(!p4_hosted_association_set_config_request(0u, &config, bytes,
                                                     sizeof bytes, &written));
    CHECK(memcmp(bytes, before, sizeof bytes) == 0);
    CHECK(written == 0x1234u);
    CHECK(!p4_hosted_association_set_config_request(1u, NULL, bytes,
                                                     sizeof bytes, &written));
    CHECK(!p4_hosted_association_set_config_request(
        1u, (const struct p4_hosted_sta_config *)invalid_address,
        bytes, sizeof bytes, &written));

    CHECK(!p4_hosted_association_set_config_request(
        1u, &config, (uint8_t *)invalid_address, 16u, &written));
    CHECK(!p4_hosted_association_set_config_request(1u, &config, bytes,
                                                    SIZE_MAX, &written));
    CHECK(!p4_hosted_association_set_config_request(
        1u, &config, bytes, sizeof bytes, (size_t *)invalid_address));
    CHECK(!p4_hosted_association_set_config_request(1u, &config, bytes,
                                                    sizeof bytes, NULL));
    CHECK(!p4_hosted_association_set_config_request(1u, &config, NULL,
                                                    sizeof bytes, &written));

    CHECK(!p4_hosted_association_connect_request(0u, bytes, sizeof bytes,
                                                 &written));
    CHECK(!p4_hosted_association_disconnect_request(1u, bytes, 9u,
                                                    &written));

    config.ssid = NULL;
    config.ssid_length = 1u;
    CHECK(!p4_hosted_association_set_config_request(1u, &config, bytes,
                                                    sizeof bytes, &written));
    fill_config(&config);
    config.ssid = (const uint8_t *)invalid_address;
    config.ssid_length = 4u;
    CHECK(!p4_hosted_association_set_config_request(1u, &config, bytes,
                                                    sizeof bytes, &written));
    fill_config(&config);
    config.password = too_long;
    config.password_length = sizeof too_long;
    CHECK(!p4_hosted_association_set_config_request(1u, &config, bytes,
                                                    sizeof bytes, &written));
    fill_config(&config);
    config.sae_h2e_identifier = NULL;
    config.sae_h2e_identifier_length = 1u;
    CHECK(!p4_hosted_association_set_config_request(1u, &config, bytes,
                                                    sizeof bytes, &written));
    fill_config(&config);
    config.bssid_length = 5u;
    CHECK(!p4_hosted_association_set_config_request(1u, &config, bytes,
                                                    sizeof bytes, &written));
    fill_config(&config);
    config.bssid_set = 2u;
    CHECK(!p4_hosted_association_set_config_request(1u, &config, bytes,
                                                    sizeof bytes, &written));
    fill_config(&config);
    config.pmf_capable = 2u;
    CHECK(!p4_hosted_association_set_config_request(1u, &config, bytes,
                                                    sizeof bytes, &written));
    fill_config(&config);
    config.pmf_required = 2u;
    CHECK(!p4_hosted_association_set_config_request(1u, &config, bytes,
                                                    sizeof bytes, &written));
    fill_config(&config);
    config.bssid_set = 0u;
    CHECK(!p4_hosted_association_set_config_request(1u, &config, bytes,
                                                    sizeof bytes, &written));

    fill_config(&config);
    before_config = config;
    CHECK(!p4_hosted_association_set_config_request(
        1u, &config, (uint8_t *)&config, sizeof config, &written));
    CHECK(memcmp(&config, &before_config, sizeof config) == 0);
    CHECK(!p4_hosted_association_set_config_request(
        1u, &config, bytes, sizeof bytes, (size_t *)&config));

    fill_config(&config);
    CHECK(!p4_hosted_association_set_config_request(
        1u, &config, (uint8_t *)fixture_ssid,
        sizeof fixture_ssid, &written));

    {
        union {
            size_t align;
            uint8_t storage[64];
        } aliased;
        struct p4_hosted_sta_config alias_config;
        size_t *alias_written = (size_t *)(void *)aliased.storage;

        fill_config(&alias_config);
        alias_config.ssid = aliased.storage;
        alias_config.ssid_length = 8u;
        memset(aliased.storage, 0x33, sizeof aliased.storage);
        *alias_written = 0x7654u;
        CHECK(!p4_hosted_association_set_config_request(
            1u, &alias_config, bytes, sizeof bytes, alias_written));
        CHECK(*alias_written == 0x7654u);
    }
}

static void test_status_responses(void)
{
    static const uint8_t success_body[] = { 0x08, 0x00 };
    static const uint8_t negative_body[] = {
        0x08, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0x01
    };
    static const uint8_t unknowns_body[] = {
        0x11, 1, 2, 3, 4, 5, 6, 7, 8,
        0x1a, 0x02, 0xaa, 0xbb,
        0x25, 1, 2, 3, 4,
        0x08, 0x07
    };
    static const uint8_t duplicate_status[] = { 0x08, 0x01, 0x08, 0x02 };
    static const uint8_t wrong_wire[] = { 0x0a, 0x00 };
    static const uint8_t bad_int32[] = {
        0x08, 0x80, 0x80, 0x80, 0x80, 0x10
    };
    static const uint8_t overlong[] = { 0x08, 0x80, 0x00 };
    struct p4_hosted_rpc rpc;
    int32_t status = INT32_C(0x11112222);

    set_rpc(&rpc, P4_HOSTED_RPC_TYPE_RESPONSE, 540u, 91u,
            NULL, 0u);
    CHECK(p4_hosted_association_status_response(&rpc, 284u, 91u, &status));
    CHECK(status == 0);

    status = 5;
    set_rpc(&rpc, P4_HOSTED_RPC_TYPE_RESPONSE, 538u, 7u,
            success_body, sizeof success_body);
    CHECK(p4_hosted_association_status_response(&rpc, 282u, 7u, &status));
    CHECK(status == 0);

    status = 0;
    set_rpc(&rpc, P4_HOSTED_RPC_TYPE_RESPONSE, 539u, 8u,
            negative_body, sizeof negative_body);
    CHECK(p4_hosted_association_status_response(&rpc, 283u, 8u, &status));
    CHECK(status == -1);

    status = 0;
    set_rpc(&rpc, P4_HOSTED_RPC_TYPE_RESPONSE, 540u, 9u,
            unknowns_body, sizeof unknowns_body);
    CHECK(p4_hosted_association_status_response(&rpc, 284u, 9u, &status));
    CHECK(status == 7);

    set_rpc(&rpc, P4_HOSTED_RPC_TYPE_RESPONSE, 540u, 91u,
            duplicate_status, sizeof duplicate_status);
    expect_status_refusal(&rpc, 284u, 91u);
    set_rpc(&rpc, P4_HOSTED_RPC_TYPE_RESPONSE, 540u, 91u,
            wrong_wire, sizeof wrong_wire);
    expect_status_refusal(&rpc, 284u, 91u);
    set_rpc(&rpc, P4_HOSTED_RPC_TYPE_RESPONSE, 540u, 91u,
            bad_int32, sizeof bad_int32);
    expect_status_refusal(&rpc, 284u, 91u);
    set_rpc(&rpc, P4_HOSTED_RPC_TYPE_RESPONSE, 540u, 91u,
            overlong, sizeof overlong);
    expect_status_refusal(&rpc, 284u, 91u);

    set_rpc(&rpc, P4_HOSTED_RPC_TYPE_RESPONSE, 539u, 8u,
            NULL, 0u);
    expect_status_refusal(&rpc, 282u, 8u);
    expect_status_refusal(&rpc, 283u, 7u);
    set_rpc(&rpc, P4_HOSTED_RPC_TYPE_REQUEST, 539u, 8u,
            NULL, 0u);
    expect_status_refusal(&rpc, 283u, 8u);
    set_rpc(&rpc, P4_HOSTED_RPC_TYPE_RESPONSE, 539u, 0u,
            NULL, 0u);
    expect_status_refusal(&rpc, 283u, 0u);
    expect_status_refusal(&rpc, 286u, 8u);

    {
        uint8_t large[P4_HOSTED_ASSOCIATION_MAX_BODY_SIZE + 1u] = { 0 };
        set_rpc(&rpc, P4_HOSTED_RPC_TYPE_RESPONSE, 538u, 7u,
                large, sizeof large);
        expect_status_refusal(&rpc, 282u, 7u);
    }
    {
        struct p4_hosted_rpc *alias = &rpc;
        CHECK(!p4_hosted_association_status_response(
            &rpc, 283u, 8u, (int32_t *)(void *)alias));
    }
}

static const uint8_t connected_wire[] = {
    0x08, 0x03, 0x10, 0x87, 0x06, 0xba, 0x30, 0x34,
    0x12, 0x32,
    0x0a, 0x20,
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
    0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f,
    0x10, 0x04, 0x1a, 0x06, 0x02, 0x11, 0x22, 0x33,
    0x44, 0x55, 0x20, 0x0b, 0x28, 0x03, 0x30, 0x07
};

static const uint8_t disconnected_wire[] = {
    0x08, 0x03, 0x10, 0x88, 0x06, 0xc2, 0x30, 0x3b,
    0x12, 0x39,
    0x0a, 0x20,
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
    0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f,
    0x10, 0x04, 0x1a, 0x06, 0x02, 0x11, 0x22, 0x33,
    0x44, 0x55, 0x20, 0x08, 0x28, 0xd6, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x01
};

static void test_owned_events(void)
{
    uint8_t connected[P4_HOSTED_RPC_MAX_SIZE];
    uint8_t disconnected[P4_HOSTED_RPC_MAX_SIZE];
    struct p4_hosted_rpc rpc;
    struct p4_hosted_sta_connected_event connected_result;
    struct p4_hosted_sta_disconnected_event disconnected_result;
    size_t index;

    memcpy(connected, connected_wire, sizeof connected_wire);
    CHECK(p4_hosted_rpc_decode(connected, sizeof connected_wire, &rpc));
    CHECK(p4_hosted_association_connected_event(&rpc, &connected_result));
    CHECK(connected_result.status == 0);
    CHECK(connected_result.ssid_length == 4u);
    CHECK(connected_result.channel == 11u);
    CHECK(connected_result.authmode == 3);
    CHECK(connected_result.aid == 7);
    for (index = 0u; index < sizeof connected_result.ssid; ++index)
        CHECK(connected_result.ssid[index] == (uint8_t)index);
    CHECK(memcmp(connected_result.bssid, fixture_bssid,
                 sizeof fixture_bssid) == 0);
    memset(connected, 0, sizeof connected_wire);
    for (index = 0u; index < sizeof connected_result.ssid; ++index)
        CHECK(connected_result.ssid[index] == (uint8_t)index);
    CHECK(connected_result.bssid[5] == 0x55u);

    memcpy(disconnected, disconnected_wire, sizeof disconnected_wire);
    CHECK(p4_hosted_rpc_decode(disconnected, sizeof disconnected_wire, &rpc));
    CHECK(p4_hosted_association_disconnected_event(&rpc,
                                                   &disconnected_result));
    CHECK(disconnected_result.status == 0);
    CHECK(disconnected_result.ssid_length == 4u);
    CHECK(disconnected_result.reason == 8u);
    CHECK(disconnected_result.rssi == -42);
    for (index = 0u; index < sizeof disconnected_result.ssid; ++index)
        CHECK(disconnected_result.ssid[index] == (uint8_t)index);
    CHECK(memcmp(disconnected_result.bssid, fixture_bssid,
                 sizeof fixture_bssid) == 0);
    memset(disconnected, 0, sizeof disconnected_wire);
    CHECK(disconnected_result.ssid[31] == 31u);
    CHECK(disconnected_result.bssid[0] == 0x02u);
}

static void test_event_refusals(void)
{
    static const uint8_t error_without_data[] = { 0x08, 0x01 };
    static const uint8_t error_with_valid_connected_data[] = {
        0x08, 0x01,
        0x12, 0x2c,
        0x0a, 0x20,
        0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,
        16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,
        0x10, 0x04,
        0x1a, 0x06, 2,17,34,51,68,85
    };
    static const uint8_t duplicate_status[] = {
        0x08, 0x00, 0x08, 0x00, 0x12, 0x00
    };
    static const uint8_t duplicate_data[] = {
        0x12, 0x00, 0x12, 0x00
    };
    static const uint8_t bad_status_wire[] = { 0x0a, 0x00, 0x12, 0x00 };
    static const uint8_t bad_ssid_length[] = {
        0x0a, 0x1f,
        0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,
        16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,
        0x1a, 0x06, 2, 17, 34, 51, 68, 85
    };
    static const uint8_t bad_ssid_meaningful_length[] = {
        0x0a, 0x20,
        0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,
        16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,
        0x10, 0x21, 0x1a, 0x06, 2, 17, 34, 51, 68, 85
    };
    static const uint8_t unknown_group[] = { 0x13, 0x14 };
    struct p4_hosted_rpc rpc;
    struct p4_hosted_sta_connected_event connected;

    set_rpc(&rpc, P4_HOSTED_RPC_TYPE_EVENT, 775u, 0u,
            error_without_data, sizeof error_without_data);
    memset(&connected, 0xa5, sizeof connected);
    CHECK(p4_hosted_association_connected_event(&rpc, &connected));
    CHECK(connected.status == 1);
    CHECK(connected.ssid_length == 0u);
    CHECK(connected.ssid[0] == 0u && connected.bssid[0] == 0u);
    set_rpc(&rpc, P4_HOSTED_RPC_TYPE_EVENT, 775u, 0u,
            error_with_valid_connected_data,
            sizeof error_with_valid_connected_data);
    memset(&connected, 0xa5, sizeof connected);
    CHECK(p4_hosted_association_connected_event(&rpc, &connected));
    CHECK(connected.status == 1);
    CHECK(connected.ssid_length == 0u && connected.channel == 0u);
    CHECK(connected.authmode == 0 && connected.aid == 0);
    CHECK(connected.ssid[0] == 0u && connected.ssid[31] == 0u);
    CHECK(connected.bssid[0] == 0u && connected.bssid[5] == 0u);

    set_rpc(&rpc, P4_HOSTED_RPC_TYPE_EVENT, 775u, 0u,
            NULL, 0u);
    expect_connected_refusal(&rpc);
    set_rpc(&rpc, P4_HOSTED_RPC_TYPE_EVENT, 775u, 0u,
            duplicate_status, sizeof duplicate_status);
    expect_connected_refusal(&rpc);
    set_rpc(&rpc, P4_HOSTED_RPC_TYPE_EVENT, 775u, 0u,
            duplicate_data, sizeof duplicate_data);
    expect_connected_refusal(&rpc);
    set_rpc(&rpc, P4_HOSTED_RPC_TYPE_EVENT, 775u, 0u,
            bad_status_wire, sizeof bad_status_wire);
    expect_connected_refusal(&rpc);
    set_rpc(&rpc, P4_HOSTED_RPC_TYPE_EVENT, 775u, 0u,
            unknown_group, sizeof unknown_group);
    expect_connected_refusal(&rpc);
    set_rpc(&rpc, P4_HOSTED_RPC_TYPE_EVENT, 776u, 0u,
            error_without_data, sizeof error_without_data);
    expect_connected_refusal(&rpc);

    set_rpc(&rpc, P4_HOSTED_RPC_TYPE_EVENT, 775u, 0u,
            bad_ssid_length, sizeof bad_ssid_length);
    {
        uint8_t wrapper[sizeof bad_ssid_length + 2u];
        wrapper[0] = 0x12;
        wrapper[1] = (uint8_t)sizeof bad_ssid_length;
        memcpy(wrapper + 2u, bad_ssid_length, sizeof bad_ssid_length);
        set_rpc(&rpc, P4_HOSTED_RPC_TYPE_EVENT, 775u, 0u,
                wrapper, sizeof wrapper);
        expect_connected_refusal(&rpc);
    }
    {
        uint8_t wrapper[sizeof bad_ssid_length + 4u];
        wrapper[0] = 0x08;
        wrapper[1] = 0x01;
        wrapper[2] = 0x12;
        wrapper[3] = (uint8_t)sizeof bad_ssid_length;
        memcpy(wrapper + 4u, bad_ssid_length, sizeof bad_ssid_length);
        set_rpc(&rpc, P4_HOSTED_RPC_TYPE_EVENT, 775u, 0u,
                wrapper, sizeof wrapper);
        expect_connected_refusal(&rpc);
    }
    {
        uint8_t wrapper[sizeof bad_ssid_meaningful_length + 2u];
        wrapper[0] = 0x12;
        wrapper[1] = (uint8_t)sizeof bad_ssid_meaningful_length;
        memcpy(wrapper + 2u, bad_ssid_meaningful_length,
               sizeof bad_ssid_meaningful_length);
        set_rpc(&rpc, P4_HOSTED_RPC_TYPE_EVENT, 775u, 0u,
                wrapper, sizeof wrapper);
        expect_connected_refusal(&rpc);
    }
    {
        uint8_t truncated[sizeof connected_wire - 1u];
        memcpy(truncated, connected_wire, sizeof truncated);
        set_rpc(&rpc, P4_HOSTED_RPC_TYPE_EVENT, 775u, 0u,
                truncated + 8u, sizeof truncated - 8u);
        expect_connected_refusal(&rpc);
    }

    set_rpc(&rpc, P4_HOSTED_RPC_TYPE_RESPONSE, 775u, 0u,
            error_without_data, sizeof error_without_data);
    expect_connected_refusal(&rpc);
    set_rpc(&rpc, P4_HOSTED_RPC_TYPE_EVENT, 775u, 1u,
            error_without_data, sizeof error_without_data);
    expect_connected_refusal(&rpc);
    set_rpc(&rpc, P4_HOSTED_RPC_TYPE_EVENT, 775u, 0u,
            error_without_data, sizeof error_without_data);
    expect_disconnected_refusal(&rpc);

    set_rpc(&rpc, P4_HOSTED_RPC_TYPE_EVENT, 776u, 0u,
            disconnected_wire + 8u, 0u);
    expect_disconnected_refusal(&rpc);
    set_rpc(&rpc, P4_HOSTED_RPC_TYPE_EVENT, 776u, 0u,
            disconnected_wire, P4_HOSTED_ASSOCIATION_MAX_BODY_SIZE + 1u);
    expect_disconnected_refusal(&rpc);

    {
        struct p4_hosted_rpc alias_rpc;
        memset(&alias_rpc, 0, sizeof alias_rpc);
        CHECK(!p4_hosted_association_connected_event(
            &alias_rpc, (struct p4_hosted_sta_connected_event *)(void *)&alias_rpc));
    }
}

int main(void)
{
    test_literal_requests();
    test_full_config_and_bounds();
    test_request_refusals_and_aliases();
    test_status_responses();
    test_owned_events();
    test_event_refusals();
    printf("hosted association: %lu checks\n", checks);
    return 0;
}
