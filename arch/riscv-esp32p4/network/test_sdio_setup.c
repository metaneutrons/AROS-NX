/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sdio_setup.h"

static unsigned int checks;
#define CHECK(expr) do { ++checks; if (!(expr)) { \
    fprintf(stderr, "line %u: %s\n", (unsigned int)__LINE__, #expr); \
    exit(1); } } while (0)

struct fake_host
{
    p4_sdio_setup_result_t *result;
    unsigned int calls;
    unsigned int reads;
    unsigned int writes;
    unsigned int io_enable_writes;
    unsigned int block_size_writes;
    unsigned int budget_calls;
    unsigned int yields;
    unsigned int ready_polls;
    unsigned int not_ready_polls;
    unsigned int fail_call;
    unsigned int budget_fail_at;
    uint8_t registers[0x112];
    uint8_t never_ready;
    uint8_t mismatch_readback;
    uint8_t mutate_failed_write;
    uint8_t ignore_io_enable_write;
};

static void fake_init(struct fake_host *fake, uint8_t io_enable)
{
    memset(fake, 0, sizeof(*fake));
    fake->registers[0x02] = io_enable;
    fake->registers[0x110] = 0x34;
    fake->registers[0x111] = 0x12;
}

static int fake_fail_command(struct fake_host *fake, unsigned int call)
{
    return fake->fail_call != 0U && fake->fail_call == call;
}

static int fake_read_register(void *context, unsigned int function,
    uint32_t address, uint8_t *value)
{
    struct fake_host *fake = (struct fake_host *)context;
    unsigned int call = ++fake->calls;
    uint8_t observed;

    CHECK(function == 0U);
    CHECK(value != NULL);
    CHECK(address == 0x02U || address == 0x03U ||
        address == 0x110U || address == 0x111U);
    ++fake->reads;

    if (address == 0x03U)
    {
        ++fake->ready_polls;
        observed = (fake->never_ready ||
            fake->ready_polls <= fake->not_ready_polls) ? 0U : 0x02U;
    }
    else if (address == 0x111U && fake->mismatch_readback &&
        fake->block_size_writes > 0U)
        observed = 0x03U;
    else
        observed = fake->registers[address];

    if (fake_fail_command(fake, call))
    {
        /* The helper must not publish callback output from a failed read. */
        *value = 0xeeU;
        return 0;
    }
    *value = observed;
    return 1;
}

static int fake_write_register(void *context, unsigned int function,
    uint32_t address, uint8_t value)
{
    struct fake_host *fake = (struct fake_host *)context;
    unsigned int call = ++fake->calls;
    int failed = fake_fail_command(fake, call);

    CHECK(function == 0U);
    CHECK(fake->result != NULL);
    CHECK(fake->result->mutation_attempted == 1U);
    ++fake->writes;
    if (address == 0x02U)
    {
        CHECK(value == (uint8_t)(fake->registers[0x02] | 0x02U));
        ++fake->io_enable_writes;
    }
    else if (address == 0x110U)
    {
        CHECK(value == 0U);
        ++fake->block_size_writes;
    }
    else if (address == 0x111U)
    {
        CHECK(value == 2U);
        ++fake->block_size_writes;
    }
    else
        CHECK(0 && "unexpected write (function2, interrupt, or reset)");

    if ((!failed || fake->mutate_failed_write) &&
        !(address == 0x02U && fake->ignore_io_enable_write))
        fake->registers[address] = value;
    return failed ? 0 : 1;
}

static int fake_budget_ok(void *context)
{
    struct fake_host *fake = (struct fake_host *)context;
    ++fake->budget_calls;
    return fake->budget_fail_at == 0U ||
        fake->budget_calls != fake->budget_fail_at;
}

static void fake_yield(void *context)
{
    struct fake_host *fake = (struct fake_host *)context;
    ++fake->yields;
}

static p4_sdio_setup_ops_t fake_ops(struct fake_host *fake)
{
    p4_sdio_setup_ops_t ops;
    ops.context = fake;
    ops.read_register = fake_read_register;
    ops.write_register = fake_write_register;
    ops.budget_ok = fake_budget_ok;
    ops.yield = fake_yield;
    return ops;
}

static int run_setup(struct fake_host *fake, p4_sdio_setup_result_t *result)
{
    p4_sdio_setup_ops_t ops = fake_ops(fake);
    fake->result = result;
    return p4_sdio_setup_function1(&ops, result);
}

static void test_success_and_peer_preservation(void)
{
    struct fake_host fake;
    p4_sdio_setup_result_t result;

    fake_init(&fake, 0x00U);
    CHECK(run_setup(&fake, &result) == 1);
    CHECK(result.complete == 1U);
    CHECK(result.stage == P4_SDIO_SETUP_STAGE_COMPLETE);
    CHECK(result.error == P4_SDIO_SETUP_ERROR_NONE);
    CHECK(result.previous_io_enable_valid == 1U);
    CHECK(result.previous_io_enable == 0x00U);
    CHECK(result.observed_io_enable_valid == 1U);
    CHECK(result.observed_io_enable == 0x02U);
    CHECK(result.observed_io_ready_valid == 1U);
    CHECK(result.observed_io_ready == 0x02U);
    CHECK(result.original_block_size_valid == 1U);
    CHECK(result.original_block_size == 0x1234U);
    CHECK(result.observed_block_size_valid == 1U);
    CHECK(result.observed_block_size == 512U);
    CHECK(result.mutation_attempted == 1U);
    CHECK(result.poll_count == 1U);
    CHECK(fake.registers[0x02] == 0x02U);
    CHECK(fake.registers[0x110] == 0U && fake.registers[0x111] == 2U);
    CHECK(fake.io_enable_writes == 1U && fake.block_size_writes == 2U);
    CHECK(fake.writes == 3U && fake.calls == 10U);
    CHECK(fake.budget_calls == 20U && fake.yields == 0U);

    /* Preserve the pre-existing peer enable bit while enabling bit 1. */
    fake_init(&fake, 0x04U);
    fake.not_ready_polls = 2U;
    CHECK(run_setup(&fake, &result) == 1);
    CHECK(result.previous_io_enable == 0x04U);
    CHECK(fake.registers[0x02] == 0x06U);
    CHECK(fake.io_enable_writes == 1U);
    CHECK(result.observed_io_enable == 0x06U);
    CHECK(result.observed_io_ready == 0x02U);
    CHECK(result.poll_count == 3U && fake.yields == 2U);

    /* An already-enabled function is still polled; no IO_ENABLE write occurs. */
    fake_init(&fake, 0x02U);
    CHECK(run_setup(&fake, &result) == 1);
    CHECK(fake.io_enable_writes == 0U);
    CHECK(result.previous_io_enable == 0x02U);
    CHECK(result.mutation_attempted == 1U); /* FBR writes were attempted. */
}

static void test_command_failures_each_phase(void)
{
    static const p4_sdio_setup_stage_t stages[] = {
        P4_SDIO_SETUP_STAGE_READ_IO_ENABLE,
        P4_SDIO_SETUP_STAGE_READ_ORIGINAL_BLOCK_SIZE_LOW,
        P4_SDIO_SETUP_STAGE_READ_ORIGINAL_BLOCK_SIZE_HIGH,
        P4_SDIO_SETUP_STAGE_WRITE_IO_ENABLE,
        P4_SDIO_SETUP_STAGE_READBACK_IO_ENABLE,
        P4_SDIO_SETUP_STAGE_POLL_IO_READY,
        P4_SDIO_SETUP_STAGE_WRITE_BLOCK_SIZE_LOW,
        P4_SDIO_SETUP_STAGE_WRITE_BLOCK_SIZE_HIGH,
        P4_SDIO_SETUP_STAGE_READBACK_BLOCK_SIZE_LOW,
        P4_SDIO_SETUP_STAGE_READBACK_BLOCK_SIZE_HIGH
    };
    unsigned int call;

    for (call = 1U; call <= sizeof(stages) / sizeof(stages[0]); ++call)
    {
        struct fake_host fake;
        p4_sdio_setup_result_t result;

        fake_init(&fake, 0U);
        fake.fail_call = call;
        fake.mutate_failed_write = 1U;
        CHECK(run_setup(&fake, &result) == 0);
        CHECK(result.complete == 0U);
        CHECK(result.error == P4_SDIO_SETUP_ERROR_COMMAND);
        CHECK(result.stage == stages[call - 1U]);
        CHECK(fake.calls == call);
        CHECK(fake.budget_calls == call * 2U);
        CHECK(result.mutation_attempted == (call >= 4U ? 1U : 0U));
    }

    /* A failed read that scribbles into its callback output cannot publish it. */
    {
        struct fake_host fake;
        p4_sdio_setup_result_t result;

        fake_init(&fake, 0U);
        fake.fail_call = 1U;
        CHECK(run_setup(&fake, &result) == 0);
        CHECK(result.previous_io_enable == 0U);
        CHECK(result.previous_io_enable_valid == 0U);
        CHECK(result.original_block_size_valid == 0U);
    }
}

static void test_budget_before_and_after_every_command(void)
{
    unsigned int budget_call;

    /* The successful path has ten commands, each with pre/post checks. */
    for (budget_call = 1U; budget_call <= 20U; ++budget_call)
    {
        struct fake_host fake;
        p4_sdio_setup_result_t result;

        fake_init(&fake, 0U);
        fake.budget_fail_at = budget_call;
        CHECK(run_setup(&fake, &result) == 0);
        CHECK(result.complete == 0U);
        CHECK(result.error == P4_SDIO_SETUP_ERROR_BUDGET);
        CHECK(fake.budget_calls == budget_call);
        CHECK(fake.calls == budget_call / 2U);
        CHECK(result.mutation_attempted ==
            (fake.calls >= 4U ? 1U : 0U));
    }
}

static void test_readiness_timeout_and_stopped_budget(void)
{
    struct fake_host fake;
    p4_sdio_setup_result_t result;

    fake_init(&fake, 0U);
    fake.never_ready = 1U;
    CHECK(run_setup(&fake, &result) == 0);
    CHECK(result.error == P4_SDIO_SETUP_ERROR_NOT_READY);
    CHECK(result.stage == P4_SDIO_SETUP_STAGE_POLL_IO_READY);
    CHECK(result.complete == 0U && result.mutation_attempted == 1U);
    CHECK(result.observed_io_ready_valid == 1U);
    CHECK(result.observed_io_ready == 0U);
    CHECK(result.poll_count == P4_SDIO_SETUP_MAX_POLLS);
    CHECK(fake.ready_polls == P4_SDIO_SETUP_MAX_POLLS);
    CHECK(fake.yields == P4_SDIO_SETUP_MAX_POLLS - 1U);
    CHECK(fake.calls == P4_SDIO_SETUP_MAX_POLLS + 5U);
    CHECK(fake.block_size_writes == 0U);

    /* Simulate a stopped timer/budget after five bounded not-ready polls. */
    fake_init(&fake, 0U);
    fake.not_ready_polls = P4_SDIO_SETUP_MAX_POLLS;
    fake.budget_fail_at = 21U; /* Before the sixth CMD52 readiness poll. */
    CHECK(run_setup(&fake, &result) == 0);
    CHECK(result.error == P4_SDIO_SETUP_ERROR_BUDGET);
    CHECK(result.stage == P4_SDIO_SETUP_STAGE_POLL_IO_READY);
    CHECK(result.poll_count == 5U && fake.ready_polls == 5U);
    CHECK(fake.yields == 5U);
    CHECK(fake.budget_calls == 21U);
}

static void test_block_size_readback_mismatch(void)
{
    struct fake_host fake;
    p4_sdio_setup_result_t result;

    fake_init(&fake, 0U);
    fake.mismatch_readback = 1U;
    CHECK(run_setup(&fake, &result) == 0);
    CHECK(result.error == P4_SDIO_SETUP_ERROR_BLOCK_SIZE_MISMATCH);
    CHECK(result.stage == P4_SDIO_SETUP_STAGE_READBACK_BLOCK_SIZE_HIGH);
    CHECK(result.complete == 0U);
    CHECK(result.original_block_size_valid == 1U);
    CHECK(result.original_block_size == 0x1234U);
    CHECK(result.observed_block_size_valid == 1U);
    CHECK(result.observed_block_size == 0x0300U);
    CHECK(result.mutation_attempted == 1U);
}

static void test_io_ready_does_not_prove_io_enable(void)
{
    struct fake_host fake;
    p4_sdio_setup_result_t result;

    fake_init(&fake, 0U);
    fake.ignore_io_enable_write = 1U;
    CHECK(run_setup(&fake, &result) == 0);
    CHECK(result.error == P4_SDIO_SETUP_ERROR_IO_ENABLE_MISMATCH);
    CHECK(result.stage == P4_SDIO_SETUP_STAGE_READBACK_IO_ENABLE);
    CHECK(result.observed_io_enable_valid == 1U);
    CHECK(result.observed_io_enable == 0U);
    CHECK(result.mutation_attempted == 1U);
    CHECK(fake.ready_polls == 0U);
    CHECK(fake.block_size_writes == 0U);
}

static void test_bad_arguments_and_write_failure_mutation(void)
{
    struct fake_host fake;
    p4_sdio_setup_ops_t ops;
    p4_sdio_setup_result_t result;

    memset(&result, 0xa5, sizeof(result));
    CHECK(p4_sdio_setup_function1(NULL, &result) == 0);
    CHECK(result.stage == P4_SDIO_SETUP_STAGE_START);
    CHECK(result.error == P4_SDIO_SETUP_ERROR_INVALID_ARGUMENT);
    CHECK(result.complete == 0U && result.mutation_attempted == 0U);
    CHECK(p4_sdio_setup_function1(NULL, NULL) == 0);

    fake_init(&fake, 0U);
    ops = fake_ops(&fake);
    ops.write_register = NULL;
    CHECK(p4_sdio_setup_function1(&ops, &result) == 0);
    CHECK(result.error == P4_SDIO_SETUP_ERROR_INVALID_ARGUMENT);
    CHECK(fake.calls == 0U && fake.budget_calls == 0U);

    fake_init(&fake, 0U);
    ops = fake_ops(&fake);
    ops.yield = NULL;
    CHECK(p4_sdio_setup_function1(&ops, &result) == 0);
    CHECK(result.error == P4_SDIO_SETUP_ERROR_INVALID_ARGUMENT);
    CHECK(fake.calls == 0U && fake.budget_calls == 0U);

    /* The write callback may mutate and still report failure; the result says
     * restoration may be required and retains the snapshots needed for it. */
    fake_init(&fake, 0U);
    fake.fail_call = 4U;
    fake.mutate_failed_write = 1U;
    CHECK(run_setup(&fake, &result) == 0);
    CHECK(result.error == P4_SDIO_SETUP_ERROR_COMMAND);
    CHECK(result.stage == P4_SDIO_SETUP_STAGE_WRITE_IO_ENABLE);
    CHECK(result.mutation_attempted == 1U);
    CHECK(result.previous_io_enable_valid == 1U);
    CHECK(result.previous_io_enable == 0U);
    CHECK(result.original_block_size_valid == 1U);
    CHECK(result.original_block_size == 0x1234U);
    CHECK(fake.registers[0x02] == 0x02U);
}

int main(void)
{
    test_success_and_peer_preservation();
    test_command_failures_each_phase();
    test_budget_before_and_after_every_command();
    test_readiness_timeout_and_stopped_budget();
    test_block_size_readback_mismatch();
    test_io_ready_does_not_prove_io_enable();
    test_bad_arguments_and_write_failure_mutation();
    printf("PASS: %u SDIO setup checks\n", checks);
    return 0;
}
