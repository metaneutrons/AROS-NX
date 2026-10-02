/* P4 matched-SMP scheduler front door. CPU paths own switch/dispatch. */
#include <exec/execbase.h>
#include <proto/exec.h>
#include <kernel_scheduler.h>
#include "p4_schedule.h"
#include "p4_public_spinlock.h"
#if !defined(__AROSEXEC_SMP__) || !defined(__AROSPLATFORM_SMP__)
#error "P4 scheduler override requires matched Exec SMP"
#endif

BOOL core_Schedule(void)
{
    return krnP4ShouldSchedule(SysBase) ? TRUE : FALSE;
}

/* P4 cpu_Switch/cpu_Dispatch call the lifetime-aware P4 implementations.
 * No generic queue mutation may bypass those ownership transactions. */
void core_Switch(void)
{
    krnP4PublicSpinFatal(P4_PUBLIC_SPIN_BAD_STATE);
}

struct Task *core_Dispatch(void)
{
    krnP4PublicSpinFatal(P4_PUBLIC_SPIN_BAD_STATE);
}
