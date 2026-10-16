/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#include "../network/hosted_link.h"

#include <stdio.h>
#include <string.h>

static unsigned int checks;
static unsigned int failures;

#define CHECK(expression) do {                                               \
    ++checks;                                                                \
    if (!(expression)) {                                                     \
        ++failures;                                                          \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,             \
                #expression);                                                \
    }                                                                        \
} while (0)

struct wire_buffer {
    uint8_t bytes[512];
    size_t size;
    int ok;
};

static void wire_init(struct wire_buffer *buffer)
{
    memset(buffer, 0, sizeof *buffer);
    buffer->ok = 1;
}

static void wire_byte(struct wire_buffer *buffer, uint8_t byte)
{
    if (!buffer->ok || buffer->size == sizeof buffer->bytes) {
        buffer->ok = 0;
        return;
    }
    buffer->bytes[buffer->size++] = byte;
}

static void wire_varint(struct wire_buffer *buffer, uint64_t value)
{
    while (value >= 0x80u) {
        wire_byte(buffer, (uint8_t)((value & 0x7fu) | 0x80u));
        value >>= 7;
    }
    wire_byte(buffer, (uint8_t)value);
}

static void wire_key(struct wire_buffer *buffer, uint32_t field,
                     unsigned int wire_type)
{
    wire_varint(buffer, ((uint64_t)field << 3) | wire_type);
}

static void wire_uint_field(struct wire_buffer *buffer, uint32_t field,
                            uint32_t value)
{
    wire_key(buffer, field, 0u);
    wire_varint(buffer, value);
}

static void wire_int32_field(struct wire_buffer *buffer, uint32_t field,
                             int32_t value)
{
    wire_key(buffer, field, 0u);
    wire_varint(buffer, (uint64_t)(int64_t)value);
}

static void wire_bytes_field(struct wire_buffer *buffer, uint32_t field,
                             const uint8_t *bytes, size_t size)
{
    wire_key(buffer, field, 2u);
    wire_varint(buffer, (uint64_t)size);
    if (size > sizeof buffer->bytes - buffer->size) {
        buffer->ok = 0;
        return;
    }
    if (size != 0u && bytes == NULL) {
        buffer->ok = 0;
        return;
    }
    if (size != 0u) {
        memcpy(buffer->bytes + buffer->size, bytes, size);
        buffer->size += size;
    }
}

static int make_event_wire(uint32_t event_id, int32_t status,
                           const uint8_t ssid[P4_HOSTED_ASSOCIATION_SSID_SIZE],
                           uint32_t ssid_length,
                           const uint8_t bssid[P4_HOSTED_ASSOCIATION_BSSID_SIZE],
                           uint8_t *output, size_t output_capacity,
                           size_t *output_size)
{
    struct wire_buffer data, wrapper, envelope;

    wire_init(&data);
    wire_bytes_field(&data, 1u, ssid, P4_HOSTED_ASSOCIATION_SSID_SIZE);
    wire_uint_field(&data, 2u, ssid_length);
    wire_bytes_field(&data, 3u, bssid, P4_HOSTED_ASSOCIATION_BSSID_SIZE);
    if (event_id == 775u) {
        wire_uint_field(&data, 4u, 11u);
        wire_uint_field(&data, 5u, 3u);
        wire_uint_field(&data, 6u, 7u);
    } else {
        wire_uint_field(&data, 4u, 8u);
        wire_int32_field(&data, 5u, -42);
    }

    wire_init(&wrapper);
    wire_uint_field(&wrapper, 1u, (uint32_t)status);
    wire_bytes_field(&wrapper, 2u, data.bytes, data.size);

    wire_init(&envelope);
    wire_uint_field(&envelope, 1u, P4_HOSTED_RPC_TYPE_EVENT);
    wire_uint_field(&envelope, 2u, event_id);
    wire_bytes_field(&envelope, event_id, wrapper.bytes, wrapper.size);

    if (!data.ok || !wrapper.ok || !envelope.ok ||
        envelope.size > output_capacity || output == NULL ||
        output_size == NULL)
        return 0;
    memcpy(output, envelope.bytes, envelope.size);
    *output_size = envelope.size;
    return 1;
}

static int decode_event(uint32_t event_id, int32_t status,
                        const uint8_t ssid[P4_HOSTED_ASSOCIATION_SSID_SIZE],
                        uint32_t ssid_length,
                        const uint8_t bssid[P4_HOSTED_ASSOCIATION_BSSID_SIZE],
                        uint8_t *wire, size_t wire_capacity,
                        struct p4_hosted_rpc *rpc)
{
    size_t wire_size = 0u;
    return make_event_wire(event_id, status, ssid, ssid_length, bssid,
                           wire, wire_capacity, &wire_size) &&
           p4_hosted_rpc_decode(wire, wire_size, rpc);
}

static int decode_wifi_event(int32_t status, int32_t event_id,
                             uint8_t *output, size_t output_capacity,
                             struct p4_hosted_rpc *rpc)
{
    struct wire_buffer body, envelope;

    wire_init(&body);
    wire_int32_field(&body, 1u, status);
    wire_int32_field(&body, 2u, event_id);
    wire_init(&envelope);
    wire_uint_field(&envelope, 1u, P4_HOSTED_RPC_TYPE_EVENT);
    wire_uint_field(&envelope, 2u, 773u);
    wire_bytes_field(&envelope, 773u, body.bytes, body.size);
    if (!body.ok || !envelope.ok || envelope.size > output_capacity ||
        output == NULL || rpc == NULL)
        return 0;
    memcpy(output, envelope.bytes, envelope.size);
    return p4_hosted_rpc_decode(output, envelope.size, rpc);
}

static void fill_pattern(uint8_t *bytes, size_t size, uint8_t seed)
{
    size_t i;
    for (i = 0u; i < size; ++i)
        bytes[i] = (uint8_t)(seed + (uint8_t)(i * 29u));
}

static int all_zero(const uint8_t *bytes, size_t size)
{
    size_t i;
    for (i = 0u; i < size; ++i) {
        if (bytes[i] != 0u)
            return 0;
    }
    return 1;
}

static void expect_refusal_unchanged(
    struct p4_hosted_link_observation *link, uint32_t generation,
    const struct p4_hosted_rpc *rpc)
{
    struct p4_hosted_link_observation before;
    memcpy(&before, link, sizeof before);
    CHECK(!p4_hosted_link_observe(link, generation, rpc));
    CHECK(memcmp(&before, link, sizeof before) == 0);
}

static int observe_from_local_wire(
    struct p4_hosted_link_observation *link, uint32_t generation,
    const uint8_t ssid[P4_HOSTED_ASSOCIATION_SSID_SIZE],
    const uint8_t bssid[P4_HOSTED_ASSOCIATION_BSSID_SIZE])
{
    uint8_t wire[512];
    struct p4_hosted_rpc rpc;
    size_t wire_size = 0u;

    if (!make_event_wire(775u, 0, ssid, 32u, bssid,
                         wire, sizeof wire, &wire_size) ||
        !p4_hosted_rpc_decode(wire, wire_size, &rpc) ||
        !p4_hosted_link_observe(link, generation, &rpc))
        return 0;
    memset(wire, 0xa5, sizeof wire);
    return 1;
}

static void test_init_and_success(void)
{
    struct p4_hosted_link_observation link, before;
    uint8_t ssid[P4_HOSTED_ASSOCIATION_SSID_SIZE];
    uint8_t bssid[P4_HOSTED_ASSOCIATION_BSSID_SIZE];

    memset(&link, 0xa5, sizeof link);
    memcpy(&before, &link, sizeof before);
    CHECK(!p4_hosted_link_init(&link, 0u));
    CHECK(memcmp(&before, &link, sizeof before) == 0);
    CHECK(!p4_hosted_link_init(NULL, 1u));
    CHECK(p4_hosted_link_init(&link, 19u));
    CHECK(link.generation == 19u);
    CHECK(link.revision == 0u);
    CHECK(link.kind == P4_HOSTED_LINK_NONE);
    CHECK(link.status == 0);
    CHECK(all_zero(link.ssid, sizeof link.ssid));
    CHECK(link.ssid_length == 0u);
    CHECK(all_zero(link.bssid, sizeof link.bssid));
    CHECK(link.channel == 0u && link.authmode == 0 && link.reason == 0u);

    fill_pattern(ssid, sizeof ssid, 0x31u);
    ssid[0] = 0u;
    ssid[17] = 0xffu;
    fill_pattern(bssid, sizeof bssid, 0x80u);
    CHECK(observe_from_local_wire(&link, 19u, ssid, bssid));
    CHECK(link.generation == 19u && link.revision == 1u);
    CHECK(link.kind == P4_HOSTED_LINK_UP_HINT);
    CHECK(link.status == 0);
    CHECK(link.ssid_length == 32u);
    CHECK(memcmp(link.ssid, ssid, sizeof ssid) == 0);
    CHECK(memcmp(link.bssid, bssid, sizeof bssid) == 0);
    CHECK(link.channel == 11u && link.authmode == 3);
    CHECK(link.reason == 0u);

    /* A local input frame was overwritten and returned before these checks. */
    CHECK(link.ssid[0] == 0u && link.ssid[17] == 0xffu);
    CHECK(memcmp(link.ssid, ssid, sizeof ssid) == 0);
    CHECK(memcmp(link.bssid, bssid, sizeof bssid) == 0);
}

static void test_disconnect_supersedes_hint(void)
{
    struct p4_hosted_link_observation link;
    struct p4_hosted_rpc rpc;
    uint8_t wire[512];
    size_t wire_size = 0u;
    uint8_t up_ssid[P4_HOSTED_ASSOCIATION_SSID_SIZE];
    uint8_t down_ssid[P4_HOSTED_ASSOCIATION_SSID_SIZE];
    uint8_t up_bssid[P4_HOSTED_ASSOCIATION_BSSID_SIZE];
    uint8_t down_bssid[P4_HOSTED_ASSOCIATION_BSSID_SIZE];

    fill_pattern(up_ssid, sizeof up_ssid, 0x11u);
    fill_pattern(down_ssid, sizeof down_ssid, 0x61u);
    fill_pattern(up_bssid, sizeof up_bssid, 0x22u);
    fill_pattern(down_bssid, sizeof down_bssid, 0x72u);
    CHECK(p4_hosted_link_init(&link, 4u));
    CHECK(decode_event(775u, 0, up_ssid, 32u, up_bssid,
                       wire, sizeof wire, &rpc));
    CHECK(p4_hosted_link_observe(&link, 4u, &rpc));
    CHECK(link.kind == P4_HOSTED_LINK_UP_HINT && link.revision == 1u);

    CHECK(make_event_wire(776u, 0, down_ssid, 9u, down_bssid,
                          wire, sizeof wire, &wire_size));
    CHECK(p4_hosted_rpc_decode(wire, wire_size, &rpc));
    CHECK(p4_hosted_link_observe(&link, 4u, &rpc));
    CHECK(link.kind == P4_HOSTED_LINK_DOWN && link.revision == 2u);
    CHECK(link.status == 0);
    CHECK(link.ssid_length == 9u);
    CHECK(memcmp(link.ssid, down_ssid, sizeof down_ssid) == 0);
    CHECK(memcmp(link.bssid, down_bssid, sizeof down_bssid) == 0);
    CHECK(link.reason == 8u);
    CHECK(link.channel == 0u && link.authmode == 0);
}

static void check_station_identity_cleared(
    const struct p4_hosted_link_observation *link)
{
    CHECK(link->status == 0);
    CHECK(all_zero(link->ssid, sizeof link->ssid));
    CHECK(link->ssid_length == 0u);
    CHECK(all_zero(link->bssid, sizeof link->bssid));
    CHECK(link->channel == 0u && link->authmode == 0 && link->reason == 0u);
}

static void test_station_lifecycle_transitions(void)
{
    struct p4_hosted_link_observation link;
    struct p4_hosted_rpc rpc;
    uint8_t wire[512];
    uint8_t before_ssid[P4_HOSTED_ASSOCIATION_SSID_SIZE];
    uint8_t after_ssid[P4_HOSTED_ASSOCIATION_SSID_SIZE];
    uint8_t before_bssid[P4_HOSTED_ASSOCIATION_BSSID_SIZE];
    uint8_t after_bssid[P4_HOSTED_ASSOCIATION_BSSID_SIZE];
    size_t wire_size = 0u;

    fill_pattern(before_ssid, sizeof before_ssid, 0x13u);
    fill_pattern(after_ssid, sizeof after_ssid, 0x73u);
    fill_pattern(before_bssid, sizeof before_bssid, 0x27u);
    fill_pattern(after_bssid, sizeof after_bssid, 0x87u);
    CHECK(p4_hosted_link_init(&link, 25u));

    CHECK(decode_event(775u, 0, before_ssid, 32u, before_bssid,
                       wire, sizeof wire, &rpc));
    CHECK(p4_hosted_link_observe(&link, 25u, &rpc));
    CHECK(link.revision == 1u && link.kind == P4_HOSTED_LINK_UP_HINT);
    CHECK(memcmp(link.ssid, before_ssid, sizeof before_ssid) == 0);
    CHECK(memcmp(link.bssid, before_bssid, sizeof before_bssid) == 0);

    CHECK(decode_wifi_event(0, P4_HOSTED_WIFI_EVENT_STA_STOP,
                            wire, sizeof wire, &rpc));
    CHECK(p4_hosted_link_observe(&link, 25u, &rpc));
    CHECK(link.revision == 2u && link.kind == P4_HOSTED_LINK_STATION_STOP);
    check_station_identity_cleared(&link);

    CHECK(decode_wifi_event(0, P4_HOSTED_WIFI_EVENT_STA_START,
                            wire, sizeof wire, &rpc));
    CHECK(p4_hosted_link_observe(&link, 25u, &rpc));
    CHECK(link.revision == 3u && link.kind == P4_HOSTED_LINK_STATION_START);
    check_station_identity_cleared(&link);

    CHECK(make_event_wire(775u, 0, after_ssid, sizeof after_ssid, after_bssid,
                          wire, sizeof wire, &wire_size));
    CHECK(p4_hosted_rpc_decode(wire, wire_size, &rpc));
    CHECK(p4_hosted_link_observe(&link, 25u, &rpc));
    CHECK(link.revision == 4u && link.kind == P4_HOSTED_LINK_UP_HINT);
    CHECK(link.ssid_length == sizeof after_ssid);
    CHECK(memcmp(link.ssid, after_ssid, sizeof after_ssid) == 0);
    CHECK(memcmp(link.bssid, after_bssid, sizeof after_bssid) == 0);
}

static void test_nonzero_status_is_down_without_metadata(void)
{
    struct p4_hosted_link_observation link;
    struct p4_hosted_rpc rpc;
    uint8_t wire[512];
    uint8_t ssid[P4_HOSTED_ASSOCIATION_SSID_SIZE];
    uint8_t bssid[P4_HOSTED_ASSOCIATION_BSSID_SIZE];

    fill_pattern(ssid, sizeof ssid, 0x10u);
    fill_pattern(bssid, sizeof bssid, 0x20u);
    CHECK(p4_hosted_link_init(&link, 6u));
    CHECK(decode_event(775u, 7, ssid, 32u, bssid,
                       wire, sizeof wire, &rpc));
    CHECK(p4_hosted_link_observe(&link, 6u, &rpc));
    CHECK(link.kind == P4_HOSTED_LINK_DOWN && link.status == 7);
    CHECK(link.revision == 1u);
    CHECK(all_zero(link.ssid, sizeof link.ssid));
    CHECK(link.ssid_length == 0u);
    CHECK(all_zero(link.bssid, sizeof link.bssid));
    CHECK(link.channel == 0u && link.authmode == 0 && link.reason == 0u);
}

static void test_wrong_envelope_fields_and_stale_generation(void)
{
    struct p4_hosted_link_observation link;
    struct p4_hosted_rpc rpc, bad;
    uint8_t wire[512];
    uint8_t ssid[P4_HOSTED_ASSOCIATION_SSID_SIZE];
    uint8_t bssid[P4_HOSTED_ASSOCIATION_BSSID_SIZE];

    fill_pattern(ssid, sizeof ssid, 0x13u);
    fill_pattern(bssid, sizeof bssid, 0x24u);
    CHECK(p4_hosted_link_init(&link, 12u));
    CHECK(decode_event(775u, 0, ssid, 32u, bssid,
                       wire, sizeof wire, &rpc));

    bad = rpc;
    bad.msg_type = P4_HOSTED_RPC_TYPE_RESPONSE;
    expect_refusal_unchanged(&link, 12u, &bad);
    bad = rpc;
    bad.msg_id = 777u;
    expect_refusal_unchanged(&link, 12u, &bad);
    bad = rpc;
    bad.uid = 1u;
    expect_refusal_unchanged(&link, 12u, &bad);
    expect_refusal_unchanged(&link, 11u, &rpc);
    expect_refusal_unchanged(&link, 0u, &rpc);
    CHECK(!p4_hosted_link_observe(&link, 12u, NULL));
    CHECK(!p4_hosted_link_observe(NULL, 12u, &rpc));

    CHECK(link.revision == 0u);
}

static void test_malformed_nested_payload(void)
{
    struct p4_hosted_link_observation link, before;
    struct p4_hosted_rpc rpc;
    uint8_t wire[64];
    const uint8_t payload[] = { 0x12u, 0x02u, 0x0au, 0x00u };
    struct wire_buffer envelope;

    wire_init(&envelope);
    wire_uint_field(&envelope, 1u, P4_HOSTED_RPC_TYPE_EVENT);
    wire_uint_field(&envelope, 2u, 775u);
    wire_bytes_field(&envelope, 775u, payload, sizeof payload);
    CHECK(envelope.ok && envelope.size <= sizeof wire);
    memcpy(wire, envelope.bytes, envelope.size);
    CHECK(p4_hosted_rpc_decode(wire, envelope.size, &rpc));
    CHECK(p4_hosted_link_init(&link, 22u));
    memcpy(&before, &link, sizeof before);
    CHECK(!p4_hosted_link_observe(&link, 22u, &rpc));
    CHECK(memcmp(&before, &link, sizeof before) == 0);
}

static void test_transactional_exhaustion_and_invalid_state(void)
{
    struct p4_hosted_link_observation link, before;
    struct p4_hosted_rpc rpc;
    uint8_t wire[512];
    uint8_t ssid[P4_HOSTED_ASSOCIATION_SSID_SIZE];
    uint8_t bssid[P4_HOSTED_ASSOCIATION_BSSID_SIZE];

    fill_pattern(ssid, sizeof ssid, 0x31u);
    fill_pattern(bssid, sizeof bssid, 0x41u);
    CHECK(p4_hosted_link_init(&link, 8u));
    CHECK(decode_event(775u, 0, ssid, 32u, bssid,
                       wire, sizeof wire, &rpc));
    link.revision = UINT32_MAX;
    memcpy(&before, &link, sizeof before);
    CHECK(!p4_hosted_link_observe(&link, 8u, &rpc));
    CHECK(memcmp(&before, &link, sizeof before) == 0);

    CHECK(decode_wifi_event(0, P4_HOSTED_WIFI_EVENT_STA_STOP,
                            wire, sizeof wire, &rpc));
    memcpy(&before, &link, sizeof before);
    CHECK(!p4_hosted_link_observe(&link, 8u, &rpc));
    CHECK(memcmp(&before, &link, sizeof before) == 0);

    link.revision = 0u;
    link.kind = P4_HOSTED_LINK_STATION_STOP + 1u;
    memcpy(&before, &link, sizeof before);
    CHECK(!p4_hosted_link_observe(&link, 8u, &rpc));
    CHECK(memcmp(&before, &link, sizeof before) == 0);
}

static void test_wifi_event_refusals_are_transactional(void)
{
    struct p4_hosted_link_observation link;
    struct p4_hosted_rpc rpc, bad;
    uint8_t wire[512];
    uint8_t ssid[P4_HOSTED_ASSOCIATION_SSID_SIZE];
    uint8_t bssid[P4_HOSTED_ASSOCIATION_BSSID_SIZE];
    static const uint8_t malformed[] = { 0x10u, 0x03u, 0x10u, 0x02u };

    fill_pattern(ssid, sizeof ssid, 0x45u);
    fill_pattern(bssid, sizeof bssid, 0x65u);
    CHECK(p4_hosted_link_init(&link, 36u));
    CHECK(decode_event(775u, 0, ssid, 32u, bssid,
                       wire, sizeof wire, &rpc));
    CHECK(p4_hosted_link_observe(&link, 36u, &rpc));

    CHECK(decode_wifi_event(0, 127, wire, sizeof wire, &rpc));
    expect_refusal_unchanged(&link, 36u, &rpc);
    CHECK(decode_wifi_event(7, P4_HOSTED_WIFI_EVENT_STA_STOP,
                            wire, sizeof wire, &rpc));
    expect_refusal_unchanged(&link, 36u, &rpc);
    CHECK(decode_wifi_event(0, P4_HOSTED_WIFI_EVENT_STA_START,
                            wire, sizeof wire, &rpc));
    expect_refusal_unchanged(&link, 35u, &rpc);

    bad = rpc;
    bad.msg_type = P4_HOSTED_RPC_TYPE_RESPONSE;
    expect_refusal_unchanged(&link, 36u, &bad);
    bad = rpc;
    bad.uid = 1u;
    expect_refusal_unchanged(&link, 36u, &bad);
    bad = rpc;
    bad.msg_id = 775u;
    expect_refusal_unchanged(&link, 36u, &bad);
    bad = rpc;
    bad.payload = malformed;
    bad.payload_size = sizeof malformed;
    expect_refusal_unchanged(&link, 36u, &bad);
}

static void test_output_input_aliases(void)
{
    union alias_storage {
        struct p4_hosted_link_observation link;
        struct p4_hosted_rpc rpc;
    } alias, before_alias;
    struct p4_hosted_link_observation link, before;
    struct p4_hosted_rpc rpc;
    uint8_t wire[512];
    uint8_t ssid[P4_HOSTED_ASSOCIATION_SSID_SIZE];
    uint8_t bssid[P4_HOSTED_ASSOCIATION_BSSID_SIZE];

    fill_pattern(ssid, sizeof ssid, 0x52u);
    fill_pattern(bssid, sizeof bssid, 0x62u);
    memset(&alias, 0x5a, sizeof alias);
    CHECK(p4_hosted_link_init(&alias.link, 2u));
    memcpy(&before_alias, &alias, sizeof before_alias);
    CHECK(!p4_hosted_link_observe(&alias.link, 2u, &alias.rpc));
    CHECK(memcmp(&before_alias, &alias, sizeof alias) == 0);

    CHECK(p4_hosted_link_init(&link, 3u));
    CHECK(decode_event(775u, 0, ssid, 32u, bssid,
                       wire, sizeof wire, &rpc));
    memcpy(&before, &link, sizeof before);
    rpc.payload = (const uint8_t *)&link;
    rpc.payload_size = 1u;
    CHECK(!p4_hosted_link_observe(&link, 3u, &rpc));
    CHECK(memcmp(&before, &link, sizeof before) == 0);

    CHECK(decode_wifi_event(0, P4_HOSTED_WIFI_EVENT_STA_STOP,
                            wire, sizeof wire, &rpc));
    memcpy(&before, &link, sizeof before);
    rpc.payload = (const uint8_t *)&link;
    rpc.payload_size = 1u;
    CHECK(!p4_hosted_link_observe(&link, 3u, &rpc));
    CHECK(memcmp(&before, &link, sizeof before) == 0);
}

int main(void)
{
    test_init_and_success();
    test_disconnect_supersedes_hint();
    test_station_lifecycle_transitions();
    test_nonzero_status_is_down_without_metadata();
    test_wrong_envelope_fields_and_stale_generation();
    test_malformed_nested_payload();
    test_transactional_exhaustion_and_invalid_state();
    test_wifi_event_refusals_are_transactional();
    test_output_input_aliases();

    if (failures != 0u) {
        fprintf(stderr, "hosted-link: %u checks, %u failures\n",
                checks, failures);
        return 1;
    }
    printf("hosted-link: %u checks, 0 failures\n", checks);
    return 0;
}
