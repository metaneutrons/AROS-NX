/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#include "hosted_fifo.h"
#include "sdio_commands.h"

/* Validate output/state separation before even generation checks can mutate
 * the state. In particular &state->pending must not be used as the output:
 * publishing a plan there otherwise aliases the transport's transaction. */
static int valid_plan_output(const struct p4_hosted_fifo *state,
                             const struct p4_hosted_fifo_plan *out)
{
    uintptr_t first, second;
    if (!state || !out)
        return 0;
    first = (uintptr_t)state;
    second = (uintptr_t)out;
    if (first > UINTPTR_MAX - sizeof *state ||
        second > UINTPTR_MAX - sizeof *out)
        return 0;
    return first + sizeof *state <= second ||
           second + sizeof *out <= first;
}

int p4_hosted_fifo_start(struct p4_hosted_fifo *s, uint32_t generation,
                         uint32_t pool_limit)
{
    struct p4_hosted_fifo fresh = {0};
    if (!s || pool_limit > P4_HOSTED_TX_MASK)
        return 0;
    fresh.generation = generation;
    fresh.pool_limit = pool_limit;
    fresh.initialized = 1;
    *s = fresh;
    return 1;
}

static int available(struct p4_hosted_fifo *s, uint32_t generation)
{
    if (!s || s->initialized != 1 || s->poisoned)
        return 0;
    if (s->generation != generation)
    {
        s->poisoned = 1;
        return 0;
    }
    return !s->busy;
}

int p4_hosted_fifo_set_pool(struct p4_hosted_fifo *s, uint32_t generation,
                            uint32_t pool_limit)
{
    if (!available(s, generation) || !pool_limit ||
        pool_limit > P4_HOSTED_TX_MASK || s->pool_limit || s->tx_sent)
        return 0;
    s->pool_limit = pool_limit;
    return 1;
}

static int prepare(struct p4_hosted_fifo *s, unsigned write, uint32_t bytes,
                    uint32_t credits, struct p4_hosted_fifo_plan *out)
{
    struct p4_hosted_fifo_plan plan;
    if (!bytes || bytes > P4_HOSTED_FIFO_BUS_MAX)
        return 0;
    plan.logical_bytes = bytes;
    plan.bus_bytes = (bytes + 511U) & ~UINT32_C(511);
    plan.credits = credits;
    if (!p4_sdio_cmd53(write, 1, 1, 1, P4_HOSTED_FIFO_END - bytes,
                       plan.bus_bytes / 512U, 512, &plan.argument))
        return 0;
    s->pending = plan;
    s->pending_write = write;
    s->busy = 1;
    *out = plan;
    return 1;
}

int p4_hosted_fifo_rx(struct p4_hosted_fifo *s, uint32_t generation,
                      uint32_t peer_total, struct p4_hosted_fifo_plan *out)
{
    uint32_t bytes;
    if (!valid_plan_output(s, out) || !available(s, generation))
        return 0;
    bytes = ((peer_total & P4_HOSTED_RX_MASK) - s->rx_consumed) &
             P4_HOSTED_RX_MASK;
    /* An oversized pending stream cannot safely be ignored or truncated:
     * it may be a reset/stale counter. Fail this session explicitly. */
    if (bytes > P4_HOSTED_FIFO_BUS_MAX)
    {
        s->poisoned = 1;
        return 0;
    }
    return prepare(s, 0, bytes, 0, out);
}

int p4_hosted_fifo_tx(struct p4_hosted_fifo *s, uint32_t generation,
                      uint32_t token, uint32_t wire_bytes,
                      struct p4_hosted_fifo_plan *out)
{
    uint32_t free_credits, needed;
    if (!valid_plan_output(s, out) || !available(s, generation) || !s->pool_limit || !wire_bytes ||
        wire_bytes > P4_HOSTED_FIFO_BUS_MAX)
        return 0;
    free_credits = (((token >> 16) & P4_HOSTED_TX_MASK) - s->tx_sent) &
                    P4_HOSTED_TX_MASK;
    if (free_credits > s->pool_limit)
    {
        s->poisoned = 1;
        return 0;
    }
    needed = (wire_bytes + 1535U) / 1536U;
    if (needed > free_credits)
        return 0;
    return prepare(s, 1, wire_bytes, needed, out);
}

int p4_hosted_fifo_finish(struct p4_hosted_fifo *s, uint32_t generation,
                          unsigned started, unsigned success,
                          uint32_t completed_bytes)
{
    if (!s || s->initialized != 1 || !s->busy || s->poisoned)
        return 0;
    s->busy = 0;
    if (s->generation != generation || started > 1 || success > 1 ||
        (success && (!started || completed_bytes != s->pending.bus_bytes)) ||
        (!success && (started || completed_bytes)))
    {
        s->poisoned = 1;
        return 0;
    }
    if (!success)
        return 0;
    if (s->pending_write)
        s->tx_sent = (s->tx_sent + s->pending.credits) & P4_HOSTED_TX_MASK;
    else
        s->rx_consumed = (s->rx_consumed + s->pending.logical_bytes) &
                          P4_HOSTED_RX_MASK;
    return 1;
}
