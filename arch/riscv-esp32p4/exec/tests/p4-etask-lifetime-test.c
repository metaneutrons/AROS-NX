/* Host test for the portable P4 ETask metadata-lifetime leaf protocol. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "../p4_etask_lifetime.h"

static unsigned checks;

#define CHECK(condition) do { \
    ++checks; \
    if (!(condition)) { \
        fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); \
        return 0; \
    } \
} while (0)

static int state_equal(const struct p4_etask_lifetime *a,
                       const struct p4_etask_lifetime *b)
{
    return a->initialized == b->initialized &&
        a->task_ref == b->task_ref &&
        a->result_ref == b->result_ref &&
        a->child_edges == b->child_edges &&
        a->readers == b->readers &&
        a->closing == b->closing &&
        a->expunge == b->expunge &&
        a->reclaim_claimed == b->reclaim_claimed;
}

/* Mock the caller's external Task RUN/pin predicate. The leaf state machine
 * has no scheduler knowledge and cannot enforce this precondition itself. */
static int begin_after_external_owners_drain(
    struct p4_etask_lifetime *state,
    unsigned int running_owners,
    unsigned int task_pins)
{
    if (running_owners != 0U || task_pins != 0U)
        return 0;
    return p4_etask_lifetime_begin_cleanup(state);
}

static int test_null_arguments(void)
{
    CHECK(!p4_etask_lifetime_valid(NULL));
    CHECK(!p4_etask_lifetime_init(NULL));
    CHECK(!p4_etask_lifetime_add_edge(NULL));
    CHECK(!p4_etask_lifetime_drop_edge(NULL));
    CHECK(!p4_etask_lifetime_read_pin(NULL));
    CHECK(p4_etask_lifetime_read_unpin(NULL) == P4_ETASK_REFUSED);
    CHECK(!p4_etask_lifetime_begin_cleanup(NULL));
    CHECK(p4_etask_lifetime_finish_cleanup(NULL, 0) == P4_ETASK_REFUSED);
    CHECK(p4_etask_lifetime_finish_cleanup(NULL, 1) == P4_ETASK_REFUSED);
    CHECK(p4_etask_lifetime_finish_cleanup(NULL, 2) == P4_ETASK_REFUSED);
    CHECK(p4_etask_lifetime_drop_result(NULL) == P4_ETASK_REFUSED);
    return 1;
}

static int test_init_requires_zero_storage(void)
{
    struct p4_etask_lifetime state;
    struct p4_etask_lifetime before;
    uint32_t *fields[8];
    size_t index;

    memset(&state, 0, sizeof(state));
    fields[0] = &state.initialized;
    fields[1] = &state.task_ref;
    fields[2] = &state.result_ref;
    fields[3] = &state.child_edges;
    fields[4] = &state.readers;
    fields[5] = &state.closing;
    fields[6] = &state.expunge;
    fields[7] = &state.reclaim_claimed;
    for (index = 0; index < 8U; ++index)
    {
        *fields[index] = 1;
        before = state;
        CHECK(!p4_etask_lifetime_init(&state));
        CHECK(state_equal(&state, &before));
        *fields[index] = 0;
    }
    CHECK(p4_etask_lifetime_init(&state));
    CHECK(p4_etask_lifetime_valid(&state));
    CHECK(state.initialized == 1U && state.task_ref == 1U);
    CHECK(state.result_ref == 0U && state.child_edges == 0U);
    CHECK(state.readers == 0U && state.closing == 0U);
    CHECK(state.expunge == 0U && state.reclaim_claimed == 0U);
    before = state;
    CHECK(!p4_etask_lifetime_init(&state));
    CHECK(state_equal(&state, &before));
    return 1;
}

static int expected_boolean_state(uint32_t initialized,
                                  uint32_t task_ref,
                                  uint32_t result_ref,
                                  uint32_t closing,
                                  uint32_t expunge,
                                  uint32_t reclaim_claimed)
{
    if (initialized != 1U)
        return 0;
    if (task_ref)
        return !result_ref && !expunge && !reclaim_claimed;
    if (!closing)
        return 0;
    if (result_ref)
        return !expunge && !reclaim_claimed;
    /* reader count is zero in the boolean-shape enumeration. */
    return expunge != 0U;
}

static int test_all_boolean_shapes(void)
{
    struct p4_etask_lifetime state;
    uint32_t bits;

    for (bits = 0; bits < 64U; ++bits)
    {
        int expected;
        state.initialized = (bits >> 0) & 1U;
        state.task_ref = (bits >> 1) & 1U;
        state.result_ref = (bits >> 2) & 1U;
        state.closing = (bits >> 3) & 1U;
        state.expunge = (bits >> 4) & 1U;
        state.reclaim_claimed = (bits >> 5) & 1U;
        state.child_edges = 0;
        state.readers = 0;
        expected = expected_boolean_state(
            state.initialized, state.task_ref, state.result_ref,
            state.closing, state.expunge, state.reclaim_claimed);
        CHECK(p4_etask_lifetime_valid(&state) == expected);
    }
    return 1;
}

static int test_corrupt_flags_refuse_without_mutation(void)
{
    struct p4_etask_lifetime state;
    struct p4_etask_lifetime before;
    uint32_t *fields[6];
    size_t index;

    memset(&state, 0, sizeof(state));
    CHECK(p4_etask_lifetime_init(&state));
    fields[0] = &state.initialized;
    fields[1] = &state.task_ref;
    fields[2] = &state.result_ref;
    fields[3] = &state.closing;
    fields[4] = &state.expunge;
    fields[5] = &state.reclaim_claimed;

    for (index = 0; index < 6U; ++index)
    {
        *fields[index] = 2;
        before = state;
        CHECK(!p4_etask_lifetime_valid(&state));
        CHECK(!p4_etask_lifetime_add_edge(&state));
        CHECK(!p4_etask_lifetime_drop_edge(&state));
        CHECK(!p4_etask_lifetime_read_pin(&state));
        CHECK(p4_etask_lifetime_read_unpin(&state) == P4_ETASK_REFUSED);
        CHECK(!p4_etask_lifetime_begin_cleanup(&state));
        CHECK(p4_etask_lifetime_finish_cleanup(&state, 0) == P4_ETASK_REFUSED);
        CHECK(p4_etask_lifetime_drop_result(&state) == P4_ETASK_REFUSED);
        CHECK(state_equal(&state, &before));
        *fields[index] = 1;
        *fields[index] = (index == 0U || index == 1U) ? 1U : 0U;
        CHECK(p4_etask_lifetime_valid(&state));
    }
    return 1;
}

static int test_invalid_combinations_refuse(void)
{
    struct p4_etask_lifetime state;
    struct p4_etask_lifetime before;

    memset(&state, 0, sizeof(state));
    CHECK(p4_etask_lifetime_init(&state));
    state.result_ref = 1;
    before = state;
    CHECK(!p4_etask_lifetime_valid(&state));
    CHECK(!p4_etask_lifetime_add_edge(&state));
    CHECK(!p4_etask_lifetime_read_pin(&state));
    CHECK(state_equal(&state, &before));

    state.task_ref = 0;
    state.closing = 0;
    state.result_ref = 0;
    state.expunge = 1;
    before = state;
    CHECK(!p4_etask_lifetime_valid(&state));
    CHECK(!p4_etask_lifetime_begin_cleanup(&state));
    CHECK(p4_etask_lifetime_finish_cleanup(&state, 0) == P4_ETASK_REFUSED);
    CHECK(state_equal(&state, &before));
    return 1;
}

static int test_edge_and_reader_counter_boundaries(void)
{
    struct p4_etask_lifetime state;
    struct p4_etask_lifetime before;

    memset(&state, 0, sizeof(state));
    CHECK(p4_etask_lifetime_init(&state));
    CHECK(!p4_etask_lifetime_drop_edge(&state));
    CHECK(p4_etask_lifetime_read_unpin(&state) == P4_ETASK_REFUSED);
    CHECK(p4_etask_lifetime_drop_result(&state) == P4_ETASK_REFUSED);
    state.child_edges = UINT32_MAX;
    before = state;
    CHECK(p4_etask_lifetime_valid(&state));
    CHECK(!p4_etask_lifetime_add_edge(&state));
    CHECK(state_equal(&state, &before));
    state.child_edges = 0;
    state.readers = UINT32_MAX;
    before = state;
    CHECK(p4_etask_lifetime_valid(&state));
    CHECK(!p4_etask_lifetime_read_pin(&state));
    CHECK(state_equal(&state, &before));
    return 1;
}

static int test_cleanup_closes_and_reparents_edges(void)
{
    struct p4_etask_lifetime state;
    struct p4_etask_lifetime before;

    memset(&state, 0, sizeof(state));
    CHECK(p4_etask_lifetime_init(&state));
    CHECK(p4_etask_lifetime_add_edge(&state));
    CHECK(p4_etask_lifetime_add_edge(&state));
    CHECK(p4_etask_lifetime_read_pin(&state));
    CHECK(p4_etask_lifetime_begin_cleanup(&state));
    CHECK(state.closing == 1U && state.task_ref == 1U);
    before = state;
    CHECK(!p4_etask_lifetime_add_edge(&state));
    CHECK(!p4_etask_lifetime_read_pin(&state));
    CHECK(state_equal(&state, &before));
    CHECK(p4_etask_lifetime_finish_cleanup(&state, 1) == P4_ETASK_REFUSED);
    CHECK(state_equal(&state, &before));

    CHECK(p4_etask_lifetime_drop_edge(&state));
    before = state;
    CHECK(p4_etask_lifetime_finish_cleanup(&state, 1) == P4_ETASK_REFUSED);
    CHECK(state_equal(&state, &before));
    CHECK(p4_etask_lifetime_drop_edge(&state));
    CHECK(p4_etask_lifetime_finish_cleanup(&state, 1) == P4_ETASK_RETAINED);
    CHECK(p4_etask_lifetime_valid(&state));
    CHECK(state.task_ref == 0U && state.result_ref == 1U);
    CHECK(state.child_edges == 0U && state.readers == 1U);
    CHECK(state.closing == 1U && state.expunge == 0U);
    CHECK(!p4_etask_lifetime_add_edge(&state));
    return 1;
}

static int test_external_owner_edge_reader_lifecycle(void)
{
    struct p4_etask_lifetime state;
    struct p4_etask_lifetime before;

    memset(&state, 0, sizeof(state));
    CHECK(p4_etask_lifetime_init(&state));
    CHECK(p4_etask_lifetime_add_edge(&state));
    CHECK(p4_etask_lifetime_read_pin(&state));
    before = state;
    CHECK(!begin_after_external_owners_drain(&state, 1U, 1U));
    CHECK(state_equal(&state, &before));
    CHECK(!begin_after_external_owners_drain(&state, 0U, 1U));
    CHECK(state_equal(&state, &before));
    CHECK(begin_after_external_owners_drain(&state, 0U, 0U));

    before = state;
    CHECK(p4_etask_lifetime_finish_cleanup(&state, 0) == P4_ETASK_REFUSED);
    CHECK(state_equal(&state, &before));
    CHECK(p4_etask_lifetime_drop_edge(&state));
    CHECK(p4_etask_lifetime_finish_cleanup(&state, 0) ==
          P4_ETASK_RETAINED);
    CHECK(state.expunge == 1U && state.readers == 1U);
    CHECK(p4_etask_lifetime_read_unpin(&state) == P4_ETASK_RECLAIM);
    CHECK(state.reclaim_claimed == 1U);
    return 1;
}

static int test_result_reference_then_last_reader_reclaims(void)
{
    struct p4_etask_lifetime state;
    struct p4_etask_lifetime before;

    memset(&state, 0, sizeof(state));
    CHECK(p4_etask_lifetime_init(&state));
    CHECK(p4_etask_lifetime_read_pin(&state));
    CHECK(p4_etask_lifetime_read_pin(&state));
    CHECK(p4_etask_lifetime_begin_cleanup(&state));
    before = state;
    CHECK(!p4_etask_lifetime_read_pin(&state));
    CHECK(state_equal(&state, &before));
    CHECK(p4_etask_lifetime_finish_cleanup(&state, 1) == P4_ETASK_RETAINED);
    CHECK(state.task_ref == 0U && state.result_ref == 1U);
    CHECK(state.expunge == 0U && state.readers == 2U);

    /* The exited-result queue reference keeps metadata alive after Task
     * ownership ends; a temporary metadata reader may still be acquired. */
    CHECK(p4_etask_lifetime_read_pin(&state));
    CHECK(state.readers == 3U);
    CHECK(p4_etask_lifetime_drop_result(&state) == P4_ETASK_RETAINED);
    CHECK(state.result_ref == 0U && state.expunge == 1U);
    before = state;
    CHECK(!p4_etask_lifetime_read_pin(&state));
    CHECK(!p4_etask_lifetime_add_edge(&state));
    CHECK(state_equal(&state, &before));

    CHECK(p4_etask_lifetime_read_unpin(&state) == P4_ETASK_RETAINED);
    CHECK(p4_etask_lifetime_read_unpin(&state) == P4_ETASK_RETAINED);
    CHECK(p4_etask_lifetime_read_unpin(&state) == P4_ETASK_RECLAIM);
    CHECK(state.reclaim_claimed == 1U && state.readers == 0U);
    CHECK(p4_etask_lifetime_valid(&state));
    return 1;
}

static int test_no_result_cleanup_reclaims_now_or_after_readers(void)
{
    struct p4_etask_lifetime immediate;
    struct p4_etask_lifetime delayed;

    memset(&immediate, 0, sizeof(immediate));
    CHECK(p4_etask_lifetime_init(&immediate));
    CHECK(p4_etask_lifetime_begin_cleanup(&immediate));
    CHECK(p4_etask_lifetime_finish_cleanup(&immediate, 0) ==
          P4_ETASK_RECLAIM);
    CHECK(immediate.reclaim_claimed == 1U && immediate.expunge == 1U);

    memset(&delayed, 0, sizeof(delayed));
    CHECK(p4_etask_lifetime_init(&delayed));
    CHECK(p4_etask_lifetime_read_pin(&delayed));
    CHECK(p4_etask_lifetime_read_pin(&delayed));
    CHECK(p4_etask_lifetime_begin_cleanup(&delayed));
    CHECK(p4_etask_lifetime_finish_cleanup(&delayed, 0) ==
          P4_ETASK_RETAINED);
    CHECK(delayed.task_ref == 0U && delayed.result_ref == 0U);
    CHECK(delayed.expunge == 1U && delayed.readers == 2U);
    CHECK(p4_etask_lifetime_read_unpin(&delayed) == P4_ETASK_RETAINED);
    CHECK(p4_etask_lifetime_read_unpin(&delayed) == P4_ETASK_RECLAIM);
    CHECK(delayed.reclaim_claimed == 1U && delayed.readers == 0U);
    return 1;
}

static int test_invalid_retain_arguments_are_transactional(void)
{
    struct p4_etask_lifetime state;
    struct p4_etask_lifetime before;

    memset(&state, 0, sizeof(state));
    CHECK(p4_etask_lifetime_init(&state));
    CHECK(p4_etask_lifetime_begin_cleanup(&state));
    before = state;
    CHECK(p4_etask_lifetime_finish_cleanup(&state, -1) == P4_ETASK_REFUSED);
    CHECK(state_equal(&state, &before));
    CHECK(p4_etask_lifetime_finish_cleanup(&state, 2) == P4_ETASK_REFUSED);
    CHECK(state_equal(&state, &before));
    CHECK(p4_etask_lifetime_finish_cleanup(&state, 0) == P4_ETASK_RECLAIM);
    return 1;
}

static int test_double_operations_and_claimed_state_refuse(void)
{
    struct p4_etask_lifetime state;
    struct p4_etask_lifetime before;

    memset(&state, 0, sizeof(state));
    CHECK(p4_etask_lifetime_init(&state));
    CHECK(p4_etask_lifetime_begin_cleanup(&state));
    before = state;
    CHECK(!p4_etask_lifetime_begin_cleanup(&state));
    CHECK(state_equal(&state, &before));
    CHECK(p4_etask_lifetime_finish_cleanup(&state, 1) == P4_ETASK_RETAINED);
    before = state;
    CHECK(p4_etask_lifetime_finish_cleanup(&state, 1) == P4_ETASK_REFUSED);
    CHECK(state_equal(&state, &before));
    CHECK(p4_etask_lifetime_drop_result(&state) == P4_ETASK_RECLAIM);
    before = state;
    CHECK(p4_etask_lifetime_drop_result(&state) == P4_ETASK_REFUSED);
    CHECK(p4_etask_lifetime_read_unpin(&state) == P4_ETASK_REFUSED);
    CHECK(!p4_etask_lifetime_add_edge(&state));
    CHECK(!p4_etask_lifetime_drop_edge(&state));
    CHECK(!p4_etask_lifetime_read_pin(&state));
    CHECK(!p4_etask_lifetime_begin_cleanup(&state));
    CHECK(p4_etask_lifetime_finish_cleanup(&state, 0) == P4_ETASK_REFUSED);
    CHECK(state_equal(&state, &before));
    return 1;
}

static int test_reclaim_return_transfers_free_authority(void)
{
    struct p4_etask_lifetime *state;
    long raw_page_size;
    size_t page_size;
    void *page;

    raw_page_size = sysconf(_SC_PAGESIZE);
    CHECK(raw_page_size > 0);
    page_size = (size_t)raw_page_size;
    page = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANON, -1, 0);
    CHECK(page != MAP_FAILED);
    state = (struct p4_etask_lifetime *)page;
    memset(state, 0, sizeof(*state));
    CHECK(p4_etask_lifetime_init(state));
    CHECK(p4_etask_lifetime_begin_cleanup(state));
    CHECK(p4_etask_lifetime_finish_cleanup(state, 0) == P4_ETASK_RECLAIM);
    CHECK(state->reclaim_claimed == 1U);

    /* Caller acts only after its gate is dropped. Poison and release the
     * storage, then intentionally make no further helper call with it. */
    CHECK(mprotect(page, page_size, PROT_NONE) == 0);
    CHECK(munmap(page, page_size) == 0);
    state = NULL;
    CHECK(state == NULL);
    return 1;
}

int main(void)
{
    int ok = 1;

    ok &= test_null_arguments();
    ok &= test_init_requires_zero_storage();
    ok &= test_all_boolean_shapes();
    ok &= test_corrupt_flags_refuse_without_mutation();
    ok &= test_invalid_combinations_refuse();
    ok &= test_edge_and_reader_counter_boundaries();
    ok &= test_cleanup_closes_and_reparents_edges();
    ok &= test_external_owner_edge_reader_lifecycle();
    ok &= test_result_reference_then_last_reader_reclaims();
    ok &= test_no_result_cleanup_reclaims_now_or_after_readers();
    ok &= test_invalid_retain_arguments_are_transactional();
    ok &= test_double_operations_and_claimed_state_refuse();
    ok &= test_reclaim_return_transfers_free_authority();
    if (!ok)
        return 1;

    printf("PASS P4 ETask lifetime leaf protocol: %u checks\n", checks);
    return 0;
}
