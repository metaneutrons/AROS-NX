/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#ifndef P4_SDIO_PROBE_H
#define P4_SDIO_PROBE_H

#include <stdint.h>

#define P4_SDIO_PROBE_HOST_OCR       UINT32_C(0x00ff8000)
#define P4_SDIO_PROBE_R1_ERROR_MASK  UINT32_C(0xfff98000)
#define P4_SDIO_PROBE_R5_ERROR_MASK  UINT32_C(0x0000cb00)
#define P4_SDIO_PROBE_R6_ERROR_MASK  UINT32_C(0x0000e000)
#define P4_SDIO_PROBE_MAX_POLLS      1000U

/* Classified transport failures, never valid response payloads. Only the
 * pre-enumeration RES command may tolerate these two negative outcomes. */
#define P4_SDIO_COMMAND_RESPONSE_TIMEOUT (-1)
#define P4_SDIO_COMMAND_RESPONSE_CRC     (-2)

typedef enum p4_sdio_response_type
{
    P4_SDIO_RESP_NONE = 0,
    P4_SDIO_RESP_R4,
    P4_SDIO_RESP_R6,
    P4_SDIO_RESP_R1,
    P4_SDIO_RESP_R5
} p4_sdio_response_type_t;

typedef enum p4_sdio_probe_stage
{
    P4_SDIO_PROBE_STAGE_START = 0,
    P4_SDIO_PROBE_STAGE_CMD0,
    P4_SDIO_PROBE_STAGE_CMD5_INITIAL,
    P4_SDIO_PROBE_STAGE_CMD5_POLL,
    P4_SDIO_PROBE_STAGE_CMD3,
    P4_SDIO_PROBE_STAGE_CMD7,
    P4_SDIO_PROBE_STAGE_CCCR,
    P4_SDIO_PROBE_STAGE_FBR1,
    P4_SDIO_PROBE_STAGE_COMPLETE,
    P4_SDIO_PROBE_STAGE_RESET
} p4_sdio_probe_stage_t;

typedef enum p4_sdio_probe_error
{
    P4_SDIO_PROBE_ERROR_NONE = 0,
    P4_SDIO_PROBE_ERROR_INVALID_ARGUMENT,
    P4_SDIO_PROBE_ERROR_BUDGET,
    P4_SDIO_PROBE_ERROR_COMMAND,
    P4_SDIO_PROBE_ERROR_NO_FUNCTIONS,
    P4_SDIO_PROBE_ERROR_NO_VOLTAGE,
    P4_SDIO_PROBE_ERROR_NOT_READY,
    P4_SDIO_PROBE_ERROR_INVALID_RCA,
    P4_SDIO_PROBE_ERROR_R6_STATUS,
    P4_SDIO_PROBE_ERROR_R1_STATUS,
    P4_SDIO_PROBE_ERROR_R5_STATUS
} p4_sdio_probe_error_t;

typedef struct p4_sdio_probe_ops
{
    void *context;
    /* Return1 on validated success, otherwise0 or one of the classified
     * negative failures above. Do not publish rejected response payloads.
     * The caller rejects every failure except the bootstrap RES exceptions. */
    int (*command)(void *context, unsigned int index, uint32_t argument,
        p4_sdio_response_type_t response_type, uint32_t error_mask,
        uint32_t *response);
    int (*budget_ok)(void *context);
    /* Required by identify: wait at least 20 ms, without renewing budget.
     * Used after successful CMD0 and between not-ready CMD5 responses. */
    void (*yield)(void *context);
} p4_sdio_probe_ops_t;

typedef struct p4_sdio_probe_result
{
    p4_sdio_probe_stage_t stage;
    p4_sdio_probe_error_t error;
    unsigned int command_index;
    uint32_t ocr;
    uint16_t rca;
    uint8_t functions;
    uint8_t cccr_revision;
    uint8_t sdio_revision;
    uint8_t bus_interface;
    uint8_t capabilities;
    uint8_t io_enable;
    uint8_t io_ready;
    uint32_t cis_pointer;
    uint8_t fbr1_interface;
    uint8_t complete;
    uint8_t reset_attempted;
    uint8_t reset_acknowledged;
    int reset_status;
} p4_sdio_probe_result_t;

/* Run the bounded read-only identification sequence. The callback owns
 * transport, host generation checks and response/error decoding. This routine
 * issues only CMD0, CMD5, CMD3, CMD7 and CMD52 reads; it never resets the host,
 * changes clocks/GPIOs, writes CCCR/FBR registers, parses CIS or identifies a
 * particular SDIO vendor/device. Returns 1 only when result->complete is set.
 * The result is zeroed before any argument validation or command attempt.
 */
int p4_sdio_probe_identify(const p4_sdio_probe_ops_t *ops,
    p4_sdio_probe_result_t *result);

/* Reset a quiesced SDIO slave, then identify under the SAME host ownership,
 * generation and aggregate budget. Exactly one Fn0 CCCR0x06=0x08 write.
 * Timeout/CRC are tolerated only for that reset; acknowledgement remains0.
 * No host reset, C6 firmware, function enable, IRQ, FIFO or OPEN operation.
 * Slave-side reset effects cannot be undone by restoring the P4 image. */
int p4_sdio_probe_bootstrap(const p4_sdio_probe_ops_t *ops,
    p4_sdio_probe_result_t *result);

#endif /* P4_SDIO_PROBE_H */
