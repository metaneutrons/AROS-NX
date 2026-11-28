/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../boardtool/ota_client.h"

#define TEST_TOKEN UINT32_C(0x10203040)
#define TEST_IMAGE_SIZE UINT32_C(8)
#define MAX_EVENTS 32u

static const uint8_t test_image[TEST_IMAGE_SIZE] = {
    0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80
};
static const uint8_t multi_chunk_test_image[
    P4_BOARD_UPDATE_CHUNK_SIZE * 2u] = { 0 };

static unsigned int assertion_count;
static unsigned int test_count;

#define CHECK(condition) do { \
    ++assertion_count; \
    if (!(condition)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, \
            #condition); \
        exit(EXIT_FAILURE); \
    } \
} while (0)

struct poll_event {
    enum p4_wifi_ota_job_state returned_state;
    struct p4_wifi_ota_observation observation;
};

struct fake_owner {
    uint32_t submit_token;
    unsigned int submit_calls;
    const uint8_t *submitted_image;
    uint32_t submitted_size;

    struct poll_event events[MAX_EVENTS];
    unsigned int event_count;
    unsigned int event_index;
    unsigned int poll_calls;
    uint32_t poll_tokens[MAX_EVENTS];

    int cancel_returns[MAX_EVENTS];
    unsigned int cancel_return_count;
    int default_cancel_return;
    unsigned int cancel_calls;
    uint32_t cancel_tokens[MAX_EVENTS];

    int discard_returns[MAX_EVENTS];
    unsigned int discard_return_count;
    int default_discard_return;
    unsigned int discard_calls;
    uint32_t discard_tokens[MAX_EVENTS];

    unsigned int callback_overflow;
};

static void fake_owner_init(struct fake_owner *fake)
{
    memset(fake, 0, sizeof(*fake));
    fake->submit_token = TEST_TOKEN;
    fake->default_cancel_return = 0;
    fake->default_discard_return = 1;
}

static struct p4_wifi_ota_request test_request(void)
{
    struct p4_wifi_ota_request request;
    memset(&request, 0, sizeof(request));
    request.image = test_image;
    request.image_size = TEST_IMAGE_SIZE;
    return request;
}

static uint32_t fake_submit(void *context,
    const struct p4_wifi_ota_request *request)
{
    struct fake_owner *fake = (struct fake_owner *)context;
    ++fake->submit_calls;
    if (request) {
        fake->submitted_image = request->image;
        fake->submitted_size = request->image_size;
    }
    return fake->submit_token;
}

static enum p4_wifi_ota_job_state fake_poll(void *context, uint32_t token,
    struct p4_wifi_ota_observation *observation)
{
    struct fake_owner *fake = (struct fake_owner *)context;
    unsigned int call = fake->poll_calls++;
    if (call < MAX_EVENTS) fake->poll_tokens[call] = token;
    else fake->callback_overflow = 1;

    if (fake->event_index >= fake->event_count) {
        memset(observation, 0, sizeof(*observation));
        return P4_WIFI_OTA_JOB_BUSY;
    }
    *observation = fake->events[fake->event_index].observation;
    return fake->events[fake->event_index++].returned_state;
}

static int fake_cancel(void *context, uint32_t token)
{
    struct fake_owner *fake = (struct fake_owner *)context;
    unsigned int call = fake->cancel_calls++;
    if (call < MAX_EVENTS) fake->cancel_tokens[call] = token;
    else fake->callback_overflow = 1;
    if (call < fake->cancel_return_count) return fake->cancel_returns[call];
    return fake->default_cancel_return;
}

static int fake_discard(void *context, uint32_t token)
{
    struct fake_owner *fake = (struct fake_owner *)context;
    unsigned int call = fake->discard_calls++;
    if (call < MAX_EVENTS) fake->discard_tokens[call] = token;
    else fake->callback_overflow = 1;
    if (call < fake->discard_return_count)
        return fake->discard_returns[call];
    return fake->default_discard_return;
}

static struct board_ota_client_ops complete_ops(void)
{
    struct board_ota_client_ops ops;
    ops.submit = fake_submit;
    ops.poll = fake_poll;
    ops.cancel = fake_cancel;
    ops.discard = fake_discard;
    return ops;
}

static struct poll_event event_for(enum p4_wifi_ota_job_state state,
    uint32_t token, uint32_t generation, uint32_t acknowledged_bytes)
{
    struct poll_event event;
    memset(&event, 0, sizeof(event));
    event.returned_state = state;
    event.observation.token = token;
    event.observation.generation = generation;
    event.observation.state = state;
    event.observation.acknowledged_bytes = acknowledged_bytes;
    return event;
}

static struct poll_event done_event(uint32_t token, uint32_t generation,
    uint32_t acknowledged_bytes, enum p4_board_update_status status)
{
    struct poll_event event = event_for(P4_WIFI_OTA_JOB_DONE, token,
        generation, acknowledged_bytes);
    event.observation.result_valid = 1;
    event.observation.result.status = status;
    event.observation.result.acknowledged_bytes = acknowledged_bytes;
    if (status == P4_BOARD_UPDATE_WAIT_REBOOT) {
        event.observation.result.image_size = TEST_IMAGE_SIZE;
        event.observation.result.total_chunks = 1;
        event.observation.result.acknowledged_chunks = 1;
        event.observation.result.lease_acquired = 1;
        event.observation.result.lease_release_called = 1;
        event.observation.result.lease_release_status = 0;
        event.observation.result.begin_attempted = 1;
        event.observation.result.end_attempted = 1;
        event.observation.result.finish_mode =
            P4_BOARD_UPDATE_FINISH_WAIT_REBOOT;
    } else {
        event.observation.result.image_size = 0;
        event.observation.result.finish_mode = status ==
            P4_BOARD_UPDATE_CANCELLED_BEFORE_BEGIN ?
            P4_BOARD_UPDATE_FINISH_NO_BEGIN : P4_BOARD_UPDATE_FINISH_NONE;
    }
    return event;
}

static void enqueue(struct fake_owner *fake, const struct poll_event *event)
{
    if (fake->event_count >= MAX_EVENTS) {
        fprintf(stderr, "test fixture event capacity exceeded\n");
        exit(EXIT_FAILURE);
    }
    fake->events[fake->event_count++] = *event;
}

static void init_started(struct board_ota_client *client,
    struct fake_owner *fake, struct p4_wifi_ota_request *request,
    struct board_ota_client_ops *ops)
{
    fake_owner_init(fake);
    board_ota_client_init(client);
    *request = test_request();
    *ops = complete_ops();
    CHECK(board_ota_client_start(client, ops, fake, request) == 1);
    CHECK(client->phase == BOARD_OTA_CLIENT_PENDING);
    CHECK(fake->submit_calls == 1);
    CHECK(fake->submitted_image == test_image);
    CHECK(fake->submitted_size == TEST_IMAGE_SIZE);
}

static void test_busy_retry_does_not_resubmit(void)
{
    struct board_ota_client client;
    struct fake_owner fake;
    struct p4_wifi_ota_request request;
    struct board_ota_client_ops ops;
    struct poll_event busy = event_for(P4_WIFI_OTA_JOB_BUSY,
        UINT32_C(0xdeadbeef), 0, UINT32_MAX);

    init_started(&client, &fake, &request, &ops);
    busy.observation.state = P4_WIFI_OTA_JOB_INVALID;
    enqueue(&fake, &busy);
    enqueue(&fake, &busy);

    CHECK(board_ota_client_start(&client, &ops, &fake, &request) == 0);
    CHECK(board_ota_client_step(&client, 0) == BOARD_OTA_CLIENT_PENDING);
    CHECK(board_ota_client_step(&client, 0) == BOARD_OTA_CLIENT_PENDING);
    CHECK(fake.submit_calls == 1);
    CHECK(fake.poll_calls == 2);
    CHECK(fake.poll_tokens[0] == TEST_TOKEN);
    CHECK(fake.poll_tokens[1] == TEST_TOKEN);
    CHECK(fake.cancel_calls == 0);
    CHECK(fake.discard_calls == 0);
    CHECK(client.observation_unconfirmed == 0);
    CHECK(client.generation == 0);
    CHECK(client.acknowledged_bytes == 0);
    CHECK(fake.callback_overflow == 0);
}

static void test_sticky_cancel_retries_until_accepted(void)
{
    struct board_ota_client client;
    struct fake_owner fake;
    struct p4_wifi_ota_request request;
    struct board_ota_client_ops ops;
    struct poll_event queued = event_for(P4_WIFI_OTA_JOB_QUEUED,
        TEST_TOKEN, 7, 0);
    struct poll_event busy = event_for(P4_WIFI_OTA_JOB_BUSY,
        TEST_TOKEN, 7, 0);

    init_started(&client, &fake, &request, &ops);
    fake.cancel_returns[0] = 0;
    fake.cancel_returns[1] = 0;
    fake.cancel_returns[2] = 1;
    fake.cancel_return_count = 3;
    queued.observation.cancel_requested = 1;
    enqueue(&fake, &busy);
    enqueue(&fake, &busy);
    enqueue(&fake, &queued);
    enqueue(&fake, &busy);

    CHECK(board_ota_client_step(&client, 1) == BOARD_OTA_CLIENT_PENDING);
    CHECK(client.cancel_requested == 1);
    CHECK(client.cancel_accepted == 0);
    CHECK(board_ota_client_step(&client, 0) == BOARD_OTA_CLIENT_PENDING);
    CHECK(client.cancel_requested == 1);
    CHECK(client.cancel_accepted == 0);
    CHECK(board_ota_client_step(&client, 0) == BOARD_OTA_CLIENT_PENDING);
    CHECK(client.cancel_accepted == 1);
    CHECK(board_ota_client_step(&client, 0) == BOARD_OTA_CLIENT_PENDING);
    CHECK(fake.cancel_calls == 3);
    CHECK(fake.cancel_tokens[0] == TEST_TOKEN);
    CHECK(fake.cancel_tokens[1] == TEST_TOKEN);
    CHECK(fake.cancel_tokens[2] == TEST_TOKEN);
    CHECK(fake.poll_calls == 4);
    CHECK(fake.submit_calls == 1);
    CHECK(fake.discard_calls == 0);
    CHECK(fake.callback_overflow == 0);
}

static void test_missing_hooks_and_refused_submit_are_one_shot(void)
{
    struct board_ota_client client;
    struct fake_owner fake;
    struct p4_wifi_ota_request request;
    struct board_ota_client_ops ops;
    unsigned int missing;

    for (missing = 0; missing < 4; ++missing) {
        fake_owner_init(&fake);
        board_ota_client_init(&client);
        request = test_request();
        ops = complete_ops();
        if (missing == 0) ops.submit = NULL;
        else if (missing == 1) ops.poll = NULL;
        else if (missing == 2) ops.cancel = NULL;
        else ops.discard = NULL;
        CHECK(board_ota_client_start(&client, &ops, &fake, &request) == 0);
        CHECK(client.phase == BOARD_OTA_CLIENT_IDLE);
        CHECK(fake.submit_calls == 0);
    }

    fake_owner_init(&fake);
    board_ota_client_init(&client);
    request = test_request();
    ops = complete_ops();
    fake.submit_token = 0;
    CHECK(board_ota_client_start(&client, &ops, &fake, &request) == 0);
    CHECK(client.phase == BOARD_OTA_CLIENT_REFUSED);
    CHECK(fake.submit_calls == 1);
    CHECK(board_ota_client_step(&client, 1) == BOARD_OTA_CLIENT_REFUSED);
    CHECK(board_ota_client_start(&client, &ops, &fake, &request) == 0);
    CHECK(fake.submit_calls == 1);
    CHECK(fake.poll_calls == 0);
    CHECK(fake.cancel_calls == 0);
    CHECK(fake.discard_calls == 0);
    CHECK(fake.callback_overflow == 0);
}

static void test_malformed_observations_never_finish_or_discard(void)
{
    struct board_ota_client client;
    struct fake_owner fake;
    struct p4_wifi_ota_request request;
    struct board_ota_client_ops ops;
    struct poll_event event;
    unsigned int malformed_count = 0;
    unsigned int i;

    init_started(&client, &fake, &request, &ops);
    fake.default_cancel_return = 0;
    event = event_for(P4_WIFI_OTA_JOB_QUEUED, TEST_TOKEN, 9, 2);
    enqueue(&fake, &event);
    CHECK(board_ota_client_step(&client, 1) == BOARD_OTA_CLIENT_PENDING);
    CHECK(client.generation == 9);
    CHECK(client.acknowledged_bytes == 2);

#define BAD_EVENT_SETUP(statement) do { \
    event = event_for(P4_WIFI_OTA_JOB_QUEUED, TEST_TOKEN, 9, 2); \
    statement; \
    enqueue(&fake, &event); \
    ++malformed_count; \
} while (0)

    BAD_EVENT_SETUP(event.observation.token = TEST_TOKEN + 1u);
    BAD_EVENT_SETUP(event.observation.generation = 0);
    BAD_EVENT_SETUP(event.observation.generation = 10);
    BAD_EVENT_SETUP(event.observation.acknowledged_bytes = TEST_IMAGE_SIZE + 1u);
    BAD_EVENT_SETUP(event.observation.acknowledged_bytes = 1);
    BAD_EVENT_SETUP(event.returned_state = P4_WIFI_OTA_JOB_INVALID;
        event.observation.state = P4_WIFI_OTA_JOB_INVALID);
    BAD_EVENT_SETUP(event.observation.state = P4_WIFI_OTA_JOB_RUNNING);
    BAD_EVENT_SETUP(event.observation.cancel_requested = 2);
    BAD_EVENT_SETUP(event.observation.result_valid = 2);
    BAD_EVENT_SETUP(event.observation.result_valid = 1);

    event = event_for(P4_WIFI_OTA_JOB_DONE, TEST_TOKEN, 9, TEST_IMAGE_SIZE);
    enqueue(&fake, &event);
    ++malformed_count;

    event = done_event(TEST_TOKEN, 9, TEST_IMAGE_SIZE,
        P4_BOARD_UPDATE_WAIT_REBOOT);
    event.observation.acknowledged_bytes = 2;
    enqueue(&fake, &event);
    ++malformed_count;

#define BAD_DONE_SETUP(statement) do { \
    event = done_event(TEST_TOKEN, 9, TEST_IMAGE_SIZE, \
        P4_BOARD_UPDATE_WAIT_REBOOT); \
    statement; \
    enqueue(&fake, &event); \
    ++malformed_count; \
} while (0)

    BAD_DONE_SETUP(event.observation.result.status =
        (enum p4_board_update_status)(P4_BOARD_UPDATE_CLOCK_FAILED + 1));
    BAD_DONE_SETUP(event.observation.result.status = P4_BOARD_UPDATE_INSTALLED);
    BAD_DONE_SETUP(event.observation.result.status =
        P4_BOARD_UPDATE_POSTBOOT_FAILED);
    BAD_DONE_SETUP(event.observation.result.finish_mode =
        (enum p4_board_update_finish_mode)(P4_BOARD_UPDATE_FINISH_WAIT_REBOOT + 1));
    BAD_DONE_SETUP(event.observation.result.budget_error =
        (enum p4_board_update_budget_error)(P4_BOARD_UPDATE_BUDGET_CLOCK + 1));
    BAD_DONE_SETUP(event.observation.result.lease_acquired = 2);
    BAD_DONE_SETUP(event.observation.result.lease_release_called = 2);
    BAD_DONE_SETUP(event.observation.result.begin_attempted = 2);
    BAD_DONE_SETUP(event.observation.result.end_attempted = 2);
    BAD_DONE_SETUP(event.observation.result.lease_acquired = 0);
    BAD_DONE_SETUP(event.observation.result.begin_attempted = 0);
    BAD_DONE_SETUP(event.observation.result.acknowledged_chunks = 2);
    BAD_DONE_SETUP(event.observation.result.total_chunks =
        P4_BOARD_UPDATE_MAX_CHUNKS + 1u);
    BAD_DONE_SETUP(event.observation.result.total_chunks = 2);
    BAD_DONE_SETUP(event.observation.result.acknowledged_chunks = 0);
    BAD_DONE_SETUP(event.observation.result.image_size =
        P4_BOARD_UPDATE_MAX_IMAGE_SIZE + 1u);
    BAD_DONE_SETUP(event.observation.result.acknowledged_bytes =
        TEST_IMAGE_SIZE + 1u);

    for (i = 0; i < malformed_count; ++i) {
        CHECK(board_ota_client_step(&client, 0) == BOARD_OTA_CLIENT_PENDING);
        CHECK(client.phase == BOARD_OTA_CLIENT_PENDING);
        CHECK(client.generation == 9);
        CHECK(client.acknowledged_bytes == 2);
        CHECK(client.observation_unconfirmed == 1);
        CHECK(client.cancel_requested == 1);
        CHECK(fake.discard_calls == 0);
        CHECK(client.terminal.state == P4_WIFI_OTA_JOB_INVALID);
    }
    CHECK(fake.submit_calls == 1);
    CHECK(fake.poll_calls == malformed_count + 1u);
    CHECK(fake.cancel_calls == malformed_count + 1u);
    CHECK(fake.discard_calls == 0);
    CHECK(client.phase == BOARD_OTA_CLIENT_PENDING);
    CHECK(fake.callback_overflow == 0);
}

static void test_running_to_queued_is_invalid(void)
{
    struct board_ota_client client;
    struct fake_owner fake;
    struct p4_wifi_ota_request request;
    struct board_ota_client_ops ops;
    struct poll_event running = event_for(P4_WIFI_OTA_JOB_RUNNING,
        TEST_TOKEN, 12, 4);
    struct poll_event queued = event_for(P4_WIFI_OTA_JOB_QUEUED,
        TEST_TOKEN, 12, 4);

    init_started(&client, &fake, &request, &ops);
    enqueue(&fake, &running);
    enqueue(&fake, &queued);
    CHECK(board_ota_client_step(&client, 0) == BOARD_OTA_CLIENT_PENDING);
    CHECK(client.saw_running == 1);
    CHECK(client.generation == 12);
    CHECK(client.acknowledged_bytes == 4);
    CHECK(board_ota_client_step(&client, 0) == BOARD_OTA_CLIENT_PENDING);
    CHECK(client.phase == BOARD_OTA_CLIENT_PENDING);
    CHECK(client.observation_unconfirmed == 1);
    CHECK(client.cancel_requested == 1);
    CHECK(client.generation == 12);
    CHECK(client.acknowledged_bytes == 4);
    CHECK(fake.discard_calls == 0);
    CHECK(fake.submit_calls == 1);
    CHECK(fake.callback_overflow == 0);
}

static void test_done_copies_terminal_and_retries_discard(void)
{
    struct board_ota_client client;
    struct fake_owner fake;
    struct p4_wifi_ota_request request;
    struct board_ota_client_ops ops;
    struct poll_event done = done_event(TEST_TOKEN, 15, TEST_IMAGE_SIZE,
        P4_BOARD_UPDATE_WAIT_REBOOT);

    init_started(&client, &fake, &request, &ops);
    fake.discard_returns[0] = 0;
    fake.discard_returns[1] = 1;
    fake.discard_return_count = 2;
    enqueue(&fake, &done);

    CHECK(board_ota_client_step(&client, 0) == BOARD_OTA_CLIENT_DISCARDING);
    CHECK(client.terminal.token == TEST_TOKEN);
    CHECK(client.terminal.generation == 15);
    CHECK(client.terminal.state == P4_WIFI_OTA_JOB_DONE);
    CHECK(client.terminal.acknowledged_bytes == TEST_IMAGE_SIZE);
    CHECK(client.terminal.result_valid == 1);
    CHECK(client.terminal.result.status == P4_BOARD_UPDATE_WAIT_REBOOT);
    CHECK(client.terminal.result.status != P4_BOARD_UPDATE_INSTALLED);
    CHECK(client.terminal.result.image_size == TEST_IMAGE_SIZE);
    CHECK(client.terminal.result.lease_acquired == 1);
    CHECK(client.terminal.result.lease_release_called == 1);
    CHECK(client.terminal.result.finish_mode ==
        P4_BOARD_UPDATE_FINISH_WAIT_REBOOT);
    CHECK(client.terminal.result.begin_attempted == 1);
    CHECK(client.terminal.result.end_attempted == 1);
    CHECK(fake.poll_calls == 1);
    CHECK(fake.cancel_calls == 0);
    CHECK(fake.discard_calls == 0);

    /* The stored result is a value copy, independent of callback storage. */
    fake.events[0].observation.result.status = P4_BOARD_UPDATE_INSTALLED;
    CHECK(board_ota_client_step(&client, 1) == BOARD_OTA_CLIENT_DISCARDING);
    CHECK(client.cancel_requested == 1);
    CHECK(client.terminal.result.status == P4_BOARD_UPDATE_WAIT_REBOOT);
    CHECK(fake.discard_calls == 1);
    CHECK(fake.discard_tokens[0] == TEST_TOKEN);
    CHECK(fake.poll_calls == 1);
    CHECK(fake.cancel_calls == 0);
    CHECK(fake.submit_calls == 1);

    /* Failed discard retains terminal evidence and retries only discard. */
    CHECK(board_ota_client_step(&client, 0) == BOARD_OTA_CLIENT_FINISHED);
    CHECK(client.terminal.result.status == P4_BOARD_UPDATE_WAIT_REBOOT);
    CHECK(fake.discard_calls == 2);
    CHECK(fake.discard_tokens[1] == TEST_TOKEN);
    CHECK(fake.poll_calls == 1);
    CHECK(fake.cancel_calls == 0);
    CHECK(fake.submit_calls == 1);
    CHECK(board_ota_client_step(&client, 0) == BOARD_OTA_CLIENT_FINISHED);
    CHECK(fake.discard_calls == 2);
    CHECK(fake.callback_overflow == 0);
}

static void test_zero_sized_wait_reboot_is_rejected(void)
{
    struct board_ota_client client;
    struct fake_owner fake;
    struct p4_wifi_ota_request request;
    struct board_ota_client_ops ops;
    struct poll_event done = done_event(TEST_TOKEN, 25, 0,
        P4_BOARD_UPDATE_WAIT_REBOOT);

    init_started(&client, &fake, &request, &ops);
    done.observation.result.image_size = 0;
    done.observation.result.total_chunks = 0;
    done.observation.result.acknowledged_chunks = 0;
    done.observation.result.acknowledged_bytes = 0;
    enqueue(&fake, &done);

    CHECK(board_ota_client_step(&client, 0) == BOARD_OTA_CLIENT_PENDING);
    CHECK(client.phase == BOARD_OTA_CLIENT_PENDING);
    CHECK(client.observation_unconfirmed == 1);
    CHECK(client.cancel_requested == 1);
    CHECK(fake.poll_calls == 1);
    CHECK(fake.discard_calls == 0);
    CHECK(fake.submit_calls == 1);
    CHECK(fake.callback_overflow == 0);
}

static void test_end_attempt_with_partial_ack_is_rejected(void)
{
    const uint32_t image_size = P4_BOARD_UPDATE_CHUNK_SIZE * 2u;
    struct board_ota_client client;
    struct fake_owner fake;
    struct p4_wifi_ota_request request;
    struct board_ota_client_ops ops;
    struct poll_event done = event_for(P4_WIFI_OTA_JOB_DONE, TEST_TOKEN,
        26, P4_BOARD_UPDATE_CHUNK_SIZE);

    fake_owner_init(&fake);
    board_ota_client_init(&client);
    request = test_request();
    request.image = multi_chunk_test_image;
    request.image_size = image_size;
    ops = complete_ops();
    CHECK(board_ota_client_start(&client, &ops, &fake, &request) == 1);

    done.observation.result_valid = 1;
    done.observation.result.status = P4_BOARD_UPDATE_WAIT_REBOOT;
    done.observation.result.image_size = image_size;
    done.observation.result.total_chunks = 2;
    done.observation.result.acknowledged_chunks = 1;
    done.observation.result.acknowledged_bytes = P4_BOARD_UPDATE_CHUNK_SIZE;
    done.observation.result.lease_acquired = 1;
    done.observation.result.lease_release_called = 1;
    done.observation.result.begin_attempted = 1;
    done.observation.result.end_attempted = 1;
    done.observation.result.finish_mode = P4_BOARD_UPDATE_FINISH_WAIT_REBOOT;
    enqueue(&fake, &done);

    CHECK(board_ota_client_step(&client, 0) == BOARD_OTA_CLIENT_PENDING);
    CHECK(client.phase == BOARD_OTA_CLIENT_PENDING);
    CHECK(client.observation_unconfirmed == 1);
    CHECK(client.cancel_requested == 1);
    CHECK(fake.submit_calls == 1);
    CHECK(fake.poll_calls == 1);
    CHECK(fake.discard_calls == 0);
    CHECK(fake.callback_overflow == 0);
}

static void test_early_done_accepts_zero_image_without_attempts(void)
{
    static const enum p4_board_update_status statuses[] = {
        P4_BOARD_UPDATE_CANCELLED_BEFORE_BEGIN,
        P4_BOARD_UPDATE_LEASE_FAILED,
        P4_BOARD_UPDATE_INVALID_INPUT
    };
    unsigned int i;

    for (i = 0; i < sizeof(statuses) / sizeof(statuses[0]); ++i) {
        struct board_ota_client client;
        struct fake_owner fake;
        struct p4_wifi_ota_request request;
        struct board_ota_client_ops ops;
        struct poll_event done = done_event(TEST_TOKEN, 20u + i, 0,
            statuses[i]);

        init_started(&client, &fake, &request, &ops);
        done.observation.result.image_size = 0;
        done.observation.result.acknowledged_bytes = 0;
        done.observation.result.begin_attempted = 0;
        done.observation.result.end_attempted = 0;
        enqueue(&fake, &done);

        CHECK(board_ota_client_step(&client, 0) ==
            BOARD_OTA_CLIENT_DISCARDING);
        CHECK(client.terminal.result.status == statuses[i]);
        CHECK(client.terminal.result.image_size == 0);
        CHECK(client.terminal.result.begin_attempted == 0);
        CHECK(client.terminal.result.end_attempted == 0);
        CHECK(client.terminal.result.acknowledged_bytes == 0);
        CHECK(client.terminal.acknowledged_bytes == 0);
        CHECK(client.terminal.result.finish_mode == (statuses[i] ==
            P4_BOARD_UPDATE_CANCELLED_BEFORE_BEGIN ?
            P4_BOARD_UPDATE_FINISH_NO_BEGIN : P4_BOARD_UPDATE_FINISH_NONE));
        CHECK(fake.discard_calls == 0);
        CHECK(board_ota_client_step(&client, 0) == BOARD_OTA_CLIENT_FINISHED);
        CHECK(fake.discard_calls == 1);
        CHECK(fake.poll_calls == 1);
        CHECK(fake.cancel_calls == 0);
        CHECK(fake.submit_calls == 1);
        CHECK(fake.callback_overflow == 0);
    }
}

static void run_test(const char *name, void (*test)(void))
{
    unsigned int before = assertion_count;
    test();
    ++test_count;
    printf("PASS %s (%u checks)\n", name, assertion_count - before);
}

int main(void)
{
    run_test("BUSY retry without resubmit", test_busy_retry_does_not_resubmit);
    run_test("sticky cancellation retries", test_sticky_cancel_retries_until_accepted);
    run_test("missing hooks and refused submit", test_missing_hooks_and_refused_submit_are_one_shot);
    run_test("malformed observations remain pending", test_malformed_observations_never_finish_or_discard);
    run_test("RUNNING to QUEUED rejection", test_running_to_queued_is_invalid);
    run_test("DONE copy and discard retry", test_done_copies_terminal_and_retries_discard);
    run_test("zero-sized WAIT_REBOOT rejection", test_zero_sized_wait_reboot_is_rejected);
    run_test("END with partial acknowledged image rejection", test_end_attempt_with_partial_ack_is_rejected);
    run_test("early zero-sized terminal results", test_early_done_accepts_zero_image_without_attempts);
    printf("PASS board OTA client: %u tests, %u checks\n",
        test_count, assertion_count);
    return EXIT_SUCCESS;
}
