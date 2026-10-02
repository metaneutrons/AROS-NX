/* Public single-semaphore transitions over permanent, cancellation-safe
 * request storage. Full LIST/Procure/Vacate routing remains a separate gate. */
#if !defined(P4_SEMAPHORE_PUBLIC_TEST)
#include <aros/config.h>
#if !defined(__AROSEXEC_SMP__) || !defined(__AROSPLATFORM_SMP__)
#error "P4 public semaphore adapters require the matched SMP ABI"
#endif
#include <exec/semaphores.h>
#include <proto/exec.h>
#endif
#include <limits.h>
#include "../kernel/p4_task_pins.h"
#include "../kernel/p4_ipi.h"
#include "../kernel/p4_public_spinlock.h"
#include "p4_semaphore_gate.h"
#include "p4_semaphore_public.h"
#include "p4_self_remove.h"
#include "../kernel/p4_callback_escape.h"

/* Caller owns current RUN, with no semaphore/registry gate held. A peer may
 * have retired this continuation while an operation wrapper was rejecting it.
 * Never resume a retired owner as if a void Obtain had succeeded. */
static void p4_semaphore_retirement_boundary(struct Task *task,
    struct ExecBase *SysBase)
{
    Disable();
    if (krnP4SelfRemoveCurrentRetired(SysBase))
        Exec_P4RemoveSelf(task, SysBase);
    Enable();
}

static void p4_semaphore_obtain_refused(struct Task *task,
    struct ExecBase *SysBase) __attribute__((noreturn));
static void p4_semaphore_obtain_refused(struct Task *task,
    struct ExecBase *SysBase)
{
    p4_semaphore_retirement_boundary(task, SysBase);
    krnP4PublicSpinFatal(P4_PUBLIC_SPIN_BAD_STATE);
}

/* Both gates remain closed over every state-changing transaction, but no
 * Wait, SetSignal, notification poke or callback executes under either. */
void Exec_P4ObtainSemaphore(struct SignalSemaphore *sem, struct Task *task,
    struct Task *owner, struct ExecBase *SysBase)
{
    struct p4_task_wait_operation reservation;
    uint32_t generation;
    int result;

    if (task == NULL || (owner != NULL && owner != task))
        krnP4PublicSpinFatal(P4_PUBLIC_SPIN_BAD_ARGUMENT);
    Forbid();
    Exec_P4SemaphoreLock(sem, SysBase);
    if (sem->ss_NestCount < 0 || sem->ss_QueueCount < -1 ||
        sem->ss_QueueCount < sem->ss_NestCount - 1 ||
        (sem->ss_QueueCount == -1 && sem->ss_NestCount != 0) ||
        (sem->ss_WaitQueue.mlh_Head ==
             (struct MinNode *)(void *)&sem->ss_WaitQueue.mlh_Tail &&
         sem->ss_QueueCount != sem->ss_NestCount - 1) ||
        (sem->ss_NestCount == 0 && sem->ss_QueueCount == -1 &&
         (sem->ss_Owner != NULL || sem->ss_WaitQueue.mlh_Head !=
          (struct MinNode *)(void *)&sem->ss_WaitQueue.mlh_Tail)))
        krnP4PublicSpinFatal(P4_PUBLIC_SPIN_BAD_STATE);
    /* Uncontended/recursive ownership needs no active blocking scope. In
     * particular a Task may nest semaphores while constructing a LIST scope.
     * Do not bypass a NestCount0 queue awaiting durable handoff. */
    if (sem->ss_QueueCount == -1 ||
        (sem->ss_NestCount > 0 &&
         (sem->ss_Owner == task || sem->ss_Owner == owner)))
    {
        if (sem->ss_QueueCount == INT16_MAX || sem->ss_NestCount == INT16_MAX)
            krnP4PublicSpinFatal(P4_PUBLIC_SPIN_BAD_STATE);
        if (sem->ss_NestCount == 0)
            sem->ss_Owner = owner;
        ++sem->ss_QueueCount;
        ++sem->ss_NestCount;
        Exec_P4SemaphoreUnlock(sem, SysBase);
        Permit();
        return;
    }
    Exec_P4SemaphoreUnlock(sem, SysBase);

    /* Reservation is allocator-free. Resource refusal cannot return from a
     * void Obtain as if ownership existed; it fails closed for now. A future
     * capacity-wait policy must not sleep while holding a semaphore gate. */
    if (krnP4SemaphoreOperationReserve(task, 1u, &reservation, SysBase) != 1)
        p4_semaphore_obtain_refused(task, SysBase);
    generation = reservation.generation;
    Exec_P4SemaphoreLock(sem, SysBase);
    result = krnP4SemaphoreOperationConfigure(task, generation, 0u, sem,
        owner == NULL ? 1u : 0u, SysBase);
    Exec_P4SemaphoreUnlock(sem, SysBase);
    if (result != 1 || krnP4SemaphoreOperationClose(task, generation, SysBase) != 1)
        p4_semaphore_obtain_refused(task, SysBase);

    /* Clear BEFORE publishing a queued request. A grant after this clear
     * either satisfies Complete or leaves a signal for the subsequent Wait. */
    SetSignal(0, SIGF_SINGLE);
    Exec_P4SemaphoreLock(sem, SysBase);
    result = krnP4SemaphoreOperationAcquire(task, generation, 0u, sem, SysBase);
    Exec_P4SemaphoreUnlock(sem, SysBase);
    if (result != 1 && result != 2)
        p4_semaphore_obtain_refused(task, SysBase);

    /* The signal is only a wake hint. Complete checks exact generation and
     * every owned entry before releasing the designated pin/arena storage.
     * No arena pointer from reservation is read, even after a stale wake. */
    for (;;)
    {
        result = krnP4SemaphoreOperationComplete(task, generation, SysBase);
        if (result == 1)
            break;
        if (result < 0)
            p4_semaphore_obtain_refused(task, SysBase);
        p4_semaphore_retirement_boundary(task, SysBase);
        /* Retirement after a false probe is caught by Wait's rejected-arm
         * handoff or the retired outer trap, not by this wake-hint loop. */
        Wait(SIGF_SINGLE);
    }
    Permit();
}

void Exec_P4ReleaseSemaphore(struct SignalSemaphore *sem,
    struct ExecBase *SysBase)
{
    int result;
    Forbid();
    Exec_P4SemaphoreLock(sem, SysBase);
    result = krnP4SemaphoreReleaseDurable(sem, SysBase);
    Exec_P4SemaphoreUnlock(sem, SysBase);
    if (result <= 0)
        krnP4PublicSpinFatal(P4_PUBLIC_SPIN_BAD_STATE);
    /* Result is 1 + notification count. The independent worker owns durable
     * notification pins, not the releasing task or its request continuation. */
    if (result > 1)
        krnP4CleanupPoke(SysBase);
    Permit();
}
