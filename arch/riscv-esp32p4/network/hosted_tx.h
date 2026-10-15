/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#ifndef P4_HOSTED_TX_H
#define P4_HOSTED_TX_H

#include <stddef.h>
#include <stdint.h>

#include "hosted_fifo.h"

#define P4_HOSTED_TX_TOKEN_ADDRESS UINT32_C(0x44)
#define P4_HOSTED_TX_MAX_POLLS 100u

struct p4_hosted_tx_completion {
    unsigned started;
    unsigned success;
    uint32_t completed_bytes;
};

enum p4_hosted_tx_stage {
    P4_HOSTED_TX_STAGE_NONE = 0,
    P4_HOSTED_TX_STAGE_SESSION,
    P4_HOSTED_TX_STAGE_BUDGET,
    P4_HOSTED_TX_STAGE_READ_TOKEN,
    P4_HOSTED_TX_STAGE_PLAN,
    P4_HOSTED_TX_STAGE_YIELD,
    P4_HOSTED_TX_STAGE_PADDING,
    P4_HOSTED_TX_STAGE_WRITE_FIFO,
    P4_HOSTED_TX_STAGE_FINISH,
    P4_HOSTED_TX_STAGE_COMPLETE
};

enum p4_hosted_tx_error {
    P4_HOSTED_TX_ERROR_NONE = 0,
    P4_HOSTED_TX_ERROR_SESSION_LOST,
    P4_HOSTED_TX_ERROR_BUDGET_EXPIRED,
    P4_HOSTED_TX_ERROR_TOKEN_READ,
    P4_HOSTED_TX_ERROR_COUNTER_INVALID,
    P4_HOSTED_TX_ERROR_PLAN_REFUSED,
    P4_HOSTED_TX_ERROR_CREDIT_EXHAUSTED,
    P4_HOSTED_TX_ERROR_WRITE_AMBIGUOUS,
    P4_HOSTED_TX_ERROR_WRITE_FAILED,
    P4_HOSTED_TX_ERROR_COMPLETION_MALFORMED,
    P4_HOSTED_TX_ERROR_FINISH_FAILED
};

/* Callbacks only adapt existing bounded shared-host operations. session_ok
 * checks both current controller ownership and the captured transport
 * generation. budget_ok checks the caller's existing aggregate deadline.
 * write_fifo returns exactly 1 only when all three completion fields are
 * authoritative; every other return is ambiguous and is treated as started.
 * yield is called after a valid token with insufficient credits, except after
 * the final poll. The gate never allocates, performs I/O itself, retries a
 * write, resets counters, or creates a deadline.
 */
struct p4_hosted_tx_ops {
    void *context;
    int (*read_word)(void *context, uint32_t address, uint32_t *value);
    int (*write_fifo)(void *context,
                      const struct p4_hosted_fifo_plan *plan,
                      const uint8_t *buffer,
                      struct p4_hosted_tx_completion *completion);
    int (*session_ok)(void *context, uint32_t generation);
    int (*budget_ok)(void *context);
    void (*yield)(void *context);
};

struct p4_hosted_tx_result {
    enum p4_hosted_tx_stage stage;
    enum p4_hosted_tx_error error;
    uint32_t polls;
    uint32_t token;
    struct p4_hosted_fifo_plan plan;
    struct p4_hosted_tx_completion completion;
    int write_callback_result;
    unsigned token_valid;
    unsigned plan_valid;
    unsigned write_called;
    unsigned completion_reported;
    unsigned completion_well_formed;
    unsigned success;
};

/* Send exactly one already-encoded, unfragmented ESP-Hosted frame through the
 * persistent FIFO state. Profiles STA/AP/SERIAL/PRIVATE are accepted only;
 * interface number, flags, packet type, throttle and sequence must be zero;
 * the canonical data offset must be 12. checksum_enabled must match negotiated
 * INIT exactly. The complete logical frame has a nonempty payload, so
 * wire_size is 13..1536; a header-only frame is invalid. Capacity must include
 * its 512-byte-rounded DMA transfer. Only buffer[wire_size..bus_bytes) is
 * zeroed.
 *
 * Before execution, state, ops, the complete buffer-capacity range and result
 * must be nonnull, overflow-safe and pairwise disjoint. Preflight refusal
 * leaves result, state and buffer unchanged. Once preflight passes, result is
 * initialized and reports the terminal stage, error, poll/token, plan and
 * completion facts. Return 1 only after one full started transfer, successful
 * post-write session/budget checks and FIFO counter commit; return 0 otherwise.
 * The caller must have initialized this persistent FIFO for a known fresh
 * slave session with an already validated, nonzero pool limit. Do not start or
 * reset it on shared-controller reacquisition.
 */
int p4_hosted_tx_send(struct p4_hosted_fifo *state, uint32_t generation,
                      const struct p4_hosted_tx_ops *ops,
                      uint8_t *buffer, size_t capacity, size_t wire_size,
                      unsigned checksum_enabled,
                      struct p4_hosted_tx_result *result);

#endif
