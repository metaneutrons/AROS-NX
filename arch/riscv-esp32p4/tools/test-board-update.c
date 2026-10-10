/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "update.h"

#define TEST_IMAGE_SIZE 500u
#define TEST_TOTAL_BUDGET_US 100000u
#define TEST_FINISH_RESERVE_US 10000u
#define TEST_MAX_CALL_US 10000u
#define TEST_TIMEOUT_CAPACITY 8u

static unsigned long checks;

#define CHECK(expression) do { \
    ++checks; \
    if (!(expression)) { \
        fprintf(stderr, "line %u: %s\n", (unsigned)__LINE__, #expression); \
        exit(1); \
    } \
} while (0)

struct fixture {
    uint8_t source[TEST_IMAGE_SIZE + 1u];
    uint8_t received[TEST_IMAGE_SIZE];
    uint32_t source_size;
    uint32_t received_size;
    uint32_t expected_begin_size;
    uint32_t short_offset;
    uint32_t fail_read_offset;
    uint32_t bad_read_count_offset;
    unsigned int acquire_calls;
    unsigned int finish_calls;
    enum p4_board_update_finish_mode finish_modes[4];
    unsigned int revalidate_calls;
    unsigned int cancel_calls;
    unsigned int begin_calls;
    unsigned int write_calls;
    unsigned int end_calls;
    uint32_t now_us;
    uint32_t clock_override_value;
    unsigned int clock_calls;
    unsigned int clock_fail_call;
    unsigned int clock_invalid_call;
    unsigned int clock_override_call;
    unsigned int clock_calls_at_first_cancel;
    uint32_t acquire_advance_us;
    uint32_t finish_advance_us;
    uint32_t revalidate_advance_us;
    uint32_t begin_advance_us;
    uint32_t write_advance_us;
    uint32_t end_advance_us;
    uint32_t read_advance_us;
    uint32_t acquire_timeouts[TEST_TIMEOUT_CAPACITY];
    uint32_t finish_timeouts[TEST_TIMEOUT_CAPACITY];
    uint32_t revalidate_timeouts[TEST_TIMEOUT_CAPACITY];
    uint32_t begin_timeouts[TEST_TIMEOUT_CAPACITY];
    uint32_t write_timeouts[TEST_TIMEOUT_CAPACITY];
    uint32_t end_timeouts[TEST_TIMEOUT_CAPACITY];
    uint32_t read_timeouts[TEST_TIMEOUT_CAPACITY];
    unsigned int acquire_timeout_count;
    unsigned int finish_timeout_count;
    unsigned int revalidate_timeout_count;
    unsigned int begin_timeout_count;
    unsigned int write_timeout_count;
    unsigned int end_timeout_count;
    unsigned int read_timeout_count;
    unsigned int event_count;
    char events[32];
    int acquire_status;
    int finish_status;
    int revalidate_status;
    int cancelled;
    int cancel_after_acquire;
    unsigned int cancel_on_call;
    int cancel_after_first_write;
    int cancel_during_end;
    enum p4_board_update_call_result begin_result;
    enum p4_board_update_call_result write_result;
    enum p4_board_update_call_result end_result;
    int32_t begin_status;
    int32_t write_status;
    int32_t end_status;
};

static void record_timeout(uint32_t *timeouts, unsigned int *count,
                           uint32_t timeout_us)
{
    if (*count < TEST_TIMEOUT_CAPACITY)
        timeouts[*count] = timeout_us;
    ++*count;
}

static void advance_clock(struct fixture *fixture, uint32_t elapsed_us)
{
    fixture->now_us += elapsed_us;
}

static int update_clock(void *context, uint32_t *now)
{
    struct fixture *fixture = context;

    ++fixture->clock_calls;
    if (fixture->clock_fail_call == fixture->clock_calls)
        return 0;
    *now = fixture->clock_override_call == fixture->clock_calls
               ? fixture->clock_override_value
               : fixture->now_us;
    if (fixture->clock_invalid_call == fixture->clock_calls)
        return 2;
    return 1;
}

static void event(struct fixture *fixture, char value)
{
    if (fixture->event_count < sizeof fixture->events)
        fixture->events[fixture->event_count++] = value;
}

static int read_source(void *context, uint32_t offset, uint8_t *destination,
                       size_t capacity, size_t *actual_size,
                       uint32_t timeout_us)
{
    struct fixture *fixture = context;
    size_t available;
    size_t count;

    record_timeout(fixture->read_timeouts, &fixture->read_timeout_count,
                   timeout_us);
    advance_clock(fixture, fixture->read_advance_us);

    if (offset == fixture->fail_read_offset)
        return -41;

    if (offset == fixture->bad_read_count_offset) {
        *actual_size = capacity + 1u;
        return 0;
    }

    if (offset >= fixture->source_size) {
        *actual_size = 0u;
        return 0;
    }
    available = (size_t)(fixture->source_size - offset);
    count = available < capacity ? available : capacity;
    if (offset == fixture->short_offset && count != 0u)
        --count;
    memcpy(destination, fixture->source + offset, count);
    *actual_size = count;
    return 0;
}

static int acquire_maintenance(void *context, uint32_t timeout_us)
{
    struct fixture *fixture = context;

    record_timeout(fixture->acquire_timeouts, &fixture->acquire_timeout_count,
                   timeout_us);
    advance_clock(fixture, fixture->acquire_advance_us);
    ++fixture->acquire_calls;
    event(fixture, 'L');
    if (fixture->cancel_after_acquire)
        fixture->cancelled = 1;
    return fixture->acquire_status;
}

static int finish_maintenance(void *context,
                              enum p4_board_update_finish_mode mode,
                              uint32_t timeout_us)
{
    struct fixture *fixture = context;

    record_timeout(fixture->finish_timeouts, &fixture->finish_timeout_count,
                   timeout_us);
    advance_clock(fixture, fixture->finish_advance_us);
    if (fixture->finish_calls <
        sizeof fixture->finish_modes / sizeof fixture->finish_modes[0])
        fixture->finish_modes[fixture->finish_calls] = mode;
    ++fixture->finish_calls;
    event(fixture, 'F');
    return fixture->finish_status;
}

static int revalidate_source(void *context, uint32_t expected_size,
                             uint32_t timeout_us)
{
    struct fixture *fixture = context;

    record_timeout(fixture->revalidate_timeouts,
                   &fixture->revalidate_timeout_count, timeout_us);
    advance_clock(fixture, fixture->revalidate_advance_us);
    ++fixture->revalidate_calls;
    fixture->expected_begin_size = expected_size;
    event(fixture, 'S');
    return fixture->revalidate_status;
}

static int is_cancelled(void *context)
{
    struct fixture *fixture = context;

    ++fixture->cancel_calls;
    if (fixture->cancel_calls == 1u)
        fixture->clock_calls_at_first_cancel = fixture->clock_calls;
    if (fixture->cancel_on_call != 0u &&
        fixture->cancel_calls == fixture->cancel_on_call)
        fixture->cancelled = 1;
    return fixture->cancelled;
}

static enum p4_board_update_call_result begin_update(void *context,
                                                      uint32_t image_size,
                                                      int32_t *remote_status,
                                                      uint32_t timeout_us)
{
    struct fixture *fixture = context;

    record_timeout(fixture->begin_timeouts, &fixture->begin_timeout_count,
                   timeout_us);
    advance_clock(fixture, fixture->begin_advance_us);
    ++fixture->begin_calls;
    fixture->expected_begin_size = image_size;
    *remote_status = fixture->begin_status;
    event(fixture, 'B');
    return fixture->begin_result;
}

static enum p4_board_update_call_result write_update(void *context,
                                                      const uint8_t *data,
                                                      size_t data_size,
                                                      int32_t *remote_status,
                                                      uint32_t timeout_us)
{
    struct fixture *fixture = context;

    record_timeout(fixture->write_timeouts, &fixture->write_timeout_count,
                   timeout_us);
    advance_clock(fixture, fixture->write_advance_us);
    ++fixture->write_calls;
    if (data_size > sizeof fixture->received - fixture->received_size)
        return P4_BOARD_UPDATE_CALL_REJECTED;
    memcpy(fixture->received + fixture->received_size, data, data_size);
    fixture->received_size += (uint32_t)data_size;
    *remote_status = fixture->write_status;
    event(fixture, 'W');
    if (fixture->cancel_after_first_write && fixture->write_calls == 1u &&
        fixture->write_result == P4_BOARD_UPDATE_CALL_ACKNOWLEDGED)
        fixture->cancelled = 1;
    return fixture->write_result;
}

static enum p4_board_update_call_result end_update(void *context,
                                                    int32_t *remote_status,
                                                    uint32_t timeout_us)
{
    struct fixture *fixture = context;

    record_timeout(fixture->end_timeouts, &fixture->end_timeout_count,
                   timeout_us);
    advance_clock(fixture, fixture->end_advance_us);
    ++fixture->end_calls;
    *remote_status = fixture->end_status;
    event(fixture, 'E');
    if (fixture->cancel_during_end)
        fixture->cancelled = 1;
    return fixture->end_result;
}

static void init_case(struct fixture *fixture, struct p4_board_update_plan *plan,
                      uint8_t *scratch, size_t image_size)
{
    size_t i;

    memset(fixture, 0, sizeof *fixture);
    for (i = 0u; i < image_size; ++i)
        fixture->source[i] = (uint8_t)(i * 29u + 3u);
    fixture->source_size = (uint32_t)image_size;
    fixture->short_offset = UINT32_MAX;
    fixture->fail_read_offset = UINT32_MAX;
    fixture->bad_read_count_offset = UINT32_MAX;
    fixture->begin_result = P4_BOARD_UPDATE_CALL_ACKNOWLEDGED;
    fixture->write_result = P4_BOARD_UPDATE_CALL_ACKNOWLEDGED;
    fixture->end_result = P4_BOARD_UPDATE_CALL_ACKNOWLEDGED;
    fixture->begin_status = 0;
    fixture->write_status = 0;
    fixture->end_status = 0;

    memset(plan, 0, sizeof *plan);
    plan->image_size = (uint32_t)image_size;
    /* Synthetic clock and deadlines make the host tests deterministic; these
     * values are fixtures, not qualified transfer timings. */
    plan->total_budget_us = TEST_TOTAL_BUDGET_US;
    plan->finish_reserve_us = TEST_FINISH_RESERVE_US;
    plan->max_call_us = TEST_MAX_CALL_US;
    plan->gates.manifest_accepted = 1u;
    plan->gates.inspection_passed = 1u;
    plan->gates.recovery_verified = 1u;
    plan->gates.target_compatible = 1u;
    plan->reader.context = fixture;
    plan->reader.context_size = sizeof *fixture;
    plan->reader.read = read_source;
    plan->ops.context = fixture;
    plan->ops.context_size = sizeof *fixture;
    plan->ops.clock_us = update_clock;
    plan->ops.acquire_maintenance = acquire_maintenance;
    plan->ops.finish_maintenance = finish_maintenance;
    plan->ops.revalidate_source = revalidate_source;
    plan->ops.is_cancelled = is_cancelled;
    plan->ops.begin = begin_update;
    plan->ops.write = write_update;
    plan->ops.end = end_update;
    plan->scratch = scratch;
    plan->scratch_size = P4_BOARD_UPDATE_CHUNK_SIZE;
}

static void check_no_mutation(const struct fixture *fixture)
{
    CHECK(fixture->begin_calls == 0u);
    CHECK(fixture->write_calls == 0u);
    CHECK(fixture->end_calls == 0u);
}

static void check_finish(const struct fixture *fixture,
                         const struct p4_board_update_result *result,
                         enum p4_board_update_finish_mode expected_mode)
{
    CHECK(result->finish_mode == expected_mode);
    CHECK(result->lease_acquired == 1u);
    CHECK(result->lease_release_called == 1u);
    CHECK(result->lease_release_status == fixture->finish_status);
    CHECK(fixture->finish_calls == 1u);
    CHECK(fixture->finish_modes[0] == expected_mode);
}

static void check_no_finish(const struct fixture *fixture,
                            const struct p4_board_update_result *result)
{
    CHECK(result->finish_mode == P4_BOARD_UPDATE_FINISH_NONE);
    CHECK(result->lease_acquired == 0u);
    CHECK(result->lease_release_called == 0u);
    CHECK(fixture->finish_calls == 0u);
}

static void check_positive_timeouts(const struct fixture *fixture)
{
    unsigned int i;

#define CHECK_TIMEOUTS(field, count_field) do { \
    CHECK(fixture->count_field <= TEST_TIMEOUT_CAPACITY); \
    for (i = 0u; i < fixture->count_field; ++i) \
        CHECK(fixture->field[i] != 0u); \
} while (0)
    CHECK_TIMEOUTS(acquire_timeouts, acquire_timeout_count);
    CHECK_TIMEOUTS(finish_timeouts, finish_timeout_count);
    CHECK_TIMEOUTS(revalidate_timeouts, revalidate_timeout_count);
    CHECK_TIMEOUTS(begin_timeouts, begin_timeout_count);
    CHECK_TIMEOUTS(write_timeouts, write_timeout_count);
    CHECK_TIMEOUTS(end_timeouts, end_timeout_count);
    CHECK_TIMEOUTS(read_timeouts, read_timeout_count);
#undef CHECK_TIMEOUTS
}

static void use_test_budget(struct p4_board_update_plan *plan,
                            uint32_t total_us, uint32_t finish_us,
                            uint32_t max_call_us)
{
    plan->total_budget_us = total_us;
    plan->finish_reserve_us = finish_us;
    plan->max_call_us = max_call_us;
}

static void check_plan_refused(struct fixture *fixture,
                               struct p4_board_update_plan *plan,
                               struct p4_board_update_result *result)
{
    CHECK(p4_board_update_run(plan, result) == P4_BOARD_UPDATE_INVALID_INPUT);
    CHECK(fixture->clock_calls == 0u);
    CHECK(fixture->acquire_calls == 0u && fixture->finish_calls == 0u);
    check_no_finish(fixture, result);
    check_no_mutation(fixture);
}

static void test_budget_input_and_clock_hook_refusals(void)
{
    struct fixture fixture;
    struct p4_board_update_plan plan;
    struct p4_board_update_result result;
    uint8_t scratch[P4_BOARD_UPDATE_CHUNK_SIZE];

    init_case(&fixture, &plan, scratch, 1u);
    plan.total_budget_us = 0u;
    check_plan_refused(&fixture, &plan, &result);

    init_case(&fixture, &plan, scratch, 1u);
    plan.total_budget_us = (uint32_t)P4_BOARD_UPDATE_MAX_TOTAL_US + 1u;
    check_plan_refused(&fixture, &plan, &result);

    init_case(&fixture, &plan, scratch, 1u);
    plan.finish_reserve_us = 0u;
    check_plan_refused(&fixture, &plan, &result);

    init_case(&fixture, &plan, scratch, 1u);
    plan.finish_reserve_us = plan.total_budget_us;
    check_plan_refused(&fixture, &plan, &result);

    init_case(&fixture, &plan, scratch, 1u);
    plan.max_call_us = 0u;
    check_plan_refused(&fixture, &plan, &result);

    init_case(&fixture, &plan, scratch, 1u);
    plan.max_call_us = P4_BOARD_UPDATE_MAX_CALL_US + 1u;
    check_plan_refused(&fixture, &plan, &result);

    init_case(&fixture, &plan, scratch, 1u);
    plan.ops.clock_us = NULL;
    check_plan_refused(&fixture, &plan, &result);

    init_case(&fixture, &plan, scratch, 1u);
    plan.ops.acquire_maintenance = NULL;
    check_plan_refused(&fixture, &plan, &result);
}

/* The work reserve prevents BEGIN at the work deadline and remains available
 * to complete the NO_BEGIN handoff against the original total deadline. */
static void test_work_deadline_before_begin_preserves_finish_reserve(void)
{
    struct fixture fixture;
    struct p4_board_update_plan plan;
    struct p4_board_update_result result;
    uint8_t scratch[P4_BOARD_UPDATE_CHUNK_SIZE];

    init_case(&fixture, &plan, scratch, 1u);
    use_test_budget(&plan, 1000u, 200u, 1000u);
    fixture.revalidate_advance_us = 800u;

    CHECK(p4_board_update_run(&plan, &result) ==
          P4_BOARD_UPDATE_BUDGET_EXPIRED);
    CHECK(result.budget_error == P4_BOARD_UPDATE_BUDGET_TIMEOUT);
    CHECK(result.elapsed_us == 800u);
    CHECK(fixture.acquire_calls == 1u && fixture.revalidate_calls == 1u);
    CHECK(fixture.acquire_timeout_count == 1u &&
          fixture.acquire_timeouts[0] == 800u);
    CHECK(fixture.revalidate_timeout_count == 1u &&
          fixture.revalidate_timeouts[0] == 800u);
    CHECK(fixture.begin_calls == 0u && fixture.write_calls == 0u &&
          fixture.end_calls == 0u);
    CHECK(fixture.finish_timeout_count == 1u &&
          fixture.finish_timeouts[0] == 200u);
    check_finish(&fixture, &result, P4_BOARD_UPDATE_FINISH_NO_BEGIN);
    check_positive_timeouts(&fixture);
}

/* Synthetic callbacks consume the same finite deadline on each request. The
 * third WRITE overruns its positive timeout but returns a verified ACK; its
 * progress is retained, the next operation is refused, and no END is sent. */
static void test_aggregate_budget_is_not_renewed_and_late_write_ack_is_counted(void)
{
    struct fixture fixture;
    struct p4_board_update_plan plan;
    struct p4_board_update_result result;
    uint8_t scratch[P4_BOARD_UPDATE_CHUNK_SIZE];

    init_case(&fixture, &plan, scratch, TEST_IMAGE_SIZE);
    use_test_budget(&plan, 1000u, 200u, 300u);
    fixture.begin_advance_us = 100u;
    fixture.write_advance_us = 250u;

    CHECK(p4_board_update_run(&plan, &result) ==
          P4_BOARD_UPDATE_BUDGET_EXPIRED);
    CHECK(result.budget_error == P4_BOARD_UPDATE_BUDGET_TIMEOUT);
    CHECK(result.elapsed_us == 850u);
    CHECK(result.begin_attempted == 1u && fixture.begin_calls == 1u);
    CHECK(fixture.write_calls == 3u);
    CHECK(fixture.write_timeout_count == 3u);
    CHECK(fixture.write_timeouts[0] == 300u);
    CHECK(fixture.write_timeouts[1] == 300u);
    CHECK(fixture.write_timeouts[2] == 200u);
    CHECK(fixture.read_timeout_count == 3u);
    CHECK(fixture.read_timeouts[0] == 300u);
    CHECK(fixture.read_timeouts[1] == 300u);
    CHECK(fixture.read_timeouts[2] == 200u);
    CHECK(result.acknowledged_chunks == 3u);
    CHECK(result.acknowledged_bytes == TEST_IMAGE_SIZE);
    CHECK(fixture.end_calls == 0u);
    CHECK(fixture.finish_timeout_count == 1u &&
          fixture.finish_timeouts[0] == 150u);
    check_finish(&fixture, &result, P4_BOARD_UPDATE_FINISH_INCOMPLETE);
    check_positive_timeouts(&fixture);
}

/* END may be acknowledged after consuming the work reserve. Preserve that
 * receipt as WAIT_REBOOT, finish within the original total budget, and refuse
 * postboot confirmation because the work budget expired. */
static void test_end_ack_in_finish_reserve_is_not_installable(void)
{
    struct fixture fixture;
    struct p4_board_update_plan plan;
    struct p4_board_update_result result;
    uint8_t scratch[P4_BOARD_UPDATE_CHUNK_SIZE];

    init_case(&fixture, &plan, scratch, 1u);
    use_test_budget(&plan, 1000u, 200u, 1000u);
    fixture.end_advance_us = 850u;

    CHECK(p4_board_update_run(&plan, &result) == P4_BOARD_UPDATE_WAIT_REBOOT);
    CHECK(result.budget_error == P4_BOARD_UPDATE_BUDGET_TIMEOUT);
    CHECK(result.elapsed_us == 850u);
    CHECK(result.end_attempted == 1u && fixture.end_calls == 1u);
    CHECK(fixture.end_timeout_count == 1u &&
          fixture.end_timeouts[0] == 800u);
    CHECK(fixture.finish_timeout_count == 1u &&
          fixture.finish_timeouts[0] == 150u);
    check_finish(&fixture, &result, P4_BOARD_UPDATE_FINISH_WAIT_REBOOT);
    CHECK(!p4_board_update_confirm_postboot(&result, 1, 1));
    CHECK(result.status == P4_BOARD_UPDATE_WAIT_REBOOT);
    check_positive_timeouts(&fixture);
}

/* A verified END receipt stays WAIT_REBOOT even if its callback returns after
 * the original total deadline. No late terminal handoff or postboot claim is
 * allowed once no bounded callback window remains. */
static void test_end_ack_after_total_deadline_is_not_installable(void)
{
    struct fixture fixture;
    struct p4_board_update_plan plan;
    struct p4_board_update_result result;
    uint8_t scratch[P4_BOARD_UPDATE_CHUNK_SIZE];

    init_case(&fixture, &plan, scratch, 1u);
    use_test_budget(&plan, 1000u, 200u, 1000u);
    fixture.end_advance_us = 1001u;

    CHECK(p4_board_update_run(&plan, &result) == P4_BOARD_UPDATE_WAIT_REBOOT);
    CHECK(result.budget_error == P4_BOARD_UPDATE_BUDGET_TIMEOUT);
    CHECK(result.elapsed_us == 1001u);
    CHECK(result.end_attempted == 1u && fixture.end_calls == 1u);
    CHECK(result.finish_mode == P4_BOARD_UPDATE_FINISH_WAIT_REBOOT);
    CHECK(result.lease_release_called == 0u);
    CHECK(result.lease_release_status == -1 && fixture.finish_calls == 0u);
    CHECK(!p4_board_update_confirm_postboot(&result, 1, 1));
    CHECK(result.status == P4_BOARD_UPDATE_WAIT_REBOOT);
    check_positive_timeouts(&fixture);
}

static void test_finish_callback_overrun_is_visible(void)
{
    struct fixture fixture;
    struct p4_board_update_plan plan;
    struct p4_board_update_result result;
    uint8_t scratch[P4_BOARD_UPDATE_CHUNK_SIZE];

    init_case(&fixture, &plan, scratch, 1u);
    use_test_budget(&plan, 1000u, 200u, 1000u);
    fixture.revalidate_advance_us = 800u;
    fixture.finish_advance_us = 201u;

    CHECK(p4_board_update_run(&plan, &result) ==
          P4_BOARD_UPDATE_BUDGET_EXPIRED);
    CHECK(result.budget_error == P4_BOARD_UPDATE_BUDGET_TIMEOUT);
    CHECK(result.elapsed_us == 1001u);
    CHECK(fixture.finish_calls == 1u && result.lease_release_called == 1u);
    CHECK(fixture.finish_timeouts[0] == 200u);
    CHECK(result.lease_release_status == -1);
    CHECK(result.finish_mode == P4_BOARD_UPDATE_FINISH_NO_BEGIN);
    check_positive_timeouts(&fixture);
}

static void test_clock_failure_backwards_clock_and_wrap(void)
{
    struct fixture fixture;
    struct p4_board_update_plan plan;
    struct p4_board_update_result result;
    uint8_t scratch[P4_BOARD_UPDATE_CHUNK_SIZE];

    init_case(&fixture, &plan, scratch, 1u);
    fixture.clock_fail_call = 1u;
    CHECK(p4_board_update_run(&plan, &result) == P4_BOARD_UPDATE_CLOCK_FAILED);
    CHECK(result.budget_error == P4_BOARD_UPDATE_BUDGET_CLOCK);
    CHECK(fixture.clock_calls == 1u && fixture.acquire_calls == 0u);
    CHECK(fixture.finish_calls == 0u && result.lease_release_status == 0);
    check_no_mutation(&fixture);

    init_case(&fixture, &plan, scratch, 1u);
    fixture.clock_invalid_call = 1u;
    CHECK(p4_board_update_run(&plan, &result) == P4_BOARD_UPDATE_CLOCK_FAILED);
    CHECK(result.budget_error == P4_BOARD_UPDATE_BUDGET_CLOCK);
    CHECK(fixture.clock_calls == 1u && fixture.acquire_calls == 0u);
    CHECK(fixture.finish_calls == 0u);
    check_no_mutation(&fixture);

    init_case(&fixture, &plan, scratch, 1u);
    fixture.now_us = 100u;
    fixture.clock_override_call = 2u;
    fixture.clock_override_value = 99u;
    CHECK(p4_board_update_run(&plan, &result) == P4_BOARD_UPDATE_CLOCK_FAILED);
    CHECK(result.budget_error == P4_BOARD_UPDATE_BUDGET_CLOCK);
    CHECK(fixture.acquire_calls == 0u && fixture.finish_calls == 0u);
    check_no_mutation(&fixture);

    init_case(&fixture, &plan, scratch, 1u);
    fixture.clock_fail_call = 5u;
    CHECK(p4_board_update_run(&plan, &result) == P4_BOARD_UPDATE_CLOCK_FAILED);
    CHECK(result.budget_error == P4_BOARD_UPDATE_BUDGET_CLOCK);
    CHECK(result.begin_attempted == 1u && fixture.begin_calls == 1u);
    CHECK(fixture.write_calls == 0u && fixture.end_calls == 0u);
    CHECK(result.finish_mode == P4_BOARD_UPDATE_FINISH_INCOMPLETE);
    CHECK(result.lease_release_called == 0u &&
          result.lease_release_status == -1 && fixture.finish_calls == 0u);
    check_positive_timeouts(&fixture);

    init_case(&fixture, &plan, scratch, 1u);
    use_test_budget(&plan, 1000u, 200u, 500u);
    fixture.now_us = UINT32_MAX - 20u;
    fixture.begin_advance_us = 50u;
    CHECK(p4_board_update_run(&plan, &result) == P4_BOARD_UPDATE_WAIT_REBOOT);
    CHECK(result.budget_error == P4_BOARD_UPDATE_BUDGET_OK);
    CHECK(result.elapsed_us == 50u);
    CHECK(fixture.finish_calls == 1u && result.lease_release_status == 0);
    CHECK(p4_board_update_confirm_postboot(&result, 1, 1));
    check_positive_timeouts(&fixture);
}

static void test_success_waits_for_postboot(void)
{
    struct fixture fixture;
    struct p4_board_update_plan plan;
    struct p4_board_update_result result;
    uint8_t scratch[P4_BOARD_UPDATE_CHUNK_SIZE];

    init_case(&fixture, &plan, scratch, TEST_IMAGE_SIZE);
    CHECK(p4_board_update_run(&plan, &result) == P4_BOARD_UPDATE_WAIT_REBOOT);
    CHECK(result.status == P4_BOARD_UPDATE_WAIT_REBOOT);
    CHECK(result.image_size == TEST_IMAGE_SIZE);
    CHECK(result.total_chunks == 3u);
    CHECK(result.acknowledged_chunks == 3u);
    CHECK(result.acknowledged_bytes == TEST_IMAGE_SIZE);
    CHECK(result.failed_chunk_index == P4_BOARD_UPDATE_NO_FAILED_CHUNK);
    CHECK(result.begin_attempted == 1u && result.end_attempted == 1u);
    CHECK(result.lease_acquired == 1u && result.lease_release_called == 1u);
    CHECK(fixture.acquire_calls == 1u && fixture.finish_calls == 1u);
    check_finish(&fixture, &result, P4_BOARD_UPDATE_FINISH_WAIT_REBOOT);
    CHECK(fixture.revalidate_calls == 1u);
    CHECK(fixture.expected_begin_size == TEST_IMAGE_SIZE);
    CHECK(fixture.begin_calls == 1u && fixture.write_calls == 3u);
    CHECK(fixture.end_calls == 1u);
    CHECK(fixture.received_size == TEST_IMAGE_SIZE);
    CHECK(memcmp(fixture.source, fixture.received, TEST_IMAGE_SIZE) == 0);
    CHECK(fixture.events[0] == 'L' && fixture.events[1] == 'S');
    CHECK(fixture.events[2] == 'B');
    CHECK(fixture.events[fixture.event_count - 2u] == 'E');
    CHECK(fixture.events[fixture.event_count - 1u] == 'F');
    CHECK(!p4_board_update_confirm_postboot(&result, 0, 1));
    CHECK(result.status == P4_BOARD_UPDATE_POSTBOOT_FAILED);
}

static void test_all_preflight_gate_refusals(void)
{
    unsigned int mask;

    for (mask = 0u; mask < 16u; ++mask) {
        struct fixture fixture;
        struct p4_board_update_plan plan;
        struct p4_board_update_result result;
        uint8_t scratch[P4_BOARD_UPDATE_CHUNK_SIZE];

        init_case(&fixture, &plan, scratch, 1u);
        plan.gates.manifest_accepted = (uint8_t)((mask >> 0) & 1u);
        plan.gates.inspection_passed = (uint8_t)((mask >> 1) & 1u);
        plan.gates.recovery_verified = (uint8_t)((mask >> 2) & 1u);
        plan.gates.target_compatible = (uint8_t)((mask >> 3) & 1u);
        if (mask == 15u) {
            CHECK(p4_board_update_run(&plan, &result) ==
                  P4_BOARD_UPDATE_WAIT_REBOOT);
            CHECK(fixture.begin_calls == 1u);
            check_finish(&fixture, &result,
                         P4_BOARD_UPDATE_FINISH_WAIT_REBOOT);
            continue;
        }
        CHECK(p4_board_update_run(&plan, &result) ==
              P4_BOARD_UPDATE_PREFLIGHT_REFUSED);
        CHECK(result.begin_attempted == 0u);
        CHECK(fixture.acquire_calls == 0u && fixture.finish_calls == 0u);
        check_no_finish(&fixture, &result);
        check_no_mutation(&fixture);
    }
}

static void test_input_and_lease_refusals(void)
{
    struct fixture fixture;
    struct p4_board_update_plan plan;
    struct p4_board_update_result result;
    uint8_t scratch[P4_BOARD_UPDATE_CHUNK_SIZE];

    init_case(&fixture, &plan, scratch, 1u);
    plan.image_size = 0u;
    CHECK(p4_board_update_run(&plan, &result) == P4_BOARD_UPDATE_INVALID_INPUT);
    check_no_finish(&fixture, &result);
    check_no_mutation(&fixture);

    init_case(&fixture, &plan, scratch, 1u);
    plan.image_size = P4_BOARD_UPDATE_MAX_IMAGE_SIZE + 1u;
    CHECK(p4_board_update_run(&plan, &result) == P4_BOARD_UPDATE_INVALID_INPUT);
    check_no_finish(&fixture, &result);
    check_no_mutation(&fixture);

    init_case(&fixture, &plan, scratch, 1u);
    plan.scratch_size = P4_BOARD_UPDATE_CHUNK_SIZE - 1u;
    CHECK(p4_board_update_run(&plan, &result) == P4_BOARD_UPDATE_INVALID_INPUT);
    check_no_finish(&fixture, &result);
    check_no_mutation(&fixture);

    init_case(&fixture, &plan, scratch, 1u);
    fixture.acquire_status = -9;
    CHECK(p4_board_update_run(&plan, &result) == P4_BOARD_UPDATE_LEASE_FAILED);
    CHECK(fixture.acquire_calls == 1u && fixture.finish_calls == 0u);
    check_no_finish(&fixture, &result);
    check_no_mutation(&fixture);

    init_case(&fixture, &plan, scratch, 1u);
    fixture.cancelled = 1;
    CHECK(p4_board_update_run(&plan, &result) ==
          P4_BOARD_UPDATE_CANCELLED_BEFORE_BEGIN);
    CHECK(fixture.acquire_calls == 0u && fixture.finish_calls == 0u);
    CHECK(fixture.clock_calls_at_first_cancel == 1u);
    check_no_finish(&fixture, &result);
    check_no_mutation(&fixture);

    init_case(&fixture, &plan, scratch, 1u);
    fixture.cancel_after_acquire = 1;
    CHECK(p4_board_update_run(&plan, &result) ==
          P4_BOARD_UPDATE_CANCELLED_BEFORE_BEGIN);
    CHECK(fixture.acquire_calls == 1u && fixture.finish_calls == 1u);
    CHECK(fixture.revalidate_calls == 0u);
    check_finish(&fixture, &result, P4_BOARD_UPDATE_FINISH_NO_BEGIN);
    check_no_mutation(&fixture);

    init_case(&fixture, &plan, scratch, 1u);
    fixture.revalidate_status = -12;
    CHECK(p4_board_update_run(&plan, &result) == P4_BOARD_UPDATE_SOURCE_CHANGED);
    CHECK(fixture.acquire_calls == 1u && fixture.finish_calls == 1u);
    CHECK(fixture.revalidate_calls == 1u);
    check_finish(&fixture, &result, P4_BOARD_UPDATE_FINISH_NO_BEGIN);
    check_no_mutation(&fixture);

    init_case(&fixture, &plan, scratch, 1u);
    fixture.cancel_on_call = 3u;
    CHECK(p4_board_update_run(&plan, &result) ==
          P4_BOARD_UPDATE_CANCELLED_BEFORE_BEGIN);
    CHECK(fixture.revalidate_calls == 1u);
    CHECK(fixture.begin_calls == 0u && fixture.write_calls == 0u);
    CHECK(fixture.end_calls == 0u);
    check_finish(&fixture, &result, P4_BOARD_UPDATE_FINISH_NO_BEGIN);
}

static void test_alias_and_overflow_refusals(void)
{
    struct fixture fixture;
    struct p4_board_update_plan plan;
    struct p4_board_update_result result;
    uint8_t scratch[P4_BOARD_UPDATE_CHUNK_SIZE];
    uint8_t before[sizeof plan];
    union {
        struct p4_board_update_plan plan;
        struct p4_board_update_result result;
    } plan_result_alias;
    union {
        uint8_t scratch[P4_BOARD_UPDATE_CHUNK_SIZE];
        struct p4_board_update_result result;
    } scratch_result_alias;

    init_case(&fixture, &plan, scratch, 1u);
    init_case(&fixture, &plan_result_alias.plan, scratch, 1u);
    memcpy(before, &plan_result_alias.plan, sizeof before);
    CHECK(p4_board_update_run(&plan_result_alias.plan,
                              &plan_result_alias.result) ==
          P4_BOARD_UPDATE_INVALID_INPUT);
    CHECK(memcmp(before, &plan_result_alias.plan, sizeof before) == 0);
    CHECK(fixture.begin_calls == 0u && fixture.acquire_calls == 0u);

    init_case(&fixture, &plan, scratch, 1u);
    memset(&scratch_result_alias, 0xa5, sizeof scratch_result_alias);
    plan.scratch = scratch_result_alias.scratch;
    plan.scratch_size = sizeof scratch_result_alias.scratch;
    memset(&scratch_result_alias.result, 0x5a,
           sizeof scratch_result_alias.result);
    memcpy(before, &scratch_result_alias.result, sizeof result);
    CHECK(p4_board_update_run(&plan, &scratch_result_alias.result) ==
          P4_BOARD_UPDATE_INVALID_INPUT);
    CHECK(memcmp(before, &scratch_result_alias.result, sizeof result) == 0);
    CHECK(fixture.begin_calls == 0u && fixture.acquire_calls == 0u);

    init_case(&fixture, &plan, scratch, 1u);
    memset(&result, 0x5a, sizeof result);
    memcpy(before, &result, sizeof result);
    plan.ops.context = &result;
    plan.ops.context_size = sizeof result;
    CHECK(p4_board_update_run(&plan, &result) ==
          P4_BOARD_UPDATE_INVALID_INPUT);
    CHECK(memcmp(before, &result, sizeof result) == 0);

    init_case(&fixture, &plan, scratch, 1u);
    memset(&result, 0x5a, sizeof result);
    memcpy(before, &result, sizeof result);
    plan.scratch = (uint8_t *)(uintptr_t)(UINTPTR_MAX - 16u);
    plan.scratch_size = P4_BOARD_UPDATE_CHUNK_SIZE;
    CHECK(p4_board_update_run(&plan, &result) ==
          P4_BOARD_UPDATE_INVALID_INPUT);
    CHECK(memcmp(before, &result, sizeof result) == 0);

    init_case(&fixture, &plan, scratch, 1u);
    plan.scratch = (uint8_t *)&plan;
    plan.scratch_size = P4_BOARD_UPDATE_CHUNK_SIZE;
    memset(&result, 0x5a, sizeof result);
    memcpy(before, &result, sizeof result);
    CHECK(p4_board_update_run(&plan, &result) ==
          P4_BOARD_UPDATE_INVALID_INPUT);
    CHECK(memcmp(before, &result, sizeof result) == 0);
    CHECK(fixture.begin_calls == 0u && fixture.acquire_calls == 0u);
}

static void test_begin_terminal_policy(void)
{
    struct fixture fixture;
    struct p4_board_update_plan plan;
    struct p4_board_update_result result;
    uint8_t scratch[P4_BOARD_UPDATE_CHUNK_SIZE];
    enum p4_board_update_call_result outcomes[] = {
        P4_BOARD_UPDATE_CALL_REJECTED,
        P4_BOARD_UPDATE_CALL_AMBIGUOUS,
        (enum p4_board_update_call_result)73
    };
    size_t i;

    for (i = 0u; i < sizeof outcomes / sizeof outcomes[0]; ++i) {
        enum p4_board_update_status expected =
            outcomes[i] == P4_BOARD_UPDATE_CALL_REJECTED
                ? P4_BOARD_UPDATE_BEGIN_FAILED
                : P4_BOARD_UPDATE_BEGIN_UNKNOWN;

        init_case(&fixture, &plan, scratch, TEST_IMAGE_SIZE);
        fixture.begin_result = outcomes[i];
        CHECK(p4_board_update_run(&plan, &result) == expected);
        CHECK(result.begin_attempted == 1u && result.end_attempted == 0u);
        CHECK(fixture.begin_calls == 1u);
        CHECK(fixture.write_calls == 0u && fixture.end_calls == 0u);
        check_finish(&fixture, &result, P4_BOARD_UPDATE_FINISH_INCOMPLETE);
    }

    init_case(&fixture, &plan, scratch, 1u);
    fixture.begin_status = -3;
    CHECK(p4_board_update_run(&plan, &result) ==
          P4_BOARD_UPDATE_BEGIN_UNKNOWN);
    CHECK(result.callback_status == -3);
    CHECK(fixture.begin_calls == 1u);
    CHECK(fixture.write_calls == 0u && fixture.end_calls == 0u);
    check_finish(&fixture, &result, P4_BOARD_UPDATE_FINISH_INCOMPLETE);

}

static void test_source_shape_failures(void)
{
    struct fixture fixture;
    struct p4_board_update_plan plan;
    struct p4_board_update_result result;
    uint8_t scratch[P4_BOARD_UPDATE_CHUNK_SIZE];

    init_case(&fixture, &plan, scratch, TEST_IMAGE_SIZE);
    fixture.short_offset = 0u;
    CHECK(p4_board_update_run(&plan, &result) == P4_BOARD_UPDATE_SHORT_READ);
    CHECK(result.acknowledged_chunks == 0u && result.acknowledged_bytes == 0u);
    CHECK(fixture.begin_calls == 1u && fixture.write_calls == 0u);
    CHECK(fixture.end_calls == 0u);
    check_finish(&fixture, &result, P4_BOARD_UPDATE_FINISH_INCOMPLETE);

    init_case(&fixture, &plan, scratch, TEST_IMAGE_SIZE);
    fixture.fail_read_offset = P4_BOARD_UPDATE_CHUNK_SIZE;
    CHECK(p4_board_update_run(&plan, &result) ==
          P4_BOARD_UPDATE_SOURCE_READ_FAILED);
    CHECK(result.acknowledged_chunks == 1u);
    CHECK(result.acknowledged_bytes == P4_BOARD_UPDATE_CHUNK_SIZE);
    CHECK(fixture.write_calls == 1u && fixture.end_calls == 0u);
    check_finish(&fixture, &result, P4_BOARD_UPDATE_FINISH_INCOMPLETE);

    init_case(&fixture, &plan, scratch, TEST_IMAGE_SIZE);
    fixture.bad_read_count_offset = P4_BOARD_UPDATE_CHUNK_SIZE;
    CHECK(p4_board_update_run(&plan, &result) ==
          P4_BOARD_UPDATE_SOURCE_BAD_COUNT);
    CHECK(fixture.write_calls == 1u && fixture.end_calls == 0u);
    check_finish(&fixture, &result, P4_BOARD_UPDATE_FINISH_INCOMPLETE);

    init_case(&fixture, &plan, scratch, TEST_IMAGE_SIZE);
    fixture.source_size = TEST_IMAGE_SIZE + 1u;
    fixture.source[TEST_IMAGE_SIZE] = 0x7bu;
    CHECK(p4_board_update_run(&plan, &result) ==
          P4_BOARD_UPDATE_TRAILING_DATA);
    CHECK(result.acknowledged_bytes == TEST_IMAGE_SIZE);
    CHECK(fixture.write_calls == 3u && fixture.end_calls == 0u);
    check_finish(&fixture, &result, P4_BOARD_UPDATE_FINISH_INCOMPLETE);

    init_case(&fixture, &plan, scratch, TEST_IMAGE_SIZE);
    fixture.fail_read_offset = TEST_IMAGE_SIZE;
    CHECK(p4_board_update_run(&plan, &result) ==
          P4_BOARD_UPDATE_SOURCE_READ_FAILED);
    CHECK(fixture.write_calls == 3u && fixture.end_calls == 0u);
    check_finish(&fixture, &result, P4_BOARD_UPDATE_FINISH_INCOMPLETE);

    init_case(&fixture, &plan, scratch, TEST_IMAGE_SIZE);
    fixture.bad_read_count_offset = TEST_IMAGE_SIZE;
    CHECK(p4_board_update_run(&plan, &result) ==
          P4_BOARD_UPDATE_SOURCE_BAD_COUNT);
    CHECK(fixture.write_calls == 3u && fixture.end_calls == 0u);
    check_finish(&fixture, &result, P4_BOARD_UPDATE_FINISH_INCOMPLETE);

    init_case(&fixture, &plan, scratch, TEST_IMAGE_SIZE);
    fixture.cancel_after_first_write = 1;
    CHECK(p4_board_update_run(&plan, &result) ==
          P4_BOARD_UPDATE_CANCELLED_INCOMPLETE);
    CHECK(result.acknowledged_chunks == 1u);
    CHECK(result.acknowledged_bytes == P4_BOARD_UPDATE_CHUNK_SIZE);
    CHECK(fixture.write_calls == 1u && fixture.end_calls == 0u);
    check_finish(&fixture, &result, P4_BOARD_UPDATE_FINISH_INCOMPLETE);

    init_case(&fixture, &plan, scratch, TEST_IMAGE_SIZE);
    fixture.cancel_on_call = 4u;
    CHECK(p4_board_update_run(&plan, &result) ==
          P4_BOARD_UPDATE_CANCELLED_INCOMPLETE);
    CHECK(fixture.begin_calls == 1u);
    CHECK(fixture.write_calls == 0u && fixture.end_calls == 0u);
    check_finish(&fixture, &result, P4_BOARD_UPDATE_FINISH_INCOMPLETE);

    init_case(&fixture, &plan, scratch, TEST_IMAGE_SIZE);
    fixture.cancel_on_call = 5u;
    CHECK(p4_board_update_run(&plan, &result) ==
          P4_BOARD_UPDATE_CANCELLED_INCOMPLETE);
    CHECK(fixture.begin_calls == 1u);
    CHECK(fixture.write_calls == 0u && fixture.end_calls == 0u);
    check_finish(&fixture, &result, P4_BOARD_UPDATE_FINISH_INCOMPLETE);

    init_case(&fixture, &plan, scratch, 1u);
    fixture.cancel_on_call = 6u;
    CHECK(p4_board_update_run(&plan, &result) ==
          P4_BOARD_UPDATE_CANCELLED_INCOMPLETE);
    CHECK(fixture.begin_calls == 1u);
    CHECK(fixture.write_calls == 1u && fixture.end_calls == 0u);
    check_finish(&fixture, &result, P4_BOARD_UPDATE_FINISH_INCOMPLETE);

    init_case(&fixture, &plan, scratch, 1u);
    fixture.cancel_on_call = 7u;
    CHECK(p4_board_update_run(&plan, &result) ==
          P4_BOARD_UPDATE_CANCELLED_INCOMPLETE);
    CHECK(fixture.begin_calls == 1u);
    CHECK(fixture.write_calls == 1u && fixture.end_calls == 0u);
    check_finish(&fixture, &result, P4_BOARD_UPDATE_FINISH_INCOMPLETE);
}

static void test_write_terminal_policy(void)
{
    struct fixture fixture;
    struct p4_board_update_plan plan;
    struct p4_board_update_result result;
    uint8_t scratch[P4_BOARD_UPDATE_CHUNK_SIZE];
    enum p4_board_update_call_result outcomes[] = {
        P4_BOARD_UPDATE_CALL_REJECTED,
        P4_BOARD_UPDATE_CALL_AMBIGUOUS,
        (enum p4_board_update_call_result)73,
        P4_BOARD_UPDATE_CALL_ACKNOWLEDGED
    };
    size_t i;

    for (i = 0u; i < sizeof outcomes / sizeof outcomes[0]; ++i) {
        enum p4_board_update_status expected =
            outcomes[i] == P4_BOARD_UPDATE_CALL_REJECTED
                ? P4_BOARD_UPDATE_WRITE_REJECTED
                : P4_BOARD_UPDATE_WRITE_UNKNOWN;

        init_case(&fixture, &plan, scratch, TEST_IMAGE_SIZE);
        fixture.write_result = outcomes[i];
        fixture.write_status = -77;
        CHECK(p4_board_update_run(&plan, &result) == expected);
        CHECK(result.acknowledged_chunks == 0u);
        CHECK(result.acknowledged_bytes == 0u);
        CHECK(result.failed_chunk_index == 0u);
        CHECK(result.callback_status == -77);
        CHECK(fixture.begin_calls == 1u && fixture.write_calls == 1u);
        CHECK(fixture.received_size == P4_BOARD_UPDATE_CHUNK_SIZE);
        CHECK(fixture.end_calls == 0u);
        check_finish(&fixture, &result, P4_BOARD_UPDATE_FINISH_INCOMPLETE);
    }
}

static void test_end_terminal_policy(void)
{
    struct fixture fixture;
    struct p4_board_update_plan plan;
    struct p4_board_update_result result;
    uint8_t scratch[P4_BOARD_UPDATE_CHUNK_SIZE];
    enum p4_board_update_call_result outcomes[] = {
        P4_BOARD_UPDATE_CALL_REJECTED,
        P4_BOARD_UPDATE_CALL_AMBIGUOUS,
        (enum p4_board_update_call_result)73,
        P4_BOARD_UPDATE_CALL_ACKNOWLEDGED
    };
    size_t i;

    for (i = 0u; i < sizeof outcomes / sizeof outcomes[0]; ++i) {
        init_case(&fixture, &plan, scratch, TEST_IMAGE_SIZE);
        fixture.end_result = outcomes[i];
        fixture.end_status = outcomes[i] ==
                                     P4_BOARD_UPDATE_CALL_ACKNOWLEDGED
                                 ? 5
                                 : -2;
        CHECK(p4_board_update_run(&plan, &result) ==
              P4_BOARD_UPDATE_END_COMMIT_UNKNOWN);
        CHECK(result.end_attempted == 1u);
        CHECK(result.callback_status == fixture.end_status);
        CHECK(result.acknowledged_bytes == TEST_IMAGE_SIZE);
        CHECK(fixture.begin_calls == 1u);
        CHECK(fixture.write_calls == result.total_chunks);
        CHECK(fixture.end_calls == 1u);
        CHECK(!p4_board_update_confirm_postboot(&result, 1, 1));
        check_finish(&fixture, &result,
                     P4_BOARD_UPDATE_FINISH_COMMIT_UNKNOWN);
    }

    init_case(&fixture, &plan, scratch, TEST_IMAGE_SIZE);
    fixture.finish_status = -17;
    CHECK(p4_board_update_run(&plan, &result) == P4_BOARD_UPDATE_WAIT_REBOOT);
    CHECK(result.lease_release_called == 1u);
    CHECK(result.lease_release_status == -17);
    CHECK(!p4_board_update_confirm_postboot(&result, 1, 1));
    CHECK(result.status == P4_BOARD_UPDATE_WAIT_REBOOT);
    check_finish(&fixture, &result, P4_BOARD_UPDATE_FINISH_WAIT_REBOOT);

    init_case(&fixture, &plan, scratch, TEST_IMAGE_SIZE);
    fixture.cancel_during_end = 1;
    CHECK(p4_board_update_run(&plan, &result) == P4_BOARD_UPDATE_WAIT_REBOOT);
    CHECK(fixture.end_calls == 1u);
    check_finish(&fixture, &result, P4_BOARD_UPDATE_FINISH_WAIT_REBOOT);
    CHECK(p4_board_update_confirm_postboot(&result, 1, 1));
    CHECK(result.status == P4_BOARD_UPDATE_INSTALLED);
}

static void test_finish_failures_preserve_transfer_status(void)
{
    struct fixture fixture;
    struct p4_board_update_plan plan;
    struct p4_board_update_result result;
    uint8_t scratch[P4_BOARD_UPDATE_CHUNK_SIZE];
    enum p4_board_update_finish_mode modes[] = {
        P4_BOARD_UPDATE_FINISH_NO_BEGIN,
        P4_BOARD_UPDATE_FINISH_INCOMPLETE,
        P4_BOARD_UPDATE_FINISH_COMMIT_UNKNOWN,
        P4_BOARD_UPDATE_FINISH_WAIT_REBOOT
    };
    int finish_statuses[] = {-17, 12};
    size_t mode_index, status_index;

    for (mode_index = 0u;
         mode_index < sizeof modes / sizeof modes[0]; ++mode_index) {
        for (status_index = 0u;
             status_index < sizeof finish_statuses / sizeof finish_statuses[0];
             ++status_index) {
            enum p4_board_update_status expected_status;
            enum p4_board_update_status returned_status;

            init_case(&fixture, &plan, scratch, TEST_IMAGE_SIZE);
            fixture.finish_status = finish_statuses[status_index];
            switch (modes[mode_index]) {
            case P4_BOARD_UPDATE_FINISH_NO_BEGIN:
                fixture.revalidate_status = -12;
                expected_status = P4_BOARD_UPDATE_SOURCE_CHANGED;
                break;
            case P4_BOARD_UPDATE_FINISH_INCOMPLETE:
                fixture.begin_result = P4_BOARD_UPDATE_CALL_REJECTED;
                expected_status = P4_BOARD_UPDATE_BEGIN_FAILED;
                break;
            case P4_BOARD_UPDATE_FINISH_COMMIT_UNKNOWN:
                fixture.end_result = P4_BOARD_UPDATE_CALL_AMBIGUOUS;
                expected_status = P4_BOARD_UPDATE_END_COMMIT_UNKNOWN;
                break;
            case P4_BOARD_UPDATE_FINISH_WAIT_REBOOT:
                expected_status = P4_BOARD_UPDATE_WAIT_REBOOT;
                break;
            default:
                CHECK(0);
                expected_status = P4_BOARD_UPDATE_INVALID_INPUT;
                break;
            }

            returned_status = p4_board_update_run(&plan, &result);
            CHECK(returned_status == expected_status);
            CHECK(result.status == expected_status);
            check_finish(&fixture, &result, modes[mode_index]);
            CHECK(fixture.begin_calls ==
                  (modes[mode_index] == P4_BOARD_UPDATE_FINISH_NO_BEGIN
                       ? 0u
                       : 1u));
            CHECK(fixture.write_calls ==
                  (modes[mode_index] == P4_BOARD_UPDATE_FINISH_COMMIT_UNKNOWN ||
                           modes[mode_index] == P4_BOARD_UPDATE_FINISH_WAIT_REBOOT
                       ? result.total_chunks
                       : 0u));
            CHECK(fixture.end_calls ==
                  (modes[mode_index] == P4_BOARD_UPDATE_FINISH_COMMIT_UNKNOWN ||
                           modes[mode_index] == P4_BOARD_UPDATE_FINISH_WAIT_REBOOT
                       ? 1u
                       : 0u));
        }
    }
}

static void test_postboot_rejects_mutated_result_without_changes(void)
{
    struct fixture fixture;
    struct p4_board_update_plan plan;
    struct p4_board_update_result valid;
    struct p4_board_update_result changed;
    struct p4_board_update_result before;
    uint8_t scratch[P4_BOARD_UPDATE_CHUNK_SIZE];

    init_case(&fixture, &plan, scratch, TEST_IMAGE_SIZE);
    CHECK(p4_board_update_run(&plan, &valid) == P4_BOARD_UPDATE_WAIT_REBOOT);
    check_finish(&fixture, &valid, P4_BOARD_UPDATE_FINISH_WAIT_REBOOT);

#define REJECT_UNCHANGED_MUTATION(field, value) do { \
    changed = valid; \
    changed.field = (value); \
    before = changed; \
    CHECK(!p4_board_update_confirm_postboot(&changed, 1, 1)); \
    CHECK(memcmp(&changed, &before, sizeof changed) == 0); \
} while (0)

    REJECT_UNCHANGED_MUTATION(status, P4_BOARD_UPDATE_POSTBOOT_FAILED);
    REJECT_UNCHANGED_MUTATION(status, P4_BOARD_UPDATE_INSTALLED);
    REJECT_UNCHANGED_MUTATION(lease_acquired, 0u);
    REJECT_UNCHANGED_MUTATION(lease_acquired, 2u);
    REJECT_UNCHANGED_MUTATION(lease_release_called, 0u);
    REJECT_UNCHANGED_MUTATION(lease_release_called, 2u);
    REJECT_UNCHANGED_MUTATION(lease_release_status, -1);
    REJECT_UNCHANGED_MUTATION(finish_mode, P4_BOARD_UPDATE_FINISH_NONE);
    REJECT_UNCHANGED_MUTATION(finish_mode, P4_BOARD_UPDATE_FINISH_NO_BEGIN);
    REJECT_UNCHANGED_MUTATION(finish_mode, P4_BOARD_UPDATE_FINISH_INCOMPLETE);
    REJECT_UNCHANGED_MUTATION(finish_mode,
                              P4_BOARD_UPDATE_FINISH_COMMIT_UNKNOWN);
    REJECT_UNCHANGED_MUTATION(
        finish_mode, (enum p4_board_update_finish_mode)99);
    REJECT_UNCHANGED_MUTATION(begin_attempted, 0u);
    REJECT_UNCHANGED_MUTATION(begin_attempted, 2u);
    REJECT_UNCHANGED_MUTATION(end_attempted, 0u);
    REJECT_UNCHANGED_MUTATION(end_attempted, 2u);
    REJECT_UNCHANGED_MUTATION(image_size, 0u);
    REJECT_UNCHANGED_MUTATION(image_size,
                              P4_BOARD_UPDATE_MAX_IMAGE_SIZE + 1u);
    REJECT_UNCHANGED_MUTATION(total_chunks, 0u);
    REJECT_UNCHANGED_MUTATION(total_chunks, valid.total_chunks - 1u);
    REJECT_UNCHANGED_MUTATION(acknowledged_chunks, 0u);
    REJECT_UNCHANGED_MUTATION(acknowledged_chunks, valid.total_chunks + 1u);
    REJECT_UNCHANGED_MUTATION(acknowledged_bytes, 0u);
    REJECT_UNCHANGED_MUTATION(acknowledged_bytes, valid.image_size + 1u);

#undef REJECT_UNCHANGED_MUTATION

    changed = valid;
    CHECK(!p4_board_update_confirm_postboot(&changed, 1, 0));
    CHECK(changed.status == P4_BOARD_UPDATE_POSTBOOT_FAILED);
}

int main(void)
{
    test_success_waits_for_postboot();
    test_all_preflight_gate_refusals();
    test_input_and_lease_refusals();
    test_alias_and_overflow_refusals();
    test_begin_terminal_policy();
    test_source_shape_failures();
    test_write_terminal_policy();
    test_end_terminal_policy();
    test_finish_failures_preserve_transfer_status();
    test_budget_input_and_clock_hook_refusals();
    test_work_deadline_before_begin_preserves_finish_reserve();
    test_aggregate_budget_is_not_renewed_and_late_write_ack_is_counted();
    test_end_ack_in_finish_reserve_is_not_installable();
    test_end_ack_after_total_deadline_is_not_installable();
    test_finish_callback_overrun_is_visible();
    test_clock_failure_backwards_clock_and_wrap();
    test_postboot_rejects_mutated_result_without_changes();
    printf("board update transaction: %lu checks\n", checks);
    return 0;
}
