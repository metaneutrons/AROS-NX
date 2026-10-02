/* Metadata-only entry snapshot for the protected cancellation continuation. */
#if !defined(P4_SEMAPHORE_NODES_TEST)
#include <exec/semaphores.h>
#endif
#include "p4_semaphore_cancel.h"
#include <stddef.h>

static int p4_cancel_overlap(uintptr_t a, uintptr_t an,
    uintptr_t b, uintptr_t bn)
{
    return an == 0 || bn == 0 || a > UINTPTR_MAX - an ||
        b > UINTPTR_MAX - bn || (a < b + bn && b < a + an);
}

int p4_semaphore_cancel_describe_locked(struct p4_task_registry *registry,
    struct p4_semaphore_ledger *ledger, struct SemaphoreRequest *pool,
    struct Task *target, uint32_t generation, uint32_t ordinal,
    struct p4_semaphore_cancel_entry_snapshot *out)
{
    struct p4_task_registry_record *record, *cursor;
    struct p4_semaphore_ledger_operation *operation;
    struct p4_semaphore_ledger_entry *entry;
    uintptr_t output = (uintptr_t)out;
    uintptr_t bytes = sizeof(*out);
    uintptr_t pool_bytes;
    int result;

    if (out == NULL ||
        output % _Alignof(struct p4_semaphore_cancel_entry_snapshot) != 0 ||
        pool == NULL ||
        (uintptr_t)pool % _Alignof(struct SemaphoreRequest) != 0 ||
        !p4_task_registry_ledger_disjoint_locked(registry, ledger))
        return -1;
    pool_bytes = (uintptr_t)ledger->entry_capacity * sizeof(*pool);
    if (p4_cancel_overlap(output, bytes, (uintptr_t)registry,
            sizeof(*registry)) ||
        p4_cancel_overlap(output, bytes, (uintptr_t)ledger, sizeof(*ledger)) ||
        p4_cancel_overlap(output, bytes, (uintptr_t)ledger->operations,
            (uintptr_t)ledger->operation_capacity * sizeof(*ledger->operations)) ||
        p4_cancel_overlap(output, bytes, (uintptr_t)ledger->entries,
            (uintptr_t)ledger->entry_capacity * sizeof(*ledger->entries)) ||
        p4_cancel_overlap(output, bytes, (uintptr_t)pool, pool_bytes))
        return -1;
    for (cursor = registry->head; cursor != NULL; cursor = cursor->next)
        if (p4_cancel_overlap(output, bytes, (uintptr_t)cursor,
                sizeof(*cursor)))
            return -1;
    result = p4_task_registry_operation_resolve_locked(registry, ledger,
        target, generation, &record, &operation);
    if (result != 1)
        return result;
    if (
        (record->lifetime.flags & P4_TASK_LIFETIME_RETIRED) == 0 ||
        record->wait_scope.cancel_requested != 1u ||
        record->wait_scope.cancel_claimed != 1u || operation->cancelled != 1u)
        return 0;
    result = p4_semaphore_ledger_entry_at(ledger, operation, ordinal, &entry);
    if (result != 1)
        return result;
    if (entry->semaphore != NULL &&
        p4_cancel_overlap(output, bytes, (uintptr_t)entry->semaphore,
            sizeof(struct SignalSemaphore)))
        return -1;

    /* All validation precedes output publication; no arena pointer escapes. */
    out->semaphore = (struct SignalSemaphore *)entry->semaphore;
    out->entry_count = operation->entry_count;
    out->state = entry->state;
    out->committed = operation->committed;
    out->running_harts = record->lifetime.running_harts;
    return 1;
}
