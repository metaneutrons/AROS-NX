/* Actual-source tests for the P4 Task-independent blocking-wait registry. */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "../p4_task_lifetime.c"
#include "../p4_task_registry.c"

static pthread_mutex_t lifetime_gate = PTHREAD_MUTEX_INITIALIZER;
static unsigned int checks;
static void *unmapped_identities;
static size_t mapped_size;

#define CHECK(condition)                                                     \
    do                                                                       \
    {                                                                        \
        ++checks;                                                            \
        if (!(condition))                                                    \
        {                                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,         \
                    #condition);                                             \
            exit(EXIT_FAILURE);                                              \
        }                                                                    \
    } while (0)

static void gate_lock(void)
{
    if (pthread_mutex_lock(&lifetime_gate) != 0)
    {
        fputs("lifetime gate lock failed\n", stderr);
        exit(EXIT_FAILURE);
    }
}

static void gate_unlock(void)
{
    if (pthread_mutex_unlock(&lifetime_gate) != 0)
    {
        fputs("lifetime gate unlock failed\n", stderr);
        exit(EXIT_FAILURE);
    }
}

static struct Task *opaque_task(size_t page_offset)
{
    return (struct Task *)((unsigned char *)unmapped_identities + page_offset);
}

static const void *opaque_index(size_t page_offset)
{
    return (const void *)((unsigned char *)unmapped_identities + page_offset);
}

static int wait_scopes_equal(const struct p4_task_wait_scope *left,
                             const struct p4_task_wait_scope *right)
{
    return left->generation == right->generation &&
           left->active == right->active && left->kind == right->kind &&
           left->queued_count == right->queued_count &&
           left->construction_closed == right->construction_closed &&
           left->cancel_requested == right->cancel_requested &&
           left->cancel_claimed == right->cancel_claimed &&
           left->semaphore_index == right->semaphore_index &&
           left->request_index == right->request_index &&
           left->list_index == right->list_index;
}

static int wait_snapshots_equal(
    const struct p4_task_wait_cancel_snapshot *left,
    const struct p4_task_wait_cancel_snapshot *right)
{
    return left->identity == right->identity &&
           left->generation == right->generation && left->kind == right->kind &&
           left->queued_count == right->queued_count &&
           left->construction_closed == right->construction_closed &&
           left->cancel_requested == right->cancel_requested &&
           left->semaphore_index == right->semaphore_index &&
           left->request_index == right->request_index &&
           left->list_index == right->list_index;
}

static void init_registry(struct p4_task_registry *registry,
                          struct p4_task_registry_record *record,
                          struct Task *identity, uint32_t hart)
{
    p4_task_registry_init(registry);
    memset(record, 0, sizeof(*record));
    gate_lock();
    CHECK(p4_task_registry_register_locked(registry, record, identity));
    CHECK(p4_task_registry_enter_running_locked(registry, identity, hart));
    gate_unlock();
}

static int begin_wait(struct p4_task_registry *registry,
                      struct Task *identity, uint32_t kind,
                      const void *sem, const void *request, const void *list,
                      uint32_t *generation)
{
    int result;
    gate_lock();
    result = p4_task_registry_wait_begin_locked(
        registry, identity, kind, sem, request, list, generation);
    gate_unlock();
    return result;
}

static int wait_cancel_ready(struct p4_task_registry *registry,
                             struct Task *identity)
{
    int result;
    gate_lock();
    result = p4_task_registry_wait_cancel_ready_locked(registry, identity);
    gate_unlock();
    return result;
}

static int claim_cancel(struct p4_task_registry *registry,
                        struct p4_task_wait_cancel_snapshot *snapshot)
{
    int result;
    gate_lock();
    result = p4_task_registry_claim_next_wait_cancel_locked(registry,
                                                            snapshot);
    gate_unlock();
    return result;
}

static void test_live_wait_generation_and_grant_pin(void)
{
    struct p4_task_registry registry;
    struct p4_task_registry_record record = {0};
    struct Task *identity = opaque_task(0);
    const void *sem = opaque_index(4096);
    const void *request = opaque_index(8192);
    uint32_t generation = UINT32_C(0xfeedbeef);
    uint32_t next_generation = 0;
    uint32_t before_generation;

    init_registry(&registry, &record, identity, 0);
    CHECK(wait_cancel_ready(&registry, identity) == 0);
    CHECK(wait_cancel_ready(&registry, opaque_task(16384)) == -1);
    CHECK(wait_cancel_ready(&registry, 0) == -1);

    CHECK(!begin_wait(&registry, identity, P4_TASK_WAIT_SINGLE,
                      0, request, 0, &generation));
    CHECK(generation == 0);
    CHECK(!begin_wait(&registry, identity, P4_TASK_WAIT_SINGLE,
                      sem, 0, 0, &generation));
    CHECK(generation == 0);
    CHECK(!begin_wait(&registry, identity, P4_TASK_WAIT_LIST,
                      sem, request, opaque_index(12288), &generation));
    CHECK(generation == 0);
    CHECK(!begin_wait(&registry, identity, UINT32_C(99),
                      sem, request, 0, &generation));
    CHECK(generation == 0);

    /* Output overlapping a record is rejected before either side changes. */
    before_generation = record.wait_scope.generation;
    CHECK(!begin_wait(&registry, identity, P4_TASK_WAIT_SINGLE,
                      sem, request, 0,
                      &record.wait_scope.generation));
    CHECK(record.wait_scope.generation == before_generation);
    CHECK(record.lifetime.notification_pins == 0);

    CHECK(begin_wait(&registry, identity, P4_TASK_WAIT_SINGLE,
                     sem, request, 0, &generation));
    CHECK(generation == 1);
    CHECK(record.lifetime.notification_pins == 1);
    CHECK(record.wait_scope.semaphore_index == sem);
    CHECK(record.wait_scope.request_index == request);
    CHECK(!p4_task_registry_wait_ready_locked(&registry, identity,
                                               generation));
    CHECK(!p4_task_registry_wait_add_request_locked(&registry, identity,
                                                     generation + 1u));
    CHECK(p4_task_registry_wait_add_request_locked(&registry, identity,
                                                    generation));
    CHECK(wait_cancel_ready(&registry, identity) == 0);
    CHECK(!p4_task_registry_wait_ready_locked(&registry, identity,
                                               generation));
    CHECK(p4_task_registry_wait_close_locked(&registry, identity,
                                              generation));
    CHECK(!p4_task_registry_wait_close_locked(&registry, identity,
                                               generation));
    CHECK(!p4_task_registry_wait_add_request_locked(&registry, identity,
                                                     generation));
    CHECK(!p4_task_registry_wait_ready_locked(&registry, identity,
                                               generation));

    /* The grant pin precedes decrement; ordinary unpin can release only it. */
    CHECK(p4_task_registry_wait_grant_request_locked(&registry, identity,
                                                      generation));
    CHECK(record.wait_scope.queued_count == 0);
    CHECK(record.lifetime.notification_pins == 2);
    CHECK(p4_task_registry_wait_ready_locked(&registry, identity,
                                              generation));
    CHECK(p4_task_registry_unpin_locked(&registry, identity));
    CHECK(record.lifetime.notification_pins == 1);
    CHECK(!p4_task_registry_unpin_locked(&registry, identity));
    CHECK(p4_task_registry_wait_finish_locked(&registry, identity,
                                              generation));
    CHECK(!record.wait_scope.active);
    CHECK(record.wait_scope.generation == generation);
    CHECK(record.lifetime.notification_pins == 0);
    CHECK(!p4_task_registry_wait_ready_locked(&registry, identity,
                                               generation));

    CHECK(begin_wait(&registry, identity, P4_TASK_WAIT_SINGLE,
                     sem, request, 0, &next_generation));
    CHECK(next_generation == generation + 1u);
    CHECK(!p4_task_registry_wait_close_locked(&registry, identity,
                                               generation));
    CHECK(p4_task_registry_wait_add_request_locked(&registry, identity,
                                                    next_generation));
    CHECK(p4_task_registry_wait_close_locked(&registry, identity,
                                              next_generation));
    CHECK(p4_task_registry_wait_grant_request_locked(&registry, identity,
                                                      next_generation));
    CHECK(p4_task_registry_unpin_locked(&registry, identity));
    CHECK(p4_task_registry_wait_finish_locked(&registry, identity,
                                              next_generation));
    CHECK(p4_task_registry_leave_running_locked(&registry, identity, 0));
    CHECK(p4_task_registry_retire_locked(&registry, identity));
    CHECK(p4_task_registry_claim_reclaim_locked(&registry, identity) ==
          &record);
}

static void test_list_cancel_partial_withdraw_and_run_drain(void)
{
    struct p4_task_registry registry;
    struct p4_task_registry_record record = {0};
    struct Task *identity = opaque_task(0);
    const void *list = opaque_index(4096);
    struct p4_task_wait_cancel_snapshot snapshot;
    struct p4_task_wait_scope before_scope;
    uint32_t generation = 0;

    init_registry(&registry, &record, identity, 1);
    CHECK(begin_wait(&registry, identity, P4_TASK_WAIT_LIST,
                     0, 0, list, &generation));
    CHECK(generation == 1);
    CHECK(p4_task_registry_wait_add_request_locked(&registry, identity,
                                                    generation));
    CHECK(p4_task_registry_wait_add_request_locked(&registry, identity,
                                                    generation));
    CHECK(p4_task_registry_wait_add_request_locked(&registry, identity,
                                                    generation));
    CHECK(p4_task_registry_wait_close_locked(&registry, identity,
                                              generation));
    CHECK(p4_task_registry_retire_for_service_locked(&registry, identity));
    CHECK(p4_task_registry_wait_cancel_ready_locked(&registry, identity) ==
          1); /* Queued stack requests are immediately cancellation work. */
    CHECK(!p4_task_registry_wait_ready_locked(&registry, identity,
                                               generation));
    CHECK(p4_task_registry_take_service_ready_locked(&registry, identity) ==
          0);
    CHECK(p4_task_registry_claim_reclaim_locked(&registry, identity) == 0);

    /* Aliased snapshots cannot corrupt the registered record. */
    before_scope = record.wait_scope;
    CHECK(p4_task_registry_claim_next_wait_cancel_locked(
              &registry,
              (struct p4_task_wait_cancel_snapshot *)&record.wait_scope) == -1);
    CHECK(wait_scopes_equal(&record.wait_scope, &before_scope));
    memset(&snapshot, 0xa5, sizeof(snapshot));
    CHECK(claim_cancel(&registry, &snapshot) == 1);
    CHECK(snapshot.identity == identity);
    CHECK(snapshot.generation == generation);
    CHECK(snapshot.kind == P4_TASK_WAIT_LIST);
    CHECK(snapshot.queued_count == 3);
    CHECK(snapshot.construction_closed == 1);
    CHECK(snapshot.cancel_requested == 1);
    CHECK(snapshot.list_index == list);
    CHECK(record.wait_scope.cancel_requested == 1);
    CHECK(record.wait_scope.cancel_claimed == 1);
    CHECK(wait_cancel_ready(&registry, identity) == 0);
    CHECK(claim_cancel(&registry, &snapshot) == 0);
    CHECK(snapshot.identity == 0 && snapshot.generation == 0);

    CHECK(p4_task_registry_wait_withdraw_request_locked(
        &registry, identity, generation));
    CHECK(p4_task_registry_wait_withdraw_request_locked(
        &registry, identity, generation));
    CHECK(record.wait_scope.queued_count == 1);
    CHECK(!p4_task_registry_wait_finish_cancel_locked(
        &registry, identity, generation));
    CHECK(p4_task_registry_wait_release_cancel_locked(
        &registry, identity, generation));
    CHECK(wait_cancel_ready(&registry, identity) == 1);
    memset(&snapshot, 0, sizeof(snapshot));
    CHECK(claim_cancel(&registry, &snapshot) == 1);
    CHECK(snapshot.queued_count == 1);
    CHECK(p4_task_registry_wait_withdraw_request_locked(
        &registry, identity, generation));
    CHECK(record.wait_scope.queued_count == 0);

    /* Empty queue is not ready while the retired Task still owns RUN. */
    CHECK(p4_task_registry_wait_release_cancel_locked(
        &registry, identity, generation));
    CHECK(wait_cancel_ready(&registry, identity) == 0);
    memset(&snapshot, 0xa5, sizeof(snapshot));
    CHECK(claim_cancel(&registry, &snapshot) == 0);
    CHECK(snapshot.identity == 0 && snapshot.generation == 0);
    CHECK(p4_task_registry_leave_running_locked(&registry, identity, 1));
    CHECK(wait_cancel_ready(&registry, identity) == 1);
    CHECK(claim_cancel(&registry, &snapshot) == 1);
    CHECK(snapshot.queued_count == 0);
    CHECK(!p4_task_registry_wait_grant_request_locked(
        &registry, identity, generation));
    CHECK(!p4_task_registry_wait_finish_locked(
        &registry, identity, generation));
    CHECK(p4_task_registry_wait_finish_cancel_locked(
        &registry, identity, generation));
    CHECK(!record.wait_scope.active);
    CHECK(record.wait_scope.generation == generation);
    CHECK(record.lifetime.notification_pins == 1); /* Service pin remains. */
    CHECK(p4_task_registry_take_service_ready_locked(&registry, identity) ==
          identity);
    {
        struct p4_task_registry_record *detached = 0;
        CHECK(p4_task_registry_claim_next_service_locked(&registry,
                                                         &detached) == 1);
        CHECK(detached == &record);
    }
}

static void test_notification_pin_survives_scope_finish_and_retirement(void)
{
    struct p4_task_registry registry;
    struct p4_task_registry_record record = {0};
    struct Task *identity = opaque_task(0);
    uint32_t generation = 0;

    init_registry(&registry, &record, identity, 0);
    CHECK(begin_wait(&registry, identity, P4_TASK_WAIT_SINGLE,
                     opaque_index(4096), opaque_index(8192), 0,
                     &generation));
    CHECK(p4_task_registry_wait_add_request_locked(&registry, identity,
                                                    generation));
    CHECK(p4_task_registry_wait_close_locked(&registry, identity,
                                              generation));
    CHECK(p4_task_registry_wait_grant_request_locked(&registry, identity,
                                                      generation));
    CHECK(p4_task_registry_wait_finish_locked(&registry, identity,
                                              generation));
    CHECK(record.lifetime.notification_pins == 1);
    CHECK(!record.wait_scope.active);
    CHECK(p4_task_registry_retire_for_service_locked(&registry, identity));
    CHECK(record.lifetime.notification_pins == 2);
    CHECK(p4_task_registry_leave_running_locked(&registry, identity, 0));
    CHECK(p4_task_registry_wait_cancel_ready_locked(&registry, identity) ==
          0);
    CHECK(p4_task_registry_take_service_ready_locked(&registry, identity) ==
          0);
    CHECK(p4_task_registry_unpin_locked(&registry, identity));
    CHECK(record.lifetime.notification_pins == 1);
    CHECK(p4_task_registry_take_service_ready_locked(&registry, identity) ==
          identity);
    CHECK(p4_task_registry_claim_service_locked(&registry, identity) ==
          &record);
}

static void test_retired_closed_scope_is_not_wait_ready(void)
{
    struct p4_task_registry registry;
    struct p4_task_registry_record record = {0};
    struct Task *identity = opaque_task(0);
    struct p4_task_wait_cancel_snapshot snapshot;
    struct p4_task_wait_scope before_scope;
    uint32_t generation = 0;

    init_registry(&registry, &record, identity, 0);
    CHECK(begin_wait(&registry, identity, P4_TASK_WAIT_SINGLE,
                     opaque_index(4096), opaque_index(8192), 0,
                     &generation));
    CHECK(p4_task_registry_wait_close_locked(&registry, identity,
                                              generation));
    CHECK(p4_task_registry_wait_ready_locked(&registry, identity,
                                              generation));
    CHECK(p4_task_registry_retire_for_service_locked(&registry, identity));
    before_scope = record.wait_scope;
    CHECK(!p4_task_registry_wait_ready_locked(&registry, identity,
                                               generation));
    CHECK(wait_scopes_equal(&record.wait_scope, &before_scope));
    CHECK(wait_cancel_ready(&registry, identity) == 0);
    CHECK(p4_task_registry_leave_running_locked(&registry, identity, 0));
    CHECK(wait_cancel_ready(&registry, identity) == 1);
    CHECK(claim_cancel(&registry, &snapshot) == 1);
    CHECK(snapshot.queued_count == 0 && snapshot.construction_closed == 1);
    CHECK(p4_task_registry_wait_finish_cancel_locked(
        &registry, identity, generation));
    CHECK(!record.wait_scope.active);
    CHECK(p4_task_registry_take_service_ready_locked(&registry, identity) ==
          identity);
    CHECK(p4_task_registry_claim_queued_service_locked(&registry, identity) ==
          &record);
}

static void test_validation_overflow_and_empty_snapshot(void)
{
    struct p4_task_registry registry;
    struct p4_task_registry_record record = {0};
    struct Task *identity = opaque_task(0);
    struct p4_task_wait_cancel_snapshot snapshot;
    struct p4_task_wait_cancel_snapshot before_snapshot;
    struct p4_task_wait_scope before_scope;
    uint32_t generation = UINT32_C(0xabcdef01);

    init_registry(&registry, &record, identity, 0);
    record.wait_scope.generation = UINT32_MAX;
    CHECK(!begin_wait(&registry, identity, P4_TASK_WAIT_SINGLE,
                      opaque_index(4096), opaque_index(8192), 0,
                      &generation));
    CHECK(generation == 0);
    CHECK(record.wait_scope.generation == UINT32_MAX);
    record.wait_scope.generation = 0;

    CHECK(begin_wait(&registry, identity, P4_TASK_WAIT_SINGLE,
                     opaque_index(4096), opaque_index(8192), 0,
                     &generation));
    CHECK(p4_task_registry_wait_add_request_locked(&registry, identity,
                                                    generation));
    record.wait_scope.queued_count = UINT32_MAX;
    before_scope = record.wait_scope;
    CHECK(!p4_task_registry_wait_add_request_locked(&registry, identity,
                                                    generation));
    CHECK(wait_scopes_equal(&record.wait_scope, &before_scope));
    record.wait_scope.queued_count = 0;

    record.wait_scope.cancel_claimed = 2;
    before_scope = record.wait_scope;
    CHECK(wait_cancel_ready(&registry, identity) == -1);
    memset(&snapshot, 0x5a, sizeof(snapshot));
    before_snapshot = snapshot;
    CHECK(claim_cancel(&registry, &snapshot) == -1);
    CHECK(wait_snapshots_equal(&snapshot, &before_snapshot));
    CHECK(wait_scopes_equal(&record.wait_scope, &before_scope));
    record.wait_scope.cancel_claimed = 0;

    CHECK(p4_task_registry_wait_close_locked(&registry, identity,
                                              generation));
    CHECK(p4_task_registry_wait_finish_locked(&registry, identity,
                                              generation));
    CHECK(p4_task_registry_wait_cancel_ready_locked(&registry, identity) ==
          0);
    memset(&snapshot, 0x5a, sizeof(snapshot));
    CHECK(claim_cancel(&registry, &snapshot) == 0);
    CHECK(snapshot.identity == 0 && snapshot.generation == 0 &&
          snapshot.kind == 0 && snapshot.queued_count == 0 &&
          snapshot.semaphore_index == 0 && snapshot.request_index == 0 &&
          snapshot.list_index == 0);
    CHECK(p4_task_registry_leave_running_locked(&registry, identity, 0));
    CHECK(p4_task_registry_retire_locked(&registry, identity));
    CHECK(p4_task_registry_claim_reclaim_locked(&registry, identity) ==
          &record);
}

static void test_registration_requires_zero_wait_generation(void)
{
    struct p4_task_registry registry;
    struct p4_task_registry_record dirty = {0};
    p4_task_registry_init(&registry);
    dirty.wait_scope.generation = 7u;
    CHECK(!p4_task_registry_register_locked(
        &registry, &dirty, opaque_task(0)));
    CHECK(dirty.wait_scope.generation == 7u);
    CHECK(dirty.next == 0 && dirty.identity == 0);
    CHECK(dirty.service_pending == 0 && dirty.service_queued == 0 &&
          dirty.service_inflight == 0 && dirty.worker_protected == 0);
    CHECK(dirty.wait_scope.active == 0 && dirty.wait_scope.kind == 0 &&
          dirty.wait_scope.queued_count == 0 &&
          dirty.wait_scope.construction_closed == 0 &&
          dirty.wait_scope.cancel_requested == 0 &&
          dirty.wait_scope.cancel_claimed == 0 &&
          dirty.wait_scope.semaphore_index == 0 &&
          dirty.wait_scope.request_index == 0 &&
          dirty.wait_scope.list_index == 0);
    CHECK(dirty.lifetime.notification_pins == 0 &&
          dirty.lifetime.running_harts == 0 && dirty.lifetime.flags == 0);
    CHECK(registry.head == 0 && registry.count == 0);
}

int main(void)
{
    long page_size = sysconf(_SC_PAGESIZE);

    if (page_size <= 0)
    {
        fputs("cannot determine page size\n", stderr);
        return EXIT_FAILURE;
    }
    mapped_size = (size_t)page_size * 4u;
    unmapped_identities = mmap(0, mapped_size, PROT_NONE,
                               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (unmapped_identities == MAP_FAILED)
    {
        perror("mmap PROT_NONE identities");
        return EXIT_FAILURE;
    }

    test_live_wait_generation_and_grant_pin();
    test_list_cancel_partial_withdraw_and_run_drain();
    test_notification_pin_survives_scope_finish_and_retirement();
    test_retired_closed_scope_is_not_wait_ready();
    test_validation_overflow_and_empty_snapshot();
    test_registration_requires_zero_wait_generation();

    if (munmap(unmapped_identities, mapped_size) != 0)
    {
        perror("munmap PROT_NONE identities");
        return EXIT_FAILURE;
    }
    printf("P4 semaphore wait registry checks: %u passed\n", checks);
    return EXIT_SUCCESS;
}
