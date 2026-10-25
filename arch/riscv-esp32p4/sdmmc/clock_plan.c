/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
*/

#include "clock_plan.h"

static uint32_t p4sdmmc_clamp_request(uint32_t requested)
{
    if (requested < P4SDMMC_CLOCK_MIN_HZ)
        return P4SDMMC_CLOCK_MIN_HZ;
    if (requested > P4SDMMC_CLOCK_MAX_HZ)
        return P4SDMMC_CLOCK_MAX_HZ;
    return requested;
}

static uint32_t p4sdmmc_ceil_div(uint32_t numerator, uint32_t denominator)
{
    uint32_t quotient = numerator / denominator;

    if (numerator % denominator != 0U)
        ++quotient;
    return quotient;
}

static void p4sdmmc_set_actual(struct p4sdmmc_clock_plan *plan)
{
    plan->actual_hz = P4SDMMC_CLOCK_SOURCE_HZ / (uint32_t)plan->host_div;
    if (plan->card_div != 0U)
        plan->actual_hz /= UINT32_C(2) * (uint32_t)plan->card_div;
}

static void p4sdmmc_plan_without_peer(
    uint32_t requested, struct p4sdmmc_clock_plan *plan)
{
    uint32_t host_div;
    uint32_t card_div;

    if (requested == P4SDMMC_CLOCK_MIN_HZ)
    {
        /* Retain the existing conservative identification setting. */
        host_div = 10U;
        card_div = 20U;
    }
    else
    {
        host_div = P4SDMMC_CLOCK_SOURCE_HZ / requested;
        if (host_div < 1U)
            host_div = 1U;

        if (host_div <= 15U)
        {
            /* Do not let integer-Hz truncation permit a fractional overspeed. */
            uint32_t required_divisor = p4sdmmc_ceil_div(
                P4SDMMC_CLOCK_SOURCE_HZ, requested);

            if (required_divisor > host_div)
                ++host_div;
            card_div = 0U;
        }
        else
        {
            uint32_t host_clock_hz;
            uint32_t twice_requested;
            uint32_t required_divisor;

            host_div = 2U;
            host_clock_hz = P4SDMMC_CLOCK_SOURCE_HZ / host_div;
            twice_requested = UINT32_C(2) * requested;
            card_div = host_clock_hz / twice_requested;
            if (card_div < 1U)
                card_div = 1U;
            required_divisor = p4sdmmc_ceil_div(
                P4SDMMC_CLOCK_SOURCE_HZ, requested);
            if (required_divisor >
                UINT32_C(2) * host_div * card_div)
                ++card_div;
            if (card_div > 255U)
                card_div = 255U;
        }
    }

    plan->host_div = (unsigned)host_div;
    plan->card_div = (unsigned)card_div;
    p4sdmmc_set_actual(plan);
}

static int p4sdmmc_plan_with_peer(
    uint32_t requested, unsigned current_host_div,
    struct p4sdmmc_clock_plan *plan)
{
    uint32_t required_divisor;
    uint32_t host_div;
    uint32_t card_div;

    host_div = (uint32_t)current_host_div;

    /* ceil(source/requested) makes the rational comparison exact without
       multiplying requested by the card divider. */
    required_divisor = p4sdmmc_ceil_div(P4SDMMC_CLOCK_SOURCE_HZ, requested);
    if (required_divisor <= host_div)
    {
        card_div = 0U;
    }
    else
    {
        uint32_t host_card_divisor = UINT32_C(2) * host_div;
        card_div = p4sdmmc_ceil_div(required_divisor, host_card_divisor);
        if (card_div < 1U)
            card_div = 1U;
        if (card_div > 255U)
            return 0;
    }

    plan->host_div = current_host_div;
    plan->card_div = (unsigned)card_div;
    p4sdmmc_set_actual(plan);
    return 1;
}

int p4sdmmc_clock_plan(uint32_t requested, unsigned current_host_div,
                       int peer_active, struct p4sdmmc_clock_plan *out)
{
    struct p4sdmmc_clock_plan plan;

    if (out == 0 || (peer_active != 0 && peer_active != 1))
        return 0;
    if (peer_active != 0 &&
        (current_host_div < 1U || current_host_div > 16U))
        return 0;

    requested = p4sdmmc_clamp_request(requested);
    if (peer_active == 0)
    {
        p4sdmmc_plan_without_peer(requested, &plan);
    }
    else if (!p4sdmmc_plan_with_peer(requested, current_host_div, &plan))
    {
        return 0;
    }

    *out = plan;
    return 1;
}
