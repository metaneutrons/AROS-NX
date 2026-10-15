/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#ifndef P4_HOSTED_STARTUP_H
#define P4_HOSTED_STARTUP_H
#include "hosted_control.h"
#include "hosted_wifi.h"

struct p4_hosted_startup_step {
    unsigned attempted, success, status_valid, write_called;
    enum p4_hosted_control_error error;
    int32_t status;
    uint32_t polls;
};
/* One fresh diagnostic epoch only; fixed UIDs1..6 are never retried. Caller
 * initializes this object to zero and owns it and all transport objects
 * exclusively. Keep it off the small process stack. */
struct p4_hosted_startup {
    struct p4_hosted_startup_step init, mode, start, stop, deinit;
    struct p4_hosted_control_result mac;
    struct p4_hosted_control_result scratch;
    uint8_t request[256];
    uint32_t generation;
    unsigned entered, mac_attempted, cleanup_entered, success, cleanup_success;
};
/* Run Init -> STA mode -> Start -> GetMAC on the existing ledger/deadline.
 * Does not open, reset, allocate, renew a budget or retry. Config is supplied
 * by the explicit reference profile, not inferred from INIT capabilities. */
int p4_hosted_startup_run(struct p4_hosted_startup *run,
    const struct p4_hosted_wifi_config *config,
    struct p4_hosted_fifo *fifo, uint32_t generation,
    const struct p4_hosted_open_ops *rx, const struct p4_hosted_tx_ops *tx,
    unsigned checksum, uint8_t *buffer, size_t capacity);
/* Caller establishes its separate bounded cleanup reserve once. Stop and
 * Deinit are attempted conservatively whenever the corresponding mutating
 * request reached write_fifo, including unknown outcomes. Failed cleanup
 * never qualifies as success; CLOSE/Fn restoration remain caller work.
 * All object ranges must be disjoint (including config, run and buffer). */
int p4_hosted_startup_cleanup(struct p4_hosted_startup *run,
    struct p4_hosted_fifo *fifo, uint32_t generation,
    const struct p4_hosted_open_ops *rx, const struct p4_hosted_tx_ops *tx,
    unsigned checksum, uint8_t *buffer, size_t capacity);
#endif
