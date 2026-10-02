/*
 * Caller-serialized registry for Task lifetime records. The gated P4 SMP
 * kernel selects its pin/admission/creation binding; removal and full
 * scheduler integration remain incomplete. Normal Exec does not select it.
 */
#ifndef P4_TASK_REGISTRY_H
#define P4_TASK_REGISTRY_H

#include <stdint.h>

#include "p4_task_lifetime.h"
#include "p4_creation_channel.h"

#define P4_TASK_REGISTRY_LIMIT 20000u

#define P4_TASK_WAIT_SINGLE 1u
#define P4_TASK_WAIT_LIST   2u
#define P4_TASK_WAIT_OPERATION 3u

struct Task;
struct p4_semaphore_ledger;
struct p4_semaphore_ledger_operation;
struct p4_remove_bank;
struct p4_remove_handle;

struct p4_task_wait_operation
{
    struct p4_semaphore_ledger_operation *operation;
    uint32_t generation;
};

/* Private waiter-kind indices. The registry stores these pointers opaquely;
 * semaphore queues and caller-owned LIST storage are arbitrated elsewhere.
 * OPERATION uses list_index as the opaque boot-arena operation identity,
 * never as a caller-owned Exec List. Cancellation must classify kind first. */
struct p4_task_wait_scope
{
    uint32_t generation;
    uint32_t active;
    uint32_t kind;
    uint32_t queued_count;
    uint32_t construction_closed;
    uint32_t cancel_requested;
    uint32_t cancel_claimed;
    const void *semaphore_index;
    const void *request_index;
    const void *list_index;
};

/* Distinct caller storage filled by claim_next_wait_cancel_locked(). */
struct p4_task_wait_cancel_snapshot
{
    struct Task *identity;
    uint32_t generation;
    uint32_t kind;
    uint32_t queued_count;
    uint32_t construction_closed;
    uint32_t cancel_requested;
    const void *semaphore_index;
    const void *request_index;
    const void *list_index;
};

/* Durable reservation for a semaphore notification batch. This snapshot is
 * caller-owned, mapped storage, distinct from the registry and every linked
 * record. Its identity remains safe to use only while the matching inflight
 * reservation is held; Finish consumes that reservation after notification. */
struct p4_semaphore_notification
{
    struct Task *identity;
    uint32_t count;
    uint32_t generation;
};

struct p4_task_registry_record
{
    struct p4_task_registry_record *next;
    struct Task *identity;
    /* TaskResource bootstrap debt only. This raw marker grants no Task
     * storage authority; it is set only under a live constructor reference. */
    uint32_t taskresource_unobserved;
    /* One counted pin belongs to the deferred ServicePort cleanup message. */
    uint32_t service_pending;
    /* Exactly-once reservation of the cleanup-ready notification. */
    uint32_t service_queued;
    /* Cleanup worker owns an inflight retained record until finish(). */
    uint32_t service_inflight;
    /* Task/context/MemLists are already freed; only this independently
     * allocated retained record may be read during a completion retry. */
    uint32_t service_postfree;
    /* SMP only: globally issued once on successful service retirement,
     * before freeing. Retained through cleanup/retry; never recycled. */
    uint32_t service_generation;
    /* Reserved semaphore notification pins, split into claimable and
     * currently-consumed batches; registry validation includes both counts
     * in the pin floor. A pending generation is published once with its first
     * batch and remains stable while that batch accumulates. */
    uint32_t semaphore_pending;
    uint32_t semaphore_pending_generation;
    uint32_t semaphore_inflight;
    uint32_t semaphore_inflight_generation;
    /* Permanent pin reserved for the trusted, non-retirable housekeeper. */
    uint32_t worker_protected;
    /* Designated construction pin on this Task, and one creator-record pin
     * per child under construction. Creator is an opaque identity, retained
     * by those pins; it is never inferred from mutable ETask parentage. */
    struct Task *construction_creator;
    uint32_t construction_generation;
    uint32_t construction_children;
    /* Opaque RUN-stack authority. Never dereference from another Task or
     * after retirement; these fields do not reserve a construction pin. */
    uintptr_t creation_frame;
    uint32_t creation_generation;
    uintptr_t creation_target;
    uint32_t creation_claimed;
    /* One Task-independent blocking semaphore-operation scope and pin. */
    struct p4_task_wait_scope wait_scope;
    struct p4_task_lifetime lifetime;
};

struct p4_task_registry
{
    struct p4_task_registry_record *head;
    uint32_t count;
    /* Monotonic batch identity, initialized to zero for a new registry.
     * Publishers refuse rather than wrapping; do not reset while records
     * from this registry may have notification snapshots in flight. */
    uint32_t semaphore_notification_serial;
    uint32_t construction_serial;
    uint32_t creation_channel_serial;
};

/*
 * Every *_locked operation requires the caller to hold the same lifetime
 * gate before lookup and through the transition. These helpers do not lock,
 * disable interrupts, allocate, call out, or dereference Task identities.
 * Linked records must remain allocated and unmodified by other code until
 * claim_reclaim_locked() or claim_service_locked() returns them detached, or
 * until finish_retained_service_locked() removes a retained in-flight record.
 * The caller frees a detached record only after leaving the gate and must not
 * pass it to these helpers again.
 *
 * register_locked() accepts only a quiescent, unpublished record (all fields
 * zero/null) and a non-null identity. The identity may be compared as a raw
 * pointer; no Task fields are read. A successful registration initializes the
 * lifetime state. Retiring keeps the record registered so delayed notification
 * unpins and running-context handoff can complete before reclamation.
 * init() is only for an unpublished registry; it does not detach active records.
 */
void p4_task_registry_init(struct p4_task_registry *registry);
/* Read-only bounded shape/state validation under the caller's registry gate.
 * Does not grant Task storage authority or dereference any identity. */
int p4_task_registry_validate_locked(struct p4_task_registry *registry);
int p4_task_registry_register_locked(
    struct p4_task_registry *registry,
    struct p4_task_registry_record *record,
    struct Task *identity);
/* Private constructor binding must call this under the shared lifetime gate
 * after authenticating the TS_ADDED continuation and sampling the resource
 * binding. The leaf cannot authenticate either fact and never dereferences
 * identity. It admits only an exact live record with either an ordinary
 * notification pin above the complete durable pin floor, or the designated
 * construction_creator reference. mark_unobserved is 0 to admit without
 * changing debt or 1 to idempotently set the debt marker. This leaf remains
 * unselected until bootstrap inventory, debt clearing and base publication
 * are installed under that same gate. Returns 1 admitted, 0 absent/ineligible,
 * -1 malformed input or registry.
 */
int p4_task_registry_taskresource_observer_locked(
    struct p4_task_registry *registry, struct Task *identity,
    uint32_t mark_unobserved);
#if defined(__AROSEXEC_SMP__) && defined(__AROSPLATFORM_SMP__)
/* Trusted private constructor scopes only. Frame is mapped, aligned caller
 * RUN-stack storage, disjoint from registry/records/other active frames;
 * caller must validate the complete stack extent before invoking these.
 * Begin/End read frame only AFTER live sole RUN admission. End is LIFO and
 * preserves value for caller cleanup. Nested previous-frame metadata is
 * trusted local state, not a user-provided capability. -1 malformed, 0
 * stale/ineligible/exhausted, 1 commit; failure changes nothing.
 * Resolver reads no frame or Task. Exact nonwrapping generation, target and
 * value address must match; one successful resolve consumes invocation
 * authority. Nested scopes restore the previous consumed/unconsumed state.
 * Returns zero unless this caller is live sole RUN.
 * A returned address must not escape the scoped constructor continuation.
 * Retirement may retain opaque metadata until cleanup, but must never resume
 * that stack or read its frames. These leaves do not implement that policy.
 */
int p4_task_registry_creation_begin_locked(struct p4_task_registry *,
    struct Task *, uint32_t hart, struct Task *target,
    struct p4_creation_channel *);
int p4_task_registry_creation_end_locked(struct p4_task_registry *,
    struct Task *, uint32_t hart, uint32_t expected_generation,
    struct p4_creation_channel *);
uintptr_t p4_task_registry_creation_resolve_locked(struct p4_task_registry *,
    struct Task *, uint32_t hart, struct Task *target,
    uint32_t generation, uintptr_t tag_value);
/* Atomic fresh registration with designated target/creator pins. Creator
 * must be the caller's live sole RUN owner on hart. This is not a Ready
 * publication or removal/cancellation API. No Task identity is dereferenced.
 * Output must be mapped, aligned caller-owned storage distinct from registry,
 * every record and both Task allocations/contexts; caller's RUN stack is OK.
 * -1 malformed/alias, 0 ineligible/capacity, 1 committed; failure unchanged.
 * The nonwrapping generation identifies this construction until Finish.
 */
int p4_task_registry_register_constructing_locked(
    struct p4_task_registry *, struct p4_task_registry_record *,
    struct Task *, struct Task *, uint32_t hart, uint32_t *out_generation);
/* Creator's post-hook release of BOTH designated pins. Exact generation and
 * live creator RUN required; caller's last Task access must precede this.
 * Retirement of target is permitted; this releases pins, not final cleanup.
 * A retired creator requires a separate no-save/orphan-worker recovery path,
 * not a normal Finish. Return -1 malformed, 0 stale/ineligible, 1 release.
 */
int p4_task_registry_finish_construction_locked(
    struct p4_task_registry *, struct Task *, struct Task *, uint32_t,
    uint32_t hart);
/* Trusted orphan-worker's final reference release, NOT cancellation itself.
 * Exact target/creator/generation, both service-retired, no RUN on either.
 * Caller must have suppressed target Ready publication, completed queue and
 * TaskRes/topology cancellation, and ended ALL its target/creator accesses
 * before this call. No Task/context pointer may be used after success.
 * Other designated/ordinary pins remain intact; the service binding must
 * reserve both identities' cleanup/resource wakes under the same gate.
 * -1 malformed, 0 stale/not drained, 1 releases both construction references.
 * This is unbound: no worker scanning, cancellation or wake policy installed.
 */
int p4_task_registry_finish_orphan_construction_locked(
    struct p4_task_registry *, struct Task *, struct Task *, uint32_t);
#endif
int p4_task_registry_pin_locked(
    struct p4_task_registry *registry, struct Task *identity);
int p4_task_registry_unpin_locked(
    struct p4_task_registry *registry, struct Task *identity);
/* Publish one blocking wait operation before its first queue insertion.
 * SINGLE requires non-NULL semaphore_index/request_index and NULL list_index;
 * LIST requires non-NULL list_index and NULL single indices. The output must
 * be distinct caller storage. A successful begin owns one designated pin. */
int p4_task_registry_wait_begin_locked(
    struct p4_task_registry *registry,
    struct Task *identity,
    uint32_t kind,
    const void *semaphore_index,
    const void *request_index,
    const void *list_index,
    uint32_t *out_generation);
#if defined(__AROSEXEC_SMP__) && defined(__AROSPLATFORM_SMP__)
/* Atomically reserve all arena entries and publish the generation/scope pin
 * under the same caller-held registry gate, before any semaphore mutation.
 * Registry, its records, arena and output storage must be disjoint; checked
 * failures leave all state and output unchanged. No allocator or callback.
 * This does not yet bind queue transitions or retirement to cancellation.
 * Legacy queue/readiness/finish helpers refuse OPERATION scopes: only fused
 * ledger transitions may change its counts or discharge its designated pin. */
int p4_task_registry_wait_reserve_operation_locked(
    struct p4_task_registry *, struct p4_semaphore_ledger *, struct Task *,
    uint32_t entry_count, struct p4_task_wait_operation *);
/* Checked internal resolution: -1 malformed storage/topology or matching
 * scope-to-operation association, 0 absent/stale/ineligible scope, 1 success.
 * Caller holds the registry gate throughout every use of borrowed pointers.
 * Outputs are independent, non-overlapping local storage and unchanged on
 * non-success. Neither pointer may escape to public callers or across unlock. */
int p4_task_registry_operation_resolve_locked(
    struct p4_task_registry *, struct p4_semaphore_ledger *, struct Task *,
    uint32_t generation, struct p4_task_registry_record **out_record,
    struct p4_semaphore_ledger_operation **out_operation);
/* Read-only registry shape and ledger-storage separation preflight. Checks
 * ledger header separation before reading its fields; does not inspect ledger
 * backing-array contents. Caller holds registry gate and validates the ledger
 * contents only after this succeeds. */
int p4_task_registry_ledger_disjoint_locked(struct p4_task_registry *,
    const struct p4_semaphore_ledger *);
/* All functions below use the SAME shared gate as reservation. Set/close
 * precede acquisition; real queue unlink and notification independence must
 * be proved by the caller before either terminal finish. */
int p4_task_registry_wait_operation_set_entry_locked(
    struct p4_task_registry *, struct p4_semaphore_ledger *, struct Task *,
    uint32_t generation, uint32_t index, const void *, const void *);
int p4_task_registry_wait_operation_close_locked(
    struct p4_task_registry *, struct p4_semaphore_ledger *, struct Task *,
    uint32_t generation);
int p4_task_registry_wait_operation_complete_locked(
    struct p4_task_registry *, struct p4_semaphore_ledger *, struct Task *,
    uint32_t generation);
int p4_task_registry_wait_operation_finish_cancel_locked(
    struct p4_task_registry *, struct p4_semaphore_ledger *, struct Task *,
    uint32_t generation);
/* For an OPERATION, publish ledger cancellation and Task retirement together.
 * for_service=1 additionally publishes its designated ServicePort pin.
 * Legacy retire entrypoints refuse OPERATION to prevent bypassing this path. */
int p4_task_registry_retire_with_ledger_locked(
    struct p4_task_registry *, struct p4_semaphore_ledger *, struct Task *,
    uint32_t for_service);
/* Kind-aware consumers must validate the opaque operation after this claim.
 * Unlike the legacy helper this rejects snapshot overlap with arena storage. */
int p4_task_registry_claim_next_wait_cancel_with_ledger_locked(
    struct p4_task_registry *, struct p4_semaphore_ledger *,
    struct p4_task_wait_cancel_snapshot *);
int p4_task_registry_claim_wait_cancel_after_with_ledger_locked(
    struct p4_task_registry *, struct p4_semaphore_ledger *, uintptr_t,
    struct p4_task_wait_cancel_snapshot *);
/* Operation-only consumer: skip legacy SINGLE/LIST without claiming or
 * changing their sticky cancellation state. Arena separation and the selected
 * record/operation owner, generation, cancellation, closure and queued count
 * cross-invariants are checked before claim or output mutation. */
int p4_task_registry_claim_wait_operation_cancel_after_with_ledger_locked(
    struct p4_task_registry *, struct p4_semaphore_ledger *, uintptr_t,
    struct p4_task_wait_cancel_snapshot *);
#endif
int p4_task_registry_wait_add_request_locked(
    struct p4_task_registry *registry,
    struct Task *identity,
    uint32_t generation);
int p4_task_registry_wait_close_locked(
    struct p4_task_registry *registry,
    struct Task *identity,
    uint32_t generation);
/* Predicate only: callers must not infer readiness from a signal bit alone. */
int p4_task_registry_wait_ready_locked(
    struct p4_task_registry *registry,
    struct Task *identity,
    uint32_t generation);
/* Caller copies/dequeues request fields under the semaphore gate first;
 * this transition pins notification before publishing the decremented count.
 * Signal outside all gates, then release the ordinary notification pin. */
int p4_task_registry_wait_grant_request_locked(
    struct p4_task_registry *registry,
    struct Task *identity,
    uint32_t generation);
int p4_task_registry_wait_withdraw_request_locked(
    struct p4_task_registry *registry,
    struct Task *identity,
    uint32_t generation);
/* Drop the designated scope pin only after a live, uncancelled operation is
 * closed and has no queued requests. Generation is preserved against ABA. */
int p4_task_registry_wait_finish_locked(
    struct p4_task_registry *registry,
    struct Task *identity,
    uint32_t generation);
/* Read-only durable-work predicate for the cleanup wake producer. Returns
 * -1 for a malformed/missing identity, 0 when no cancellation can be claimed,
 * and 1 for a retired, active, unclaimed scope with queued work or no RUN
 * owner. It never dereferences the Task identity. */
int p4_task_registry_wait_cancel_ready_locked(
    struct p4_task_registry *registry, struct Task *identity);
/* Exclusive cancellation claim. On empty, a distinct valid output snapshot
 * is zeroed. Malformed/aliased inputs fail without mutation. */
int p4_task_registry_claim_next_wait_cancel_locked(
    struct p4_task_registry *registry,
    struct p4_task_wait_cancel_snapshot *out_snapshot);
/* Fair-pass building block: select the eligible identity with the smallest
 * raw address strictly above after_identity; never dereference that identity.
 * Start each pass at zero and retain only the scalar cursor across unlocks.
 * Even if a blocked claim is released, that scope is not selected again in
 * the same pass. Empty clears the snapshot, with no wrap to lower addresses.
 * Newly eligible lower addresses require a later pass/durable wake. This is
 * selection only, not cancellation progress, retry wake or worker routing.
 * OPERATION consumers must use the with_ledger variant for arena separation. */
int p4_task_registry_claim_wait_cancel_after_locked(
    struct p4_task_registry *registry, uintptr_t after_identity,
    struct p4_task_wait_cancel_snapshot *out_snapshot);
/* Internal selector used by the arena-aware operation-only wrapper. */
int p4_task_registry_claim_wait_operation_cancel_after_locked(
    struct p4_task_registry *registry, uintptr_t after_identity,
    struct p4_task_wait_cancel_snapshot *out_snapshot);
int p4_task_registry_wait_finish_cancel_locked(
    struct p4_task_registry *registry,
    struct Task *identity,
    uint32_t generation);
/* Release only the exclusive claim; cancellation remains sticky for retry. */
int p4_task_registry_wait_release_cancel_locked(
    struct p4_task_registry *registry,
    struct Task *identity,
    uint32_t generation);
/* Protect one live single-hart RUN identity with a permanent designated pin.
 * Transient pins and RUN handoff remain valid; retirement is refused. */
int p4_task_registry_protect_worker_locked(
    struct p4_task_registry *registry, struct Task *identity);
/* Returns 1 for the protected worker, 0 for a valid non-worker, -1 if missing
 * or if full registry validation fails. */
int p4_task_registry_is_worker_locked(
    struct p4_task_registry *registry, struct Task *identity);
int p4_task_registry_retire_locked(
    struct p4_task_registry *registry, struct Task *identity);
/* Atomically marks retired and acquires the designated ServiceTask pin.
 * Ordinary unpin cannot consume the final service reference. */
int p4_task_registry_retire_for_service_locked(
    struct p4_task_registry *registry, struct Task *identity);
int p4_task_registry_enter_running_locked(
    struct p4_task_registry *registry, struct Task *identity, uint32_t hart);
int p4_task_registry_leave_running_locked(
    struct p4_task_registry *registry, struct Task *identity, uint32_t hart);
struct p4_task_registry_record *p4_task_registry_claim_reclaim_locked(
    struct p4_task_registry *registry, struct Task *identity);
/* Atomically consumes the sole service pin and detaches a drained record.
 * Requires retired state, no other pins, and no running hart. Generic claim
 * refuses every record with service_pending set. */
struct p4_task_registry_record *p4_task_registry_claim_service_locked(
    struct p4_task_registry *registry, struct Task *identity);
/* Delivered-message consumer: in addition to full drain, requires the
 * exactly-once readiness reservation. The trusted claim above is for a
 * caller that already owns the designated service reference, not GetMsg. */
struct p4_task_registry_record *p4_task_registry_claim_queued_service_locked(
    struct p4_task_registry *registry, struct Task *identity);
/* Move one record's pending batch to its sole inflight slot. No pin count
 * changes: the reserved refs remain held until the exact snapshot is finished.
 * A record already inflight is skipped so a later record can be serviced.
 * Returns -1 for malformed/aliased input, 0 when no batch is claimable (and
 * clears a valid distinct output), or 1 with the exact moved batch. */
int p4_task_registry_claim_next_semaphore_notification_locked(
    struct p4_task_registry *registry,
    struct p4_semaphore_notification *out_snapshot);
/* Consume only an exact identity/count/generation match. Returns 1 on finish,
 * 0 for a stale/mismatched snapshot, and -1 for malformed/aliased input.
 * Failed calls do not change the registry or caller storage. */
int p4_task_registry_finish_semaphore_notification_locked(
    struct p4_task_registry *registry,
    const struct p4_semaphore_notification *snapshot);
/* Claim and detach the first durably queued service record. Validates the
 * complete bounded registry before scanning or mutation. Returns -1 for a
 * malformed registry/output or duplicate selected identity, 0 when empty,
 * and 1 with the exclusively owned detached record in *out_record. */
int p4_task_registry_claim_next_service_locked(
    struct p4_task_registry *registry,
    struct p4_task_registry_record **out_record);
/* Atomically consume the queued service reference while retaining the record
 * in the validated registry. The returned record stays linked, with its
 * identity preserved and service_inflight set, until finish_retained_service
 * removes it after cleanup. A distinct output pointer is required. Returns
 * -1 on invalid/aliased input, 0 when no queued work exists, and 1 on claim. */
int p4_task_registry_claim_next_retained_service_locked(
    struct p4_task_registry *registry,
    struct p4_task_registry_record **out_record);
/* Finish one retained cleanup claim. The raw pointer is compared against the
 * validated registry before any field is read. Returns 1 when detached and
 * 0 for unknown, duplicate, protected, pinned, running or malformed input. */
int p4_task_registry_finish_retained_service_locked(
    struct p4_task_registry *registry,
    struct p4_task_registry_record *raw_record);
#if defined(__AROSEXEC_SMP__) && defined(__AROSPLATFORM_SMP__)
/* Atomic external-removal admission. Caller holds the registry/bank gate;
 * a live retirement additionally requires an ordinary target pin, canonical
 * Task/queue locks and IRQ masking. requester must be the live, unprotected
 * RUN owner of hart (0/1), distinct from target. retire_live=0 permits only a
 * raw-identity join to an indexed service-retired target, with no Task reads;
 * retire_live=1 permits only new live retirement, never an implicit join.
 * All metadata/header/pool storage is mapped and stable. The trusted caller
 * supplies mapped handle storage disjoint from all target-owned allocations
 * and from requester Task-object/saved-context storage. A private automatic
 * slot on the requester's live RUN-owned stack is allowed. Opaque identities
 * do not describe allocation extents, so the leaf cannot validate that
 * precondition. Registry/bank/ledger aliases are checked here.
 * Returns1 ticket plus new service retirement,2 ticket joining existing
 * service ownership,0 absent/ineligible target or finite capacity refusal,
 * -1 malformed/aliased metadata domains. All refusals preserve state and output.
 * At most one requester_ref ticket per live requester (pending or ACKed):
 * duplicate admission refuses unchanged. Cancelled requester_ref0 slots may
 * be reused, with a new generation, only after the last-fallible transition.
 * Their ticket service ref is NOT target registry cleanup ownership. All
 * production service consumers must scan under this gate, not retain a bank
 * handle across unlock. With bank capacity at least the registry limit and
 * this sole production admission route, slot pressure cannot refuse a live
 * requester owning no ticket; nonwrapping generation exhaustion still refuses
 * before mutation and requires a terminal public policy.
 * Ticket publication and requester-interest cancellation are infallible after
 * the last-fallible retirement/ledger cancellation. Queue state remains the
 * outer caller's responsibility. Release its ordinary pin/locks before Wait;
 * no Task pointer may be used after the retained target finishes/detaches. */
int p4_task_registry_admit_removal_locked(struct p4_task_registry *,
    struct p4_semaphore_ledger *, struct p4_remove_bank *, struct Task *requester,
    struct Task *target, uint32_t hart, uint32_t retire_live,
    struct p4_remove_handle *);
/* After the last target/context/free access, atomically ACK every outstanding
 * target ticket, reserve generation-tagged SIGF_SINGLE recipient pins, and
 * detach the retained target record. Caller holds the lifetime/bank gate and
 * excludes joins through detach; header/pool/records must be mapped, disjoint
 * permanent storage. No Task dereference, allocation, callback or unlock.
 * Positive result is1 + notification count, 0 refuses non-finishable target or
 * pin/count headroom, -1 malformed/cross-object state. New pending batches use
 * the target token reserved before retirement, not a post-free allocation.
 * Every refusal
 * leaves both objects unchanged. The protected worker consumes durable batches
 * after unlock; callers latch/poke that worker for result>1. Capacity retry
 * policy and public admission remain separate required runtime bindings. */
int p4_task_registry_finish_removal_locked(struct p4_task_registry *,
    struct p4_remove_bank *, struct p4_task_registry_record *);
/* Only AFTER final Task/context/entry free. Validates raw membership before
 * setting postfree; retained claim remains indexed and cannot be reclaimed.
 * Returns1 success/already marked,0 invalid/nonfinishable unchanged. */
int p4_task_registry_defer_removal_finish_locked(struct p4_task_registry *,
    struct p4_task_registry_record *);
/* Nonwrapping address-ordered scan of validated postfree records. Does not
 * claim another Task or read Task storage. Distinct mapped output required;
 * returns1 borrowed retained record,0 empty,-1 malformed/aliased unchanged.
 * Caller keeps the gate through finish; no borrowed pointer crosses unlock. */
int p4_task_registry_next_postfree_after_locked(struct p4_task_registry *,
    uintptr_t after, struct p4_task_registry_record **);
#endif
/* Reserve, but do not enqueue or detach, one cleanup-ready notification.
 * Returns the raw identity only after the service pin is the sole reference
 * and the record has no Running owner. Duplicate reservations return NULL. */
struct Task *p4_task_registry_take_service_ready_locked(
    struct p4_task_registry *registry, struct Task *identity);
/* Read-only proof that removal owns its designated cleanup reference while
 * the indicated hart still holds the sole Running reference. */
int p4_task_registry_service_owner_running_locked(
    struct p4_task_registry *, struct Task *, uint32_t);
/* Live identity: zero means no RUN owner, hart+1 identifies the sole owner.
 * Negative means missing, retired or malformed. No Task dereference. */
int p4_task_registry_running_owner_locked(struct p4_task_registry *, struct Task *);

#endif
