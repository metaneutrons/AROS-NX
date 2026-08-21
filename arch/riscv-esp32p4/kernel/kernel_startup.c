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
#include <exec/io.h>
#include <devices/timer.h>

#include <kernel_base.h>
#include <kernel_globals.h>
#include <kernel_romtags.h>
#include <tlsf.h>

#include "hardware.h"
#include "psram.h"
#include "kernel_intern.h"

/* Filled in by the link script; array form so the name is the address */
extern char __text_start[], __text_end[];
extern char __rodata_start[], __rodata_end[];
extern char __data_start[], __data_end[];
extern char __bss_start[], __bss_end[];
extern char __kernel_end[];
extern char __romtags_start[], __romtags_end[];
extern char __kernel_lowest[], __kernel_highest[];

/*
 * Nothing calls this for its effect. It exists so the report can say which
 * window a P4_SRAMCODE function actually landed in, in a build where the
 * rest of the code is mapped from flash - a claim about the link script
 * that is cheap to check and expensive to get wrong later.
 */
P4_SRAMCODE void krnSRAMResidencyCheck(void)
{
}

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
/* Set by the PSRAM bring-up, printed by the report */
static unsigned long __esp32p4_psram_size;

/* Set by clic_selftest() below, printed by the report */
static int clic_selftest_passed;

/*
 * Whether the last wait was served by the tick or timed out spinning, and
 * -1 while no wait has happened yet. The first report runs before the
 * first wait, and without the third state it says the tick is not running
 * when all that is true is that nothing has asked it to.
 */
static int timer_serving = -1;

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
    krnP4PutStr(timer_serving < 0 ? ", no wait measured yet\n"
                : timer_serving ? ", tick serving waits\n"
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

    krnP4PutStr("[kernel] sramfn ");
    krnP4PutHex32((uint32_t)(IPTR)krnSRAMResidencyCheck);
    krnP4PutStr(((IPTR)krnSRAMResidencyCheck >= P4_SRAM_BASE) ? "  in SRAM\n"
                                                             : "  NOT in SRAM\n");

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
    if (__esp32p4_psram_size)
    {
        krnP4PutStr("  ");
        krnP4PutDec((uint32_t)(__esp32p4_psram_size / (1024 * 1024)));
        krnP4PutStr(" MB, not yet mapped\n");
    }
    else
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
    /* The RAM the image sits in, which is not where its code is when the
       code is mapped from flash */
    tag->ti_Tag  = KRN_KernelLowest;
    tag->ti_Data = (IPTR)__kernel_lowest;
    tag++;
    tag->ti_Tag  = KRN_KernelHighest;
    tag->ti_Data = (IPTR)__kernel_highest;
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

#ifdef P4_TASK_TEST
/*
 * Two tasks at the same priority as exec's bootstrap task, each doing
 * nothing but counting. If both counters climb, the tick reached exec's
 * scheduler and the context switch works; if one stays put, the switch
 * never happened, and the counters say which task kept the machine.
 * Counting rather than printing, because two tasks writing to the
 * console interleave mid-string and the output would be the least
 * trustworthy part of the test.
 */
volatile unsigned long __esp32p4_taskbeat[2];


static void test_task_a(void)
{
    for (;;)
        __esp32p4_taskbeat[0]++;
}

/*
 * M4's other half wants a wait that ends when it should. Delay() lives in
 * dos.library, which this kickstart has not got, but what Delay() does
 * internally is available: a timerequest on timer.device's VBLANK unit.
 * Measured in ticks rather than in seconds, because the tick is the thing
 * under test, and 500000 microseconds at 100 Hz should be 50 of them.
 */
volatile unsigned long __esp32p4_delay_ticks;
volatile unsigned long __esp32p4_delay_rounds;

static void test_task_b(void)
{
    struct MsgPort *mp = CreateMsgPort();
    struct timerequest *tr = NULL;

    if (mp)
        tr = (struct timerequest *)AllocMem(sizeof(struct timerequest),
                                            MEMF_CLEAR | MEMF_PUBLIC);
    if (tr)
    {
        tr->tr_node.io_Message.mn_ReplyPort = mp;
        if (OpenDevice("timer.device", UNIT_VBLANK,
                       (struct IORequest *)tr, 0) != 0)
        {
            FreeMem(tr, sizeof(struct timerequest));
            tr = NULL;
        }
    }

    /* No timer.device to talk to: keep counting so the task still shows */
    if (!tr)
    {
        for (;;)
            __esp32p4_taskbeat[1]++;
    }

    tr->tr_node.io_Command = TR_ADDREQUEST;

    for (;;)
    {
        unsigned long before = __esp32p4_ticks;

        tr->tr_time.tv_secs  = 0;
        tr->tr_time.tv_micro = 500000;
        DoIO((struct IORequest *)tr);

        __esp32p4_delay_ticks = __esp32p4_ticks - before;
        __esp32p4_delay_rounds++;
        __esp32p4_taskbeat[1]++;
    }
}

static void test_task_exit(void)
{
    /* SysBase->TaskExitCode may be unset in a kickstart this small, and
       NewAddTask uses it when finalPC is NULL, so name one explicitly. */
    for (;;)
        ;
}

static struct Task *spawn_counter_task(const char *name, BYTE pri,
                                      void (*entry)(void))
{
    const ULONG stacksize = 4096;
    struct Task *t = AllocMem(sizeof(struct Task), MEMF_CLEAR | MEMF_PUBLIC);
    APTR stack = AllocMem(stacksize, MEMF_CLEAR);

    if (!t || !stack)
        return NULL;

    t->tc_Node.ln_Type = NT_TASK;
    t->tc_Node.ln_Pri  = pri;
    t->tc_Node.ln_Name = (char *)name;
    t->tc_SPLower      = stack;
    t->tc_SPUpper      = (APTR)((IPTR)stack + stacksize);

    /* NewAddTask fills in tc_SPReg, the MemEntry list and the ETask */
    return AddTask(t, (APTR)entry, (APTR)test_task_exit) ? t : NULL;
}

#endif /* P4_TASK_TEST */

#ifdef P4_PSRAM_PROBE
/*
 * Does anything answer in the PSRAM window before we have configured a
 * thing? The ESP-IDF second stage bootloader is what normally brings the
 * MSPI PSRAM up, and this image replaces that bootloader, so the honest
 * expectation is no. Worth asking rather than assuming, because the ROM
 * does configure MSPI far enough to fetch this image out of flash, and
 * whatever it left behind decides how much work M5 is.
 *
 * Build with -DP4_KEEP_WATCHDOG alongside this: an unconfigured external
 * bus can hang rather than fault, and a hang with the watchdogs armed
 * reboots and repeats its output instead of going quiet.
 */
static void psram_probe(void)
{
    volatile uint32_t *p = (volatile uint32_t *)P4_PSRAM_BASE;
    uint32_t first, second;
    int i;

    krnP4PutStr("[psram]  window ");
    krnP4PutHex32(P4_PSRAM_BASE);
    krnP4PutStr(" - ");
    krnP4PutHex32(P4_PSRAM_END);
    krnP4PutStr("\n[psram]  reading, a fault here is an answer too\n");

    first  = p[0];
    second = p[1];
    krnP4PutStr("[psram]  as found  ");
    krnP4PutHex32(first);
    krnP4PutStr(" ");
    krnP4PutHex32(second);

    p[0] = 0xA5A5A5A5;
    p[1] = 0x5A5A5A5A;
    krnP4PutStr("\n[psram]  read back ");
    krnP4PutHex32(p[0]);
    krnP4PutStr(" ");
    krnP4PutHex32(p[1]);
    krnP4PutStr((p[0] == 0xA5A5A5A5 && p[1] == 0x5A5A5A5A) ? "  HOLDS"
                                                           : "  does not hold");

    /*
     * If it holds, how much of it is real? An uninitialised or smaller
     * part answers the same bytes at several offsets, so walk powers of
     * two and say where the window starts repeating itself.
     */
    for (i = 1; i <= 32; i <<= 1)
    {
        volatile uint32_t *q = (volatile uint32_t *)(P4_PSRAM_BASE
                                                     + (unsigned long)i * 1024 * 1024);
        p[0] = 0x11111111;
        q[0] = 0x22222222;
        krnP4PutStr("\n[psram]  +");
        krnP4PutDec((uint32_t)i);
        krnP4PutStr(" MB ");
        krnP4PutStr(p[0] == 0x22222222 ? "aliases the base" : "is its own");
    }
    krnP4PutStr("\n");
}
#endif /* P4_PSRAM_PROBE */

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

    ranges[0] = (UWORD *)__romtags_start;
    ranges[1] = (UWORD *)__romtags_end;
    ranges[2] = (UWORD *)-1;

    {
        struct KernelBase *kb = getKernelBase();

        krnP4PutStr("[exec]   KernelBase @ ");
        krnP4PutHex32((uint32_t)(IPTR)kb);
        krnP4PutStr("  ContextSize ");
        krnP4PutDec(kb ? (uint32_t)kb->kb_ContextSize : 0);
        krnP4PutStr("\n");
    }

    krnDumpResidents((UWORD *)__romtags_start, (UWORD *)__romtags_end);

    krnP4PutStr("[exec]   preparing ExecBase\n");

    if (!krnPrepareExecBase(ranges, mh, krnPrepareBootTags()))
    {
        krnP4PutStr("[exec]   krnPrepareExecBase FAILED\n");
        return;
    }

    krnP4PutStr("[exec]   SysBase @ ");
    krnP4PutHex32((uint32_t)(IPTR)SysBase);
    krnP4PutStr("\n");

    /*
     * The rate the VBlank chain is raised at. timer.device reads it for
     * its EClock and derives VBlankTime from it, so a wrong value here
     * becomes wrong time everywhere later.
     */
    SysBase->VBlankFrequency = P4_TICK_HZ;

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

        krnP4PutStr("[exec]   KernelBase @ ");
        krnP4PutHex32((uint32_t)(IPTR)kb);
        krnP4PutStr("  ContextSize ");
        krnP4PutDec(kb ? (uint32_t)kb->kb_ContextSize : 0);
        krnP4PutStr("\n");
    }

    krnP4PutStr("[exec]   InitCode(RTF_COLDSTART)\n");
    InitCode(RTF_COLDSTART, 0);
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

#ifdef P4_TASK_TEST
    {
        /*
         * A spins at the bootstrap task's own priority; the waiter sits
         * above it, so that what the wait measures is the timer and not
         * how long a CPU-bound task at equal priority holds on to its
         * quantum afterwards.
         */
        struct Task *ta = spawn_counter_task("esp32p4 counter A", 0,
                                             test_task_a);
        struct Task *tb = spawn_counter_task("esp32p4 waiter B", 5,
                                             test_task_b);

        krnP4PutStr("[exec]   AddTask A ");
        krnP4PutHex32((uint32_t)(IPTR)ta);
        krnP4PutStr("  B ");
        krnP4PutHex32((uint32_t)(IPTR)tb);
        krnP4PutStr("\n");
    }
#endif /* P4_TASK_TEST */
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
    {
        struct P4PSRAMInfo psram;
        int up;

        /*
         * With interrupts off. ESP-IDF brings the PSRAM up before its
         * scheduler exists and so never has to think about this; here the
         * timer is already running by the time this code is reached, and a
         * bring-up sequence that can be interrupted between a controller
         * write and the transaction that depends on it is not a sequence.
         */
        csr_clear(mstatus, MSTATUS_MIE);
        up = krnPSRAMBringUp(&psram);
        csr_set(mstatus, MSTATUS_MIE);

        if (up)
        {
            krnP4PutStr("[psram]  chip   ");
            krnP4PutDec((uint32_t)(psram.size / (1024 * 1024)));
            krnP4PutStr(" MB at ");
            krnP4PutDec((uint32_t)(psram.clock_hz / 1000000));
            krnP4PutStr(" MHz, vendor ");
            krnP4PutHex32((uint32_t)psram.vendor);
            krnP4PutStr(", a word written and read back\n");
        }
        else if (!psram.mpll_up)
            krnP4PutStr("[psram]  chip   the mpll did not calibrate\n");
        else if (!psram.clock_hz)
            krnP4PutStr("[psram]  chip   the controller kept no clock\n");
        else
        {
            krnP4PutStr("[psram]  chip   no answer - vendor ");
            krnP4PutHex32((uint32_t)psram.vendor);
            krnP4PutStr(" mr2 ");
            krnP4PutHex32((uint32_t)psram.density);
            krnP4PutStr("\n");
        }

        __esp32p4_psram_size = psram.size;
    }
#ifdef P4_PSRAM_PROBE
    psram_probe();
#endif
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
            krnP4PutStr(timer_serving ? "  (tick)" : "  (spun)");
#ifdef P4_TASK_TEST
            krnP4PutStr("  tasks ");
            krnP4PutDec((uint32_t)__esp32p4_taskbeat[0]);
            krnP4PutStr("/");
            krnP4PutDec((uint32_t)__esp32p4_taskbeat[1]);
            krnP4PutStr("  wait ");
            krnP4PutDec((uint32_t)__esp32p4_delay_ticks);
            krnP4PutStr(" ticks x");
            krnP4PutDec((uint32_t)__esp32p4_delay_rounds);
#endif
            krnP4PutStr("\n");
        }
    }
}
