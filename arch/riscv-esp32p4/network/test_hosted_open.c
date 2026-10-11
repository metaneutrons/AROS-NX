/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#include "hosted_open.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(condition) do { \
    ++checks; \
    if (!(condition)) { \
        fprintf(stderr, "line %d: %s\n", __LINE__, #condition); \
        exit(1); \
    } \
} while (0)

#define STREAM_CAPACITY 8192u
#define TEST_GENERATION 17u

struct mock {
    uint8_t fn0[256];
    uint8_t stream[STREAM_CAPACITY];
    size_t stream_size;
    uint32_t preopen_counter;
    uint32_t postopen_counter;
    uint32_t raw_value;
    unsigned read_byte_calls;
    unsigned write_byte_calls;
    unsigned read_word_calls;
    unsigned write_word_calls;
    unsigned fifo_calls;
    unsigned budget_calls;
    unsigned yield_calls;
    unsigned fail_read_byte_at;
    unsigned fail_write_byte_at;
    unsigned fail_read_word_at;
    unsigned fail_write_word_at;
    unsigned fail_fifo;
    unsigned fail_budget_at;
    unsigned mismatch_read_byte_at;
    uint32_t clear_value;
    unsigned clear_calls;
    unsigned open_writes;
    unsigned close_writes;
    unsigned partial_fifo;
    char writes[128];
    size_t writes_size;
};

static uint8_t receive_buffer[STREAM_CAPACITY];

static void put_le16(uint8_t *bytes, uint16_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
}

static uint16_t checksum(const uint8_t *bytes, size_t size)
{
    size_t i;
    uint16_t sum = 0;
    for (i = 0; i < size; ++i)
        if (i != 6u && i != 7u)
            sum = (uint16_t)(sum + bytes[i]);
    return sum;
}

static size_t append_frame(uint8_t *bytes, size_t offset, size_t capacity,
                           uint8_t interface_type, uint8_t interface_number,
                           uint8_t flags, uint8_t packet_type,
                           const uint8_t *payload, size_t payload_size,
                           unsigned checksum_enabled, uint16_t sequence)
{
    uint8_t *frame;
    size_t total = P4_HOSTED_HEADER_SIZE + payload_size;
    CHECK(offset <= capacity && total <= capacity - offset);
    CHECK(payload_size <= UINT16_MAX);
    frame = bytes + offset;
    memset(frame, 0, P4_HOSTED_HEADER_SIZE);
    frame[0] = (uint8_t)((interface_number << 4) | interface_type);
    frame[1] = flags;
    put_le16(frame + 2, (uint16_t)payload_size);
    put_le16(frame + 4, P4_HOSTED_HEADER_SIZE);
    put_le16(frame + 8, sequence);
    frame[10] = 0;
    /* Pin the protocol's EVENT packet byte literally; do not derive it from
     * the production macro so the fixture detects a wrong 0x33 mapping. */
    frame[11] = packet_type;
    if (payload_size)
        memcpy(frame + P4_HOSTED_HEADER_SIZE, payload, payload_size);
    if (checksum_enabled)
        put_le16(frame + 6, checksum(frame, total));
    return offset + total;
}

static size_t make_init_payload(uint8_t *payload, size_t capacity,
                                uint8_t capabilities, uint8_t chip_id)
{
    static const uint8_t other_tlvs[] = {
        0x14, 1, 10, /* RX queue size */
        0x15, 1, 10  /* TX queue size */
    };
    size_t size = 2u + 3u + 3u + sizeof other_tlvs;
    CHECK(capacity >= size);
    payload[0] = 0x22;
    payload[1] = (uint8_t)(size - 2u);
    payload[2] = 0x11;
    payload[3] = 1;
    payload[4] = capabilities;
    payload[5] = 0x12;
    payload[6] = 1;
    payload[7] = chip_id;
    memcpy(payload + 8, other_tlvs, sizeof other_tlvs);
    return size;
}

static size_t make_opaque_serial_payload(uint8_t *payload, size_t capacity)
{
    static const uint8_t opaque_payload[] = { 0xff, 0x00, 0xa5 };
    CHECK(capacity >= sizeof opaque_payload);
    memcpy(payload, opaque_payload, sizeof opaque_payload);
    return sizeof opaque_payload;
}

static void make_stream(struct mock *mock, unsigned checksum_enabled,
                        unsigned add_serial_frame)
{
    uint8_t payload[128];
    size_t payload_size;
    mock->stream_size = 0;
    payload_size = make_init_payload(payload, sizeof payload,
        (uint8_t)(P4_HOSTED_INIT_CAP_WLAN_SDIO |
                  (checksum_enabled ? (1u << 7) : 0u)),
        P4_HOSTED_INIT_CHIP_ESP32C6);
    mock->stream_size = append_frame(mock->stream, 0,
        sizeof mock->stream, 5, 0, 0, 0x33, payload, payload_size,
        checksum_enabled, 1);
    if (add_serial_frame) {
        payload_size = make_opaque_serial_payload(payload, sizeof payload);
        mock->stream_size = append_frame(mock->stream, mock->stream_size,
            sizeof mock->stream, 3, 0, 0, 0, payload, payload_size,
            checksum_enabled, 2);
    }
    mock->postopen_counter = (uint32_t)mock->stream_size;
    mock->raw_value = P4_HOSTED_OPEN_INT_PACKET;
}

static void init_mock(struct mock *mock, unsigned checksum_enabled,
                      unsigned add_serial_frame)
{
    memset(mock, 0, sizeof *mock);
    memset(receive_buffer, 0xa5, sizeof receive_buffer);
    mock->fn0[0x04] = 0xa0;
    mock->fn0[0x10] = 0x34;
    mock->fn0[0x11] = 0x12;
    make_stream(mock, checksum_enabled, add_serial_frame);
    mock->raw_value |= (UINT32_C(1) << 31) | (UINT32_C(1) << 7) |
        (UINT32_C(1) << 6);
}

static void append_write(struct mock *mock, char value)
{
    if (mock->writes_size < sizeof mock->writes)
        mock->writes[mock->writes_size++] = value;
}

static int mock_read_byte(void *opaque, unsigned function, uint32_t address,
                          uint8_t *value)
{
    struct mock *mock = opaque;
    unsigned call = ++mock->read_byte_calls;
    if (!value || function != 0 || address >= sizeof mock->fn0)
        return 0;
    if (call == mock->fail_read_byte_at)
        return 0;
    *value = mock->fn0[address];
    if (call == mock->mismatch_read_byte_at)
        *value ^= 1u;
    return 1;
}

static int mock_write_byte(void *opaque, unsigned function, uint32_t address,
                           uint8_t value)
{
    struct mock *mock = opaque;
    unsigned call = ++mock->write_byte_calls;
    int ok = call != mock->fail_write_byte_at;
    if (function == 0 && address < sizeof mock->fn0) {
        mock->fn0[address] = value;
        if (address == 0x04)
            append_write(mock, 'i');
        else if (address == 0x10)
            append_write(mock, 'l');
        else if (address == 0x11)
            append_write(mock, 'h');
        else
            append_write(mock, '?');
        return ok;
    }
    if (function == 1 && address == P4_HOSTED_OPEN_CONTROL_ADDRESS) {
        if (value == P4_HOSTED_OPEN_VALUE) {
            ++mock->open_writes;
            append_write(mock, 'O');
        } else if (value == P4_HOSTED_CLOSE_VALUE) {
            ++mock->close_writes;
            append_write(mock, 'C');
        } else {
            return 0;
        }
        return ok;
    }
    return 0;
}

static int mock_read_word(void *opaque, uint32_t address, uint32_t *value)
{
    struct mock *mock = opaque;
    unsigned call = ++mock->read_word_calls;
    if (!value || call == mock->fail_read_word_at)
        return 0;
    if (address == P4_HOSTED_OPEN_PACKET_LENGTH) {
        if (call == 1)
            *value = mock->preopen_counter;
        else
            *value = mock->postopen_counter;
        return 1;
    }
    if (address == P4_HOSTED_OPEN_INT_RAW) {
        *value = mock->raw_value;
        return 1;
    }
    return 0;
}

static int mock_write_word(void *opaque, uint32_t address, uint32_t value)
{
    struct mock *mock = opaque;
    unsigned call = ++mock->write_word_calls;
    if (address != P4_HOSTED_OPEN_INT_CLEAR)
        return 0;
    ++mock->clear_calls;
    mock->clear_value = value;
    return call != mock->fail_write_word_at;
}

static int mock_read_fifo(void *opaque,
                          const struct p4_hosted_fifo_plan *plan,
                          uint8_t *buffer)
{
    struct mock *mock = opaque;
    size_t copy_size;
    ++mock->fifo_calls;
    CHECK(plan != NULL && buffer != NULL);
    CHECK(plan->logical_bytes == mock->stream_size || mock->fail_fifo);
    CHECK(plan->bus_bytes >= plan->logical_bytes);
    if (mock->fail_fifo) {
        if (mock->partial_fifo && plan->logical_bytes)
            buffer[0] = mock->stream[0];
        return 0;
    }
    copy_size = plan->logical_bytes;
    memcpy(buffer, mock->stream, copy_size);
    memset(buffer + copy_size, 0, plan->bus_bytes - copy_size);
    return 1;
}

static int mock_budget(void *opaque)
{
    struct mock *mock = opaque;
    unsigned call = ++mock->budget_calls;
    return call != mock->fail_budget_at;
}

static void mock_yield(void *opaque)
{
    struct mock *mock = opaque;
    ++mock->yield_calls;
}

static struct p4_hosted_open_ops ops_for(struct mock *mock)
{
    struct p4_hosted_open_ops ops;
    ops.context = mock;
    ops.read_byte = mock_read_byte;
    ops.write_byte = mock_write_byte;
    ops.read_word = mock_read_word;
    ops.write_word = mock_write_word;
    ops.read_fifo = mock_read_fifo;
    ops.budget_ok = mock_budget;
    ops.yield = mock_yield;
    return ops;
}

static int run_gate(struct mock *mock, size_t capacity,
                    struct p4_hosted_open_result *result)
{
    struct p4_hosted_open_ops ops = ops_for(mock);
    return p4_hosted_open_receive_init(&ops, TEST_GENERATION,
                                       receive_buffer, capacity, result);
}

static int cleanup(struct mock *mock, struct p4_hosted_open_result *result)
{
    struct p4_hosted_open_ops ops = ops_for(mock);
    return p4_hosted_open_cleanup(&ops, result);
}

static void check_invalid_init_diagnostic(
    const struct p4_hosted_open_result *result)
{
    const uint8_t expected_present = P4_HOSTED_INIT_HAS_CAPABILITIES |
        P4_HOSTED_INIT_HAS_CHIP_ID | P4_HOSTED_INIT_HAS_RX_QUEUE_SIZE |
        P4_HOSTED_INIT_HAS_TX_QUEUE_SIZE;

    CHECK(result->init_diagnostic.error ==
          P4_HOSTED_INIT_ERROR_CAPABILITIES);
    CHECK(result->init_diagnostic.offset == result->frame.payload_size);
    CHECK(result->init_diagnostic.tag == P4_HOSTED_INIT_TAG_CAPABILITIES);
    CHECK(result->init_diagnostic.tag_size == 1);
    CHECK(result->init_diagnostic.present == expected_present);
    CHECK(result->init_diagnostic.declared_tlv_bytes ==
          result->frame.payload[1]);
}

static void test_success(void)
{
    struct mock mock;
    struct p4_hosted_open_result result;
    uint32_t expected_clear = P4_HOSTED_OPEN_INT_CLEAR_MASK;

    init_mock(&mock, 0, 0);
    mock.raw_value |= (UINT32_C(1) << 31); /* Unknown bit must not be cleared. */
    CHECK(run_gate(&mock, sizeof receive_buffer, &result));
    CHECK(result.gate_ok && result.error == P4_HOSTED_OPEN_ERROR_NONE);
    CHECK(result.snapshot_flags == P4_HOSTED_OPEN_SAVED_ALL);
    CHECK(result.preopen_length_counter == 0);
    CHECK(result.open_attempted && result.stream_consumed);
    CHECK(!result.stream_ambiguous && !result.fifo_poisoned);
    CHECK(result.polls == 1 && mock.yield_calls == 0);
    CHECK(result.raw_interrupt == mock.raw_value);
    CHECK(mock.clear_calls == 1 && mock.clear_value == expected_clear);
    CHECK(result.logical_bytes == mock.stream_size);
    CHECK(result.fifo_state.rx_consumed == mock.stream_size);
    CHECK(result.frame.interface_type == 5 && result.frame.interface_number == 0);
    CHECK(result.frame.packet_type == 0x33 && result.frame.flags == 0);
    CHECK(receive_buffer[11] == 0x33);
    CHECK(result.init_diagnostic.error == P4_HOSTED_INIT_ERROR_NONE);
    CHECK(result.init_diagnostic.offset == 0);
    CHECK(result.init_diagnostic.tag == 0);
    CHECK(result.init_diagnostic.tag_size == 0);
    CHECK(result.init_diagnostic.present == 0);
    CHECK(result.init_diagnostic.declared_tlv_bytes == 0);
    CHECK(result.init.chip_id == 0x0d);
    CHECK((result.init.capabilities & P4_HOSTED_INIT_CAP_WLAN_SDIO) != 0);
    CHECK(mock.fn0[0x04] == (uint8_t)(0xa0 | 3));
    CHECK(mock.fn0[0x10] == 0 && mock.fn0[0x11] == 2);
    CHECK(!result.cleanup_attempted && mock.close_writes == 0);
    CHECK(cleanup(&mock, &result));
    CHECK(result.gate_ok && result.error == P4_HOSTED_OPEN_ERROR_NONE);
    CHECK(result.snapshot_flags == P4_HOSTED_OPEN_SAVED_ALL);
    CHECK(result.cleanup_complete &&
          result.cleanup_error == P4_HOSTED_OPEN_CLEANUP_OK);
    CHECK(mock.close_writes == 1);
    CHECK(mock.fn0[0x04] == 0xa0 && mock.fn0[0x10] == 0x34 &&
          mock.fn0[0x11] == 0x12);
    CHECK(mock.writes_size == 8 && memcmp(mock.writes, "ilhOCilh", 8) == 0);
}

static void test_checksum_and_aggregate(void)
{
    struct mock mock;
    struct p4_hosted_open_result result;

    init_mock(&mock, 1, 1);
    CHECK(run_gate(&mock, sizeof receive_buffer, &result));
    CHECK(result.gate_ok && (result.init.capabilities & (1u << 7)) != 0);
    CHECK(result.additional_frame_count == 1);
    CHECK(result.additional_serial_frame_count == 1);
    CHECK(result.additional_frame_bytes > 0);
    CHECK(result.additional_bytes_discarded == result.additional_frame_bytes);
    CHECK(result.logical_bytes == mock.stream_size);
    CHECK(result.fifo_state.rx_consumed == mock.stream_size);
    CHECK(cleanup(&mock, &result));
}

static void test_callback_failures(void)
{
    struct mock mock;
    struct p4_hosted_open_result result;
    unsigned call;

    for (call = 1; call <= 6; ++call) {
        init_mock(&mock, 0, 0);
        mock.fail_read_byte_at = call;
        CHECK(!run_gate(&mock, sizeof receive_buffer, &result));
        CHECK(result.error == P4_HOSTED_OPEN_ERROR_CALLBACK);
        CHECK(result.snapshot_flags <= P4_HOSTED_OPEN_SAVED_ALL);
        CHECK(result.fn0_mutation_attempted == (call >= 4));
        CHECK(cleanup(&mock, &result));
        CHECK(result.cleanup_complete);
        CHECK(!result.gate_ok);
    }

    for (call = 1; call <= 4; ++call) {
        init_mock(&mock, 0, 0);
        mock.fail_write_byte_at = call;
        CHECK(!run_gate(&mock, sizeof receive_buffer, &result));
        CHECK(result.error == P4_HOSTED_OPEN_ERROR_CALLBACK);
        CHECK(result.fn0_mutation_attempted);
        CHECK(result.open_attempted == (call == 4));
        CHECK(cleanup(&mock, &result));
        CHECK(mock.close_writes == (call == 4));
        CHECK(mock.fn0[0x04] == 0xa0 && mock.fn0[0x10] == 0x34 &&
              mock.fn0[0x11] == 0x12);
    }

    for (call = 1; call <= 3; ++call) {
        init_mock(&mock, 0, 0);
        mock.fail_read_word_at = call;
        CHECK(!run_gate(&mock, sizeof receive_buffer, &result));
        CHECK(result.error == P4_HOSTED_OPEN_ERROR_CALLBACK);
        CHECK(result.open_attempted == (call != 1));
        CHECK(cleanup(&mock, &result));
    }

    init_mock(&mock, 0, 0);
    mock.fail_write_word_at = 1;
    CHECK(!run_gate(&mock, sizeof receive_buffer, &result));
    CHECK(result.error == P4_HOSTED_OPEN_ERROR_CALLBACK);
    CHECK(result.stage == P4_HOSTED_OPEN_STAGE_CLEAR_INTERRUPT);
    CHECK(result.open_attempted && mock.fifo_calls == 0);
    CHECK(cleanup(&mock, &result));

    init_mock(&mock, 0, 0);
    mock.fail_fifo = 1;
    mock.partial_fifo = 1;
    CHECK(!run_gate(&mock, sizeof receive_buffer, &result));
    CHECK(result.error == P4_HOSTED_OPEN_ERROR_CALLBACK);
    CHECK(result.stream_ambiguous && result.fifo_poisoned);
    CHECK(!result.stream_consumed && result.fifo_state.rx_consumed == 0);
    CHECK(mock.fifo_calls == 1);
    CHECK(cleanup(&mock, &result));
}

static void test_budget_expiry(void)
{
    struct mock mock;
    struct p4_hosted_open_result baseline, result;
    unsigned calls, fail_at, saw_post_fifo = 0;

    init_mock(&mock, 0, 0);
    CHECK(run_gate(&mock, sizeof receive_buffer, &baseline));
    calls = mock.budget_calls;
    CHECK(calls > 0);

    for (fail_at = 1; fail_at <= calls; ++fail_at) {
        init_mock(&mock, 0, 0);
        mock.fail_budget_at = fail_at;
        CHECK(!run_gate(&mock, sizeof receive_buffer, &result));
        CHECK(result.error == P4_HOSTED_OPEN_ERROR_BUDGET_OR_GENERATION);
        if (result.stream_consumed && result.stage ==
            P4_HOSTED_OPEN_STAGE_FINISH_FIFO) {
            saw_post_fifo = 1;
            CHECK(result.fifo_poisoned);
            CHECK(result.fifo_state.rx_consumed == 0);
        }
        mock.fail_budget_at = 0;
        mock.budget_calls = 0;
        CHECK(cleanup(&mock, &result));
        CHECK(!result.gate_ok);
    }
    CHECK(saw_post_fifo);
}

static void test_poll_and_interrupt_failures(void)
{
    struct mock mock;
    struct p4_hosted_open_result result;

    init_mock(&mock, 0, 0);
    mock.raw_value = 0;
    CHECK(!run_gate(&mock, sizeof receive_buffer, &result));
    CHECK(result.error == P4_HOSTED_OPEN_ERROR_POLL_EXHAUSTED);
    CHECK(result.polls == P4_HOSTED_OPEN_MAX_POLLS);
    CHECK(mock.yield_calls == P4_HOSTED_OPEN_MAX_POLLS);
    CHECK(mock.fifo_calls == 0);
    CHECK(cleanup(&mock, &result));

    init_mock(&mock, 0, 0);
    mock.raw_value = P4_HOSTED_OPEN_INT_RX_UNDERFLOW |
        P4_HOSTED_OPEN_INT_PACKET;
    CHECK(!run_gate(&mock, sizeof receive_buffer, &result));
    CHECK(result.error == P4_HOSTED_OPEN_ERROR_RX_UNDERFLOW);
    CHECK(mock.clear_calls == 0 && mock.fifo_calls == 0);
    CHECK(cleanup(&mock, &result));

    init_mock(&mock, 0, 0);
    mock.raw_value = P4_HOSTED_OPEN_INT_TX_OVERFLOW;
    CHECK(!run_gate(&mock, sizeof receive_buffer, &result));
    CHECK(result.error == P4_HOSTED_OPEN_ERROR_TX_OVERFLOW);
    CHECK(cleanup(&mock, &result));
}

static void test_bad_counters_and_capacity(void)
{
    struct mock mock;
    struct p4_hosted_open_result result;

    init_mock(&mock, 0, 0);
    mock.preopen_counter = 3;
    CHECK(!run_gate(&mock, sizeof receive_buffer, &result));
    CHECK(result.error == P4_HOSTED_OPEN_ERROR_STALE_COUNTER);
    CHECK(result.preopen_length_counter == 3);
    CHECK(!result.open_attempted && mock.fifo_calls == 0);
    CHECK(cleanup(&mock, &result));

    init_mock(&mock, 0, 0);
    mock.postopen_counter = P4_HOSTED_FIFO_BUS_MAX + 1u;
    CHECK(!run_gate(&mock, sizeof receive_buffer, &result));
    CHECK(result.error == P4_HOSTED_OPEN_ERROR_FIFO_COUNTER);
    CHECK(result.fifo_poisoned && mock.fifo_calls == 0);
    CHECK(cleanup(&mock, &result));

    init_mock(&mock, 0, 0);
    mock.postopen_counter = 4097;
    CHECK(!run_gate(&mock, 4096, &result));
    CHECK(result.error == P4_HOSTED_OPEN_ERROR_BUFFER_TOO_SMALL);
    CHECK(result.logical_bytes == 4097 && result.fifo_plan.bus_bytes == 4608);
    CHECK(mock.fifo_calls == 0 && !result.fifo_state.poisoned);
    CHECK(cleanup(&mock, &result));
}

static void test_malformed_and_wrong_frames(void)
{
    struct mock mock;
    struct p4_hosted_open_result result;

    init_mock(&mock, 0, 0);
    mock.stream[11] = 0x32;
    CHECK(!run_gate(&mock, sizeof receive_buffer, &result));
    CHECK(result.error == P4_HOSTED_OPEN_ERROR_FRAME_PROFILE);
    CHECK(result.stream_consumed);
    CHECK(cleanup(&mock, &result));

    init_mock(&mock, 0, 0);
    mock.stream[0] = (uint8_t)((1u << 4) | 5u);
    CHECK(!run_gate(&mock, sizeof receive_buffer, &result));
    CHECK(result.error == P4_HOSTED_OPEN_ERROR_FRAME_PROFILE);
    CHECK(cleanup(&mock, &result));

    init_mock(&mock, 0, 0);
    mock.stream[1] = P4_HOSTED_MORE_FRAGMENT;
    CHECK(!run_gate(&mock, sizeof receive_buffer, &result));
    CHECK(result.error == P4_HOSTED_OPEN_ERROR_FRAME_PROFILE);
    CHECK(cleanup(&mock, &result));

    init_mock(&mock, 0, 0);
    mock.stream[0] = 6;
    CHECK(!run_gate(&mock, sizeof receive_buffer, &result));
    CHECK(result.error == P4_HOSTED_OPEN_ERROR_FRAME_PROFILE);
    CHECK(cleanup(&mock, &result));

    init_mock(&mock, 0, 0);
    mock.stream[P4_HOSTED_HEADER_SIZE + 7u] = 0x09;
    CHECK(!run_gate(&mock, sizeof receive_buffer, &result));
    CHECK(result.error == P4_HOSTED_OPEN_ERROR_UNSUPPORTED_CHIP);
    CHECK(cleanup(&mock, &result));

    init_mock(&mock, 0, 0);
    mock.stream[P4_HOSTED_HEADER_SIZE + 4u] = 0;
    /* Remove required WLAN SDIO capability. */
    CHECK(!run_gate(&mock, sizeof receive_buffer, &result));
    CHECK(result.error == P4_HOSTED_OPEN_ERROR_INVALID_INIT);
    check_invalid_init_diagnostic(&result);
    CHECK(cleanup(&mock, &result));
    check_invalid_init_diagnostic(&result);

    init_mock(&mock, 1, 0);
    mock.stream[8] ^= 1; /* Header sequence is covered by checksum. */
    CHECK(!run_gate(&mock, sizeof receive_buffer, &result));
    CHECK(result.error == P4_HOSTED_OPEN_ERROR_CHECKSUM);
    CHECK(cleanup(&mock, &result));

    init_mock(&mock, 0, 1);
    {
        struct p4_hosted_frame first;
        CHECK(p4_hosted_decode(mock.stream, mock.stream_size, 0, &first));
        mock.stream[first.wire_size + 4u] = 0;
        mock.stream[first.wire_size + 5u] = 0;
    }
    CHECK(!run_gate(&mock, sizeof receive_buffer, &result));
    CHECK(result.error == P4_HOSTED_OPEN_ERROR_INVALID_FRAME);
    CHECK(cleanup(&mock, &result));

    init_mock(&mock, 1, 1);
    {
        struct p4_hosted_frame first;
        CHECK(p4_hosted_decode(mock.stream, mock.stream_size, 0, &first));
        mock.stream[first.wire_size + P4_HOSTED_HEADER_SIZE] ^= 1u;
    }
    CHECK(!run_gate(&mock, sizeof receive_buffer, &result));
    CHECK(result.error == P4_HOSTED_OPEN_ERROR_CHECKSUM);
    CHECK(cleanup(&mock, &result));

    init_mock(&mock, 0, 0);
    mock.stream[mock.stream_size] = 0xff;
    ++mock.stream_size;
    ++mock.postopen_counter;
    CHECK(!run_gate(&mock, sizeof receive_buffer, &result));
    CHECK(result.error == P4_HOSTED_OPEN_ERROR_INVALID_FRAME);
    CHECK(cleanup(&mock, &result));

    init_mock(&mock, 0, 0);
    {
        uint8_t payload[128];
        size_t payload_size = make_init_payload(payload, sizeof payload,
            P4_HOSTED_INIT_CAP_WLAN_SDIO, P4_HOSTED_INIT_CHIP_ESP32C6);
        mock.stream_size = append_frame(mock.stream, mock.stream_size,
            sizeof mock.stream, 5, 0, 0, 0x33, payload, payload_size, 0, 2);
        mock.postopen_counter = (uint32_t)mock.stream_size;
    }
    CHECK(!run_gate(&mock, sizeof receive_buffer, &result));
    CHECK(result.error == P4_HOSTED_OPEN_ERROR_DUPLICATE_INIT);
    CHECK(cleanup(&mock, &result));
}

static void test_cleanup_failures(void)
{
    struct mock mock;
    struct p4_hosted_open_result result;
    unsigned base_read, base_write, call;

    for (call = 1; call <= 4; ++call) {
        init_mock(&mock, 0, 0);
        CHECK(run_gate(&mock, sizeof receive_buffer, &result));
        base_write = mock.write_byte_calls;
        mock.fail_write_byte_at = base_write + call;
        CHECK(!cleanup(&mock, &result));
        CHECK(!result.cleanup_complete);
        CHECK(result.gate_ok);
        CHECK(result.snapshot_flags == P4_HOSTED_OPEN_SAVED_ALL);
        CHECK((result.cleanup_flags &
               P4_HOSTED_OPEN_CLEANUP_CLOSE_ATTEMPTED) != 0);
        if (call == 1)
            CHECK((result.cleanup_flags &
                   P4_HOSTED_OPEN_CLEANUP_CLOSE_WRITE_OK) == 0);
        CHECK((result.cleanup_flags &
               P4_HOSTED_OPEN_CLEANUP_WRITE_IE_ATTEMPTED) != 0);
    }

    for (call = 1; call <= 3; ++call) {
        init_mock(&mock, 0, 0);
        CHECK(run_gate(&mock, sizeof receive_buffer, &result));
        base_read = mock.read_byte_calls;
        mock.fail_read_byte_at = base_read + call;
        CHECK(!cleanup(&mock, &result));
        CHECK(result.cleanup_error == P4_HOSTED_OPEN_CLEANUP_CALLBACK);
        CHECK(result.gate_ok && result.stage == P4_HOSTED_OPEN_STAGE_NONE);
        CHECK(result.snapshot_flags == P4_HOSTED_OPEN_SAVED_ALL);
    }

    init_mock(&mock, 0, 0);
    CHECK(run_gate(&mock, sizeof receive_buffer, &result));
    base_read = mock.read_byte_calls;
    mock.mismatch_read_byte_at = base_read + 1;
    CHECK(!cleanup(&mock, &result));
    CHECK(result.cleanup_error == P4_HOSTED_OPEN_CLEANUP_VERIFY_MISMATCH);
    CHECK(result.cleanup_stage ==
          P4_HOSTED_OPEN_STAGE_CLEANUP_READ_INTERRUPT_ENABLE);

    init_mock(&mock, 0, 0);
    CHECK(run_gate(&mock, sizeof receive_buffer, &result));
    mock.budget_calls = 0;
    mock.fail_budget_at = 2; /* CLOSE write succeeds; cleanup then expires. */
    CHECK(!cleanup(&mock, &result));
    CHECK(result.cleanup_error ==
          P4_HOSTED_OPEN_CLEANUP_BUDGET_OR_GENERATION);
    CHECK((result.cleanup_flags &
           P4_HOSTED_OPEN_CLEANUP_CLOSE_WRITE_OK) != 0);
    CHECK((result.cleanup_flags &
           P4_HOSTED_OPEN_CLEANUP_WRITE_IE_ATTEMPTED) == 0);
}

static void test_invalid_inputs(void)
{
    struct mock mock;
    struct p4_hosted_open_result result;
    struct p4_hosted_open_ops ops;

    init_mock(&mock, 0, 0);
    ops = ops_for(&mock);
    ops.yield = NULL;
    CHECK(!p4_hosted_open_receive_init(&ops, TEST_GENERATION,
          receive_buffer, sizeof receive_buffer, &result));
    CHECK(result.error == P4_HOSTED_OPEN_ERROR_INVALID_ARGUMENT);
    CHECK(cleanup(&mock, &result));

    init_mock(&mock, 0, 0);
    CHECK(!run_gate(&mock, 128, &result));
    CHECK(result.error == P4_HOSTED_OPEN_ERROR_BUFFER_TOO_SMALL);
    CHECK(!result.fn0_mutation_attempted && !result.open_attempted);
    CHECK(cleanup(&mock, &result));

    {
        union {
            struct p4_hosted_open_ops ops;
            struct p4_hosted_open_result result;
        } alias;
        unsigned char before[sizeof alias];

        memset(&alias, 0x5a, sizeof alias);
        memcpy(before, &alias, sizeof alias);
        init_mock(&mock, 0, 0);
        CHECK(!p4_hosted_open_receive_init(&alias.ops, TEST_GENERATION,
              receive_buffer, sizeof receive_buffer, &alias.result));
        CHECK(memcmp(before, &alias, sizeof alias) == 0);
        CHECK(mock.read_byte_calls == 0 && mock.write_byte_calls == 0);
    }

    {
        union {
            struct p4_hosted_open_ops alignment;
            uint8_t bytes[4096u + sizeof(struct p4_hosted_open_ops)];
        } shared;
        struct p4_hosted_open_result untouched;
        uint8_t before[sizeof untouched];
        const struct p4_hosted_open_ops *overlapping_ops =
            (const struct p4_hosted_open_ops *)(const void *)shared.bytes;

        memset(shared.bytes, 0, sizeof shared.bytes);
        memset(&untouched, 0x3c, sizeof untouched);
        memcpy(before, &untouched, sizeof untouched);
        init_mock(&mock, 0, 0);
        CHECK(!p4_hosted_open_receive_init(overlapping_ops,
              TEST_GENERATION, shared.bytes, 4096u, &untouched));
        CHECK(memcmp(before, &untouched, sizeof untouched) == 0);
        CHECK(mock.read_byte_calls == 0 && mock.write_byte_calls == 0);
    }

    {
        struct p4_hosted_open_ops separate_ops;
        uint8_t before[sizeof result];

        memset(&result, 0x69, sizeof result);
        memcpy(before, &result, sizeof result);
        init_mock(&mock, 0, 0);
        separate_ops = ops_for(&mock);
        CHECK(!p4_hosted_open_receive_init(&separate_ops, TEST_GENERATION,
              (uint8_t *)(void *)&result, sizeof result, &result));
        CHECK(memcmp(before, &result, sizeof result) == 0);
        CHECK(mock.read_byte_calls == 0 && mock.write_byte_calls == 0);
    }
}

int main(void)
{
    test_success();
    test_checksum_and_aggregate();
    test_callback_failures();
    test_budget_expiry();
    test_poll_and_interrupt_failures();
    test_bad_counters_and_capacity();
    test_malformed_and_wrong_frames();
    test_cleanup_failures();
    test_invalid_inputs();
    printf("hosted OPEN/INIT: %u checks passed\n", checks);
    return 0;
}
