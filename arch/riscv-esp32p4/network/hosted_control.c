/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#include "hosted_control.h"
#include "hosted_serial.h"

static int control_disjoint(const void *const *objects, const size_t *sizes,
                            unsigned count)
{
    uintptr_t starts[6], ends[6];
    unsigned i, j;
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

static int control_fail(struct p4_hosted_control_result *result,
                        enum p4_hosted_control_error error)
{
    result->success = 0;
    result->error = error;
    return 0;
}

static void control_poison(struct p4_hosted_fifo *state, uint32_t generation)
{
    /* A callback belonging to an old epoch must never alter a new ledger. */
    if (state->generation != generation) return;
    if (state->busy)
        (void)p4_hosted_fifo_finish(state, generation, 1, 0, 0);
    state->busy = 0;
    state->poisoned = 1;
}

static int control_guard(struct p4_hosted_fifo *state, uint32_t generation,
                         const struct p4_hosted_tx_ops *ops,
                         struct p4_hosted_control_result *result)
{
    if (state->generation != generation ||
        ops->session_ok(ops->context, generation) != 1 ||
        state->generation != generation) {
        control_poison(state, generation);
        return control_fail(result, P4_HOSTED_CONTROL_SESSION);
    }
    if (ops->budget_ok(ops->context) != 1)
        return control_fail(result, P4_HOSTED_CONTROL_BUDGET);
    return 1;
}

static int control_read(struct p4_hosted_fifo *state, uint32_t generation,
    const struct p4_hosted_open_ops *rx, const struct p4_hosted_tx_ops *tx,
    uint32_t address, uint32_t *word, struct p4_hosted_control_result *result)
{
    int ok;
    if (!control_guard(state, generation, tx, result)) return 0;
    ok = rx->read_word(rx->context, address, word);
    if (!control_guard(state, generation, tx, result)) return 0;
    return ok == 1 ? 1 : control_fail(result, P4_HOSTED_CONTROL_REGISTER);
}

static int control_decode_batch(struct p4_hosted_fifo *state,
    uint32_t generation, uint32_t uid, uint32_t response_id,
    const uint8_t *buffer, size_t logical,
    unsigned checksum_enabled, struct p4_hosted_control_result *result)
{
    size_t offset = 0;
    struct p4_hosted_frame frame;
    struct hosted_serial_message message;
    struct p4_hosted_rpc rpc;
    while (offset < logical) {
        if (!p4_hosted_decode(buffer + offset, logical - offset,
                              checksum_enabled, &frame) ||
            frame.interface_type != P4_HOSTED_SERIAL ||
            frame.interface_number != 0 || frame.flags != 0 ||
            frame.packet_type != 0 ||
            !hosted_serial_decode(frame.payload, frame.payload_size, &message) ||
            !p4_hosted_rpc_decode(message.protobuf, message.protobuf_size, &rpc))
            goto malformed;
        if (message.endpoint == HOSTED_SERIAL_ENDPOINT_RPCEVT &&
            rpc.msg_type == P4_HOSTED_RPC_TYPE_EVENT) {
            ++result->events;
        } else if (message.endpoint == HOSTED_SERIAL_ENDPOINT_RPCRSP &&
                   rpc.msg_type == P4_HOSTED_RPC_TYPE_RESPONSE) {
            if (rpc.uid == uid && rpc.msg_id == response_id) {
                size_t i;
                volatile uint8_t *destination = result->response_body;
                if (result->matched || rpc.payload_size > sizeof(result->response_body))
                    goto malformed;
                for (i = 0; i < rpc.payload_size; ++i)
                    destination[i] = rpc.payload[i];
                result->response_size = rpc.payload_size;
                result->response_id = rpc.msg_id;
                result->response_uid = rpc.uid;
                result->matched = 1;
            } else ++result->unmatched;
        } else goto malformed;
        offset += frame.wire_size;
    }
    return 1;
malformed:
    control_poison(state, generation);
    return control_fail(result, P4_HOSTED_CONTROL_PROTOCOL);
}

int p4_hosted_control_exchange(struct p4_hosted_fifo *state, uint32_t generation,
    const struct p4_hosted_open_ops *rx,
    const struct p4_hosted_tx_ops *tx, unsigned checksum_enabled,
    const uint8_t *request, size_t request_size, uint8_t *buffer, size_t capacity,
    struct p4_hosted_control_result *result)
{
    const void *objects[6] = {state, rx, tx, buffer, result, request};
    const size_t sizes[6] = {sizeof(*state), sizeof(*rx), sizeof(*tx),
                            capacity, sizeof(*result), request_size};
    uint8_t serial[P4_HOSTED_CONTROL_MAX_RPC_SIZE + 12u];
    size_t serial_size, wire_size, i;
    struct p4_hosted_rpc rpc;
    volatile uint8_t *zero;
    struct hosted_serial_message message;
    struct p4_hosted_frame frame;
    struct p4_hosted_fifo_plan plan;
    uint32_t raw, counter, clear, delta;
    unsigned poll;
    unsigned packet_acknowledged;
    int ok;
    if (capacity < 512 || capacity > 4096 || checksum_enabled > 1 ||
        !generation || request_size == 0 ||
        request_size > P4_HOSTED_CONTROL_MAX_RPC_SIZE ||
        !control_disjoint(objects, sizes, 6)) return 0;
    if (!rx->read_word || !rx->write_word || !rx->read_fifo || !rx->yield ||
        !tx->session_ok || !tx->budget_ok || !tx->read_word ||
        !tx->write_fifo || !tx->yield || rx->context != tx->context ||
        state->initialized != 1 || state->generation != generation ||
        state->busy || state->poisoned || !state->pool_limit ||
        state->pool_limit > P4_HOSTED_TX_MASK) return 0;
    if (!p4_hosted_rpc_decode(request, request_size, &rpc) ||
        rpc.msg_type != P4_HOSTED_RPC_TYPE_REQUEST || !rpc.uid ||
        (rpc.msg_id != 257u && rpc.msg_id != 260u && rpc.msg_id != 278u &&
         rpc.msg_id != 279u && rpc.msg_id != 280u && rpc.msg_id != 281u)) return 0;
    zero = (volatile uint8_t *)result;
    for (i = 0; i < sizeof(*result); ++i) zero[i] = 0;
    message.endpoint = HOSTED_SERIAL_ENDPOINT_RPCRSP;
    message.protobuf = request;
    message.protobuf_size = request_size;
    message.wire_size = 0;
    if (!hosted_serial_encode(serial, sizeof(serial), &message, &serial_size))
        return control_fail(result, P4_HOSTED_CONTROL_ENCODE);
    frame.interface_type = P4_HOSTED_SERIAL;
    frame.interface_number = 0; frame.flags = 0; frame.throttle = 0;
    frame.packet_type = 0; frame.sequence = 0;
    frame.payload = serial; frame.payload_size = serial_size; frame.wire_size = 0;
    if (!p4_hosted_encode(buffer, capacity, checksum_enabled, &frame, &wire_size))
        return control_fail(result, P4_HOSTED_CONTROL_ENCODE);
    if (!p4_hosted_tx_send(state, generation, tx, buffer, capacity, wire_size,
                           checksum_enabled, &result->tx))
        return control_fail(result, P4_HOSTED_CONTROL_TX);
    for (poll = 0; poll < P4_HOSTED_CONTROL_MAX_POLLS; ++poll) {
        packet_acknowledged = 0;
        ++result->polls;
        if (!control_read(state, generation, rx, tx,
                           P4_HOSTED_OPEN_INT_RAW, &raw, result)) return 0;
        if (raw & (P4_HOSTED_OPEN_INT_RX_UNDERFLOW | P4_HOSTED_OPEN_INT_TX_OVERFLOW)) {
            control_poison(state, generation);
            return control_fail(result, P4_HOSTED_CONTROL_PEER_FAULT);
        }
        clear = raw & P4_HOSTED_OPEN_INT_CLEAR_MASK;
        if (clear) {
            if (!control_guard(state, generation, tx, result)) return 0;
            packet_acknowledged = !!(clear & P4_HOSTED_OPEN_INT_PACKET);
            ok = rx->write_word(rx->context, P4_HOSTED_OPEN_INT_CLEAR, clear);
            if (!control_guard(state, generation, tx, result)) {
                if (packet_acknowledged) control_poison(state, generation);
                return 0;
            }
            if (ok != 1) {
                if (packet_acknowledged) control_poison(state, generation);
                return control_fail(result, P4_HOSTED_CONTROL_REGISTER);
            }
        }
        if (raw & P4_HOSTED_OPEN_INT_PACKET) {
            if (!control_read(state, generation, rx, tx,
                    P4_HOSTED_OPEN_PACKET_LENGTH, &counter, result)) {
                if (packet_acknowledged) control_poison(state, generation);
                return 0;
            }
            delta = ((counter & P4_HOSTED_RX_MASK) - state->rx_consumed) & P4_HOSTED_RX_MASK;
            if (delta) {
                if (!p4_hosted_fifo_rx(state, generation, counter, &plan)) {
                    if (packet_acknowledged) control_poison(state, generation);
                    return control_fail(result, P4_HOSTED_CONTROL_COUNTER);
                }
                if (plan.bus_bytes > capacity) {
                    (void)p4_hosted_fifo_finish(state, generation, 0, 0, 0);
                    if (packet_acknowledged) control_poison(state, generation);
                    return control_fail(result, P4_HOSTED_CONTROL_CAPACITY);
                }
                if (!control_guard(state, generation, tx, result)) {
                    if (state->generation == generation && state->busy)
                        (void)p4_hosted_fifo_finish(state, generation, 0, 0, 0);
                    if (packet_acknowledged) control_poison(state, generation);
                    return 0;
                }
                ok = rx->read_fifo(rx->context, &plan, buffer);
                if (!control_guard(state, generation, tx, result)) {
                    control_poison(state, generation); return 0;
                }
                if (ok != 1) {
                    control_poison(state, generation);
                    return control_fail(result, P4_HOSTED_CONTROL_RX);
                }
                if (!p4_hosted_fifo_finish(state, generation, 1, 1, plan.bus_bytes)) {
                    control_poison(state, generation);
                    return control_fail(result, P4_HOSTED_CONTROL_COUNTER);
                }
                ++result->batches;
                if (!control_decode_batch(state, generation, rpc.uid,
                        rpc.msg_id + 256u, buffer,
                        plan.logical_bytes, checksum_enabled, result)) return 0;
                if (result->matched) {
                    if (!control_guard(state, generation, tx, result)) return 0;
                    result->success = 1;
                    return 1;
                }
            }
        }
        if (poll + 1u < P4_HOSTED_CONTROL_MAX_POLLS) {
            rx->yield(rx->context);
            if (!control_guard(state, generation, tx, result)) return 0;
        }
    }
    return control_fail(result, P4_HOSTED_CONTROL_TIMEOUT);
}

int p4_hosted_control_get_mac(struct p4_hosted_fifo *state, uint32_t generation,
    const struct p4_hosted_open_ops *rx, const struct p4_hosted_tx_ops *tx,
    unsigned checksum_enabled, uint32_t uid, uint8_t *buffer, size_t capacity,
    struct p4_hosted_control_result *result)
{
    uint8_t request[P4_HOSTED_CONTROL_MAX_RPC_SIZE];
    size_t request_size;
    struct p4_hosted_rpc response;
    if (!p4_hosted_mac_request(uid, request, sizeof(request), &request_size) ||
        !p4_hosted_control_exchange(state, generation, rx, tx, checksum_enabled,
            request, request_size, buffer, capacity, result)) return 0;
    response.msg_type = P4_HOSTED_RPC_TYPE_RESPONSE;
    response.msg_id = result->response_id;
    response.uid = result->response_uid;
    response.payload = result->response_body;
    response.payload_size = result->response_size;
    if (!p4_hosted_mac_response(&response, uid, &result->mac)) {
        control_poison(state, generation);
        return control_fail(result, P4_HOSTED_CONTROL_PROTOCOL);
    }
    if (result->mac.status || !result->mac.valid_mac)
        return control_fail(result, P4_HOSTED_CONTROL_REMOTE);
    return 1;
}
