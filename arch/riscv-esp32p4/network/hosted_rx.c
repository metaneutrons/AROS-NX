/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#include "hosted_rx.h"

#define P4_HOSTED_PRIVATE_INTERFACE 5u
#define P4_HOSTED_PRIVATE_INIT_PACKET UINT8_C(0x33)
#define P4_HOSTED_CHECKSUM_CAPABILITY (UINT8_C(1) << 7)

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

static int inbox_init_allowed(const struct p4_hosted_inbox *inbox,
                              uint32_t generation)
{
    if (inbox->initialized == 0u)
        return inbox->generation == 0u && inbox->head == 0u &&
               inbox->count == 0u && inbox->poisoned == 0u &&
               inbox->discovery_generation == 0u &&
               inbox->discovery_uid == 0u && inbox->discovery_armed == 0u &&
               inbox->discovery_seen == 0u;
    return inbox->initialized == 1u && inbox->generation != 0u &&
           inbox->generation != generation;
}

static int inbox_matches(const struct p4_hosted_inbox *inbox,
                         uint32_t generation)
{
    if (inbox->initialized != 1u || inbox->poisoned != 0u ||
        inbox->generation != generation ||
        inbox->count > P4_HOSTED_INBOX_CAPACITY ||
        inbox->head >= P4_HOSTED_INBOX_CAPACITY ||
        inbox->discovery_armed > 1u || inbox->discovery_seen > 1u)
        return 0;
    if (inbox->discovery_armed == 0u)
        return inbox->discovery_generation == 0u &&
               inbox->discovery_uid == 0u && inbox->discovery_seen == 0u;
    return inbox->discovery_generation == generation &&
           inbox->discovery_uid != 0u;
}

static int frames_init_allowed(const struct p4_hosted_frames *frames,
                               uint32_t generation)
{
    if (frames->initialized == 0u)
        return frames->generation == 0u && frames->head == 0u &&
               frames->count == 0u && frames->poisoned == 0u;
    if (frames->initialized != 1u || frames->generation == 0u ||
        frames->generation == generation ||
        frames->head >= P4_HOSTED_FRAME_QUEUE_CAPACITY ||
        frames->count > P4_HOSTED_FRAME_QUEUE_CAPACITY ||
        frames->poisoned > 1u)
        return 0;
    return frames->poisoned == 0u ||
           (frames->head == 0u && frames->count == 0u);
}

int p4_hosted_rx_start(struct p4_hosted_rx *state, uint32_t generation)
{
    const void *objects[1] = {state};
    const size_t sizes[1] = {sizeof(*state)};
    if (!generation || !rx_ranges(objects, sizes, 1u)) return 0;
    if (state->initialized > 1u ||
        (state->initialized == 1u && state->generation == generation)) return 0;
    /* Validate each child's complete start precondition before touching any
     * child. Under exclusive ownership, the three initializers then cannot
     * refuse partway through the composite rebind. No large stack copy needed. */
    if (state->assembly.initialized > 1u ||
        (state->assembly.initialized &&
         state->assembly.generation == generation) ||
        !inbox_init_allowed(&state->inbox, generation) ||
        !frames_init_allowed(&state->frames, generation)) return 0;
    if (!p4_hosted_inbox_init(&state->inbox, generation)) return 0;
    if (!p4_hosted_reassembly_start(&state->assembly, generation)) return 0;
    if (!p4_hosted_frames_init(&state->frames, generation)) return 0;
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
        (void)p4_hosted_frames_invalidate(&state->frames, generation);
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
        !inbox_matches(&state->inbox, generation) ||
        state->frames.initialized != 1u || state->frames.poisoned ||
        state->frames.generation != generation ||
        state->frames.count > P4_HOSTED_FRAME_QUEUE_CAPACITY ||
        state->frames.head >= P4_HOSTED_FRAME_QUEUE_CAPACITY)
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
        !inbox_matches(&state->inbox, generation) ||
        state->frames.initialized != 1u || state->frames.poisoned ||
        state->frames.generation != generation ||
        state->frames.count > P4_HOSTED_FRAME_QUEUE_CAPACITY ||
        state->frames.head >= P4_HOSTED_FRAME_QUEUE_CAPACITY)
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
        if (frame.interface_type == P4_HOSTED_STA) {
            full = state->frames.count == P4_HOSTED_FRAME_QUEUE_CAPACITY;
            if (frame.interface_number != 0u || frame.flags != 0u ||
                frame.sequence != 0u || frame.packet_type != 0u ||
                frame.throttle != 0u ||
                frame.payload_size < P4_HOSTED_ETHERNET_HEADER_SIZE ||
                frame.payload_size > P4_HOSTED_ETHERNET_FRAME_MAX)
                goto malformed;
            if (!p4_hosted_frames_push(&state->frames, generation,
                                       frame.payload, frame.payload_size))
                return rx_fail(state, fifo, generation, result,
                    full ? P4_HOSTED_RX_FRAME_QUEUE_FULL :
                           P4_HOSTED_RX_PROTOCOL, 1u);
            ++result->frames;
            offset += frame.wire_size;
            continue;
        }
        assembled = p4_hosted_reassembly_feed(&state->assembly, generation,
                                               &frame);
        if (assembled == P4_HOSTED_REASSEMBLY_PARTIAL) {
            offset += frame.wire_size;
            continue;
        }
        if (assembled != P4_HOSTED_REASSEMBLY_COMPLETE ||
            !p4_hosted_reassembly_view(&state->assembly, generation, &message))
            goto malformed;
        if (!p4_hosted_rpc_decode(message.protobuf, message.protobuf_size,
                                  &rpc)) {
            uint32_t expected_uid =
                p4_hosted_inbox_discovery_response_expected_uid(
                    &state->inbox, generation);
            if (expected_uid == 0u ||
                !p4_hosted_rpc_decode_board_discovery(
                    message.protobuf, message.protobuf_size, &rpc) ||
                rpc.msg_type != P4_HOSTED_RPC_TYPE_RESPONSE ||
                rpc.msg_id != 606u || rpc.uid != expected_uid)
                goto malformed;
        }
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

int p4_hosted_rx_take_open(struct p4_hosted_rx *state,
    struct p4_hosted_open_result *opened, uint32_t generation,
    const uint8_t *buffer, size_t capacity, struct p4_hosted_rx_result *result)
{
    const void *objects[4] = {state, opened, buffer, result};
    const size_t sizes[4] = {sizeof(*state), sizeof(*opened), capacity,
                             sizeof(*result)};
    struct p4_hosted_frame initial;
    struct p4_hosted_init identity;
    unsigned checksum;
    int ok;
    if (!generation || capacity < 512u || capacity > 4096u ||
        !rx_ranges(objects, sizes, 4u)) return 0;
    if (opened->initialized != 1u || opened->generation != generation ||
        opened->gate_ok != 1u || opened->suffix_preserved != 1u ||
        opened->suffix_taken != 0u ||
        opened->stream_consumed != 1u || opened->stream_ambiguous ||
        opened->cleanup_attempted || opened->fifo_poisoned ||
        opened->fifo_state.initialized != 1u ||
        opened->fifo_state.generation != generation ||
        opened->fifo_state.busy || opened->fifo_state.poisoned ||
        opened->logical_bytes > capacity ||
        opened->logical_bytes != opened->fifo_plan.logical_bytes ||
        opened->serial_suffix_offset == 0u ||
        opened->serial_suffix_offset != opened->frame.wire_size ||
        opened->serial_suffix_offset > opened->logical_bytes ||
        opened->serial_suffix_bytes !=
            opened->logical_bytes - opened->serial_suffix_offset ||
        opened->additional_bytes_discarded != 0u ||
        opened->fifo_state.rx_consumed !=
            (opened->length_counter & P4_HOSTED_RX_MASK)) return 0;
    if (!p4_hosted_rx_start(state, generation)) return 0;
    /* Consume the transfer capability even if its already-read contents fail
     * validation below. A second receiver must never replay this suffix. */
    opened->suffix_taken = 1u;
    result->error = P4_HOSTED_RX_OK;
    result->batches = result->messages = result->frames =
        result->partial = 0u;
    result->logical_bytes = opened->serial_suffix_bytes;
    checksum = !!(opened->init.capabilities & P4_HOSTED_CHECKSUM_CAPABILITY);
    if (!p4_hosted_decode(buffer, opened->logical_bytes, checksum, &initial) ||
        initial.interface_type != P4_HOSTED_PRIVATE_INTERFACE ||
        initial.interface_number != 0u || initial.flags != 0u ||
        initial.packet_type != P4_HOSTED_PRIVATE_INIT_PACKET ||
        initial.wire_size != opened->serial_suffix_offset ||
        !p4_hosted_init_parse(initial.payload, initial.payload_size, &identity) ||
        identity.chip_id != opened->init.chip_id ||
        identity.capabilities != opened->init.capabilities ||
        identity.test_raw_tp != opened->init.test_raw_tp ||
        identity.rx_queue_size != opened->init.rx_queue_size ||
        identity.tx_queue_size != opened->init.tx_queue_size ||
        identity.extended_capabilities != opened->init.extended_capabilities ||
        identity.present != opened->init.present)
        ok = rx_fail(state, &opened->fifo_state, generation, result,
                     P4_HOSTED_RX_PROTOCOL, 1u);
    else
        ok = rx_batch(state, &opened->fifo_state, generation,
            buffer + opened->serial_suffix_offset, opened->serial_suffix_bytes,
            checksum, result);
    if (opened->fifo_state.generation == generation)
        opened->fifo_poisoned = opened->fifo_state.poisoned;
    return ok;
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
        !inbox_matches(&state->inbox, generation) ||
        state->frames.initialized != 1u || state->frames.generation != generation ||
        state->frames.poisoned ||
        state->frames.count > P4_HOSTED_FRAME_QUEUE_CAPACITY ||
        state->frames.head >= P4_HOSTED_FRAME_QUEUE_CAPACITY ||
        fifo->initialized != 1u || fifo->generation != generation ||
        fifo->poisoned || fifo->busy) return 0;
    result->error = P4_HOSTED_RX_OK;
    result->batches = result->messages = result->frames =
        result->partial = 0u;
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
