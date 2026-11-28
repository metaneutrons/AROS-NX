/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#ifndef P4_SDIO_RESET_QUERY_H
#define P4_SDIO_RESET_QUERY_H

#include "sdio_probe.h"

typedef enum p4_sdio_reset_query_stage
{
    P4_SDIO_RESET_QUERY_START = 0,
    P4_SDIO_RESET_QUERY_RESET,
    P4_SDIO_RESET_QUERY_CMD0,
    P4_SDIO_RESET_QUERY_CMD5,
    P4_SDIO_RESET_QUERY_COMPLETE
} p4_sdio_reset_query_stage_t;

typedef struct p4_sdio_reset_query_result
{
    p4_sdio_reset_query_stage_t stage;
    p4_sdio_probe_error_t error;
    unsigned int command_index;
    uint32_t response;
    uint32_t ocr;
    uint8_t functions;
    uint8_t reset_attempted;
    uint8_t reset_acknowledged;
    uint8_t complete;
} p4_sdio_reset_query_result_t;

/* One Fn0 CCCR RES write, CMD0 and CMD5(0), with pre/post budget checks.
 * Stops on any ambiguous reset/command result. No OCR negotiation, selection,
 * function/IRQ enable, FIFO or OPEN. Caller quiesces WLAN and owns the healthy
 * shared host throughout. Reset side effects are not reversible by restoring
 * FBR/IO_ENABLE; attempted is set before the possibly ambiguous write.
 * complete means a valid nonzero function/OCR query, not a ready WLAN link.
 */
int p4_sdio_reset_query(const p4_sdio_probe_ops_t *ops,
    p4_sdio_reset_query_result_t *result);

#endif
