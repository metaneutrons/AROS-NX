/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#include "sdio_reset_query.h"
#include "sdio_commands.h"

static int reset_query_command(const p4_sdio_probe_ops_t *ops,
    p4_sdio_reset_query_result_t *result, p4_sdio_reset_query_stage_t stage,
    unsigned int index, uint32_t argument,
    p4_sdio_response_type_t response_type, uint32_t error_mask)
{
    result->stage = stage;
    result->command_index = index;
    result->response = 0;
    if (ops->budget_ok(ops->context) <= 0)
    {
        result->error = P4_SDIO_PROBE_ERROR_BUDGET;
        return 0;
    }
    if (stage == P4_SDIO_RESET_QUERY_RESET)
        result->reset_attempted = 1;
    if (ops->command(ops->context, index, argument, response_type,
                    error_mask, &result->response) <= 0)
    {
        result->error = P4_SDIO_PROBE_ERROR_COMMAND;
        return 0;
    }
    if (result->response & error_mask)
    {
        result->error = P4_SDIO_PROBE_ERROR_R5_STATUS;
        return 0;
    }
    if (ops->budget_ok(ops->context) <= 0)
    {
        result->error = P4_SDIO_PROBE_ERROR_BUDGET;
        return 0;
    }
    return 1;
}

int p4_sdio_reset_query(const p4_sdio_probe_ops_t *ops,
    p4_sdio_reset_query_result_t *result)
{
    volatile unsigned char *bytes;
    unsigned int i;
    uint32_t argument = 0;

    if (!result)
        return 0;
    bytes = (volatile unsigned char *)result;
    for (i = 0; i < sizeof(*result); ++i)
        bytes[i] = 0;
    if (!ops || !ops->command || !ops->budget_ok ||
        !p4_sdio_cmd52(1, 0, 0, UINT32_C(0x06), 0x08, &argument))
    {
        result->error = P4_SDIO_PROBE_ERROR_INVALID_ARGUMENT;
        return 0;
    }
    if (!reset_query_command(ops, result, P4_SDIO_RESET_QUERY_RESET,
                            52, argument, P4_SDIO_RESP_R5,
                            P4_SDIO_PROBE_R5_ERROR_MASK))
        return 0;
    result->reset_acknowledged = 1;
    if (!reset_query_command(ops, result, P4_SDIO_RESET_QUERY_CMD0,
                            0, 0, P4_SDIO_RESP_NONE, 0))
        return 0;
    if (!reset_query_command(ops, result, P4_SDIO_RESET_QUERY_CMD5,
                            5, 0, P4_SDIO_RESP_R4, 0))
        return 0;
    result->functions = (uint8_t)((result->response >> 28) & 7U);
    result->ocr = result->response & UINT32_C(0x00ffffff);
    if (!result->functions)
    {
        result->error = P4_SDIO_PROBE_ERROR_NO_FUNCTIONS;
        return 0;
    }
    if (!(result->ocr & P4_SDIO_PROBE_HOST_OCR))
    {
        result->error = P4_SDIO_PROBE_ERROR_NO_VOLTAGE;
        return 0;
    }
    result->stage = P4_SDIO_RESET_QUERY_COMPLETE;
    result->complete = 1;
    return 1;
}
