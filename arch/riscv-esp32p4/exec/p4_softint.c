/* P4 shared softint leaf gate. Only owner0 executes callbacks, outside it. */
#if !defined(P4_SOFTINT_GATE_TEST)
#include <proto/exec.h>
#include <asm/cpu.h>
#include "exec_intern.h"
#include "../kernel/hardware.h"
#include "../kernel/kernel_intern.h"
#include "../kernel/cpu_local_runtime.h"
#include "../kernel/p4_public_spinlock.h"
#include "p4_softint_queue.h"
#include "p4_softint.h"
#endif
#if !defined(__AROSEXEC_SMP__) || !defined(__AROSPLATFORM_SMP__)
#error "P4 shared softints require matched Exec SMP"
#endif

#if !defined(P4_SOFTINT_GATE_TEST)
#define P4_SOFTINT_FENCE() __asm__ volatile("fence rw,rw" ::: "memory")
#define P4_SOFTINT_MASK(status) \
    __asm__ volatile("csrrci %0,mstatus,8" : "=r"(status) : : "memory")
#define P4_SOFTINT_RESTORE() __asm__ volatile("csrsi mstatus,8" : : : "memory")
#endif

static spinlock_t softint_gate P4_SRAMDATA;
static struct p4_softint_state softint_state P4_SRAMDATA;
static unsigned int softint_prepared P4_SRAMDATA;

int krnP4SoftIntPrepare(void)
{
    if (softint_prepared || SysBase != NULL ||
        P4_CPU_LOCAL_READ_MHARTID() != 0 || (csr_read(mstatus) & MSTATUS_MIE))
        return 0;
    if (krnP4PublicSpinInit(&softint_gate) != P4_PUBLIC_SPIN_OK)
        return 0;
    softint_state.pending = softint_state.active = 0;
    P4_SOFTINT_FENCE();
    softint_prepared = 1;
    return 1;
}

static uint32_t softint_lock(struct ExecBase *base)
{
    struct p4_exec_runtime *runtime;
    uint32_t status;
    unsigned int attempt;
    P4_SOFTINT_MASK(status);
    runtime = p4_exec_runtime_current(base);
    if (!softint_prepared || runtime == NULL || runtime->hart_id >= 2 ||
        runtime->cpu_local == NULL ||
        runtime->cpu_local->hart_id != runtime->hart_id)
        krnP4PublicSpinFatal(P4_PUBLIC_SPIN_BAD_STATE);
    for (attempt = 0; attempt < 20000u; ++attempt)
    {
        enum p4_public_spinlock_result result =
            krnP4PublicSpinTryAcquire(&softint_gate, SPINLOCK_MODE_WRITE);
        if (result == P4_PUBLIC_SPIN_OK)
            return status & MSTATUS_MIE;
        if (result != P4_PUBLIC_SPIN_BUSY && result != P4_PUBLIC_SPIN_TRY_TIMEOUT)
            krnP4PublicSpinFatal(result);
    }
    krnP4PublicSpinFatal(P4_PUBLIC_SPIN_SERVICE_TIMEOUT);
}

static void softint_unlock(uint32_t saved)
{
    enum p4_public_spinlock_result result = krnP4PublicSpinUnlock(&softint_gate);
    if (result != P4_PUBLIC_SPIN_OK)
        krnP4PublicSpinFatal(result);
    if (saved)
        P4_SOFTINT_RESTORE();
}

int krnP4SoftIntPending(struct ExecBase *base)
{
    uint32_t saved;
    int pending;
    /* Hart1 never reads/drains the shared queue or calls global handlers. */
    if (P4_CPU_LOCAL_READ_MHARTID() != 0)
        return 0;
    saved = softint_lock(base);
    pending = softint_state.pending != 0 && softint_state.active == 0;
    softint_unlock(saved);
    return pending;
}

void krnP4SoftIntEnqueue(struct Interrupt *interrupt, struct ExecBase *base)
{
    uint32_t saved = softint_lock(base);
    if (!p4_softint_enqueue(&softint_state, base, interrupt))
        krnP4PublicSpinFatal(P4_PUBLIC_SPIN_BAD_STATE);
    softint_unlock(saved);
}

int krnP4SoftIntBegin(struct ExecBase *base)
{
    uint32_t saved = softint_lock(base);
    int result = p4_softint_begin(&softint_state, P4_CPU_LOCAL_READ_MHARTID());
    if (result < 0)
        krnP4PublicSpinFatal(P4_PUBLIC_SPIN_BAD_STATE);
    softint_unlock(saved);
    return result;
}

struct Interrupt *krnP4SoftIntTake(struct ExecBase *base)
{
    struct Interrupt *interrupt;
    uint32_t saved = softint_lock(base);
    if (!p4_softint_take(&softint_state, base, P4_CPU_LOCAL_READ_MHARTID(),
                         &interrupt))
        krnP4PublicSpinFatal(P4_PUBLIC_SPIN_BAD_STATE);
    softint_unlock(saved);
    return interrupt;
}
