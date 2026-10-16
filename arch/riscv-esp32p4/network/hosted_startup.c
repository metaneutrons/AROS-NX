/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#include "hosted_startup.h"

static int disjoint(const void *a, size_t na, const void *b, size_t nb)
{
    uintptr_t pa = (uintptr_t)a, pb = (uintptr_t)b;
    return a && b && na && nb && na <= UINTPTR_MAX - pa &&
        nb <= UINTPTR_MAX - pb && (pa + na <= pb || pb + nb <= pa);
}

static int valid_ranges(struct p4_hosted_startup *run,
    struct p4_hosted_fifo *fifo, const struct p4_hosted_open_ops *rx,
    const struct p4_hosted_tx_ops *tx, uint8_t *buffer, size_t capacity,
    const struct p4_hosted_wifi_config *config)
{
    const void *ranges[6];
    size_t sizes[6], count = config ? 6u : 5u, i, j;
    ranges[0] = run; sizes[0] = sizeof(*run);
    ranges[1] = fifo; sizes[1] = sizeof(*fifo);
    ranges[2] = rx; sizes[2] = sizeof(*rx);
    ranges[3] = tx; sizes[3] = sizeof(*tx);
    ranges[4] = buffer; sizes[4] = capacity;
    ranges[5] = config; sizes[5] = sizeof(*config);
    if (capacity < 512u || capacity > 4096u)
        return 0;
    for (i = 0; i < count; ++i)
        for (j = i + 1; j < count; ++j)
            if (!disjoint(ranges[i], sizes[i], ranges[j], sizes[j]))
                return 0;
    return 1;
}

static int status_step(struct p4_hosted_startup *run,
    struct p4_hosted_startup_step *step, uint32_t id, uint32_t uid,
    const struct p4_hosted_wifi_config *config,
    struct p4_hosted_fifo *fifo, uint32_t generation,
    const struct p4_hosted_open_ops *rx, const struct p4_hosted_tx_ops *tx,
    unsigned checksum, uint8_t *buffer, size_t capacity)
{
    struct p4_hosted_rpc rpc;
    size_t size = 0;
    int encoded;
    step->attempted = 1;
    if (id == 278u)
        encoded = p4_hosted_wifi_init_request(uid, config, run->request,
            sizeof(run->request), &size);
    else if (id == 260u)
        encoded = p4_hosted_wifi_set_mode_request(uid, 1, run->request,
            sizeof(run->request), &size);
    else
        encoded = p4_hosted_wifi_simple_request(id, uid, run->request,
            sizeof(run->request), &size);
    if (!encoded) {
        step->error = P4_HOSTED_CONTROL_ENCODE;
        return 0;
    }
    /* exchange preflight can refuse without writing its output. Clear only
     * our scratch, never the transport ledger or accumulated step evidence. */
    {
        volatile uint8_t *p = (volatile uint8_t *)&run->scratch;
        size_t i;
        for (i = 0; i < sizeof(run->scratch); ++i) p[i] = 0;
    }
    encoded = p4_hosted_control_exchange(fifo, generation, rx, tx, checksum,
        run->request, size, buffer, capacity, &run->scratch);
    step->write_called = run->scratch.tx.write_called;
    step->polls = run->scratch.polls;
    step->error = run->scratch.error;
    if (!encoded) {
        if (step->error == P4_HOSTED_CONTROL_OK)
            step->error = P4_HOSTED_CONTROL_PREFLIGHT;
        return 0;
    }
    rpc.msg_type = P4_HOSTED_RPC_TYPE_RESPONSE;
    rpc.msg_id = run->scratch.response_id;
    rpc.uid = run->scratch.response_uid;
    rpc.payload = run->scratch.response_body;
    rpc.payload_size = run->scratch.response_size;
    step->status_valid = p4_hosted_wifi_status_response(&rpc, id, uid,
                                                       &step->status);
    if (!step->status_valid) {
        step->error = P4_HOSTED_CONTROL_PROTOCOL;
        if (fifo->generation == generation) fifo->poisoned = 1;
        return 0;
    }
    if (step->status != 0) {
        step->error = P4_HOSTED_CONTROL_REMOTE;
        return 0;
    }
    step->success = 1;
    return 1;
}

int p4_hosted_startup_run(struct p4_hosted_startup *run,
    const struct p4_hosted_wifi_config *config,
    struct p4_hosted_fifo *fifo, uint32_t generation,
    const struct p4_hosted_open_ops *rx, const struct p4_hosted_tx_ops *tx,
    unsigned checksum, uint8_t *buffer, size_t capacity)
{
    if (!config || !valid_ranges(run, fifo, rx, tx, buffer, capacity, config) ||
        run->entered || run->cleanup_entered) return 0;
    run->entered = 1;
    run->generation = generation;
    if (!status_step(run, &run->init, 278, 1, config, fifo, generation,
        rx, tx, checksum, buffer, capacity) ||
        !status_step(run, &run->mode, 260, 2, config, fifo, generation,
        rx, tx, checksum, buffer, capacity) ||
        !status_step(run, &run->start, 280, 3, config, fifo, generation,
        rx, tx, checksum, buffer, capacity)) return 0;
    run->mac_attempted = 1;
    run->success = p4_hosted_control_get_mac(fifo, generation, rx, tx, checksum,
        4, buffer, capacity, &run->mac);
    if (!run->success && run->mac.error == P4_HOSTED_CONTROL_OK)
        run->mac.error = P4_HOSTED_CONTROL_PREFLIGHT;
    return run->success;
}

int p4_hosted_startup_cleanup(struct p4_hosted_startup *run,
    struct p4_hosted_fifo *fifo, uint32_t generation,
    const struct p4_hosted_open_ops *rx, const struct p4_hosted_tx_ops *tx,
    unsigned checksum, uint8_t *buffer, size_t capacity)
{
    int stop_ok = 1, deinit_ok = 1;
    if (!valid_ranges(run, fifo, rx, tx, buffer, capacity, NULL) ||
        !run->entered || run->cleanup_entered ||
        run->generation != generation || fifo->generation != generation)
        return 0;
    run->cleanup_entered = 1;
    if (run->start.write_called)
        stop_ok = status_step(run, &run->stop, 281, 5, NULL, fifo, generation,
            rx, tx, checksum, buffer, capacity);
    if (run->init.write_called)
        deinit_ok = status_step(run, &run->deinit, 279, 6, NULL, fifo, generation,
            rx, tx, checksum, buffer, capacity);
    run->cleanup_success = stop_ok && deinit_ok;
    return run->cleanup_success;
}
