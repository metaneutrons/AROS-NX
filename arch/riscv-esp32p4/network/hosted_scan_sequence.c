/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#include "hosted_scan_sequence.h"

static int valid_ranges(struct p4_hosted_scan_sequence *run,
    const struct p4_hosted_startup *startup, struct p4_hosted_fifo *fifo,
    const struct p4_hosted_open_ops *rx, const struct p4_hosted_tx_ops *tx,
    uint8_t *buffer, size_t capacity)
{
    const void *objects[6] = {run, startup, fifo, rx, tx, buffer};
    const size_t sizes[6] = {sizeof *run, sizeof *startup, sizeof *fifo,
                            sizeof *rx, sizeof *tx, capacity};
    uintptr_t starts[6], ends[6];
    unsigned i, j;
    if (capacity < 512u || capacity > 4096u) return 0;
    for (i = 0; i < 6u; ++i) {
        starts[i] = (uintptr_t)objects[i];
        if (!objects[i] || sizes[i] > UINTPTR_MAX - starts[i]) return 0;
        ends[i] = starts[i] + sizes[i];
    }
    for (i = 0; i < 6u; ++i)
        for (j = i + 1u; j < 6u; ++j)
            if (starts[i] < ends[j] && starts[j] < ends[i]) return 0;
    return 1;
}

static int protocol_fail(struct p4_hosted_startup_step *step,
    struct p4_hosted_fifo *fifo, uint32_t generation)
{
    step->success = 0;
    step->error = P4_HOSTED_CONTROL_PROTOCOL;
    if (fifo->generation == generation) fifo->poisoned = 1;
    return 0;
}

static int exchange(struct p4_hosted_scan_sequence *run,
    struct p4_hosted_startup_step *step, uint32_t id, uint32_t uid,
    uint16_t count, struct p4_hosted_fifo *fifo, uint32_t generation,
    const struct p4_hosted_open_ops *rx, const struct p4_hosted_tx_ops *tx,
    unsigned checksum, uint8_t *buffer, size_t capacity,
    struct p4_hosted_rpc *rpc)
{
    size_t size = 0, i;
    int encoded;
    volatile uint8_t *zero = (volatile uint8_t *)&run->reply;
    step->attempted = 1;
    if (id == 286u)
        encoded = p4_hosted_scan_start_request(uid, 1, run->request,
                                               sizeof run->request, &size);
    else if (id == 287u)
        encoded = p4_hosted_scan_stop_request(uid, run->request,
                                              sizeof run->request, &size);
    else if (id == 288u)
        encoded = p4_hosted_scan_num_request(uid, run->request,
                                             sizeof run->request, &size);
    else
        encoded = p4_hosted_scan_records_request(uid, count, run->request,
                                                 sizeof run->request, &size);
    if (!encoded) {
        step->error = P4_HOSTED_CONTROL_ENCODE;
        return 0;
    }
    for (i = 0; i < sizeof run->reply; ++i) zero[i] = 0;
    encoded = p4_hosted_control_exchange(fifo, generation, rx, tx, checksum,
        run->request, size, buffer, capacity, &run->reply);
    step->write_called = run->reply.tx.write_called;
    step->polls = run->reply.polls;
    step->error = run->reply.error;
    if (!encoded) {
        if (step->error == P4_HOSTED_CONTROL_OK)
            step->error = P4_HOSTED_CONTROL_PREFLIGHT;
        return 0;
    }
    rpc->msg_type = P4_HOSTED_RPC_TYPE_RESPONSE;
    rpc->msg_id = run->reply.response_id;
    rpc->uid = run->reply.response_uid;
    rpc->payload = run->reply.response_body;
    rpc->payload_size = run->reply.response_size;
    return 1;
}

static int status_step(struct p4_hosted_scan_sequence *run,
    struct p4_hosted_startup_step *step, uint32_t id, uint32_t uid,
    struct p4_hosted_fifo *fifo, uint32_t generation,
    const struct p4_hosted_open_ops *rx, const struct p4_hosted_tx_ops *tx,
    unsigned checksum, uint8_t *buffer, size_t capacity)
{
    struct p4_hosted_rpc rpc;
    if (!exchange(run, step, id, uid, 0, fifo, generation, rx, tx, checksum,
                  buffer, capacity, &rpc)) return 0;
    step->status_valid = p4_hosted_scan_status_response(&rpc, id, uid,
                                                       &step->status);
    if (!step->status_valid) return protocol_fail(step, fifo, generation);
    if (step->status) {
        step->error = P4_HOSTED_CONTROL_REMOTE;
        return 0;
    }
    step->success = 1;
    return 1;
}

int p4_hosted_scan_sequence_run(struct p4_hosted_scan_sequence *run,
    const struct p4_hosted_startup *startup, struct p4_hosted_fifo *fifo,
    uint32_t generation, const struct p4_hosted_open_ops *rx,
    const struct p4_hosted_tx_ops *tx, unsigned checksum,
    uint8_t *buffer, size_t capacity)
{
    struct p4_hosted_rpc rpc;
    uint16_t copied;
    if (!valid_ranges(run, startup, fifo, rx, tx, buffer, capacity) ||
        !generation || checksum > 1u || run->entered || run->cleanup_entered ||
        startup->entered != 1u || startup->success != 1u ||
        startup->cleanup_entered || startup->generation != generation ||
        fifo->generation != generation) return 0;
    run->entered = 1;
    run->generation = generation;
    if (!status_step(run, &run->start, 286, 7, fifo, generation, rx, tx,
                     checksum, buffer, capacity)) return 0;
    if (!exchange(run, &run->count, 288, 8, 0, fifo, generation, rx, tx,
                  checksum, buffer, capacity, &rpc)) return 0;
    run->count.status_valid = p4_hosted_scan_num_response(&rpc, 8,
                                                         &run->ap_count);
    if (!run->count.status_valid)
        return protocol_fail(&run->count, fifo, generation);
    run->count.status = run->ap_count.status;
    if (run->count.status) {
        run->count.error = P4_HOSTED_CONTROL_REMOTE;
        return 0;
    }
    run->count.success = 1;
    if (!run->ap_count.number) {
        /* Pinned slave rejects get_records for an empty list. This explicitly
         * compensated path cannot close the uncompensated N1E root-fix gate. */
        run->skipped_empty_records = 1;
        run->success = 1;
        return 1;
    }
    copied = run->ap_count.number < P4_HOSTED_SCAN_MAX_RECORDS ?
        run->ap_count.number : P4_HOSTED_SCAN_MAX_RECORDS;
    if (!exchange(run, &run->records, 289, 9, copied, fifo, generation, rx, tx,
                  checksum, buffer, capacity, &rpc)) return 0;
    run->records.status_valid = p4_hosted_scan_records_response(&rpc, 9, copied,
                                                               &run->aps);
    if (!run->records.status_valid)
        return protocol_fail(&run->records, fifo, generation);
    run->records.status = run->aps.status;
    if (run->records.status) {
        run->records.error = P4_HOSTED_CONTROL_REMOTE;
        return 0;
    }
    if (run->aps.number != copied)
        return protocol_fail(&run->records, fifo, generation);
    run->records.success = 1;
    run->success = 1;
    return 1;
}

int p4_hosted_scan_sequence_cleanup(struct p4_hosted_scan_sequence *run,
    const struct p4_hosted_startup *startup, struct p4_hosted_fifo *fifo,
    uint32_t generation, const struct p4_hosted_open_ops *rx,
    const struct p4_hosted_tx_ops *tx, unsigned checksum,
    uint8_t *buffer, size_t capacity)
{
    if (!valid_ranges(run, startup, fifo, rx, tx, buffer, capacity) ||
        !generation || checksum > 1u || run->entered != 1u ||
        run->cleanup_entered || run->generation != generation ||
        startup->generation != generation || startup->cleanup_entered ||
        fifo->generation != generation) return 0;
    run->cleanup_entered = 1;
    if (run->start.write_called && !run->success)
        run->cleanup_success = status_step(run, &run->stop, 287, 10, fifo,
            generation, rx, tx, checksum, buffer, capacity);
    else run->cleanup_success = 1;
    return run->cleanup_success;
}
