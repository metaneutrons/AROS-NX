/* Host sanitizer fixture for the actual caller-serialized service registry. */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

struct Task
{
    uint32_t test_identity;
};

#include "../p4_task_lifetime.c"
#include "../p4_task_registry.c"

static pthread_mutex_t lifetime_gate = PTHREAD_MUTEX_INITIALIZER;
static unsigned int checks;

#define CHECK(condition)                                                     \
    do                                                                       \
    {                                                                        \
        ++checks;                                                            \
        if (!(condition))                                                    \
        {                                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,        \
                    #condition);                                            \
            exit(EXIT_FAILURE);                                              \
        }                                                                    \
    } while (0)

struct record_snapshot
{
    struct p4_task_registry_record *next;
    struct Task *identity;
    uint32_t service_pending;
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
    CHECK(actual.notification_pins == expected->notification_pins);
    CHECK(actual.running_harts == expected->running_harts);
    CHECK(actual.flags == expected->flags);
}

static void check_registry(
    const struct p4_task_registry *registry,
    struct p4_task_registry_record *head, uint32_t count)
{
    struct registry_snapshot actual = snapshot_registry(registry);
    CHECK(actual.head == head);
    CHECK(actual.count == count);
}

static int register_record(struct p4_task_registry *registry,
                           struct p4_task_registry_record *record,
                           struct Task *identity)
{
    int result;
    gate_lock();
    result = p4_task_registry_register_locked(registry, record, identity);
    gate_unlock();
    return result;
}

static int pin_record(struct p4_task_registry *registry, struct Task *identity)
{
    int result;
    gate_lock();
    result = p4_task_registry_pin_locked(registry, identity);
    gate_unlock();
    return result;
}

static int unpin_record(struct p4_task_registry *registry,
                        struct Task *identity)
{
    int result;
    gate_lock();
    result = p4_task_registry_unpin_locked(registry, identity);
    gate_unlock();
    return result;
}

static int retire_for_service(struct p4_task_registry *registry,
                              struct Task *identity)
{
    int result;
    gate_lock();
    result = p4_task_registry_retire_for_service_locked(registry, identity);
    gate_unlock();
    return result;
}

static int retire_without_service(struct p4_task_registry *registry,
                                  struct Task *identity)
{
    int result;
    gate_lock();
    result = p4_task_registry_retire_locked(registry, identity);
    gate_unlock();
    return result;
}

static int enter_running(struct p4_task_registry *registry,
                         struct Task *identity, uint32_t hart)
{
    int result;
    gate_lock();
    result = p4_task_registry_enter_running_locked(registry, identity, hart);
    gate_unlock();
    return result;
}

static int leave_running(struct p4_task_registry *registry,
                         struct Task *identity, uint32_t hart)
{
    int result;
    gate_lock();
    result = p4_task_registry_leave_running_locked(registry, identity, hart);
    gate_unlock();
    return result;
}

static struct p4_task_registry_record *claim_service(
    struct p4_task_registry *registry, struct Task *identity)
{
    struct p4_task_registry_record *result;
    gate_lock();
    result = p4_task_registry_claim_service_locked(registry, identity);
    gate_unlock();
    return result;
}

static struct p4_task_registry_record *claim_generic(
    struct p4_task_registry *registry, struct Task *identity)
{
    struct p4_task_registry_record *result;
    gate_lock();
    result = p4_task_registry_claim_reclaim_locked(registry, identity);
    gate_unlock();
    return result;
}

static struct record_snapshot record_state(
    struct p4_task_registry_record *record)
{
    struct record_snapshot snapshot = snapshot_record(record);
    return snapshot;
}

static void test_service_reference_and_atomic_claim(void)
{
    struct p4_task_registry registry;
    struct p4_task_registry_record record = {0};
    struct Task identity = {101};
    struct record_snapshot before;
    struct p4_task_registry_record *detached;

    p4_task_registry_init(&registry);
    CHECK(register_record(&registry, &record, &identity));
    CHECK(pin_record(&registry, &identity)); /* non-service outstanding work */
    CHECK(retire_for_service(&registry, &identity));
    CHECK(record.lifetime.notification_pins == 2);
    CHECK(record.service_pending == 1);
    CHECK(record.lifetime.flags == P4_TASK_LIFETIME_RETIRED);

    before = record_state(&record);
    CHECK(!retire_for_service(&registry, &identity)); /* duplicate */
    check_record(&record, &before);
    CHECK(!retire_without_service(&registry, &identity));
    check_record(&record, &before);
    CHECK(!pin_record(&registry, &identity));
    check_record(&record, &before);
    CHECK(claim_service(&registry, &identity) == NULL); /* extra pin remains */
    check_record(&record, &before);
    CHECK(claim_generic(&registry, &identity) == NULL);
    check_record(&record, &before);

    CHECK(unpin_record(&registry, &identity)); /* drain ordinary pin */
    CHECK(record.lifetime.notification_pins == 1);
    before = record_state(&record);
    CHECK(!unpin_record(&registry, &identity)); /* preserve service pin */
    check_record(&record, &before);
    CHECK(claim_generic(&registry, &identity) == NULL);
    check_record(&record, &before);

    detached = claim_service(&registry, &identity);
    CHECK(detached == &record);
    CHECK(detached->next == NULL && detached->identity == &identity);
    CHECK(detached->service_pending == 0);
    CHECK(detached->lifetime.notification_pins == 0);
    CHECK(detached->lifetime.running_harts == 0);
    CHECK(detached->lifetime.flags ==
          (P4_TASK_LIFETIME_RETIRED | P4_TASK_LIFETIME_CLAIMED));
    check_registry(&registry, NULL, 0);
    CHECK(claim_service(&registry, &identity) == NULL);
    CHECK(claim_generic(&registry, &identity) == NULL);
    before = record_state(detached);
    CHECK(!register_record(&registry, detached, &identity));
    check_record(detached, &before);
}

static void test_service_claim_waits_for_each_hart(void)
{
    struct p4_task_registry registry;
    struct p4_task_registry_record records[2] = {{0}};
    struct Task identities[2] = {{201}, {202}};

    p4_task_registry_init(&registry);
    for (uint32_t hart = 0; hart < 2; ++hart)
    {
        struct record_snapshot before;
        CHECK(register_record(&registry, &records[hart], &identities[hart]));
        CHECK(enter_running(&registry, &identities[hart], hart));
        CHECK(retire_for_service(&registry, &identities[hart]));
        before = record_state(&records[hart]);
        CHECK(claim_service(&registry, &identities[hart]) == NULL);
        check_record(&records[hart], &before);
        CHECK(leave_running(&registry, &identities[hart], hart));
        CHECK(claim_service(&registry, &identities[hart]) == &records[hart]);
    }
    check_registry(&registry, NULL, 0);
}

static void test_invalid_inputs_and_overflow_refuse_without_mutation(void)
{
    struct p4_task_registry registry;
    struct p4_task_registry_record record = {0};
    struct p4_task_registry_record dirty = {0};
    struct Task identity = {301};
    struct Task other = {302};
    struct Task *invalid = (struct Task *)(uintptr_t)1;
    struct record_snapshot before;

    p4_task_registry_init(&registry);
    CHECK(!retire_for_service(&registry, invalid));
    CHECK(claim_service(&registry, invalid) == NULL);
    CHECK(register_record(&registry, &record, &identity));
    CHECK(!retire_for_service(&registry, invalid));
    CHECK(claim_service(&registry, invalid) == NULL);
    check_registry(&registry, &record, 1);

    dirty.service_pending = 1;
    before = record_state(&dirty);
    CHECK(!register_record(&registry, &dirty, &other));
    check_record(&dirty, &before);
    check_registry(&registry, &record, 1);

    gate_lock();
    record.lifetime.notification_pins = UINT32_MAX;
    gate_unlock();
    before = record_state(&record);
    CHECK(!retire_for_service(&registry, &identity));
    check_record(&record, &before);
    check_registry(&registry, &record, 1);
}

static void test_corrupt_service_state_and_dual_running_mask(void)
{
    struct p4_task_registry registry;
    struct p4_task_registry_record record = {0};
    struct Task identity = {401};
    struct record_snapshot before;

    p4_task_registry_init(&registry);
    CHECK(register_record(&registry, &record, &identity));
    CHECK(retire_for_service(&registry, &identity));

    gate_lock();
    record.lifetime.flags = 0; /* pending service ref without retirement */
    gate_unlock();
    before = record_state(&record);
    CHECK(!pin_record(&registry, &identity));
    CHECK(!unpin_record(&registry, &identity));
    CHECK(claim_service(&registry, &identity) == NULL);
    check_record(&record, &before);

    gate_lock();
    record.lifetime.flags = P4_TASK_LIFETIME_RETIRED;
    gate_unlock();
    gate_lock();
    record.service_pending = 2;
    gate_unlock();
    before = record_state(&record);
    CHECK(!unpin_record(&registry, &identity));
    CHECK(claim_service(&registry, &identity) == NULL);
    CHECK(claim_generic(&registry, &identity) == NULL);
    check_record(&record, &before);

    gate_lock();
    record.service_pending = 1;
    record.lifetime.running_harts = UINT32_C(3);
    gate_unlock();
    before = record_state(&record);
    CHECK(!leave_running(&registry, &identity, 0));
    CHECK(claim_service(&registry, &identity) == NULL);
    CHECK(claim_generic(&registry, &identity) == NULL);
    check_record(&record, &before);

    gate_lock();
    record.service_pending = 1;
    record.lifetime.running_harts = 0;
    record.lifetime.notification_pins = 0;
    gate_unlock();
    before = record_state(&record); /* pending must own at least one pin */
    CHECK(!pin_record(&registry, &identity));
    CHECK(!unpin_record(&registry, &identity));
    CHECK(claim_service(&registry, &identity) == NULL);
    check_record(&record, &before);
}

static void test_generic_reclaim_remains_available(void)
{
    struct p4_task_registry registry;
    struct p4_task_registry_record record = {0};
    struct Task identity = {501};

    p4_task_registry_init(&registry);
    CHECK(register_record(&registry, &record, &identity));
    CHECK(retire_without_service(&registry, &identity));
    CHECK(record.service_pending == 0);
    CHECK(claim_generic(&registry, &identity) == &record);
    check_registry(&registry, NULL, 0);
}

static void test_detached_record_is_freed_outside_gate(void)
{
    struct p4_task_registry registry;
    struct p4_task_registry_record *record = calloc(1, sizeof(*record));
    struct p4_task_registry_record *detached;
    struct Task identity = {601};

    CHECK(record != NULL);
    p4_task_registry_init(&registry);
    CHECK(register_record(&registry, record, &identity));
    CHECK(retire_for_service(&registry, &identity));
    detached = claim_service(&registry, &identity);
    CHECK(detached == record);
    CHECK(detached->service_pending == 0);
    CHECK((detached->lifetime.flags & P4_TASK_LIFETIME_CLAIMED) != 0);
    check_registry(&registry, NULL, 0);

    /* The claim wrapper has released the host gate before caller reclamation. */
    CHECK(pthread_mutex_trylock(&lifetime_gate) == 0);
    CHECK(pthread_mutex_unlock(&lifetime_gate) == 0);
    free(detached);
    CHECK(claim_service(&registry, &identity) == NULL);
}

int main(void)
{
    test_service_reference_and_atomic_claim();
    test_service_claim_waits_for_each_hart();
    test_invalid_inputs_and_overflow_refuse_without_mutation();
    test_corrupt_service_state_and_dual_running_mask();
    test_generic_reclaim_remains_available();
    test_detached_record_is_freed_outside_gate();
    printf("PASS: %u checks; actual service registry under host lifetime gate\n",
           checks);
    return EXIT_SUCCESS;
}
