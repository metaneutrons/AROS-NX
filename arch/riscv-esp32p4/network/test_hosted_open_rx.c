/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
/* Exercise the preserving OPEN suffix handoff against the actual OPEN, RX,
 * wire, serial, RPC, reassembly, inbox and FIFO implementations. */
#define main hosted_open_legacy_test_main
#include "test_hosted_open.c"
#undef main

#include "hosted_rx.h"
#include "hosted_rpc.h"
#include "hosted_serial.h"

#define HANDOFF_GENERATION UINT32_C(0x73a5)
#define HANDOFF_CAPACITY 4096u

struct poll_mock {
    uint32_t raw;
    uint32_t counter;
    uint8_t stream[STREAM_CAPACITY];
    size_t stream_size;
    unsigned session_calls;
    unsigned budget_calls;
    unsigned raw_reads;
    unsigned length_reads;
    unsigned clear_writes;
    unsigned fifo_reads;
};

static uint8_t continuation_buffer[HANDOFF_CAPACITY];

static int preserve_open(struct mock *mock,
                         struct p4_hosted_open_result *opened)
{
    struct p4_hosted_open_ops ops = ops_for(mock);
    return p4_hosted_open_receive_init_preserve(&ops, HANDOFF_GENERATION,
        receive_buffer, HANDOFF_CAPACITY, opened);
}

static int legacy_open(struct mock *mock,
                       struct p4_hosted_open_result *opened)
{
    struct p4_hosted_open_ops ops = ops_for(mock);
    return p4_hosted_open_receive_init(&ops, HANDOFF_GENERATION,
        receive_buffer, HANDOFF_CAPACITY, opened);
}

static size_t make_serial_rpc(uint8_t *bytes, size_t capacity,
    enum hosted_serial_endpoint endpoint, uint32_t type, uint32_t id,
    uint32_t uid, const uint8_t *body, size_t body_size)
{
    uint8_t protobuf[P4_HOSTED_RPC_MAX_SIZE];
    struct p4_hosted_rpc rpc = {0};
    struct hosted_serial_message serial = {0};
    size_t protobuf_size = 0u, written = 0u;

    rpc.msg_type = type;
    rpc.msg_id = id;
    rpc.uid = uid;
    rpc.payload = body;
    rpc.payload_size = body_size;
    CHECK(p4_hosted_rpc_encode(protobuf, sizeof protobuf, &rpc,
                               &protobuf_size));
    serial.endpoint = endpoint;
    serial.protobuf = protobuf;
    serial.protobuf_size = protobuf_size;
    CHECK(hosted_serial_encode(bytes, capacity, &serial, &written));
    return written;
}

static size_t make_serial_raw(uint8_t *bytes, size_t capacity,
    enum hosted_serial_endpoint endpoint, const uint8_t *protobuf,
    size_t protobuf_size)
{
    struct hosted_serial_message serial = {0};
    size_t written = 0u;

    serial.endpoint = endpoint;
    serial.protobuf = protobuf;
    serial.protobuf_size = protobuf_size;
    CHECK(hosted_serial_encode(bytes, capacity, &serial, &written));
    return written;
}

static size_t append_serial_rpc(struct mock *mock, size_t offset,
    enum hosted_serial_endpoint endpoint, uint32_t type, uint32_t id,
    uint32_t uid, const uint8_t *body, size_t body_size,
    unsigned checksum_enabled, unsigned flags, uint16_t sequence)
{
    uint8_t serial[512];
    size_t serial_size = make_serial_rpc(serial, sizeof serial, endpoint,
        type, id, uid, body, body_size);
    size_t after = append_frame(mock->stream, offset, sizeof mock->stream,
        3u, 0u, (uint8_t)flags, 0u, serial, serial_size,
        checksum_enabled, sequence);

    mock->stream_size = after;
    mock->postopen_counter = (uint32_t)after;
    return after;
}

static void handoff_fixture_init(struct mock *mock, unsigned checksum_enabled)
{
    init_mock(mock, checksum_enabled, 0u);
}

static int poll_session(void *context, uint32_t generation)
{
    struct poll_mock *mock = context;
    ++mock->session_calls;
    return generation == HANDOFF_GENERATION;
}

static int poll_budget(void *context)
{
    struct poll_mock *mock = context;
    ++mock->budget_calls;
    return 1;
}

static int poll_read_word(void *context, uint32_t address, uint32_t *value)
{
    struct poll_mock *mock = context;
    if (!value) return 0;
    if (address == P4_HOSTED_OPEN_INT_RAW) {
        ++mock->raw_reads;
        *value = mock->raw;
        return 1;
    }
    if (address == P4_HOSTED_OPEN_PACKET_LENGTH) {
        ++mock->length_reads;
        *value = mock->counter;
        return 1;
    }
    return 0;
}

static int poll_write_word(void *context, uint32_t address, uint32_t value)
{
    struct poll_mock *mock = context;
    if (address != P4_HOSTED_OPEN_INT_CLEAR ||
        value == 0u || (value & ~P4_HOSTED_OPEN_INT_CLEAR_MASK) != 0u)
        return 0;
    ++mock->clear_writes;
    mock->raw &= ~value;
    return 1;
}

static int poll_read_fifo(void *context,
    const struct p4_hosted_fifo_plan *plan, uint8_t *buffer)
{
    struct poll_mock *mock = context;
    if (!plan || !buffer || plan->logical_bytes != mock->stream_size ||
        plan->bus_bytes < plan->logical_bytes)
        return 0;
    ++mock->fifo_reads;
    memcpy(buffer, mock->stream, mock->stream_size);
    memset(buffer + mock->stream_size, 0,
           plan->bus_bytes - plan->logical_bytes);
    return 1;
}

static struct p4_hosted_rx_ops poll_ops_for(struct poll_mock *mock)
{
    struct p4_hosted_rx_ops ops;
    ops.context = mock;
    ops.session_ok = poll_session;
    ops.budget_ok = poll_budget;
    ops.read_word = poll_read_word;
    ops.write_word = poll_write_word;
    ops.read_fifo = poll_read_fifo;
    return ops;
}

static int take_open(struct p4_hosted_rx *rx,
    struct p4_hosted_open_result *opened, uint32_t generation,
    const uint8_t *buffer, size_t capacity,
    struct p4_hosted_rx_result *result)
{
    return p4_hosted_rx_take_open(rx, opened, generation, buffer,
                                  capacity, result);
}

static void test_init_only_handoff_is_local(void)
{
    struct mock mock;
    struct p4_hosted_open_result opened;
    struct p4_hosted_rx rx;
    struct p4_hosted_rx_result result;
    struct p4_hosted_fifo fifo_before;
    unsigned reads, writes, word_reads, word_writes, fifo_reads, budgets;

    handoff_fixture_init(&mock, 0u);
    CHECK(preserve_open(&mock, &opened));
    CHECK(opened.gate_ok && opened.suffix_preserved);
    CHECK(opened.serial_suffix_offset == opened.frame.wire_size);
    CHECK(opened.serial_suffix_bytes == 0u);
    CHECK(opened.additional_bytes_discarded == 0u);
    CHECK(opened.suffix_taken == 0u);
    memset(&rx, 0, sizeof rx);
    memset(&result, 0x5a, sizeof result);
    fifo_before = opened.fifo_state;
    reads = mock.read_byte_calls;
    writes = mock.write_byte_calls;
    word_reads = mock.read_word_calls;
    word_writes = mock.write_word_calls;
    fifo_reads = mock.fifo_calls;
    budgets = mock.budget_calls;

    CHECK(take_open(&rx, &opened, HANDOFF_GENERATION, receive_buffer,
                    HANDOFF_CAPACITY, &result));
    CHECK(result.error == P4_HOSTED_RX_OK && result.messages == 0u &&
          result.partial == 0u && result.batches == 0u &&
          result.logical_bytes == 0u);
    CHECK(rx.initialized == 1u && rx.generation == HANDOFF_GENERATION &&
          !rx.poisoned && rx.inbox.count == 0u && rx.assembly.fragments == 0u);
    CHECK(memcmp(&fifo_before, &opened.fifo_state, sizeof fifo_before) == 0);
    CHECK(opened.fifo_state.rx_consumed == opened.length_counter);
    CHECK(opened.suffix_taken == 1u);
    CHECK(mock.read_byte_calls == reads && mock.write_byte_calls == writes &&
          mock.read_word_calls == word_reads &&
          mock.write_word_calls == word_writes &&
          mock.fifo_calls == fifo_reads && mock.budget_calls == budgets);
}

static void verify_owned_item(const struct p4_hosted_inbox_item *item,
    uint32_t type, uint32_t id, uint32_t uid,
    const uint8_t *body, size_t body_size)
{
    CHECK(item->msg_type == type && item->msg_id == id && item->uid == uid);
    CHECK(item->payload_size == body_size);
    CHECK(body_size == 0u || memcmp(item->payload, body, body_size) == 0);
}

static void test_owned_suffix_handoff(unsigned checksum_enabled)
{
    static const uint8_t event_body[] = {0x00u, 0x7fu, 0xffu, 0x41u};
    static const uint8_t response_body[] = {0x91u, 0x00u, 0x22u};
    struct mock mock;
    struct p4_hosted_open_result opened;
    struct p4_hosted_rx rx;
    struct p4_hosted_rx_result result;
    struct p4_hosted_fifo fifo_before;
    struct p4_hosted_inbox_item event, response;
    unsigned reads, writes, word_reads, word_writes, fifo_reads, budgets;
    size_t offset;

    handoff_fixture_init(&mock, checksum_enabled);
    offset = mock.stream_size;
    offset = append_serial_rpc(&mock, offset, HOSTED_SERIAL_ENDPOINT_RPCEVT,
        P4_HOSTED_RPC_TYPE_EVENT, 769u, 0u, event_body, sizeof event_body,
        checksum_enabled, 0u, 1u);
    offset = append_serial_rpc(&mock, offset, HOSTED_SERIAL_ENDPOINT_RPCRSP,
        P4_HOSTED_RPC_TYPE_RESPONSE, 513u, UINT32_C(0x1012),
        response_body, sizeof response_body, checksum_enabled, 0u, 2u);
    CHECK(offset == mock.stream_size);
    CHECK(preserve_open(&mock, &opened));
    CHECK(opened.gate_ok && opened.suffix_preserved &&
          opened.suffix_taken == 0u);
    CHECK(opened.serial_suffix_offset == opened.frame.wire_size);
    CHECK(opened.serial_suffix_bytes ==
          opened.logical_bytes - opened.serial_suffix_offset);
    CHECK(opened.additional_bytes_discarded == 0u);
    CHECK(opened.additional_serial_frame_count == 2u);
    memset(&rx, 0, sizeof rx);
    memset(&result, 0, sizeof result);
    fifo_before = opened.fifo_state;
    reads = mock.read_byte_calls;
    writes = mock.write_byte_calls;
    word_reads = mock.read_word_calls;
    word_writes = mock.write_word_calls;
    fifo_reads = mock.fifo_calls;
    budgets = mock.budget_calls;

    CHECK(take_open(&rx, &opened, HANDOFF_GENERATION, receive_buffer,
                    HANDOFF_CAPACITY, &result));
    CHECK(result.error == P4_HOSTED_RX_OK && result.messages == 2u &&
          result.partial == 0u && result.batches == 0u);
    CHECK(result.logical_bytes == opened.serial_suffix_bytes);
    CHECK(rx.inbox.count == 2u && rx.assembly.fragments == 0u);
    CHECK(opened.fifo_state.rx_consumed == fifo_before.rx_consumed &&
          opened.fifo_state.tx_sent == fifo_before.tx_sent &&
          !opened.fifo_state.poisoned && opened.suffix_taken == 1u);
    CHECK(mock.read_byte_calls == reads && mock.write_byte_calls == writes &&
          mock.read_word_calls == word_reads &&
          mock.write_word_calls == word_writes &&
          mock.fifo_calls == fifo_reads && mock.budget_calls == budgets);

    memset(receive_buffer, 0xcc, opened.logical_bytes);
    CHECK(p4_hosted_inbox_pop(&rx.inbox, HANDOFF_GENERATION, &event));
    CHECK(p4_hosted_inbox_pop(&rx.inbox, HANDOFF_GENERATION, &response));
    verify_owned_item(&event, P4_HOSTED_RPC_TYPE_EVENT, 769u, 0u,
                      event_body, sizeof event_body);
    verify_owned_item(&response, P4_HOSTED_RPC_TYPE_RESPONSE, 513u,
                      UINT32_C(0x1012), response_body,
                      sizeof response_body);
}

static void test_partial_suffix_continues_in_later_poll(void)
{
    uint8_t body[64], serial[512], continuation[STREAM_CAPACITY];
    struct mock mock;
    struct p4_hosted_open_result opened;
    struct p4_hosted_rx rx;
    struct p4_hosted_rx_result handoff_result, poll_result;
    struct p4_hosted_inbox_item item;
    struct p4_hosted_fifo fifo_before;
    struct poll_mock poll;
    struct p4_hosted_rx_ops rx_ops;
    size_t serial_size, first_size, offset, continuation_size;
    unsigned i;

    for (i = 0; i < sizeof body; ++i)
        body[i] = (uint8_t)(i * 13u + 7u);
    serial_size = make_serial_rpc(serial, sizeof serial,
        HOSTED_SERIAL_ENDPOINT_RPCEVT, P4_HOSTED_RPC_TYPE_EVENT,
        769u, 0u, body, sizeof body);
    CHECK(serial_size > 8u);
    first_size = serial_size / 2u;
    CHECK(first_size > 0u && first_size < serial_size);

    handoff_fixture_init(&mock, 1u);
    offset = mock.stream_size;
    offset = append_frame(mock.stream, offset, sizeof mock.stream,
        3u, 0u, P4_HOSTED_MORE_FRAGMENT, 0u, serial, first_size,
        1u, UINT16_MAX);
    mock.stream_size = offset;
    mock.postopen_counter = (uint32_t)offset;
    CHECK(preserve_open(&mock, &opened));
    CHECK(opened.gate_ok && opened.suffix_preserved &&
          opened.additional_serial_frame_count == 1u);

    memset(&rx, 0, sizeof rx);
    memset(&handoff_result, 0, sizeof handoff_result);
    fifo_before = opened.fifo_state;
    CHECK(take_open(&rx, &opened, HANDOFF_GENERATION, receive_buffer,
                    HANDOFF_CAPACITY, &handoff_result));
    CHECK(handoff_result.error == P4_HOSTED_RX_OK &&
          handoff_result.partial == 1u && handoff_result.messages == 0u);
    CHECK(rx.assembly.fragments == 1u && rx.assembly.used == first_size);
    CHECK(opened.fifo_state.rx_consumed == fifo_before.rx_consumed &&
          !opened.fifo_state.busy);

    memset(&poll, 0, sizeof poll);
    continuation_size = append_frame(continuation, 0u, sizeof continuation,
        3u, 0u, 0u, 0u, serial + first_size, serial_size - first_size,
        1u, 0u);
    CHECK(continuation_size != 0u);
    memcpy(poll.stream, continuation, continuation_size);
    poll.stream_size = continuation_size;
    poll.raw = P4_HOSTED_OPEN_INT_PACKET;
    poll.counter = (opened.fifo_state.rx_consumed +
                    (uint32_t)continuation_size) & P4_HOSTED_RX_MASK;
    rx_ops = poll_ops_for(&poll);
    memset(&poll_result, 0, sizeof poll_result);
    memset(continuation_buffer, 0xa7, sizeof continuation_buffer);

    CHECK(p4_hosted_rx_poll(&rx, &opened.fifo_state, HANDOFF_GENERATION,
        &rx_ops, 1u, continuation_buffer, sizeof continuation_buffer,
        &poll_result));
    CHECK(poll_result.error == P4_HOSTED_RX_OK &&
          poll_result.batches == 1u && poll_result.messages == 1u &&
          poll_result.partial == 0u);
    CHECK(rx.assembly.fragments == 0u && rx.inbox.count == 1u);
    CHECK(opened.fifo_state.rx_consumed == poll.counter &&
          !opened.fifo_state.poisoned && !rx.poisoned);
    CHECK(poll.raw_reads == 1u && poll.length_reads == 1u &&
          poll.clear_writes == 1u && poll.fifo_reads == 1u);
    memset(continuation_buffer, 0xdd, continuation_size);
    CHECK(p4_hosted_inbox_pop(&rx.inbox, HANDOFF_GENERATION, &item));
    verify_owned_item(&item, P4_HOSTED_RPC_TYPE_EVENT, 769u, 0u,
                      body, sizeof body);
}

static void test_legacy_suffix_is_not_handoffable(void)
{
    static const uint8_t body[] = {0x51u};
    struct mock mock;
    struct p4_hosted_open_result opened, opened_before;
    struct p4_hosted_rx rx, rx_before;
    struct p4_hosted_rx_result result, result_before;

    handoff_fixture_init(&mock, 0u);
    (void)append_serial_rpc(&mock, mock.stream_size,
        HOSTED_SERIAL_ENDPOINT_RPCEVT, P4_HOSTED_RPC_TYPE_EVENT,
        769u, 0u, body, sizeof body, 0u, 0u, 1u);
    CHECK(legacy_open(&mock, &opened));
    CHECK(opened.gate_ok && !opened.suffix_preserved &&
          opened.serial_suffix_bytes == 0u && opened.suffix_taken == 0u);
    CHECK(opened.additional_bytes_discarded == opened.additional_frame_bytes);
    memset(&rx, 0, sizeof rx);
    memset(&result, 0x6d, sizeof result);
    memcpy(&opened_before, &opened, sizeof opened_before);
    memcpy(&rx_before, &rx, sizeof rx_before);
    memcpy(&result_before, &result, sizeof result_before);

    CHECK(!take_open(&rx, &opened, HANDOFF_GENERATION, receive_buffer,
                     HANDOFF_CAPACITY, &result));
    CHECK(memcmp(&opened_before, &opened, sizeof opened_before) == 0);
    CHECK(memcmp(&rx_before, &rx, sizeof rx_before) == 0);
    CHECK(memcmp(&result_before, &result, sizeof result_before) == 0);
}

static void test_handoff_is_single_use_and_preflight_transactional(void)
{
    static const uint8_t body[] = {0x23u, 0x24u};
    struct mock mock;
    struct p4_hosted_open_result opened, opened_before;
    struct p4_hosted_rx rx, fresh_rx;
    struct p4_hosted_rx_result result, result_before, second_result;
    size_t offset;

    handoff_fixture_init(&mock, 0u);
    offset = append_serial_rpc(&mock, mock.stream_size,
        HOSTED_SERIAL_ENDPOINT_RPCEVT, P4_HOSTED_RPC_TYPE_EVENT,
        769u, 0u, body, sizeof body, 0u, 0u, 1u);
    CHECK(offset == mock.stream_size);
    CHECK(preserve_open(&mock, &opened));
    memset(&rx, 0, sizeof rx);
    memset(&result, 0x47, sizeof result);
    memcpy(&opened_before, &opened, sizeof opened_before);
    memcpy(&result_before, &result, sizeof result_before);

    CHECK(!take_open(&rx, &opened, HANDOFF_GENERATION + 1u,
                     receive_buffer, HANDOFF_CAPACITY, &result));
    CHECK(memcmp(&opened_before, &opened, sizeof opened_before) == 0);
    CHECK(memcmp(&result_before, &result, sizeof result_before) == 0);
    CHECK(rx.initialized == 0u);

    CHECK(take_open(&rx, &opened, HANDOFF_GENERATION, receive_buffer,
                    HANDOFF_CAPACITY, &result));
    CHECK(opened.suffix_taken == 1u && result.messages == 1u);
    memset(&fresh_rx, 0, sizeof fresh_rx);
    memset(&second_result, 0x72, sizeof second_result);
    memcpy(&opened_before, &opened, sizeof opened_before);
    memcpy(&result_before, &second_result, sizeof result_before);
    CHECK(!take_open(&fresh_rx, &opened, HANDOFF_GENERATION,
                     receive_buffer, HANDOFF_CAPACITY, &second_result));
    CHECK(memcmp(&opened_before, &opened, sizeof opened_before) == 0);
    CHECK(fresh_rx.initialized == 0u);
    CHECK(memcmp(&result_before, &second_result, sizeof result_before) == 0);

    /* Use a separate unconsumed OPEN result for all remaining preflight
     * refusals. Each must preserve its one-shot marker at zero. */
    {
        struct mock alias_mock;
        struct p4_hosted_open_result alias_opened, alias_opened_before;
        struct p4_hosted_rx_result untouched, untouched_before;

        handoff_fixture_init(&alias_mock, 0u);
        (void)append_serial_rpc(&alias_mock, alias_mock.stream_size,
            HOSTED_SERIAL_ENDPOINT_RPCEVT, P4_HOSTED_RPC_TYPE_EVENT,
            769u, 0u, body, sizeof body, 0u, 0u, 1u);
        CHECK(preserve_open(&alias_mock, &alias_opened));
        CHECK(alias_opened.suffix_taken == 0u);
        memcpy(&alias_opened_before, &alias_opened,
               sizeof alias_opened_before);

        /* Buffer/state overlap is refused with an otherwise valid,
         * unconsumed OPEN receipt. */
        union {
            struct p4_hosted_rx state;
            uint8_t bytes[HANDOFF_CAPACITY];
        } aliased;
        uint8_t aliased_before[sizeof aliased];
        memset(&aliased, 0x3b, sizeof aliased);
        memset(&untouched, 0xc9, sizeof untouched);
        memcpy(aliased_before, &aliased, sizeof aliased);
        memcpy(&untouched_before, &untouched, sizeof untouched_before);
        CHECK(!take_open(&aliased.state, &alias_opened, HANDOFF_GENERATION,
            aliased.bytes, HANDOFF_CAPACITY, &untouched));
        CHECK(memcmp(aliased_before, &aliased, sizeof aliased) == 0);
        CHECK(memcmp(&untouched_before, &untouched, sizeof untouched) == 0);
        CHECK(memcmp(&alias_opened_before, &alias_opened,
                     sizeof alias_opened_before) == 0);
    }
    {
        struct p4_hosted_rx untouched_rx;
        struct p4_hosted_rx_result untouched, untouched_before;
        const uint8_t *overflow =
            (const uint8_t *)(uintptr_t)(UINTPTR_MAX - 7u);
        struct mock overflow_mock;
        struct p4_hosted_open_result overflow_opened, overflow_before;
        handoff_fixture_init(&overflow_mock, 0u);
        CHECK(preserve_open(&overflow_mock, &overflow_opened));
        CHECK(overflow_opened.suffix_taken == 0u);
        memcpy(&overflow_before, &overflow_opened, sizeof overflow_before);
        memset(&untouched_rx, 0, sizeof untouched_rx);
        memset(&untouched, 0x81, sizeof untouched);
        memcpy(&untouched_before, &untouched, sizeof untouched_before);
        CHECK(!take_open(&untouched_rx, &overflow_opened, HANDOFF_GENERATION,
            overflow, HANDOFF_CAPACITY, &untouched));
        CHECK(untouched_rx.initialized == 0u);
        CHECK(memcmp(&untouched_before, &untouched, sizeof untouched) == 0);
        CHECK(memcmp(&overflow_before, &overflow_opened,
                     sizeof overflow_before) == 0);
    }
    {
        struct mock alias_mock;
        struct p4_hosted_open_result valid_opened, valid_opened_before;
        union {
            struct p4_hosted_open_result opened;
            struct p4_hosted_rx_result result;
        } alias;
        struct p4_hosted_rx untouched_rx;
        uint8_t before[sizeof alias];
        handoff_fixture_init(&alias_mock, 0u);
        CHECK(preserve_open(&alias_mock, &valid_opened));
        CHECK(valid_opened.suffix_taken == 0u);
        memset(&alias, 0, sizeof alias);
        memcpy(&alias.opened, &valid_opened, sizeof valid_opened);
        memcpy(before, &alias, sizeof alias);
        memcpy(&valid_opened_before, &alias.opened,
               sizeof valid_opened_before);
        memset(&untouched_rx, 0, sizeof untouched_rx);
        CHECK(!take_open(&untouched_rx, &alias.opened,
            HANDOFF_GENERATION, receive_buffer, HANDOFF_CAPACITY,
            &alias.result));
        CHECK(memcmp(before, &alias, sizeof alias) == 0);
        CHECK(memcmp(&valid_opened_before, &alias.opened,
                     sizeof valid_opened_before) == 0);
        CHECK(untouched_rx.initialized == 0u);
    }
    {
        struct mock start_mock;
        struct p4_hosted_open_result start_opened, start_opened_before;
        struct p4_hosted_rx same_generation, same_generation_before;
        struct p4_hosted_rx_result untouched, untouched_before;
        handoff_fixture_init(&start_mock, 0u);
        CHECK(preserve_open(&start_mock, &start_opened));
        CHECK(start_opened.suffix_taken == 0u);
        memset(&same_generation, 0, sizeof same_generation);
        same_generation.initialized = 1u;
        same_generation.generation = HANDOFF_GENERATION;
        memset(&untouched, 0x94, sizeof untouched);
        memcpy(&same_generation_before, &same_generation,
               sizeof same_generation_before);
        memcpy(&start_opened_before, &start_opened,
               sizeof start_opened_before);
        memcpy(&untouched_before, &untouched, sizeof untouched_before);
        CHECK(!take_open(&same_generation, &start_opened,
            HANDOFF_GENERATION, receive_buffer, HANDOFF_CAPACITY, &untouched));
        CHECK(memcmp(&same_generation_before, &same_generation,
                     sizeof same_generation_before) == 0);
        CHECK(memcmp(&start_opened_before, &start_opened,
                     sizeof start_opened_before) == 0);
        CHECK(memcmp(&untouched_before, &untouched, sizeof untouched_before) == 0);
        CHECK(start_opened.suffix_taken == 0u);
    }
}

static void test_malformed_suffixes_poison_accepted_epoch(void)
{
    static const uint8_t bad_rpc[] = {0x08u, 0x03u, 0x10u, 0x80u};
    static const uint8_t body[] = {0x11u};
    struct mock mock;
    struct p4_hosted_open_result opened;
    struct p4_hosted_rx rx;
    struct p4_hosted_rx_result result;
    uint8_t serial[128];
    size_t serial_size, offset;

    handoff_fixture_init(&mock, 0u);
    serial_size = make_serial_raw(serial, sizeof serial,
        HOSTED_SERIAL_ENDPOINT_RPCEVT, bad_rpc, sizeof bad_rpc);
    offset = append_frame(mock.stream, mock.stream_size, sizeof mock.stream,
        3u, 0u, 0u, 0u, serial, serial_size, 0u, 1u);
    mock.stream_size = offset;
    mock.postopen_counter = (uint32_t)offset;
    CHECK(preserve_open(&mock, &opened));
    memset(&rx, 0, sizeof rx);
    memset(&result, 0, sizeof result);
    CHECK(!take_open(&rx, &opened, HANDOFF_GENERATION, receive_buffer,
                     HANDOFF_CAPACITY, &result));
    CHECK(result.error == P4_HOSTED_RX_PROTOCOL);
    CHECK(opened.suffix_taken == 1u && opened.fifo_poisoned);
    CHECK(rx.initialized == 1u && rx.poisoned && rx.inbox.poisoned);
    CHECK(opened.fifo_state.poisoned && !opened.fifo_state.busy);

    /* Alter the already-consumed INIT prefix before ownership transfer. The
     * handoff was accepted, so this is terminal rather than retryable. */
    handoff_fixture_init(&mock, 0u);
    offset = append_serial_rpc(&mock, mock.stream_size,
        HOSTED_SERIAL_ENDPOINT_RPCEVT, P4_HOSTED_RPC_TYPE_EVENT,
        769u, 0u, body, sizeof body, 0u, 0u, 1u);
    CHECK(offset == mock.stream_size);
    CHECK(preserve_open(&mock, &opened));
    memset(&rx, 0, sizeof rx);
    memset(&result, 0, sizeof result);
    receive_buffer[P4_HOSTED_HEADER_SIZE + 4u] ^=
        P4_HOSTED_INIT_CAP_WLAN_SDIO;
    CHECK(!take_open(&rx, &opened, HANDOFF_GENERATION, receive_buffer,
                     HANDOFF_CAPACITY, &result));
    CHECK(result.error == P4_HOSTED_RX_PROTOCOL);
    CHECK(opened.suffix_taken == 1u && opened.fifo_poisoned);
    CHECK(rx.poisoned && rx.inbox.poisoned && opened.fifo_state.poisoned);
}

static void test_suffix_queue_overflow_poisoning(void)
{
    static const uint8_t body[] = {0x5du};
    struct mock mock;
    struct p4_hosted_open_result opened;
    struct p4_hosted_rx rx;
    struct p4_hosted_rx_result result;
    size_t offset = 0u;
    unsigned i;

    handoff_fixture_init(&mock, 0u);
    offset = mock.stream_size;
    for (i = 0; i <= P4_HOSTED_INBOX_CAPACITY; ++i)
        offset = append_serial_rpc(&mock, offset,
            HOSTED_SERIAL_ENDPOINT_RPCEVT, P4_HOSTED_RPC_TYPE_EVENT,
            769u + i, 0u, body, sizeof body, 0u, 0u,
            (uint16_t)(i + 1u));
    CHECK(offset == mock.stream_size);
    CHECK(preserve_open(&mock, &opened));
    CHECK(opened.additional_serial_frame_count ==
          P4_HOSTED_INBOX_CAPACITY + 1u);
    memset(&rx, 0, sizeof rx);
    memset(&result, 0, sizeof result);
    CHECK(!take_open(&rx, &opened, HANDOFF_GENERATION, receive_buffer,
                     HANDOFF_CAPACITY, &result));
    CHECK(result.error == P4_HOSTED_RX_QUEUE_FULL);
    CHECK(result.messages == P4_HOSTED_INBOX_CAPACITY);
    CHECK(opened.suffix_taken == 1u && opened.fifo_poisoned);
    CHECK(rx.poisoned && rx.inbox.poisoned && rx.inbox.count == 0u);
    CHECK(opened.fifo_state.poisoned && !opened.fifo_state.busy);
}

static void test_checksum_negotiation_and_bad_suffix_checksum(void)
{
    static const uint8_t body[] = {0x12u, 0x34u};
    struct mock mock;
    struct p4_hosted_open_result opened;
    struct p4_hosted_rx rx;
    struct p4_hosted_rx_result result;
    size_t offset;

    handoff_fixture_init(&mock, 1u);
    offset = append_serial_rpc(&mock, mock.stream_size,
        HOSTED_SERIAL_ENDPOINT_RPCEVT, P4_HOSTED_RPC_TYPE_EVENT,
        769u, 0u, body, sizeof body, 1u, 0u, 1u);
    CHECK(offset == mock.stream_size);
    CHECK(preserve_open(&mock, &opened));
    CHECK((opened.init.capabilities & (1u << 7)) != 0u);
    memset(&rx, 0, sizeof rx);
    memset(&result, 0, sizeof result);
    CHECK(take_open(&rx, &opened, HANDOFF_GENERATION, receive_buffer,
                    HANDOFF_CAPACITY, &result));
    CHECK(result.messages == 1u && result.error == P4_HOSTED_RX_OK);

    handoff_fixture_init(&mock, 1u);
    offset = append_serial_rpc(&mock, mock.stream_size,
        HOSTED_SERIAL_ENDPOINT_RPCEVT, P4_HOSTED_RPC_TYPE_EVENT,
        769u, 0u, body, sizeof body, 1u, 0u, 1u);
    CHECK(offset == mock.stream_size);
    mock.stream[mock.stream_size - 1u] ^= 1u;
    CHECK(!preserve_open(&mock, &opened));
    CHECK(opened.error == P4_HOSTED_OPEN_ERROR_CHECKSUM);
    CHECK(!opened.suffix_preserved && opened.stream_consumed);
    CHECK(opened.fifo_poisoned && opened.fifo_state.poisoned);
}

static void test_malformed_outer_suffix_poisoning(void)
{
    static const uint8_t body[] = {0x6au};
    struct mock mock;
    struct p4_hosted_open_result opened;
    size_t offset;
    struct p4_hosted_frame initial;

    handoff_fixture_init(&mock, 0u);
    offset = append_serial_rpc(&mock, mock.stream_size,
        HOSTED_SERIAL_ENDPOINT_RPCEVT, P4_HOSTED_RPC_TYPE_EVENT,
        769u, 0u, body, sizeof body, 0u, 0u, 1u);
    CHECK(offset == mock.stream_size);
    CHECK(p4_hosted_decode(mock.stream, mock.stream_size, 0u, &initial));
    mock.stream[initial.wire_size] |= 0x10u; /* SERIAL interface number 1. */
    CHECK(!preserve_open(&mock, &opened));
    CHECK(opened.error == P4_HOSTED_OPEN_ERROR_FRAME_PROFILE);
    CHECK(opened.stream_consumed && !opened.suffix_preserved);
    CHECK(opened.fifo_poisoned && opened.fifo_state.poisoned);

    handoff_fixture_init(&mock, 1u);
    mock.stream[6] ^= 1u; /* Corrupt the checksummed private INIT frame. */
    CHECK(!preserve_open(&mock, &opened));
    CHECK(opened.error == P4_HOSTED_OPEN_ERROR_CHECKSUM);
    CHECK(opened.stream_consumed && opened.fifo_poisoned &&
          opened.fifo_state.poisoned);
}

int main(void)
{
    unsigned legacy_checks;
    /* Preserve the complete pre-existing OPEN regression suite in this build. */
    (void)hosted_open_legacy_test_main();
    legacy_checks = checks;
    test_init_only_handoff_is_local();
    test_owned_suffix_handoff(0u);
    test_owned_suffix_handoff(1u);
    test_partial_suffix_continues_in_later_poll();
    test_legacy_suffix_is_not_handoffable();
    test_handoff_is_single_use_and_preflight_transactional();
    test_malformed_suffixes_poison_accepted_epoch();
    test_suffix_queue_overflow_poisoning();
    test_checksum_negotiation_and_bad_suffix_checksum();
    test_malformed_outer_suffix_poisoning();
    printf("hosted OPEN->RX handoff: %u new checks passed\n",
           checks - legacy_checks);
    return 0;
}
