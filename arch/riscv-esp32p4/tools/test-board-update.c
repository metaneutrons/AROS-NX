/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "update.h"

#define TEST_IMAGE_SIZE 500u

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
    unsigned int acquire_calls;
    unsigned int release_calls;
    unsigned int revalidate_calls;
    unsigned int cancel_calls;
    unsigned int begin_calls;
    unsigned int write_calls;
    unsigned int end_calls;
    unsigned int event_count;
    char events[32];
    int acquire_status;
    int release_status;
    int revalidate_status;
    int cancelled;
    int cancel_after_acquire;
    int cancel_after_first_write;
    int cancel_during_end;
    enum p4_board_update_call_result begin_result;
    enum p4_board_update_call_result write_result;
    enum p4_board_update_call_result end_result;
    int32_t begin_status;
    int32_t write_status;
    int32_t end_status;
};

static void event(struct fixture *fixture, char value)
{
    if (fixture->event_count < sizeof fixture->events)
        fixture->events[fixture->event_count++] = value;
}

static int read_source(void *context, uint32_t offset, uint8_t *destination,
                       size_t capacity, size_t *actual_size)
{
    struct fixture *fixture = context;
    size_t available;
    size_t count;

    if (offset == fixture->fail_read_offset)
        return -41;

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

static int acquire_maintenance(void *context)
{
    struct fixture *fixture = context;

    ++fixture->acquire_calls;
    event(fixture, 'L');
    if (fixture->cancel_after_acquire)
        fixture->cancelled = 1;
    return fixture->acquire_status;
}

static int release_maintenance(void *context)
{
    struct fixture *fixture = context;

    ++fixture->release_calls;
    event(fixture, 'R');
    return fixture->release_status;
}

static int revalidate_source(void *context, uint32_t expected_size)
{
    struct fixture *fixture = context;

    ++fixture->revalidate_calls;
    fixture->expected_begin_size = expected_size;
    event(fixture, 'S');
    return fixture->revalidate_status;
}

static int is_cancelled(void *context)
{
    struct fixture *fixture = context;

    ++fixture->cancel_calls;
    return fixture->cancelled;
}

static enum p4_board_update_call_result begin_update(void *context,
                                                      uint32_t image_size,
                                                      int32_t *remote_status)
{
    struct fixture *fixture = context;

    ++fixture->begin_calls;
    fixture->expected_begin_size = image_size;
    *remote_status = fixture->begin_status;
    event(fixture, 'B');
    return fixture->begin_result;
}

static enum p4_board_update_call_result write_update(void *context,
                                                      const uint8_t *data,
                                                      size_t data_size,
                                                      int32_t *remote_status)
{
    struct fixture *fixture = context;

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
                                                    int32_t *remote_status)
{
    struct fixture *fixture = context;

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
    fixture->begin_result = P4_BOARD_UPDATE_CALL_ACKNOWLEDGED;
    fixture->write_result = P4_BOARD_UPDATE_CALL_ACKNOWLEDGED;
    fixture->end_result = P4_BOARD_UPDATE_CALL_ACKNOWLEDGED;
    fixture->begin_status = 0;
    fixture->write_status = 0;
    fixture->end_status = 0;

    memset(plan, 0, sizeof *plan);
    plan->image_size = (uint32_t)image_size;
    plan->gates.manifest_accepted = 1u;
    plan->gates.inspection_passed = 1u;
    plan->gates.recovery_verified = 1u;
    plan->gates.target_compatible = 1u;
    plan->reader.context = fixture;
    plan->reader.context_size = sizeof *fixture;
    plan->reader.read = read_source;
    plan->ops.context = fixture;
    plan->ops.context_size = sizeof *fixture;
    plan->ops.acquire_maintenance = acquire_maintenance;
    plan->ops.release_maintenance = release_maintenance;
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
    CHECK(fixture.acquire_calls == 1u && fixture.release_calls == 1u);
    CHECK(fixture.revalidate_calls == 1u);
    CHECK(fixture.expected_begin_size == TEST_IMAGE_SIZE);
    CHECK(fixture.begin_calls == 1u && fixture.write_calls == 3u);
    CHECK(fixture.end_calls == 1u);
    CHECK(fixture.received_size == TEST_IMAGE_SIZE);
    CHECK(memcmp(fixture.source, fixture.received, TEST_IMAGE_SIZE) == 0);
    CHECK(fixture.events[0] == 'L' && fixture.events[1] == 'S');
    CHECK(fixture.events[2] == 'B');
    CHECK(fixture.events[fixture.event_count - 2u] == 'E');
    CHECK(fixture.events[fixture.event_count - 1u] == 'R');
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
            continue;
        }
        CHECK(p4_board_update_run(&plan, &result) ==
              P4_BOARD_UPDATE_PREFLIGHT_REFUSED);
        CHECK(result.begin_attempted == 0u);
        CHECK(fixture.acquire_calls == 0u && fixture.release_calls == 0u);
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
    check_no_mutation(&fixture);

    init_case(&fixture, &plan, scratch, 1u);
    plan.image_size = P4_BOARD_UPDATE_MAX_IMAGE_SIZE + 1u;
    CHECK(p4_board_update_run(&plan, &result) == P4_BOARD_UPDATE_INVALID_INPUT);
    check_no_mutation(&fixture);

    init_case(&fixture, &plan, scratch, 1u);
    plan.scratch_size = P4_BOARD_UPDATE_CHUNK_SIZE - 1u;
    CHECK(p4_board_update_run(&plan, &result) == P4_BOARD_UPDATE_INVALID_INPUT);
    check_no_mutation(&fixture);

    init_case(&fixture, &plan, scratch, 1u);
    fixture.acquire_status = -9;
    CHECK(p4_board_update_run(&plan, &result) == P4_BOARD_UPDATE_LEASE_FAILED);
    CHECK(fixture.acquire_calls == 1u && fixture.release_calls == 0u);
    check_no_mutation(&fixture);

    init_case(&fixture, &plan, scratch, 1u);
    fixture.cancelled = 1;
    CHECK(p4_board_update_run(&plan, &result) ==
          P4_BOARD_UPDATE_CANCELLED_BEFORE_BEGIN);
    CHECK(fixture.acquire_calls == 0u && fixture.release_calls == 0u);
    check_no_mutation(&fixture);

    init_case(&fixture, &plan, scratch, 1u);
    fixture.cancel_after_acquire = 1;
    CHECK(p4_board_update_run(&plan, &result) ==
          P4_BOARD_UPDATE_CANCELLED_BEFORE_BEGIN);
    CHECK(fixture.acquire_calls == 1u && fixture.release_calls == 1u);
    CHECK(fixture.revalidate_calls == 0u);
    check_no_mutation(&fixture);

    init_case(&fixture, &plan, scratch, 1u);
    fixture.revalidate_status = -12;
    CHECK(p4_board_update_run(&plan, &result) == P4_BOARD_UPDATE_SOURCE_CHANGED);
    CHECK(fixture.acquire_calls == 1u && fixture.release_calls == 1u);
    CHECK(fixture.revalidate_calls == 1u);
    check_no_mutation(&fixture);
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

static void test_begin_failure_and_ambiguity(void)
{
    struct fixture fixture;
    struct p4_board_update_plan plan;
    struct p4_board_update_result result;
    uint8_t scratch[P4_BOARD_UPDATE_CHUNK_SIZE];

    init_case(&fixture, &plan, scratch, TEST_IMAGE_SIZE);
    fixture.begin_result = P4_BOARD_UPDATE_CALL_REJECTED;
    fixture.begin_status = -88;
    CHECK(p4_board_update_run(&plan, &result) == P4_BOARD_UPDATE_BEGIN_FAILED);
    CHECK(result.callback_status == -88);
    CHECK(result.begin_attempted == 1u && result.end_attempted == 0u);
    CHECK(fixture.begin_calls == 1u && fixture.release_calls == 1u);
    CHECK(fixture.write_calls == 0u && fixture.end_calls == 0u);

    init_case(&fixture, &plan, scratch, 1u);
    fixture.begin_result = P4_BOARD_UPDATE_CALL_AMBIGUOUS;
    CHECK(p4_board_update_run(&plan, &result) ==
          P4_BOARD_UPDATE_BEGIN_UNKNOWN);
    CHECK(fixture.begin_calls == 1u && fixture.release_calls == 1u);
    CHECK(fixture.write_calls == 0u && fixture.end_calls == 0u);

    init_case(&fixture, &plan, scratch, 1u);
    fixture.begin_status = -3;
    CHECK(p4_board_update_run(&plan, &result) ==
          P4_BOARD_UPDATE_BEGIN_UNKNOWN);
    CHECK(fixture.begin_calls == 1u && fixture.write_calls == 0u);
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
    CHECK(fixture.end_calls == 0u && fixture.release_calls == 1u);

    init_case(&fixture, &plan, scratch, TEST_IMAGE_SIZE);
    fixture.fail_read_offset = P4_BOARD_UPDATE_CHUNK_SIZE;
    CHECK(p4_board_update_run(&plan, &result) ==
          P4_BOARD_UPDATE_SOURCE_READ_FAILED);
    CHECK(result.acknowledged_chunks == 1u);
    CHECK(result.acknowledged_bytes == P4_BOARD_UPDATE_CHUNK_SIZE);
    CHECK(fixture.write_calls == 1u && fixture.end_calls == 0u);

    init_case(&fixture, &plan, scratch, TEST_IMAGE_SIZE);
    fixture.source_size = TEST_IMAGE_SIZE + 1u;
    fixture.source[TEST_IMAGE_SIZE] = 0x7bu;
    CHECK(p4_board_update_run(&plan, &result) ==
          P4_BOARD_UPDATE_TRAILING_DATA);
    CHECK(result.acknowledged_bytes == TEST_IMAGE_SIZE);
    CHECK(fixture.write_calls == 3u && fixture.end_calls == 0u);

    init_case(&fixture, &plan, scratch, TEST_IMAGE_SIZE);
    fixture.cancel_after_first_write = 1;
    CHECK(p4_board_update_run(&plan, &result) ==
          P4_BOARD_UPDATE_CANCELLED_INCOMPLETE);
    CHECK(result.acknowledged_chunks == 1u);
    CHECK(result.acknowledged_bytes == P4_BOARD_UPDATE_CHUNK_SIZE);
    CHECK(fixture.write_calls == 1u && fixture.end_calls == 0u);
    CHECK(fixture.release_calls == 1u);
}

static void test_write_ack_is_never_retried(void)
{
    struct fixture fixture;
    struct p4_board_update_plan plan;
    struct p4_board_update_result result;
    uint8_t scratch[P4_BOARD_UPDATE_CHUNK_SIZE];

    init_case(&fixture, &plan, scratch, TEST_IMAGE_SIZE);
    fixture.write_result = P4_BOARD_UPDATE_CALL_AMBIGUOUS;
    fixture.write_status = -77;
    CHECK(p4_board_update_run(&plan, &result) ==
          P4_BOARD_UPDATE_WRITE_UNKNOWN);
    CHECK(result.acknowledged_chunks == 0u && result.acknowledged_bytes == 0u);
    CHECK(result.failed_chunk_index == 0u);
    CHECK(result.callback_status == -77);
    CHECK(fixture.write_calls == 1u && fixture.received_size == 238u);
    CHECK(fixture.end_calls == 0u && fixture.release_calls == 1u);

    init_case(&fixture, &plan, scratch, TEST_IMAGE_SIZE);
    fixture.write_status = 9;
    CHECK(p4_board_update_run(&plan, &result) ==
          P4_BOARD_UPDATE_WRITE_UNKNOWN);
    CHECK(result.acknowledged_chunks == 0u);
    CHECK(fixture.write_calls == 1u && fixture.end_calls == 0u);
}

static void test_end_unknown_and_postboot_confirmation(void)
{
    struct fixture fixture;
    struct p4_board_update_plan plan;
    struct p4_board_update_result result;
    uint8_t scratch[P4_BOARD_UPDATE_CHUNK_SIZE];

    init_case(&fixture, &plan, scratch, TEST_IMAGE_SIZE);
    fixture.end_result = P4_BOARD_UPDATE_CALL_REJECTED;
    fixture.end_status = -2;
    CHECK(p4_board_update_run(&plan, &result) ==
          P4_BOARD_UPDATE_END_COMMIT_UNKNOWN);
    CHECK(result.end_attempted == 1u && result.callback_status == -2);
    CHECK(result.acknowledged_bytes == TEST_IMAGE_SIZE);
    CHECK(fixture.end_calls == 1u && fixture.release_calls == 1u);
    CHECK(!p4_board_update_confirm_postboot(&result, 1, 1));

    init_case(&fixture, &plan, scratch, TEST_IMAGE_SIZE);
    fixture.end_status = 5;
    CHECK(p4_board_update_run(&plan, &result) ==
          P4_BOARD_UPDATE_END_COMMIT_UNKNOWN);
    CHECK(result.end_attempted == 1u && fixture.end_calls == 1u);

    init_case(&fixture, &plan, scratch, TEST_IMAGE_SIZE);
    fixture.release_status = -17;
    CHECK(p4_board_update_run(&plan, &result) == P4_BOARD_UPDATE_WAIT_REBOOT);
    CHECK(result.lease_release_called == 1u);
    CHECK(result.lease_release_status == -17);
    CHECK(!p4_board_update_confirm_postboot(&result, 1, 1));
    CHECK(result.status == P4_BOARD_UPDATE_WAIT_REBOOT);

    init_case(&fixture, &plan, scratch, TEST_IMAGE_SIZE);
    fixture.cancel_during_end = 1;
    CHECK(p4_board_update_run(&plan, &result) == P4_BOARD_UPDATE_WAIT_REBOOT);
    CHECK(fixture.end_calls == 1u);
    CHECK(p4_board_update_confirm_postboot(&result, 1, 1));
    CHECK(result.status == P4_BOARD_UPDATE_INSTALLED);
}

int main(void)
{
    test_success_waits_for_postboot();
    test_all_preflight_gate_refusals();
    test_input_and_lease_refusals();
    test_alias_and_overflow_refusals();
    test_begin_failure_and_ambiguity();
    test_source_shape_failures();
    test_write_ack_is_never_retried();
    test_end_unknown_and_postboot_confirmation();
    printf("board update transaction: %lu checks\n", checks);
    return 0;
}
