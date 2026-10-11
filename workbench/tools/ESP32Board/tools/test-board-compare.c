/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../compare_file.h"
#include "../source_file.h"

enum event_kind {
    EVENT_LOAD = 1,
    EVENT_DISCOVER,
    EVENT_RECHECK,
    EVENT_COMPARE,
    EVENT_DISPOSE
};

struct test_state {
    int load_result;
    int discover_result;
    int recheck_result;
    int compare_result;
    int compare_writes_result;
    struct board_c6_target_report compare_value;
    struct boardtool_c6_source *source;
    struct board_client_observation *observation;
    struct board_c6_target_report *caller_report;
    uint8_t *owned_bytes;
    int load_calls;
    int discover_calls;
    int recheck_calls;
    int compare_calls;
    int dispose_calls;
    int allocations;
    int frees;
    int active_allocations;
    int lifetime_errors;
    int events[8];
    size_t event_count;
};

static struct test_state state;
static unsigned long checks;
static unsigned long failures;
static unsigned long cases;
static const char *case_name = "startup";

#define CHECK(expression) do { \
    ++checks; \
    if (!(expression)) { \
        ++failures; \
        fprintf(stderr, "FAIL [%s] line %d: %s\n", case_name, __LINE__, \
            #expression); \
    } \
} while (0)

static void begin_case(const char *name)
{
    case_name = name;
    ++cases;
}

static void record_event(enum event_kind event)
{
    if (state.event_count >= sizeof(state.events) / sizeof(state.events[0])) {
        ++state.lifetime_errors;
        return;
    }
    state.events[state.event_count++] = (int)event;
}

static void reset_state(void)
{
    memset(&state, 0, sizeof(state));
    state.load_result = 1;
    state.discover_result = 1;
    state.recheck_result = 1;
    state.compare_result = 1;
    state.compare_writes_result = 1;
    state.compare_value.rejected = 0x13579bdfu;
    state.compare_value.unknown = 0x2468ace0u;
    state.compare_value.risk = 0x0badc0deu;
}

static int source_is_zero(const struct boardtool_c6_source *source)
{
    const unsigned char *bytes = (const unsigned char *)source;
    size_t i;
    for (i = 0; i < sizeof(*source); ++i)
        if (bytes[i] != 0) return 0;
    return 1;
}

static int report_equals(const struct board_c6_target_report *left,
    const struct board_c6_target_report *right)
{
    return left->rejected == right->rejected &&
        left->unknown == right->unknown && left->risk == right->risk;
}

static void expect_report(const struct board_c6_target_report *actual,
    uint32_t rejected, uint32_t unknown, uint32_t risk)
{
    CHECK(actual->rejected == rejected);
    CHECK(actual->unknown == unknown);
    CHECK(actual->risk == risk);
}

static void expect_events(const int *expected, size_t count)
{
    size_t i;
    CHECK(state.event_count == count);
    if (state.event_count != count) return;
    for (i = 0; i < count; ++i)
        CHECK(state.events[i] == expected[i]);
}

static void expect_clean(int allocations, int frees, int active,
    int disposals)
{
    CHECK(state.allocations == allocations);
    CHECK(state.frees == frees);
    CHECK(state.active_allocations == active);
    CHECK(state.dispose_calls == disposals);
    CHECK(state.lifetime_errors == 0);
}

int boardtool_c6_source_load(const char *path, const char *sha256,
    struct boardtool_c6_source *source, char *status, size_t capacity)
{
    static const uint8_t source_bytes[] = {0xc6, 0x01, 0x02, 0x03};

    ++state.load_calls;
    record_event(EVENT_LOAD);
    CHECK(source != NULL);
    CHECK(status != NULL);
    CHECK(capacity != 0);
    if (!source || !status || !capacity) return 0;
    CHECK(source_is_zero(source));
    if (!state.load_result || !path || !sha256) {
        CHECK(source_is_zero(source));
        if (path && sha256)
            snprintf(status, capacity, "mock invalid source; no write");
        return 0;
    }

    state.owned_bytes = (uint8_t *)malloc(sizeof(source_bytes));
    CHECK(state.owned_bytes != NULL);
    if (!state.owned_bytes) return 0;
    memcpy(state.owned_bytes, source_bytes, sizeof(source_bytes));
    ++state.allocations;
    ++state.active_allocations;

    source->image = state.owned_bytes;
    source->size = sizeof(source_bytes);
    source->info.bytes = 512;
    source->info.secure_version = 9;
    source->info.min_revision = 2;
    source->info.max_revision = 7;
    memcpy(source->expected_sha256,
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
        sizeof(source->expected_sha256));
    state.source = source;
    snprintf(status, capacity, "mock source accepted");
    return 1;
}

int boardtool_c6_source_recheck(const struct boardtool_c6_source *source)
{
    ++state.recheck_calls;
    record_event(EVENT_RECHECK);
    CHECK(source != NULL);
    CHECK(source == state.source);
    CHECK(state.discover_calls == 1);
    CHECK(state.load_calls == 1);
    CHECK(state.active_allocations == 1);
    CHECK(source && source->image == state.owned_bytes);
    CHECK(source && source->size != 0);
    if (!source || source != state.source ||
        state.active_allocations != 1 || source->image != state.owned_bytes)
        ++state.lifetime_errors;
    return state.recheck_result;
}

void boardtool_c6_source_dispose(struct boardtool_c6_source *source)
{
    uint8_t *bytes = NULL;

    ++state.dispose_calls;
    record_event(EVENT_DISPOSE);
    CHECK(source != NULL);
    CHECK(source == state.source);
    if (source) {
        CHECK(source->image == state.owned_bytes);
        bytes = source->image;
        memset(source, 0, sizeof(*source));
    }
    if (!bytes || bytes != state.owned_bytes ||
        state.active_allocations != 1) {
        ++state.lifetime_errors;
    } else {
        --state.active_allocations;
        ++state.frees;
        free(bytes);
        state.owned_bytes = NULL;
    }
}

int boardtool_discover_c6(struct board_client_observation *observation,
    char *status, size_t capacity)
{
    ++state.discover_calls;
    record_event(EVENT_DISCOVER);
    CHECK(observation != NULL);
    CHECK(status != NULL);
    CHECK(capacity != 0);
    CHECK(state.load_calls == 1);
    CHECK(state.active_allocations == 1);
    CHECK(state.source != NULL && state.source->image == state.owned_bytes);
    if (!observation || !status || !capacity || state.active_allocations != 1 ||
        !state.source || state.source->image != state.owned_bytes)
        ++state.lifetime_errors;
    state.observation = observation;
    if (!state.discover_result) return 0;
    memset(observation, 0, sizeof(*observation));
    observation->token = 0x10203040u;
    observation->generation = 0x50607080u;
    return 1;
}

int board_c6_target_compare(const struct board_image_info *image,
    const struct board_discovery_snapshot *target,
    struct board_c6_target_report *report)
{
    ++state.compare_calls;
    record_event(EVENT_COMPARE);
    CHECK(image != NULL);
    CHECK(target != NULL);
    CHECK(report != NULL);
    CHECK(state.recheck_calls == 1);
    CHECK(state.load_calls == 1);
    CHECK(state.active_allocations == 1);
    CHECK(state.source != NULL && image == &state.source->info);
    CHECK(state.observation != NULL && target == &state.observation->snapshot);
    CHECK(state.observation && state.observation->token == 0x10203040u);
    CHECK(report != state.caller_report);
    if (!image || !target || !report || state.active_allocations != 1 ||
        !state.source || image != &state.source->info ||
        !state.observation || target != &state.observation->snapshot)
        ++state.lifetime_errors;
    if (state.compare_writes_result) *report = state.compare_value;
    return state.compare_result;
}

static void prepare_report(struct board_c6_target_report *report)
{
    report->rejected = 0xa1a2a3a4u;
    report->unknown = 0xb1b2b3b4u;
    report->risk = 0xc1c2c3c4u;
}

static void test_invalid_source(void)
{
    struct board_c6_target_report report, before;
    struct board_client_observation observation;
    char status[160];
    static const int events[] = {EVENT_LOAD};

    begin_case("invalid source");
    reset_state();
    state.load_result = 0;
    prepare_report(&report);
    before = report;
    memset(&observation, 0x5a, sizeof(observation));
    memset(status, 0xa5, sizeof(status));

    CHECK(boardtool_compare_c6("firmware.bin", "0123456789abcdef"
        "0123456789abcdef0123456789abcdef0123456789abcdef",
        &observation, &report, status, sizeof(status)) == 0);
    CHECK(state.load_calls == 1);
    CHECK(state.discover_calls == 0);
    CHECK(state.recheck_calls == 0);
    CHECK(state.compare_calls == 0);
    CHECK(report_equals(&report, &before));
    expect_clean(0, 0, 0, 0);
    expect_events(events, sizeof(events) / sizeof(events[0]));
}

static void test_discovery_failure(void)
{
    struct board_c6_target_report report, before;
    struct board_client_observation observation;
    char status[160];
    static const int events[] = {EVENT_LOAD, EVENT_DISCOVER, EVENT_DISPOSE};

    begin_case("discovery failure");
    reset_state();
    state.discover_result = 0;
    prepare_report(&report);
    before = report;
    memset(&observation, 0x5a, sizeof(observation));
    memset(status, 0, sizeof(status));
    state.caller_report = &report;

    CHECK(boardtool_compare_c6("firmware.bin", "0123456789abcdef"
        "0123456789abcdef0123456789abcdef0123456789abcdef",
        &observation, &report, status, sizeof(status)) == 0);
    CHECK(state.load_calls == 1);
    CHECK(state.discover_calls == 1);
    CHECK(state.recheck_calls == 0);
    CHECK(state.compare_calls == 0);
    CHECK(report_equals(&report, &before));
    expect_clean(1, 1, 0, 1);
    expect_events(events, sizeof(events) / sizeof(events[0]));
}

static void test_recheck_failure(void)
{
    struct board_c6_target_report report, before;
    struct board_client_observation observation;
    char status[160];
    static const int events[] = {
        EVENT_LOAD, EVENT_DISCOVER, EVENT_RECHECK, EVENT_DISPOSE
    };

    begin_case("source recheck failure");
    reset_state();
    state.recheck_result = 0;
    prepare_report(&report);
    before = report;
    memset(&observation, 0x5a, sizeof(observation));
    memset(status, 0, sizeof(status));
    state.caller_report = &report;

    CHECK(boardtool_compare_c6("firmware.bin", "0123456789abcdef"
        "0123456789abcdef0123456789abcdef0123456789abcdef",
        &observation, &report, status, sizeof(status)) == 0);
    CHECK(state.load_calls == 1);
    CHECK(state.discover_calls == 1);
    CHECK(state.recheck_calls == 1);
    CHECK(state.compare_calls == 0);
    CHECK(report_equals(&report, &before));
    CHECK(strstr(status, "comparison refused") != NULL);
    expect_clean(1, 1, 0, 1);
    expect_events(events, sizeof(events) / sizeof(events[0]));
}

static void test_comparison_failure(void)
{
    struct board_c6_target_report report, before;
    struct board_client_observation observation;
    char status[160];
    static const int events[] = {
        EVENT_LOAD, EVENT_DISCOVER, EVENT_RECHECK, EVENT_COMPARE, EVENT_DISPOSE
    };

    begin_case("target comparison failure");
    reset_state();
    state.compare_result = 0;
    prepare_report(&report);
    before = report;
    memset(&observation, 0x5a, sizeof(observation));
    memset(status, 0, sizeof(status));
    state.caller_report = &report;

    CHECK(boardtool_compare_c6("firmware.bin", "0123456789abcdef"
        "0123456789abcdef0123456789abcdef0123456789abcdef",
        &observation, &report, status, sizeof(status)) == 0);
    CHECK(state.load_calls == 1);
    CHECK(state.discover_calls == 1);
    CHECK(state.recheck_calls == 1);
    CHECK(state.compare_calls == 1);
    CHECK(report_equals(&report, &before));
    CHECK(strstr(status, "comparison refused") != NULL);
    expect_clean(1, 1, 0, 1);
    expect_events(events, sizeof(events) / sizeof(events[0]));
}

static void test_report_success_and_lifetime(void)
{
    struct board_c6_target_report report;
    struct board_client_observation observation;
    char status[320];
    static const int events[] = {
        EVENT_LOAD, EVENT_DISCOVER, EVENT_RECHECK, EVENT_COMPARE, EVENT_DISPOSE
    };

    begin_case("report success and source lifetime");
    reset_state();
    prepare_report(&report);
    memset(&observation, 0x5a, sizeof(observation));
    memset(status, 0, sizeof(status));
    state.caller_report = &report;

    CHECK(boardtool_compare_c6("firmware.bin", "0123456789abcdef"
        "0123456789abcdef0123456789abcdef0123456789abcdef",
        &observation, &report, status, sizeof(status)) == 1);
    expect_report(&report, 0x13579bdfu, 0x2468ace0u, 0x0badc0deu);
    CHECK(strstr(status, "rejected=13579bdf unknown=2468ace0 risk=badc0de") != NULL);
    CHECK(state.load_calls == 1);
    CHECK(state.discover_calls == 1);
    CHECK(state.recheck_calls == 1);
    CHECK(state.compare_calls == 1);
    CHECK(state.dispose_calls == 1);
    CHECK(state.observation == &observation);
    CHECK(observation.token == 0x10203040u);
    CHECK(observation.generation == 0x50607080u);
    expect_clean(1, 1, 0, 1);
    expect_events(events, sizeof(events) / sizeof(events[0]));
}

static void test_null_arguments(void)
{
    struct board_c6_target_report report, before;
    struct board_client_observation observation;
    char status[160];

    begin_case("null path");
    reset_state();
    prepare_report(&report);
    before = report;
    memset(&observation, 0x5a, sizeof(observation));
    CHECK(boardtool_compare_c6(NULL, "0123456789abcdef"
        "0123456789abcdef0123456789abcdef0123456789abcdef",
        &observation, &report, status, sizeof(status)) == 0);
    CHECK(state.load_calls == 1 && state.discover_calls == 0);
    CHECK(report_equals(&report, &before));
    expect_clean(0, 0, 0, 0);
    expect_events((const int[]){EVENT_LOAD}, 1);

    begin_case("null digest");
    reset_state();
    CHECK(boardtool_compare_c6("firmware.bin", NULL, &observation,
        &report, status, sizeof(status)) == 0);
    CHECK(state.load_calls == 1 && state.discover_calls == 0);
    expect_clean(0, 0, 0, 0);
    expect_events((const int[]){EVENT_LOAD}, 1);

    begin_case("null observation");
    reset_state();
    CHECK(boardtool_compare_c6("firmware.bin", "0123456789abcdef"
        "0123456789abcdef0123456789abcdef0123456789abcdef",
        NULL, &report, status, sizeof(status)) == 0);
    CHECK(state.load_calls == 0 && state.discover_calls == 0);
    expect_clean(0, 0, 0, 0);
    expect_events(NULL, 0);

    begin_case("null report");
    reset_state();
    CHECK(boardtool_compare_c6("firmware.bin", "0123456789abcdef"
        "0123456789abcdef0123456789abcdef0123456789abcdef",
        &observation, NULL, status, sizeof(status)) == 0);
    CHECK(state.load_calls == 0 && state.discover_calls == 0);
    expect_clean(0, 0, 0, 0);
    expect_events(NULL, 0);

    begin_case("null status");
    reset_state();
    CHECK(boardtool_compare_c6("firmware.bin", "0123456789abcdef"
        "0123456789abcdef0123456789abcdef0123456789abcdef",
        &observation, &report, NULL, sizeof(status)) == 0);
    CHECK(state.load_calls == 0 && state.discover_calls == 0);
    expect_clean(0, 0, 0, 0);
    expect_events(NULL, 0);

    begin_case("zero status capacity");
    reset_state();
    CHECK(boardtool_compare_c6("firmware.bin", "0123456789abcdef"
        "0123456789abcdef0123456789abcdef0123456789abcdef",
        &observation, &report, status, 0) == 0);
    CHECK(state.load_calls == 0 && state.discover_calls == 0);
    expect_clean(0, 0, 0, 0);
    expect_events(NULL, 0);
}

int main(void)
{
    test_invalid_source();
    test_discovery_failure();
    test_recheck_failure();
    test_comparison_failure();
    test_report_success_and_lifetime();
    test_null_arguments();

    printf("RESULT cases=%lu checks=%lu failures=%lu\n", cases, checks,
        failures);
    return failures == 0 ? 0 : 1;
}
