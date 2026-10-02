/* Actual Task-registry gate binding; selection awaits full lifecycle wiring. */
#ifndef P4_TASK_PINS_H
#define P4_TASK_PINS_H
#include "p4_task_registry.h"
struct ExecBase;
struct SignalSemaphore;
struct p4_semaphore_cancel_entry_snapshot;
#if defined(__AROSEXEC_SMP__) && defined(__AROSPLATFORM_SMP__)
struct p4_remove_bank;
/* Actual current Task/hart, normal RUN continuation, complete frame inside
 * its stack. Private constructor capability; not yet selected by platform
 * macros. Begin/End return 0 on retired/ineligible caller; caller must use
 * the completed constructor recovery/no-save policy before installation.
 * Resolve returns only a scoped address, never reads an unknown tag pointer.
 */
int krnP4CreationChannelBegin(struct Task *target,
    struct p4_creation_channel *, struct ExecBase *);
int krnP4CreationChannelEnd(uint32_t expected_generation,
    struct p4_creation_channel *, struct ExecBase *);
uint32_t *krnP4CreationChannelResolve(struct Task *target, uint32_t generation,
    uintptr_t, struct ExecBase *);
/* Bootstrap-only, raw MIE off/hart1 reset-held, before any registry publication
 * or Exec vectors. No Exec call/lock. Permanent bank storage
 * must remain mapped; all future bank transitions share the lifetime gate. */
int krnP4TaskPinsAttachRemovalBank(struct p4_remove_bank *);
/* Registry/bank admission binding. A new live retirement (retire_live=1)
 * requires caller target pin, canonical Task/queue locks and IRQ mask; raw
 * retained join (0) never reads Task storage. Handle output is private mapped
 * caller storage following the registry contract: no target-owned allocation
 * or requester Task-object/saved-context alias; its live stack slot is allowed.
 * Returns1 new retirement,2 retained join,0 refusal; corruption is
 * fatal. Latches cleanup/resource work but NEVER pokes under outer locks.
 * Caller must poke cleanup and a copied RUN owner after releasing outer
 * locks; drop its ordinary target pin before waiting on the scalar handle. */
int krnP4TaskRecordAdmitRemoval(struct Task *requester, struct Task *target,
    uint32_t hart, uint32_t retire_live, struct p4_remove_handle *, struct ExecBase *);
/* Normal Task continuation only; caller owns the indicated RUN reference.
 * Poll returns0 pending,1 ACK complete,2 requester retired (its ticket interest
 * is already cancelled; perform no-save retirement transfer, never return to
 * user code). No Task/storage/slot pointers escape. Malformed or stale handles
 * on a live requester are fatal. Drop consumes only its ACKed requester ref:
 * returns1 success,0 retired requester. Call outside all outer locks; capacity
 * release reserves resource retry wake under the gate and pokes after unlock.
 * Neither function clears a signal bit or implements Wait/context policy.
 * Re-query the current hart for every invocation: a Wait may migrate tasks. */
int krnP4TaskRemovalPoll(struct Task *, uint32_t hart,
    struct p4_remove_handle, struct ExecBase *);
int krnP4TaskRemovalDrop(struct Task *, uint32_t hart,
    struct p4_remove_handle, struct ExecBase *);
/* Operation bindings return1 on success,0 on refusal; malformed leaf state
 * is fatal while the gate/IRQ mask is still held, never a truthy -1 result.
 * Reserve all entries and publish the scope pin before semaphore mutation.
 * Only this gate binding may publish a boot-arena OPERATION scope. */
int krnP4SemaphoreOperationReserve(struct Task *, uint32_t,
    struct p4_task_wait_operation *, struct ExecBase *);
/* Real reserved-node transitions: caller holds the initialized semaphore
 * gate before this registry leaf gate and owns its current RUN continuation.
 * Configure uses the permanent boot request selected by absolute ledger slot,
 * before Close. Acquire follows Close: 1 immediate, 2 queued, 0 refusal.
 * No pool/entry/request pointer escapes, no poke or callout under either gate.
 * Semaphore storage lifetime remains a separate caller obligation. */
int krnP4SemaphoreOperationConfigure(struct Task *, uint32_t, uint32_t,
    struct SignalSemaphore *, uint32_t shared, struct ExecBase *);
int krnP4SemaphoreOperationAcquire(struct Task *, uint32_t, uint32_t,
    struct SignalSemaphore *, struct ExecBase *);
int krnP4SemaphoreOperationSetEntry(struct Task *, uint32_t, uint32_t,
    const void *, const void *, struct ExecBase *);
int krnP4SemaphoreOperationClose(struct Task *, uint32_t, struct ExecBase *);
int krnP4SemaphoreOperationComplete(struct Task *, uint32_t, struct ExecBase *);
int krnP4SemaphoreOperationFinishCancel(struct Task *, uint32_t,
    struct ExecBase *);
/* Caller holds the initialized semaphore gate; these acquire only the leaf
 * registry gate and use the permanent boot arena. A positive durable Release
 * result is 1 + notification count; Grant returns the count. Work is latched
 * before registry unlock, but no poke/callout occurs under the outer gate.
 * After semaphore unlock, caller must krnP4CleanupPoke for a nonzero count. */
int krnP4SemaphoreReleaseDurable(struct SignalSemaphore *, struct ExecBase *);
int krnP4SemaphoreGrantDurable(struct SignalSemaphore *, struct ExecBase *);
/* Protected, armed service worker with a live RUN continuation only; caller
 * holds this initialized semaphore's gate before the registry leaf gate.
 * The target's exact generation/ordinal must have an exclusive cancellation
 * claim. Withdraw returns1 for progress; rollback returns1+notifications.
 * Clean refusal0 changes no unit/queue/pin; malformed state fails closed.
 * Positive progress latches work under the registry gate, including retries
 * for another cancelled owner unblocked by withdrawal. No poke under the
 * outer gate: caller must krnP4CleanupPoke after unlocking on positive result.
 * These bindings do not install fair cancellation scanning or its worker. */
int krnP4SemaphoreCancelWithdraw(struct Task *worker, struct Task *target,
    uint32_t generation, uint32_t ordinal, struct SignalSemaphore *,
    struct ExecBase *);
int krnP4SemaphoreCancelRollbackDurable(struct Task *worker,
    struct Task *target, uint32_t generation, uint32_t ordinal,
    struct SignalSemaphore *, struct ExecBase *);
/* Protected armed RUN worker's own continuation, no outer semaphore gate.
 * Claim selects OPERATION only (legacy SINGLE/LIST stay untouched), uses
 * non-wrapping address ordering and clears all borrowed legacy
 * index fields before unlock. Output is mapped/aligned independent worker
 * storage (including disjointness from semaphores); arena/pool aliases fail
 * closed. Describe uses the exact permanent boot pool and returns copied
 * entry metadata only; separately owned semaphore storage must remain live.
 * ReleaseClaim is quiet for an already-RUN0 blocked operation, but latches
 * and pokes after unlock when RUN drained since observed_running_harts was
 * captured while claimed. This prevents the claimed-RUN-drain lost wake.
 * It does NOT supply retry wakes after another Task's pin/headroom changes,
 * a fair worker, legacy-scope cancellation, or public routing. Those remain
 * mandatory before installing the actual operation cancellation consumer. */
int krnP4SemaphoreCancelClaimAfter(struct Task *worker, uintptr_t,
    struct p4_task_wait_cancel_snapshot *, struct ExecBase *);
int krnP4SemaphoreCancelDescribe(struct Task *worker, struct Task *target,
    uint32_t generation, uint32_t ordinal,
    struct p4_semaphore_cancel_entry_snapshot *, struct ExecBase *);
int krnP4SemaphoreCancelReleaseClaim(struct Task *worker, struct Task *target,
    uint32_t generation, uint32_t observed_running_harts, struct ExecBase *);
/* Protected service worker's live continuation only. Snapshot carries a
 * designated durable lifetime reservation, independent of request reuse.
 * Claim/finish never dereference Task, nor retain a registry record pointer.
 * Deliver using existing ownership, not public Signal's extra pin. */
int krnP4SemaphoreNotificationClaim(struct Task *worker,
    struct p4_semaphore_notification *, struct ExecBase *);
void krnP4SemaphoreNotificationFinish(struct Task *worker,
    const struct p4_semaphore_notification *, struct ExecBase *);
#endif

/* Snapshot handed to the asynchronous cleanup-wake transport. The consumer
 * must take it under the binding gate and deliver outside outer locks; it must
 * not inspect Task. The permanent worker pin keeps the identity allocated. */
struct p4_cleanup_wake
{
    struct Task *worker;
    uint32_t mask;
};

/* Once, after software atomics prepare, MIE clear and hart1 reset-held. */
int krnP4TaskPinsPrepare(void);
/* Bootstrap cutover only: caller keeps IRQs masked and hart1 reset-held
 * through inventory publication. Acquires the lifetime gate; refuses malformed,
 * retired/claimed records, construction/channel scopes and ordinary pins.
 * NewAddTask holds an ordinary pin across observer lookup through Ready.
 * No identity is dereferenced or escapes; no allocation.
 * A transient constructor refusal is not a runtime recovery/liveness policy. */
int krnP4TaskResourceBootstrapQuiescent(struct ExecBase *);
/* Trusted boot-only callback; caller independently proves hardware isolation
 * and keeps Disable held. Callback cannot allocate, enable IRQs, acquire this
 * gate recursively, invoke hooks or retain registry/Task snapshot pointers.
 * Returns 0 refusal, 1 committed, 2 capacity retry without mutation. Complete
 * validation must precede every mutation; there is no rollback here. */
int krnP4TaskResourceBootstrapApply(int (*)(void *), void *, struct ExecBase *);
/* Private constructor observer admission, after staged ETask publication and
 * while its ordinary/designated construction reference is held. Output is
 * mapped caller-owned storage, not target-owned storage. Acquire-loads the
 * direct binding and records a NULL observation under the same lifetime gate.
 * No Task dereference/callback/allocation; 0 leaves output unchanged. This
 * foundation is not selected until bootstrap inventories/clears the debt and
 * publishes the binding under that gate. The marker itself is not a pin. */
struct Library;
int krnP4TaskResourceObserve(struct Task *, struct Library **, struct ExecBase *);
/* Once from the service Task's live RUN continuation. Acquires a permanent
 * registry reference and forbids retirement; no target Task dereference.
 * The caller must already own current Task storage through this call. */
int krnP4ProtectCleanupWorker(struct Task *, struct ExecBase *);
/* One-time, live-RUN admission of the protected cleanup worker's destination
 * mask. Durable ready work is re-poked after the gate is released. */
int krnP4ArmCleanupWake(struct Task *, uint32_t, struct ExecBase *);
/* True only when the protected singleton has an installed nonzero mask. */
int krnP4CleanupWorkerReady(struct ExecBase *);
/* Consume one coalesced durable wake. Returns 1 with worker/mask, 0 if empty
 * or not yet armed (pending work is retained), and -1 on invalid state. The
 * caller delivers the snapshot asynchronously after releasing outer locks. */
int krnP4TakeCleanupWake(struct p4_cleanup_wake *, struct ExecBase *);
/* RemTask's early check, before debug/Task access: reject the protected worker
 * and fail closed on an unregistered/malformed identity. Not a retire claim. */
int krnP4TaskRemovalAllowed(struct Task *, struct ExecBase *);
/* Records are independently allocated, zeroed, unpublished caller storage.
 * Reclaim returns that storage only after retiring and draining every user.
 * No allocation/free or Task dereference takes place within the gate. */
int krnP4TaskRecordPublish(struct Task *, struct p4_task_registry_record *, struct ExecBase *);
/* Quiescent publication plus one creator pin, indivisible to reclamation. */
int krnP4TaskRecordPublishPinned(struct Task *, struct p4_task_registry_record *, struct ExecBase *);
#if defined(__AROSEXEC_SMP__) && defined(__AROSPLATFORM_SMP__)
/* Unbound constructor helpers. Caller owns the creator's sole live RUN on
 * hart; record/output follow register_constructing_locked's storage contract.
 * Publication reserves both designated references under the actual gate.
 * Finish follows the caller's last target access, with no outer locks held;
 * it reserves BOTH identities' cleanup/resource wakes before unlocking and
 * pokes only after unlock. Neither helper reads Task storage. Refusal returns
 * zero unchanged, malformed state fails closed. Retired creator recovery and
 * NewAddTask/Ready/cancellation routing remain required before activation. */
int krnP4TaskConstructionPublish(struct Task *, struct p4_task_registry_record *,
    struct Task *creator, uint32_t hart, uint32_t *, struct ExecBase *);
int krnP4TaskConstructionFinish(struct Task *, struct Task *creator,
    uint32_t generation, uint32_t hart, struct ExecBase *);
/* Unbound trusted protected/armed RUN worker only, with no outer locks.
 * All finish_orphan_construction_locked cancellation/last-access prerequisites
 * must already hold. Both resource wakes are reserved in the release gate;
 * poke follows unlock. Does not select or cancel orphan constructions. */
int krnP4TaskConstructionOrphanFinish(struct Task *worker, struct Task *target,
    struct Task *creator, uint32_t generation, struct ExecBase *);
#endif
int krnP4SignalTaskPin(struct Task *, struct ExecBase *);
void krnP4SignalTaskUnpin(struct Task *, struct ExecBase *);
/* Blocking-operation sidecar binding. Publish before queue insertion; these
 * calls arbitrate registry state only, never semaphore/request storage.
 * Begin requires the current live RUN identity. Grant follows request-field
 * snapshot/dequeue under its semaphore gate and reserves an ordinary Task
 * notification pin; Signal outside all gates, then SignalTaskUnpin only.
 * No old request/scope access may follow that notification. */
int krnP4SemaphoreWaitBegin(struct Task *, uint32_t, const void *,
    const void *, const void *, uint32_t *, struct ExecBase *);
int krnP4SemaphoreWaitAddRequest(struct Task *, uint32_t, struct ExecBase *);
int krnP4SemaphoreWaitClose(struct Task *, uint32_t, struct ExecBase *);
int krnP4SemaphoreWaitReady(struct Task *, uint32_t, struct ExecBase *);
int krnP4SemaphoreWaitGrantRequest(struct Task *, uint32_t, struct ExecBase *);
int krnP4SemaphoreWaitWithdrawRequest(struct Task *, uint32_t, struct ExecBase *);
int krnP4SemaphoreWaitFinish(struct Task *, uint32_t, struct ExecBase *);
/* Exclusive raw-identity cancellation claim: 1 snapshot, 0 empty, invalid
 * state is fatal. Finish requires every node withdrawn and final RUN drain;
 * release keeps cancellation sticky. No queue cancellation consumer is
 * installed by these leaf bindings themselves. */
int krnP4SemaphoreWaitClaimCancel(struct p4_task_wait_cancel_snapshot *,
    struct ExecBase *);
int krnP4SemaphoreWaitFinishCancel(struct Task *, uint32_t, struct ExecBase *);
int krnP4SemaphoreWaitReleaseCancel(struct Task *, uint32_t, struct ExecBase *);
/* Retire/admission transitions require caller's canonical Task lock, and IRQ
 * masking. The registry gate is a leaf: never hold it while acquiring Task,
 * queue or semaphore locks. Removal must cancel pinned queued requests. */
int krnP4TaskRecordRetire(struct Task *, struct ExecBase *);
/* Caller holds Task lock/IRQ mask and owns storage. Acquires the designated
 * deferred-cleanup reference atomically with retirement. Caller must transfer
 * it to exactly one service message; no ordinary Unpin releases it. */
int krnP4TaskRecordRetireForService(struct Task *, struct ExecBase *);
int krnP4TaskRecordEnterRunning(struct Task *, unsigned int, struct ExecBase *);
void krnP4TaskRecordLeaveRunning(struct Task *, unsigned int, struct ExecBase *);
/* LeaveRunning may instead be the owning hart's final raw-identity operation:
 * IRQs remain masked, its current_task is already clear, and all Task/context
 * accesses (including Task unlock) ended before releasing this RUN reference.
 * This avoids reclaiming retired Task storage before its final unlock. */
struct p4_task_registry_record *krnP4TaskRecordClaimReclaim(struct Task *, struct ExecBase *);
/* Service-message owner only: no Task dereference is needed for this check.
 * NULL retains the designated reference for a later retry/requeue. Success
 * consumes it atomically with detach after all users/Running owners drain.
 * Caller then exclusively owns cleanup, and frees the record outside gate. */
struct p4_task_registry_record *krnP4TaskRecordClaimService(struct Task *, struct ExecBase *);
/* GetMsg consumer only; refuses even a drained record unless exactly-once
 * readiness was reserved before ServicePort delivery. Claim BEFORE logging
 * or reading Task fields. No notification binding is installed yet. */
struct p4_task_registry_record *krnP4TaskRecordClaimQueuedService(struct Task *, struct ExecBase *);
/* Leaf-gate service consumer: atomically claims and detaches one durably
 * queued service record without dereferencing Task. Returns NULL when empty;
 * malformed registry state is fatal. The caller owns cleanup on success. */
struct p4_task_registry_record *krnP4TaskRecordClaimNextService(struct ExecBase *);
/* Selected cleanup consumer: keep a Task-independent in-flight identity in
 * the registry through the last target/context/memory free. Exclusive cleanup
 * ownership does not grant waiter completion. Finish must run after all such
 * accesses, with no Task/list lock, and precede out-of-gate record FreeMem. */
struct p4_task_registry_record *krnP4TaskRecordClaimNextRetainedService(struct ExecBase *);
void krnP4TaskRecordFinishRetainedService(struct p4_task_registry_record *, struct ExecBase *);
#if defined(__AROSEXEC_SMP__) && defined(__AROSPLATFORM_SMP__)
/* Trusted protected, armed RUN service worker ONLY, after every Task/context/
 * entry free and outside all outer locks. Try returns1 detached/free record,
 * 0 retained postfree pressure; malformed/nonfinishable and generation
 * exhaustion fail closed. Retry attempts at most8 records per invocation,
 * skips blocked records with a persistent nonwrapping scalar address cursor,
 * latches a continuation wake if more remain, and
 * returns one detached record to FreeMem outside the gate, or NULL. Neither
 * reads Task nor reclaims a record still accessing Task storage. Interest and
 * pin-drop/requester-cancel wakes share the registry gate; no pressure self-poke.
 * Serial exhaustion requires pre-free admission policy before SMP activation. */
int krnP4TaskRecordTryFinishPostFree(struct p4_task_registry_record *, struct ExecBase *);
struct p4_task_registry_record *krnP4TaskRecordRetryPostFree(struct ExecBase *);
#endif
int krnP4TaskRecordServiceOwnerRunning(struct Task *, unsigned int, struct ExecBase *);
int krnP4TaskRecordRunningOwner(struct Task *, struct ExecBase *);
#endif
