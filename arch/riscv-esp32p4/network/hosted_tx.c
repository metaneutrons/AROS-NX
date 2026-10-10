/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#include "hosted_tx.h"

#include "hosted_wire.h"

struct tx_range {
    uintptr_t first;
    uintptr_t end;
};

static int make_range(const void *pointer, size_t size, struct tx_range *range)
{
    uintptr_t first;

    if (!pointer || size == 0 || !range)
        return 0;
    first = (uintptr_t)pointer;
    if ((uintmax_t)size > (uintmax_t)(UINTPTR_MAX - first))
        return 0;
    range->first = first;
    range->end = first + (uintptr_t)size;
    return 1;
}

static int disjoint(const struct tx_range *first,
                    const struct tx_range *second)
{
    return first->end <= second->first || second->end <= first->first;
}

static void clear_result(struct p4_hosted_tx_result *result)
{
    volatile unsigned char *bytes = (volatile unsigned char *)result;
    size_t index;

    for (index = 0; index < sizeof(*result); ++index)
        bytes[index] = 0;
}

static void copy_plan(struct p4_hosted_fifo_plan *destination,
                      const struct p4_hosted_fifo_plan *source)
{
    destination->argument = source->argument;
    destination->logical_bytes = source->logical_bytes;
    destination->bus_bytes = source->bus_bytes;
    destination->credits = source->credits;
}

static int valid_frame(const uint8_t *buffer, size_t wire_size,
                       unsigned checksum_enabled)
{
    struct p4_hosted_frame frame;

    if (!p4_hosted_decode(buffer, wire_size, checksum_enabled, &frame) ||
        frame.wire_size != wire_size || frame.interface_number != 0 ||
        frame.flags != 0 || frame.throttle != 0 || frame.packet_type != 0 ||
        frame.sequence != 0 || buffer[4] != P4_HOSTED_HEADER_SIZE ||
        buffer[5] != 0)
        return 0;

    return frame.interface_type == P4_HOSTED_STA ||
           frame.interface_type == P4_HOSTED_AP ||
           frame.interface_type == P4_HOSTED_SERIAL ||
           frame.interface_type == P4_HOSTED_PRIVATE;
}

static int preflight(struct p4_hosted_fifo *state, uint32_t generation,
                     const struct p4_hosted_tx_ops *ops,
                     uint8_t *buffer, size_t capacity, size_t wire_size,
                     unsigned checksum_enabled,
                     struct p4_hosted_tx_result *result,
                     uint32_t *bus_bytes)
{
    struct tx_range ranges[4];
    size_t rounded;
    unsigned first, second;

    if (!state || !ops || !buffer || !result || generation == 0 ||
        checksum_enabled > 1 ||
        wire_size < P4_HOSTED_HEADER_SIZE + 1u ||
        wire_size > P4_HOSTED_FRAME_MAX)
        return 0;
    rounded = (wire_size + 511u) & ~(size_t)511u;
    if (capacity < rounded || rounded > P4_HOSTED_FIFO_BUS_MAX ||
        !make_range(state, sizeof(*state), &ranges[0]) ||
        !make_range(ops, sizeof(*ops), &ranges[1]) ||
        !make_range(buffer, capacity, &ranges[2]) ||
        !make_range(result, sizeof(*result), &ranges[3]))
        return 0;

    for (first = 0; first < 4; ++first)
        for (second = first + 1; second < 4; ++second)
            if (!disjoint(&ranges[first], &ranges[second]))
                return 0;

    if (!ops->read_word || !ops->write_fifo || !ops->session_ok ||
        !ops->budget_ok || !ops->yield ||
        !valid_frame(buffer, wire_size, checksum_enabled))
        return 0;

    /* This is a persistent session object. The gate never initializes it or
     * repairs an unknown/invalid pool during controller reacquisition. */
    if (state->generation != generation || state->initialized != 1 ||
        state->busy != 0 || state->poisoned != 0 ||
        state->pool_limit == 0 || state->pool_limit > P4_HOSTED_TX_MASK)
        return 0;

    *bus_bytes = (uint32_t)rounded;
    return 1;
}

static void release_unstarted(struct p4_hosted_fifo *state,
                              uint32_t generation)
{
    if (state->busy == 1)
    {
        (void)p4_hosted_fifo_finish(state, generation, 0, 0, 0);
        if (state->busy != 0)
        {
            state->busy = 0;
            state->poisoned = 1;
        }
    }
}

static void poison_pending(struct p4_hosted_fifo *state,
                           uint32_t generation)
{
    if (state->busy == 1)
    {
        (void)p4_hosted_fifo_finish(state, generation, 1, 0, 0);
        if (state->busy != 0)
        {
            state->busy = 0;
            state->poisoned = 1;
        }
    }
    else
        state->poisoned = 1;
}

static int check_session(struct p4_hosted_fifo *state, uint32_t generation,
                         const struct p4_hosted_tx_ops *ops,
                         struct p4_hosted_tx_result *result)
{
    result->stage = P4_HOSTED_TX_STAGE_SESSION;
    if (ops->session_ok(ops->context, generation) == 1)
        return 1;

    poison_pending(state, generation);
    result->error = P4_HOSTED_TX_ERROR_SESSION_LOST;
    return 0;
}

static int check_budget(struct p4_hosted_tx_ops const *ops,
                        struct p4_hosted_tx_result *result)
{
    result->stage = P4_HOSTED_TX_STAGE_BUDGET;
    if (ops->budget_ok(ops->context) == 1)
        return 1;

    result->error = P4_HOSTED_TX_ERROR_BUDGET_EXPIRED;
    return 0;
}

static int check_before_read(struct p4_hosted_fifo *state,
                             uint32_t generation,
                             const struct p4_hosted_tx_ops *ops,
                             struct p4_hosted_tx_result *result)
{
    return check_session(state, generation, ops, result) &&
           check_budget(ops, result);
}

static int check_after_write(struct p4_hosted_fifo *state,
                             uint32_t generation,
                             const struct p4_hosted_tx_ops *ops,
                             struct p4_hosted_tx_result *result)
{
    if (!check_session(state, generation, ops, result))
        return 0;
    if (check_budget(ops, result))
        return 1;

    /* Once write_fifo has been entered, a late deadline cannot prove that no
     * bytes reached the slave. Even a reported full completion is not committed. */
    poison_pending(state, generation);
    return 0;
}

static int completion_well_formed(
    const struct p4_hosted_tx_completion *completion, uint32_t bus_bytes)
{
    if (completion->started > 1 || completion->success > 1)
        return 0;
    if (completion->success)
        return completion->started == 1 &&
               completion->completed_bytes == bus_bytes;
    if (!completion->started)
        return completion->completed_bytes == 0;
    return completion->completed_bytes <= bus_bytes;
}

int p4_hosted_tx_send(struct p4_hosted_fifo *state, uint32_t generation,
                      const struct p4_hosted_tx_ops *ops,
                      uint8_t *buffer, size_t capacity, size_t wire_size,
                      unsigned checksum_enabled,
                      struct p4_hosted_tx_result *result)
{
    struct p4_hosted_fifo_plan plan;
    struct p4_hosted_tx_completion completion;
    uint32_t bus_bytes, token;
    uint32_t poll;
    int read_result, write_result;
    volatile uint8_t *padding;
    uint32_t index;

    if (!preflight(state, generation, ops, buffer, capacity, wire_size,
                   checksum_enabled, result, &bus_bytes))
        return 0;

    clear_result(result);

    for (poll = 0; poll < P4_HOSTED_TX_MAX_POLLS; ++poll)
    {
        if (!check_before_read(state, generation, ops, result))
            return 0;

        token = 0;
        result->stage = P4_HOSTED_TX_STAGE_READ_TOKEN;
        ++result->polls;
        read_result = ops->read_word(ops->context,
                                     P4_HOSTED_TX_TOKEN_ADDRESS, &token);

        if (read_result == 1)
            result->token = token;
        if (!check_session(state, generation, ops, result))
            return 0;
        if (!check_budget(ops, result))
            return 0;
        if (read_result != 1)
        {
            result->stage = P4_HOSTED_TX_STAGE_READ_TOKEN;
            result->error = P4_HOSTED_TX_ERROR_TOKEN_READ;
            return 0;
        }
        result->token_valid = 1;

        result->stage = P4_HOSTED_TX_STAGE_PLAN;
        if (p4_hosted_fifo_tx(state, generation, token, (uint32_t)wire_size,
                              &plan))
        {
            copy_plan(&result->plan, &plan);
            result->plan_valid = 1;
            break;
        }

        if (state->poisoned)
        {
            result->error = P4_HOSTED_TX_ERROR_COUNTER_INVALID;
            return 0;
        }
        if (state->generation != generation || state->busy != 0 ||
            state->initialized != 1 || state->pool_limit == 0)
        {
            state->poisoned = 1;
            result->error = P4_HOSTED_TX_ERROR_PLAN_REFUSED;
            return 0;
        }

        if (poll + 1u == P4_HOSTED_TX_MAX_POLLS)
        {
            result->error = P4_HOSTED_TX_ERROR_CREDIT_EXHAUSTED;
            return 0;
        }

        result->stage = P4_HOSTED_TX_STAGE_YIELD;
        ops->yield(ops->context);
        if (!check_session(state, generation, ops, result))
            return 0;
        if (!check_budget(ops, result))
            return 0;
    }

    if (!result->plan_valid || result->plan.bus_bytes != bus_bytes ||
        result->plan.logical_bytes != wire_size ||
        capacity < result->plan.bus_bytes)
    {
        release_unstarted(state, generation);
        result->error = P4_HOSTED_TX_ERROR_PLAN_REFUSED;
        return 0;
    }

    /* The plan is now outstanding. This is the final check before padding
     * and write_fifo. There is no
     * callback or unbounded work between it, bounded padding, and the one
     * write attempt. */
    if (!check_session(state, generation, ops, result))
        return 0;
    if (!check_budget(ops, result))
    {
        release_unstarted(state, generation);
        return 0;
    }

    result->stage = P4_HOSTED_TX_STAGE_PADDING;
    padding = (volatile uint8_t *)buffer;
    for (index = (uint32_t)wire_size; index < bus_bytes; ++index)
        padding[index] = 0;

    completion.started = 0;
    completion.success = 0;
    completion.completed_bytes = 0;
    result->stage = P4_HOSTED_TX_STAGE_WRITE_FIFO;
    result->write_called = 1;
    write_result = ops->write_fifo(ops->context, &plan, buffer, &completion);
    result->write_callback_result = write_result;
    result->completion.started = completion.started;
    result->completion.success = completion.success;
    result->completion.completed_bytes = completion.completed_bytes;
    result->completion_reported = write_result == 1;
    result->completion_well_formed = write_result == 1 &&
        completion_well_formed(&completion, bus_bytes);

    if (!check_after_write(state, generation, ops, result))
        return 0;

    result->stage = P4_HOSTED_TX_STAGE_FINISH;
    if (write_result != 1)
    {
        poison_pending(state, generation);
        result->error = P4_HOSTED_TX_ERROR_WRITE_AMBIGUOUS;
        return 0;
    }

    if (!result->completion_well_formed)
    {
        (void)p4_hosted_fifo_finish(state, generation, completion.started,
                                    completion.success,
                                    completion.completed_bytes);
        if (state->busy != 0)
        {
            state->busy = 0;
            state->poisoned = 1;
        }
        result->error = P4_HOSTED_TX_ERROR_COMPLETION_MALFORMED;
        return 0;
    }

    if (!completion.success)
    {
        (void)p4_hosted_fifo_finish(state, generation, completion.started,
                                    completion.success,
                                    completion.completed_bytes);
        if (state->busy != 0)
        {
            state->busy = 0;
            state->poisoned = 1;
        }
        result->error = P4_HOSTED_TX_ERROR_WRITE_FAILED;
        return 0;
    }

    if (!p4_hosted_fifo_finish(state, generation, completion.started,
                               completion.success,
                               completion.completed_bytes))
    {
        if (state->busy != 0)
        {
            state->busy = 0;
            state->poisoned = 1;
        }
        result->error = P4_HOSTED_TX_ERROR_FINISH_FAILED;
        return 0;
    }

    result->stage = P4_HOSTED_TX_STAGE_COMPLETE;
    result->success = 1;
    return 1;
}
