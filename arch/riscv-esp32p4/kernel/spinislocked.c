/*
 * P4 override for the public KrnSpinIsLocked LVO. Selected only by the
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

AROS_LH1(int, KrnSpinIsLocked,
         AROS_LHA(spinlock_t *, lock, A0),
         struct KernelBase *, KernelBase, 50, Kernel)
{
    AROS_LIBFUNC_INIT

    (void)KernelBase;
#if defined(P4_E3_PUBLIC_SPINLOCK)
    int held = 0;
    enum p4_public_spinlock_result result =
        krnP4PublicSpinIsLocked(lock, &held);

    if (result != P4_PUBLIC_SPIN_OK)
        krnP4PublicSpinFatal(result);
    return held;
#else
    (void)lock;
    return 0;
#endif

    AROS_LIBFUNC_EXIT
}
