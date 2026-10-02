/*
 * Portable lifetime state machine used by the gated P4 SMP registry binding.
 * Creation/admission use it; complete removal, notification and semaphore
 * cancellation integration is still required before SMP activation.
 */
#ifndef P4_TASK_LIFETIME_H
#define P4_TASK_LIFETIME_H

#include <stdint.h>

#define P4_TASK_LIFETIME_HARTS 2u

#define P4_TASK_LIFETIME_RETIRED  UINT32_C(0x00000001)
#define P4_TASK_LIFETIME_CLAIMED  UINT32_C(0x00000002)

struct p4_task_lifetime
{
    uint32_t notification_pins;
    uint32_t running_harts;
    uint32_t flags;
};

/*
 * All *_locked operations require one caller-owned lifetime gate to be held.
 * A caller must acquire that gate before looking up a task and keep it through
 * pin_locked(); merely receiving a raw Task pointer does not make this record
 * safe to dereference.  The same gate serializes retire+registry removal and
 * claim_reclaim_locked().  Every asynchronous user of task storage must hold
 * a pin, and the scheduler must hold a running-hart reference from before
 * dispatch until the previous context is no longer executing.  This model
 * intentionally matches the P4's two application harts; the mask is not a
 * generic multi-processor ABI.  Reclamation is eligible only after
 * retirement, all pins, and all running references
 * have drained.  Self-retirement only marks state; a later service claims and
 * performs reclamation.  These helpers do not lock, wait, allocate, notify,
 * or reclaim memory.
 *
 * Semaphore integration still has two mandatory obligations: pin the Task
 * (and therefore its stack-backed SemaphoreRequest) before queue publication,
 * retaining that pin through grant/cancel and the out-of-lock Signal; and
 * serialize RemTask retirement with removal/cancellation of every queued
 * request before its stack can be reclaimed.  This model alone does not make
 * the existing semaphore queues safe.
 */
void p4_task_lifetime_init(struct p4_task_lifetime *lifetime);
int p4_task_lifetime_pin_locked(struct p4_task_lifetime *lifetime);
int p4_task_lifetime_unpin_locked(struct p4_task_lifetime *lifetime);
int p4_task_lifetime_retire_locked(struct p4_task_lifetime *lifetime);
int p4_task_lifetime_enter_running_locked(
    struct p4_task_lifetime *lifetime, uint32_t hart);
int p4_task_lifetime_leave_running_locked(
    struct p4_task_lifetime *lifetime, uint32_t hart);
int p4_task_lifetime_reclaimable_locked(
    const struct p4_task_lifetime *lifetime);
int p4_task_lifetime_claim_reclaim_locked(
    struct p4_task_lifetime *lifetime);

#endif
