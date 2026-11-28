/*
    Copyright (c) 2026, The AROS Development Team. All rights reserved.
    $Id$

    Desc: The esp32p4-riscv platform under riscv-native.

    riscv-native holds the context switch, the trap dispatch, the SMP
    scheduler and the IPI core. This file fills its hook table
    (kernel_riscv.h) for the ESP32-P4 and translates what differs:

    - Interrupts arrive through Espressif's CLIC. mcause carries the CLIC
      line in its low twelve bits rather than the standard interrupt codes,
      so krnP4TrapHandler() maps the tick line to RV_IRQ_TIMER, the inter-
      hart line to RV_IRQ_SOFT and every other line to RV_IRQ_EXT before it
      calls riscv-native's krnTrapHandler().
    - There is no time CSR. The time is SYSTIMER's counter (RVI_ReadTime).
    - Only hart 0 has a tick. Hart 1 is ticked by hart 0 through its
      inter-hart interrupt, as the platform did before riscv-native.
    - Flash operations close the cache for a moment, and with it PSRAM. The
      other hart then has to wait in internal SRAM; it is asked to through
      the inter-hart interrupt (krnP4ParkOthers()), so traps are taken on
      a stack in internal SRAM.
*/

#define __KERNEL_NOLIBBASE__

#include <aros/kernel.h>
#include <exec/execbase.h>
#include <hardware/intbits.h>
#include <asm/cpu.h>

#include <kernel_base.h>
#include <kernel_debug.h>
#include <kernel_interrupts.h>
#include <kernel_intr.h>
#include <exec_platform.h>

#include "kernel_riscv.h"
#include "hardware.h"
#include "secondary_hw.h"
#include "kernel_intern.h"

extern void krnTrapHandler(struct ExceptionContext *ctx, unsigned long cause,
                           unsigned long tval);
extern void __attribute__((noreturn)) cpu_Register(void);
extern char __p4_smp_entry[], __p4_smp_entry_end[];

#define P4_HARTS                2
#define P4_SMP_PARK_CYCLES      360000000UL     /* 1 s at 360 MHz */

/* Work for a hart, raised with its inter-hart interrupt, besides
   riscv-native's own (which riscv-native keeps itself) */
#define P4_IPI_TICK             (1UL << 0)
#define P4_IPI_PARK             (1UL << 1)

/*
 * Hart 1's trap stack, in internal SRAM. riscv-native gives a secondary
 * hart one from AllocMem(), which may be PSRAM, and a hart waiting out a
 * cache-off window on such a stack cannot reach it. Hart 0's is
 * riscv-native's __riscv_boot_trapstack, which is in internal SRAM.
 */
#define P4_TRAP_STACK_BYTES     8192
static unsigned char __p4_trap_stack[P4_TRAP_STACK_BYTES]
    __attribute__((aligned(16)));

/* For __p4_smp_entry (secondary_smp.S): mtvec, hart 1's TLS, the C entry */
volatile ULONG __p4_smp_boot[3];

P4_SRAMDATA volatile ULONG __p4_ipi_work[P4_HARTS];
P4_SRAMDATA volatile ULONG __p4_harts_online = 1;
/* The cache-off window: requested by one hart, acknowledged by the other.
   The request holds 1 + the requesting hart, so that a hart that waits for
   a lock can tell a request from its peer from its own. */
P4_SRAMDATA volatile ULONG __p4_park_request;
P4_SRAMDATA volatile ULONG __p4_park_ack;

/* The device line each hart is handling, for RVI_ExternalIRQ */
static ULONG p4_irq_line[P4_HARTS];

P4_ALWAYS_INLINE unsigned long cycles(void)
{
    unsigned long n;

    asm volatile("csrr %0, mcycle" : "=r"(n));
    return n;
}

P4_ALWAYS_INLINE void fence(void)
{
    asm volatile("fence iorw, iorw" ::: "memory");
}

P4_ALWAYS_INLINE unsigned int p4_hart(void)
{
    unsigned long hart;

    asm volatile("csrr %0, mhartid" : "=r"(hart));
    return (unsigned int)hart;
}

static void update(unsigned long reg, ULONG mask, ULONG value)
{
    p4_w32(reg, (p4_r32(reg) & ~mask) | value);
    fence();
}

/* SRAM: also called from inside a cache-off window's preparation. */
P4_SRAMCODE static void p4_IPISend(unsigned int hart, ULONG work)
{
    if (work)
        __atomic_fetch_or(&__p4_ipi_work[hart], work, __ATOMIC_SEQ_CST);
    fence();
    p4_w32(P4_SMP_IPI_FROM(hart), 1);
}

/*
 * Wait in SRAM, on the trap stack, with interrupts masked, until the other
 * hart has its cache-off window behind it. The branch predictor is switched
 * off meanwhile, as krnP4CacheOff() does on the hart that owns the window,
 * so nothing fetches speculatively from flash.
 */
P4_SRAMCODE static void park_here(void)
{
    unsigned long predictor;

    asm volatile("csrr %0, 0x7c1" : "=r"(predictor));
    asm volatile("csrc 0x7c1, %0" :: "r"((1UL << 4) | (1UL << 5) | (1UL << 12)));
    __atomic_store_n(&__p4_park_ack, 1, __ATOMIC_RELEASE);
    fence();
    while (__atomic_load_n(&__p4_park_request, __ATOMIC_ACQUIRE))
        ;
    asm volatile("csrs 0x7c1, %0" ::
                 "r"(predictor & ((1UL << 4) | (1UL << 5) | (1UL << 12))));
    asm volatile("fence.i" ::: "memory");
    __atomic_store_n(&__p4_park_ack, 0, __ATOMIC_RELEASE);
}

/*
 * Before a cache-off window: the other hart, if it runs, waits in SRAM.
 * The caller has interrupts masked. 0 if it did not answer within a
 * second; the window must then not be opened.
 */
P4_SRAMCODE int krnP4ParkOthers(void)
{
    unsigned int other = 1 - p4_hart();
    unsigned long start;

    if (!(__atomic_load_n(&__p4_harts_online, __ATOMIC_ACQUIRE) & (1UL << other)))
        return 1;

    __atomic_store_n(&__p4_park_request, 1 + (1 - other), __ATOMIC_RELEASE);
    p4_IPISend(other, P4_IPI_PARK);
    start = cycles();
    while (!__atomic_load_n(&__p4_park_ack, __ATOMIC_ACQUIRE))
    {
        if (cycles() - start > P4_SMP_PARK_CYCLES)
        {
            __atomic_store_n(&__p4_park_request, 0, __ATOMIC_RELEASE);
            return 0;
        }
    }
    return 1;
}

P4_SRAMCODE void krnP4UnparkOthers(void)
{
    __atomic_store_n(&__p4_park_request, 0, __ATOMIC_RELEASE);
}

/*
 * For a hart that waits with interrupts masked, which cannot take the
 * park interrupt: serve a request of the other hart from here. Without
 * this a hart spinning for a lock that the requester holds would keep the
 * requester waiting until the window is refused.
 */
P4_SRAMCODE void krnP4ParkServe(void)
{
    ULONG request = __atomic_load_n(&__p4_park_request, __ATOMIC_ACQUIRE);

    if (request && request != 1 + p4_hart())
        park_here();
}

/*
 * The trap entry's C half (traps.S): translate the CLIC's cause into the
 * standard one riscv-native dispatches on.
 */
void krnP4TrapHandler(struct ExceptionContext *ctx, unsigned long mcause,
                      unsigned long mtval)
{
    unsigned long code = mcause & 0xFFF;
    unsigned long cause = code;
    int clear = 0;

    if (mcause & RV_CAUSE_INTERRUPT)
    {
        unsigned int me = p4_hart();

        if (code == P4_TIMER_LINE)
        {
            cause = RV_CAUSE_INTERRUPT | RV_IRQ_TIMER;
            clear = 1;
        }
        else if (code == P4_SMP_IPI_LINE)
        {
            ULONG work;

            /* The latch first: the matrix source stays raised while it
               is set */
            p4_w32(P4_SMP_IPI_FROM(me), 0);
            fence();
            krnCLICClear(P4_SMP_IPI_LINE);
            work = __atomic_exchange_n(&__p4_ipi_work[me], 0, __ATOMIC_SEQ_CST);

            if (work & P4_IPI_PARK)
                park_here();

            /* Hart 0's tick, passed on: count this hart's quantum */
            if ((work & P4_IPI_TICK) && SysBase)
            {
                UWORD current = SCHEDELAPSED_GET;

                krnWdtBeat(me);
                if (current)
                    SCHEDELAPSED_SET(--current);
                if (current == 0)
                {
                    FLAG_SCHEDQUANTUM_SET;
                    FLAG_SCHEDSWITCH_SET;
                }
            }
            cause = RV_CAUSE_INTERRUPT | RV_IRQ_SOFT;
        }
        else
        {
            p4_irq_line[me] = code;
            cause = RV_CAUSE_INTERRUPT | RV_IRQ_EXT;
            clear = 1;
        }
    }

    krnTrapHandler(ctx, cause, mtval);

    /*
     * The peripheral was acknowledged in the handler, before the
     * controller here, and it matters in that order: a level triggered
     * line is asserted again the moment its pending bit is cleared while
     * the source is still raised.
     */
    if (clear)
        krnCLICClear(code);
}

/* RVI_TimerTick, on hart 0 only: hart 1 has no tick line */
static void p4_TimerTick(struct ExceptionContext *ctx)
{
    (void)ctx;

    krnTimerAck();
    __esp32p4_ticks++;
    krnWdtTick();

    /*
     * What counts hart 0's quantum down is exec's VBlankServer on
     * INTB_VERTB; timer.device hangs off the same chain. Skipped while
     * interrupts are disabled, as the server would then run at the wrong
     * moment.
     */
    if (SysBase && (IDNESTCOUNT_GET < 0))
        core_Cause(INTB_VERTB, 1L << INTB_VERTB);

    /* Hart 1 counts its quantum from this tick too */
    if (__atomic_load_n(&__p4_harts_online, __ATOMIC_ACQUIRE) & (1UL << 1))
        p4_IPISend(1, P4_IPI_TICK);
}

/* RVI_ExternalIRQ: the line krnP4TrapHandler() found */
static void p4_ExternalIRQ(void)
{
    struct KernelBase *KernelBase = getKernelBase();

    if (KernelBase)
        krnRunIRQHandlers(KernelBase, p4_irq_line[p4_hart()]);
}

/* RVI_SendIPI */
static void p4_SendIPI(const ULONG *mask)
{
    unsigned int cpu;

    for (cpu = 0; cpu < P4_HARTS; cpu++)
        if (mask[0] & (1UL << cpu))
            p4_IPISend(cpu, 0);
}

/* RVI_AckIPI: krnP4TrapHandler() has already cleared the latch */
static void p4_AckIPI(void)
{
}

/* RVI_InitCore, on the new hart before it schedules */
static void p4_InitCore(struct KernelBase *KernelBase, struct ExecBase *SysBase)
{
    (void)KernelBase;
    (void)SysBase;

    krnCLICInit();
    krnCLICEnable(P4_SMP_IPI_LINE, 0);
    __atomic_fetch_or(&__p4_harts_online, 1UL << p4_hart(), __ATOMIC_SEQ_CST);
}

/* RVI_RemoteSFence: there is no MMU to fence */
static void p4_RemoteSFence(const ULONG *mask, IPTR va, IPTR size)
{
    (void)mask;
    (void)va;
    (void)size;
}

/* RVI_StopCPU */
static void p4_StopCPU(void)
{
    __atomic_fetch_and(&__p4_harts_online, ~(1UL << p4_hart()), __ATOMIC_SEQ_CST);
    csr_clear(mstatus, MSTATUS_MIE);
    for (;;)
        asm volatile("wfi");
}

/* RVI_ReadTime */
static UQUAD p4_ReadTime(void)
{
    return krnTimerCount();
}

/*
 * RVI_StartCPU: start hart 1 from internal SRAM (__p4_smp_entry,
 * secondary_smp.S) with the release sequence ESP-IDF uses. It sees only
 * its inter-hart interrupt.
 */
static int p4_StartCPU(uint32_t cpu, tls_t *tls)
{
    unsigned long mtvec;
    unsigned int s;

    if (cpu >= P4_HARTS)
        return -1;

    tls->HartID = cpu;
    tls->StackTop = &__p4_trap_stack[P4_TRAP_STACK_BYTES];

    asm volatile("csrr %0, mtvec" : "=r"(mtvec));
    __p4_smp_boot[0] = mtvec;
    __p4_smp_boot[1] = (ULONG)(IPTR)tls;
    __p4_smp_boot[2] = (ULONG)(IPTR)cpu_Register;
    fence();
    krnP4CacheWritebackData((void *)__p4_smp_boot, sizeof(__p4_smp_boot));
    krnP4CacheWritebackData(tls, sizeof(*tls));
    krnP4SyncCode(__p4_smp_entry, __p4_smp_entry_end - __p4_smp_entry);

    for (s = 0; s < P4_SMP_INTMTX_SOURCES; s++)
        p4_w32(P4_SMP_INTMTX_ROUTE(cpu, s),
               s == P4_SMP_IPI_SOURCE(cpu) ? P4_SMP_IPI_LINE : 0);
    p4_w32(P4_SMP_IPI_FROM(cpu), 0);
    __p4_ipi_work[cpu] = 0;

    /* Held, in this order, as at boot */
    update(P4_SECONDARY_RESET_REG, P4_SECONDARY_RESET_BIT, P4_SECONDARY_RESET_BIT);
    update(P4_SECONDARY_CLOCK_REG, P4_SECONDARY_CLOCK_BIT, 0);
    update(P4_SECONDARY_STALL_REG, P4_SECONDARY_STALL_MASK, P4_SECONDARY_STALL);
    p4_w32(P4_SECONDARY_BOOT_REG, 0);
    fence();

    /* Released in ESP-IDF's order; the boot address is written last */
    update(P4_SECONDARY_STALL_REG, P4_SECONDARY_STALL_MASK, P4_SECONDARY_UNSTALL);
    update(P4_SECONDARY_CLOCK_REG, P4_SECONDARY_CLOCK_BIT, P4_SECONDARY_CLOCK_BIT);
    update(P4_SECONDARY_RESET_REG, P4_SECONDARY_RESET_BIT, 0);
    p4_w32(P4_SECONDARY_BOOT_REG, (ULONG)(IPTR)__p4_smp_entry);
    fence();

    return 0;
}

/*
 * Fill riscv-native's hook table and give hart 0 its own inter-hart line,
 * which only its peer raises. Called once on hart 0,
 * before exec starts.
 */
void krnP4PlatformHooks(void)
{
    __riscv_arosintern.RVI_StartCPU      = p4_StartCPU;
    __riscv_arosintern.RVI_SendIPI       = p4_SendIPI;
    __riscv_arosintern.RVI_AckIPI        = p4_AckIPI;
    __riscv_arosintern.RVI_InitCore      = p4_InitCore;
    __riscv_arosintern.RVI_InitTimerCore = NULL;
    __riscv_arosintern.RVI_RemoteSFence  = p4_RemoteSFence;
    __riscv_arosintern.RVI_StopCPU       = p4_StopCPU;
    __riscv_arosintern.RVI_TimebaseFreq  = P4_SYSTIMER_HZ;
    __riscv_arosintern.RVI_ReadTime      = p4_ReadTime;
    __riscv_arosintern.RVI_TimerTick     = p4_TimerTick;
    __riscv_arosintern.RVI_ExternalIRQ   = p4_ExternalIRQ;

    p4_w32(P4_SMP_IPI_FROM(0), 0);
    __p4_ipi_work[0] = 0;
    p4_w32(P4_SMP_INTMTX_ROUTE(0, P4_SMP_IPI_SOURCE(0)), P4_SMP_IPI_LINE);
    krnCLICEnable(P4_SMP_IPI_LINE, 0);
}

/*
 * A lock between harts for state that also interrupt handlers touch: the
 * holder runs with interrupts masked, so nothing preempts it, and a hart
 * that waits serves the other's park request meanwhile. Returns the
 * interrupt state for krnP4LockRelease(). Not recursive.
 */
P4_SRAMCODE unsigned long krnP4LockTake(volatile ULONG *lock)
{
    unsigned long state = p4_irq_mask();

    while (__atomic_exchange_n(lock, 1, __ATOMIC_ACQUIRE))
    {
        while (__atomic_load_n(lock, __ATOMIC_RELAXED))
            krnP4ParkServe();
    }
    return state;
}

P4_SRAMCODE void krnP4LockRelease(volatile ULONG *lock, unsigned long state)
{
    __atomic_store_n(lock, 0, __ATOMIC_RELEASE);
    p4_irq_unmask(state);
}
