/* Bounded P4 SMP arbitration for one initialized SignalSemaphore. */
#if !defined(P4_SEMAPHORE_GATE_TEST)
#include <aros/config.h>
#if !defined(__AROSEXEC_SMP__) || !defined(__AROSPLATFORM_SMP__)
#error "P4 semaphore gates require the matched SMP ABI"
#endif
#include <exec/semaphores.h>
#include <proto/exec.h>
#endif

#include "../kernel/p4_public_spinlock.h"
#include "p4_semaphore_gate.h"

#define P4_SEMAPHORE_GATE_ATTEMPTS 20000u

void Exec_P4SemaphoreLock(struct SignalSemaphore *sigSem,
                          struct ExecBase *SysBase)
{
    unsigned int attempt;
    (void)SysBase;

    /* The matching Unlock() consumes this one Disable nesting level. */
    Disable();
    if (sigSem == 0)
        krnP4PublicSpinFatal(P4_PUBLIC_SPIN_BAD_ARGUMENT);

    for (attempt = 0; attempt < P4_SEMAPHORE_GATE_ATTEMPTS; ++attempt)
    {
        enum p4_public_spinlock_result result = krnP4PublicSpinTryAcquire(
            &sigSem->ss_MultipleLink.sr_SpinLock, SPINLOCK_MODE_WRITE);

        if (result == P4_PUBLIC_SPIN_OK)
            return;
        if (result != P4_PUBLIC_SPIN_BUSY &&
            result != P4_PUBLIC_SPIN_TRY_TIMEOUT)
            krnP4PublicSpinFatal(result);
    }

    krnP4PublicSpinFatal(P4_PUBLIC_SPIN_SERVICE_TIMEOUT);
}

void Exec_P4SemaphoreUnlock(struct SignalSemaphore *sigSem,
                            struct ExecBase *SysBase)
{
    enum p4_public_spinlock_result result;
    (void)SysBase;

    if (sigSem == 0)
    {
        /* Preserve fail-closed behavior even for a mismatched null Unlock. */
        Disable();
        krnP4PublicSpinFatal(P4_PUBLIC_SPIN_BAD_ARGUMENT);
    }

    result = krnP4PublicSpinUnlock(&sigSem->ss_MultipleLink.sr_SpinLock);
    if (result != P4_PUBLIC_SPIN_OK)
        krnP4PublicSpinFatal(result);

    /* Release the semaphore before restoring the caller's prior IRQ nesting. */
    Enable();
}
