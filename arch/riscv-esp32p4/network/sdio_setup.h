/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#ifndef P4_SDIO_SETUP_H
#define P4_SDIO_SETUP_H

#include <stdint.h>

#define P4_SDIO_SETUP_MAX_POLLS 1000U

typedef enum p4_sdio_setup_stage
{
    P4_SDIO_SETUP_STAGE_START = 0,
    P4_SDIO_SETUP_STAGE_READ_IO_ENABLE,
    P4_SDIO_SETUP_STAGE_READ_ORIGINAL_BLOCK_SIZE_LOW,
    P4_SDIO_SETUP_STAGE_READ_ORIGINAL_BLOCK_SIZE_HIGH,
    P4_SDIO_SETUP_STAGE_WRITE_IO_ENABLE,
    P4_SDIO_SETUP_STAGE_READBACK_IO_ENABLE,
    P4_SDIO_SETUP_STAGE_POLL_IO_READY,
    P4_SDIO_SETUP_STAGE_WRITE_BLOCK_SIZE_LOW,
    P4_SDIO_SETUP_STAGE_WRITE_BLOCK_SIZE_HIGH,
    P4_SDIO_SETUP_STAGE_READBACK_BLOCK_SIZE_LOW,
    P4_SDIO_SETUP_STAGE_READBACK_BLOCK_SIZE_HIGH,
    P4_SDIO_SETUP_STAGE_COMPLETE
} p4_sdio_setup_stage_t;

typedef enum p4_sdio_setup_error
{
    P4_SDIO_SETUP_ERROR_NONE = 0,
    P4_SDIO_SETUP_ERROR_INVALID_ARGUMENT,
    P4_SDIO_SETUP_ERROR_BUDGET,
    P4_SDIO_SETUP_ERROR_COMMAND,
    P4_SDIO_SETUP_ERROR_IO_ENABLE_MISMATCH,
    P4_SDIO_SETUP_ERROR_NOT_READY,
    P4_SDIO_SETUP_ERROR_BLOCK_SIZE_MISMATCH
} p4_sdio_setup_error_t;

typedef struct p4_sdio_setup_ops
{
    void *context;
    int (*read_register)(void *context, unsigned int function,
        uint32_t address, uint8_t *value);
    int (*write_register)(void *context, unsigned int function,
        uint32_t address, uint8_t value);
    int (*budget_ok)(void *context);
    void (*yield)(void *context);
} p4_sdio_setup_ops_t;

typedef struct p4_sdio_setup_result
{
    p4_sdio_setup_stage_t stage;
    p4_sdio_setup_error_t error;
    unsigned int poll_count;
    uint8_t previous_io_enable;
    uint8_t previous_io_enable_valid;
    uint8_t observed_io_enable;
    uint8_t observed_io_enable_valid;
    uint8_t observed_io_ready;
    uint8_t observed_io_ready_valid;
    uint16_t original_block_size;
    uint8_t original_block_size_valid;
    uint16_t observed_block_size;
    uint8_t observed_block_size_valid;
    uint8_t mutation_attempted;
    uint8_t complete;
} p4_sdio_setup_result_t;

/* Set up SDIO function 1 using only Function-0 CMD52 register callbacks.
 * The callback owns transport and response decoding. This routine snapshots
 * CCCR IO_ENABLE and FBR1 block size before its first write, enables IO_ENABLE
 * bit 1 without disturbing other bits, polls IO_READY bit 1, writes FBR1's
 * block-size bytes to 512 and verifies the readback. It does not roll back
 * card state; callers can use the saved values to restore it under the same
 * owned budget. The poll count is hard-bounded and yield is required.
 * Returns 1 only when result->complete is set. The result is zeroed before
 * argument validation or any callback.
 */
int p4_sdio_setup_function1(const p4_sdio_setup_ops_t *ops,
    p4_sdio_setup_result_t *result);

#endif /* P4_SDIO_SETUP_H */
