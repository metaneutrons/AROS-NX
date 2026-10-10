/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#include "hosted_link.h"

static int overlap(const void *left, size_t left_size,
                     const void *right, size_t right_size)
{
    uintptr_t a = (uintptr_t)left, b = (uintptr_t)right;
    if (!left_size || !right_size)
        return 0;
    if (!left || !right || left_size > UINTPTR_MAX - a ||
        right_size > UINTPTR_MAX - b)
        return 1;
    return a < b + right_size && b < a + left_size;
}

static void clear_bytes(void *memory, size_t size)
{
    volatile uint8_t *bytes = memory;
    while (size--)
        *bytes++ = 0;
}

static void copy_bytes(void *destination, const void *source, size_t size)
{
    uint8_t *out = destination;
    const uint8_t *in = source;
    while (size--)
        *out++ = *in++;
}

int p4_hosted_link_init(struct p4_hosted_link_observation *link,
                         uint32_t generation)
{
    if (!link || generation == 0 ||
        sizeof(*link) > UINTPTR_MAX - (uintptr_t)link)
        return 0;
    clear_bytes(link, sizeof(*link));
    link->generation = generation;
    return 1;
}

int p4_hosted_link_observe(struct p4_hosted_link_observation *link,
                            uint32_t generation,
                            const struct p4_hosted_rpc *rpc)
{
    struct p4_hosted_link_observation next;
    if (!link || !rpc || generation == 0 ||
        sizeof(*link) > UINTPTR_MAX - (uintptr_t)link ||
        sizeof(*rpc) > UINTPTR_MAX - (uintptr_t)rpc ||
        overlap(link, sizeof(*link), rpc, sizeof(*rpc)) ||
        overlap(link, sizeof(*link), rpc->payload, rpc->payload_size) ||
        link->generation != generation || link->revision == UINT32_MAX ||
        link->kind > P4_HOSTED_LINK_DOWN)
        return 0;
    clear_bytes(&next, sizeof(next));
    next.generation = generation;
    next.revision = link->revision + 1;
    if (rpc->msg_id == 775u) {
        struct p4_hosted_sta_connected_event event;
        if (!p4_hosted_association_connected_event(rpc, &event))
            return 0;
        next.status = event.status;
        next.kind = event.status == 0 ? P4_HOSTED_LINK_UP_HINT :
                                       P4_HOSTED_LINK_DOWN;
        copy_bytes(next.ssid, event.ssid, sizeof(next.ssid));
        next.ssid_length = event.ssid_length;
        copy_bytes(next.bssid, event.bssid, sizeof(next.bssid));
        next.channel = event.channel;
        next.authmode = event.authmode;
        clear_bytes(&event, sizeof(event));
    } else if (rpc->msg_id == 776u) {
        struct p4_hosted_sta_disconnected_event event;
        if (!p4_hosted_association_disconnected_event(rpc, &event))
            return 0;
        next.status = event.status;
        next.kind = P4_HOSTED_LINK_DOWN;
        copy_bytes(next.ssid, event.ssid, sizeof(next.ssid));
        next.ssid_length = event.ssid_length;
        copy_bytes(next.bssid, event.bssid, sizeof(next.bssid));
        next.reason = event.reason;
        clear_bytes(&event, sizeof(event));
    } else {
        return 0;
    }
    copy_bytes(link, &next, sizeof(*link));
    clear_bytes(&next, sizeof(next));
    return 1;
}
