/* Actual-source host tests for the private cancellation retry-interest leaf. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../p4_semaphore_cancel_retry.c"

static unsigned checks;

#define CHECK(condition) do { \
    ++checks; \
    if (!(condition)) { \
        fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); \
        return 0; \
    } \
} while (0)

/* This adapter models only the caller's sticky wake bit. The leaf must not
 * consume or clear it; production gate ordering and IPI delivery are outside
 * this fixture. */
static int modeled_producer_event(struct p4_semaphore_cancel_retry *state,
    uint32_t *durable_wake)
{
    int interested = p4_semaphore_cancel_retry_event(state);

    if (interested > 0)
        *durable_wake = 1u;
    return interested;
}

static int test_zero_interest_and_pass_events(void)
{
    struct p4_semaphore_cancel_retry state = { 0, 0 };
    struct p4_semaphore_cancel_retry before;
    uint32_t durable_wake = 0;

    CHECK(p4_semaphore_cancel_retry_event(&state) == 0);
    before = state;
    CHECK(p4_semaphore_cancel_retry_end(&state, 0) == 0);
    CHECK(memcmp(&before, &state, sizeof(state)) == 0);
    CHECK(p4_semaphore_cancel_retry_end(&state, 1) == 0);
    CHECK(memcmp(&before, &state, sizeof(state)) == 0);

    CHECK(p4_semaphore_cancel_retry_begin(&state) == 1);
    CHECK(state.active == 1u && state.waiting == 1u);
    CHECK(p4_semaphore_cancel_retry_event(&state) == 1);

    /* A producer publishes the caller-owned sticky wake during the pass. */
    CHECK(modeled_producer_event(&state, &durable_wake) == 1);
    CHECK(durable_wake == 1u);
    CHECK(p4_semaphore_cancel_retry_end(&state, 0) == 1);
    CHECK(state.active == 0u && state.waiting == 0u);
    CHECK(p4_semaphore_cancel_retry_event(&state) == 0);

    /* Ending the pass cannot erase a wake already latched by the caller. */
    CHECK(durable_wake == 1u);
    durable_wake = 0u; /* model the external worker draining that wake */
    CHECK(durable_wake == 0u);
    return 1;
}

static int test_blocked_pass_and_later_producer(void)
{
    struct p4_semaphore_cancel_retry state = { 0, 0 };
    struct p4_semaphore_cancel_retry before;
    uint32_t durable_wake = 0;

    CHECK(p4_semaphore_cancel_retry_begin(&state) == 1);
    CHECK(p4_semaphore_cancel_retry_end(&state, 1) == 1);
    CHECK(state.active == 0u && state.waiting == 1u);
    CHECK(p4_semaphore_cancel_retry_event(&state) == 1);

    /* The next producer event sees retained interest and requests another
     * durable wake. This is a state-model check, not an IPI test. */
    CHECK(modeled_producer_event(&state, &durable_wake) == 1);
    CHECK(durable_wake == 1u);

    CHECK(p4_semaphore_cancel_retry_begin(&state) == 1);
    CHECK(state.active == 1u && state.waiting == 1u);
    before = state;
    CHECK(p4_semaphore_cancel_retry_begin(&state) == 0);
    CHECK(memcmp(&before, &state, sizeof(state)) == 0);

    CHECK(p4_semaphore_cancel_retry_end(&state, 1) == 1);
    CHECK(state.active == 0u && state.waiting == 1u);
    before = state;
    CHECK(p4_semaphore_cancel_retry_end(&state, 0) == 0);
    CHECK(memcmp(&before, &state, sizeof(state)) == 0);
    return 1;
}

static int test_inactive_interest_and_refusal_immutability(void)
{
    struct p4_semaphore_cancel_retry state = { 0, 1 };
    struct p4_semaphore_cancel_retry before;

    CHECK(p4_semaphore_cancel_retry_event(&state) == 1);
    before = state;
    CHECK(p4_semaphore_cancel_retry_begin(&state) == 1);
    CHECK(state.active == 1u && state.waiting == 1u);
    CHECK(p4_semaphore_cancel_retry_end(&state, 0) == 1);
    CHECK(state.active == 0u && state.waiting == 0u);

    state.active = 0u;
    state.waiting = 0u;
    before = state;
    CHECK(p4_semaphore_cancel_retry_end(&state, 0) == 0);
    CHECK(memcmp(&before, &state, sizeof(state)) == 0);
    CHECK(p4_semaphore_cancel_retry_end(&state, 1) == 0);
    CHECK(memcmp(&before, &state, sizeof(state)) == 0);

    state.active = 1u;
    state.waiting = 1u;
    before = state;
    CHECK(p4_semaphore_cancel_retry_end(&state, 2u) == -1);
    CHECK(memcmp(&before, &state, sizeof(state)) == 0);

    state.active = 2u;
    state.waiting = 1u;
    before = state;
    CHECK(p4_semaphore_cancel_retry_begin(&state) == -1);
    CHECK(memcmp(&before, &state, sizeof(state)) == 0);
    CHECK(p4_semaphore_cancel_retry_end(&state, 0) == -1);
    CHECK(memcmp(&before, &state, sizeof(state)) == 0);
    CHECK(p4_semaphore_cancel_retry_event(&state) == -1);
    CHECK(memcmp(&before, &state, sizeof(state)) == 0);

    state.active = 1u;
    state.waiting = 0u;
    before = state;
    CHECK(p4_semaphore_cancel_retry_begin(&state) == -1);
    CHECK(memcmp(&before, &state, sizeof(state)) == 0);
    CHECK(p4_semaphore_cancel_retry_end(&state, 1) == -1);
    CHECK(memcmp(&before, &state, sizeof(state)) == 0);
    CHECK(p4_semaphore_cancel_retry_event(&state) == -1);
    CHECK(memcmp(&before, &state, sizeof(state)) == 0);

    state.active = 0u;
    state.waiting = 2u;
    before = state;
    CHECK(p4_semaphore_cancel_retry_begin(&state) == -1);
    CHECK(memcmp(&before, &state, sizeof(state)) == 0);
    CHECK(p4_semaphore_cancel_retry_end(&state, 0) == -1);
    CHECK(memcmp(&before, &state, sizeof(state)) == 0);
    CHECK(p4_semaphore_cancel_retry_event(&state) == -1);
    CHECK(memcmp(&before, &state, sizeof(state)) == 0);
    return 1;
}

static int test_null_and_misaligned_refusals(void)
{
    struct p4_semaphore_cancel_retry aligned = { 0, 0 };
    _Alignas(struct p4_semaphore_cancel_retry) unsigned char storage[
        sizeof(struct p4_semaphore_cancel_retry) +
        _Alignof(struct p4_semaphore_cancel_retry)];
    struct p4_semaphore_cancel_retry *misaligned;
    unsigned char before[sizeof(storage)];

    CHECK(p4_semaphore_cancel_retry_begin(NULL) == -1);
    CHECK(p4_semaphore_cancel_retry_end(NULL, 0) == -1);
    CHECK(p4_semaphore_cancel_retry_end(NULL, 2u) == -1);
    CHECK(p4_semaphore_cancel_retry_event(NULL) == -1);

    memset(storage, 0xa7, sizeof(storage));
    memcpy(before, storage, sizeof(storage));
    misaligned = (struct p4_semaphore_cancel_retry *)(void *)(storage + 1u);
    CHECK(p4_semaphore_cancel_retry_begin(misaligned) == -1);
    CHECK(p4_semaphore_cancel_retry_end(misaligned, 1) == -1);
    CHECK(p4_semaphore_cancel_retry_end(misaligned, 2u) == -1);
    CHECK(p4_semaphore_cancel_retry_event(misaligned) == -1);
    CHECK(memcmp(before, storage, sizeof(storage)) == 0);

    CHECK(p4_semaphore_cancel_retry_begin(&aligned) == 1);
    CHECK(aligned.active == 1u && aligned.waiting == 1u);
    return 1;
}

int main(void)
{
    if (!test_zero_interest_and_pass_events() ||
        !test_blocked_pass_and_later_producer() ||
        !test_inactive_interest_and_refusal_immutability() ||
        !test_null_and_misaligned_refusals())
        return 1;

    printf("p4 semaphore cancel-retry checks: %u\n", checks);
    return 0;
}
