/* Host fixture for the actual P4 ETask identity binding and leaf. */
#define P4_ETASK_IDENTITY_BINDING_TEST 1

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../p4_etask_identity.h"
#include "../p4_etask_lifetime.h"
#include "../p4_etask_identity_binding.h"

typedef uint32_t ULONG;
typedef void *APTR;

#define MEMF_PUBLIC 1u
#define MEMF_CLEAR 2u
#define P4_SRAMDATA
#define P4_PUBLIC_SPIN_BAD_STATE 7

struct ETask
{
    uint32_t et_UniqueID;
};

struct ExecBase
{
    uint32_t ex_TaskID;
};

static struct ExecBase *test_base;
static unsigned checks;
static unsigned gate_depth;
static unsigned gate_lock_calls;
static unsigned gate_unlock_calls;
static unsigned alloc_calls;
static unsigned free_calls;
static unsigned live_records;
static unsigned fatal_calls;
static unsigned storage_free_calls;
static unsigned storage_free_expected_calls;
static unsigned storage_free_expected_owner_frees;
static struct ETask *storage_free_target;
static int allocation_fails;
static int interleave_allocation;
static struct ETask *interleave_task;
static struct ETask *outer_task;

enum watch_kind
{
    WATCH_NONE,
    WATCH_RESERVE,
    WATCH_UNCHANGED
};

static enum watch_kind watch;
static struct ETask *watch_etask;
static uint32_t watch_counter;
static uint32_t watch_pid;
static uint32_t watch_etask_pid;

#define CHECK(condition) do { \
    ++checks; \
    if (!(condition)) { \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", \
                __FILE__, __LINE__, #condition); \
        exit(1); \
    } \
} while (0)

static void watch_reserve(struct ETask *et, uint32_t counter, uint32_t pid)
{
    CHECK(gate_depth == 0);
    watch = WATCH_RESERVE;
    watch_etask = et;
    watch_counter = counter;
    watch_pid = pid;
}

static void watch_unchanged(struct ETask *et, uint32_t counter)
{
    CHECK(gate_depth == 0);
    watch = WATCH_UNCHANGED;
    watch_etask = et;
    watch_counter = counter;
    watch_etask_pid = et != NULL ? et->et_UniqueID : 0;
}

uint32_t krnP4ETaskGateLock(struct ExecBase *base)
{
    CHECK(base != NULL && base == test_base);
    CHECK(gate_depth == 0);
    ++gate_lock_calls;
    gate_depth = 1;
    return 0;
}

void krnP4ETaskGateUnlock(uint32_t token)
{
    CHECK(gate_depth == 1);
    CHECK(token == 0);
    if (watch == WATCH_RESERVE)
    {
        CHECK(test_base->ex_TaskID == watch_counter);
        CHECK(watch_etask != NULL);
        CHECK(watch_etask->et_UniqueID == watch_pid);
    }
    else if (watch == WATCH_UNCHANGED)
    {
        CHECK(test_base->ex_TaskID == watch_counter);
        if (watch_etask != NULL)
            CHECK(watch_etask->et_UniqueID == watch_etask_pid);
    }
    watch = WATCH_NONE;
    gate_depth = 0;
    ++gate_unlock_calls;
}

void krnP4PublicSpinFatal(int code)
{
    CHECK(gate_depth == 0);
    CHECK(code == P4_PUBLIC_SPIN_BAD_STATE);
    ++fatal_calls;
}

static void *AllocMem(size_t size, ULONG flags);
static void FreeMem(APTR memory, size_t size);

#include "../p4_etask_lifetime.c"
#include "../p4_etask_identity.c"
#include "../p4_etask_identity_binding.c"

static void *AllocMem(size_t size, ULONG flags)
{
    void *memory;

    CHECK(gate_depth == 0);
    CHECK(size == sizeof(struct p4_etask_owner_record));
    CHECK(flags == (MEMF_PUBLIC | MEMF_CLEAR));
    ++alloc_calls;
    if (allocation_fails)
        return NULL;

    memory = malloc(size);
    if (memory != NULL)
    {
        memset(memory, 0xa5, size);
        if (flags & MEMF_CLEAR)
            memset(memory, 0, size);
        ++live_records;
    }

    if (memory != NULL && interleave_allocation)
    {
        uint32_t expected_pid;

        interleave_allocation = 0;
        expected_pid = test_base->ex_TaskID + 1u;
        watch_reserve(interleave_task, expected_pid, expected_pid);
        CHECK(krnP4ETaskIdentityReserve(interleave_task, test_base));

        /* The outer allocation has not entered the gate yet. Its eventual
         * candidate must use the counter committed by the nested reserve. */
        expected_pid = test_base->ex_TaskID + 1u;
        watch_reserve(outer_task, expected_pid, expected_pid);
    }
    return memory;
}

static void FreeMem(APTR memory, size_t size)
{
    CHECK(gate_depth == 0);
    CHECK(memory != NULL);
    CHECK(size == sizeof(struct p4_etask_owner_record));
    CHECK(live_records > 0);
    --live_records;
    ++free_calls;
    free(memory);
}

void Exec_FreeETaskStorage(struct ETask *et, struct ExecBase *base)
{
    CHECK(gate_depth == 0u);
    CHECK(base == test_base && et != NULL && et == storage_free_target);
    CHECK(storage_free_calls + 1u == storage_free_expected_calls);
    CHECK(free_calls == storage_free_expected_owner_frees);
    CHECK(identities.head == NULL && identities.count == 0u);
    ++storage_free_calls;
}

static void reset_observers(struct ExecBase *base)
{
    test_base = base;
    gate_depth = 0;
    gate_lock_calls = 0;
    gate_unlock_calls = 0;
    alloc_calls = 0;
    free_calls = 0;
    live_records = 0;
    fatal_calls = 0;
    storage_free_calls = 0;
    storage_free_expected_calls = 0;
    storage_free_expected_owner_frees = 0;
    storage_free_target = NULL;
    allocation_fails = 0;
    interleave_allocation = 0;
    interleave_task = NULL;
    outer_task = NULL;
    watch = WATCH_NONE;
    watch_etask = NULL;
    watch_counter = 0;
    watch_pid = 0;
    watch_etask_pid = 0;
}

static void reset_private_registry(void)
{
    CHECK(gate_depth == 0);
    CHECK(identities.head == NULL && identities.count == 0);
    CHECK(live_records == 0);
}

static void test_refusals_leave_shared_state_unchanged(void)
{
    struct ExecBase base = { 75u };
    struct ETask fresh = { 0u };
    struct ETask already_reserved = { 9u };
    unsigned allocations_before;
    unsigned locks_before;

    reset_observers(&base);
    reset_private_registry();
    allocations_before = alloc_calls;
    locks_before = gate_lock_calls;
    CHECK(!krnP4ETaskIdentityReserve(NULL, &base));
    CHECK(alloc_calls == allocations_before && gate_lock_calls == locks_before);
    CHECK(base.ex_TaskID == 75u);

    CHECK(!krnP4ETaskIdentityReserve(&already_reserved, &base));
    CHECK(alloc_calls == allocations_before && gate_lock_calls == locks_before);
    CHECK(already_reserved.et_UniqueID == 9u && base.ex_TaskID == 75u);

    allocation_fails = 1;
    CHECK(!krnP4ETaskIdentityReserve(&fresh, &base));
    CHECK(alloc_calls == allocations_before + 1u);
    CHECK(gate_lock_calls == locks_before && gate_unlock_calls == 0u);
    CHECK(fresh.et_UniqueID == 0u && base.ex_TaskID == 75u);
    CHECK(identities.head == NULL && identities.count == 0u);
    CHECK(live_records == 0u && free_calls == 0u);
}

static void test_reserve_release_and_task_absence(void)
{
    struct ExecBase base = { 100u };
    struct ETask et = { 0u };
    const struct p4_etask_identity_record *record;
    const struct p4_etask_owner_record *owner;
    uint32_t token;
    int task_storage_exists = 1;

    reset_observers(&base);
    reset_private_registry();
    watch_reserve(&et, 101u, 101u);
    CHECK(krnP4ETaskIdentityReserve(&et, &base));
    CHECK(et.et_UniqueID == 101u && base.ex_TaskID == 101u);
    CHECK(gate_lock_calls == 1u && gate_unlock_calls == 1u);
    CHECK(alloc_calls == 1u && free_calls == 0u && live_records == 1u);
    CHECK(identities.count == 1u && identities.head != NULL);
    record = identities.head;
    CHECK(record->identity == &et && record->pid == 101u && record->next == NULL);
    owner = (const struct p4_etask_owner_record *)record;
    CHECK(owner->lifetime.initialized == 1u);
    CHECK(owner->lifetime.task_ref == 1u &&
          owner->lifetime.result_ref == 0u && owner->lifetime.closing == 0u);

    krnP4ETaskMetadataBeginCleanup(&et, &base);
    CHECK(owner->lifetime.task_ref == 1u && owner->lifetime.closing == 1u);
    token = krnP4ETaskGateLock(&base);
    CHECK(!p4_etask_lifetime_read_pin((struct p4_etask_lifetime *)&owner->lifetime));
    krnP4ETaskGateUnlock(token);
    CHECK(owner->lifetime.readers == 0u);
    krnP4ETaskMetadataRetainResult(&et, &base);
    CHECK(owner->lifetime.task_ref == 0u && owner->lifetime.result_ref == 1u);
    CHECK(owner->lifetime.closing == 1u && owner->lifetime.expunge == 0u);

    /* A Task may already be absent while retained ETask metadata still owns
     * this PID and a result-owner reference. */
    task_storage_exists = 0;
    CHECK(!task_storage_exists);
    CHECK(identities.count == 1u && identities.head->identity == &et);
    watch_unchanged(&et, 101u);
    CHECK(krnP4ETaskIdentityRelease(&et, &base) == 1);
    CHECK(identities.head == NULL && identities.count == 0u);
    CHECK(gate_lock_calls == 5u && gate_unlock_calls == 5u);
    CHECK(free_calls == 1u && live_records == 0u);
    CHECK(base.ex_TaskID == 101u && et.et_UniqueID == 101u);
    CHECK(fatal_calls == 0u);
}

static void test_duplicate_cleanup_and_result_retain_refuse(void)
{
    struct ExecBase base = { 150u };
    struct ETask et = { 0u };
    const struct p4_etask_owner_record *owner;
    unsigned before_fatal;

    reset_observers(&base);
    reset_private_registry();
    CHECK(krnP4ETaskIdentityReserve(&et, &base));
    owner = (const struct p4_etask_owner_record *)identities.head;

    krnP4ETaskMetadataBeginCleanup(&et, &base);
    before_fatal = fatal_calls;
    krnP4ETaskMetadataBeginCleanup(&et, &base);
    CHECK(fatal_calls == before_fatal + 1u);
    CHECK(owner->lifetime.task_ref == 1u && owner->lifetime.closing == 1u);
    CHECK(identities.count == 1u && free_calls == 0u && live_records == 1u);

    krnP4ETaskMetadataRetainResult(&et, &base);
    CHECK(owner->lifetime.task_ref == 0u && owner->lifetime.result_ref == 1u);
    before_fatal = fatal_calls;
    krnP4ETaskMetadataRetainResult(&et, &base);
    CHECK(fatal_calls == before_fatal + 1u);
    CHECK(owner->lifetime.task_ref == 0u && owner->lifetime.result_ref == 1u);
    CHECK(identities.count == 1u && free_calls == 0u && live_records == 1u);

    CHECK(krnP4ETaskIdentityRelease(&et, &base) == 1);
    CHECK(identities.head == NULL && identities.count == 0u);
    CHECK(free_calls == 1u && live_records == 0u && fatal_calls == before_fatal + 1u);
}

static void test_multiple_readers_defer_task_owner_release(void)
{
    struct ExecBase base = { 250u };
    struct ETask et = { 0u };
    struct p4_etask_owner_record *owner;
    unsigned before_fatal;

    reset_observers(&base);
    reset_private_registry();
    CHECK(krnP4ETaskIdentityReserve(&et, &base));
    owner = (struct p4_etask_owner_record *)identities.head;
    CHECK(krnP4ETaskMetadataReadPin(&et, &base));
    CHECK(krnP4ETaskMetadataReadPin(&et, &base));
    CHECK(owner->lifetime.readers == 2u);
    krnP4ETaskMetadataBeginCleanup(&et, &base);

    CHECK(!krnP4ETaskMetadataReadPin(&et, &base));
    CHECK(owner->lifetime.readers == 2u);

    before_fatal = fatal_calls;
    CHECK(krnP4ETaskIdentityRelease(&et, &base) == 0);
    CHECK(fatal_calls == before_fatal);
    CHECK(identities.count == 1u && identities.head == &owner->identity);
    CHECK(owner->lifetime.task_ref == 0u && owner->lifetime.result_ref == 0u);
    CHECK(owner->lifetime.expunge == 1u && owner->lifetime.readers == 2u);
    CHECK(owner->lifetime.reclaim_claimed == 0u);
    CHECK(free_calls == 0u && live_records == 1u);
    CHECK(identities.head->pid == 251u && et.et_UniqueID == 251u);
    CHECK(!krnP4ETaskMetadataReadPin(&et, &base));
    CHECK(owner->lifetime.readers == 2u && fatal_calls == before_fatal);

    storage_free_target = &et;
    storage_free_expected_calls = 1u;
    storage_free_expected_owner_frees = free_calls + 1u;
    krnP4ETaskMetadataReadUnpin(&et, &base);
    CHECK(identities.count == 1u && identities.head->pid == 251u);
    CHECK(free_calls == 0u && storage_free_calls == 0u);
    CHECK(!krnP4ETaskMetadataReadPin(&et, &base));
    krnP4ETaskMetadataReadUnpin(&et, &base);
    CHECK(identities.head == NULL && identities.count == 0u);
    CHECK(free_calls == 1u && live_records == 0u && storage_free_calls == 1u);
    CHECK(fatal_calls == before_fatal);

    before_fatal = fatal_calls;
    krnP4ETaskMetadataReadUnpin(&et, &base);
    CHECK(fatal_calls == before_fatal + 1u);
    CHECK(free_calls == 1u && storage_free_calls == 1u);
}

static void test_retained_result_readers_defer_final_storage_release(void)
{
    struct ExecBase base = { 275u };
    struct ETask et = { 0u };
    struct p4_etask_owner_record *owner;
    unsigned before_fatal;

    reset_observers(&base);
    reset_private_registry();
    CHECK(krnP4ETaskIdentityReserve(&et, &base));
    owner = (struct p4_etask_owner_record *)identities.head;
    CHECK(krnP4ETaskMetadataReadPin(&et, &base));
    krnP4ETaskMetadataBeginCleanup(&et, &base);
    CHECK(owner->lifetime.closing == 1u && owner->lifetime.readers == 1u);
    krnP4ETaskMetadataRetainResult(&et, &base);
    CHECK(owner->lifetime.task_ref == 0u && owner->lifetime.result_ref == 1u);
    CHECK(krnP4ETaskMetadataReadPin(&et, &base));
    CHECK(owner->lifetime.readers == 2u);

    before_fatal = fatal_calls;
    CHECK(krnP4ETaskIdentityRelease(&et, &base) == 0);
    CHECK(fatal_calls == before_fatal);
    CHECK(owner->lifetime.result_ref == 0u && owner->lifetime.expunge == 1u);
    CHECK(owner->lifetime.readers == 2u && owner->identity.pid == 276u);
    CHECK(identities.count == 1u && free_calls == 0u && storage_free_calls == 0u);
    CHECK(!krnP4ETaskMetadataReadPin(&et, &base));

    storage_free_target = &et;
    storage_free_expected_calls = 1u;
    storage_free_expected_owner_frees = free_calls + 1u;
    krnP4ETaskMetadataReadUnpin(&et, &base);
    CHECK(identities.count == 1u && owner->identity.pid == 276u);
    CHECK(free_calls == 0u && storage_free_calls == 0u);
    krnP4ETaskMetadataReadUnpin(&et, &base);
    CHECK(identities.count == 0u && free_calls == 1u);
    CHECK(storage_free_calls == 1u && fatal_calls == before_fatal);
}

static void test_outstanding_child_edge_refuses_reclamation(void)
{
    struct ExecBase base = { 350u };
    struct ETask et = { 0u };
    struct p4_etask_owner_record *owner;
    uint32_t token;
    unsigned before_fatal;

    reset_observers(&base);
    reset_private_registry();
    CHECK(krnP4ETaskIdentityReserve(&et, &base));
    owner = (struct p4_etask_owner_record *)identities.head;
    token = krnP4ETaskGateLock(&base);
    CHECK(p4_etask_lifetime_add_edge(&owner->lifetime));
    krnP4ETaskGateUnlock(token);
    krnP4ETaskMetadataBeginCleanup(&et, &base);

    before_fatal = fatal_calls;
    CHECK(krnP4ETaskIdentityRelease(&et, &base) == 0);
    CHECK(fatal_calls == before_fatal + 1u);
    CHECK(owner->lifetime.task_ref == 1u && owner->lifetime.closing == 1u);
    CHECK(owner->lifetime.child_edges == 1u);
    CHECK(identities.count == 1u && free_calls == 0u && live_records == 1u);

    token = krnP4ETaskGateLock(&base);
    CHECK(p4_etask_lifetime_drop_edge(&owner->lifetime));
    krnP4ETaskGateUnlock(token);
    CHECK(krnP4ETaskIdentityRelease(&et, &base) == 1);
    CHECK(identities.head == NULL && identities.count == 0u);
    CHECK(free_calls == 1u && live_records == 0u);
}

static void test_interleaved_reservations_share_counter(void)
{
    struct ExecBase base = { 200u };
    struct ETask outer = { 0u };
    struct ETask nested = { 0u };

    reset_observers(&base);
    reset_private_registry();
    interleave_allocation = 1;
    interleave_task = &nested;
    outer_task = &outer;
    CHECK(krnP4ETaskIdentityReserve(&outer, &base));
    CHECK(outer.et_UniqueID == 202u);
    CHECK(nested.et_UniqueID == 201u);
    CHECK(base.ex_TaskID == 202u);
    CHECK(identities.count == 2u);
    CHECK(identities.head->identity == &nested &&
          identities.head->pid == 201u);
    CHECK(identities.head->next->identity == &outer &&
          identities.head->next->pid == 202u);
    CHECK(gate_lock_calls == 2u && gate_unlock_calls == 2u);
    CHECK(alloc_calls == 2u && free_calls == 0u && live_records == 2u);

    watch_unchanged(&nested, 202u);
    CHECK(krnP4ETaskIdentityRelease(&nested, &base) == 1);
    watch_unchanged(&outer, 202u);
    CHECK(krnP4ETaskIdentityRelease(&outer, &base) == 1);
    CHECK(identities.head == NULL && identities.count == 0u);
    CHECK(live_records == 0u && free_calls == 2u && fatal_calls == 0u);
}

static void test_duplicate_identity_refusal_frees_temporary_record(void)
{
    struct ExecBase base = { 7u };
    struct ETask et = { 0u };
    uint32_t reserved_pid;
    unsigned alloc_before;
    unsigned free_before;

    reset_observers(&base);
    reset_private_registry();
    watch_reserve(&et, 8u, 8u);
    CHECK(krnP4ETaskIdentityReserve(&et, &base));
    reserved_pid = et.et_UniqueID;
    CHECK(identities.count == 1u && live_records == 1u);

    /* Model a corrupted/reinitialized ETask PID field while its identity is
     * still registered, to exercise the leaf duplicate-identity rejection. */
    et.et_UniqueID = 0u;
    alloc_before = alloc_calls;
    free_before = free_calls;
    watch_unchanged(&et, base.ex_TaskID);
    CHECK(!krnP4ETaskIdentityReserve(&et, &base));
    CHECK(et.et_UniqueID == 0u && base.ex_TaskID == reserved_pid);
    CHECK(identities.count == 1u && identities.head->identity == &et);
    CHECK(alloc_calls == alloc_before + 1u);
    CHECK(free_calls == free_before + 1u && live_records == 1u);
    CHECK(gate_depth == 0u && fatal_calls == 0u);

    et.et_UniqueID = reserved_pid;
    watch_unchanged(&et, base.ex_TaskID);
    CHECK(krnP4ETaskIdentityRelease(&et, &base) == 1);
    CHECK(identities.head == NULL && identities.count == 0u);
    CHECK(free_calls == 2u && live_records == 0u);
}

static struct p4_etask_identity_record full_records[P4_ETASK_IDENTITY_LIMIT];
static unsigned char full_identities[P4_ETASK_IDENTITY_LIMIT];

static void install_full_registry(void)
{
    uint32_t index;
    for (index = 0; index < P4_ETASK_IDENTITY_LIMIT; ++index)
    {
        full_records[index].identity = &full_identities[index];
        full_records[index].pid = index + 1u;
        full_records[index].next =
            index + 1u < P4_ETASK_IDENTITY_LIMIT
                ? &full_records[index + 1u] : NULL;
    }
    identities.head = &full_records[0];
    identities.count = P4_ETASK_IDENTITY_LIMIT;
}

static void test_full_registry_refusal_frees_record(void)
{
    struct ExecBase base = { 900u };
    struct ETask fresh = { 0u };
    unsigned free_before;

    reset_observers(&base);
    reset_private_registry();
    install_full_registry();
    watch_unchanged(&fresh, base.ex_TaskID);
    free_before = free_calls;
    CHECK(!krnP4ETaskIdentityReserve(&fresh, &base));
    CHECK(fresh.et_UniqueID == 0u && base.ex_TaskID == 900u);
    CHECK(identities.head == &full_records[0]);
    CHECK(identities.count == P4_ETASK_IDENTITY_LIMIT);
    CHECK(alloc_calls == 1u && free_calls == free_before + 1u);
    CHECK(live_records == 0u && gate_depth == 0u && fatal_calls == 0u);

    /* This fixture seeded static records directly to reach capacity; discard
     * that synthetic state under the same modeled caller gate. */
    (void)krnP4ETaskGateLock(&base);
    identities.head = NULL;
    identities.count = 0;
    krnP4ETaskGateUnlock(0);
    CHECK(identities.head == NULL && identities.count == 0u);
}

static void test_wrap_and_release_missing_fatal(void)
{
    struct ExecBase base = { UINT32_MAX };
    struct ETask a = { 0u };
    struct ETask b = { 0u };
    struct ETask absent = { 0u };
    unsigned before_fatal;

    reset_observers(&base);
    reset_private_registry();
    watch_reserve(&a, 1024u, 1024u);
    CHECK(krnP4ETaskIdentityReserve(&a, &base));
    CHECK(a.et_UniqueID == 1024u && base.ex_TaskID == 1024u);
    watch_reserve(&b, 1025u, 1025u);
    CHECK(krnP4ETaskIdentityReserve(&b, &base));
    CHECK(b.et_UniqueID == 1025u && base.ex_TaskID == 1025u);

    before_fatal = fatal_calls;
    watch_unchanged(NULL, base.ex_TaskID);
    CHECK(krnP4ETaskIdentityRelease(&absent, &base) == 0);
    CHECK(fatal_calls == before_fatal + 1u);
    CHECK(identities.count == 2u && gate_depth == 0u);
    CHECK(live_records == 2u && base.ex_TaskID == 1025u);

    watch_unchanged(&a, base.ex_TaskID);
    CHECK(krnP4ETaskIdentityRelease(&a, &base) == 1);
    watch_unchanged(&b, base.ex_TaskID);
    CHECK(krnP4ETaskIdentityRelease(&b, &base) == 1);
    CHECK(identities.head == NULL && identities.count == 0u);
    CHECK(live_records == 0u && fatal_calls == before_fatal + 1u);
}

enum registry_corruption
{
    CORRUPT_COUNT_OVER_LIMIT,
    CORRUPT_TAIL_TOO_LONG,
    CORRUPT_TAIL_TOO_SHORT,
    CORRUPT_CYCLE,
    CORRUPT_DUPLICATE_TARGET,
    CORRUPT_ZERO_PID,
    CORRUPT_OUT_OF_ORDER_PID,
    REGISTRY_CORRUPTION_COUNT
};

enum owner_operation
{
    OWNER_OP_BEGIN_CLEANUP,
    OWNER_OP_RETAIN_RESULT,
    OWNER_OP_RELEASE,
    OWNER_OP_READ_PIN,
    OWNER_OP_READ_UNPIN,
    OWNER_OPERATION_COUNT
};

static const char *const corruption_names[REGISTRY_CORRUPTION_COUNT] =
{
    "count-over-limit",
    "tail-too-long",
    "tail-too-short",
    "cycle",
    "duplicate-target-identity",
    "zero-pid",
    "out-of-order-pid"
};

static const char *const owner_operation_names[OWNER_OPERATION_COUNT] =
{
    "begin-cleanup",
    "retain-result",
    "release",
    "read-pin",
    "read-unpin"
};

static void apply_registry_corruption(
    enum registry_corruption corruption,
    struct p4_etask_owner_record *first,
    struct p4_etask_owner_record *second,
    struct ETask *target)
{
    switch (corruption)
    {
        case CORRUPT_COUNT_OVER_LIMIT:
            identities.count = P4_ETASK_IDENTITY_LIMIT + 1u;
            break;
        case CORRUPT_TAIL_TOO_LONG:
            identities.count = 1u;
            break;
        case CORRUPT_TAIL_TOO_SHORT:
            identities.count = 3u;
            break;
        case CORRUPT_CYCLE:
            second->identity.next = &first->identity;
            break;
        case CORRUPT_DUPLICATE_TARGET:
            second->identity.identity = target;
            break;
        case CORRUPT_ZERO_PID:
            first->identity.pid = 0u;
            break;
        case CORRUPT_OUT_OF_ORDER_PID:
            second->identity.pid = first->identity.pid - 1u;
            break;
        case REGISTRY_CORRUPTION_COUNT:
            CHECK(0 && "invalid corruption selector");
            break;
    }
}

static void exercise_corrupt_registry_refusal(
    enum registry_corruption corruption,
    enum owner_operation operation)
{
    struct ExecBase base = { 1200u };
    struct ETask first_task = { 0u };
    struct ETask second_task = { 0u };
    struct p4_etask_owner_record *first;
    struct p4_etask_owner_record *second;
    struct p4_etask_identity_registry registry_valid;
    struct p4_etask_owner_record first_valid;
    struct p4_etask_owner_record second_valid;
    struct p4_etask_identity_registry registry_before;
    struct p4_etask_owner_record first_before;
    struct p4_etask_owner_record second_before;
    uint32_t counter_before;
    uint32_t first_pid_before;
    uint32_t second_pid_before;
    unsigned frees_before;
    unsigned records_before;
    unsigned fatal_before;

    reset_observers(&base);
    reset_private_registry();
    CHECK(krnP4ETaskIdentityReserve(&first_task, &base));
    CHECK(krnP4ETaskIdentityReserve(&second_task, &base));
    first = (struct p4_etask_owner_record *)identities.head;
    CHECK(first != NULL && first->identity.identity == &first_task);
    second = (struct p4_etask_owner_record *)first->identity.next;
    CHECK(second != NULL && second->identity.identity == &second_task);
    CHECK(second->identity.next == NULL && identities.count == 2u);

    if (operation == OWNER_OP_RETAIN_RESULT)
        krnP4ETaskMetadataBeginCleanup(&first_task, &base);
    if (operation == OWNER_OP_READ_UNPIN)
        CHECK(krnP4ETaskMetadataReadPin(&first_task, &base));

    registry_valid = identities;
    first_valid = *first;
    second_valid = *second;
    apply_registry_corruption(corruption, first, second, &first_task);
    registry_before = identities;
    first_before = *first;
    second_before = *second;
    counter_before = base.ex_TaskID;
    first_pid_before = first_task.et_UniqueID;
    second_pid_before = second_task.et_UniqueID;
    frees_before = free_calls;
    records_before = live_records;
    fatal_before = fatal_calls;

    switch (operation)
    {
        case OWNER_OP_BEGIN_CLEANUP:
            krnP4ETaskMetadataBeginCleanup(&first_task, &base);
            break;
        case OWNER_OP_RETAIN_RESULT:
            krnP4ETaskMetadataRetainResult(&first_task, &base);
            break;
        case OWNER_OP_RELEASE:
            CHECK(krnP4ETaskIdentityRelease(&first_task, &base) == 0);
            break;
        case OWNER_OP_READ_PIN:
            CHECK(!krnP4ETaskMetadataReadPin(&first_task, &base));
            break;
        case OWNER_OP_READ_UNPIN:
            krnP4ETaskMetadataReadUnpin(&first_task, &base);
            break;
        case OWNER_OPERATION_COUNT:
            CHECK(0 && "invalid owner operation selector");
            break;
    }

    if (operation == OWNER_OP_READ_PIN)
        CHECK(fatal_calls == fatal_before);
    else
        CHECK(fatal_calls == fatal_before + 1u);
    CHECK(memcmp(&identities, &registry_before, sizeof(identities)) == 0);
    CHECK(memcmp(first, &first_before, sizeof(*first)) == 0);
    CHECK(memcmp(second, &second_before, sizeof(*second)) == 0);
    CHECK(base.ex_TaskID == counter_before);
    CHECK(first_task.et_UniqueID == first_pid_before);
    CHECK(second_task.et_UniqueID == second_pid_before);
    CHECK(free_calls == frees_before && live_records == records_before);
    CHECK(gate_depth == 0u && identities.count != 0u);

    /* Restore the exact valid snapshot only after checking that the refused
     * operation left the corrupted registry and owner bytes untouched. */
    identities = registry_valid;
    *first = first_valid;
    *second = second_valid;
    if (operation == OWNER_OP_READ_UNPIN)
        krnP4ETaskMetadataReadUnpin(&first_task, &base);
    CHECK(krnP4ETaskIdentityRelease(&first_task, &base) == 1);
    CHECK(krnP4ETaskIdentityRelease(&second_task, &base) == 1);
    CHECK(identities.head == NULL && identities.count == 0u);
    CHECK(live_records == 0u && free_calls == 2u);
}

static void test_invalid_registry_refuses_owner_operations_unchanged(void)
{
    enum registry_corruption corruption;
    enum owner_operation operation;

    for (corruption = CORRUPT_COUNT_OVER_LIMIT;
         corruption < REGISTRY_CORRUPTION_COUNT;
         corruption = (enum registry_corruption)(corruption + 1))
    {
        for (operation = OWNER_OP_BEGIN_CLEANUP;
             operation < OWNER_OPERATION_COUNT;
             operation = (enum owner_operation)(operation + 1))
        {
            exercise_corrupt_registry_refusal(corruption, operation);
            printf("  checked %s via %s\n",
                   corruption_names[corruption],
                   owner_operation_names[operation]);
        }
    }
}

int main(void)
{
    struct ExecBase initial_base = { 0u };

    reset_observers(&initial_base);
    reset_private_registry();
    test_refusals_leave_shared_state_unchanged();
    test_reserve_release_and_task_absence();
    test_duplicate_cleanup_and_result_retain_refuse();
    test_multiple_readers_defer_task_owner_release();
    test_retained_result_readers_defer_final_storage_release();
    test_outstanding_child_edge_refuses_reclamation();
    test_interleaved_reservations_share_counter();
    test_duplicate_identity_refusal_frees_temporary_record();
    test_full_registry_refusal_frees_record();
    test_wrap_and_release_missing_fatal();
    test_invalid_registry_refuses_owner_operations_unchanged();
    CHECK(gate_depth == 0u);
    CHECK(identities.head == NULL && identities.count == 0u);
    CHECK(live_records == 0u);
    printf("PASS ETask identity binding: %u checks\n", checks);
    return 0;
}
