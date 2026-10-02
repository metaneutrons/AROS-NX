/* Host fixture for the actual P4 semaphore ledger implementation. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif

#include "../p4_semaphore_ledger.h"
#include "../p4_semaphore_ledger.c"

#define TEST_OPERATION_CAPACITY 4u
#define TEST_ENTRY_CAPACITY 8u
#define TEST_IDENTITY_COUNT 24u

struct test_fixture
{
    struct p4_semaphore_ledger ledger;
    struct p4_semaphore_ledger_operation operations[TEST_OPERATION_CAPACITY];
    struct p4_semaphore_ledger_entry entries[TEST_ENTRY_CAPACITY];
};

static unsigned int checks;
static unsigned int failures;
static void *identities[TEST_IDENTITY_COUNT];
static size_t identity_mapping_size;

#define CHECK(condition)                                                     \
    do                                                                       \
    {                                                                        \
        ++checks;                                                            \
        if (!(condition))                                                    \
        {                                                                    \
            ++failures;                                                     \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,       \
                    #condition);                                            \
        }                                                                    \
    } while (0)

#define CHECK_UNCHANGED(fixture, expression, expected_result)                \
    do                                                                       \
    {                                                                        \
        struct test_fixture before__;                                        \
        memcpy(&before__, (fixture), sizeof(before__));                      \
        CHECK((expression) == (expected_result));                            \
        CHECK(memcmp((fixture), &before__, sizeof(before__)) == 0);           \
    } while (0)

static const void *identity(unsigned int index)
{
    CHECK(index < TEST_IDENTITY_COUNT);
    return index < TEST_IDENTITY_COUNT ? identities[index] : NULL;
}

static void fixture_init(struct test_fixture *fixture,
                         uint32_t operation_capacity,
                         uint32_t entry_capacity)
{
    memset(fixture, 0, sizeof(*fixture));
    CHECK(p4_semaphore_ledger_init(
              &fixture->ledger, fixture->operations, operation_capacity,
              fixture->entries, entry_capacity) == 1);
}

static int reserve_operation(struct test_fixture *fixture,
                             const void *owner,
                             uint32_t generation,
                             uint32_t entry_count,
                             struct p4_semaphore_ledger_operation **out)
{
    return p4_semaphore_ledger_reserve(
        &fixture->ledger, owner, generation, entry_count, out);
}

static int configure_entry(struct test_fixture *fixture,
                           struct p4_semaphore_ledger_operation *operation,
                           uint32_t index,
                           unsigned int semaphore_identity,
                           unsigned int request_identity)
{
    return p4_semaphore_ledger_set_entry(
        &fixture->ledger, operation, index, identity(semaphore_identity),
        identity(request_identity));
}

static void test_init_refusals(void)
{
    struct test_fixture fixture;
    struct test_fixture before;
    uintptr_t near_end = UINTPTR_MAX - (UINTPTR_MAX % sizeof(void *));

    memset(&fixture, 0, sizeof(fixture));
    memcpy(&before, &fixture, sizeof(before));
    CHECK(p4_semaphore_ledger_init(
              &fixture.ledger, fixture.operations, 0u,
              fixture.entries, 1u) == -1);
    CHECK(memcmp(&fixture, &before, sizeof(fixture)) == 0);
    CHECK(p4_semaphore_ledger_init(
              &fixture.ledger, fixture.operations,
              P4_SEMAPHORE_LEDGER_LIMIT + 1u,
              fixture.entries, 1u) == -1);
    CHECK(memcmp(&fixture, &before, sizeof(fixture)) == 0);
    CHECK(p4_semaphore_ledger_init(
              &fixture.ledger,
              (struct p4_semaphore_ledger_operation *)near_end,
              1u, fixture.entries, 1u) == -1);
    CHECK(memcmp(&fixture, &before, sizeof(fixture)) == 0);
    CHECK(p4_semaphore_ledger_init(
              &fixture.ledger,
              (struct p4_semaphore_ledger_operation *)&fixture.ledger,
              1u, fixture.entries, 1u) == -1);
    CHECK(memcmp(&fixture, &before, sizeof(fixture)) == 0);
    CHECK(p4_semaphore_ledger_init(
              &fixture.ledger, fixture.operations, 1u,
              (struct p4_semaphore_ledger_entry *)(void *)fixture.operations,
              1u) == -1);
    CHECK(memcmp(&fixture, &before, sizeof(fixture)) == 0);

    fixture.operations[0].active = 1u;
    memcpy(&before, &fixture, sizeof(before));
    CHECK(p4_semaphore_ledger_init(
              &fixture.ledger, fixture.operations, 1u,
              fixture.entries, 1u) == 0);
    CHECK(memcmp(&fixture, &before, sizeof(fixture)) == 0);
}

static void test_reservation_configuration_and_aliases(void)
{
    struct test_fixture fixture;
    struct test_fixture before;
    struct p4_semaphore_ledger_operation *first = NULL;
    struct p4_semaphore_ledger_operation *second = NULL;
    struct p4_semaphore_ledger_operation *out =
        (struct p4_semaphore_ledger_operation *)(uintptr_t)UINT32_C(0x12345678);
    struct p4_semaphore_ledger_operation **reserve_alias;
    struct p4_semaphore_ledger_entry **entry_alias;
    struct p4_semaphore_ledger_entry *entry_out =
        (struct p4_semaphore_ledger_entry *)(uintptr_t)UINT32_C(0x76543210);
    uint32_t *release_alias;
    uint32_t selected = UINT32_C(0xA5A5A5A5);

    fixture_init(&fixture, 2u, 3u);
    CHECK(reserve_operation(&fixture, identity(0u), 1u, 2u, &first) == 1);
    CHECK(first == &fixture.operations[0]);
    CHECK(fixture.ledger.active_operations == 1u);
    CHECK(fixture.ledger.active_entries == 2u);

    memcpy(&before, &fixture, sizeof(before));
    CHECK(reserve_operation(&fixture, identity(0u), 1u, 1u, &out) == 0);
    CHECK(out == (struct p4_semaphore_ledger_operation *)(uintptr_t)
                     UINT32_C(0x12345678));
    CHECK(memcmp(&fixture, &before, sizeof(fixture)) == 0);
    CHECK(reserve_operation(&fixture, identity(2u), 1u, UINT32_MAX, &out) ==
          0);
    CHECK(out == (struct p4_semaphore_ledger_operation *)(uintptr_t)
                     UINT32_C(0x12345678));
    CHECK(memcmp(&fixture, &before, sizeof(fixture)) == 0);
    CHECK(reserve_operation(&fixture, identity(1u), 1u, 2u, &out) == 0);
    CHECK(out == (struct p4_semaphore_ledger_operation *)(uintptr_t)
                     UINT32_C(0x12345678));
    CHECK(memcmp(&fixture, &before, sizeof(fixture)) == 0);
    CHECK(reserve_operation(&fixture, identity(0u), 1u, 1u, &out) == 0);
    CHECK(out == (struct p4_semaphore_ledger_operation *)(uintptr_t)
                     UINT32_C(0x12345678));
    CHECK(memcmp(&fixture, &before, sizeof(fixture)) == 0);

    reserve_alias = (struct p4_semaphore_ledger_operation **)(void *)
        &fixture.ledger.operations_head;
    CHECK_UNCHANGED(&fixture,
        p4_semaphore_ledger_reserve(&fixture.ledger, identity(2u), 1u, 1u,
                                    reserve_alias), -1);
    entry_alias = (struct p4_semaphore_ledger_entry **)(void *)
        &fixture.operations[0].active_next;
    CHECK_UNCHANGED(&fixture,
        p4_semaphore_ledger_entry_at(
            &fixture.ledger, first, 0u, entry_alias), -1);
    CHECK_UNCHANGED(&fixture,
        p4_semaphore_ledger_entry_at(
            &fixture.ledger, first, 2u, &entry_out), 0);
    CHECK(entry_out == (struct p4_semaphore_ledger_entry *)(uintptr_t)
                          UINT32_C(0x76543210));
    release_alias = &fixture.entries[0].state;
    CHECK_UNCHANGED(&fixture,
        p4_semaphore_ledger_release(
            &fixture.ledger, identity(8u), 1u, release_alias), -1);
    CHECK(selected == UINT32_C(0xA5A5A5A5));

    CHECK(p4_semaphore_ledger_acquire(&fixture.ledger, first, 0u) == 0);
    CHECK(p4_semaphore_ledger_close(&fixture.ledger, first) == 0);
    CHECK(configure_entry(&fixture, first, 0u, 8u, 9u) == 1);
    CHECK(configure_entry(&fixture, first, 1u, 8u, 10u) == 1);
    CHECK(fixture.ledger.request_count == 2u);
    CHECK(fixture.ledger.requests_head != NULL);
    CHECK(fixture.ledger.requests_head->request != NULL);
    CHECK(p4_semaphore_ledger_close(&fixture.ledger, first) == 1);
    CHECK(configure_entry(&fixture, first, 0u, 8u, 9u) == 0);
    CHECK(p4_semaphore_ledger_reserve(
              &fixture.ledger, identity(1u), 1u, 1u, &second) == 1);
    before = fixture;
    CHECK(configure_entry(&fixture, second, 0u, 8u, 9u) == 0);
    CHECK(memcmp(&fixture, &before, sizeof(fixture)) == 0);
    CHECK(configure_entry(&fixture, second, 0u, 9u, 9u) == 0);
    CHECK(memcmp(&fixture, &before, sizeof(fixture)) == 0);
    CHECK(configure_entry(&fixture, second, 0u, 8u, 11u) == 1);
    CHECK(p4_semaphore_ledger_close(&fixture.ledger, second) == 1);
    CHECK(fixture.ledger.active_operations == 2u);
    CHECK(fixture.ledger.active_entries == 3u);
}

static void test_partial_list_cancel_withdraw_rollback(void)
{
    struct test_fixture fixture;
    struct test_fixture before;
    struct p4_semaphore_ledger_operation *operation = NULL;
    struct p4_semaphore_ledger_entry *entry = NULL;

    fixture_init(&fixture, 2u, 6u);
    CHECK(reserve_operation(&fixture, identity(0u), 2u, 3u, &operation) == 1);
    CHECK(configure_entry(&fixture, operation, 0u, 8u, 9u) == 1);
    CHECK(configure_entry(&fixture, operation, 1u, 8u, 10u) == 1);
    before = fixture;
    CHECK(p4_semaphore_ledger_close(&fixture.ledger, operation) == 0);
    CHECK(memcmp(&fixture, &before, sizeof(fixture)) == 0);
    CHECK(p4_semaphore_ledger_acquire(&fixture.ledger, operation, 0u) == 0);
    CHECK(p4_semaphore_ledger_entry_at(
              &fixture.ledger, operation, 0u, &entry) == 1);
    CHECK(entry == &fixture.entries[0]);
    CHECK(configure_entry(&fixture, operation, 2u, 9u, 12u) == 1);
    CHECK(p4_semaphore_ledger_close(&fixture.ledger, operation) == 1);
    CHECK(p4_semaphore_ledger_acquire(&fixture.ledger, operation, 0u) == 1);
    CHECK(p4_semaphore_ledger_queue(&fixture.ledger, operation, 0u) == 0);
    CHECK(p4_semaphore_ledger_queue(&fixture.ledger, operation, 1u) == 1);
    CHECK(p4_semaphore_ledger_grant(&fixture.ledger, operation, 1u) == 1);
    CHECK(p4_semaphore_ledger_queue(&fixture.ledger, operation, 2u) == 1);
    CHECK(fixture.entries[0].state == P4_SEMAPHORE_LEDGER_ENTRY_OWNED);
    CHECK(fixture.entries[1].state == P4_SEMAPHORE_LEDGER_ENTRY_OWNED);
    CHECK(fixture.entries[2].state == P4_SEMAPHORE_LEDGER_ENTRY_QUEUED);
    {
        uint32_t selected = UINT32_C(0xA5A5A5A5);
        before = fixture;
        CHECK(p4_semaphore_ledger_release(
                  &fixture.ledger, identity(8u), 0u, &selected) == -1);
        CHECK(selected == UINT32_C(0xA5A5A5A5));
        CHECK(memcmp(&fixture, &before, sizeof(fixture)) == 0);
    }
    before = fixture;
    CHECK(p4_semaphore_ledger_commit(&fixture.ledger, operation) == 0);
    CHECK(memcmp(&fixture, &before, sizeof(fixture)) == 0);

    before = fixture;
    CHECK(p4_semaphore_ledger_acquire(&fixture.ledger, operation, 0u) == 0);
    CHECK(memcmp(&fixture, &before, sizeof(fixture)) == 0);
    CHECK(p4_semaphore_ledger_cancel(&fixture.ledger, operation) == 1);
    before = fixture;
    CHECK(p4_semaphore_ledger_grant(&fixture.ledger, operation, 2u) == 0);
    CHECK(memcmp(&fixture, &before, sizeof(fixture)) == 0);
    CHECK(p4_semaphore_ledger_withdraw(&fixture.ledger, operation, 2u) == 1);
    CHECK(p4_semaphore_ledger_withdraw(&fixture.ledger, operation, 0u) == 0);
    CHECK(fixture.entries[2].state == P4_SEMAPHORE_LEDGER_ENTRY_WITHDRAWN);
    CHECK(fixture.ledger.request_count == 3u);
    CHECK(p4_semaphore_ledger_rollback_owned(&fixture.ledger, operation, 0u) ==
          1);
    CHECK(p4_semaphore_ledger_rollback_owned(&fixture.ledger, operation, 1u) ==
          1);
    before = fixture;
    CHECK(p4_semaphore_ledger_rollback_owned(&fixture.ledger, operation, 2u) ==
          0);
    CHECK(memcmp(&fixture, &before, sizeof(fixture)) == 0);
    CHECK(p4_semaphore_ledger_rollback_owned(&fixture.ledger, operation, 1u) ==
          0);
    CHECK(memcmp(&fixture, &before, sizeof(fixture)) == 0);
    CHECK(p4_semaphore_ledger_restart_released(
              &fixture.ledger, operation, 0u) == 0);
    CHECK(p4_semaphore_ledger_reclaim(&fixture.ledger, operation) == 1);
    CHECK(fixture.ledger.active_operations == 0u);
    CHECK(fixture.ledger.active_entries == 0u);
    CHECK(fixture.ledger.request_count == 0u);
}

static void test_interleaved_operations_and_release_order(void)
{
    struct test_fixture fixture;
    struct test_fixture before;
    struct p4_semaphore_ledger_operation *completed = NULL;
    struct p4_semaphore_ledger_operation *pending = NULL;
    uint32_t selected = UINT32_C(0xA5A5A5A5);
    uint32_t nest = 3u;

    fixture_init(&fixture, 3u, 6u);
    CHECK(reserve_operation(&fixture, identity(2u), 3u, 2u, &completed) == 1);
    CHECK(reserve_operation(&fixture, identity(3u), 4u, 1u, &pending) == 1);
    CHECK(configure_entry(&fixture, completed, 0u, 8u, 9u) == 1);
    CHECK(configure_entry(&fixture, completed, 1u, 8u, 11u) == 1);
    CHECK(configure_entry(&fixture, pending, 0u, 8u, 10u) == 1);
    CHECK(p4_semaphore_ledger_close(&fixture.ledger, completed) == 1);
    CHECK(p4_semaphore_ledger_close(&fixture.ledger, pending) == 1);

    /* Caller-serialized interleaving: one owner recursively acquires two
       units while another owner queues for the same semaphore. */
    CHECK(p4_semaphore_ledger_acquire(&fixture.ledger, completed, 0u) == 1);
    CHECK(p4_semaphore_ledger_queue(&fixture.ledger, pending, 0u) == 1);
    CHECK(p4_semaphore_ledger_acquire(&fixture.ledger, completed, 1u) == 1);
    CHECK(fixture.ledger.request_count == 3u);
    CHECK(p4_semaphore_ledger_commit(&fixture.ledger, completed) == 1);
    CHECK(p4_semaphore_ledger_grant(&fixture.ledger, pending, 0u) == 1);
    CHECK(pending->committed == 0u);
    CHECK(fixture.ledger.active_operations == 2u);
    CHECK(fixture.ledger.active_entries == 3u);
    CHECK(completed->committed == 1u);
    CHECK(completed->first->state == P4_SEMAPHORE_LEDGER_ENTRY_OWNED);
    CHECK(pending->first->state == P4_SEMAPHORE_LEDGER_ENTRY_OWNED);
    CHECK(completed->first->operation_next->state ==
          P4_SEMAPHORE_LEDGER_ENTRY_OWNED);

    /* total_nest includes two completed units and one pending unit. Anonymous
       Releases consume both completed units before selecting pending owner. */
    before = fixture;
    CHECK(p4_semaphore_ledger_release(
              &fixture.ledger, identity(8u), nest, &selected) == 1);
    CHECK(selected == 0u);
    CHECK(memcmp(&fixture, &before, sizeof(fixture)) == 0);
    --nest;
    selected = UINT32_C(0xA5A5A5A5);
    before = fixture;
    CHECK(p4_semaphore_ledger_release(
              &fixture.ledger, identity(8u), nest, &selected) == 1);
    CHECK(selected == 0u);
    CHECK(memcmp(&fixture, &before, sizeof(fixture)) == 0);
    --nest;

    selected = UINT32_C(0xA5A5A5A5);
    CHECK(p4_semaphore_ledger_release(
              &fixture.ledger, identity(8u), nest, &selected) == 1);
    CHECK(selected == 1u);
    --nest;
    CHECK(nest == 0u);
    CHECK(pending->first->state == P4_SEMAPHORE_LEDGER_ENTRY_RELEASED);

    /* The pending unit has been consumed once; with no units left, another
       anonymous release refuses without changing output or ledger. */
    before = fixture;
    selected = UINT32_C(0xA5A5A5A5);
    CHECK(p4_semaphore_ledger_release(
              &fixture.ledger, identity(8u), nest, &selected) == -1);
    CHECK(selected == UINT32_C(0xA5A5A5A5));
    CHECK(memcmp(&fixture, &before, sizeof(fixture)) == 0);
    CHECK(p4_semaphore_ledger_cancel(&fixture.ledger, pending) == 1);
    before = fixture;
    CHECK(p4_semaphore_ledger_rollback_owned(
              &fixture.ledger, pending, 0u) == 0);
    CHECK(memcmp(&fixture, &before, sizeof(fixture)) == 0);
    CHECK(p4_semaphore_ledger_reclaim(&fixture.ledger, pending) == 1);

    /* Committed cancellation reclaims only ledger metadata: rollback cannot
       consume a completed unit, and committed entries stay OWNED until the
       caller independently accounts for the matching Release. */
    CHECK(p4_semaphore_ledger_cancel(&fixture.ledger, completed) == 1);
    before = fixture;
    CHECK(p4_semaphore_ledger_rollback_owned(
              &fixture.ledger, completed, 0u) == 0);
    CHECK(memcmp(&fixture, &before, sizeof(fixture)) == 0);
    CHECK(completed->committed == 1u);
    CHECK(completed->first->state == P4_SEMAPHORE_LEDGER_ENTRY_OWNED);
    CHECK(completed->first->operation_next->state ==
          P4_SEMAPHORE_LEDGER_ENTRY_OWNED);
    CHECK(p4_semaphore_ledger_reclaim(&fixture.ledger, completed) == 1);
    CHECK(fixture.ledger.active_operations == 0u);
    CHECK(fixture.ledger.active_entries == 0u);
}

static void test_restart_generation_and_pool_reuse(void)
{
    struct test_fixture fixture;
    struct test_fixture before;
    struct p4_semaphore_ledger_operation *first = NULL;
    struct p4_semaphore_ledger_operation *second = NULL;
    struct p4_semaphore_ledger_operation *third = NULL;
    uint32_t selected = 0u;

    fixture_init(&fixture, 1u, 2u);
    CHECK(reserve_operation(&fixture, identity(5u), 1u, 2u, &first) == 1);
    before = fixture;
    CHECK(reserve_operation(&fixture, identity(6u), 1u, 1u, &second) == 0);
    CHECK(second == NULL);
    CHECK(memcmp(&fixture, &before, sizeof(fixture)) == 0);
    CHECK(configure_entry(&fixture, first, 0u, 13u, 14u) == 1);
    CHECK(configure_entry(&fixture, first, 1u, 13u, 15u) == 1);
    CHECK(p4_semaphore_ledger_close(&fixture.ledger, first) == 1);
    CHECK(p4_semaphore_ledger_acquire(&fixture.ledger, first, 0u) == 1);
    CHECK(p4_semaphore_ledger_release(
              &fixture.ledger, identity(13u), 1u, &selected) == 1);
    CHECK(selected == 1u);
    CHECK(first->first->state == P4_SEMAPHORE_LEDGER_ENTRY_RELEASED);
    CHECK(p4_semaphore_ledger_restart_released(
              &fixture.ledger, first, 0u) == 1);
    CHECK(first->first->state == P4_SEMAPHORE_LEDGER_ENTRY_UNVISITED);
    CHECK(p4_semaphore_ledger_acquire(&fixture.ledger, first, 0u) == 1);
    CHECK(p4_semaphore_ledger_cancel(&fixture.ledger, first) == 1);
    CHECK(p4_semaphore_ledger_rollback_owned(&fixture.ledger, first, 0u) == 1);
    CHECK(p4_semaphore_ledger_reclaim(&fixture.ledger, first) == 1);
    CHECK(fixture.ledger.active_operations == 0u);
    CHECK(fixture.ledger.active_entries == 0u);
    CHECK(fixture.ledger.request_count == 0u);

    CHECK(reserve_operation(&fixture, identity(5u), 1u, 1u, &second) == 1);
    CHECK(configure_entry(&fixture, second, 0u, 13u, 14u) == 1);
    CHECK(p4_semaphore_ledger_close(&fixture.ledger, second) == 1);
    CHECK(p4_semaphore_ledger_acquire(&fixture.ledger, second, 0u) == 1);
    before = fixture;
    selected = UINT32_C(0xA5A5A5A5);
    CHECK(p4_semaphore_ledger_release(
              &fixture.ledger, identity(13u), 0u, &selected) == -1);
    CHECK(selected == UINT32_C(0xA5A5A5A5));
    CHECK(memcmp(&fixture, &before, sizeof(fixture)) == 0);
    CHECK(p4_semaphore_ledger_commit(&fixture.ledger, second) == 1);
    CHECK(p4_semaphore_ledger_reclaim(&fixture.ledger, second) == 1);
    CHECK(reserve_operation(&fixture, identity(5u), 2u, 2u, &third) == 1);
    CHECK(configure_entry(&fixture, third, 0u, 13u, 14u) == 1);
    CHECK(configure_entry(&fixture, third, 1u, 13u, 15u) == 1);
    CHECK(p4_semaphore_ledger_close(&fixture.ledger, third) == 1);
    CHECK(p4_semaphore_ledger_cancel(&fixture.ledger, third) == 1);
    CHECK(p4_semaphore_ledger_reclaim(&fixture.ledger, third) == 1);
    CHECK(fixture.ledger.active_operations == 0u);
    CHECK(fixture.ledger.active_entries == 0u);
    CHECK(fixture.ledger.request_count == 0u);

    /* The operation arena is exhausted independently of its spare entry. */
    CHECK(reserve_operation(&fixture, identity(5u), 3u, 1u, &first) == 1);
    before = fixture;
    CHECK(reserve_operation(&fixture, identity(6u), 3u, 1u, &second) == 0);
    CHECK(memcmp(&fixture, &before, sizeof(fixture)) == 0);
    CHECK(p4_semaphore_ledger_cancel(&fixture.ledger, first) == 1);
    before = fixture;
    CHECK(p4_semaphore_ledger_cancel(&fixture.ledger, first) == 0);
    CHECK(memcmp(&fixture, &before, sizeof(fixture)) == 0);
    CHECK(p4_semaphore_ledger_reclaim(&fixture.ledger, first) == 1);
}

static void test_corruption_refusal_is_atomic(void)
{
    struct test_fixture fixture;
    struct test_fixture valid;
    struct test_fixture corrupt;
    struct p4_semaphore_ledger_operation *operation = NULL;

    fixture_init(&fixture, 3u, 5u);
    CHECK(reserve_operation(&fixture, identity(7u), 1u, 2u, &operation) == 1);
    CHECK(configure_entry(&fixture, operation, 0u, 16u, 17u) == 1);
    CHECK(configure_entry(&fixture, operation, 1u, 16u, 18u) == 1);
    CHECK(p4_semaphore_ledger_close(&fixture.ledger, operation) == 1);
    CHECK(p4_semaphore_ledger_acquire(&fixture.ledger, operation, 0u) == 1);
    CHECK(p4_semaphore_ledger_queue(&fixture.ledger, operation, 1u) == 1);
    valid = fixture;

    fixture.ledger.active_entries = UINT32_MAX;
    corrupt = fixture;
    CHECK(p4_semaphore_ledger_cancel(&fixture.ledger, operation) == -1);
    CHECK(memcmp(&fixture, &corrupt, sizeof(fixture)) == 0);

    fixture = valid;
    fixture.ledger.request_count = UINT32_MAX;
    corrupt = fixture;
    CHECK(p4_semaphore_ledger_commit(&fixture.ledger, operation) == -1);
    CHECK(memcmp(&fixture, &corrupt, sizeof(fixture)) == 0);

    fixture = valid;
    fixture.operations[0].active_next = &fixture.operations[0];
    corrupt = fixture;
    CHECK(p4_semaphore_ledger_cancel(&fixture.ledger, operation) == -1);
    CHECK(memcmp(&fixture, &corrupt, sizeof(fixture)) == 0);

    fixture = valid;
    fixture.operations[0].first->operation_next =
        fixture.operations[0].first;
    corrupt = fixture;
    CHECK(p4_semaphore_ledger_cancel(&fixture.ledger, operation) == -1);
    CHECK(memcmp(&fixture, &corrupt, sizeof(fixture)) == 0);

    fixture = valid;
    fixture.ledger.requests_head->request_next =
        (struct p4_semaphore_ledger_entry *)(uintptr_t)identity(23u);
    corrupt = fixture;
    CHECK(p4_semaphore_ledger_cancel(&fixture.ledger, operation) == -1);
    CHECK(memcmp(&fixture, &corrupt, sizeof(fixture)) == 0);

    fixture = valid;
    fixture.operations[0].entry_count = UINT32_MAX;
    corrupt = fixture;
    CHECK(p4_semaphore_ledger_reclaim(&fixture.ledger, operation) == -1);
    CHECK(memcmp(&fixture, &corrupt, sizeof(fixture)) == 0);

    fixture = valid;
    fixture.entries[0].operation =
        (struct p4_semaphore_ledger_operation *)(uintptr_t)identity(23u);
    corrupt = fixture;
    CHECK(p4_semaphore_ledger_rollback_owned(
              &fixture.ledger, operation, 0u) == -1);
    CHECK(memcmp(&fixture, &corrupt, sizeof(fixture)) == 0);
}

int main(void)
{
    long page_size = sysconf(_SC_PAGESIZE);
    unsigned int i;

    if (page_size <= 0)
    {
        fputs("could not determine page size\n", stderr);
        return EXIT_FAILURE;
    }
    identity_mapping_size = (size_t)page_size;
    for (i = 0u; i < TEST_IDENTITY_COUNT; ++i)
    {
        identities[i] = mmap(NULL, identity_mapping_size, PROT_NONE,
                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (identities[i] == MAP_FAILED)
        {
            perror("mmap PROT_NONE ledger identity");
            return EXIT_FAILURE;
        }
    }

    test_init_refusals();
    test_reservation_configuration_and_aliases();
    test_partial_list_cancel_withdraw_rollback();
    test_interleaved_operations_and_release_order();
    test_restart_generation_and_pool_reuse();
    test_corruption_refusal_is_atomic();

    for (i = 0u; i < TEST_IDENTITY_COUNT; ++i)
        if (munmap(identities[i], identity_mapping_size) != 0)
        {
            perror("munmap PROT_NONE ledger identity");
            return EXIT_FAILURE;
        }

    if (failures != 0u)
    {
        fprintf(stderr, "%u of %u checks failed\n", failures, checks);
        return EXIT_FAILURE;
    }
    printf("P4 semaphore ledger checks: %u passed\n", checks);
    return EXIT_SUCCESS;
}
