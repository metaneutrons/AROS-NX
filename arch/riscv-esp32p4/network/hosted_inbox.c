/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#include "hosted_inbox.h"

static int valid_range(const void *pointer, size_t size)
{
    uintptr_t start;

    if (!pointer || size == 0)
        return 0;
    start = (uintptr_t)pointer;
    return (uintmax_t)size <= (uintmax_t)(UINTPTR_MAX - start);
}

static int ranges_disjoint(const void *left, size_t left_size,
                           const void *right, size_t right_size)
{
    uintptr_t left_start, right_start;

    if (left_size == 0 || right_size == 0)
        return 1;
    if (!valid_range(left, left_size) || !valid_range(right, right_size))
        return 0;
    left_start = (uintptr_t)left;
    right_start = (uintptr_t)right;
    return left_start + left_size <= right_start ||
           right_start + right_size <= left_start;
}

static void wipe_bytes(void *memory, size_t size)
{
    volatile uint8_t *bytes = (volatile uint8_t *)memory;
    size_t i;

    for (i = 0; i < size; ++i)
        bytes[i] = 0;
}

static void copy_bytes(void *destination, const void *source, size_t size)
{
    uint8_t *out = (uint8_t *)destination;
    const uint8_t *in = (const uint8_t *)source;
    size_t i;

    for (i = 0; i < size; ++i)
        out[i] = in[i];
}

static void wipe_items(struct p4_hosted_inbox *inbox)
{
    size_t i;

    for (i = 0; i < P4_HOSTED_INBOX_CAPACITY; ++i)
        wipe_bytes(&inbox->items[i], sizeof(inbox->items[i]));
}

static int valid_envelope(uint32_t msg_type, uint32_t msg_id,
                          uint32_t uid, size_t payload_size)
{
    if (payload_size > P4_HOSTED_RPC_MAX_SIZE)
        return 0;
    if (msg_type == P4_HOSTED_RPC_TYPE_EVENT)
        return msg_id >= 769u && msg_id <= 776u;
    if (msg_type == P4_HOSTED_RPC_TYPE_RESPONSE)
        return msg_id >= 513u && msg_id <= 605u && uid != 0;
    return 0;
}

static int discovery_arm_matches(const struct p4_hosted_inbox *inbox,
                                 uint32_t generation, uint32_t uid)
{
    return inbox->discovery_armed == 1u &&
           inbox->discovery_generation == generation &&
           inbox->generation == generation && inbox->discovery_uid == uid &&
           uid != 0u;
}

static int queue_matches(const struct p4_hosted_inbox *inbox,
                         uint32_t generation)
{
    if (generation == 0u || inbox->initialized != 1u ||
        inbox->generation != generation || inbox->poisoned != 0u ||
        inbox->head >= P4_HOSTED_INBOX_CAPACITY ||
        inbox->count > P4_HOSTED_INBOX_CAPACITY ||
        inbox->discovery_armed > 1u || inbox->discovery_seen > 1u)
        return 0;
    if (inbox->discovery_armed == 0u)
        return inbox->discovery_generation == 0u &&
               inbox->discovery_uid == 0u && inbox->discovery_seen == 0u;
    return inbox->discovery_generation == generation &&
           inbox->discovery_uid != 0u;
}

int p4_hosted_inbox_init(struct p4_hosted_inbox *inbox,
                         uint32_t generation)
{
    if (!valid_range(inbox, sizeof(*inbox)) || generation == 0)
        return 0;

    if (inbox->initialized == 0u) {
        if (inbox->generation != 0 || inbox->head != 0 || inbox->count != 0 ||
            inbox->poisoned != 0 || inbox->discovery_generation != 0u ||
            inbox->discovery_uid != 0u || inbox->discovery_armed != 0u ||
            inbox->discovery_seen != 0u)
            return 0;
    } else if (inbox->initialized == 1u) {
        if (inbox->generation == 0 || inbox->generation == generation)
            return 0;
    } else {
        return 0;
    }

    wipe_items(inbox);
    inbox->generation = generation;
    inbox->head = 0;
    inbox->count = 0;
    inbox->initialized = 1;
    inbox->poisoned = 0;
    inbox->discovery_generation = 0u;
    inbox->discovery_uid = 0u;
    inbox->discovery_armed = 0u;
    inbox->discovery_seen = 0u;
    return 1;
}

int p4_hosted_inbox_push(struct p4_hosted_inbox *inbox,
                         uint32_t generation,
                         const struct p4_hosted_rpc *rpc)
{
    struct p4_hosted_inbox_item *item;
    size_t tail, i;
    volatile uint8_t *destination;
    const volatile uint8_t *source;

    if (!valid_range(inbox, sizeof(*inbox)) ||
        !valid_range(rpc, sizeof(*rpc)) ||
        !ranges_disjoint(inbox, sizeof(*inbox), rpc, sizeof(*rpc)))
        return 0;

    if (rpc->payload_size > P4_HOSTED_RPC_MAX_SIZE)
        return 0;
    if (rpc->payload_size != 0 &&
        (!valid_range(rpc->payload, rpc->payload_size) ||
         !ranges_disjoint(inbox, sizeof(*inbox), rpc->payload,
                          rpc->payload_size) ||
         !ranges_disjoint(rpc, sizeof(*rpc), rpc->payload,
                          rpc->payload_size)))
        return 0;

    if (!queue_matches(inbox, generation))
        return 0;
    if (rpc->msg_type == P4_HOSTED_RPC_TYPE_RESPONSE && rpc->msg_id == 606u) {
        if (rpc->uid == 0u || !discovery_arm_matches(inbox, generation,
                                                      rpc->uid) ||
            inbox->discovery_seen != 0u)
            return 0;
    } else if (!valid_envelope(rpc->msg_type, rpc->msg_id, rpc->uid,
                               rpc->payload_size)) {
        return 0;
    }
    if (inbox->count == P4_HOSTED_INBOX_CAPACITY) {
        wipe_items(inbox);
        inbox->head = 0;
        inbox->count = 0;
        inbox->poisoned = 1;
        inbox->discovery_generation = 0u;
        inbox->discovery_uid = 0u;
        inbox->discovery_armed = 0u;
        inbox->discovery_seen = 0u;
        return 0;
    }

    tail = inbox->head + inbox->count;
    if (tail >= P4_HOSTED_INBOX_CAPACITY)
        tail -= P4_HOSTED_INBOX_CAPACITY;
    item = &inbox->items[tail];
    wipe_bytes(item, sizeof(*item));
    item->msg_type = rpc->msg_type;
    item->msg_id = rpc->msg_id;
    item->uid = rpc->uid;
    item->payload_size = rpc->payload_size;
    destination = item->payload;
    source = rpc->payload;
    for (i = 0; i < rpc->payload_size; ++i)
        destination[i] = source[i];
    ++inbox->count;
    if (rpc->msg_type == P4_HOSTED_RPC_TYPE_RESPONSE && rpc->msg_id == 606u)
        inbox->discovery_seen = 1u;
    return 1;
}

int p4_hosted_inbox_pop(struct p4_hosted_inbox *inbox,
                        uint32_t generation,
                        struct p4_hosted_inbox_item *out)
{
    struct p4_hosted_inbox_item *item;

    if (!valid_range(inbox, sizeof(*inbox)) ||
        !valid_range(out, sizeof(*out)) ||
        !ranges_disjoint(inbox, sizeof(*inbox), out, sizeof(*out)) ||
        !queue_matches(inbox, generation) || inbox->count == 0)
        return 0;

    item = &inbox->items[inbox->head];
    if (item->msg_type == P4_HOSTED_RPC_TYPE_RESPONSE &&
        item->msg_id == 606u) {
        if (item->uid == 0u || !discovery_arm_matches(inbox, generation,
                                                       item->uid) ||
            inbox->discovery_seen != 1u ||
            item->payload_size > P4_HOSTED_RPC_MAX_SIZE)
            return 0;
    } else if (!valid_envelope(item->msg_type, item->msg_id, item->uid,
                               item->payload_size)) {
        return 0;
    }

    copy_bytes(out, item, sizeof(*out));
    wipe_bytes(item, sizeof(*item));
    ++inbox->head;
    if (inbox->head == P4_HOSTED_INBOX_CAPACITY)
        inbox->head = 0;
    --inbox->count;
    return 1;
}

int p4_hosted_inbox_arm_discovery_response(struct p4_hosted_inbox *inbox,
                                           uint32_t generation,
                                           uint32_t uid)
{
    if (!valid_range(inbox, sizeof(*inbox)) || uid == 0u ||
        !queue_matches(inbox, generation) || inbox->count != 0u ||
        inbox->discovery_armed != 0u)
        return 0;

    inbox->discovery_generation = generation;
    inbox->discovery_uid = uid;
    inbox->discovery_seen = 0u;
    inbox->discovery_armed = 1u;
    return 1;
}

int p4_hosted_inbox_disarm_discovery_response(struct p4_hosted_inbox *inbox,
                                              uint32_t generation,
                                              uint32_t uid)
{
    if (!valid_range(inbox, sizeof(*inbox)) || uid == 0u ||
        !queue_matches(inbox, generation) || inbox->count != 0u ||
        !discovery_arm_matches(inbox, generation, uid))
        return 0;

    inbox->discovery_generation = 0u;
    inbox->discovery_uid = 0u;
    inbox->discovery_armed = 0u;
    inbox->discovery_seen = 0u;
    return 1;
}

uint32_t p4_hosted_inbox_discovery_response_expected_uid(
    const struct p4_hosted_inbox *inbox, uint32_t generation)
{
    if (!valid_range(inbox, sizeof(*inbox)) ||
        !queue_matches(inbox, generation) ||
        inbox->discovery_armed != 1u ||
        inbox->discovery_generation != generation ||
        inbox->discovery_uid == 0u)
        return 0u;
    return inbox->discovery_uid;
}

int p4_hosted_inbox_invalidate(struct p4_hosted_inbox *inbox,
                               uint32_t generation)
{
    if (!valid_range(inbox, sizeof(*inbox)) || generation == 0 ||
        inbox->initialized != 1u || inbox->generation != generation)
        return 0;

    wipe_items(inbox);
    inbox->head = 0;
    inbox->count = 0;
    inbox->poisoned = 1;
    inbox->discovery_generation = 0u;
    inbox->discovery_uid = 0u;
    inbox->discovery_armed = 0u;
    inbox->discovery_seen = 0u;
    return 1;
}
