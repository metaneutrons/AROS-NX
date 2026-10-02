/* Caller-serialized bounded ledger for P4 SMP semaphore operations.
 * This is private kernel state, not an Exec/public ABI. */
#ifndef P4_SEMAPHORE_LEDGER_H
#define P4_SEMAPHORE_LEDGER_H

#include <stdint.h>

#define P4_SEMAPHORE_LEDGER_LIMIT 20000u

enum p4_semaphore_ledger_entry_state
{
    P4_SEMAPHORE_LEDGER_ENTRY_FREE = 0u,
    P4_SEMAPHORE_LEDGER_ENTRY_UNVISITED = 1u,
    P4_SEMAPHORE_LEDGER_ENTRY_QUEUED = 2u,
    P4_SEMAPHORE_LEDGER_ENTRY_OWNED = 3u,
    P4_SEMAPHORE_LEDGER_ENTRY_RELEASED = 4u,
    P4_SEMAPHORE_LEDGER_ENTRY_WITHDRAWN = 5u
};

struct p4_semaphore_ledger_operation;
struct p4_semaphore_ledger_entry;

/* These objects live in caller-provided boot storage. Pointer fields contain
 * only opaque identities or links into those fixed arrays. */
struct p4_semaphore_ledger_entry
{
    struct p4_semaphore_ledger_operation *operation;
    struct p4_semaphore_ledger_entry *operation_next;
    struct p4_semaphore_ledger_entry *request_next;
    const void *semaphore;
    const void *request;
    uint32_t state;
};

struct p4_semaphore_ledger_operation
{
    struct p4_semaphore_ledger_operation *active_next;
    struct p4_semaphore_ledger_entry *first;
    const void *owner;
    uint32_t generation;
    uint32_t entry_count;
    uint32_t construction_closed;
    uint32_t committed;
    uint32_t cancelled;
    uint32_t active;
};

struct p4_semaphore_ledger
{
    struct p4_semaphore_ledger_operation *operations_head;
    struct p4_semaphore_ledger_entry *requests_head;
    struct p4_semaphore_ledger_operation *operations;
    struct p4_semaphore_ledger_entry *entries;
    uint32_t operation_capacity;
    uint32_t entry_capacity;
    uint32_t active_operations;
    uint32_t active_entries;
    uint32_t request_count;
    uint32_t initialized;
};

/* Every non-init operation requires the caller's shared lifetime gate.
 * Semaphore state transitions additionally hold that semaphore's leaf gate.
 * These helpers never lock, allocate/free, call out, or dereference Task,
 * semaphore, or request identities. Return 1 on success, 0 on refusal, and
 * -1 on malformed ledger/arguments; failed calls do not mutate state or
 * outputs. Capacities must be in [1, P4_SEMAPHORE_LEDGER_LIMIT]. */
int p4_semaphore_ledger_init(
    struct p4_semaphore_ledger *ledger,
    struct p4_semaphore_ledger_operation *operations,
    uint32_t operation_capacity,
    struct p4_semaphore_ledger_entry *entries,
    uint32_t entry_capacity);
/* Read-only full-shape predicate under the same caller-held gate. This is
 * used before a joint registry transition that has no specific entry index. */
int p4_semaphore_ledger_validate(const struct p4_semaphore_ledger *);

/* Reserve one operation and entry_count UNVISITED entries atomically.
 * Owner/generation pairs are unique while active. out_operation must be
 * distinct from the ledger and both backing arrays. */
int p4_semaphore_ledger_reserve(
    struct p4_semaphore_ledger *ledger,
    const void *owner,
    uint32_t generation,
    uint32_t entry_count,
    struct p4_semaphore_ledger_operation **out_operation);

/* Configure each reserved entry exactly once, then close construction before
 * acquisition/queue transitions. Request identities are unique across all
 * active entries; semaphore identities need not be unique in this model. */
int p4_semaphore_ledger_set_entry(
    struct p4_semaphore_ledger *ledger,
    struct p4_semaphore_ledger_operation *operation,
    uint32_t index,
    const void *semaphore,
    const void *request);
int p4_semaphore_ledger_close(
    struct p4_semaphore_ledger *ledger,
    struct p4_semaphore_ledger_operation *operation);

/* The returned entry is valid only while the caller still holds the ledger
 * gate and the independent operation-scope pin. Never retain it past either.
 * The output must be distinct from the ledger and backing arrays. */
int p4_semaphore_ledger_entry_at(
    struct p4_semaphore_ledger *ledger,
    struct p4_semaphore_ledger_operation *operation,
    uint32_t index,
    struct p4_semaphore_ledger_entry **out_entry);

/* UNVISITED->OWNED is immediate acquisition; UNVISITED->QUEUED publishes a
 * waiter only after its request node has been inserted by the caller.
 * Grant follows dequeue and is refused after cancellation. Withdraw is only
 * for a cancelled operation and follows caller unlink of that queued node.
 * RELEASED->UNVISITED permits a live, uncancelled operation to retry an
 * acquisition without changing sem count. */
int p4_semaphore_ledger_acquire(
    struct p4_semaphore_ledger *ledger,
    struct p4_semaphore_ledger_operation *operation,
    uint32_t index);
int p4_semaphore_ledger_queue(
    struct p4_semaphore_ledger *ledger,
    struct p4_semaphore_ledger_operation *operation,
    uint32_t index);
int p4_semaphore_ledger_grant(
    struct p4_semaphore_ledger *ledger,
    struct p4_semaphore_ledger_operation *operation,
    uint32_t index);
int p4_semaphore_ledger_withdraw(
    struct p4_semaphore_ledger *ledger,
    struct p4_semaphore_ledger_operation *operation,
    uint32_t index);
int p4_semaphore_ledger_restart_released(
    struct p4_semaphore_ledger *ledger,
    struct p4_semaphore_ledger_operation *operation,
    uint32_t index);

/* Commit requires a closed, live operation with every entry OWNED and all
 * request nodes already unlinked by the caller. It makes those units
 * completed acquisitions; later cancellation only reclaims bookkeeping and
 * never releases the units. Cancellation is sticky. Rollback changes an
 * OWNED ledger entry to RELEASED only for an uncommitted cancelled op; the
 * caller updates semaphore NestCount under its semaphore gate. */
int p4_semaphore_ledger_commit(
    struct p4_semaphore_ledger *ledger,
    struct p4_semaphore_ledger_operation *operation);

/* Terminal successful acquisition: after the caller unlinks every real
 * request node and proves no notification retains entry pointers, validate a
 * closed, live, uncommitted operation with every entry OWNED, then atomically
 * remove its ledger bookkeeping. This does not change semaphore NestCount or
 * release any acquired units. */
int p4_semaphore_ledger_complete(
    struct p4_semaphore_ledger *ledger,
    struct p4_semaphore_ledger_operation *operation);

int p4_semaphore_ledger_cancel(
    struct p4_semaphore_ledger *ledger,
    struct p4_semaphore_ledger_operation *operation);
int p4_semaphore_ledger_rollback_owned(
    struct p4_semaphore_ledger *ledger,
    struct p4_semaphore_ledger_operation *operation,
    uint32_t index);

/* Resolve an anonymous Release against the semaphore's supplied total nest:
 * completed units are consumed first. On success, out_selected_pending is 1
 * only if one matching uncommitted OWNED entry was marked RELEASED; 0 means
 * a completed unit was available. The output is written only on success. */
int p4_semaphore_ledger_release(
    struct p4_semaphore_ledger *ledger,
    const void *semaphore,
    uint32_t total_nest,
    uint32_t *out_selected_pending);

/* Reuse operation/entry slots only after the caller has independently proved
 * all intrusive request nodes unlinked and no notification retains ledger
 * pointers. A committed op can be reclaimed once its queue refs are gone; an
 * uncommitted op must be cancelled and have no QUEUED/OWNED entries. */
int p4_semaphore_ledger_reclaim(
    struct p4_semaphore_ledger *ledger,
    struct p4_semaphore_ledger_operation *operation);

#endif
