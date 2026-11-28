/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#ifndef P4_SDIO_RESTORE_H
#define P4_SDIO_RESTORE_H

#include "sdio_setup.h"

typedef enum p4_sdio_restore_stage
{
    P4_SDIO_RESTORE_STAGE_START = 0,
    P4_SDIO_RESTORE_STAGE_WRITE_BLOCK_SIZE_LOW,
    P4_SDIO_RESTORE_STAGE_WRITE_BLOCK_SIZE_HIGH,
    P4_SDIO_RESTORE_STAGE_WRITE_IO_ENABLE,
    P4_SDIO_RESTORE_STAGE_VERIFY_BLOCK_SIZE_LOW,
    P4_SDIO_RESTORE_STAGE_VERIFY_BLOCK_SIZE_HIGH,
    P4_SDIO_RESTORE_STAGE_VERIFY_IO_ENABLE,
    P4_SDIO_RESTORE_STAGE_COMPLETE
} p4_sdio_restore_stage_t;

typedef enum p4_sdio_restore_error
{
    P4_SDIO_RESTORE_ERROR_NONE = 0,
    P4_SDIO_RESTORE_ERROR_INVALID_ARGUMENT,
    P4_SDIO_RESTORE_ERROR_SNAPSHOT_UNAVAILABLE,
    P4_SDIO_RESTORE_ERROR_BUDGET,
    P4_SDIO_RESTORE_ERROR_COMMAND,
    P4_SDIO_RESTORE_ERROR_MISMATCH
} p4_sdio_restore_error_t;

typedef struct p4_sdio_restore_result
{
    p4_sdio_restore_stage_t stage;
    p4_sdio_restore_error_t error;
    uint8_t observed_block_size_low;
    uint8_t observed_block_size_low_valid;
    uint8_t observed_block_size_high;
    uint8_t observed_block_size_high_valid;
    uint8_t observed_io_enable;
    uint8_t observed_io_enable_valid;
    uint8_t attempted;
    uint8_t complete;
} p4_sdio_restore_result_t;

/* Restore the exact IO_ENABLE and FBR1 block-size bytes saved by setup.
 *
 * The caller must retain controller ownership and reserve a cleanup budget
 * independently of the setup budget. budget_ok must report that active
 * cleanup reservation; this function neither extends nor reuses an expired
 * setup budget. It issues at most six Function-0 CMD52 commands, checking
 * the caller's budget before and after each command. It has no polling loop
 * and makes no libc calls.
 *
 * If setup_result->mutation_attempted is clear, this succeeds without
 * callbacks or a mutation attempt. Otherwise both saved snapshots must be
 * valid. FBR low/high and then IO_ENABLE are written before all three bytes
 * are read back. attempted is set before the first write callback, because a
 * failed callback may still have changed card state. complete is set only
 * after all exact readbacks succeed. Returns 1 only when complete is set.
 */
int p4_sdio_restore_function1(const p4_sdio_setup_ops_t *ops,
    const p4_sdio_setup_result_t *setup_result,
    p4_sdio_restore_result_t *result);

#endif /* P4_SDIO_RESTORE_H */
