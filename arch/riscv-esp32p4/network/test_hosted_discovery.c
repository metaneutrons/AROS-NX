/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
/* Native host tests for the negotiated P4/C6 discovery RPC codec. */
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "discovery_wire.h"
#include "esp_hosted_rpc.pb-c.h"
#include "hosted_discovery.h"
#include "hosted_rpc.h"

#define TEST_UID UINT32_C(0x12345678)
#define TEST_NONCE UINT32_C(0x89abcdef)
#define OTHER_NONCE UINT32_C(0x10203040)
#define ARRAY_COUNT(array) (sizeof(array) / sizeof((array)[0]))

static unsigned checks;
static unsigned cases;
static const char *active_case = "setup";

#define CHECK(expression) do { \
    ++checks; \
    if (!(expression)) { \
        fprintf(stderr, "%s: line %u: %s\n", active_case, \
                (unsigned)__LINE__, #expression); \
        exit(EXIT_FAILURE); \
    } \
} while (0)

static void begin_case(const char *name)
{
    active_case = name;
    ++cases;
}

static void fill_valid_init(struct p4_hosted_init *init)
{
    memset(init, 0, sizeof(*init));
    init->chip_id = P4_HOSTED_INIT_CHIP_ESP32C6;
    init->capabilities = P4_HOSTED_INIT_CAP_WLAN_SDIO;
    init->board_discovery_version = P4_HOSTED_INIT_BOARD_DISCOVERY_VERSION_CURRENT;
    init->present = P4_HOSTED_INIT_HAS_BOARD_DISCOVERY |
        P4_HOSTED_INIT_HAS_CHIP_ID | P4_HOSTED_INIT_HAS_CAPABILITIES;
}

static void fill_valid_snapshot(struct board_discovery_snapshot *snapshot)
{
    static const char *const labels[] = { "nvs", "ota_0", "ota_1" };
    size_t i;

    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->physical_bytes = UINT32_C(0x01000000);
    snapshot->layout.flash_bytes = UINT32_C(0x00800000);
    snapshot->layout.count = ARRAY_COUNT(labels);
    snapshot->running = 0;
    snapshot->boot = 1;
    snapshot->update = 2;
    snapshot->running_state = 3;
    snapshot->security_known = BOARD_DISCOVERY_SECURITY_ALL;
    snapshot->security_enabled = BOARD_DISCOVERY_SECURITY_ENCRYPTION |
        BOARD_DISCOVERY_SECURITY_ANTI_ROLLBACK;

    for (i = 0; i < ARRAY_COUNT(labels); ++i) {
        struct board_partition *part = &snapshot->layout.partitions[i];
        part->offset = UINT32_C(0x00010000) + (uint32_t)i * UINT32_C(0x10000);
        part->size = UINT32_C(0x00010000);
        part->type = 0;
        part->subtype = (uint8_t)(0x10u + i);
        (void)snprintf(part->label, sizeof(part->label), "%s", labels[i]);
    }
}

static void fill_max_snapshot(struct board_discovery_snapshot *snapshot)
{
    size_t i;

    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->physical_bytes = UINT32_C(0x01000000);
    snapshot->layout.flash_bytes = UINT32_C(0x00800000);
    snapshot->layout.count = BOARD_LAYOUT_MAX_PARTITIONS;
    snapshot->running = 0;
    snapshot->boot = 1;
    snapshot->update = 2;
    snapshot->running_state = 3;
    snapshot->security_known = BOARD_DISCOVERY_SECURITY_ALL;
    snapshot->security_enabled = BOARD_DISCOVERY_SECURITY_ENCRYPTION |
        BOARD_DISCOVERY_SECURITY_ANTI_ROLLBACK;

    for (i = 0; i < snapshot->layout.count; ++i) {
        struct board_partition *part = &snapshot->layout.partitions[i];
        if (i < 3u) {
            part->offset = UINT32_C(0x00010000) +
                (uint32_t)i * UINT32_C(0x00010000);
            part->size = UINT32_C(0x00010000);
            part->type = 0;
            part->subtype = (uint8_t)(0x10u + i);
        } else {
            part->offset = UINT32_C(0x00040000) +
                (uint32_t)(i - 3u) * UINT32_C(0x1000);
            part->size = UINT32_C(0x1000);
            part->type = 1;
            part->subtype = 2;
        }
        part->flags = (uint32_t)(i & 1u);
        (void)snprintf(part->label, sizeof(part->label), "part%02u",
                       (unsigned)i);
    }
}

static int snapshot_equal(const struct board_discovery_snapshot *left,
                          const struct board_discovery_snapshot *right)
{
    size_t i;

    if (left->layout.flash_bytes != right->layout.flash_bytes ||
        left->layout.count != right->layout.count ||
        left->layout.md5_record_present != right->layout.md5_record_present ||
        left->physical_bytes != right->physical_bytes ||
        left->running != right->running || left->boot != right->boot ||
        left->update != right->update ||
        left->running_state != right->running_state ||
        left->security_known != right->security_known ||
        left->security_enabled != right->security_enabled)
        return 0;

    for (i = 0; i < left->layout.count; ++i) {
        const struct board_partition *a = &left->layout.partitions[i];
        const struct board_partition *b = &right->layout.partitions[i];
        if (a->offset != b->offset || a->size != b->size ||
            a->flags != b->flags || a->type != b->type ||
            a->subtype != b->subtype || strcmp(a->label, b->label))
            return 0;
    }
    return 1;
}

static int snapshot_is_zero(const struct board_discovery_snapshot *snapshot)
{
    struct board_discovery_snapshot zeroed;
    size_t i, j;

    memset(&zeroed, 0, sizeof(zeroed));
    if (snapshot->layout.flash_bytes != zeroed.layout.flash_bytes ||
        snapshot->layout.count != zeroed.layout.count ||
        snapshot->layout.md5_record_present != zeroed.layout.md5_record_present ||
        snapshot->physical_bytes != zeroed.physical_bytes ||
        snapshot->running != zeroed.running || snapshot->boot != zeroed.boot ||
        snapshot->update != zeroed.update ||
        snapshot->running_state != zeroed.running_state ||
        snapshot->security_known != zeroed.security_known ||
        snapshot->security_enabled != zeroed.security_enabled)
        return 0;
    for (i = 0; i < BOARD_LAYOUT_MAX_PARTITIONS; ++i) {
        const struct board_partition *part = &snapshot->layout.partitions[i];
        if (part->offset || part->size || part->flags || part->type ||
            part->subtype)
            return 0;
        for (j = 0; j < sizeof(part->label); ++j)
            if (part->label[j]) return 0;
    }
    return 1;
}

static size_t encode_valid_inventory(uint32_t nonce, uint8_t *bytes,
                                     size_t capacity)
{
    struct board_discovery_snapshot snapshot;
    size_t written = 0;

    fill_valid_snapshot(&snapshot);
    CHECK(board_discovery_validate(&snapshot));
    CHECK(board_discovery_encode(&snapshot, nonce, bytes, capacity, &written));
    return written;
}

static size_t reference_pack(Rpc *rpc, uint8_t *bytes, size_t capacity)
{
    size_t size = rpc__get_packed_size(rpc);
    CHECK(size <= capacity);
    CHECK(rpc__pack(rpc, bytes) == size);
    return size;
}

static size_t reference_request(uint32_t uid, uint32_t nonce,
                                uint8_t *bytes, size_t capacity)
{
    Rpc rpc = RPC__INIT;
    RpcReqBoardDiscovery body = RPC__REQ__BOARD_DISCOVERY__INIT;

    body.version = BOARD_DISCOVERY_WIRE_VERSION;
    body.nonce = nonce;
    rpc.msg_type = RPC_TYPE__Req;
    rpc.msg_id = RPC_ID__Req_BoardDiscovery;
    rpc.uid = uid;
    rpc.payload_case = RPC__PAYLOAD_REQ_BOARD_DISCOVERY;
    rpc.req_board_discovery = &body;
    return reference_pack(&rpc, bytes, capacity);
}

static size_t reference_response(uint32_t uid, int32_t status,
                                 const uint8_t *snapshot, size_t snapshot_size,
                                 uint8_t *bytes, size_t capacity)
{
    Rpc rpc = RPC__INIT;
    RpcRespBoardDiscovery body = RPC__RESP__BOARD_DISCOVERY__INIT;

    body.resp = status;
    body.snapshot.data = (uint8_t *)snapshot;
    body.snapshot.len = snapshot_size;
    rpc.msg_type = RPC_TYPE__Resp;
    rpc.msg_id = RPC_ID__Resp_BoardDiscovery;
    rpc.uid = uid;
    rpc.payload_case = RPC__PAYLOAD_RESP_BOARD_DISCOVERY;
    rpc.resp_board_discovery = &body;
    return reference_pack(&rpc, bytes, capacity);
}

static void append_varint(uint8_t *bytes, size_t capacity, size_t *offset,
                          uint64_t value)
{
    do {
        uint8_t byte = (uint8_t)(value & UINT64_C(0x7f));
        value >>= 7;
        if (value) byte = (uint8_t)(byte | 0x80u);
        CHECK(*offset < capacity);
        bytes[(*offset)++] = byte;
    } while (value);
}

static void append_key(uint8_t *bytes, size_t capacity, size_t *offset,
                       uint32_t field, unsigned wire_type)
{
    append_varint(bytes, capacity, offset,
                  ((uint64_t)field << 3) | (uint64_t)wire_type);
}

static void append_raw(uint8_t *bytes, size_t capacity, size_t *offset,
                       const uint8_t *source, size_t length)
{
    CHECK(*offset <= capacity);
    CHECK(length <= capacity - *offset);
    if (length) memcpy(bytes + *offset, source, length);
    *offset += length;
}

static void append_bytes_field(uint8_t *bytes, size_t capacity, size_t *offset,
                               uint32_t field, const uint8_t *value,
                               size_t value_size)
{
    append_key(bytes, capacity, offset, field, 2u);
    append_varint(bytes, capacity, offset, value_size);
    append_raw(bytes, capacity, offset, value, value_size);
}

static size_t make_body(int include_status, int32_t status,
                        int include_snapshot, const uint8_t *snapshot,
                        size_t snapshot_size, uint8_t *bytes, size_t capacity)
{
    size_t offset = 0;

    if (include_status) {
        append_key(bytes, capacity, &offset, 1u, 0u);
        append_varint(bytes, capacity, &offset, (uint64_t)(int64_t)status);
    }
    if (include_snapshot)
        append_bytes_field(bytes, capacity, &offset, 2u, snapshot,
                           snapshot_size);
    return offset;
}

static size_t wrap_payload(const uint8_t *payload, size_t payload_size,
                           uint32_t type, uint32_t id, uint32_t uid,
                           int include_uid, uint32_t payload_field,
                           uint8_t *bytes, size_t capacity)
{
    size_t offset = 0;

    append_key(bytes, capacity, &offset, 1u, 0u);
    append_varint(bytes, capacity, &offset, type);
    append_key(bytes, capacity, &offset, 2u, 0u);
    append_varint(bytes, capacity, &offset, id);
    if (include_uid) {
        append_key(bytes, capacity, &offset, 3u, 0u);
        append_varint(bytes, capacity, &offset, uid);
    }
    append_bytes_field(bytes, capacity, &offset, payload_field, payload,
                       payload_size);
    return offset;
}

static size_t raw_response(const uint8_t *payload, size_t payload_size,
                           uint32_t uid, uint8_t *bytes, size_t capacity)
{
    return wrap_payload(payload, payload_size, P4_HOSTED_RPC_TYPE_RESPONSE,
        P4_HOSTED_DISCOVERY_RESPONSE_ID, uid, 1,
        P4_HOSTED_DISCOVERY_RESPONSE_ID, bytes, capacity);
}

static void expect_response_refusal(const uint8_t *bytes, size_t size,
                                    uint32_t uid, uint32_t nonce)
{
    struct p4_hosted_discovery_result result;
    uint8_t before[sizeof(result)];

    memset(&result, 0xa7, sizeof(result));
    memcpy(before, &result, sizeof(result));
    CHECK(!p4_hosted_discovery_response(bytes, size, uid, nonce, &result));
    CHECK(memcmp(&result, before, sizeof(result)) == 0);
}

static void expect_request_refusal(const struct p4_hosted_init *init,
                                   uint32_t uid, uint32_t nonce,
                                   size_t capacity)
{
    uint8_t bytes[128], before[sizeof(bytes)];
    size_t written = SIZE_MAX;

    memset(bytes, 0x5a, sizeof(bytes));
    memcpy(before, bytes, sizeof(bytes));
    CHECK(!p4_hosted_discovery_request(init, uid, nonce, bytes, capacity,
                                       &written));
    CHECK(written == SIZE_MAX);
    CHECK(memcmp(bytes, before, sizeof(bytes)) == 0);
}

static void test_request_matches_generated_schema(void)
{
    struct p4_hosted_init init;
    struct p4_hosted_rpc decoded;
    uint8_t native[128], reference[128];
    size_t native_size = 0, reference_size;

    begin_case("request_matches_generated_schema");
    fill_valid_init(&init);
    CHECK(p4_hosted_discovery_request(&init, TEST_UID, TEST_NONCE, native,
                                     sizeof(native), &native_size));
    reference_size = reference_request(TEST_UID, TEST_NONCE, reference,
                                       sizeof(reference));
    CHECK(native_size == reference_size);
    CHECK(memcmp(native, reference, native_size) == 0);
    CHECK(p4_hosted_rpc_decode_board_discovery(native, native_size, &decoded));
    CHECK(decoded.msg_type == P4_HOSTED_RPC_TYPE_REQUEST);
    CHECK(decoded.msg_id == P4_HOSTED_DISCOVERY_REQUEST_ID);
    CHECK(decoded.uid == TEST_UID);
    {
        Rpc *unpacked = rpc__unpack(NULL, reference_size, native);
        CHECK(unpacked != NULL);
        CHECK(unpacked->msg_type == RPC_TYPE__Req);
        CHECK(unpacked->msg_id == RPC_ID__Req_BoardDiscovery);
        CHECK(unpacked->uid == TEST_UID);
        CHECK(unpacked->payload_case == RPC__PAYLOAD_REQ_BOARD_DISCOVERY);
        CHECK(unpacked->req_board_discovery->version ==
              BOARD_DISCOVERY_WIRE_VERSION);
        CHECK(unpacked->req_board_discovery->nonce == TEST_NONCE);
        rpc__free_unpacked(unpacked, NULL);
    }
    CHECK(!p4_hosted_rpc_decode(native, native_size, &decoded));
}

static void test_request_refusals_and_aliases(void)
{
    struct p4_hosted_init init, invalid;
    union {
        max_align_t alignment;
        uint8_t bytes[128];
    } alias_bytes;
    union {
        struct p4_hosted_init init;
        size_t written;
    } alias_written;
    uint8_t output[128], before[sizeof(output)];
    size_t written;

    begin_case("request_refusals_and_aliases");
    fill_valid_init(&init);
    expect_request_refusal(NULL, TEST_UID, TEST_NONCE, sizeof(output));
    expect_request_refusal(&init, 0, TEST_NONCE, sizeof(output));
    expect_request_refusal(&init, TEST_UID, 0, sizeof(output));
    expect_request_refusal(&init, TEST_UID, TEST_NONCE, 0);
    expect_request_refusal(&init, TEST_UID, TEST_NONCE, 1);

    invalid = init;
    invalid.present &= (uint8_t)~P4_HOSTED_INIT_HAS_BOARD_DISCOVERY;
    expect_request_refusal(&invalid, TEST_UID, TEST_NONCE, sizeof(output));
    invalid = init;
    invalid.present &= (uint8_t)~P4_HOSTED_INIT_HAS_CHIP_ID;
    expect_request_refusal(&invalid, TEST_UID, TEST_NONCE, sizeof(output));
    invalid = init;
    invalid.present &= (uint8_t)~P4_HOSTED_INIT_HAS_CAPABILITIES;
    expect_request_refusal(&invalid, TEST_UID, TEST_NONCE, sizeof(output));
    invalid = init;
    invalid.board_discovery_version = 1;
    expect_request_refusal(&invalid, TEST_UID, TEST_NONCE, sizeof(output));
    invalid = init;
    invalid.chip_id = P4_HOSTED_INIT_CHIP_ESP32C3;
    expect_request_refusal(&invalid, TEST_UID, TEST_NONCE, sizeof(output));
    invalid = init;
    invalid.capabilities = 0;
    expect_request_refusal(&invalid, TEST_UID, TEST_NONCE, sizeof(output));

    memset(&alias_bytes, 0x31, sizeof(alias_bytes));
    fill_valid_init((struct p4_hosted_init *)(void *)alias_bytes.bytes);
    memcpy(before, alias_bytes.bytes, sizeof(before));
    written = SIZE_MAX;
    CHECK(!p4_hosted_discovery_request(
        (const struct p4_hosted_init *)(const void *)alias_bytes.bytes,
        TEST_UID, TEST_NONCE, alias_bytes.bytes, sizeof(alias_bytes.bytes),
        &written));
    CHECK(written == SIZE_MAX);
    CHECK(memcmp(alias_bytes.bytes, before, sizeof(before)) == 0);

    fill_valid_init(&alias_written.init);
    {
        uint8_t alias_before[sizeof(alias_written)];
        memcpy(alias_before, &alias_written, sizeof(alias_written));
        memset(output, 0x5a, sizeof(output));
        memcpy(before, output, sizeof(output));
        CHECK(!p4_hosted_discovery_request(&alias_written.init, TEST_UID,
            TEST_NONCE, output, sizeof(output), &alias_written.written));
        CHECK(memcmp(&alias_written, alias_before, sizeof(alias_written)) == 0);
        CHECK(memcmp(output, before, sizeof(output)) == 0);
    }

    memset(output, 0x5a, sizeof(output));
    written = SIZE_MAX;
    memcpy(before, output, sizeof(output));
    CHECK(!p4_hosted_discovery_request(&init, TEST_UID, TEST_NONCE,
        output, sizeof(output), (size_t *)(void *)(output + 8)));
    CHECK(memcmp(output, before, sizeof(output)) == 0);

    memset(output, 0x5a, sizeof(output));
    memcpy(before, output, sizeof(output));
    written = SIZE_MAX;
    CHECK(!p4_hosted_discovery_request(&init, TEST_UID, TEST_NONCE,
        output, SIZE_MAX, &written));
    CHECK(written == SIZE_MAX);
    CHECK(memcmp(output, before, sizeof(output)) == 0);
}

static void test_success_and_reference_response(void)
{
    struct board_discovery_snapshot expected;
    struct p4_hosted_discovery_result result;
    uint8_t inventory[BOARD_DISCOVERY_WIRE_MAX];
    uint8_t response[P4_HOSTED_RPC_MAX_SIZE];
    size_t inventory_size, response_size;

    begin_case("success_and_reference_response");
    fill_valid_snapshot(&expected);
    inventory_size = encode_valid_inventory(TEST_NONCE, inventory,
                                             sizeof(inventory));
    response_size = reference_response(TEST_UID, 0, inventory, inventory_size,
                                       response, sizeof(response));
    memset(&result, 0xa5, sizeof(result));
    CHECK(p4_hosted_discovery_response(response, response_size, TEST_UID,
                                       TEST_NONCE, &result));
    CHECK(result.status == 0);
    CHECK(snapshot_equal(&result.snapshot, &expected));

    /* Proto3 omits its default status field in a canonical generated message. */
    {
        uint8_t body[BOARD_DISCOVERY_WIRE_MAX + 32u];
        size_t body_size = make_body(0, 0, 1, inventory, inventory_size,
                                     body, sizeof(body));
        response_size = raw_response(body, body_size, TEST_UID, response,
                                     sizeof(response));
        CHECK(p4_hosted_discovery_response(response, response_size, TEST_UID,
                                           TEST_NONCE, &result));
        CHECK(result.status == 0);
        CHECK(snapshot_equal(&result.snapshot, &expected));
    }

    /* Both native special-purpose codecs reject discovery in ordinary RPC. */
    {
        struct p4_hosted_rpc rpc;
        uint8_t ordinary[128], before[sizeof(ordinary)];
        uint8_t payload[] = { 8u, 1u };
        size_t written = SIZE_MAX;

        memset(&rpc, 0, sizeof(rpc));
        rpc.msg_type = P4_HOSTED_RPC_TYPE_REQUEST;
        rpc.msg_id = P4_HOSTED_DISCOVERY_REQUEST_ID;
        rpc.uid = TEST_UID;
        rpc.payload = payload;
        rpc.payload_size = sizeof(payload);
        memset(ordinary, 0x6d, sizeof(ordinary));
        memcpy(before, ordinary, sizeof(ordinary));
        CHECK(!p4_hosted_rpc_encode(ordinary, sizeof(ordinary), &rpc,
                                    &written));
        CHECK(written == SIZE_MAX);
        CHECK(memcmp(ordinary, before, sizeof(ordinary)) == 0);
        CHECK(!p4_hosted_rpc_decode(response, response_size, &rpc));
    }
}

static void test_maximum_inventory_response(void)
{
    struct board_discovery_snapshot expected;
    struct p4_hosted_discovery_result result;
    uint8_t inventory[BOARD_DISCOVERY_WIRE_MAX];
    uint8_t response[P4_HOSTED_RPC_MAX_SIZE];
    size_t inventory_size, response_size;

    begin_case("maximum_inventory_response");
    fill_max_snapshot(&expected);
    CHECK(expected.layout.count == BOARD_LAYOUT_MAX_PARTITIONS);
    CHECK(board_discovery_validate(&expected));
    CHECK(board_discovery_encode(&expected, TEST_NONCE, inventory,
                                 sizeof(inventory), &inventory_size));
    CHECK(inventory_size == BOARD_DISCOVERY_WIRE_MAX);
    response_size = reference_response(TEST_UID, 0, inventory, inventory_size,
                                       response, sizeof(response));
    CHECK(response_size <= sizeof(response));
    CHECK(p4_hosted_discovery_response(response, response_size, TEST_UID,
                                       TEST_NONCE, &result));
    CHECK(result.status == 0);
    CHECK(snapshot_equal(&result.snapshot, &expected));
}

static void test_uint32_max_uid_nonce(void)
{
    struct p4_hosted_init init;
    struct p4_hosted_discovery_result result;
    uint8_t native[128], reference[128];
    uint8_t inventory[BOARD_DISCOVERY_WIRE_MAX];
    uint8_t response[P4_HOSTED_RPC_MAX_SIZE];
    size_t native_size = 0, reference_size, inventory_size, response_size;

    begin_case("uint32_max_uid_nonce");
    fill_valid_init(&init);
    CHECK(p4_hosted_discovery_request(&init, UINT32_MAX, UINT32_MAX,
                                     native, sizeof(native), &native_size));
    reference_size = reference_request(UINT32_MAX, UINT32_MAX, reference,
                                       sizeof(reference));
    CHECK(native_size == reference_size);
    CHECK(memcmp(native, reference, native_size) == 0);

    inventory_size = encode_valid_inventory(UINT32_MAX, inventory,
                                             sizeof(inventory));
    response_size = reference_response(UINT32_MAX, 0, inventory,
                                       inventory_size, response,
                                       sizeof(response));
    CHECK(p4_hosted_discovery_response(response, response_size, UINT32_MAX,
                                       UINT32_MAX, &result));
    CHECK(result.status == 0);
    CHECK(result.snapshot.physical_bytes == UINT32_C(0x01000000));
    CHECK(result.snapshot.layout.count == 3u);
}

static void test_remote_error_responses(void)
{
    static const int32_t statuses[] = { 1, -1, INT32_MIN, INT32_MAX };
    struct p4_hosted_discovery_result result;
    uint8_t response[P4_HOSTED_RPC_MAX_SIZE];
    size_t i, response_size;

    begin_case("remote_error_responses");
    for (i = 0; i < ARRAY_COUNT(statuses); ++i) {
        response_size = reference_response(TEST_UID, statuses[i], NULL, 0,
                                           response, sizeof(response));
        memset(&result, 0xa5, sizeof(result));
        CHECK(p4_hosted_discovery_response(response, response_size, TEST_UID,
                                           TEST_NONCE, &result));
        CHECK(result.status == statuses[i]);
        CHECK(snapshot_is_zero(&result.snapshot));
    }

    /* An explicitly present, empty bytes field is allowed for a remote error. */
    {
        uint8_t body[16];
        size_t body_size = make_body(1, -7, 1, NULL, 0, body, sizeof(body));
        response_size = raw_response(body, body_size, TEST_UID, response,
                                     sizeof(response));
        CHECK(p4_hosted_discovery_response(response, response_size, TEST_UID,
                                           TEST_NONCE, &result));
        CHECK(result.status == -7);
        CHECK(snapshot_is_zero(&result.snapshot));
    }

    /* An absent snapshot is also valid for a remote error. */
    {
        uint8_t body[16];
        size_t body_size = make_body(1, -9, 0, NULL, 0, body, sizeof(body));
        response_size = raw_response(body, body_size, TEST_UID, response,
                                     sizeof(response));
        CHECK(p4_hosted_discovery_response(response, response_size, TEST_UID,
                                           TEST_NONCE, &result));
        CHECK(result.status == -9);
        CHECK(snapshot_is_zero(&result.snapshot));
    }

    /* Error responses cannot publish even a valid inventory. */
    {
        uint8_t inventory[BOARD_DISCOVERY_WIRE_MAX];
        uint8_t body[BOARD_DISCOVERY_WIRE_MAX + 32u];
        size_t inventory_size = encode_valid_inventory(TEST_NONCE, inventory,
                                                       sizeof(inventory));
        size_t body_size = make_body(1, -1, 1, inventory, inventory_size,
                                     body, sizeof(body));
        response_size = raw_response(body, body_size, TEST_UID, response,
                                     sizeof(response));
        expect_response_refusal(response, response_size, TEST_UID, TEST_NONCE);
    }
    {
        uint8_t body[] = { 8u, 1u, 18u, 1u, 0xffu };
        response_size = raw_response(body, sizeof(body), TEST_UID, response,
                                     sizeof(response));
        expect_response_refusal(response, response_size, TEST_UID, TEST_NONCE);
    }
}

static void test_missing_and_empty_success_inventory(void)
{
    uint8_t body[16], response[P4_HOSTED_RPC_MAX_SIZE];
    size_t body_size, response_size;

    begin_case("missing_and_empty_success_inventory");
    body_size = make_body(1, 0, 0, NULL, 0, body, sizeof(body));
    response_size = raw_response(body, body_size, TEST_UID, response,
                                 sizeof(response));
    expect_response_refusal(response, response_size, TEST_UID, TEST_NONCE);

    body_size = make_body(1, 0, 1, NULL, 0, body, sizeof(body));
    response_size = raw_response(body, body_size, TEST_UID, response,
                                 sizeof(response));
    expect_response_refusal(response, response_size, TEST_UID, TEST_NONCE);

    /* Status defaults to zero when omitted, so it still requires inventory. */
    body_size = 0;
    response_size = raw_response(body, body_size, TEST_UID, response,
                                 sizeof(response));
    expect_response_refusal(response, response_size, TEST_UID, TEST_NONCE);
}

static void test_rpc_envelope_rejections(void)
{
    uint8_t inventory[BOARD_DISCOVERY_WIRE_MAX];
    uint8_t body[BOARD_DISCOVERY_WIRE_MAX + 32u];
    uint8_t response[P4_HOSTED_RPC_MAX_SIZE + 1u];
    size_t inventory_size, body_size, response_size, offset;

    begin_case("rpc_envelope_rejections");
    inventory_size = encode_valid_inventory(TEST_NONCE, inventory,
                                             sizeof(inventory));
    body_size = make_body(1, 0, 1, inventory, inventory_size,
                          body, sizeof(body));

    response_size = wrap_payload(body, body_size, P4_HOSTED_RPC_TYPE_REQUEST,
        P4_HOSTED_DISCOVERY_REQUEST_ID, TEST_UID, 1,
        P4_HOSTED_DISCOVERY_REQUEST_ID, response, sizeof(response));
    expect_response_refusal(response, response_size, TEST_UID, TEST_NONCE);
    response_size = wrap_payload(body, body_size, P4_HOSTED_RPC_TYPE_EVENT,
        P4_HOSTED_DISCOVERY_RESPONSE_ID, TEST_UID, 1,
        P4_HOSTED_DISCOVERY_RESPONSE_ID, response, sizeof(response));
    expect_response_refusal(response, response_size, TEST_UID, TEST_NONCE);
    response_size = wrap_payload(body, body_size, P4_HOSTED_RPC_TYPE_RESPONSE,
        P4_HOSTED_DISCOVERY_RESPONSE_ID - 1u, TEST_UID, 1,
        P4_HOSTED_DISCOVERY_RESPONSE_ID - 1u, response, sizeof(response));
    expect_response_refusal(response, response_size, TEST_UID, TEST_NONCE);
    response_size = wrap_payload(body, body_size, P4_HOSTED_RPC_TYPE_RESPONSE,
        P4_HOSTED_DISCOVERY_RESPONSE_ID, TEST_UID, 1,
        P4_HOSTED_DISCOVERY_RESPONSE_ID - 1u, response, sizeof(response));
    expect_response_refusal(response, response_size, TEST_UID, TEST_NONCE);
    response_size = raw_response(body, body_size, TEST_UID, response,
                                 sizeof(response));
    expect_response_refusal(response, response_size, TEST_UID + 1u, TEST_NONCE);
    response_size = wrap_payload(body, body_size, P4_HOSTED_RPC_TYPE_RESPONSE,
        P4_HOSTED_DISCOVERY_RESPONSE_ID, 0, 1,
        P4_HOSTED_DISCOVERY_RESPONSE_ID, response, sizeof(response));
    expect_response_refusal(response, response_size, 0, TEST_NONCE);
    response_size = wrap_payload(body, body_size, P4_HOSTED_RPC_TYPE_RESPONSE,
        P4_HOSTED_DISCOVERY_RESPONSE_ID, 0, 0,
        P4_HOSTED_DISCOVERY_RESPONSE_ID, response, sizeof(response));
    expect_response_refusal(response, response_size, TEST_UID, TEST_NONCE);
    response_size = raw_response(body, body_size, TEST_UID, response,
                                 sizeof(response));
    expect_response_refusal(response, response_size, TEST_UID, 0);

    /* Duplicate envelope singletons, including the oneof payload, refuse. */
    offset = 0;
    append_key(response, sizeof(response), &offset, 1u, 0u);
    append_varint(response, sizeof(response), &offset, P4_HOSTED_RPC_TYPE_RESPONSE);
    append_key(response, sizeof(response), &offset, 1u, 0u);
    append_varint(response, sizeof(response), &offset, P4_HOSTED_RPC_TYPE_RESPONSE);
    append_key(response, sizeof(response), &offset, 2u, 0u);
    append_varint(response, sizeof(response), &offset,
                  P4_HOSTED_DISCOVERY_RESPONSE_ID);
    append_key(response, sizeof(response), &offset, 3u, 0u);
    append_varint(response, sizeof(response), &offset, TEST_UID);
    append_bytes_field(response, sizeof(response), &offset,
        P4_HOSTED_DISCOVERY_RESPONSE_ID, body, body_size);
    expect_response_refusal(response, offset, TEST_UID, TEST_NONCE);

    offset = 0;
    append_key(response, sizeof(response), &offset, 1u, 0u);
    append_varint(response, sizeof(response), &offset, P4_HOSTED_RPC_TYPE_RESPONSE);
    append_key(response, sizeof(response), &offset, 2u, 0u);
    append_varint(response, sizeof(response), &offset,
                  P4_HOSTED_DISCOVERY_RESPONSE_ID);
    append_key(response, sizeof(response), &offset, 2u, 0u);
    append_varint(response, sizeof(response), &offset,
                  P4_HOSTED_DISCOVERY_RESPONSE_ID);
    append_key(response, sizeof(response), &offset, 3u, 0u);
    append_varint(response, sizeof(response), &offset, TEST_UID);
    append_bytes_field(response, sizeof(response), &offset,
        P4_HOSTED_DISCOVERY_RESPONSE_ID, body, body_size);
    expect_response_refusal(response, offset, TEST_UID, TEST_NONCE);

    offset = 0;
    append_key(response, sizeof(response), &offset, 1u, 0u);
    append_varint(response, sizeof(response), &offset, P4_HOSTED_RPC_TYPE_RESPONSE);
    append_key(response, sizeof(response), &offset, 2u, 0u);
    append_varint(response, sizeof(response), &offset,
                  P4_HOSTED_DISCOVERY_RESPONSE_ID);
    append_key(response, sizeof(response), &offset, 3u, 0u);
    append_varint(response, sizeof(response), &offset, TEST_UID);
    append_key(response, sizeof(response), &offset, 3u, 0u);
    append_varint(response, sizeof(response), &offset, TEST_UID);
    append_bytes_field(response, sizeof(response), &offset,
        P4_HOSTED_DISCOVERY_RESPONSE_ID, body, body_size);
    expect_response_refusal(response, offset, TEST_UID, TEST_NONCE);

    offset = 0;
    append_key(response, sizeof(response), &offset, 1u, 0u);
    append_varint(response, sizeof(response), &offset, P4_HOSTED_RPC_TYPE_RESPONSE);
    append_key(response, sizeof(response), &offset, 2u, 0u);
    append_varint(response, sizeof(response), &offset,
                  P4_HOSTED_DISCOVERY_RESPONSE_ID);
    append_key(response, sizeof(response), &offset, 3u, 0u);
    append_varint(response, sizeof(response), &offset, TEST_UID);
    append_bytes_field(response, sizeof(response), &offset,
        P4_HOSTED_DISCOVERY_RESPONSE_ID, body, body_size);
    append_bytes_field(response, sizeof(response), &offset,
        P4_HOSTED_DISCOVERY_RESPONSE_ID, body, body_size);
    expect_response_refusal(response, offset, TEST_UID, TEST_NONCE);

    /* Wrong wire types for required Rpc metadata are rejected. */
    offset = 0;
    append_bytes_field(response, sizeof(response), &offset, 1u,
                       (const uint8_t *)"\x02", 1u);
    append_key(response, sizeof(response), &offset, 2u, 0u);
    append_varint(response, sizeof(response), &offset,
                  P4_HOSTED_DISCOVERY_RESPONSE_ID);
    append_key(response, sizeof(response), &offset, 3u, 0u);
    append_varint(response, sizeof(response), &offset, TEST_UID);
    append_bytes_field(response, sizeof(response), &offset,
        P4_HOSTED_DISCOVERY_RESPONSE_ID, body, body_size);
    expect_response_refusal(response, offset, TEST_UID, TEST_NONCE);

    offset = 0;
    append_key(response, sizeof(response), &offset, 1u, 0u);
    append_varint(response, sizeof(response), &offset, P4_HOSTED_RPC_TYPE_RESPONSE);
    append_bytes_field(response, sizeof(response), &offset, 2u,
                       (const uint8_t *)"\x9e\x26", 2u);
    append_key(response, sizeof(response), &offset, 3u, 0u);
    append_varint(response, sizeof(response), &offset, TEST_UID);
    append_bytes_field(response, sizeof(response), &offset,
        P4_HOSTED_DISCOVERY_RESPONSE_ID, body, body_size);
    expect_response_refusal(response, offset, TEST_UID, TEST_NONCE);

    offset = 0;
    append_key(response, sizeof(response), &offset, 1u, 0u);
    append_varint(response, sizeof(response), &offset, P4_HOSTED_RPC_TYPE_RESPONSE);
    append_key(response, sizeof(response), &offset, 2u, 0u);
    append_varint(response, sizeof(response), &offset,
                  P4_HOSTED_DISCOVERY_RESPONSE_ID);
    append_key(response, sizeof(response), &offset, 3u, 1u);
    append_raw(response, sizeof(response), &offset, inventory, 8u);
    append_bytes_field(response, sizeof(response), &offset,
        P4_HOSTED_DISCOVERY_RESPONSE_ID, body, body_size);
    expect_response_refusal(response, offset, TEST_UID, TEST_NONCE);

    CHECK(!p4_hosted_discovery_response(NULL, 1u, TEST_UID, TEST_NONCE,
                                        NULL));
    expect_response_refusal(response, P4_HOSTED_RPC_MAX_SIZE + 1u,
                            TEST_UID, TEST_NONCE);
}

static void test_body_field_rejections(void)
{
    uint8_t inventory[BOARD_DISCOVERY_WIRE_MAX];
    uint8_t body[BOARD_DISCOVERY_WIRE_MAX + 64u];
    uint8_t response[P4_HOSTED_RPC_MAX_SIZE];
    size_t inventory_size, body_size, response_size, offset;

    begin_case("body_field_rejections");
    inventory_size = encode_valid_inventory(TEST_NONCE, inventory,
                                             sizeof(inventory));

    body_size = make_body(1, 0, 1, inventory, inventory_size,
                          body, sizeof(body));
    append_key(body, sizeof(body), &body_size, 1u, 0u);
    append_varint(body, sizeof(body), &body_size, 0u);
    response_size = raw_response(body, body_size, TEST_UID, response,
                                 sizeof(response));
    expect_response_refusal(response, response_size, TEST_UID, TEST_NONCE);

    body_size = make_body(1, 0, 1, inventory, inventory_size,
                          body, sizeof(body));
    append_bytes_field(body, sizeof(body), &body_size, 2u,
                       inventory, inventory_size);
    response_size = raw_response(body, body_size, TEST_UID, response,
                                 sizeof(response));
    expect_response_refusal(response, response_size, TEST_UID, TEST_NONCE);

    offset = 0;
    append_bytes_field(body, sizeof(body), &offset, 1u,
                       (const uint8_t *)"\x00", 1u);
    append_bytes_field(body, sizeof(body), &offset, 2u,
                       inventory, inventory_size);
    response_size = raw_response(body, offset, TEST_UID, response,
                                 sizeof(response));
    expect_response_refusal(response, response_size, TEST_UID, TEST_NONCE);

    offset = 0;
    append_key(body, sizeof(body), &offset, 1u, 0u);
    append_varint(body, sizeof(body), &offset, 0u);
    append_key(body, sizeof(body), &offset, 2u, 0u);
    append_varint(body, sizeof(body), &offset, 0u);
    response_size = raw_response(body, offset, TEST_UID, response,
                                 sizeof(response));
    expect_response_refusal(response, response_size, TEST_UID, TEST_NONCE);

    /* A bounded oneof body is still rejected when it has duplicate singulars. */
    offset = 0;
    append_key(body, sizeof(body), &offset, 1u, 0u);
    append_varint(body, sizeof(body), &offset, 0u);
    append_bytes_field(body, sizeof(body), &offset, 2u,
                       inventory, inventory_size);
    append_bytes_field(body, sizeof(body), &offset, 2u,
                       inventory, inventory_size);
    response_size = raw_response(body, offset, TEST_UID, response,
                                 sizeof(response));
    expect_response_refusal(response, response_size, TEST_UID, TEST_NONCE);

    /* Wrong nonce and a noncanonical 32-bit negative int32 encoding refuse. */
    offset = 0;
    append_key(body, sizeof(body), &offset, 1u, 0u);
    append_varint(body, sizeof(body), &offset, UINT32_MAX);
    response_size = raw_response(body, offset, TEST_UID, response,
                                 sizeof(response));
    expect_response_refusal(response, response_size, TEST_UID, TEST_NONCE);

    offset = 0;
    append_key(body, sizeof(body), &offset, 1u, 0u);
    append_varint(body, sizeof(body), &offset,
                  UINT64_C(0x0000000100000000));
    response_size = raw_response(body, offset, TEST_UID, response,
                                 sizeof(response));
    expect_response_refusal(response, response_size, TEST_UID, TEST_NONCE);

    offset = 0;
    append_key(body, sizeof(body), &offset, 1u, 0u);
    append_varint(body, sizeof(body), &offset, UINT64_C(0xffffffff80000000));
    response_size = raw_response(body, offset, TEST_UID, response,
                                 sizeof(response));
    {
        struct p4_hosted_discovery_result result;
        CHECK(p4_hosted_discovery_response(response, response_size, TEST_UID,
                                           TEST_NONCE, &result));
        CHECK(result.status == INT32_MIN);
        CHECK(snapshot_is_zero(&result.snapshot));
    }
}

static void test_unknown_field_skipping_and_groups(void)
{
    static const unsigned wire_types[] = { 0u, 1u, 2u, 5u };
    uint8_t inventory[BOARD_DISCOVERY_WIRE_MAX];
    uint8_t body[BOARD_DISCOVERY_WIRE_MAX + 128u];
    uint8_t response[P4_HOSTED_RPC_MAX_SIZE];
    struct p4_hosted_discovery_result result;
    size_t inventory_size, body_size, response_size, i, offset;

    begin_case("unknown_field_skipping_and_groups");
    inventory_size = encode_valid_inventory(TEST_NONCE, inventory,
                                             sizeof(inventory));
    for (i = 0; i < ARRAY_COUNT(wire_types); ++i) {
        body_size = make_body(1, 0, 1, inventory, inventory_size,
                              body, sizeof(body));
        append_key(body, sizeof(body), &body_size, 100u,
                   wire_types[i]);
        if (wire_types[i] == 0u) {
            append_varint(body, sizeof(body), &body_size, UINT32_C(0x123456));
        } else if (wire_types[i] == 1u) {
            static const uint8_t fixed64[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
            append_raw(body, sizeof(body), &body_size, fixed64, sizeof(fixed64));
        } else if (wire_types[i] == 2u) {
            static const uint8_t blob[] = { 0xa1, 0xb2, 0xc3 };
            append_varint(body, sizeof(body), &body_size, sizeof(blob));
            append_raw(body, sizeof(body), &body_size, blob, sizeof(blob));
        } else {
            static const uint8_t fixed32[4] = { 9, 10, 11, 12 };
            append_raw(body, sizeof(body), &body_size, fixed32, sizeof(fixed32));
        }
        response_size = raw_response(body, body_size, TEST_UID, response,
                                     sizeof(response));
        CHECK(p4_hosted_discovery_response(response, response_size, TEST_UID,
                                           TEST_NONCE, &result));
        CHECK(result.status == 0);
    }

    /* Unknown top-level fields use the same supported wire types. */
    for (i = 0; i < ARRAY_COUNT(wire_types); ++i) {
        response_size = raw_response(body, body_size, TEST_UID, response,
                                     sizeof(response));
        offset = response_size;
        append_key(response, sizeof(response), &offset, 100u,
                   wire_types[i]);
        if (wire_types[i] == 0u) {
            append_varint(response, sizeof(response), &offset, 37u);
        } else if (wire_types[i] == 1u) {
            static const uint8_t fixed64[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
            append_raw(response, sizeof(response), &offset, fixed64,
                       sizeof(fixed64));
        } else if (wire_types[i] == 2u) {
            static const uint8_t blob[] = { 0x10, 0x20 };
            append_varint(response, sizeof(response), &offset, sizeof(blob));
            append_raw(response, sizeof(response), &offset, blob, sizeof(blob));
        } else {
            static const uint8_t fixed32[4] = { 0, 1, 2, 3 };
            append_raw(response, sizeof(response), &offset, fixed32,
                       sizeof(fixed32));
        }
        CHECK(p4_hosted_discovery_response(response, offset, TEST_UID,
                                           TEST_NONCE, &result));
    }

    body_size = make_body(1, 0, 1, inventory, inventory_size,
                          body, sizeof(body));
    append_key(body, sizeof(body), &body_size, 100u, 3u);
    append_key(body, sizeof(body), &body_size, 100u, 4u);
    response_size = raw_response(body, body_size, TEST_UID, response,
                                 sizeof(response));
    expect_response_refusal(response, response_size, TEST_UID, TEST_NONCE);

    body_size = make_body(1, 0, 1, inventory, inventory_size,
                          body, sizeof(body));
    response_size = raw_response(body, body_size, TEST_UID, response,
                                 sizeof(response));
    offset = response_size;
    append_key(response, sizeof(response), &offset, 100u, 3u);
    append_key(response, sizeof(response), &offset, 100u, 4u);
    expect_response_refusal(response, offset, TEST_UID, TEST_NONCE);

    /* Truncated or reserved unknown fields cannot run past their bounded message. */
    body_size = make_body(1, 0, 1, inventory, inventory_size,
                          body, sizeof(body));
    append_key(body, sizeof(body), &body_size, 100u, 0u);
    CHECK(body_size < sizeof(body));
    body[body_size++] = 0x80u;
    response_size = raw_response(body, body_size, TEST_UID, response,
                                 sizeof(response));
    expect_response_refusal(response, response_size, TEST_UID, TEST_NONCE);

    body_size = make_body(1, 0, 1, inventory, inventory_size,
                          body, sizeof(body));
    append_key(body, sizeof(body), &body_size, 100u, 6u);
    response_size = raw_response(body, body_size, TEST_UID, response,
                                 sizeof(response));
    expect_response_refusal(response, response_size, TEST_UID, TEST_NONCE);
}

static void test_snapshot_validation_and_bounds(void)
{
    struct mutation {
        const char *name;
        size_t offset;
        uint8_t value;
    };
    static const struct mutation mutations[] = {
        { "magic", 0u, 'X' },
        { "legacy_version", 4u, 1u },
        { "header_length", 5u, 31u },
        { "record_length", 6u, 31u },
        { "empty_count", 7u, 0u },
        { "too_many_records", 7u, BOARD_LAYOUT_MAX_PARTITIONS + 1u },
        { "unknown_running_role", 8u, BOARD_DISCOVERY_UNKNOWN_ROLE },
        { "invalid_running_state", 11u, 7u },
        { "unknown_security", 12u, 0x80u },
        { "security_not_known", 13u, 0x80u },
        { "reserved_header", 15u, 1u },
        { "physical_capacity", 19u, 0u },
        { "flash_capacity", 22u, 0u },
        { "mismatched_nonce", 24u, 0u },
        { "reserved_tail", 36u, 1u },
        { "zero_partition_size", BOARD_DISCOVERY_WIRE_HEADER + 6u, 0u },
        { "invalid_partition_flags", BOARD_DISCOVERY_WIRE_HEADER + 8u, 0x80u },
        { "empty_label", BOARD_DISCOVERY_WIRE_HEADER + 14u, 0u },
        { "record_reserved", BOARD_DISCOVERY_WIRE_HEADER + 15u, 1u }
    };
    uint8_t valid[BOARD_DISCOVERY_WIRE_MAX];
    uint8_t mutated[BOARD_DISCOVERY_WIRE_MAX + 1u];
    uint8_t body[BOARD_DISCOVERY_WIRE_MAX + 32u];
    uint8_t response[P4_HOSTED_RPC_MAX_SIZE];
    size_t valid_size, body_size, response_size, i;

    begin_case("snapshot_validation_and_bounds");
    valid_size = encode_valid_inventory(TEST_NONCE, valid, sizeof(valid));
    for (i = 0; i < ARRAY_COUNT(mutations); ++i) {
        CHECK(mutations[i].offset < valid_size);
        memcpy(mutated, valid, valid_size);
        mutated[mutations[i].offset] = mutations[i].value;
        body_size = make_body(1, 0, 1, mutated, valid_size,
                              body, sizeof(body));
        response_size = raw_response(body, body_size, TEST_UID, response,
                                     sizeof(response));
        expect_response_refusal(response, response_size, TEST_UID, TEST_NONCE);
    }

    memcpy(mutated, valid, valid_size);
    mutated[24] = (uint8_t)(OTHER_NONCE & 0xffu);
    mutated[25] = (uint8_t)((OTHER_NONCE >> 8) & 0xffu);
    mutated[26] = (uint8_t)((OTHER_NONCE >> 16) & 0xffu);
    mutated[27] = (uint8_t)((OTHER_NONCE >> 24) & 0xffu);
    body_size = make_body(1, 0, 1, mutated, valid_size,
                          body, sizeof(body));
    response_size = raw_response(body, body_size, TEST_UID, response,
                                 sizeof(response));
    expect_response_refusal(response, response_size, TEST_UID, TEST_NONCE);

    memcpy(mutated, valid, valid_size);
    memset(mutated + 24u, 0, 4u);
    body_size = make_body(1, 0, 1, mutated, valid_size,
                          body, sizeof(body));
    response_size = raw_response(body, body_size, TEST_UID, response,
                                 sizeof(response));
    expect_response_refusal(response, response_size, TEST_UID, TEST_NONCE);

    /* Duplicate inventory labels are structurally invalid despite valid bytes. */
    memcpy(mutated, valid, valid_size);
    mutated[7] = 2u;
    mutated[BOARD_DISCOVERY_WIRE_HEADER + BOARD_DISCOVERY_WIRE_RECORD + 14u] = 3u;
    memcpy(mutated + BOARD_DISCOVERY_WIRE_HEADER + BOARD_DISCOVERY_WIRE_RECORD + 16u,
           mutated + BOARD_DISCOVERY_WIRE_HEADER + 16u, 3u);
    memset(mutated + BOARD_DISCOVERY_WIRE_HEADER + BOARD_DISCOVERY_WIRE_RECORD + 19u, 0, 13u);
    body_size = make_body(1, 0, 1, mutated, valid_size -
                          BOARD_DISCOVERY_WIRE_RECORD,
                          body, sizeof(body));
    response_size = raw_response(body, body_size, TEST_UID, response,
                                 sizeof(response));
    expect_response_refusal(response, response_size, TEST_UID, TEST_NONCE);

    /* A truncated inventory is rejected even when all visible fields match. */
    body_size = make_body(1, 0, 1, valid, valid_size - 1u,
                          body, sizeof(body));
    response_size = raw_response(body, body_size, TEST_UID, response,
                                 sizeof(response));
    expect_response_refusal(response, response_size, TEST_UID, TEST_NONCE);

    memcpy(mutated, valid, valid_size);
    mutated[valid_size] = 0;
    body_size = make_body(1, 0, 1, mutated, valid_size + 1u,
                          body, sizeof(body));
    response_size = raw_response(body, body_size, TEST_UID, response,
                                 sizeof(response));
    expect_response_refusal(response, response_size, TEST_UID, TEST_NONCE);

    memset(mutated, 0, sizeof(mutated));
    body_size = make_body(1, 0, 1, mutated,
                          BOARD_DISCOVERY_WIRE_MAX + 1u,
                          body, sizeof(body));
    response_size = raw_response(body, body_size, TEST_UID, response,
                                 sizeof(response));
    expect_response_refusal(response, response_size, TEST_UID, TEST_NONCE);
}

static void test_response_alias_and_null_refusals(void)
{
    uint8_t inventory[BOARD_DISCOVERY_WIRE_MAX];
    uint8_t response[P4_HOSTED_RPC_MAX_SIZE];
    size_t inventory_size, response_size;

    begin_case("response_alias_and_null_refusals");
    inventory_size = encode_valid_inventory(TEST_NONCE, inventory,
                                             sizeof(inventory));
    response_size = reference_response(TEST_UID, 0, inventory, inventory_size,
                                       response, sizeof(response));
    CHECK(!p4_hosted_discovery_response(response, response_size, 0,
                                        TEST_NONCE,
                                        (struct p4_hosted_discovery_result *)
                                            (void *)inventory));
    CHECK(!p4_hosted_discovery_response(response, response_size, TEST_UID, 0,
                                        (struct p4_hosted_discovery_result *)
                                            (void *)inventory));
    CHECK(!p4_hosted_discovery_response(response, response_size, TEST_UID,
                                        TEST_NONCE, NULL));
    expect_response_refusal(NULL, 0u, TEST_UID, TEST_NONCE);

    /* The result object may not alias the response bytes. */
    {
        struct p4_hosted_discovery_result result;
        uint8_t before[sizeof(result)];
        CHECK(response_size <= sizeof(result));
        memset(&result, 0x69, sizeof(result));
        memcpy(&result, response, response_size);
        memcpy(before, &result, sizeof(result));
        CHECK(!p4_hosted_discovery_response((const uint8_t *)(const void *)&result,
            response_size, TEST_UID, TEST_NONCE, &result));
        CHECK(memcmp(&result, before, sizeof(result)) == 0);
    }
}

int main(int argc, char **argv)
{
    const char *tag = argc > 1 ? argv[1] : "host";

    test_request_matches_generated_schema();
    test_request_refusals_and_aliases();
    test_success_and_reference_response();
    test_maximum_inventory_response();
    test_uint32_max_uid_nonce();
    test_remote_error_responses();
    test_missing_and_empty_success_inventory();
    test_rpc_envelope_rejections();
    test_body_field_rejections();
    test_unknown_field_skipping_and_groups();
    test_snapshot_validation_and_bounds();
    test_response_alias_and_null_refusals();

    printf("%s PASS: %u checks, %u cases\n", tag, checks, cases);
    return 0;
}
