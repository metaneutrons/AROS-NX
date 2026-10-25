/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#include "hosted_request_ids.h"

#include <stddef.h>
#include <stdint.h>

#define REQUEST_ID_MAX (UINT32_MAX - UINT32_C(256))

static int range_is_valid(const void *pointer, size_t size)
{
    uintptr_t start, length;

    if (size == 0u)
        return 1;
    if (!pointer)
        return 0;
    start = (uintptr_t)pointer;
    length = (uintptr_t)size;
    if ((size_t)length != size || length > UINTPTR_MAX - start)
        return 0;
    return 1;
}

/* Invalid ranges fail closed as overlapping. */
static int ranges_overlap(const void *left, size_t left_size,
                          const void *right, size_t right_size)
{
    uintptr_t left_start, right_start, left_length, right_length;

    if (left_size == 0u || right_size == 0u)
        return 0;
    if (!range_is_valid(left, left_size) ||
        !range_is_valid(right, right_size))
        return 1;
    left_start = (uintptr_t)left;
    right_start = (uintptr_t)right;
    left_length = (uintptr_t)left_size;
    right_length = (uintptr_t)right_size;
    return left_start < right_start + right_length &&
           right_start < left_start + left_length;
}

static int request_id_is_valid(uint32_t request_id)
{
    return request_id != 0u && request_id <= REQUEST_ID_MAX;
}

static int ledger_is_zero(const struct p4_hosted_request_ids *ids)
{
    return ids->initialized == 0u && ids->retired == 0u &&
           ids->generation == 0u && ids->next_uid == 0u &&
           ids->active_uid == 0u && ids->active_request_id == 0u &&
           ids->exhausted == 0u;
}

static int ledger_is_consistent(const struct p4_hosted_request_ids *ids)
{
    if (ids->initialized != 1u || ids->generation == 0u ||
        ids->next_uid == 0u || ids->retired > 1u || ids->exhausted > 1u)
        return 0;
    if (ids->exhausted != 0u && ids->next_uid != UINT32_MAX)
        return 0;
    if ((ids->active_uid == 0u) != (ids->active_request_id == 0u))
        return 0;
    if (ids->active_request_id != 0u &&
        !request_id_is_valid(ids->active_request_id))
        return 0;
    if (ids->retired != 0u &&
        (ids->active_uid != 0u || ids->active_request_id != 0u))
        return 0;
    return 1;
}

int p4_hosted_request_ids_init(struct p4_hosted_request_ids *ids,
                               uint32_t generation)
{
    if (!ids || generation == 0u ||
        !range_is_valid(ids, sizeof *ids))
        return 0;

    if (ledger_is_zero(ids)) {
        ids->initialized = 1u;
        ids->retired = 0u;
        ids->generation = generation;
        ids->next_uid = 1u;
        ids->active_uid = 0u;
        ids->active_request_id = 0u;
        ids->exhausted = 0u;
        return 1;
    }

    if (!ledger_is_consistent(ids) || ids->retired == 0u ||
        ids->active_uid != 0u || ids->active_request_id != 0u ||
        ids->generation == generation)
        return 0;

    /* Keep the monotonic peer identity history across host epochs. */
    ids->generation = generation;
    ids->retired = 0u;
    return 1;
}

int p4_hosted_request_ids_reserve(struct p4_hosted_request_ids *ids,
                                  uint32_t generation,
                                  uint32_t request_id, uint32_t *uid)
{
    uint32_t reserved_uid;

    if (!ids || !uid || generation == 0u ||
        !request_id_is_valid(request_id) ||
        !range_is_valid(ids, sizeof *ids) ||
        !range_is_valid(uid, sizeof *uid) ||
        ranges_overlap(ids, sizeof *ids, uid, sizeof *uid))
        return 0;

    if (!ledger_is_consistent(ids) || ids->retired != 0u ||
        ids->generation != generation || ids->active_uid != 0u ||
        ids->active_request_id != 0u || ids->exhausted != 0u)
        return 0;

    reserved_uid = ids->next_uid;
    if (reserved_uid == UINT32_MAX) {
        ids->exhausted = 1u;
    } else {
        ids->next_uid = reserved_uid + 1u;
    }
    ids->active_uid = reserved_uid;
    ids->active_request_id = request_id;
    *uid = reserved_uid;
    return 1;
}

int p4_hosted_request_ids_finish(struct p4_hosted_request_ids *ids,
                                 uint32_t generation, uint32_t uid,
                                 uint32_t request_id)
{
    if (!ids || generation == 0u || uid == 0u ||
        !request_id_is_valid(request_id) ||
        !range_is_valid(ids, sizeof *ids) ||
        !ledger_is_consistent(ids) || ids->retired != 0u ||
        ids->generation != generation || ids->active_uid != uid ||
        ids->active_request_id != request_id)
        return 0;

    ids->active_uid = 0u;
    ids->active_request_id = 0u;
    return 1;
}

int p4_hosted_request_ids_invalidate(struct p4_hosted_request_ids *ids,
                                     uint32_t generation)
{
    if (!ids || generation == 0u || !range_is_valid(ids, sizeof *ids) ||
        !ledger_is_consistent(ids) || ids->retired != 0u ||
        ids->generation != generation)
        return 0;

    ids->active_uid = 0u;
    ids->active_request_id = 0u;
    ids->retired = 1u;
    return 1;
}
