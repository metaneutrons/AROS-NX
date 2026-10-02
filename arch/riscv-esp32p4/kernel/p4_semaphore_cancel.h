/* Private cancellation consumer metadata; no borrowed arena pointers. */
#ifndef P4_SEMAPHORE_CANCEL_H
#define P4_SEMAPHORE_CANCEL_H
#include "p4_semaphore_ledger.h"
#include "p4_task_registry.h"
struct SemaphoreRequest;
struct SignalSemaphore;

struct p4_semaphore_cancel_entry_snapshot
{
    struct SignalSemaphore *semaphore;
    uint32_t entry_count;
    uint32_t state;
    uint32_t committed;
    uint32_t running_harts;
};

/* Caller holds the registry gate and an exclusive cancellation claim for the
 * exact target/generation. Output is mapped, aligned, independent storage.
 * Resolve each ordinal again under this gate: no record, operation, request
 * or ledger entry pointer survives unlock. The copied semaphore identity is
 * caller-owned storage whose lifetime must cover the operation; no semaphore
 * or request fields are accessed here. Its queue gate is acquired separately,
 * then the mutation binding revalidates target/generation/ordinal under the
 * registry gate in semaphore-before-registry order. No nested sem gates.
 * Returns1 snapshot,0 stale/ineligible/out-of-range with unchanged output,
 * -1 malformed/aliased state with unchanged output. This does not claim,
 * finish or release a scope, install a worker, or supply retry wake semantics.
 * Committed entries must never be rolled back by the consumer. */
int p4_semaphore_cancel_describe_locked(struct p4_task_registry *,
    struct p4_semaphore_ledger *, struct SemaphoreRequest *, struct Task *,
    uint32_t generation, uint32_t ordinal,
    struct p4_semaphore_cancel_entry_snapshot *);
#endif
