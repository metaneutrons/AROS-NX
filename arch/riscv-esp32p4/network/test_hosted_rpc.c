/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>

#include "hosted_rpc.h"

static unsigned long checks;

#define CHECK(expression) do { \
    ++checks; \
    if (!(expression)) { \
        fprintf(stderr, "line %u: %s\n", (unsigned int)__LINE__, #expression); \
        exit(1); \
    } \
} while (0)

/* Literal vectors independently encode the pinned Rpc schema's empty
 * GetMACAddress request and ESPInit event. */
static const uint8_t mac_request[] = {
    0x08, 0x01,             /* msg_type = Req */
    0x10, 0x81, 0x02,       /* msg_id = Req_GetMACAddress (257) */
    0x18, 0x01,             /* uid = 1 */
    0x8a, 0x10, 0x00        /* oneof field 257, empty nested message */
};

static const uint8_t esp_init_event[] = {
    0x08, 0x03,             /* msg_type = Event */
    0x10, 0x81, 0x06,       /* msg_id = Event_ESPInit (769) */
    0x8a, 0x30, 0x00        /* oneof field 769, empty nested message */
};

static void set_rpc(struct p4_hosted_rpc *rpc, uint32_t type, uint32_t id,
                    uint32_t uid, const uint8_t *payload, size_t payload_size)
{
    rpc->msg_type = type;
    rpc->msg_id = id;
    rpc->uid = uid;
    rpc->payload = payload;
    rpc->payload_size = payload_size;
}

static void expect_decode_refusal(const uint8_t *bytes, size_t size)
{
    struct p4_hosted_rpc rpc;
    uint8_t before[sizeof rpc];

    memset(&rpc, 0xa5, sizeof rpc);
    memcpy(before, &rpc, sizeof rpc);
    CHECK(!p4_hosted_rpc_decode(bytes, size, &rpc));
    CHECK(memcmp(&rpc, before, sizeof rpc) == 0);
}

static void expect_encode_refusal(const struct p4_hosted_rpc *rpc,
                                 size_t capacity)
{
    uint8_t bytes[128];
    uint8_t before[sizeof bytes];
    size_t written = (size_t)0x5a5a;
    size_t before_written = written;

    memset(bytes, 0xc7, sizeof bytes);
    memcpy(before, bytes, sizeof bytes);
    CHECK(!p4_hosted_rpc_encode(bytes, capacity, rpc, &written));
    CHECK(memcmp(bytes, before, sizeof bytes) == 0);
    CHECK(written == before_written);
}

static void test_literal_vectors_and_canonical_encoding(void)
{
    struct p4_hosted_rpc rpc;
    uint8_t encoded[32];
    size_t written;

    CHECK(p4_hosted_rpc_decode(mac_request, sizeof mac_request, &rpc));
    CHECK(rpc.msg_type == P4_HOSTED_RPC_TYPE_REQUEST);
    CHECK(rpc.msg_id == 257u);
    CHECK(rpc.uid == 1u);
    CHECK(rpc.payload == mac_request + sizeof mac_request);
    CHECK(rpc.payload_size == 0u);

    memset(encoded, 0x6d, sizeof encoded);
    set_rpc(&rpc, P4_HOSTED_RPC_TYPE_REQUEST, 257u, 1u, NULL, 0u);
    written = 0;
    CHECK(p4_hosted_rpc_encode(encoded, sizeof encoded, &rpc, &written));
    CHECK(written == sizeof mac_request);
    CHECK(memcmp(encoded, mac_request, sizeof mac_request) == 0);
    CHECK(encoded[written] == 0x6d);

    CHECK(p4_hosted_rpc_decode(esp_init_event, sizeof esp_init_event, &rpc));
    CHECK(rpc.msg_type == P4_HOSTED_RPC_TYPE_EVENT);
    CHECK(rpc.msg_id == 769u);
    CHECK(rpc.uid == 0u);
    CHECK(rpc.payload == esp_init_event + sizeof esp_init_event);
    CHECK(rpc.payload_size == 0u);

    memset(encoded, 0x39, sizeof encoded);
    set_rpc(&rpc, P4_HOSTED_RPC_TYPE_EVENT, 769u, 0u, NULL, 0u);
    written = 0;
    CHECK(p4_hosted_rpc_encode(encoded, sizeof encoded, &rpc, &written));
    CHECK(written == sizeof esp_init_event);
    CHECK(memcmp(encoded, esp_init_event, sizeof esp_init_event) == 0);
    CHECK(encoded[written] == 0x39);
}

static void test_uid_boundaries_and_profile_edges(void)
{
    static const uint8_t body[] = { 0x08, 0x01 };
    struct p4_hosted_rpc input, output;
    uint8_t bytes[P4_HOSTED_RPC_MAX_SIZE];
    size_t written;

    set_rpc(&input, P4_HOSTED_RPC_TYPE_REQUEST, 257u, UINT32_MAX,
            body, sizeof body);
    CHECK(p4_hosted_rpc_encode(bytes, sizeof bytes, &input, &written));
    CHECK(written <= sizeof bytes);
    CHECK(p4_hosted_rpc_decode(bytes, written, &output));
    CHECK(output.uid == UINT32_MAX);
    CHECK(output.msg_id == input.msg_id);
    CHECK(output.payload_size == sizeof body);
    CHECK(memcmp(output.payload, body, sizeof body) == 0);

    set_rpc(&input, P4_HOSTED_RPC_TYPE_REQUEST, 349u, 1u, NULL, 0u);
    CHECK(p4_hosted_rpc_encode(bytes, sizeof bytes, &input, &written));
    CHECK(p4_hosted_rpc_decode(bytes, written, &output));
    CHECK(output.msg_id == 349u);

    set_rpc(&input, P4_HOSTED_RPC_TYPE_RESPONSE, 513u, 1u, NULL, 0u);
    CHECK(p4_hosted_rpc_encode(bytes, sizeof bytes, &input, &written));
    CHECK(p4_hosted_rpc_decode(bytes, written, &output));
    CHECK(output.msg_id == 513u);

    set_rpc(&input, P4_HOSTED_RPC_TYPE_RESPONSE, 605u, 1u, NULL, 0u);
    CHECK(p4_hosted_rpc_encode(bytes, sizeof bytes, &input, &written));
    CHECK(p4_hosted_rpc_decode(bytes, written, &output));
    CHECK(output.msg_id == 605u);

    set_rpc(&input, P4_HOSTED_RPC_TYPE_EVENT, 776u, 0u, NULL, 0u);
    CHECK(p4_hosted_rpc_encode(bytes, sizeof bytes, &input, &written));
    CHECK(p4_hosted_rpc_decode(bytes, written, &output));
    CHECK(output.msg_id == 776u);
}

static void test_message_size_boundary(void)
{
    uint8_t payload[P4_HOSTED_RPC_MAX_SIZE];
    uint8_t encoded[P4_HOSTED_RPC_MAX_SIZE];
    uint8_t before[P4_HOSTED_RPC_MAX_SIZE];
    struct p4_hosted_rpc input, output;
    size_t written;
    size_t i;

    for (i = 0; i < sizeof payload; ++i)
        payload[i] = (uint8_t)(i * 37u + 11u);

    set_rpc(&input, P4_HOSTED_RPC_TYPE_REQUEST, 257u, 1u,
            payload, P4_HOSTED_RPC_MAX_SIZE - 11u);
    CHECK(p4_hosted_rpc_encode(encoded, sizeof encoded, &input, &written));
    CHECK(written == P4_HOSTED_RPC_MAX_SIZE);
    CHECK(p4_hosted_rpc_decode(encoded, written, &output));
    CHECK(output.payload_size == P4_HOSTED_RPC_MAX_SIZE - 11u);
    for (i = 0; i < output.payload_size; ++i)
        CHECK(output.payload[i] == payload[i]);

    memset(encoded, 0x73, sizeof encoded);
    memcpy(before, encoded, sizeof encoded);
    written = 0x5a5au;
    set_rpc(&input, P4_HOSTED_RPC_TYPE_REQUEST, 257u, 1u,
            payload, P4_HOSTED_RPC_MAX_SIZE - 10u);
    CHECK(!p4_hosted_rpc_encode(encoded, sizeof encoded, &input, &written));
    CHECK(memcmp(encoded, before, sizeof encoded) == 0);
    CHECK(written == 0x5a5au);
}

static void test_reordered_metadata_and_unknown_fields(void)
{
    static const uint8_t reordered[] = {
        0x20, 0x96, 0x01,             /* unknown field 4, varint 150 */
        0x29, 1, 2, 3, 4, 5, 6, 7, 8, /* unknown field 5, fixed64 */
        0x32, 3, 0xaa, 0xbb, 0xcc,   /* unknown field 6, length-delimited */
        0x3d, 0x11, 0x22, 0x33, 0x44,/* unknown field 7, fixed32 */
        0xf8, 0xff, 0xff, 0xff, 0x0f, 0x01, /* maximum field number */
        0x8a, 0x10, 0x00,             /* payload before metadata */
        0x18, 0x01,
        0x10, 0x81, 0x02,
        0x08, 0x01
    };
    struct p4_hosted_rpc rpc;

    CHECK(p4_hosted_rpc_decode(reordered, sizeof reordered, &rpc));
    CHECK(rpc.msg_type == P4_HOSTED_RPC_TYPE_REQUEST);
    CHECK(rpc.msg_id == 257u);
    CHECK(rpc.uid == 1u);
    CHECK(rpc.payload_size == 0u);
}

static void test_duplicate_fields_and_profile_mismatch(void)
{
    uint8_t bytes[64];
    size_t size;

    memcpy(bytes, mac_request, sizeof mac_request);
    size = sizeof mac_request;
    bytes[size++] = 0x08;
    bytes[size++] = 0x01;
    expect_decode_refusal(bytes, size); /* duplicate msg_type */

    memcpy(bytes, mac_request, sizeof mac_request);
    size = sizeof mac_request;
    bytes[size++] = 0x10;
    bytes[size++] = 0x81;
    bytes[size++] = 0x02;
    expect_decode_refusal(bytes, size); /* duplicate msg_id */

    memcpy(bytes, mac_request, sizeof mac_request);
    size = sizeof mac_request;
    bytes[size++] = 0x18;
    bytes[size++] = 0x02;
    expect_decode_refusal(bytes, size); /* duplicate UID */

    memcpy(bytes, mac_request, sizeof mac_request);
    size = sizeof mac_request;
    bytes[size++] = 0x8a;
    bytes[size++] = 0x10;
    bytes[size++] = 0x00;
    expect_decode_refusal(bytes, size); /* duplicate same oneof */

    memcpy(bytes, mac_request, sizeof mac_request);
    size = sizeof mac_request;
    bytes[size++] = 0x92;
    bytes[size++] = 0x10;
    bytes[size++] = 0x00;
    expect_decode_refusal(bytes, size); /* second oneof field */

    memcpy(bytes, mac_request, sizeof mac_request);
    bytes[1] = P4_HOSTED_RPC_TYPE_RESPONSE;
    expect_decode_refusal(bytes, sizeof mac_request); /* request ID as response */

    memcpy(bytes, mac_request, sizeof mac_request);
    bytes[3] = 0x82; /* msg_id 258; payload tag remains 257 */
    expect_decode_refusal(bytes, sizeof mac_request);

    memcpy(bytes, mac_request, sizeof mac_request);
    bytes[1] = P4_HOSTED_RPC_TYPE_EVENT;
    expect_decode_refusal(bytes, sizeof mac_request); /* request profile as event */

    {
        static const uint8_t request_without_uid[] = {
            0x08, 0x01, 0x10, 0x81, 0x02, 0x8a, 0x10, 0x00
        };
        static const uint8_t request_with_zero_uid[] = {
            0x08, 0x01, 0x10, 0x81, 0x02, 0x18, 0x00,
            0x8a, 0x10, 0x00
        };
        static const uint8_t payload_field_mismatch[] = {
            0x08, 0x01, 0x10, 0x81, 0x02, 0x18, 0x01,
            0x8a, 0x20, 0x00 /* field 513 does not match msg_id 257 */
        };
        expect_decode_refusal(request_without_uid, sizeof request_without_uid);
        expect_decode_refusal(request_with_zero_uid, sizeof request_with_zero_uid);
        expect_decode_refusal(payload_field_mismatch,
                              sizeof payload_field_mismatch);
    }
}

static void test_bad_metadata_and_id_sentinels(void)
{
    struct p4_hosted_rpc rpc;

    set_rpc(&rpc, P4_HOSTED_RPC_TYPE_INVALID, 257u, 1u, NULL, 0u);
    expect_encode_refusal(&rpc, 128u);
    set_rpc(&rpc, 4u, 257u, 1u, NULL, 0u);
    expect_encode_refusal(&rpc, 128u);
    set_rpc(&rpc, P4_HOSTED_RPC_TYPE_REQUEST, 256u, 1u, NULL, 0u);
    expect_encode_refusal(&rpc, 128u);
    set_rpc(&rpc, P4_HOSTED_RPC_TYPE_REQUEST, 350u, 1u, NULL, 0u);
    expect_encode_refusal(&rpc, 128u);
    set_rpc(&rpc, P4_HOSTED_RPC_TYPE_RESPONSE, 512u, 1u, NULL, 0u);
    expect_encode_refusal(&rpc, 128u);
    set_rpc(&rpc, P4_HOSTED_RPC_TYPE_RESPONSE, 606u, 1u, NULL, 0u);
    expect_encode_refusal(&rpc, 128u);
    set_rpc(&rpc, P4_HOSTED_RPC_TYPE_EVENT, 768u, 0u, NULL, 0u);
    expect_encode_refusal(&rpc, 128u);
    set_rpc(&rpc, P4_HOSTED_RPC_TYPE_EVENT, 777u, 0u, NULL, 0u);
    expect_encode_refusal(&rpc, 128u);
    set_rpc(&rpc, P4_HOSTED_RPC_TYPE_REQUEST, 257u, 0u, NULL, 0u);
    expect_encode_refusal(&rpc, 128u);
    set_rpc(&rpc, P4_HOSTED_RPC_TYPE_RESPONSE, 513u, 0u, NULL, 0u);
    expect_encode_refusal(&rpc, 128u);
    set_rpc(&rpc, P4_HOSTED_RPC_TYPE_REQUEST, 257u, 1u, NULL, 1u);
    expect_encode_refusal(&rpc, 128u);
    set_rpc(&rpc, P4_HOSTED_RPC_TYPE_REQUEST, 257u, 1u, NULL,
            P4_HOSTED_RPC_MAX_SIZE + 1u);
    expect_encode_refusal(&rpc, 128u);

    {
        static const uint8_t wrong_type[] = {
            0x0a, 0x01, 0x01, /* msg_type encoded with wire type 2 */
            0x10, 0x81, 0x02, 0x18, 0x01, 0x8a, 0x10, 0x00
        };
        static const uint8_t wrong_payload_wire[] = {
            0x08, 0x01, 0x10, 0x81, 0x02, 0x18, 0x01,
            0x88, 0x10, 0x00
        };
        static const uint8_t request_sentinel[] = {
            0x08, 0x01, 0x10, 0xde, 0x02, 0x18, 0x01,
            0xf2, 0x15, 0x00 /* request sentinel ID 350 is not a message */
        };
        expect_decode_refusal(wrong_type, sizeof wrong_type);
        expect_decode_refusal(wrong_payload_wire, sizeof wrong_payload_wire);
        expect_decode_refusal(request_sentinel, sizeof request_sentinel);
    }
}

static void test_malformed_keys_varints_and_lengths(void)
{
    static const uint8_t zero_key[] = { 0x00 };
    static const uint8_t truncated_key[] = { 0x80 };
    static const uint8_t key_overflow[] = { 0x80, 0x80, 0x80, 0x80, 0x10 };
    static const uint8_t truncated_value[] = { 0x08, 0x80 };
    static const uint8_t value_overflow[] = {
        0x08, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0x02
    };
    static const uint8_t unknown_wire3[] = { 0x23, 0x00 };
    static const uint8_t unknown_wire4[] = { 0x24 };
    static const uint8_t unknown_wire6[] = { 0x26 };
    static const uint8_t unknown_wire7[] = { 0x27 };
    static const uint8_t unknown_field_zero[] = { 0x05, 1, 2, 3, 4 };
    static const uint8_t unknown_fixed64_short[] = { 0x09, 1, 2, 3, 4, 5, 6, 7 };
    static const uint8_t unknown_fixed32_short[] = { 0x0d, 1, 2, 3 };
    static const uint8_t unknown_length_short[] = { 0x12, 3, 1, 2 };
    static const uint8_t unknown_length_overflow[] = {
        0x12, 0x80, 0x80, 0x80, 0x80, 0x10
    };
    static const uint8_t payload_length_short[] = {
        0x08, 0x01, 0x10, 0x81, 0x02, 0x18, 0x01,
        0x8a, 0x10, 0x03, 0x01, 0x02
    };
    static const uint8_t payload_length_over_cap[] = {
        0x08, 0x01, 0x10, 0x81, 0x02, 0x18, 0x01,
        0x8a, 0x10, 0x81, 0x20
    };
    static const uint8_t payload_length_varint_overflow[] = {
        0x08, 0x01, 0x10, 0x81, 0x02, 0x18, 0x01,
        0x8a, 0x10, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0x02
    };
    static const uint8_t uid_overflow[] = {
        0x08, 0x01, 0x10, 0x81, 0x02,
        0x18, 0x80, 0x80, 0x80, 0x80, 0x10,
        0x8a, 0x10, 0x00
    };
    static const uint8_t trailing_truncated_key[] = {
        0x08, 0x01, 0x10, 0x81, 0x02, 0x18, 0x01,
        0x8a, 0x10, 0x00, 0x80
    };
    uint8_t oversized[P4_HOSTED_RPC_MAX_SIZE + 1u];
    size_t i;

    expect_decode_refusal(NULL, 0);
    expect_decode_refusal(zero_key, sizeof zero_key);
    expect_decode_refusal(truncated_key, sizeof truncated_key);
    expect_decode_refusal(key_overflow, sizeof key_overflow);
    expect_decode_refusal(truncated_value, sizeof truncated_value);
    expect_decode_refusal(value_overflow, sizeof value_overflow);
    expect_decode_refusal(unknown_wire3, sizeof unknown_wire3);
    expect_decode_refusal(unknown_wire4, sizeof unknown_wire4);
    expect_decode_refusal(unknown_wire6, sizeof unknown_wire6);
    expect_decode_refusal(unknown_wire7, sizeof unknown_wire7);
    expect_decode_refusal(unknown_field_zero, sizeof unknown_field_zero);
    expect_decode_refusal(unknown_fixed64_short, sizeof unknown_fixed64_short);
    expect_decode_refusal(unknown_fixed32_short, sizeof unknown_fixed32_short);
    expect_decode_refusal(unknown_length_short, sizeof unknown_length_short);
    expect_decode_refusal(unknown_length_overflow, sizeof unknown_length_overflow);
    expect_decode_refusal(payload_length_short, sizeof payload_length_short);
    expect_decode_refusal(payload_length_over_cap, sizeof payload_length_over_cap);
    expect_decode_refusal(payload_length_varint_overflow,
                          sizeof payload_length_varint_overflow);
    expect_decode_refusal(uid_overflow, sizeof uid_overflow);
    expect_decode_refusal(trailing_truncated_key, sizeof trailing_truncated_key);

    for (i = 0; i < sizeof mac_request; ++i)
        expect_decode_refusal(mac_request, i);
    memset(oversized, 0, sizeof oversized);
    memcpy(oversized, mac_request, sizeof mac_request);
    expect_decode_refusal(oversized, sizeof oversized);
}

static void test_encode_capacity_and_alias_refusal(void)
{
    struct p4_hosted_rpc rpc;
    uint8_t storage[64];
    uint8_t before[sizeof storage];
    size_t written;

    set_rpc(&rpc, P4_HOSTED_RPC_TYPE_REQUEST, 257u, 1u, NULL, 0u);
    expect_encode_refusal(&rpc, sizeof mac_request - 1u);

    memset(storage, 0xb4, sizeof storage);
    memcpy(before, storage, sizeof storage);
    written = 0x1234u;
    set_rpc(&rpc, P4_HOSTED_RPC_TYPE_REQUEST, 257u, 1u,
            storage + 40u, 2u);
    CHECK(!p4_hosted_rpc_encode(storage, sizeof storage, &rpc, &written));
    CHECK(memcmp(storage, before, sizeof storage) == 0);
    CHECK(written == 0x1234u);

    {
        union {
            struct p4_hosted_rpc rpc;
            uint8_t bytes[128];
        } alias;
        struct p4_hosted_rpc original;
        uint8_t snapshot[sizeof alias.bytes];

        memset(alias.bytes, 0xd1, sizeof alias.bytes);
        set_rpc(&alias.rpc, P4_HOSTED_RPC_TYPE_REQUEST, 257u, 1u, NULL, 0u);
        memcpy(snapshot, alias.bytes, sizeof alias.bytes);
        written = 0x4567u;
        CHECK(!p4_hosted_rpc_encode(alias.bytes, sizeof alias.bytes,
                                   &alias.rpc, &written));
        CHECK(memcmp(alias.bytes, snapshot, sizeof alias.bytes) == 0);
        CHECK(written == 0x4567u);

        set_rpc(&original, P4_HOSTED_RPC_TYPE_REQUEST, 257u, 1u, NULL, 0u);
        {
            union {
                size_t value;
                uint8_t bytes[64];
            } written_alias;
            memset(written_alias.bytes, 0x92, sizeof written_alias.bytes);
            memcpy(snapshot, written_alias.bytes,
                   sizeof written_alias.bytes);
            CHECK(!p4_hosted_rpc_encode(written_alias.bytes,
                                       sizeof written_alias.bytes,
                                       &original, &written_alias.value));
            CHECK(memcmp(written_alias.bytes, snapshot,
                         sizeof written_alias.bytes) == 0);
        }
    }

    set_rpc(&rpc, P4_HOSTED_RPC_TYPE_REQUEST, 257u, 1u,
            (const uint8_t *)&rpc, 1u);
    memset(storage, 0x4a, sizeof storage);
    memcpy(before, storage, sizeof storage);
    written = 0x6789u;
    CHECK(!p4_hosted_rpc_encode(storage, sizeof storage, &rpc, &written));
    CHECK(memcmp(storage, before, sizeof storage) == 0);
    CHECK(written == 0x6789u);
}

static void test_decode_alias_refusal(void)
{
    union {
        struct p4_hosted_rpc rpc;
        uint8_t bytes[sizeof(struct p4_hosted_rpc) + 16u];
    } alias;
    uint8_t before[sizeof alias.bytes];

    memcpy(alias.bytes, mac_request, sizeof mac_request);
    memset(alias.bytes + sizeof mac_request, 0x39,
           sizeof alias.bytes - sizeof mac_request);
    memcpy(before, alias.bytes, sizeof alias.bytes);
    CHECK(!p4_hosted_rpc_decode(alias.bytes, sizeof mac_request, &alias.rpc));
    CHECK(memcmp(alias.bytes, before, sizeof alias.bytes) == 0);
}

static uint32_t random_state = UINT32_C(0x62f17a4d);

static uint32_t next_random(void)
{
    random_state = random_state * UINT32_C(1664525) + UINT32_C(1013904223);
    return random_state;
}

static void test_deterministic_hostile_inputs(void)
{
    uint8_t bytes[96];
    uint8_t encoded[P4_HOSTED_RPC_MAX_SIZE];
    unsigned int iteration;

    for (iteration = 0; iteration < 20000u; ++iteration) {
        struct p4_hosted_rpc rpc;
        uint8_t before[sizeof rpc];
        size_t size = (size_t)(next_random() % (sizeof bytes + 1u));
        size_t i;
        int accepted;

        for (i = 0; i < size; ++i)
            bytes[i] = (uint8_t)next_random();
        memset(&rpc, 0x5b, sizeof rpc);
        memcpy(before, &rpc, sizeof rpc);
        accepted = p4_hosted_rpc_decode(bytes, size, &rpc);
        ++checks;
        if (!accepted) {
            CHECK(memcmp(&rpc, before, sizeof rpc) == 0);
            continue;
        }

        CHECK((rpc.msg_type == P4_HOSTED_RPC_TYPE_REQUEST &&
               rpc.msg_id > 256u && rpc.msg_id < 350u) ||
              (rpc.msg_type == P4_HOSTED_RPC_TYPE_RESPONSE &&
               rpc.msg_id > 512u && rpc.msg_id < 606u) ||
              (rpc.msg_type == P4_HOSTED_RPC_TYPE_EVENT &&
               rpc.msg_id > 768u && rpc.msg_id < 777u));
        CHECK(rpc.payload_size <= size);
        CHECK(rpc.payload >= bytes && rpc.payload <= bytes + size);
        CHECK(rpc.payload_size <= (size_t)((bytes + size) - rpc.payload));
        CHECK(p4_hosted_rpc_encode(encoded, sizeof encoded, &rpc, &i));
        CHECK(p4_hosted_rpc_decode(encoded, i, &rpc));
    }
}

int main(void)
{
    test_literal_vectors_and_canonical_encoding();
    test_uid_boundaries_and_profile_edges();
    test_message_size_boundary();
    test_reordered_metadata_and_unknown_fields();
    test_duplicate_fields_and_profile_mismatch();
    test_bad_metadata_and_id_sentinels();
    test_malformed_keys_varints_and_lengths();
    test_encode_capacity_and_alias_refusal();
    test_decode_alias_refusal();
    test_deterministic_hostile_inputs();
    printf("hosted RPC: %lu checks\n", checks);
    return 0;
}
