/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
*/

#ifndef P4_SDMMC_CLOCK_PLAN_H
#define P4_SDMMC_CLOCK_PLAN_H

#include <stdint.h>

#define P4SDMMC_CLOCK_SOURCE_HZ UINT32_C(160000000)
#define P4SDMMC_CLOCK_MIN_HZ    UINT32_C(400000)
#define P4SDMMC_CLOCK_MAX_HZ    UINT32_C(40000000)

struct p4sdmmc_clock_plan
{
    unsigned host_div;
    unsigned card_div;
    uint32_t actual_hz;
};

int p4sdmmc_clock_plan(uint32_t requested, unsigned current_host_div,
                       int peer_active, struct p4sdmmc_clock_plan *out);

#endif
