/* Host fixture for the actual caller-serialized Task registry helpers. */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,         \
                    #condition);                                            \
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

static int pin_task(struct p4_task_registry *registry, struct Task *identity)
{
    int result;
    gate_lock();
    result = p4_task_registry_pin_locked(registry, identity);
    gate_unlock();
    return result;
}

static int unpin_task(struct p4_task_registry *registry,
                      struct Task *identity)
{
    int result;
    gate_lock();
    result = p4_task_registry_unpin_locked(registry, identity);
    gate_unlock();
    return result;
}

static int retire_task(struct p4_task_registry *registry,
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

static struct p4_task_registry_record *claim_reclaim(
    struct p4_task_registry *registry, struct Task *identity)
{
    struct p4_task_registry_record *result;
    gate_lock();
    result = p4_task_registry_claim_reclaim_locked(registry, identity);
    gate_unlock();
    return result;
}

static void check_registry(struct p4_task_registry *registry,
                           struct p4_task_registry_record *head,
                           uint32_t count)
{
    gate_lock();
    CHECK(registry->head == head);
    CHECK(registry->count == count);
    gate_unlock();
}

static void test_invalid_and_duplicate_inputs(void)
{
    struct p4_task_registry registry;
    struct p4_task_registry_record first = {0};
    struct p4_task_registry_record duplicate_identity = {0};
    struct p4_task_registry_record dirty = {0};
    struct Task a = {1};
    struct Task b = {2};
    struct Task *invalid = (struct Task *)(uintptr_t)1;
    struct p4_task_registry_record before;

    p4_task_registry_init(&registry);
    CHECK(!register_record(&registry, &first, 0));
    CHECK(!register_record(&registry, 0, &a));
    CHECK(!pin_task(&registry, 0));
    CHECK(!pin_task(&registry, invalid));
    CHECK(!unpin_task(&registry, invalid));
    CHECK(!retire_task(&registry, invalid));
    CHECK(!enter_running(&registry, invalid, 0));
    CHECK(!leave_running(&registry, invalid, 0));
    CHECK(claim_reclaim(&registry, invalid) == 0);
    check_registry(&registry, 0, 0);

    CHECK(register_record(&registry, &first, &a));
    before = duplicate_identity;
    CHECK(!register_record(&registry, &duplicate_identity, &a));
    CHECK(memcmp(&duplicate_identity, &before, sizeof(before)) == 0);
    check_registry(&registry, &first, 1);

    before = first;
    CHECK(!register_record(&registry, &first, &b));
    CHECK(memcmp(&first, &before, sizeof(first)) == 0);
    check_registry(&registry, &first, 1);

    gate_lock();
    before = first;
    CHECK(!p4_task_registry_unpin_locked(&registry, &a));
    CHECK(!p4_task_registry_enter_running_locked(
        &registry, &a, P4_TASK_LIFETIME_HARTS));
    CHECK(memcmp(&first, &before, sizeof(first)) == 0);
    gate_unlock();

    dirty.lifetime.notification_pins = 1;
    before = dirty;
    CHECK(!register_record(&registry, &dirty, &b));
    CHECK(memcmp(&dirty, &before, sizeof(dirty)) == 0);
    check_registry(&registry, &first, 1);

    CHECK(!pin_task(&registry, invalid));
    CHECK(!unpin_task(&registry, invalid));
    check_registry(&registry, &first, 1);
}

static void test_pin_retire_running_handoff_and_reclaim(void)
{
    struct p4_task_registry registry;
    struct p4_task_registry_record request_record = {0};
    struct p4_task_registry_record other_record = {0};
    struct Task request_task = {11};
    struct Task other_task = {12};
    struct p4_task_registry_record *detached;

    p4_task_registry_init(&registry);
    CHECK(register_record(&registry, &request_record, &request_task));
    CHECK(register_record(&registry, &other_record, &other_task));

    /* The queue's stack request owns this pin through delayed notification. */
    CHECK(pin_task(&registry, &request_task));
    CHECK(enter_running(&registry, &request_task, 0));
    CHECK(!enter_running(&registry, &request_task, 1));
    CHECK(retire_task(&registry, &request_task));
    CHECK(!retire_task(&registry, &request_task));
    CHECK(!pin_task(&registry, &request_task));
    CHECK(claim_reclaim(&registry, &request_task) == 0);
    CHECK(unpin_task(&registry, &request_task));
    CHECK(claim_reclaim(&registry, &request_task) == 0);
    CHECK(!leave_running(&registry, &request_task, 1));
    CHECK(claim_reclaim(&registry, &request_task) == 0);
    CHECK(leave_running(&registry, &request_task, 0));

    detached = claim_reclaim(&registry, &request_task);
    CHECK(detached == &request_record);
    check_registry(&registry, &other_record, 1);
    CHECK(claim_reclaim(&registry, &request_task) == 0);
    CHECK(!pin_task(&registry, &request_task));

    CHECK(!claim_reclaim(&registry, &other_task));
    CHECK(pin_task(&registry, &other_task));
    CHECK(retire_task(&registry, &other_task));
    CHECK(!pin_task(&registry, &other_task));
    CHECK(!claim_reclaim(&registry, &other_task));
    CHECK(unpin_task(&registry, &other_task));
    detached = claim_reclaim(&registry, &other_task);
    CHECK(detached == &other_record);
    check_registry(&registry, 0, 0);
}

static void test_corrupt_list_rejected_without_mutation(void)
{
    struct p4_task_registry registry;
    struct p4_task_registry_record record = {0};
    struct Task identity = {21};
    struct p4_task_lifetime before_lifetime;
    struct p4_task_registry_record *before_next;

    p4_task_registry_init(&registry);
    CHECK(register_record(&registry, &record, &identity));

    gate_lock();
    before_lifetime = record.lifetime;
    before_next = record.next;
    registry.count = 2;
    CHECK(!p4_task_registry_pin_locked(&registry, &identity));
    CHECK(memcmp(&record.lifetime, &before_lifetime,
                 sizeof(before_lifetime)) == 0);
    CHECK(record.next == before_next);
    CHECK(registry.head == &record && registry.count == 2);
    registry.count = 1;
    record.next = &record;
    CHECK(!p4_task_registry_retire_locked(&registry, &identity));
    CHECK(memcmp(&record.lifetime, &before_lifetime,
                 sizeof(before_lifetime)) == 0);
    CHECK(registry.head == &record && registry.count == 1);
    record.next = 0;
    record.lifetime.flags = UINT32_C(0x80);
    before_lifetime = record.lifetime;
    CHECK(!p4_task_registry_pin_locked(&registry, &identity));
    CHECK(memcmp(&record.lifetime, &before_lifetime,
                 sizeof(before_lifetime)) == 0);
    record.lifetime.flags = 0;
    gate_unlock();

    CHECK(retire_task(&registry, &identity));
    CHECK(claim_reclaim(&registry, &identity) == &record);
    check_registry(&registry, 0, 0);
}

static void test_capacity_refusal(void)
{
    struct p4_task_registry registry;
    struct p4_task_registry_record *records;
    struct p4_task_registry_record candidate = {0};
    struct p4_task_registry_record candidate_before;
    struct Task *identities;
    struct Task candidate_identity = {P4_TASK_REGISTRY_LIMIT + 1u};
    uint32_t i;
    struct p4_task_registry_record *last;

    records = calloc(P4_TASK_REGISTRY_LIMIT, sizeof(*records));
    identities = calloc(P4_TASK_REGISTRY_LIMIT, sizeof(*identities));
    CHECK(records != 0 && identities != 0);

    for (i = 0; i < P4_TASK_REGISTRY_LIMIT; ++i)
    {
        records[i].identity = &identities[i];
        records[i].next = i + 1u < P4_TASK_REGISTRY_LIMIT ?
                          &records[i + 1u] : 0;
        p4_task_lifetime_init(&records[i].lifetime);
    }

    registry.head = &records[0];
    registry.count = P4_TASK_REGISTRY_LIMIT;
    last = &records[P4_TASK_REGISTRY_LIMIT - 1u];
    candidate_before = candidate;
    CHECK(!register_record(&registry, &candidate, &candidate_identity));
    check_registry(&registry, &records[0], P4_TASK_REGISTRY_LIMIT);
    CHECK(records[P4_TASK_REGISTRY_LIMIT - 1u].next == 0);
    CHECK(last->identity == &identities[P4_TASK_REGISTRY_LIMIT - 1u]);
    CHECK(memcmp(&candidate, &candidate_before, sizeof(candidate)) == 0);

    free(identities);
    free(records);
}

int main(void)
{
    test_invalid_and_duplicate_inputs();
    test_pin_retire_running_handoff_and_reclaim();
    test_corrupt_list_rejected_without_mutation();
    test_capacity_refusal();
    printf("PASS: %u checks; actual registry and lifetime sources under host gate\n",
           checks);
    return EXIT_SUCCESS;
}
