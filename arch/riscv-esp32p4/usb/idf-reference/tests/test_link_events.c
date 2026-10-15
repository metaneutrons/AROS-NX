#include <assert.h>
#include <stdint.h>

#include "../main/link_events.h"

static void test_init_and_snapshot(void)
{
    hs_link_events_t state;
    hs_link_counts_t counts;

    hs_link_events_init(&state);
    hs_link_events_snapshot(&state, &counts);
    for (uint32_t i = 0; i < HS_LINK_EVENT_COUNT; ++i)
        assert(counts.event[i] == 0);
    assert(counts.ignored == 0);
}

static void test_counts_each_supported_event(void)
{
    hs_link_events_t state;
    hs_link_counts_t counts;

    hs_link_events_init(&state);
    for (uint32_t event = HS_LINK_EVENT_BUS_RESET;
         event <= HS_LINK_EVENT_FUNC_CALL; ++event) {
        hs_link_events_record(&state, 1, event);
        hs_link_events_record(&state, 1, event);
    }

    hs_link_events_snapshot(&state, &counts);
    assert(counts.event[HS_LINK_EVENT_INVALID] == 0);
    for (uint32_t event = HS_LINK_EVENT_BUS_RESET;
         event <= HS_LINK_EVENT_FUNC_CALL; ++event)
        assert(counts.event[event] == 2);
    assert(counts.ignored == 0);
}

static void test_rejects_wrong_port_and_invalid_ids(void)
{
    hs_link_events_t state;
    hs_link_counts_t counts;

    hs_link_events_init(&state);
    hs_link_events_record(&state, 0, HS_LINK_EVENT_BUS_RESET);
    hs_link_events_record(&state, 2, HS_LINK_EVENT_SETUP);
    hs_link_events_record(&state, UINT8_MAX, HS_LINK_EVENT_XFER);
    hs_link_events_record(&state, 1, HS_LINK_EVENT_INVALID);
    hs_link_events_record(&state, 1, HS_LINK_EVENT_COUNT);
    hs_link_events_record(&state, 1, UINT32_MAX);

    hs_link_events_snapshot(&state, &counts);
    for (uint32_t event = 0; event < HS_LINK_EVENT_COUNT; ++event)
        assert(counts.event[event] == 0);
    assert(counts.ignored == 6);
}

static void test_wraps_modulo_uint32(void)
{
    hs_link_events_t state;
    hs_link_counts_t counts;

    hs_link_events_init(&state);
    atomic_store_explicit(&state.counts[HS_LINK_EVENT_SETUP], UINT32_MAX,
                          memory_order_relaxed);
    atomic_store_explicit(&state.ignored, UINT32_MAX, memory_order_relaxed);

    hs_link_events_record(&state, 1, HS_LINK_EVENT_SETUP);
    hs_link_events_record(&state, 0, HS_LINK_EVENT_SETUP);
    hs_link_events_snapshot(&state, &counts);
    assert(counts.event[HS_LINK_EVENT_SETUP] == 0);
    assert(counts.ignored == 0);
}

static void test_null_arguments_are_safe(void)
{
    hs_link_events_t state;
    hs_link_counts_t counts = {{0}, 0};

    hs_link_events_init(&state);
    hs_link_events_init(NULL);
    hs_link_events_record(NULL, 1, HS_LINK_EVENT_BUS_RESET);
    hs_link_events_snapshot(NULL, &counts);
    hs_link_events_snapshot(NULL, NULL);
    hs_link_events_snapshot(&state, NULL);
    assert(counts.ignored == 0);
}

int main(void)
{
    test_init_and_snapshot();
    test_counts_each_supported_event();
    test_rejects_wrong_port_and_invalid_ids();
    test_wraps_modulo_uint32();
    test_null_arguments_are_safe();
    return 0;
}
