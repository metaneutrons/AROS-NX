/* Owner-only application of a detached scheduler mailbox batch. */
#if !defined(P4_IPI_RUNTIME_TEST)
#include <asm/cpu.h>
#include "cpu_local_runtime.h"
#endif
#include "p4_public_spinlock.h"
#include "p4_ipi.h"

void krnP4IpiApplyBatch(const struct p4_ipi_batch *batch,
                       struct ExecBase *SysBase)
{
    struct p4_exec_runtime *runtime = p4_exec_runtime_current(SysBase);
    struct p4_cpu_local *local;
    const uint32_t allowed = P4_EXEC_SCHEDULE_SWITCH | P4_EXEC_SCHEDULE_DISPATCH;
    if (batch == NULL || runtime == NULL || runtime->hart_id >= 2 ||
        runtime->cpu_local == NULL ||
        runtime->cpu_local->hart_id != runtime->hart_id ||
        runtime->cpu_local->trap_depth == 0 ||
        (csr_read(mstatus) & MSTATUS_MIE) != 0 ||
        (batch->schedule_flags & ~allowed) != 0 ||
        (batch->ticks != 0 && runtime->hart_id != 1))
        krnP4PublicSpinFatal(P4_PUBLIC_SPIN_BAD_STATE);

    local = runtime->cpu_local;
    local->schedule_flags |= batch->schedule_flags;
    local->ticks += batch->ticks; /* unsigned diagnostic counter wraps */
    if (batch->ticks != 0)
    {
        if (batch->ticks >= runtime->elapsed)
        {
            runtime->elapsed = 0;
            local->schedule_flags |= P4_EXEC_SCHEDULE_QUANTUM |
                                     P4_EXEC_SCHEDULE_SWITCH;
        }
        else
            runtime->elapsed -= batch->ticks;
    }
    /* The global VBlank/DOS/softint chain belongs to hart0, not this batch. */
}
