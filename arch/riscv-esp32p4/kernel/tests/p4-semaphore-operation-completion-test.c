/* Actual private C bodies; caller gate and real queue/count work are modeled.
 * This does not qualify public Exec calls, concurrency or hardware. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define __AROSEXEC_SMP__ 1
#define __AROSPLATFORM_SMP__ 1
#include "../p4_task_lifetime.c"
#include "../p4_semaphore_ledger.c"
#include "../p4_task_registry.c"

static unsigned int checks;
#define CHECK(c) do { ++checks; if (!(c)) { \
    fprintf(stderr, "FAIL %u at %d: %s\n", checks, __LINE__, #c); \
    exit(EXIT_FAILURE); } } while (0)

struct fixture
{
    struct p4_task_registry registry;
    struct p4_task_registry_record record;
    struct p4_semaphore_ledger ledger;
    struct p4_semaphore_ledger_operation operations[3];
    struct p4_semaphore_ledger_entry entries[6];
};

static struct Task *const owner = (struct Task *)(uintptr_t)0x1000;

static void initialize(struct fixture *f, uint32_t hart)
{
    memset(f, 0, sizeof(*f));
    CHECK(p4_semaphore_ledger_init(&f->ledger, f->operations, 3,
        f->entries, 6) == 1);
    CHECK(p4_task_registry_register_locked(&f->registry, &f->record, owner));
    CHECK(p4_task_registry_enter_running_locked(&f->registry, owner, hart));
}

static struct p4_task_wait_operation reserve(struct fixture *f, uint32_t count)
{
    struct p4_task_wait_operation out = {0};
    CHECK(p4_task_registry_wait_reserve_operation_locked(&f->registry,
        &f->ledger, owner, count, &out) == 1);
    CHECK(out.operation != NULL && out.generation != 0);
    return out;
}

static void configure(struct fixture *f, uint32_t generation, uint32_t count)
{
    uint32_t i;
    for (i = 0; i < count; ++i)
        CHECK(p4_task_registry_wait_operation_set_entry_locked(&f->registry,
            &f->ledger, owner, generation, i,
            (const void *)(uintptr_t)(0x2000u + i * 16u),
            (const void *)(uintptr_t)(0x3000u + i * 16u)) == 1);
}

static void complete_owned(uint32_t hart)
{
    struct fixture f, before;
    struct p4_task_wait_operation op;
    uint32_t i;
    initialize(&f, hart);
    CHECK(p4_task_registry_pin_locked(&f.registry, owner));
    op = reserve(&f, 3);
    CHECK(f.record.lifetime.notification_pins == 2);
    before = f;
    CHECK(p4_task_registry_wait_operation_close_locked(&f.registry,
        &f.ledger, owner, op.generation) == 0);
    CHECK(memcmp(&before, &f, sizeof(f)) == 0);
    configure(&f, op.generation, 3);
    CHECK(p4_task_registry_wait_operation_close_locked(&f.registry,
        &f.ledger, owner, op.generation) == 1);
    CHECK(f.record.wait_scope.construction_closed == 1);
    CHECK(op.operation->construction_closed == 1);
    before = f;
    CHECK(p4_task_registry_wait_operation_complete_locked(&f.registry,
        &f.ledger, owner, op.generation) == 0);
    CHECK(memcmp(&before, &f, sizeof(f)) == 0);
    for (i = 0; i < 3; ++i)
        CHECK(p4_semaphore_ledger_acquire(&f.ledger, op.operation, i) == 1);
    CHECK(p4_task_registry_wait_operation_complete_locked(&f.registry,
        &f.ledger, owner, op.generation) == 1);
    CHECK(f.record.wait_scope.active == 0);
    CHECK(f.record.wait_scope.generation == op.generation);
    CHECK(f.record.lifetime.notification_pins == 1);
    CHECK(f.ledger.active_operations == 0 && f.ledger.active_entries == 0);
    CHECK(f.ledger.requests_head == NULL && f.ledger.request_count == 0);
    before = f;
    CHECK(p4_task_registry_wait_operation_complete_locked(&f.registry,
        &f.ledger, owner, op.generation) == 0);
    CHECK(memcmp(&before, &f, sizeof(f)) == 0);
    op = reserve(&f, 1);
    CHECK(op.generation == 2);
}

static void cancel_owned(uint32_t hart, uint32_t service)
{
    struct fixture f, before;
    struct p4_task_wait_operation op;
    struct p4_task_wait_cancel_snapshot snapshot = {0};
    _Alignas(struct p4_task_wait_cancel_snapshot)
        unsigned char unaligned[sizeof(snapshot) + 8];
    unsigned char saved_unaligned[sizeof(unaligned)];
    initialize(&f, hart);
    CHECK(p4_task_registry_pin_locked(&f.registry, owner));
    op = reserve(&f, 1);
    configure(&f, op.generation, 1);
    CHECK(p4_task_registry_wait_operation_close_locked(&f.registry,
        &f.ledger, owner, op.generation) == 1);
    CHECK(p4_semaphore_ledger_acquire(&f.ledger, op.operation, 0) == 1);
    before = f;
    CHECK(p4_task_registry_retire_locked(&f.registry, owner) == 0);
    CHECK(p4_task_registry_retire_for_service_locked(&f.registry, owner) == 0);
    CHECK(memcmp(&before, &f, sizeof(f)) == 0);
    CHECK(p4_task_registry_retire_with_ledger_locked(&f.registry, &f.ledger,
        owner, service) == 1);
    CHECK(op.operation->cancelled == 1);
    CHECK(f.record.wait_scope.cancel_requested == 1);
    CHECK(f.record.service_pending == service);
    CHECK(p4_task_registry_claim_next_wait_cancel_with_ledger_locked(
        &f.registry, &f.ledger, &snapshot) == 0);
    CHECK(p4_task_registry_leave_running_locked(&f.registry, owner, hart));
    memset(unaligned, 0xaa, sizeof(unaligned));
    memcpy(saved_unaligned, unaligned, sizeof(unaligned));
    before = f;
    CHECK(p4_task_registry_claim_next_wait_cancel_with_ledger_locked(
        &f.registry, &f.ledger,
        (struct p4_task_wait_cancel_snapshot *)(void *)(unaligned + 1)) == -1);
    CHECK(memcmp(&before, &f, sizeof(f)) == 0);
    CHECK(memcmp(saved_unaligned, unaligned, sizeof(unaligned)) == 0);
    CHECK(p4_task_registry_claim_next_wait_cancel_with_ledger_locked(
        &f.registry, &f.ledger,
        (struct p4_task_wait_cancel_snapshot *)(void *)op.operation) == -1);
    CHECK(memcmp(&before, &f, sizeof(f)) == 0);
    CHECK(p4_task_registry_claim_next_wait_cancel_with_ledger_locked(
        &f.registry, &f.ledger, &snapshot) == 1);
    CHECK(snapshot.kind == P4_TASK_WAIT_OPERATION);
    CHECK(snapshot.list_index == op.operation);
    before = f;
    CHECK(p4_task_registry_wait_operation_finish_cancel_locked(&f.registry,
        &f.ledger, owner, op.generation) == 0);
    CHECK(memcmp(&before, &f, sizeof(f)) == 0);
    /* Models the caller's separately gated semaphore NestCount rollback.
     * No real semaphore is mutated or qualified by this fixture. */
    CHECK(p4_semaphore_ledger_rollback_owned(&f.ledger, op.operation, 0) == 1);
    CHECK(p4_task_registry_wait_operation_finish_cancel_locked(&f.registry,
        &f.ledger, owner, op.generation) == 1);
    CHECK(f.record.wait_scope.active == 0);
    CHECK(f.record.lifetime.notification_pins == 1u + service);
    CHECK(f.ledger.active_operations == 0 && f.ledger.active_entries == 0);
}

static void cancel_construction(void)
{
    struct fixture f, before;
    struct p4_task_wait_operation op;
    struct p4_task_wait_cancel_snapshot snapshot = {0};
    initialize(&f, 0);
    op = reserve(&f, 3);
    configure(&f, op.generation, 1);
    CHECK(p4_task_registry_retire_with_ledger_locked(&f.registry, &f.ledger,
        owner, 1) == 1);
    before = f;
    CHECK(p4_task_registry_wait_operation_set_entry_locked(&f.registry,
        &f.ledger, owner, op.generation, 1, (void *)(uintptr_t)0x4000,
        (void *)(uintptr_t)0x5000) == 0);
    CHECK(memcmp(&before, &f, sizeof(f)) == 0);
    CHECK(p4_task_registry_leave_running_locked(&f.registry, owner, 0));
    CHECK(p4_task_registry_claim_next_wait_cancel_with_ledger_locked(
        &f.registry, &f.ledger, &snapshot) == 1);
    CHECK(p4_task_registry_wait_operation_finish_cancel_locked(&f.registry,
        &f.ledger, owner, op.generation) == 1);
    CHECK(f.record.lifetime.notification_pins == 1);
    CHECK(f.ledger.active_operations == 0 && f.ledger.active_entries == 0);
}

int main(void)
{
    uint32_t hart, service;
    for (hart = 0; hart < 2; ++hart)
    {
        complete_owned(hart);
        for (service = 0; service < 2; ++service)
            cancel_owned(hart, service);
    }
    cancel_construction();
    printf("P4 operation completion checks: %u passed\n", checks);
    return EXIT_SUCCESS;
}
