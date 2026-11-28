/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#ifndef P4_HOSTED_SCAN_SEQUENCE_H
#define P4_HOSTED_SCAN_SEQUENCE_H
#include "hosted_startup.h"
#include "hosted_scan.h"

/* One opt-in scan after the successful single-use startup epoch. UIDs7..10
 * are disjoint from startup/teardown1..6 and are never retried. The caller
 * owns all objects exclusively; initialize this object to zero and allocate
 * it off small stacks. Never start a new object on an already-used epoch. */
struct p4_hosted_scan_sequence {
    struct p4_hosted_startup_step start, count, records, stop;
    struct p4_hosted_control_result reply;
    struct p4_hosted_scan_count ap_count;
    struct p4_hosted_scan_records aps;
    uint8_t request[64];
    uint32_t generation;
    unsigned entered, cleanup_entered, success, cleanup_success;
    /* Named vendor empty-record workaround, not an uncompensated N1E pass. */
    unsigned skipped_empty_records;
};

/* Blocking NULL/default-config scan -> count -> at most8 owned AP records.
 * The startup object must still be live and not cleaned up. No opening,
 * allocation, reset, retries or budget renewal. The caller's single aggregate
 * deadline governs every exchange; SERIAL-only diagnostic, not a datapath. */
int p4_hosted_scan_sequence_run(struct p4_hosted_scan_sequence *run,
    const struct p4_hosted_startup *startup, struct p4_hosted_fifo *fifo,
    uint32_t generation, const struct p4_hosted_open_ops *rx,
    const struct p4_hosted_tx_ops *tx, unsigned checksum,
    uint8_t *buffer, size_t capacity);

/* Within the caller's separate cleanup reserve, attempt ScanStop after a
 * written but unsuccessful scan, including uncertain completion. Call before
 * startup Stop/Deinit. Failure is not hidden by later teardown or restoration. */
int p4_hosted_scan_sequence_cleanup(struct p4_hosted_scan_sequence *run,
    const struct p4_hosted_startup *startup, struct p4_hosted_fifo *fifo,
    uint32_t generation, const struct p4_hosted_open_ops *rx,
    const struct p4_hosted_tx_ops *tx, unsigned checksum,
    uint8_t *buffer, size_t capacity);
#endif
