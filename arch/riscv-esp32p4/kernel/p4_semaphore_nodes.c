/* Real request storage and semaphore counts move with their private ledger.
 * Gates/storage ownership are caller contracts; no public caller is selected. */
#if !defined(P4_SEMAPHORE_NODES_TEST)
#include <exec/semaphores.h>
#endif
#include "p4_semaphore_nodes.h"
#include <stddef.h>
#include <limits.h>

static int p4_nodes_overlap(uintptr_t a, uintptr_t an,
    uintptr_t b, uintptr_t bn)
{
    return an == 0 || bn == 0 || a > UINTPTR_MAX - an ||
        b > UINTPTR_MAX - bn || (a < b + bn && b < a + an);
}

/* Full ledger validation happened in operation resolution. Check the complete
 * typed node pool before computing/dereferencing its selected absolute slot. */
static int p4_nodes_storage(struct p4_task_registry *registry,
    struct p4_semaphore_ledger *ledger, struct SemaphoreRequest *pool,
    struct SignalSemaphore *sem)
{
    uintptr_t p = (uintptr_t)pool;
    uintptr_t n = (uintptr_t)ledger->entry_capacity * sizeof(*pool);
    uintptr_t s = (uintptr_t)sem;
    struct p4_task_registry_record *record;
    if (pool == NULL || sem == NULL ||
        p % _Alignof(struct SemaphoreRequest) != 0 ||
        s % _Alignof(struct SignalSemaphore) != 0 ||
        p4_nodes_overlap(p, n, s, sizeof(*sem)) ||
        p4_nodes_overlap(p, n, (uintptr_t)registry, sizeof(*registry)) ||
        p4_nodes_overlap(p, n, (uintptr_t)ledger, sizeof(*ledger)) ||
        p4_nodes_overlap(p, n, (uintptr_t)ledger->operations,
            (uintptr_t)ledger->operation_capacity * sizeof(*ledger->operations)) ||
        p4_nodes_overlap(p, n, (uintptr_t)ledger->entries,
            (uintptr_t)ledger->entry_capacity * sizeof(*ledger->entries)) ||
        p4_nodes_overlap(s, sizeof(*sem), (uintptr_t)registry, sizeof(*registry)) ||
        p4_nodes_overlap(s, sizeof(*sem), (uintptr_t)ledger, sizeof(*ledger)) ||
        p4_nodes_overlap(s, sizeof(*sem), (uintptr_t)ledger->operations,
            (uintptr_t)ledger->operation_capacity * sizeof(*ledger->operations)) ||
        p4_nodes_overlap(s, sizeof(*sem), (uintptr_t)ledger->entries,
            (uintptr_t)ledger->entry_capacity * sizeof(*ledger->entries)))
        return 0;
    for (record = registry->head; record != NULL; record = record->next)
        if (p4_nodes_overlap(p, n, (uintptr_t)record, sizeof(*record)) ||
            p4_nodes_overlap(s, sizeof(*sem), (uintptr_t)record, sizeof(*record)))
            return 0;
    return 1;
}

struct p4_nodes_selection
{
    struct p4_task_registry_record *record;
    struct p4_semaphore_ledger_operation *operation;
    struct p4_semaphore_ledger_entry *entry;
    struct SemaphoreRequest *node;
};

/* The caller has validated ledger storage and topology. Include committed
 * entries: raw anonymous Release counts only pending units, whereas actual
 * NestCount must cover every indexed OWNED unit before any transition. */
static int p4_nodes_owned_consistent(struct p4_semaphore_ledger *ledger,
    struct SignalSemaphore *sem)
{
    struct p4_semaphore_ledger_entry *cursor;
    uint32_t owned = 0;
    for (cursor = ledger->requests_head; cursor != NULL;
         cursor = cursor->request_next)
        if (cursor->semaphore == sem &&
            cursor->state == P4_SEMAPHORE_LEDGER_ENTRY_OWNED)
            ++owned;
    return sem->ss_NestCount >= 0 &&
        owned <= (uint32_t)sem->ss_NestCount;
}

static int p4_nodes_select(struct p4_task_registry *registry,
    struct p4_semaphore_ledger *ledger, struct SemaphoreRequest *pool,
    struct Task *task, uint32_t generation, uint32_t ordinal,
    struct SignalSemaphore *sem, struct p4_nodes_selection *selection)
{
    uint32_t queued = 0;
    struct p4_semaphore_ledger_entry *cursor;
    int result;
    /* Pool/semaphore separation must precede the checked resolver's typed
     * arena reads. It preserves corruption separately from stale refusal. */
    if (!p4_task_registry_ledger_disjoint_locked(registry, ledger) ||
        !p4_nodes_storage(registry, ledger, pool, sem))
        return -1;
    result = p4_task_registry_operation_resolve_locked(registry, ledger,
        task, generation, &selection->record, &selection->operation);
    if (result != 1)
        return result;
    result = p4_semaphore_ledger_entry_at(ledger, selection->operation,
        ordinal, &selection->entry);
    if (result != 1)
        return result;
    /* Membership has been established; subtraction is now within one array. */
    selection->node = &pool[selection->entry - ledger->entries];
    for (cursor = selection->operation->first; cursor != NULL;
         cursor = cursor->operation_next)
        if (cursor->state == P4_SEMAPHORE_LEDGER_ENTRY_QUEUED)
            ++queued;
    if (queued != selection->record->wait_scope.queued_count)
        return -1;
    if (!p4_nodes_owned_consistent(ledger, sem))
        return -1;
    return 1;
}

/* Initialized, valid caller-owned intrusive queue storage is required. Bound
 * traversal and check reciprocal links/counts, without reading Task or the
 * sr_Waiter/Message overlay of any unrelated node. */
static int p4_nodes_queue(struct SignalSemaphore *sem,
    struct SemaphoreRequest *selected, uint32_t *found)
{
    struct MinNode *head = (struct MinNode *)(void *)&sem->ss_WaitQueue.mlh_Head;
    struct MinNode *tail = (struct MinNode *)(void *)&sem->ss_WaitQueue.mlh_Tail;
    struct MinNode *previous = head;
    struct MinNode *cursor = sem->ss_WaitQueue.mlh_Head;
    uint32_t count = 0;
    *found = 0;
    if (sem->ss_WaitQueue.mlh_Tail != NULL || cursor == NULL ||
        sem->ss_NestCount < 0 || sem->ss_QueueCount < -1)
        return 0;
    while (cursor != tail)
    {
        if (cursor == NULL || count == (uint32_t)INT16_MAX + 1u ||
            (uintptr_t)cursor % _Alignof(struct MinNode) != 0 ||
            cursor->mln_Pred != previous || cursor->mln_Succ == NULL)
            return 0;
        if (cursor == &selected->sr_Link)
            ++*found;
        previous = cursor;
        cursor = cursor->mln_Succ;
        ++count;
    }
    return tail->mln_Pred == previous &&
        (uint32_t)sem->ss_NestCount + count ==
            (uint32_t)((int32_t)sem->ss_QueueCount + 1) &&
        (sem->ss_NestCount != 0 || sem->ss_Owner == NULL);
}

static int p4_nodes_configured(const struct p4_nodes_selection *selection,
    struct SignalSemaphore *sem, struct Task *task)
{
    uintptr_t waiter = (uintptr_t)selection->node->sr_Waiter;
    return selection->entry->semaphore == sem &&
        selection->entry->request == selection->node &&
        (waiter & ~(uintptr_t)1u) == (uintptr_t)task &&
        selection->record->wait_scope.construction_closed == 1u &&
        selection->operation->construction_closed == 1u;
}

int p4_semaphore_nodes_configure_locked(struct p4_task_registry *registry,
    struct p4_semaphore_ledger *ledger, struct SemaphoreRequest *pool,
    struct Task *task, uint32_t generation, uint32_t ordinal,
    struct SignalSemaphore *sem, uint32_t shared)
{
    struct p4_nodes_selection selection;
    int result = p4_nodes_select(registry, ledger, pool, task, generation,
        ordinal, sem, &selection);
    if (result != 1)
        return result;
    if (selection.record->lifetime.flags != 0 ||
        (selection.record->lifetime.running_harts != 1u &&
         selection.record->lifetime.running_harts != 2u))
        return 0;
    if (shared > 1u || ((uintptr_t)task & 1u) != 0 ||
        selection.node->sr_Link.mln_Succ != NULL ||
        selection.node->sr_Link.mln_Pred != NULL)
        return -1;
    result = p4_task_registry_wait_operation_set_entry_locked(registry,
        ledger, task, generation, ordinal, sem, selection.node);
    if (result != 1)
        return result;
    selection.node->sr_Waiter = (struct Task *)((uintptr_t)task | shared);
    return 1;
}

int p4_semaphore_nodes_acquire_locked(struct p4_task_registry *registry,
    struct p4_semaphore_ledger *ledger, struct SemaphoreRequest *pool,
    struct Task *task, uint32_t generation, uint32_t ordinal,
    struct SignalSemaphore *sem)
{
    struct p4_nodes_selection selection;
    struct Task *owner;
    uint32_t found;
    int immediate, result = p4_nodes_select(registry, ledger, pool, task,
        generation, ordinal, sem, &selection);
    if (result != 1)
        return result;
    if (selection.record->lifetime.flags != 0 ||
        (selection.record->lifetime.running_harts != 1u &&
         selection.record->lifetime.running_harts != 2u) ||
        selection.operation->cancelled != 0 ||
        selection.entry->state != P4_SEMAPHORE_LEDGER_ENTRY_UNVISITED)
        return 0;
    if (!p4_nodes_configured(&selection, sem, task) ||
        !p4_nodes_queue(sem, selection.node, &found) || found != 0 ||
        selection.node->sr_Link.mln_Succ != NULL ||
        selection.node->sr_Link.mln_Pred != NULL ||
        sem->ss_QueueCount == INT16_MAX)
        return -1;
    owner = ((uintptr_t)selection.node->sr_Waiter & 1u) != 0 ? NULL : task;
    immediate = sem->ss_QueueCount == -1 ||
        sem->ss_Owner == task || sem->ss_Owner == owner;
    /* A count-free queue awaiting a release handoff cannot be bypassed. */
    if (sem->ss_NestCount == 0 && sem->ss_QueueCount != -1)
        immediate = 0;
    if (immediate && sem->ss_NestCount == INT16_MAX)
        return -1;
    result = immediate ?
        p4_semaphore_ledger_acquire(ledger, selection.operation, ordinal) :
        p4_semaphore_ledger_queue(ledger, selection.operation, ordinal);
    if (result != 1)
        return result;
    /* All refusal points precede this infallible actual-node/count commit. */
    ++sem->ss_QueueCount;
    if (immediate)
    {
        if (sem->ss_NestCount == 0)
            sem->ss_Owner = owner;
        ++sem->ss_NestCount;
        return 1;
    }
    selection.node->sr_Link.mln_Pred = sem->ss_WaitQueue.mlh_TailPred;
    selection.node->sr_Link.mln_Succ =
        (struct MinNode *)(void *)&sem->ss_WaitQueue.mlh_Tail;
    sem->ss_WaitQueue.mlh_TailPred->mln_Succ = &selection.node->sr_Link;
    sem->ss_WaitQueue.mlh_TailPred = &selection.node->sr_Link;
    ++selection.record->wait_scope.queued_count;
    return 2;
}

int p4_semaphore_nodes_withdraw_locked(struct p4_task_registry *registry,
    struct p4_semaphore_ledger *ledger, struct SemaphoreRequest *pool,
    struct Task *task, uint32_t generation, uint32_t ordinal,
    struct SignalSemaphore *sem)
{
    struct p4_nodes_selection selection;
    uint32_t found;
    int result = p4_nodes_select(registry, ledger, pool, task, generation,
        ordinal, sem, &selection);
    if (result != 1)
        return result;
    if (selection.record->wait_scope.cancel_claimed != 1u ||
        selection.operation->cancelled != 1u ||
        selection.entry->state != P4_SEMAPHORE_LEDGER_ENTRY_QUEUED)
        return 0;
    if (!p4_nodes_configured(&selection, sem, task) ||
        !p4_nodes_queue(sem, selection.node, &found) || found != 1u)
        return -1;
    result = p4_semaphore_ledger_withdraw(ledger, selection.operation, ordinal);
    if (result != 1)
        return result;
    selection.node->sr_Link.mln_Pred->mln_Succ = selection.node->sr_Link.mln_Succ;
    selection.node->sr_Link.mln_Succ->mln_Pred = selection.node->sr_Link.mln_Pred;
    selection.node->sr_Link.mln_Succ = NULL;
    selection.node->sr_Link.mln_Pred = NULL;
    --sem->ss_QueueCount;
    --selection.record->wait_scope.queued_count;
    return 1;
}

static int p4_nodes_rollback_preflight(struct p4_task_registry *registry,
    struct p4_semaphore_ledger *ledger, struct SemaphoreRequest *pool,
    struct Task *task, uint32_t generation, uint32_t ordinal,
    struct SignalSemaphore *sem, struct p4_nodes_selection *selection)
{
    uint32_t found;
    int result = p4_nodes_select(registry, ledger, pool, task, generation,
        ordinal, sem, selection);
    if (result != 1)
        return result;
    if (selection->record->wait_scope.cancel_claimed != 1u ||
        selection->record->lifetime.running_harts != 0 ||
        selection->operation->cancelled != 1u ||
        selection->operation->committed != 0 ||
        selection->entry->state != P4_SEMAPHORE_LEDGER_ENTRY_OWNED)
        return 0;
    if (!p4_nodes_configured(selection, sem, task) ||
        !p4_nodes_queue(sem, selection->node, &found) || found != 0 ||
        selection->node->sr_Link.mln_Succ != NULL ||
        selection->node->sr_Link.mln_Pred != NULL || sem->ss_NestCount == 0)
        return -1;
    return 1;
}

static int p4_nodes_rollback_commit(struct p4_semaphore_ledger *ledger,
    struct p4_nodes_selection *selection, uint32_t ordinal,
    struct SignalSemaphore *sem)
{
    int result = p4_semaphore_ledger_rollback_owned(ledger,
        selection->operation, ordinal);
    if (result != 1)
        return result;
    --sem->ss_NestCount;
    --sem->ss_QueueCount;
    if (sem->ss_NestCount == 0)
        sem->ss_Owner = NULL;
    return 1;
}

int p4_semaphore_nodes_rollback_locked(struct p4_task_registry *registry,
    struct p4_semaphore_ledger *ledger, struct SemaphoreRequest *pool,
    struct Task *task, uint32_t generation, uint32_t ordinal,
    struct SignalSemaphore *sem)
{
    struct p4_nodes_selection selection;
    int result = p4_nodes_rollback_preflight(registry, ledger, pool, task,
        generation, ordinal, sem, &selection);
    if (result != 1)
        return result;
    return p4_nodes_rollback_commit(ledger, &selection, ordinal, sem);
}

/* Classify by the permanent arena/index BEFORE inspecting the waiter tag.
 * Unknown legacy/Procure overlays are deliberately not interpreted here. */
static int p4_nodes_queued_selection(struct p4_task_registry *registry,
    struct p4_semaphore_ledger *ledger, struct SemaphoreRequest *pool,
    struct SignalSemaphore *sem, struct MinNode *node,
    struct p4_nodes_selection *selection)
{
    uintptr_t address = (uintptr_t)node, base = (uintptr_t)pool;
    uintptr_t bytes = (uintptr_t)ledger->entry_capacity * sizeof(*pool);
    struct p4_semaphore_ledger_entry *entry, *cursor;
    uint32_t ordinal = 0;
    int result;
    if (pool == NULL || base > UINTPTR_MAX - bytes || address < base ||
        address >= base + bytes || (address - base) % sizeof(*pool) != 0)
        return -1;
    entry = &ledger->entries[(address - base) / sizeof(*pool)];
    if (entry->state != P4_SEMAPHORE_LEDGER_ENTRY_QUEUED ||
        entry->request != node || entry->semaphore != sem)
        return -1;
    for (cursor = entry->operation->first; cursor != entry;
         cursor = cursor->operation_next)
        ++ordinal; /* Full ledger validation proved membership and bounds. */
    result = p4_nodes_select(registry, ledger, pool,
        (struct Task *)entry->operation->owner, entry->operation->generation,
        ordinal, sem, selection);
    if (result != 1)
        return result;
    if (!p4_nodes_configured(selection, sem, selection->record->identity))
        return -1;
    if (selection->record->lifetime.flags != 0 ||
        selection->operation->cancelled != 0 ||
        selection->operation->committed != 0)
        return 0;
    return 1;
}

static int p4_nodes_targets_storage(struct p4_task_registry *registry,
    struct p4_semaphore_ledger *ledger, struct SemaphoreRequest *pool,
    struct SignalSemaphore *sem, struct Task **targets, uint32_t capacity)
{
    uintptr_t t = (uintptr_t)targets;
    uintptr_t n = (uintptr_t)capacity * sizeof(*targets);
    struct p4_task_registry_record *record;
    if (targets == NULL || t % _Alignof(struct Task *) != 0 ||
        p4_nodes_overlap(t, n, (uintptr_t)sem, sizeof(*sem)) ||
        p4_nodes_overlap(t, n, (uintptr_t)pool,
            (uintptr_t)ledger->entry_capacity * sizeof(*pool)) ||
        p4_nodes_overlap(t, n, (uintptr_t)registry, sizeof(*registry)) ||
        p4_nodes_overlap(t, n, (uintptr_t)ledger, sizeof(*ledger)) ||
        p4_nodes_overlap(t, n, (uintptr_t)ledger->operations,
            (uintptr_t)ledger->operation_capacity * sizeof(*ledger->operations)) ||
        p4_nodes_overlap(t, n, (uintptr_t)ledger->entries,
            (uintptr_t)ledger->entry_capacity * sizeof(*ledger->entries)))
        return 0;
    for (record = registry->head; record != NULL; record = record->next)
        if (p4_nodes_overlap(t, n, (uintptr_t)record, sizeof(*record)))
            return 0;
    return 1;
}

/* Positive result is cohort size + 1, including an empty valid queue.
 * Zero/-1 remain refusal/malformed, with no shared-state/output mutation. */
static int p4_nodes_cohort_preflight(struct p4_task_registry *registry,
    struct p4_semaphore_ledger *ledger, struct SemaphoreRequest *pool,
    struct SignalSemaphore *sem, struct Task **targets, uint32_t capacity,
    uint32_t expected_nest, int durable, int *out_shared_head)
{
    struct MinNode *cursor, *other, *tail;
    struct p4_nodes_selection selection;
    struct p4_semaphore_ledger_entry *indexed;
    struct p4_task_registry_record *pin_record;
    uint32_t found, count = 0, copies, queued = 0, indexed_queued = 0;
    uint32_t new_batches = 0;
    int result, shared_head;
    if (registry == NULL || ledger == NULL || pool == NULL || sem == NULL ||
        !p4_task_registry_ledger_disjoint_locked(registry, ledger))
        return -1;
    if (durable)
        capacity = ledger->entry_capacity;
    if (capacity == 0 || capacity > ledger->entry_capacity)
        return 0;
    /* All typed storage separation precedes ledger-array/queue dereferences. */
    if (!p4_nodes_storage(registry, ledger, pool, sem) ||
        (!durable && !p4_nodes_targets_storage(registry, ledger, pool, sem,
            targets, capacity)) ||
        !p4_semaphore_ledger_validate(ledger) ||
        !p4_nodes_queue(sem, &pool[0], &found) ||
        !p4_nodes_owned_consistent(ledger, sem))
        return -1;
    if ((uint32_t)sem->ss_NestCount != expected_nest)
        return 0;
    tail = (struct MinNode *)(void *)&sem->ss_WaitQueue.mlh_Tail;
    cursor = sem->ss_WaitQueue.mlh_Head;
    for (indexed = ledger->requests_head; indexed != NULL;
         indexed = indexed->request_next)
        if (indexed->semaphore == sem &&
            indexed->state == P4_SEMAPHORE_LEDGER_ENTRY_QUEUED)
            ++indexed_queued;
    if (cursor == tail)
    {
        if (indexed_queued != 0)
            return -1;
        *out_shared_head = 0;
        return 1;
    }
    result = p4_nodes_queued_selection(registry, ledger, pool, sem, cursor,
        &selection);
    if (result != 1)
        return result;
    shared_head = ((uintptr_t)selection.node->sr_Waiter & 1u) != 0;
    /* Validate every queue identity, including skipped exclusive entries. */
    for (; cursor != tail; cursor = cursor->mln_Succ)
    {
        result = p4_nodes_queued_selection(registry, ledger, pool, sem, cursor,
            &selection);
        if (result != 1)
            return result;
        ++queued;
        if (cursor != sem->ss_WaitQueue.mlh_Head &&
            (!shared_head || ((uintptr_t)selection.node->sr_Waiter & 1u) == 0))
            continue;
        if (++count > capacity || count > (uint32_t)INT16_MAX)
            return 0;
    }
    if (indexed_queued != queued)
        return -1; /* Includes ledger-only lost queue nodes. */
    /* Every node/tag/identity is now validated and both gates still exclude
     * mutation. Aggregate pins once per record using copied identity values,
     * not another full ledger/registry resolution for each peer node. */
    for (pin_record = registry->head; pin_record != NULL;
         pin_record = pin_record->next)
    {
        copies = 0;
        for (other = sem->ss_WaitQueue.mlh_Head; other != tail;
             other = other->mln_Succ)
        {
            uintptr_t waiter = (uintptr_t)
                ((struct SemaphoreRequest *)(void *)other)->sr_Waiter;
            if ((other == sem->ss_WaitQueue.mlh_Head ||
                 (shared_head && (waiter & 1u) != 0)) &&
                (waiter & ~(uintptr_t)1u) == (uintptr_t)pin_record->identity)
                ++copies;
        }
        if (pin_record->lifetime.notification_pins > UINT32_MAX - copies)
            return 0;
        if (durable && copies != 0)
        {
            if (pin_record->semaphore_pending > UINT32_MAX - copies)
                return 0;
            if (pin_record->semaphore_pending == 0)
                ++new_batches;
        }
    }
    if (durable && registry->semaphore_notification_serial >
            UINT32_MAX - new_batches)
        return 0; /* Never wrap a notification batch identity. */
    *out_shared_head = shared_head;
    return (int)count + 1;
}

static int p4_nodes_cohort_commit(struct p4_task_registry *registry,
    struct p4_semaphore_ledger *ledger, struct SemaphoreRequest *pool,
    struct SignalSemaphore *sem, struct Task **targets, int shared_head,
    int durable)
{
    struct MinNode *cursor, *next;
    struct MinNode *tail = (struct MinNode *)(void *)&sem->ss_WaitQueue.mlh_Tail;
    uint32_t output = 0;
    struct Task *first_target = NULL;
    /* Both gates remain held: all refusal points have passed, and every
     * field below is validated. No fallible helper/callout during commit. */
    cursor = sem->ss_WaitQueue.mlh_Head;
    while (cursor != tail)
    {
        struct SemaphoreRequest *node = (struct SemaphoreRequest *)(void *)cursor;
        struct p4_semaphore_ledger_entry *entry = &ledger->entries[
            ((uintptr_t)cursor - (uintptr_t)pool) / sizeof(*pool)];
        struct p4_task_registry_record *record;
        next = cursor->mln_Succ;
        if (output == 0 || (shared_head && ((uintptr_t)node->sr_Waiter & 1u) != 0))
        {
            /* Preflight proved this identity present; the registry gate
             * excludes detach. No fallible lookup during cohort commit. */
            for (record = registry->head;
                 record->identity != entry->operation->owner;
                 record = record->next)
                ;
            ++record->lifetime.notification_pins;
            if (durable)
            {
                if (record->semaphore_pending == 0)
                    record->semaphore_pending_generation =
                        ++registry->semaphore_notification_serial;
                ++record->semaphore_pending;
            }
            --record->wait_scope.queued_count;
            entry->state = P4_SEMAPHORE_LEDGER_ENTRY_OWNED;
            cursor->mln_Pred->mln_Succ = next;
            next->mln_Pred = cursor->mln_Pred;
            cursor->mln_Succ = NULL;
            cursor->mln_Pred = NULL;
            node->sr_Waiter = record->identity; /* Normalize detached tag. */
            if (output == 0)
                first_target = record->identity;
            if (!durable)
                targets[output] = record->identity;
            ++output;
            ++sem->ss_NestCount;
        }
        cursor = next;
    }
    sem->ss_Owner = shared_head ? NULL : first_target;
    return (int)output;
}

int p4_semaphore_nodes_grant_cohort_locked(struct p4_task_registry *registry,
    struct p4_semaphore_ledger *ledger, struct SemaphoreRequest *pool,
    struct SignalSemaphore *sem, struct Task **targets, uint32_t capacity)
{
    int shared_head, result = p4_nodes_cohort_preflight(registry, ledger, pool,
        sem, targets, capacity, 0, 0, &shared_head);
    if (result <= 1)
        return result == 1 ? 0 : result;
    return p4_nodes_cohort_commit(registry, ledger, pool, sem, targets,
        shared_head, 0);
}

int p4_semaphore_nodes_grant_durable_locked(struct p4_task_registry *registry,
    struct p4_semaphore_ledger *ledger, struct SemaphoreRequest *pool,
    struct SignalSemaphore *sem)
{
    int shared_head, result = p4_nodes_cohort_preflight(registry, ledger, pool,
        sem, NULL, 0, 0, 1, &shared_head);
    if (result <= 1)
        return result == 1 ? 0 : result;
    return p4_nodes_cohort_commit(registry, ledger, pool, sem, NULL,
        shared_head, 1);
}

static int p4_nodes_release(struct p4_task_registry *registry,
    struct p4_semaphore_ledger *ledger, struct SemaphoreRequest *pool,
    struct SignalSemaphore *sem, struct Task **targets, uint32_t capacity,
    int durable)
{
    uint32_t found, selected_pending;
    int result, shared_head = 0;
    if (registry == NULL || ledger == NULL || pool == NULL || sem == NULL ||
        !p4_task_registry_ledger_disjoint_locked(registry, ledger) ||
        !p4_nodes_storage(registry, ledger, pool, sem) ||
        !p4_semaphore_ledger_validate(ledger) ||
        !p4_nodes_queue(sem, &pool[0], &found) ||
        !p4_nodes_owned_consistent(ledger, sem) || sem->ss_NestCount == 0)
        return -1;
    result = 1;
    if (sem->ss_NestCount == 1)
    {
        result = p4_nodes_cohort_preflight(registry, ledger, pool, sem,
            targets, capacity, 1, durable, &shared_head);
        if (result <= 0)
            return result; /* No ownership/count/pin/output mutation yet. */
    }
    /* The anonymous ledger transition consumes completed units first; only
     * if all current units are pending does it mark one OWNED as RELEASED.
     * Its refusal points precede all mutation. Local output is independent. */
    if (p4_semaphore_ledger_release(ledger, sem,
            (uint32_t)sem->ss_NestCount, &selected_pending) != 1)
        return -1;
    --sem->ss_NestCount;
    --sem->ss_QueueCount;
    if (sem->ss_NestCount != 0)
        return 1;
    sem->ss_Owner = NULL;
    if (result == 1)
        return 1; /* Preflight proved an empty queue. */
    return p4_nodes_cohort_commit(registry, ledger, pool, sem,
        targets, shared_head, durable) + 1;
}

int p4_semaphore_nodes_release_locked(struct p4_task_registry *registry,
    struct p4_semaphore_ledger *ledger, struct SemaphoreRequest *pool,
    struct SignalSemaphore *sem, struct Task **targets, uint32_t capacity)
{
    return p4_nodes_release(registry, ledger, pool, sem, targets, capacity, 0);
}

int p4_semaphore_nodes_release_durable_locked(struct p4_task_registry *registry,
    struct p4_semaphore_ledger *ledger, struct SemaphoreRequest *pool,
    struct SignalSemaphore *sem)
{
    return p4_nodes_release(registry, ledger, pool, sem, NULL, 0, 1);
}

int p4_semaphore_nodes_rollback_durable_locked(struct p4_task_registry *registry,
    struct p4_semaphore_ledger *ledger, struct SemaphoreRequest *pool,
    struct Task *task, uint32_t generation, uint32_t ordinal,
    struct SignalSemaphore *sem)
{
    struct p4_nodes_selection selection;
    int shared_head = 0;
    int cohort = 1;
    int result = p4_nodes_rollback_preflight(registry, ledger, pool, task,
        generation, ordinal, sem, &selection);
    if (result != 1)
        return result;
    if (sem->ss_NestCount == 1)
    {
        cohort = p4_nodes_cohort_preflight(registry, ledger, pool, sem,
            NULL, 0, 1, 1, &shared_head);
        if (cohort <= 0)
            return cohort; /* Retain the cancelled unit until handoff fits. */
    }
    /* The same two gates exclude changes between preflight and commit.
     * Ledger rollback has no refusal after its own mutation. Thereafter,
     * grant commit has no fallible helper, allocation or callback. */
    result = p4_nodes_rollback_commit(ledger, &selection, ordinal, sem);
    if (result != 1)
        return result;
    if (sem->ss_NestCount != 0 || cohort == 1)
        return 1;
    return 1 + p4_nodes_cohort_commit(registry, ledger, pool, sem, NULL,
        shared_head, 1);
}
