/* Host fixture for the retained in-flight service state in the real registry. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif

struct Task
{
    uint32_t unused;
};

#include "../p4_task_lifetime.c"
#include "../p4_task_registry.c"

static unsigned int checks;
static unsigned int gate_depth;

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

static void gate_lock(void)
{
    CHECK(gate_depth == 0u);
    gate_depth = 1u;
}

static void gate_unlock(void)
{
    CHECK(gate_depth == 1u);
    gate_depth = 0u;
}

static struct Task *guarded_task_identity(void)
{
    long page_size = sysconf(_SC_PAGESIZE);
    void *page;

    CHECK(page_size > 0);
    page = mmap(0, (size_t)page_size, PROT_NONE,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(page != MAP_FAILED);
    return (struct Task *)page;
}

static void release_guarded_task_identity(struct Task *identity)
{
    long page_size = sysconf(_SC_PAGESIZE);

    CHECK(page_size > 0);
    CHECK(munmap(identity, (size_t)page_size) == 0);
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

static int retire_service(struct p4_task_registry *registry,
                          struct Task *identity)
{
    int result;
    gate_lock();
    result = p4_task_registry_retire_for_service_locked(registry, identity);
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

static int protect_worker(struct p4_task_registry *registry,
                          struct Task *identity)
{
    int result;
    gate_lock();
    result = p4_task_registry_protect_worker_locked(registry, identity);
    gate_unlock();
    return result;
}

static struct Task *take_ready(struct p4_task_registry *registry,
                               struct Task *identity)
{
    struct Task *result;
    gate_lock();
    result = p4_task_registry_take_service_ready_locked(registry, identity);
    gate_unlock();
    return result;
}

static int claim_next_retained(
    struct p4_task_registry *registry,
    struct p4_task_registry_record **out_record)
{
    int result;
    gate_lock();
    result = p4_task_registry_claim_next_retained_service_locked(
        registry, out_record);
    gate_unlock();
    return result;
}

static int finish_retained(
    struct p4_task_registry *registry,
    struct p4_task_registry_record *raw_record)
{
    int result;
    gate_lock();
    result = p4_task_registry_finish_retained_service_locked(
        registry, raw_record);
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

static struct p4_task_registry_record *claim_reclaim(
    struct p4_task_registry *registry, struct Task *identity)
{
    struct p4_task_registry_record *result;
    gate_lock();
    result = p4_task_registry_claim_reclaim_locked(registry, identity);
    gate_unlock();
    return result;
}

static int claim_next_service(
    struct p4_task_registry *registry,
    struct p4_task_registry_record **out_record)
{
    int result;
    gate_lock();
    result = p4_task_registry_claim_next_service_locked(registry, out_record);
    gate_unlock();
    return result;
}

static struct record_values
{
    struct p4_task_registry_record *next;
    struct Task *identity;
    uint32_t service_pending;
    uint32_t service_queued;
    uint32_t service_inflight;
    uint32_t worker_protected;
    uint32_t pins;
    uint32_t running;
    uint32_t flags;
} snapshot_record(const struct p4_task_registry_record *record)
{
    struct record_values value;
    value.next = record->next;
    value.identity = record->identity;
    value.service_pending = record->service_pending;
    value.service_queued = record->service_queued;
    value.service_inflight = record->service_inflight;
    value.worker_protected = record->worker_protected;
    value.pins = record->lifetime.notification_pins;
    value.running = record->lifetime.running_harts;
    value.flags = record->lifetime.flags;
    return value;
}

static void check_record_values(
    const struct p4_task_registry_record *record,
    const struct record_values *expected)
{
    struct record_values actual = snapshot_record(record);
    CHECK(actual.next == expected->next);
    CHECK(actual.identity == expected->identity);
    CHECK(actual.service_pending == expected->service_pending);
    CHECK(actual.service_queued == expected->service_queued);
    CHECK(actual.service_inflight == expected->service_inflight);
    CHECK(actual.worker_protected == expected->worker_protected);
    CHECK(actual.pins == expected->pins);
    CHECK(actual.running == expected->running);
    CHECK(actual.flags == expected->flags);
}

static void check_registry_values(
    const struct p4_task_registry *registry,
    struct p4_task_registry_record *head, uint32_t count)
{
    CHECK(registry->head == head);
    CHECK(registry->count == count);
}

static void make_queued_service(struct p4_task_registry *registry,
                                struct p4_task_registry_record *record,
                                struct Task *identity)
{
    CHECK(register_record(registry, record, identity));
    CHECK(retire_service(registry, identity));
    CHECK(take_ready(registry, identity) == identity);
}

static void test_retained_lifecycle_and_ordinary_refusals(void)
{
    struct p4_task_registry registry;
    struct p4_task_registry_record record = {0};
    struct p4_task_registry_record duplicate = {0};
    struct p4_task_registry_record *claimed = 0;
    struct p4_task_registry_record *old_claim = (void *)(uintptr_t)1;
    struct record_values saved;
    struct Task *identity = guarded_task_identity();
    struct p4_task_registry_record *unmapped_record =
        (struct p4_task_registry_record *)(void *)identity;
    int cleanup_complete = 0;

    p4_task_registry_init(&registry);
    make_queued_service(&registry, &record, identity);
    CHECK(claim_next_retained(&registry, &claimed) == 1);
    CHECK(claimed == &record);
    CHECK(record.identity == identity); /* raw identity is never dereferenced */
    CHECK(record.service_pending == 0);
    CHECK(record.service_queued == 0);
    CHECK(record.service_inflight == 1);
    CHECK(record.lifetime.notification_pins == 0);
    CHECK(record.lifetime.running_harts == 0);
    CHECK(record.lifetime.flags ==
          (P4_TASK_LIFETIME_RETIRED | P4_TASK_LIFETIME_CLAIMED));
    check_registry_values(&registry, &record, 1);

    /* A second claim sees no queued work and cannot take the in-flight item. */
    claimed = (void *)(uintptr_t)1;
    CHECK(claim_next_retained(&registry, &claimed) == 0);
    CHECK(claimed == 0);
    CHECK(claim_next_service(&registry, &old_claim) == 0);
    CHECK(old_claim == 0);

    saved = snapshot_record(&record);
    CHECK(!register_record(&registry, &duplicate, identity));
    check_record_values(&record, &saved);
    CHECK(duplicate.identity == 0 && duplicate.service_inflight == 0);

    CHECK(!pin_record(&registry, identity));
    CHECK(!unpin_record(&registry, identity));
    CHECK(!retire_task(&registry, identity));
    CHECK(!retire_service(&registry, identity));
    CHECK(!enter_running(&registry, identity, 0));
    CHECK(!leave_running(&registry, identity, 0));
    CHECK(claim_reclaim(&registry, identity) == 0);
    CHECK(claim_service(&registry, identity) == 0);
    CHECK(take_ready(&registry, identity) == 0);
    check_record_values(&record, &saved);
    check_registry_values(&registry, &record, 1);

    CHECK(finish_retained(&registry, unmapped_record) == 0);
    check_record_values(&record, &saved);
    check_registry_values(&registry, &record, 1);

    /* The leaf cannot observe external cleanup completion. Its caller must
     * invoke finish only after the explicit cleanup phase has ended. */
    CHECK(cleanup_complete == 0);
    cleanup_complete = 1;
    CHECK(cleanup_complete == 1);
    CHECK(finish_retained(&registry, &record) == 1);
    CHECK(registry.head == 0 && registry.count == 0);
    CHECK(record.next == 0 && record.service_inflight == 0);
    CHECK(record.identity == identity);
    CHECK(record.lifetime.flags ==
          (P4_TASK_LIFETIME_RETIRED | P4_TASK_LIFETIME_CLAIMED));
    CHECK(record.lifetime.notification_pins == 0);
    CHECK(finish_retained(&registry, &record) == 0); /* exactly once */
    CHECK(finish_retained(&registry, unmapped_record) == 0);
    release_guarded_task_identity(identity);
}

static void test_claim_eligibility_and_finish_preconditions(void)
{
    struct p4_task_registry registry;
    struct p4_task_registry_record record = {0};
    struct p4_task_registry_record dirty = {0};
    struct p4_task_registry_record *claimed = 0;
    struct Task *identity = guarded_task_identity();
    struct Task dirty_identity = {8};
    struct p4_task_registry_record unknown = {0};
    struct record_values saved;

    p4_task_registry_init(&registry);
    dirty.service_inflight = 1u;
    CHECK(!register_record(&registry, &dirty, &dirty_identity));
    CHECK(dirty.identity == 0 && dirty.service_inflight == 1u);
    dirty.service_inflight = 0;
    CHECK(register_record(&registry, &record, identity));
    CHECK(pin_record(&registry, identity));
    CHECK(retire_service(&registry, identity));
    saved = snapshot_record(&record);
    CHECK(take_ready(&registry, identity) == 0); /* ordinary pin remains */
    CHECK(claim_next_retained(&registry, &claimed) == 0);
    CHECK(claimed == 0);
    check_record_values(&record, &saved);
    CHECK(unpin_record(&registry, identity));
    CHECK(take_ready(&registry, identity) == identity);
    CHECK(claim_next_retained(&registry, &claimed) == 1);
    CHECK(claimed == &record);

    /* A valid but non-inflight record cannot be finished as a cleanup claim. */
    CHECK(finish_retained(&registry, &unknown) == 0);

    /* Corrupt each exclusive-owner invariant and require fail-closed behavior. */
    record.lifetime.notification_pins = 1;
    saved = snapshot_record(&record);
    CHECK(finish_retained(&registry, &record) == 0);
    CHECK(claim_next_retained(&registry, &claimed) == -1);
    check_record_values(&record, &saved);
    record.lifetime.notification_pins = 0;

    record.lifetime.running_harts = 1;
    saved = snapshot_record(&record);
    CHECK(finish_retained(&registry, &record) == 0);
    check_record_values(&record, &saved);
    record.lifetime.running_harts = 0;

    record.worker_protected = 1;
    saved = snapshot_record(&record);
    CHECK(finish_retained(&registry, &record) == 0);
    check_record_values(&record, &saved);
    record.worker_protected = 0;
    saved = snapshot_record(&record);
    CHECK(finish_retained(&registry, &record) == 1);
    CHECK(registry.head == 0 && registry.count == 0);
    release_guarded_task_identity(identity);

    /* Protected worker records remain live and cannot be queued or claimed. */
    {
        struct p4_task_registry protected_registry;
        struct p4_task_registry_record worker = {0};
        struct Task worker_identity = {7};
        struct record_values worker_before;

        p4_task_registry_init(&protected_registry);
        CHECK(register_record(&protected_registry, &worker, &worker_identity));
        CHECK(enter_running(&protected_registry, &worker_identity, 1));
        CHECK(protect_worker(&protected_registry, &worker_identity));
        worker_before = snapshot_record(&worker);
        claimed = (void *)(uintptr_t)1;
        CHECK(claim_next_retained(&protected_registry, &claimed) == 0);
        CHECK(claimed == 0);
        CHECK(finish_retained(&protected_registry, &worker) == 0);
        check_record_values(&worker, &worker_before);
        check_registry_values(&protected_registry, &worker, 1);
    }
}

static void test_running_readiness_and_legacy_detach(void)
{
    struct p4_task_registry registry;
    struct p4_task_registry_record record = {0};
    struct p4_task_registry_record *claimed = 0;
    struct Task *identity = guarded_task_identity();

    p4_task_registry_init(&registry);
    CHECK(register_record(&registry, &record, identity));
    CHECK(enter_running(&registry, identity, 0));
    CHECK(retire_service(&registry, identity));
    CHECK(take_ready(&registry, identity) == 0);
    CHECK(claim_next_retained(&registry, &claimed) == 0);
    CHECK(claimed == 0);
    CHECK(leave_running(&registry, identity, 0));
    CHECK(take_ready(&registry, identity) == identity);

    /* The legacy detached API keeps its old ownership transfer semantics. */
    CHECK(claim_next_service(&registry, &claimed) == 1);
    CHECK(claimed == &record);
    CHECK(record.next == 0 && record.service_inflight == 0);
    CHECK(record.service_pending == 0 && record.service_queued == 0);
    CHECK(registry.head == 0 && registry.count == 0);
    CHECK(finish_retained(&registry, &record) == 0);
    release_guarded_task_identity(identity);
}

static void test_multiple_retained_claims_are_counted_and_detached_once(void)
{
    struct p4_task_registry registry;
    struct p4_task_registry_record first = {0};
    struct p4_task_registry_record second = {0};
    struct p4_task_registry_record *claimed = 0;
    struct Task *first_identity = guarded_task_identity();
    struct Task *second_identity = guarded_task_identity();

    p4_task_registry_init(&registry);
    make_queued_service(&registry, &first, first_identity);
    make_queued_service(&registry, &second, second_identity);
    CHECK(registry.head == &second && registry.count == 2);

    CHECK(claim_next_retained(&registry, &claimed) == 1);
    CHECK(claimed == &second);
    CHECK(second.service_inflight == 1);
    CHECK(registry.head == &second && registry.count == 2);
    CHECK(claim_next_retained(&registry, &claimed) == 1);
    CHECK(claimed == &first);
    CHECK(first.service_inflight == 1);
    CHECK(registry.head == &second && registry.count == 2);

    /* Completing the tail preserves the still-inflight head and count. */
    CHECK(finish_retained(&registry, &first) == 1);
    CHECK(registry.head == &second && registry.count == 1);
    CHECK(second.service_inflight == 1);
    CHECK(finish_retained(&registry, &second) == 1);
    CHECK(registry.head == 0 && registry.count == 0);
    CHECK(first.next == 0 && first.service_inflight == 0);
    CHECK(second.next == 0 && second.service_inflight == 0);

    release_guarded_task_identity(first_identity);
    release_guarded_task_identity(second_identity);
}

static void test_alias_duplicate_and_malformed_refusals(void)
{
    struct p4_task_registry registry;
    struct p4_task_registry_record first = {0};
    struct p4_task_registry_record second = {0};
    struct p4_task_registry_record *out = 0;
    struct p4_task_registry_record *first_next;
    struct p4_task_registry_record *registry_head;
    struct record_values first_saved;
    struct record_values second_saved;
    struct Task *identity_a = guarded_task_identity();
    struct Task *identity_b = guarded_task_identity();

    p4_task_registry_init(&registry);
    make_queued_service(&registry, &first, identity_a);

    first_saved = snapshot_record(&first);
    CHECK(claim_next_retained(&registry, &first.next) == -1);
    check_record_values(&first, &first_saved);
    CHECK(registry.head == &first && registry.count == 1);

    registry_head = registry.head;
    CHECK(claim_next_retained(
              &registry, &registry.head) == -1);
    CHECK(registry.head == registry_head && registry.count == 1);
    check_record_values(&first, &first_saved);

    /* Duplicate identity on the selected queued record fails before mutation. */
    CHECK(register_record(&registry, &second, identity_b));
    CHECK(retire_service(&registry, identity_b));
    CHECK(take_ready(&registry, identity_b) == identity_b);
    second.identity = first.identity;
    first_saved = snapshot_record(&first);
    second_saved = snapshot_record(&second);
    out = 0;
    CHECK(claim_next_retained(&registry, &out) == -1);
    CHECK(out == 0);
    check_record_values(&first, &first_saved);
    check_record_values(&second, &second_saved);
    check_registry_values(&registry, &second, 2);

    /* Restore a unique ID, then corrupt the chain into a bounded cycle. */
    second.identity = identity_b;
    first_next = first.next;
    first.next = &second;
    second.next = &first;
    registry.head = &first;
    registry.count = 2;
    first_saved = snapshot_record(&first);
    second_saved = snapshot_record(&second);
    out = 0;
    CHECK(claim_next_retained(&registry, &out) == -1);
    CHECK(out == 0);
    CHECK(finish_retained(&registry, &first) == 0);
    check_record_values(&first, &first_saved);
    check_record_values(&second, &second_saved);
    CHECK(registry.head == &first && registry.count == 2);

    /* An invalid in-flight encoding is rejected by every full-list operation. */
    first.next = first_next;
    second.next = &first;
    registry.head = &second;
    registry.count = 2;
    first.service_inflight = 1u; /* missing RETIRED|CLAIMED encoding */
    first_saved = snapshot_record(&first);
    second_saved = snapshot_record(&second);
    out = 0;
    CHECK(claim_next_retained(&registry, &out) == -1);
    CHECK(out == 0);
    CHECK(finish_retained(&registry, &first) == 0);
    check_record_values(&first, &first_saved);
    check_record_values(&second, &second_saved);
    CHECK(registry.head == &second && registry.count == 2);

    first.service_inflight = 2u; /* the marker is a strict boolean */
    first_saved = snapshot_record(&first);
    out = 0;
    CHECK(claim_next_retained(&registry, &out) == -1);
    CHECK(out == 0);
    CHECK(finish_retained(&registry, &first) == 0);
    check_record_values(&first, &first_saved);

    first.service_inflight = 0;
    first.lifetime.flags |= P4_TASK_LIFETIME_CLAIMED;
    first_saved = snapshot_record(&first);
    out = 0;
    CHECK(claim_next_retained(&registry, &out) == -1);
    CHECK(out == 0);
    CHECK(finish_retained(&registry, &first) == 0);
    check_record_values(&first, &first_saved);

    release_guarded_task_identity(identity_a);
    release_guarded_task_identity(identity_b);
}

int main(void)
{
    test_retained_lifecycle_and_ordinary_refusals();
    test_claim_eligibility_and_finish_preconditions();
    test_running_readiness_and_legacy_detach();
    test_multiple_retained_claims_are_counted_and_detached_once();
    test_alias_duplicate_and_malformed_refusals();
    CHECK(gate_depth == 0u);
    printf("P4 retained service registry checks: %u passed\n", checks);
    return EXIT_SUCCESS;
}
