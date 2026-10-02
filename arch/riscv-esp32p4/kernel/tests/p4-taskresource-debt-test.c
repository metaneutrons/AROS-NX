/* Focused tests for the actual bounded Task lifetime registry debt leaf. */
#define _DEFAULT_SOURCE 1
#define __AROSEXEC_SMP__ 1
#define __AROSPLATFORM_SMP__ 1

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

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
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,       \
                    #condition);                                            \
            exit(EXIT_FAILURE);                                              \
        }                                                                    \
    } while (0)

static void gate_lock(void)
{
    CHECK(pthread_mutex_lock(&lifetime_gate) == 0);
}

static void gate_unlock(void)
{
    CHECK(pthread_mutex_unlock(&lifetime_gate) == 0);
}

static struct Task *opaque_identity(void *page, size_t offset)
{
    return (struct Task *)((unsigned char *)page + offset);
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

static int observer(struct p4_task_registry *registry,
                    struct Task *identity, uint32_t mark_unobserved)
{
    int result;
    gate_lock();
    result = p4_task_registry_taskresource_observer_locked(
        registry, identity, mark_unobserved);
    gate_unlock();
    return result;
}

static int pin_identity(struct p4_task_registry *registry,
                        struct Task *identity)
{
    int result;
    gate_lock();
    result = p4_task_registry_pin_locked(registry, identity);
    gate_unlock();
    return result;
}

static int unpin_identity(struct p4_task_registry *registry,
                          struct Task *identity)
{
    int result;
    gate_lock();
    result = p4_task_registry_unpin_locked(registry, identity);
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

static int protect_worker(struct p4_task_registry *registry,
                          struct Task *identity)
{
    int result;
    gate_lock();
    result = p4_task_registry_protect_worker_locked(registry, identity);
    gate_unlock();
    return result;
}

static int begin_wait(struct p4_task_registry *registry,
                      struct Task *identity, uint32_t *generation)
{
    int result;
    gate_lock();
    result = p4_task_registry_wait_begin_locked(registry, identity,
        P4_TASK_WAIT_SINGLE, (const void *)(uintptr_t)0x1000u,
        (const void *)(uintptr_t)0x2000u, 0, generation);
    gate_unlock();
    return result;
}

static int validate_registry(struct p4_task_registry *registry)
{
    int result;
    gate_lock();
    result = p4_task_registry_validate_locked(registry);
    gate_unlock();
    return result;
}

static int retire_identity(struct p4_task_registry *registry,
                           struct Task *identity)
{
    int result;
    gate_lock();
    result = p4_task_registry_retire_locked(registry, identity);
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

static int retire_for_service(struct p4_task_registry *registry,
                              struct Task *identity)
{
    int result;
    gate_lock();
    result = p4_task_registry_retire_for_service_locked(registry, identity);
    gate_unlock();
    return result;
}

static struct Task *take_service_ready(struct p4_task_registry *registry,
                                       struct Task *identity)
{
    struct Task *result;
    gate_lock();
    result = p4_task_registry_take_service_ready_locked(registry, identity);
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

static int claim_next_service(struct p4_task_registry *registry,
                              struct p4_task_registry_record **out_record)
{
    int result;
    gate_lock();
    result = p4_task_registry_claim_next_service_locked(registry, out_record);
    gate_unlock();
    return result;
}

static int claim_next_retained_service(
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

static int finish_retained_service(
    struct p4_task_registry *registry,
    struct p4_task_registry_record *record)
{
    int result;
    gate_lock();
    result = p4_task_registry_finish_retained_service_locked(registry, record);
    gate_unlock();
    return result;
}

static void test_marker_validation_and_ordinary_pin(void *page)
{
    struct p4_task_registry registry;
    struct p4_task_registry_record record = {0};
    struct p4_task_registry_record stale = {0};
    struct p4_task_registry_record before_record;
    struct p4_task_registry_record stale_before;
    struct Task *identity = opaque_identity(page, 64u);
    struct Task *missing = opaque_identity(page, 128u);

    p4_task_registry_init(&registry);
    CHECK(record.taskresource_unobserved == 0u);
    CHECK(register_record(&registry, &record, identity));
    CHECK(validate_registry(&registry));

    CHECK(observer(&registry, identity, 0u) == 0);
    CHECK(observer(&registry, identity, 1u) == 0);
    CHECK(observer(&registry, missing, 1u) == 0);
    CHECK(observer(&registry, 0, 1u) == -1);
    CHECK(record.taskresource_unobserved == 0u);

    CHECK(pin_identity(&registry, identity));
    CHECK(observer(&registry, identity, 0u) == 1);
    CHECK(record.taskresource_unobserved == 0u);
    CHECK(observer(&registry, identity, 1u) == 1);
    CHECK(record.taskresource_unobserved == 1u);
    CHECK(observer(&registry, identity, 1u) == 1);
    CHECK(observer(&registry, identity, 0u) == 1);
    CHECK(record.taskresource_unobserved == 1u);

    gate_lock();
    before_record = record;
    CHECK(p4_task_registry_taskresource_observer_locked(
        &registry, identity, 2u) == -1);
    CHECK(memcmp(&record, &before_record, sizeof(record)) == 0);
    CHECK(p4_task_registry_validate_locked(&registry));
    record.taskresource_unobserved = 2u;
    before_record = record;
    CHECK(!p4_task_registry_validate_locked(&registry));
    CHECK(p4_task_registry_taskresource_observer_locked(
        &registry, identity, 1u) == -1);
    CHECK(memcmp(&record, &before_record, sizeof(record)) == 0);
    record.taskresource_unobserved = 1u;
    gate_unlock();

    stale.taskresource_unobserved = 1u;
    stale_before = stale;
    CHECK(!register_record(&registry, &stale, missing));
    CHECK(memcmp(&stale, &stale_before, sizeof(stale)) == 0);
    CHECK(validate_registry(&registry));
}

static void test_complete_durable_pin_floor(void *page)
{
    struct p4_task_registry registry;
    struct p4_task_registry_record record = {0};
    struct p4_task_registry_record before_record;
    struct Task *identity = opaque_identity(page, 256u);
    uint32_t wait_generation = 0u;

    p4_task_registry_init(&registry);
    CHECK(register_record(&registry, &record, identity));
    CHECK(enter_running(&registry, identity, 0u));
    CHECK(protect_worker(&registry, identity));
    CHECK(begin_wait(&registry, identity, &wait_generation));
    CHECK(wait_generation == 1u);

    /* Model a fully validated pending plus inflight semaphore batch. Their
     * durable pins and generation invariants are read by the real validator. */
    gate_lock();
    CHECK(record.lifetime.notification_pins == 2u);
    registry.semaphore_notification_serial = 2u;
    record.semaphore_pending = 1u;
    record.semaphore_pending_generation = 1u;
    record.semaphore_inflight = 1u;
    record.semaphore_inflight_generation = 2u;
    record.lifetime.notification_pins += 2u;
    CHECK(p4_task_registry_validate_locked(&registry));
    CHECK(record.lifetime.notification_pins == 4u);
    before_record = record;
    CHECK(p4_task_registry_taskresource_observer_locked(
        &registry, identity, 1u) == 0);
    CHECK(memcmp(&record, &before_record, sizeof(record)) == 0);
    CHECK(p4_task_registry_pin_locked(&registry, identity));
    CHECK(record.lifetime.notification_pins == 5u);
    CHECK(p4_task_registry_taskresource_observer_locked(
        &registry, identity, 0u) == 1);
    CHECK(record.taskresource_unobserved == 0u);
    CHECK(p4_task_registry_taskresource_observer_locked(
        &registry, identity, 1u) == 1);
    CHECK(record.taskresource_unobserved == 1u);
    CHECK(p4_task_registry_validate_locked(&registry));
    gate_unlock();
}

static void test_designated_construction_reference(void *page)
{
    struct p4_task_registry registry;
    struct p4_task_registry_record creator_record = {0};
    struct p4_task_registry_record child_record = {0};
    struct Task *creator = opaque_identity(page, 384u);
    struct Task *child = opaque_identity(page, 448u);
    uint32_t generation = UINT32_C(0xabcdef01);

    p4_task_registry_init(&registry);
    CHECK(register_record(&registry, &creator_record, creator));
    CHECK(enter_running(&registry, creator, 0u));
    gate_lock();
    CHECK(p4_task_registry_register_constructing_locked(
        &registry, &child_record, child, creator, 0u, &generation) == 1);
    CHECK(generation == 1u);
    CHECK(creator_record.construction_children == 1u);
    CHECK(creator_record.lifetime.notification_pins == 1u);
    CHECK(child_record.construction_creator == creator);
    CHECK(child_record.lifetime.notification_pins == 1u);
    CHECK(p4_task_registry_validate_locked(&registry));
    CHECK(p4_task_registry_taskresource_observer_locked(
        &registry, creator, 1u) == 0);
    CHECK(p4_task_registry_taskresource_observer_locked(
        &registry, child, 0u) == 1);
    CHECK(child_record.taskresource_unobserved == 0u);
    CHECK(p4_task_registry_taskresource_observer_locked(
        &registry, child, 1u) == 1);
    CHECK(child_record.taskresource_unobserved == 1u);
    CHECK(p4_task_registry_pin_locked(&registry, creator));
    CHECK(p4_task_registry_taskresource_observer_locked(
        &registry, creator, 1u) == 1);
    CHECK(creator_record.taskresource_unobserved == 1u);
    CHECK(p4_task_registry_validate_locked(&registry));
    gate_unlock();
}

static void test_retired_marker_and_reclaim_clear(void *page)
{
    struct p4_task_registry registry;
    struct p4_task_registry_record record = {0};
    struct Task *identity = opaque_identity(page, 512u);

    p4_task_registry_init(&registry);
    CHECK(register_record(&registry, &record, identity));
    CHECK(pin_identity(&registry, identity));
    CHECK(observer(&registry, identity, 1u) == 1);
    CHECK(retire_identity(&registry, identity));
    CHECK(record.taskresource_unobserved == 1u);
    CHECK(validate_registry(&registry));
    CHECK(observer(&registry, identity, 0u) == 0);
    CHECK(unpin_identity(&registry, identity));
    CHECK(claim_reclaim(&registry, identity) == &record);
    CHECK(record.taskresource_unobserved == 0u);
    CHECK(registry.head == 0 && registry.count == 0u);
}

static void prepare_service_record(struct p4_task_registry *registry,
                                   struct p4_task_registry_record *record,
                                   struct Task *identity)
{
    p4_task_registry_init(registry);
    memset(record, 0, sizeof(*record));
    CHECK(register_record(registry, record, identity));
    CHECK(pin_identity(registry, identity));
    CHECK(observer(registry, identity, 1u) == 1);
    CHECK(unpin_identity(registry, identity));
    CHECK(retire_for_service(registry, identity));
    CHECK(record->taskresource_unobserved == 1u);
    CHECK(validate_registry(registry));
    CHECK(observer(registry, identity, 0u) == 0);
}

static void test_service_claim_and_finish_clear(void *page)
{
    struct p4_task_registry registry;
    struct p4_task_registry_record record = {0};
    struct p4_task_registry_record *claimed = 0;
    struct p4_task_registry_record before_record;
    struct Task *identity = opaque_identity(page, 640u);

    prepare_service_record(&registry, &record, identity);
    CHECK(take_service_ready(&registry, identity) == identity);
    CHECK(claim_next_retained_service(&registry, &claimed) == 1);
    CHECK(claimed == &record);
    CHECK(record.service_inflight == 1u);
    CHECK(record.taskresource_unobserved == 0u);
    CHECK(validate_registry(&registry));

    gate_lock();
    before_record = record;
    CHECK(p4_task_registry_taskresource_observer_locked(
        &registry, identity, 1u) == 0);
    CHECK(memcmp(&record, &before_record, sizeof(record)) == 0);
    gate_unlock();

    CHECK(finish_retained_service(&registry, &record));
    CHECK(registry.head == 0 && registry.count == 0u);
    CHECK(record.taskresource_unobserved == 0u);
}

static void test_direct_and_queued_service_detach_clear(void *page)
{
    struct p4_task_registry registry;
    struct p4_task_registry_record record = {0};
    struct p4_task_registry_record *claimed = 0;
    struct Task *identity = opaque_identity(page, 768u);

    prepare_service_record(&registry, &record, identity);
    CHECK(claim_service(&registry, identity) == &record);
    CHECK(record.taskresource_unobserved == 0u);
    CHECK(registry.head == 0 && registry.count == 0u);

    memset(&record, 0, sizeof(record));
    CHECK(register_record(&registry, &record, identity));
    CHECK(pin_identity(&registry, identity));
    CHECK(observer(&registry, identity, 1u) == 1);
    CHECK(unpin_identity(&registry, identity));
    CHECK(retire_for_service(&registry, identity));
    CHECK(take_service_ready(&registry, identity) == identity);
    CHECK(claim_next_service(&registry, &claimed) == 1);
    CHECK(claimed == &record);
    CHECK(record.taskresource_unobserved == 0u);
    CHECK(registry.head == 0 && registry.count == 0u);
}

int main(void)
{
    long page_size = sysconf(_SC_PAGESIZE);
    void *page;

    CHECK(page_size > 0);
#if defined(MAP_ANON)
    page = mmap(0, (size_t)page_size, PROT_NONE,
                MAP_PRIVATE | MAP_ANON, -1, 0);
#else
    page = mmap(0, (size_t)page_size, PROT_NONE,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
#endif
    CHECK(page != MAP_FAILED);

    test_marker_validation_and_ordinary_pin(page);
    test_complete_durable_pin_floor(page);
    test_designated_construction_reference(page);
    test_retired_marker_and_reclaim_clear(page);
    test_service_claim_and_finish_clear(page);
    test_direct_and_queued_service_detach_clear(page);

    CHECK(munmap(page, (size_t)page_size) == 0);
    printf("PASS: %u checks; actual registry/lifetime sources, PROT_NONE identities\n",
           checks);
    return EXIT_SUCCESS;
}
