/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#include "update.h"

static int range_is_valid(const void *pointer, size_t size)
{
    uintptr_t start, length;

    if (size == 0u)
        return 1;
    if (!pointer)
        return 0;
    start = (uintptr_t)pointer;
    length = (uintptr_t)size;
    return (size_t)length == size && length <= UINTPTR_MAX - start;
}

/* Invalid ranges are considered overlapping so callers fail closed. */
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

static int context_is_valid(const void *context, size_t context_size)
{
    return context ? context_size != 0u &&
                         range_is_valid(context, context_size)
                   : context_size == 0u;
}

static int spans_are_disjoint(const struct p4_board_update_plan *plan,
                              const struct p4_board_update_result *result)
{
    if (ranges_overlap(plan, sizeof *plan, result, sizeof *result) ||
        ranges_overlap(plan, sizeof *plan,
                       plan->scratch, plan->scratch_size) ||
        ranges_overlap(result, sizeof *result,
                       plan->scratch, plan->scratch_size))
        return 0;

    if (ranges_overlap(plan->reader.context, plan->reader.context_size,
                       plan, sizeof *plan) ||
        ranges_overlap(plan->reader.context, plan->reader.context_size,
                       result, sizeof *result) ||
        ranges_overlap(plan->reader.context, plan->reader.context_size,
                       plan->scratch, plan->scratch_size) ||
        ranges_overlap(plan->ops.context, plan->ops.context_size,
                       plan, sizeof *plan) ||
        ranges_overlap(plan->ops.context, plan->ops.context_size,
                       result, sizeof *result) ||
        ranges_overlap(plan->ops.context, plan->ops.context_size,
                       plan->scratch, plan->scratch_size))
        return 0;
    return 1;
}

static uint32_t chunks_for_size(uint32_t image_size)
{
    return image_size / P4_BOARD_UPDATE_CHUNK_SIZE +
           (image_size % P4_BOARD_UPDATE_CHUNK_SIZE != 0u);
}

static int cancelled(const struct p4_board_update_ops *ops)
{
    /* A failed cancellation query is treated as a request to stop. */
    return ops->is_cancelled(ops->context) != 0;
}

static int plan_is_valid(const struct p4_board_update_plan *plan)
{
    return plan && plan->image_size != 0u &&
           plan->image_size <= P4_BOARD_UPDATE_MAX_IMAGE_SIZE &&
           chunks_for_size(plan->image_size) != 0u &&
           chunks_for_size(plan->image_size) <= P4_BOARD_UPDATE_MAX_CHUNKS &&
           plan->scratch &&
           plan->scratch_size >= P4_BOARD_UPDATE_CHUNK_SIZE &&
           range_is_valid(plan->scratch, plan->scratch_size) &&
           context_is_valid(plan->reader.context,
                            plan->reader.context_size) &&
           context_is_valid(plan->ops.context, plan->ops.context_size) &&
           plan->reader.read && plan->ops.acquire_maintenance &&
           plan->ops.finish_maintenance && plan->ops.revalidate_source &&
           plan->ops.is_cancelled && plan->ops.begin && plan->ops.write &&
           plan->ops.end;
}

static int gates_pass(const struct p4_board_update_gates *gates)
{
    return gates->manifest_accepted == 1u &&
           gates->inspection_passed == 1u &&
           gates->recovery_verified == 1u &&
           gates->target_compatible == 1u;
}

static void finish_lease(const struct p4_board_update_plan *plan,
                          struct p4_board_update_result *result)
{
    if (!result->lease_acquired || result->lease_release_called)
        return;

    if (!result->begin_attempted)
        result->finish_mode = P4_BOARD_UPDATE_FINISH_NO_BEGIN;
    else if (!result->end_attempted)
        result->finish_mode = P4_BOARD_UPDATE_FINISH_INCOMPLETE;
    else if (result->status == P4_BOARD_UPDATE_WAIT_REBOOT)
        result->finish_mode = P4_BOARD_UPDATE_FINISH_WAIT_REBOOT;
    else
        result->finish_mode = P4_BOARD_UPDATE_FINISH_COMMIT_UNKNOWN;

    /* Mark before invoking user code so even a re-entrant callback is once. */
    result->lease_release_called = 1u;
    result->lease_release_status =
        (int32_t)plan->ops.finish_maintenance(plan->ops.context,
                                             result->finish_mode);
}

static enum p4_board_update_status finish(
    const struct p4_board_update_plan *plan,
    struct p4_board_update_result *result,
    enum p4_board_update_status status)
{
    result->status = status;
    finish_lease(plan, result);
    return result->status;
}

static int read_exact(const struct p4_board_update_plan *plan,
                      struct p4_board_update_result *result,
                      uint32_t offset, size_t requested,
                      size_t *actual_size,
                      enum p4_board_update_status *failure_status)
{
    size_t actual = 0u;
    int read_status = plan->reader.read(plan->reader.context, offset,
                                        plan->scratch, requested, &actual);

    result->local_status = (int32_t)read_status;
    if (read_status != 0) {
        *failure_status = P4_BOARD_UPDATE_SOURCE_READ_FAILED;
        return 0;
    }
    if (actual > requested) {
        *failure_status = P4_BOARD_UPDATE_SOURCE_BAD_COUNT;
        return 0;
    }
    if (actual != requested) {
        *failure_status = P4_BOARD_UPDATE_SHORT_READ;
        return 0;
    }
    *actual_size = actual;
    return 1;
}

enum p4_board_update_status
p4_board_update_run(const struct p4_board_update_plan *plan,
                    struct p4_board_update_result *result)
{
    uint32_t chunk_index;
    enum p4_board_update_status status;

    if (!result || !range_is_valid(result, sizeof *result))
        return P4_BOARD_UPDATE_INVALID_INPUT;

    /* Refuse aliases before touching output: initialization would itself
     * corrupt the plan or scratch used to detect them. */
    if (!plan || !range_is_valid(plan, sizeof *plan) ||
        ranges_overlap(plan, sizeof *plan, result, sizeof *result) ||
        !range_is_valid(plan->scratch, plan->scratch_size) ||
        !context_is_valid(plan->reader.context,
                          plan->reader.context_size) ||
        !context_is_valid(plan->ops.context, plan->ops.context_size) ||
        !spans_are_disjoint(plan, result))
        return P4_BOARD_UPDATE_INVALID_INPUT;

    result->status = P4_BOARD_UPDATE_INVALID_INPUT;
    result->image_size = 0u;
    result->total_chunks = 0u;
    result->acknowledged_chunks = 0u;
    result->acknowledged_bytes = 0u;
    result->failed_chunk_index = P4_BOARD_UPDATE_NO_FAILED_CHUNK;
    result->callback_status = 0;
    result->local_status = 0;
    result->lease_release_status = 0;
    result->finish_mode = P4_BOARD_UPDATE_FINISH_NONE;
    result->lease_acquired = 0u;
    result->lease_release_called = 0u;
    result->begin_attempted = 0u;
    result->end_attempted = 0u;

    if (!plan_is_valid(plan))
        return result->status;

    result->image_size = plan->image_size;
    result->total_chunks = chunks_for_size(plan->image_size);
    if (!gates_pass(&plan->gates))
        return finish(plan, result, P4_BOARD_UPDATE_PREFLIGHT_REFUSED);

    if (cancelled(&plan->ops))
        return finish(plan, result, P4_BOARD_UPDATE_CANCELLED_BEFORE_BEGIN);

    result->local_status =
        (int32_t)plan->ops.acquire_maintenance(plan->ops.context);
    if (result->local_status != 0)
        return finish(plan, result, P4_BOARD_UPDATE_LEASE_FAILED);
    result->lease_acquired = 1u;

    if (cancelled(&plan->ops))
        return finish(plan, result, P4_BOARD_UPDATE_CANCELLED_BEFORE_BEGIN);

    result->local_status = (int32_t)plan->ops.revalidate_source(
        plan->ops.context, plan->image_size);
    if (result->local_status != 0)
        return finish(plan, result, P4_BOARD_UPDATE_SOURCE_CHANGED);

    /* BEGIN erases the inactive slot, so all gates and ownership checks are
     * complete before this one-shot request. */
    if (cancelled(&plan->ops))
        return finish(plan, result, P4_BOARD_UPDATE_CANCELLED_BEFORE_BEGIN);

    result->begin_attempted = 1u;
    result->callback_status = 0;
    {
        enum p4_board_update_call_result call_result =
            plan->ops.begin(plan->ops.context, plan->image_size,
                            &result->callback_status);
        if (call_result != P4_BOARD_UPDATE_CALL_ACKNOWLEDGED ||
            result->callback_status != 0) {
            status = call_result == P4_BOARD_UPDATE_CALL_REJECTED
                         ? P4_BOARD_UPDATE_BEGIN_FAILED
                         : P4_BOARD_UPDATE_BEGIN_UNKNOWN;
            return finish(plan, result, status);
        }
    }

    for (chunk_index = 0u; chunk_index < result->total_chunks;
         ++chunk_index) {
        uint32_t offset = chunk_index * P4_BOARD_UPDATE_CHUNK_SIZE;
        uint32_t remaining = plan->image_size - offset;
        size_t requested = remaining < P4_BOARD_UPDATE_CHUNK_SIZE
                               ? (size_t)remaining
                               : P4_BOARD_UPDATE_CHUNK_SIZE;
        size_t actual = 0u;
        enum p4_board_update_call_result call_result;

        if (cancelled(&plan->ops))
            return finish(plan, result,
                          P4_BOARD_UPDATE_CANCELLED_INCOMPLETE);

        if (!read_exact(plan, result, offset, requested, &actual, &status))
            return finish(plan, result, status);

        if (cancelled(&plan->ops))
            return finish(plan, result,
                          P4_BOARD_UPDATE_CANCELLED_INCOMPLETE);

        result->callback_status = 0;
        call_result = plan->ops.write(plan->ops.context, plan->scratch,
                                      actual, &result->callback_status);
        if (call_result != P4_BOARD_UPDATE_CALL_ACKNOWLEDGED ||
            result->callback_status != 0) {
            result->failed_chunk_index = chunk_index;
            status = call_result == P4_BOARD_UPDATE_CALL_REJECTED
                         ? P4_BOARD_UPDATE_WRITE_REJECTED
                         : P4_BOARD_UPDATE_WRITE_UNKNOWN;
            return finish(plan, result, status);
        }

        ++result->acknowledged_chunks;
        result->acknowledged_bytes += (uint32_t)actual;
    }

    if (cancelled(&plan->ops))
        return finish(plan, result, P4_BOARD_UPDATE_CANCELLED_INCOMPLETE);

    /* Probe exactly at the declared end. Any byte means the immutable source
     * no longer matches the preflighted image, and END must not be sent. */
    {
        size_t trailing_size = 0u;
        int read_status = plan->reader.read(plan->reader.context,
                                            plan->image_size, plan->scratch,
                                            1u, &trailing_size);
        result->local_status = (int32_t)read_status;
        if (read_status != 0)
            return finish(plan, result,
                          P4_BOARD_UPDATE_SOURCE_READ_FAILED);
        if (trailing_size > 1u)
            return finish(plan, result,
                          P4_BOARD_UPDATE_SOURCE_BAD_COUNT);
        if (trailing_size != 0u)
            return finish(plan, result, P4_BOARD_UPDATE_TRAILING_DATA);
    }

    if (cancelled(&plan->ops))
        return finish(plan, result, P4_BOARD_UPDATE_CANCELLED_INCOMPLETE);

    /* END may set the boot slot before returning an error. Once attempted,
     * neither a failure nor cancellation can be reported as a clean abort. */
    result->end_attempted = 1u;
    result->callback_status = 0;
    if (plan->ops.end(plan->ops.context, &result->callback_status) !=
            P4_BOARD_UPDATE_CALL_ACKNOWLEDGED ||
        result->callback_status != 0)
        return finish(plan, result, P4_BOARD_UPDATE_END_COMMIT_UNKNOWN);

    return finish(plan, result, P4_BOARD_UPDATE_WAIT_REBOOT);
}

int p4_board_update_confirm_postboot(struct p4_board_update_result *result,
                                     int target_identity_matches,
                                     int health_check_passed)
{
    if (!result || result->status != P4_BOARD_UPDATE_WAIT_REBOOT ||
        result->lease_acquired != 1u ||
        result->lease_release_called != 1u ||
        result->lease_release_status != 0 ||
        result->finish_mode != P4_BOARD_UPDATE_FINISH_WAIT_REBOOT ||
        result->begin_attempted != 1u || result->end_attempted != 1u ||
        result->image_size == 0u ||
        result->image_size > P4_BOARD_UPDATE_MAX_IMAGE_SIZE ||
        result->total_chunks != chunks_for_size(result->image_size) ||
        result->acknowledged_chunks != result->total_chunks ||
        result->acknowledged_bytes != result->image_size)
        return 0;

    if (target_identity_matches != 1 || health_check_passed != 1) {
        result->status = P4_BOARD_UPDATE_POSTBOOT_FAILED;
        return 0;
    }

    result->status = P4_BOARD_UPDATE_INSTALLED;
    return 1;
}
