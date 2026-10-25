/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#include "sdio_setup.h"

#define P4_SDIO_SETUP_FUNCTION0             0U
#define P4_SDIO_SETUP_CCCR_IO_ENABLE        UINT32_C(0x02)
#define P4_SDIO_SETUP_CCCR_IO_READY          UINT32_C(0x03)
#define P4_SDIO_SETUP_IO_ENABLE_FUNCTION1   UINT8_C(0x02)
#define P4_SDIO_SETUP_IO_READY_FUNCTION1    UINT8_C(0x02)
#define P4_SDIO_SETUP_FBR1_BLOCK_SIZE_LOW   UINT32_C(0x110)
#define P4_SDIO_SETUP_FBR1_BLOCK_SIZE_HIGH  UINT32_C(0x111)
#define P4_SDIO_SETUP_BLOCK_SIZE_LOW_VALUE  UINT8_C(0x00)
#define P4_SDIO_SETUP_BLOCK_SIZE_HIGH_VALUE UINT8_C(0x02)
#define P4_SDIO_SETUP_BLOCK_SIZE            UINT16_C(512)

static void p4_sdio_setup_zero_result(p4_sdio_setup_result_t *result)
{
    volatile unsigned char *bytes = (volatile unsigned char *)result;
    unsigned int i;

    for (i = 0; i < sizeof(*result); ++i)
        bytes[i] = 0;
}

static int p4_sdio_setup_fail(p4_sdio_setup_result_t *result,
    p4_sdio_setup_error_t error)
{
    result->error = error;
    return 0;
}

static int p4_sdio_setup_read(const p4_sdio_setup_ops_t *ops,
    p4_sdio_setup_result_t *result, p4_sdio_setup_stage_t stage,
    uint32_t address, uint8_t *value, int count_poll)
{
    uint8_t callback_value = 0;
    int command_ok;
    int budget_after;

    result->stage = stage;
    if (ops->budget_ok(ops->context) <= 0)
        return p4_sdio_setup_fail(result, P4_SDIO_SETUP_ERROR_BUDGET);

    if (count_poll)
        ++result->poll_count;
    command_ok = ops->read_register(ops->context, P4_SDIO_SETUP_FUNCTION0,
        address, &callback_value);
    budget_after = ops->budget_ok(ops->context) > 0;
    if (!budget_after)
        return p4_sdio_setup_fail(result, P4_SDIO_SETUP_ERROR_BUDGET);
    if (command_ok <= 0)
        return p4_sdio_setup_fail(result, P4_SDIO_SETUP_ERROR_COMMAND);

    *value = callback_value;
    return 1;
}

static int p4_sdio_setup_write(const p4_sdio_setup_ops_t *ops,
    p4_sdio_setup_result_t *result, p4_sdio_setup_stage_t stage,
    uint32_t address, uint8_t value)
{
    int command_ok;
    int budget_after;

    result->stage = stage;
    if (ops->budget_ok(ops->context) <= 0)
        return p4_sdio_setup_fail(result, P4_SDIO_SETUP_ERROR_BUDGET);

    /* A failed callback may still have reached the card. */
    result->mutation_attempted = 1;
    command_ok = ops->write_register(ops->context, P4_SDIO_SETUP_FUNCTION0,
        address, value);
    budget_after = ops->budget_ok(ops->context) > 0;
    if (!budget_after)
        return p4_sdio_setup_fail(result, P4_SDIO_SETUP_ERROR_BUDGET);
    if (command_ok <= 0)
        return p4_sdio_setup_fail(result, P4_SDIO_SETUP_ERROR_COMMAND);

    return 1;
}

int p4_sdio_setup_function1(const p4_sdio_setup_ops_t *ops,
    p4_sdio_setup_result_t *result)
{
    uint8_t io_enable = 0;
    uint8_t block_size_low = 0;
    uint8_t block_size_high = 0;
    uint8_t observed_io_enable = 0;
    uint8_t io_ready = 0;
    uint8_t expected_io_enable;
    unsigned int poll;

    if (!result)
        return 0;
    p4_sdio_setup_zero_result(result);
    if (!ops || !ops->read_register || !ops->write_register ||
        !ops->budget_ok || !ops->yield)
        return p4_sdio_setup_fail(result,
            P4_SDIO_SETUP_ERROR_INVALID_ARGUMENT);

    if (!p4_sdio_setup_read(ops, result,
            P4_SDIO_SETUP_STAGE_READ_IO_ENABLE,
            P4_SDIO_SETUP_CCCR_IO_ENABLE, &io_enable, 0))
        return 0;
    result->previous_io_enable = io_enable;
    result->previous_io_enable_valid = 1;

    if (!p4_sdio_setup_read(ops, result,
            P4_SDIO_SETUP_STAGE_READ_ORIGINAL_BLOCK_SIZE_LOW,
            P4_SDIO_SETUP_FBR1_BLOCK_SIZE_LOW, &block_size_low, 0))
        return 0;
    if (!p4_sdio_setup_read(ops, result,
            P4_SDIO_SETUP_STAGE_READ_ORIGINAL_BLOCK_SIZE_HIGH,
            P4_SDIO_SETUP_FBR1_BLOCK_SIZE_HIGH, &block_size_high, 0))
        return 0;
    result->original_block_size = (uint16_t)((uint16_t)block_size_low |
        (uint16_t)((uint16_t)block_size_high << 8));
    result->original_block_size_valid = 1;

    expected_io_enable = (uint8_t)(io_enable |
        P4_SDIO_SETUP_IO_ENABLE_FUNCTION1);
    if ((io_enable & P4_SDIO_SETUP_IO_ENABLE_FUNCTION1) == 0)
    {
        if (!p4_sdio_setup_write(ops, result,
                P4_SDIO_SETUP_STAGE_WRITE_IO_ENABLE,
                P4_SDIO_SETUP_CCCR_IO_ENABLE,
                expected_io_enable))
            return 0;
    }

    if (!p4_sdio_setup_read(ops, result,
            P4_SDIO_SETUP_STAGE_READBACK_IO_ENABLE,
            P4_SDIO_SETUP_CCCR_IO_ENABLE, &observed_io_enable, 0))
        return 0;
    result->observed_io_enable = observed_io_enable;
    result->observed_io_enable_valid = 1;
    if (observed_io_enable != expected_io_enable)
        return p4_sdio_setup_fail(result,
            P4_SDIO_SETUP_ERROR_IO_ENABLE_MISMATCH);

    for (poll = 0; poll < P4_SDIO_SETUP_MAX_POLLS; ++poll)
    {
        if (!p4_sdio_setup_read(ops, result,
                P4_SDIO_SETUP_STAGE_POLL_IO_READY,
                P4_SDIO_SETUP_CCCR_IO_READY, &io_ready, 1))
            return 0;
        result->observed_io_ready = io_ready;
        result->observed_io_ready_valid = 1;
        if ((io_ready & P4_SDIO_SETUP_IO_READY_FUNCTION1) != 0)
            break;
        if (poll + 1U < P4_SDIO_SETUP_MAX_POLLS)
            ops->yield(ops->context);
    }
    if ((io_ready & P4_SDIO_SETUP_IO_READY_FUNCTION1) == 0)
        return p4_sdio_setup_fail(result, P4_SDIO_SETUP_ERROR_NOT_READY);

    if (!p4_sdio_setup_write(ops, result,
            P4_SDIO_SETUP_STAGE_WRITE_BLOCK_SIZE_LOW,
            P4_SDIO_SETUP_FBR1_BLOCK_SIZE_LOW,
            P4_SDIO_SETUP_BLOCK_SIZE_LOW_VALUE))
        return 0;
    if (!p4_sdio_setup_write(ops, result,
            P4_SDIO_SETUP_STAGE_WRITE_BLOCK_SIZE_HIGH,
            P4_SDIO_SETUP_FBR1_BLOCK_SIZE_HIGH,
            P4_SDIO_SETUP_BLOCK_SIZE_HIGH_VALUE))
        return 0;

    if (!p4_sdio_setup_read(ops, result,
            P4_SDIO_SETUP_STAGE_READBACK_BLOCK_SIZE_LOW,
            P4_SDIO_SETUP_FBR1_BLOCK_SIZE_LOW, &block_size_low, 0))
        return 0;
    if (!p4_sdio_setup_read(ops, result,
            P4_SDIO_SETUP_STAGE_READBACK_BLOCK_SIZE_HIGH,
            P4_SDIO_SETUP_FBR1_BLOCK_SIZE_HIGH, &block_size_high, 0))
        return 0;
    result->observed_block_size = (uint16_t)((uint16_t)block_size_low |
        (uint16_t)((uint16_t)block_size_high << 8));
    result->observed_block_size_valid = 1;
    if (result->observed_block_size != P4_SDIO_SETUP_BLOCK_SIZE)
        return p4_sdio_setup_fail(result,
            P4_SDIO_SETUP_ERROR_BLOCK_SIZE_MISMATCH);

    result->stage = P4_SDIO_SETUP_STAGE_COMPLETE;
    result->error = P4_SDIO_SETUP_ERROR_NONE;
    result->complete = 1;
    return 1;
}
