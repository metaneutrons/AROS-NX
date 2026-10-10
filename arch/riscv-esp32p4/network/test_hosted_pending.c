/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
/* Host-only model for one asynchronous Hosted TX submission and its later
 * response correlation. No RX callback, hardware or elapsed-time claim. */
#include "hosted_association.h"
#include "hosted_pending.h"
#include "hosted_ota.h"
#include "hosted_rpc.h"
#include "hosted_serial.h"
#include "hosted_wire.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_GENERATION UINT32_C(0x5bd1)
#define REPLACEMENT_GENERATION UINT32_C(0x5bd2)
#define TEST_POOL_LIMIT 20u
#define TEST_REQUEST_ID 282u
#define TEST_AP_INFO_REQUEST_ID 294u
#define TEST_DISCOVERY_REQUEST_ID 350u
#define TEST_DISCOVERY_RESPONSE_ID 606u
#define TEST_DISCOVERY_NONCE UINT32_C(0x12345678)
#define TEST_OTA_BEGIN_REQUEST_ID 272u
#define TEST_OTA_WRITE_REQUEST_ID 273u
#define TEST_OTA_END_REQUEST_ID 274u
#define TEST_CAPACITY P4_HOSTED_PENDING_BUFFER_MAX
#define TEST_FRAME_CAPACITY P4_HOSTED_FRAME_MAX

static unsigned long checks;

#define CHECK(expression) do { \
    ++checks; \
    if (!(expression)) { \
        fprintf(stderr, "check failed at %s:%d: %s\n", __FILE__, __LINE__, \
                #expression); \
        exit(1); \
    } \
} while (0)

struct fake_tx {
    struct p4_hosted_fifo *fifo;
    struct p4_hosted_request_ids *ids;
    uint32_t generation;
    uint32_t token;
    int token_result;
    int write_result;
    int session_result;
    int budget_result;
    unsigned budget_fail_at;
    unsigned token_reads;
    unsigned other_reads;
    unsigned write_calls;
    unsigned session_calls;
    unsigned budget_calls;
    unsigned yield_calls;
    unsigned bad_address;
    unsigned replace_epoch_on_write;
    unsigned replace_ids_on_write;
    struct p4_hosted_fifo replacement_fifo;
    struct p4_hosted_request_ids replacement_ids;
    struct p4_hosted_tx_completion completion_override;
    unsigned completion_overridden;
    struct p4_hosted_fifo_plan plan;
    uint8_t captured[TEST_FRAME_CAPACITY];
    size_t captured_size;
    unsigned captured_padding_zero;
};

struct fixture {
    struct p4_hosted_fifo fifo;
    struct p4_hosted_request_ids ids;
    struct p4_hosted_tx_ops ops;
    struct fake_tx fake;
    struct p4_hosted_init init;
    uint8_t request[P4_HOSTED_PENDING_MAX_REQUEST_SIZE];
    size_t request_size;
    uint8_t txbuffer[TEST_CAPACITY];
    struct p4_hosted_pending_result result;
};

static size_t encode_discovery_body(uint8_t version, uint32_t nonce,
                                    uint8_t body[8])
{
    size_t size = 0u;

    body[size++] = 8u; /* DiscoveryRequest.version, field 1. */
    body[size++] = version;
    body[size++] = 16u; /* DiscoveryRequest.nonce, field 2. */
    while (nonce >= 128u) {
        body[size++] = (uint8_t)((nonce & UINT32_C(0x7f)) |
                                 UINT32_C(0x80));
        nonce >>= 7;
    }
    body[size++] = (uint8_t)nonce;
    return size;
}

static size_t encode_board_discovery_request(uint32_t uid, uint8_t version,
                                             uint32_t nonce, uint8_t *bytes,
                                             size_t capacity)
{
    uint8_t body[8];
    size_t body_size, written = 0u;
    struct p4_hosted_rpc rpc = {0};

    body_size = encode_discovery_body(version, nonce, body);
    rpc.msg_type = P4_HOSTED_RPC_TYPE_REQUEST;
    rpc.msg_id = TEST_DISCOVERY_REQUEST_ID;
    rpc.uid = uid;
    rpc.payload = body;
    rpc.payload_size = body_size;
    if (!p4_hosted_rpc_encode_board_discovery(bytes, capacity, &rpc,
                                               &written))
        return 0u;
    return written;
}

static void set_supported_board_discovery_init(struct p4_hosted_init *init)
{
    /* The normal parsed C6 INIT facts, including the singleton version TLV. */
    static const uint8_t event[] = {
        P4_HOSTED_INIT_EVENT_TYPE, 9u,
        P4_HOSTED_INIT_TAG_CAPABILITIES, 1u,
        P4_HOSTED_INIT_CAP_WLAN_SDIO,
        P4_HOSTED_INIT_TAG_CHIP_ID, 1u,
        P4_HOSTED_INIT_CHIP_ESP32C6,
        P4_HOSTED_INIT_TAG_BOARD_DISCOVERY, 1u,
        P4_HOSTED_INIT_BOARD_DISCOVERY_VERSION_1
    };

    memset(init, 0xa7, sizeof *init);
    CHECK(p4_hosted_init_parse(event, sizeof event, init));
    CHECK(p4_hosted_init_has_board_discovery(init));
}

static size_t encode_request(uint32_t request_id, uint32_t uid,
                             uint8_t *bytes, size_t capacity)
{
    size_t written = 0u;
    struct p4_hosted_rpc rpc = {0};

    if (request_id == 282u)
        return p4_hosted_association_connect_request(uid, bytes, capacity,
                                                      &written) ? written : 0u;
    if (request_id == 283u)
        return p4_hosted_association_disconnect_request(uid, bytes, capacity,
                                                         &written) ? written : 0u;
    if (request_id == TEST_DISCOVERY_REQUEST_ID)
        return encode_board_discovery_request(uid,
            P4_HOSTED_INIT_BOARD_DISCOVERY_VERSION_1,
            TEST_DISCOVERY_NONCE, bytes, capacity);
    rpc.msg_type = P4_HOSTED_RPC_TYPE_REQUEST;
    rpc.msg_id = request_id;
    rpc.uid = uid;
    if (!p4_hosted_rpc_encode(bytes, capacity, &rpc, &written))
        return 0u;
    return written;
}

static int fake_read_word(void *context, uint32_t address, uint32_t *value)
{
    struct fake_tx *fake = context;
    if (address != P4_HOSTED_TX_TOKEN_ADDRESS || !value) {
        ++fake->other_reads;
        fake->bad_address = 1u;
        return 0;
    }
    ++fake->token_reads;
    *value = fake->token;
    return fake->token_result;
}

static int fake_write_fifo(void *context,
    const struct p4_hosted_fifo_plan *plan, const uint8_t *buffer,
    struct p4_hosted_tx_completion *completion)
{
    struct fake_tx *fake = context;
    uint32_t index;

    ++fake->write_calls;
    if (!plan || !buffer || !completion ||
        plan->logical_bytes > sizeof fake->captured ||
        plan->bus_bytes < plan->logical_bytes ||
        plan->bus_bytes > TEST_CAPACITY) {
        fake->bad_address = 1u;
        return 0;
    }
    fake->plan = *plan;
    fake->captured_size = plan->logical_bytes;
    memcpy(fake->captured, buffer, fake->captured_size);
    fake->captured_padding_zero = 1u;
    for (index = plan->logical_bytes; index < plan->bus_bytes; ++index)
        if (buffer[index] != 0u)
            fake->captured_padding_zero = 0u;

    if (fake->completion_overridden)
        *completion = fake->completion_override;
    else {
        completion->started = 1u;
        completion->success = 1u;
        completion->completed_bytes = plan->bus_bytes;
    }

    if (fake->replace_epoch_on_write) {
        *fake->fifo = fake->replacement_fifo;
        *fake->ids = fake->replacement_ids;
        fake->generation = REPLACEMENT_GENERATION;
    }
    else if (fake->replace_ids_on_write)
        *fake->ids = fake->replacement_ids;
    return fake->write_result;
}

static int fake_session_ok(void *context, uint32_t generation)
{
    struct fake_tx *fake = context;
    ++fake->session_calls;
    return fake->session_result == 1 && fake->generation == generation;
}

static int fake_budget_ok(void *context)
{
    struct fake_tx *fake = context;
    ++fake->budget_calls;
    if (fake->budget_fail_at != 0u &&
        fake->budget_calls >= fake->budget_fail_at)
        return 0;
    return fake->budget_result;
}

static void fake_yield(void *context)
{
    struct fake_tx *fake = context;
    ++fake->yield_calls;
}

static void fixture_init(struct fixture *fixture, uint32_t request_id,
                         unsigned checksum_enabled)
{
    uint32_t uid = 0u;

    memset(fixture, 0, sizeof *fixture);
    memset(fixture->txbuffer, 0xa5, sizeof fixture->txbuffer);
    memset(&fixture->result, 0xc7, sizeof fixture->result);
    CHECK(p4_hosted_fifo_start(&fixture->fifo, TEST_GENERATION,
                               TEST_POOL_LIMIT));
    CHECK(p4_hosted_request_ids_init(&fixture->ids, TEST_GENERATION));
    CHECK(p4_hosted_request_ids_reserve(&fixture->ids, TEST_GENERATION,
                                        request_id, &uid));
    fixture->request_size = encode_request(request_id, uid,
        fixture->request, sizeof fixture->request);
    CHECK(fixture->request_size != 0u);

    fixture->fake.fifo = &fixture->fifo;
    fixture->fake.ids = &fixture->ids;
    fixture->fake.generation = TEST_GENERATION;
    fixture->fake.token = UINT32_C(1) << 16;
    fixture->fake.token_result = 1;
    fixture->fake.write_result = 1;
    fixture->fake.session_result = 1;
    fixture->fake.budget_result = 1;
    fixture->ops.context = &fixture->fake;
    fixture->ops.read_word = fake_read_word;
    fixture->ops.write_fifo = fake_write_fifo;
    fixture->ops.session_ok = fake_session_ok;
    fixture->ops.budget_ok = fake_budget_ok;
    fixture->ops.yield = fake_yield;
    (void)checksum_enabled;
}

static uint32_t fixture_uid(const struct fixture *fixture)
{
    return fixture->ids.active_uid;
}

static int submit(struct fixture *fixture, uint32_t generation,
                  const struct p4_hosted_tx_ops *ops,
                  unsigned checksum_enabled, const uint8_t *request,
                  size_t request_size, uint8_t *buffer, size_t capacity)
{
    return p4_hosted_pending_submit(&fixture->fifo, &fixture->ids,
        generation, ops, checksum_enabled, request, request_size, buffer,
        capacity, &fixture->result);
}

static int submit_board_discovery(struct fixture *fixture,
    uint32_t generation, const struct p4_hosted_tx_ops *ops,
    unsigned checksum_enabled, const uint8_t *request, size_t request_size,
    uint8_t *buffer, size_t capacity, const struct p4_hosted_init *init,
    uint32_t nonce)
{
    return p4_hosted_pending_submit_board_discovery(&fixture->fifo,
        &fixture->ids, generation, ops, checksum_enabled, request,
        request_size, buffer, capacity, &fixture->result, init, nonce);
}

static int encode_ota_request(uint32_t request_id, uint32_t uid,
    const uint8_t *data, size_t data_size, uint8_t *bytes, size_t capacity,
    size_t *written)
{
    if (request_id == TEST_OTA_BEGIN_REQUEST_ID)
        return p4_hosted_ota_begin_request(uid, bytes, capacity, written);
    if (request_id == TEST_OTA_WRITE_REQUEST_ID)
        return p4_hosted_ota_write_request(uid, data, data_size, bytes,
                                           capacity, written);
    if (request_id == TEST_OTA_END_REQUEST_ID)
        return p4_hosted_ota_end_request(uid, bytes, capacity, written);
    return 0;
}

static void fixture_init_ota(struct fixture *fixture, uint32_t request_id,
    unsigned checksum_enabled, const uint8_t *data, size_t data_size)
{
    size_t written = 0u;

    fixture_init(fixture, request_id, checksum_enabled);
    CHECK(encode_ota_request(request_id, fixture_uid(fixture), data,
        data_size, fixture->request, sizeof fixture->request, &written));
    CHECK(written != 0u && written <= sizeof fixture->request);
    fixture->request_size = written;
}

static int submit_ota(struct fixture *fixture, uint32_t generation,
    const struct p4_hosted_tx_ops *ops, unsigned checksum_enabled,
    const uint8_t *request, size_t request_size, uint8_t *buffer,
    size_t capacity)
{
    return p4_hosted_pending_submit_ota(&fixture->fifo, &fixture->ids,
        generation, ops, checksum_enabled, request, request_size, buffer,
        capacity, &fixture->result);
}

static size_t encode_raw_rpc(uint32_t msg_type, uint32_t request_id,
    uint32_t uid, const uint8_t *body, size_t body_size, uint8_t *bytes,
    size_t capacity)
{
    size_t written = 0u;
    struct p4_hosted_rpc rpc = {0};

    rpc.msg_type = msg_type;
    rpc.msg_id = request_id;
    rpc.uid = uid;
    rpc.payload = body;
    rpc.payload_size = body_size;
    if (!p4_hosted_rpc_encode(bytes, capacity, &rpc, &written))
        return 0u;
    return written;
}

static void verify_request_frame(const struct fake_tx *fake,
                                 uint32_t uid, uint32_t request_id,
                                 unsigned checksum_enabled)
{
    struct p4_hosted_frame frame;
    struct hosted_serial_message serial;
    struct p4_hosted_rpc rpc;

    CHECK(fake->captured_size == fake->plan.logical_bytes);
    CHECK(p4_hosted_decode(fake->captured, fake->captured_size,
                           checksum_enabled, &frame));
    CHECK(frame.interface_type == P4_HOSTED_SERIAL &&
          frame.interface_number == 0u && frame.flags == 0u &&
          frame.packet_type == 0u && frame.throttle == 0u &&
          frame.sequence == 0u);
    CHECK(hosted_serial_decode(frame.payload, frame.payload_size, &serial));
    CHECK(serial.endpoint == HOSTED_SERIAL_ENDPOINT_RPCRSP);
    CHECK(p4_hosted_rpc_decode(serial.protobuf, serial.protobuf_size, &rpc));
    CHECK(rpc.msg_type == P4_HOSTED_RPC_TYPE_REQUEST &&
          rpc.msg_id == request_id && rpc.uid == uid &&
          rpc.payload_size == 0u);
}

static void verify_board_discovery_request_frame(const struct fake_tx *fake,
    uint32_t uid, uint32_t nonce, unsigned checksum_enabled)
{
    static const uint8_t sample_body[] = {
        0x08u, 0x01u, 0x10u, 0xf8u, 0xacu, 0xd1u, 0x91u, 0x01u
    };
    static const uint8_t maximum_body[] = {
        0x08u, 0x01u, 0x10u, 0xffu, 0xffu, 0xffu, 0xffu, 0x0fu
    };
    const uint8_t *expected_body;
    struct p4_hosted_frame frame;
    struct hosted_serial_message serial;
    struct p4_hosted_rpc rpc;
    uint8_t generated_body[8];
    size_t expected_size;

    CHECK(fake->captured_size == fake->plan.logical_bytes);
    CHECK(p4_hosted_decode(fake->captured, fake->captured_size,
                           checksum_enabled, &frame));
    CHECK(frame.interface_type == P4_HOSTED_SERIAL &&
          frame.interface_number == 0u && frame.flags == 0u &&
          frame.packet_type == 0u && frame.throttle == 0u &&
          frame.sequence == 0u);
    CHECK(hosted_serial_decode(frame.payload, frame.payload_size, &serial));
    CHECK(serial.endpoint == HOSTED_SERIAL_ENDPOINT_RPCRSP);
    CHECK(p4_hosted_rpc_decode_board_discovery(serial.protobuf,
        serial.protobuf_size, &rpc));
    CHECK(rpc.msg_type == P4_HOSTED_RPC_TYPE_REQUEST &&
          rpc.msg_id == TEST_DISCOVERY_REQUEST_ID && rpc.uid == uid);

    expected_body = nonce == UINT32_MAX ? maximum_body : sample_body;
    expected_size = nonce == UINT32_MAX ? sizeof maximum_body
                                         : sizeof sample_body;
    CHECK(nonce == UINT32_MAX || nonce == TEST_DISCOVERY_NONCE);
    CHECK(rpc.payload_size == expected_size && expected_size == 8u);
    CHECK(memcmp(rpc.payload, expected_body, expected_size) == 0);
    CHECK(encode_discovery_body(P4_HOSTED_INIT_BOARD_DISCOVERY_VERSION_1,
        nonce, generated_body) == expected_size);
    CHECK(memcmp(rpc.payload, generated_body, expected_size) == 0);
}

static void verify_ota_request_frame(const struct fake_tx *fake,
    const uint8_t *request, size_t request_size, uint32_t uid,
    uint32_t request_id, const uint8_t *data, size_t data_size,
    unsigned checksum_enabled)
{
    struct p4_hosted_frame frame;
    struct hosted_serial_message serial;
    struct p4_hosted_rpc rpc;
    uint8_t expected_body[3u + P4_HOSTED_OTA_MAX_WRITE_SIZE];
    size_t body_prefix;

    CHECK(fake->captured_size == fake->plan.logical_bytes);
    CHECK(p4_hosted_decode(fake->captured, fake->captured_size,
                           checksum_enabled, &frame));
    CHECK(frame.interface_type == P4_HOSTED_SERIAL &&
          frame.interface_number == 0u && frame.flags == 0u &&
          frame.packet_type == 0u && frame.throttle == 0u &&
          frame.sequence == 0u);
    CHECK(hosted_serial_decode(frame.payload, frame.payload_size, &serial));
    CHECK(serial.endpoint == HOSTED_SERIAL_ENDPOINT_RPCRSP &&
          serial.protobuf_size == request_size &&
          memcmp(serial.protobuf, request, request_size) == 0);
    CHECK(p4_hosted_rpc_decode(serial.protobuf, serial.protobuf_size, &rpc));
    CHECK(rpc.msg_type == P4_HOSTED_RPC_TYPE_REQUEST &&
          rpc.msg_id == request_id && rpc.uid == uid);

    if (request_id != TEST_OTA_WRITE_REQUEST_ID) {
        CHECK(data_size == 0u && rpc.payload_size == 0u);
        return;
    }

    CHECK(data != NULL && data_size >= 1u &&
          data_size <= P4_HOSTED_OTA_MAX_WRITE_SIZE);
    expected_body[0] = 0x0au;
    if (data_size < 128u) {
        expected_body[1] = (uint8_t)data_size;
        body_prefix = 2u;
    } else {
        expected_body[1] = (uint8_t)((data_size & 0x7fu) | 0x80u);
        expected_body[2] = (uint8_t)(data_size >> 7);
        body_prefix = 3u;
    }
    memcpy(expected_body + body_prefix, data, data_size);
    CHECK(rpc.payload_size == body_prefix + data_size &&
          memcmp(rpc.payload, expected_body,
                 body_prefix + data_size) == 0);
}

static void test_submit_encodes_one_frame_and_leaves_identity_pending(
    unsigned checksum_enabled)
{
    struct fixture fixture;
    uint32_t uid;

    fixture_init(&fixture, TEST_REQUEST_ID, checksum_enabled);
    uid = fixture_uid(&fixture);
    CHECK(uid == 1u && fixture.ids.active_request_id == TEST_REQUEST_ID);
    CHECK(submit(&fixture, TEST_GENERATION, &fixture.ops, checksum_enabled,
        fixture.request, fixture.request_size, fixture.txbuffer,
        sizeof fixture.txbuffer));
    CHECK(fixture.result.error == P4_HOSTED_PENDING_ERROR_NONE);
    CHECK(fixture.result.submitted == 1u && fixture.result.uid == uid &&
          fixture.result.request_id == TEST_REQUEST_ID);
    CHECK(fixture.result.request_size == fixture.request_size &&
          fixture.result.serial_size > fixture.request_size &&
          fixture.result.wire_size == fixture.result.tx.plan.logical_bytes);
    CHECK(fixture.ids.generation == TEST_GENERATION &&
          fixture.ids.active_uid == uid &&
          fixture.ids.active_request_id == TEST_REQUEST_ID &&
          fixture.ids.next_uid == 2u && fixture.ids.retired == 0u);
    CHECK(fixture.fifo.tx_sent == 1u && fixture.fifo.rx_consumed == 0u &&
          fixture.fifo.pool_limit == TEST_POOL_LIMIT &&
          !fixture.fifo.busy && !fixture.fifo.poisoned);
    CHECK(fixture.fake.token_reads == 1u && fixture.fake.other_reads == 0u &&
          fixture.fake.write_calls == 1u && fixture.fake.yield_calls == 0u &&
          !fixture.fake.bad_address && fixture.fake.captured_padding_zero);
    verify_request_frame(&fixture.fake, uid, TEST_REQUEST_ID,
                         checksum_enabled);
}

static void init_response_item(struct p4_hosted_inbox_item *item,
                               uint32_t request_id, uint32_t uid,
                               const uint8_t *payload, size_t payload_size)
{
    memset(item, 0, sizeof *item);
    item->msg_type = P4_HOSTED_RPC_TYPE_RESPONSE;
    item->msg_id = request_id + 256u;
    item->uid = uid;
    item->payload_size = payload_size;
    if (payload_size != 0u)
        memcpy(item->payload, payload, payload_size);
}

static void test_events_unmatched_and_remote_error_only_correlate(void)
{
    /* Protobuf int32(-7): field 1, sign-extended negative varint. */
    static const uint8_t error_body[] = {
        0x08u, 0xf9u, 0xffu, 0xffu, 0xffu, 0xffu,
        0xffu, 0xffu, 0xffu, 0xffu, 0x01u
    };
    struct fixture fixture;
    struct p4_hosted_request_ids ids_before;
    struct p4_hosted_inbox_item item;
    struct p4_hosted_rpc rpc = {0};
    int32_t remote_status = 0;
    uint32_t uid;

    fixture_init(&fixture, TEST_REQUEST_ID, 0u);
    uid = fixture_uid(&fixture);
    CHECK(submit(&fixture, TEST_GENERATION, &fixture.ops, 0u,
        fixture.request, fixture.request_size, fixture.txbuffer,
        sizeof fixture.txbuffer));
    ids_before = fixture.ids;

    memset(&item, 0, sizeof item);
    item.msg_type = P4_HOSTED_RPC_TYPE_EVENT;
    item.msg_id = 775u;
    item.uid = 0u;
    CHECK(p4_hosted_pending_correlate(&fixture.ids, TEST_GENERATION,
        &item) == P4_HOSTED_PENDING_CORRELATION_EVENT);
    CHECK(memcmp(&ids_before, &fixture.ids, sizeof ids_before) == 0);

    init_response_item(&item, TEST_REQUEST_ID, uid + 1u,
                       error_body, sizeof error_body);
    CHECK(p4_hosted_pending_correlate(&fixture.ids, TEST_GENERATION,
        &item) == P4_HOSTED_PENDING_CORRELATION_UNMATCHED);
    CHECK(memcmp(&ids_before, &fixture.ids, sizeof ids_before) == 0);

    init_response_item(&item, TEST_REQUEST_ID + 1u, uid,
                       error_body, sizeof error_body);
    CHECK(p4_hosted_pending_correlate(&fixture.ids, TEST_GENERATION,
        &item) == P4_HOSTED_PENDING_CORRELATION_UNMATCHED);
    CHECK(memcmp(&ids_before, &fixture.ids, sizeof ids_before) == 0);

    rpc.msg_type = P4_HOSTED_RPC_TYPE_RESPONSE;
    rpc.msg_id = TEST_REQUEST_ID + 256u;
    rpc.uid = uid;
    rpc.payload = error_body;
    rpc.payload_size = sizeof error_body;
    CHECK(p4_hosted_association_status_response(&rpc, TEST_REQUEST_ID, uid,
                                                &remote_status));
    CHECK(remote_status == -7);

    init_response_item(&item, TEST_REQUEST_ID, uid,
                       error_body, sizeof error_body);
    CHECK(p4_hosted_pending_correlate(&fixture.ids, TEST_GENERATION,
        &item) == P4_HOSTED_PENDING_CORRELATION_MATCHED);
    CHECK(fixture.ids.active_uid == 0u &&
          fixture.ids.active_request_id == 0u &&
          fixture.ids.next_uid == 2u && fixture.ids.retired == 0u);
    /* Correlation is not acceptance of this negative remote status. */
    CHECK(remote_status < 0);
}

static void test_ap_info_request_uses_response_550_and_exact_uid(void)
{
    static const uint8_t not_connected_status[] = { 0x08u, 0x8fu, 0x60u };
    struct fixture fixture;
    struct p4_hosted_request_ids ids_before;
    struct p4_hosted_inbox_item item;
    uint32_t uid, next_uid;

    fixture_init(&fixture, TEST_AP_INFO_REQUEST_ID, 0u);
    uid = fixture_uid(&fixture);
    CHECK(uid == 1u &&
          fixture.ids.active_request_id == TEST_AP_INFO_REQUEST_ID);
    CHECK(submit(&fixture, TEST_GENERATION, &fixture.ops, 0u,
        fixture.request, fixture.request_size, fixture.txbuffer,
        sizeof fixture.txbuffer));
    CHECK(fixture.result.error == P4_HOSTED_PENDING_ERROR_NONE &&
          fixture.result.submitted == 1u && fixture.result.uid == uid &&
          fixture.result.request_id == TEST_AP_INFO_REQUEST_ID);
    verify_request_frame(&fixture.fake, uid, TEST_AP_INFO_REQUEST_ID, 0u);
    ids_before = fixture.ids;

    memset(&item, 0, sizeof item);
    item.msg_type = P4_HOSTED_RPC_TYPE_EVENT;
    item.msg_id = 775u;
    item.uid = 0u;
    CHECK(p4_hosted_pending_correlate(&fixture.ids, TEST_GENERATION,
        &item) == P4_HOSTED_PENDING_CORRELATION_EVENT);
    CHECK(memcmp(&ids_before, &fixture.ids, sizeof ids_before) == 0);

    init_response_item(&item, TEST_AP_INFO_REQUEST_ID, uid + 1u,
                       not_connected_status, sizeof not_connected_status);
    CHECK(item.msg_id == 550u &&
          p4_hosted_pending_correlate(&fixture.ids, TEST_GENERATION,
              &item) == P4_HOSTED_PENDING_CORRELATION_UNMATCHED);
    CHECK(memcmp(&ids_before, &fixture.ids, sizeof ids_before) == 0);

    init_response_item(&item, TEST_AP_INFO_REQUEST_ID + 1u, uid,
                       not_connected_status, sizeof not_connected_status);
    CHECK(item.msg_id == 551u &&
          p4_hosted_pending_correlate(&fixture.ids, TEST_GENERATION,
              &item) == P4_HOSTED_PENDING_CORRELATION_UNMATCHED);
    CHECK(memcmp(&ids_before, &fixture.ids, sizeof ids_before) == 0);

    init_response_item(&item, TEST_AP_INFO_REQUEST_ID, uid,
                       not_connected_status, sizeof not_connected_status);
    CHECK(item.msg_id == 550u &&
          p4_hosted_pending_correlate(&fixture.ids, TEST_GENERATION,
              &item) == P4_HOSTED_PENDING_CORRELATION_MATCHED);
    CHECK(fixture.ids.active_uid == 0u &&
          fixture.ids.active_request_id == 0u &&
          fixture.ids.next_uid == 2u && fixture.ids.retired == 0u);

    CHECK(p4_hosted_request_ids_reserve(&fixture.ids, TEST_GENERATION,
        TEST_AP_INFO_REQUEST_ID, &next_uid));
    CHECK(next_uid == 2u);
    ids_before = fixture.ids;
    init_response_item(&item, TEST_AP_INFO_REQUEST_ID, uid,
                       not_connected_status, sizeof not_connected_status);
    CHECK(p4_hosted_pending_correlate(&fixture.ids, TEST_GENERATION,
        &item) == P4_HOSTED_PENDING_CORRELATION_UNMATCHED);
    CHECK(memcmp(&ids_before, &fixture.ids, sizeof ids_before) == 0);

    /* A caller timeout closes the second query without reusing its UID. */
    CHECK(p4_hosted_request_ids_finish(&fixture.ids, TEST_GENERATION,
        next_uid, TEST_AP_INFO_REQUEST_ID));
    CHECK(fixture.ids.next_uid == 3u && fixture.ids.active_uid == 0u);
    CHECK(p4_hosted_request_ids_reserve(&fixture.ids, TEST_GENERATION,
        TEST_AP_INFO_REQUEST_ID, &next_uid));
    CHECK(next_uid == 3u);
}

static void test_cancel_consumes_uid_and_old_reply_is_unmatched(void)
{
    struct fixture fixture;
    struct p4_hosted_inbox_item old_response;
    struct p4_hosted_request_ids ids_before;
    uint32_t first_uid, second_uid;

    fixture_init(&fixture, TEST_REQUEST_ID, 0u);
    first_uid = fixture_uid(&fixture);
    CHECK(submit(&fixture, TEST_GENERATION, &fixture.ops, 0u,
        fixture.request, fixture.request_size, fixture.txbuffer,
        sizeof fixture.txbuffer));
    /* Caller timeout/cancellation closes only the active reservation. */
    CHECK(p4_hosted_request_ids_finish(&fixture.ids, TEST_GENERATION,
        first_uid, TEST_REQUEST_ID));
    CHECK(fixture.ids.next_uid == 2u && fixture.ids.active_uid == 0u);
    CHECK(p4_hosted_request_ids_reserve(&fixture.ids, TEST_GENERATION,
        TEST_REQUEST_ID, &second_uid));
    CHECK(second_uid == 2u);
    ids_before = fixture.ids;
    init_response_item(&old_response, TEST_REQUEST_ID, first_uid, NULL, 0u);
    CHECK(p4_hosted_pending_correlate(&fixture.ids, TEST_GENERATION,
        &old_response) == P4_HOSTED_PENDING_CORRELATION_UNMATCHED);
    CHECK(memcmp(&ids_before, &fixture.ids, sizeof ids_before) == 0);
    CHECK(fixture.ids.active_uid == second_uid &&
          fixture.ids.active_request_id == TEST_REQUEST_ID);
}

static void expect_preflight_unchanged(struct fixture *fixture,
    struct p4_hosted_fifo *fifo, struct p4_hosted_request_ids *ids,
    uint32_t generation, const struct p4_hosted_tx_ops *ops,
    unsigned checksum_enabled, const uint8_t *request, size_t request_size,
    uint8_t *buffer, size_t capacity)
{
    struct p4_hosted_fifo fifo_before;
    struct p4_hosted_request_ids ids_before;
    struct p4_hosted_pending_result result_before;
    unsigned token_reads = fixture->fake.token_reads;
    unsigned other_reads = fixture->fake.other_reads;
    unsigned writes = fixture->fake.write_calls;
    unsigned sessions = fixture->fake.session_calls;
    unsigned budgets = fixture->fake.budget_calls;
    unsigned yields = fixture->fake.yield_calls;
    uint8_t buffer_before[P4_HOSTED_PENDING_BUFFER_MAX + 1u];
    int accepted;

    CHECK(capacity <= sizeof buffer_before);
    memcpy(&fifo_before, fifo, sizeof fifo_before);
    memcpy(&ids_before, ids, sizeof ids_before);
    memcpy(&result_before, &fixture->result, sizeof result_before);
    memcpy(buffer_before, buffer, capacity);
    accepted = p4_hosted_pending_submit(fifo, ids, generation, ops,
        checksum_enabled, request, request_size, buffer, capacity,
        &fixture->result);
    CHECK(!accepted);
    CHECK(memcmp(&fifo_before, fifo, sizeof fifo_before) == 0);
    CHECK(memcmp(&ids_before, ids, sizeof ids_before) == 0);
    CHECK(memcmp(&result_before, &fixture->result,
                 sizeof result_before) == 0);
    CHECK(memcmp(buffer_before, buffer, capacity) == 0);
    CHECK(fixture->fake.token_reads == token_reads &&
          fixture->fake.other_reads == other_reads &&
          fixture->fake.write_calls == writes &&
          fixture->fake.session_calls == sessions &&
          fixture->fake.budget_calls == budgets &&
          fixture->fake.yield_calls == yields);
}

static void expect_board_discovery_preflight_unchanged(struct fixture *fixture,
    struct p4_hosted_fifo *fifo, struct p4_hosted_request_ids *ids,
    uint32_t generation, const struct p4_hosted_tx_ops *ops,
    unsigned checksum_enabled, const uint8_t *request, size_t request_size,
    uint8_t *buffer, size_t capacity, const struct p4_hosted_init *init,
    uint32_t nonce)
{
    struct p4_hosted_fifo fifo_before;
    struct p4_hosted_request_ids ids_before;
    struct p4_hosted_tx_ops ops_before;
    struct p4_hosted_pending_result result_before;
    struct p4_hosted_init init_before;
    uint8_t request_before[P4_HOSTED_PENDING_MAX_REQUEST_SIZE];
    uint8_t buffer_before[P4_HOSTED_PENDING_BUFFER_MAX];
    unsigned token_reads = fixture->fake.token_reads;
    unsigned other_reads = fixture->fake.other_reads;
    unsigned writes = fixture->fake.write_calls;
    unsigned sessions = fixture->fake.session_calls;
    unsigned budgets = fixture->fake.budget_calls;
    unsigned yields = fixture->fake.yield_calls;
    int accepted;

    CHECK(request_size <= sizeof request_before);
    CHECK(capacity <= sizeof buffer_before);
    memcpy(&fifo_before, fifo, sizeof fifo_before);
    memcpy(&ids_before, ids, sizeof ids_before);
    memcpy(&ops_before, ops, sizeof ops_before);
    memcpy(&result_before, &fixture->result, sizeof result_before);
    if (request_size != 0u)
        memcpy(request_before, request, request_size);
    memcpy(buffer_before, buffer, capacity);
    if (init)
        memcpy(&init_before, init, sizeof init_before);

    accepted = p4_hosted_pending_submit_board_discovery(fifo, ids,
        generation, ops, checksum_enabled, request, request_size, buffer,
        capacity, &fixture->result, init, nonce);
    CHECK(!accepted);
    CHECK(memcmp(&fifo_before, fifo, sizeof fifo_before) == 0);
    CHECK(memcmp(&ids_before, ids, sizeof ids_before) == 0);
    CHECK(memcmp(&ops_before, ops, sizeof ops_before) == 0);
    CHECK(memcmp(&result_before, &fixture->result,
                 sizeof result_before) == 0);
    if (request_size != 0u)
        CHECK(memcmp(request_before, request, request_size) == 0);
    CHECK(memcmp(buffer_before, buffer, capacity) == 0);
    if (init)
        CHECK(memcmp(&init_before, init, sizeof init_before) == 0);
    CHECK(fixture->fake.token_reads == token_reads &&
          fixture->fake.other_reads == other_reads &&
          fixture->fake.write_calls == writes &&
          fixture->fake.session_calls == sessions &&
          fixture->fake.budget_calls == budgets &&
          fixture->fake.yield_calls == yields);
}

static void test_board_discovery_init_aliases_each_pending_object(void)
{
    struct fixture fixture;
    void *objects[6];
    unsigned index;

    fixture_init(&fixture, TEST_DISCOVERY_REQUEST_ID, 0u);
    set_supported_board_discovery_init(&fixture.init);
    objects[0] = &fixture.fifo;
    objects[1] = &fixture.ids;
    objects[2] = &fixture.ops;
    objects[3] = fixture.request;
    objects[4] = fixture.txbuffer;
    objects[5] = &fixture.result;

    for (index = 0u; index < 6u; ++index)
        expect_board_discovery_preflight_unchanged(&fixture,
            &fixture.fifo, &fixture.ids, TEST_GENERATION, &fixture.ops,
            0u, fixture.request, fixture.request_size, fixture.txbuffer,
            sizeof fixture.txbuffer,
            (const struct p4_hosted_init *)objects[index],
            TEST_DISCOVERY_NONCE);
}

static void test_submit_preflight_refusals_are_transactional(void)
{
    struct fixture fixture;
    uint8_t mismatch[P4_HOSTED_PENDING_MAX_REQUEST_SIZE];
    uint8_t oversized[P4_HOSTED_PENDING_MAX_REQUEST_SIZE + 1u];
    uint8_t extended_buffer[P4_HOSTED_PENDING_BUFFER_MAX + 1u];
    struct p4_hosted_request_ids corrupt_ids;
    struct p4_hosted_fifo corrupt_fifo;
    struct p4_hosted_tx_ops missing_ops;
    size_t mismatch_size;

    fixture_init(&fixture, TEST_REQUEST_ID, 0u);
    mismatch_size = encode_request(TEST_REQUEST_ID, fixture_uid(&fixture) + 1u,
                                   mismatch, sizeof mismatch);
    CHECK(mismatch_size != 0u);
    expect_preflight_unchanged(&fixture, &fixture.fifo, &fixture.ids,
        TEST_GENERATION, &fixture.ops, 0u, mismatch, mismatch_size,
        fixture.txbuffer, sizeof fixture.txbuffer);

    mismatch_size = encode_request(TEST_REQUEST_ID + 1u,
        fixture_uid(&fixture), mismatch, sizeof mismatch);
    CHECK(mismatch_size != 0u);
    expect_preflight_unchanged(&fixture, &fixture.fifo, &fixture.ids,
        TEST_GENERATION, &fixture.ops, 0u, mismatch, mismatch_size,
        fixture.txbuffer, sizeof fixture.txbuffer);

    expect_preflight_unchanged(&fixture, &fixture.fifo, &fixture.ids,
        TEST_GENERATION + 1u, &fixture.ops, 0u, fixture.request,
        fixture.request_size, fixture.txbuffer, sizeof fixture.txbuffer);
    expect_preflight_unchanged(&fixture, &fixture.fifo, &fixture.ids,
        TEST_GENERATION, &fixture.ops, 2u, fixture.request,
        fixture.request_size, fixture.txbuffer, sizeof fixture.txbuffer);
    expect_preflight_unchanged(&fixture, &fixture.fifo, &fixture.ids,
        TEST_GENERATION, &fixture.ops, 0u, fixture.request, 0u,
        fixture.txbuffer, sizeof fixture.txbuffer);
    memset(mismatch, 0, sizeof mismatch);
    expect_preflight_unchanged(&fixture, &fixture.fifo, &fixture.ids,
        TEST_GENERATION, &fixture.ops, 0u, mismatch, sizeof mismatch,
        fixture.txbuffer, sizeof fixture.txbuffer);
    memset(oversized, 0x7a, sizeof oversized);
    expect_preflight_unchanged(&fixture, &fixture.fifo, &fixture.ids,
        TEST_GENERATION, &fixture.ops, 0u, oversized, sizeof oversized,
        fixture.txbuffer, sizeof fixture.txbuffer);
    expect_preflight_unchanged(&fixture, &fixture.fifo, &fixture.ids,
        TEST_GENERATION, &fixture.ops, 0u, fixture.request,
        fixture.request_size, fixture.txbuffer,
        P4_HOSTED_PENDING_BUFFER_MIN - 1u);
    expect_preflight_unchanged(&fixture, &fixture.fifo, &fixture.ids,
        TEST_GENERATION, &fixture.ops, 0u, fixture.request,
        fixture.request_size, extended_buffer, sizeof extended_buffer);

    /* Unsupported request 285 is well-formed and reservable by the generic
     * ID ledger, but is deliberately outside the transport allowlist. */
    {
        struct fixture unsupported;
        fixture_init(&unsupported, 285u, 0u);
        expect_preflight_unchanged(&unsupported, &unsupported.fifo,
            &unsupported.ids, TEST_GENERATION, &unsupported.ops, 0u,
            unsupported.request, unsupported.request_size,
            unsupported.txbuffer, sizeof unsupported.txbuffer);
    }

    corrupt_ids = fixture.ids;
    corrupt_ids.retired = 2u;
    expect_preflight_unchanged(&fixture, &fixture.fifo, &corrupt_ids,
        TEST_GENERATION, &fixture.ops, 0u, fixture.request,
        fixture.request_size, fixture.txbuffer, sizeof fixture.txbuffer);
    corrupt_fifo = fixture.fifo;
    corrupt_fifo.pool_limit = 0u;
    expect_preflight_unchanged(&fixture, &corrupt_fifo, &fixture.ids,
        TEST_GENERATION, &fixture.ops, 0u, fixture.request,
        fixture.request_size, fixture.txbuffer, sizeof fixture.txbuffer);
    missing_ops = fixture.ops;
    missing_ops.write_fifo = NULL;
    expect_preflight_unchanged(&fixture, &fixture.fifo, &fixture.ids,
        TEST_GENERATION, &missing_ops, 0u, fixture.request,
        fixture.request_size, fixture.txbuffer, sizeof fixture.txbuffer);

    expect_preflight_unchanged(&fixture, &fixture.fifo, &fixture.ids,
        TEST_GENERATION, &fixture.ops, 0u,
        (const uint8_t *)(uintptr_t)(UINTPTR_MAX - 3u), 8u,
        fixture.txbuffer, sizeof fixture.txbuffer);
    expect_preflight_unchanged(&fixture, &fixture.fifo, &fixture.ids,
        TEST_GENERATION, &fixture.ops, 0u,
        (const uint8_t *)(const void *)&fixture.fifo,
        sizeof fixture.fifo, fixture.txbuffer, sizeof fixture.txbuffer);
}

union pending_alias_slab {
    struct p4_hosted_fifo fifo;
    struct p4_hosted_request_ids ids;
    struct p4_hosted_tx_ops ops;
    struct p4_hosted_pending_result result;
    uint8_t bytes[P4_HOSTED_PENDING_BUFFER_MAX];
};

static void expect_pairwise_alias_refusal(struct fixture *fixture,
                                          unsigned first, unsigned second)
{
    struct p4_hosted_fifo fifo_before;
    struct p4_hosted_request_ids ids_before;
    struct p4_hosted_tx_ops ops_before;
    struct p4_hosted_pending_result result_before;
    uint8_t request_before[P4_HOSTED_PENDING_MAX_REQUEST_SIZE];
    uint8_t txbuffer_before[TEST_CAPACITY];
    uint8_t slab_before[P4_HOSTED_PENDING_BUFFER_MAX];
    union pending_alias_slab slab;
    void *objects[6];
    unsigned token_reads = fixture->fake.token_reads;
    unsigned other_reads = fixture->fake.other_reads;
    unsigned writes = fixture->fake.write_calls;

    CHECK(first < second && second < 6u);
    objects[0] = &fixture->fifo;
    objects[1] = &fixture->ids;
    objects[2] = &fixture->ops;
    objects[3] = fixture->request;
    objects[4] = fixture->txbuffer;
    objects[5] = &fixture->result;
    memset(&slab, 0x6du, sizeof slab);
    objects[first] = slab.bytes;
    objects[second] = slab.bytes;

    memcpy(&fifo_before, &fixture->fifo, sizeof fifo_before);
    memcpy(&ids_before, &fixture->ids, sizeof ids_before);
    memcpy(&ops_before, &fixture->ops, sizeof ops_before);
    memcpy(&result_before, &fixture->result, sizeof result_before);
    memcpy(request_before, fixture->request, sizeof request_before);
    memcpy(txbuffer_before, fixture->txbuffer, sizeof txbuffer_before);
    memcpy(slab_before, slab.bytes, sizeof slab_before);

    CHECK(!p4_hosted_pending_submit(
        (struct p4_hosted_fifo *)objects[0],
        (struct p4_hosted_request_ids *)objects[1], TEST_GENERATION,
        (const struct p4_hosted_tx_ops *)objects[2], 0u,
        (const uint8_t *)objects[3], fixture->request_size,
        (uint8_t *)objects[4], TEST_CAPACITY,
        (struct p4_hosted_pending_result *)objects[5]));

    CHECK(memcmp(&fifo_before, &fixture->fifo, sizeof fifo_before) == 0);
    CHECK(memcmp(&ids_before, &fixture->ids, sizeof ids_before) == 0);
    CHECK(memcmp(&ops_before, &fixture->ops, sizeof ops_before) == 0);
    CHECK(memcmp(&result_before, &fixture->result, sizeof result_before) == 0);
    CHECK(memcmp(request_before, fixture->request, sizeof request_before) == 0);
    CHECK(memcmp(txbuffer_before, fixture->txbuffer,
                 sizeof txbuffer_before) == 0);
    CHECK(memcmp(slab_before, slab.bytes, sizeof slab_before) == 0);
    CHECK(fixture->fake.token_reads == token_reads &&
          fixture->fake.other_reads == other_reads &&
          fixture->fake.write_calls == writes);
}

static void test_every_pending_object_pair_alias_refuses(void)
{
    struct fixture fixture;
    unsigned first, second;

    fixture_init(&fixture, TEST_REQUEST_ID, 0u);
    for (first = 0u; first < 6u; ++first)
        for (second = first + 1u; second < 6u; ++second)
            expect_pairwise_alias_refusal(&fixture, first, second);
}

static void test_failed_submission_consumes_or_retires_as_needed(void)
{
    struct fixture fixture;
    uint32_t uid;

    fixture_init(&fixture, TEST_REQUEST_ID, 0u);
    uid = fixture_uid(&fixture);
    fixture.fake.token_result = 0;
    CHECK(!submit(&fixture, TEST_GENERATION, &fixture.ops, 0u,
        fixture.request, fixture.request_size, fixture.txbuffer,
        sizeof fixture.txbuffer));
    CHECK(fixture.result.error == P4_HOSTED_PENDING_ERROR_TX);
    CHECK(fixture.result.tx.error == P4_HOSTED_TX_ERROR_TOKEN_READ);
    CHECK(fixture.result.submitted == 0u && fixture.fake.token_reads == 1u &&
          fixture.fake.write_calls == 0u);
    CHECK(!fixture.fifo.poisoned && fixture.ids.retired == 0u);
    CHECK(fixture.ids.next_uid == uid + 1u && fixture.ids.active_uid == 0u &&
          fixture.ids.active_request_id == 0u);

    fixture_init(&fixture, TEST_REQUEST_ID, 0u);
    uid = fixture_uid(&fixture);
    fixture.fake.budget_result = 0;
    CHECK(!submit(&fixture, TEST_GENERATION, &fixture.ops, 0u,
        fixture.request, fixture.request_size, fixture.txbuffer,
        sizeof fixture.txbuffer));
    CHECK(fixture.result.error == P4_HOSTED_PENDING_ERROR_TX &&
          fixture.result.tx.error == P4_HOSTED_TX_ERROR_BUDGET_EXPIRED);
    CHECK(fixture.fake.budget_calls == 1u && fixture.fake.token_reads == 0u &&
          fixture.fake.write_calls == 0u && !fixture.fifo.poisoned);
    CHECK(fixture.ids.next_uid == uid + 1u && fixture.ids.active_uid == 0u &&
          fixture.ids.retired == 0u);

    fixture_init(&fixture, TEST_REQUEST_ID, 0u);
    fixture.fake.session_result = 0;
    CHECK(!submit(&fixture, TEST_GENERATION, &fixture.ops, 0u,
        fixture.request, fixture.request_size, fixture.txbuffer,
        sizeof fixture.txbuffer));
    CHECK(fixture.result.error == P4_HOSTED_PENDING_ERROR_SESSION &&
          fixture.result.tx.error == P4_HOSTED_TX_ERROR_SESSION_LOST);
    CHECK(fixture.fake.token_reads == 0u && fixture.fake.write_calls == 0u &&
          fixture.fifo.poisoned && fixture.ids.retired == 1u &&
          fixture.ids.next_uid == 2u);

    fixture_init(&fixture, TEST_REQUEST_ID, 0u);
    uid = fixture_uid(&fixture);
    fixture.fake.write_result = 0;
    fixture.fake.completion_overridden = 1u;
    fixture.fake.completion_override.started = 1u;
    fixture.fake.completion_override.success = 0u;
    fixture.fake.completion_override.completed_bytes = 0u;
    CHECK(!submit(&fixture, TEST_GENERATION, &fixture.ops, 0u,
        fixture.request, fixture.request_size, fixture.txbuffer,
        sizeof fixture.txbuffer));
    CHECK(fixture.result.error == P4_HOSTED_PENDING_ERROR_SESSION);
    CHECK(fixture.result.tx.error == P4_HOSTED_TX_ERROR_WRITE_AMBIGUOUS);
    CHECK(fixture.fake.write_calls == 1u && fixture.fifo.poisoned &&
          !fixture.fifo.busy);
    CHECK(fixture.ids.retired == 1u && fixture.ids.active_uid == 0u &&
          fixture.ids.active_request_id == 0u &&
          fixture.ids.next_uid == uid + 1u);

    fixture_init(&fixture, TEST_REQUEST_ID, 0u);
    uid = fixture_uid(&fixture);
    /* TX checks the supplied aggregate deadline before/after the token read,
     * once more before writing, and once after the authoritative callback. */
    fixture.fake.budget_fail_at = 4u;
    CHECK(!submit(&fixture, TEST_GENERATION, &fixture.ops, 0u,
        fixture.request, fixture.request_size, fixture.txbuffer,
        sizeof fixture.txbuffer));
    CHECK(fixture.result.error == P4_HOSTED_PENDING_ERROR_SESSION &&
          fixture.result.tx.error == P4_HOSTED_TX_ERROR_BUDGET_EXPIRED);
    CHECK(fixture.fake.budget_calls == 4u && fixture.fake.write_calls == 1u &&
          fixture.result.tx.write_called == 1u && fixture.fifo.poisoned &&
          fixture.fifo.tx_sent == 0u);
    CHECK(fixture.ids.retired == 1u && fixture.ids.next_uid == uid + 1u &&
          fixture.ids.active_uid == 0u);
}

static void test_replacement_epoch_is_never_mutated_by_stale_tx(void)
{
    struct fixture fixture;
    struct p4_hosted_fifo replacement_fifo, fifo_before;
    struct p4_hosted_request_ids replacement_ids, ids_before;
    uint32_t replacement_uid = 0u;

    fixture_init(&fixture, TEST_REQUEST_ID, 0u);
    memset(&replacement_fifo, 0, sizeof replacement_fifo);
    memset(&replacement_ids, 0, sizeof replacement_ids);
    CHECK(p4_hosted_fifo_start(&replacement_fifo, REPLACEMENT_GENERATION,
                               TEST_POOL_LIMIT));
    replacement_fifo.tx_sent = 7u;
    CHECK(p4_hosted_request_ids_init(&replacement_ids,
                                     REPLACEMENT_GENERATION));
    CHECK(p4_hosted_request_ids_reserve(&replacement_ids,
        REPLACEMENT_GENERATION, 283u, &replacement_uid));
    CHECK(replacement_uid == 1u);
    fixture.fake.replacement_fifo = replacement_fifo;
    fixture.fake.replacement_ids = replacement_ids;
    fixture.fake.replace_epoch_on_write = 1u;

    CHECK(!submit(&fixture, TEST_GENERATION, &fixture.ops, 0u,
        fixture.request, fixture.request_size, fixture.txbuffer,
        sizeof fixture.txbuffer));
    fifo_before = replacement_fifo;
    ids_before = replacement_ids;
    CHECK(memcmp(&fifo_before, &fixture.fifo, sizeof fifo_before) == 0);
    CHECK(memcmp(&ids_before, &fixture.ids, sizeof ids_before) == 0);
    CHECK(fixture.result.error == P4_HOSTED_PENDING_ERROR_SESSION &&
          fixture.result.submitted == 0u);
    CHECK(fixture.fake.write_calls == 1u && fixture.fake.other_reads == 0u);
}

static void test_replacement_identity_is_not_completed_after_write(void)
{
    struct fixture fixture;
    struct p4_hosted_request_ids replacement_ids, ids_before;
    uint32_t replacement_uid = 0u;

    fixture_init(&fixture, TEST_REQUEST_ID, 0u);
    memset(&replacement_ids, 0, sizeof replacement_ids);
    CHECK(p4_hosted_request_ids_init(&replacement_ids,
                                     REPLACEMENT_GENERATION));
    CHECK(p4_hosted_request_ids_reserve(&replacement_ids,
        REPLACEMENT_GENERATION, 283u, &replacement_uid));
    CHECK(replacement_uid == 1u);
    fixture.fake.replacement_ids = replacement_ids;
    fixture.fake.replace_ids_on_write = 1u;
    ids_before = replacement_ids;

    CHECK(!submit(&fixture, TEST_GENERATION, &fixture.ops, 0u,
        fixture.request, fixture.request_size, fixture.txbuffer,
        sizeof fixture.txbuffer));
    CHECK(fixture.result.error == P4_HOSTED_PENDING_ERROR_SESSION &&
          fixture.result.submitted == 1u && fixture.result.tx.success == 1u);
    CHECK(fixture.fifo.tx_sent == 1u && !fixture.fifo.poisoned &&
          !fixture.fifo.busy);
    CHECK(memcmp(&ids_before, &fixture.ids, sizeof ids_before) == 0);
    CHECK(fixture.fake.write_calls == 1u && fixture.fake.other_reads == 0u);
}

static void test_correlator_refuses_bad_metadata_without_mutation(void)
{
    struct fixture fixture;
    struct p4_hosted_request_ids ids_before;
    struct p4_hosted_inbox_item item, item_before;

    fixture_init(&fixture, TEST_REQUEST_ID, 0u);
    CHECK(submit(&fixture, TEST_GENERATION, &fixture.ops, 0u,
        fixture.request, fixture.request_size, fixture.txbuffer,
        sizeof fixture.txbuffer));
    ids_before = fixture.ids;

    memset(&item, 0, sizeof item);
    item.msg_type = P4_HOSTED_RPC_TYPE_REQUEST;
    item.msg_id = TEST_REQUEST_ID;
    item.uid = fixture.ids.active_uid;
    item_before = item;
    CHECK(p4_hosted_pending_correlate(&fixture.ids, TEST_GENERATION,
        &item) == P4_HOSTED_PENDING_CORRELATION_REFUSED);
    CHECK(memcmp(&ids_before, &fixture.ids, sizeof ids_before) == 0);
    CHECK(memcmp(&item_before, &item, sizeof item_before) == 0);

    item.msg_type = P4_HOSTED_RPC_TYPE_RESPONSE;
    item.msg_id = 606u;
    item.uid = fixture.ids.active_uid;
    CHECK(p4_hosted_pending_correlate(&fixture.ids, TEST_GENERATION,
        &item) == P4_HOSTED_PENDING_CORRELATION_REFUSED);
    CHECK(memcmp(&ids_before, &fixture.ids, sizeof ids_before) == 0);

    item.msg_id = TEST_REQUEST_ID + 256u;
    item.uid = 0u;
    CHECK(p4_hosted_pending_correlate(&fixture.ids, TEST_GENERATION,
        &item) == P4_HOSTED_PENDING_CORRELATION_REFUSED);
    CHECK(memcmp(&ids_before, &fixture.ids, sizeof ids_before) == 0);

    item.uid = fixture.ids.active_uid;
    item.payload_size = P4_HOSTED_RPC_MAX_SIZE + 1u;
    CHECK(p4_hosted_pending_correlate(&fixture.ids, TEST_GENERATION,
        &item) == P4_HOSTED_PENDING_CORRELATION_REFUSED);
    CHECK(memcmp(&ids_before, &fixture.ids, sizeof ids_before) == 0);

    CHECK(p4_hosted_pending_correlate(&fixture.ids, TEST_GENERATION + 1u,
        &item) == P4_HOSTED_PENDING_CORRELATION_REFUSED);
    CHECK(memcmp(&ids_before, &fixture.ids, sizeof ids_before) == 0);

    CHECK(p4_hosted_pending_correlate(&fixture.ids, TEST_GENERATION,
        NULL) == P4_HOSTED_PENDING_CORRELATION_REFUSED);
    CHECK(memcmp(&ids_before, &fixture.ids, sizeof ids_before) == 0);
}

static void test_max_uid_response_finishes_but_never_reuses_uid(void)
{
    struct p4_hosted_request_ids ids;
    struct p4_hosted_inbox_item item;
    uint32_t uid = 0u, next_uid = 0u;

    memset(&ids, 0, sizeof ids);
    CHECK(p4_hosted_request_ids_init(&ids, TEST_GENERATION));
    ids.next_uid = UINT32_MAX;
    CHECK(p4_hosted_request_ids_reserve(&ids, TEST_GENERATION,
        TEST_REQUEST_ID, &uid));
    CHECK(uid == UINT32_MAX && ids.exhausted == 1u &&
          ids.next_uid == UINT32_MAX);
    init_response_item(&item, TEST_REQUEST_ID, uid, NULL, 0u);
    CHECK(p4_hosted_pending_correlate(&ids, TEST_GENERATION, &item) ==
          P4_HOSTED_PENDING_CORRELATION_MATCHED);
    CHECK(ids.active_uid == 0u && ids.active_request_id == 0u &&
          ids.exhausted == 1u && ids.next_uid == UINT32_MAX);
    CHECK(!p4_hosted_request_ids_reserve(&ids, TEST_GENERATION,
        TEST_REQUEST_ID, &next_uid));
    CHECK(next_uid == 0u && ids.exhausted == 1u);
}

static void test_board_discovery_submit_canonical_body_and_checksums(void)
{
    unsigned checksum_enabled;

    for (checksum_enabled = 0u; checksum_enabled <= 1u;
         ++checksum_enabled) {
        struct fixture fixture;
        uint32_t uid;

        fixture_init(&fixture, TEST_DISCOVERY_REQUEST_ID,
                     checksum_enabled);
        set_supported_board_discovery_init(&fixture.init);
        uid = fixture_uid(&fixture);
        CHECK(uid == 1u &&
              fixture.ids.active_request_id == TEST_DISCOVERY_REQUEST_ID);
        CHECK(submit_board_discovery(&fixture, TEST_GENERATION,
            &fixture.ops, checksum_enabled, fixture.request,
            fixture.request_size, fixture.txbuffer,
            sizeof fixture.txbuffer, &fixture.init,
            TEST_DISCOVERY_NONCE));
        CHECK(fixture.result.error == P4_HOSTED_PENDING_ERROR_NONE &&
              fixture.result.submitted == 1u && fixture.result.uid == uid &&
              fixture.result.request_id == TEST_DISCOVERY_REQUEST_ID &&
              fixture.result.request_size == fixture.request_size);
        CHECK(fixture.ids.generation == TEST_GENERATION &&
              fixture.ids.active_uid == uid &&
              fixture.ids.active_request_id == TEST_DISCOVERY_REQUEST_ID &&
              fixture.ids.next_uid == 2u && fixture.ids.retired == 0u);
        CHECK(fixture.fake.token_reads == 1u &&
              fixture.fake.write_calls == 1u &&
              fixture.fake.captured_padding_zero &&
              fixture.fifo.tx_sent == 1u && !fixture.fifo.poisoned);
        verify_board_discovery_request_frame(&fixture.fake, uid,
            TEST_DISCOVERY_NONCE, checksum_enabled);

        /* UINT32_MAX must fit the canonical eight-byte nested request body. */
        fixture_init(&fixture, TEST_DISCOVERY_REQUEST_ID,
                     checksum_enabled);
        set_supported_board_discovery_init(&fixture.init);
        uid = fixture_uid(&fixture);
        fixture.request_size = encode_board_discovery_request(uid,
            P4_HOSTED_INIT_BOARD_DISCOVERY_VERSION_1, UINT32_MAX,
            fixture.request, sizeof fixture.request);
        CHECK(fixture.request_size != 0u);
        CHECK(submit_board_discovery(&fixture, TEST_GENERATION,
            &fixture.ops, checksum_enabled, fixture.request,
            fixture.request_size, fixture.txbuffer,
            sizeof fixture.txbuffer, &fixture.init, UINT32_MAX));
        CHECK(fixture.result.error == P4_HOSTED_PENDING_ERROR_NONE &&
              fixture.result.submitted == 1u && fixture.result.uid == uid &&
              fixture.result.request_id == TEST_DISCOVERY_REQUEST_ID);
        verify_board_discovery_request_frame(&fixture.fake, uid,
            UINT32_MAX, checksum_enabled);
    }
}

static void test_board_discovery_submit_preflight_refusals_are_atomic(void)
{
    struct fixture fixture;
    struct p4_hosted_init unsupported_init;
    struct p4_hosted_request_ids wrong_ids;
    uint8_t malformed[P4_HOSTED_PENDING_MAX_REQUEST_SIZE];
    uint32_t uid;
    size_t malformed_size;

    fixture_init(&fixture, TEST_DISCOVERY_REQUEST_ID, 0u);
    set_supported_board_discovery_init(&fixture.init);
    uid = fixture_uid(&fixture);

    malformed_size = encode_board_discovery_request(uid,
        P4_HOSTED_INIT_BOARD_DISCOVERY_VERSION_1,
        TEST_DISCOVERY_NONCE + 1u, malformed, sizeof malformed);
    CHECK(malformed_size != 0u);
    expect_board_discovery_preflight_unchanged(&fixture, &fixture.fifo,
        &fixture.ids, TEST_GENERATION, &fixture.ops, 0u, malformed,
        malformed_size, fixture.txbuffer, sizeof fixture.txbuffer,
        &fixture.init, TEST_DISCOVERY_NONCE);

    malformed_size = encode_board_discovery_request(uid, 2u,
        TEST_DISCOVERY_NONCE, malformed, sizeof malformed);
    CHECK(malformed_size != 0u);
    expect_board_discovery_preflight_unchanged(&fixture, &fixture.fifo,
        &fixture.ids, TEST_GENERATION, &fixture.ops, 0u, malformed,
        malformed_size, fixture.txbuffer, sizeof fixture.txbuffer,
        &fixture.init, TEST_DISCOVERY_NONCE);

    malformed_size = encode_board_discovery_request(uid,
        P4_HOSTED_INIT_BOARD_DISCOVERY_VERSION_1, 0u, malformed,
        sizeof malformed);
    CHECK(malformed_size != 0u);
    expect_board_discovery_preflight_unchanged(&fixture, &fixture.fifo,
        &fixture.ids, TEST_GENERATION, &fixture.ops, 0u, malformed,
        malformed_size, fixture.txbuffer, sizeof fixture.txbuffer,
        &fixture.init, 0u);

    malformed_size = encode_board_discovery_request(uid + 1u,
        P4_HOSTED_INIT_BOARD_DISCOVERY_VERSION_1, TEST_DISCOVERY_NONCE,
        malformed, sizeof malformed);
    CHECK(malformed_size != 0u);
    expect_board_discovery_preflight_unchanged(&fixture, &fixture.fifo,
        &fixture.ids, TEST_GENERATION, &fixture.ops, 0u, malformed,
        malformed_size, fixture.txbuffer, sizeof fixture.txbuffer,
        &fixture.init, TEST_DISCOVERY_NONCE);

    /* Missing or changed INIT singleton facts never reach the callbacks. */
    unsupported_init = fixture.init;
    unsupported_init.chip_id = P4_HOSTED_INIT_CHIP_ESP32S3;
    expect_board_discovery_preflight_unchanged(&fixture, &fixture.fifo,
        &fixture.ids, TEST_GENERATION, &fixture.ops, 0u, fixture.request,
        fixture.request_size, fixture.txbuffer, sizeof fixture.txbuffer,
        &unsupported_init, TEST_DISCOVERY_NONCE);

    unsupported_init = fixture.init;
    unsupported_init.capabilities = 0u;
    expect_board_discovery_preflight_unchanged(&fixture, &fixture.fifo,
        &fixture.ids, TEST_GENERATION, &fixture.ops, 0u, fixture.request,
        fixture.request_size, fixture.txbuffer, sizeof fixture.txbuffer,
        &unsupported_init, TEST_DISCOVERY_NONCE);

    unsupported_init = fixture.init;
    unsupported_init.present = (uint8_t)(unsupported_init.present &
        (uint8_t)~P4_HOSTED_INIT_HAS_CAPABILITIES);
    expect_board_discovery_preflight_unchanged(&fixture, &fixture.fifo,
        &fixture.ids, TEST_GENERATION, &fixture.ops, 0u, fixture.request,
        fixture.request_size, fixture.txbuffer, sizeof fixture.txbuffer,
        &unsupported_init, TEST_DISCOVERY_NONCE);

    unsupported_init = fixture.init;
    unsupported_init.present = (uint8_t)(unsupported_init.present &
        (uint8_t)~P4_HOSTED_INIT_HAS_CHIP_ID);
    expect_board_discovery_preflight_unchanged(&fixture, &fixture.fifo,
        &fixture.ids, TEST_GENERATION, &fixture.ops, 0u, fixture.request,
        fixture.request_size, fixture.txbuffer, sizeof fixture.txbuffer,
        &unsupported_init, TEST_DISCOVERY_NONCE);

    unsupported_init = fixture.init;
    unsupported_init.present = (uint8_t)(unsupported_init.present &
        (uint8_t)~P4_HOSTED_INIT_HAS_BOARD_DISCOVERY);
    expect_board_discovery_preflight_unchanged(&fixture, &fixture.fifo,
        &fixture.ids, TEST_GENERATION, &fixture.ops, 0u, fixture.request,
        fixture.request_size, fixture.txbuffer, sizeof fixture.txbuffer,
        &unsupported_init, TEST_DISCOVERY_NONCE);

    /* A parsed peer advertising version 2 is not the version-1 contract. */
    unsupported_init = fixture.init;
    unsupported_init.board_discovery_version = 2u;
    expect_board_discovery_preflight_unchanged(&fixture, &fixture.fifo,
        &fixture.ids, TEST_GENERATION, &fixture.ops, 0u, fixture.request,
        fixture.request_size, fixture.txbuffer, sizeof fixture.txbuffer,
        &unsupported_init, TEST_DISCOVERY_NONCE);

    expect_board_discovery_preflight_unchanged(&fixture, &fixture.fifo,
        &fixture.ids, TEST_GENERATION, &fixture.ops, 2u, fixture.request,
        fixture.request_size, fixture.txbuffer, sizeof fixture.txbuffer,
        &fixture.init, TEST_DISCOVERY_NONCE);
    expect_board_discovery_preflight_unchanged(&fixture, &fixture.fifo,
        &fixture.ids, TEST_GENERATION + 1u, &fixture.ops, 0u,
        fixture.request, fixture.request_size, fixture.txbuffer,
        sizeof fixture.txbuffer, &fixture.init, TEST_DISCOVERY_NONCE);
    expect_board_discovery_preflight_unchanged(&fixture, &fixture.fifo,
        &fixture.ids, TEST_GENERATION, &fixture.ops, 0u, fixture.request,
        fixture.request_size, fixture.txbuffer, sizeof fixture.txbuffer,
        NULL, TEST_DISCOVERY_NONCE);

    /* The call must match an already reserved 350 UID and generation. */
    wrong_ids = fixture.ids;
    wrong_ids.active_request_id = 349u;
    expect_board_discovery_preflight_unchanged(&fixture, &fixture.fifo,
        &wrong_ids, TEST_GENERATION, &fixture.ops, 0u, fixture.request,
        fixture.request_size, fixture.txbuffer, sizeof fixture.txbuffer,
        &fixture.init, TEST_DISCOVERY_NONCE);

    wrong_ids = fixture.ids;
    CHECK(p4_hosted_request_ids_finish(&wrong_ids, TEST_GENERATION, uid,
        TEST_DISCOVERY_REQUEST_ID));
    expect_board_discovery_preflight_unchanged(&fixture, &fixture.fifo,
        &wrong_ids, TEST_GENERATION, &fixture.ops, 0u, fixture.request,
        fixture.request_size, fixture.txbuffer, sizeof fixture.txbuffer,
        &fixture.init, TEST_DISCOVERY_NONCE);

    wrong_ids = fixture.ids;
    wrong_ids.active_uid = uid + 1u;
    expect_board_discovery_preflight_unchanged(&fixture, &fixture.fifo,
        &wrong_ids, TEST_GENERATION, &fixture.ops, 0u, fixture.request,
        fixture.request_size, fixture.txbuffer, sizeof fixture.txbuffer,
        &fixture.init, TEST_DISCOVERY_NONCE);

    expect_board_discovery_preflight_unchanged(&fixture, &fixture.fifo,
        &fixture.ids, TEST_GENERATION, &fixture.ops, 0u,
        (const uint8_t *)(const void *)&fixture.fifo, fixture.request_size,
        fixture.txbuffer, sizeof fixture.txbuffer, &fixture.init,
        TEST_DISCOVERY_NONCE);
}

static void test_board_discovery_tx_failures_consume_and_retire_once(void)
{
    struct fixture fixture;
    struct p4_hosted_request_ids ids_before;
    uint32_t uid;

    fixture_init(&fixture, TEST_DISCOVERY_REQUEST_ID, 0u);
    set_supported_board_discovery_init(&fixture.init);
    uid = fixture_uid(&fixture);
    fixture.fake.token_result = 0;
    CHECK(!submit_board_discovery(&fixture, TEST_GENERATION, &fixture.ops,
        0u, fixture.request, fixture.request_size, fixture.txbuffer,
        sizeof fixture.txbuffer, &fixture.init, TEST_DISCOVERY_NONCE));
    CHECK(fixture.result.error == P4_HOSTED_PENDING_ERROR_TX &&
          fixture.result.tx.error == P4_HOSTED_TX_ERROR_TOKEN_READ &&
          fixture.result.submitted == 0u);
    CHECK(fixture.fake.token_reads == 1u && fixture.fake.write_calls == 0u &&
          !fixture.fifo.poisoned && fixture.ids.retired == 0u &&
          fixture.ids.next_uid == uid + 1u && fixture.ids.active_uid == 0u &&
          fixture.ids.active_request_id == 0u);

    fixture_init(&fixture, TEST_DISCOVERY_REQUEST_ID, 0u);
    set_supported_board_discovery_init(&fixture.init);
    uid = fixture_uid(&fixture);
    fixture.fake.write_result = 0;
    fixture.fake.completion_overridden = 1u;
    fixture.fake.completion_override.started = 1u;
    fixture.fake.completion_override.success = 0u;
    fixture.fake.completion_override.completed_bytes = 0u;
    CHECK(!submit_board_discovery(&fixture, TEST_GENERATION, &fixture.ops,
        0u, fixture.request, fixture.request_size, fixture.txbuffer,
        sizeof fixture.txbuffer, &fixture.init, TEST_DISCOVERY_NONCE));
    CHECK(fixture.result.error == P4_HOSTED_PENDING_ERROR_SESSION &&
          fixture.result.tx.error == P4_HOSTED_TX_ERROR_WRITE_AMBIGUOUS &&
          fixture.result.submitted == 0u &&
          fixture.result.tx.write_called == 1u);
    CHECK(fixture.fake.write_calls == 1u && fixture.fifo.poisoned &&
          !fixture.fifo.busy && fixture.ids.retired == 1u &&
          fixture.ids.next_uid == uid + 1u && fixture.ids.active_uid == 0u &&
          fixture.ids.active_request_id == 0u);
    ids_before = fixture.ids;
    expect_board_discovery_preflight_unchanged(&fixture, &fixture.fifo,
        &fixture.ids, TEST_GENERATION, &fixture.ops, 0u, fixture.request,
        fixture.request_size, fixture.txbuffer, sizeof fixture.txbuffer,
        &fixture.init, TEST_DISCOVERY_NONCE);
    CHECK(memcmp(&ids_before, &fixture.ids, sizeof ids_before) == 0 &&
          fixture.fake.write_calls == 1u);
}

static void test_board_discovery_tx_does_not_mutate_callback_replacements(void)
{
    struct fixture fixture;
    struct p4_hosted_fifo replacement_fifo, fifo_before;
    struct p4_hosted_request_ids replacement_ids, ids_before;
    uint32_t replacement_uid = 0u;

    fixture_init(&fixture, TEST_DISCOVERY_REQUEST_ID, 0u);
    set_supported_board_discovery_init(&fixture.init);
    memset(&replacement_fifo, 0, sizeof replacement_fifo);
    memset(&replacement_ids, 0, sizeof replacement_ids);
    CHECK(p4_hosted_fifo_start(&replacement_fifo, REPLACEMENT_GENERATION,
                               TEST_POOL_LIMIT));
    replacement_fifo.tx_sent = 7u;
    CHECK(p4_hosted_request_ids_init(&replacement_ids,
                                     REPLACEMENT_GENERATION));
    CHECK(p4_hosted_request_ids_reserve(&replacement_ids,
        REPLACEMENT_GENERATION, 257u, &replacement_uid));
    CHECK(replacement_uid == 1u);
    fixture.fake.replacement_fifo = replacement_fifo;
    fixture.fake.replacement_ids = replacement_ids;
    fixture.fake.replace_epoch_on_write = 1u;

    CHECK(!submit_board_discovery(&fixture, TEST_GENERATION, &fixture.ops,
        0u, fixture.request, fixture.request_size, fixture.txbuffer,
        sizeof fixture.txbuffer, &fixture.init, TEST_DISCOVERY_NONCE));
    fifo_before = replacement_fifo;
    ids_before = replacement_ids;
    CHECK(memcmp(&fifo_before, &fixture.fifo, sizeof fifo_before) == 0 &&
          memcmp(&ids_before, &fixture.ids, sizeof ids_before) == 0);
    CHECK(fixture.result.error == P4_HOSTED_PENDING_ERROR_SESSION &&
          fixture.result.submitted == 0u && fixture.fake.write_calls == 1u);

    fixture_init(&fixture, TEST_DISCOVERY_REQUEST_ID, 0u);
    set_supported_board_discovery_init(&fixture.init);
    memset(&replacement_ids, 0, sizeof replacement_ids);
    CHECK(p4_hosted_request_ids_init(&replacement_ids,
                                     REPLACEMENT_GENERATION));
    CHECK(p4_hosted_request_ids_reserve(&replacement_ids,
        REPLACEMENT_GENERATION, 283u, &replacement_uid));
    fixture.fake.replacement_ids = replacement_ids;
    fixture.fake.replace_ids_on_write = 1u;

    CHECK(!submit_board_discovery(&fixture, TEST_GENERATION, &fixture.ops,
        0u, fixture.request, fixture.request_size, fixture.txbuffer,
        sizeof fixture.txbuffer, &fixture.init, TEST_DISCOVERY_NONCE));
    ids_before = replacement_ids;
    CHECK(memcmp(&ids_before, &fixture.ids, sizeof ids_before) == 0);
    CHECK(fixture.result.error == P4_HOSTED_PENDING_ERROR_SESSION &&
          fixture.result.submitted == 1u && fixture.result.tx.success == 1u &&
          fixture.fake.write_calls == 1u && fixture.fifo.tx_sent == 1u &&
          !fixture.fifo.poisoned && !fixture.fifo.busy);
}

static void test_board_discovery_correlation_is_private_and_uid_exact(void)
{
    struct fixture fixture;
    struct p4_hosted_request_ids wrong_ids;
    struct p4_hosted_request_ids ids_before;
    struct p4_hosted_inbox_item item;
    uint32_t uid, next_uid = 0u;

    fixture_init(&fixture, TEST_DISCOVERY_REQUEST_ID, 0u);
    set_supported_board_discovery_init(&fixture.init);
    uid = fixture_uid(&fixture);
    CHECK(submit_board_discovery(&fixture, TEST_GENERATION, &fixture.ops,
        0u, fixture.request, fixture.request_size, fixture.txbuffer,
        sizeof fixture.txbuffer, &fixture.init, TEST_DISCOVERY_NONCE));
    ids_before = fixture.ids;

    memset(&item, 0, sizeof item);
    item.msg_type = P4_HOSTED_RPC_TYPE_EVENT;
    item.msg_id = 775u;
    item.uid = 0u;
    CHECK(p4_hosted_pending_correlate_board_discovery(&fixture.ids,
        TEST_GENERATION, &item) == P4_HOSTED_PENDING_CORRELATION_EVENT);
    CHECK(memcmp(&ids_before, &fixture.ids, sizeof ids_before) == 0);

    wrong_ids = fixture.ids;
    wrong_ids.active_request_id = 349u;
    init_response_item(&item, TEST_DISCOVERY_REQUEST_ID, uid, NULL, 0u);
    ids_before = wrong_ids;
    CHECK(p4_hosted_pending_correlate_board_discovery(&wrong_ids,
        TEST_GENERATION, &item) == P4_HOSTED_PENDING_CORRELATION_REFUSED);
    CHECK(memcmp(&ids_before, &wrong_ids, sizeof ids_before) == 0);
    ids_before = fixture.ids;

    init_response_item(&item, TEST_AP_INFO_REQUEST_ID, uid, NULL, 0u);
    CHECK(item.msg_id == 550u &&
          p4_hosted_pending_correlate_board_discovery(&fixture.ids,
              TEST_GENERATION, &item) ==
              P4_HOSTED_PENDING_CORRELATION_UNMATCHED);
    CHECK(memcmp(&ids_before, &fixture.ids, sizeof ids_before) == 0);
    init_response_item(&item, 349u, uid, NULL, 0u);
    CHECK(item.msg_id == 605u &&
          p4_hosted_pending_correlate_board_discovery(&fixture.ids,
              TEST_GENERATION, &item) ==
              P4_HOSTED_PENDING_CORRELATION_UNMATCHED);
    CHECK(memcmp(&ids_before, &fixture.ids, sizeof ids_before) == 0);

    init_response_item(&item, TEST_DISCOVERY_REQUEST_ID, uid + 1u,
                       NULL, 0u);
    CHECK(item.msg_id == TEST_DISCOVERY_RESPONSE_ID &&
          p4_hosted_pending_correlate_board_discovery(&fixture.ids,
              TEST_GENERATION, &item) ==
              P4_HOSTED_PENDING_CORRELATION_UNMATCHED);
    CHECK(memcmp(&ids_before, &fixture.ids, sizeof ids_before) == 0);

    /* The normal WLAN correlator must not admit the private 606 response. */
    init_response_item(&item, TEST_DISCOVERY_REQUEST_ID, uid, NULL, 0u);
    CHECK(p4_hosted_pending_correlate(&fixture.ids, TEST_GENERATION,
        &item) == P4_HOSTED_PENDING_CORRELATION_REFUSED);
    CHECK(memcmp(&ids_before, &fixture.ids, sizeof ids_before) == 0);

    CHECK(p4_hosted_pending_correlate_board_discovery(&fixture.ids,
        TEST_GENERATION, &item) == P4_HOSTED_PENDING_CORRELATION_MATCHED);
    CHECK(fixture.ids.active_uid == 0u &&
          fixture.ids.active_request_id == 0u &&
          fixture.ids.next_uid == uid + 1u && fixture.ids.retired == 0u);
    ids_before = fixture.ids;
    CHECK(p4_hosted_pending_correlate_board_discovery(&fixture.ids,
        TEST_GENERATION, &item) == P4_HOSTED_PENDING_CORRELATION_REFUSED);
    CHECK(memcmp(&ids_before, &fixture.ids, sizeof ids_before) == 0);

    CHECK(p4_hosted_request_ids_reserve(&fixture.ids, TEST_GENERATION,
        TEST_DISCOVERY_REQUEST_ID, &next_uid));
    CHECK(next_uid == uid + 1u);
    ids_before = fixture.ids;
    CHECK(p4_hosted_pending_correlate_board_discovery(&fixture.ids,
        TEST_GENERATION, &item) == P4_HOSTED_PENDING_CORRELATION_UNMATCHED);
    CHECK(memcmp(&ids_before, &fixture.ids, sizeof ids_before) == 0);
    init_response_item(&item, TEST_DISCOVERY_REQUEST_ID, next_uid, NULL, 0u);
    CHECK(p4_hosted_pending_correlate_board_discovery(&fixture.ids,
        TEST_GENERATION + 1u, &item) == P4_HOSTED_PENDING_CORRELATION_REFUSED);
    CHECK(memcmp(&ids_before, &fixture.ids, sizeof ids_before) == 0);
    CHECK(p4_hosted_pending_correlate_board_discovery(&fixture.ids,
        TEST_GENERATION, &item) == P4_HOSTED_PENDING_CORRELATION_MATCHED);
}

static void test_normal_pending_path_rejects_private_discovery_ids(void)
{
    struct fixture fixture;
    struct p4_hosted_inbox_item item;
    struct p4_hosted_request_ids ids_before;
    uint32_t uid;

    fixture_init(&fixture, TEST_DISCOVERY_REQUEST_ID, 0u);
    set_supported_board_discovery_init(&fixture.init);
    expect_preflight_unchanged(&fixture, &fixture.fifo, &fixture.ids,
        TEST_GENERATION, &fixture.ops, 0u, fixture.request,
        fixture.request_size, fixture.txbuffer, sizeof fixture.txbuffer);
    uid = fixture_uid(&fixture);
    CHECK(submit_board_discovery(&fixture, TEST_GENERATION, &fixture.ops,
        0u, fixture.request, fixture.request_size, fixture.txbuffer,
        sizeof fixture.txbuffer, &fixture.init, TEST_DISCOVERY_NONCE));

    memset(&item, 0, sizeof item);
    item.msg_type = P4_HOSTED_RPC_TYPE_RESPONSE;
    item.msg_id = TEST_DISCOVERY_RESPONSE_ID;
    item.uid = uid;
    ids_before = fixture.ids;
    CHECK(p4_hosted_pending_correlate(&fixture.ids, TEST_GENERATION,
        &item) == P4_HOSTED_PENDING_CORRELATION_REFUSED);
    CHECK(memcmp(&ids_before, &fixture.ids, sizeof ids_before) == 0);
}

static void expect_ota_preflight_unchanged(struct fixture *fixture,
    struct p4_hosted_fifo *fifo, struct p4_hosted_request_ids *ids,
    uint32_t generation, const struct p4_hosted_tx_ops *ops,
    unsigned checksum_enabled, const uint8_t *request, size_t request_size,
    uint8_t *buffer, size_t capacity)
{
    struct p4_hosted_fifo fifo_before;
    struct p4_hosted_request_ids ids_before;
    struct p4_hosted_tx_ops ops_before;
    struct p4_hosted_pending_result result_before;
    uint8_t request_before[P4_HOSTED_PENDING_MAX_REQUEST_SIZE + 1u];
    uint8_t buffer_before[P4_HOSTED_PENDING_BUFFER_MAX + 1u];
    unsigned token_reads = fixture->fake.token_reads;
    unsigned other_reads = fixture->fake.other_reads;
    unsigned writes = fixture->fake.write_calls;
    unsigned sessions = fixture->fake.session_calls;
    unsigned budgets = fixture->fake.budget_calls;
    unsigned yields = fixture->fake.yield_calls;

    CHECK(request_size <= sizeof request_before);
    CHECK(capacity <= sizeof buffer_before);
    memcpy(&fifo_before, fifo, sizeof fifo_before);
    memcpy(&ids_before, ids, sizeof ids_before);
    memcpy(&ops_before, ops, sizeof ops_before);
    memcpy(&result_before, &fixture->result, sizeof result_before);
    if (request && request_size != 0u)
        memcpy(request_before, request, request_size);
    if (buffer && capacity != 0u)
        memcpy(buffer_before, buffer, capacity);

    CHECK(!p4_hosted_pending_submit_ota(fifo, ids, generation, ops,
        checksum_enabled, request, request_size, buffer, capacity,
        &fixture->result));
    CHECK(memcmp(&fifo_before, fifo, sizeof fifo_before) == 0);
    CHECK(memcmp(&ids_before, ids, sizeof ids_before) == 0);
    CHECK(memcmp(&ops_before, ops, sizeof ops_before) == 0);
    CHECK(memcmp(&result_before, &fixture->result,
                 sizeof result_before) == 0);
    if (request && request_size != 0u)
        CHECK(memcmp(request_before, request, request_size) == 0);
    if (buffer && capacity != 0u)
        CHECK(memcmp(buffer_before, buffer, capacity) == 0);
    CHECK(fixture->fake.token_reads == token_reads &&
          fixture->fake.other_reads == other_reads &&
          fixture->fake.write_calls == writes &&
          fixture->fake.session_calls == sessions &&
          fixture->fake.budget_calls == budgets &&
          fixture->fake.yield_calls == yields);
}

static size_t append_test_varint(uint8_t *bytes, size_t capacity,
                                 size_t offset, uint64_t value)
{
    while (value >= UINT64_C(0x80)) {
        if (offset >= capacity)
            return 0u;
        bytes[offset++] = (uint8_t)((value & UINT64_C(0x7f)) |
                                    UINT64_C(0x80));
        value >>= 7;
    }
    if (offset >= capacity)
        return 0u;
    bytes[offset++] = (uint8_t)value;
    return offset;
}

static size_t encode_noncanonical_ota_envelope(uint32_t request_id,
    uint32_t uid, const uint8_t *body, size_t body_size, uint8_t *bytes,
    size_t capacity)
{
    uint64_t payload_key = ((uint64_t)request_id << 3) | UINT64_C(2);
    size_t offset = 0u;

    /* Reordered envelope fields decode successfully, but are not canonical. */
    offset = append_test_varint(bytes, capacity, offset, UINT64_C(8));
    if (offset == 0u)
        return 0u;
    offset = append_test_varint(bytes, capacity, offset,
                                P4_HOSTED_RPC_TYPE_REQUEST);
    if (offset == 0u)
        return 0u;
    offset = append_test_varint(bytes, capacity, offset, UINT64_C(24));
    if (offset == 0u)
        return 0u;
    offset = append_test_varint(bytes, capacity, offset, uid);
    if (offset == 0u)
        return 0u;
    offset = append_test_varint(bytes, capacity, offset, UINT64_C(16));
    if (offset == 0u)
        return 0u;
    offset = append_test_varint(bytes, capacity, offset, request_id);
    if (offset == 0u)
        return 0u;
    offset = append_test_varint(bytes, capacity, offset, payload_key);
    if (offset == 0u)
        return 0u;
    offset = append_test_varint(bytes, capacity, offset, body_size);
    if (offset == 0u || body_size > capacity - offset)
        return 0u;
    if (body_size != 0u) {
        memcpy(bytes + offset, body, body_size);
        offset += body_size;
    }
    return offset;
}

static void test_ota_submit_sends_each_private_request_once(void)
{
    static const uint8_t one_byte[] = {0xe9u};
    unsigned checksum_enabled;
    uint32_t request_id;

    for (checksum_enabled = 0u; checksum_enabled <= 1u;
         ++checksum_enabled) {
        for (request_id = TEST_OTA_BEGIN_REQUEST_ID;
             request_id <= TEST_OTA_END_REQUEST_ID; ++request_id) {
            struct fixture fixture;
            const uint8_t *data = request_id == TEST_OTA_WRITE_REQUEST_ID
                                ? one_byte : NULL;
            size_t data_size = data ? sizeof one_byte : 0u;
            uint32_t uid;

            fixture_init_ota(&fixture, request_id, checksum_enabled, data,
                             data_size);
            uid = fixture_uid(&fixture);
            CHECK(uid == 1u &&
                  fixture.ids.active_request_id == request_id);
            CHECK(submit_ota(&fixture, TEST_GENERATION, &fixture.ops,
                checksum_enabled, fixture.request, fixture.request_size,
                fixture.txbuffer, sizeof fixture.txbuffer));
            CHECK(fixture.result.error == P4_HOSTED_PENDING_ERROR_NONE &&
                  fixture.result.submitted == 1u &&
                  fixture.result.uid == uid &&
                  fixture.result.request_id == request_id &&
                  fixture.result.request_size == fixture.request_size);
            CHECK(fixture.ids.generation == TEST_GENERATION &&
                  fixture.ids.active_uid == uid &&
                  fixture.ids.active_request_id == request_id &&
                  fixture.ids.next_uid == 2u && !fixture.ids.retired);
            CHECK(fixture.fake.token_reads == 1u &&
                  fixture.fake.other_reads == 0u &&
                  fixture.fake.write_calls == 1u &&
                  fixture.fake.yield_calls == 0u &&
                  !fixture.fake.bad_address &&
                  fixture.fake.captured_padding_zero);
            CHECK(fixture.fifo.tx_sent == 1u &&
                  fixture.fifo.rx_consumed == 0u &&
                  !fixture.fifo.busy && !fixture.fifo.poisoned);
            verify_ota_request_frame(&fixture.fake, fixture.request,
                fixture.request_size, uid, request_id, data, data_size,
                checksum_enabled);
        }
    }
}

static void test_ota_write_size_bounds_include_maximum_envelope(void)
{
    struct fixture fixture;
    uint8_t maximum_data[P4_HOSTED_OTA_MAX_WRITE_SIZE];
    uint32_t uid = 0u;
    size_t index;

    for (index = 0u; index < sizeof maximum_data; ++index)
        maximum_data[index] = (uint8_t)(index ^ 0xa5u);

    fixture_init_ota(&fixture, TEST_OTA_WRITE_REQUEST_ID, 1u,
                     maximum_data, 1u);
    CHECK(p4_hosted_request_ids_finish(&fixture.ids, TEST_GENERATION,
        fixture_uid(&fixture), TEST_OTA_WRITE_REQUEST_ID));
    fixture.ids.next_uid = UINT32_MAX;
    CHECK(p4_hosted_request_ids_reserve(&fixture.ids, TEST_GENERATION,
        TEST_OTA_WRITE_REQUEST_ID, &uid));
    CHECK(uid == UINT32_MAX && fixture.ids.exhausted == 1u);
    CHECK(encode_ota_request(TEST_OTA_WRITE_REQUEST_ID, uid, maximum_data,
        sizeof maximum_data, fixture.request, sizeof fixture.request,
        &fixture.request_size));
    CHECK(fixture.request_size == P4_HOSTED_OTA_MAX_REQUEST_SIZE &&
          fixture.request_size == P4_HOSTED_PENDING_MAX_REQUEST_SIZE);
    CHECK(submit_ota(&fixture, TEST_GENERATION, &fixture.ops, 1u,
        fixture.request, fixture.request_size, fixture.txbuffer,
        sizeof fixture.txbuffer));
    CHECK(fixture.result.request_size == P4_HOSTED_OTA_MAX_REQUEST_SIZE &&
          fixture.fake.write_calls == 1u && fixture.fifo.tx_sent == 1u &&
          fixture.fifo.rx_consumed == 0u);
    verify_ota_request_frame(&fixture.fake, fixture.request,
        fixture.request_size, uid, TEST_OTA_WRITE_REQUEST_ID, maximum_data,
        sizeof maximum_data, 1u);
}

static void test_ota_preflight_rejects_wrong_identity_and_type(void)
{
    struct fixture fixture;
    struct p4_hosted_request_ids wrong_ids;
    uint8_t malformed[P4_HOSTED_PENDING_MAX_REQUEST_SIZE];
    uint8_t body[] = {0x0au, 1u, 0x5au};
    size_t malformed_size, encoded_size;
    uint32_t uid;

    fixture_init_ota(&fixture, TEST_OTA_BEGIN_REQUEST_ID, 0u, NULL, 0u);
    uid = fixture_uid(&fixture);

    CHECK(encode_ota_request(TEST_OTA_BEGIN_REQUEST_ID, uid + 1u, NULL,
        0u, malformed, sizeof malformed, &encoded_size));
    malformed_size = encoded_size;
    CHECK(malformed_size != 0u);
    expect_ota_preflight_unchanged(&fixture, &fixture.fifo, &fixture.ids,
        TEST_GENERATION, &fixture.ops, 0u, malformed, malformed_size,
        fixture.txbuffer, sizeof fixture.txbuffer);

    CHECK(encode_ota_request(TEST_OTA_WRITE_REQUEST_ID, uid, body + 2u,
        1u, malformed, sizeof malformed, &encoded_size));
    malformed_size = encoded_size;
    CHECK(malformed_size != 0u);
    expect_ota_preflight_unchanged(&fixture, &fixture.fifo, &fixture.ids,
        TEST_GENERATION, &fixture.ops, 0u, malformed, malformed_size,
        fixture.txbuffer, sizeof fixture.txbuffer);

    malformed_size = encode_raw_rpc(P4_HOSTED_RPC_TYPE_RESPONSE,
        TEST_OTA_BEGIN_REQUEST_ID + 256u, uid, NULL, 0u, malformed,
        sizeof malformed);
    CHECK(malformed_size != 0u);
    expect_ota_preflight_unchanged(&fixture, &fixture.fifo, &fixture.ids,
        TEST_GENERATION, &fixture.ops, 0u, malformed, malformed_size,
        fixture.txbuffer, sizeof fixture.txbuffer);

    malformed_size = encode_raw_rpc(P4_HOSTED_RPC_TYPE_REQUEST,
        TEST_OTA_END_REQUEST_ID, uid, NULL, 0u, malformed,
        sizeof malformed);
    CHECK(malformed_size != 0u);
    expect_ota_preflight_unchanged(&fixture, &fixture.fifo, &fixture.ids,
        TEST_GENERATION, &fixture.ops, 0u, malformed, malformed_size,
        fixture.txbuffer, sizeof fixture.txbuffer);

    malformed_size = encode_raw_rpc(P4_HOSTED_RPC_TYPE_REQUEST, 275u, uid,
        NULL, 0u, malformed, sizeof malformed);
    CHECK(malformed_size != 0u);
    expect_ota_preflight_unchanged(&fixture, &fixture.fifo, &fixture.ids,
        TEST_GENERATION, &fixture.ops, 0u, malformed, malformed_size,
        fixture.txbuffer, sizeof fixture.txbuffer);

    wrong_ids = fixture.ids;
    wrong_ids.active_request_id = TEST_OTA_END_REQUEST_ID;
    expect_ota_preflight_unchanged(&fixture, &fixture.fifo, &wrong_ids,
        TEST_GENERATION, &fixture.ops, 0u, fixture.request,
        fixture.request_size, fixture.txbuffer, sizeof fixture.txbuffer);
    expect_ota_preflight_unchanged(&fixture, &fixture.fifo, &fixture.ids,
        TEST_GENERATION + 1u, &fixture.ops, 0u, fixture.request,
        fixture.request_size, fixture.txbuffer, sizeof fixture.txbuffer);
}

static void test_ota_preflight_rejects_noncanonical_and_malformed_bodies(void)
{
    static const uint8_t nonempty_empty_body[] = {0x08u, 0x01u};
    static const uint8_t truncated_length[] = {0x0au};
    static const uint8_t zero_length_data[] = {0x0au, 0x00u};
    static const uint8_t wrong_wire_type[] = {0x08u, 0x01u};
    static const uint8_t overlong_length[] = {0x0au, 0x81u, 0x00u, 0xaau};
    static const uint8_t duplicate_data[] = {
        0x0au, 0x01u, 0xaau, 0x0au, 0x01u, 0xbbu
    };
    static const uint8_t truncated_data[] = {0x0au, 0x02u, 0xaau};
    static const uint8_t one_byte[] = {0xa5u};
    struct fixture fixture;
    uint8_t malformed[P4_HOSTED_PENDING_MAX_REQUEST_SIZE];
    size_t malformed_size;
    uint32_t request_id;
    unsigned index;
    static const struct {
        const uint8_t *bytes;
        size_t size;
    } malformed_bodies[] = {
        {truncated_length, sizeof truncated_length},
        {zero_length_data, sizeof zero_length_data},
        {wrong_wire_type, sizeof wrong_wire_type},
        {overlong_length, sizeof overlong_length},
        {duplicate_data, sizeof duplicate_data},
        {truncated_data, sizeof truncated_data}
    };

    for (request_id = TEST_OTA_BEGIN_REQUEST_ID;
         request_id <= TEST_OTA_END_REQUEST_ID;
         request_id += TEST_OTA_END_REQUEST_ID -
                       TEST_OTA_BEGIN_REQUEST_ID) {
        fixture_init_ota(&fixture, request_id, 0u, NULL, 0u);
        malformed_size = encode_raw_rpc(P4_HOSTED_RPC_TYPE_REQUEST,
            request_id, fixture_uid(&fixture), nonempty_empty_body,
            sizeof nonempty_empty_body, malformed, sizeof malformed);
        CHECK(malformed_size != 0u);
        expect_ota_preflight_unchanged(&fixture, &fixture.fifo,
            &fixture.ids, TEST_GENERATION, &fixture.ops, 0u, malformed,
            malformed_size, fixture.txbuffer, sizeof fixture.txbuffer);
    }

    fixture_init_ota(&fixture, TEST_OTA_WRITE_REQUEST_ID, 0u,
                     one_byte, sizeof one_byte);
    malformed_size = encode_noncanonical_ota_envelope(
        TEST_OTA_WRITE_REQUEST_ID, fixture_uid(&fixture),
        (const uint8_t[]){0x0au, 0x01u, 0xa5u}, 3u, malformed,
        sizeof malformed);
    CHECK(malformed_size != 0u);
    expect_ota_preflight_unchanged(&fixture, &fixture.fifo, &fixture.ids,
        TEST_GENERATION, &fixture.ops, 0u, malformed, malformed_size,
        fixture.txbuffer, sizeof fixture.txbuffer);

    for (index = 0u;
         index < sizeof malformed_bodies / sizeof malformed_bodies[0];
         ++index) {
        fixture_init_ota(&fixture, TEST_OTA_WRITE_REQUEST_ID, 0u,
                         malformed_bodies[index].bytes,
                         malformed_bodies[index].size);
        malformed_size = encode_raw_rpc(P4_HOSTED_RPC_TYPE_REQUEST,
            TEST_OTA_WRITE_REQUEST_ID, fixture_uid(&fixture),
            malformed_bodies[index].bytes, malformed_bodies[index].size,
            malformed, sizeof malformed);
        CHECK(malformed_size != 0u);
        expect_ota_preflight_unchanged(&fixture, &fixture.fifo,
            &fixture.ids, TEST_GENERATION, &fixture.ops, 0u, malformed,
            malformed_size, fixture.txbuffer, sizeof fixture.txbuffer);
    }
}

static void test_ota_preflight_bounds_and_alias_refusals_are_atomic(void)
{
    struct fixture fixture;
    struct p4_hosted_fifo corrupt_fifo;
    struct p4_hosted_tx_ops missing_ops;
    uint8_t oversized_request[P4_HOSTED_PENDING_MAX_REQUEST_SIZE + 1u];
    uint8_t extended_buffer[P4_HOSTED_PENDING_BUFFER_MAX + 1u];
    uint8_t too_long_data[P4_HOSTED_OTA_MAX_WRITE_SIZE + 1u];
    uint8_t malformed[P4_HOSTED_PENDING_MAX_REQUEST_SIZE];
    uint8_t one_byte = 0xa5u;
    size_t malformed_size;

    fixture_init_ota(&fixture, TEST_OTA_WRITE_REQUEST_ID, 0u,
                     &one_byte, sizeof one_byte);
    memset(extended_buffer, 0xa7, sizeof extended_buffer);
    expect_ota_preflight_unchanged(&fixture, &fixture.fifo, &fixture.ids,
        TEST_GENERATION, &fixture.ops, 2u, fixture.request,
        fixture.request_size, fixture.txbuffer, sizeof fixture.txbuffer);
    expect_ota_preflight_unchanged(&fixture, &fixture.fifo, &fixture.ids,
        TEST_GENERATION, &fixture.ops, 0u, fixture.request, 0u,
        fixture.txbuffer, sizeof fixture.txbuffer);
    memset(oversized_request, 0x6cu, sizeof oversized_request);
    expect_ota_preflight_unchanged(&fixture, &fixture.fifo, &fixture.ids,
        TEST_GENERATION, &fixture.ops, 0u, oversized_request,
        sizeof oversized_request, fixture.txbuffer,
        sizeof fixture.txbuffer);
    expect_ota_preflight_unchanged(&fixture, &fixture.fifo, &fixture.ids,
        TEST_GENERATION, &fixture.ops, 0u, fixture.request,
        fixture.request_size, fixture.txbuffer,
        P4_HOSTED_PENDING_BUFFER_MIN - 1u);
    expect_ota_preflight_unchanged(&fixture, &fixture.fifo, &fixture.ids,
        TEST_GENERATION, &fixture.ops, 0u, fixture.request,
        fixture.request_size, extended_buffer, sizeof extended_buffer);
    expect_ota_preflight_unchanged(&fixture, &fixture.fifo, &fixture.ids,
        TEST_GENERATION, &fixture.ops, 0u, NULL, fixture.request_size,
        fixture.txbuffer, sizeof fixture.txbuffer);
    expect_ota_preflight_unchanged(&fixture, &fixture.fifo, &fixture.ids,
        TEST_GENERATION, &fixture.ops, 0u, fixture.request,
        fixture.request_size, NULL, sizeof fixture.txbuffer);

    missing_ops = fixture.ops;
    missing_ops.yield = NULL;
    expect_ota_preflight_unchanged(&fixture, &fixture.fifo, &fixture.ids,
        TEST_GENERATION, &missing_ops, 0u, fixture.request,
        fixture.request_size, fixture.txbuffer, sizeof fixture.txbuffer);
    corrupt_fifo = fixture.fifo;
    corrupt_fifo.pool_limit = 0u;
    expect_ota_preflight_unchanged(&fixture, &corrupt_fifo, &fixture.ids,
        TEST_GENERATION, &fixture.ops, 0u, fixture.request,
        fixture.request_size, fixture.txbuffer, sizeof fixture.txbuffer);

    memset(too_long_data, 0xabu, sizeof too_long_data);
    /* A syntactically valid empty-envelope test is insufficient here: create
     * the canonical bytes field around the over-limit 239-byte payload. */
    {
        uint8_t body[3u + P4_HOSTED_OTA_MAX_WRITE_SIZE + 1u];
        body[0] = 0x0au;
        body[1] = (uint8_t)((sizeof too_long_data & 0x7fu) | 0x80u);
        body[2] = (uint8_t)(sizeof too_long_data >> 7);
        memcpy(body + 3u, too_long_data, sizeof too_long_data);
        malformed_size = encode_raw_rpc(P4_HOSTED_RPC_TYPE_REQUEST,
            TEST_OTA_WRITE_REQUEST_ID, fixture_uid(&fixture), body,
            sizeof body, malformed, sizeof malformed);
    }
    CHECK(malformed_size != 0u && malformed_size <= sizeof malformed);
    expect_ota_preflight_unchanged(&fixture, &fixture.fifo, &fixture.ids,
        TEST_GENERATION, &fixture.ops, 0u, malformed, malformed_size,
        fixture.txbuffer, sizeof fixture.txbuffer);

    /* The six complete object ranges must remain pairwise disjoint. */
    {
        union pending_alias_slab slab;
        void *objects[6];
        unsigned first, second;

        objects[0] = &fixture.fifo;
        objects[1] = &fixture.ids;
        objects[2] = &fixture.ops;
        objects[3] = fixture.request;
        objects[4] = fixture.txbuffer;
        objects[5] = &fixture.result;
        for (first = 0u; first < 6u; ++first) {
            for (second = first + 1u; second < 6u; ++second) {
                struct p4_hosted_fifo fifo_before;
                struct p4_hosted_request_ids ids_before;
                struct p4_hosted_tx_ops ops_before;
                struct p4_hosted_pending_result result_before;
                uint8_t request_before[P4_HOSTED_PENDING_MAX_REQUEST_SIZE];
                uint8_t txbuffer_before[TEST_CAPACITY];
                uint8_t slab_before[P4_HOSTED_PENDING_BUFFER_MAX];
                unsigned writes = fixture.fake.write_calls;
                unsigned reads = fixture.fake.token_reads;

                memset(&slab, 0x73, sizeof slab);
                objects[0] = &fixture.fifo;
                objects[1] = &fixture.ids;
                objects[2] = &fixture.ops;
                objects[3] = fixture.request;
                objects[4] = fixture.txbuffer;
                objects[5] = &fixture.result;
                objects[first] = slab.bytes;
                objects[second] = slab.bytes;
                memcpy(&fifo_before, &fixture.fifo, sizeof fifo_before);
                memcpy(&ids_before, &fixture.ids, sizeof ids_before);
                memcpy(&ops_before, &fixture.ops, sizeof ops_before);
                memcpy(&result_before, &fixture.result, sizeof result_before);
                memcpy(request_before, fixture.request,
                       sizeof request_before);
                memcpy(txbuffer_before, fixture.txbuffer,
                       sizeof txbuffer_before);
                memcpy(slab_before, slab.bytes, sizeof slab_before);

                CHECK(!p4_hosted_pending_submit_ota(
                    (struct p4_hosted_fifo *)objects[0],
                    (struct p4_hosted_request_ids *)objects[1],
                    TEST_GENERATION,
                    (const struct p4_hosted_tx_ops *)objects[2], 0u,
                    (const uint8_t *)objects[3], fixture.request_size,
                    (uint8_t *)objects[4], TEST_CAPACITY,
                    (struct p4_hosted_pending_result *)objects[5]));
                CHECK(memcmp(&fifo_before, &fixture.fifo,
                             sizeof fifo_before) == 0);
                CHECK(memcmp(&ids_before, &fixture.ids,
                             sizeof ids_before) == 0);
                CHECK(memcmp(&ops_before, &fixture.ops,
                             sizeof ops_before) == 0);
                CHECK(memcmp(&result_before, &fixture.result,
                             sizeof result_before) == 0);
                CHECK(memcmp(request_before, fixture.request,
                             sizeof request_before) == 0);
                CHECK(memcmp(txbuffer_before, fixture.txbuffer,
                             sizeof txbuffer_before) == 0);
                CHECK(memcmp(slab_before, slab.bytes,
                             sizeof slab_before) == 0);
                CHECK(fixture.fake.write_calls == writes &&
                      fixture.fake.token_reads == reads);
            }
        }
    }
}

static void test_ota_ambiguous_tx_failure_consumes_uid_without_retry(void)
{
    struct fixture fixture;
    struct p4_hosted_request_ids ids_before;
    uint32_t uid;

    fixture_init_ota(&fixture, TEST_OTA_BEGIN_REQUEST_ID, 0u, NULL, 0u);
    uid = fixture_uid(&fixture);
    fixture.fake.write_result = 0;
    fixture.fake.completion_overridden = 1u;
    fixture.fake.completion_override.started = 1u;
    fixture.fake.completion_override.success = 0u;
    fixture.fake.completion_override.completed_bytes = 0u;
    CHECK(!submit_ota(&fixture, TEST_GENERATION, &fixture.ops, 0u,
        fixture.request, fixture.request_size, fixture.txbuffer,
        sizeof fixture.txbuffer));
    CHECK(fixture.result.error == P4_HOSTED_PENDING_ERROR_SESSION &&
          fixture.result.tx.error == P4_HOSTED_TX_ERROR_WRITE_AMBIGUOUS &&
          fixture.result.tx.write_called == 1u &&
          fixture.result.submitted == 0u);
    CHECK(fixture.fake.write_calls == 1u && fixture.fake.token_reads == 1u &&
          fixture.fifo.poisoned && !fixture.fifo.busy &&
          fixture.fifo.rx_consumed == 0u && fixture.ids.retired == 1u &&
          fixture.ids.active_uid == 0u &&
          fixture.ids.active_request_id == 0u &&
          fixture.ids.next_uid == uid + 1u);

    ids_before = fixture.ids;
    expect_ota_preflight_unchanged(&fixture, &fixture.fifo, &fixture.ids,
        TEST_GENERATION, &fixture.ops, 0u, fixture.request,
        fixture.request_size, fixture.txbuffer, sizeof fixture.txbuffer);
    CHECK(memcmp(&ids_before, &fixture.ids, sizeof ids_before) == 0 &&
          fixture.fake.write_calls == 1u);
}

static void test_ota_callback_replacement_epoch_is_preserved(void)
{
    struct fixture fixture;
    struct p4_hosted_fifo replacement_fifo, fifo_before;
    struct p4_hosted_request_ids replacement_ids, ids_before;
    uint32_t replacement_uid = 0u;

    fixture_init_ota(&fixture, TEST_OTA_BEGIN_REQUEST_ID, 0u, NULL, 0u);
    memset(&replacement_fifo, 0, sizeof replacement_fifo);
    memset(&replacement_ids, 0, sizeof replacement_ids);
    CHECK(p4_hosted_fifo_start(&replacement_fifo, REPLACEMENT_GENERATION,
                               TEST_POOL_LIMIT));
    replacement_fifo.tx_sent = 7u;
    CHECK(p4_hosted_request_ids_init(&replacement_ids,
                                     REPLACEMENT_GENERATION));
    CHECK(p4_hosted_request_ids_reserve(&replacement_ids,
        REPLACEMENT_GENERATION, TEST_OTA_END_REQUEST_ID, &replacement_uid));
    CHECK(replacement_uid == 1u);
    fixture.fake.replacement_fifo = replacement_fifo;
    fixture.fake.replacement_ids = replacement_ids;
    fixture.fake.replace_epoch_on_write = 1u;

    CHECK(!submit_ota(&fixture, TEST_GENERATION, &fixture.ops, 0u,
        fixture.request, fixture.request_size, fixture.txbuffer,
        sizeof fixture.txbuffer));
    fifo_before = replacement_fifo;
    ids_before = replacement_ids;
    CHECK(memcmp(&fifo_before, &fixture.fifo, sizeof fifo_before) == 0 &&
          memcmp(&ids_before, &fixture.ids, sizeof ids_before) == 0);
    CHECK(fixture.result.error == P4_HOSTED_PENDING_ERROR_SESSION &&
          fixture.result.submitted == 0u &&
          fixture.fake.write_calls == 1u &&
          fixture.fake.other_reads == 0u);
}

static void test_ota_cannot_use_normal_control_submission(void)
{
    uint32_t request_id;
    unsigned checksum;
    static const uint8_t data[] = {0xe9, 0x00, 0xff};

    /* Even a well-formed request with a live reserved identity cannot bypass
     * the missing exclusive maintenance backend through the WLAN API. */
    for (checksum = 0u; checksum <= 1u; ++checksum) {
        for (request_id = 272u; request_id <= 274u; ++request_id) {
            struct fixture fixture;
            int encoded;
            fixture_init(&fixture, request_id, checksum);
            if (request_id == 272u)
                encoded = p4_hosted_ota_begin_request(fixture_uid(&fixture),
                    fixture.request, sizeof fixture.request,
                    &fixture.request_size);
            else if (request_id == 273u)
                encoded = p4_hosted_ota_write_request(fixture_uid(&fixture),
                    data, sizeof data, fixture.request, sizeof fixture.request,
                    &fixture.request_size);
            else
                encoded = p4_hosted_ota_end_request(fixture_uid(&fixture),
                    fixture.request, sizeof fixture.request,
                    &fixture.request_size);
            CHECK(encoded);
            expect_preflight_unchanged(&fixture, &fixture.fifo, &fixture.ids,
                TEST_GENERATION, &fixture.ops, checksum, fixture.request,
                fixture.request_size, fixture.txbuffer, sizeof fixture.txbuffer);
        }
    }
}

int main(void)
{
    test_submit_encodes_one_frame_and_leaves_identity_pending(0u);
    test_submit_encodes_one_frame_and_leaves_identity_pending(1u);
    test_events_unmatched_and_remote_error_only_correlate();
    test_ap_info_request_uses_response_550_and_exact_uid();
    test_cancel_consumes_uid_and_old_reply_is_unmatched();
    test_submit_preflight_refusals_are_transactional();
    test_every_pending_object_pair_alias_refuses();
    test_failed_submission_consumes_or_retires_as_needed();
    test_replacement_epoch_is_never_mutated_by_stale_tx();
    test_replacement_identity_is_not_completed_after_write();
    test_correlator_refuses_bad_metadata_without_mutation();
    test_max_uid_response_finishes_but_never_reuses_uid();
    test_board_discovery_submit_canonical_body_and_checksums();
    test_board_discovery_submit_preflight_refusals_are_atomic();
    test_board_discovery_init_aliases_each_pending_object();
    test_board_discovery_tx_failures_consume_and_retire_once();
    test_board_discovery_tx_does_not_mutate_callback_replacements();
    test_board_discovery_correlation_is_private_and_uid_exact();
    test_normal_pending_path_rejects_private_discovery_ids();
    test_ota_submit_sends_each_private_request_once();
    test_ota_write_size_bounds_include_maximum_envelope();
    test_ota_preflight_rejects_wrong_identity_and_type();
    test_ota_preflight_rejects_noncanonical_and_malformed_bodies();
    test_ota_preflight_bounds_and_alias_refusals_are_atomic();
    test_ota_ambiguous_tx_failure_consumes_uid_without_retry();
    test_ota_callback_replacement_epoch_is_preserved();
    test_ota_cannot_use_normal_control_submission();
    printf("hosted pending TX/correlation: %lu checks passed\n", checks);
    return 0;
}
