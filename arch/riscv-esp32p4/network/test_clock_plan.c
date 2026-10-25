/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Host-side contract tests for the freestanding SDMMC clock planner.
*/
#include "../sdmmc/clock_plan.h"

#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define LEGACY_SD_MAX_HZ UINT32_C(25000000)

static uint64_t assertions;
static uint64_t no_peer_cases;
static uint64_t peer_cases;
static uint64_t legacy_oracle_cases;
static uint64_t fractional_boundary_corrections;
static uint64_t refused_cases;

static void fail_check(const char *expression, int line)
{
    (void)fprintf(stderr, "FAIL test_clock_plan.c:%d: %s\n", line,
                  expression);
    exit(EXIT_FAILURE);
}

static void check(int condition, const char *expression, int line)
{
    ++assertions;
    if (!condition)
        fail_check(expression, line);
}

#define CHECK(expression) \
    check((expression) != 0, #expression, __LINE__)

static uint32_t clamp_request(uint32_t requested)
{
    if (requested < P4SDMMC_CLOCK_MIN_HZ)
        return P4SDMMC_CLOCK_MIN_HZ;
    if (requested > P4SDMMC_CLOCK_MAX_HZ)
        return P4SDMMC_CLOCK_MAX_HZ;
    return requested;
}

static uint32_t ceiling_division(uint32_t numerator, uint32_t denominator)
{
    uint32_t quotient = numerator / denominator;

    if (numerator % denominator != 0U)
        ++quotient;
    return quotient;
}

/* Existing SD backend mapping, including its integer-truncated comparisons.
   It is retained as a comparison oracle through the current 25 MHz limit. */
static void old_integer_clock_oracle(
    uint32_t requested, struct p4sdmmc_clock_plan *expected)
{
    uint32_t host_div;
    uint32_t card_div;

    if (requested < P4SDMMC_CLOCK_MIN_HZ)
        requested = P4SDMMC_CLOCK_MIN_HZ;
    if (requested == P4SDMMC_CLOCK_MIN_HZ)
    {
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
            if (P4SDMMC_CLOCK_SOURCE_HZ / host_div > requested)
                ++host_div;
            card_div = 0U;
        }
        else
        {
            uint32_t host_hz = P4SDMMC_CLOCK_SOURCE_HZ / 2U;

            host_div = 2U;
            card_div = host_hz / (2U * requested);
            if (card_div < 1U)
                card_div = 1U;
            if (host_hz / (2U * card_div) > requested)
                ++card_div;
            if (card_div > 255U)
                card_div = 255U;
        }
    }

    expected->host_div = (unsigned)host_div;
    expected->card_div = (unsigned)card_div;
    expected->actual_hz = P4SDMMC_CLOCK_SOURCE_HZ / host_div;
    if (card_div != 0U)
        expected->actual_hz /= 2U * card_div;
}

/* The old mapping with exact comparisons: only fractional-Hz overspeed
   boundaries should differ from the backend's integer-truncated decisions. */
static void safe_clock_oracle(
    uint32_t requested, struct p4sdmmc_clock_plan *expected)
{
    uint32_t host_div;
    uint32_t card_div;

    if (requested < P4SDMMC_CLOCK_MIN_HZ)
        requested = P4SDMMC_CLOCK_MIN_HZ;
    if (requested > P4SDMMC_CLOCK_MAX_HZ)
        requested = P4SDMMC_CLOCK_MAX_HZ;

    if (requested == P4SDMMC_CLOCK_MIN_HZ)
    {
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
            if (ceiling_division(P4SDMMC_CLOCK_SOURCE_HZ, requested) >
                host_div)
                ++host_div;
            card_div = 0U;
        }
        else
        {
            uint32_t host_hz = P4SDMMC_CLOCK_SOURCE_HZ / 2U;
            uint32_t required_divisor = ceiling_division(
                P4SDMMC_CLOCK_SOURCE_HZ, requested);

            host_div = 2U;
            card_div = host_hz / (2U * requested);
            if (card_div < 1U)
                card_div = 1U;
            if (required_divisor > 2U * host_div * card_div)
                ++card_div;
            if (card_div > 255U)
                card_div = 255U;
        }
    }

    expected->host_div = (unsigned)host_div;
    expected->card_div = (unsigned)card_div;
    expected->actual_hz = P4SDMMC_CLOCK_SOURCE_HZ / host_div;
    if (card_div != 0U)
        expected->actual_hz /= 2U * card_div;
}

static void check_no_peer_legacy(uint32_t requested)
{
    struct p4sdmmc_clock_plan actual;
    struct p4sdmmc_clock_plan old_expected;
    struct p4sdmmc_clock_plan expected;
    uint32_t effective_request = clamp_request(requested);
    uint32_t card_factor;
    uint64_t source = (uint64_t)P4SDMMC_CLOCK_SOURCE_HZ;
    uint64_t selected_divisor;

    CHECK(requested <= LEGACY_SD_MAX_HZ);
    old_integer_clock_oracle(requested, &old_expected);
    safe_clock_oracle(requested, &expected);
    CHECK(p4sdmmc_clock_plan(requested, 0U, 0, &actual) == 1);
    CHECK(actual.host_div == expected.host_div);
    CHECK(actual.card_div == expected.card_div);
    CHECK(actual.actual_hz == expected.actual_hz);
    card_factor = actual.card_div == 0U ? 1U : 2U * actual.card_div;
    selected_divisor = (uint64_t)actual.host_div * (uint64_t)card_factor;
    CHECK(source <= (uint64_t)effective_request * selected_divisor);

    if (old_expected.host_div != actual.host_div ||
        old_expected.card_div != actual.card_div)
    {
        uint32_t old_card_factor = old_expected.card_div == 0U ?
            1U : 2U * (uint32_t)old_expected.card_div;
        uint64_t old_divisor = (uint64_t)old_expected.host_div *
                               (uint64_t)old_card_factor;
        uint64_t old_limit = (uint64_t)effective_request * old_divisor;

        CHECK(old_expected.actual_hz <= effective_request);
        CHECK(source > old_limit);
        CHECK(source - old_limit < old_divisor);
        ++fractional_boundary_corrections;
    }
    else
    {
        CHECK(old_expected.actual_hz == actual.actual_hz);
    }
    ++no_peer_cases;
    ++legacy_oracle_cases;
}

static void check_no_peer_edges(uint32_t boundary)
{
    if (boundary >= P4SDMMC_CLOCK_MIN_HZ + 2U &&
        boundary - 2U <= LEGACY_SD_MAX_HZ)
        check_no_peer_legacy(boundary - 2U);
    if (boundary >= P4SDMMC_CLOCK_MIN_HZ + 1U &&
        boundary - 1U <= LEGACY_SD_MAX_HZ)
        check_no_peer_legacy(boundary - 1U);
    if (boundary >= P4SDMMC_CLOCK_MIN_HZ &&
        boundary <= LEGACY_SD_MAX_HZ)
        check_no_peer_legacy(boundary);
    if (boundary < LEGACY_SD_MAX_HZ &&
        boundary + 1U >= P4SDMMC_CLOCK_MIN_HZ)
        check_no_peer_legacy(boundary + 1U);
    if (boundary < LEGACY_SD_MAX_HZ - 1U &&
        boundary + 2U >= P4SDMMC_CLOCK_MIN_HZ)
        check_no_peer_legacy(boundary + 2U);
}

static void check_peer(uint32_t requested, unsigned current_host_div)
{
    struct p4sdmmc_clock_plan actual;
    uint32_t effective_request = clamp_request(requested);
    uint32_t card_factor;
    uint32_t expected_actual;
    uint64_t source = (uint64_t)P4SDMMC_CLOCK_SOURCE_HZ;
    uint64_t requested_host = (uint64_t)effective_request *
                              (uint64_t)current_host_div;

    CHECK(p4sdmmc_clock_plan(requested, current_host_div, 1, &actual) == 1);
    CHECK(actual.host_div == current_host_div);
    CHECK(actual.card_div <= 255U);

    card_factor = actual.card_div == 0U ? 1U : 2U * actual.card_div;
    expected_actual = P4SDMMC_CLOCK_SOURCE_HZ /
                      (uint32_t)current_host_div;
    expected_actual /= card_factor;
    CHECK(actual.actual_hz == expected_actual);

    /* Compare the exact rational clock, not its truncated integer report. */
    CHECK(source <= requested_host * (uint64_t)card_factor);

    /* The chosen card divider is the least one that meets the request. */
    if (actual.card_div == 0U)
    {
        CHECK(source <= requested_host);
    }
    else if (actual.card_div == 1U)
    {
        CHECK(source > requested_host);
    }
    else
    {
        uint64_t previous_factor = UINT64_C(2) *
                                   (uint64_t)current_host_div *
                                   (uint64_t)(actual.card_div - 1U);
        CHECK(source > (uint64_t)effective_request * previous_factor);
    }
    ++peer_cases;
}

static void check_generic_no_peer(uint32_t requested)
{
    struct p4sdmmc_clock_plan actual;
    uint32_t effective_request = clamp_request(requested);
    uint32_t card_factor;
    uint64_t divisor;

    CHECK(p4sdmmc_clock_plan(requested, UINT_MAX, 0, &actual) == 1);
    CHECK(actual.host_div >= 1U && actual.host_div <= 16U);
    CHECK(actual.card_div <= 255U);
    card_factor = actual.card_div == 0U ? 1U : 2U * actual.card_div;
    divisor = (uint64_t)actual.host_div * (uint64_t)card_factor;
    CHECK((uint64_t)P4SDMMC_CLOCK_SOURCE_HZ <=
          (uint64_t)effective_request * divisor);
    CHECK(actual.actual_hz ==
          P4SDMMC_CLOCK_SOURCE_HZ / (uint32_t)divisor);
    ++no_peer_cases;
}

static void expect_refusal(uint32_t requested, unsigned current_host_div,
                           int peer_active)
{
    struct p4sdmmc_clock_plan output;
    int result;

    output.host_div = 12345U;
    output.card_div = 54321U;
    output.actual_hz = UINT32_C(0xdecafbad);
    result = p4sdmmc_clock_plan(requested, current_host_div, peer_active,
                                &output);
    CHECK(result == 0);
    CHECK(output.host_div == 12345U);
    CHECK(output.card_div == 54321U);
    CHECK(output.actual_hz == UINT32_C(0xdecafbad));
    ++refused_cases;
}

static void test_legacy_range_and_thresholds(void)
{
    uint32_t requested;
    unsigned divider;

    for (requested = P4SDMMC_CLOCK_MIN_HZ;
         requested <= LEGACY_SD_MAX_HZ; requested += 97U)
        check_no_peer_legacy(requested);
    check_no_peer_legacy(LEGACY_SD_MAX_HZ);
    check_no_peer_legacy(0U);

    check_no_peer_edges(P4SDMMC_CLOCK_MIN_HZ);
    check_no_peer_edges(P4SDMMC_CLOCK_MIN_HZ + 1U);
    for (divider = 1U; divider <= 16U; ++divider)
        check_no_peer_edges(P4SDMMC_CLOCK_SOURCE_HZ / (uint32_t)divider);
    for (divider = 1U; divider <= 255U; ++divider)
        check_no_peer_edges(P4SDMMC_CLOCK_MAX_HZ / (uint32_t)divider);
}

static void test_generic_no_peer_upper_range(void)
{
    static const uint32_t requests[] = {
        UINT32_C(25000001), UINT32_C(30000000), UINT32_C(39999999),
        P4SDMMC_CLOCK_MAX_HZ
    };
    struct p4sdmmc_clock_plan actual;
    uint32_t requested;
    unsigned index;

    for (requested = LEGACY_SD_MAX_HZ + 1U;
         requested <= P4SDMMC_CLOCK_MAX_HZ; requested += 97U)
        check_generic_no_peer(requested);

    for (index = 0U; index < (unsigned)(sizeof(requests) / sizeof(requests[0]));
         ++index)
    {
        check_generic_no_peer(requests[index]);
        CHECK(p4sdmmc_clock_plan(requests[index], UINT_MAX, 0, &actual) == 1);
        if (requests[index] == P4SDMMC_CLOCK_MAX_HZ)
        {
            CHECK(actual.host_div == 4U);
            CHECK(actual.card_div == 0U);
            CHECK(actual.actual_hz == P4SDMMC_CLOCK_MAX_HZ);
        }
        else
        {
            CHECK(requests[index] > LEGACY_SD_MAX_HZ);
        }
    }

    for (index = 1U; index <= 16U; ++index)
    {
        uint32_t boundary = P4SDMMC_CLOCK_SOURCE_HZ / (uint32_t)index;

        if (boundary > LEGACY_SD_MAX_HZ &&
            boundary <= P4SDMMC_CLOCK_MAX_HZ)
        {
            if (boundary > LEGACY_SD_MAX_HZ + 2U)
                check_generic_no_peer(boundary - 2U);
            if (boundary > LEGACY_SD_MAX_HZ + 1U)
                check_generic_no_peer(boundary - 1U);
            check_generic_no_peer(boundary);
            if (boundary < P4SDMMC_CLOCK_MAX_HZ)
                check_generic_no_peer(boundary + 1U);
            if (boundary < P4SDMMC_CLOCK_MAX_HZ - 1U)
                check_generic_no_peer(boundary + 2U);
        }
    }
}

static void test_peer_range_and_thresholds(void)
{
    uint32_t requested;
    unsigned host_div;
    unsigned card_div;

    for (requested = P4SDMMC_CLOCK_MIN_HZ;
         requested < P4SDMMC_CLOCK_MAX_HZ; requested += 7919U)
    {
        for (host_div = 1U; host_div <= 16U; ++host_div)
            check_peer(requested, host_div);
    }
    for (host_div = 1U; host_div <= 16U; ++host_div)
    {
        check_peer(P4SDMMC_CLOCK_MIN_HZ, host_div);
        check_peer(P4SDMMC_CLOCK_MAX_HZ, host_div);
        check_peer(UINT32_MAX, host_div);

        for (card_div = 1U; card_div <= 255U; ++card_div)
        {
            uint32_t denominator = 2U * (uint32_t)host_div *
                                   (uint32_t)card_div;
            uint32_t boundary = ceiling_division(
                P4SDMMC_CLOCK_SOURCE_HZ, denominator);

            if (boundary > P4SDMMC_CLOCK_MIN_HZ)
                check_peer(boundary - 1U, host_div);
            if (boundary >= P4SDMMC_CLOCK_MIN_HZ &&
                boundary <= P4SDMMC_CLOCK_MAX_HZ)
                check_peer(boundary, host_div);
            if (boundary < P4SDMMC_CLOCK_MAX_HZ)
                check_peer(boundary + 1U, host_div);
        }
    }
}

static void test_clamps_and_refusals(void)
{
    struct p4sdmmc_clock_plan minimum;
    struct p4sdmmc_clock_plan zero_request;
    struct p4sdmmc_clock_plan maximum;
    struct p4sdmmc_clock_plan oversized_request;
    unsigned invalid_divider;

    CHECK(p4sdmmc_clock_plan(P4SDMMC_CLOCK_MIN_HZ, 1U, 0, &minimum) == 1);
    CHECK(p4sdmmc_clock_plan(0U, 0U, 0, &zero_request) == 1);
    CHECK(zero_request.host_div == minimum.host_div);
    CHECK(zero_request.card_div == minimum.card_div);
    CHECK(zero_request.actual_hz == minimum.actual_hz);
    CHECK(minimum.host_div == 10U && minimum.card_div == 20U);
    CHECK(minimum.actual_hz == P4SDMMC_CLOCK_MIN_HZ);

    CHECK(p4sdmmc_clock_plan(P4SDMMC_CLOCK_MAX_HZ, 1U, 0, &maximum) == 1);
    CHECK(p4sdmmc_clock_plan(UINT32_MAX, 1U, 0, &oversized_request) == 1);
    CHECK(oversized_request.host_div == maximum.host_div);
    CHECK(oversized_request.card_div == maximum.card_div);
    CHECK(oversized_request.actual_hz == maximum.actual_hz);

    expect_refusal(1000000U, 1U, -1);
    expect_refusal(1000000U, 1U, 2);
    expect_refusal(1000000U, 1U, INT_MIN);
    expect_refusal(1000000U, 1U, INT_MAX);

    expect_refusal(1000000U, 0U, 1);
    for (invalid_divider = 17U; invalid_divider < 20U; ++invalid_divider)
        expect_refusal(1000000U, invalid_divider, 1);
    expect_refusal(1000000U, UINT_MAX, 1);

    CHECK(p4sdmmc_clock_plan(1000000U, 1U, 0, 0) == 0);

    /* The divider input is ignored when there is no active peer. */
    CHECK(p4sdmmc_clock_plan(1000000U, UINT_MAX, 0, &oversized_request) == 1);
    CHECK(oversized_request.host_div == 2U);
    CHECK(oversized_request.card_div <= 255U);
}

int main(void)
{
    test_legacy_range_and_thresholds();
    test_generic_no_peer_upper_range();
    test_peer_range_and_thresholds();
    test_clamps_and_refusals();

    (void)printf("clock-plan tests: assertions=%" PRIu64
                 " no-peer=%" PRIu64 " peer=%" PRIu64
                 " legacy-oracle=%" PRIu64 " fractional-fixes=%" PRIu64
                 " refusals=%" PRIu64 "\n",
                 assertions, no_peer_cases, peer_cases, legacy_oracle_cases,
                 fractional_boundary_corrections, refused_cases);
    return EXIT_SUCCESS;
}
