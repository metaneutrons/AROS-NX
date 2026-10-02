/*
 * P4 override for the public KrnSpinLock LVO. The legacy fail hook is called
 * after each busy/timeout attempt, outside the atomic service, and its return
 * value is only a notification.
 */
#include <aros/types/spinlock_s.h>
#include <aros/kernel.h>
#include <aros/libcall.h>
#include <utility/hooks.h>
#include <kernel_base.h>

#include <proto/kernel.h>

#if defined(__AROSEXEC_SMP__) && !defined(P4_E3_PUBLIC_SPINLOCK)
#error "P4 Exec SMP requires the public spinlock adapter"
#endif

#if defined(P4_E3_PUBLIC_SPINLOCK)
#include "p4_public_spinlock.h"

static uintptr_t p4_public_spin_hook_bridge(spinlock_t *lock, void *context)
{
    struct Hook *hook = (struct Hook *)context;

    return (uintptr_t)CALLHOOKPKT(hook, (APTR)lock, 0);
}
#endif

AROS_LH3(spinlock_t *, KrnSpinLock,
         AROS_LHA(spinlock_t *, lock, A1),
         AROS_LHA(struct Hook *, failhook, A0),
         AROS_LHA(ULONG, mode, D0),
         struct KernelBase *, KernelBase, 52, Kernel)
{
    AROS_LIBFUNC_INIT

    (void)KernelBase;
#if defined(P4_E3_PUBLIC_SPINLOCK)
    for (;;)
    {
        enum p4_public_spinlock_result result =
            krnP4PublicSpinBlockingAttempt(
                lock, mode, failhook != NULL ? p4_public_spin_hook_bridge : NULL,
                failhook);

        if (result == P4_PUBLIC_SPIN_OK)
            return lock;
        if (result != P4_PUBLIC_SPIN_BUSY &&
            result != P4_PUBLIC_SPIN_TRY_TIMEOUT)
            krnP4PublicSpinFatal(result);
    }
#else
    (void)failhook;
    (void)mode;
    return lock;
#endif

    AROS_LIBFUNC_EXIT
}
