/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#include "discovery_client.h"
#include <string.h>

static int disjoint(const void *a, size_t na, const void *b, size_t nb)
{
    uintptr_t pa = (uintptr_t)a, pb = (uintptr_t)b;
    return a && b && na <= UINTPTR_MAX - pa && nb <= UINTPTR_MAX - pb &&
        (pa + na <= pb || pb + nb <= pa);
}

static enum board_client_result deadline(const struct board_client_ops *ops,
    void *context, uint32_t start, uint32_t limit)
{
    uint32_t now;
    if (!ops->clock_ms(context, &now)) return BOARD_CLIENT_CLOCK_FAILED;
    return (uint32_t)(now - start) >= limit ? BOARD_CLIENT_TIMEOUT : BOARD_CLIENT_OK;
}

enum board_client_result board_client_discover(const struct board_client_ops *ops,
    void *context, size_t context_size, struct board_client_workspace *work,
    struct board_client_observation *output)
{
    uint32_t start, token;
    unsigned state;
    enum board_client_result result, timing;
    int release_accepted = 0;

    if (!ops || !context || !context_size || !work || !output || !ops->clock_ms || !ops->wait ||
        !ops->cancelled || !ops->request || !ops->lease_poll || !ops->discover ||
        !ops->discovery_poll || !ops->release ||
        !disjoint(work, sizeof(*work), output, sizeof(*output)) ||
        !disjoint(work, sizeof(*work), ops, sizeof(*ops)) ||
        !disjoint(output, sizeof(*output), ops, sizeof(*ops)) ||
        !disjoint(work, sizeof(*work), context, context_size) ||
        !disjoint(output, sizeof(*output), context, context_size))
        return BOARD_CLIENT_INVALID;
    memset(work, 0, sizeof(*work));
    if (!ops->clock_ms(context, &start)) return BOARD_CLIENT_CLOCK_FAILED;
    if (ops->cancelled(context)) return BOARD_CLIENT_CANCELLED;
    token = ops->request(context);
    if (!token) return BOARD_CLIENT_UNAVAILABLE;
    result = BOARD_CLIENT_FAILED;

    for (;;) {
        timing = deadline(ops, context, start, BOARD_CLIENT_WORK_MS);
        if (timing != BOARD_CLIENT_OK) { result = timing; goto cleanup; }
        if (ops->cancelled(context)) { result = BOARD_CLIENT_CANCELLED; goto cleanup; }
        state = ops->lease_poll(context, token);
        if (state == BOARD_CLIENT_LEASE_ACTIVE) break;
        if (state != BOARD_CLIENT_LEASE_REQUESTED && state != BOARD_CLIENT_LEASE_BUSY)
            goto cleanup;
        ops->wait(context);
    }
    /* Polling is a hook too: charge its duration before queuing a job. */
    timing = deadline(ops, context, start, BOARD_CLIENT_WORK_MS);
    if (timing != BOARD_CLIENT_OK) { result = timing; goto cleanup; }
    if (ops->cancelled(context)) { result = BOARD_CLIENT_CANCELLED; goto cleanup; }
    if (!ops->discover(context, token)) goto cleanup;
    for (;;) {
        timing = deadline(ops, context, start, BOARD_CLIENT_WORK_MS);
        if (timing != BOARD_CLIENT_OK) { result = timing; goto cleanup; }
        if (ops->cancelled(context)) { result = BOARD_CLIENT_CANCELLED; goto cleanup; }
        state = ops->discovery_poll(context, token, &work->reply);
        if (state == BOARD_CLIENT_DISCOVERY_READY) {
            timing = deadline(ops, context, start, BOARD_CLIENT_WORK_MS);
            if (timing != BOARD_CLIENT_OK) { result = timing; goto cleanup; }
            if (work->reply.token != token || !work->reply.generation ||
                !work->reply.uid || !work->reply.nonce ||
                work->reply.inventory_size > sizeof(work->reply.inventory)) {
                result = BOARD_CLIENT_BAD_REPLY; goto cleanup;
            }
            if (work->reply.remote_status) {
                result = work->reply.inventory_size ? BOARD_CLIENT_BAD_REPLY :
                    BOARD_CLIENT_REMOTE_ERROR;
                goto cleanup;
            }
            if (!board_discovery_decode(work->reply.inventory,
                    work->reply.inventory_size, work->reply.nonce,
                    &work->candidate.snapshot)) {
                result = BOARD_CLIENT_BAD_REPLY; goto cleanup;
            }
            work->candidate.token = token;
            work->candidate.generation = work->reply.generation;
            work->candidate.uid = work->reply.uid;
            work->candidate.nonce = work->reply.nonce;
            result = BOARD_CLIENT_OK;
            goto cleanup;
        }
        if (state == BOARD_CLIENT_DISCOVERY_UNSUPPORTED) {
            result = BOARD_CLIENT_UNSUPPORTED; goto cleanup;
        }
        if (state != BOARD_CLIENT_DISCOVERY_REQUESTED &&
            state != BOARD_CLIENT_DISCOVERY_BUSY) goto cleanup;
        ops->wait(context);
    }

cleanup:
    /* Even a broken clock/cancellation must attempt release once. No reset,
     * detach, retry lease or silent WiFi association is a cleanup substitute. */
    release_accepted = ops->release(context, token);
    for (;;) {
        timing = deadline(ops, context, start, BOARD_CLIENT_TOTAL_MS);
        if (timing != BOARD_CLIENT_OK) {
            result = BOARD_CLIENT_RELEASE_UNCONFIRMED; break;
        }
        state = ops->lease_poll(context, token);
        timing = deadline(ops, context, start, BOARD_CLIENT_TOTAL_MS);
        if (timing != BOARD_CLIENT_OK) {
            result = BOARD_CLIENT_RELEASE_UNCONFIRMED; break;
        }
        if (state == BOARD_CLIENT_LEASE_RELEASED && release_accepted) {
            if (result == BOARD_CLIENT_OK && ops->cancelled(context))
                result = BOARD_CLIENT_CANCELLED;
            if (result == BOARD_CLIENT_OK) *output = work->candidate;
            break;
        }
        if (state == BOARD_CLIENT_LEASE_FAILED || state == BOARD_CLIENT_LEASE_IDLE ||
            state > BOARD_CLIENT_LEASE_BUSY || state == BOARD_CLIENT_LEASE_RELEASED) {
            result = BOARD_CLIENT_RELEASE_UNCONFIRMED; break;
        }
        if (!release_accepted) release_accepted = ops->release(context, token);
        ops->wait(context);
    }
    memset(work, 0, sizeof(*work));
    return result;
}
