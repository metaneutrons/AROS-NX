/* Durable cleanup wake consumer. Full removal/topology integration remains
 * required before enabling a configured SMP system. */
#if !defined(P4_CLEANUP_WORKER_TEST)
#include <exec/execbase.h>
#include <exec/ports.h>
#include <proto/exec.h>
#include "exec_intern.h"
#include "p4_signal_adapter.h"
#include "p4_task_cleanup.h"
#include "../kernel/p4_task_pins.h"
#include "../kernel/p4_public_spinlock.h"
#endif
#include "p4_cleanup_worker.h"
#include "p4_semaphore_cancel_worker.h"

void Exec_P4DeliverCleanupWake(struct ExecBase *SysBase)
{
    struct p4_cleanup_wake wake;
    struct p4_signal_effect effect;
    int result;

    /* The registry snapshot retains a permanent, non-retirable worker owner.
     * Taking it neither reads Task fields nor retains the registry gate. */
    Disable();
    result = krnP4TakeCleanupWake(&wake, SysBase);
    if (result < 0)
        p4_signal_adapter_fail_closed();
    if (result == 0)
    {
        Enable();
        return;
    }
    if (wake.worker == NULL || wake.mask == 0)
        p4_signal_adapter_fail_closed();
    p4_signal_adapter_lock_disabled(wake.worker);
    effect = p4_signal_adapter_raise_locked(wake.worker, SysBase, wake.mask);
    if (effect.status == P4_SIGNAL_STATUS_OK &&
        effect.queue_transition != P4_SIGNAL_QUEUE_NONE)
        (void)p4_signal_adapter_apply_queue_locked(wake.worker, SysBase, &effect);
    p4_signal_adapter_unlock_disabled(wake.worker);
    if (effect.status != P4_SIGNAL_STATUS_OK)
        p4_signal_adapter_fail_closed();
    p4_signal_adapter_poke_after_unlock(&effect, SysBase);
    Enable();
}

void Exec_P4ServiceLoop(struct ExecBase *SysBase)
{
    struct Task *worker = GET_THIS_TASK;
    struct MsgPort *port = PrivExecBase(SysBase)->ServicePort;
    uint32_t mask;

    /* Only the canonical ServiceTask, already protected by its entry hook,
     * owns these current-task/port accesses. Priority is not a ready ACK. */
    if (worker == NULL || port == NULL || port->mp_SigTask != worker ||
        port->mp_SigBit >= 32)
        p4_signal_adapter_fail_closed();
    mask = UINT32_C(1) << port->mp_SigBit;
    if (!krnP4ArmCleanupWake(worker, mask, SysBase))
        p4_signal_adapter_fail_closed();

    for (;;)
    {
        /* Clear BEFORE draining: any new durable readiness after that point
         * is sticky across empty detection and Wait. Never GetMsg a raw Task. */
        (void)SetSignal(0, mask);
        for (;;)
        {
            int notified = Exec_P4DeliverOneSemaphoreNotification(worker, SysBase);
            int cancelled = Exec_P4SemaphoreCancellationPass(worker, SysBase);
            int cleaned = krnP4CleanupOneRetiredTask(SysBase);
            /* One batch, bounded cancellation pass and cleanup per iteration.
             * Cancellation reports mutations only: a resource-blocked pass
             * retains retry interest but must not busy-loop by itself. This
             * class ordering does not bound high-priority CPU occupancy. */
            if (!notified && !cancelled && !cleaned)
                break;
        }
        (void)Wait(mask);
    }
}

int Exec_P4DeliverOneSemaphoreNotification(struct Task *worker,
    struct ExecBase *SysBase)
{
    struct p4_semaphore_notification snapshot;
    struct p4_signal_effect effect;
    int result = krnP4SemaphoreNotificationClaim(worker, &snapshot, SysBase);
    if (result < 0)
        p4_signal_adapter_fail_closed();
    if (result == 0)
        return 0;
    if (snapshot.identity == NULL || snapshot.count == 0 ||
        snapshot.generation == 0)
        p4_signal_adapter_fail_closed();

    /* Claim's inflight reservation already retains target storage, even if
     * retirement races delivery. Public Signal would ask for another pin and
     * could silently skip the wake at saturation. No registry/semaphore lock
     * remains here; take Task then queue locks in the canonical adapter order. */
    Disable();
    p4_signal_adapter_lock_disabled(snapshot.identity);
    effect = p4_signal_adapter_raise_locked(snapshot.identity, SysBase, SIGF_SINGLE);
    if (effect.status == P4_SIGNAL_STATUS_OK &&
        effect.queue_transition != P4_SIGNAL_QUEUE_NONE)
        (void)p4_signal_adapter_apply_queue_locked(snapshot.identity, SysBase, &effect);
    p4_signal_adapter_unlock_disabled(snapshot.identity);
    if (effect.status != P4_SIGNAL_STATUS_OK &&
        effect.status != P4_SIGNAL_STATUS_NOT_LIVE)
        p4_signal_adapter_fail_closed();
    p4_signal_adapter_poke_after_unlock(&effect, SysBase);
    Enable();
    /* Final target access ended before consuming exactly this batch's pins.
     * Completion may make cleanup ready; its binding publishes that wake. */
    krnP4SemaphoreNotificationFinish(worker, &snapshot, SysBase);
    return 1;
}
