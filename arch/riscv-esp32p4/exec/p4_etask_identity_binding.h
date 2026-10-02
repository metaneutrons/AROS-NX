#ifndef P4_ETASK_IDENTITY_BINDING_H
#define P4_ETASK_IDENTITY_BINDING_H
struct ETask;
struct ExecBase;
/* Fresh unpublished ETask only. Allocation is outside the metadata gate;
 * reservation, ETask PID and shared counter publication form one transaction.
 * Release requires exclusive final-expunge ownership, not merely a Task pin. */
int krnP4ETaskIdentityReserve(struct ETask *, struct ExecBase *);
/* Returns one only when the caller owns final ETask-storage reclamation.
 * Zero means logical expunge retained by existing readers (or fatal refusal).
 * Do not free ETask/Result2 on zero. PID remains reserved until last reader. */
int krnP4ETaskIdentityRelease(struct ETask *, struct ExecBase *);
/* This internally locking admission requires an existing metadata owner that
 * protects et until the lease is acquired. A pointer discovered under a gate
 * that was then dropped is insufficient; discovery-based admission must be
 * implemented inside the same topology transaction, not through this wrapper.
 * An arbitrary stale pointer is not a valid admission request.
 * Leases retain ETask storage, never Task storage; no Task fields may be read
 * without a separately acquired/revalidated Task pin. Unpin consumes one
 * successfully acquired lease and may free the ETask before returning. */
int krnP4ETaskMetadataReadPin(struct ETask *, struct ExecBase *);
void krnP4ETaskMetadataReadUnpin(struct ETask *, struct ExecBase *);
/* Private core helper: caller already owns the unique reclaim claim and has
 * detached identity/sidecar. No topology gate or port lock may be held. */
void Exec_FreeETaskStorage(struct ETask *, struct ExecBase *);
/* Cleanup requires exclusive drained Task ownership, or private failed
 * construction. These hooks do not establish parent/reader lifetime safety. */
void krnP4ETaskMetadataBeginCleanup(struct ETask *, struct ExecBase *);
void krnP4ETaskMetadataRetainResult(struct ETask *, struct ExecBase *);
#endif
