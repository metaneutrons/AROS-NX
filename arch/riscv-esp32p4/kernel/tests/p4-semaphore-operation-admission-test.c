/* Actual-source test for fused Task-scope and semaphore-ledger admission. */
#define _DEFAULT_SOURCE 1
#define __AROSEXEC_SMP__ 1
#define __AROSPLATFORM_SMP__ 1
#define P4_TASK_PINS_TEST 1
#define P4_PUBLIC_SPINLOCK_TEST 1
#define P4_SEMAPHORE_NODES_TEST 1

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "../p4_public_spinlock.h"
#include "../p4_task_pins.h"
#include "../p4_semaphore_boot.h"

struct ExecBase
{
    uint32_t unused;
};

typedef int16_t WORD;
struct Task { uintptr_t opaque; };
struct MinNode { struct MinNode *mln_Succ; struct MinNode *mln_Pred; };
struct MinList
{
    struct MinNode *mlh_Head;
    struct MinNode *mlh_Tail;
    struct MinNode *mlh_TailPred;
};
struct Node { struct Node *ln_Succ; struct Node *ln_Pred; };
struct SemaphoreRequest
{
    struct MinNode sr_Link;
    struct Task *sr_Waiter;
};
struct SignalSemaphore
{
    struct Node ss_Link;
    WORD ss_NestCount;
    struct MinList ss_WaitQueue;
    struct SemaphoreRequest ss_MultipleLink;
    struct Task *ss_Owner;
    WORD ss_QueueCount;
};

void Disable(void);
void Enable(void);
void krnP4CleanupPoke(struct ExecBase *);
void krnP4RetiredOwnerPoke(uint32_t, struct ExecBase *);

static struct p4_semaphore_ledger *test_boot_ledger;
static struct SemaphoreRequest test_boot_requests[8];

struct p4_semaphore_ledger *krnP4SemaphoreBootLedger(void)
{
    return test_boot_ledger;
}

struct SemaphoreRequest *krnP4SemaphoreBootRequest(uint32_t index)
{
    return index < 8u ? &test_boot_requests[index] : NULL;
}

#include "../p4_task_lifetime.c"
#include "../p4_semaphore_ledger.c"
#include "../p4_task_registry.c"
#include "../p4_task_pins.c"
#include "../p4_semaphore_nodes.c"
#include "../p4_semaphore_cancel.c"
#include "../p4_semaphore_cancel_retry.c"

#define TEST_OP_CAPACITY 4u
#define TEST_ENTRY_CAPACITY 8u
#define TEST_RECORD_CAPACITY 4u

static unsigned int checks;
static unsigned int failures;
static unsigned int mock_irq_depth;
static unsigned int mock_disable_calls;
static unsigned int mock_enable_calls;
static unsigned int mock_acquire_calls;
static unsigned int mock_unlock_calls;
static unsigned int mock_fatal_calls;
static unsigned int mock_poke_calls;
static unsigned int mock_retired_poke_calls;
static int mock_gate_held;
static int mock_outer_semaphore_gate_held;
static int mock_task_lock_held;
static spinlock_t *mock_gate_address;

#define CHECK(condition) do { \
    ++checks; \
    if (!(condition)) { \
        ++failures; \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                #condition); \
    } \
} while (0)

void Disable(void)
{
    ++mock_disable_calls;
    ++mock_irq_depth;
}

void Enable(void)
{
    ++mock_enable_calls;
    if (mock_gate_held || mock_irq_depth == 0u)
    {
        ++failures;
        fprintf(stderr, "FAIL Enable outside unlocked/disabled state\n");
        return;
    }
    --mock_irq_depth;
}

enum p4_public_spinlock_result krnP4PublicSpinInit(spinlock_t *lock)
{
    if (lock == NULL)
        return P4_PUBLIC_SPIN_BAD_ARGUMENT;
    lock->lock = 0;
    lock->s_Owner = 0;
    return P4_PUBLIC_SPIN_OK;
}

enum p4_public_spinlock_result krnP4PublicSpinTryAcquire(
    spinlock_t *lock, uint32_t mode)
{
    ++mock_acquire_calls;
    if (mock_irq_depth == 0u || lock == NULL ||
        mode != SPINLOCK_MODE_WRITE || mock_gate_held)
        return P4_PUBLIC_SPIN_BAD_STATE;
    if (mock_gate_address == NULL)
        mock_gate_address = lock;
    if (mock_gate_address != lock)
        return P4_PUBLIC_SPIN_BAD_ADDRESS;
    lock->lock = 1;
    mock_gate_held = 1;
    return P4_PUBLIC_SPIN_OK;
}

enum p4_public_spinlock_result krnP4PublicSpinUnlock(spinlock_t *lock)
{
    ++mock_unlock_calls;
    if (!mock_gate_held || lock == NULL || lock != mock_gate_address)
        return P4_PUBLIC_SPIN_BAD_STATE;
    lock->lock = 0;
    mock_gate_held = 0;
    return P4_PUBLIC_SPIN_OK;
}

void krnP4PublicSpinFatal(enum p4_public_spinlock_result reason)
{
    ++mock_fatal_calls;
    fprintf(stderr, "unexpected fatal spin result: %d\n", (int)reason);
    abort();
}

void krnP4CleanupPoke(struct ExecBase *SysBase)
{
    (void)SysBase;
    ++mock_poke_calls;
    if (mock_outer_semaphore_gate_held || mock_gate_held)
    {
        ++failures;
        fprintf(stderr, "FAIL cleanup poke while a gate is held\n");
    }
}

void krnP4RetiredOwnerPoke(uint32_t hart, struct ExecBase *SysBase)
{
    (void)hart;
    (void)SysBase;
    ++mock_retired_poke_calls;
    if (!mock_task_lock_held || mock_gate_held)
    {
        ++failures;
        fprintf(stderr, "FAIL retired-owner poke outside expected lock state\n");
    }
}

struct direct_fixture
{
    struct p4_task_registry registry;
    struct p4_task_registry_record records[TEST_RECORD_CAPACITY];
    struct p4_semaphore_ledger ledger;
    struct p4_semaphore_ledger_operation operations[TEST_OP_CAPACITY];
    struct p4_semaphore_ledger_entry entries[TEST_ENTRY_CAPACITY];
};

struct byte_snapshot
{
    unsigned char bytes[sizeof(struct direct_fixture)];
};

static void fixture_init(struct direct_fixture *fixture,
                         uint32_t operation_capacity,
                         uint32_t entry_capacity)
{
    int result;
    memset(fixture, 0, sizeof(*fixture));
    p4_task_registry_init(&fixture->registry);
    result = p4_semaphore_ledger_init(&fixture->ledger,
        fixture->operations, operation_capacity, fixture->entries,
        entry_capacity);
    CHECK(result == 1);
}

static struct p4_task_registry_record *fixture_add_task(
    struct direct_fixture *fixture, unsigned int slot, struct Task *identity,
    uint32_t hart)
{
    struct p4_task_registry_record *record;
    int result;

    if (slot >= TEST_RECORD_CAPACITY)
        return NULL;
    record = &fixture->records[slot];
    result = p4_task_registry_register_locked(&fixture->registry, record,
                                               identity);
    CHECK(result == 1);
    result = p4_task_registry_enter_running_locked(&fixture->registry,
                                                    identity, hart);
    CHECK(result == 1);
    return record;
}

static void snapshot_fixture(const struct direct_fixture *fixture,
                             struct byte_snapshot *snapshot)
{
    memcpy(snapshot->bytes, fixture, sizeof(*fixture));
}

static int fixture_matches_snapshot(const struct direct_fixture *fixture,
                                    const struct byte_snapshot *snapshot)
{
    return memcmp(fixture, snapshot->bytes, sizeof(*fixture)) == 0;
}

static struct Task *guarded_identity(void *page, size_t offset)
{
    return (struct Task *)((unsigned char *)page + offset);
}

static int reserve_direct(struct direct_fixture *fixture,
                          struct Task *identity, uint32_t count,
                          struct p4_task_wait_operation *output)
{
    return p4_task_registry_wait_reserve_operation_locked(
        &fixture->registry, &fixture->ledger, identity, count, output);
}

static void test_success_and_legacy_guards(void *page)
{
    struct direct_fixture fixture;
    struct p4_task_wait_operation first = { 0, 0 };
    struct p4_task_wait_operation second = { 0, 0 };
    struct p4_task_wait_operation duplicate = {
        (struct p4_semaphore_ledger_operation *)(uintptr_t)0x1234u, 0x5a5a5a5au
    };
    struct p4_task_wait_cancel_snapshot cancel;
    struct p4_task_wait_cancel_snapshot invalid_cancel;
    struct p4_task_wait_cancel_snapshot invalid_cancel_before;
    struct p4_task_registry_record *record0;
    struct p4_task_registry_record *record1;
    struct p4_task_registry_record *record2;
    struct p4_semaphore_ledger_entry *entry;
    struct byte_snapshot before;
    struct p4_semaphore_ledger_operation *saved_operations;
    struct p4_semaphore_ledger_entry *saved_entries;
    uintptr_t alias_address;
    uint32_t legacy_generation = UINT32_C(0xabcdef01);
    struct Task *task0 = guarded_identity(page, 64u);
    struct Task *task1 = guarded_identity(page, 128u);
    struct Task *task2 = guarded_identity(page, 160u);
    uint32_t i;

    fixture_init(&fixture, TEST_OP_CAPACITY, TEST_ENTRY_CAPACITY);
    record0 = fixture_add_task(&fixture, 0u, task0, 0u);
    record1 = fixture_add_task(&fixture, 1u, task1, 1u);
    record2 = fixture_add_task(&fixture, 2u, task2, 0u);

    CHECK(reserve_direct(&fixture, task0, 2u, &first) == 1);
    CHECK(first.operation != NULL && first.generation == 1u);
    CHECK(record0->lifetime.notification_pins == 1u);
    CHECK(record0->wait_scope.active == 1u);
    CHECK(record0->wait_scope.kind == P4_TASK_WAIT_OPERATION);
    CHECK(record0->wait_scope.generation == first.generation);
    CHECK(record0->wait_scope.list_index == first.operation);
    CHECK(record0->wait_scope.semaphore_index == NULL);
    CHECK(record0->wait_scope.request_index == NULL);
    CHECK(first.operation->owner == task0);
    CHECK(first.operation->generation == first.generation);
    CHECK(first.operation->active == 1u);
    CHECK(first.operation->entry_count == 2u);
    CHECK(first.operation->construction_closed == 0u);
    CHECK(fixture.ledger.active_operations == 1u);
    CHECK(fixture.ledger.active_entries == 2u);
    entry = first.operation->first;
    for (i = 0; i < 2u; ++i)
    {
        CHECK(entry != NULL);
        CHECK(entry->operation == first.operation);
        CHECK(entry->state == P4_SEMAPHORE_LEDGER_ENTRY_UNVISITED);
        CHECK(entry->semaphore == NULL && entry->request == NULL);
        entry = entry->operation_next;
    }
    CHECK(entry == NULL);

    /* No queue/external mutation occurs in admission; all entries are already
     * reserved and all caller-visible output is published at return. */
    CHECK(reserve_direct(&fixture, task1, 1u, &second) == 1);
    CHECK(second.operation != NULL && second.generation == 1u);
    CHECK(record1->lifetime.notification_pins == 1u);
    CHECK(record1->wait_scope.kind == P4_TASK_WAIT_OPERATION);
    CHECK(record1->wait_scope.list_index == second.operation);
    CHECK(second.operation->owner == task1);
    CHECK(second.operation->entry_count == 1u);
    CHECK(fixture.ledger.active_operations == 2u);
    CHECK(fixture.ledger.active_entries == 3u);

    /* The operation kind passes the expanded validator but cannot be created
     * through the legacy SINGLE/LIST API. */
    CHECK(p4_task_registry_wait_cancel_ready_locked(
              &fixture.registry, task0) == 0);
    CHECK(p4_task_registry_wait_begin_locked(&fixture.registry, task2,
              P4_TASK_WAIT_OPERATION, NULL, NULL, first.operation,
              &legacy_generation) == 0);
    CHECK(legacy_generation == 0u);
    CHECK(record2->wait_scope.active == 0u);
    CHECK(record2->lifetime.notification_pins == 0u);
    CHECK(record1->wait_scope.kind == P4_TASK_WAIT_OPERATION);
    CHECK(record1->wait_scope.list_index == second.operation);

    snapshot_fixture(&fixture, &before);
    CHECK(p4_task_registry_wait_finish_locked(&fixture.registry, task0,
                                               first.generation) == 0);
    CHECK(fixture_matches_snapshot(&fixture, &before));

    /* Legacy retirement cannot bypass the operation ledger. The fused path
     * publishes ledger cancellation and Task retirement as one transition. */
    snapshot_fixture(&fixture, &before);
    CHECK(p4_task_registry_retire_locked(&fixture.registry, task0) == 0);
    CHECK(fixture_matches_snapshot(&fixture, &before));
    CHECK(p4_task_registry_retire_for_service_locked(&fixture.registry,
              task0) == 0);
    CHECK(fixture_matches_snapshot(&fixture, &before));
    CHECK(p4_task_registry_retire_with_ledger_locked(&fixture.registry,
              &fixture.ledger, task0, 0u) == 1);
    CHECK(record0->wait_scope.cancel_requested == 1u);
    CHECK(first.operation->cancelled == 1u);
    CHECK(p4_task_registry_leave_running_locked(&fixture.registry, task0,
                                                 0u) == 1);

    /* Snapshot output must not alias registry metadata or either ledger
     * array. These checks happen before the cancellation claim mutates scope
     * state or writes the caller snapshot. */
    snapshot_fixture(&fixture, &before);
    CHECK(p4_task_registry_claim_next_wait_cancel_with_ledger_locked(
              &fixture.registry, &fixture.ledger,
              (struct p4_task_wait_cancel_snapshot *)(void *)record0) == -1);
    CHECK(fixture_matches_snapshot(&fixture, &before));
    alias_address = (uintptr_t)record0 + sizeof(void *);
    CHECK(alias_address % _Alignof(struct p4_task_wait_cancel_snapshot) == 0u);
    CHECK(p4_task_registry_claim_next_wait_cancel_with_ledger_locked(
              &fixture.registry, &fixture.ledger,
              (struct p4_task_wait_cancel_snapshot *)alias_address) == -1);
    CHECK(fixture_matches_snapshot(&fixture, &before));
    CHECK(p4_task_registry_claim_next_wait_cancel_with_ledger_locked(
              &fixture.registry, &fixture.ledger,
              (struct p4_task_wait_cancel_snapshot *)(void *)&fixture.operations[0]) == -1);
    CHECK(fixture_matches_snapshot(&fixture, &before));
    CHECK(p4_task_registry_claim_next_wait_cancel_with_ledger_locked(
              &fixture.registry, &fixture.ledger,
              (struct p4_task_wait_cancel_snapshot *)(void *)&fixture.entries[0]) == -1);
    CHECK(fixture_matches_snapshot(&fixture, &before));

    /* Malformed arena array pointers into registry storage are rejected
     * before full ledger validation can dereference those typed arrays. */
    saved_operations = fixture.ledger.operations;
    fixture.ledger.operations =
        (struct p4_semaphore_ledger_operation *)(void *)record0;
    snapshot_fixture(&fixture, &before);
    memset(&invalid_cancel, 0x5a, sizeof(invalid_cancel));
    invalid_cancel_before = invalid_cancel;
    CHECK(p4_task_registry_claim_next_wait_cancel_with_ledger_locked(
              &fixture.registry, &fixture.ledger, &invalid_cancel) == -1);
    CHECK(fixture_matches_snapshot(&fixture, &before));
    CHECK(memcmp(&invalid_cancel, &invalid_cancel_before,
                 sizeof(invalid_cancel)) == 0);
    fixture.ledger.operations = saved_operations;

    fixture.ledger.operations =
        (struct p4_semaphore_ledger_operation *)(void *)&fixture.registry;
    snapshot_fixture(&fixture, &before);
    CHECK(p4_task_registry_claim_next_wait_cancel_with_ledger_locked(
              &fixture.registry, &fixture.ledger, &invalid_cancel) == -1);
    CHECK(fixture_matches_snapshot(&fixture, &before));
    fixture.ledger.operations = saved_operations;

    saved_entries = fixture.ledger.entries;
    fixture.ledger.entries =
        (struct p4_semaphore_ledger_entry *)(void *)record0;
    snapshot_fixture(&fixture, &before);
    CHECK(p4_task_registry_claim_next_wait_cancel_with_ledger_locked(
              &fixture.registry, &fixture.ledger, &invalid_cancel) == -1);
    CHECK(fixture_matches_snapshot(&fixture, &before));
    fixture.ledger.entries = saved_entries;

    memset(&cancel, 0xa5, sizeof(cancel));
    CHECK(p4_task_registry_claim_next_wait_cancel_with_ledger_locked(
              &fixture.registry, &fixture.ledger, &cancel) == 1);
    CHECK(cancel.identity == task0);
    CHECK(cancel.generation == first.generation);
    CHECK(cancel.kind == P4_TASK_WAIT_OPERATION);
    CHECK(cancel.list_index == first.operation);
    CHECK(cancel.semaphore_index == NULL && cancel.request_index == NULL);
    snapshot_fixture(&fixture, &before);
    CHECK(p4_task_registry_wait_finish_cancel_locked(&fixture.registry,
              task0, first.generation) == 0);
    CHECK(fixture_matches_snapshot(&fixture, &before));

    /* A live operation's second reserve is refused atomically. */
    memset(&before, 0, sizeof(before));
    snapshot_fixture(&fixture, &before);
    CHECK(reserve_direct(&fixture, task1, 1u, &duplicate) == 0);
    CHECK(fixture_matches_snapshot(&fixture, &before));
    CHECK(duplicate.operation ==
          (struct p4_semaphore_ledger_operation *)(uintptr_t)0x1234u);
    CHECK(duplicate.generation == 0x5a5a5a5au);
}

static void test_legacy_mutators_reject_operation(void *page)
{
    struct direct_fixture fixture;
    struct p4_task_wait_operation operation = { 0, 0 };
    struct p4_task_registry_record *record;
    struct byte_snapshot before;
    struct Task *task = guarded_identity(page, 224u);
    uint32_t generation;

    fixture_init(&fixture, TEST_OP_CAPACITY, TEST_ENTRY_CAPACITY);
    record = fixture_add_task(&fixture, 0u, task, 0u);
    CHECK(reserve_direct(&fixture, task, 1u, &operation) == 1);
    generation = operation.generation;

    snapshot_fixture(&fixture, &before);
    CHECK(p4_task_registry_wait_add_request_locked(&fixture.registry,
              task, generation) == 0);
    CHECK(fixture_matches_snapshot(&fixture, &before));
    CHECK(p4_task_registry_wait_close_locked(&fixture.registry,
              task, generation) == 0);
    CHECK(fixture_matches_snapshot(&fixture, &before));

    /* Make every non-kind readiness condition true. A zero queued count must
     * not make a boot-arena operation look like a completed LIST wait. */
    record->wait_scope.construction_closed = 1u;
    record->wait_scope.queued_count = 0u;
    snapshot_fixture(&fixture, &before);
    CHECK(p4_task_registry_wait_ready_locked(&fixture.registry,
              task, generation) == 0);
    CHECK(fixture_matches_snapshot(&fixture, &before));
    record->wait_scope.construction_closed = 0u;

    /* Both grant and withdrawal receive otherwise eligible queue/cancel
     * fields; neither may reinterpret the opaque operation index. */
    record->wait_scope.queued_count = 1u;
    snapshot_fixture(&fixture, &before);
    CHECK(p4_task_registry_wait_grant_request_locked(&fixture.registry,
              task, generation) == 0);
    CHECK(fixture_matches_snapshot(&fixture, &before));
    record->lifetime.flags = P4_TASK_LIFETIME_RETIRED;
    record->wait_scope.cancel_requested = 1u;
    snapshot_fixture(&fixture, &before);
    CHECK(p4_task_registry_wait_withdraw_request_locked(&fixture.registry,
              task, generation) == 0);
    CHECK(fixture_matches_snapshot(&fixture, &before));

    /* Legacy completion entrypoints must retain the operation's designated
     * pin until a fused ledger/scope completion exists. */
    record->lifetime.flags = 0u;
    record->wait_scope.cancel_requested = 0u;
    record->wait_scope.queued_count = 0u;
    record->wait_scope.construction_closed = 1u;
    snapshot_fixture(&fixture, &before);
    CHECK(p4_task_registry_wait_finish_locked(&fixture.registry,
              task, generation) == 0);
    CHECK(fixture_matches_snapshot(&fixture, &before));

    record->lifetime.flags = P4_TASK_LIFETIME_RETIRED;
    record->lifetime.running_harts = 0u;
    record->wait_scope.cancel_requested = 1u;
    record->wait_scope.cancel_claimed = 1u;
    snapshot_fixture(&fixture, &before);
    CHECK(p4_task_registry_wait_finish_cancel_locked(&fixture.registry,
              task, generation) == 0);
    CHECK(fixture_matches_snapshot(&fixture, &before));
}

static void expect_refusal_unchanged(const char *name,
                                     struct direct_fixture *fixture,
                                     struct Task *identity, uint32_t count)
{
    struct p4_task_wait_operation output = {
        (struct p4_semaphore_ledger_operation *)(uintptr_t)0x4321u,
        UINT32_C(0x87654321)
    };
    struct p4_task_wait_operation old_output = output;
    struct byte_snapshot before;
    int result;

    snapshot_fixture(fixture, &before);
    result = reserve_direct(fixture, identity, count, &output);
    CHECK(result == 0);
    CHECK(fixture_matches_snapshot(fixture, &before));
    CHECK(memcmp(&output, &old_output, sizeof(output)) == 0);
    (void)name;
}

static void test_refusals_and_capacity(void *page)
{
    struct direct_fixture fixture;
    struct p4_task_wait_operation first = { 0, 0 };
    struct Task *task0 = guarded_identity(page, 192u);
    struct Task *task1 = guarded_identity(page, 256u);
    struct Task *missing = guarded_identity(page, 320u);
    struct p4_task_registry_record *record;

    fixture_init(&fixture, TEST_OP_CAPACITY, TEST_ENTRY_CAPACITY);
    record = fixture_add_task(&fixture, 0u, task0, 0u);
    expect_refusal_unchanged("missing", &fixture, missing, 1u);

    /* A registered identity without a Running lease is not an admissible
     * current task. */
    CHECK(p4_task_registry_register_locked(&fixture.registry,
              &fixture.records[1], task1) == 1);
    expect_refusal_unchanged("no RUN", &fixture, task1, 1u);

    CHECK(p4_task_registry_retire_locked(&fixture.registry, task0) == 1);
    expect_refusal_unchanged("retired", &fixture, task0, 1u);

    fixture_init(&fixture, TEST_OP_CAPACITY, TEST_ENTRY_CAPACITY);
    record = fixture_add_task(&fixture, 0u, task0, 0u);
    record->wait_scope.generation = UINT32_MAX;
    expect_refusal_unchanged("generation exhausted", &fixture, task0, 1u);

    fixture_init(&fixture, TEST_OP_CAPACITY, TEST_ENTRY_CAPACITY);
    record = fixture_add_task(&fixture, 0u, task0, 0u);
    record->lifetime.notification_pins = UINT32_MAX;
    expect_refusal_unchanged("pin exhausted", &fixture, task0, 1u);

    /* Entry capacity refusal after valid admission checks changes neither
     * sidecar nor ledger arrays nor caller output. */
    fixture_init(&fixture, TEST_OP_CAPACITY, 1u);
    (void)fixture_add_task(&fixture, 0u, task0, 0u);
    expect_refusal_unchanged("entry capacity", &fixture, task0, 2u);

    /* Fill operation slots, then request one more with a distinct live RUN
     * task. The full arena is unchanged on refusal. */
    fixture_init(&fixture, 1u, TEST_ENTRY_CAPACITY);
    (void)fixture_add_task(&fixture, 0u, task0, 0u);
    CHECK(reserve_direct(&fixture, task0, 1u, &first) == 1);
    CHECK(p4_task_registry_register_locked(&fixture.registry,
              &fixture.records[1], task1) == 1);
    CHECK(p4_task_registry_enter_running_locked(&fixture.registry,
              task1, 1u) == 1);
    expect_refusal_unchanged("operation capacity", &fixture, task1, 1u);

    /* The same owner/generation already present in the ledger is rejected
     * without publishing a registry pin/scope. */
    fixture_init(&fixture, TEST_OP_CAPACITY, TEST_ENTRY_CAPACITY);
    (void)fixture_add_task(&fixture, 0u, task0, 1u);
    CHECK(p4_semaphore_ledger_reserve(&fixture.ledger, task0, 1u, 1u,
              &first.operation) == 1);
    first.generation = 0x2468u;
    expect_refusal_unchanged("duplicate owner/generation", &fixture,
                             task0, 1u);
}

static void test_alias_refusals(void *page)
{
    struct direct_fixture fixture;
    struct Task *task = guarded_identity(page, 384u);
    struct p4_task_registry_record *record;
    struct p4_task_wait_operation output = { 0, 0 };
    struct byte_snapshot before;
    uintptr_t address;
    int result;

    fixture_init(&fixture, TEST_OP_CAPACITY, TEST_ENTRY_CAPACITY);
    record = fixture_add_task(&fixture, 0u, task, 0u);

    /* Exact and partial output overlap with registry record storage. */
    snapshot_fixture(&fixture, &before);
    result = reserve_direct(&fixture, task, 1u,
        (struct p4_task_wait_operation *)(void *)record);
    CHECK(result == 0);
    CHECK(fixture_matches_snapshot(&fixture, &before));
    address = (uintptr_t)record + sizeof(void *);
    CHECK(address % _Alignof(struct p4_task_wait_operation) == 0u);
    result = reserve_direct(&fixture, task, 1u,
        (struct p4_task_wait_operation *)address);
    CHECK(result == 0);
    CHECK(fixture_matches_snapshot(&fixture, &before));

    /* Output overlapping the ledger object or its arrays is rejected before
     * either output or arena state can be changed. */
    result = reserve_direct(&fixture, task, 1u,
        (struct p4_task_wait_operation *)(void *)&fixture.ledger);
    CHECK(result == 0);
    CHECK(fixture_matches_snapshot(&fixture, &before));
    result = reserve_direct(&fixture, task, 1u,
        (struct p4_task_wait_operation *)(void *)&fixture.operations[0]);
    CHECK(result == 0);
    CHECK(fixture_matches_snapshot(&fixture, &before));
    result = reserve_direct(&fixture, task, 1u,
        (struct p4_task_wait_operation *)(void *)&fixture.operations[0].owner);
    CHECK(result == 0);
    CHECK(fixture_matches_snapshot(&fixture, &before));
    result = reserve_direct(&fixture, task, 1u,
        (struct p4_task_wait_operation *)(void *)&fixture.entries[0]);
    CHECK(result == 0);
    CHECK(fixture_matches_snapshot(&fixture, &before));
    result = reserve_direct(&fixture, task, 1u,
        (struct p4_task_wait_operation *)(void *)&fixture.entries[0].semaphore);
    CHECK(result == 0);
    CHECK(fixture_matches_snapshot(&fixture, &before));

    /* A raw ledger pointer exactly/partially inside registry storage is
     * rejected before typed ledger metadata is read. */
    result = p4_task_registry_wait_reserve_operation_locked(
        &fixture.registry, (struct p4_semaphore_ledger *)(void *)&fixture.registry,
        task, 1u, &output);
    CHECK(result == 0);
    CHECK(fixture_matches_snapshot(&fixture, &before));
    result = p4_task_registry_wait_reserve_operation_locked(
        &fixture.registry, (struct p4_semaphore_ledger *)(void *)record,
        task, 1u, &output);
    CHECK(result == 0);
    CHECK(fixture_matches_snapshot(&fixture, &before));
    address = (uintptr_t)record + sizeof(void *);
    CHECK(address % _Alignof(struct p4_semaphore_ledger) == 0u);
    result = p4_task_registry_wait_reserve_operation_locked(
        &fixture.registry, (struct p4_semaphore_ledger *)address,
        task, 1u, &output);
    CHECK(result == 0);
    CHECK(fixture_matches_snapshot(&fixture, &before));
    CHECK(output.operation == NULL && output.generation == 0u);

    /* Independently valid ledger metadata with operation/entry backing arrays
     * forged to overlap the registry object or a linked record fails before
     * p4_semaphore_ledger_reserve can inspect or mutate either object. */
    fixture.ledger.operations =
        (struct p4_semaphore_ledger_operation *)(void *)&fixture.registry;
    snapshot_fixture(&fixture, &before);
    result = reserve_direct(&fixture, task, 1u, &output);
    CHECK(result == 0);
    CHECK(fixture_matches_snapshot(&fixture, &before));

    fixture_init(&fixture, TEST_OP_CAPACITY, TEST_ENTRY_CAPACITY);
    record = fixture_add_task(&fixture, 0u, task, 0u);
    fixture.ledger.operations =
        (struct p4_semaphore_ledger_operation *)(void *)record;
    snapshot_fixture(&fixture, &before);
    result = reserve_direct(&fixture, task, 1u, &output);
    CHECK(result == 0);
    CHECK(fixture_matches_snapshot(&fixture, &before));

    fixture_init(&fixture, TEST_OP_CAPACITY, TEST_ENTRY_CAPACITY);
    record = fixture_add_task(&fixture, 0u, task, 0u);
    fixture.ledger.entries =
        (struct p4_semaphore_ledger_entry *)(void *)record;
    snapshot_fixture(&fixture, &before);
    result = reserve_direct(&fixture, task, 1u, &output);
    CHECK(result == 0);
    CHECK(fixture_matches_snapshot(&fixture, &before));
}

static void test_empty_semaphore(struct SignalSemaphore *sem)
{
    struct MinNode *head =
        (struct MinNode *)(void *)&sem->ss_WaitQueue.mlh_Head;
    struct MinNode *tail =
        (struct MinNode *)(void *)&sem->ss_WaitQueue.mlh_Tail;
    memset(sem, 0, sizeof(*sem));
    sem->ss_WaitQueue.mlh_Head = tail;
    sem->ss_WaitQueue.mlh_Tail = NULL;
    sem->ss_WaitQueue.mlh_TailPred = head;
    sem->ss_QueueCount = -1;
}

static void test_semaphore_gate_lock(void)
{
    CHECK(!mock_outer_semaphore_gate_held);
    Disable();
    mock_outer_semaphore_gate_held = 1;
}

static void test_semaphore_gate_unlock(void)
{
    CHECK(mock_outer_semaphore_gate_held);
    mock_outer_semaphore_gate_held = 0;
    Enable();
}

static void test_registry_gate_lock_under_sem(void)
{
    enum p4_public_spinlock_result result;
    CHECK(mock_outer_semaphore_gate_held);
    Disable();
    result = krnP4PublicSpinTryAcquire(&p4_task_pin_gate,
                                      SPINLOCK_MODE_WRITE);
    CHECK(result == P4_PUBLIC_SPIN_OK);
}

static void test_registry_gate_unlock_under_sem(void)
{
    CHECK(mock_outer_semaphore_gate_held);
    CHECK(krnP4PublicSpinUnlock(&p4_task_pin_gate) == P4_PUBLIC_SPIN_OK);
    Enable();
}

static void test_configure_operation(struct Task *task,
    struct p4_task_wait_operation *operation,
    struct SignalSemaphore *const semaphores[], const uint32_t shared[],
    uint32_t count, struct ExecBase *SysBase)
{
    uint32_t i;

    CHECK(operation->operation != NULL && operation->generation != 0u);
    test_semaphore_gate_lock();
    test_registry_gate_lock_under_sem();
    for (i = 0; i < count; ++i)
        CHECK(p4_semaphore_nodes_configure_locked(&p4_task_records,
                  test_boot_ledger, test_boot_requests, task,
                  operation->generation, i, semaphores[i], shared[i]) == 1);
    test_registry_gate_unlock_under_sem();
    CHECK(krnP4SemaphoreOperationClose(task, operation->generation,
              SysBase) == 1);
    test_semaphore_gate_unlock();
}

static void test_acquire_operation(struct Task *task,
    struct p4_task_wait_operation *operation,
    struct SignalSemaphore *const semaphores[], const int expected[],
    uint32_t count)
{
    uint32_t i;

    test_semaphore_gate_lock();
    test_registry_gate_lock_under_sem();
    for (i = 0; i < count; ++i)
        CHECK(p4_semaphore_nodes_acquire_locked(&p4_task_records,
                  test_boot_ledger, test_boot_requests, task,
                  operation->generation, i, semaphores[i]) == expected[i]);
    test_registry_gate_unlock_under_sem();
    test_semaphore_gate_unlock();
}

static int test_durable_release(struct SignalSemaphore *sem,
                                struct ExecBase *SysBase)
{
    int result;
    test_semaphore_gate_lock();
    result = krnP4SemaphoreReleaseDurable(sem, SysBase);
    CHECK(mock_outer_semaphore_gate_held && !mock_gate_held);
    test_semaphore_gate_unlock();
    return result;
}

static int test_durable_grant(struct SignalSemaphore *sem,
                              struct ExecBase *SysBase)
{
    int result;
    test_semaphore_gate_lock();
    result = krnP4SemaphoreGrantDurable(sem, SysBase);
    CHECK(mock_outer_semaphore_gate_held && !mock_gate_held);
    test_semaphore_gate_unlock();
    return result;
}

static void test_prepare_unreleased_handoff(struct SignalSemaphore *sem)
{
    uint32_t selected_pending = UINT32_MAX;

    /* Construct the valid post-release/pre-grant state with actual ledger
     * release and real queue links; the durable Grant below is production C. */
    test_semaphore_gate_lock();
    test_registry_gate_lock_under_sem();
    CHECK(sem->ss_NestCount == 1 && sem->ss_QueueCount == 1);
    CHECK(p4_semaphore_ledger_release(test_boot_ledger, sem,
              (uint32_t)sem->ss_NestCount, &selected_pending) == 1);
    CHECK(selected_pending == 0u);
    --sem->ss_NestCount;
    --sem->ss_QueueCount;
    sem->ss_Owner = NULL;
    test_registry_gate_unlock_under_sem();
    test_semaphore_gate_unlock();
}

static void test_actual_durable_release_grant_and_delivery(
    void *page, struct ExecBase *SysBase,
    struct Task *task0, struct Task *task1, struct Task *worker,
    struct p4_task_wait_operation *owner,
    struct p4_task_wait_operation *waiter,
    struct p4_task_registry_record *record1,
    struct p4_task_registry_record *worker_record,
    struct p4_task_registry_record *record3)
{
    static struct SignalSemaphore sem_a;
    static struct SignalSemaphore sem_b;
    struct p4_task_wait_operation third = { 0, 0 };
    struct p4_semaphore_notification snapshot;
    struct p4_semaphore_notification next_snapshot;
    struct p4_cleanup_wake wake;
    struct SignalSemaphore *owner_semaphores[2] = { &sem_a, &sem_b };
    struct SignalSemaphore *second_semaphores[1] = { &sem_a };
    struct SignalSemaphore *third_semaphores[1] = { &sem_b };
    const uint32_t shared_owner[2] = { 0u, 0u };
    const uint32_t shared_one[1] = { 0u };
    const int immediate[2] = { 1, 1 };
    const int queued[1] = { 2 };
    struct Task *task3 = guarded_identity(page, 640u);
    unsigned int poke_before;
    int result;

    test_empty_semaphore(&sem_a);
    test_empty_semaphore(&sem_b);
    memset(test_boot_requests, 0, sizeof(test_boot_requests));

    CHECK(krnP4ProtectCleanupWorker(worker, SysBase) == 1);
    CHECK(worker_record->worker_protected == 1u);
    CHECK(worker_record->lifetime.notification_pins == 1u);

    /* The first reserved operation is the holder for two actual semaphores. */
    test_configure_operation(task0, owner, owner_semaphores,
                             shared_owner, 2u, SysBase);
    test_acquire_operation(task0, owner, owner_semaphores, immediate, 2u);
    CHECK(sem_a.ss_NestCount == 1 && sem_a.ss_QueueCount == 0);
    CHECK(sem_a.ss_Owner == task0);
    CHECK(sem_b.ss_NestCount == 1 && sem_b.ss_QueueCount == 0);
    CHECK(sem_b.ss_Owner == task0);
    CHECK(krnP4SemaphoreOperationComplete(task0, owner->generation,
              SysBase) == 1);

    /* Reuse the second pre-reserved admission for a real queued waiter. */
    test_configure_operation(task1, waiter, second_semaphores,
                             shared_one, 1u, SysBase);
    test_acquire_operation(task1, waiter, second_semaphores, queued, 1u);
    CHECK(sem_a.ss_NestCount == 1 && sem_a.ss_QueueCount == 1);
    CHECK(sem_a.ss_WaitQueue.mlh_Head ==
          (struct MinNode *)(void *)waiter->operation->first->request);
    CHECK(record1->wait_scope.queued_count == 1u);

    CHECK(krnP4TaskRecordPublish(task3, record3, SysBase) == 1);
    CHECK(krnP4TaskRecordEnterRunning(task3, 1u, SysBase) == 1);
    CHECK(krnP4SemaphoreOperationReserve(task3, 1u, &third, SysBase) == 1);
    test_configure_operation(task3, &third, third_semaphores,
                             shared_one, 1u, SysBase);
    test_acquire_operation(task3, &third, third_semaphores, queued, 1u);
    CHECK(sem_b.ss_NestCount == 1 && sem_b.ss_QueueCount == 1);
    CHECK(sem_b.ss_WaitQueue.mlh_Head ==
          (struct MinNode *)(void *)third.operation->first->request);
    CHECK(record3->wait_scope.queued_count == 1u);

    poke_before = mock_poke_calls;
    CHECK(test_durable_release(&sem_a, SysBase) == 2);
    CHECK(sem_a.ss_NestCount == 1 && sem_a.ss_QueueCount == 0);
    CHECK(sem_a.ss_Owner == task1);
    CHECK(sem_a.ss_WaitQueue.mlh_Head ==
          (struct MinNode *)(void *)&sem_a.ss_WaitQueue.mlh_Tail);
    CHECK(record1->semaphore_pending == 1u);
    CHECK(record1->semaphore_pending_generation == 1u);
    CHECK(record1->semaphore_inflight == 0u);
    CHECK(record1->lifetime.notification_pins == 2u);
    CHECK(p4_cleanup_wake_pending);
    CHECK(mock_poke_calls == poke_before);

    test_prepare_unreleased_handoff(&sem_b);
    CHECK(sem_b.ss_NestCount == 0 && sem_b.ss_QueueCount == 0);
    CHECK(sem_b.ss_Owner == NULL);
    CHECK(test_durable_grant(&sem_b, SysBase) == 1);
    CHECK(sem_b.ss_NestCount == 1 && sem_b.ss_QueueCount == 0);
    CHECK(sem_b.ss_Owner == task3);
    CHECK(record3->semaphore_pending == 1u);
    CHECK(record3->semaphore_pending_generation == 2u);
    CHECK(record3->lifetime.notification_pins == 2u);
    CHECK(p4_cleanup_wake_pending);
    CHECK(mock_poke_calls == poke_before);

    /* Neither outer-gate binding poked. The API caller's required poke occurs
     * only after its semaphore gate is released; pre-arm work stays durable. */
    krnP4CleanupPoke(SysBase);
    CHECK(mock_poke_calls == poke_before + 1u);
    memset(&wake, 0xa5, sizeof(wake));
    CHECK(krnP4TakeCleanupWake(&wake, SysBase) == 0);
    CHECK(wake.worker == NULL && wake.mask == 0u);
    CHECK(p4_cleanup_wake_pending);
    CHECK(krnP4ArmCleanupWake(worker, UINT32_C(1) << 7, SysBase) == 1);
    CHECK(mock_poke_calls == poke_before + 2u); /* Arm re-poked the latch. */
    CHECK(krnP4TakeCleanupWake(&wake, SysBase) == 1);
    CHECK(wake.worker == worker && wake.mask == (UINT32_C(1) << 7));
    CHECK(!p4_cleanup_wake_pending);

    /* Registry claim keeps the complete exact batch pin while the worker
     * signals outside both gates. Task3's completed semaphore ownership is
     * released before its modeled retirement, so no semaphore remains held. */
    CHECK(krnP4SemaphoreNotificationClaim(worker, &snapshot, SysBase) == 1);
    CHECK(snapshot.identity == task3 && snapshot.count == 1u &&
          snapshot.generation == 2u);
    CHECK(record3->semaphore_pending == 0u);
    CHECK(record3->semaphore_inflight == 1u);
    CHECK(record3->lifetime.notification_pins == 2u);
    CHECK(krnP4SemaphoreOperationComplete(task3, third.generation,
              SysBase) == 1);
    CHECK(record3->lifetime.notification_pins == 1u);
    CHECK(test_durable_release(&sem_b, SysBase) == 1);
    CHECK(sem_b.ss_NestCount == 0 && sem_b.ss_QueueCount == -1);

    /* Retirement and RUN drain cannot queue cleanup while notification pins
     * remain. Exact Finish drops the last notification reference and then
     * reserves cleanup under the same registry gate. */
    mock_task_lock_held = 1;
    CHECK(krnP4TaskRecordRetireForService(task3, SysBase) == 1);
    mock_task_lock_held = 0;
    CHECK(record3->service_pending == 1u);
    CHECK(record3->semaphore_inflight == 1u);
    CHECK(record3->service_queued == 0u);
    krnP4TaskRecordLeaveRunning(task3, 1u, SysBase);
    CHECK(record3->lifetime.running_harts == 0u);
    CHECK(record3->service_queued == 0u);
    CHECK(record3->lifetime.notification_pins == 2u);

    poke_before = mock_poke_calls;
    krnP4SemaphoreNotificationFinish(worker, &snapshot, SysBase);
    CHECK(record3->semaphore_inflight == 0u);
    CHECK(record3->semaphore_inflight_generation == 0u);
    CHECK(record3->service_queued == 1u);
    CHECK(record3->lifetime.notification_pins == 1u);
    CHECK(p4_cleanup_wake_pending);
    CHECK(mock_poke_calls == poke_before + 1u);
    CHECK(krnP4TakeCleanupWake(&wake, SysBase) == 1);
    CHECK(wake.worker == worker && wake.mask == (UINT32_C(1) << 7));

    /* A second task's independent pending generation remains claimable after
     * the first exact batch and can be completed without disturbing it. */
    CHECK(krnP4SemaphoreOperationComplete(task1, waiter->generation,
              SysBase) == 1);
    CHECK(test_durable_release(&sem_a, SysBase) == 1);
    CHECK(sem_a.ss_NestCount == 0 && sem_a.ss_QueueCount == -1);
    CHECK(krnP4SemaphoreNotificationClaim(worker, &next_snapshot, SysBase) == 1);
    CHECK(next_snapshot.identity == task1 && next_snapshot.count == 1u &&
          next_snapshot.generation == 1u);
    CHECK(record1->semaphore_inflight == 1u);
    CHECK(record1->lifetime.notification_pins == 1u);
    krnP4SemaphoreNotificationFinish(worker, &next_snapshot, SysBase);
    CHECK(record1->semaphore_inflight == 0u);
    CHECK(record1->lifetime.notification_pins == 0u);
    CHECK(!p4_cleanup_wake_pending);
    CHECK(mock_poke_calls == poke_before + 1u);
    CHECK(mock_fatal_calls == 0u);
    CHECK(mock_gate_held == 0 && !mock_outer_semaphore_gate_held);
    CHECK(mock_irq_depth == 1u);
    CHECK(mock_disable_calls == mock_enable_calls);
    CHECK(mock_acquire_calls == mock_unlock_calls);

    /* Keep the final result visible to the caller if this helper's setup ever
     * returns an impossible registry address. */
    result = p4_task_registry_is_worker_locked(&p4_task_records, worker);
    CHECK(result == 1);
}

static void test_actual_gate_binding(void *page)
{
    static struct p4_semaphore_ledger boot_ledger;
    static struct p4_semaphore_ledger_operation boot_operations[TEST_OP_CAPACITY];
    static struct p4_semaphore_ledger_entry boot_entries[TEST_ENTRY_CAPACITY];
    static struct p4_task_registry_record record0;
    static struct p4_task_registry_record record1;
    static struct p4_task_registry_record record2;
    static struct p4_task_registry_record record3;
    struct p4_task_wait_operation first = { 0, 0 };
    struct p4_task_wait_operation second = { 0, 0 };
    struct p4_task_wait_operation failed = {
        (struct p4_semaphore_ledger_operation *)(uintptr_t)0x9876u,
        UINT32_C(0xdeadbeef)
    };
    struct p4_task_wait_operation before_failed = failed;
    struct p4_task_registry registry_before;
    struct p4_task_registry_record records_before[3];
    struct direct_fixture boot_state_view;
    struct ExecBase sysbase = { 0 };
    struct Task *task0 = guarded_identity(page, 448u);
    struct Task *task1 = guarded_identity(page, 512u);
    struct Task *task2 = guarded_identity(page, 576u);
    unsigned int lock_before;
    unsigned int unlock_before;
    int result;

    memset(&boot_ledger, 0, sizeof(boot_ledger));
    memset(boot_operations, 0, sizeof(boot_operations));
    memset(boot_entries, 0, sizeof(boot_entries));
    memset(&record0, 0, sizeof(record0));
    memset(&record1, 0, sizeof(record1));
    memset(&record2, 0, sizeof(record2));
    memset(&record3, 0, sizeof(record3));
    CHECK(p4_semaphore_ledger_init(&boot_ledger, boot_operations,
              TEST_OP_CAPACITY, boot_entries, TEST_ENTRY_CAPACITY) == 1);
    test_boot_ledger = &boot_ledger;

    mock_irq_depth = 1u; /* Match boot caller's already-masked precondition. */
    CHECK(krnP4TaskPinsPrepare() == 1);
    CHECK(mock_irq_depth == 1u);
    CHECK(krnP4TaskRecordPublish(task0, &record0, &sysbase) == 1);
    CHECK(krnP4TaskRecordPublish(task1, &record1, &sysbase) == 1);
    CHECK(krnP4TaskRecordPublish(task2, &record2, &sysbase) == 1);
    CHECK(krnP4TaskRecordEnterRunning(task0, 0u, &sysbase) == 1);
    CHECK(krnP4TaskRecordEnterRunning(task1, 1u, &sysbase) == 1);
    CHECK(krnP4TaskRecordEnterRunning(task2, 0u, &sysbase) == 1);
    lock_before = mock_acquire_calls;
    unlock_before = mock_unlock_calls;
    CHECK(krnP4SemaphoreOperationReserve(task0, 2u, &first, &sysbase) == 1);
    CHECK(first.operation != NULL && first.generation == 1u);
    CHECK(krnP4SemaphoreOperationReserve(task1, 1u, &second, &sysbase) == 1);
    CHECK(second.operation != NULL && second.generation == 1u);
    CHECK(mock_acquire_calls == lock_before + 2u);
    CHECK(mock_unlock_calls == unlock_before + 2u);
    CHECK(mock_irq_depth == 1u && !mock_gate_held);
    CHECK(first.operation->owner == task0 && first.operation->entry_count == 2u);
    CHECK(second.operation->owner == task1 && second.operation->entry_count == 1u);
    CHECK(p4_task_records.count == 3u);

    /* Third operation cannot fit: wrapper balances its gate while preserving
     * both registries and output. */
    memcpy(&registry_before, &p4_task_records, sizeof(p4_task_records));
    memcpy(records_before, (struct p4_task_registry_record[]){
        record0, record1, record2
    }, sizeof(records_before));
    memset(&boot_state_view, 0, sizeof(boot_state_view));
    memcpy(&boot_state_view.ledger, &boot_ledger, sizeof(boot_ledger));
    memcpy(boot_state_view.operations, boot_operations, sizeof(boot_operations));
    memcpy(boot_state_view.entries, boot_entries, sizeof(boot_entries));
    result = krnP4SemaphoreOperationReserve(task2,
              TEST_ENTRY_CAPACITY - boot_ledger.active_entries + 1u,
              &failed, &sysbase);
    CHECK(result == 0);
    CHECK(memcmp(&p4_task_records, &registry_before,
                 sizeof(p4_task_records)) == 0);
    CHECK(memcmp(records_before, (struct p4_task_registry_record[]){
                 record0, record1, record2
             }, sizeof(records_before)) == 0);
    CHECK(memcmp(&boot_ledger, &boot_state_view.ledger,
                 sizeof(boot_ledger)) == 0);
    CHECK(memcmp(boot_operations, boot_state_view.operations,
                 sizeof(boot_operations)) == 0);
    CHECK(memcmp(boot_entries, boot_state_view.entries,
                 sizeof(boot_entries)) == 0);
    CHECK(memcmp(&failed, &before_failed, sizeof(failed)) == 0);
    CHECK(mock_irq_depth == 1u && !mock_gate_held);
    CHECK(mock_fatal_calls == 0u);
    CHECK(mock_disable_calls == mock_enable_calls);
    CHECK(mock_acquire_calls == mock_unlock_calls);
    CHECK(mock_poke_calls == 0u);

    test_actual_durable_release_grant_and_delivery(page, &sysbase,
        task0, task1, task2, &first, &second,
        &record1, &record2, &record3);

    --mock_irq_depth;
}

int main(void)
{
    long page_size = sysconf(_SC_PAGESIZE);
    void *page;

    if (page_size <= 0)
    {
        fprintf(stderr, "sysconf(_SC_PAGESIZE) failed: %d\n", errno);
        return 2;
    }
#if defined(MAP_ANONYMOUS)
    page = mmap(NULL, (size_t)page_size, PROT_NONE,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
#else
    page = mmap(NULL, (size_t)page_size, PROT_NONE,
                MAP_PRIVATE | MAP_ANON, -1, 0);
#endif
    if (page == MAP_FAILED)
    {
        fprintf(stderr, "mmap(PROT_NONE) failed: %d\n", errno);
        return 2;
    }

    test_success_and_legacy_guards(page);
    test_legacy_mutators_reject_operation(page);
    test_refusals_and_capacity(page);
    test_alias_refusals(page);
    test_actual_gate_binding(page);

    if (munmap(page, (size_t)page_size) != 0)
    {
        fprintf(stderr, "munmap failed: %d\n", errno);
        return 2;
    }
    if (failures != 0u)
    {
        fprintf(stderr, "P4 semaphore operation admission: %u/%u checks failed\n",
                failures, checks);
        return 1;
    }
    printf("P4 semaphore operation admission: %u checks passed\n", checks);
    return 0;
}
