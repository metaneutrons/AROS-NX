/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sdio_restore.h"

static unsigned int checks;
#define CHECK(expr) do { ++checks; if (!(expr)) { \
    fprintf(stderr, "line %u: %s\n", (unsigned int)__LINE__, #expr); \
    exit(1); } } while (0)

struct fake_host
{
    p4_sdio_restore_result_t *result;
    unsigned int calls;
    unsigned int budget_calls;
    unsigned int reads;
    unsigned int writes;
    unsigned int budget_fail_at;
    unsigned int fail_call;
    unsigned int override_read_call;
    int budget_fail_value;
    int command_fail_value;
    uint32_t write_addresses[3];
    uint8_t write_values[3];
    uint32_t read_addresses[3];
    uint8_t registers[0x112];
    uint8_t mutate_failed_write;
    uint8_t override_read_value;
};

static void fake_init(struct fake_host *fake)
{
    memset(fake, 0, sizeof(*fake));
    fake->budget_fail_value = 0;
    fake->command_fail_value = 0;
    fake->registers[0x02] = 0x02U;
    fake->registers[0x110] = 0x00U;
    fake->registers[0x111] = 0x02U;
}

static p4_sdio_setup_result_t saved_setup_result(uint8_t mutation_attempted,
    uint8_t io_enable, uint16_t block_size)
{
    p4_sdio_setup_result_t setup_result;

    memset(&setup_result, 0, sizeof(setup_result));
    setup_result.mutation_attempted = mutation_attempted;
    setup_result.previous_io_enable = io_enable;
    setup_result.previous_io_enable_valid = 1U;
    setup_result.original_block_size = block_size;
    setup_result.original_block_size_valid = 1U;
    return setup_result;
}

static int fake_read_register(void *context, unsigned int function,
    uint32_t address, uint8_t *value)
{
    struct fake_host *fake = (struct fake_host *)context;
    unsigned int call = ++fake->calls;

    CHECK(function == 0U);
    CHECK(value != NULL);
    CHECK(address == 0x02U || address == 0x110U || address == 0x111U);
    CHECK(fake->reads < 3U);
    fake->read_addresses[fake->reads++] = address;

    if (call == fake->fail_call)
    {
        /* Failed callback output must not be copied into the result. */
        *value = 0xeeU;
        return fake->command_fail_value;
    }
    if (call == fake->override_read_call)
        *value = fake->override_read_value;
    else
        *value = fake->registers[address];
    return 1;
}

static int fake_write_register(void *context, unsigned int function,
    uint32_t address, uint8_t value)
{
    struct fake_host *fake = (struct fake_host *)context;
    unsigned int call = ++fake->calls;
    int failed = call == fake->fail_call;

    CHECK(function == 0U);
    CHECK(fake->result != NULL);
    CHECK(fake->result->attempted == 1U);
    CHECK(fake->writes < 3U);
    CHECK(address == 0x110U || address == 0x111U || address == 0x02U);
    fake->write_addresses[fake->writes] = address;
    fake->write_values[fake->writes] = value;
    ++fake->writes;

    if (!failed || fake->mutate_failed_write)
        fake->registers[address] = value;
    return failed ? fake->command_fail_value : 1;
}

static int fake_budget_ok(void *context)
{
    struct fake_host *fake = (struct fake_host *)context;
    unsigned int call = ++fake->budget_calls;

    return call == fake->budget_fail_at ? fake->budget_fail_value : 1;
}

static p4_sdio_setup_ops_t fake_ops(struct fake_host *fake)
{
    p4_sdio_setup_ops_t ops;

    ops.context = fake;
    ops.read_register = fake_read_register;
    ops.write_register = fake_write_register;
    ops.budget_ok = fake_budget_ok;
    ops.yield = NULL;
    return ops;
}

static void test_success_restores_saved_values_and_peer_bits(void)
{
    struct fake_host fake;
    p4_sdio_setup_ops_t ops;
    p4_sdio_setup_result_t setup_result;
    p4_sdio_restore_result_t result;

    fake_init(&fake);
    fake.registers[0x02] = 0x02U;
    fake.registers[0x110] = 0x00U;
    fake.registers[0x111] = 0x02U;
    setup_result = saved_setup_result(1U, 0xa5U, 0xb2a1U);
    ops = fake_ops(&fake);
    fake.result = &result;

    CHECK(p4_sdio_restore_function1(&ops, &setup_result, &result) == 1);
    CHECK(result.stage == P4_SDIO_RESTORE_STAGE_COMPLETE);
    CHECK(result.error == P4_SDIO_RESTORE_ERROR_NONE);
    CHECK(result.attempted == 1U && result.complete == 1U);
    CHECK(fake.calls == 6U && fake.budget_calls == 12U);
    CHECK(fake.writes == 3U && fake.reads == 3U);
    CHECK(fake.write_addresses[0] == 0x110U &&
        fake.write_values[0] == 0xa1U);
    CHECK(fake.write_addresses[1] == 0x111U &&
        fake.write_values[1] == 0xb2U);
    CHECK(fake.write_addresses[2] == 0x02U &&
        fake.write_values[2] == 0xa5U);
    CHECK(fake.read_addresses[0] == 0x110U);
    CHECK(fake.read_addresses[1] == 0x111U);
    CHECK(fake.read_addresses[2] == 0x02U);
    CHECK(fake.registers[0x110] == 0xa1U);
    CHECK(fake.registers[0x111] == 0xb2U);
    CHECK(fake.registers[0x02] == 0xa5U);
    CHECK(result.observed_block_size_low_valid == 1U &&
        result.observed_block_size_low == 0xa1U);
    CHECK(result.observed_block_size_high_valid == 1U &&
        result.observed_block_size_high == 0xb2U);
    CHECK(result.observed_io_enable_valid == 1U &&
        result.observed_io_enable == 0xa5U);
}

static void test_no_mutation_is_a_noop_success(void)
{
    p4_sdio_setup_result_t setup_result;
    p4_sdio_restore_result_t result;

    setup_result = saved_setup_result(0U, 0U, 0U);
    memset(&result, 0xa5, sizeof(result));
    CHECK(p4_sdio_restore_function1(NULL, &setup_result, &result) == 1);
    CHECK(result.stage == P4_SDIO_RESTORE_STAGE_COMPLETE);
    CHECK(result.error == P4_SDIO_RESTORE_ERROR_NONE);
    CHECK(result.attempted == 0U && result.complete == 1U);
    CHECK(result.observed_block_size_low_valid == 0U);
    CHECK(result.observed_block_size_high_valid == 0U);
    CHECK(result.observed_io_enable_valid == 0U);
}

static void test_invalid_arguments_and_missing_snapshots(void)
{
    struct fake_host fake;
    p4_sdio_setup_ops_t ops;
    p4_sdio_setup_result_t setup_result;
    p4_sdio_restore_result_t result;

    fake_init(&fake);
    ops = fake_ops(&fake);
    setup_result = saved_setup_result(1U, 0U, 0x1234U);
    CHECK(p4_sdio_restore_function1(&ops, &setup_result, NULL) == 0);
    CHECK(p4_sdio_restore_function1(&ops, NULL, &result) == 0);
    CHECK(result.error == P4_SDIO_RESTORE_ERROR_INVALID_ARGUMENT);
    CHECK(p4_sdio_restore_function1(NULL, &setup_result, &result) == 0);
    CHECK(result.error == P4_SDIO_RESTORE_ERROR_INVALID_ARGUMENT);
    CHECK(fake.calls == 0U && fake.budget_calls == 0U);

    ops.read_register = NULL;
    CHECK(p4_sdio_restore_function1(&ops, &setup_result, &result) == 0);
    CHECK(result.error == P4_SDIO_RESTORE_ERROR_INVALID_ARGUMENT);
    ops = fake_ops(&fake);
    ops.write_register = NULL;
    CHECK(p4_sdio_restore_function1(&ops, &setup_result, &result) == 0);
    CHECK(result.error == P4_SDIO_RESTORE_ERROR_INVALID_ARGUMENT);
    ops = fake_ops(&fake);
    ops.budget_ok = NULL;
    CHECK(p4_sdio_restore_function1(&ops, &setup_result, &result) == 0);
    CHECK(result.error == P4_SDIO_RESTORE_ERROR_INVALID_ARGUMENT);
    CHECK(fake.calls == 0U && fake.budget_calls == 0U);

    ops = fake_ops(&fake);
    setup_result.previous_io_enable_valid = 0U;
    CHECK(p4_sdio_restore_function1(&ops, &setup_result, &result) == 0);
    CHECK(result.error == P4_SDIO_RESTORE_ERROR_SNAPSHOT_UNAVAILABLE);
    CHECK(result.attempted == 0U && fake.calls == 0U);
    setup_result.previous_io_enable_valid = 1U;
    setup_result.original_block_size_valid = 0U;
    CHECK(p4_sdio_restore_function1(&ops, &setup_result, &result) == 0);
    CHECK(result.error == P4_SDIO_RESTORE_ERROR_SNAPSHOT_UNAVAILABLE);
    CHECK(result.attempted == 0U && fake.calls == 0U);
}

static void test_every_command_failure_and_ambiguous_write(void)
{
    static const p4_sdio_restore_stage_t stages[] = {
        P4_SDIO_RESTORE_STAGE_WRITE_BLOCK_SIZE_LOW,
        P4_SDIO_RESTORE_STAGE_WRITE_BLOCK_SIZE_HIGH,
        P4_SDIO_RESTORE_STAGE_WRITE_IO_ENABLE,
        P4_SDIO_RESTORE_STAGE_VERIFY_BLOCK_SIZE_LOW,
        P4_SDIO_RESTORE_STAGE_VERIFY_BLOCK_SIZE_HIGH,
        P4_SDIO_RESTORE_STAGE_VERIFY_IO_ENABLE
    };
    unsigned int call;

    for (call = 1U; call <= 6U; ++call)
    {
        struct fake_host fake;
        p4_sdio_setup_ops_t ops;
        p4_sdio_setup_result_t setup_result;
        p4_sdio_restore_result_t result;

        fake_init(&fake);
        fake.fail_call = call;
        fake.command_fail_value = (call == 2U || call == 5U) ? -1 : 0;
        fake.mutate_failed_write = 1U;
        setup_result = saved_setup_result(1U, 0xa5U, 0xb2a1U);
        ops = fake_ops(&fake);
        fake.result = &result;

        CHECK(p4_sdio_restore_function1(&ops, &setup_result, &result) == 0);
        CHECK(result.complete == 0U);
        CHECK(result.error == P4_SDIO_RESTORE_ERROR_COMMAND);
        CHECK(result.stage == stages[call - 1U]);
        CHECK(fake.calls == call && fake.budget_calls == call * 2U);
        CHECK(result.attempted == 1U);
        if (call <= 3U)
        {
            /* The callback can fail after the write reached the card. */
            CHECK(fake.writes == call && fake.reads == 0U);
        }
        else
        {
            CHECK(fake.writes == 3U);
            CHECK(fake.reads == call - 3U);
        }
        if (call == 4U)
            CHECK(result.observed_block_size_low_valid == 0U &&
                result.observed_block_size_low == 0U);
        if (call == 5U)
            CHECK(result.observed_block_size_high_valid == 0U &&
                result.observed_block_size_high == 0U);
        if (call == 6U)
            CHECK(result.observed_io_enable_valid == 0U &&
                result.observed_io_enable == 0U);
    }
}

static void test_budget_before_and_after_every_command(void)
{
    static const p4_sdio_restore_stage_t stages[] = {
        P4_SDIO_RESTORE_STAGE_WRITE_BLOCK_SIZE_LOW,
        P4_SDIO_RESTORE_STAGE_WRITE_BLOCK_SIZE_HIGH,
        P4_SDIO_RESTORE_STAGE_WRITE_IO_ENABLE,
        P4_SDIO_RESTORE_STAGE_VERIFY_BLOCK_SIZE_LOW,
        P4_SDIO_RESTORE_STAGE_VERIFY_BLOCK_SIZE_HIGH,
        P4_SDIO_RESTORE_STAGE_VERIFY_IO_ENABLE
    };
    unsigned int budget_call;

    for (budget_call = 1U; budget_call <= 12U; ++budget_call)
    {
        struct fake_host fake;
        p4_sdio_setup_ops_t ops;
        p4_sdio_setup_result_t setup_result;
        p4_sdio_restore_result_t result;
        unsigned int commands = budget_call / 2U;

        fake_init(&fake);
        fake.budget_fail_at = budget_call;
        fake.budget_fail_value = budget_call == 7U ? -1 : 0;
        setup_result = saved_setup_result(1U, 0xa5U, 0xb2a1U);
        ops = fake_ops(&fake);
        fake.result = &result;

        CHECK(p4_sdio_restore_function1(&ops, &setup_result, &result) == 0);
        CHECK(result.complete == 0U);
        CHECK(result.error == P4_SDIO_RESTORE_ERROR_BUDGET);
        CHECK(result.stage == stages[(budget_call - 1U) / 2U]);
        CHECK(fake.budget_calls == budget_call);
        CHECK(fake.calls == commands);
        CHECK(result.attempted == (commands == 0U ? 0U : 1U));
        CHECK(fake.writes == (commands < 3U ? commands : 3U));
        CHECK(fake.reads == (commands > 3U ? commands - 3U : 0U));
    }
}

static void test_each_readback_mismatch_is_reported(void)
{
    static const unsigned int calls[] = { 4U, 5U, 6U };
    static const p4_sdio_restore_stage_t stages[] = {
        P4_SDIO_RESTORE_STAGE_VERIFY_BLOCK_SIZE_LOW,
        P4_SDIO_RESTORE_STAGE_VERIFY_BLOCK_SIZE_HIGH,
        P4_SDIO_RESTORE_STAGE_VERIFY_IO_ENABLE
    };
    unsigned int i;

    for (i = 0U; i < 3U; ++i)
    {
        struct fake_host fake;
        p4_sdio_setup_ops_t ops;
        p4_sdio_setup_result_t setup_result;
        p4_sdio_restore_result_t result;

        fake_init(&fake);
        fake.override_read_call = calls[i];
        fake.override_read_value = 0xeeU;
        setup_result = saved_setup_result(1U, 0xa5U, 0xb2a1U);
        ops = fake_ops(&fake);
        fake.result = &result;

        CHECK(p4_sdio_restore_function1(&ops, &setup_result, &result) == 0);
        CHECK(result.error == P4_SDIO_RESTORE_ERROR_MISMATCH);
        CHECK(result.stage == stages[i]);
        CHECK(result.attempted == 1U && result.complete == 0U);
        CHECK(fake.calls == 6U && fake.budget_calls == 12U);
        CHECK(result.observed_block_size_low_valid == 1U);
        CHECK(result.observed_block_size_high_valid == 1U);
        CHECK(result.observed_io_enable_valid == 1U);
    }
}

int main(void)
{
    test_success_restores_saved_values_and_peer_bits();
    test_no_mutation_is_a_noop_success();
    test_invalid_arguments_and_missing_snapshots();
    test_every_command_failure_and_ambiguous_write();
    test_budget_before_and_after_every_command();
    test_each_readback_mismatch_is_reported();
    printf("PASS: %u SDIO restore checks\n", checks);
    return 0;
}
