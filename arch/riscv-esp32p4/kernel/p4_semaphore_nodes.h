/* Private real Exec request-node transactions, not public API routing. */
#ifndef P4_SEMAPHORE_NODES_H
#define P4_SEMAPHORE_NODES_H
#include <stdint.h>
#include "p4_task_registry.h"
#include "p4_semaphore_ledger.h"
struct SignalSemaphore;
struct SemaphoreRequest;

/* Caller owns initialized semaphore/pool storage, holds its semaphore gate
 * THEN the lifetime/registry gate for the entire call. The pool has exactly
 * ledger->entry_capacity nodes; absolute ledger slot, not operation ordinal,
 * selects its node. No callback, allocation, Task dereference or nested lock.
 * Return 1 success, 0 lifecycle refusal, -1 malformed state. Failed calls
 * leave every object unchanged. Acquire returns 2 for a queued request.
 * Configure precedes Close; all other calls require closed construction.
 * No retained notifier may refer to a recycled pool node. This implementation
 * does not install public Obtain/Release/Procure/Vacate or cleanup consumers. */
int p4_semaphore_nodes_configure_locked(struct p4_task_registry *,
    struct p4_semaphore_ledger *, struct SemaphoreRequest *, struct Task *,
    uint32_t generation, uint32_t ordinal, struct SignalSemaphore *,
    uint32_t shared);
int p4_semaphore_nodes_acquire_locked(struct p4_task_registry *,
    struct p4_semaphore_ledger *, struct SemaphoreRequest *, struct Task *,
    uint32_t generation, uint32_t ordinal, struct SignalSemaphore *);
/* Cancelled, exclusively claimed scope only. Withdraw unlinks a real QUEUED
 * node and may run while target RUN remains held. Rollback releases one
 * uncommitted OWNED unit only after RUN0, preventing the retired continuation
 * from accessing an already-released unit. Terminal FinishCancel also
 * independently requires RUN0. Empty-owner handoff and
 * granting subsequent waiters belong to the coordinated Release protocol. */
int p4_semaphore_nodes_withdraw_locked(struct p4_task_registry *,
    struct p4_semaphore_ledger *, struct SemaphoreRequest *, struct Task *,
    uint32_t generation, uint32_t ordinal, struct SignalSemaphore *);
int p4_semaphore_nodes_rollback_locked(struct p4_task_registry *,
    struct p4_semaphore_ledger *, struct SemaphoreRequest *, struct Task *,
    uint32_t generation, uint32_t ordinal, struct SignalSemaphore *);
/* Reserve and grant a complete Task-only handoff cohort while NestCount0.
 * Shared head selects every shared waiter, including beyond exclusive nodes;
 * exclusive head selects only itself. Unknown/non-pool/Procure nodes refuse
 * before any mutation. targets is separate durable mapped caller storage,
 * capacity <= entry_capacity; all alias/overflow preflights precede commit.
 * Returns positive copied-target count (one ordinary notification pin each),
 * 0 lifecycle/capacity refusal or empty queue, -1 malformed state. QueueCount
 * is unchanged by grants. After unlock, a trusted non-retirable consumer must
 * Signal and Unpin each copied identity, without retaining any request/entry
 * pointer. That durable consumer/public Release binding is not installed. */
int p4_semaphore_nodes_grant_cohort_locked(struct p4_task_registry *,
    struct p4_semaphore_ledger *, struct SemaphoreRequest *,
    struct SignalSemaphore *, struct Task **targets, uint32_t capacity);
/* Anonymous actual-unit release, including atomic last-unit cohort handoff.
 * Same gate/storage contract. Returns 1 + copied notification-target count,
 * 0 if last-unit handoff cannot be reserved, -1 malformed state. Failed calls
 * leave ownership, ledger, queue, pins and outputs unchanged. Completed units
 * are consumed before uncommitted OWNED entries; a selected pending entry is
 * marked RELEASED and its live operation must retry before Complete.
 * Does not require calling Task == owner (cross-Task Release is supported).
 * Non-last release does not use targets; last release requires valid distinct
 * durable targets/capacity even if its queue is empty. Last-unit unknown or
 * Procure queues remain unsupported and refuse before release. No public
 * ReleaseSemaphore or out-of-gate notification consumer is installed. */
int p4_semaphore_nodes_release_locked(struct p4_task_registry *,
    struct p4_semaphore_ledger *, struct SemaphoreRequest *,
    struct SignalSemaphore *, struct Task **targets, uint32_t capacity);
/* Durable variants publish each grant's notification reservation directly in
 * its permanent registry record with its matching pin. No caller target array
 * or request pointer owns the out-of-gate obligation. Preflight includes all
 * per-recipient counters and globally unique batch serial capacity; refusal
 * precedes release/grant mutation. Return conventions match the APIs above.
 * The caller must set/poke the protected service worker's durable work latch
 * under this same registry gate, with delivery only after both gates unlock.
 * These leaves do not install that wake/consumer or public routing themselves. */
int p4_semaphore_nodes_grant_durable_locked(struct p4_task_registry *,
    struct p4_semaphore_ledger *, struct SemaphoreRequest *,
    struct SignalSemaphore *);
int p4_semaphore_nodes_release_durable_locked(struct p4_task_registry *,
    struct p4_semaphore_ledger *, struct SemaphoreRequest *,
    struct SignalSemaphore *);
/* Cancelled, exclusively claimed, uncommitted OWNED entry after RUN0 only.
 * Unlike the bookkeeping-only rollback above, the last-unit transition
 * preflights and commits the complete durable cohort before dropping either
 * gate. Returns 1 + published notifications, 0 unchanged lifecycle/capacity
 * refusal, -1 unchanged malformed state. A cancelled/retired queued peer must
 * first be withdrawn by its own cancellation claim; do not spin on this scope
 * and thereby prevent that peer's withdrawal. Caller latches worker work
 * under the registry gate and pokes only after the semaphore gate is released.
 * No cancellation worker or public caller is installed by this leaf. */
int p4_semaphore_nodes_rollback_durable_locked(struct p4_task_registry *,
    struct p4_semaphore_ledger *, struct SemaphoreRequest *, struct Task *,
    uint32_t generation, uint32_t ordinal, struct SignalSemaphore *);
#endif
