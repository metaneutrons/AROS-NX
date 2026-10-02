/*
 * IRQ-safe asynchronous P4 scheduler mailbox candidate.
 * Shared pending fields are accessed only under the prepared leaf gate.
 * Only the target hart writes its CPU-local runtime, after draining.
 * Selection/trap/startup wiring awaits qualification; no hart is launched here.
 */
#if !defined(P4_IPI_TEST)
#include <exec/execbase.h>
#include <proto/exec.h>
#include <asm/cpu.h>
#include "kernel_intern.h"
#include "hardware.h"
#include "cpu_local_runtime.h"
#include "../exec/p4_signal_protocol.h"
#include "p4_task_pins.h"
#if !defined(__AROSEXEC_SMP__) || !defined(__AROSPLATFORM_SMP__)
#error "P4 IPI requires matched Exec SMP"
#endif
#define P4_IPI_STORAGE P4_SRAMDATA
#define P4_IPI_FENCE() __asm__ volatile("fence iorw,iorw" ::: "memory")
#define P4_IPI_LATCH(h, value) p4_w32(0x500e5010u + (h) * 4u, (value))
#define P4_IPI_ROUTE(h) p4_ipi_route(h)
#else
#define P4_IPI_STORAGE
#endif
#include "p4_public_spinlock.h"
#include "p4_ipi.h"

#if !defined(P4_IPI_TEST) || defined(P4_IPI_ROUTE_TEST)
static void p4_ipi_route(uint32_t hart)
{
    uint32_t source;
    /* Reserve only this local line. Remove retained foreign routes to it
     * and the opposite destination latch, preserving every other route. */
    for (source = 0; source < 128u; ++source)
    {
        uint32_t address = 0x500d6000u + hart * 0x800u + source * 4u;
        uint32_t value = (uint32_t)p4_r32(address);
        if (source == 79u + hart)
            p4_w32(address, P4_IPI_LINE);
        else if ((value & 0x3fu) == P4_IPI_LINE || source == 80u - hart)
            p4_w32(address, 0);
    }
}
#endif

struct p4_ipi_slot
{
    uint32_t schedule_flags;
    uint32_t ticks;
    uint32_t armed;
    uint8_t reserved[52];
} __attribute__((aligned(64)));
_Static_assert(sizeof(struct p4_ipi_slot) == 64, "IPI slot cache-line size");

static struct p4_ipi_slot p4_ipi_slots[2] P4_IPI_STORAGE;
static spinlock_t p4_ipi_gate P4_IPI_STORAGE;
static unsigned int p4_ipi_prepared P4_IPI_STORAGE;

int krnP4IpiPrepare(void)
{
    if (p4_ipi_prepared || (csr_read(mstatus) & MSTATUS_MIE) != 0 ||
        P4_CPU_LOCAL_READ_MHARTID() != 0)
        return 0;
    if (krnP4PublicSpinInit(&p4_ipi_gate) != P4_PUBLIC_SPIN_OK)
        return 0;
    /* BSS state is private while the secondary is reset-held. */
    p4_ipi_slots[0].schedule_flags = p4_ipi_slots[1].schedule_flags = 0;
    p4_ipi_slots[0].ticks = p4_ipi_slots[1].ticks = 0;
    p4_ipi_slots[0].armed = p4_ipi_slots[1].armed = 0;
    P4_IPI_FENCE();
    p4_ipi_prepared = 1;
    return 1;
}

static uint32_t p4_ipi_lock_disabled(struct ExecBase *SysBase)
{
    struct p4_exec_runtime *runtime;
    unsigned int attempt;
    runtime = p4_exec_runtime_current(SysBase);
    if ((csr_read(mstatus) & MSTATUS_MIE) != 0 || !p4_ipi_prepared ||
        runtime == NULL || runtime->hart_id >= 2 ||
        runtime->cpu_local == NULL ||
        runtime->cpu_local->hart_id != runtime->hart_id)
        krnP4PublicSpinFatal(P4_PUBLIC_SPIN_BAD_STATE);
    for (attempt = 0; attempt < 20000u; ++attempt)
    {
        enum p4_public_spinlock_result result =
            krnP4PublicSpinTryAcquire(&p4_ipi_gate, SPINLOCK_MODE_WRITE);
        if (result == P4_PUBLIC_SPIN_OK)
            return runtime->hart_id;
        if (result != P4_PUBLIC_SPIN_BUSY && result != P4_PUBLIC_SPIN_TRY_TIMEOUT)
            krnP4PublicSpinFatal(result);
    }
    krnP4PublicSpinFatal(P4_PUBLIC_SPIN_SERVICE_TIMEOUT);
}

static uint32_t p4_ipi_lock(struct ExecBase *SysBase)
{
    Disable();
    return p4_ipi_lock_disabled(SysBase);
}

static void p4_ipi_unlock_disabled(struct ExecBase *SysBase)
{
    enum p4_public_spinlock_result result;
    (void)SysBase;
    P4_IPI_FENCE(); /* Complete MMIO before another hart can drain/latch. */
    result = krnP4PublicSpinUnlock(&p4_ipi_gate);
    if (result != P4_PUBLIC_SPIN_OK)
        krnP4PublicSpinFatal(result);
}

static void p4_ipi_unlock(struct ExecBase *SysBase)
{
    p4_ipi_unlock_disabled(SysBase);
    Enable();
}

void krnP4IpiArm(struct ExecBase *SysBase)
{
    uint32_t hart;
    struct p4_exec_runtime *runtime;
    struct p4_ipi_slot *slot;
    if ((csr_read(mstatus) & MSTATUS_MIE) != 0)
        krnP4PublicSpinFatal(P4_PUBLIC_SPIN_BAD_STATE);
    runtime = p4_exec_runtime_current(SysBase);
    if (runtime == NULL || runtime->cpu_local == NULL ||
        runtime->hart_id >= 2 ||
        runtime->cpu_local->hart_id != runtime->hart_id)
        krnP4PublicSpinFatal(P4_PUBLIC_SPIN_BAD_STATE);
    /* Before taking the IPI leaf gate, prove the permanent worker/mask ACK.
     * The future secondary entry must retain outer Disable nesting through
     * arm; the registry query's balanced Enable must not reopen MIE here.
     * Primary early boot does not depend on a not-yet-created ServiceTask. */
    if (runtime->hart_id == 1 &&
        (runtime->cpu_local->id_nest < 0 ||
         !krnP4CleanupWorkerReady(SysBase)))
        krnP4PublicSpinFatal(P4_PUBLIC_SPIN_BAD_STATE);
    hart = p4_ipi_lock_disabled(SysBase);
    slot = &p4_ipi_slots[hart];
    if (slot->armed)
        krnP4PublicSpinFatal(P4_PUBLIC_SPIN_BAD_STATE);
    /* Runtime/CLIC are initialized before this owner-only arm; never touch
     * the remote CLIC alias while its clock may be gated. */
    P4_IPI_LATCH(hart, 0);
    P4_IPI_ROUTE(hart);
    krnCLICClear(P4_IPI_LINE);
    krnCLICEnable(P4_IPI_LINE, 0);
    slot->armed = 1;
    P4_IPI_FENCE();
    if (slot->schedule_flags != 0)
        P4_IPI_LATCH(hart, 1);
    p4_ipi_unlock_disabled(SysBase);
}

struct p4_ipi_batch krnP4IpiDrain(struct ExecBase *SysBase)
{
    struct p4_ipi_batch batch;
    struct p4_ipi_slot *slot;
    uint32_t hart;
    if ((csr_read(mstatus) & MSTATUS_MIE) != 0)
        krnP4PublicSpinFatal(P4_PUBLIC_SPIN_BAD_STATE);
    hart = p4_ipi_lock_disabled(SysBase);
    slot = &p4_ipi_slots[hart];
    if (!slot->armed)
        krnP4PublicSpinFatal(P4_PUBLIC_SPIN_BAD_STATE);
    /* Both clears precede work consumption under the same gate. A new
     * producer can only publish/latch after this snapshot is detached. */
    P4_IPI_LATCH(hart, 0);
    krnCLICClear(P4_IPI_LINE);
    P4_IPI_FENCE();
    batch.schedule_flags = slot->schedule_flags;
    batch.ticks = slot->ticks;
    slot->schedule_flags = 0;
    slot->ticks = 0;
    p4_ipi_unlock_disabled(SysBase);
    return batch;
}

static void p4_ipi_post_schedule_mask(uint32_t destinations,
                                     struct ExecBase *SysBase)
{
    uint32_t destination;
    if (destinations == 0 || (destinations & ~UINT32_C(3)) != 0)
        krnP4PublicSpinFatal(P4_PUBLIC_SPIN_BAD_ARGUMENT);
    (void)p4_ipi_lock(SysBase);
    for (destination = 0; destination < 2; ++destination)
    {
        struct p4_ipi_slot *slot;
        if ((destinations & (UINT32_C(1) << destination)) == 0)
            continue;
        slot = &p4_ipi_slots[destination];
        slot->schedule_flags |= P4_EXEC_SCHEDULE_SWITCH | P4_EXEC_SCHEDULE_DISPATCH;
        P4_IPI_FENCE();
        if (slot->armed)
            P4_IPI_LATCH(destination, 1);
    }
    p4_ipi_unlock(SysBase);
}

static void p4_ipi_post_schedule(uint32_t destination,
                                struct ExecBase *SysBase)
{
    if (destination >= 2)
        krnP4PublicSpinFatal(P4_PUBLIC_SPIN_BAD_ARGUMENT);
    p4_ipi_post_schedule_mask(UINT32_C(1) << destination, SysBase);
}

void krnP4SignalPoke(const struct p4_signal_effect *effect,
                     struct ExecBase *SysBase)
{
    if (effect == NULL || effect->status != P4_SIGNAL_STATUS_OK ||
        !effect->request_reschedule)
        return;
    p4_ipi_post_schedule(effect->reschedule_hart, SysBase);
}

void krnP4CleanupPoke(struct ExecBase *SysBase)
{
    /* Either admitted hart may deliver the durable wake at a safe boundary.
     * Publish both mailboxes in one masked transaction; unarmed destinations
     * retain pending flags without accessing their CLIC/MMIO latch. */
    p4_ipi_post_schedule_mask(UINT32_C(3), SysBase);
}

void krnP4RetiredOwnerPoke(uint32_t destination, struct ExecBase *SysBase)
{
    p4_ipi_post_schedule(destination, SysBase);
}

void krnP4IpiPostTick(uint32_t destination, struct ExecBase *SysBase)
{
    uint32_t hart;
    struct p4_ipi_slot *slot;
    if (destination >= 2)
        krnP4PublicSpinFatal(P4_PUBLIC_SPIN_BAD_ARGUMENT);
    hart = p4_ipi_lock(SysBase);
    if (hart != 0 || destination != 1)
        krnP4PublicSpinFatal(P4_PUBLIC_SPIN_BAD_STATE);
    slot = &p4_ipi_slots[destination];
    if (slot->armed)
    {
        if (slot->ticks == UINT32_MAX)
            krnP4PublicSpinFatal(P4_PUBLIC_SPIN_BAD_STATE);
        ++slot->ticks;
        P4_IPI_FENCE();
        P4_IPI_LATCH(destination, 1);
    }
    p4_ipi_unlock(SysBase);
}
