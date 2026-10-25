/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#include "sdio_restore.h"

#define P4_SDIO_RESTORE_FUNCTION0             0U
#define P4_SDIO_RESTORE_CCCR_IO_ENABLE        UINT32_C(0x02)
#define P4_SDIO_RESTORE_FBR1_BLOCK_SIZE_LOW   UINT32_C(0x110)
#define P4_SDIO_RESTORE_FBR1_BLOCK_SIZE_HIGH  UINT32_C(0x111)

static void p4_sdio_restore_zero_result(p4_sdio_restore_result_t *result)
{
    result->stage = P4_SDIO_RESTORE_STAGE_START;
    result->error = P4_SDIO_RESTORE_ERROR_NONE;
    result->observed_block_size_low = 0;
    result->observed_block_size_low_valid = 0;
    result->observed_block_size_high = 0;
    result->observed_block_size_high_valid = 0;
    result->observed_io_enable = 0;
    result->observed_io_enable_valid = 0;
    result->attempted = 0;
    result->complete = 0;
}

static int p4_sdio_restore_fail(p4_sdio_restore_result_t *result,
    p4_sdio_restore_error_t error)
{
    result->error = error;
    return 0;
}

static int p4_sdio_restore_read(const p4_sdio_setup_ops_t *ops,
    p4_sdio_restore_result_t *result, p4_sdio_restore_stage_t stage,
    uint32_t address, uint8_t *value)
{
    uint8_t callback_value = 0;
    int command_ok;
    int budget_after;

    result->stage = stage;
    if (ops->budget_ok(ops->context) <= 0)
        return p4_sdio_restore_fail(result, P4_SDIO_RESTORE_ERROR_BUDGET);

    command_ok = ops->read_register(ops->context,
        P4_SDIO_RESTORE_FUNCTION0, address, &callback_value);
    budget_after = ops->budget_ok(ops->context) > 0;
    if (!budget_after)
        return p4_sdio_restore_fail(result, P4_SDIO_RESTORE_ERROR_BUDGET);
    if (command_ok <= 0)
        return p4_sdio_restore_fail(result, P4_SDIO_RESTORE_ERROR_COMMAND);

    *value = callback_value;
    return 1;
}

static int p4_sdio_restore_write(const p4_sdio_setup_ops_t *ops,
    p4_sdio_restore_result_t *result, p4_sdio_restore_stage_t stage,
    uint32_t address, uint8_t value)
{
    int command_ok;
    int budget_after;

    result->stage = stage;
    if (ops->budget_ok(ops->context) <= 0)
        return p4_sdio_restore_fail(result, P4_SDIO_RESTORE_ERROR_BUDGET);

    /* A failed callback may still have reached the card. */
    result->attempted = 1;
    command_ok = ops->write_register(ops->context,
        P4_SDIO_RESTORE_FUNCTION0, address, value);
    budget_after = ops->budget_ok(ops->context) > 0;
    if (!budget_after)
        return p4_sdio_restore_fail(result, P4_SDIO_RESTORE_ERROR_BUDGET);
    if (command_ok <= 0)
        return p4_sdio_restore_fail(result, P4_SDIO_RESTORE_ERROR_COMMAND);

    return 1;
}

int p4_sdio_restore_function1(const p4_sdio_setup_ops_t *ops,
    const p4_sdio_setup_result_t *setup_result,
    p4_sdio_restore_result_t *result)
{
    uint8_t observed_block_size_low = 0;
    uint8_t observed_block_size_high = 0;
    uint8_t observed_io_enable = 0;
    uint8_t original_block_size_low;
    uint8_t original_block_size_high;

    if (!result)
        return 0;
    p4_sdio_restore_zero_result(result);
    if (!setup_result)
        return p4_sdio_restore_fail(result,
            P4_SDIO_RESTORE_ERROR_INVALID_ARGUMENT);

    if (!setup_result->mutation_attempted)
    {
        result->stage = P4_SDIO_RESTORE_STAGE_COMPLETE;
        result->complete = 1;
        return 1;
    }

    if (!ops || !ops->read_register || !ops->write_register ||
        !ops->budget_ok)
        return p4_sdio_restore_fail(result,
            P4_SDIO_RESTORE_ERROR_INVALID_ARGUMENT);
    if (!setup_result->previous_io_enable_valid ||
        !setup_result->original_block_size_valid)
        return p4_sdio_restore_fail(result,
            P4_SDIO_RESTORE_ERROR_SNAPSHOT_UNAVAILABLE);

    original_block_size_low =
        (uint8_t)(setup_result->original_block_size & UINT16_C(0x00ff));
    original_block_size_high =
        (uint8_t)(setup_result->original_block_size >> 8);

    if (!p4_sdio_restore_write(ops, result,
            P4_SDIO_RESTORE_STAGE_WRITE_BLOCK_SIZE_LOW,
            P4_SDIO_RESTORE_FBR1_BLOCK_SIZE_LOW,
            original_block_size_low))
        return 0;
    if (!p4_sdio_restore_write(ops, result,
            P4_SDIO_RESTORE_STAGE_WRITE_BLOCK_SIZE_HIGH,
            P4_SDIO_RESTORE_FBR1_BLOCK_SIZE_HIGH,
            original_block_size_high))
        return 0;
    if (!p4_sdio_restore_write(ops, result,
            P4_SDIO_RESTORE_STAGE_WRITE_IO_ENABLE,
            P4_SDIO_RESTORE_CCCR_IO_ENABLE,
            setup_result->previous_io_enable))
        return 0;

    if (!p4_sdio_restore_read(ops, result,
            P4_SDIO_RESTORE_STAGE_VERIFY_BLOCK_SIZE_LOW,
            P4_SDIO_RESTORE_FBR1_BLOCK_SIZE_LOW,
            &observed_block_size_low))
        return 0;
    result->observed_block_size_low = observed_block_size_low;
    result->observed_block_size_low_valid = 1;

    if (!p4_sdio_restore_read(ops, result,
            P4_SDIO_RESTORE_STAGE_VERIFY_BLOCK_SIZE_HIGH,
            P4_SDIO_RESTORE_FBR1_BLOCK_SIZE_HIGH,
            &observed_block_size_high))
        return 0;
    result->observed_block_size_high = observed_block_size_high;
    result->observed_block_size_high_valid = 1;

    if (!p4_sdio_restore_read(ops, result,
            P4_SDIO_RESTORE_STAGE_VERIFY_IO_ENABLE,
            P4_SDIO_RESTORE_CCCR_IO_ENABLE, &observed_io_enable))
        return 0;
    result->observed_io_enable = observed_io_enable;
    result->observed_io_enable_valid = 1;

    if (observed_block_size_low != original_block_size_low)
    {
        result->stage = P4_SDIO_RESTORE_STAGE_VERIFY_BLOCK_SIZE_LOW;
        return p4_sdio_restore_fail(result,
            P4_SDIO_RESTORE_ERROR_MISMATCH);
    }
    if (observed_block_size_high != original_block_size_high)
    {
        result->stage = P4_SDIO_RESTORE_STAGE_VERIFY_BLOCK_SIZE_HIGH;
        return p4_sdio_restore_fail(result,
            P4_SDIO_RESTORE_ERROR_MISMATCH);
    }
    if (observed_io_enable != setup_result->previous_io_enable)
    {
        result->stage = P4_SDIO_RESTORE_STAGE_VERIFY_IO_ENABLE;
        return p4_sdio_restore_fail(result,
            P4_SDIO_RESTORE_ERROR_MISMATCH);
    }

    result->stage = P4_SDIO_RESTORE_STAGE_COMPLETE;
    result->error = P4_SDIO_RESTORE_ERROR_NONE;
    result->complete = 1;
    return 1;
}
