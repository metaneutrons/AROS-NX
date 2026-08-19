/*
    Copyright (c) 2026, The AROS Development Team. All rights reserved.

    Desc: Machine mode trap handling for the esp32p4-riscv target.
*/

#define __KERNEL_NOLIBBASE__

#include <inttypes.h>
#include <stddef.h>

#include <exec/types.h>
#include <exec/execbase.h>
#include <proto/exec.h>
#include <aros/riscv/cpucontext.h>
#include <asm/cpu.h>

#include "hardware.h"
#include "kernel_intern.h"
#include "kernel_cpu.h"

#include <kernel_globals.h>
#include <kernel_intr.h>
#include <kernel_scheduler.h>
#include <kernel_syscall.h>

#include <proto/kernel.h>

/*
 * The offsets traps.S stores the frame at. Asserted rather than trusted:
 * the two have to agree exactly, and nothing else would notice if they
 * stopped agreeing.
 */
#define CHK(field, off) \
    _Static_assert(offsetof(struct ExceptionContext, field) == (off), \
                   "traps.S stores " #field " at " #off)
CHK(x,          0);
CHK(ra,         0);
CHK(sp,         4);
CHK(t0,         8);
CHK(t1,        12);
CHK(t2,        16);
CHK(fp,        20);
CHK(s1,        24);
CHK(a0,        28);
CHK(a7,        56);
CHK(s2,        60);
CHK(s11,       96);
CHK(t3,       100);
CHK(t6,       112);
CHK(pc,       116);
CHK(sr,       120);
CHK(Flags,    124);
CHK(fpuContext, 128);
CHK(vecContext, 132);
_Static_assert(sizeof(struct ExceptionContext) == 136,
               "traps.S reserves 144 bytes for a 136 byte context plus alignment");
#undef CHK

/* The interrupt flag is the top bit, which on RV32 is bit 31 */
#define MCAUSE_INTERRUPT    (1UL << 31)

/* a7 carries the syscall number (see krnSysCall in kernel_cpu.h) */
#define CTX_REG_A7          14
#define SC_MAX              0x100   /* SC_REBOOT is the highest code */

/* What the dispatcher leaves for the scheduler, once the depth is down */
#define TRAP_DONE       0
#define TRAP_RESCHEDULE 1
#define TRAP_SYSCALL    2

/* Trap/interrupt nesting depth, reported through KrnIsSuper() */
extern int __esp32p4_trap_depth;

/* What has arrived, for the bring-up report to be able to say so */
volatile unsigned long __esp32p4_irq_count;
volatile unsigned long __esp32p4_irq_last;

static const char * const exc_names[] =
{
    "Instruction address misaligned",   /*  0 */
    "Instruction access fault",         /*  1 */
    "Illegal instruction",              /*  2 */
    "Breakpoint",                       /*  3 */
    "Load address misaligned",          /*  4 */
    "Load access fault",                /*  5 */
    "Store/AMO address misaligned",     /*  6 */
    "Store/AMO access fault",           /*  7 */
    "Environment call from U-mode",     /*  8 */
    "Environment call from S-mode",     /*  9 */
    NULL,                               /* 10 */
    "Environment call from M-mode",     /* 11 */
    "Instruction page fault",           /* 12 */
    "Load page fault",                  /* 13 */
    NULL,                               /* 14 */
    "Store/AMO page fault",             /* 15 */
};

/* Instruction-side faults: mepc itself may not be readable */
#define CAUSE_IS_IFETCH(c) \
    ((c) == CAUSE_MISALIGNED_FETCH || (c) == CAUSE_FETCH_ACCESS || \
     (c) == CAUSE_FETCH_PAGE_FAULT)

static void krnDumpContext(struct ExceptionContext *ctx)
{
    static const char * const regnames[] =
    {
        "ra ", "sp ", "t0 ", "t1 ", "t2 ", "fp ", "s1 ",
        "a0 ", "a1 ", "a2 ", "a3 ", "a4 ", "a5 ", "a6 ", "a7 ",
        "s2 ", "s3 ", "s4 ", "s5 ", "s6 ", "s7 ", "s8 ", "s9 ",
        "s10", "s11", "t3 ", "t4 ", "t5 ", "t6 "
    };
    int i;

    for (i = 0; i < RISCV_REGSAVE_CNT; i++)
    {
        krnP4PutStr(regnames[i]);
        krnP4PutStr("=");
        krnP4PutHex32(ctx->x[i]);
        krnP4PutStr(((i % 4) == 3) ? "\n" : "  ");
    }
    krnP4PutStr("\npc =");
    krnP4PutHex32(ctx->pc);
    krnP4PutStr("  sr =");
    krnP4PutHex32(ctx->sr);
    krnP4PutStr("\n");
}

/*
 * The instruction stream around mepc, as bytes, with the faulting parcel
 * marked. Enough to disassemble the crash site offline without the
 * binary at hand. 16-bit parcels are what both compressed and full
 * length instructions are built from, hence the pairing.
 */
static void krnDumpCode(struct ExceptionContext *ctx)
{
    unsigned char *pc = (unsigned char *)(ctx->pc & ~(IPTR)1);
    int i;

    if (!pc)
        return;

    krnP4PutStr("[trap] code  ");
    for (i = -16; i < 16; i++)
    {
        static const char hexchars[] = "0123456789abcdef";
        char pair[2];

        if (i == 0)
            krnP4PutStr("\n[trap] mepc> ");
        pair[0] = hexchars[(pc[i] >> 4) & 0xF];
        pair[1] = hexchars[pc[i] & 0xF];
        krnP4PutC(pair[0]);
        krnP4PutC(pair[1]);
        if ((i & 1) && i != 15)
            krnP4PutC(' ');
    }
    krnP4PutStr("\n");
}

/*
 * Name mepc, ra and every stack word the symbol resolver can attribute
 * to a module. With no frame chain to walk - this port omits frame
 * pointers - the scan overreports, but the real call chain is in there.
 */
static void krnDumpBacktrace(struct ExceptionContext *ctx)
{
    struct KernelBase *kbase = getKernelBase();
    APTR pcs[34];
    ULONG n = 0;

    if (!kbase)
        return;

    pcs[n++] = (APTR)ctx->pc;
    if (ctx->ra && ctx->ra != ctx->pc)
        pcs[n++] = (APTR)ctx->ra;

    n += KrnBacktraceFromFrame((APTR)ctx->sp, &pcs[n], 32 - n);

    KrnPrintBacktrace("[trap] ", pcs, n);
}

static void krnReportException(struct ExceptionContext *ctx,
                               unsigned long mcause, unsigned long mtval)
{
    const char *name = (mcause < sizeof(exc_names) / sizeof(exc_names[0]))
                        ? exc_names[mcause] : NULL;

    krnP4PutStr("\n[trap] ");
    if (name)
        krnP4PutStr(name);
    else
    {
        krnP4PutStr("Unknown exception ");
        krnP4PutDec(mcause);
    }
    krnP4PutStr("\n       mepc  = ");
    krnP4PutHex32(ctx->pc);
    krnP4PutStr("\n       mtval = ");
    krnP4PutHex32(mtval);
    krnP4PutStr("\n");

    krnDumpContext(ctx);

    /*
     * Name the faulting task, so a crash can be attributed to its owner,
     * and print its stack bounds: a runaway stack explains a trap frame
     * landing somewhere unlikely.
     */
    if (SysBase)
    {
        struct Task *t = FindTask(NULL);

        if (t)
        {
            krnP4PutStr("[trap] task '");
            krnP4PutStr(t->tc_Node.ln_Name ? t->tc_Node.ln_Name : "<unnamed>");
            krnP4PutStr("' @ ");
            krnP4PutHex32((IPTR)t);
            krnP4PutStr("\n[trap] stack ");
            krnP4PutHex32((IPTR)t->tc_SPLower);
            krnP4PutStr(" - ");
            krnP4PutHex32((IPTR)t->tc_SPUpper);
            krnP4PutStr(" sp ");
            krnP4PutHex32(ctx->sp);
            krnP4PutStr(((IPTR)ctx->sp < (IPTR)t->tc_SPLower ||
                         (IPTR)ctx->sp > (IPTR)t->tc_SPUpper)
                        ? " OUTSIDE\n" : " in bounds\n");
        }
    }

    if (!CAUSE_IS_IFETCH(mcause))
        krnDumpCode(ctx);
    krnDumpBacktrace(ctx);
}

static int krnTrapDispatch(struct ExceptionContext *ctx, unsigned long mcause,
                           unsigned long mtval)
{
    if (mcause & MCAUSE_INTERRUPT)
    {
        /*
         * With mtvec in direct mode the CLIC leaves the line number in
         * mcause, so no vector table is consulted to find out what
         * arrived. Acknowledging by clearing the pending bit is what
         * stops a level triggered line from re-entering immediately; an
         * edge triggered one has already cleared itself.
         *
         * Counting and remembering rather than printing: a line nobody
         * handles would otherwise flood the console faster than it could
         * be read. The bring-up report says how many arrived and which
         * was last. Dispatch to registered handlers, and the scheduler
         * call on the way out, arrive with kernel_timer.c.
         */
        unsigned long line = mcause & 0xFFF;

        /*
         * The peripheral is acknowledged before the controller, and it
         * matters in that order: the tick line is level triggered, so
         * while SYSTIMER still has its interrupt raised the line is
         * asserted again the moment the pending bit is cleared.
         */
        if (line == P4_TIMER_LINE)
        {
            krnTimerAck();
            __esp32p4_ticks++;
        }

        krnCLICClear(line);
        __esp32p4_irq_last = line;
        __esp32p4_irq_count++;

        /* Something may have become runnable; let the scheduler look on
           the way out */
        return TRAP_RESCHEDULE;
    }

    if (mcause == CAUSE_MACHINE_ECALL && SysBase &&
        ctx->x[CTX_REG_A7] <= SC_MAX)
    {
        /*
         * A scheduler syscall - KrnDispatch, KrnSwitch, KrnSchedule and
         * the rest - with the function code in a7. Step over the ecall,
         * which is always four bytes; it has no compressed form.
         */
        ctx->pc += 4;
        return TRAP_SYSCALL;
    }

    krnReportException(ctx, mcause, mtval);

    krnP4PutStr("[trap] fatal - halting hart.\n");
    for (;;)
        asm volatile("wfi");

    return TRAP_DONE;
}

void krnTrapHandler(struct ExceptionContext *ctx, unsigned long mcause,
                    unsigned long mtval)
{
    int action;

    __esp32p4_trap_depth++;
    action = krnTrapDispatch(ctx, mcause, mtval);

    /*
     * Only the outermost trap enters the scheduler. A trap taken while
     * the kernel is already inside one - which includes one taken while
     * the dispatcher itself is waiting - must not reschedule, or it
     * re-enters the dispatcher from inside itself. The depth is held
     * across the call so nesting stays visible to it, and released after.
     */
    if (SysBase && (__esp32p4_trap_depth == 1))
    {
        if (action == TRAP_RESCHEDULE)
            core_ExitInterrupt(ctx);
        else if (action == TRAP_SYSCALL)
            core_SysCall((int)ctx->x[CTX_REG_A7], ctx);
    }

    __esp32p4_trap_depth--;
}
