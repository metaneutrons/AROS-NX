/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#include "hosted_tx.h"
#include "hosted_wire.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GENERATION 7u
#define POOL_LIMIT 20u
#define BUFFER_CAPACITY 2048u

static unsigned long checks;

#define CHECK(expression) do { \
    ++checks; \
    if (!(expression)) { \
        fprintf(stderr, "check failed at %s:%d: %s\n", __FILE__, __LINE__, \
                #expression); \
        exit(1); \
    } \
} while (0)

static const uint8_t c6_config_without_checksum[] = {
    0x05, 0x00, 0x11, 0x00, 0x0c, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x22, 0x0f,
    0x44, 0x01, 0x00,
    0x45, 0x01, 0x0d,
    0x46, 0x01, 0x00,
    0x47, 0x01, 0x00,
    0x48, 0x01, 0x00
};

static const uint8_t c6_config_with_checksum[] = {
    0x05, 0x00, 0x11, 0x00, 0x0c, 0x00, 0xc3, 0x01,
    0x00, 0x00, 0x00, 0x00,
    0x22, 0x0f,
    0x44, 0x01, 0x00,
    0x45, 0x01, 0x0d,
    0x46, 0x01, 0x00,
    0x47, 0x01, 0x00,
    0x48, 0x01, 0x00
};

struct mock {
    struct p4_hosted_fifo *state;
    struct p4_hosted_fifo replacement;
    unsigned replace_on_session, replace_on_read, replace_on_write;
    int session_value;
    int budget_value;
    uint32_t generation;
    uint32_t tokens[4];
    unsigned token_count;
    unsigned session_calls;
    unsigned budget_calls;
    unsigned read_calls;
    unsigned write_calls;
    unsigned yield_calls;
    unsigned fail_session_at;
    unsigned fail_budget_at;
    unsigned change_session_after_read;
    unsigned change_budget_after_read;
    unsigned change_session_after_yield;
    unsigned change_budget_after_yield;
    unsigned change_session_after_write;
    unsigned change_budget_after_write;
    int read_return;
    int write_return;
    unsigned address_bad;
    unsigned plan_seen;
    unsigned body_unchanged;
    unsigned padding_zero;
    uint8_t expected_frame[64];
    size_t expected_size;
    struct p4_hosted_fifo_plan observed_plan;
    struct p4_hosted_tx_completion reported_completion;
};

struct fixture {
    struct p4_hosted_fifo state;
    struct p4_hosted_tx_ops ops;
    uint8_t buffer[BUFFER_CAPACITY];
    struct p4_hosted_tx_result result;
    struct mock mock;
};

static int mock_session(void *context, uint32_t generation)
{
    struct mock *mock = (struct mock *)context;
    ++mock->session_calls;
    if (mock->replace_on_session == mock->session_calls)
        *mock->state = mock->replacement;
    if (mock->fail_session_at == mock->session_calls ||
        mock->generation != generation)
        return 0;
    return mock->session_value;
}

static int mock_budget(void *context)
{
    struct mock *mock = (struct mock *)context;
    ++mock->budget_calls;
    if (mock->fail_budget_at == mock->budget_calls)
        return 0;
    return mock->budget_value;
}

static int mock_read_word(void *context, uint32_t address, uint32_t *value)
{
    struct mock *mock = (struct mock *)context;
    unsigned index;

    ++mock->read_calls;
    if (mock->replace_on_read == mock->read_calls)
        *mock->state = mock->replacement;
    if (address != P4_HOSTED_TX_TOKEN_ADDRESS || !value)
        mock->address_bad = 1;
    index = mock->read_calls - 1u;
    if (mock->token_count != 0)
    {
        if (index >= mock->token_count)
            index = mock->token_count - 1u;
        *value = mock->tokens[index];
    }
    if (mock->change_session_after_read)
        mock->session_value = 0;
    if (mock->change_budget_after_read)
        mock->budget_value = 0;
    return mock->read_return;
}

static int mock_write_fifo(void *context,
                           const struct p4_hosted_fifo_plan *plan,
                           const uint8_t *buffer,
                           struct p4_hosted_tx_completion *completion)
{
    struct mock *mock = (struct mock *)context;
    uint32_t index;

    ++mock->write_calls;
    if (mock->replace_on_write == mock->write_calls)
        *mock->state = mock->replacement;
    if (plan)
    {
        mock->observed_plan.argument = plan->argument;
        mock->observed_plan.logical_bytes = plan->logical_bytes;
        mock->observed_plan.bus_bytes = plan->bus_bytes;
        mock->observed_plan.credits = plan->credits;
        mock->plan_seen = 1;
    }
    mock->body_unchanged = plan && buffer &&
        plan->logical_bytes == mock->expected_size &&
        memcmp(buffer, mock->expected_frame, mock->expected_size) == 0;
    mock->padding_zero = plan && buffer;
    if (plan && buffer)
        for (index = plan->logical_bytes; index < plan->bus_bytes; ++index)
            if (buffer[index] != 0)
                mock->padding_zero = 0;

    if (completion)
    {
        completion->started = mock->reported_completion.started;
        completion->success = mock->reported_completion.success;
        completion->completed_bytes =
            mock->reported_completion.completed_bytes;
    }
    if (mock->change_session_after_write)
        mock->session_value = 0;
    if (mock->change_budget_after_write)
        mock->budget_value = 0;
    return mock->write_return;
}

static void mock_yield(void *context)
{
    struct mock *mock = (struct mock *)context;
    ++mock->yield_calls;
    if (mock->change_session_after_yield)
        mock->session_value = 0;
    if (mock->change_budget_after_yield)
        mock->budget_value = 0;
}

static void expect_frame(struct mock *mock, const uint8_t *frame, size_t size)
{
    CHECK(size <= sizeof(mock->expected_frame));
    memcpy(mock->expected_frame, frame, size);
    mock->expected_size = size;
}

static void fixture_init(struct fixture *fixture)
{
    memset(fixture, 0, sizeof(*fixture));
    fixture->mock.state = &fixture->state;
    memset(fixture->buffer, 0xa5, sizeof(fixture->buffer));
    memcpy(fixture->buffer, c6_config_without_checksum,
           sizeof(c6_config_without_checksum));
    CHECK(p4_hosted_fifo_start(&fixture->state, GENERATION, POOL_LIMIT));
    fixture->mock.session_value = 1;
    fixture->mock.budget_value = 1;
    fixture->mock.generation = GENERATION;
    fixture->mock.tokens[0] = UINT32_C(1) << 16;
    fixture->mock.token_count = 1;
    fixture->mock.read_return = 1;
    fixture->mock.write_return = 1;
    fixture->mock.reported_completion.started = 1;
    fixture->mock.reported_completion.success = 1;
    fixture->mock.reported_completion.completed_bytes = 512;
    expect_frame(&fixture->mock, fixture->buffer,
                 sizeof(c6_config_without_checksum));
    fixture->ops.context = &fixture->mock;
    fixture->ops.read_word = mock_read_word;
    fixture->ops.write_fifo = mock_write_fifo;
    fixture->ops.session_ok = mock_session;
    fixture->ops.budget_ok = mock_budget;
    fixture->ops.yield = mock_yield;
}

static int send_fixture(struct fixture *fixture, size_t wire_size,
                        unsigned checksum_enabled)
{
    return p4_hosted_tx_send(&fixture->state, GENERATION, &fixture->ops,
                             fixture->buffer, sizeof(fixture->buffer),
                             wire_size, checksum_enabled, &fixture->result);
}

static void expect_preflight_refusal(struct fixture *fixture,
                                     size_t wire_size, size_t capacity,
                                     unsigned checksum_enabled)
{
    struct p4_hosted_fifo state_before;
    uint8_t buffer_before[BUFFER_CAPACITY];
    struct p4_hosted_tx_result result_before;

    memcpy(&state_before, &fixture->state, sizeof(state_before));
    memcpy(buffer_before, fixture->buffer, sizeof(buffer_before));
    memset(&fixture->result, 0xc7, sizeof(fixture->result));
    memcpy(&result_before, &fixture->result, sizeof(result_before));

    CHECK(!p4_hosted_tx_send(&fixture->state, GENERATION, &fixture->ops,
                             fixture->buffer, capacity, wire_size,
                             checksum_enabled, &fixture->result));
    CHECK(memcmp(&state_before, &fixture->state, sizeof(state_before)) == 0);
    CHECK(memcmp(buffer_before, fixture->buffer, sizeof(buffer_before)) == 0);
    CHECK(memcmp(&result_before, &fixture->result, sizeof(result_before)) == 0);
    CHECK(fixture->mock.session_calls == 0);
    CHECK(fixture->mock.budget_calls == 0);
    CHECK(fixture->mock.read_calls == 0);
    CHECK(fixture->mock.write_calls == 0);
    CHECK(fixture->mock.yield_calls == 0);
}

static void expect_generation_refusal(struct fixture *fixture,
                                     uint32_t generation)
{
    struct p4_hosted_fifo state_before;
    uint8_t buffer_before[BUFFER_CAPACITY];
    struct p4_hosted_tx_result result_before;

    memcpy(&state_before, &fixture->state, sizeof(state_before));
    memcpy(buffer_before, fixture->buffer, sizeof(buffer_before));
    memset(&fixture->result, 0xc7, sizeof(fixture->result));
    memcpy(&result_before, &fixture->result, sizeof(result_before));

    CHECK(!p4_hosted_tx_send(&fixture->state, generation, &fixture->ops,
                             fixture->buffer, sizeof(fixture->buffer), 29, 0,
                             &fixture->result));
    CHECK(memcmp(&state_before, &fixture->state, sizeof(state_before)) == 0);
    CHECK(memcmp(buffer_before, fixture->buffer, sizeof(buffer_before)) == 0);
    CHECK(memcmp(&result_before, &fixture->result, sizeof(result_before)) == 0);
    CHECK(fixture->mock.session_calls == 0);
    CHECK(fixture->mock.budget_calls == 0);
    CHECK(fixture->mock.read_calls == 0);
    CHECK(fixture->mock.write_calls == 0);
    CHECK(fixture->mock.yield_calls == 0);
}

static void test_literal_config_send(void)
{
    struct fixture fixture;
    uint8_t logical_before[sizeof(c6_config_without_checksum)];
    unsigned index;

    fixture_init(&fixture);
    memcpy(logical_before, fixture.buffer, sizeof(logical_before));
    CHECK(send_fixture(&fixture, sizeof(c6_config_without_checksum), 0) == 1);
    CHECK(fixture.result.success == 1);
    CHECK(fixture.result.error == P4_HOSTED_TX_ERROR_NONE);
    CHECK(fixture.result.polls == 1);
    CHECK(fixture.result.token_valid == 1);
    CHECK(fixture.result.token == (UINT32_C(1) << 16));
    CHECK(fixture.result.plan_valid == 1);
    CHECK(fixture.result.plan.logical_bytes == 29);
    CHECK(fixture.result.plan.bus_bytes == 512);
    CHECK(fixture.result.plan.credits == 1);
    CHECK(((fixture.result.plan.argument >> 9) & 0x1ffffu) == 0x1f7e3u);
    CHECK((fixture.result.plan.argument & 0x1ffu) == 1);
    CHECK(fixture.result.write_called == 1);
    CHECK(fixture.result.completion_reported == 1);
    CHECK(fixture.result.completion_well_formed == 1);
    CHECK(fixture.result.completion.started == 1);
    CHECK(fixture.result.completion.success == 1);
    CHECK(fixture.result.completion.completed_bytes == 512);
    CHECK(fixture.mock.address_bad == 0);
    CHECK(fixture.mock.read_calls == 1);
    CHECK(fixture.mock.yield_calls == 0);
    CHECK(fixture.mock.write_calls == 1);
    CHECK(fixture.mock.plan_seen == 1);
    CHECK(fixture.mock.observed_plan.argument == fixture.result.plan.argument);
    CHECK(fixture.mock.body_unchanged == 1);
    CHECK(fixture.mock.padding_zero == 1);
    CHECK(memcmp(logical_before, fixture.buffer, sizeof(logical_before)) == 0);
    for (index = (unsigned)sizeof(logical_before); index < 512; ++index)
        CHECK(fixture.buffer[index] == 0);
    for (index = 512; index < sizeof(fixture.buffer); ++index)
        CHECK(fixture.buffer[index] == 0xa5);
    CHECK(fixture.state.tx_sent == 1);
    CHECK(fixture.state.rx_consumed == 0);
    CHECK(fixture.state.busy == 0);
    CHECK(fixture.state.poisoned == 0);
}

static void test_checksum_and_profiles(void)
{
    static const unsigned types[] = {
        P4_HOSTED_STA, P4_HOSTED_AP, P4_HOSTED_SERIAL, P4_HOSTED_PRIVATE
    };
    struct fixture fixture;
    size_t index;

    fixture_init(&fixture);
    memcpy(fixture.buffer, c6_config_with_checksum,
           sizeof(c6_config_with_checksum));
    expect_frame(&fixture.mock, fixture.buffer,
                 sizeof(c6_config_with_checksum));
    CHECK(send_fixture(&fixture, sizeof(c6_config_with_checksum), 1) == 1);
    CHECK(fixture.result.success == 1);
    CHECK(fixture.mock.body_unchanged == 1);

    for (index = 0; index < sizeof(types) / sizeof(types[0]); ++index)
    {
        fixture_init(&fixture);
        fixture.buffer[0] = (uint8_t)types[index];
        expect_frame(&fixture.mock, fixture.buffer,
                     sizeof(c6_config_without_checksum));
        CHECK(send_fixture(&fixture, sizeof(c6_config_without_checksum),
                           0) == 1);
        CHECK(fixture.result.success == 1);
        CHECK(fixture.mock.write_calls == 1);
    }
}

static void test_preflight_bounds_and_bad_frames(void)
{
    struct fixture fixture;
    struct p4_hosted_frame decoded;
    uint8_t header_only[P4_HOSTED_HEADER_SIZE] = {0};
    size_t shifted_size;

    fixture_init(&fixture);
    expect_preflight_refusal(&fixture, 12, sizeof(fixture.buffer), 0);
    expect_preflight_refusal(&fixture, 11, sizeof(fixture.buffer), 0);
    expect_preflight_refusal(&fixture, 1537, sizeof(fixture.buffer), 0);
    expect_preflight_refusal(&fixture,
                             sizeof(c6_config_without_checksum), 511, 0);
    expect_preflight_refusal(&fixture,
                             sizeof(c6_config_without_checksum),
                             sizeof(fixture.buffer), 2);
    expect_preflight_refusal(&fixture,
                             sizeof(c6_config_without_checksum), SIZE_MAX, 0);

    fixture_init(&fixture);
    fixture.buffer[0] = P4_HOSTED_HCI;
    expect_preflight_refusal(&fixture, 29, sizeof(fixture.buffer), 0);
    fixture_init(&fixture);
    fixture.buffer[0] = (uint8_t)(0x10u | P4_HOSTED_PRIVATE);
    expect_preflight_refusal(&fixture, 29, sizeof(fixture.buffer), 0);
    fixture_init(&fixture);
    fixture.buffer[1] = P4_HOSTED_MORE_FRAGMENT;
    expect_preflight_refusal(&fixture, 29, sizeof(fixture.buffer), 0);
    fixture_init(&fixture);
    fixture.buffer[8] = 1;
    expect_preflight_refusal(&fixture, 29, sizeof(fixture.buffer), 0);
    fixture_init(&fixture);
    fixture.buffer[10] = 1;
    expect_preflight_refusal(&fixture, 29, sizeof(fixture.buffer), 0);
    fixture_init(&fixture);
    fixture.buffer[11] = 1;
    expect_preflight_refusal(&fixture, 29, sizeof(fixture.buffer), 0);
    fixture_init(&fixture);
    fixture.buffer[4] = 13;
    memmove(fixture.buffer + 13, fixture.buffer + 12, 17);
    fixture.buffer[2] = 17;
    fixture.buffer[3] = 0;
    shifted_size = 30;
    expect_preflight_refusal(&fixture, shifted_size,
                             sizeof(fixture.buffer), 0);

    fixture_init(&fixture);
    expect_preflight_refusal(&fixture, 29, sizeof(fixture.buffer), 1);

    header_only[0] = P4_HOSTED_PRIVATE;
    header_only[4] = P4_HOSTED_HEADER_SIZE;
    CHECK(!p4_hosted_decode(header_only, sizeof(header_only), 0, &decoded));

    fixture_init(&fixture);
    fixture.state.initialized = 0;
    expect_preflight_refusal(&fixture, 29, sizeof(fixture.buffer), 0);
    fixture_init(&fixture);
    fixture.state.pool_limit = 0;
    expect_preflight_refusal(&fixture, 29, sizeof(fixture.buffer), 0);
    fixture_init(&fixture);
    fixture.state.pool_limit = P4_HOSTED_TX_MASK + 1u;
    expect_preflight_refusal(&fixture, 29, sizeof(fixture.buffer), 0);

    fixture_init(&fixture);
    expect_generation_refusal(&fixture, 0);
    fixture_init(&fixture);
    expect_generation_refusal(&fixture, GENERATION + 1u);
    fixture_init(&fixture);
    fixture.state.generation = 0;
    expect_generation_refusal(&fixture, 0);
}

static void test_null_and_missing_callbacks(void)
{
    struct fixture fixture;

    fixture_init(&fixture);
    CHECK(!p4_hosted_tx_send(NULL, GENERATION, &fixture.ops, fixture.buffer,
                             sizeof(fixture.buffer), 29, 0,
                             &fixture.result));
    CHECK(!p4_hosted_tx_send(&fixture.state, GENERATION, NULL, fixture.buffer,
                             sizeof(fixture.buffer), 29, 0, &fixture.result));
    CHECK(!p4_hosted_tx_send(&fixture.state, GENERATION, &fixture.ops, NULL,
                             sizeof(fixture.buffer), 29, 0, &fixture.result));
    CHECK(!p4_hosted_tx_send(&fixture.state, GENERATION, &fixture.ops,
                             fixture.buffer, sizeof(fixture.buffer), 29, 0,
                             NULL));
    fixture.ops.read_word = NULL;
    expect_preflight_refusal(&fixture, 29, sizeof(fixture.buffer), 0);
    fixture_init(&fixture);
    fixture.ops.write_fifo = NULL;
    expect_preflight_refusal(&fixture, 29, sizeof(fixture.buffer), 0);
    fixture_init(&fixture);
    fixture.ops.session_ok = NULL;
    expect_preflight_refusal(&fixture, 29, sizeof(fixture.buffer), 0);
    fixture_init(&fixture);
    fixture.ops.budget_ok = NULL;
    expect_preflight_refusal(&fixture, 29, sizeof(fixture.buffer), 0);
    fixture_init(&fixture);
    fixture.ops.yield = NULL;
    expect_preflight_refusal(&fixture, 29, sizeof(fixture.buffer), 0);
}

static void test_range_alias_refusals(void)
{
    struct fixture fixture;
    struct p4_hosted_fifo state_before;
    struct p4_hosted_tx_result result_before;
    uint8_t buffer_before[BUFFER_CAPACITY];
    union state_ops_alias {
        struct p4_hosted_fifo state;
        struct p4_hosted_tx_ops ops;
    } state_ops;

    fixture_init(&fixture);
    memcpy(&state_before, &fixture.state, sizeof(state_before));
    memcpy(buffer_before, fixture.buffer, sizeof(buffer_before));
    memset(&fixture.result, 0xd2, sizeof(fixture.result));
    memcpy(&result_before, &fixture.result, sizeof(result_before));
    CHECK(!p4_hosted_tx_send(&fixture.state, GENERATION, &fixture.ops,
                             (uint8_t *)&fixture.state, 512, 29, 0,
                             &fixture.result));
    CHECK(memcmp(&state_before, &fixture.state, sizeof(state_before)) == 0);
    CHECK(memcmp(buffer_before, fixture.buffer, sizeof(buffer_before)) == 0);
    CHECK(memcmp(&result_before, &fixture.result, sizeof(result_before)) == 0);

    CHECK(!p4_hosted_tx_send(&fixture.state, GENERATION, &fixture.ops,
                             (uint8_t *)&fixture.ops, 512, 29, 0,
                             &fixture.result));
    CHECK(memcmp(&state_before, &fixture.state, sizeof(state_before)) == 0);
    CHECK(memcmp(&result_before, &fixture.result, sizeof(result_before)) == 0);
    CHECK(!p4_hosted_tx_send(&fixture.state, GENERATION, &fixture.ops,
                             fixture.buffer, sizeof(fixture.buffer), 29, 0,
                             (struct p4_hosted_tx_result *)(void *)fixture.buffer));
    CHECK(memcmp(&state_before, &fixture.state, sizeof(state_before)) == 0);
    CHECK(memcmp(buffer_before, fixture.buffer, sizeof(buffer_before)) == 0);
    CHECK(!p4_hosted_tx_send(&fixture.state, GENERATION, &fixture.ops,
                             fixture.buffer, sizeof(fixture.buffer), 29, 0,
                             (struct p4_hosted_tx_result *)(void *)&fixture.state));
    CHECK(memcmp(&state_before, &fixture.state, sizeof(state_before)) == 0);

    fixture_init(&fixture);
    memcpy(&state_before, &fixture.state, sizeof(state_before));
    memset(&fixture.result, 0x6b, sizeof(fixture.result));
    memcpy(&result_before, &fixture.result, sizeof(result_before));
    CHECK(!p4_hosted_tx_send(&fixture.state, GENERATION,
                             (const struct p4_hosted_tx_ops *)(void *)&fixture.state,
                             fixture.buffer, sizeof(fixture.buffer), 29, 0,
                             &fixture.result));
    CHECK(memcmp(&state_before, &fixture.state, sizeof(state_before)) == 0);
    CHECK(memcmp(&result_before, &fixture.result, sizeof(result_before)) == 0);

    memset(&state_ops, 0, sizeof(state_ops));
    CHECK(p4_hosted_fifo_start(&state_ops.state, GENERATION, POOL_LIMIT));
    fixture_init(&fixture);
    CHECK(!p4_hosted_tx_send(&state_ops.state, GENERATION,
                             (const struct p4_hosted_tx_ops *)(void *)&state_ops.ops,
                             fixture.buffer, sizeof(fixture.buffer), 29, 0,
                             &fixture.result));

    fixture_init(&fixture);
    memset(&fixture.result, 0x3a, sizeof(fixture.result));
    memcpy(&result_before, &fixture.result, sizeof(result_before));
    CHECK(!p4_hosted_tx_send(&fixture.state, GENERATION, &fixture.ops,
                             fixture.buffer, sizeof(fixture.buffer), 29, 0,
                             (struct p4_hosted_tx_result *)(void *)&fixture.ops));
    CHECK(memcmp(&result_before, &fixture.result, sizeof(result_before)) == 0);
}

static void test_credit_polling_and_wrap(void)
{
    struct fixture fixture;

    fixture_init(&fixture);
    fixture.mock.tokens[0] = 0;
    fixture.mock.tokens[1] = UINT32_C(1) << 16;
    fixture.mock.token_count = 2;
    CHECK(send_fixture(&fixture, 29, 0) == 1);
    CHECK(fixture.result.polls == 2);
    CHECK(fixture.mock.read_calls == 2);
    CHECK(fixture.mock.yield_calls == 1);
    CHECK(fixture.mock.write_calls == 1);

    fixture_init(&fixture);
    fixture.state.tx_sent = P4_HOSTED_TX_MASK;
    fixture.mock.tokens[0] = 0;
    CHECK(send_fixture(&fixture, 29, 0) == 1);
    CHECK(fixture.result.plan.credits == 1);
    CHECK(fixture.state.tx_sent == 0);

    fixture_init(&fixture);
    fixture.mock.tokens[0] = 0;
    fixture.mock.token_count = 1;
    CHECK(!send_fixture(&fixture, 29, 0));
    CHECK(fixture.result.error == P4_HOSTED_TX_ERROR_CREDIT_EXHAUSTED);
    CHECK(fixture.result.polls == P4_HOSTED_TX_MAX_POLLS);
    CHECK(fixture.mock.read_calls == P4_HOSTED_TX_MAX_POLLS);
    CHECK(fixture.mock.yield_calls == P4_HOSTED_TX_MAX_POLLS - 1u);
    CHECK(fixture.mock.write_calls == 0);
    CHECK(fixture.state.tx_sent == 0);
    CHECK(fixture.state.busy == 0);
    CHECK(fixture.state.poisoned == 0);
}

static void test_bad_counter_and_read_failure(void)
{
    struct fixture fixture;

    fixture_init(&fixture);
    fixture.mock.tokens[0] = (POOL_LIMIT + 1u) << 16;
    CHECK(!send_fixture(&fixture, 29, 0));
    CHECK(fixture.result.error == P4_HOSTED_TX_ERROR_COUNTER_INVALID);
    CHECK(fixture.result.polls == 1);
    CHECK(fixture.mock.read_calls == 1);
    CHECK(fixture.mock.yield_calls == 0);
    CHECK(fixture.mock.write_calls == 0);
    CHECK(fixture.state.poisoned == 1);
    CHECK(fixture.state.tx_sent == 0);
    CHECK(fixture.state.rx_consumed == 0);

    fixture_init(&fixture);
    fixture.mock.read_return = 0;
    CHECK(!send_fixture(&fixture, 29, 0));
    CHECK(fixture.result.error == P4_HOSTED_TX_ERROR_TOKEN_READ);
    CHECK(fixture.result.polls == 1);
    CHECK(fixture.result.token_valid == 0);
    CHECK(fixture.mock.write_calls == 0);
    CHECK(fixture.state.busy == 0);
    CHECK(fixture.state.poisoned == 0);
    CHECK(fixture.state.tx_sent == 0);
}

static void test_session_and_budget_guards(void)
{
    struct fixture fixture;

    fixture_init(&fixture);
    fixture.mock.fail_session_at = 1;
    CHECK(!send_fixture(&fixture, 29, 0));
    CHECK(fixture.result.error == P4_HOSTED_TX_ERROR_SESSION_LOST);
    CHECK(fixture.mock.read_calls == 0);
    CHECK(fixture.state.poisoned == 1);

    fixture_init(&fixture);
    fixture.mock.fail_budget_at = 1;
    CHECK(!send_fixture(&fixture, 29, 0));
    CHECK(fixture.result.error == P4_HOSTED_TX_ERROR_BUDGET_EXPIRED);
    CHECK(fixture.mock.read_calls == 0);
    CHECK(fixture.state.poisoned == 0);

    fixture_init(&fixture);
    fixture.mock.change_session_after_read = 1;
    CHECK(!send_fixture(&fixture, 29, 0));
    CHECK(fixture.result.error == P4_HOSTED_TX_ERROR_SESSION_LOST);
    CHECK(fixture.mock.read_calls == 1);
    CHECK(fixture.mock.write_calls == 0);
    CHECK(fixture.state.poisoned == 1);
    CHECK(fixture.state.tx_sent == 0);

    fixture_init(&fixture);
    fixture.mock.change_budget_after_read = 1;
    CHECK(!send_fixture(&fixture, 29, 0));
    CHECK(fixture.result.error == P4_HOSTED_TX_ERROR_BUDGET_EXPIRED);
    CHECK(fixture.mock.read_calls == 1);
    CHECK(fixture.mock.write_calls == 0);
    CHECK(fixture.state.poisoned == 0);
    CHECK(fixture.state.tx_sent == 0);

    fixture_init(&fixture);
    fixture.mock.tokens[0] = 0;
    fixture.mock.change_session_after_yield = 1;
    CHECK(!send_fixture(&fixture, 29, 0));
    CHECK(fixture.result.error == P4_HOSTED_TX_ERROR_SESSION_LOST);
    CHECK(fixture.mock.read_calls == 1);
    CHECK(fixture.mock.yield_calls == 1);
    CHECK(fixture.mock.write_calls == 0);
    CHECK(fixture.state.poisoned == 1);

    fixture_init(&fixture);
    fixture.mock.tokens[0] = 0;
    fixture.mock.change_budget_after_yield = 1;
    CHECK(!send_fixture(&fixture, 29, 0));
    CHECK(fixture.result.error == P4_HOSTED_TX_ERROR_BUDGET_EXPIRED);
    CHECK(fixture.mock.read_calls == 1);
    CHECK(fixture.mock.yield_calls == 1);
    CHECK(fixture.mock.write_calls == 0);
    CHECK(fixture.state.poisoned == 0);

    fixture_init(&fixture);
    fixture.mock.tokens[0] = 0;
    fixture.mock.token_count = 2;
    fixture.mock.fail_session_at = 4;
    CHECK(!send_fixture(&fixture, 29, 0));
    CHECK(fixture.result.error == P4_HOSTED_TX_ERROR_SESSION_LOST);
    CHECK(fixture.mock.read_calls == 1);
    CHECK(fixture.mock.yield_calls == 1);
    CHECK(fixture.state.poisoned == 1);

    fixture_init(&fixture);
    fixture.mock.tokens[0] = 0;
    fixture.mock.token_count = 2;
    fixture.mock.fail_budget_at = 4;
    CHECK(!send_fixture(&fixture, 29, 0));
    CHECK(fixture.result.error == P4_HOSTED_TX_ERROR_BUDGET_EXPIRED);
    CHECK(fixture.mock.read_calls == 1);
    CHECK(fixture.mock.yield_calls == 1);
    CHECK(fixture.state.poisoned == 0);
}

static void test_prewrite_expiry_and_session_loss(void)
{
    struct fixture fixture;
    uint8_t before[BUFFER_CAPACITY];

    fixture_init(&fixture);
    memcpy(before, fixture.buffer, sizeof(before));
    fixture.mock.fail_session_at = 3;
    CHECK(!send_fixture(&fixture, 29, 0));
    CHECK(fixture.result.error == P4_HOSTED_TX_ERROR_SESSION_LOST);
    CHECK(fixture.mock.read_calls == 1);
    CHECK(fixture.mock.write_calls == 0);
    CHECK(fixture.state.busy == 0);
    CHECK(fixture.state.poisoned == 1);
    CHECK(fixture.state.tx_sent == 0);
    CHECK(memcmp(before, fixture.buffer, sizeof(before)) == 0);

    fixture_init(&fixture);
    memcpy(before, fixture.buffer, sizeof(before));
    fixture.mock.fail_budget_at = 3;
    CHECK(!send_fixture(&fixture, 29, 0));
    CHECK(fixture.result.error == P4_HOSTED_TX_ERROR_BUDGET_EXPIRED);
    CHECK(fixture.mock.read_calls == 1);
    CHECK(fixture.mock.write_calls == 0);
    CHECK(fixture.state.busy == 0);
    CHECK(fixture.state.poisoned == 0);
    CHECK(fixture.state.tx_sent == 0);
    CHECK(memcmp(before, fixture.buffer, sizeof(before)) == 0);
}

static void test_write_completion_outcomes(void)
{
    struct fixture fixture;

    fixture_init(&fixture);
    fixture.mock.reported_completion.started = 0;
    fixture.mock.reported_completion.success = 0;
    fixture.mock.reported_completion.completed_bytes = 0;
    CHECK(!send_fixture(&fixture, 29, 0));
    CHECK(fixture.result.error == P4_HOSTED_TX_ERROR_WRITE_FAILED);
    CHECK(fixture.result.completion_well_formed == 1);
    CHECK(fixture.mock.write_calls == 1);
    CHECK(fixture.state.busy == 0);
    CHECK(fixture.state.poisoned == 0);
    CHECK(fixture.state.tx_sent == 0);

    fixture_init(&fixture);
    fixture.mock.write_return = 0;
    fixture.mock.reported_completion.started = 1;
    fixture.mock.reported_completion.success = 0;
    fixture.mock.reported_completion.completed_bytes = 113;
    CHECK(!send_fixture(&fixture, 29, 0));
    CHECK(fixture.result.error == P4_HOSTED_TX_ERROR_WRITE_AMBIGUOUS);
    CHECK(fixture.result.completion_reported == 0);
    CHECK(fixture.mock.write_calls == 1);
    CHECK(fixture.state.poisoned == 1);
    CHECK(fixture.state.tx_sent == 0);

    fixture_init(&fixture);
    fixture.mock.reported_completion.started = 1;
    fixture.mock.reported_completion.success = 0;
    fixture.mock.reported_completion.completed_bytes = 113;
    CHECK(!send_fixture(&fixture, 29, 0));
    CHECK(fixture.result.error == P4_HOSTED_TX_ERROR_WRITE_FAILED);
    CHECK(fixture.result.completion_well_formed == 1);
    CHECK(fixture.mock.write_calls == 1);
    CHECK(fixture.state.poisoned == 1);
    CHECK(fixture.state.tx_sent == 0);

    fixture_init(&fixture);
    fixture.mock.reported_completion.started = 1;
    fixture.mock.reported_completion.success = 1;
    fixture.mock.reported_completion.completed_bytes = 511;
    CHECK(!send_fixture(&fixture, 29, 0));
    CHECK(fixture.result.error == P4_HOSTED_TX_ERROR_COMPLETION_MALFORMED);
    CHECK(fixture.result.completion_well_formed == 0);
    CHECK(fixture.mock.write_calls == 1);
    CHECK(fixture.state.poisoned == 1);
    CHECK(fixture.state.tx_sent == 0);

    fixture_init(&fixture);
    fixture.mock.reported_completion.started = 2;
    fixture.mock.reported_completion.success = 0;
    fixture.mock.reported_completion.completed_bytes = 0;
    CHECK(!send_fixture(&fixture, 29, 0));
    CHECK(fixture.result.error == P4_HOSTED_TX_ERROR_COMPLETION_MALFORMED);
    CHECK(fixture.result.completion.started == 2);
    CHECK(fixture.mock.write_calls == 1);
    CHECK(fixture.state.poisoned == 1);
    CHECK(fixture.state.tx_sent == 0);

    fixture_init(&fixture);
    fixture.mock.reported_completion.started = 0;
    fixture.mock.reported_completion.success = 0;
    fixture.mock.reported_completion.completed_bytes = 1;
    CHECK(!send_fixture(&fixture, 29, 0));
    CHECK(fixture.result.error == P4_HOSTED_TX_ERROR_COMPLETION_MALFORMED);
    CHECK(fixture.state.poisoned == 1);
    CHECK(fixture.state.tx_sent == 0);

    fixture_init(&fixture);
    fixture.mock.reported_completion.started = 1;
    fixture.mock.reported_completion.success = 0;
    fixture.mock.reported_completion.completed_bytes = 513;
    CHECK(!send_fixture(&fixture, 29, 0));
    CHECK(fixture.result.error == P4_HOSTED_TX_ERROR_COMPLETION_MALFORMED);
    CHECK(fixture.state.poisoned == 1);
    CHECK(fixture.state.tx_sent == 0);
}

static void test_postwrite_expiry_and_loss(void)
{
    struct fixture fixture;

    fixture_init(&fixture);
    fixture.mock.change_session_after_write = 1;
    CHECK(!send_fixture(&fixture, 29, 0));
    CHECK(fixture.result.error == P4_HOSTED_TX_ERROR_SESSION_LOST);
    CHECK(fixture.result.completion_well_formed == 1);
    CHECK(fixture.mock.write_calls == 1);
    CHECK(fixture.state.busy == 0);
    CHECK(fixture.state.poisoned == 1);
    CHECK(fixture.state.tx_sent == 0);

    fixture_init(&fixture);
    fixture.mock.change_budget_after_write = 1;
    CHECK(!send_fixture(&fixture, 29, 0));
    CHECK(fixture.result.error == P4_HOSTED_TX_ERROR_BUDGET_EXPIRED);
    CHECK(fixture.result.completion_well_formed == 1);
    CHECK(fixture.mock.write_calls == 1);
    CHECK(fixture.state.busy == 0);
    CHECK(fixture.state.poisoned == 1);
    CHECK(fixture.state.tx_sent == 0);

    fixture_init(&fixture);
    fixture.mock.fail_session_at = 4;
    CHECK(!send_fixture(&fixture, 29, 0));
    CHECK(fixture.result.error == P4_HOSTED_TX_ERROR_SESSION_LOST);
    CHECK(fixture.mock.write_calls == 1);
    CHECK(fixture.state.poisoned == 1);
    CHECK(fixture.state.tx_sent == 0);

    fixture_init(&fixture);
    fixture.mock.fail_budget_at = 4;
    CHECK(!send_fixture(&fixture, 29, 0));
    CHECK(fixture.result.error == P4_HOSTED_TX_ERROR_BUDGET_EXPIRED);
    CHECK(fixture.mock.write_calls == 1);
    CHECK(fixture.state.poisoned == 1);
    CHECK(fixture.state.tx_sent == 0);
}

static void test_stale_callbacks_preserve_new_ledger(void)
{
    unsigned variant;
    for (variant = 0; variant < 4; ++variant) {
        struct fixture fixture;
        fixture_init(&fixture);
        CHECK(p4_hosted_fifo_start(&fixture.mock.replacement,
                                  GENERATION + 1u, POOL_LIMIT));
        fixture.mock.replacement.rx_consumed = 37;
        fixture.mock.replacement.tx_sent = 3;
        /* Model a new owner's in-flight transfer; stale cleanup must not
         * release its busy marker, pending plan or poison flag. */
        fixture.mock.replacement.busy = 1;
        if (variant == 0) fixture.mock.replace_on_session = 1;
        if (variant == 1) fixture.mock.replace_on_read = 1;
        if (variant == 2) fixture.mock.replace_on_session = 3;
        if (variant == 3) fixture.mock.replace_on_write = 1;
        CHECK(!send_fixture(&fixture, 29, 0));
        CHECK(fixture.result.error == P4_HOSTED_TX_ERROR_SESSION_LOST);
        CHECK(memcmp(&fixture.state, &fixture.mock.replacement,
                     sizeof fixture.state) == 0);
        CHECK(fixture.mock.write_calls == (variant == 3 ? 1u : 0u));
    }
}

int main(void)
{
    test_literal_config_send();
    test_checksum_and_profiles();
    test_preflight_bounds_and_bad_frames();
    test_null_and_missing_callbacks();
    test_range_alias_refusals();
    test_credit_polling_and_wrap();
    test_bad_counter_and_read_failure();
    test_session_and_budget_guards();
    test_prewrite_expiry_and_session_loss();
    test_write_completion_outcomes();
    test_postwrite_expiry_and_loss();
    test_stale_callbacks_preserve_new_ledger();
    printf("hosted TX checks: %lu\n", checks);
    return 0;
}
