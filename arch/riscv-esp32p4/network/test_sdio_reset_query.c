/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sdio_reset_query.h"

static unsigned int checks;

#define CHECK(expr) do { ++checks; if (!(expr)) { \
    fprintf(stderr, "line %u: %s\n", (unsigned int)__LINE__, #expr); \
    exit(1); } } while (0)

struct expected_command
{
    unsigned int index;
    uint32_t argument;
    p4_sdio_response_type_t response_type;
    uint32_t error_mask;
    p4_sdio_reset_query_stage_t stage;
};

static const struct expected_command expected_commands[] = {
    { 52U, UINT32_C(0x80000c08), P4_SDIO_RESP_R5,
        UINT32_C(0x0000cb00), P4_SDIO_RESET_QUERY_RESET },
    { 0U, 0U, P4_SDIO_RESP_NONE, 0U, P4_SDIO_RESET_QUERY_CMD0 },
    { 5U, 0U, P4_SDIO_RESP_R4, 0U, P4_SDIO_RESET_QUERY_CMD5 }
};

struct fake_host
{
    p4_sdio_reset_query_result_t *result;
    uint32_t responses[3];
    unsigned int command_calls;
    unsigned int budget_calls;
    unsigned int fail_command_at;
    unsigned int fail_budget_at;
};

static void fake_init(struct fake_host *fake)
{
    memset(fake, 0, sizeof(*fake));
    fake->responses[2] = UINT32_C(0x20ffff00);
}

static int fake_command(void *context, unsigned int index,
    uint32_t argument, p4_sdio_response_type_t response_type,
    uint32_t error_mask, uint32_t *response)
{
    struct fake_host *fake = (struct fake_host *)context;
    unsigned int call = ++fake->command_calls;
    const struct expected_command *expected;

    CHECK(fake->result != NULL);
    CHECK(response != NULL);
    CHECK(call <= sizeof(expected_commands) / sizeof(expected_commands[0]));
    expected = &expected_commands[call - 1U];
    CHECK(index == expected->index);
    CHECK(argument == expected->argument);
    CHECK(response_type == expected->response_type);
    CHECK(error_mask == expected->error_mask);
    CHECK(fake->result->stage == expected->stage);
    CHECK(fake->result->command_index == expected->index);
    CHECK(fake->result->response == 0U);
    CHECK(fake->result->reset_attempted == 1U);
    CHECK(fake->result->reset_acknowledged == (call > 1U ? 1U : 0U));
    CHECK(fake->result->complete == 0U);

    if (fake->fail_command_at == call)
        return 0;
    *response = fake->responses[call - 1U];
    return 1;
}

static int fake_budget_ok(void *context)
{
    struct fake_host *fake = (struct fake_host *)context;
    unsigned int call = ++fake->budget_calls;
    unsigned int command_slot;
    const struct expected_command *expected;

    CHECK(fake->result != NULL);
    CHECK(call <= 2U * (sizeof(expected_commands) /
        sizeof(expected_commands[0])));
    command_slot = (call - 1U) / 2U;
    expected = &expected_commands[command_slot];
    CHECK(fake->result->stage == expected->stage);
    CHECK(fake->result->command_index == expected->index);
    CHECK(fake->result->reset_attempted == (call > 1U ? 1U : 0U));
    CHECK(fake->result->reset_acknowledged == (call > 2U ? 1U : 0U));
    CHECK(fake->result->complete == 0U);
    return fake->fail_budget_at != call;
}

static p4_sdio_probe_ops_t fake_ops(struct fake_host *fake)
{
    p4_sdio_probe_ops_t ops;
    ops.context = fake;
    ops.command = fake_command;
    ops.budget_ok = fake_budget_ok;
    ops.yield = NULL;
    return ops;
}

static int run_query(struct fake_host *fake,
    p4_sdio_reset_query_result_t *result)
{
    p4_sdio_probe_ops_t ops = fake_ops(fake);
    fake->result = result;
    return p4_sdio_reset_query(&ops, result);
}

static void check_initialized_error(const p4_sdio_reset_query_result_t *result,
    p4_sdio_probe_error_t error)
{
    CHECK(result->stage == P4_SDIO_RESET_QUERY_START);
    CHECK(result->error == error);
    CHECK(result->command_index == 0U);
    CHECK(result->response == 0U);
    CHECK(result->ocr == 0U);
    CHECK(result->functions == 0U);
    CHECK(result->reset_attempted == 0U);
    CHECK(result->reset_acknowledged == 0U);
    CHECK(result->complete == 0U);
}

static void test_exact_three_command_query(void)
{
    struct fake_host fake;
    p4_sdio_reset_query_result_t result;

    fake_init(&fake);
    memset(&result, 0xa5, sizeof(result));
    CHECK(run_query(&fake, &result) == 1);
    CHECK(result.stage == P4_SDIO_RESET_QUERY_COMPLETE);
    CHECK(result.error == P4_SDIO_PROBE_ERROR_NONE);
    CHECK(result.command_index == 5U);
    CHECK(result.response == UINT32_C(0x20ffff00));
    CHECK(result.ocr == UINT32_C(0x00ffff00));
    CHECK(result.functions == 2U);
    CHECK(result.reset_attempted == 1U);
    CHECK(result.reset_acknowledged == 1U);
    CHECK(result.complete == 1U);
    CHECK(fake.command_calls == 3U);
    CHECK(fake.budget_calls == 6U);
}

static void test_callback_failure_stops_sequence(void)
{
    unsigned int failed_call;

    for (failed_call = 1U; failed_call <= 3U; ++failed_call)
    {
        struct fake_host fake;
        p4_sdio_reset_query_result_t result;

        fake_init(&fake);
        fake.fail_command_at = failed_call;
        memset(&result, 0xa5, sizeof(result));
        CHECK(run_query(&fake, &result) == 0);
        CHECK(result.error == P4_SDIO_PROBE_ERROR_COMMAND);
        CHECK(result.stage == expected_commands[failed_call - 1U].stage);
        CHECK(result.command_index == expected_commands[failed_call - 1U].index);
        CHECK(result.reset_attempted == 1U);
        CHECK(result.reset_acknowledged == (failed_call > 1U ? 1U : 0U));
        CHECK(result.complete == 0U);
        CHECK(fake.command_calls == failed_call);
        CHECK(fake.budget_calls == 2U * failed_call - 1U);
    }
}

static void test_pre_and_post_budget_failures_stop_sequence(void)
{
    unsigned int failed_budget_call;

    for (failed_budget_call = 1U; failed_budget_call <= 6U;
         ++failed_budget_call)
    {
        struct fake_host fake;
        p4_sdio_reset_query_result_t result;
        unsigned int command_slot = (failed_budget_call - 1U) / 2U;

        fake_init(&fake);
        fake.fail_budget_at = failed_budget_call;
        memset(&result, 0xa5, sizeof(result));
        CHECK(run_query(&fake, &result) == 0);
        CHECK(result.error == P4_SDIO_PROBE_ERROR_BUDGET);
        CHECK(result.stage == expected_commands[command_slot].stage);
        CHECK(result.command_index == expected_commands[command_slot].index);
        CHECK(result.reset_attempted ==
            (failed_budget_call > 1U ? 1U : 0U));
        CHECK(result.reset_acknowledged ==
            (failed_budget_call > 2U ? 1U : 0U));
        CHECK(result.complete == 0U);
        CHECK(fake.command_calls == failed_budget_call / 2U);
        CHECK(fake.budget_calls == failed_budget_call);
    }
}

static void test_every_r5_error_mask_bit_refuses_reset(void)
{
    uint32_t bit;

    for (bit = 0U; bit < 32U; ++bit)
    {
        uint32_t response_bit = UINT32_C(1) << bit;
        struct fake_host fake;
        p4_sdio_reset_query_result_t result;

        if (!(response_bit & P4_SDIO_PROBE_R5_ERROR_MASK))
            continue;
        fake_init(&fake);
        fake.responses[0] = response_bit;
        CHECK(run_query(&fake, &result) == 0);
        CHECK(result.error == P4_SDIO_PROBE_ERROR_R5_STATUS);
        CHECK(result.stage == P4_SDIO_RESET_QUERY_RESET);
        CHECK(result.command_index == 52U);
        CHECK(result.response == response_bit);
        CHECK(result.reset_attempted == 1U);
        CHECK(result.reset_acknowledged == 0U);
        CHECK(result.complete == 0U);
        CHECK(fake.command_calls == 1U);
        CHECK(fake.budget_calls == 1U);
    }
}

static void test_reset_acknowledgement_requires_bounded_success(void)
{
    struct fake_host fake;
    p4_sdio_reset_query_result_t result;

    /* The reset write itself can fail ambiguously after attempted is set. */
    fake_init(&fake);
    fake.fail_command_at = 1U;
    CHECK(run_query(&fake, &result) == 0);
    CHECK(result.reset_attempted == 1U);
    CHECK(result.reset_acknowledged == 0U);
    CHECK(fake.command_calls == 1U && fake.budget_calls == 1U);

    /* A successful write without its post-command budget check is not acked. */
    fake_init(&fake);
    fake.fail_budget_at = 2U;
    CHECK(run_query(&fake, &result) == 0);
    CHECK(result.error == P4_SDIO_PROBE_ERROR_BUDGET);
    CHECK(result.reset_attempted == 1U);
    CHECK(result.reset_acknowledged == 0U);
    CHECK(fake.command_calls == 1U && fake.budget_calls == 2U);
}

static void test_cmd5_function_and_ocr_validation(void)
{
    struct fake_host fake;
    p4_sdio_reset_query_result_t result;

    /* A ready-zero R4 is still a valid capability query. */
    fake_init(&fake);
    fake.responses[2] = UINT32_C(0x20ffff00);
    CHECK(run_query(&fake, &result) == 1);
    CHECK((result.response & UINT32_C(0x80000000)) == 0U);
    CHECK(result.functions == 2U);
    CHECK(result.ocr == UINT32_C(0x00ffff00));
    CHECK(fake.command_calls == 3U && fake.budget_calls == 6U);

    /* Functions are mandatory even when the voltage window is usable. */
    fake_init(&fake);
    fake.responses[2] = UINT32_C(0x00ffff00);
    CHECK(run_query(&fake, &result) == 0);
    CHECK(result.error == P4_SDIO_PROBE_ERROR_NO_FUNCTIONS);
    CHECK(result.functions == 0U);
    CHECK(result.reset_acknowledged == 1U);
    CHECK(result.complete == 0U);
    CHECK(fake.command_calls == 3U && fake.budget_calls == 6U);

    /* A nonzero function count cannot substitute for an absent OCR. */
    fake_init(&fake);
    fake.responses[2] = UINT32_C(0x20000000);
    CHECK(run_query(&fake, &result) == 0);
    CHECK(result.error == P4_SDIO_PROBE_ERROR_NO_VOLTAGE);
    CHECK(result.functions == 2U);
    CHECK(result.ocr == 0U);
    CHECK(result.reset_acknowledged == 1U);
    CHECK(result.complete == 0U);
    CHECK(fake.command_calls == 3U && fake.budget_calls == 6U);

    /* Function support without an overlapping host OCR is refused. */
    fake_init(&fake);
    fake.responses[2] = UINT32_C(0x20000001);
    CHECK(run_query(&fake, &result) == 0);
    CHECK(result.error == P4_SDIO_PROBE_ERROR_NO_VOLTAGE);
    CHECK(result.functions == 2U);
    CHECK(result.ocr == UINT32_C(0x00000001));
    CHECK(result.reset_acknowledged == 1U);
    CHECK(result.complete == 0U);
    CHECK(fake.command_calls == 3U && fake.budget_calls == 6U);
}

static void test_invalid_arguments_initialize_result(void)
{
    struct fake_host fake;
    p4_sdio_probe_ops_t ops;
    p4_sdio_reset_query_result_t result;

    fake_init(&fake);
    ops = fake_ops(&fake);
    memset(&result, 0xa5, sizeof(result));
    CHECK(p4_sdio_reset_query(NULL, &result) == 0);
    check_initialized_error(&result, P4_SDIO_PROBE_ERROR_INVALID_ARGUMENT);

    ops.command = NULL;
    memset(&result, 0xa5, sizeof(result));
    CHECK(p4_sdio_reset_query(&ops, &result) == 0);
    check_initialized_error(&result, P4_SDIO_PROBE_ERROR_INVALID_ARGUMENT);
    CHECK(fake.command_calls == 0U && fake.budget_calls == 0U);

    ops = fake_ops(&fake);
    ops.budget_ok = NULL;
    memset(&result, 0xa5, sizeof(result));
    CHECK(p4_sdio_reset_query(&ops, &result) == 0);
    check_initialized_error(&result, P4_SDIO_PROBE_ERROR_INVALID_ARGUMENT);
    CHECK(fake.command_calls == 0U && fake.budget_calls == 0U);

    ops = fake_ops(&fake);
    CHECK(p4_sdio_reset_query(&ops, NULL) == 0);
    CHECK(fake.command_calls == 0U && fake.budget_calls == 0U);
}

int main(void)
{
    test_exact_three_command_query();
    test_callback_failure_stops_sequence();
    test_pre_and_post_budget_failures_stop_sequence();
    test_every_r5_error_mask_bit_refuses_reset();
    test_reset_acknowledgement_requires_bounded_success();
    test_cmd5_function_and_ocr_validation();
    test_invalid_arguments_initialize_result();
    printf("PASS: %u SDIO reset-query checks\n", checks);
    return 0;
}
