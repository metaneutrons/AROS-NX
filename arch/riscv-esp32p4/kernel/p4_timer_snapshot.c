/* Shared SYSTIMER UNIT0 snapshot, serialized across both Exec harts. */
#if !defined(P4_TIMER_SNAPSHOT_TEST)
#include <asm/cpu.h>
#include <proto/exec.h>
#include "hardware.h"
#include "kernel_intern.h"
#include "cpu_local_runtime.h"
#if !defined(__AROSEXEC_SMP__)
#error "The P4 snapshot gate requires matched Exec SMP"
#endif
#define P4_TIMER_STORAGE P4_SRAMDATA
#define P4_TIMER_FENCE() __asm__ volatile("fence iorw,iorw" ::: "memory")
#define P4_TIMER_READ(off) ((uint32_t)p4_r32(P4_SYSTIMER_BASE + (off)))
#define P4_TIMER_WRITE(off, value) p4_w32(P4_SYSTIMER_BASE + (off), (value))
static uint32_t p4_timer_mask(void)
{
    uint32_t status;
    __asm__ volatile("csrrci %0,mstatus,8" : "=r"(status) : : "memory");
    return status & MSTATUS_MIE;
}
#define P4_TIMER_MASK() p4_timer_mask()
#define P4_TIMER_RESTORE(saved) do { if (saved) \
    __asm__ volatile("csrsi mstatus,8" : : : "memory"); } while (0)
#else
#define P4_TIMER_STORAGE
#endif
#include "p4_public_spinlock.h"
#include "p4_timer_snapshot.h"
#include <aros/p4timer.h>

static spinlock_t p4_timer_gate P4_TIMER_STORAGE;
static unsigned int p4_timer_prepared P4_TIMER_STORAGE;

static const struct KrnP4TimerOps p4_timer_ops = {
    KRN_P4_TIMER_OPS_VERSION, sizeof(struct KrnP4TimerOps),
    P4_SYSTIMER_HZ, krnP4TimerSnapshotRead
};

const struct KrnP4TimerOps *krnP4TimerSnapshotOps(void)
{
    return &p4_timer_ops;
}

int krnP4TimerSnapshotPrepare(void)
{
    if (p4_timer_prepared || P4_CPU_LOCAL_READ_MHARTID() != 0 ||
        (csr_read(mstatus) & MSTATUS_MIE) != 0 || SysBase != NULL)
        return 0;
    if (krnP4PublicSpinInit(&p4_timer_gate) != P4_PUBLIC_SPIN_OK)
        return 0;
    P4_TIMER_FENCE();
    p4_timer_prepared = 1;
    return 1;
}

uint64_t krnP4TimerSnapshotRead(void)
{
    uint32_t saved = P4_TIMER_MASK();
    uint32_t hi, lo;
    unsigned int attempt;
    int locked = 0, valid = 0;

    if (SysBase == NULL)
    {
        /* Before publication the primary is the only running hart. */
        if (P4_CPU_LOCAL_READ_MHARTID() != 0)
            krnP4PublicSpinFatal(P4_PUBLIC_SPIN_BAD_STATE);
    }
    else
    {
        struct p4_exec_runtime *runtime = p4_exec_runtime_current(SysBase);
        if (!p4_timer_prepared || runtime == NULL || runtime->hart_id >= 2 ||
            runtime->cpu_local == NULL ||
            runtime->cpu_local->hart_id != runtime->hart_id)
            krnP4PublicSpinFatal(P4_PUBLIC_SPIN_BAD_STATE);
        for (attempt = 0; attempt < 20000u; ++attempt)
        {
            enum p4_public_spinlock_result result =
                krnP4PublicSpinTryAcquire(&p4_timer_gate, SPINLOCK_MODE_WRITE);
            if (result == P4_PUBLIC_SPIN_OK)
            {
                locked = 1;
                break;
            }
            if (result != P4_PUBLIC_SPIN_BUSY && result != P4_PUBLIC_SPIN_TRY_TIMEOUT)
                krnP4PublicSpinFatal(result);
        }
        if (!locked)
            krnP4PublicSpinFatal(P4_PUBLIC_SPIN_SERVICE_TIMEOUT);
    }

    P4_TIMER_WRITE(P4_ST_UNIT0_OP, P4_ST_UNIT0_UPDATE);
    P4_TIMER_FENCE();
    for (attempt = 0; attempt < 1000u; ++attempt)
        if (P4_TIMER_READ(P4_ST_UNIT0_OP) & P4_ST_UNIT0_VALID)
        {
            valid = 1;
            break;
        }
    /* A failed latch must not manufacture a stale EClock value. */
    hi = valid ? P4_TIMER_READ(P4_ST_UNIT0_VALUE_HI) : 0;
    lo = valid ? P4_TIMER_READ(P4_ST_UNIT0_VALUE_LO) : 0;
    P4_TIMER_FENCE();
    if (locked)
    {
        enum p4_public_spinlock_result result = krnP4PublicSpinUnlock(&p4_timer_gate);
        if (result != P4_PUBLIC_SPIN_OK)
            krnP4PublicSpinFatal(result);
    }
    if (!valid)
        krnP4PublicSpinFatal(P4_PUBLIC_SPIN_SERVICE_TIMEOUT);
    P4_TIMER_RESTORE(saved);
    return ((uint64_t)hi << 32) | lo;
}
