/* P4 SMP-only queue adapter; full scheduler activation remains gated. */
#ifndef P4_SCHEDULER_QUEUE_H
#define P4_SCHEDULER_QUEUE_H
#include "../exec/p4_signal_protocol.h"
struct Task;
struct ExecBase;
struct p4_remove_handle;

/* Caller holds Task.tc_SpinLock, masks local interrupts, and keeps task
 * storage alive. Source/destination list locks are acquired internally in
 * Ready -> Wait -> Running order. The caller publishes no task state until
 * this function succeeds. Return <0 on a rejected transition, 0 for Wait,
 * or destination hart+1 for a Ready insertion requiring an unlocked poke.
 * This does not release a running-context lifetime reference; the scheduler
 * must do so only after the complete context and stack handoff. */
int krnP4SignalQueueApplyLocked(struct Task *task,
    enum p4_signal_queue_transition transition, struct ExecBase *SysBase);
/* Same Task lock/lifetime contract. No queue mutation; returns eligible hart+1
 * or -1. Signal also uses this for a task already on Ready. */
int krnP4SignalEligibleHartLocked(struct Task *task);
/* Already pinned candidate, canonical Task lock and local IRQ masking are
 * required. Does NOT select a candidate or publish ThisTask/runtime context.
 * Acquires Ready then Running, admits unique lifetime ownership, publishes
 * iet_CpuNumber before TS_RUN/list insertion. Returns 1 on admission, 0 on
 * refusal with no state/list mutation. The caller must not abandon an admitted
 * Task; its saved context must subsequently be installed on this hart. */
int krnP4ReadyToRunningLocked(struct Task *, unsigned int, struct ExecBase *);
/* Quiescent creation / hart0 bootstrap, caller holds Task lock and IRQ mask.
 * New task already has a registered creator pin. Return hart+1 / 0 on Ready
 * publication, and 1 / 0 on bootstrap owner+Running publication. */
int krnP4NewTaskReadyLocked(struct Task *, struct ExecBase *);
int krnP4BootTaskRunningLocked(struct Task *, struct ExecBase *);
/* Task lock and lifetime ownership required. Read-only bounded absence check
 * under Ready/Wait/Running locks; ServicePort membership is independent. */
int krnP4TaskDetachedLocked(struct Task *, struct ExecBase *);
/* Removal transaction, caller owns an ordinary pin, Task lock and IRQ mask.
 * Validates all three queues before atomically acquiring the service pin and
 * retiring. Detaches exactly one scheduler node and publishes TS_REMOVED.
 * No cleanup, notification, RUN release or caller completion occurs here.
 * Queued semaphore cancellation and external RemTask completion remain the
 * caller's responsibility. Returns 1 on success, 0 with no mutation. */
int krnP4TaskRetireLocked(struct Task *, struct ExecBase *);
/* External removal with an atomic completion ticket. Same ordinary pin,
 * Task lock and IRQ contract as RetireLocked. Caller is a live RUN requester
 * on hart. Validates queues before registry/bank admission, then infallibly
 * detaches and sets TS_REMOVED. Returns1/0, outputs unchanged on refusal.
 * Handle storage follows the registry output-lifetime contract; a private
 * automatic slot on the requester's live RUN-owned stack is allowed.
 * Caller snapshots the RUN owner before this call, releases outer locks and
 * target pin, then pokes that owner and cleanup; no poke occurs here. Already-
 * retired raw-identity joins use the pins binding separately, without Task
 * reads. This does not implement the public wait or capacity policy. */
int krnP4TaskRetireWithTicketLocked(struct Task *target, struct Task *requester,
    uint32_t hart, struct p4_remove_handle *, struct ExecBase *);
/* Pinned Task, Task lock and IRQ mask. Validate all memberships/owner before
 * writing priority; only READY is reordered, under Ready's write lock.
 * Outputs are copied values for an optional post-unlock reschedule effect.
 * Failure leaves Task/list/output values unchanged. Return one on success. */
int krnP4TaskPriorityLocked(struct Task *, int8_t, int8_t *, uint32_t *, struct ExecBase *);
#endif
