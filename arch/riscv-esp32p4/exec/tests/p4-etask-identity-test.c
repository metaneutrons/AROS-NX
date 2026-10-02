#define _DEFAULT_SOURCE 1

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "../p4_etask_identity.h"
#include "../p4_etask_identity.c"

static unsigned checks;
#define CHECK(expr) do { \
    ++checks; \
    if (!(expr)) { \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        exit(1); \
    } \
} while (0)

/* The metadata topology gate and ExecBase counter are caller-owned. This
 * wrapper models the required critical section and counter publication. */
static unsigned external_gate_depth;
static unsigned counter_updates;
static uint32_t shared_counter;

static int registry_init_under_gate(struct p4_etask_identity_registry *registry)
{
    CHECK(external_gate_depth == 0);
    ++external_gate_depth;
    int result = p4_etask_identity_registry_init(registry);
    --external_gate_depth;
    return result;
}

static uint32_t reserve_under_gate(
    struct p4_etask_identity_registry *registry,
    struct p4_etask_identity_record *record,
    const void *identity)
{
    CHECK(external_gate_depth == 0);
    ++external_gate_depth;
    uint32_t pid = p4_etask_identity_reserve(
        registry, record, identity, shared_counter);
    if (pid != 0)
    {
        shared_counter = pid;
        ++counter_updates;
    }
    --external_gate_depth;
    return pid;
}

static struct p4_etask_identity_record *release_under_gate(
    struct p4_etask_identity_registry *registry, const void *identity)
{
    CHECK(external_gate_depth == 0);
    ++external_gate_depth;
    struct p4_etask_identity_record *record =
        p4_etask_identity_release(registry, identity);
    --external_gate_depth;
    return record;
}

static struct p4_etask_identity_record *lookup_under_gate(
    const struct p4_etask_identity_registry *registry, const void *identity)
{
    CHECK(external_gate_depth == 0);
    ++external_gate_depth;
    struct p4_etask_identity_record *record =
        p4_etask_identity_lookup(registry, identity);
    --external_gate_depth;
    return record;
}

static void reset_gate_and_counter(uint32_t counter)
{
    external_gate_depth = 0;
    counter_updates = 0;
    shared_counter = counter;
}

static void test_init_reserve_release_and_duplicates(void)
{
    struct p4_etask_identity_registry registry;
    struct p4_etask_identity_record a, b, c, fresh;
    unsigned char identity_a = 0, identity_b = 0, identity_c = 0;
    memset(&registry, 0, sizeof(registry));
    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));
    memset(&c, 0, sizeof(c));
    memset(&fresh, 0, sizeof(fresh));
    reset_gate_and_counter(100);

    CHECK(registry_init_under_gate(&registry) == 1);
    struct p4_etask_identity_registry occupied_registry = {0};
    struct p4_etask_identity_record occupied_record = {0};
    occupied_record.identity = &identity_a;
    occupied_record.pid = 77;
    occupied_registry.head = &occupied_record;
    occupied_registry.count = 1;
    struct p4_etask_identity_registry occupied_before = occupied_registry;
    CHECK(registry_init_under_gate(&occupied_registry) == 0);
    CHECK(memcmp(&occupied_registry, &occupied_before,
                 sizeof(occupied_registry)) == 0);
    CHECK(reserve_under_gate(&registry, &a, &identity_a) == 101);
    CHECK(reserve_under_gate(&registry, &b, &identity_b) == 102);
    CHECK(reserve_under_gate(&registry, &c, &identity_c) == 103);
    CHECK(shared_counter == 103 && counter_updates == 3);
    CHECK(registry.count == 3 && registry.head == &a);
    CHECK(a.next == &b && b.next == &c && c.next == NULL);
    CHECK(a.pid == 101 && b.pid == 102 && c.pid == 103);

    struct p4_etask_identity_registry registry_before = registry;
    struct p4_etask_identity_record fresh_before = fresh;
    struct p4_etask_identity_record a_before = a, b_before = b, c_before = c;
    uint32_t counter_before = shared_counter;
    CHECK(reserve_under_gate(&registry, &fresh, &identity_b) == 0);
    CHECK(memcmp(&registry, &registry_before, sizeof(registry)) == 0);
    CHECK(memcmp(&fresh, &fresh_before, sizeof(fresh)) == 0);
    CHECK(memcmp(&a, &a_before, sizeof(a)) == 0);
    CHECK(memcmp(&b, &b_before, sizeof(b)) == 0);
    CHECK(memcmp(&c, &c_before, sizeof(c)) == 0);
    CHECK(shared_counter == counter_before && counter_updates == 3);

    b_before = b;
    a_before = a;
    c_before = c;
    registry_before = registry;
    CHECK(reserve_under_gate(&registry, &b, &identity_c) == 0);
    CHECK(memcmp(&registry, &registry_before, sizeof(registry)) == 0);
    CHECK(memcmp(&b, &b_before, sizeof(b)) == 0);
    CHECK(memcmp(&a, &a_before, sizeof(a)) == 0);
    CHECK(memcmp(&c, &c_before, sizeof(c)) == 0);
    CHECK(shared_counter == counter_before && counter_updates == 3);

    struct p4_etask_identity_record *detached =
        release_under_gate(&registry, &identity_b);
    CHECK(detached == &b);
    CHECK(detached->next == NULL && detached->identity == &identity_b);
    CHECK(detached->pid == 102);
    CHECK(registry.count == 2 && registry.head == &a && a.next == &c);
    CHECK(external_gate_depth == 0);
    CHECK(shared_counter == 103);

    registry_before = registry;
    CHECK(release_under_gate(&registry, &identity_b) == NULL);
    CHECK(memcmp(&registry, &registry_before, sizeof(registry)) == 0);

    fresh_before = b;
    registry_before = registry;
    CHECK(reserve_under_gate(&registry, &b, &identity_b) == 0);
    CHECK(memcmp(&registry, &registry_before, sizeof(registry)) == 0);
    CHECK(memcmp(&b, &fresh_before, sizeof(b)) == 0);
    CHECK(shared_counter == 103);

    CHECK(release_under_gate(&registry, &identity_a) == &a);
    CHECK(release_under_gate(&registry, &identity_c) == &c);
    CHECK(registry.head == NULL && registry.count == 0);
    CHECK(shared_counter == 103);

    memset(&b, 0, sizeof(b));
    CHECK(reserve_under_gate(&registry, &b, &identity_b) == 104);
    CHECK(registry.head == &b && registry.count == 1);
}

static void test_wrap_and_occupied_gaps(void)
{
    struct p4_etask_identity_registry registry;
    struct p4_etask_identity_record records[3];
    struct p4_etask_identity_record fresh;
    unsigned char identities[4];
    memset(&registry, 0, sizeof(registry));
    memset(records, 0, sizeof(records));
    memset(&fresh, 0, sizeof(fresh));
    records[0].identity = &identities[0];
    records[0].pid = 1024;
    records[0].next = &records[1];
    records[1].identity = &identities[1];
    records[1].pid = 1025;
    records[1].next = &records[2];
    records[2].identity = &identities[2];
    records[2].pid = 1027;
    registry.head = &records[0];
    registry.count = 3;
    reset_gate_and_counter(UINT32_MAX);

    CHECK(reserve_under_gate(&registry, &fresh, &identities[3]) == 1026);
    CHECK(shared_counter == 1026 && counter_updates == 1);
    CHECK(registry.count == 4 && registry.head == &records[0]);
    CHECK(records[0].next == &records[1]);
    CHECK(records[1].next == &fresh && fresh.pid == 1026);
    CHECK(fresh.next == &records[2] && records[2].pid == 1027);
}

static void expect_malformed_refusal(
    struct p4_etask_identity_registry *registry,
    struct p4_etask_identity_record *fresh,
    const void *identity,
    struct p4_etask_identity_record *first,
    struct p4_etask_identity_record *second)
{
    struct p4_etask_identity_registry registry_before = *registry;
    struct p4_etask_identity_record fresh_before = *fresh;
    struct p4_etask_identity_record first_before = {0}, second_before = {0};
    if (first != NULL)
        memcpy(&first_before, first, sizeof(first_before));
    if (second != NULL)
        memcpy(&second_before, second, sizeof(second_before));
    uint32_t counter_before = shared_counter;
    unsigned updates_before = counter_updates;
    CHECK(reserve_under_gate(registry, fresh, identity) == 0);
    CHECK(memcmp(registry, &registry_before, sizeof(*registry)) == 0);
    CHECK(memcmp(fresh, &fresh_before, sizeof(*fresh)) == 0);
    if (first != NULL)
        CHECK(memcmp(first, &first_before, sizeof(*first)) == 0);
    if (second != NULL)
        CHECK(memcmp(second, &second_before, sizeof(*second)) == 0);
    CHECK(shared_counter == counter_before && counter_updates == updates_before);

    registry_before = *registry;
    CHECK(release_under_gate(registry, identity) == NULL);
    CHECK(memcmp(registry, &registry_before, sizeof(*registry)) == 0);
    CHECK(external_gate_depth == 0);
}

static void test_corrupt_shapes_fail_bounded_and_unchanged(void)
{
    struct p4_etask_identity_registry registry;
    struct p4_etask_identity_record a, b, fresh;
    unsigned char id_a = 0, id_b = 0, id_new = 0;
    reset_gate_and_counter(500);

    memset(&registry, 0, sizeof(registry));
    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));
    memset(&fresh, 0, sizeof(fresh));
    a.identity = &id_a; a.pid = 10;
    registry.head = &a; registry.count = 2; /* short list */
    expect_malformed_refusal(&registry, &fresh, &id_new, &a, &b);

    memset(&registry, 0, sizeof(registry));
    memset(&a, 0, sizeof(a)); memset(&b, 0, sizeof(b)); memset(&fresh, 0, sizeof(fresh));
    a.identity = &id_a; a.pid = 10; a.next = &b;
    b.identity = &id_b; b.pid = 20; b.next = &a;
    registry.head = &a; registry.count = 2; /* cycle */
    expect_malformed_refusal(&registry, &fresh, &id_new, &a, &b);

    memset(&registry, 0, sizeof(registry));
    memset(&a, 0, sizeof(a)); memset(&b, 0, sizeof(b)); memset(&fresh, 0, sizeof(fresh));
    a.identity = &id_a; a.pid = 20; a.next = &b;
    b.identity = &id_b; b.pid = 10;
    registry.head = &a; registry.count = 2; /* out of PID order */
    expect_malformed_refusal(&registry, &fresh, &id_new, &a, &b);

    memset(&registry, 0, sizeof(registry));
    memset(&a, 0, sizeof(a)); memset(&b, 0, sizeof(b)); memset(&fresh, 0, sizeof(fresh));
    a.identity = &id_a; a.pid = 10; a.next = &b;
    b.identity = &id_b; b.pid = 10;
    registry.head = &a; registry.count = 2; /* duplicate PID */
    expect_malformed_refusal(&registry, &fresh, &id_new, &a, &b);

    memset(&registry, 0, sizeof(registry));
    memset(&a, 0, sizeof(a)); memset(&fresh, 0, sizeof(fresh));
    a.identity = &id_a; a.pid = 0;
    registry.head = &a; registry.count = 1; /* zero PID */
    expect_malformed_refusal(&registry, &fresh, &id_new, &a, &b);

    memset(&registry, 0, sizeof(registry));
    memset(&a, 0, sizeof(a)); memset(&fresh, 0, sizeof(fresh));
    a.identity = NULL; a.pid = 10;
    registry.head = &a; registry.count = 1; /* zero identity */
    expect_malformed_refusal(&registry, &fresh, &id_new, &a, &b);

    memset(&registry, 0, sizeof(registry));
    memset(&a, 0, sizeof(a)); memset(&fresh, 0, sizeof(fresh));
    a.identity = &id_a; a.pid = 10;
    registry.head = &a; registry.count = 0; /* head/count disagreement */
    expect_malformed_refusal(&registry, &fresh, &id_new, &a, &b);

    memset(&registry, 0, sizeof(registry));
    memset(&a, 0, sizeof(a)); memset(&b, 0, sizeof(b)); memset(&fresh, 0, sizeof(fresh));
    a.identity = &id_a; a.pid = 10; a.next = &b;
    b.identity = &id_b; b.pid = 20;
    registry.head = &a; registry.count = 1; /* extra tail */
    expect_malformed_refusal(&registry, &fresh, &id_new, &a, &b);

    memset(&registry, 0, sizeof(registry));
    memset(&fresh, 0, sizeof(fresh));
    registry.count = P4_ETASK_IDENTITY_LIMIT + 1u;
    expect_malformed_refusal(&registry, &fresh, &id_new, NULL, NULL);

    /* A valid full registry is also refused without updating the counter. */
    static struct p4_etask_identity_record full[P4_ETASK_IDENTITY_LIMIT];
    static struct p4_etask_identity_record full_before[P4_ETASK_IDENTITY_LIMIT];
    static unsigned char full_identity[P4_ETASK_IDENTITY_LIMIT + 1u];
    for (uint32_t i = 0; i < P4_ETASK_IDENTITY_LIMIT; ++i)
    {
        full[i].identity = &full_identity[i];
        full[i].pid = i + 1u;
        full[i].next = i + 1u < P4_ETASK_IDENTITY_LIMIT ? &full[i + 1u] : NULL;
    }
    memset(&registry, 0, sizeof(registry));
    registry.head = &full[0];
    registry.count = P4_ETASK_IDENTITY_LIMIT;
    memset(&fresh, 0, sizeof(fresh));
    memcpy(full_before, full, sizeof(full));
    uint32_t counter_before = shared_counter;
    unsigned updates_before = counter_updates;
    CHECK(reserve_under_gate(&registry, &fresh, &full_identity[P4_ETASK_IDENTITY_LIMIT]) == 0);
    CHECK(registry.head == &full[0] && registry.count == P4_ETASK_IDENTITY_LIMIT);
    CHECK(memcmp(full, full_before, sizeof(full)) == 0);
    CHECK(fresh.next == NULL && fresh.identity == NULL && fresh.pid == 0);
    CHECK(shared_counter == counter_before && counter_updates == updates_before);
}

static void test_duplicate_identity_release_refuses_unchanged(void)
{
    struct p4_etask_identity_registry registry;
    struct p4_etask_identity_record a, b;
    unsigned char shared_identity;
    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));
    a.identity = &shared_identity; a.pid = 7; a.next = &b;
    b.identity = &shared_identity; b.pid = 9;
    registry.head = &a; registry.count = 2;
    reset_gate_and_counter(9);
    struct p4_etask_identity_registry registry_before = registry;
    struct p4_etask_identity_record a_before = a, b_before = b;
    CHECK(release_under_gate(&registry, &shared_identity) == NULL);
    CHECK(memcmp(&registry, &registry_before, sizeof(registry)) == 0);
    CHECK(memcmp(&a, &a_before, sizeof(a)) == 0);
    CHECK(memcmp(&b, &b_before, sizeof(b)) == 0);
}

static void test_opaque_identity_is_never_dereferenced(void)
{
    size_t page_size = (size_t)sysconf(_SC_PAGESIZE);
    void *opaque = mmap(NULL, page_size, PROT_NONE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(opaque != MAP_FAILED);

    struct p4_etask_identity_registry registry = {0};
    struct p4_etask_identity_record record = {0};
    reset_gate_and_counter(0);
    CHECK(registry_init_under_gate(&registry) == 1);
    CHECK(reserve_under_gate(&registry, &record, opaque) == 1);
    CHECK(record.identity == opaque && registry.count == 1);
    struct p4_etask_identity_registry registry_before = registry;
    struct p4_etask_identity_record record_before = record;
    unsigned char absent = 0;
    CHECK(lookup_under_gate(NULL, opaque) == NULL);
    CHECK(lookup_under_gate(&registry, NULL) == NULL);
    CHECK(lookup_under_gate(&registry, &absent) == NULL);
    CHECK(lookup_under_gate(&registry, opaque) == &record);
    CHECK(memcmp(&registry, &registry_before, sizeof(registry)) == 0);
    CHECK(memcmp(&record, &record_before, sizeof(record)) == 0);
    CHECK(shared_counter == 1 && counter_updates == 1);
    struct p4_etask_identity_record *detached = release_under_gate(&registry, opaque);
    CHECK(detached == &record && detached->identity == opaque);
    CHECK(detached->pid == 1 && detached->next == NULL);
    CHECK(registry.head == NULL && registry.count == 0);
    CHECK(lookup_under_gate(&registry, opaque) == NULL);
    CHECK(munmap(opaque, page_size) == 0);
}

int main(void)
{
    test_init_reserve_release_and_duplicates();
    test_wrap_and_occupied_gaps();
    test_corrupt_shapes_fail_bounded_and_unchanged();
    test_duplicate_identity_release_refuses_unchanged();
    test_opaque_identity_is_never_dereferenced();
    CHECK(external_gate_depth == 0);
    printf("p4-etask-identity-test: %u checks passed\n", checks);
    return 0;
}
