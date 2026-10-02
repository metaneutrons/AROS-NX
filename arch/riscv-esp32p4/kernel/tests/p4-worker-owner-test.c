/* Actual-source checks for the private permanent housekeeper pin. */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif

#include "../p4_task_lifetime.c"
#include "../p4_task_registry.c"

static pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
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

struct record_snapshot
{
    struct p4_task_registry_record *next;
    struct Task *identity;
    uint32_t service_pending;
    uint32_t service_queued;
    uint32_t worker_protected;
    uint32_t pins;
    uint32_t running_harts;
    uint32_t flags;
};

static void lock_gate(void)
{
    CHECK(pthread_mutex_lock(&gate) == 0);
}

static void unlock_gate(void)
{
    CHECK(pthread_mutex_unlock(&gate) == 0);
}

static struct record_snapshot snapshot_record(
    const struct p4_task_registry_record *record)
{
    struct record_snapshot result;
    result.next = record->next;
    result.identity = record->identity;
    result.service_pending = record->service_pending;
    result.service_queued = record->service_queued;
    result.worker_protected = record->worker_protected;
    result.pins = record->lifetime.notification_pins;
    result.running_harts = record->lifetime.running_harts;
    result.flags = record->lifetime.flags;
    return result;
}

static void check_unchanged(const struct p4_task_registry_record *record,
                            const struct record_snapshot *before)
{
    struct record_snapshot after = snapshot_record(record);
    CHECK(after.next == before->next);
    CHECK(after.identity == before->identity);
    CHECK(after.service_pending == before->service_pending);
    CHECK(after.service_queued == before->service_queued);
    CHECK(after.worker_protected == before->worker_protected);
    CHECK(after.pins == before->pins);
    CHECK(after.running_harts == before->running_harts);
    CHECK(after.flags == before->flags);
}

static int register_record(struct p4_task_registry *registry,
                           struct p4_task_registry_record *record,
                           struct Task *identity)
{
    int result;
    lock_gate();
    result = p4_task_registry_register_locked(registry, record, identity);
    unlock_gate();
    return result;
}

static int protect_worker(struct p4_task_registry *registry,
                          struct Task *identity)
{
    int result;
    lock_gate();
    result = p4_task_registry_protect_worker_locked(registry, identity);
    unlock_gate();
    return result;
}

static int is_worker(struct p4_task_registry *registry,
                     struct Task *identity)
{
    int result;
    lock_gate();
    result = p4_task_registry_is_worker_locked(registry, identity);
    unlock_gate();
    return result;
}

static int enter_running(struct p4_task_registry *registry,
                         struct Task *identity, uint32_t hart)
{
    int result;
    lock_gate();
    result = p4_task_registry_enter_running_locked(registry, identity, hart);
    unlock_gate();
    return result;
}

static int leave_running(struct p4_task_registry *registry,
                         struct Task *identity, uint32_t hart)
{
    int result;
    lock_gate();
    result = p4_task_registry_leave_running_locked(registry, identity, hart);
    unlock_gate();
    return result;
}

static int pin_task(struct p4_task_registry *registry, struct Task *identity)
{
    int result;
    lock_gate();
    result = p4_task_registry_pin_locked(registry, identity);
    unlock_gate();
    return result;
}

static int unpin_task(struct p4_task_registry *registry,
                      struct Task *identity)
{
    int result;
    lock_gate();
    result = p4_task_registry_unpin_locked(registry, identity);
    unlock_gate();
    return result;
}

static int retire_task(struct p4_task_registry *registry,
                       struct Task *identity)
{
    int result;
    lock_gate();
    result = p4_task_registry_retire_locked(registry, identity);
    unlock_gate();
    return result;
}

static int retire_for_service(struct p4_task_registry *registry,
                              struct Task *identity)
{
    int result;
    lock_gate();
    result = p4_task_registry_retire_for_service_locked(registry, identity);
    unlock_gate();
    return result;
}

static struct p4_task_registry_record *claim_reclaim(
    struct p4_task_registry *registry, struct Task *identity)
{
    struct p4_task_registry_record *result;
    lock_gate();
    result = p4_task_registry_claim_reclaim_locked(registry, identity);
    unlock_gate();
    return result;
}

static struct p4_task_registry_record *claim_service(
    struct p4_task_registry *registry, struct Task *identity)
{
    struct p4_task_registry_record *result;
    lock_gate();
    result = p4_task_registry_claim_service_locked(registry, identity);
    unlock_gate();
    return result;
}

static int claim_next_service(struct p4_task_registry *registry,
                              struct p4_task_registry_record **out)
{
    int result;
    lock_gate();
    result = p4_task_registry_claim_next_service_locked(registry, out);
    unlock_gate();
    return result;
}

static void corrupt_u32(struct p4_task_registry_record *record,
                        uint32_t *field, uint32_t value)
{
    (void)record;
    lock_gate();
    *field = value;
    unlock_gate();
}

static void test_unregistered_and_quiescent_records(struct Task *identity,
                                                     struct Task *unknown)
{
    struct p4_task_registry registry;
    struct p4_task_registry_record record = {0};
    struct p4_task_registry_record dirty = {0};
    struct record_snapshot before;

    p4_task_registry_init(&registry);
    CHECK(is_worker(&registry, NULL) == -1);
    CHECK(is_worker(&registry, unknown) == -1);
    CHECK(!protect_worker(&registry, NULL));
    CHECK(!protect_worker(&registry, unknown));

    dirty.worker_protected = 1u;
    before = snapshot_record(&dirty);
    CHECK(!register_record(&registry, &dirty, identity));
    check_unchanged(&dirty, &before);
    CHECK(register_record(&registry, &record, identity));
    CHECK(record.worker_protected == 0u);
    CHECK(is_worker(&registry, identity) == 0);
    before = snapshot_record(&record);
    CHECK(!protect_worker(&registry, identity)); /* no RUN owner */
    check_unchanged(&record, &before);
}

static void test_admission_and_lifetime(struct Task *identity, uint32_t hart)
{
    struct p4_task_registry registry;
    struct p4_task_registry_record record = {0};
    struct record_snapshot before;
    uint32_t other_hart = hart ^ UINT32_C(1);
    struct p4_task_registry_record *out =
        (struct p4_task_registry_record *)(uintptr_t)1u;

    p4_task_registry_init(&registry);
    CHECK(register_record(&registry, &record, identity));
    CHECK(enter_running(&registry, identity, hart));
    CHECK(p4_task_registry_running_owner_locked(&registry, identity) ==
          (int)hart + 1);
    CHECK(protect_worker(&registry, identity));
    CHECK(record.worker_protected == 1u);
    CHECK(record.lifetime.notification_pins == 1u);
    CHECK(record.lifetime.running_harts == (UINT32_C(1) << hart));
    CHECK(is_worker(&registry, identity) == 1);

    before = snapshot_record(&record);
    CHECK(!protect_worker(&registry, identity)); /* exactly once */
    check_unchanged(&record, &before);
    CHECK(!retire_task(&registry, identity));
    check_unchanged(&record, &before);
    CHECK(!retire_for_service(&registry, identity));
    check_unchanged(&record, &before);
    CHECK(!unpin_task(&registry, identity)); /* preserve permanent pin */
    check_unchanged(&record, &before);
    CHECK(!claim_reclaim(&registry, identity));
    check_unchanged(&record, &before);
    CHECK(claim_service(&registry, identity) == NULL);
    check_unchanged(&record, &before);
    CHECK(claim_next_service(&registry, &out) == 0);
    CHECK(out == NULL);

    CHECK(pin_task(&registry, identity)); /* transient pin may coexist */
    CHECK(record.lifetime.notification_pins == 2u);
    CHECK(unpin_task(&registry, identity));
    CHECK(record.lifetime.notification_pins == 1u);
    CHECK(is_worker(&registry, identity) == 1);

    CHECK(leave_running(&registry, identity, hart));
    CHECK(record.lifetime.running_harts == 0u);
    CHECK(enter_running(&registry, identity, other_hart));
    CHECK(record.lifetime.running_harts == (UINT32_C(1) << other_hart));
    CHECK(is_worker(&registry, identity) == 1);
    CHECK(leave_running(&registry, identity, other_hart));
}

static void test_overflow_and_multiple_owner_rollback(struct Task *identity)
{
    struct p4_task_registry registry;
    struct p4_task_registry_record record = {0};
    struct record_snapshot before;

    p4_task_registry_init(&registry);
    CHECK(register_record(&registry, &record, identity));
    CHECK(enter_running(&registry, identity, 0));
    corrupt_u32(&record, &record.lifetime.notification_pins, UINT32_MAX);
    before = snapshot_record(&record);
    CHECK(!protect_worker(&registry, identity));
    check_unchanged(&record, &before);

    corrupt_u32(&record, &record.lifetime.notification_pins, 0u);
    corrupt_u32(&record, &record.lifetime.running_harts, UINT32_C(3));
    before = snapshot_record(&record);
    CHECK(!protect_worker(&registry, identity)); /* corrupt dual owner */
    check_unchanged(&record, &before);
    CHECK(is_worker(&registry, identity) == -1);
}

static void test_corrupt_protected_shape(struct Task *identity)
{
    struct p4_task_registry registry;
    struct p4_task_registry_record record = {0};
    struct record_snapshot before;
    struct p4_task_registry_record *out;

    p4_task_registry_init(&registry);
    CHECK(register_record(&registry, &record, identity));
    CHECK(enter_running(&registry, identity, 1));
    CHECK(protect_worker(&registry, identity));

#define CHECK_CORRUPT_REFUSAL(field, value, original)                         \
    do                                                                       \
    {                                                                        \
        corrupt_u32(&record, &(field), (value));                              \
        before = snapshot_record(&record);                                    \
        CHECK(is_worker(&registry, identity) == -1);                         \
        CHECK(!pin_task(&registry, identity));                                \
        CHECK(!unpin_task(&registry, identity));                              \
        CHECK(!retire_task(&registry, identity));                             \
        CHECK(!retire_for_service(&registry, identity));                      \
        CHECK(claim_service(&registry, identity) == NULL);                   \
        out = (struct p4_task_registry_record *)(uintptr_t)1u;                \
        CHECK(claim_next_service(&registry, &out) == -1);                    \
        CHECK(out == NULL);                                                   \
        check_unchanged(&record, &before);                                   \
        corrupt_u32(&record, &(field), (original));                           \
    } while (0)

    CHECK_CORRUPT_REFUSAL(record.worker_protected, 2u, 1u);
    CHECK_CORRUPT_REFUSAL(record.lifetime.flags,
                          P4_TASK_LIFETIME_RETIRED, 0u);
    CHECK_CORRUPT_REFUSAL(record.lifetime.notification_pins, 0u, 1u);
    CHECK_CORRUPT_REFUSAL(record.service_pending, 1u, 0u);
    /* A legal queued-service tuple without worker protection is malformed
     * while protected because both service fields must remain zero. */
    corrupt_u32(&record, &record.lifetime.running_harts, 0u);
    corrupt_u32(&record, &record.lifetime.flags, P4_TASK_LIFETIME_RETIRED);
    corrupt_u32(&record, &record.service_pending, 1u);
    corrupt_u32(&record, &record.service_queued, 1u);
    before = snapshot_record(&record);
    CHECK(is_worker(&registry, identity) == -1);
    CHECK(!retire_for_service(&registry, identity));
    out = (struct p4_task_registry_record *)(uintptr_t)1u;
    CHECK(claim_next_service(&registry, &out) == -1);
    CHECK(out == NULL);
    check_unchanged(&record, &before);

#undef CHECK_CORRUPT_REFUSAL
}

static void test_nonworker_path_is_unchanged(struct Task *identity)
{
    struct p4_task_registry registry;
    struct p4_task_registry_record record = {0};
    struct p4_task_registry_record *detached;

    p4_task_registry_init(&registry);
    CHECK(register_record(&registry, &record, identity));
    CHECK(is_worker(&registry, identity) == 0);
    CHECK(pin_task(&registry, identity));
    CHECK(unpin_task(&registry, identity));
    CHECK(retire_task(&registry, identity));
    CHECK(is_worker(&registry, identity) == 0); /* valid unprotected record */
    detached = claim_reclaim(&registry, identity);
    CHECK(detached == &record);
    CHECK(detached->worker_protected == 0u);
    CHECK(detached->next == NULL);
}

int main(void)
{
    long page_size = sysconf(_SC_PAGESIZE);
    void *mapping;
    size_t mapping_size;
    struct Task *identity0;
    struct Task *identity1;
    struct Task *identity2;

    if (page_size <= 0)
    {
        fputs("could not determine page size\n", stderr);
        return EXIT_FAILURE;
    }
    mapping_size = (size_t)page_size * 3u;
    mapping = mmap(NULL, mapping_size, PROT_NONE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapping == MAP_FAILED)
    {
        perror("mmap PROT_NONE identities");
        return EXIT_FAILURE;
    }

    identity0 = (struct Task *)mapping;
    identity1 = (struct Task *)((unsigned char *)mapping + (size_t)page_size);
    identity2 = (struct Task *)((unsigned char *)mapping +
                                ((size_t)page_size * 2u));

    test_unregistered_and_quiescent_records(identity0, identity1);
    test_admission_and_lifetime(identity0, 0u);
    test_admission_and_lifetime(identity1, 1u);
    test_overflow_and_multiple_owner_rollback(identity2);
    test_corrupt_protected_shape(identity2);
    test_nonworker_path_is_unchanged(identity0);

    if (munmap(mapping, mapping_size) != 0)
    {
        perror("munmap PROT_NONE identities");
        return EXIT_FAILURE;
    }
    printf("PASS: %u checks; actual worker-protection registry leaf; "
           "Task identities were PROT_NONE\n", checks);
    return EXIT_SUCCESS;
}
