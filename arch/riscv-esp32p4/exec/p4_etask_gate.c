/* Private SRAM serialization for ETask identity/topology transactions. */
#if !defined(P4_ETASK_GATE_TEST)
#include <asm/cpu.h>
#include "../kernel/hardware.h"
#include "../kernel/kernel_intern.h"
#include "../kernel/cpu_local_runtime.h"
#include "../kernel/p4_public_spinlock.h"
#include "p4_etask_gate.h"
#define P4_ETASK_GATE_MASK(status) \
    __asm__ volatile("csrrci %0,mstatus,8" : "=r"(status) : : "memory")
#define P4_ETASK_GATE_RESTORE() \
    __asm__ volatile("csrsi mstatus,8" : : : "memory")
#define P4_ETASK_GATE_FENCE() \
    __asm__ volatile("fence rw,rw" ::: "memory")
#endif

static spinlock_t etask_gate P4_SRAMDATA;
static unsigned int etask_gate_prepared P4_SRAMDATA;

int krnP4ETaskGatePrepare(void)
{
    if (etask_gate_prepared || P4_CPU_LOCAL_READ_MHARTID() != 0 ||
        (csr_read(mstatus) & MSTATUS_MIE) != 0)
        return 0;
    if (krnP4PublicSpinInit(&etask_gate) != P4_PUBLIC_SPIN_OK)
        return 0;
    P4_ETASK_GATE_FENCE();
    etask_gate_prepared = 1;
    return 1;
}

uint32_t krnP4ETaskGateLock(struct ExecBase *base)
{
    struct p4_exec_runtime *runtime;
    uint32_t status;
    unsigned int attempt;

    P4_ETASK_GATE_MASK(status);
    runtime = p4_exec_runtime_current(base);
    if (!etask_gate_prepared || runtime == NULL || runtime->hart_id >= 2 ||
        runtime->cpu_local == NULL ||
        runtime->cpu_local->hart_id != runtime->hart_id)
        krnP4PublicSpinFatal(P4_PUBLIC_SPIN_BAD_STATE);
    for (attempt = 0; attempt < 20000u; ++attempt)
    {
        enum p4_public_spinlock_result result =
            krnP4PublicSpinTryAcquire(&etask_gate, SPINLOCK_MODE_WRITE);
        if (result == P4_PUBLIC_SPIN_OK)
            return status & MSTATUS_MIE;
        if (result != P4_PUBLIC_SPIN_BUSY && result != P4_PUBLIC_SPIN_TRY_TIMEOUT)
            krnP4PublicSpinFatal(result);
    }
    krnP4PublicSpinFatal(P4_PUBLIC_SPIN_SERVICE_TIMEOUT);
    return 0;
}

void krnP4ETaskGateUnlock(uint32_t token)
{
    enum p4_public_spinlock_result result;

    if (token != 0 && token != MSTATUS_MIE)
        krnP4PublicSpinFatal(P4_PUBLIC_SPIN_BAD_STATE);
    result = krnP4PublicSpinUnlock(&etask_gate);
    if (result != P4_PUBLIC_SPIN_OK)
        krnP4PublicSpinFatal(result);
    if (token)
        P4_ETASK_GATE_RESTORE();
}
