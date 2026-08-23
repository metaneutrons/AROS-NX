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
#include <exec/libraries.h>
#include <exec/resident.h>
#include <exec/memory.h>
#include <aros/kernel.h>
#include <aros/asmcall.h>
#include <utility/tagitem.h>
#include <proto/exec.h>
#include <exec/io.h>
#include <devices/timer.h>
#ifdef P4_SDCARD_DEVICE_TEST
#include <devices/trackdisk.h>
#include <devices/newstyle.h>
#endif
#ifdef P4_PARTITION_TEST
#include <libraries/partition.h>
#include <proto/partition.h>
#endif
#ifdef P4_DOS_PROBE
#include <aros/bootloader.h>
#include <resources/filesysres.h>
#include <proto/bootloader.h>
#endif
#ifdef P4_AFTERDOS_PROBE
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/filehandler.h>
#include <proto/dos.h>
#endif

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
/* Not static: the exec arch layer's CacheClearU() has no range of its own
   and needs to know how much of the external window is real. */
unsigned long __esp32p4_psram_size;
static struct MemHeader *__esp32p4_mh_psram;
static UWORD *__esp32p4_modules_low;
static UWORD *__esp32p4_modules_high;

#ifdef P4_PARTITION_TEST
struct PartitionBase *PartitionBase;
#ifdef P4_DOS_PROBE
/* GetBootInfo() reaches the resource through this, the same way the
   partition test reaches partition.library through PartitionBase. */
APTR BootLoaderBase;
#endif
#endif

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
        krnP4PutStr(" MB, mapped and verified\n");
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
/*
 * Room for every tag below plus TAG_DONE, with slack.  A boot tag list that
 * runs off its own end is not diagnosable from the far side: the resource
 * that reads it walks until TAG_DONE and finds whatever follows in memory.
 */
static struct TagItem BootTags[12];

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
#ifdef P4_CMDLINE
    /*
     * The kernel command line, which this platform has no firmware source
     * for: there is no bootloader to be handed one by and no NVS entry is
     * read yet, so the only honest place for it is the build.
     *
     * bootloader.resource splits this on whitespace into the argument list
     * dosboot reads, so the words here are the boot arguments.  It matters
     * which: `nomonitors nocomposition` together are what let dos.library
     * skip C:AROSMonDrvs, which does not exist in the flash package, and
     * `econsole` is what gives it a console when no display driver does.
     */
    tag->ti_Tag  = KRN_CmdLine;
    tag->ti_Data = (IPTR)P4_CMDLINE;
    tag++;
#endif
    tag->ti_Tag  = KRN_DebugInfo;
    tag->ti_Data = (IPTR)__ks_debuginfo;
    tag++;
    tag->ti_Tag  = TAG_DONE;
    tag->ti_Data = 0;

    /*
     * And the same list where kernel.resource hands it out.  Passing it to
     * krnPrepareExecBase() is not enough: that reaches exec, while
     * KrnGetBootInfo() returns this global, and bootloader.resource reads
     * only the latter.  Until this was set, that resource came up with no
     * loader name and no arguments no matter what the tags said, which is
     * how the command line looked plumbed and was not.  The sibling
     * riscv-native/sifive_u port assigns it in the same place.
     */
    BootMsg = BootTags;

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
        krnP4PutDecS((int32_t)(signed char)res->rt_Pri);
        krnP4PutStr("  type ");
        krnP4PutDec((uint32_t)res->rt_Type);
        krnP4PutStr("  flags ");
        krnP4PutHex32((uint32_t)res->rt_Flags);
        krnP4PutStr("  ");
        krnP4PutStr(res->rt_Name ? (const char *)res->rt_Name : "(unnamed)");
        krnP4PutStr("\n");

        /*
         * Match krnScanResidents(): most modules place EndSkip after the
         * RomTag, but secondary residents may live in a later section and
         * still point at the module's text-end marker.  Never follow such a
         * pointer backwards or this diagnostic walk will rediscover the
         * same resident forever before exec gets a chance to start.
         */
        if ((IPTR)res->rt_EndSkip > (IPTR)p)
            p = (UWORD *)((IPTR)res->rt_EndSkip - 2);
    }
}

#if defined(P4_TASK_TEST) || defined(P4_HEARTBEAT_TASK)
static void krnP4TaskExit(void)
{
    /* SysBase->TaskExitCode may be unset in a kickstart this small, and
       NewAddTask uses it when finalPC is NULL, so name one explicitly. */
    for (;;)
        ;
}

static struct Task *krnP4SpawnTask(const char *name, BYTE pri,
                                   void (*entry)(void), ULONG stacksize)
{
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
    return AddTask(t, (APTR)entry, (APTR)krnP4TaskExit) ? t : NULL;
}
#endif /* P4_TASK_TEST || P4_HEARTBEAT_TASK */

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

#endif /* P4_TASK_TEST */

#ifdef P4_HEARTBEAT_TASK

#ifndef P4_HEARTBEAT_SECS
#define P4_HEARTBEAT_SECS 5
#endif

/*
 * The heartbeat, as a task started from a resident rather than as a loop in
 * kernel_cstart().
 *
 * That loop only runs because nothing before it takes the machine over.
 * dosboot.resource does: its COLDSTART init either boots or retries for
 * ever, so from the moment it joins the package the loop is unreachable and
 * with it the only statement this port makes that the system is still alive.
 * The A4 gate asks for exactly that statement, so it has to move somewhere a
 * boot cannot take away.
 *
 * Two wrong attempts came before this one and are worth recording, because
 * each looked reasonable and each failed for a different reason.
 *
 * Spawning the task before InitCode(RTF_COLDSTART) at priority -20: the task
 * never ran at all.  The bootstrap task's own loop calls krnTimerWait(),
 * which is a busy spin at priority 0, and Reschedule() only ever picks a
 * ready task of equal or higher priority, so nothing below zero is scheduled
 * while that loop runs.
 *
 * The same, at priority 5: the boot stopped dead after
 * "InitCode(RTF_COLDSTART)".  timer.device initialises inside that pass, in
 * the bootstrap task at priority 0, while the new task sat above it in a
 * retry loop waiting for timer.device to appear.  It could not appear.  A
 * plain priority inversion, and entirely self-inflicted.
 *
 * What both attempts got wrong is the moment, not the priority.  Started
 * from a COLDSTART resident ordered after timer.device (50) and before both
 * `SDCard boot wait` (-49) and dosboot (-50), the task finds timer.device
 * already there, opens it once, and from then on only ever blocks in DoIO().
 * It needs no retry loop.
 *
 * The priority took a third attempt.  At 5 it beat correctly until dosboot
 * was added, after which it opened timer.device, issued one request and was
 * never seen again.  What starves it is econsole: DOS starts a handler
 * process at dn_Priority 10, and ECON:'s Raw_Read() has no way to block, so
 * it spins on RawMayGetChar() and Reschedule() at that priority for as long
 * as a shell waits for a keystroke.  KrnMayGetChar() has no implementation on
 * this platform and always returns -1, so that is for ever, and nothing
 * below priority 10 runs again once a shell reaches its prompt.  20 is above
 * every handler DOS starts and below nothing that matters.
 */
static void krnP4HeartbeatTask(void)
{
    struct MsgPort *mp;
    struct timerequest *tr = NULL;
    unsigned long beat = 0;

    /* Three statements before anything can go wrong, because the first
       version of this printed nothing at all in a dosboot build and there
       was no way to tell how far it had got. */
    Forbid();
    krnP4PutStr("[beat]   task running\n");
    Permit();

    mp = CreateMsgPort();

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

    if (!tr)
    {
        /*
         * Say so once and stop.  There is no way to wait without the timer
         * that does not spin, and a heartbeat that starves the system it is
         * reporting on is worse than one that admits it cannot report.
         */
        Forbid();
        krnP4PutStr("[beat]   no timer.device, heartbeat not running\n");
        Permit();
        return;
    }

    tr->tr_node.io_Command = TR_ADDREQUEST;

    Forbid();
    krnP4PutStr("[beat]   timer.device open, port ");
    krnP4PutHex32((uint32_t)(IPTR)mp);
    krnP4PutStr(" sigbit ");
    krnP4PutDec((uint32_t)mp->mp_SigBit);
    krnP4PutStr("\n");
    Permit();

    for (;;)
    {
        tr->tr_time.tv_secs  = P4_HEARTBEAT_SECS;
        tr->tr_time.tv_micro = 0;
        DoIO((struct IORequest *)tr);

        ++beat;

        /*
         * Forbid() here is not about the data - one task writes it - but
         * about the console: krnP4PutC() waits per character, and a task
         * switch in the middle of a line would interleave it with dos or
         * econsole output on the same channel and make both unreadable.
         */
        Forbid();
        krnP4PutStr("[beat]   ");
        krnP4PutDec((uint32_t)beat);
        krnP4PutStr("  ticks ");
        krnP4PutDec((uint32_t)__esp32p4_ticks);
        krnP4PutStr("  irqs ");
        krnP4PutDec((uint32_t)__esp32p4_irq_count);
        krnP4PutStr("  avail ");
        krnP4PutDec((uint32_t)AvailMem(MEMF_ANY));
        krnP4PutStr("  tasks ready ");
        {
            ULONG n;

            ListLength(&SysBase->TaskReady, n);
            krnP4PutDec((uint32_t)n);
            ListLength(&SysBase->TaskWait, n);
            krnP4PutStr(" waiting ");
            krnP4PutDec((uint32_t)n);
        }
        krnP4PutStr("\n");
        Permit();
    }
}

extern const struct Resident krnP4HeartbeatResident;

AROS_UFH3(static APTR, krnP4HeartbeatInit,
          AROS_UFPA(void *, dummy, D0),
          AROS_UFPA(BPTR, segList, A0),
          AROS_UFPA(struct ExecBase *, SysBase, A6))
{
    AROS_USERFUNC_INIT

    struct Task *t = krnP4SpawnTask("esp32p4 heartbeat", 20,
                                    krnP4HeartbeatTask, 8192);

    krnP4PutStr("[beat]   heartbeat task ");
    krnP4PutHex32((uint32_t)(IPTR)t);
    krnP4PutStr(t ? ", every " : " NOT STARTED, every ");
    krnP4PutDec(P4_HEARTBEAT_SECS);
    krnP4PutStr("s\n");

    return NULL;

    AROS_USERFUNC_EXIT
}

/*
 * Priority -40 puts this in the one window that works: below timer.device at
 * 50, so the device exists by the time the task opens it, and above `SDCard
 * boot wait` at -49 and dosboot at -50, so the heartbeat is already
 * reporting while those two run.  Nothing else in either the kickstart or
 * the package sits at -40, so the order does not depend on link order.
 *
 * rt_EndSkip points just past this structure, which is what the romtag
 * scanner continues from; pointing it anywhere further would skip whatever
 * tag happened to follow.
 */
const struct Resident krnP4HeartbeatResident =
{
    RTC_MATCHWORD,
    (struct Resident *)&krnP4HeartbeatResident,
    (APTR)((const char *)&krnP4HeartbeatResident + sizeof(struct Resident)),
    RTF_COLDSTART,
    1,
    NT_TASK,
    -40,
    "esp32p4 heartbeat",
    "esp32p4 heartbeat 1.0",
    &krnP4HeartbeatInit
};

#endif /* P4_HEARTBEAT_TASK */

#ifdef P4_AFTERDOS_PROBE

/*
 * The DOS device name dosboot gives the first MBR partition of unit 0:
 * bootscan.c builds it from the device name, the unit number, 'P' and the
 * partition position, so "sdcard.device" unit 0 partition 0 is SDCARD0P0.
 */
#ifndef P4_PROBE_DEVICE
#define P4_PROBE_DEVICE "SDCARD0P0:"
#endif

/*
 * The half of the A4 gate that only a running DOS can answer.
 *
 * Everything about write protection above the block device - Info() reporting
 * it, packets being refused, nothing left dirty - needs SYS: to exist, and
 * SYS: exists only after dos.library has mounted a bootable medium.  There is
 * no resident Info command in the shellcommands set and no C: directory to
 * load one from, so this runs as an RTF_AFTERDOS resident instead:
 * rom/dos/cliinit.c calls InitCode(RTF_AFTERDOS) once SYS: and the boot
 * assigns are in place and before the Shell starts.
 *
 * It is deliberately read-only in intent and bounded in every call.  The
 * mutations it attempts are the ones a filesystem has to refuse, and each is
 * judged on three things: that it failed, that it failed with
 * ERROR_DISK_WRITE_PROTECTED rather than something incidental, and that the
 * medium is bit-identical afterwards.  The third is the one that matters,
 * because an error code says the request was refused while only the unchanged
 * content says nothing was written.
 */
struct DosLibrary *DOSBase;

/* FNV-1a-32, the same primitive the SD and partition tests compare with */
static uint32_t krnP4HashBytes(const unsigned char *data, ULONG length)
{
    uint32_t hash = 2166136261UL;

    while (length--)
    {
        hash ^= *data++;
        hash *= 16777619UL;
    }
    return hash;
}

/* Read a whole file into the shared probe buffer and hash it.  Returns the
   byte count, or -1 if it could not be read at all. */
static LONG krnP4ProbeReadFile(const char *name, uint32_t *hash)
{
    static UBYTE buffer[1024] __attribute__((aligned(64)));
    BPTR fh = Open((CONST_STRPTR)name, MODE_OLDFILE);
    LONG total = 0;

    if (!fh)
        return -1;

    for (;;)
    {
        LONG got = Read(fh, buffer + total, sizeof(buffer) - total);

        if (got <= 0)
            break;
        total += got;
        if ((ULONG)total >= sizeof(buffer))
            break;
    }
    Close(fh);

    *hash = krnP4HashBytes(buffer, (ULONG)total);
    return total;
}

static void krnP4ReportState(const char *what, LONG state)
{
    krnP4PutStr(what);
    krnP4PutDecS((int32_t)state);
    krnP4PutStr(state == ID_WRITE_PROTECTED ? " (ID_WRITE_PROTECTED)"
              : state == ID_VALIDATED       ? " (ID_VALIDATED)"
              : state == ID_VALIDATING      ? " (ID_VALIDATING)"
                                            : " (unexpected)");
    krnP4PutStr("\n");
}

static int krnP4AfterDosProbe(void)
{
    /* Info() writes through a caller-supplied InfoData and AmigaOS requires
       it longword aligned; there is no AllocDosObject type for it. */
    static struct InfoData info __attribute__((aligned(8)));
    struct InfoData *id = &info;
    BPTR lock;
    LONG state_before, state_after;
    uint32_t hash_before = 0, hash_after = 0;
    LONG size_before, size_after;
    int passed = 1;
    int mutations_run = 0;

    krnP4PutStr("[sysfs]  AFTERDOS probe starting\n");

    lock = Lock((CONST_STRPTR)"SYS:", SHARED_LOCK);
    krnP4PutStr("[sysfs]  Lock(\"SYS:\") = ");
    krnP4PutHex32((uint32_t)(IPTR)lock);
    krnP4PutStr("\n");
    if (!lock)
    {
        krnP4PutStr("[sysfs]  no SYS: to examine\n");
        return 0;
    }

    if (!Info(lock, id))
    {
        krnP4PutStr("[sysfs]  Info() failed, IoErr ");
        krnP4PutDecS((int32_t)IoErr());
        krnP4PutStr("\n");
        UnLock(lock);
        return 0;
    }

    state_before = id->id_DiskState;
    krnP4ReportState("[sysfs]  Info() id_DiskState ", state_before);
    krnP4PutStr("[sysfs]    blocks ");
    krnP4PutDec((uint32_t)id->id_NumBlocks);
    krnP4PutStr(", used ");
    krnP4PutDec((uint32_t)id->id_NumBlocksUsed);
    krnP4PutStr(", block size ");
    krnP4PutDec((uint32_t)id->id_BytesPerBlock);
    krnP4PutStr(", disk type ");
    krnP4PutHex32((uint32_t)id->id_DiskType);
    krnP4PutStr("\n");

    /* Reading has to keep working; that is the whole point of mounting it. */
    size_before = krnP4ProbeReadFile("SYS:AROS.boot", &hash_before);
    krnP4PutStr("[sysfs]  read SYS:AROS.boot = ");
    krnP4PutDecS((int32_t)size_before);
    if (size_before >= 0)
    {
        krnP4PutStr(" bytes, hash ");
        krnP4PutHex32(hash_before);
    }
    else
    {
        krnP4PutStr(" (IoErr ");
        krnP4PutDecS((int32_t)IoErr());
        krnP4PutStr(")");
    }
    krnP4PutStr("\n");

    if (state_before != ID_WRITE_PROTECTED)
    {
        krnP4PutStr("[sysfs]  SYS: is not a write protected volume;"
                    " the mutation cases do not apply to it\n");
        UnLock(lock);
        return 0;
    }

    /*
     * Every mutation DOS can express against a file that exists and one that
     * does not.  Each must fail, and fail as write protection.
     */
    {
        static const char probe_new[] = "SYS:p4probe.tmp";
        static const char probe_dir[] = "SYS:p4probedir";
        static const char victim[]    = "SYS:AROS.boot";
        static const char renamed[]   = "SYS:AROS.renamed";
        struct
        {
            const char *name;
            LONG result;
            LONG error;
        } cases[8];
        unsigned int n = 0, i;
        BPTR fh, dir;

        fh = Open((CONST_STRPTR)probe_new, MODE_NEWFILE);
        cases[n].name = "Open(MODE_NEWFILE)";
        cases[n].result = (LONG)(IPTR)fh;
        cases[n].error = IoErr();
        if (fh)
            Close(fh);
        ++n;

        fh = Open((CONST_STRPTR)victim, MODE_READWRITE);
        cases[n].name = "Open(MODE_READWRITE)";
        cases[n].result = (LONG)(IPTR)fh;
        cases[n].error = IoErr();
        if (fh)
            Close(fh);
        ++n;

        dir = CreateDir((CONST_STRPTR)probe_dir);
        cases[n].name = "CreateDir";
        cases[n].result = (LONG)(IPTR)dir;
        cases[n].error = IoErr();
        if (dir)
            UnLock(dir);
        ++n;

        cases[n].name = "DeleteFile";
        cases[n].result = DeleteFile((CONST_STRPTR)victim);
        cases[n].error = IoErr();
        ++n;

        cases[n].name = "Rename";
        cases[n].result = Rename((CONST_STRPTR)victim, (CONST_STRPTR)renamed);
        cases[n].error = IoErr();
        ++n;

        cases[n].name = "SetProtection";
        cases[n].result = SetProtection((CONST_STRPTR)victim, 0);
        cases[n].error = IoErr();
        ++n;

        cases[n].name = "SetComment";
        cases[n].result = SetComment((CONST_STRPTR)victim,
                                     (CONST_STRPTR)"esp32p4 probe");
        cases[n].error = IoErr();
        ++n;

        /*
         * Relabel wants a device, not an assign.  Aimed at "SYS:" it returned
         * ERROR_DEVICE_NOT_MOUNTED without the packet ever reaching the
         * handler, which said nothing about write protection; that was a
         * defect in this test, not in the filesystem.  The device node
         * dosboot created for the partition is what ACTION_RENAME_DISK has to
         * be sent to.
         */
        cases[n].name = "Relabel(device)";
        cases[n].result = Relabel((CONST_STRPTR)P4_PROBE_DEVICE,
                                  (CONST_STRPTR)"P4Probe");
        cases[n].error = IoErr();
        ++n;

        mutations_run = (int)n;

        for (i = 0; i < n; ++i)
        {
            krnP4PutStr("[sysfs]    ");
            krnP4PutStr(cases[i].name);
            krnP4PutStr(": result ");
            krnP4PutHex32((uint32_t)cases[i].result);
            krnP4PutStr(", IoErr ");
            krnP4PutDecS((int32_t)cases[i].error);
            if (cases[i].result != 0)
            {
                krnP4PutStr("  SUCCEEDED, MUST NOT\n");
                passed = 0;
            }
            else if (cases[i].error != ERROR_DISK_WRITE_PROTECTED)
            {
                krnP4PutStr("  refused, but not as write protection\n");
                passed = 0;
            }
            else
                krnP4PutStr("  refused as write protection\n");
        }
    }

    /* The medium as it was, and the volume still usable */
    size_after = krnP4ProbeReadFile("SYS:AROS.boot", &hash_after);
    krnP4PutStr("[sysfs]  re-read SYS:AROS.boot = ");
    krnP4PutDecS((int32_t)size_after);
    krnP4PutStr(" bytes, hash ");
    krnP4PutHex32(hash_after);
    if (size_after != size_before || hash_after != hash_before)
    {
        krnP4PutStr("  CHANGED\n");
        passed = 0;
    }
    else
        krnP4PutStr("  unchanged\n");

    if (Info(lock, id))
    {
        state_after = id->id_DiskState;
        krnP4ReportState("[sysfs]  Info() id_DiskState after ", state_after);
        if (state_after != state_before)
            passed = 0;
    }
    else
    {
        krnP4PutStr("[sysfs]  Info() failed after the mutations\n");
        passed = 0;
    }

    UnLock(lock);

    krnP4PutStr("[sysfs]  AFTERDOS probe ");
    krnP4PutStr(passed ? "passed, " : "FAILED, ");
    krnP4PutDec((uint32_t)mutations_run);
    krnP4PutStr(" mutation cases\n");
    return passed;
}

extern const struct Resident krnP4AfterDosResident;

AROS_UFH3(static APTR, krnP4AfterDosInit,
          AROS_UFPA(void *, dummy, D0),
          AROS_UFPA(BPTR, segList, A0),
          AROS_UFPA(struct ExecBase *, SysBase, A6))
{
    AROS_USERFUNC_INIT

    DOSBase = (struct DosLibrary *)OpenLibrary("dos.library", 0);
    if (!DOSBase)
        krnP4PutStr("[sysfs]  dos.library unavailable in AFTERDOS\n");
    else
    {
        (void)krnP4AfterDosProbe();
        CloseLibrary((struct Library *)DOSBase);
        DOSBase = NULL;
    }

    return NULL;

    AROS_USERFUNC_EXIT
}

/*
 * RTF_AFTERDOS, so cliinit.c starts it once SYS: and the boot assigns exist.
 * Priority 0 within that pass: nothing else in this package is AFTERDOS
 * except lddemon, shell and shellcommands at -123, and this wants to run
 * before the Shell reaches a prompt and starts competing for the console.
 */
const struct Resident krnP4AfterDosResident =
{
    RTC_MATCHWORD,
    (struct Resident *)&krnP4AfterDosResident,
    (APTR)((const char *)&krnP4AfterDosResident + sizeof(struct Resident)),
    RTF_AFTERDOS,
    1,
    NT_TASK,
    0,
    "esp32p4 sysfs probe",
    "esp32p4 sysfs probe 1.0",
    &krnP4AfterDosInit
};

#endif /* P4_AFTERDOS_PROBE */

#ifdef P4_SDCARD_DEVICE_TEST
/*
 * Exercise the complete external-module path, rather than only the early
 * controller probe: open the dynamically loaded device, ask its public
 * trackdisk/NSD interfaces what they expose, and read exactly one sector.
 * The buffer is deliberately in internal SRAM for this first PIO test.
 */
/* 64-byte aligned: that is the L1 cache line, and the backend's cache
   maintenance operates on whole lines. */
static uint32_t sdcard_device_sector[128] P4_SRAMDATA
    __attribute__((aligned(64)));

static int krnP4SDCardRead(struct IOStdReq *io, uint32_t lba,
                           uint32_t sectors, int use_64bit, APTR buffer)
{
    uint64_t byte_offset = (uint64_t)lba << 9;
    ULONG bytes = sectors << 9;

    io->io_Command = use_64bit ? NSCMD_TD_READ64 : CMD_READ;
    io->io_Data = buffer;
    io->io_Length = bytes;
    io->io_Actual = use_64bit ? (uint32_t)(byte_offset >> 32) : 0;
    io->io_Offset = (uint32_t)byte_offset;
    DoIO((struct IORequest *)io);

    return io->io_Error == 0 && io->io_Actual == bytes;
}

/* Ground truth for the D1001 test card.  Captured read-only from the card in
   a host reader on 2026-08-22; the first 2 MiB have SHA-256
   3aeaff747c68f8c087cda1e3a9fcd877e323bb7ad238f9462efd5d9322fa6e45.

   The matrix used to compare CMD17 against CMD18 and nothing else, so a
   fault common to both pathways read as a pass.  LBA 0 did exactly that:
   the card holds a valid MBR there, hash 0xdebe99c1, while the driver
   returned 512 zero bytes and both sides of the comparison agreed on it.

   LBA 2049, 2080 and 2081 are FAT32 FSInfo and FAT sectors.  A host that
   mounts the volume rewrites them, so this table needs a fresh capture
   after any mount.  Everything in LBA 0..4095 outside this list is
   zero-filled, which is why a misdirected read almost always looks like
   zeroes rather than like an error. */
#define P4SD_HASH_ZERO                  0x4d7705c5U
#define P4SD_HASH_SENTINEL              0x52c707c5U
#define P4SD_SENTINEL_BYTE              0xa5
#define P4SD_REF_LIMIT                  4096U

struct p4sd_ref_sector
{
    uint32_t lba;
    uint32_t hash;
};

static const struct p4sd_ref_sector p4sd_ref_sectors[] =
{
    {     0, 0xdebe99c1U },     /* MBR, one entry: type 0x0b, start 2048 */
    {  2048, 0x730d1cbdU },     /* FAT32 VBR, OEM "BSD  4.4" */
    {  2049, 0x5534e4b0U },     /* FSInfo, volatile */
    {  2054, 0x730d1cbdU },     /* backup VBR, byte-identical to 2048 */
    {  2055, 0xc7ef8842U },     /* backup FSInfo */
    {  2080, 0xc747ea06U },     /* FAT 1, volatile */
    {  2081, 0x1657a963U },     /* FAT 1, volatile */
    {  2082, 0x672050ceU },     /* FAT 1, volatile */
    {  2083, 0x9a2ba598U },
    {  2084, 0xcf015645U },
    {  2085, 0x05ba934eU },
};

static uint32_t krnP4SDCardHash(const unsigned char *data, ULONG length)
{
    uint32_t hash = 2166136261U;
    ULONG i;

    for (i = 0; i < length; ++i)
    {
        hash ^= data[i];
        hash *= 16777619U;
    }
    return hash;
}

/* TRUE when the card content of one sector is known.  Anything below the
   captured limit that is not listed is zero. */
static int krnP4SDCardRefSector(uint32_t lba, uint32_t *hash)
{
    unsigned int i;

    for (i = 0; i < sizeof(p4sd_ref_sectors) / sizeof(p4sd_ref_sectors[0]);
         ++i)
        if (p4sd_ref_sectors[i].lba == lba)
        {
            *hash = p4sd_ref_sectors[i].hash;
            return 1;
        }
    if (lba < P4SD_REF_LIMIT)
    {
        *hash = P4SD_HASH_ZERO;
        return 1;
    }
    return 0;
}

/* Name what a block actually holds.  This is the difference between
   "block 1 differs" and "block 1 holds LBA 2048 a second time".  2048 and
   2054 are byte-identical, so both are printed when they match. */
static void krnP4SDCardNameHash(uint32_t hash)
{
    unsigned int i, matches = 0;

    if (hash == P4SD_HASH_SENTINEL)
    {
        krnP4PutStr("untouched a5");
        return;
    }
    if (hash == P4SD_HASH_ZERO)
    {
        krnP4PutStr("zero sector");
        return;
    }
    for (i = 0; i < sizeof(p4sd_ref_sectors) / sizeof(p4sd_ref_sectors[0]);
         ++i)
        if (p4sd_ref_sectors[i].hash == hash)
        {
            krnP4PutStr(matches++ ? "/" : "LBA ");
            krnP4PutDec(p4sd_ref_sectors[i].lba);
        }
    if (!matches)
        krnP4PutStr("unknown");
}

static int krnP4SDCardReadSector(struct IOStdReq *io, uint32_t lba,
                                 int use_64bit)
{
    const unsigned char *sector =
        (const unsigned char *)sdcard_device_sector;
    uint32_t hash = 2166136261U;
    uint32_t nonzero = 0;
    uint32_t unchanged = 0;
    uint32_t expected;
    unsigned int i;

    /* A nonzero sentinel catches a backend that reports success without
       actually replacing the caller's buffer. */
    for (i = 0; i < 128; ++i)
        sdcard_device_sector[i] = 0xa5a5a5a5U;

    krnP4PutStr("[sddev]  LBA ");
    krnP4PutDec(lba);
    if (!krnP4SDCardRead(io, lba, 1, use_64bit, sdcard_device_sector))
    {
        krnP4PutStr(" read failed, error ");
        krnP4PutDec((uint32_t)(unsigned char)io->io_Error);
        krnP4PutStr(", actual ");
        krnP4PutDec(io->io_Actual);
        krnP4PutStr("\n");
        return 0;
    }

    for (i = 0; i < sizeof(sdcard_device_sector); ++i)
    {
        hash ^= sector[i];
        hash *= 16777619U;
        if (sector[i] != 0)
            ++nonzero;
        if (sector[i] == 0xa5)
            ++unchanged;
    }

    krnP4PutStr(" hash ");
    krnP4PutHex32(hash);
    krnP4PutStr(", nonzero ");
    krnP4PutDec(nonzero);
    krnP4PutStr(", a5 bytes ");
    krnP4PutDec(unchanged);
    krnP4PutStr(", signature ");
    krnP4PutStr(sector[510] == 0x55 && sector[511] == 0xaa ?
                "55aa" : "absent");
    /* State the card's own content where it is known.  A single-sector read
       that silently returns zeroes is otherwise indistinguishable from a
       sector that is genuinely zero. */
    if (krnP4SDCardRefSector(lba, &expected))
    {
        krnP4PutStr(", card ");
        krnP4PutHex32(expected);
        krnP4PutStr(hash == expected ? ", match" : ", WRONG");
    }
    krnP4PutStr("\n");
    return 1;
}

/* Decide whether the LBA-0 fault is address dependent or an artefact of
   being the first data transfer of the boot.  The old matrix always read
   LBA 0 first, so those two explanations were indistinguishable.  Here
   LBA 2048 goes first on purpose, LBA 0 is then read twice, and LBA 2048
   is read again at the end to show the path still works. */
static void krnP4SDCardAddressProbe(struct IOStdReq *io)
{
    static const uint32_t order[] = { 2048, 0, 0, 1, 2048, 2083 };
    unsigned int step;

    krnP4PutStr("[sddev]  address probe, LBA 2048 before LBA 0 on purpose\n");
    for (step = 0; step < sizeof(order) / sizeof(order[0]); ++step)
    {
        uint32_t hash, expected;
        unsigned int i;

        for (i = 0; i < 128; ++i)
            sdcard_device_sector[i] = 0xa5a5a5a5U;

        krnP4PutStr("[sddev]    step ");
        krnP4PutDec(step);
        krnP4PutStr(" LBA ");
        krnP4PutDec(order[step]);
        if (!krnP4SDCardRead(io, order[step], 1, 0, sdcard_device_sector))
        {
            krnP4PutStr(" read failed, error ");
            krnP4PutDec((uint32_t)(unsigned char)io->io_Error);
            krnP4PutStr(", actual ");
            krnP4PutDec(io->io_Actual);
            krnP4PutStr("\n");
            continue;
        }

        hash = krnP4SDCardHash((const unsigned char *)sdcard_device_sector,
                               512);
        krnP4PutStr(" got ");
        krnP4PutHex32(hash);
        krnP4PutStr(" (");
        krnP4SDCardNameHash(hash);
        krnP4PutStr(")");
        if (krnP4SDCardRefSector(order[step], &expected))
        {
            krnP4PutStr(", card ");
            krnP4PutHex32(expected);
            krnP4PutStr(hash == expected ? ", match\n" : ", WRONG\n");
        }
        else
            krnP4PutStr(", no card reference\n");
    }
}

#ifdef P4_SDCARD_MULTIBLOCK_TEST

/* P4_A1_DIAGNOSTIC selects this bounded, read-only matrix at build time. */

#ifndef P4_SDCARD_REPEAT
#define P4_SDCARD_REPEAT 1
#endif

/* How many block detail lines one cell may print.  A 128-sector cell would
   otherwise emit 128 of them. */
#define P4SD_BLOCK_REPORT_LIMIT         12

struct p4sd_ref_range
{
    uint32_t lba;
    uint32_t sectors;
    uint32_t hash;
    int volatile_range;
};

/* FNV-1a-32 over the whole requested range.  `volatile_range` marks a range
   that contains FAT32 metadata a host rewrites when it mounts the volume.
   Those hashes are true for the capture but go stale the moment the card
   visits a host, so a mismatch there is treated differently below: if both
   the CMD17 and the CMD18 path return the same value, the reference is old
   rather than the driver wrong, and the cell is reported as unverified
   instead of failed.  A disagreement between the two paths is always a
   failure, volatile or not. */
static const struct p4sd_ref_range p4sd_ref_ranges[] =
{
    {          0,   1, 0xdebe99c1U, 0 },
    {          0,   2, 0x32fbe1c1U, 0 },
    {          0,  32, 0xba6a51c1U, 0 },
    {          0, 128, 0x6d6551c1U, 0 },
    {       2047,   1, 0x4d7705c5U, 0 },
    {       2047,   2, 0x44a784bdU, 0 },
    {       2047,  32, 0x66f5bb97U, 1 },
    {       2047, 128, 0xf4871f17U, 1 },
    {       2048,   1, 0x730d1cbdU, 0 },
    {       2048,   2, 0xa84cbdc8U, 1 },
    {       2048,  32, 0xb306cb97U, 1 },
    {       2048, 128, 0x69142f17U, 1 },
    {       2049,   1, 0x5534e4b0U, 1 },
    {       2049,   2, 0x34d664b0U, 1 },
    {       2049,  32, 0x6b62b540U, 1 },
    {       2049, 128, 0xd5ed1f1fU, 1 },
    {       2053,   1, 0x4d7705c5U, 0 },
    {       2053,   2, 0x44a784bdU, 0 },
    {       2054,   1, 0x730d1cbdU, 0 },
    {       2054,   2, 0x00a2061aU, 0 },
    {       2083,   1, 0x9a2ba598U, 0 },
    {       2083,   2, 0xdbf2f318U, 0 },
    {       2083,  32, 0xadc117e7U, 0 },
    {       2083, 128, 0xe07e17e7U, 0 },
    /* The card end, where the original READ64 cells read.  Captured
       2026-08-22: the last 128 sectors are entirely zero-filled, a single
       distinct sector hash across all of them.  These cells can be checked
       against the card, but they cannot detect a repeated block, and the
       harness says so separately.  The clusters behind them are free
       according to the FAT, so nothing was written to give them content;
       the two ranges below make that unnecessary. */
    {  249737215,   1, 0x4d7705c5U, 0 },
    {  249737214,   2, 0x1f116dc5U, 0 },
    {  249737184,  32, 0x38699dc5U, 0 },
    {  249737088, 128, 0x5e509dc5U, 0 },
    /* What the card-end cells were meant to prove is a byte offset beyond
       what a 32-bit ULONG can express, and LBA 8388608 is exactly that
       boundary: it is the first sector a 32-bit byte offset cannot reach.
       Both ranges hold file data, 128 distinct sectors each, so unlike the
       card end they can detect a repeated or misplaced block.  File contents
       are far more stable than the FAT metadata above, but they are not
       eternal: if the files on this card change, recapture. */
    {    8388608,   1, 0x55a37550U, 0 },
    {    8388608,   2, 0xeba3bb4fU, 0 },
    {    8388608,  32, 0x3fc8a95eU, 0 },
    {    8388608, 128, 0x894be777U, 0 },
    {   10000000,   1, 0x73bf86bfU, 0 },
    {   10000000,   2, 0xe462007bU, 0 },
    {   10000000,  32, 0x02ddf4f0U, 0 },
    {   10000000, 128, 0xa3d171beU, 0 },
};

/* Cells that could not be verified because their reference is stale. */
static ULONG p4sd_unverified_cells;

static const struct p4sd_ref_range *krnP4SDCardRefRange(uint32_t lba,
                                                        uint32_t sectors)
{
    unsigned int i;

    for (i = 0; i < sizeof(p4sd_ref_ranges) / sizeof(p4sd_ref_ranges[0]); ++i)
        if (p4sd_ref_ranges[i].lba == lba &&
            p4sd_ref_ranges[i].sectors == sectors)
            return &p4sd_ref_ranges[i];
    return NULL;
}

/* Compare one CMD18 request against independently issued CMD17 reads and,
   where the card content is known, against the card itself.  The card is
   the deciding reference: CMD17 and CMD18 share a controller, a FIFO and a
   cache wrapper, so agreement between them proves nothing on its own.

   Buffers are allocated separately and rounded to a 64-byte cache line.
   The previous version carved both halves out of one AllocMem block, whose
   alignment is 16 bytes, so a receive buffer could share its first and last
   cache line with the reference buffer it was compared against. */
static int krnP4SDCardCompareBlocks(struct IOStdReq *io, uint32_t lba,
                                    uint32_t sectors, int use_64bit,
                                    int report)
{
    ULONG bytes = sectors << 9;
    const struct p4sd_ref_range *ref = krnP4SDCardRefRange(lba, sectors);
    unsigned char *raw_single = NULL;
    unsigned char *raw_multi = NULL;
    unsigned char *single;
    unsigned char *multi;
    uint32_t single_hash = 0, multi_hash = 0, first_hash = 0;
    ULONG i, first_bad = 0, printed = 0;
    int have_bad = 0;
    int reference_blocks_differ = 0;
    int stale = 0;
    int ok = 0;

    if (sectors == 0 || sectors > 128)
        return 0;

    raw_single = AllocMem(bytes + 64, MEMF_PUBLIC | MEMF_CLEAR);
    raw_multi = AllocMem(bytes + 64, MEMF_PUBLIC | MEMF_CLEAR);
    if (!raw_single || !raw_multi)
    {
        krnP4PutStr("[sddev]  CMD18 buffers unavailable\n");
        goto out;
    }
    single = (unsigned char *)(((IPTR)raw_single + 63) & ~(IPTR)63);
    multi = (unsigned char *)(((IPTR)raw_multi + 63) & ~(IPTR)63);

    for (i = 0; i < sectors; ++i)
        if (!krnP4SDCardRead(io, lba + i, 1, use_64bit, single + (i << 9)))
        {
            krnP4PutStr("[sddev]  CMD18 baseline CMD17 failed at LBA ");
            krnP4PutDec(lba + i);
            krnP4PutStr(", error ");
            krnP4PutDec((uint32_t)(unsigned char)io->io_Error);
            krnP4PutStr(", actual ");
            krnP4PutDec(io->io_Actual);
            krnP4PutStr("\n");
            goto out;
        }

    for (i = 0; i < bytes; ++i)
        multi[i] = P4SD_SENTINEL_BYTE;
    if (!krnP4SDCardRead(io, lba, sectors, use_64bit, multi))
    {
        krnP4PutStr("[sddev]  CMD18 request failed, error ");
        krnP4PutDec((uint32_t)(unsigned char)io->io_Error);
        krnP4PutStr(", actual ");
        krnP4PutDec(io->io_Actual);
        krnP4PutStr("\n");
        goto out;
    }

    single_hash = krnP4SDCardHash(single, bytes);
    multi_hash = krnP4SDCardHash(multi, bytes);
    first_hash = krnP4SDCardHash(multi, 512);

    for (i = 0; i < bytes; ++i)
        if (multi[i] != single[i])
        {
            have_bad = 1;
            first_bad = i;
            break;
        }

    for (i = 1; i < sectors; ++i)
        if (krnP4SDCardHash(single + (i << 9), 512) !=
            krnP4SDCardHash(single, 512))
        {
            reference_blocks_differ = 1;
            break;
        }

    /* Accept against the card where the card is known.  Fall back to the
       old self-comparison only where it is not, and say so. */
    if (ref)
    {
        if (single_hash == ref->hash && multi_hash == ref->hash)
            ok = 1;
        else if (ref->volatile_range && single_hash == multi_hash)
        {
            /* Both paths agree and only the reference disagrees, on a range
               that holds FAT32 metadata a host rewrites.  That is a stale
               reference, not a driver fault.  Do not call it a pass either. */
            ok = 1;
            stale = 1;
            ++p4sd_unverified_cells;
        }
        else
            ok = 0;
    }
    else
        ok = !have_bad;

    if (report || !ok)
    {
        krnP4PutStr("[sddev]  CMD18 LBA ");
        krnP4PutDec(lba);
        krnP4PutStr(" x ");
        krnP4PutDec(sectors);
        krnP4PutStr(use_64bit ? " sectors READ64 " : " sectors READ ");
        krnP4PutStr(stale ? "unverified, stale reference\n" :
                    ok ? "match\n" : "FAILED\n");

        krnP4PutStr("[sddev]    buffers single ");
        krnP4PutHex32((uint32_t)(IPTR)single);
        krnP4PutStr(" multi ");
        krnP4PutHex32((uint32_t)(IPTR)multi);
        krnP4PutStr(", allocated ");
        krnP4PutHex32((uint32_t)(IPTR)raw_single);
        krnP4PutStr("/");
        krnP4PutHex32((uint32_t)(IPTR)raw_multi);
        krnP4PutStr("\n");

        krnP4PutStr("[sddev]    cmd17 ");
        krnP4PutHex32(single_hash);
        krnP4PutStr(", cmd18 ");
        krnP4PutHex32(multi_hash);
        krnP4PutStr(", card ");
        if (ref)
            krnP4PutHex32(ref->hash);
        else
            krnP4PutStr("unknown");
        krnP4PutStr("\n");

        for (i = 0; i < sectors && printed < P4SD_BLOCK_REPORT_LIMIT; ++i)
        {
            uint32_t block_single = krnP4SDCardHash(single + (i << 9), 512);
            uint32_t block_multi = krnP4SDCardHash(multi + (i << 9), 512);

            /* Print every differing block, but only the leading few that
               agree, so one cell cannot flood the console. */
            if (block_single == block_multi && i >= 4)
                continue;
            ++printed;
            krnP4PutStr("[sddev]    block ");
            krnP4PutDec(i);
            krnP4PutStr(": cmd17 ");
            krnP4PutHex32(block_single);
            krnP4PutStr(" (");
            krnP4SDCardNameHash(block_single);
            krnP4PutStr("), cmd18 ");
            krnP4PutHex32(block_multi);
            krnP4PutStr(" (");
            krnP4SDCardNameHash(block_multi);
            if (i != 0 && block_multi == first_hash)
                krnP4PutStr(", repeat of block 0");
            krnP4PutStr(block_single == block_multi ? ")\n" : ") MISMATCH\n");
        }

        if (have_bad)
        {
            krnP4PutStr("[sddev]    first differing byte ");
            krnP4PutDec(first_bad);
            krnP4PutStr("\n");
        }
        if (sectors > 1 && !reference_blocks_differ)
            krnP4PutStr("[sddev]    warning every reference block is "
                        "identical, a repeated block is undetectable here\n");
        if (ref && stale)
            krnP4PutStr("[sddev]    verdict both paths agree, reference is "
                        "stale for this volatile range, not verified\n");
        else if (ref)
        {
            krnP4PutStr("[sddev]    verdict cmd17 ");
            krnP4PutStr(single_hash == ref->hash ?
                        "matches card, cmd18 " : "DIFFERS from card, cmd18 ");
            krnP4PutStr(multi_hash == ref->hash ?
                        "matches card\n" : "DIFFERS from card\n");
        }
        else
            krnP4PutStr("[sddev]    verdict self-comparison only, "
                        "no card reference for this range\n");
    }

out:
    if (raw_multi)
        FreeMem(raw_multi, bytes + 64);
    if (raw_single)
        FreeMem(raw_single, bytes + 64);
    return ok;
}

/* The rejection point of the A1 gate.  Every malformed request must fail
   without disturbing controller or card state, and the proof of the second
   half is that a known-good read still matches the card afterwards.

   Two limits are worth stating rather than papering over.  First, on a card
   this large an out-of-range request cannot be expressed through 32-bit
   CMD_READ at all: the largest byte offset a ULONG holds is block 8,388,607
   while the card has 249,737,216, so every 32-bit offset is inside the media
   and those cases have to go through READ64.  Second, the backend's own
   P4SD_MAX_DATA_LEN cap sits behind the generic layer's chunking at 128
   blocks, so a device request cannot reach it; it is defence in depth, and
   the oversized case below is recorded rather than expected to fail. */
static int krnP4SDCardRejectionTest(struct IOStdReq *io, uint32_t total_sectors)
{
    static const struct
    {
        const char *name;
        uint32_t high;
        uint32_t offset;
        uint32_t length;
        int use_64bit;
        int must_fail;
    } cases[] =
    {
        { "unaligned offset",         0, 256,          512,       0, 1 },
        { "unaligned length",         0, 2048UL * 512, 511,       0, 1 },
        { "both unaligned",           0, 1,            1,         0, 1 },
        { "unaligned offset, READ64", 0, 256,          512,       1, 1 },
        { "first sector past the end",0, 0,            512,       1, 1 },
        { "far past the end",         0xFF, 0,         512,       1, 1 },
        { "zero length",              0, 2048UL * 512, 0,         0, 0 },
    };
    /* There is deliberately no oversized case here.  The backend's
       P4SD_MAX_DATA_LEN cap sits behind the generic layer's chunking at 128
       blocks, so no device request can reach it, and an earlier attempt to
       provoke it asked for 256 sectors into the 512-byte SRAM probe buffer.
       That is a caller bug no layer can catch, because io_Length is the only
       statement of the buffer's size, and it duly trapped with pc=0. */
    unsigned int i;
    int passed = 1;

    krnP4PutStr("[sddev]  rejection test starting\n");
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
    {
        uint32_t high = cases[i].high;
        uint32_t offset = cases[i].offset;
        uint32_t hash, expected;
        int failed;

        /* Case 4 needs the runtime capacity: the first byte beyond the last
           sector, split across the READ64 offset pair. */
        if (i == 4)
        {
            uint64_t past = (uint64_t)total_sectors << 9;

            high = (uint32_t)(past >> 32);
            offset = (uint32_t)past;
        }

        io->io_Command = cases[i].use_64bit ? NSCMD_TD_READ64 : CMD_READ;
        io->io_Data = sdcard_device_sector;
        io->io_Length = cases[i].length;
        io->io_Actual = cases[i].use_64bit ? high : 0;
        io->io_Offset = offset;
        DoIO((struct IORequest *)io);
        failed = io->io_Error != 0;

        krnP4PutStr("[sddev]    ");
        krnP4PutStr(cases[i].name);
        krnP4PutStr(": high ");
        krnP4PutHex32(high);
        krnP4PutStr(" offset ");
        krnP4PutHex32(offset);
        krnP4PutStr(" length ");
        krnP4PutDec(cases[i].length);
        krnP4PutStr(" -> error ");
        krnP4PutDec((uint32_t)(unsigned char)io->io_Error);
        krnP4PutStr(", actual ");
        krnP4PutDec(io->io_Actual);
        if (cases[i].must_fail && !failed)
        {
            krnP4PutStr(", ACCEPTED BUT MUST BE REJECTED");
            passed = 0;
        }
        else if (cases[i].must_fail)
            krnP4PutStr(", rejected as required");
        else
            krnP4PutStr(", not a rejection case, recorded only");
        krnP4PutStr("\n");

        /* Controller and card state intact?  A known sector must still read
           correctly, which is the half of the requirement that a plain
           error code cannot show. */
        if (!krnP4SDCardRead(io, 2048, 1, 0, sdcard_device_sector))
        {
            krnP4PutStr("[sddev]      follow-up read FAILED, error ");
            krnP4PutDec((uint32_t)(unsigned char)io->io_Error);
            krnP4PutStr("\n");
            passed = 0;
            continue;
        }
        hash = krnP4SDCardHash((const unsigned char *)sdcard_device_sector,
                               512);
        if (!krnP4SDCardRefSector(2048, &expected) || hash != expected)
        {
            krnP4PutStr("[sddev]      follow-up read WRONG, got ");
            krnP4PutHex32(hash);
            krnP4PutStr("\n");
            passed = 0;
        }
    }

    krnP4PutStr("[sddev]  rejection test ");
    krnP4PutStr(passed ? "passed\n" : "FAILED\n");
    return passed;
}

/*
 * The device half of A4's read-only requirement.
 *
 * Two claims have to hold at the block-device level before FAT can be
 * trusted to act on them, and neither is provable from source alone:
 *
 *   - TD_PROTSTATUS says the medium is protected.  This is what FAT asks at
 *     mount time and what Info() ends up reporting, so if it answered
 *     "writable" the whole chain above it would be wrong;
 *   - a write is refused with TDERR_WriteProt and io_Actual left at zero.
 *     The error code matters because a filesystem can act on TDERR_WriteProt
 *     and can only guess at IOERR_ABORTED, and io_Actual matters because
 *     "nothing was written" has to be readable from the reply itself.
 *
 * A write command is issued deliberately, at a sector whose content is
 * known, and that sector is read back and compared afterwards.  The
 * comparison is the point: an error code says the request was refused, and
 * only the unchanged content says nothing reached the card.  The buffer
 * handed over holds a pattern the sector demonstrably does not contain, so a
 * write that did go through would be visible rather than merely unlikely.
 */
static int krnP4SDCardWriteDenialTest(struct IOStdReq *io)
{
    static const uint32_t probe_lba = 2048;
    uint32_t hash_before, hash_after, expected = 0;
    int passed = 1;
    int have_reference;
    unsigned int k;

    krnP4PutStr("[sddev]  write denial test starting\n");

    /* What TD_PROTSTATUS says */
    io->io_Command = TD_PROTSTATUS;
    io->io_Data = NULL;
    io->io_Length = 0;
    io->io_Actual = 0;
    io->io_Offset = 0;
    DoIO((struct IORequest *)io);
    krnP4PutStr("[sddev]    TD_PROTSTATUS: error ");
    krnP4PutDecS((int32_t)(signed char)io->io_Error);
    krnP4PutStr(", actual ");
    krnP4PutHex32(io->io_Actual);
    if (io->io_Error == 0 && io->io_Actual != 0)
        krnP4PutStr(", medium reported protected\n");
    else
    {
        krnP4PutStr(", MEDIUM NOT REPORTED PROTECTED\n");
        passed = 0;
    }

    /* The sector as it stands */
    if (!krnP4SDCardRead(io, probe_lba, 1, 0, sdcard_device_sector))
    {
        krnP4PutStr("[sddev]    baseline read FAILED, cannot judge a write\n");
        return 0;
    }
    hash_before = krnP4SDCardHash((const unsigned char *)sdcard_device_sector,
                                  512);
    have_reference = krnP4SDCardRefSector(probe_lba, &expected);

    /* A write of something the sector demonstrably does not contain */
    for (k = 0; k < 128; ++k)
        sdcard_device_sector[k] = 0x5a5a5a5aU;

    io->io_Command = CMD_WRITE;
    io->io_Data = sdcard_device_sector;
    io->io_Length = 512;
    io->io_Actual = 0xdeadbeefU;        /* has to come back as zero */
    io->io_Offset = probe_lba * 512UL;
    DoIO((struct IORequest *)io);

    krnP4PutStr("[sddev]    CMD_WRITE at LBA ");
    krnP4PutDec(probe_lba);
    krnP4PutStr(": error ");
    krnP4PutDecS((int32_t)(signed char)io->io_Error);
    krnP4PutStr(" (");
    krnP4PutStr((signed char)io->io_Error == (signed char)TDERR_WriteProt
                ? "TDERR_WriteProt" : "NOT TDERR_WriteProt");
    krnP4PutStr("), actual ");
    krnP4PutHex32(io->io_Actual);
    krnP4PutStr("\n");

    if (io->io_Error == 0)
    {
        krnP4PutStr("[sddev]    WRITE WAS ACCEPTED\n");
        passed = 0;
    }
    else if ((signed char)io->io_Error != (signed char)TDERR_WriteProt)
    {
        krnP4PutStr("[sddev]    refused, but not as write protection\n");
        passed = 0;
    }
    if (io->io_Actual != 0)
    {
        krnP4PutStr("[sddev]    io_Actual NOT ZERO after a denied write\n");
        passed = 0;
    }

    /* And the medium is what it was */
    for (k = 0; k < 128; ++k)
        sdcard_device_sector[k] = 0xa5a5a5a5U;
    if (!krnP4SDCardRead(io, probe_lba, 1, 0, sdcard_device_sector))
    {
        krnP4PutStr("[sddev]    read-back FAILED after the denied write\n");
        return 0;
    }
    hash_after = krnP4SDCardHash((const unsigned char *)sdcard_device_sector,
                                 512);

    krnP4PutStr("[sddev]    sector hash before ");
    krnP4PutHex32(hash_before);
    krnP4PutStr(", after ");
    krnP4PutHex32(hash_after);
    if (hash_after != hash_before)
    {
        krnP4PutStr(", CHANGED\n");
        passed = 0;
    }
    else if (have_reference && hash_after != expected)
    {
        krnP4PutStr(", unchanged but NEITHER MATCHES THE CARD REFERENCE\n");
        passed = 0;
    }
    else
        krnP4PutStr(have_reference ? ", unchanged and matching the card\n"
                                   : ", unchanged\n");

    krnP4PutStr("[sddev]  write denial test ");
    krnP4PutStr(passed ? "passed\n" : "FAILED\n");
    return passed;
}

static int krnP4SDCardMultiblockTest(struct IOStdReq *io,
                                     uint32_t total_sectors)
{
    static const uint32_t sector_counts[] = { 1, 2, 32, 128 };
    /* 2083 comes first so LBA 0 is not the run's first data transfer.  2047
       and 2049 straddle the partition start in both directions.  Ranges that
       reach into FAT32 metadata are flagged volatile in the reference table
       and report as unverified rather than failed if a host has since
       rewritten them. */
    static const uint32_t test_lbas[] = { 2083, 0, 2047, 2048, 2049 };
    /* Straddle and single-sector cases that stay clear of FSInfo and FAT.
       2047/2048 crosses the partition start, 2053/2054 the backup boot
       sector, and 2054 alone is byte-identical to 2048. */
    static const struct
    {
        uint32_t lba;
        uint32_t sectors;
    } stable_extras[] =
    {
        { 2048, 1 }, { 2047, 1 }, { 2047, 2 },
        { 2054, 1 }, { 2053, 1 }, { 2053, 2 }, { 2054, 2 },
    };
    /* Beyond LBA 8388608 a byte offset no longer fits in a ULONG, so these
       only work through READ64.  8388608 is the boundary itself. */
    static const uint32_t high_lbas[] = { 8388608UL, 10000000UL };
    unsigned int extra_index;
    unsigned int lba_index, count_index, repeat;
    int expected_fault =
#ifdef P4_SDCARD_EXPECT_FAULT
        1;
#else
        0;
#endif
    int passed = 1;

    krnP4PutStr("[sddev]  CMD18 compare matrix starting\n");
    for (lba_index = 0; lba_index < sizeof(test_lbas) / sizeof(test_lbas[0]);
         ++lba_index)
        for (count_index = 0;
             count_index < sizeof(sector_counts) / sizeof(sector_counts[0]);
             ++count_index)
            if (!krnP4SDCardCompareBlocks(io, test_lbas[lba_index],
                                           sector_counts[count_index], 0, 1))
            {
                if (expected_fault)
                {
                    expected_fault = 0;
                    krnP4PutStr("[sddev]  expected injected failure; CMD17 recovery ");
                    if (krnP4SDCardRead(io, test_lbas[lba_index], 1, 0,
                                         sdcard_device_sector))
                        krnP4PutStr("passed\n");
                    else
                    {
                        krnP4PutStr("FAILED\n");
                        passed = 0;
                    }
                }
                else
                    passed = 0;
            }

    /* The else used to bind to the inner if, not to this one, so every
       successful READ64 cell set passed = 0 and the matrix could never
       report success no matter what the hardware did. */
    for (extra_index = 0;
         extra_index < sizeof(stable_extras) / sizeof(stable_extras[0]);
         ++extra_index)
        if (!krnP4SDCardCompareBlocks(io, stable_extras[extra_index].lba,
                                       stable_extras[extra_index].sectors,
                                       0, 1))
            passed = 0;

    /* The 64-bit path at referenced addresses.  The gate asks for READ64
       near the card end, and those cells run below, but no card reference
       exists for that range, so they can only self-compare.  Running READ64
       at the referenced addresses too is what actually proves the 64-bit
       code path against the card. */
    for (lba_index = 0; lba_index < sizeof(test_lbas) / sizeof(test_lbas[0]);
         ++lba_index)
        for (count_index = 0;
             count_index < sizeof(sector_counts) / sizeof(sector_counts[0]);
             ++count_index)
            if (!krnP4SDCardCompareBlocks(io, test_lbas[lba_index],
                                           sector_counts[count_index], 1, 1))
                passed = 0;

    /* READ64 beyond the 32-bit byte-offset boundary, against real file data.
       This is the cell the card end was supposed to be. */
    for (lba_index = 0;
         lba_index < sizeof(high_lbas) / sizeof(high_lbas[0]);
         ++lba_index)
        for (count_index = 0;
             count_index < sizeof(sector_counts) / sizeof(sector_counts[0]);
             ++count_index)
            if (!krnP4SDCardCompareBlocks(io, high_lbas[lba_index],
                                           sector_counts[count_index], 1, 1))
                passed = 0;

    if (total_sectors >= 128)
    {
        for (count_index = 0;
             count_index < sizeof(sector_counts) / sizeof(sector_counts[0]);
             ++count_index)
            if (!krnP4SDCardCompareBlocks(io,
                                           total_sectors - sector_counts[count_index],
                                           sector_counts[count_index], 1, 1))
                passed = 0;
    }
    else
        passed = 0;

    if (expected_fault)
    {
        krnP4PutStr("[sddev]  expected fault was not injected\n");
        passed = 0;
    }

    /* The repetition point of the A1 gate.  Rotate the address rather than
       hammering one, which costs nothing and covers both straddle cases and
       the FAT sector as well.  Two sectors keeps one iteration at about two
       kilobytes, so a thousand of them stay inside a reasonable capture. */
    for (repeat = 0; passed && repeat < P4_SDCARD_REPEAT; ++repeat)
    {
        uint32_t lba = test_lbas[repeat % (sizeof(test_lbas) /
                                           sizeof(test_lbas[0]))];

        if (!krnP4SDCardCompareBlocks(io, lba, 2, 0, 0))
        {
            krnP4PutStr("[sddev]  repeated CMD18 mismatch at iteration ");
            krnP4PutDec(repeat);
            krnP4PutStr(", LBA ");
            krnP4PutDec(lba);
            krnP4PutStr("\n");
            passed = 0;
        }
        else if ((repeat % 100) == 99)
        {
            /* Progress, so a long silent run is distinguishable from a
               hang, both for a human and for a capture timeout. */
            krnP4PutStr("[sddev]  repetitions completed ");
            krnP4PutDec(repeat + 1);
            krnP4PutStr("\n");
        }
    }

    krnP4PutStr("[sddev]  CMD18 compare matrix ");
    krnP4PutStr(passed ? "passed" : "FAILED");
    krnP4PutStr(", two-sector repetitions ");
    krnP4PutDec(repeat);
    krnP4PutStr(", cells unverified against a stale reference ");
    krnP4PutDec(p4sd_unverified_cells);
    krnP4PutStr("\n");
    return passed;
}

#endif /* P4_SDCARD_MULTIBLOCK_TEST */

static void krnP4SDCardDeviceTest(void)
{
    struct MsgPort *port = NULL;
    struct IOStdReq *io = NULL;
    struct DriveGeometry geometry;
    struct NSDeviceQueryResult query;
    static const uint32_t probe_lbas[] =
        { 0, 1, 2, 2048, 8192, 32768, 65536 };
    uint32_t total_sectors = 0;
    unsigned int i;
    LONG open_error;

    krnP4PutStr("[sddev]  opening sdcard.device unit 0\n");

    port = CreateMsgPort();
    if (port)
        io = (struct IOStdReq *)CreateIORequest(port, sizeof(*io));
    if (!port || !io)
    {
        krnP4PutStr("[sddev]  could not allocate an IO request\n");
        goto out;
    }

    open_error = OpenDevice("sdcard.device", 0,
                            (struct IORequest *)io, 0);
    if (open_error != 0)
    {
        krnP4PutStr("[sddev]  unit 0 is unavailable, error ");
        krnP4PutDec((uint32_t)open_error);
        krnP4PutStr("\n");
        goto out;
    }

    io->io_Command = TD_GETGEOMETRY;
    io->io_Data = &geometry;
    io->io_Length = sizeof(geometry);
    io->io_Actual = 0;
    io->io_Offset = 0;
    DoIO((struct IORequest *)io);
    if (io->io_Error == 0 && io->io_Actual == sizeof(geometry))
    {
        krnP4PutStr("[sddev]  geometry sector ");
        krnP4PutDec(geometry.dg_SectorSize);
        krnP4PutStr(" bytes, total ");
        krnP4PutDec(geometry.dg_TotalSectors);
        krnP4PutStr(" sectors\n");
        total_sectors = geometry.dg_TotalSectors;
    }
    else
    {
        krnP4PutStr("[sddev]  TD_GETGEOMETRY failed, error ");
        krnP4PutDec((uint32_t)(unsigned char)io->io_Error);
        krnP4PutStr("\n");
    }

    io->io_Command = NSCMD_DEVICEQUERY;
    io->io_Data = &query;
    io->io_Length = sizeof(query);
    io->io_Actual = 0;
    io->io_Offset = 0;
    DoIO((struct IORequest *)io);
    if (io->io_Error == 0 && io->io_Actual == sizeof(query))
    {
        krnP4PutStr("[sddev]  NSD type ");
        krnP4PutDec(query.DeviceType);
        krnP4PutStr(", query size ");
        krnP4PutDec(query.SizeAvailable);
        krnP4PutStr("\n");
    }
    else
    {
        krnP4PutStr("[sddev]  NSCMD_DEVICEQUERY failed, error ");
        krnP4PutDec((uint32_t)(unsigned char)io->io_Error);
        krnP4PutStr("\n");
    }

    krnP4SDCardAddressProbe(io);

    for (i = 0; i < sizeof(probe_lbas) / sizeof(probe_lbas[0]); ++i)
        if (!krnP4SDCardReadSector(io, probe_lbas[i], 0))
            break;

    /* Exercise the TD64 byte-offset convention near the end of an SDXC
       card.  These still become single-block CMD17 transactions in the
       backend, so the first-stage no-CMD18 safety boundary remains intact. */
    if (i == sizeof(probe_lbas) / sizeof(probe_lbas[0]) &&
        total_sectors > 33)
    {
        (void)krnP4SDCardReadSector(io, total_sectors - 33, 1);
        (void)krnP4SDCardReadSector(io, total_sectors - 1, 1);
        (void)krnP4SDCardReadSector(io, 0, 0);
#ifdef P4_SDCARD_MULTIBLOCK_TEST
        (void)krnP4SDCardMultiblockTest(io, total_sectors);
        (void)krnP4SDCardRejectionTest(io, total_sectors);
        (void)krnP4SDCardWriteDenialTest(io);
#endif
    }
    krnP4PutStr("[sddev]  read-only device test complete\n");

    CloseDevice((struct IORequest *)io);

out:
    if (io)
        DeleteIORequest((struct IORequest *)io);
    if (port)
        DeleteMsgPort(port);
}
#endif /* P4_SDCARD_DEVICE_TEST */

#ifdef P4_PARTITION_TEST
static uint32_t partition_test_sector[128] P4_SRAMDATA
    __attribute__((aligned(4)));

static void krnP4PartitionLibraryTest(void)
{
    struct PartitionHandle *root = NULL;
    const unsigned char *sector =
        (const unsigned char *)partition_test_sector;
    uint32_t hash = 2166136261U;
    uint32_t nonzero = 0;
    uint32_t a5bytes = 0;
    LONG result;
    unsigned int i;

    krnP4PutStr("[part]   opening partition.library\n");
    PartitionBase = (struct PartitionBase *)
        OpenLibrary("partition.library", 3);
    if (!PartitionBase)
    {
        krnP4PutStr("[part]   library unavailable\n");
        return;
    }

    krnP4PutStr("[part]   base ");
    krnP4PutHex32((uint32_t)(IPTR)PartitionBase);
    krnP4PutStr(", version ");
    krnP4PutDec(PartitionBase->lib.lib_Version);
    krnP4PutStr("\n");

    root = OpenRootPartition("sdcard.device", 0);
    if (!root)
    {
        krnP4PutStr("[part]   OpenRootPartition failed\n");
        goto out;
    }

    krnP4PutStr("[part]   root sector ");
    krnP4PutDec(root->dg.dg_SectorSize);
    krnP4PutStr(", total ");
    krnP4PutDec(root->dg.dg_TotalSectors);
    krnP4PutStr(", read command ");
    krnP4PutHex32(root->bd->cmdread);
    krnP4PutStr("\n");

    for (i = 0; i < 128; ++i)
        partition_test_sector[i] = 0xa5a5a5a5U;

    /* Call the real ABI entry directly.  The legacy convenience macro in
       partition.h has a historical argument-order mismatch. */
    result = ReadPartitionDataQ(root, partition_test_sector, 512, 0);
    for (i = 0; i < sizeof(partition_test_sector); ++i)
    {
        hash ^= sector[i];
        hash *= 16777619U;
        if (sector[i] != 0)
            ++nonzero;
        if (sector[i] == 0xa5)
            ++a5bytes;
    }

    krnP4PutStr("[part]   LBA 0 result ");
    krnP4PutDec((uint32_t)result);
    krnP4PutStr(", actual ");
    krnP4PutDec(root->bd->ioreq->iotd_Req.io_Actual);
    krnP4PutStr(", hash ");
    krnP4PutHex32(hash);
    krnP4PutStr(", nonzero ");
    krnP4PutDec(nonzero);
    krnP4PutStr(", a5 bytes ");
    krnP4PutDec(a5bytes);
    krnP4PutStr(", signature ");
    krnP4PutStr(sector[510] == 0x55 && sector[511] == 0xaa ?
                "55aa\n" : "absent\n");

    /* A2, first acceptance point: does bounded discovery find exactly the
       partition the card actually has?  Until the A1 fault was fixed this
       could not work at all, because LBA 0 read back as zeroes. */
    if (OpenPartitionTable(root) == 0)
    {
        struct PartitionHandle *ph;
        ULONG found = 0;

        krnP4PutStr("[part]   table type ");
        krnP4PutDec(root->table->type);
        krnP4PutStr(" (2 = MBR, 3 = EBR, 4 = GPT)\n");

        ForeachNode(&root->table->list, ph)
        {
            /* de_LowCyl and de_HighCyl are in the units the handler chose,
               so convert back through the cylinder size it recorded. */
            ULONG cyl = ph->dg.dg_CylSectors ? ph->dg.dg_CylSectors : 1;

            krnP4PutStr("[part]     partition ");
            krnP4PutDec(found);
            krnP4PutStr(": start ");
            krnP4PutDec(ph->de.de_LowCyl * cyl);
            krnP4PutStr(", sectors ");
            krnP4PutDec((ph->de.de_HighCyl - ph->de.de_LowCyl + 1) * cyl);
            krnP4PutStr(", dostype ");
            krnP4PutHex32(ph->de.de_DosType);
            krnP4PutStr("\n");
            ++found;
        }

        krnP4PutStr("[part]   partitions found ");
        krnP4PutDec(found);
        krnP4PutStr(found == 1 ? ", as expected\n" :
                                ", EXPECTED EXACTLY ONE\n");
        ClosePartitionTable(root);
    }
    else
        krnP4PutStr("[part]   OpenPartitionTable found no table\n");

    /* The gate requires a normal read to still work after discovery, whatever
       discovery decided. */
    for (i = 0; i < 128; ++i)
        partition_test_sector[i] = 0xa5a5a5a5U;
    result = ReadPartitionDataQ(root, partition_test_sector, 512, 2048);
    krnP4PutStr("[part]   read after discovery: result ");
    krnP4PutDec((uint32_t)result);
    krnP4PutStr(", hash ");
    krnP4PutHex32(krnP4SDCardHash((const unsigned char *)partition_test_sector,
                                  512));
    krnP4PutStr(", card 0x730d1cbd\n");

    if (result == 0 &&
        root->bd->ioreq->iotd_Req.io_Actual == 512 &&
        a5bytes != 512)
        krnP4PutStr("[part]   public read path complete\n");

out:
    if (root)
        CloseRootPartition(root);
    CloseLibrary((struct Library *)PartitionBase);
    PartitionBase = NULL;
}

/* A2 acceptance: every malformed table must be refused inside a budget, and
   a normal read must still work afterwards.  The corpus lives in
   ramtest.device, one case per unit, so nothing is ever written to a medium.

   Two things the first version of this test got wrong, both recorded because
   they change what the results mean.  It only opened the top-level table, so
   a cyclic EBR chain was never walked and the cycle guard was never
   exercised; nested tables are now opened too, under a depth limit.  And it
   expected zero partitions everywhere, which is wrong for a broken GPT behind
   a valid protective MBR: falling back to the MBR is correct, so a case now
   states how many partitions it may yield and which table type it must never
   accept. */
#define P4_CORPUS_MAX_DEPTH 4

static int krnP4PartitionOpenNested(struct PartitionHandle *ph,
                                    unsigned int depth, ULONG *nested)
{
    struct PartitionHandle *child;
    int ok = 1;

    if (depth >= P4_CORPUS_MAX_DEPTH)
    {
        krnP4PutStr("[corpus]   depth limit reached, stopping\n");
        return 1;
    }

    if (OpenPartitionTable(ph) != 0)
    {
        /* Say so explicitly.  A silent zero here is ambiguous: it could mean
           the nested table was refused, which is the point of the cyclic
           case, or that nothing was ever tried. */
        krnP4PutStr("[corpus]     nested table at depth ");
        krnP4PutDec(depth);
        krnP4PutStr(" refused\n");
        return 1;
    }

    ForeachNode(&ph->table->list, child)
    {
        ++*nested;
        krnP4PutStr("[corpus]     nested at depth ");
        krnP4PutDec(depth + 1);
        krnP4PutStr(", type ");
        krnP4PutDec(ph->table->type);
        krnP4PutStr("\n");
        if (*nested > 64)
        {
            krnP4PutStr("[corpus]     TOO MANY NESTED PARTITIONS\n");
            ok = 0;
            break;
        }
        if (!krnP4PartitionOpenNested(child, depth + 1, nested))
            ok = 0;
    }

    ClosePartitionTable(ph);
    return ok;
}

/*
 * What each corpus unit may produce.  This mirrors the table in
 * arch/riscv-esp32p4/ramtest/ramtest_corpus.c and has to be kept in the same
 * order; the core and the device are separate modules, so the core cannot
 * read the device's own copy.  Regenerating the corpus means updating this.
 */
static const struct
{
    UWORD max_partitions;
    UWORD forbidden_type;   /* 0 = none, 4 = PHPTT_GPT */
} p4CorpusExpect[] =
{
    { 0, 0 },   /* mbr entry past end of medium          */
    { 0, 0 },   /* mbr entry wraps 32 bits               */
    { 0, 0 },   /* mbr entry of zero length              */
    { 1, 0 },   /* ebr chain cycles on itself            */
    { 0, 4 },   /* gpt header size 0xffffffff            */
    { 0, 4 },   /* gpt entry array wraps                 */
    { 0, 4 },   /* gpt header bad crc                    */
    { 0, 4 },   /* gpt truncated before entries          */
    { 0, 4 },   /* gpt entry size below the structure    */
    { 0, 4 },   /* gpt entry count zero                  */
    { 1, 4 },   /* bad gpt behind a valid protective mbr */
};

static void krnP4PartitionCorpusTest(void)
{
    unsigned int unit;
    int passed = 1;

    PartitionBase = (struct PartitionBase *)
        OpenLibrary("partition.library", 3);
    if (!PartitionBase)
    {
        krnP4PutStr("[corpus] partition.library unavailable\n");
        return;
    }

    krnP4PutStr("[corpus] hostile table corpus starting\n");

    for (unit = 0; unit < 16; ++unit)
    {
        struct PartitionHandle *root;
        ULONG found = 0;
        ULONG nested = 0;
        LONG opened;
        ULONG type = 0;

        root = OpenRootPartition("ramtest.device", unit);
        if (!root)
        {
            if (unit == 0)
            {
                krnP4PutStr("[corpus] ramtest.device unavailable\n");
                passed = 0;
            }
            break;
        }

        krnP4PutStr("[corpus] unit ");
        krnP4PutDec(unit);
        krnP4PutStr(": geometry ");
        krnP4PutDec(root->dg.dg_TotalSectors);
        krnP4PutStr(" sectors\n");

        opened = OpenPartitionTable(root);
        if (opened == 0)
        {
            struct PartitionHandle *ph;

            type = root->table->type;
            ForeachNode(&root->table->list, ph)
            {
                ++found;
                /* Walk into the entry as well.  This is what makes a cyclic
                   EBR chain actually get followed, and the only thing that
                   can prove the cycle guard works. */
                if (!krnP4PartitionOpenNested(ph, 1, &nested))
                    passed = 0;
            }

            krnP4PutStr("[corpus]   table type ");
            krnP4PutDec(type);
            krnP4PutStr(", partitions ");
            krnP4PutDec(found);
            krnP4PutStr(", nested ");
            krnP4PutDec(nested);
            ClosePartitionTable(root);
        }
        else
        {
            krnP4PutStr("[corpus]   no table accepted, result ");
            krnP4PutDec((uint32_t)opened);
        }

        /* Judge against what this case is allowed to produce. */
        if (unit < sizeof(p4CorpusExpect) / sizeof(p4CorpusExpect[0]))
        {
            int bad = 0;

            if (found > p4CorpusExpect[unit].max_partitions)
            {
                krnP4PutStr(", TOO MANY PARTITIONS, allowed ");
                krnP4PutDec(p4CorpusExpect[unit].max_partitions);
                bad = 1;
            }
            if (p4CorpusExpect[unit].forbidden_type != 0 &&
                type == p4CorpusExpect[unit].forbidden_type)
            {
                krnP4PutStr(", ACCEPTED A FORBIDDEN TABLE TYPE");
                bad = 1;
            }
            if (bad)
                passed = 0;
            else
                krnP4PutStr(", within expectation");
        }
        else
        {
            krnP4PutStr(", NO EXPECTATION RECORDED FOR THIS UNIT");
            passed = 0;
        }
        krnP4PutStr("\n");

        /* A plain read has to work after every failure. */
        {
            static uint32_t probe[128] P4_SRAMDATA __attribute__((aligned(64)));
            LONG r;
            unsigned int k;

            for (k = 0; k < 128; ++k)
                probe[k] = 0xa5a5a5a5U;
            r = ReadPartitionDataQ(root, probe, 512, 0);
            krnP4PutStr("[corpus]   read after failure: result ");
            krnP4PutDec((uint32_t)r);
            krnP4PutStr(r == 0 ? ", usable\n" : ", UNUSABLE\n");
            if (r != 0)
                passed = 0;
        }

        CloseRootPartition(root);
    }

    krnP4PutStr("[corpus] hostile table corpus ");
    krnP4PutStr(passed ? "passed\n" : "FAILED\n");

    CloseLibrary((struct Library *)PartitionBase);
    PartitionBase = NULL;
}

#endif /* P4_PARTITION_TEST */

#ifdef P4_DOS_PROBE
/*
 * What A4 can be asked before dosboot.resource exists.
 *
 * The package gains dos.library, the resources around it and the FAT
 * handler in one step, and dosboot in the next.  That split is deliberate:
 * dosboot's COLDSTART init never returns, so everything printed from here
 * would become unreachable the moment it joins the package.  So the half
 * of the A4 gate that concerns what is present rather than what boots is
 * asked here, while it can still be read on the console.
 *
 * Four questions, each with a definite wrong answer:
 *
 *   - does bootloader.resource exist, and what did it make of the command
 *     line?  The platform supplies none yet, so the honest expectation is
 *     an empty argument list, and this is what will show the KRN_CmdLine
 *     plumbing working when it arrives;
 *   - does FileSystem.resource carry the FAT entries?  dos.library scans
 *     that list exactly once during its own initialisation, so an entry
 *     that is missing here is missing for good;
 *   - is 0x46415402, the DosType the A3 image's partition actually
 *     reports, among them?  Three FAT DosTypes are registered and only a
 *     match on that one makes the medium mountable;
 *   - is dos.library's romtag present but not marked for automatic
 *     initialisation?  It has to be found by dosboot through
 *     FindResident() and started by hand.  A COLDSTART bit here would
 *     mean it starts on its own, in the wrong order, before dosboot has
 *     chosen a boot node.
 */
static void krnP4DosProbe(void)
{
    struct FileSysResource *fsr;
    struct Resident *res;
    int fat_entries = 0;
    int a3_dostype = 0;
    int failed = 0;

    krnP4PutStr("[dos]    pre-dosboot probe starting\n");

    /* bootloader.resource and the command line it parsed */
    BootLoaderBase = OpenResource("bootloader.resource");
    krnP4PutStr("[dos]    bootloader.resource @ ");
    krnP4PutHex32((uint32_t)(IPTR)BootLoaderBase);
    if (!BootLoaderBase)
    {
        krnP4PutStr("  MISSING\n");
        failed = 1;
    }
    else
    {
        struct List *args = (struct List *)GetBootInfo(BL_Args);
        ULONG count = 0;

        krnP4PutStr("  loader ");
        {
            const char *name = (const char *)GetBootInfo(BL_LoaderName);

            krnP4PutHex32((uint32_t)(IPTR)name);
            krnP4PutStr(" '");
            krnP4PutStr(name ? name : "");
            krnP4PutStr("'\n");
        }

        if (args)
        {
            struct Node *node;

            ForeachNode(args, node)
            {
                krnP4PutStr("[dos]      arg ");
                krnP4PutDec(count);
                krnP4PutStr(" '");
                krnP4PutStr(node->ln_Name ? node->ln_Name : "");
                krnP4PutStr("'\n");
                ++count;
            }
        }
        krnP4PutStr("[dos]      command line arguments ");
        krnP4PutDec(count);
        krnP4PutStr(count ? "\n" : " (platform supplies none yet)\n");
    }

    /* FileSystem.resource and its entries */
    fsr = (struct FileSysResource *)OpenResource("FileSystem.resource");
    krnP4PutStr("[dos]    FileSystem.resource @ ");
    krnP4PutHex32((uint32_t)(IPTR)fsr);
    krnP4PutStr("\n");
    if (!fsr)
        failed = 1;
    else
    {
        struct FileSysEntry *fse;

        ForeachNode(&fsr->fsr_FileSysEntries, fse)
        {
            krnP4PutStr("[dos]      dostype ");
            krnP4PutHex32((uint32_t)fse->fse_DosType);
            krnP4PutStr("  version ");
            krnP4PutDec((uint32_t)(fse->fse_Version >> 16));
            krnP4PutStr(".");
            krnP4PutDec((uint32_t)(fse->fse_Version & 0xffff));
            krnP4PutStr("  pri ");
            krnP4PutDecS((int32_t)fse->fse_Priority);
            krnP4PutStr("  patch ");
            krnP4PutHex32((uint32_t)fse->fse_PatchFlags);
            krnP4PutStr("  seglist ");
            krnP4PutHex32((uint32_t)(IPTR)fse->fse_SegList);
            krnP4PutStr("  '");
            krnP4PutStr(fse->fse_Node.ln_Name ? fse->fse_Node.ln_Name : "");
            krnP4PutStr("'\n");

            /* The three the FAT handler registers, 'FAT\0', 'FAT\1' and
               'FAT\2'; the last is what the A3 image's partition reports. */
            if ((fse->fse_DosType & 0xffffff00UL) == 0x46415400UL)
                ++fat_entries;
            if (fse->fse_DosType == 0x46415402UL)
                a3_dostype = 1;
        }
    }

    krnP4PutStr("[dos]    FAT entries ");
    krnP4PutDec((uint32_t)fat_entries);
    krnP4PutStr(fat_entries == 3 ? " (all three)" : " EXPECTED THREE");
    krnP4PutStr(", A3 dostype 0x46415402 ");
    krnP4PutStr(a3_dostype ? "present\n" : "MISSING\n");
    if (fat_entries != 3 || !a3_dostype)
        failed = 1;

    /* the named handler resident the FAT entries point at by name */
    res = FindResident("fat-handler");
    krnP4PutStr("[dos]    resident 'fat-handler' @ ");
    krnP4PutHex32((uint32_t)(IPTR)res);
    krnP4PutStr("\n");
    if (!res)
        failed = 1;

    /* dos.library: present, and deliberately not self-starting */
    res = FindResident("dos.library");
    krnP4PutStr("[dos]    resident 'dos.library' @ ");
    krnP4PutHex32((uint32_t)(IPTR)res);
    if (!res)
    {
        krnP4PutStr("  MISSING - dosboot could not start it\n");
        failed = 1;
    }
    else
    {
        krnP4PutStr("  version ");
        krnP4PutDec(res->rt_Version);
        krnP4PutStr("  pri ");
        krnP4PutDecS((int32_t)(signed char)res->rt_Pri);
        krnP4PutStr("  flags ");
        krnP4PutHex32((uint32_t)res->rt_Flags);
        if (res->rt_Flags & (RTF_COLDSTART | RTF_SINGLETASK | RTF_AFTERDOS))
        {
            krnP4PutStr("  SELF-STARTING, must be started by dosboot\n");
            failed = 1;
        }
        else
            krnP4PutStr("  not self-starting, as required\n");
    }

    krnP4PutStr("[dos]    pre-dosboot probe ");
    krnP4PutStr(failed ? "FAILED\n" : "passed\n");
}
#endif /* P4_DOS_PROBE */

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

static uint32_t krnP4ReadBE32(const unsigned char *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/*
 * Publish the part of PSRAM that the boot package did not reserve.  The
 * MemHeader itself lives at the start of that free part, as it did when the
 * whole window was free.  Keeping this in one place makes it impossible for
 * the package-success and no-package paths to describe overlapping pools.
 */
static int krnP4PublishPSRAM(IPTR first_free)
{
    IPTR end = P4_PSRAM_WINDOW_BASE + __esp32p4_psram_size;

    if (first_free > ~(IPTR)0 - 15)
        return 0;
    first_free = (first_free + 15) & ~(IPTR)15;
    if (first_free >= end || end - first_free <= sizeof(struct MemHeader) * 2)
    {
        krnP4PutStr("[psram]  no room remains for an external memory header\n");
        return 0;
    }

    __esp32p4_mh_psram = (struct MemHeader *)first_free;
    krnCreateMemHeader("External Memory", -20, (APTR)first_free,
                       end - first_free,
                       MEMF_PUBLIC | MEMF_KICK | MEMF_LOCAL);
    return 1;
}

/*
 * Load the standard PKG container from its live partition.
 *
 * The flash mapping is read-only, while the ELF loader records placed
 * section addresses back into each member's headers and debug.library keeps
 * pointers into those headers.  Copy the declared package bytes to the
 * bottom of PSRAM first and keep both that copy and every placed section
 * below the MemHeader for the lifetime of the system.
 */
static int krnP4LoadBSPPackage(unsigned long part_off,
                               unsigned long part_size)
{
    const unsigned char *flash;
    unsigned char *copy = (unsigned char *)P4_PSRAM_WINDOW_BASE;
    IPTR psram_end = P4_PSRAM_WINDOW_BASE + __esp32p4_psram_size;
    IPTR pkg_size, memlow, lo = 0, hi = 0, used = 0;
    IPTR i;
    int modules;

    flash = krnP4FlashMap(part_off, 8);
    if (!flash || flash[0] != 'P' || flash[1] != 'K' ||
        flash[2] != 'G' || flash[3] != 1)
    {
        krnP4PutStr("[flash] pkg   partition does not start with PKG v1\n");
        return 0;
    }

    pkg_size = krnP4ReadBE32(flash + 4);
    if (pkg_size < 8 || pkg_size > part_size ||
        pkg_size > __esp32p4_psram_size)
    {
        krnP4PutStr("[flash] pkg   declared package size is outside its bounds\n");
        return 0;
    }

    flash = krnP4FlashMap(part_off, pkg_size);
    if (!flash)
        return 0;

    for (i = 0; i < pkg_size; i++)
        copy[i] = flash[i];

    if ((IPTR)copy > ~(IPTR)0 - pkg_size - 15)
        return -1;
    memlow = ((IPTR)copy + pkg_size + 15) & ~(IPTR)15;
    if (memlow >= psram_end)
        return -1;

    modules = krnLoadPackage(copy, pkg_size, memlow, psram_end,
                             &lo, &hi, &used);
    if (modules <= 0 || !lo || hi <= lo || used < hi || used > psram_end)
    {
        krnP4PutStr("[flash] pkg   load failed; PSRAM withheld from exec\n");
        return -1;
    }

    /* The loader wrote instructions through the data path. */
    krnP4SyncCode((void *)lo, hi - lo);

    __esp32p4_modules_low = (UWORD *)lo;
    __esp32p4_modules_high = (UWORD *)hi;

    krnP4PutStr("[flash] pkg   loaded ");
    krnP4PutDec((uint32_t)modules);
    krnP4PutStr(modules == 1 ? " module, reserved " : " modules, reserved ");
    krnP4PutDec((uint32_t)(used - P4_PSRAM_WINDOW_BASE));
    krnP4PutStr(" bytes of PSRAM\n");

    if (!krnP4PublishPSRAM(used))
    {
        krnP4PutStr("[flash] pkg   loaded, but its remaining PSRAM is unusable\n");
        return -1;
    }

    return modules;
}

/*
 * Hand the machine to exec. Everything above this point exists to make
 * this call possible: a heap it can allocate from, the extent of the
 * image it must not hand out, and a tick to schedule on.
 */
static void krnStartExec(void)
{
    struct MemHeader *mh = __esp32p4_mh_low;
    UWORD *ranges[5];

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
    if (__esp32p4_modules_high > __esp32p4_modules_low)
    {
        ranges[2] = __esp32p4_modules_low;
        ranges[3] = __esp32p4_modules_high;
        ranges[4] = (UWORD *)-1;
    }
    else
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
    if (__esp32p4_modules_high > __esp32p4_modules_low)
        krnDumpResidents(__esp32p4_modules_low, __esp32p4_modules_high);

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

    /* And the external memory, converted like the primary region: thirty
       two megabytes is more than the plain allocator's linear scan should
       be asked to walk. */
    if (__esp32p4_mh_psram)
    {
        struct MemHeader *tmh = krnConvertMemHeaderToTLSF(__esp32p4_mh_psram);

        if (tmh)
            __esp32p4_mh_psram = tmh;

        Enqueue(&SysBase->MemList, &__esp32p4_mh_psram->mh_Node);
        krnP4PutStr(tmh ? "[exec]   external memory added, TLSF\n"
                        : "[exec]   external memory added\n");
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

        {
            struct Library *utility = OpenLibrary("utility.library", 0);

            krnP4PutStr("[exec]   OpenLibrary(utility.library) = ");
            krnP4PutHex32((uint32_t)(IPTR)utility);
            if (utility)
            {
                krnP4PutStr("  version ");
                krnP4PutDec(utility->lib_Version);
                CloseLibrary(utility);
            }
            krnP4PutStr("\n");
        }
    }

#ifdef P4_TASK_TEST
    {
        /*
         * A spins at the bootstrap task's own priority; the waiter sits
         * above it, so that what the wait measures is the timer and not
         * how long a CPU-bound task at equal priority holds on to its
         * quantum afterwards.
         */
        struct Task *ta = krnP4SpawnTask("esp32p4 counter A", 0,
                                          test_task_a, 4096);
        struct Task *tb = krnP4SpawnTask("esp32p4 waiter B", 5,
                                          test_task_b, 4096);

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

            {
                unsigned long bad = 0;

                krnPSRAMMap(psram.size);
                if (krnPSRAMVerify(psram.size, &bad))
                {
                    krnP4PutStr("[psram]  window ");
                    krnP4PutHex32(P4_PSRAM_WINDOW_BASE);
                    krnP4PutStr(" mapped, one word per megabyte verified\n");

                    /*
                     * Do not create the MemHeader yet.  A package found
                     * below will occupy the bottom of this window before
                     * exec can allocate from it; the header then starts at
                     * the loader's high-water mark.  With no package the
                     * whole range is published after the table scan.
                     */
#ifdef P4_PROBE
                    /*
                     * The two silicon questions, while the whole window is
                     * still nobody's. The scratch it writes is the base of
                     * the window, which the memory header below overwrites
                     * a few lines later.
                     */
                    krnP4Probe((void *)P4_PSRAM_WINDOW_BASE);
#endif
                }
                else
                {
                    krnP4PutStr("[psram]  window mapped but ");
                    krnP4PutHex32((uint32_t)bad);
                    krnP4PutStr(" did not read back\n");
                    psram.size = 0;
                }
            }
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

#ifdef P4_SDMMC_PROBE
    /*
     * A deliberately small first step toward storage: power and identify
     * the socket, negotiate one-bit SD mode, then read sector zero through
     * the FIFO.  No command issued by this probe can modify card media.
     */
    krnP4SDMMCProbe();
#endif

    /*
     * Survey the flash MMU and parse the live partition table.  The table
     * read uses one temporary scratch mapping; a matching BSP partition is
     * then validated, copied and relocated into the reserved low end of
     * PSRAM before exec is allowed to publish the remaining memory.
     */
    krnP4FlashSurvey();
    {
        unsigned long pkg_off = 0, pkg_size = 0;
        int found = krnP4PartitionScan(P4_BSP_PART_TYPE, P4_BSP_PART_LABEL,
                                       &pkg_off, &pkg_size, 1);
        int package_claimed_psram = 0;

        if (found > 0)
        {
            krnP4PutStr("[flash] pkg   ");
            krnP4PutStr(P4_BSP_PART_LABEL);
            krnP4PutStr(" at ");
            krnP4PutHex32((uint32_t)pkg_off);
            krnP4PutStr(", ");
            krnP4PutDec((uint32_t)(pkg_size / 1024));
            krnP4PutStr(" KB\n");

            if (__esp32p4_psram_size)
            {
                int loaded = krnP4LoadBSPPackage(pkg_off, pkg_size);

                /* A negative result means bytes were already copied or
                   placed.  Withhold that window rather than describe them
                   to exec as free.  A bad header was rejected before any
                   PSRAM write and can safely fall back to the full pool. */
                package_claimed_psram = loaded != 0;
            }
            else
                krnP4PutStr("[flash] pkg   PSRAM unavailable; cannot load\n");
        }
        else if (found == 0)
            krnP4PutStr("[flash] pkg   no package partition on this board yet\n");

        if (__esp32p4_psram_size && !__esp32p4_mh_psram &&
            !package_claimed_psram)
            (void)krnP4PublishPSRAM(P4_PSRAM_WINDOW_BASE);
    }

    krnStartExec();

#ifdef P4_SDCARD_DEVICE_TEST
    if (SysBase)
        krnP4SDCardDeviceTest();
#endif
#ifdef P4_PARTITION_TEST
    if (SysBase)
    {
        krnP4PartitionLibraryTest();
        krnP4PartitionCorpusTest();
    }
#endif
#ifdef P4_DOS_PROBE
    if (SysBase)
        krnP4DosProbe();
#endif

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
