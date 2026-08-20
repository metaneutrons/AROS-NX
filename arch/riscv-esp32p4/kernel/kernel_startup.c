/*
    Copyright (c) 2026, The AROS Development Team. All rights reserved.

    Desc: Machine mode bring-up for the esp32p4-riscv target.

    Entered from startup.S with the hart number in a0 and, for now,
    nothing in a1; the second argument is where a built-in device tree
    would arrive if this platform grows one.

    The order it works in: silence the watchdogs the ROM armed, bring up
    the CLIC and the 100 Hz tick, build the memory list, hand the machine
    to exec. Then it reports what it found and keeps repeating that
    report, because the USB console only carries what a host is attached
    for and a one-shot line is lost to whoever was not listening yet.
*/

#define __KERNEL_NOLIBBASE__

#include <inttypes.h>

#include <exec/types.h>
#include <asm/cpu.h>

#include <exec/execbase.h>
#include <exec/lists.h>
#include <exec/resident.h>
#include <exec/memory.h>
#include <aros/kernel.h>
#include <utility/tagitem.h>
#include <proto/exec.h>

#include <kernel_base.h>
#include <kernel_globals.h>
#include <kernel_romtags.h>
#include <tlsf.h>

#include "hardware.h"
#include "kernel_intern.h"

/* Filled in by the link script; array form so the name is the address */
extern char __text_start[], __text_end[];
extern char __rodata_start[], __rodata_end[];
extern char __data_start[], __data_end[];
extern char __bss_start[], __bss_end[];
extern char __kernel_end[];

static void report_extent(const char *what, const void *from, const void *to)
{
    krnP4PutStr("[kernel] ");
    krnP4PutStr(what);
    krnP4PutStr(" ");
    krnP4PutHex32((uint32_t)(IPTR)from);
    krnP4PutStr(" - ");
    krnP4PutHex32((uint32_t)(IPTR)to);
    krnP4PutStr("  ");
    krnP4PutDec((uint32_t)((IPTR)to - (IPTR)from));
    krnP4PutStr(" bytes\n");
}

/*
 * misa names the implemented extensions, one bit per letter from A at
 * bit 0. It reads as zero on an implementation that chose not to provide
 * it, which is legal and says nothing about what the machine can do.
 */
static void report_misa(void)
{
    unsigned long misa = csr_read(misa);
    int i;

    krnP4PutStr("[kernel] misa   ");
    krnP4PutHex32((uint32_t)misa);
    if (!misa)
    {
        krnP4PutStr("  (not implemented)\n");
        return;
    }

    krnP4PutStr("  rv32");
    for (i = 0; i < 26; i++)
    {
        if (misa & (1UL << i))
            krnP4PutC('a' + i);
    }
    krnP4PutStr("\n");
}

/*
 * Repeated rather than said once, and the reason is the console. The USB
 * serial/JTAG peripheral only accepts data while a host is attached and
 * reading, and krnP4PutC drops what it cannot hand over, so a report
 * given at boot - before the host has enumerated the device - is given
 * to nobody. Saying it again every so often means a listener can attach
 * whenever it likes and still learn the whole state.
 */
/* Set by clic_selftest() below, printed by the report */
static int clic_selftest_passed;

/* Whether the last wait was served by the tick or timed out spinning */
static int timer_serving;

/*
 * Does an interrupt actually arrive? Raise a line nobody uses by hand,
 * with machine interrupts briefly enabled, and see whether the trap
 * handler counted it. Proves the whole path - CLIC configuration, mtvec,
 * the trap entry, mcause carrying the line number - before anything
 * depends on it.
 */
#define CLIC_TEST_LINE  (P4_CLIC_LINES - 1)

static int clic_pended_ok;      /* did the pending bit take the write */

static void clic_selftest(void)
{
    unsigned long before = __esp32p4_irq_count;
    volatile unsigned long spin;

    /*
     * Edge triggered, not level. For a level triggered line the pending
     * bit mirrors the input and software cannot raise it; only an edge
     * triggered one is software settable, which is what makes a check
     * like this possible at all.
     */
    krnCLICEnable(CLIC_TEST_LINE, 1);
    csr_set(mstatus, MSTATUS_MIE);
    krnCLICPend(CLIC_TEST_LINE);
    clic_pended_ok = krnCLICPending(CLIC_TEST_LINE) ||
                     __esp32p4_irq_count > before;

    for (spin = 0; spin < 1000 && __esp32p4_irq_count == before; spin++)
        ;

    csr_clear(mstatus, MSTATUS_MIE);
    krnCLICDisable(CLIC_TEST_LINE);

    clic_selftest_passed = (__esp32p4_irq_count > before) &&
                           (__esp32p4_irq_last == CLIC_TEST_LINE);
}

static void report(unsigned long hartid)
{
    krnP4PutStr("\nAROS/esp32p4-riscv\n");

    krnP4PutStr("[kernel] wdt    ");
    krnP4PutStr(platform_wdt_quiet() ? "timer groups and low power watchdogs off, "
                                       "super watchdog self-feeding\n"
                                     : "STILL ARMED - expect a reset\n");

    krnP4PutStr("[kernel] clic   ");
    krnP4PutStr(clic_selftest_passed ? "raised line reached the trap handler"
                : clic_pended_ok     ? "line pended but was NOT delivered"
                                     : "line would NOT pend");
    krnP4PutStr(", irqs seen ");
    krnP4PutDec((uint32_t)__esp32p4_irq_count);
    krnP4PutStr(", last line ");
    krnP4PutDec((uint32_t)__esp32p4_irq_last);
    krnP4PutStr("\n");

    krnP4PutStr("[kernel] timer  ");
    krnP4PutDec(P4_TICK_HZ);
    krnP4PutStr(" Hz on clic line ");
    krnP4PutDec(P4_TIMER_LINE);
    krnP4PutStr(", ticks ");
    krnP4PutDec((uint32_t)__esp32p4_ticks);
    krnP4PutStr(", counter ");
    krnP4PutDec((uint32_t)krnTimerCount());
    krnP4PutStr(timer_serving ? ", tick serving waits\n"
                              : ", TICK NOT RUNNING - waits are spinning\n");

    krnP4PutStr("[kernel] hart   ");
    krnP4PutDec((uint32_t)hartid);
    krnP4PutStr("\n");

    report_misa();

    krnP4PutStr("[kernel] vendor ");
    krnP4PutHex32((uint32_t)csr_read(mvendorid));
    krnP4PutStr("  arch ");
    krnP4PutHex32((uint32_t)csr_read(marchid));
    krnP4PutStr("  impl ");
    krnP4PutHex32((uint32_t)csr_read(mimpid));
    krnP4PutStr("\n");

    report_extent("text  ", __text_start, __text_end);
    report_extent("rodata", __rodata_start, __rodata_end);
    report_extent("data  ", __data_start, __data_end);
    report_extent("bss   ", __bss_start, __bss_end);

    krnRAMReport();

    krnP4PutStr("[kernel] sram   ");
    krnP4PutHex32(P4_SRAM_BASE);
    krnP4PutStr(" - ");
    krnP4PutHex32(P4_SRAM_END);
    krnP4PutStr("\n[kernel] psram  ");
    krnP4PutHex32(P4_PSRAM_BASE);
    krnP4PutStr(" - ");
    krnP4PutHex32(P4_PSRAM_END);
    krnP4PutStr("  (not brought up)\n");

    /*
     * The report outlives the state it was first written for, so it says
     * what is true when it runs rather than what was true when it was
     * written: exec either took the machine or it did not.
     */
    if (SysBase)
    {
        struct KernelBase *kb = getKernelBase();

        krnP4PutStr("[kernel] exec   SysBase ");
        krnP4PutHex32((uint32_t)(IPTR)SysBase);
        krnP4PutStr("  KernelBase ");
        krnP4PutHex32((uint32_t)(IPTR)kb);
        krnP4PutStr("  memory list handed over\n");
    }
    else
        krnP4PutStr("[kernel] exec   not reached - no SysBase, no memory list\n");
}

/*
 * What the kernel tells exec about the machine. The minimum here: where
 * this image is, so exec does not hand it out, and what the heap is.
 * There is no device tree and no command line to pass on.
 */
static struct TagItem BootTags[8];

static struct TagItem *krnPrepareBootTags(void)
{
    struct TagItem *tag = BootTags;

    tag->ti_Tag  = KRN_KernelBase;
    tag->ti_Data = (IPTR)__text_start;
    tag++;
    tag->ti_Tag  = KRN_KernelLowest;
    tag->ti_Data = (IPTR)__text_start;
    tag++;
    tag->ti_Tag  = KRN_KernelHighest;
    tag->ti_Data = (IPTR)__kernel_end;
    tag++;
    tag->ti_Tag  = KRN_MEMLower;
    tag->ti_Data = (IPTR)__kernel_end;
    tag++;
    tag->ti_Tag  = KRN_MEMUpper;
    tag->ti_Data = P4_HEAP_LOW_END;
    tag++;
    tag->ti_Tag  = KRN_BootLoader;
    tag->ti_Data = (IPTR)"ESP32-P4 ROM";
    tag++;
    tag->ti_Tag  = KRN_DebugInfo;
    tag->ti_Data = 0;
    tag++;
    tag->ti_Tag  = TAG_DONE;
    tag->ti_Data = 0;

    return BootTags;
}

/*
 * Every resident in the kickstart, in the order the scan meets them, with
 * the priority that decides the order they are initialised in. If exec
 * comes up before kernel.resource, the priorities say so here.
 */
static void krnDumpResidents(UWORD *lo, UWORD *hi)
{
    UWORD *p;

    krnP4PutStr("[boot]   residents:\n");

    for (p = lo; p < hi; p++)
    {
        struct Resident *res = (struct Resident *)p;

        if (*p != RTC_MATCHWORD || res->rt_MatchTag != res)
            continue;

        krnP4PutStr("[boot]     ");
        krnP4PutHex32((uint32_t)(IPTR)res);
        krnP4PutStr("  pri ");
        krnP4PutDec((uint32_t)(int)(signed char)res->rt_Pri);
        krnP4PutStr("  type ");
        krnP4PutDec((uint32_t)res->rt_Type);
        krnP4PutStr("  flags ");
        krnP4PutHex32((uint32_t)res->rt_Flags);
        krnP4PutStr("  ");
        krnP4PutStr(res->rt_Name ? (const char *)res->rt_Name : "(unnamed)");
        krnP4PutStr("\n");

        p = (UWORD *)((IPTR)res->rt_EndSkip - 2);
    }
}

/*
 * Hand the machine to exec. Everything above this point exists to make
 * this call possible: a heap it can allocate from, the extent of the
 * image it must not hand out, and a tick to schedule on.
 */
static void krnStartExec(void)
{
    struct MemHeader *mh = __esp32p4_mh_low;
    UWORD *ranges[3];

    if (!mh)
    {
        krnP4PutStr("[exec]   no usable heap - cannot start exec\n");
        return;
    }

    /* TLSF, as the other modern ports do. The converted header is placed
       at the first free chunk, so the returned pointer is not the region
       base. */
    {
        struct MemHeader *tmh = krnConvertMemHeaderToTLSF(mh);

        if (tmh)
        {
            mh = tmh;
            krnP4PutStr("[exec]   TLSF allocator enabled\n");
        }
    }

    ranges[0] = (UWORD *)__text_start;
    ranges[1] = (UWORD *)__kernel_end;
    ranges[2] = (UWORD *)-1;

    {
        struct KernelBase *kb = getKernelBase();

        krnP4PutStr("[exec]   KernelBase @ ");
        krnP4PutHex32((uint32_t)(IPTR)kb);
        krnP4PutStr("  ContextSize ");
        krnP4PutDec(kb ? (uint32_t)kb->kb_ContextSize : 0);
        krnP4PutStr("\n");
    }

    krnDumpResidents((UWORD *)__text_start, (UWORD *)__kernel_end);

    krnP4PutStr("[exec]   preparing ExecBase\n");

    if (!krnPrepareExecBase(ranges, mh, krnPrepareBootTags()))
    {
        krnP4PutStr("[exec]   krnPrepareExecBase FAILED\n");
        return;
    }

    krnP4PutStr("[exec]   SysBase @ ");
    krnP4PutHex32((uint32_t)(IPTR)SysBase);
    krnP4PutStr("\n");

    /* Exec owns a memory list now, so the data-only region can join it */
    if (__esp32p4_mh_high)
    {
        Enqueue(&SysBase->MemList, &__esp32p4_mh_high->mh_Node);
        krnP4PutStr("[exec]   data region added to the memory list\n");
    }

    /*
     * What exec thinks it has, before anything asks for it. The alert
     * that follows a failed allocation says only that one failed, not
     * which or from where - and the two candidate explanations, too
     * little memory overall and a pool that only the smaller region
     * qualifies for, are told apart by these four numbers.
     */
    {
        struct MemHeader *m;

        krnP4PutStr("[exec]   avail  ANY ");
        krnP4PutDec((uint32_t)AvailMem(MEMF_ANY));
        krnP4PutStr("  KICK ");
        krnP4PutDec((uint32_t)AvailMem(MEMF_KICK));
        krnP4PutStr("  LOCAL ");
        krnP4PutDec((uint32_t)AvailMem(MEMF_LOCAL));
        krnP4PutStr("  largest ");
        krnP4PutDec((uint32_t)AvailMem(MEMF_ANY | MEMF_LARGEST));
        krnP4PutStr("\n");

        ForeachNode(&SysBase->MemList, m)
        {
            krnP4PutStr("[exec]   mh '");
            krnP4PutStr(m->mh_Node.ln_Name ? m->mh_Node.ln_Name : "?");
            krnP4PutStr("' ");
            krnP4PutHex32((uint32_t)(IPTR)m->mh_Lower);
            krnP4PutStr(" - ");
            krnP4PutHex32((uint32_t)(IPTR)m->mh_Upper);
            krnP4PutStr(" free ");
            krnP4PutDec((uint32_t)m->mh_Free);
            krnP4PutStr(" attr ");
            krnP4PutHex32((uint32_t)m->mh_Attributes);
            krnP4PutStr("\n");
        }
    }

    krnP4PutStr("[exec]   InitCode(RTF_SINGLETASK)\n");
    InitCode(RTF_SINGLETASK, 0);
    /*
     * Again, now that the resident scan has run. Before the handover this
     * is necessarily zero and says nothing; what matters is whether
     * kernel.resource came up before exec's own init needs a context
     * size from it.
     */
    {
        struct KernelBase *kb = getKernelBase();

        ULONG want = sizeof(struct ExceptionContext) + 15
                   + sizeof(struct FpuContext);

        krnP4PutStr("[exec]   KernelBase @ ");
        krnP4PutHex32((uint32_t)(IPTR)kb);
        krnP4PutStr("  ContextSize ");
        krnP4PutDec(kb ? (uint32_t)kb->kb_ContextSize : 0);
        krnP4PutStr("  want ");
        krnP4PutDec((uint32_t)want);

        /*
         * An experiment, not a fix. If the field is implausible then
         * cpu_Init has not run yet, KrnCreateContext will allocate
         * nothing and exec's init will fail claiming no memory. Filling
         * it in here says whether that really is the whole story - and
         * if it is, the thing to repair is the order the two modules
         * initialise in, not this.
         */
        if (kb && (kb->kb_ContextSize < want || kb->kb_ContextSize > 4096))
        {
            kb->kb_ContextSize = want;
            krnP4PutStr("  -> forced");
        }
        krnP4PutStr("\n");
    }

    krnP4PutStr("[exec]   InitCode(RTF_COLDSTART)\n");
    InitCode(RTF_COLDSTART, 0);

    /*
     * With only kernel.resource, exec.library and task.resource in the
     * kickstart there is nothing to take the machine over, so InitCode
     * returns. Show that exec answers through its LVO table.
     */
    {
        struct Task *me = FindTask(NULL);
        APTR mem;

        krnP4PutStr("[exec]   ThisTask = ");
        krnP4PutStr((me && me->tc_Node.ln_Name) ? me->tc_Node.ln_Name
                                                : "(unnamed)");
        krnP4PutStr("\n[exec]   AvailMem(MEMF_ANY) = ");
        krnP4PutDec((uint32_t)AvailMem(MEMF_ANY));
        krnP4PutStr("\n");

        mem = AllocMem(64 << 10, MEMF_ANY | MEMF_CLEAR);
        krnP4PutStr("[exec]   AllocMem(64K) = ");
        krnP4PutHex32((uint32_t)(IPTR)mem);
        krnP4PutStr("\n");
        if (mem)
            FreeMem(mem, 64 << 10);
    }
}

void kernel_cstart(unsigned long hartid, void *fdt)
{
    unsigned long beat = 0;

    (void)fdt;

    /*
     * platform_init() comes after a line of output, not before, so that a
     * hang inside it is still attributable to it.
     */
    krnP4PutStr("\n\n[kernel] entered\n");
#ifndef P4_KEEP_WATCHDOG
    platform_init();
#endif

    krnCLICInit();
    clic_selftest();

    krnTimerInit();
    csr_set(mstatus, MSTATUS_MIE);

    krnRAMInit();
    krnRAMReport();
    krnStartExec();

#ifdef P4_KEEP_WATCHDOG
    /*
     * Diagnostic build: the watchdog is left armed through the bring-up
     * so that a hang restarts the board and the whole sequence is said
     * again. A single boot cannot be caught over this console.
     */
    platform_init();
#endif

    report(hartid);

    for (;;)
    {
        /* One second, from the tick if the tick is running */
        timer_serving = krnTimerWait(P4_TICK_HZ);

        if ((++beat & 15) == 0)
            report(hartid);
        else
        {
            krnP4PutStr("[kernel] alive ");
            krnP4PutDec((uint32_t)beat);
            krnP4PutStr("  ticks ");
            krnP4PutDec((uint32_t)__esp32p4_ticks);
            krnP4PutStr("  counter ");
            krnP4PutDec((uint32_t)krnTimerCount());
            krnP4PutStr("  irqs ");
            krnP4PutDec((uint32_t)__esp32p4_irq_count);
            krnP4PutStr(" on line ");
            krnP4PutDec((uint32_t)__esp32p4_irq_last);
            krnP4PutStr(timer_serving ? "  (tick)\n" : "  (spun)\n");
        }
    }
}
