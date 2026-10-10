/*
 * Host tests for the production bounded discovery transaction. Only the
 * adapter hooks are mocked; the client, wire decoder and inventory validator
 * are linked from their production translation units.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "discovery_client.h"

#define SENTINEL 0xa5u
#define EVENT_CAP 256u
#define TEST_TOKEN 0x13579bdfu
#define TEST_GENERATION 0x2468ace0u
#define TEST_UID 0x10293847u
#define TEST_NONCE 0x55667788u

static const char *active_case;
static unsigned checks_run;
static unsigned cases_run;

static int fail(const char *message)
{
    fprintf(stderr, "FAIL %s: %s\n", active_case ? active_case : "setup", message);
    return 0;
}

#define CHECK(condition, message) do { \
    ++checks_run; \
    if (!(condition)) return fail(message); \
} while (0)

static void begin_case(const char *name)
{
    active_case = name;
    ++cases_run;
}

struct fake_adapter {
    uint32_t now;
    uint32_t wait_step;
    unsigned clock_calls;
    unsigned clock_fail_call;
    unsigned cancelled_calls;
    unsigned cancel_on_check;
    unsigned cancel_on_wait;
    int cancelled_flag;

    uint32_t request_token;
    unsigned request_calls;
    unsigned pre_lease_states[EVENT_CAP];
    size_t pre_lease_state_count;
    unsigned pre_lease_default;
    uint32_t pre_lease_delays[EVENT_CAP];
    unsigned pre_lease_calls;

    int discover_result;
    unsigned discover_calls;
    unsigned discovery_states[EVENT_CAP];
    size_t discovery_state_count;
    unsigned discovery_default;
    uint32_t discovery_delays[EVENT_CAP];
    unsigned discovery_calls;

    int release_results[EVENT_CAP];
    size_t release_result_count;
    int release_default;
    unsigned release_calls;
    unsigned post_lease_states[EVENT_CAP];
    size_t post_lease_state_count;
    unsigned post_lease_default;
    uint32_t post_lease_delays[EVENT_CAP];
    unsigned post_lease_calls;

    unsigned wait_calls;
    struct board_client_reply reply;
    struct board_discovery_snapshot source_snapshot;
    struct board_client_observation *output;
    uint8_t initial_output[sizeof(struct board_client_observation)];
    int output_changed_in_callback;
};

static void make_snapshot(struct board_discovery_snapshot *snapshot)
{
    struct board_partition *part;

    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->physical_bytes = 0x01000000u;
    snapshot->layout.flash_bytes = 0x00800000u;
    snapshot->layout.count = 1u;
    snapshot->running = 0u;
    snapshot->boot = 0u;
    snapshot->update = 0u;
    snapshot->running_state = 3u;
    snapshot->security_known = BOARD_DISCOVERY_SECURITY_ALL;
    snapshot->security_enabled = BOARD_DISCOVERY_SECURITY_ENCRYPTION |
        BOARD_DISCOVERY_SECURITY_ANTI_ROLLBACK;

    part = &snapshot->layout.partitions[0];
    part->offset = 0x00010000u;
    part->size = 0x00001000u;
    part->type = 0u;
    part->subtype = 0x10u;
    memcpy(part->label, "app", 4u);
}

static int same_snapshot(const struct board_discovery_snapshot *a,
    const struct board_discovery_snapshot *b)
{
    size_t i;

    if (a->physical_bytes != b->physical_bytes ||
        a->layout.flash_bytes != b->layout.flash_bytes ||
        a->layout.count != b->layout.count ||
        a->layout.md5_record_present != b->layout.md5_record_present ||
        a->running != b->running || a->boot != b->boot ||
        a->update != b->update || a->running_state != b->running_state ||
        a->security_known != b->security_known ||
        a->security_enabled != b->security_enabled)
        return 0;

    for (i = 0; i < a->layout.count; ++i) {
        const struct board_partition *left = &a->layout.partitions[i];
        const struct board_partition *right = &b->layout.partitions[i];
        if (left->offset != right->offset || left->size != right->size ||
            left->flags != right->flags || left->type != right->type ||
            left->subtype != right->subtype ||
            memcmp(left->label, right->label, sizeof(left->label)) != 0)
            return 0;
    }
    return 1;
}

static void observe_output(struct fake_adapter *fake)
{
    if (memcmp(fake->initial_output, fake->output, sizeof(fake->initial_output)) != 0)
        fake->output_changed_in_callback = 1;
}

static int fake_clock(void *context, uint32_t *milliseconds)
{
    struct fake_adapter *fake = (struct fake_adapter *)context;
    observe_output(fake);
    ++fake->clock_calls;
    if (fake->clock_calls == fake->clock_fail_call) return 0;
    *milliseconds = fake->now;
    return 1;
}

static void fake_wait(void *context)
{
    struct fake_adapter *fake = (struct fake_adapter *)context;
    observe_output(fake);
    ++fake->wait_calls;
    fake->now += fake->wait_step;
    if (fake->wait_calls == fake->cancel_on_wait) fake->cancelled_flag = 1;
}

static int fake_cancelled(void *context)
{
    struct fake_adapter *fake = (struct fake_adapter *)context;
    observe_output(fake);
    ++fake->cancelled_calls;
    if (fake->cancelled_calls == fake->cancel_on_check) fake->cancelled_flag = 1;
    return fake->cancelled_flag;
}

static uint32_t fake_request(void *context)
{
    struct fake_adapter *fake = (struct fake_adapter *)context;
    observe_output(fake);
    ++fake->request_calls;
    return fake->request_token;
}

static unsigned fake_lease_poll(void *context, uint32_t token)
{
    struct fake_adapter *fake = (struct fake_adapter *)context;
    unsigned state;
    unsigned index;

    observe_output(fake);
    if (token != TEST_TOKEN) return BOARD_CLIENT_LEASE_FAILED;
    if (fake->release_calls == 0u) {
        index = fake->pre_lease_calls++;
        state = index < fake->pre_lease_state_count ?
            fake->pre_lease_states[index] : fake->pre_lease_default;
        if (index < EVENT_CAP) fake->now += fake->pre_lease_delays[index];
        return state;
    }

    index = fake->post_lease_calls++;
    state = index < fake->post_lease_state_count ?
        fake->post_lease_states[index] : fake->post_lease_default;
    if (index < EVENT_CAP) fake->now += fake->post_lease_delays[index];
    return state;
}

static int fake_discover(void *context, uint32_t token)
{
    struct fake_adapter *fake = (struct fake_adapter *)context;
    observe_output(fake);
    ++fake->discover_calls;
    if (token != TEST_TOKEN) return 0;
    return fake->discover_result;
}

static unsigned fake_discovery_poll(void *context, uint32_t token,
    struct board_client_reply *reply)
{
    struct fake_adapter *fake = (struct fake_adapter *)context;
    unsigned state;
    unsigned index;

    observe_output(fake);
    if (token != TEST_TOKEN) return BOARD_CLIENT_DISCOVERY_FAILED;
    index = fake->discovery_calls++;
    state = index < fake->discovery_state_count ?
        fake->discovery_states[index] : fake->discovery_default;
    if (index < EVENT_CAP) fake->now += fake->discovery_delays[index];
    if (state == BOARD_CLIENT_DISCOVERY_READY)
        memcpy(reply, &fake->reply, sizeof(*reply));
    return state;
}

static int fake_release(void *context, uint32_t token)
{
    struct fake_adapter *fake = (struct fake_adapter *)context;
    unsigned index;

    observe_output(fake);
    ++fake->release_calls;
    if (token != TEST_TOKEN) return 0;
    index = fake->release_calls - 1u;
    return index < fake->release_result_count ?
        fake->release_results[index] : fake->release_default;
}

static const struct board_client_ops complete_ops = {
    fake_clock,
    fake_wait,
    fake_cancelled,
    fake_request,
    fake_lease_poll,
    fake_discover,
    fake_discovery_poll,
    fake_release
};

static int init_case(struct fake_adapter *fake,
    struct board_client_workspace *work,
    struct board_client_observation *output)
{
    size_t written = 0u;

    memset(fake, 0, sizeof(*fake));
    memset(work, SENTINEL, sizeof(*work));
    memset(output, SENTINEL, sizeof(*output));
    fake->output = output;
    memcpy(fake->initial_output, output, sizeof(fake->initial_output));
    fake->wait_step = 1u;
    fake->request_token = TEST_TOKEN;
    fake->pre_lease_default = BOARD_CLIENT_LEASE_ACTIVE;
    fake->discovery_default = BOARD_CLIENT_DISCOVERY_READY;
    fake->discover_result = 1;
    fake->release_default = 1;
    fake->post_lease_default = BOARD_CLIENT_LEASE_RELEASED;

    make_snapshot(&fake->source_snapshot);
    if (!board_discovery_validate(&fake->source_snapshot)) return 0;
    fake->reply.token = TEST_TOKEN;
    fake->reply.generation = TEST_GENERATION;
    fake->reply.uid = TEST_UID;
    fake->reply.nonce = TEST_NONCE;
    if (!board_discovery_encode(&fake->source_snapshot, TEST_NONCE,
            fake->reply.inventory, sizeof(fake->reply.inventory), &written))
        return 0;
    fake->reply.inventory_size = (uint32_t)written;
    return 1;
}

static int all_zero(const void *memory, size_t size)
{
    const uint8_t *bytes = (const uint8_t *)memory;
    size_t i;
    for (i = 0; i < size; ++i) if (bytes[i] != 0u) return 0;
    return 1;
}

static int run_case(const char *name, struct fake_adapter *fake,
    struct board_client_workspace *work,
    struct board_client_observation *output,
    enum board_client_result expected_result, int expect_publication,
    unsigned expected_requests, unsigned expected_jobs,
    unsigned expected_releases, unsigned expected_post_polls)
{
    struct board_client_ops ops = complete_ops;
    enum board_client_result result;

    begin_case(name);
    result = board_client_discover(&ops, fake, sizeof(*fake), work, output);
    CHECK(result == expected_result, "unexpected transaction result");
    CHECK(fake->request_calls == expected_requests, "request count differs");
    CHECK(fake->discover_calls == expected_jobs, "job count differs");
    CHECK(fake->release_calls == expected_releases, "release count differs");
    CHECK(fake->post_lease_calls == expected_post_polls,
        "release poll count differs");
    CHECK(!fake->output_changed_in_callback,
        "output changed before a callback returned");
    if (expect_publication) {
        CHECK(output->token == TEST_TOKEN &&
            output->generation == TEST_GENERATION && output->uid == TEST_UID &&
            output->nonce == TEST_NONCE, "published identity differs");
        CHECK(same_snapshot(&output->snapshot, &fake->source_snapshot),
            "published inventory differs from the decoded snapshot");
    } else {
        CHECK(memcmp(output, fake->initial_output, sizeof(*output)) == 0,
            "refusal changed caller output");
    }
    CHECK(all_zero(work, sizeof(*work)), "transaction did not wipe its workspace");
    return 1;
}

static int test_success_with_pacing(void)
{
    struct fake_adapter fake;
    struct board_client_workspace work;
    struct board_client_observation output;

    if (!init_case(&fake, &work, &output)) return fail("could not make valid fixture");
    fake.pre_lease_states[0] = BOARD_CLIENT_LEASE_REQUESTED;
    fake.pre_lease_states[1] = BOARD_CLIENT_LEASE_BUSY;
    fake.pre_lease_states[2] = BOARD_CLIENT_LEASE_ACTIVE;
    fake.pre_lease_state_count = 3u;
    fake.discovery_states[0] = BOARD_CLIENT_DISCOVERY_REQUESTED;
    fake.discovery_states[1] = BOARD_CLIENT_DISCOVERY_BUSY;
    fake.discovery_states[2] = BOARD_CLIENT_DISCOVERY_READY;
    fake.discovery_state_count = 3u;
    fake.wait_step = 3u;
    if (!run_case("success-one-lease-one-job-after-release", &fake, &work, &output,
            BOARD_CLIENT_OK, 1, 1u, 1u, 1u, 1u)) return 0;
    CHECK(fake.pre_lease_calls == 3u, "lease was not paced through REQUESTED/BUSY");
    CHECK(fake.discovery_calls == 3u, "job was not paced through REQUESTED/BUSY");
    CHECK(fake.wait_calls == 4u, "busy states did not use bounded wait pacing");
    return 1;
}

static int test_terminal_states_and_queue_refusal(void)
{
    struct fake_adapter fake;
    struct board_client_workspace work;
    struct board_client_observation output;

    if (!init_case(&fake, &work, &output)) return fail("could not make valid fixture");
    fake.discovery_default = BOARD_CLIENT_DISCOVERY_UNSUPPORTED;
    if (!run_case("unsupported-releases-lease", &fake, &work, &output,
            BOARD_CLIENT_UNSUPPORTED, 0, 1u, 1u, 1u, 1u)) return 0;

    if (!init_case(&fake, &work, &output)) return fail("could not make valid fixture");
    fake.pre_lease_default = BOARD_CLIENT_LEASE_FAILED;
    if (!run_case("lease-failure-releases-request", &fake, &work, &output,
            BOARD_CLIENT_FAILED, 0, 1u, 0u, 1u, 1u)) return 0;

    if (!init_case(&fake, &work, &output)) return fail("could not make valid fixture");
    fake.discovery_default = BOARD_CLIENT_DISCOVERY_FAILED;
    if (!run_case("discovery-failure-releases-lease", &fake, &work, &output,
            BOARD_CLIENT_FAILED, 0, 1u, 1u, 1u, 1u)) return 0;

    if (!init_case(&fake, &work, &output)) return fail("could not make valid fixture");
    fake.discover_result = 0;
    if (!run_case("job-queue-refusal-releases-lease", &fake, &work, &output,
            BOARD_CLIENT_FAILED, 0, 1u, 1u, 1u, 1u)) return 0;
    return 1;
}

enum reply_variant {
    REPLY_VALID,
    REPLY_WRONG_TOKEN,
    REPLY_ZERO_GENERATION,
    REPLY_ZERO_UID,
    REPLY_ZERO_NONCE,
    REPLY_OVERSIZE,
    REPLY_BAD_MAGIC,
    REPLY_BAD_VALIDATION,
    REPLY_REMOTE_ERROR,
    REPLY_REMOTE_ERROR_WITH_INVENTORY
};

static void set_reply_variant(struct fake_adapter *fake, enum reply_variant variant)
{
    switch (variant) {
    case REPLY_WRONG_TOKEN:
        ++fake->reply.token;
        break;
    case REPLY_ZERO_GENERATION:
        fake->reply.generation = 0u;
        break;
    case REPLY_ZERO_UID:
        fake->reply.uid = 0u;
        break;
    case REPLY_ZERO_NONCE:
        fake->reply.nonce = 0u;
        break;
    case REPLY_OVERSIZE:
        fake->reply.inventory_size = BOARD_DISCOVERY_WIRE_MAX + 1u;
        break;
    case REPLY_BAD_MAGIC:
        fake->reply.inventory[0] = (uint8_t)'X';
        break;
    case REPLY_BAD_VALIDATION:
        fake->reply.inventory[11] = 7u;
        break;
    case REPLY_REMOTE_ERROR:
        fake->reply.remote_status = -7;
        fake->reply.inventory_size = 0u;
        break;
    case REPLY_REMOTE_ERROR_WITH_INVENTORY:
        fake->reply.remote_status = -7;
        break;
    case REPLY_VALID:
    default:
        break;
    }
}

static int test_reply_refusals(void)
{
    static const struct {
        const char *name;
        enum reply_variant variant;
        enum board_client_result result;
    } cases[] = {
        {"bad-token", REPLY_WRONG_TOKEN, BOARD_CLIENT_BAD_REPLY},
        {"zero-generation", REPLY_ZERO_GENERATION, BOARD_CLIENT_BAD_REPLY},
        {"zero-uid", REPLY_ZERO_UID, BOARD_CLIENT_BAD_REPLY},
        {"zero-nonce", REPLY_ZERO_NONCE, BOARD_CLIENT_BAD_REPLY},
        {"oversized-payload", REPLY_OVERSIZE, BOARD_CLIENT_BAD_REPLY},
        {"malformed-wire-magic", REPLY_BAD_MAGIC, BOARD_CLIENT_BAD_REPLY},
        {"payload-fails-inventory-validation", REPLY_BAD_VALIDATION,
            BOARD_CLIENT_BAD_REPLY},
        {"remote-error-without-payload", REPLY_REMOTE_ERROR,
            BOARD_CLIENT_REMOTE_ERROR},
        {"remote-error-with-inventory", REPLY_REMOTE_ERROR_WITH_INVENTORY,
            BOARD_CLIENT_BAD_REPLY}
    };
    struct fake_adapter fake;
    struct board_client_workspace work;
    struct board_client_observation output;
    size_t i;

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        if (!init_case(&fake, &work, &output))
            return fail("could not make valid fixture");
        set_reply_variant(&fake, cases[i].variant);
        if (!run_case(cases[i].name, &fake, &work, &output, cases[i].result,
                0, 1u, 1u, 1u, 1u)) return 0;
    }
    return 1;
}

static int test_cancellation_and_no_lease(void)
{
    struct fake_adapter fake;
    struct board_client_workspace work;
    struct board_client_observation output;

    if (!init_case(&fake, &work, &output)) return fail("could not make valid fixture");
    fake.cancel_on_check = 1u;
    if (!run_case("cancel-before-request", &fake, &work, &output,
            BOARD_CLIENT_CANCELLED, 0, 0u, 0u, 0u, 0u)) return 0;

    if (!init_case(&fake, &work, &output)) return fail("could not make valid fixture");
    fake.cancel_on_check = 3u;
    if (!run_case("cancel-after-active-lease-before-job", &fake, &work, &output,
            BOARD_CLIENT_CANCELLED, 0, 1u, 0u, 1u, 1u)) return 0;

    if (!init_case(&fake, &work, &output)) return fail("could not make valid fixture");
    fake.cancel_on_check = 4u;
    if (!run_case("cancel-after-job-queued-before-result-poll", &fake,
            &work, &output, BOARD_CLIENT_CANCELLED, 0,
            1u, 1u, 1u, 1u)) return 0;
    CHECK(fake.discovery_calls == 0u,
        "cancelled transaction polled a discovery result after cancellation");

    if (!init_case(&fake, &work, &output)) return fail("could not make valid fixture");
    fake.request_token = 0u;
    if (!run_case("request-without-token", &fake, &work, &output,
            BOARD_CLIENT_UNAVAILABLE, 0, 1u, 0u, 0u, 0u)) return 0;
    return 1;
}

static int test_release_retry_and_refusal(void)
{
    struct fake_adapter fake;
    struct board_client_workspace work;
    struct board_client_observation output;

    if (!init_case(&fake, &work, &output)) return fail("could not make valid fixture");
    fake.release_results[0] = 0;
    fake.release_results[1] = 1;
    fake.release_result_count = 2u;
    fake.post_lease_states[0] = BOARD_CLIENT_LEASE_RELEASING;
    fake.post_lease_states[1] = BOARD_CLIENT_LEASE_RELEASED;
    fake.post_lease_state_count = 2u;
    if (!run_case("release-retry-confirmed", &fake, &work, &output,
            BOARD_CLIENT_OK, 1, 1u, 1u, 2u, 2u)) return 0;
    CHECK(fake.wait_calls == 1u, "release retry did not use bounded wait pacing");

    if (!init_case(&fake, &work, &output)) return fail("could not make valid fixture");
    fake.release_results[0] = 0;
    fake.release_result_count = 1u;
    if (!run_case("release-rejected-even-if-state-says-released", &fake,
            &work, &output, BOARD_CLIENT_RELEASE_UNCONFIRMED, 0,
            1u, 1u, 1u, 1u)) return 0;

    if (!init_case(&fake, &work, &output)) return fail("could not make valid fixture");
    fake.post_lease_default = BOARD_CLIENT_LEASE_FAILED;
    if (!run_case("release-state-failed", &fake, &work, &output,
            BOARD_CLIENT_RELEASE_UNCONFIRMED, 0, 1u, 1u, 1u, 1u)) return 0;
    return 1;
}

static int test_deadlines_and_clock_failures(void)
{
    struct fake_adapter fake;
    struct board_client_workspace work;
    struct board_client_observation output;
    size_t i;

    if (!init_case(&fake, &work, &output)) return fail("could not make valid fixture");
    fake.pre_lease_delays[0] = BOARD_CLIENT_WORK_MS;
    fake.now = UINT32_MAX - 10000u;
    if (!run_case("work-deadline-after-unsigned-wrap", &fake, &work, &output,
            BOARD_CLIENT_TIMEOUT, 0, 1u, 0u, 1u, 1u)) return 0;

    if (!init_case(&fake, &work, &output)) return fail("could not make valid fixture");
    fake.discovery_delays[0] = BOARD_CLIENT_WORK_MS;
    if (!run_case("ready-callback-crosses-22s-work-deadline", &fake,
            &work, &output, BOARD_CLIENT_TIMEOUT, 0, 1u, 1u, 1u, 1u)) return 0;

    if (!init_case(&fake, &work, &output)) return fail("could not make valid fixture");
    fake.wait_step = 100u;
    fake.pre_lease_state_count = 220u;
    for (i = 0; i < 219u; ++i)
        fake.pre_lease_states[i] = BOARD_CLIENT_LEASE_REQUESTED;
    fake.pre_lease_states[219] = BOARD_CLIENT_LEASE_ACTIVE;
    fake.post_lease_delays[0] = BOARD_CLIENT_TOTAL_MS - 21900u;
    if (!run_case("release-poll-return-at-30s-is-unconfirmed", &fake,
            &work, &output, BOARD_CLIENT_RELEASE_UNCONFIRMED, 0,
            1u, 1u, 1u, 1u)) return 0;
    CHECK(fake.now == BOARD_CLIENT_TOTAL_MS,
        "late release fixture did not reach the aggregate deadline exactly");
    CHECK(fake.wait_calls == 219u, "late release fixture pacing differs");

    if (!init_case(&fake, &work, &output)) return fail("could not make valid fixture");
    fake.now = UINT32_MAX - 5u;
    fake.wait_step = 32u;
    fake.pre_lease_states[0] = BOARD_CLIENT_LEASE_REQUESTED;
    fake.pre_lease_states[1] = BOARD_CLIENT_LEASE_ACTIVE;
    fake.pre_lease_state_count = 2u;
    if (!run_case("successful-clock-wrap", &fake, &work, &output,
            BOARD_CLIENT_OK, 1, 1u, 1u, 1u, 1u)) return 0;

    if (!init_case(&fake, &work, &output)) return fail("could not make valid fixture");
    fake.clock_fail_call = 1u;
    if (!run_case("clock-unavailable-before-request", &fake, &work, &output,
            BOARD_CLIENT_CLOCK_FAILED, 0, 0u, 0u, 0u, 0u)) return 0;

    if (!init_case(&fake, &work, &output)) return fail("could not make valid fixture");
    fake.clock_fail_call = 3u;
    if (!run_case("clock-failure-after-lease-still-releases", &fake,
            &work, &output, BOARD_CLIENT_CLOCK_FAILED, 0,
            1u, 0u, 1u, 1u)) return 0;

    if (!init_case(&fake, &work, &output)) return fail("could not make valid fixture");
    fake.clock_fail_call = 6u;
    if (!run_case("clock-failure-during-release-is-unconfirmed", &fake,
            &work, &output, BOARD_CLIENT_RELEASE_UNCONFIRMED, 0,
            1u, 1u, 1u, 0u)) return 0;
    return 1;
}

enum missing_hook {
    MISSING_CLOCK,
    MISSING_WAIT,
    MISSING_CANCELLED,
    MISSING_REQUEST,
    MISSING_LEASE_POLL,
    MISSING_DISCOVER,
    MISSING_DISCOVERY_POLL,
    MISSING_RELEASE
};

static void remove_hook(struct board_client_ops *ops, enum missing_hook hook)
{
    switch (hook) {
    case MISSING_CLOCK: ops->clock_ms = NULL; break;
    case MISSING_WAIT: ops->wait = NULL; break;
    case MISSING_CANCELLED: ops->cancelled = NULL; break;
    case MISSING_REQUEST: ops->request = NULL; break;
    case MISSING_LEASE_POLL: ops->lease_poll = NULL; break;
    case MISSING_DISCOVER: ops->discover = NULL; break;
    case MISSING_DISCOVERY_POLL: ops->discovery_poll = NULL; break;
    case MISSING_RELEASE: ops->release = NULL; break;
    }
}

static int invalid_call(const char *name, struct fake_adapter *fake,
    struct board_client_workspace *work,
    struct board_client_observation *output,
    struct board_client_ops *ops, void *context, size_t context_size)
{
    uint8_t workspace_before[sizeof(*work)];
    uint8_t output_before[sizeof(*output)];
    enum board_client_result result;

    begin_case(name);
    memcpy(workspace_before, work, sizeof(workspace_before));
    memcpy(output_before, output, sizeof(output_before));
    result = board_client_discover(ops, context, context_size, work, output);
    CHECK(result == BOARD_CLIENT_INVALID, "invalid ranges/hooks were accepted");
    CHECK(memcmp(workspace_before, work, sizeof(workspace_before)) == 0,
        "invalid call touched workspace before refusal");
    CHECK(memcmp(output_before, output, sizeof(output_before)) == 0,
        "invalid call changed caller output");
    CHECK(fake->request_calls == 0u && fake->release_calls == 0u,
        "invalid call entered adapter hooks");
    return 1;
}

static int test_missing_hooks_alias_and_overflow(void)
{
    static const char *const names[] = {
        "missing-clock-hook", "missing-wait-hook", "missing-cancel-hook",
        "missing-request-hook", "missing-lease-poll-hook", "missing-job-hook",
        "missing-job-poll-hook", "missing-release-hook"
    };
    struct fake_adapter fake;
    struct board_client_workspace work;
    struct board_client_observation output;
    struct board_client_ops ops;
    enum missing_hook hook;

    for (hook = MISSING_CLOCK; hook <= MISSING_RELEASE;
         hook = (enum missing_hook)(hook + 1)) {
        if (!init_case(&fake, &work, &output))
            return fail("could not make valid fixture");
        ops = complete_ops;
        remove_hook(&ops, hook);
        if (!invalid_call(names[hook], &fake, &work, &output, &ops,
                &fake, sizeof(fake))) return 0;
    }

    if (!init_case(&fake, &work, &output)) return fail("could not make valid fixture");
    ops = complete_ops;
    if (!invalid_call("workspace-output-alias", &fake, &work,
            (struct board_client_observation *)(void *)&work, &ops,
            &fake, sizeof(fake))) return 0;

    if (!init_case(&fake, &work, &output)) return fail("could not make valid fixture");
    ops = complete_ops;
    if (!invalid_call("workspace-context-alias", &fake, &work, &output,
            &ops, &work, sizeof(work))) return 0;

    if (!init_case(&fake, &work, &output)) return fail("could not make valid fixture");
    ops = complete_ops;
    if (!invalid_call("output-context-alias", &fake, &work, &output,
            &ops, &output, sizeof(output))) return 0;

    if (!init_case(&fake, &work, &output)) return fail("could not make valid fixture");
    ops = complete_ops;
    if (!invalid_call("workspace-address-range-overflow", &fake, &work,
            &output, &ops,
            (void *)(uintptr_t)(UINTPTR_MAX - sizeof(work) + 1u),
            sizeof(fake))) return 0;

    if (!init_case(&fake, &work, &output)) return fail("could not make valid fixture");
    ops = complete_ops;
    if (!invalid_call("context-address-range-overflow", &fake, &work,
            &output, &ops, (void *)(uintptr_t)(UINTPTR_MAX - 3u), 8u)) return 0;
    return 1;
}

int main(int argc, char **argv)
{
    const char *tag = argc > 1 ? argv[1] : "host";

    if (!test_success_with_pacing() ||
        !test_terminal_states_and_queue_refusal() ||
        !test_reply_refusals() ||
        !test_cancellation_and_no_lease() ||
        !test_release_retry_and_refusal() ||
        !test_deadlines_and_clock_failures() ||
        !test_missing_hooks_alias_and_overflow())
        return 1;

    printf("PASS %s: %u checks across %u cases\n", tag, checks_run, cases_run);
    return 0;
}
