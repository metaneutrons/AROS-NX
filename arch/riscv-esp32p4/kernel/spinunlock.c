/*
 * P4 override for the mode-less public KrnSpinUnLock LVO.
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

AROS_LH1(void, KrnSpinUnLock,
         AROS_LHA(spinlock_t *, lock, A0),
         struct KernelBase *, KernelBase, 53, Kernel)
{
    AROS_LIBFUNC_INIT

    (void)KernelBase;
#if defined(P4_E3_PUBLIC_SPINLOCK)
    enum p4_public_spinlock_result result = krnP4PublicSpinUnlock(lock);

    if (result != P4_PUBLIC_SPIN_OK)
        krnP4PublicSpinFatal(result);
#else
    (void)lock;
#endif

    AROS_LIBFUNC_EXIT
}
