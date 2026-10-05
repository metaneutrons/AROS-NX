/*
    Copyright (c) 2026, The AROS Development Team. All rights reserved.

    Desc: the second hart in the smp variant of the esp32p4-riscv target
          (SMP.md, S3): its start, the inter-hart interrupt, the per-hart
          interrupt stacks and the park for cache-off windows.
*/

#include <aros/kernel.h>
#include <exec/execbase.h>
#include <exec/resident.h>
#include <exec/tasks.h>
#include <proto/exec.h>
#include <utility/tagitem.h>

#include <kernel_base.h>
#include <proto/kernel.h>
#include <exec_platform.h>

#include "etask.h"
#include "hardware.h"
#include "kernel_intern.h"
#include "kernel_ipi.h"
#include "secondary_hw.h"
#include "tls.h"

#define P4_ISR_STACK_BYTES      8192
#define P4_SMP_BOOT_STACK_BYTES 4096
#define P4_SMP_START_CYCLES     36000000UL      /* 100 ms at 360 MHz */
#define P4_SMP_PARK_CYCLES      360000000UL     /* 1 s */

extern struct Task *cpu_InitBootStrap(struct ExecBase *, APTR, APTR);
extern void cpu_BootStrap(struct Task *, struct ExecBase *);
extern unsigned char __p4_smp_entry[], __p4_smp_entry_end[];

/* Per hart, in internal SRAM; traps.S switches to it (see there). */
unsigned char __p4_isr_stack[P4_TLS_HARTS][P4_ISR_STACK_BYTES]
    __attribute__((aligned(16)));
/* Hart 1's first stack, which then stays its bootstrap task's. */
unsigned char __p4_smp_boot_stack[P4_SMP_BOOT_STACK_BYTES]
    __attribute__((aligned(16)));
/* For __p4_smp_entry: mtvec, mscratch, the C entry, the boot stack top. */
volatile ULONG __p4_smp_boot[4];

/* Work for a hart, raised with its inter-hart interrupt. */
P4_SRAMDATA volatile ULONG __p4_ipi_work[P4_TLS_HARTS];
/* The cache-off window: requested by one hart, acknowledged by the other. */
P4_SRAMDATA volatile ULONG __p4_park_request;
P4_SRAMDATA volatile ULONG __p4_park_ack;
/* Windows the other hart has waited out, for the bring-up report */
P4_SRAMDATA volatile ULONG __p4_parks;

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

static void update(unsigned long reg, ULONG mask, ULONG value)
{
    p4_w32(reg, (p4_r32(reg) & ~mask) | value);
    fence();
}

/* Hart 0's own interrupt stack; called once before exec starts. */
void krnP4SMPInitPrimary(void)
{
    asm volatile("csrw mscratch, %0" ::
                 "r"(&__p4_isr_stack[0][P4_ISR_STACK_BYTES]));
}

/* SRAM: also called from inside a cache-off window's preparation. */
P4_SRAMCODE void krnP4IPISend(unsigned int hart, ULONG work)
{
    __atomic_fetch_or(&__p4_ipi_work[hart], work, __ATOMIC_SEQ_CST);
    fence();
    p4_w32(P4_SMP_IPI_FROM(hart), 1);
}

/*
 * Wait in SRAM, on the interrupt stack, with interrupts masked, until the
 * other hart has its cache-off window behind it. The branch predictor is
 * switched off meanwhile, as krnP4CacheOff() does on the hart that owns
 * the window, so nothing fetches speculatively from flash.
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

/* Line 22, on the hart it was aimed at */
void krnP4IPIInterrupt(void)
{
    unsigned int me = GetCPUNumber();
    ULONG work;

    /* The latch first: the matrix source stays raised while it is set */
    p4_w32(P4_SMP_IPI_FROM(me), 0);
    fence();
    krnCLICClear(P4_SMP_IPI_LINE);
    work = __atomic_exchange_n(&__p4_ipi_work[me], 0, __ATOMIC_SEQ_CST);

    if (work & P4_IPI_PARK)
        park_here();

    /* The quantum, counted as exec's VBlankServer counts it on hart 0 */
    if (work & P4_IPI_TICK)
    {
        UWORD current = SCHEDELAPSED_GET;

        if (current)
            SCHEDELAPSED_SET(--current);
        if (current == 0)
        {
            FLAG_SCHEDQUANTUM_SET;
            FLAG_SCHEDSWITCH_SET;
        }
    }

    /* Something became ready for this hart. The quantum is given up as
       well, or a running task of equal priority would never yield. */
    if (work & P4_IPI_SCHEDULE)
    {
        FLAG_SCHEDQUANTUM_SET;
        FLAG_SCHEDSWITCH_SET;
    }

    if (work & P4_IPI_CALL_HOOK)
        core_RunCallIPIs(me);
}

/* From hart 0's tick: the other harts have no timer interrupt of their own */
void krnP4TickOthers(void)
{
    if (__atomic_load_n(&__p4_harts_online, __ATOMIC_ACQUIRE) & (1UL << 1))
        krnP4IPISend(1, P4_IPI_TICK);
}

/*
 * Before a cache-off window: the other hart, if it runs, waits in SRAM.
 * The caller has interrupts masked. 0 if it did not answer within a
 * second; the window must then not be opened.
 */
P4_SRAMCODE int krnP4ParkOthers(void)
{
    unsigned int other = 1 - GetCPUNumber();
    unsigned long start;

    if (!(__atomic_load_n(&__p4_harts_online, __ATOMIC_ACQUIRE) & (1UL << other)))
        return 1;

    __atomic_store_n(&__p4_park_request, 1, __ATOMIC_RELEASE);
    krnP4IPISend(other, P4_IPI_PARK);
    start = cycles();
    while (!__atomic_load_n(&__p4_park_ack, __ATOMIC_ACQUIRE))
    {
        if (cycles() - start > P4_SMP_PARK_CYCLES)
        {
            __atomic_store_n(&__p4_park_request, 0, __ATOMIC_RELEASE);
            return 0;
        }
    }
    __p4_parks++;
    return 1;
}

P4_SRAMCODE void krnP4UnparkOthers(void)
{
    __atomic_store_n(&__p4_park_request, 0, __ATOMIC_RELEASE);
}

/* Hart 1's idle task: only its inter-hart interrupt wakes it. */
static void p4_IdleTask(struct ExecBase *SysBase)
{
    (void)SysBase;

    for (;;)
        asm volatile("wfi");
}

/*
 * Hart 1 in C, on its boot stack, with interrupts masked. It becomes a
 * hart of the scheduler through a bootstrap task, registers, and creates
 * its idle task, which runs at once: the bootstrap's -128 is lower.
 */
void krnP4SecondaryMain(void)
{
    struct Task *bstask, *idle;
    void *aff;

    krnCLICInit();
    krnCLICEnable(P4_SMP_IPI_LINE, 0);

    bstask = cpu_InitBootStrap(SysBase, __p4_smp_boot_stack,
                               __p4_smp_boot_stack + P4_SMP_BOOT_STACK_BYTES);
    if (!bstask)
        goto halt;

    IDNESTCOUNT_SET(-1);
    TDNESTCOUNT_SET(-1);
    cpu_BootStrap(bstask, SysBase);
    __atomic_fetch_or(&__p4_harts_online, 1UL << 1, __ATOMIC_SEQ_CST);

    aff = KrnAllocCPUMask();
    if (!aff)
        goto halt;
    KrnGetCPUMask(1, aff);
    idle = NewCreateTask(TASKTAG_NAME, (IPTR)"CPU #01 Idle",
                         TASKTAG_AFFINITY, (IPTR)aff,
                         TASKTAG_PRI, -127,
                         TASKTAG_PC, (IPTR)p4_IdleTask,
                         TASKTAG_ARG1, (IPTR)SysBase,
                         TAG_DONE);
    if (idle)
    {
        /* Only reached if the idle task did not take over at once */
        for (;;)
            KrnSwitch();
    }

halt:
    /* Without a task to be, stay out of the way; a park is still served */
    for (;;)
        asm volatile("wfi");
}

/*
 * Start hart 1 from internal SRAM (__p4_smp_entry, secondary_smp.S) with
 * the release sequence E1 qualified. It sees only its inter-hart
 * interrupt; hart 0 gets its own as well. TRUE once hart 1 has registered.
 */
static BOOL p4_smp_start_secondary(void)
{
    unsigned long mtvec, start;
    unsigned int s;

    asm volatile("csrr %0, mtvec" : "=r"(mtvec));
    __p4_smp_boot[0] = mtvec;
    __p4_smp_boot[1] = (ULONG)(IPTR)&__p4_isr_stack[1][P4_ISR_STACK_BYTES];
    __p4_smp_boot[2] = (ULONG)(IPTR)krnP4SecondaryMain;
    __p4_smp_boot[3] = (ULONG)(IPTR)&__p4_smp_boot_stack[P4_SMP_BOOT_STACK_BYTES];
    fence();
    krnP4CacheWritebackData((void *)__p4_smp_boot, sizeof(__p4_smp_boot));
    krnP4SyncCode(__p4_smp_entry, __p4_smp_entry_end - __p4_smp_entry);

    for (s = 0; s < P4_SMP_INTMTX_SOURCES; s++)
        p4_w32(P4_SMP_INTMTX_ROUTE(1, s),
               s == P4_SMP_IPI_SOURCE(1) ? P4_SMP_IPI_LINE : 0);
    p4_w32(P4_SMP_IPI_FROM(1), 0);
    __p4_ipi_work[1] = 0;

    /* And hart 0 its own, which only its peer raises: signals to hart 0's
       tasks from hart 1 travel this way. Nothing else of hart 0's is on
       line 22. */
    p4_w32(P4_SMP_IPI_FROM(0), 0);
    __p4_ipi_work[0] = 0;
    p4_w32(P4_SMP_INTMTX_ROUTE(0, P4_SMP_IPI_SOURCE(0)), P4_SMP_IPI_LINE);
    krnCLICEnable(P4_SMP_IPI_LINE, 0);

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

    start = cycles();
    while (!(__atomic_load_n(&__p4_harts_online, __ATOMIC_ACQUIRE) & (1UL << 1)))
    {
        if (cycles() - start > P4_SMP_START_CYCLES)
        {
            update(P4_SECONDARY_RESET_REG, P4_SECONDARY_RESET_BIT,
                   P4_SECONDARY_RESET_BIT);
            update(P4_SECONDARY_CLOCK_REG, P4_SECONDARY_CLOCK_BIT, 0);
            return FALSE;
        }
    }
    return TRUE;
}

/*
 * A COLDSTART resident at 104: after exec.library (120), which creates the
 * boot task, and before the rest of the cold start. A start from
 * krnStartExec() is too early, there is no task to be yet.
 */
AROS_UFH3(static APTR, p4smp_init,
          AROS_UFPA(void *, dummy, D0),
          AROS_UFPA(BPTR, segList, A0),
          AROS_UFPA(struct ExecBase *, SysBase, A6))
{
    AROS_USERFUNC_INIT

    struct Task *idle = NULL;
    unsigned long start;

    krnP4ConsoleBlocking();
    if (!p4_smp_start_secondary())
        krnP4PutStr("[smp] hart 1 did not come online; it stays held\n");
    else
    {
        /* Its idle task appears once hart 1 has created it */
        start = cycles();
        while (!idle && cycles() - start < P4_SMP_START_CYCLES)
        {
            Forbid();
            idle = FindTask("CPU #01 Idle");
            Permit();
        }
        krnP4PutStr("[smp] hart 1 online, cpus ");
        krnP4PutDec(KrnGetCPUCount());
        krnP4PutStr(idle ? ", idling in 'CPU #01 Idle'\n"
                         : ", but its idle task did not appear\n");
    }
    krnP4ConsoleRuntime();
    return NULL;

    AROS_USERFUNC_EXIT
}

const struct Resident p4smp_resident =
{
    RTC_MATCHWORD,
    (struct Resident *)&p4smp_resident,
    (APTR)((const char *)&p4smp_resident + sizeof(struct Resident)),
    RTF_COLDSTART,
    1,
    NT_UNKNOWN,
    104,
    "esp32p4 smp",
    "esp32p4 smp 1.0",
    &p4smp_init
};
