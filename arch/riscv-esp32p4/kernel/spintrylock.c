/*
 * P4 override for the public KrnSpinTryLock LVO. Selected only by the
 * explicit public-spinlock candidate build option.
 */
#include <aros/types/spinlock_s.h>
#include <aros/kernel.h>
#include <aros/libcall.h>
#include <kernel_base.h>

#include <proto/kernel.h>

#if defined(__AROSEXEC_SMP__) && !defined(P4_E3_PUBLIC_SPINLOCK)
#error "P4 Exec SMP requires the public spinlock adapter"
#endif

#if defined(P4_E3_PUBLIC_SPINLOCK)
#include "p4_public_spinlock.h"
#endif

AROS_LH2(spinlock_t *, KrnSpinTryLock,
         AROS_LHA(spinlock_t *, lock, A0),
         AROS_LHA(ULONG, mode, D0),
         struct KernelBase *, KernelBase, 51, Kernel)
{
    AROS_LIBFUNC_INIT

    (void)KernelBase;
    (void)mode;
#if defined(P4_E3_PUBLIC_SPINLOCK)
    enum p4_public_spinlock_result result =
        krnP4PublicSpinTryAcquire(lock, mode);

    if (result == P4_PUBLIC_SPIN_OK)
        return lock;
    if (result == P4_PUBLIC_SPIN_BUSY ||
        result == P4_PUBLIC_SPIN_TRY_TIMEOUT)
        return NULL;
    krnP4PublicSpinFatal(result);
#else
    return lock;
#endif

    AROS_LIBFUNC_EXIT
}
