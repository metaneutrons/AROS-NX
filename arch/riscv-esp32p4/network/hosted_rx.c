/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#include "hosted_rx.h"

static int rx_ranges(const void *const *objects, const size_t *sizes,
                     unsigned count)
{
    uintptr_t starts[5], ends[5];
    unsigned i, j;
    if (count > 5u) return 0;
    for (i = 0; i < count; ++i) {
        starts[i] = (uintptr_t)objects[i];
        if (!objects[i] || (uintmax_t)sizes[i] > UINTPTR_MAX - starts[i])
            return 0;
        ends[i] = starts[i] + sizes[i];
    }
    for (i = 0; i < count; ++i)
        for (j = i + 1; j < count; ++j)
            if (starts[i] < ends[j] && starts[j] < ends[i]) return 0;
    return 1;
}

int p4_hosted_rx_start(struct p4_hosted_rx *state, uint32_t generation)
{
    const void *objects[1] = {state};
    const size_t sizes[1] = {sizeof(*state)};
    if (!generation || !rx_ranges(objects, sizes, 1u)) return 0;
    if (state->initialized > 1u ||
        (state->initialized == 1u && state->generation == generation)) return 0;
    /* Validate the assembler's complete start precondition before touching
     * either child. Inbox init may additionally reject corrupt queue state;
     * its refusal is transactional. After it succeeds, assembler start cannot
     * refuse under the exclusive-owner contract. No large stack copy needed. */
    if (state->assembly.initialized > 1u ||
        (state->assembly.initialized &&
         state->assembly.generation == generation)) return 0;
    if (!p4_hosted_inbox_init(&state->inbox, generation)) return 0;
    if (!p4_hosted_reassembly_start(&state->assembly, generation)) return 0;
    state->generation = generation;
    state->initialized = 1u;
    state->poisoned = 0u;
    return 1;
}

int p4_hosted_rx_invalidate(struct p4_hosted_rx *state,
    struct p4_hosted_fifo *fifo, uint32_t generation)
{
    const void *objects[2] = {state, fifo};
    const size_t sizes[2] = {sizeof(*state), sizeof(*fifo)};
    int matched = 0;
    if (!generation || !rx_ranges(objects, sizes, 2u)) return 0;
    if (state->initialized == 1u && state->generation == generation) {
        (void)p4_hosted_reassembly_abort(&state->assembly, generation);
        (void)p4_hosted_inbox_invalidate(&state->inbox, generation);
        state->poisoned = 1u;
        matched = 1;
    }
    if (fifo->initialized == 1u && fifo->generation == generation) {
        if (fifo->busy)
            (void)p4_hosted_fifo_finish(fifo, generation, 1u, 0u, 0u);
        fifo->poisoned = 1u;
        matched = 1;
    }
    return matched;
}

static int rx_fail(struct p4_hosted_rx *state, struct p4_hosted_fifo *fifo,
    uint32_t generation, struct p4_hosted_rx_result *result,
    enum p4_hosted_rx_error error, unsigned retire)
{
    if (retire) (void)p4_hosted_rx_invalidate(state, fifo, generation);
    result->error = error;
    return 0;
}

static int rx_guard(struct p4_hosted_rx *state, struct p4_hosted_fifo *fifo,
    uint32_t generation, const struct p4_hosted_rx_ops *ops,
    struct p4_hosted_rx_result *result, unsigned acknowledged)
{
    if (state->initialized != 1u || state->poisoned ||
        fifo->initialized != 1u || fifo->poisoned ||
        state->generation != generation || fifo->generation != generation ||
        ops->session_ok(ops->context, generation) != 1 ||
        state->generation != generation || fifo->generation != generation ||
        state->assembly.initialized != 1u || state->assembly.poisoned ||
        state->assembly.generation != generation ||
        state->inbox.initialized != 1u || state->inbox.poisoned ||
        state->inbox.generation != generation)
        return rx_fail(state, fifo, generation, result,
                       P4_HOSTED_RX_SESSION, 1u);
    if (ops->budget_ok(ops->context) != 1)
        return rx_fail(state, fifo, generation, result,
            P4_HOSTED_RX_BUDGET, acknowledged || state->assembly.fragments);
    /* budget_ok is also an external callback; never trust the old epoch after
     * it returns. Normal callbacks cannot mutate ownership or state. */
    if (state->initialized != 1u || state->poisoned ||
        fifo->initialized != 1u || fifo->poisoned ||
        state->generation != generation || fifo->generation != generation ||
        state->assembly.initialized != 1u || state->assembly.poisoned ||
        state->assembly.generation != generation ||
        state->inbox.initialized != 1u || state->inbox.poisoned ||
        state->inbox.generation != generation)
        return rx_fail(state, fifo, generation, result,
                       P4_HOSTED_RX_SESSION, 1u);
    return 1;
}

static int rx_batch(struct p4_hosted_rx *state, struct p4_hosted_fifo *fifo,
    uint32_t generation, const uint8_t *buffer, size_t logical,
    unsigned checksum_enabled, struct p4_hosted_rx_result *result)
{
    size_t offset = 0;
    struct p4_hosted_frame frame;
    struct hosted_serial_message message;
    struct p4_hosted_rpc rpc;
    unsigned full;
    enum p4_hosted_reassembly_result assembled;
    while (offset < logical) {
        if (!p4_hosted_decode(buffer + offset, logical - offset,
                              checksum_enabled, &frame)) goto malformed;
        assembled = p4_hosted_reassembly_feed(&state->assembly, generation,
                                               &frame);
        if (assembled == P4_HOSTED_REASSEMBLY_PARTIAL) {
            offset += frame.wire_size;
            continue;
        }
        if (assembled != P4_HOSTED_REASSEMBLY_COMPLETE ||
            !p4_hosted_reassembly_view(&state->assembly, generation, &message) ||
            !p4_hosted_rpc_decode(message.protobuf, message.protobuf_size, &rpc))
            goto malformed;
        if (!((message.endpoint == HOSTED_SERIAL_ENDPOINT_RPCEVT &&
               rpc.msg_type == P4_HOSTED_RPC_TYPE_EVENT) ||
              (message.endpoint == HOSTED_SERIAL_ENDPOINT_RPCRSP &&
               rpc.msg_type == P4_HOSTED_RPC_TYPE_RESPONSE))) goto malformed;
        full = state->inbox.count == P4_HOSTED_INBOX_CAPACITY;
        if (!p4_hosted_inbox_push(&state->inbox, generation, &rpc))
            return rx_fail(state, fifo, generation, result,
                full ? P4_HOSTED_RX_QUEUE_FULL : P4_HOSTED_RX_PROTOCOL, 1u);
        ++result->messages;
        if (!p4_hosted_reassembly_release(&state->assembly, generation))
            goto malformed;
        offset += frame.wire_size;
    }
    result->partial = !!state->assembly.fragments;
    return 1;
malformed:
    return rx_fail(state, fifo, generation, result, P4_HOSTED_RX_PROTOCOL, 1u);
}

int p4_hosted_rx_poll(struct p4_hosted_rx *state,
    struct p4_hosted_fifo *fifo, uint32_t generation,
    const struct p4_hosted_rx_ops *ops, unsigned checksum_enabled,
    uint8_t *buffer, size_t capacity, struct p4_hosted_rx_result *result)
{
    const void *objects[5] = {state, fifo, ops, buffer, result};
    const size_t sizes[5] = {sizeof(*state), sizeof(*fifo), sizeof(*ops),
                             capacity, sizeof(*result)};
    struct p4_hosted_fifo_plan plan;
    uint32_t raw, counter, clear, delta;
    unsigned acknowledged = 0u;
    int ok;
    if (!generation || checksum_enabled > 1u || capacity < 512u ||
        capacity > 4096u || !rx_ranges(objects, sizes, 5u)) return 0;
    if (!ops->session_ok || !ops->budget_ok || !ops->read_word ||
        !ops->write_word || !ops->read_fifo || state->initialized != 1u ||
        state->poisoned || state->generation != generation ||
        state->assembly.initialized != 1u || state->assembly.poisoned ||
        state->assembly.generation != generation ||
        state->inbox.initialized != 1u || state->inbox.poisoned ||
        state->inbox.generation != generation ||
        state->inbox.count > P4_HOSTED_INBOX_CAPACITY ||
        state->inbox.head >= P4_HOSTED_INBOX_CAPACITY ||
        fifo->initialized != 1u || fifo->generation != generation ||
        fifo->poisoned || fifo->busy) return 0;
    result->error = P4_HOSTED_RX_OK;
    result->batches = result->messages = result->partial = 0u;
    result->logical_bytes = 0u;
    if (!rx_guard(state, fifo, generation, ops, result, 0u)) return 0;
    ok = ops->read_word(ops->context, P4_HOSTED_OPEN_INT_RAW, &raw);
    if (!rx_guard(state, fifo, generation, ops, result, 0u)) return 0;
    if (ok != 1) return rx_fail(state, fifo, generation, result,
                               P4_HOSTED_RX_REGISTER, 0u);
    if (raw & (P4_HOSTED_OPEN_INT_RX_UNDERFLOW | P4_HOSTED_OPEN_INT_TX_OVERFLOW))
        return rx_fail(state, fifo, generation, result,
                       P4_HOSTED_RX_PEER_FAULT, 1u);
    clear = raw & P4_HOSTED_OPEN_INT_CLEAR_MASK;
    if (clear) {
        acknowledged = !!(clear & P4_HOSTED_OPEN_INT_PACKET);
        ok = ops->write_word(ops->context, P4_HOSTED_OPEN_INT_CLEAR, clear);
        if (!rx_guard(state, fifo, generation, ops, result, acknowledged))
            return 0;
        if (ok != 1) return rx_fail(state, fifo, generation, result,
                                   P4_HOSTED_RX_REGISTER, acknowledged);
    }
    if (!(raw & P4_HOSTED_OPEN_INT_PACKET)) {
        result->partial = !!state->assembly.fragments;
        return 1;
    }
    ok = ops->read_word(ops->context, P4_HOSTED_OPEN_PACKET_LENGTH, &counter);
    if (!rx_guard(state, fifo, generation, ops, result, acknowledged)) return 0;
    if (ok != 1) return rx_fail(state, fifo, generation, result,
                               P4_HOSTED_RX_REGISTER, acknowledged);
    delta = ((counter & P4_HOSTED_RX_MASK) - fifo->rx_consumed) &
             P4_HOSTED_RX_MASK;
    if (!delta) {
        result->partial = !!state->assembly.fragments;
        return 1;
    }
    if (!p4_hosted_fifo_rx(fifo, generation, counter, &plan))
        return rx_fail(state, fifo, generation, result,
                       P4_HOSTED_RX_COUNTER, acknowledged);
    if (plan.bus_bytes > capacity)
        return rx_fail(state, fifo, generation, result,
                       P4_HOSTED_RX_CAPACITY, 1u);
    if (!rx_guard(state, fifo, generation, ops, result, 1u)) return 0;
    ok = ops->read_fifo(ops->context, &plan, buffer);
    if (!rx_guard(state, fifo, generation, ops, result, 1u)) return 0;
    if (ok != 1) return rx_fail(state, fifo, generation, result,
                               P4_HOSTED_RX_TRANSFER, 1u);
    if (!p4_hosted_fifo_finish(fifo, generation, 1u, 1u, plan.bus_bytes))
        return rx_fail(state, fifo, generation, result,
                       P4_HOSTED_RX_COUNTER, 1u);
    result->batches = 1u;
    result->logical_bytes = plan.logical_bytes;
    if (!rx_batch(state, fifo, generation, buffer, plan.logical_bytes,
                   checksum_enabled, result)) return 0;
    return rx_guard(state, fifo, generation, ops, result, 0u);
}
