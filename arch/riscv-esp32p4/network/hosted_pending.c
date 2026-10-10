/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#include "hosted_pending.h"

#include "hosted_rpc.h"
#include "hosted_serial.h"
#include "hosted_wire.h"
#include "hosted_init.h"

/* Deliberately private: normal WLAN admission must not grow with extensions. */
#define DISCOVERY_REQUEST_ID 350u
#define DISCOVERY_RESPONSE_ID 606u

static int discovery_body_matches(const struct p4_hosted_rpc *rpc,
                                   uint32_t nonce)
{
    uint8_t body[8];
    size_t size = 0, i;
    if (!nonce) return 0;
    body[size++] = 8u;
    body[size++] = P4_HOSTED_INIT_BOARD_DISCOVERY_VERSION_1;
    body[size++] = 16u;
    while (nonce >= 128u) {
        body[size++] = (uint8_t)(nonce | 128u);
        nonce >>= 7;
    }
    body[size++] = (uint8_t)nonce;
    if (rpc->payload_size != size) return 0;
    for (i = 0; i < size; ++i)
        if (rpc->payload[i] != body[i]) return 0;
    return 1;
}

struct pending_range {
    uintptr_t first;
    uintptr_t end;
};

static int make_range(const void *pointer, size_t size,
                      struct pending_range *range)
{
    uintptr_t first;

    if (!pointer || size == 0u || !range)
        return 0;
    first = (uintptr_t)pointer;
    if ((uintmax_t)size > (uintmax_t)(UINTPTR_MAX - first))
        return 0;
    range->first = first;
    range->end = first + (uintptr_t)size;
    return 1;
}

static int ranges_disjoint(const struct pending_range *left,
                           const struct pending_range *right)
{
    return left->end <= right->first || right->end <= left->first;
}

static int request_id_supported(uint32_t request_id)
{
    return request_id == 257u || request_id == 260u ||
           (request_id >= 278u && request_id <= 284u) ||
           (request_id >= 286u && request_id <= 289u) || request_id == 294u;
}

static int request_id_valid(uint32_t request_id)
{
    return request_id != 0u &&
           request_id <= UINT32_MAX - UINT32_C(256);
}

static int ledger_consistent(const struct p4_hosted_request_ids *ids)
{
    if (ids->initialized != 1u || ids->generation == 0u ||
        ids->next_uid == 0u || ids->retired > 1u || ids->exhausted > 1u)
        return 0;
    if (ids->exhausted != 0u && ids->next_uid != UINT32_MAX)
        return 0;
    if ((ids->active_uid == 0u) != (ids->active_request_id == 0u))
        return 0;
    if (ids->active_request_id != 0u &&
        !request_id_valid(ids->active_request_id))
        return 0;
    if (ids->active_uid != 0u &&
        ((ids->exhausted != 0u && ids->active_uid != UINT32_MAX) ||
         (ids->exhausted == 0u && ids->active_uid >= ids->next_uid)))
        return 0;
    if (ids->retired != 0u &&
        (ids->active_uid != 0u || ids->active_request_id != 0u))
        return 0;
    return 1;
}

static int live_identity_matches(const struct p4_hosted_request_ids *ids,
                                 uint32_t generation, uint32_t uid,
                                 uint32_t request_id)
{
    return ledger_consistent(ids) && ids->retired == 0u &&
           ids->generation == generation && ids->active_uid == uid &&
           ids->active_request_id == request_id;
}

static int fifo_session_healthy(const struct p4_hosted_fifo *fifo,
                                uint32_t generation)
{
    return fifo->generation == generation && fifo->initialized == 1u &&
           fifo->busy == 0u && fifo->poisoned == 0u &&
           fifo->pool_limit != 0u &&
           fifo->pool_limit <= P4_HOSTED_TX_MASK;
}

static void clear_result(struct p4_hosted_pending_result *result)
{
    volatile uint8_t *bytes = (volatile uint8_t *)result;
    size_t index;

    for (index = 0u; index < sizeof(*result); ++index)
        bytes[index] = 0u;
}

static void wipe_bytes(void *memory, size_t size)
{
    volatile uint8_t *bytes = (volatile uint8_t *)memory;
    size_t index;

    for (index = 0u; index < size; ++index)
        bytes[index] = 0u;
}

static void settle_failed_identity(struct p4_hosted_fifo *fifo,
                                   struct p4_hosted_request_ids *ids,
                                   uint32_t generation, uint32_t uid,
                                   uint32_t request_id, int session_error)
{
    int retire_epoch;

    /* A callback may have replaced the ledger while TX ran. Never complete
     * or retire anything except the exact live identity captured at entry. */
    if (!live_identity_matches(ids, generation, uid, request_id))
        return;

    retire_epoch = session_error ||
                   !fifo_session_healthy(fifo, generation);
    if (retire_epoch)
        (void)p4_hosted_request_ids_invalidate(ids, generation);
    else
        (void)p4_hosted_request_ids_finish(ids, generation, uid,
                                           request_id);
}

static int submit_preflight(struct p4_hosted_fifo *fifo,
                            struct p4_hosted_request_ids *ids,
                            uint32_t generation,
                            const struct p4_hosted_tx_ops *tx_ops,
                            unsigned checksum_enabled,
                            const uint8_t *request, size_t request_size,
                            uint8_t *txbuffer, size_t capacity,
                            struct p4_hosted_pending_result *result,
                            struct p4_hosted_rpc *rpc,
                            const struct p4_hosted_init *init,
                            uint32_t nonce, unsigned discovery)
{
    struct pending_range ranges[7];
    unsigned count = discovery ? 7u : 6u;
    unsigned first, second;

    if (!fifo || !ids || !tx_ops || !request || !txbuffer || !result ||
        !rpc || generation == 0u || checksum_enabled > 1u ||
        request_size == 0u ||
        request_size > P4_HOSTED_PENDING_MAX_REQUEST_SIZE ||
        capacity < P4_HOSTED_PENDING_BUFFER_MIN ||
        capacity > P4_HOSTED_PENDING_BUFFER_MAX ||
        !make_range(fifo, sizeof(*fifo), &ranges[0]) ||
        !make_range(ids, sizeof(*ids), &ranges[1]) ||
        !make_range(tx_ops, sizeof(*tx_ops), &ranges[2]) ||
        !make_range(request, request_size, &ranges[3]) ||
        !make_range(txbuffer, capacity, &ranges[4]) ||
        !make_range(result, sizeof(*result), &ranges[5]) ||
        (discovery && (!nonce ||
         !make_range(init, sizeof(*init), &ranges[6]))))
        return 0;

    for (first = 0u; first < count; ++first)
        for (second = first + 1u; second < count; ++second)
            if (!ranges_disjoint(&ranges[first], &ranges[second]))
                return 0;

    if (!tx_ops->read_word || !tx_ops->write_fifo ||
        !tx_ops->session_ok || !tx_ops->budget_ok || !tx_ops->yield ||
        !fifo_session_healthy(fifo, generation) ||
        !ledger_consistent(ids) || ids->retired != 0u ||
        ids->generation != generation || ids->active_uid == 0u ||
        !(discovery ? p4_hosted_rpc_decode_board_discovery(request, request_size, rpc)
                    : p4_hosted_rpc_decode(request, request_size, rpc)) ||
        rpc->msg_type != P4_HOSTED_RPC_TYPE_REQUEST || rpc->uid == 0u ||
        (discovery ? (rpc->msg_id != DISCOVERY_REQUEST_ID ||
                      !p4_hosted_init_has_board_discovery(init) ||
                      !discovery_body_matches(rpc, nonce))
                   : !request_id_supported(rpc->msg_id)) ||
        rpc->uid != ids->active_uid ||
        rpc->msg_id != ids->active_request_id)
        return 0;

    return 1;
}

static int pending_submit(struct p4_hosted_fifo *fifo,
                             struct p4_hosted_request_ids *ids,
                             uint32_t generation,
                             const struct p4_hosted_tx_ops *tx_ops,
                             unsigned checksum_enabled,
                             const uint8_t *request, size_t request_size,
                             uint8_t *txbuffer, size_t capacity,
                             struct p4_hosted_pending_result *result,
                             const struct p4_hosted_init *init,
                             uint32_t nonce, unsigned discovery)
{
    uint8_t serial_buffer[HOSTED_SERIAL_MESSAGE_OVERHEAD +
                          P4_HOSTED_PENDING_MAX_REQUEST_SIZE];
    struct p4_hosted_rpc rpc;
    struct hosted_serial_message serial_message;
    struct p4_hosted_frame frame;
    size_t serial_size = 0u, wire_size = 0u;
    int tx_ok, submit_ok = 0;

    if (!submit_preflight(fifo, ids, generation, tx_ops, checksum_enabled,
                          request, request_size, txbuffer, capacity, result,
                          &rpc, init, nonce, discovery))
        return 0;

    clear_result(result);
    result->request_id = rpc.msg_id;
    result->uid = rpc.uid;
    result->request_size = request_size;

    serial_message.endpoint = HOSTED_SERIAL_ENDPOINT_RPCRSP;
    serial_message.protobuf = request;
    serial_message.protobuf_size = request_size;
    serial_message.wire_size = 0u;
    if (!hosted_serial_encode(serial_buffer, sizeof(serial_buffer),
                              &serial_message, &serial_size)) {
        result->error = P4_HOSTED_PENDING_ERROR_ENCODE;
        settle_failed_identity(fifo, ids, generation, rpc.uid, rpc.msg_id, 0);
        goto done;
    }
    result->serial_size = serial_size;

    frame.interface_type = P4_HOSTED_SERIAL;
    frame.interface_number = 0u;
    frame.flags = 0u;
    frame.throttle = 0u;
    frame.packet_type = 0u;
    frame.sequence = 0u;
    frame.payload = serial_buffer;
    frame.payload_size = serial_size;
    frame.wire_size = 0u;
    if (!p4_hosted_encode(txbuffer, capacity, checksum_enabled, &frame,
                          &wire_size)) {
        result->error = P4_HOSTED_PENDING_ERROR_ENCODE;
        settle_failed_identity(fifo, ids, generation, rpc.uid, rpc.msg_id, 0);
        goto done;
    }
    result->wire_size = wire_size;

    tx_ok = p4_hosted_tx_send(fifo, generation, tx_ops, txbuffer, capacity,
                              wire_size, checksum_enabled, &result->tx);
    result->submitted = tx_ok ? 1u : 0u;

    if (!live_identity_matches(ids, generation, rpc.uid, rpc.msg_id)) {
        result->error = P4_HOSTED_PENDING_ERROR_SESSION;
        goto done;
    }

    if (!tx_ok) {
        int session_error =
            result->tx.error == P4_HOSTED_TX_ERROR_SESSION_LOST;
        result->error = session_error ||
                        !fifo_session_healthy(fifo, generation)
                      ? P4_HOSTED_PENDING_ERROR_SESSION
                      : P4_HOSTED_PENDING_ERROR_TX;
        settle_failed_identity(fifo, ids, generation, rpc.uid, rpc.msg_id,
                               session_error);
        goto done;
    }

    if (!fifo_session_healthy(fifo, generation)) {
        result->error = P4_HOSTED_PENDING_ERROR_SESSION;
        (void)p4_hosted_request_ids_invalidate(ids, generation);
        goto done;
    }

    result->error = P4_HOSTED_PENDING_ERROR_NONE;
    submit_ok = 1;

done:
    wipe_bytes(serial_buffer, sizeof(serial_buffer));
    return submit_ok;
}

int p4_hosted_pending_submit(struct p4_hosted_fifo *fifo,
    struct p4_hosted_request_ids *ids, uint32_t generation,
    const struct p4_hosted_tx_ops *tx_ops, unsigned checksum_enabled,
    const uint8_t *request, size_t request_size, uint8_t *txbuffer,
    size_t capacity, struct p4_hosted_pending_result *result)
{
    return pending_submit(fifo, ids, generation, tx_ops, checksum_enabled,
        request, request_size, txbuffer, capacity, result, NULL, 0u, 0u);
}

int p4_hosted_pending_submit_board_discovery(struct p4_hosted_fifo *fifo,
    struct p4_hosted_request_ids *ids, uint32_t generation,
    const struct p4_hosted_tx_ops *tx_ops, unsigned checksum_enabled,
    const uint8_t *request, size_t request_size, uint8_t *txbuffer,
    size_t capacity, struct p4_hosted_pending_result *result,
    const struct p4_hosted_init *init, uint32_t nonce)
{
    return pending_submit(fifo, ids, generation, tx_ops, checksum_enabled,
        request, request_size, txbuffer, capacity, result, init, nonce, 1u);
}

static int valid_inbox_envelope(const struct p4_hosted_inbox_item *item,
                                unsigned discovery)
{
    if (item->payload_size > P4_HOSTED_RPC_MAX_SIZE)
        return 0;
    if (item->msg_type == P4_HOSTED_RPC_TYPE_EVENT)
        return item->msg_id >= 769u && item->msg_id <= 776u;
    if (item->msg_type == P4_HOSTED_RPC_TYPE_RESPONSE)
        return ((item->msg_id >= 513u && item->msg_id <= 605u) ||
                (discovery && item->msg_id == DISCOVERY_RESPONSE_ID)) &&
               item->uid != 0u;
    return 0;
}

static enum p4_hosted_pending_correlation
pending_correlate(struct p4_hosted_request_ids *ids,
                            uint32_t generation,
                            const struct p4_hosted_inbox_item *item,
                            unsigned discovery)
{
    struct pending_range ids_range, item_range;

    if (!ids || !item || generation == 0u ||
        !make_range(ids, sizeof(*ids), &ids_range) ||
        !make_range(item, sizeof(*item), &item_range) ||
        !ranges_disjoint(&ids_range, &item_range) ||
        !ledger_consistent(ids) || ids->retired != 0u ||
        ids->generation != generation || !valid_inbox_envelope(item, discovery) ||
        (discovery && (!ids->active_uid ||
                       ids->active_request_id != DISCOVERY_REQUEST_ID)))
        return P4_HOSTED_PENDING_CORRELATION_REFUSED;

    if (item->msg_type == P4_HOSTED_RPC_TYPE_EVENT)
        return P4_HOSTED_PENDING_CORRELATION_EVENT;

    if (ids->active_uid == 0u ||
        item->uid != ids->active_uid ||
        item->msg_id != ids->active_request_id + 256u)
        return P4_HOSTED_PENDING_CORRELATION_UNMATCHED;

    if (!p4_hosted_request_ids_finish(ids, generation, ids->active_uid,
                                      ids->active_request_id))
        return P4_HOSTED_PENDING_CORRELATION_REFUSED;
    return P4_HOSTED_PENDING_CORRELATION_MATCHED;
}

enum p4_hosted_pending_correlation
p4_hosted_pending_correlate(struct p4_hosted_request_ids *ids,
    uint32_t generation, const struct p4_hosted_inbox_item *item)
{
    return pending_correlate(ids, generation, item, 0u);
}

enum p4_hosted_pending_correlation
p4_hosted_pending_correlate_board_discovery(struct p4_hosted_request_ids *ids,
    uint32_t generation, const struct p4_hosted_inbox_item *item)
{
    return pending_correlate(ids, generation, item, 1u);
}
