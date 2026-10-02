/* Host sanitizer fixture for the actual private cleanup-ready registry API. */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sanitizer/asan_interface.h>

struct Task
{
    unsigned char poisoned_storage[32];
};

#include "../p4_task_lifetime.c"
#include "../p4_task_registry.c"

static pthread_mutex_t lifetime_gate = PTHREAD_MUTEX_INITIALIZER;
static unsigned checks;

#define CHECK(condition) do { \
    ++checks; \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: check failed: %s\n", \
                __FILE__, __LINE__, #condition); \
        exit(EXIT_FAILURE); \
    } \
} while (0)

struct record_snapshot
{
    struct p4_task_registry_record *next;
    struct Task *identity;
    uint32_t service_pending;
    uint32_t service_queued;
    uint32_t notification_pins;
    uint32_t running_harts;
    uint32_t flags;
};

struct registry_snapshot
{
    struct p4_task_registry_record *head;
    uint32_t count;
};

static void gate_lock(void)
{
    CHECK(pthread_mutex_lock(&lifetime_gate) == 0);
}

static void gate_unlock(void)
{
    CHECK(pthread_mutex_unlock(&lifetime_gate) == 0);
}

static struct record_snapshot snapshot_record(
    const struct p4_task_registry_record *record)
{
    struct record_snapshot snapshot;
    gate_lock();
    snapshot.next = record->next;
    snapshot.identity = record->identity;
    snapshot.service_pending = record->service_pending;
    snapshot.service_queued = record->service_queued;
    snapshot.notification_pins = record->lifetime.notification_pins;
    snapshot.running_harts = record->lifetime.running_harts;
    snapshot.flags = record->lifetime.flags;
    gate_unlock();
    return snapshot;
}

static struct registry_snapshot snapshot_registry(
    const struct p4_task_registry *registry)
{
    struct registry_snapshot snapshot;
    gate_lock();
    snapshot.head = registry->head;
    snapshot.count = registry->count;
    gate_unlock();
    return snapshot;
}

static void check_record(
    const struct p4_task_registry_record *record,
    const struct record_snapshot *expected)
{
    struct record_snapshot actual = snapshot_record(record);
    CHECK(actual.next == expected->next);
    CHECK(actual.identity == expected->identity);
    CHECK(actual.service_pending == expected->service_pending);
    CHECK(actual.service_queued == expected->service_queued);
    CHECK(actual.notification_pins == expected->notification_pins);
    CHECK(actual.running_harts == expected->running_harts);
    CHECK(actual.flags == expected->flags);
}

static void check_registry(
    const struct p4_task_registry *registry,
    struct p4_task_registry_record *head,
    uint32_t count)
{
    struct registry_snapshot actual = snapshot_registry(registry);
    CHECK(actual.head == head);
    CHECK(actual.count == count);
}

static int register_task(
    struct p4_task_registry *registry,
    struct p4_task_registry_record *record,
    struct Task *identity)
{
    int result;
    gate_lock();
    result = p4_task_registry_register_locked(registry, record, identity);
    gate_unlock();
    return result;
}

static int pin_task(
    struct p4_task_registry *registry,
    struct Task *identity)
{
    int result;
    gate_lock();
    result = p4_task_registry_pin_locked(registry, identity);
    gate_unlock();
    return result;
}

static int unpin_task(
    struct p4_task_registry *registry,
    struct Task *identity)
{
    int result;
    gate_lock();
    result = p4_task_registry_unpin_locked(registry, identity);
    gate_unlock();
    return result;
}

static int enter_running(
    struct p4_task_registry *registry,
    struct Task *identity,
    uint32_t hart)
{
    int result;
    gate_lock();
    result = p4_task_registry_enter_running_locked(registry, identity, hart);
    gate_unlock();
    return result;
}

static int leave_running(
    struct p4_task_registry *registry,
    struct Task *identity,
    uint32_t hart)
{
    int result;
    gate_lock();
    result = p4_task_registry_leave_running_locked(registry, identity, hart);
    gate_unlock();
    return result;
}

static int retire_for_service(
    struct p4_task_registry *registry,
    struct Task *identity)
{
    int result;
    gate_lock();
    result = p4_task_registry_retire_for_service_locked(registry, identity);
    gate_unlock();
    return result;
}

static struct Task *take_ready(
    struct p4_task_registry *registry,
    struct Task *identity)
{
    struct Task *result;
    gate_lock();
    result = p4_task_registry_take_service_ready_locked(registry, identity);
    gate_unlock();
    return result;
}

static struct p4_task_registry_record *claim_service(
    struct p4_task_registry *registry,
    struct Task *identity)
{
    struct p4_task_registry_record *result;
    gate_lock();
    result = p4_task_registry_claim_service_locked(registry, identity);
    gate_unlock();
    return result;
}

static struct p4_task_registry_record *claim_generic(
    struct p4_task_registry *registry,
    struct Task *identity)
{
    struct p4_task_registry_record *result;
    gate_lock();
    result = p4_task_registry_claim_reclaim_locked(registry, identity);
    gate_unlock();
    return result;
}

static void test_ready_requires_pin_and_both_hart_drains(void)
{
    uint32_t hart;

    for (hart = 0; hart < P4_TASK_LIFETIME_HARTS; ++hart)
    {
        struct p4_task_registry registry;
        struct p4_task_registry_record record = {0};
        struct Task *identity = calloc(1, sizeof(*identity));
        struct record_snapshot before;
        struct p4_task_registry_record *detached;

        CHECK(identity != NULL);
        p4_task_registry_init(&registry);
        CHECK(register_task(&registry, &record, identity));
        CHECK(pin_task(&registry, identity)); /* ordinary notification/user */
        CHECK(enter_running(&registry, identity, hart));
        CHECK(retire_for_service(&registry, identity));
        CHECK(record.service_pending == 1u);
        CHECK(record.service_queued == 0u);
        CHECK(record.lifetime.notification_pins == 2u);
        CHECK(record.lifetime.running_harts == (UINT32_C(1) << hart));

        before = snapshot_record(&record);
        CHECK(take_ready(&registry, identity) == NULL);
        check_record(&record, &before); /* both ordinary pin and RUN remain */

        CHECK(unpin_task(&registry, identity));
        CHECK(record.lifetime.notification_pins == 1u);
        before = snapshot_record(&record);
        CHECK(take_ready(&registry, identity) == NULL);
        check_record(&record, &before); /* RUN owner still prevents notice */

        CHECK(leave_running(&registry, identity, hart));
        CHECK(record.lifetime.running_harts == 0u);
        CHECK(record.lifetime.notification_pins == 1u);
        __asan_poison_memory_region(identity, sizeof(*identity));

        /* The ready reservation treats Task identity as opaque. It does not
         * inspect the now-poisoned Task allocation or detach the record. */
        CHECK(take_ready(&registry, identity) == identity);
        CHECK(record.service_queued == 1u);
        check_registry(&registry, &record, 1);
        before = snapshot_record(&record);
        CHECK(take_ready(&registry, identity) == NULL); /* exactly once */
        check_record(&record, &before);
        CHECK(!pin_task(&registry, identity)); /* retirement blocks new pins */
        check_record(&record, &before);
        CHECK(!enter_running(&registry, identity, hart ^ 1u));
        check_record(&record, &before);
        CHECK(claim_generic(&registry, identity) == NULL);
        check_record(&record, &before);
        CHECK(!unpin_task(&registry, identity)); /* designated pin retained */
        check_record(&record, &before);

        detached = claim_service(&registry, identity); /* queued is accepted */
        CHECK(detached == &record);
        CHECK(detached->identity == identity);
        CHECK(detached->service_pending == 0u);
        CHECK(detached->service_queued == 0u);
        CHECK(detached->lifetime.notification_pins == 0u);
        CHECK(detached->lifetime.running_harts == 0u);
        CHECK(detached->lifetime.flags ==
              (P4_TASK_LIFETIME_RETIRED | P4_TASK_LIFETIME_CLAIMED));
        check_registry(&registry, NULL, 0);
        __asan_unpoison_memory_region(identity, sizeof(*identity));
        free(identity);
    }
}

static void test_independent_reservations_and_pending_claim(void)
{
    struct p4_task_registry registry;
    struct p4_task_registry_record records[2] = {{0}};
    struct Task identities[2] = {{{0}}, {{0}}};
    struct record_snapshot before;

    p4_task_registry_init(&registry);
    CHECK(register_task(&registry, &records[0], &identities[0]));
    CHECK(register_task(&registry, &records[1], &identities[1]));
    CHECK(retire_for_service(&registry, &identities[0]));
    CHECK(retire_for_service(&registry, &identities[1]));
    CHECK(take_ready(&registry, &identities[0]) == &identities[0]);
    CHECK(records[0].service_queued == 1u);
    CHECK(records[1].service_queued == 0u);
    CHECK(registry.count == 2u); /* reserve is not detach or enqueue */

    before = snapshot_record(&records[0]);
    CHECK(take_ready(&registry, &identities[0]) == NULL);
    check_record(&records[0], &before);
    CHECK(take_ready(&registry, &identities[1]) == &identities[1]);
    CHECK(records[1].service_queued == 1u);
    CHECK(claim_generic(&registry, &identities[0]) == NULL);
    CHECK(claim_generic(&registry, &identities[1]) == NULL);
    CHECK(claim_service(&registry, &identities[0]) == &records[0]);
    CHECK(claim_service(&registry, &identities[1]) == &records[1]);
    check_registry(&registry, NULL, 0);
}

static void test_corrupt_queued_state_fails_without_mutation(void)
{
    struct p4_task_registry registry;
    struct p4_task_registry_record record = {0};
    struct Task identity = {{0}};
    struct Task unrelated = {{0}};
    struct record_snapshot before;

    p4_task_registry_init(&registry);
    CHECK(register_task(&registry, &record, &identity));
    CHECK(retire_for_service(&registry, &identity));

#define BAD_READY_CASE(change) do { \
    gate_lock(); \
    change; \
    gate_unlock(); \
    before = snapshot_record(&record); \
    CHECK(take_ready(&registry, &identity) == NULL); \
    check_record(&record, &before); \
    gate_lock(); \
    record.service_pending = 1u; \
    record.service_queued = 0u; \
    record.lifetime.notification_pins = 1u; \
    record.lifetime.running_harts = 0u; \
    record.lifetime.flags = P4_TASK_LIFETIME_RETIRED; \
    gate_unlock(); \
} while (0)

    BAD_READY_CASE(record.service_queued = 2u);
    BAD_READY_CASE(record.service_queued = 1u; record.service_pending = 0u);
    BAD_READY_CASE(record.service_queued = 1u; record.lifetime.flags = 0u);
    BAD_READY_CASE(record.service_queued = 1u;
                   record.lifetime.notification_pins = 2u);
    BAD_READY_CASE(record.service_queued = 1u;
                   record.lifetime.running_harts = UINT32_C(1));
#undef BAD_READY_CASE

    before = snapshot_record(&record);
    CHECK(take_ready(&registry, NULL) == NULL);
    check_record(&record, &before);
    CHECK(take_ready(&registry, &unrelated) == NULL);
    check_record(&record, &before);
    CHECK(registry.count == 1u && registry.head == &record);
}

static void test_registration_claim_and_list_corruption(void)
{
    struct p4_task_registry registry;
    struct p4_task_registry_record record = {0};
    struct p4_task_registry_record dirty = {0};
    struct Task identity = {{0}};
    struct record_snapshot before;
    struct registry_snapshot list_before;
    struct Task *invalid = (struct Task *)(uintptr_t)1u;

    p4_task_registry_init(&registry);
    dirty.service_queued = 1u;
    before = snapshot_record(&dirty);
    CHECK(!register_task(&registry, &dirty, &identity));
    check_record(&dirty, &before);
    CHECK(register_task(&registry, &record, &identity));
    CHECK(retire_for_service(&registry, &identity));
    before = snapshot_record(&record);
    CHECK(take_ready(&registry, invalid) == NULL);
    check_record(&record, &before);

    gate_lock();
    registry.count = 0u; /* nonempty list with inconsistent count */
    gate_unlock();
    before = snapshot_record(&record);
    list_before = snapshot_registry(&registry);
    CHECK(take_ready(&registry, &identity) == NULL);
    check_record(&record, &before);
    CHECK(snapshot_registry(&registry).head == list_before.head);
    CHECK(snapshot_registry(&registry).count == list_before.count);

    gate_lock();
    registry.count = 1u;
    record.next = &record; /* cycle bounded by count */
    gate_unlock();
    before = snapshot_record(&record);
    list_before = snapshot_registry(&registry);
    CHECK(take_ready(&registry, &identity) == NULL);
    check_record(&record, &before);
    CHECK(snapshot_registry(&registry).head == list_before.head);
    CHECK(snapshot_registry(&registry).count == list_before.count);
}

int main(void)
{
    test_ready_requires_pin_and_both_hart_drains();
    test_independent_reservations_and_pending_claim();
    test_corrupt_queued_state_fails_without_mutation();
    test_registration_claim_and_list_corruption();
    printf("p4-service-ready-test: %u checks passed\n", checks);
    return EXIT_SUCCESS;
}
