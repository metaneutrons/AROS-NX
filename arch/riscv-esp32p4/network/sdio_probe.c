/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#include "sdio_probe.h"
#include "sdio_commands.h"

#define P4_SDIO_CMD_GO_IDLE_STATE       0U
#define P4_SDIO_CMD_SEND_OP_COND         5U
#define P4_SDIO_CMD_SET_RELATIVE_ADDR    3U
#define P4_SDIO_CMD_SELECT_CARD          7U
#define P4_SDIO_CMD_IO_RW_DIRECT        52U

#define P4_SDIO_R4_READY                 UINT32_C(0x80000000)
#define P4_SDIO_R4_FUNCTIONS_MASK        UINT32_C(0x70000000)
#define P4_SDIO_R4_FUNCTIONS_SHIFT       28U
#define P4_SDIO_R4_OCR_MASK              UINT32_C(0x00ffffff)
#define P4_SDIO_R6_RCA_SHIFT             16U
#define P4_SDIO_R6_RCA_MASK              UINT32_C(0xffff0000)
#define P4_SDIO_R5_DATA_MASK             UINT32_C(0x000000ff)

static void p4_sdio_probe_zero_result(p4_sdio_probe_result_t *result)
{
    volatile unsigned char *bytes = (volatile unsigned char *)result;
    unsigned int i;

    for (i = 0; i < sizeof(*result); ++i)
        bytes[i] = 0;
}

static int p4_sdio_probe_fail(p4_sdio_probe_result_t *result,
    p4_sdio_probe_error_t error)
{
    result->error = error;
    return 0;
}

static p4_sdio_probe_error_t p4_sdio_probe_status_error(
    p4_sdio_response_type_t response_type, uint32_t response,
    uint32_t error_mask)
{
    if ((response & error_mask) == 0)
        return P4_SDIO_PROBE_ERROR_NONE;

    switch (response_type)
    {
        case P4_SDIO_RESP_R6:
            return P4_SDIO_PROBE_ERROR_R6_STATUS;
        case P4_SDIO_RESP_R1:
            return P4_SDIO_PROBE_ERROR_R1_STATUS;
        case P4_SDIO_RESP_R5:
            return P4_SDIO_PROBE_ERROR_R5_STATUS;
        default:
            return P4_SDIO_PROBE_ERROR_COMMAND;
    }
}

static int p4_sdio_probe_command(const p4_sdio_probe_ops_t *ops,
    p4_sdio_probe_result_t *result, p4_sdio_probe_stage_t stage,
    unsigned int index, uint32_t argument,
    p4_sdio_response_type_t response_type, uint32_t error_mask,
    uint32_t *response)
{
    p4_sdio_probe_error_t status_error;

    result->stage = stage;
    result->command_index = index;
    *response = 0;
    if (ops->budget_ok(ops->context) <= 0)
        return p4_sdio_probe_fail(result, P4_SDIO_PROBE_ERROR_BUDGET);

    if (ops->command(ops->context, index, argument, response_type,
            error_mask, response) <= 0)
    {
        status_error = p4_sdio_probe_status_error(response_type, *response,
            error_mask);
        if (status_error != P4_SDIO_PROBE_ERROR_NONE)
            return p4_sdio_probe_fail(result, status_error);
        return p4_sdio_probe_fail(result, P4_SDIO_PROBE_ERROR_COMMAND);
    }

    status_error = p4_sdio_probe_status_error(response_type, *response,
        error_mask);
    if (status_error != P4_SDIO_PROBE_ERROR_NONE)
        return p4_sdio_probe_fail(result, status_error);
    return 1;
}

static int p4_sdio_probe_read_cccr(const p4_sdio_probe_ops_t *ops,
    p4_sdio_probe_result_t *result, uint32_t address, uint8_t *value)
{
    uint32_t argument = 0;
    uint32_t response = 0;

    if (!p4_sdio_cmd52(0, 0, 0, address, 0, &argument))
        return p4_sdio_probe_fail(result,
            P4_SDIO_PROBE_ERROR_INVALID_ARGUMENT);
    if (!p4_sdio_probe_command(ops, result, result->stage,
            P4_SDIO_CMD_IO_RW_DIRECT, argument, P4_SDIO_RESP_R5,
            P4_SDIO_PROBE_R5_ERROR_MASK, &response))
        return 0;

    *value = (uint8_t)(response & P4_SDIO_R5_DATA_MASK);
    return 1;
}

int p4_sdio_probe_identify(const p4_sdio_probe_ops_t *ops,
    p4_sdio_probe_result_t *result)
{
    static const uint8_t cccr_addresses[] = {
        0x00U, 0x01U, 0x02U, 0x03U, 0x07U, 0x08U, 0x09U, 0x0aU, 0x0bU
    };
    uint32_t response = 0;
    uint32_t selected_ocr;
    uint16_t rca;
    uint8_t cccr_values[sizeof(cccr_addresses) / sizeof(cccr_addresses[0])];
    uint8_t fbr1_interface = 0;
    unsigned int i;
    unsigned int poll;
    int ready = 0;

    if (!result)
        return 0;
    p4_sdio_probe_zero_result(result);
    if (!ops || !ops->command || !ops->budget_ok)
        return p4_sdio_probe_fail(result,
            P4_SDIO_PROBE_ERROR_INVALID_ARGUMENT);

    if (!p4_sdio_probe_command(ops, result, P4_SDIO_PROBE_STAGE_CMD0,
            P4_SDIO_CMD_GO_IDLE_STATE, 0, P4_SDIO_RESP_NONE, 0,
            &response))
        return 0;

    if (!p4_sdio_probe_command(ops, result,
            P4_SDIO_PROBE_STAGE_CMD5_INITIAL, P4_SDIO_CMD_SEND_OP_COND, 0,
            P4_SDIO_RESP_R4, 0, &response))
        return 0;
    result->functions = (uint8_t)((response & P4_SDIO_R4_FUNCTIONS_MASK) >>
        P4_SDIO_R4_FUNCTIONS_SHIFT);
    result->ocr = response & P4_SDIO_R4_OCR_MASK;
    if (result->functions == 0)
        return p4_sdio_probe_fail(result, P4_SDIO_PROBE_ERROR_NO_FUNCTIONS);

    selected_ocr = result->ocr & P4_SDIO_PROBE_HOST_OCR;
    if (selected_ocr == 0)
        return p4_sdio_probe_fail(result, P4_SDIO_PROBE_ERROR_NO_VOLTAGE);

    for (poll = 0; poll < P4_SDIO_PROBE_MAX_POLLS; ++poll)
    {
        if (!p4_sdio_probe_command(ops, result,
                P4_SDIO_PROBE_STAGE_CMD5_POLL, P4_SDIO_CMD_SEND_OP_COND,
                selected_ocr, P4_SDIO_RESP_R4, 0, &response))
            return 0;
        result->ocr = response & P4_SDIO_R4_OCR_MASK;
        if ((response & P4_SDIO_R4_FUNCTIONS_MASK) == 0)
            return p4_sdio_probe_fail(result,
                P4_SDIO_PROBE_ERROR_NO_FUNCTIONS);
        if ((result->ocr & selected_ocr) == 0)
            return p4_sdio_probe_fail(result,
                P4_SDIO_PROBE_ERROR_NO_VOLTAGE);
        if (response & P4_SDIO_R4_READY)
        {
            ready = 1;
            break;
        }
        if (poll + 1U < P4_SDIO_PROBE_MAX_POLLS && ops->yield)
            ops->yield(ops->context);
    }
    if (!ready)
        return p4_sdio_probe_fail(result, P4_SDIO_PROBE_ERROR_NOT_READY);

    if (!p4_sdio_probe_command(ops, result, P4_SDIO_PROBE_STAGE_CMD3,
            P4_SDIO_CMD_SET_RELATIVE_ADDR, 0, P4_SDIO_RESP_R6,
            P4_SDIO_PROBE_R6_ERROR_MASK, &response))
        return 0;
    rca = (uint16_t)((response & P4_SDIO_R6_RCA_MASK) >>
        P4_SDIO_R6_RCA_SHIFT);
    if (rca == 0)
        return p4_sdio_probe_fail(result, P4_SDIO_PROBE_ERROR_INVALID_RCA);
    result->rca = rca;

    if (!p4_sdio_probe_command(ops, result, P4_SDIO_PROBE_STAGE_CMD7,
            P4_SDIO_CMD_SELECT_CARD, (uint32_t)rca << 16,
            P4_SDIO_RESP_R1, P4_SDIO_PROBE_R1_ERROR_MASK, &response))
        return 0;

    result->stage = P4_SDIO_PROBE_STAGE_CCCR;
    for (i = 0; i < sizeof(cccr_addresses) / sizeof(cccr_addresses[0]); ++i)
    {
        result->stage = P4_SDIO_PROBE_STAGE_CCCR;
        if (!p4_sdio_probe_read_cccr(ops, result, cccr_addresses[i],
                &cccr_values[i]))
            return 0;
    }
    result->cccr_revision = (uint8_t)(cccr_values[0] & 0x0fU);
    result->sdio_revision = (uint8_t)(cccr_values[0] >> 4);
    result->io_enable = cccr_values[2];
    result->io_ready = cccr_values[3];
    result->bus_interface = cccr_values[4];
    result->capabilities = cccr_values[5];
    result->cis_pointer = (uint32_t)cccr_values[6] |
        ((uint32_t)cccr_values[7] << 8) |
        ((uint32_t)cccr_values[8] << 16);

    result->stage = P4_SDIO_PROBE_STAGE_FBR1;
    if (!p4_sdio_probe_read_cccr(ops, result, UINT32_C(0x100),
            &fbr1_interface))
        return 0;
    result->fbr1_interface = fbr1_interface;
    result->stage = P4_SDIO_PROBE_STAGE_FBR1;
    result->command_index = P4_SDIO_CMD_IO_RW_DIRECT;
    if (ops->budget_ok(ops->context) <= 0)
        return p4_sdio_probe_fail(result, P4_SDIO_PROBE_ERROR_BUDGET);

    result->stage = P4_SDIO_PROBE_STAGE_COMPLETE;
    result->complete = 1;
    result->error = P4_SDIO_PROBE_ERROR_NONE;
    return 1;
}
