/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
/* Fake callbacks count operations only; these tests measure no wall time and
 * make no hardware behavior claim. */
#include "hosted_control.h"
#include "hosted_rpc.h"
#include "hosted_serial.h"
#include "hosted_wire.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_GENERATION UINT32_C(0x4a32)
#define TEST_UID UINT32_C(0x12345678)
#define TEST_POOL_LIMIT 20u
#define TEST_CAPACITY 4096u
#define TEST_MAX_BATCHES 4u
#define TEST_FRAME_CAPACITY 1536u
#define TEST_RAW_CAPACITY P4_HOSTED_CONTROL_MAX_POLLS

static unsigned long checks;

#define CHECK(expression) do { \
    ++checks; \
    if (!(expression)) { \
        fprintf(stderr, "check failed at %s:%d: %s\n", __FILE__, __LINE__, \
                #expression); \
        exit(1); \
    } \
} while (0)

struct rx_batch {
    uint8_t bytes[TEST_CAPACITY];
    size_t length;
};

struct fake_host {
    struct p4_hosted_fifo *state;
    uint32_t generation;
    int session_value;
    int budget_value;
    uint32_t token;
    uint32_t rx_total;

    unsigned session_calls;
    unsigned budget_calls;
    unsigned budget_fail_countdown;
    unsigned tx_token_reads;
    unsigned rx_word_reads;
    unsigned raw_reads;
    unsigned length_reads;
    unsigned rx_fifo_reads;
    unsigned tx_fifo_writes;
    unsigned write_word_calls;
    unsigned yield_calls;

    uint32_t raw_values[TEST_RAW_CAPACITY];
    unsigned raw_value_count;
    uint32_t raw_default;
    struct rx_batch batches[TEST_MAX_BATCHES];
    unsigned batch_count;
    uint32_t counter_override;
    unsigned counter_override_enabled;

    uint32_t raw_last;
    uint32_t length_last;
    uint32_t clear_last;
    uint32_t token_address_last;
    uint32_t token_last;
    uint32_t write_address_last;

    int raw_return;
    int length_return;
    int clear_return;
    int fifo_return;
    int token_return;
    int tx_fifo_return;
    unsigned bad_address;
    unsigned bad_plan;
    unsigned padding_zero;

    unsigned expire_after_raw;
    unsigned expire_after_length;
    unsigned expire_after_fifo;
    unsigned lose_session_after_raw;
    unsigned lose_session_after_fifo;
    unsigned change_generation_after_raw;

    uint8_t tx_capture[TEST_FRAME_CAPACITY];
    size_t tx_capture_size;
    struct p4_hosted_fifo_plan tx_plan;
    struct p4_hosted_tx_completion tx_completion;
};

struct fixture {
    struct p4_hosted_fifo state;
    struct p4_hosted_open_ops rx_ops;
    struct p4_hosted_tx_ops tx_ops;
    uint8_t buffer[TEST_CAPACITY];
    struct p4_hosted_control_result result;
    struct fake_host fake;
};

static void put_le16(uint8_t *bytes, uint16_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
}

static int put_varint(uint8_t *bytes, size_t capacity, size_t *offset,
                      uint64_t value)
{
    do {
        uint8_t byte = (uint8_t)(value & UINT64_C(0x7f));
        value >>= 7;
        if (value != 0)
            byte |= UINT8_C(0x80);
        if (*offset >= capacity)
            return 0;
        bytes[(*offset)++] = byte;
    } while (value != 0);
    return 1;
}

/* Independent, deliberately small protobuf fixture writer. The control
 * tests do not use production RPC, serial, or Hosted encoders for RX input. */
static size_t make_rpc(uint8_t *bytes, size_t capacity, uint32_t type,
                       uint32_t id, uint32_t uid, const uint8_t *payload,
                       size_t payload_size)
{
    size_t offset = 0;
    uint64_t payload_key = ((uint64_t)id << 3) | UINT64_C(2);

    if (!put_varint(bytes, capacity, &offset, 8u) ||
        !put_varint(bytes, capacity, &offset, type) ||
        !put_varint(bytes, capacity, &offset, 16u) ||
        !put_varint(bytes, capacity, &offset, id))
        return 0;
    if (uid != 0 &&
        (!put_varint(bytes, capacity, &offset, 24u) ||
         !put_varint(bytes, capacity, &offset, uid)))
        return 0;
    if (!put_varint(bytes, capacity, &offset, payload_key) ||
        !put_varint(bytes, capacity, &offset, payload_size) ||
        payload_size > capacity - offset)
        return 0;
    if (payload_size != 0) {
        memcpy(bytes + offset, payload, payload_size);
        offset += payload_size;
    }
    return offset;
}

static uint16_t frame_checksum(const uint8_t *bytes, size_t size)
{
    size_t index;
    uint16_t sum = 0;

    for (index = 0; index < size; ++index)
        if (index != 6u && index != 7u)
            sum = (uint16_t)(sum + bytes[index]);
    return sum;
}

static size_t make_frame(uint8_t *bytes, size_t capacity,
                         unsigned interface_type, unsigned interface_number,
                         unsigned flags, unsigned packet_type,
                         unsigned checksum_enabled, const uint8_t *payload,
                         size_t payload_size)
{
    size_t total = P4_HOSTED_HEADER_SIZE + payload_size;

    if (capacity < total || total > P4_HOSTED_FRAME_MAX || payload_size == 0)
        return 0;
    memset(bytes, 0, P4_HOSTED_HEADER_SIZE);
    bytes[0] = (uint8_t)(((interface_number & 15u) << 4) |
                         (interface_type & 15u));
    bytes[1] = (uint8_t)flags;
    put_le16(bytes + 2u, (uint16_t)payload_size);
    put_le16(bytes + 4u, P4_HOSTED_HEADER_SIZE);
    bytes[11] = (uint8_t)packet_type;
    memcpy(bytes + P4_HOSTED_HEADER_SIZE, payload, payload_size);
    if (checksum_enabled)
        put_le16(bytes + 6u, frame_checksum(bytes, total));
    return total;
}

static size_t make_serial_rpc_frame(uint8_t *bytes, size_t capacity,
                                    enum hosted_serial_endpoint endpoint,
                                    uint32_t type, uint32_t id, uint32_t uid,
                                    const uint8_t *body, size_t body_size,
                                    unsigned checksum_enabled,
                                    unsigned interface_type,
                                    unsigned interface_number,
                                    unsigned flags, unsigned packet_type)
{
    uint8_t rpc[512];
    uint8_t serial[HOSTED_SERIAL_MESSAGE_OVERHEAD + sizeof rpc];
    static const uint8_t response_name[HOSTED_SERIAL_ENDPOINT_SIZE] = {
        'R', 'P', 'C', 'R', 's', 'p'
    };
    static const uint8_t event_name[HOSTED_SERIAL_ENDPOINT_SIZE] = {
        'R', 'P', 'C', 'E', 'v', 't'
    };
    const uint8_t *endpoint_name;
    size_t rpc_size, serial_size;

    rpc_size = make_rpc(rpc, sizeof rpc, type, id, uid, body, body_size);
    if (rpc_size == 0 || rpc_size > UINT16_MAX)
        return 0;
    endpoint_name = endpoint == HOSTED_SERIAL_ENDPOINT_RPCRSP ?
        response_name : event_name;
    serial[0] = 1u;
    put_le16(serial + 1u, HOSTED_SERIAL_ENDPOINT_SIZE);
    memcpy(serial + 3u, endpoint_name, HOSTED_SERIAL_ENDPOINT_SIZE);
    serial[9] = 2u;
    put_le16(serial + 10u, (uint16_t)rpc_size);
    memcpy(serial + HOSTED_SERIAL_MESSAGE_OVERHEAD, rpc, rpc_size);
    serial_size = HOSTED_SERIAL_MESSAGE_OVERHEAD + rpc_size;
    return make_frame(bytes, capacity, interface_type, interface_number,
                      flags, packet_type, checksum_enabled, serial,
                      serial_size);
}

static size_t make_mac_body(uint8_t *bytes, size_t capacity,
                            const uint8_t *mac, size_t mac_size,
                            unsigned include_mac, uint32_t status)
{
    size_t offset = 0;

    if (include_mac) {
        if (!put_varint(bytes, capacity, &offset, 10u) ||
            !put_varint(bytes, capacity, &offset, mac_size) ||
            mac_size > capacity - offset)
            return 0;
        if (mac_size != 0) {
            memcpy(bytes + offset, mac, mac_size);
            offset += mac_size;
        }
    }
    if (status != 0 &&
        (!put_varint(bytes, capacity, &offset, 16u) ||
         !put_varint(bytes, capacity, &offset, status)))
        return 0;
    return offset;
}

static int add_batch(struct fake_host *fake, const uint8_t *bytes, size_t size)
{
    struct rx_batch *batch;

    if (fake->batch_count >= TEST_MAX_BATCHES || size == 0 ||
        size > sizeof fake->batches[0].bytes)
        return 0;
    batch = &fake->batches[fake->batch_count++];
    memcpy(batch->bytes, bytes, size);
    batch->length = size;
    return 1;
}

static int append_bytes(uint8_t *destination, size_t capacity, size_t *used,
                        const uint8_t *source, size_t size)
{
    if (size > capacity - *used)
        return 0;
    memcpy(destination + *used, source, size);
    *used += size;
    return 1;
}

static const struct rx_batch *next_batch(const struct fake_host *fake,
                                         unsigned index)
{
    if (fake->batch_count == 0)
        return NULL;
    if (index < fake->batch_count)
        return &fake->batches[index];
    return &fake->batches[fake->batch_count - 1u];
}

static int fake_session_ok(void *context, uint32_t generation)
{
    struct fake_host *fake = (struct fake_host *)context;
    ++fake->session_calls;
    return fake->session_value == 1 && fake->generation == generation;
}

static int fake_budget_ok(void *context)
{
    struct fake_host *fake = (struct fake_host *)context;
    ++fake->budget_calls;
    if (fake->budget_fail_countdown != 0) {
        --fake->budget_fail_countdown;
        if (fake->budget_fail_countdown == 0) {
            fake->budget_value = 0;
            return 0;
        }
    }
    return fake->budget_value == 1;
}

static int fake_tx_read_word(void *context, uint32_t address, uint32_t *value)
{
    struct fake_host *fake = (struct fake_host *)context;

    ++fake->tx_token_reads;
    fake->token_address_last = address;
    fake->token_last = fake->token;
    if (address != P4_HOSTED_TX_TOKEN_ADDRESS || !value)
        fake->bad_address = 1;
    if (value)
        *value = fake->token;
    return fake->token_return;
}

static int fake_rx_read_word(void *context, uint32_t address, uint32_t *value)
{
    struct fake_host *fake = (struct fake_host *)context;

    ++fake->rx_word_reads;
    if (!value) {
        fake->bad_address = 1;
        return 0;
    }
    if (address == P4_HOSTED_OPEN_INT_RAW) {
        unsigned index = fake->raw_reads++;
        if (index < fake->raw_value_count)
            *value = fake->raw_values[index];
        else
            *value = fake->raw_default;
        fake->raw_last = *value;
        if (fake->expire_after_raw)
            fake->budget_fail_countdown = 1u;
        if (fake->lose_session_after_raw)
            fake->session_value = 0;
        if (fake->change_generation_after_raw)
            ++fake->state->generation;
        return fake->raw_return;
    }
    if (address == P4_HOSTED_OPEN_PACKET_LENGTH) {
        const struct rx_batch *batch = next_batch(fake, fake->rx_fifo_reads);
        ++fake->length_reads;
        if (fake->counter_override_enabled)
            *value = fake->counter_override;
        else if (batch)
            *value = (fake->rx_total + (uint32_t)batch->length) &
                     P4_HOSTED_RX_MASK;
        else
            *value = fake->rx_total;
        fake->length_last = *value;
        if (fake->expire_after_length)
            fake->budget_fail_countdown = 2u;
        return fake->length_return;
    }
    fake->bad_address = 1;
    *value = 0;
    return 1;
}

static int fake_write_word(void *context, uint32_t address, uint32_t value)
{
    struct fake_host *fake = (struct fake_host *)context;
    ++fake->write_word_calls;
    fake->write_address_last = address;
    fake->clear_last = value;
    if (address != P4_HOSTED_OPEN_INT_CLEAR)
        fake->bad_address = 1;
    return fake->clear_return;
}

static int fake_read_fifo(void *context,
                          const struct p4_hosted_fifo_plan *plan,
                          uint8_t *buffer)
{
    struct fake_host *fake = (struct fake_host *)context;
    const struct rx_batch *batch = next_batch(fake, fake->rx_fifo_reads);
    uint32_t index;

    ++fake->rx_fifo_reads;
    if (!plan || !buffer || !batch || plan->logical_bytes != batch->length ||
        plan->bus_bytes < plan->logical_bytes ||
        plan->bus_bytes > TEST_CAPACITY) {
        fake->bad_plan = 1;
        return 0;
    }
    memcpy(buffer, batch->bytes, plan->logical_bytes);
    if (fake->fifo_return != 1 && plan->logical_bytes != 0)
        buffer[0] ^= UINT8_C(0x40);
    for (index = plan->logical_bytes; index < plan->bus_bytes; ++index)
        buffer[index] = 0;
    fake->padding_zero = 1;
    for (index = plan->logical_bytes; index < plan->bus_bytes; ++index)
        if (buffer[index] != 0)
            fake->padding_zero = 0;
    fake->rx_total = (fake->rx_total + plan->logical_bytes) &
                     P4_HOSTED_RX_MASK;
    if (fake->expire_after_fifo)
        fake->budget_fail_countdown = 1u;
    if (fake->lose_session_after_fifo)
        fake->session_value = 0;
    return fake->fifo_return;
}

static int fake_write_fifo(void *context,
                           const struct p4_hosted_fifo_plan *plan,
                           const uint8_t *buffer,
                           struct p4_hosted_tx_completion *completion)
{
    struct fake_host *fake = (struct fake_host *)context;

    ++fake->tx_fifo_writes;
    if (!plan || !buffer || !completion ||
        plan->logical_bytes > sizeof fake->tx_capture ||
        plan->bus_bytes < plan->logical_bytes) {
        fake->bad_plan = 1;
        return 0;
    }
    fake->tx_plan = *plan;
    fake->tx_capture_size = plan->logical_bytes;
    memcpy(fake->tx_capture, buffer, fake->tx_capture_size);
    completion->started = 1;
    completion->success = 1;
    completion->completed_bytes = plan->bus_bytes;
    fake->tx_completion = *completion;
    return fake->tx_fifo_return;
}

static void fake_yield(void *context)
{
    struct fake_host *fake = (struct fake_host *)context;
    ++fake->yield_calls;
}

static void fixture_init(struct fixture *fixture)
{
    memset(fixture, 0, sizeof *fixture);
    memset(fixture->buffer, 0xa5, sizeof fixture->buffer);
    memset(&fixture->result, 0xc7, sizeof fixture->result);
    CHECK(p4_hosted_fifo_start(&fixture->state, TEST_GENERATION,
                               TEST_POOL_LIMIT));
    fixture->fake.state = &fixture->state;
    fixture->fake.generation = TEST_GENERATION;
    fixture->fake.session_value = 1;
    fixture->fake.budget_value = 1;
    fixture->fake.token = UINT32_C(1) << 16;
    fixture->fake.raw_return = 1;
    fixture->fake.length_return = 1;
    fixture->fake.clear_return = 1;
    fixture->fake.fifo_return = 1;
    fixture->fake.token_return = 1;
    fixture->fake.tx_fifo_return = 1;

    fixture->rx_ops.context = &fixture->fake;
    fixture->rx_ops.read_word = fake_rx_read_word;
    fixture->rx_ops.write_word = fake_write_word;
    fixture->rx_ops.read_fifo = fake_read_fifo;
    fixture->rx_ops.yield = fake_yield;

    fixture->tx_ops.context = &fixture->fake;
    fixture->tx_ops.read_word = fake_tx_read_word;
    fixture->tx_ops.write_fifo = fake_write_fifo;
    fixture->tx_ops.session_ok = fake_session_ok;
    fixture->tx_ops.budget_ok = fake_budget_ok;
    fixture->tx_ops.yield = fake_yield;
}

static int call_gate(struct fixture *fixture, uint32_t generation,
                     const struct p4_hosted_open_ops *rx_ops,
                     const struct p4_hosted_tx_ops *tx_ops,
                     unsigned checksum_enabled, uint32_t uid,
                     uint8_t *buffer, size_t capacity,
                     struct p4_hosted_control_result *result)
{
    return p4_hosted_control_get_mac(&fixture->state, generation, rx_ops,
                                     tx_ops, checksum_enabled, uid, buffer,
                                     capacity, result);
}

static size_t make_expected_request(uint8_t *bytes, size_t capacity,
                                    uint32_t uid, unsigned checksum_enabled)
{
    uint8_t protobuf[32];
    uint8_t serial[64];
    size_t protobuf_size = 0, serial_size, frame_size;

    /* Exact canonical RpcReq_GetMACAddress request, with proto3 STA mode
     * omitted as its zero default. */
    if (capacity < P4_HOSTED_HEADER_SIZE + HOSTED_SERIAL_MESSAGE_OVERHEAD ||
        !put_varint(protobuf, sizeof protobuf, &protobuf_size, 8u) ||
        !put_varint(protobuf, sizeof protobuf, &protobuf_size, 1u) ||
        !put_varint(protobuf, sizeof protobuf, &protobuf_size, 16u) ||
        !put_varint(protobuf, sizeof protobuf, &protobuf_size, 257u) ||
        !put_varint(protobuf, sizeof protobuf, &protobuf_size, 24u) ||
        !put_varint(protobuf, sizeof protobuf, &protobuf_size, uid) ||
        !put_varint(protobuf, sizeof protobuf, &protobuf_size,
                    ((uint64_t)257u << 3) | 2u) ||
        !put_varint(protobuf, sizeof protobuf, &protobuf_size, 0u))
        return 0;

    serial[0] = 1u;
    put_le16(serial + 1u, HOSTED_SERIAL_ENDPOINT_SIZE);
    memcpy(serial + 3u, "RPCRsp", HOSTED_SERIAL_ENDPOINT_SIZE);
    serial[9] = 2u;
    put_le16(serial + 10u, (uint16_t)protobuf_size);
    memcpy(serial + HOSTED_SERIAL_MESSAGE_OVERHEAD, protobuf, protobuf_size);
    serial_size = HOSTED_SERIAL_MESSAGE_OVERHEAD + protobuf_size;
    frame_size = make_frame(bytes, capacity, P4_HOSTED_SERIAL, 0, 0, 0,
                            checksum_enabled, serial, serial_size);
    return frame_size;
}

static void verify_single_request(struct fixture *fixture, uint32_t uid,
                                  unsigned checksum_enabled)
{
    uint8_t expected[TEST_FRAME_CAPACITY];
    size_t expected_size;
    struct p4_hosted_frame frame;
    struct hosted_serial_message serial;
    struct p4_hosted_rpc rpc;

    expected_size = make_expected_request(expected, sizeof expected, uid,
                                          checksum_enabled);
    CHECK(expected_size != 0);
    CHECK(fixture->fake.tx_fifo_writes == 1u);
    CHECK(fixture->fake.tx_token_reads == 1u);
    CHECK(fixture->fake.token_address_last == P4_HOSTED_TX_TOKEN_ADDRESS);
    CHECK(fixture->fake.token_last == fixture->fake.token);
    CHECK(fixture->fake.token == (UINT32_C(1) << 16));
    CHECK(fixture->result.tx.token == fixture->fake.token);
    CHECK(fixture->result.tx.write_called == 1u);
    CHECK(fixture->result.tx.completion_well_formed == 1u);
    CHECK(fixture->result.tx.success == 1u);
    CHECK(fixture->fake.tx_capture_size == expected_size);
    CHECK(memcmp(fixture->fake.tx_capture, expected, expected_size) == 0);
    CHECK(fixture->fake.tx_plan.logical_bytes == expected_size);
    CHECK(fixture->fake.tx_plan.bus_bytes == 512u);
    CHECK(fixture->fake.tx_plan.credits == 1u);
    CHECK(fixture->fake.tx_completion.started == 1u);
    CHECK(fixture->fake.tx_completion.success == 1u);
    CHECK(fixture->fake.tx_completion.completed_bytes == 512u);
    CHECK(fixture->fake.bad_address == 0u);
    CHECK(fixture->fake.bad_plan == 0u);

    CHECK(p4_hosted_decode(fixture->fake.tx_capture,
                           fixture->fake.tx_capture_size,
                           checksum_enabled, &frame));
    CHECK(frame.interface_type == P4_HOSTED_SERIAL);
    CHECK(frame.interface_number == 0u);
    CHECK(frame.flags == 0u);
    CHECK(frame.throttle == 0u);
    CHECK(frame.packet_type == 0u);
    CHECK(frame.sequence == 0u);
    CHECK(hosted_serial_decode(frame.payload, frame.payload_size, &serial));
    CHECK(serial.endpoint == HOSTED_SERIAL_ENDPOINT_RPCRSP);
    CHECK(p4_hosted_rpc_decode(serial.protobuf, serial.protobuf_size, &rpc));
    CHECK(rpc.msg_type == P4_HOSTED_RPC_TYPE_REQUEST);
    CHECK(rpc.msg_id == 257u);
    CHECK(rpc.uid == uid);
    CHECK(rpc.payload_size == 0u);
}

static void set_raws(struct fake_host *fake, const uint32_t *values,
                     unsigned count)
{
    unsigned index;
    CHECK(count <= TEST_RAW_CAPACITY);
    fake->raw_value_count = count;
    for (index = 0; index < count; ++index)
        fake->raw_values[index] = values[index];
}

static size_t make_response(uint8_t *frame, size_t capacity, uint32_t uid,
                            uint32_t id, const uint8_t *mac, size_t mac_size,
                            unsigned include_mac, uint32_t status,
                            unsigned checksum_enabled)
{
    uint8_t body[64];
    size_t body_size = make_mac_body(body, sizeof body, mac, mac_size,
                                     include_mac, status);
    if (body_size == 0 && !include_mac && status == 0)
        return 0;
    return make_serial_rpc_frame(frame, capacity,
        HOSTED_SERIAL_ENDPOINT_RPCRSP, P4_HOSTED_RPC_TYPE_RESPONSE, id, uid,
        body, body_size, checksum_enabled, P4_HOSTED_SERIAL, 0, 0, 0);
}

static void verify_success_state(struct fixture *fixture, size_t rx_bytes)
{
    CHECK(fixture->state.initialized == 1u);
    CHECK(fixture->state.generation == TEST_GENERATION);
    CHECK(fixture->state.pool_limit == TEST_POOL_LIMIT);
    CHECK(fixture->state.tx_sent == 1u);
    CHECK(fixture->state.rx_consumed == (uint32_t)rx_bytes);
    CHECK(fixture->state.busy == 0u);
    CHECK(fixture->state.poisoned == 0u);
    CHECK(fixture->fake.padding_zero == 1u);
    CHECK(fixture->fake.bad_plan == 0u);
}

static void test_idle_then_response(void)
{
    static const uint8_t mac[6] = { 0x02u, 0x44u, 0x55u, 0x66u, 0x77u, 0x88u };
    struct fixture fixture;
    uint8_t response[TEST_FRAME_CAPACITY];
    uint32_t raw[] = { 0u, P4_HOSTED_OPEN_INT_PACKET | (1u << 7) |
                              (1u << 6) | (1u << 4) };
    size_t response_size;

    fixture_init(&fixture);
    response_size = make_response(response, sizeof response, TEST_UID, 513u,
                                  mac, sizeof mac, 1u, 0u, 0u);
    CHECK(response_size != 0u);
    CHECK(add_batch(&fixture.fake, response, response_size));
    set_raws(&fixture.fake, raw, 2u);

    CHECK(call_gate(&fixture, TEST_GENERATION, &fixture.rx_ops,
                    &fixture.tx_ops, 0u, TEST_UID, fixture.buffer,
                    sizeof fixture.buffer, &fixture.result) == 1);
    CHECK(fixture.result.error == P4_HOSTED_CONTROL_OK);
    CHECK(fixture.result.success == 1u);
    CHECK(fixture.result.matched == 1u);
    CHECK(fixture.result.polls == 2u);
    CHECK(fixture.result.batches == 1u);
    CHECK(fixture.result.events == 0u);
    CHECK(fixture.result.unmatched == 0u);
    CHECK(fixture.result.mac.status == 0);
    CHECK(fixture.result.mac.valid_mac == 1u);
    CHECK(memcmp(fixture.result.mac.mac, mac, sizeof mac) == 0);
    CHECK(fixture.fake.raw_reads == 2u);
    CHECK(fixture.fake.rx_fifo_reads == 1u);
    CHECK(fixture.fake.yield_calls == 1u);
    CHECK(fixture.fake.write_word_calls == 1u);
    CHECK(fixture.fake.write_address_last == P4_HOSTED_OPEN_INT_CLEAR);
    CHECK(fixture.fake.clear_last == (P4_HOSTED_OPEN_INT_PACKET |
                                      (1u << 7) | (1u << 6)));
    verify_single_request(&fixture, TEST_UID, 0u);
    verify_success_state(&fixture, response_size);

    /* The copied result remains valid after the receive bytes are overwritten. */
    memset(fixture.buffer, 0, sizeof fixture.buffer);
    CHECK(memcmp(fixture.result.mac.mac, mac, sizeof mac) == 0);
}

static void test_event_then_response(void)
{
    static const uint8_t mac[6] = { 0x02u, 0x11u, 0x22u, 0x33u, 0x44u, 0x55u };
    static const uint8_t opaque_event_body[] = { 0xffu, 0x00u, 0x81u };
    struct fixture fixture;
    uint8_t event[TEST_FRAME_CAPACITY], response[TEST_FRAME_CAPACITY];
    uint32_t raw[] = { P4_HOSTED_OPEN_INT_PACKET, P4_HOSTED_OPEN_INT_PACKET };
    size_t event_size, response_size;

    fixture_init(&fixture);
    event_size = make_serial_rpc_frame(event, sizeof event,
        HOSTED_SERIAL_ENDPOINT_RPCEVT, P4_HOSTED_RPC_TYPE_EVENT, 769u, 0u,
        opaque_event_body, sizeof opaque_event_body, 0u, P4_HOSTED_SERIAL,
        0u, 0u, 0u);
    response_size = make_response(response, sizeof response, TEST_UID, 513u,
                                  mac, sizeof mac, 1u, 0u, 0u);
    CHECK(event_size != 0u);
    CHECK(response_size != 0u);
    CHECK(add_batch(&fixture.fake, event, event_size));
    CHECK(add_batch(&fixture.fake, response, response_size));
    set_raws(&fixture.fake, raw, 2u);

    CHECK(call_gate(&fixture, TEST_GENERATION, &fixture.rx_ops,
                    &fixture.tx_ops, 0u, TEST_UID, fixture.buffer,
                    sizeof fixture.buffer, &fixture.result) == 1);
    CHECK(fixture.result.events == 1u);
    CHECK(fixture.result.batches == 2u);
    CHECK(fixture.result.polls == 2u);
    CHECK(fixture.result.success == 1u);
    CHECK(fixture.result.unmatched == 0u);
    CHECK(fixture.state.rx_consumed == (uint32_t)(event_size + response_size));
    verify_single_request(&fixture, TEST_UID, 0u);
    verify_success_state(&fixture, event_size + response_size);
}

static void test_unmatched_uid_and_operation(void)
{
    static const uint8_t mac[6] = { 0x02u, 0xaau, 0xbbu, 0xccu, 0xddu, 0xeeu };
    struct fixture fixture;
    uint8_t batch[TEST_CAPACITY], frame[TEST_FRAME_CAPACITY];
    size_t used = 0, frame_size;
    uint32_t raw[] = { P4_HOSTED_OPEN_INT_PACKET };

    fixture_init(&fixture);
    frame_size = make_response(frame, sizeof frame, TEST_UID + 1u, 513u,
                               mac, sizeof mac, 1u, 0u, 0u);
    CHECK(frame_size != 0u && append_bytes(batch, sizeof batch, &used,
                                           frame, frame_size));
    frame_size = make_response(frame, sizeof frame, TEST_UID, 514u,
                               mac, sizeof mac, 1u, 0u, 0u);
    CHECK(frame_size != 0u && append_bytes(batch, sizeof batch, &used,
                                           frame, frame_size));
    frame_size = make_response(frame, sizeof frame, TEST_UID, 513u,
                               mac, sizeof mac, 1u, 0u, 0u);
    CHECK(frame_size != 0u && append_bytes(batch, sizeof batch, &used,
                                           frame, frame_size));
    CHECK(add_batch(&fixture.fake, batch, used));
    set_raws(&fixture.fake, raw, 1u);

    CHECK(call_gate(&fixture, TEST_GENERATION, &fixture.rx_ops,
                    &fixture.tx_ops, 0u, TEST_UID, fixture.buffer,
                    sizeof fixture.buffer, &fixture.result) == 1);
    CHECK(fixture.result.unmatched == 2u);
    CHECK(fixture.result.matched == 1u);
    CHECK(fixture.result.success == 1u);
    CHECK(fixture.result.batches == 1u);
    CHECK(fixture.state.rx_consumed == (uint32_t)used);
    verify_single_request(&fixture, TEST_UID, 0u);
}

static void test_trailing_malformed_and_duplicate_match(void)
{
    static const uint8_t mac[6] = { 0x02u, 0x01u, 0x02u, 0x03u, 0x04u, 0x05u };
    struct fixture fixture;
    uint8_t batch[TEST_CAPACITY], response[TEST_FRAME_CAPACITY];
    uint8_t malformed[P4_HOSTED_HEADER_SIZE] = { 0 };
    size_t response_size, used;
    uint32_t raw[] = { P4_HOSTED_OPEN_INT_PACKET };

    fixture_init(&fixture);
    response_size = make_response(response, sizeof response, TEST_UID, 513u,
                                  mac, sizeof mac, 1u, 0u, 0u);
    CHECK(response_size != 0u);
    used = 0;
    CHECK(append_bytes(batch, sizeof batch, &used, response, response_size));
    CHECK(append_bytes(batch, sizeof batch, &used, malformed,
                       sizeof malformed));
    CHECK(add_batch(&fixture.fake, batch, used));
    set_raws(&fixture.fake, raw, 1u);

    CHECK(call_gate(&fixture, TEST_GENERATION, &fixture.rx_ops,
                    &fixture.tx_ops, 0u, TEST_UID, fixture.buffer,
                    sizeof fixture.buffer, &fixture.result) == 0);
    CHECK(fixture.result.error == P4_HOSTED_CONTROL_PROTOCOL);
    CHECK(fixture.result.matched == 1u);
    CHECK(fixture.result.success == 0u);
    CHECK(fixture.result.batches == 1u);
    CHECK(fixture.state.rx_consumed == (uint32_t)used);
    CHECK(fixture.state.busy == 0u);
    CHECK(fixture.state.poisoned == 1u);
    CHECK(fixture.fake.rx_fifo_reads == 1u);
    verify_single_request(&fixture, TEST_UID, 0u);

    fixture_init(&fixture);
    used = 0;
    CHECK(append_bytes(batch, sizeof batch, &used, response, response_size));
    CHECK(append_bytes(batch, sizeof batch, &used, response, response_size));
    CHECK(add_batch(&fixture.fake, batch, used));
    set_raws(&fixture.fake, raw, 1u);
    CHECK(call_gate(&fixture, TEST_GENERATION, &fixture.rx_ops,
                    &fixture.tx_ops, 0u, TEST_UID, fixture.buffer,
                    sizeof fixture.buffer, &fixture.result) == 0);
    CHECK(fixture.result.error == P4_HOSTED_CONTROL_PROTOCOL);
    CHECK(fixture.result.matched == 1u);
    CHECK(fixture.state.poisoned == 1u);
    CHECK(fixture.fake.rx_fifo_reads == 1u);
    verify_single_request(&fixture, TEST_UID, 0u);

    {
        uint8_t body[32];
        size_t body_size;

        fixture_init(&fixture);
        body_size = make_mac_body(body, sizeof body, mac, sizeof mac, 1u, 0u);
        CHECK(body_size != 0u);
        CHECK(append_bytes(body, sizeof body, &body_size, body, 8u));
        response_size = make_serial_rpc_frame(response, sizeof response,
            HOSTED_SERIAL_ENDPOINT_RPCRSP, P4_HOSTED_RPC_TYPE_RESPONSE,
            513u, TEST_UID, body, body_size, 0u, P4_HOSTED_SERIAL, 0u,
            0u, 0u);
        CHECK(response_size != 0u);
        CHECK(add_batch(&fixture.fake, response, response_size));
        set_raws(&fixture.fake, raw, 1u);
        CHECK(call_gate(&fixture, TEST_GENERATION, &fixture.rx_ops,
                        &fixture.tx_ops, 0u, TEST_UID, fixture.buffer,
                        sizeof fixture.buffer, &fixture.result) == 0);
        CHECK(fixture.result.error == P4_HOSTED_CONTROL_PROTOCOL);
        CHECK(fixture.result.matched == 0u);
        CHECK(fixture.state.poisoned == 1u);
        CHECK(fixture.fake.rx_fifo_reads == 1u);
        verify_single_request(&fixture, TEST_UID, 0u);
    }
}

static void test_remote_error_and_invalid_address(void)
{
    struct fixture fixture;
    uint8_t response[TEST_FRAME_CAPACITY];
    uint8_t mac[6] = { 0x03u, 0x20u, 0x30u, 0x40u, 0x50u, 0x60u };
    uint32_t raw[] = { P4_HOSTED_OPEN_INT_PACKET };
    size_t response_size;

    fixture_init(&fixture);
    response_size = make_response(response, sizeof response, TEST_UID, 513u,
                                  NULL, 0u, 0u, 7u, 0u);
    CHECK(response_size != 0u);
    CHECK(add_batch(&fixture.fake, response, response_size));
    set_raws(&fixture.fake, raw, 1u);
    CHECK(call_gate(&fixture, TEST_GENERATION, &fixture.rx_ops,
                    &fixture.tx_ops, 0u, TEST_UID, fixture.buffer,
                    sizeof fixture.buffer, &fixture.result) == 0);
    CHECK(fixture.result.error == P4_HOSTED_CONTROL_REMOTE);
    CHECK(fixture.result.matched == 1u);
    CHECK(fixture.result.mac.status == 7);
    CHECK(fixture.result.mac.valid_mac == 0u);
    CHECK(fixture.state.poisoned == 0u);
    CHECK(fixture.state.busy == 0u);
    CHECK(fixture.state.rx_consumed == (uint32_t)response_size);
    verify_single_request(&fixture, TEST_UID, 0u);

    fixture_init(&fixture);
    response_size = make_response(response, sizeof response, TEST_UID, 513u,
                                  mac, sizeof mac, 1u, 0u, 0u);
    CHECK(response_size != 0u);
    CHECK(add_batch(&fixture.fake, response, response_size));
    set_raws(&fixture.fake, raw, 1u);
    CHECK(call_gate(&fixture, TEST_GENERATION, &fixture.rx_ops,
                    &fixture.tx_ops, 0u, TEST_UID, fixture.buffer,
                    sizeof fixture.buffer, &fixture.result) == 0);
    CHECK(fixture.result.error == P4_HOSTED_CONTROL_REMOTE);
    CHECK(fixture.result.matched == 1u);
    CHECK(fixture.result.mac.status == 0);
    CHECK(fixture.result.mac.valid_mac == 0u);
    CHECK(fixture.state.poisoned == 0u);
    verify_single_request(&fixture, TEST_UID, 0u);
}

static void test_timeout_and_unmatched_bound(void)
{
    struct fixture fixture;
    uint8_t unmatched[TEST_FRAME_CAPACITY];
    uint8_t payload[] = { 0x0au, 0x00u };
    size_t unmatched_size;

    fixture_init(&fixture);
    CHECK(call_gate(&fixture, TEST_GENERATION, &fixture.rx_ops,
                    &fixture.tx_ops, 0u, TEST_UID, fixture.buffer,
                    sizeof fixture.buffer, &fixture.result) == 0);
    CHECK(fixture.result.error == P4_HOSTED_CONTROL_TIMEOUT);
    CHECK(fixture.result.polls == P4_HOSTED_CONTROL_MAX_POLLS);
    CHECK(fixture.result.batches == 0u);
    CHECK(fixture.fake.raw_reads == P4_HOSTED_CONTROL_MAX_POLLS);
    CHECK(fixture.fake.yield_calls == P4_HOSTED_CONTROL_MAX_POLLS - 1u);
    CHECK(fixture.fake.rx_fifo_reads == 0u);
    CHECK(fixture.fake.tx_fifo_writes == 1u);
    CHECK(fixture.state.tx_sent == 1u);
    CHECK(fixture.state.rx_consumed == 0u);
    CHECK(fixture.state.poisoned == 0u);
    verify_single_request(&fixture, TEST_UID, 0u);

    fixture_init(&fixture);
    unmatched_size = make_serial_rpc_frame(unmatched, sizeof unmatched,
        HOSTED_SERIAL_ENDPOINT_RPCRSP, P4_HOSTED_RPC_TYPE_RESPONSE, 513u,
        TEST_UID + 1u, payload, sizeof payload, 0u, P4_HOSTED_SERIAL,
        0u, 0u, 0u);
    CHECK(unmatched_size != 0u);
    CHECK(add_batch(&fixture.fake, unmatched, unmatched_size));
    fixture.fake.raw_default = P4_HOSTED_OPEN_INT_PACKET;
    CHECK(call_gate(&fixture, TEST_GENERATION, &fixture.rx_ops,
                    &fixture.tx_ops, 0u, TEST_UID, fixture.buffer,
                    sizeof fixture.buffer, &fixture.result) == 0);
    CHECK(fixture.result.error == P4_HOSTED_CONTROL_TIMEOUT);
    CHECK(fixture.result.polls == P4_HOSTED_CONTROL_MAX_POLLS);
    CHECK(fixture.result.batches == P4_HOSTED_CONTROL_MAX_POLLS);
    CHECK(fixture.result.unmatched == P4_HOSTED_CONTROL_MAX_POLLS);
    CHECK(fixture.fake.rx_fifo_reads == P4_HOSTED_CONTROL_MAX_POLLS);
    CHECK(fixture.fake.yield_calls == P4_HOSTED_CONTROL_MAX_POLLS - 1u);
    CHECK(fixture.state.rx_consumed ==
          (uint32_t)(unmatched_size * P4_HOSTED_CONTROL_MAX_POLLS));
    CHECK(fixture.state.poisoned == 0u);
    CHECK(fixture.fake.tx_fifo_writes == 1u);
    verify_single_request(&fixture, TEST_UID, 0u);
}

static void test_peer_faults(void)
{
    const uint32_t faults[] = {
        P4_HOSTED_OPEN_INT_RX_UNDERFLOW,
        P4_HOSTED_OPEN_INT_TX_OVERFLOW
    };
    unsigned index;

    for (index = 0; index < sizeof faults / sizeof faults[0]; ++index) {
        struct fixture fixture;
        uint32_t raw[] = { faults[index] | P4_HOSTED_OPEN_INT_PACKET |
                           (1u << 7) };
        fixture_init(&fixture);
        set_raws(&fixture.fake, raw, 1u);
        CHECK(call_gate(&fixture, TEST_GENERATION, &fixture.rx_ops,
                        &fixture.tx_ops, 0u, TEST_UID, fixture.buffer,
                        sizeof fixture.buffer, &fixture.result) == 0);
        CHECK(fixture.result.error == P4_HOSTED_CONTROL_PEER_FAULT);
        CHECK(fixture.result.polls == 1u);
        CHECK(fixture.fake.raw_reads == 1u);
        CHECK(fixture.fake.write_word_calls == 0u);
        CHECK(fixture.fake.rx_fifo_reads == 0u);
        CHECK(fixture.fake.yield_calls == 0u);
        CHECK(fixture.state.poisoned == 1u);
        CHECK(fixture.state.rx_consumed == 0u);
        CHECK(fixture.fake.tx_fifo_writes == 1u);
        verify_single_request(&fixture, TEST_UID, 0u);
    }
}

static void test_wrapped_counter_and_capacity_refusal(void)
{
    static const uint8_t mac[6] = { 0x02u, 0x12u, 0x34u, 0x56u, 0x78u, 0x9au };
    struct fixture fixture;
    uint8_t response[TEST_FRAME_CAPACITY];
    uint32_t raw[] = { P4_HOSTED_OPEN_INT_PACKET };
    size_t response_size;
    uint32_t initial;

    fixture_init(&fixture);
    response_size = make_response(response, sizeof response, TEST_UID, 513u,
                                  mac, sizeof mac, 1u, 0u, 0u);
    CHECK(response_size != 0u);
    initial = P4_HOSTED_RX_MASK - 17u;
    fixture.state.rx_consumed = initial;
    fixture.fake.rx_total = initial;
    CHECK(add_batch(&fixture.fake, response, response_size));
    set_raws(&fixture.fake, raw, 1u);
    CHECK(call_gate(&fixture, TEST_GENERATION, &fixture.rx_ops,
                    &fixture.tx_ops, 0u, TEST_UID, fixture.buffer,
                    sizeof fixture.buffer, &fixture.result) == 1);
    CHECK(fixture.fake.length_last ==
          ((initial + (uint32_t)response_size) & P4_HOSTED_RX_MASK));
    CHECK(fixture.state.rx_consumed ==
          ((initial + (uint32_t)response_size) & P4_HOSTED_RX_MASK));
    CHECK(fixture.result.success == 1u);
    CHECK(fixture.state.poisoned == 0u);
    verify_single_request(&fixture, TEST_UID, 0u);

    fixture_init(&fixture);
    fixture.fake.counter_override_enabled = 1u;
    fixture.fake.counter_override = 4097u;
    set_raws(&fixture.fake, raw, 1u);
    CHECK(call_gate(&fixture, TEST_GENERATION, &fixture.rx_ops,
                    &fixture.tx_ops, 0u, TEST_UID, fixture.buffer,
                    sizeof fixture.buffer, &fixture.result) == 0);
    CHECK(fixture.result.error == P4_HOSTED_CONTROL_CAPACITY);
    CHECK(fixture.fake.rx_fifo_reads == 0u);
    CHECK(fixture.state.busy == 0u);
    CHECK(fixture.state.poisoned == 0u);
    CHECK(fixture.state.rx_consumed == 0u);
    CHECK(fixture.fake.tx_fifo_writes == 1u);
    verify_single_request(&fixture, TEST_UID, 0u);

    fixture_init(&fixture);
    fixture.fake.counter_override_enabled = 1u;
    fixture.fake.counter_override = P4_HOSTED_FIFO_BUS_MAX + 1u;
    set_raws(&fixture.fake, raw, 1u);
    CHECK(call_gate(&fixture, TEST_GENERATION, &fixture.rx_ops,
                    &fixture.tx_ops, 0u, TEST_UID, fixture.buffer,
                    sizeof fixture.buffer, &fixture.result) == 0);
    CHECK(fixture.result.error == P4_HOSTED_CONTROL_COUNTER);
    CHECK(fixture.fake.rx_fifo_reads == 0u);
    CHECK(fixture.state.busy == 0u);
    CHECK(fixture.state.poisoned == 1u);
    CHECK(fixture.state.rx_consumed == 0u);
    verify_single_request(&fixture, TEST_UID, 0u);
}

static void test_budget_session_and_ambiguous_rx(void)
{
    static const uint8_t mac[6] = { 0x02u, 0x31u, 0x32u, 0x33u, 0x34u, 0x35u };
    struct fixture fixture;
    uint8_t response[TEST_FRAME_CAPACITY];
    uint32_t raw[] = { P4_HOSTED_OPEN_INT_PACKET };
    size_t response_size;

    response_size = make_response(response, sizeof response, TEST_UID, 513u,
                                  mac, sizeof mac, 1u, 0u, 0u);
    CHECK(response_size != 0u);

    fixture_init(&fixture);
    CHECK(add_batch(&fixture.fake, response, response_size));
    set_raws(&fixture.fake, raw, 1u);
    fixture.fake.expire_after_raw = 1u;
    CHECK(call_gate(&fixture, TEST_GENERATION, &fixture.rx_ops,
                    &fixture.tx_ops, 0u, TEST_UID, fixture.buffer,
                    sizeof fixture.buffer, &fixture.result) == 0);
    CHECK(fixture.result.error == P4_HOSTED_CONTROL_BUDGET);
    CHECK(fixture.fake.rx_fifo_reads == 0u);
    CHECK(fixture.state.rx_consumed == 0u);
    CHECK(fixture.state.busy == 0u);
    CHECK(fixture.state.poisoned == 0u);
    CHECK(fixture.fake.write_word_calls == 0u);
    verify_single_request(&fixture, TEST_UID, 0u);

    fixture_init(&fixture);
    CHECK(add_batch(&fixture.fake, response, response_size));
    set_raws(&fixture.fake, raw, 1u);
    fixture.fake.change_generation_after_raw = 1u;
    CHECK(call_gate(&fixture, TEST_GENERATION, &fixture.rx_ops,
                    &fixture.tx_ops, 0u, TEST_UID, fixture.buffer,
                    sizeof fixture.buffer, &fixture.result) == 0);
    CHECK(fixture.result.error == P4_HOSTED_CONTROL_SESSION);
    CHECK(fixture.fake.rx_fifo_reads == 0u);
    CHECK(fixture.state.rx_consumed == 0u);
    CHECK(fixture.state.generation == TEST_GENERATION + 1u);
    CHECK(fixture.state.poisoned == 1u);
    verify_single_request(&fixture, TEST_UID, 0u);

    fixture_init(&fixture);
    CHECK(add_batch(&fixture.fake, response, response_size));
    set_raws(&fixture.fake, raw, 1u);
    fixture.fake.expire_after_length = 1u;
    CHECK(call_gate(&fixture, TEST_GENERATION, &fixture.rx_ops,
                    &fixture.tx_ops, 0u, TEST_UID, fixture.buffer,
                    sizeof fixture.buffer, &fixture.result) == 0);
    CHECK(fixture.result.error == P4_HOSTED_CONTROL_BUDGET);
    CHECK(fixture.fake.rx_fifo_reads == 0u);
    CHECK(fixture.state.busy == 0u);
    CHECK(fixture.state.poisoned == 0u);
    CHECK(fixture.state.rx_consumed == 0u);
    verify_single_request(&fixture, TEST_UID, 0u);

    fixture_init(&fixture);
    set_raws(&fixture.fake, raw, 1u);
    fixture.fake.raw_return = 0;
    CHECK(call_gate(&fixture, TEST_GENERATION, &fixture.rx_ops,
                    &fixture.tx_ops, 0u, TEST_UID, fixture.buffer,
                    sizeof fixture.buffer, &fixture.result) == 0);
    CHECK(fixture.result.error == P4_HOSTED_CONTROL_REGISTER);
    CHECK(fixture.fake.raw_reads == 1u);
    CHECK(fixture.fake.rx_fifo_reads == 0u);
    CHECK(fixture.fake.write_word_calls == 0u);
    CHECK(fixture.state.rx_consumed == 0u);
    CHECK(fixture.state.busy == 0u);
    CHECK(fixture.state.poisoned == 0u);
    verify_single_request(&fixture, TEST_UID, 0u);

    fixture_init(&fixture);
    CHECK(add_batch(&fixture.fake, response, response_size));
    set_raws(&fixture.fake, raw, 1u);
    fixture.fake.fifo_return = 0;
    CHECK(call_gate(&fixture, TEST_GENERATION, &fixture.rx_ops,
                    &fixture.tx_ops, 0u, TEST_UID, fixture.buffer,
                    sizeof fixture.buffer, &fixture.result) == 0);
    CHECK(fixture.result.error == P4_HOSTED_CONTROL_RX);
    CHECK(fixture.fake.rx_fifo_reads == 1u);
    CHECK(fixture.state.busy == 0u);
    CHECK(fixture.state.poisoned == 1u);
    CHECK(fixture.state.rx_consumed == 0u);
    verify_single_request(&fixture, TEST_UID, 0u);

    fixture_init(&fixture);
    CHECK(add_batch(&fixture.fake, response, response_size));
    set_raws(&fixture.fake, raw, 1u);
    fixture.fake.expire_after_fifo = 1u;
    CHECK(call_gate(&fixture, TEST_GENERATION, &fixture.rx_ops,
                    &fixture.tx_ops, 0u, TEST_UID, fixture.buffer,
                    sizeof fixture.buffer, &fixture.result) == 0);
    CHECK(fixture.result.error == P4_HOSTED_CONTROL_BUDGET);
    CHECK(fixture.fake.rx_fifo_reads == 1u);
    CHECK(fixture.state.rx_consumed == 0u);
    CHECK(fixture.state.busy == 0u);
    CHECK(fixture.state.poisoned == 1u);
    verify_single_request(&fixture, TEST_UID, 0u);

    fixture_init(&fixture);
    CHECK(add_batch(&fixture.fake, response, response_size));
    set_raws(&fixture.fake, raw, 1u);
    fixture.fake.lose_session_after_fifo = 1u;
    CHECK(call_gate(&fixture, TEST_GENERATION, &fixture.rx_ops,
                    &fixture.tx_ops, 0u, TEST_UID, fixture.buffer,
                    sizeof fixture.buffer, &fixture.result) == 0);
    CHECK(fixture.result.error == P4_HOSTED_CONTROL_SESSION);
    CHECK(fixture.fake.rx_fifo_reads == 1u);
    CHECK(fixture.state.rx_consumed == 0u);
    CHECK(fixture.state.busy == 0u);
    CHECK(fixture.state.poisoned == 1u);
    verify_single_request(&fixture, TEST_UID, 0u);
}

static void test_endpoint_profile_and_checksum(void)
{
    static const uint8_t mac[6] = { 0x02u, 0x40u, 0x41u, 0x42u, 0x43u, 0x44u };
    struct fixture fixture;
    uint8_t frame[TEST_FRAME_CAPACITY];
    size_t frame_size;
    uint32_t raw[] = { P4_HOSTED_OPEN_INT_PACKET };
    unsigned variant;

    for (variant = 0; variant < 5u; ++variant) {
        unsigned interface_type = P4_HOSTED_SERIAL;
        unsigned interface_number = 0u;
        unsigned flags = 0u;
        unsigned packet_type = 0u;
        enum hosted_serial_endpoint endpoint = HOSTED_SERIAL_ENDPOINT_RPCRSP;
        uint32_t type = P4_HOSTED_RPC_TYPE_RESPONSE;
        fixture_init(&fixture);
        if (variant == 0u)
            endpoint = HOSTED_SERIAL_ENDPOINT_RPCEVT;
        else if (variant == 1u)
            type = P4_HOSTED_RPC_TYPE_EVENT;
        else if (variant == 2u)
            interface_number = 1u;
        else if (variant == 3u)
            flags = P4_HOSTED_MORE_FRAGMENT;
        else if (variant == 4u)
            interface_type = P4_HOSTED_STA;
        frame_size = make_serial_rpc_frame(frame, sizeof frame, endpoint,
            type, type == P4_HOSTED_RPC_TYPE_EVENT ? 769u : 513u,
            type == P4_HOSTED_RPC_TYPE_EVENT ? 0u : TEST_UID,
            mac, sizeof mac, 0u, interface_type, interface_number, flags,
            packet_type);
        CHECK(frame_size != 0u);
        CHECK(add_batch(&fixture.fake, frame, frame_size));
        set_raws(&fixture.fake, raw, 1u);
        CHECK(call_gate(&fixture, TEST_GENERATION, &fixture.rx_ops,
                        &fixture.tx_ops, 0u, TEST_UID, fixture.buffer,
                        sizeof fixture.buffer, &fixture.result) == 0);
        CHECK(fixture.result.error == P4_HOSTED_CONTROL_PROTOCOL);
        CHECK(fixture.state.poisoned == 1u);
        CHECK(fixture.fake.rx_fifo_reads == 1u);
        verify_single_request(&fixture, TEST_UID, 0u);
    }

    fixture_init(&fixture);
    frame_size = make_response(frame, sizeof frame, TEST_UID, 513u, mac,
                               sizeof mac, 1u, 0u, 1u);
    CHECK(frame_size != 0u);
    CHECK(add_batch(&fixture.fake, frame, frame_size));
    set_raws(&fixture.fake, raw, 1u);
    CHECK(call_gate(&fixture, TEST_GENERATION, &fixture.rx_ops,
                    &fixture.tx_ops, 1u, TEST_UID, fixture.buffer,
                    sizeof fixture.buffer, &fixture.result) == 1);
    CHECK(fixture.result.success == 1u);
    CHECK(fixture.result.mac.valid_mac == 1u);
    verify_single_request(&fixture, TEST_UID, 1u);

    fixture_init(&fixture);
    frame_size = make_response(frame, sizeof frame, TEST_UID, 513u, mac,
                               sizeof mac, 1u, 0u, 1u);
    CHECK(frame_size != 0u);
    frame[6] ^= 1u;
    CHECK(add_batch(&fixture.fake, frame, frame_size));
    set_raws(&fixture.fake, raw, 1u);
    CHECK(call_gate(&fixture, TEST_GENERATION, &fixture.rx_ops,
                    &fixture.tx_ops, 1u, TEST_UID, fixture.buffer,
                    sizeof fixture.buffer, &fixture.result) == 0);
    CHECK(fixture.result.error == P4_HOSTED_CONTROL_PROTOCOL);
    CHECK(fixture.state.poisoned == 1u);
    CHECK(fixture.fake.rx_fifo_reads == 1u);
    verify_single_request(&fixture, TEST_UID, 1u);
}

static void no_callbacks(const struct fake_host *fake)
{
    CHECK(fake->session_calls == 0u);
    CHECK(fake->budget_calls == 0u);
    CHECK(fake->tx_token_reads == 0u);
    CHECK(fake->rx_word_reads == 0u);
    CHECK(fake->raw_reads == 0u);
    CHECK(fake->length_reads == 0u);
    CHECK(fake->rx_fifo_reads == 0u);
    CHECK(fake->tx_fifo_writes == 0u);
    CHECK(fake->write_word_calls == 0u);
    CHECK(fake->yield_calls == 0u);
}

static void expect_plain_preflight_refusal(struct fixture *fixture,
                                           uint32_t generation,
                                           uint32_t uid,
                                           unsigned checksum_enabled,
                                           uint8_t *buffer, size_t capacity)
{
    struct p4_hosted_fifo state_before;
    struct p4_hosted_control_result result_before;
    uint8_t buffer_before[TEST_CAPACITY];

    memcpy(&state_before, &fixture->state, sizeof state_before);
    memcpy(&result_before, &fixture->result, sizeof result_before);
    memcpy(buffer_before, fixture->buffer, sizeof buffer_before);
    CHECK(call_gate(fixture, generation, &fixture->rx_ops, &fixture->tx_ops,
                    checksum_enabled, uid, buffer, capacity,
                    &fixture->result) == 0);
    CHECK(memcmp(&state_before, &fixture->state, sizeof state_before) == 0);
    CHECK(memcmp(&result_before, &fixture->result, sizeof result_before) == 0);
    CHECK(memcmp(buffer_before, fixture->buffer, sizeof buffer_before) == 0);
    no_callbacks(&fixture->fake);
}

static void test_invalid_inputs_and_state(void)
{
    struct fixture fixture;

    fixture_init(&fixture);
    expect_plain_preflight_refusal(&fixture, 0u, TEST_UID, 0u,
                                  fixture.buffer, sizeof fixture.buffer);
    fixture_init(&fixture);
    expect_plain_preflight_refusal(&fixture, TEST_GENERATION, 0u, 0u,
                                  fixture.buffer, sizeof fixture.buffer);
    fixture_init(&fixture);
    expect_plain_preflight_refusal(&fixture, TEST_GENERATION, TEST_UID, 2u,
                                  fixture.buffer, sizeof fixture.buffer);
    fixture_init(&fixture);
    expect_plain_preflight_refusal(&fixture, TEST_GENERATION, TEST_UID, 0u,
                                  fixture.buffer, 511u);
    fixture_init(&fixture);
    expect_plain_preflight_refusal(&fixture, TEST_GENERATION, TEST_UID, 0u,
                                  fixture.buffer, TEST_CAPACITY + 1u);

    fixture_init(&fixture);
    fixture.state.busy = 1u;
    expect_plain_preflight_refusal(&fixture, TEST_GENERATION, TEST_UID, 0u,
                                  fixture.buffer, sizeof fixture.buffer);
    fixture_init(&fixture);
    fixture.state.poisoned = 1u;
    expect_plain_preflight_refusal(&fixture, TEST_GENERATION, TEST_UID, 0u,
                                  fixture.buffer, sizeof fixture.buffer);
    fixture_init(&fixture);
    fixture.state.pool_limit = 0u;
    expect_plain_preflight_refusal(&fixture, TEST_GENERATION, TEST_UID, 0u,
                                  fixture.buffer, sizeof fixture.buffer);
    fixture_init(&fixture);
    fixture.state.initialized = 0u;
    expect_plain_preflight_refusal(&fixture, TEST_GENERATION, TEST_UID, 0u,
                                  fixture.buffer, sizeof fixture.buffer);
    fixture_init(&fixture);
    expect_plain_preflight_refusal(&fixture, TEST_GENERATION + 1u, TEST_UID,
                                  0u, fixture.buffer, sizeof fixture.buffer);
}

union aligned_alias_region {
    long double alignment;
    uint8_t bytes[8192];
};

static size_t alias_object_size(unsigned index)
{
    switch (index) {
    case 0: return sizeof(struct p4_hosted_fifo);
    case 1: return sizeof(struct p4_hosted_open_ops);
    case 2: return sizeof(struct p4_hosted_tx_ops);
    case 4: return sizeof(struct p4_hosted_control_result);
    default: return 0u;
    }
}

static void test_pairwise_alias_refusals(void)
{
    unsigned first, second;

    for (first = 0; first < 5u; ++first) {
        for (second = first + 1u; second < 5u; ++second) {
            struct fixture fixture;
            union aligned_alias_region area;
            uint8_t before[sizeof area.bytes];
            struct p4_hosted_fifo state_before;
            struct p4_hosted_control_result result_before;
            uint8_t buffer_before[TEST_CAPACITY];
            struct p4_hosted_fifo *state = &fixture.state;
            const struct p4_hosted_open_ops *rx = &fixture.rx_ops;
            const struct p4_hosted_tx_ops *tx = &fixture.tx_ops;
            uint8_t *buffer = fixture.buffer;
            struct p4_hosted_control_result *result = &fixture.result;
            size_t capacity = TEST_CAPACITY;
            uintptr_t base;

            fixture_init(&fixture);
            memcpy(&state_before, &fixture.state, sizeof state_before);
            memcpy(&result_before, &fixture.result, sizeof result_before);
            memcpy(buffer_before, fixture.buffer, sizeof buffer_before);
            memset(&area, 0x6d, sizeof area);
            memcpy(before, area.bytes, sizeof before);
            base = (uintptr_t)area.bytes;
            if (first == 3u || second == 3u) {
                unsigned object = first == 3u ? second : first;
                if (object == 0u) state = (struct p4_hosted_fifo *)base;
                if (object == 1u) rx = (const struct p4_hosted_open_ops *)base;
                if (object == 2u) tx = (const struct p4_hosted_tx_ops *)base;
                if (object == 4u)
                    result = (struct p4_hosted_control_result *)base;
                buffer = (uint8_t *)(base + alias_object_size(object) - 1u);
            } else {
                if (first == 0u) state = (struct p4_hosted_fifo *)base;
                if (first == 1u) rx = (const struct p4_hosted_open_ops *)base;
                if (first == 2u) tx = (const struct p4_hosted_tx_ops *)base;
                if (first == 4u)
                    result = (struct p4_hosted_control_result *)base;
                if (second == 0u) state = (struct p4_hosted_fifo *)base;
                if (second == 1u) rx = (const struct p4_hosted_open_ops *)base;
                if (second == 2u) tx = (const struct p4_hosted_tx_ops *)base;
                if (second == 4u)
                    result = (struct p4_hosted_control_result *)base;
            }
            CHECK(p4_hosted_control_get_mac(state, TEST_GENERATION, rx, tx,
                    0u, TEST_UID, buffer, capacity, result) == 0);
            CHECK(memcmp(before, area.bytes, sizeof before) == 0);
            CHECK(memcmp(&state_before, &fixture.state, sizeof state_before) == 0);
            CHECK(memcmp(&result_before, &fixture.result,
                         sizeof result_before) == 0);
            CHECK(memcmp(buffer_before, fixture.buffer,
                         sizeof buffer_before) == 0);
            no_callbacks(&fixture.fake);
        }
    }
}

static void test_pointer_range_overflow_refusals(void)
{
    unsigned invalid_index;

    for (invalid_index = 0; invalid_index < 5u; ++invalid_index) {
        struct fixture fixture;
        struct p4_hosted_fifo *state;
        const struct p4_hosted_open_ops *rx;
        const struct p4_hosted_tx_ops *tx;
        uint8_t *buffer;
        struct p4_hosted_control_result *result;
        uintptr_t near_end = UINTPTR_MAX - 1u;
        struct p4_hosted_fifo state_before;
        struct p4_hosted_control_result result_before;
        uint8_t buffer_before[TEST_CAPACITY];

        fixture_init(&fixture);
        memcpy(&state_before, &fixture.state, sizeof state_before);
        memcpy(&result_before, &fixture.result, sizeof result_before);
        memcpy(buffer_before, fixture.buffer, sizeof buffer_before);
        state = &fixture.state;
        rx = &fixture.rx_ops;
        tx = &fixture.tx_ops;
        buffer = fixture.buffer;
        result = &fixture.result;
        if (invalid_index == 0u) state = (struct p4_hosted_fifo *)near_end;
        if (invalid_index == 1u) rx = (const struct p4_hosted_open_ops *)near_end;
        if (invalid_index == 2u) tx = (const struct p4_hosted_tx_ops *)near_end;
        if (invalid_index == 3u) buffer = (uint8_t *)near_end;
        if (invalid_index == 4u)
            result = (struct p4_hosted_control_result *)near_end;
        CHECK(p4_hosted_control_get_mac(state, TEST_GENERATION, rx, tx,
                    0u, TEST_UID, buffer, TEST_CAPACITY, result) == 0);
        CHECK(memcmp(&state_before, &fixture.state, sizeof state_before) == 0);
        CHECK(memcmp(&result_before, &fixture.result,
                     sizeof result_before) == 0);
        CHECK(memcmp(buffer_before, fixture.buffer, sizeof buffer_before) == 0);
        no_callbacks(&fixture.fake);
    }
}

int main(void)
{
    test_idle_then_response();
    test_event_then_response();
    test_unmatched_uid_and_operation();
    test_trailing_malformed_and_duplicate_match();
    test_remote_error_and_invalid_address();
    test_timeout_and_unmatched_bound();
    test_peer_faults();
    test_wrapped_counter_and_capacity_refusal();
    test_budget_session_and_ambiguous_rx();
    test_endpoint_profile_and_checksum();
    test_invalid_inputs_and_state();
    test_pairwise_alias_refusals();
    test_pointer_range_overflow_refusals();
    printf("hosted_control: %lu checks passed\n", checks);
    return 0;
}
