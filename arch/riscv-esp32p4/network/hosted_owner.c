/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#include "hosted_owner.h"

#include "hosted_rpc.h"
#include "hosted_ota.h"

struct owner_range {
    uintptr_t first;
    uintptr_t end;
};

struct owner_runtime {
    struct p4_hosted_owner *owner;
    struct p4_hosted_fifo *fifo;
    const struct p4_hosted_owner_ops *ops;
    const struct p4_hosted_rx_ops *rx;
    const struct p4_hosted_tx_ops *tx;
    uint32_t generation;
    uint32_t deadline_start;
    uint32_t deadline_us;
    uint32_t slice_start;
    uint32_t slice_us;
};

enum owner_lifetime {
    OWNER_LIFETIME_OK = 0,
    OWNER_LIFETIME_STOPPED,
    OWNER_LIFETIME_TIMEOUT,
    OWNER_LIFETIME_ACQUIRE,
    OWNER_LIFETIME_BUDGET,
    OWNER_LIFETIME_ERROR
};

static int make_range(const void *pointer, size_t size,
                      struct owner_range *range)
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

static int ranges_disjoint(const struct owner_range *left,
                           const struct owner_range *right)
{
    return left->end <= right->first || right->end <= left->first;
}

static int collect_ranges(struct owner_range *ranges,
                          const void *const *objects,
                          const size_t *sizes, unsigned count)
{
    unsigned first, second;

    if (!ranges || !objects || !sizes || count > 8u)
        return 0;
    for (first = 0u; first < count; ++first)
        if (!make_range(objects[first], sizes[first], &ranges[first]))
            return 0;
    for (first = 0u; first < count; ++first)
        for (second = first + 1u; second < count; ++second)
            if (!ranges_disjoint(&ranges[first], &ranges[second]))
                return 0;
    return 1;
}

static void wipe_bytes(void *memory, size_t size)
{
    volatile uint8_t *bytes = (volatile uint8_t *)memory;
    size_t index;

    for (index = 0u; index < size; ++index)
        bytes[index] = 0u;
}

static void copy_bytes(void *destination, const void *source, size_t size)
{
    volatile uint8_t *out = (volatile uint8_t *)destination;
    const volatile uint8_t *in = (const volatile uint8_t *)source;
    size_t index;

    for (index = 0u; index < size; ++index)
        out[index] = in[index];
}

static int request_id_valid(uint32_t request_id)
{
    return request_id != 0u &&
           request_id <= UINT32_MAX - UINT32_C(256);
}

static int request_id_supported(uint32_t request_id)
{
    return request_id == 257u || request_id == 260u ||
           (request_id >= 278u && request_id <= 284u) ||
           (request_id >= 286u && request_id <= 289u) || request_id == 294u;
}

static int ids_consistent(const struct p4_hosted_request_ids *ids)
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

static int fifo_matches(const struct p4_hosted_fifo *fifo,
                        uint32_t generation)
{
    return fifo->initialized == 1u && fifo->generation == generation &&
           /* Direction belongs to the last plan; only busy denotes an
            * outstanding operation. Successful TX retains direction=1. */
           fifo->busy == 0u && fifo->pending_write <= 1u &&
           fifo->poisoned == 0u && fifo->rx_consumed <= P4_HOSTED_RX_MASK &&
           fifo->tx_sent <= P4_HOSTED_TX_MASK &&
           fifo->pool_limit <= P4_HOSTED_TX_MASK;
}

static int owner_matches(const struct p4_hosted_owner *owner,
                         uint32_t generation)
{
    int pending_consistent;
    const struct p4_hosted_inbox *inbox = &owner->rx.inbox;

    if (inbox->discovery_armed > 1u || inbox->discovery_seen > 1u)
        return 0;
    if (inbox->discovery_armed == 0u) {
        if (inbox->discovery_generation != 0u || inbox->discovery_uid != 0u ||
            inbox->discovery_seen != 0u ||
            (owner->pending_active && owner->pending_request_id == 350u))
            return 0;
    } else if (inbox->discovery_generation != generation ||
               inbox->discovery_uid == 0u ||
               owner->ids.active_request_id != 350u ||
               inbox->discovery_uid != owner->ids.active_uid) {
        return 0;
    }

    pending_consistent = owner->pending_active == 0u
        ? owner->pending_uid == 0u && owner->pending_request_id == 0u
        : owner->pending_uid != 0u &&
          (request_id_supported(owner->pending_request_id) ||
           owner->pending_request_id == 350u) &&
          owner->ids.active_uid == owner->pending_uid &&
          owner->ids.active_request_id == owner->pending_request_id;

    return owner->initialized == 1u && owner->poisoned == 0u &&
           owner->generation == generation &&
           owner->rx.initialized == 1u && owner->rx.poisoned == 0u &&
           owner->rx.generation == generation &&
           owner->rx.assembly.initialized == 1u &&
           owner->rx.assembly.poisoned == 0u &&
           owner->rx.assembly.generation == generation &&
           owner->rx.inbox.initialized == 1u &&
           owner->rx.inbox.poisoned == 0u &&
           owner->rx.inbox.generation == generation &&
           owner->rx.inbox.count <= P4_HOSTED_INBOX_CAPACITY &&
           owner->rx.inbox.head < P4_HOSTED_INBOX_CAPACITY &&
           owner->rx.frames.initialized == 1u &&
           owner->rx.frames.poisoned == 0u &&
           owner->rx.frames.generation == generation &&
           owner->rx.frames.count <= P4_HOSTED_FRAME_QUEUE_CAPACITY &&
           owner->rx.frames.head < P4_HOSTED_FRAME_QUEUE_CAPACITY &&
           owner->partial_active <= 1u && owner->pending_active <= 1u &&
           pending_consistent &&
           owner->partial_active == !!owner->rx.assembly.fragments &&
           ids_consistent(&owner->ids) && owner->ids.retired == 0u &&
           owner->ids.generation == generation;
}

static int ops_valid(const struct p4_hosted_owner_ops *ops)
{
    return ops->acquire && ops->release && ops->now_us && ops->wait &&
           ops->set_budget && ops->stopping && ops->rx && ops->tx &&
           ops->rx->session_ok && ops->rx->budget_ok &&
           ops->rx->read_word && ops->rx->write_word && ops->rx->read_fifo &&
           ops->tx->read_word && ops->tx->write_fifo &&
           ops->tx->session_ok && ops->tx->budget_ok && ops->tx->yield;
}

static void clear_owner_scratch(struct p4_hosted_owner *owner)
{
    wipe_bytes(&owner->pending, sizeof(owner->pending));
    wipe_bytes(&owner->rx_result, sizeof(owner->rx_result));
    wipe_bytes(&owner->inbox_scratch, sizeof(owner->inbox_scratch));
    wipe_bytes(&owner->reply, sizeof(owner->reply));
    wipe_bytes(owner->request, sizeof(owner->request));
    wipe_bytes(owner->wire, sizeof(owner->wire));
    wipe_bytes(owner->frame, sizeof(owner->frame));
}

static void clear_result(struct p4_hosted_owner_result *result)
{
    wipe_bytes(result, sizeof(*result));
}

int p4_hosted_owner_ids_init(struct p4_hosted_owner *owner,
                             uint32_t generation, uint32_t now_us)
{
    int first_epoch;

    if (!owner || generation == 0u ||
        !make_range(owner, sizeof(*owner), &(struct owner_range){0}) ||
        owner->rx.initialized != 1u || owner->rx.poisoned != 0u ||
        owner->rx.generation != generation ||
        owner->rx.assembly.initialized != 1u ||
        owner->rx.assembly.poisoned != 0u ||
        owner->rx.assembly.generation != generation ||
        owner->rx.inbox.initialized != 1u || owner->rx.inbox.poisoned != 0u ||
        owner->rx.inbox.generation != generation ||
        owner->rx.inbox.discovery_armed != 0u ||
        owner->rx.inbox.discovery_generation != 0u ||
        owner->rx.inbox.discovery_uid != 0u ||
        owner->rx.inbox.discovery_seen != 0u ||
        owner->rx.frames.initialized != 1u || owner->rx.frames.poisoned != 0u ||
        owner->rx.frames.generation != generation)
        return 0;

    first_epoch = owner->initialized == 0u;
    if (first_epoch) {
        if (owner->generation != 0u || owner->poisoned != 0u ||
            owner->pending_active != 0u || owner->partial_active != 0u ||
            owner->pending_uid != 0u || owner->pending_request_id != 0u)
            return 0;
    } else if (owner->initialized != 1u || owner->poisoned != 1u ||
               owner->generation == 0u || owner->generation == generation ||
               owner->pending_active != 0u ||
               !ids_consistent(&owner->ids) || owner->ids.retired != 1u ||
               owner->ids.generation != owner->generation) {
        return 0;
    }

    if (!p4_hosted_request_ids_init(&owner->ids, generation))
        return 0;

    /* Do not touch owner->rx: its OPEN suffix may already own RPCs/frames. */
    clear_owner_scratch(owner);
    owner->generation = generation;
    owner->initialized = 1u;
    owner->poisoned = 0u;
    owner->pending_active = 0u;
    owner->pending_uid = 0u;
    owner->pending_request_id = 0u;
    owner->partial_active = !!owner->rx.assembly.fragments;
    owner->partial_started_us = now_us;
    return 1;
}

static enum owner_lifetime check_lifetime(struct p4_hosted_owner *owner,
    const struct p4_hosted_owner_ops *ops, uint32_t generation,
    uint32_t start, uint32_t limit)
{
    uint32_t now;
    int stopping;

    stopping = ops->stopping(ops->context);
    if (stopping == 1)
        return OWNER_LIFETIME_STOPPED;
    if (stopping != 0)
        return OWNER_LIFETIME_ERROR;
    if (owner->generation != generation || owner->initialized != 1u ||
        owner->poisoned != 0u)
        return OWNER_LIFETIME_ERROR;
    now = ops->now_us(ops->context);
    if ((uint32_t)(now - start) >= limit)
        return OWNER_LIFETIME_TIMEOUT;
    if (owner->partial_active != 0u &&
        (uint32_t)(now - owner->partial_started_us) >=
            P4_HOSTED_OWNER_PARTIAL_TIMEOUT_US)
        return OWNER_LIFETIME_TIMEOUT;
    return OWNER_LIFETIME_OK;
}

static void owner_retire(struct p4_hosted_owner *owner,
                         struct p4_hosted_fifo *fifo,
                         uint32_t generation)
{
    if (owner->initialized != 1u || owner->generation != generation)
        return;

    (void)p4_hosted_rx_invalidate(&owner->rx, fifo, generation);
    if (owner->ids.initialized == 1u &&
        owner->ids.generation == generation && owner->ids.retired == 0u)
        (void)p4_hosted_request_ids_invalidate(&owner->ids, generation);
    owner->poisoned = 1u;
    owner->pending_active = 0u;
    owner->partial_active = 0u;
    owner->pending_uid = 0u;
    owner->pending_request_id = 0u;
    wipe_bytes(&owner->inbox_scratch, sizeof(owner->inbox_scratch));
    wipe_bytes(&owner->reply, sizeof(owner->reply));
    wipe_bytes(owner->request, sizeof(owner->request));
    wipe_bytes(owner->wire, sizeof(owner->wire));
    wipe_bytes(owner->frame, sizeof(owner->frame));
}

int p4_hosted_owner_cancel(struct p4_hosted_owner *owner,
    struct p4_hosted_fifo *fifo, uint32_t generation)
{
    const void *objects[2] = {owner, fifo};
    const size_t sizes[2] = {sizeof(*owner), sizeof(*fifo)};
    struct owner_range ranges[2];

    if (generation == 0u || !collect_ranges(ranges, objects, sizes, 2u) ||
        owner->initialized != 1u || owner->generation != generation)
        return 0;

    owner_retire(owner, fifo, generation);
    return 1;
}

static int owner_ops_ranges(const struct p4_hosted_owner *owner,
    const struct p4_hosted_fifo *fifo, const struct p4_hosted_owner_ops *ops,
    const void *request, size_t request_size,
    const struct p4_hosted_owner_result *result,
    const struct p4_hosted_init *init)
{
    const void *objects[8];
    size_t sizes[8];
    struct owner_range ranges[8];
    unsigned count = 0u;

    objects[count] = owner;
    sizes[count++] = sizeof(*owner);
    objects[count] = fifo;
    sizes[count++] = sizeof(*fifo);
    objects[count] = ops;
    sizes[count++] = sizeof(*ops);
    objects[count] = ops ? ops->rx : NULL;
    sizes[count++] = sizeof(struct p4_hosted_rx_ops);
    objects[count] = ops ? ops->tx : NULL;
    sizes[count++] = sizeof(struct p4_hosted_tx_ops);
    if (request_size != 0u) {
        objects[count] = request;
        sizes[count++] = request_size;
    }
    objects[count] = result;
    sizes[count++] = sizeof(*result);
    if (init) {
        objects[count] = init;
        sizes[count++] = sizeof(*init);
    }
    return collect_ranges(ranges, objects, sizes, count);
}

static int owner_discovery_body_matches(const struct p4_hosted_rpc *rpc,
                                        uint32_t nonce)
{
    uint8_t body[8];
    size_t size = 0u, i;
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
    for (i = 0u; i < size; ++i)
        if (rpc->payload[i] != body[i]) return 0;
    return 1;
}

static int owner_exchange_preflight(struct p4_hosted_owner *owner,
    struct p4_hosted_fifo *fifo, const struct p4_hosted_owner_ops *ops,
    uint32_t generation, unsigned checksum_enabled,
    const uint8_t *request, size_t request_size, uint32_t timeout_us,
    struct p4_hosted_owner_result *result, struct p4_hosted_rpc *rpc,
    const struct p4_hosted_init *init, uint32_t nonce, unsigned path)
{
    unsigned discovery = path == 1u;
    unsigned ota = path == 2u;
    if (!owner || !fifo || !ops || !request || !result || !rpc ||
        generation == 0u || checksum_enabled > 1u || path > 2u ||
        request_size == 0u ||
        request_size > P4_HOSTED_PENDING_MAX_REQUEST_SIZE ||
        timeout_us == 0u || timeout_us > P4_HOSTED_OWNER_MAX_EXCHANGE_US ||
        (discovery && (!init || !nonce)) ||
        !owner_ops_ranges(owner, fifo, ops, request, request_size, result,
                          discovery ? init : NULL) ||
        !ops_valid(ops) || !owner_matches(owner, generation) ||
        !fifo_matches(fifo, generation) || owner->pending_active != 0u ||
        owner->ids.active_uid == 0u ||
        owner->rx.inbox.discovery_armed != 0u ||
        owner->rx.inbox.discovery_generation != 0u ||
        owner->rx.inbox.discovery_uid != 0u ||
        owner->rx.inbox.discovery_seen != 0u ||
        ((discovery || ota) && owner->partial_active != 0u) ||
        !(discovery ? p4_hosted_rpc_decode_board_discovery(request, request_size, rpc)
                    : p4_hosted_rpc_decode(request, request_size, rpc)) ||
        rpc->msg_type != P4_HOSTED_RPC_TYPE_REQUEST || rpc->uid == 0u ||
        (discovery ? (rpc->msg_id != 350u ||
                      !p4_hosted_init_has_board_discovery(init) ||
                      !owner_discovery_body_matches(rpc, nonce))
                   : (ota
                       ? !p4_hosted_ota_request_is_canonical(request, request_size)
                       : !request_id_supported(rpc->msg_id))) ||
        owner->ids.active_uid != rpc->uid ||
        owner->ids.active_request_id != rpc->msg_id)
        return 0;
    return 1;
}

static int owner_step_preflight(struct p4_hosted_owner *owner,
    struct p4_hosted_fifo *fifo, const struct p4_hosted_owner_ops *ops,
    uint32_t generation, unsigned checksum_enabled,
    uint32_t slice_budget_us, struct p4_hosted_owner_result *result)
{
    if (!owner || !fifo || !ops || !result || generation == 0u ||
        checksum_enabled > 1u || slice_budget_us == 0u ||
        slice_budget_us > P4_HOSTED_OWNER_MAX_SLICE_US ||
        !owner_ops_ranges(owner, fifo, ops, NULL, 0u, result, NULL) ||
        !ops_valid(ops) || !owner_matches(owner, generation) ||
        !fifo_matches(fifo, generation) || owner->pending_active != 0u ||
        owner->ids.active_uid != 0u ||
        owner->ids.active_request_id != 0u ||
        owner->rx.inbox.discovery_armed != 0u ||
        owner->rx.inbox.discovery_generation != 0u ||
        owner->rx.inbox.discovery_uid != 0u ||
        owner->rx.inbox.discovery_seen != 0u)
        return 0;
    return 1;
}

static int runtime_within_deadlines(struct owner_runtime *runtime)
{
    uint32_t now;

    if (runtime->owner->generation != runtime->generation ||
        runtime->owner->poisoned != 0u ||
        runtime->fifo->generation != runtime->generation ||
        runtime->fifo->poisoned != 0u)
        return 0;
    now = runtime->ops->now_us(runtime->ops->context);
    if ((uint32_t)(now - runtime->deadline_start) >= runtime->deadline_us ||
        (uint32_t)(now - runtime->slice_start) >= runtime->slice_us)
        return 0;
    if (runtime->owner->partial_active != 0u &&
        (uint32_t)(now - runtime->owner->partial_started_us) >=
            P4_HOSTED_OWNER_PARTIAL_TIMEOUT_US)
        return 0;
    return 1;
}

static int runtime_generation_matches(const struct owner_runtime *runtime,
                                     uint32_t generation)
{
    return generation == runtime->generation &&
           runtime->owner->initialized == 1u &&
           runtime->owner->poisoned == 0u &&
           runtime->owner->generation == generation &&
           runtime->fifo->initialized == 1u &&
           runtime->fifo->generation == generation &&
           runtime->fifo->poisoned == 0u;
}

static int owner_rx_session(void *context, uint32_t generation)
{
    struct owner_runtime *runtime = (struct owner_runtime *)context;
    int ok;

    if (!runtime_generation_matches(runtime, generation))
        return 0;
    ok = runtime->rx->session_ok(runtime->rx->context, generation);
    return ok == 1 && runtime_generation_matches(runtime, generation);
}

static int owner_tx_session(void *context, uint32_t generation)
{
    struct owner_runtime *runtime = (struct owner_runtime *)context;
    int ok;

    if (!runtime_generation_matches(runtime, generation))
        return 0;
    ok = runtime->tx->session_ok(runtime->tx->context, generation);
    return ok == 1 && runtime_generation_matches(runtime, generation);
}

static int owner_rx_budget(void *context)
{
    struct owner_runtime *runtime = (struct owner_runtime *)context;
    int ok;

    if (!runtime_within_deadlines(runtime))
        return 0;
    ok = runtime->rx->budget_ok(runtime->rx->context);
    return ok == 1 && runtime_within_deadlines(runtime);
}

static int owner_tx_budget(void *context)
{
    struct owner_runtime *runtime = (struct owner_runtime *)context;
    int ok;

    if (!runtime_within_deadlines(runtime))
        return 0;
    ok = runtime->tx->budget_ok(runtime->tx->context);
    return ok == 1 && runtime_within_deadlines(runtime);
}

static int owner_rx_read_word(void *context, uint32_t address, uint32_t *word)
{
    struct owner_runtime *runtime = (struct owner_runtime *)context;
    return runtime->rx->read_word(runtime->rx->context, address, word);
}

static int owner_rx_write_word(void *context, uint32_t address, uint32_t word)
{
    struct owner_runtime *runtime = (struct owner_runtime *)context;
    return runtime->rx->write_word(runtime->rx->context, address, word);
}

static int owner_rx_read_fifo(void *context,
    const struct p4_hosted_fifo_plan *plan, uint8_t *buffer)
{
    struct owner_runtime *runtime = (struct owner_runtime *)context;
    return runtime->rx->read_fifo(runtime->rx->context, plan, buffer);
}

static int owner_tx_read_word(void *context, uint32_t address,
                              uint32_t *value)
{
    struct owner_runtime *runtime = (struct owner_runtime *)context;
    return runtime->tx->read_word(runtime->tx->context, address, value);
}

static int owner_tx_write_fifo(void *context,
    const struct p4_hosted_fifo_plan *plan, const uint8_t *buffer,
    struct p4_hosted_tx_completion *completion)
{
    struct owner_runtime *runtime = (struct owner_runtime *)context;
    return runtime->tx->write_fifo(runtime->tx->context, plan, buffer,
                                   completion);
}

static void owner_tx_yield(void *context)
{
    struct owner_runtime *runtime = (struct owner_runtime *)context;
    runtime->tx->yield(runtime->tx->context);
}

static void make_rx_adapter(struct owner_runtime *runtime,
                            struct p4_hosted_rx_ops *adapter)
{
    adapter->context = runtime;
    adapter->session_ok = owner_rx_session;
    adapter->budget_ok = owner_rx_budget;
    adapter->read_word = owner_rx_read_word;
    adapter->write_word = owner_rx_write_word;
    adapter->read_fifo = owner_rx_read_fifo;
}

static void make_tx_adapter(struct owner_runtime *runtime,
                            struct p4_hosted_tx_ops *adapter)
{
    adapter->context = runtime;
    adapter->read_word = owner_tx_read_word;
    adapter->write_fifo = owner_tx_write_fifo;
    adapter->session_ok = owner_tx_session;
    adapter->budget_ok = owner_tx_budget;
    adapter->yield = owner_tx_yield;
}

static enum owner_lifetime acquire_slice(struct owner_runtime *runtime,
    uint32_t *slice_us, unsigned *acquired)
{
    uint32_t now, elapsed, remaining, acquire_wait;
    int result;
    enum owner_lifetime lifetime;

    *acquired = 0u;
    lifetime = check_lifetime(runtime->owner, runtime->ops,
        runtime->generation, runtime->deadline_start, runtime->deadline_us);
    if (lifetime != OWNER_LIFETIME_OK)
        return lifetime;

    now = runtime->ops->now_us(runtime->ops->context);
    elapsed = (uint32_t)(now - runtime->deadline_start);
    if (elapsed >= runtime->deadline_us)
        return OWNER_LIFETIME_TIMEOUT;
    remaining = runtime->deadline_us - elapsed;
    acquire_wait = remaining;
    if (acquire_wait > P4_HOSTED_OWNER_MAX_ACQUIRE_US)
        acquire_wait = P4_HOSTED_OWNER_MAX_ACQUIRE_US;

    result = runtime->ops->acquire(runtime->ops->context, acquire_wait);
    if (result == 0)
        return OWNER_LIFETIME_OK;
    if (result != 1)
        return OWNER_LIFETIME_ACQUIRE;
    *acquired = 1u;

    lifetime = check_lifetime(runtime->owner, runtime->ops,
        runtime->generation, runtime->deadline_start, runtime->deadline_us);
    if (lifetime != OWNER_LIFETIME_OK)
        return lifetime;

    now = runtime->ops->now_us(runtime->ops->context);
    elapsed = (uint32_t)(now - runtime->deadline_start);
    if (elapsed >= runtime->deadline_us)
        return OWNER_LIFETIME_TIMEOUT;
    remaining = runtime->deadline_us - elapsed;
    *slice_us = remaining;
    if (*slice_us > P4_HOSTED_OWNER_MAX_SLICE_US)
        *slice_us = P4_HOSTED_OWNER_MAX_SLICE_US;
    runtime->slice_start = now;
    runtime->slice_us = *slice_us;

    result = runtime->ops->set_budget(runtime->ops->context, *slice_us);
    if (result != 1)
        return OWNER_LIFETIME_BUDGET;
    if (!runtime_within_deadlines(runtime))
        return OWNER_LIFETIME_TIMEOUT;
    return OWNER_LIFETIME_OK;
}

static void owner_update_partial(struct p4_hosted_owner *owner,
                                 uint32_t poll_started_us,
                                 unsigned completed_messages)
{
    unsigned partial = !!owner->rx.assembly.fragments;

    if (partial == 0u) {
        owner->partial_active = 0u;
        owner->partial_started_us = 0u;
    } else if (owner->partial_active == 0u || completed_messages != 0u) {
        /* A batch may complete one SERIAL stream and end with the beginning
         * of another. Use acquisition start as a conservative timestamp: the
         * exact fragment arrival time within the batch is not observable. */
        owner->partial_active = 1u;
        owner->partial_started_us = poll_started_us;
    }
}

static int owner_valid_response(const struct p4_hosted_inbox_item *item,
                                 unsigned discovery)
{
    return item->payload_size <= P4_HOSTED_RPC_MAX_SIZE &&
           item->msg_type == P4_HOSTED_RPC_TYPE_RESPONSE &&
           ((item->msg_id >= 513u && item->msg_id <= 605u) ||
            (discovery && item->msg_id == 606u)) && item->uid != 0u;
}

static void copy_reply(struct p4_hosted_inbox_item *destination,
                       const struct p4_hosted_inbox_item *source)
{
    wipe_bytes(destination, sizeof(*destination));
    destination->msg_type = source->msg_type;
    destination->msg_id = source->msg_id;
    destination->uid = source->uid;
    destination->payload_size = source->payload_size;
    copy_bytes(destination->payload, source->payload, source->payload_size);
}

static int owner_drain(struct p4_hosted_owner *owner,
    const struct p4_hosted_owner_ops *ops, uint32_t generation,
    struct p4_hosted_owner_result *result, unsigned *worked)
{
    struct p4_hosted_inbox_item *item = &owner->inbox_scratch;
    enum p4_hosted_pending_correlation correlation;
    size_t frame_size;
    int ok;

    while (owner->rx.inbox.count != 0u) {
        if (!p4_hosted_inbox_pop(&owner->rx.inbox, generation, item))
            return 0;
        *worked = 1u;
        /* A suffix may contain a response matching an identity reserved by
         * the caller but not yet transmitted. Never let that pre-TX response
         * finish the reservation. */
        if (!owner->pending_active && owner_valid_response(item, 0u)) {
            ++result->unmatched;
            wipe_bytes(item, sizeof(*item));
            continue;
        }

        correlation = owner->pending_active && owner->pending_request_id == 350u
            ? p4_hosted_pending_correlate_board_discovery(&owner->ids,
                                                         generation, item)
            : p4_hosted_pending_correlate(&owner->ids, generation, item);
        if (correlation == P4_HOSTED_PENDING_CORRELATION_REFUSED) {
            wipe_bytes(item, sizeof(*item));
            return 0;
        }
        if (correlation == P4_HOSTED_PENDING_CORRELATION_EVENT) {
            if (!ops->dispatch_event) {
                wipe_bytes(item, sizeof(*item));
                return -1;
            }
            ok = ops->dispatch_event(ops->context, item);
            wipe_bytes(item, sizeof(*item));
            if (ok != 1)
                return -1;
            ++result->events;
            continue;
        }
        if (correlation == P4_HOSTED_PENDING_CORRELATION_MATCHED) {
            if (!owner->pending_active ||
                !owner_valid_response(item, owner->pending_request_id == 350u) ||
                item->uid != owner->pending_uid ||
                item->msg_id != owner->pending_request_id + 256u) {
                wipe_bytes(item, sizeof(*item));
                return 0;
            }
            copy_reply(&owner->reply, item);
            owner->pending_active = 0u;
            owner->pending_uid = 0u;
            owner->pending_request_id = 0u;
            result->matched = 1u;
            result->uid = item->uid;
            result->request_id = item->msg_id - 256u;
            wipe_bytes(item, sizeof(*item));
            continue;
        }

        /* A valid response observed before this invocation's TX cannot
         * complete a merely reserved identity. Keep it explicitly unmatched. */
        if (!owner_valid_response(item, 0u)) {
            wipe_bytes(item, sizeof(*item));
            return 0;
        }
        if (owner->pending_active) {
            /* pending_correlate already validated this item and left the
             * live reservation unchanged because its UID/ID did not match. */
            ++result->unmatched;
        } else if (owner->ids.active_uid != 0u &&
                   item->uid == owner->ids.active_uid &&
                   item->msg_id == owner->ids.active_request_id + 256u) {
            ++result->unmatched;
        } else {
            ++result->unmatched;
        }
        wipe_bytes(item, sizeof(*item));
    }

    while (owner->rx.frames.count != 0u) {
        frame_size = 0u;
        if (!p4_hosted_frames_pop(&owner->rx.frames, generation,
                                  owner->frame, sizeof(owner->frame),
                                  &frame_size))
            return 0;
        *worked = 1u;
        if (!ops->dispatch_frame) {
            wipe_bytes(owner->frame, sizeof(owner->frame));
            return -1;
        }
        ok = ops->dispatch_frame(ops->context, owner->frame, frame_size);
        wipe_bytes(owner->frame, sizeof(owner->frame));
        if (ok != 1)
            return -1;
        ++result->frames;
    }
    return 1;
}

static void owner_set_error(struct p4_hosted_owner_result *result,
                            enum p4_hosted_owner_error error,
                            struct p4_hosted_owner *owner,
                            struct p4_hosted_fifo *fifo,
                            uint32_t generation)
{
    owner_retire(owner, fifo, generation);
    result->error = error;
}

static int drain_or_retire(struct p4_hosted_owner *owner,
    struct p4_hosted_fifo *fifo, const struct p4_hosted_owner_ops *ops,
    uint32_t generation, struct p4_hosted_owner_result *result,
    unsigned *worked)
{
    int drained;

    drained = owner_drain(owner, ops, generation, result, worked);
    if (drained == 1)
        return 1;
    owner_set_error(result, drained == -1 ? P4_HOSTED_OWNER_DISPATCH :
                    P4_HOSTED_OWNER_PROTOCOL, owner, fifo, generation);
    return 0;
}

static void set_elapsed(struct p4_hosted_owner_result *result,
    const struct p4_hosted_owner_ops *ops, uint32_t start)
{
    result->elapsed_us = (uint32_t)(ops->now_us(ops->context) - start);
}

static enum p4_hosted_owner_error lifetime_error(
    enum owner_lifetime lifetime)
{
    if (lifetime == OWNER_LIFETIME_STOPPED)
        return P4_HOSTED_OWNER_STOPPED;
    if (lifetime == OWNER_LIFETIME_TIMEOUT)
        return P4_HOSTED_OWNER_TIMEOUT;
    if (lifetime == OWNER_LIFETIME_ACQUIRE)
        return P4_HOSTED_OWNER_ACQUIRE;
    if (lifetime == OWNER_LIFETIME_BUDGET)
        return P4_HOSTED_OWNER_BUDGET;
    return P4_HOSTED_OWNER_SESSION;
}

static int poll_one_acquisition(struct p4_hosted_owner *owner,
    struct p4_hosted_fifo *fifo, const struct p4_hosted_owner_ops *ops,
    uint32_t generation, unsigned checksum_enabled,
    struct owner_runtime *runtime, struct p4_hosted_owner_result *result,
    unsigned *worked, enum owner_lifetime *lifetime)
{
    struct p4_hosted_rx_ops rx_adapter;
    uint32_t poll_started_us = runtime->slice_start;
    int ok;

    make_rx_adapter(runtime, &rx_adapter);
    ok = p4_hosted_rx_poll(&owner->rx, fifo, generation, &rx_adapter,
        checksum_enabled, owner->wire, sizeof(owner->wire), &owner->rx_result);
    ops->release(ops->context);
    if (owner->rx_result.batches != 0u)
        result->batches += owner->rx_result.batches;
    result->rx_error = owner->rx_result.error;
    if (!ok) {
        *lifetime = OWNER_LIFETIME_ERROR;
        return 0;
    }

    owner_update_partial(owner, poll_started_us,
                         owner->rx_result.messages);
    if (owner->rx_result.batches != 0u || owner->rx_result.messages != 0u ||
        owner->rx_result.frames != 0u)
        *worked = 1u;

    if (!drain_or_retire(owner, fifo, ops, generation, result, worked)) {
        *lifetime = OWNER_LIFETIME_ERROR;
        return -1;
    }

    *lifetime = check_lifetime(owner, ops, generation,
        runtime->deadline_start, runtime->deadline_us);
    if (*lifetime != OWNER_LIFETIME_OK)
        return -2;
    return 1;
}

static int owner_exchange(struct p4_hosted_owner *owner,
    struct p4_hosted_fifo *fifo, const struct p4_hosted_owner_ops *ops,
    uint32_t generation, unsigned checksum_enabled,
    const uint8_t *request, size_t request_size, uint32_t timeout_us,
    struct p4_hosted_owner_result *result,
    const struct p4_hosted_init *init, uint32_t nonce, unsigned path)
{
    struct p4_hosted_rpc rpc;
    struct owner_runtime runtime;
    struct p4_hosted_tx_ops tx_adapter;
    uint32_t start, slice_us;
    unsigned acquired, attempts = 0u, polls = 0u, worked = 0u;
    enum owner_lifetime lifetime;
    int submitted, drained;
    unsigned discovery = path == 1u;

    if (!owner_exchange_preflight(owner, fifo, ops, generation,
        checksum_enabled, request, request_size, timeout_us, result, &rpc,
        init, nonce, path))
        return 0;

    clear_result(result);
    result->uid = rpc.uid;
    result->request_id = rpc.msg_id;
    clear_owner_scratch(owner);
    copy_bytes(owner->request, request, request_size);

    start = ops->now_us(ops->context);
    lifetime = check_lifetime(owner, ops, generation, start, timeout_us);
    if (lifetime != OWNER_LIFETIME_OK) {
        owner_set_error(result, lifetime_error(lifetime), owner, fifo,
                        generation);
        set_elapsed(result, ops, start);
        return 0;
    }

    /* Initial OPEN suffix events/frames must not be stranded by the first TX.
     * A response matching a reservation is explicitly unmatched until TX. */
    drained = owner_drain(owner, ops, generation, result, &worked);
    if (drained != 1) {
        owner_set_error(result, drained == -1 ? P4_HOSTED_OWNER_DISPATCH :
                        P4_HOSTED_OWNER_PROTOCOL, owner, fifo, generation);
        set_elapsed(result, ops, start);
        return 0;
    }

    if (discovery && !p4_hosted_inbox_arm_discovery_response(
        &owner->rx.inbox, generation, rpc.uid)) {
        owner_set_error(result, P4_HOSTED_OWNER_PROTOCOL, owner, fifo,
                        generation);
        set_elapsed(result, ops, start);
        return 0;
    }

    runtime.owner = owner;
    runtime.fifo = fifo;
    runtime.ops = ops;
    runtime.rx = ops->rx;
    runtime.tx = ops->tx;
    runtime.generation = generation;
    runtime.deadline_start = start;
    runtime.deadline_us = timeout_us;
    runtime.slice_start = start;
    runtime.slice_us = timeout_us;

    for (;;) {
        lifetime = check_lifetime(owner, ops, generation, start, timeout_us);
        if (lifetime != OWNER_LIFETIME_OK) {
            owner_set_error(result, lifetime_error(lifetime), owner, fifo,
                            generation);
            set_elapsed(result, ops, start);
            return 0;
        }

        lifetime = acquire_slice(&runtime, &slice_us, &acquired);
        ++attempts;
        if (lifetime != OWNER_LIFETIME_OK) {
            if (acquired != 0u)
                ops->release(ops->context);
            owner_set_error(result, lifetime_error(lifetime), owner, fifo,
                            generation);
            set_elapsed(result, ops, start);
            return 0;
        }
        if (acquired == 0u) {
            if (attempts >= P4_HOSTED_OWNER_MAX_POLLS) {
                owner_set_error(result, P4_HOSTED_OWNER_TIMEOUT, owner, fifo,
                                generation);
                set_elapsed(result, ops, start);
                return 0;
            }
            ops->wait(ops->context);
            continue;
        }

        make_tx_adapter(&runtime, &tx_adapter);
        submitted = discovery
            ? p4_hosted_pending_submit_board_discovery(fifo, &owner->ids,
                generation, &tx_adapter, checksum_enabled, owner->request,
                request_size, owner->wire, sizeof(owner->wire), &owner->pending,
                init, nonce)
            : (path == 2u
                ? p4_hosted_pending_submit_ota(fifo, &owner->ids, generation,
                    &tx_adapter, checksum_enabled, owner->request, request_size,
                    owner->wire, sizeof(owner->wire), &owner->pending)
                : p4_hosted_pending_submit(fifo, &owner->ids, generation,
                &tx_adapter, checksum_enabled, owner->request, request_size,
                owner->wire, sizeof(owner->wire), &owner->pending));
        ops->release(ops->context);
        result->pending_error = owner->pending.error;
        result->submitted = owner->pending.submitted;
        if (!submitted) {
            owner_set_error(result,
                owner->pending.error == P4_HOSTED_PENDING_ERROR_SESSION
                    ? P4_HOSTED_OWNER_SESSION : P4_HOSTED_OWNER_TX,
                owner, fifo, generation);
            set_elapsed(result, ops, start);
            return 0;
        }

        if (!owner_matches(owner, generation) ||
            !fifo_matches(fifo, generation) ||
            owner->ids.active_uid != rpc.uid ||
            owner->ids.active_request_id != rpc.msg_id) {
            owner_set_error(result, P4_HOSTED_OWNER_SESSION, owner, fifo,
                            generation);
            set_elapsed(result, ops, start);
            return 0;
        }
        owner->pending_active = 1u;
        owner->pending_uid = rpc.uid;
        owner->pending_request_id = rpc.msg_id;
        wipe_bytes(owner->request, sizeof(owner->request));
        wipe_bytes(owner->wire, sizeof(owner->wire));

        /* TX and each RX poll use separate host acquisitions. */
        for (;;) {
            lifetime = check_lifetime(owner, ops, generation, start,
                                      timeout_us);
            if (lifetime != OWNER_LIFETIME_OK) {
                owner_set_error(result, lifetime_error(lifetime), owner, fifo,
                                generation);
                set_elapsed(result, ops, start);
                return 0;
            }
            if (polls >= P4_HOSTED_OWNER_MAX_POLLS ||
                attempts >= P4_HOSTED_OWNER_MAX_POLLS) {
                owner_set_error(result, P4_HOSTED_OWNER_TIMEOUT, owner, fifo,
                                generation);
                set_elapsed(result, ops, start);
                return 0;
            }

            lifetime = acquire_slice(&runtime, &slice_us, &acquired);
            ++attempts;
            if (lifetime != OWNER_LIFETIME_OK) {
                if (acquired != 0u)
                    ops->release(ops->context);
                owner_set_error(result, lifetime_error(lifetime), owner, fifo,
                                generation);
                set_elapsed(result, ops, start);
                return 0;
            }
            if (acquired == 0u) {
                ops->wait(ops->context);
                continue;
            }

            ++polls;
            {
                int poll_status = poll_one_acquisition(owner, fifo, ops,
                    generation, checksum_enabled, &runtime, result, &worked,
                    &lifetime);

                if (poll_status == 0) {
                    enum p4_hosted_owner_error rx_error =
                        owner->rx_result.error == P4_HOSTED_RX_SESSION
                            ? P4_HOSTED_OWNER_SESSION
                            : (owner->rx_result.error == P4_HOSTED_RX_BUDGET
                                ? P4_HOSTED_OWNER_TIMEOUT
                                : P4_HOSTED_OWNER_RX);

                    owner_set_error(result, rx_error, owner, fifo, generation);
                    set_elapsed(result, ops, start);
                    return 0;
                }
                if (poll_status == -1) {
                    set_elapsed(result, ops, start);
                    return 0;
                }
                if (poll_status == -2) {
                    owner_set_error(result, lifetime_error(lifetime), owner,
                                    fifo, generation);
                    set_elapsed(result, ops, start);
                    return 0;
                }
            }
            if (result->matched != 0u) {
                if (discovery && !p4_hosted_inbox_disarm_discovery_response(
                    &owner->rx.inbox, generation, rpc.uid)) {
                    owner_set_error(result, P4_HOSTED_OWNER_PROTOCOL, owner,
                                    fifo, generation);
                    set_elapsed(result, ops, start);
                    return 0;
                }
                result->error = P4_HOSTED_OWNER_OK;
                wipe_bytes(&owner->inbox_scratch,
                           sizeof(owner->inbox_scratch));
                set_elapsed(result, ops, start);
                return 1;
            }

            lifetime = check_lifetime(owner, ops, generation, start,
                                      timeout_us);
            if (lifetime != OWNER_LIFETIME_OK) {
                owner_set_error(result, lifetime_error(lifetime), owner, fifo,
                                generation);
                set_elapsed(result, ops, start);
                return 0;
            }
            ops->wait(ops->context);
        }
    }
}

int p4_hosted_owner_exchange(struct p4_hosted_owner *owner,
    struct p4_hosted_fifo *fifo, const struct p4_hosted_owner_ops *ops,
    uint32_t generation, unsigned checksum_enabled,
    const uint8_t *request, size_t request_size, uint32_t timeout_us,
    struct p4_hosted_owner_result *result)
{
    return owner_exchange(owner, fifo, ops, generation, checksum_enabled,
        request, request_size, timeout_us, result, NULL, 0u, 0u);
}

int p4_hosted_owner_exchange_board_discovery(struct p4_hosted_owner *owner,
    struct p4_hosted_fifo *fifo, const struct p4_hosted_owner_ops *ops,
    uint32_t generation, unsigned checksum_enabled,
    const uint8_t *request, size_t request_size, uint32_t timeout_us,
    struct p4_hosted_owner_result *result,
    const struct p4_hosted_init *init, uint32_t nonce)
{
    return owner_exchange(owner, fifo, ops, generation, checksum_enabled,
        request, request_size, timeout_us, result, init, nonce, 1u);
}

int p4_hosted_owner_exchange_ota(struct p4_hosted_owner *owner,
    struct p4_hosted_fifo *fifo, const struct p4_hosted_owner_ops *ops,
    uint32_t generation, unsigned checksum_enabled,
    const uint8_t *request, size_t request_size, uint32_t timeout_us,
    struct p4_hosted_owner_result *result)
{
    return owner_exchange(owner, fifo, ops, generation, checksum_enabled,
        request, request_size, timeout_us, result, NULL, 0u, 2u);
}

int p4_hosted_owner_step(struct p4_hosted_owner *owner,
    struct p4_hosted_fifo *fifo, const struct p4_hosted_owner_ops *ops,
    uint32_t generation, unsigned checksum_enabled,
    uint32_t slice_budget_us, struct p4_hosted_owner_result *result)
{
    struct owner_runtime runtime;
    uint32_t start, slice_us;
    unsigned acquired, worked = 0u;
    enum owner_lifetime lifetime;

    if (!owner_step_preflight(owner, fifo, ops, generation, checksum_enabled,
                              slice_budget_us, result))
        return 0;
    clear_result(result);
    start = ops->now_us(ops->context);
    lifetime = check_lifetime(owner, ops, generation, start, slice_budget_us);
    if (lifetime != OWNER_LIFETIME_OK) {
        owner_set_error(result, lifetime_error(lifetime), owner, fifo,
                        generation);
        set_elapsed(result, ops, start);
        return 0;
    }

    if (!drain_or_retire(owner, fifo, ops, generation, result, &worked)) {
        set_elapsed(result, ops, start);
        return 0;
    }

    runtime.owner = owner;
    runtime.fifo = fifo;
    runtime.ops = ops;
    runtime.rx = ops->rx;
    runtime.tx = ops->tx;
    runtime.generation = generation;
    runtime.deadline_start = start;
    runtime.deadline_us = slice_budget_us;
    runtime.slice_start = start;
    runtime.slice_us = slice_budget_us;

    lifetime = acquire_slice(&runtime, &slice_us, &acquired);
    if (lifetime != OWNER_LIFETIME_OK) {
        if (acquired != 0u)
            ops->release(ops->context);
        owner_set_error(result, lifetime_error(lifetime), owner, fifo,
                        generation);
        set_elapsed(result, ops, start);
        return 0;
    }
    if (acquired == 0u) {
        lifetime = check_lifetime(owner, ops, generation, start,
                                  slice_budget_us);
        if (lifetime != OWNER_LIFETIME_OK) {
            owner_set_error(result, lifetime_error(lifetime), owner, fifo,
                            generation);
            set_elapsed(result, ops, start);
            return 0;
        }
        result->idle = worked == 0u;
        set_elapsed(result, ops, start);
        return 1;
    }

    {
        int poll_status = poll_one_acquisition(owner, fifo, ops, generation,
            checksum_enabled, &runtime, result, &worked, &lifetime);

        if (poll_status == 0) {
            enum p4_hosted_owner_error rx_error =
                owner->rx_result.error == P4_HOSTED_RX_SESSION
                    ? P4_HOSTED_OWNER_SESSION
                    : (owner->rx_result.error == P4_HOSTED_RX_BUDGET
                        ? P4_HOSTED_OWNER_TIMEOUT : P4_HOSTED_OWNER_RX);

            owner_set_error(result, rx_error, owner, fifo, generation);
            set_elapsed(result, ops, start);
            return 0;
        }
        if (poll_status == -1) {
            set_elapsed(result, ops, start);
            return 0;
        }
        if (poll_status == -2) {
            owner_set_error(result, lifetime_error(lifetime), owner, fifo,
                            generation);
            set_elapsed(result, ops, start);
            return 0;
        }
    }
    result->idle = worked == 0u;
    result->error = P4_HOSTED_OWNER_OK;
    set_elapsed(result, ops, start);
    return 1;
}
