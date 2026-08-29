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
#if defined(P4_SDCARD_DEVICE_TEST) || defined(P4_B5_CONCURRENT_STRESS)
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
#if defined(P4_AFTERDOS_PROBE) || defined(P4_B5_CONCURRENT_STRESS)
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/filehandler.h>
#include <proto/dos.h>
#endif

#ifdef P4_A5_PROBE
#include <aros/libcall.h>
#include "../proof/proof_id.h"

/* Written out rather than taken from <proto/sdproof.h>, for the same reason
   the proof command does it: the library's generated headers are build
   artefacts of a module the kickstart must not depend on. */
#define SDPROOF_QUERY_LVO   5
#define SDProofQuery(base, what)                                        \
    AROS_LC1(ULONG, SDProofQuery,                                       \
             AROS_LCA(ULONG, (what), D0),                               \
             struct Library *, (base), SDPROOF_QUERY_LVO, SDProof)
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

/*
 * Which read latency the chip was found in, kept for the late report.
 *
 * The early [psram] lines are written before anything drains the USB serial
 * buffer, so on a boot that gets far enough to be chatty they are overwritten
 * before a host can read them.  The one value worth not losing is this: it
 * says whether the latency sweep is what got in, which is the difference
 * between a port that works after any firmware and one that works only after
 * itself.
 */
signed char p4_psram_probe_latency_seen = -1;

/*
 * ANA_PLL_CTRL0 as the PSRAM bring-up found it, kept for the late report.
 *
 * Bit 8 is MSPI_CAL_END.  Clear here and PSRAM up means this boot calibrated
 * the MSPI PLL itself; set here means it inherited a calibration from earlier
 * firmware and proves nothing.  It belongs in the late report because the
 * early [psram] lines are written before anything drains the USB serial buffer
 * and are gone by the time a host can read them.
 */
unsigned long p4_psram_calib_entry;

/*
 * The digital supply setting the bring-up left, which is the value that
 * decided the 23 August failure and the one a future one would turn on.
 */
unsigned char p4_psram_bias_seen;

/* Where the flash development volume starts, for flashdisk.device.  Zero
   until the arosbsp partition has been found, and left at zero if it never
   is, which is what makes the device refuse to open rather than serve
   whatever is at offset zero. */
unsigned long __esp32p4_flashdisk_base;
static struct MemHeader *__esp32p4_mh_psram;
/* Not static: the A5 probe judges loaded addresses against this range. */
UWORD *__esp32p4_modules_low;
UWORD *__esp32p4_modules_high;

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
        krnP4PutStr(" MB, mapped and verified, pll ");
        krnP4PutStr((p4_psram_calib_entry & (1UL << 8))
                    ? "inherited" : "calibrated here");
        krnP4PutStr(", supply ");
        krnP4PutDec((uint32_t)p4_psram_bias_seen);
        krnP4PutStr(", found in read latency ");
        if (p4_psram_probe_latency_seen >= 0)
            krnP4PutDec((uint32_t)p4_psram_probe_latency_seen);
        else
            krnP4PutStr("none");
        krnP4PutStr("\n");
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
 * Overridable so the fairness of everything below it can be tested rather
 * than assumed: a build at 5 is what shows whether a handler polling at DOS's
 * priority 10 still starves the rest of the machine.
 */
#ifndef P4_HEARTBEAT_PRI
#define P4_HEARTBEAT_PRI 20
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
 * every handler DOS starts and below nothing that matters, and it is kept
 * there even now that econsole waits instead of spinning: a heartbeat should
 * be able to report while something else is spinning, whoever that turns out
 * to be next.  P4_HEARTBEAT_PRI overrides it, which is how the econsole fix
 * was tested.
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
            struct Task *t;

            ListLength(&SysBase->TaskReady, n);
            krnP4PutDec((uint32_t)n);
            ListLength(&SysBase->TaskWait, n);
            krnP4PutStr(" waiting ");
            krnP4PutDec((uint32_t)n);

            /*
             * Naming them, not just counting them.  A count says something is
             * ready and says nothing about what, which is exactly the question
             * when a lower-priority task stops being scheduled.  Reasoning
             * from priorities alone got this wrong once already.
             */
            krnP4PutStr("  ready:");
            ForeachNode(&SysBase->TaskReady, t)
            {
                krnP4PutStr(" '");
                krnP4PutStr(t->tc_Node.ln_Name ? t->tc_Node.ln_Name
                                               : "(unnamed)");
                krnP4PutStr("'@");
                krnP4PutDecS((int32_t)(signed char)t->tc_Node.ln_Pri);
            }
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

    struct Task *t = krnP4SpawnTask("esp32p4 heartbeat", P4_HEARTBEAT_PRI,
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

#if defined(P4_AFTERDOS_PROBE) || defined(P4_B5_CONCURRENT_STRESS)

#ifdef P4_B5_CONCURRENT_STRESS
static void krnP4PanelProbe(void);
#endif

/*
 * The Relabel case needs the name of the *device* node serving SYS:, because
 * getdevpacketinfo() in rom/dos/packethelper.c refuses anything whose
 * dol_Type is not DLT_DEVICE; an assign or a volume name returns
 * ERROR_DEVICE_NOT_MOUNTED without the packet ever reaching the handler.
 *
 * A fixed name is wrong, and a run with no card in the board is what showed
 * it: dosboot names that node from the device, the unit and the partition
 * position in bootscan.c, so it is SDCARD0P0 when the card booted and
 * FLASHDISK0P0 when the flash volume did.  krnP4SysDeviceName() finds it by
 * matching the handler port SYS: resolves to against the device list, which
 * is exact and needs no assumption about what booted.  A build can still pin
 * it with -DP4_PROBE_DEVICE="...".
 */
#ifndef P4_PROBE_DEVICE
#define P4_PROBE_DEVICE ""
#endif

/* Writes "NAME:" into out, or leaves it empty if SYS: cannot be traced back
   to a device node.  Returns 1 on success. */
static int krnP4SysDeviceName(char *out, ULONG size)
{
    struct DevProc *dp;
    struct DosList *dl;
    int found = 0;

    out[0] = '\0';

    if (P4_PROBE_DEVICE[0] != '\0')
    {
        const char *fixed = P4_PROBE_DEVICE;
        ULONG i = 0;

        while (fixed[i] != '\0' && i + 1 < size)
        {
            out[i] = fixed[i];
            ++i;
        }
        out[i] = '\0';
        return 1;
    }

    dp = GetDeviceProc((CONST_STRPTR)"SYS:", NULL);
    if (!dp)
        return 0;

    dl = LockDosList(LDF_DEVICES | LDF_READ);
    while ((dl = NextDosEntry(dl, LDF_DEVICES | LDF_READ)) != NULL)
    {
        if (dl->dol_Task != dp->dvp_Port)
            continue;

        {
            /* Copied to the terminator rather than by AROS_BSTR_strlen(),
               which is strlen() here and the kernel is built freestanding. */
            const char *name = AROS_BSTR_ADDR(dl->dol_Name);
            ULONG i = 0;

            while (name[i] != '\0' && i + 2 < size)
            {
                out[i] = name[i];
                ++i;
            }
            if (i > 0)
            {
                out[i] = ':';
                out[i + 1] = '\0';
                found = 1;
            }
        }
        break;
    }
    UnLockDosList(LDF_DEVICES | LDF_READ);
    FreeDeviceProc(dp);

    return found;
}

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
        static char sysdevice[40];
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
        if (krnP4SysDeviceName(sysdevice, sizeof(sysdevice)))
        {
            krnP4PutStr("[sysfs]    SYS: is served by ");
            krnP4PutStr(sysdevice);
            krnP4PutStr("\n");
            cases[n].result = Relabel((CONST_STRPTR)sysdevice,
                                      (CONST_STRPTR)"P4Probe");
            cases[n].error = IoErr();
        }
        else
        {
            /* Not a filesystem result, so it must not be scored as one. */
            krnP4PutStr("[sysfs]    SYS: could not be traced to a device node,"
                        " skipping Relabel\n");
            cases[n].name = NULL;
        }
        ++n;

        for (i = 0; i < n; ++i)
        {
            /* A case that could not be set up at all is not a result.  It is
               left out of the count rather than scored either way. */
            if (cases[i].name == NULL)
                continue;
            ++mutations_run;

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

#ifdef P4_A5_PROBE
#ifdef P4_FLASHDISK_TEST
/*
 * The flash volume through the whole stack, rather than through a map.
 *
 * The earlier probe reads the volume's structures with krnP4FlashMap() and
 * says the bytes are there.  This asks the harder question: does
 * flashdisk.device serve them as a block device, and does partition.library
 * find the FAT16 partition inside?  Same discovery code A2 hardened and
 * verified against the card and against eleven malformed tables, so if it
 * reports one partition of the expected size at the expected place, the
 * device underneath behaves like a disk.
 *
 * Deliberately not a mount and not a boot node yet.  A boot node would put
 * this volume in dosboot's MountList next to the card's, and which of them
 * wins is a decision with consequences; it gets its own step.
 */
static int krnP4FlashDiskDeviceTest(void)
{
    struct MsgPort *port = NULL;
    struct IOStdReq *io = NULL;
    struct PartitionHandle *root = NULL;
    int passed = 1;

    krnP4PutStr("[fddev]  flash block device test starting\n");

    if (__esp32p4_flashdisk_base == 0)
    {
        krnP4PutStr("[fddev]  no volume base was published\n");
        return 0;
    }

    port = CreateMsgPort();
    if (port)
        io = (struct IOStdReq *)CreateIORequest(port,
                                               sizeof(struct IOStdReq));
    if (!io)
    {
        krnP4PutStr("[fddev]  could not create an IO request\n");
        goto out;
    }

    if (OpenDevice((CONST_STRPTR)"flashdisk.device", 0,
                   (struct IORequest *)io, 0) != 0)
    {
        krnP4PutStr("[fddev]  flashdisk.device did not open, error ");
        krnP4PutDecS((int32_t)(signed char)io->io_Error);
        krnP4PutStr("\n");
        passed = 0;
        goto out;
    }
    krnP4PutStr("[fddev]  flashdisk.device open\n");

    /* Geometry, and whether it agrees with the constant the volume was
       written against. */
    {
        struct DriveGeometry dg;

        io->io_Command = TD_GETGEOMETRY;
        io->io_Data = &dg;
        io->io_Length = sizeof(dg);
        io->io_Actual = 0;
        io->io_Offset = 0;
        DoIO((struct IORequest *)io);

        krnP4PutStr("[fddev]  geometry: error ");
        krnP4PutDecS((int32_t)(signed char)io->io_Error);
        krnP4PutStr(", sector size ");
        krnP4PutDec(dg.dg_SectorSize);
        krnP4PutStr(", sectors ");
        krnP4PutDec(dg.dg_TotalSectors);
        krnP4PutStr("\n");
        if (io->io_Error != 0 || dg.dg_SectorSize != 512
            || dg.dg_TotalSectors != P4_FLASHDISK_SIZE / 512)
            passed = 0;
    }

    /* Write protection, which a filesystem above will read. */
    {
        io->io_Command = TD_PROTSTATUS;
        io->io_Data = NULL;
        io->io_Length = 0;
        io->io_Actual = 0;
        DoIO((struct IORequest *)io);
        krnP4PutStr("[fddev]  TD_PROTSTATUS actual ");
        krnP4PutHex32(io->io_Actual);
        krnP4PutStr(io->io_Actual ? ", protected\n" : ", NOT PROTECTED\n");
        if (!io->io_Actual)
            passed = 0;
    }

    /* A write must be refused, and the command list must not offer one. */
    {
        static uint32_t pattern[128] P4_SRAMDATA;
        unsigned int k;

        for (k = 0; k < 128; ++k)
            pattern[k] = 0x5a5a5a5aU;

        io->io_Command = CMD_WRITE;
        io->io_Data = pattern;
        io->io_Length = 512;
        io->io_Actual = 0xdeadbeefU;
        io->io_Offset = 0;
        DoIO((struct IORequest *)io);
        krnP4PutStr("[fddev]  CMD_WRITE: error ");
        krnP4PutDecS((int32_t)(signed char)io->io_Error);
        krnP4PutStr(" (");
        krnP4PutStr((signed char)io->io_Error == (signed char)TDERR_WriteProt
                    ? "TDERR_WriteProt" : "NOT TDERR_WriteProt");
        krnP4PutStr("), actual ");
        krnP4PutHex32(io->io_Actual);
        if (io->io_Error == 0)
        {
            krnP4PutStr(", ACCEPTED, MUST NOT BE\n");
            passed = 0;
        }
        else if ((signed char)io->io_Error != (signed char)TDERR_WriteProt
                 || io->io_Actual != 0)
        {
            /* Refused, but not in a form a filesystem can act on.  The same
               two properties A4 required of sdcard.device. */
            krnP4PutStr(", refused, but not as write protection with a zero"
                        " count\n");
            passed = 0;
        }
        else
            krnP4PutStr(", refused as write protection\n");
    }

    CloseDevice((struct IORequest *)io);

    /* And the same discovery code A2 hardened, over this device. */
    PartitionBase = (struct PartitionBase *)
        OpenLibrary("partition.library", 3);
    if (!PartitionBase)
    {
        krnP4PutStr("[fddev]  partition.library unavailable\n");
        passed = 0;
        goto out;
    }

    root = OpenRootPartition((CONST_STRPTR)"flashdisk.device", 0);
    krnP4PutStr("[fddev]  OpenRootPartition = ");
    krnP4PutHex32((uint32_t)(IPTR)root);
    krnP4PutStr("\n");
    if (!root)
        passed = 0;
    else
    {
        if (OpenPartitionTable(root) != 0)
        {
            krnP4PutStr("[fddev]  no partition table accepted\n");
            passed = 0;
        }
        else
        {
            struct PartitionHandle *ph;
            ULONG found = 0;

            krnP4PutStr("[fddev]  table type ");
            krnP4PutDec(root->table->type);
            krnP4PutStr("\n");

            ForeachNode(&root->table->list, ph)
            {
                ++found;
                krnP4PutStr("[fddev]    partition: start ");
                krnP4PutDec(ph->de.de_LowCyl * ph->de.de_Surfaces
                            * ph->de.de_BlocksPerTrack);
                krnP4PutStr(", sectors ");
                krnP4PutDec((ph->de.de_HighCyl - ph->de.de_LowCyl + 1)
                            * ph->de.de_Surfaces * ph->de.de_BlocksPerTrack);
                krnP4PutStr(", dostype ");
                krnP4PutHex32((uint32_t)ph->de.de_DosType);
                /* 0x46415401 is FAT\1, which the FAT handler registers for
                   FAT16 and which type byte 0x0e maps to. */
                krnP4PutStr(ph->de.de_DosType == 0x46415401UL
                            ? "  FAT16\n" : "  NOT FAT16\n");
                if (ph->de.de_DosType != 0x46415401UL)
                    passed = 0;
            }
            krnP4PutStr("[fddev]  partitions found ");
            krnP4PutDec(found);
            krnP4PutStr(found == 1 ? ", as expected\n" : ", EXPECTED ONE\n");
            if (found != 1)
                passed = 0;
            ClosePartitionTable(root);
        }

        /* A read through the public API, after discovery, like A2's. */
        {
            static uint32_t probe[128] P4_SRAMDATA;
            LONG r;
            unsigned int k;

            for (k = 0; k < 128; ++k)
                probe[k] = 0xa5a5a5a5U;
            r = ReadPartitionDataQ(root, probe, 512, 0);
            krnP4PutStr("[fddev]  read after discovery: result ");
            krnP4PutDecS((int32_t)r);
            if (r == 0)
            {
                krnP4PutStr(", hash ");
                krnP4PutHex32(krnP4HashBytes((const unsigned char *)probe,
                                             512));
                krnP4PutStr("\n");
            }
            else
            {
                krnP4PutStr(", UNUSABLE\n");
                passed = 0;
            }
        }

        CloseRootPartition(root);
    }

    CloseLibrary((struct Library *)PartitionBase);
    PartitionBase = NULL;

out:
    if (io)
        DeleteIORequest((struct IORequest *)io);
    if (port)
        DeleteMsgPort(port);

    krnP4PutStr("[fddev]  flash block device test ");
    krnP4PutStr(passed ? "passed\n" : "FAILED\n");
    return passed;
}

/*
 * The kernel is built freestanding and has no <string.h>, and this has to
 * ignore case anyway.  AROS's FAT handler does not hand back the eleven bytes
 * it read: GetVolumeIdentity() in rom/filesys/fat/volume.c keeps the first
 * character of each word and lowercases the rest, so the label AROSP4DEV
 * comes back as 'Arosp4dev'.  That is deliberate Amiga cosmetics rather than
 * a defect, and DOS name comparison is case-insensitive, so both spellings
 * address the volume.
 */
static int krnP4StrEqNoCase(const char *a, const char *b)
{
    while (*a && *b)
    {
        char ca = (*a >= 'a' && *a <= 'z') ? (char)(*a - 32) : *a;
        char cb = (*b >= 'a' && *b <= 'z') ? (char)(*b - 32) : *b;

        if (ca != cb)
            return 0;
        ++a;
        ++b;
    }
    return *a == *b;
}

/*
 * And the volume through DOS, which is what the whole exercise was for.
 *
 * Everything above this proves the bytes arrive.  This asks whether a program
 * can read a file off the flash volume by name, which is the thing that ends
 * the card handoffs.  The AROS.boot cross-check is the sharp part: the same
 * staged file exists on the card as FAT32 and in flash as FAT16, so the two
 * hashes must be equal.  If they are, two different filesystems on two
 * different media served identical bytes, and neither the generator nor the
 * FAT handler is quietly transforming anything.
 */
static int krnP4FlashVolumeDosTest(void)
{
    static struct InfoData info __attribute__((aligned(8)));
    static struct FileInfoBlock fib __attribute__((aligned(8)));
    BPTR lock;
    uint32_t flash_hash = 0, card_hash = 0;
    LONG flash_size, card_size;
    int passed = 1;

    krnP4PutStr("[fdvol]  FLASHDISK0P0: through DOS\n");

    lock = Lock((CONST_STRPTR)"FLASHDISK0P0:", SHARED_LOCK);
    krnP4PutStr("[fdvol]  Lock = ");
    krnP4PutHex32((uint32_t)(IPTR)lock);
    krnP4PutStr("\n");
    if (!lock)
    {
        krnP4PutStr("[fdvol]  the volume did not mount, IoErr ");
        krnP4PutDecS((int32_t)IoErr());
        krnP4PutStr("\n");
        return 0;
    }

    /* The volume name, which is the label the generator wrote as a root
       directory entry.  A lock on a volume examines to the volume itself. */
    if (Examine(lock, &fib))
    {
        krnP4PutStr("[fdvol]  volume name '");
        krnP4PutStr(fib.fib_FileName);
        krnP4PutStr("'");
        if (krnP4StrEqNoCase(fib.fib_FileName, "AROSP4DEV"))
            krnP4PutStr(", the generated label\n");
        else
        {
            krnP4PutStr(", EXPECTED AROSP4DEV\n");
            passed = 0;
        }
    }
    else
    {
        krnP4PutStr("[fdvol]  Examine() failed, IoErr ");
        krnP4PutDecS((int32_t)IoErr());
        krnP4PutStr("\n");
        passed = 0;
    }

    if (Info(lock, &info))
    {
        krnP4ReportState("[fdvol]  id_DiskState ", info.id_DiskState);
        krnP4PutStr("[fdvol]    blocks ");
        krnP4PutDec((uint32_t)info.id_NumBlocks);
        krnP4PutStr(", used ");
        krnP4PutDec((uint32_t)info.id_NumBlocksUsed);
        krnP4PutStr(", block size ");
        krnP4PutDec((uint32_t)info.id_BytesPerBlock);
        krnP4PutStr(", disk type ");
        krnP4PutHex32((uint32_t)info.id_DiskType);
        krnP4PutStr("\n");
        /*
         * ID_DOS_DISK, not the FAT\1 DosType the volume was mounted with:
         * FillDiskInfo() in rom/filesys/fat/volume.c reports ID_DOS_DISK for
         * every volume it serves, so Info() is not a way to tell FAT16 from
         * FAT32.  The card's volume reports the same value.  What has to hold
         * here is the state, because the device refuses every write.
         */
        if (info.id_DiskState != ID_WRITE_PROTECTED
            || (ULONG)info.id_DiskType != (ULONG)ID_DOS_DISK)
            passed = 0;
        if ((ULONG)info.id_NumBlocks != P4_FLASHDISK_SIZE / 512 - 2048)
        {
            /* The partition's own sector count, not the whole device's. */
            krnP4PutStr("[fdvol]  block count does not match the partition\n");
            passed = 0;
        }
    }
    else
    {
        krnP4PutStr("[fdvol]  Info() failed, IoErr ");
        krnP4PutDecS((int32_t)IoErr());
        krnP4PutStr("\n");
        passed = 0;
    }

    UnLock(lock);

    /* The cross-check.  Same file, two filesystems, two media. */
    flash_size = krnP4ProbeReadFile("FLASHDISK0P0:AROS.boot", &flash_hash);
    card_size  = krnP4ProbeReadFile("SYS:AROS.boot", &card_hash);
    krnP4PutStr("[fdvol]  AROS.boot from flash: ");
    krnP4PutDecS((int32_t)flash_size);
    krnP4PutStr(" bytes, hash ");
    krnP4PutHex32(flash_hash);
    krnP4PutStr("\n[fdvol]  AROS.boot from card:  ");
    krnP4PutDecS((int32_t)card_size);
    krnP4PutStr(" bytes, hash ");
    krnP4PutHex32(card_hash);
    krnP4PutStr("\n");
    if (flash_size <= 0)
    {
        krnP4PutStr("[fdvol]  UNREADABLE from flash\n");
        passed = 0;
    }
    else if (flash_size != card_size || flash_hash != card_hash)
    {
        krnP4PutStr("[fdvol]  THE TWO DIFFER, one of the two paths is"
                    " transforming bytes\n");
        passed = 0;
    }
    else
        krnP4PutStr("[fdvol]  identical, FAT16 in flash and FAT32 on the card"
                    " agree\n");

    /* And one mutation, which FAT has to refuse before it touches anything. */
    {
        BPTR fh = Open((CONST_STRPTR)"FLASHDISK0P0:probe.tmp", MODE_NEWFILE);
        LONG err = IoErr();

        krnP4PutStr("[fdvol]  Open(MODE_NEWFILE) = ");
        krnP4PutHex32((uint32_t)(IPTR)fh);
        krnP4PutStr(", IoErr ");
        krnP4PutDecS((int32_t)err);
        if (fh)
        {
            krnP4PutStr(", ACCEPTED, MUST NOT BE\n");
            Close(fh);
            passed = 0;
        }
        else if (err != ERROR_DISK_WRITE_PROTECTED)
        {
            krnP4PutStr(", refused for the wrong reason\n");
            passed = 0;
        }
        else
            krnP4PutStr(" (ERROR_DISK_WRITE_PROTECTED)\n");
    }

    krnP4PutStr("[fdvol]  DOS-level test ");
    krnP4PutStr(passed ? "passed\n" : "FAILED\n");
    return passed;
}
#endif /* P4_FLASHDISK_TEST */

/*
 * A5's non-interactive half: prove that code came off the card, and judge the
 * addresses it came to.
 *
 * The judging is why this lives in the kickstart rather than in the proof
 * files themselves.  The claim A5 has to support is that the command and the
 * library were loaded, not merely that something with the right name ran, and
 * the way to support it is that their addresses fall outside both the
 * kickstart and the flash package.  Only code linked into the kickstart knows
 * where those two are: the link script's symbols for one, and the range the
 * package loader recorded for the other.  A proof file asked to check itself
 * would have to be told those numbers, and then the check would only be as
 * good as the telling.
 *
 * The command's own output is not visible from here.  RTF_AFTERDOS runs
 * inside dos.library's boot process, whose standard output is not yet a
 * console, and opening ECON: from here would start the emergency console
 * handler early with the priority-10 spin the risk table describes.  So the
 * command is run with NIL: handles and judged by its exit code, which is
 * RETURN_OK only if it opened the library and the marker matched.  The
 * readable version of the same run is what typing it at the prompt produces.
 */

/*
 * Where the proof files must not be.
 *
 * Three intervals, not two, and the reason is worth stating because the first
 * version of this got it wrong and said so on the first run.  The kickstart is
 * not contiguous: in an XIP build its text and rodata are executed from flash
 * around 0x40000000 while its data and bss live in SRAM around 0x4ff00000.
 * Treating [__text_start, __kernel_end) as one interval spans everything
 * between, which is the entire 32 MB external window - so a segment correctly
 * loaded into PSRAM was reported as being inside the kickstart.  The link
 * script says the same thing about scanning that gap; this is the same trap in
 * a different place.
 */
static int krnP4InRange(IPTR address, IPTR lo, IPTR hi)
{
    return hi > lo && address >= lo && address < hi;
}

static int krnP4OutsideResident(IPTR address)
{
    /* The kickstart, as its two separate windows */
    if (krnP4InRange(address, (IPTR)__text_start, (IPTR)__rodata_end))
        return 0;
    if (krnP4InRange(address, (IPTR)__data_start, (IPTR)__kernel_end))
        return 0;
    /* And the flash package, wherever the loader put it */
    if (krnP4InRange(address, (IPTR)__esp32p4_modules_low,
                     (IPTR)__esp32p4_modules_high))
        return 0;
    return 1;
}

static void krnP4ReportAddress(const char *what, IPTR address)
{
    krnP4PutStr(what);
    krnP4PutHex32((uint32_t)address);
    krnP4PutStr(krnP4OutsideResident(address)
                ? "  outside the kickstart and the package\n"
                : "  INSIDE A RESIDENT RANGE, not a fresh load\n");
}

/*
 * A5's rejection half: four inputs that must fail, and fail cleanly.
 *
 * The gate names a missing file, a malformed one, one for the wrong machine
 * and one carrying a relocation the loader does not implement.  Only the
 * first of those is really about the medium; the other three are about what
 * rom/dos/internalloadseg_elf.c does with the bytes it is handed.  So they
 * are handed to it directly.
 *
 * InternalLoadSeg() takes the read, seek, allocate and free functions as an
 * argument array - LoadSeg() itself only supplies four wrappers around
 * Read(), Seek(), AllocMem() and FreeMem() - so a caller may serve the file
 * from anywhere.  Serving it from memory is what makes this test possible at
 * all here: there is no writable filesystem in this package, and putting
 * fixtures on the card would mean a card handoff per fixture.  The loader
 * sees exactly the same code path either way, which is the point.
 *
 * Each fixture is the *known-good* command mutated in one documented field,
 * not a blob invented for the purpose.  That matters: a hand-made file that
 * fails proves only that something about it was wrong, while a file that
 * differs from a proven-loadable one in a single field pins the rejection to
 * that field.  The unmodified bytes are loaded first through the same memory
 * path, so a failure below cannot be blamed on the path.
 */

struct p4MemFile
{
    const UBYTE *data;
    LONG         len;
    LONG         pos;
};

static AROS_UFH4(LONG, p4MemRead,
        AROS_UFHA(BPTR, handle, D1),
        AROS_UFHA(APTR, buffer, D2),
        AROS_UFHA(LONG, length, D3),
        AROS_UFHA(struct DosLibrary *, DOSBase, A6))
{
    AROS_USERFUNC_INIT

    struct p4MemFile *f = (struct p4MemFile *)handle;
    LONG left = f->len - f->pos;

    if (length > left)
        length = left;
    if (length > 0)
    {
        CopyMem((APTR)(f->data + f->pos), buffer, length);
        f->pos += length;
    }
    return length;

    AROS_USERFUNC_EXIT
}

static AROS_UFH4(LONG, p4MemSeek,
        AROS_UFHA(BPTR, handle, D1),
        AROS_UFHA(LONG, pos,    D2),
        AROS_UFHA(LONG, mode,   D3),
        AROS_UFHA(struct DosLibrary *, DOSBase, A6))
{
    AROS_USERFUNC_INIT

    struct p4MemFile *f = (struct p4MemFile *)handle;
    LONG old = f->pos;
    LONG want;

    /* Seek() returns the position it had, or -1, and leaves the position
       alone on a bad request.  Both matter: the ELF loader seeks back and
       forth through the section table and checks the return. */
    switch (mode)
    {
    case OFFSET_BEGINNING: want = pos;          break;
    case OFFSET_CURRENT:   want = f->pos + pos; break;
    case OFFSET_END:       want = f->len + pos; break;
    default:               return -1;
    }

    if (want < 0 || want > f->len)
        return -1;

    f->pos = want;
    return old;

    AROS_USERFUNC_EXIT
}

static AROS_UFH3(APTR, p4MemAlloc,
        AROS_UFHA(ULONG, length, D0),
        AROS_UFHA(ULONG, flags,  D1),
        AROS_UFHA(struct ExecBase *, SysBase, A6))
{
    AROS_USERFUNC_INIT

    return AllocMem(length, flags);

    AROS_USERFUNC_EXIT
}

static AROS_UFH3(void, p4MemFree,
        AROS_UFHA(APTR,  buffer, A1),
        AROS_UFHA(ULONG, length, D0),
        AROS_UFHA(struct ExecBase *, SysBase, A6))
{
    AROS_USERFUNC_INIT

    FreeMem(buffer, length);

    AROS_USERFUNC_EXIT
}

/* Load a memory image through the DOS ELF loader.  Returns the seglist. */
static BPTR krnP4LoadFromMemory(const UBYTE *data, LONG len)
{
    static LONG_FUNC funcarray[4];
    struct p4MemFile f;

    funcarray[0] = (LONG_FUNC)p4MemRead;
    funcarray[1] = (LONG_FUNC)p4MemAlloc;
    funcarray[2] = (LONG_FUNC)p4MemFree;
    funcarray[3] = (LONG_FUNC)p4MemSeek;

    f.data = data;
    f.len  = len;
    f.pos  = 0;

    return InternalLoadSeg((BPTR)&f, BNULL, funcarray, NULL);
}

/* ELF32 header and section header offsets, from <aros/kernel.h>'s layout and
   the psABI; spelled out so the mutation sites are visible here. */
#define P4ELF_E_MACHINE     18      /* UWORD */
#define P4ELF_E_SHOFF       32      /* ULONG */
#define P4ELF_E_SHENTSIZE   46      /* UWORD */
#define P4ELF_E_SHNUM       48      /* UWORD */
#define P4ELF_SH_TYPE        4      /* ULONG */
#define P4ELF_SH_OFFSET     16      /* ULONG */
#define P4ELF_SH_SIZE       20      /* ULONG */
#define P4ELF_SH_ENTSIZE    36      /* ULONG */
#define P4ELF_SHT_RELA       4
#define P4ELF_EM_386         3

static ULONG p4rd32(const UBYTE *p)
{
    return (ULONG)p[0] | ((ULONG)p[1] << 8) | ((ULONG)p[2] << 16)
                       | ((ULONG)p[3] << 24);
}

static UWORD p4rd16(const UBYTE *p)
{
    return (UWORD)(p[0] | (p[1] << 8));
}

/*
 * Find the first RELA entry in the image and return its offset, so its type
 * byte can be changed.  Returns 0 if there is none, which would make the
 * relocation fixture meaningless rather than passing by accident.
 */
static ULONG krnP4FirstRelaTypeOffset(const UBYTE *data, LONG len)
{
    ULONG shoff = p4rd32(data + P4ELF_E_SHOFF);
    UWORD shent = p4rd16(data + P4ELF_E_SHENTSIZE);
    UWORD shnum = p4rd16(data + P4ELF_E_SHNUM);
    UWORD i;

    if (!shoff || !shent || !shnum)
        return 0;
    if (shoff + (ULONG)shent * shnum > (ULONG)len)
        return 0;

    for (i = 0; i < shnum; ++i)
    {
        const UBYTE *sh = data + shoff + (ULONG)shent * i;

        if (p4rd32(sh + P4ELF_SH_TYPE) != P4ELF_SHT_RELA)
            continue;
        if (p4rd32(sh + P4ELF_SH_SIZE) < p4rd32(sh + P4ELF_SH_ENTSIZE))
            continue;

        /* ELF32 Rela is offset, info, addend; the type is the low byte of
           info, so four bytes past the entry start on a little endian
           target. */
        return p4rd32(sh + P4ELF_SH_OFFSET) + 4;
    }
    return 0;
}

/*
 * A mutated image must be refused, and refused with ERROR_NOT_EXECUTABLE.
 *
 * Not with the more specific error the ELF loader itself determined: the last
 * thing InternalLoadSeg() does on any failure is SetIoErr(
 * ERROR_NOT_EXECUTABLE), overwriting it, and its own comment acknowledges
 * that ELF "has a mess of SetIoErr() calls in it".  So 305 is the contract,
 * and an earlier version of this test expecting ERROR_BAD_HUNK was checking
 * for something the interface does not promise.
 *
 * The specific reason is not lost, it is just not in IoErr(): with dos built
 * with DOS_DEBUG=1 the loader names it on the console, and that is where the
 * evidence for *why* each case was refused comes from.
 */
static int krnP4JudgeRefusal(BPTR seg)
{
    LONG err = IoErr();

    krnP4PutStr("seglist ");
    krnP4PutHex32((uint32_t)(IPTR)seg);
    krnP4PutStr(", IoErr ");
    krnP4PutDecS((int32_t)err);

    if (seg)
    {
        krnP4PutStr("  ACCEPTED, MUST NOT BE\n");
        UnLoadSeg(seg);
        return 0;
    }
    if (err != ERROR_NOT_EXECUTABLE)
    {
        krnP4PutStr("  refused, but not as ERROR_NOT_EXECUTABLE\n");
        return 0;
    }
    krnP4PutStr("  refused as not executable\n");
    return 1;
}

static int krnP4A5RejectTest(void)
{
    static const char good[] = "SYS:C/sdload-test";
    static const char absent[] = "SYS:C/p4-no-such-command";
    UBYTE *image = NULL;
    LONG len = 0;
    int passed = 1;
    BPTR seg;

    krnP4PutStr("[a5rej]  rejection cases starting\n");

    /* 1. A path with nothing behind it.  The only case that is about the
          filesystem rather than about the bytes. */
    seg = LoadSeg((CONST_STRPTR)absent);
    krnP4PutStr("[a5rej]  missing file: LoadSeg = ");
    krnP4PutHex32((uint32_t)(IPTR)seg);
    krnP4PutStr(", IoErr ");
    krnP4PutDecS((int32_t)IoErr());
    if (!seg && IoErr() == ERROR_OBJECT_NOT_FOUND)
        krnP4PutStr("  refused as not found\n");
    else
    {
        krnP4PutStr("  NOT REFUSED AS EXPECTED\n");
        passed = 0;
        if (seg)
            UnLoadSeg(seg);
    }

    /* Read the known-good command, which the load proof has already run. */
    {
        BPTR fh = Open((CONST_STRPTR)good, MODE_OLDFILE);

        if (fh)
        {
            /*
             * Seek() reports the position it had, not the one it moved to.
             * So going to the end and then back to the beginning returns the
             * end, which is the length.  Two calls, in that order; a third
             * one would report zero and that is what an earlier version of
             * this did.
             */
            Seek(fh, 0, OFFSET_END);
            len = Seek(fh, 0, OFFSET_BEGINNING);

            if (len > 0 && (image = AllocMem(len, MEMF_ANY)) != NULL)
            {
                if (Read(fh, image, len) != len)
                {
                    FreeMem(image, len);
                    image = NULL;
                }
            }
            Close(fh);
        }
    }

    if (!image)
    {
        krnP4PutStr("[a5rej]  could not read the reference command;"
                    " the mutation cases cannot be judged\n");
        return 0;
    }

    krnP4PutStr("[a5rej]  reference image ");
    krnP4PutDec((uint32_t)len);
    krnP4PutStr(" bytes, machine ");
    krnP4PutDec((uint32_t)p4rd16(image + P4ELF_E_MACHINE));
    krnP4PutStr("\n");

    /* 2. The control: the same bytes through the memory path.  If this fails
          the path is at fault and nothing below means anything. */
    seg = krnP4LoadFromMemory(image, len);
    krnP4PutStr("[a5rej]  unmodified through memory: seglist ");
    krnP4PutHex32((uint32_t)(IPTR)seg);
    if (seg)
    {
        krnP4PutStr("  loaded, the memory path is sound\n");
        UnLoadSeg(seg);
    }
    else
    {
        krnP4PutStr("  DID NOT LOAD, IoErr ");
        krnP4PutDecS((int32_t)IoErr());
        krnP4PutStr("\n");
        passed = 0;
    }

    /* 3. Truncated to less than an ELF header. */
    seg = krnP4LoadFromMemory(image, 40);
    krnP4PutStr("[a5rej]  truncated to 40 bytes: ");
    if (!krnP4JudgeRefusal(seg))
        passed = 0;

    /* 4. The wrong machine, one field changed. */
    {
        UWORD was = p4rd16(image + P4ELF_E_MACHINE);

        image[P4ELF_E_MACHINE]     = P4ELF_EM_386;
        image[P4ELF_E_MACHINE + 1] = 0;

        seg = krnP4LoadFromMemory(image, len);
        krnP4PutStr("[a5rej]  e_machine ");
        krnP4PutDec((uint32_t)was);
        krnP4PutStr(" changed to ");
        krnP4PutDec((uint32_t)P4ELF_EM_386);
        krnP4PutStr(": ");
        if (!krnP4JudgeRefusal(seg))
            passed = 0;

        image[P4ELF_E_MACHINE]     = (UBYTE)(was & 0xFF);
        image[P4ELF_E_MACHINE + 1] = (UBYTE)(was >> 8);
    }

    /* 5. A relocation type the loader does not implement.  200 is
          unassigned in the RISC-V psABI, and the loader's default case
          prints the number it did not recognise. */
    {
        ULONG off = krnP4FirstRelaTypeOffset(image, len);

        if (!off || off >= (ULONG)len)
        {
            krnP4PutStr("[a5rej]  no RELA entry found;"
                        " the relocation case cannot be judged\n");
            passed = 0;
        }
        else
        {
            UBYTE was = image[off];

            image[off] = 200;
            seg = krnP4LoadFromMemory(image, len);
            krnP4PutStr("[a5rej]  relocation type ");
            krnP4PutDec((uint32_t)was);
            krnP4PutStr(" at file offset ");
            krnP4PutHex32(off);
            krnP4PutStr(" changed to 200: ");
            if (!krnP4JudgeRefusal(seg))
                passed = 0;
            image[off] = was;
        }
    }

    FreeMem(image, len);

    /* 6. And the system is still usable: the real thing still loads. */
    seg = LoadSeg((CONST_STRPTR)good);
    krnP4PutStr("[a5rej]  good load after the refusals: seglist ");
    krnP4PutHex32((uint32_t)(IPTR)seg);
    if (seg)
    {
        krnP4PutStr("  still works\n");
        UnLoadSeg(seg);
    }
    else
    {
        krnP4PutStr("  BROKEN\n");
        passed = 0;
    }

    krnP4PutStr("[a5rej]  rejection cases ");
    krnP4PutStr(passed ? "passed\n" : "FAILED\n");
    return passed;
}

static int krnP4A5Probe(void)
{
    struct Library *SDProofBase;
    BPTR seg;
    int passed = 1;

    krnP4PutStr("[a5]     load proof starting\n");
    krnP4PutStr("[a5]     excluded: kickstart code ");
    krnP4PutHex32((uint32_t)(IPTR)__text_start);
    krnP4PutStr(" - ");
    krnP4PutHex32((uint32_t)(IPTR)__rodata_end);
    krnP4PutStr(", kickstart data ");
    krnP4PutHex32((uint32_t)(IPTR)__data_start);
    krnP4PutStr(" - ");
    krnP4PutHex32((uint32_t)(IPTR)__kernel_end);
    krnP4PutStr(",\n[a5]               package ");
    krnP4PutHex32((uint32_t)(IPTR)__esp32p4_modules_low);
    krnP4PutStr(" - ");
    krnP4PutHex32((uint32_t)(IPTR)__esp32p4_modules_high);
    krnP4PutStr("\n");

    /*
     * The library first, then the command.  Not the order the phase describes
     * them in, and deliberately so: the command is run through RunCommand(),
     * and a command that hangs takes everything after it with it - which it
     * did on the first attempt, losing the library result that had nothing to
     * do with the failure.  The two tests are independent, so the one that
     * cannot hang goes first.
     */
    /* The library, through lddemon */
    SDProofBase = OpenLibrary((CONST_STRPTR)"sdproof.library", 1);
    krnP4PutStr("[a5]     OpenLibrary(\"sdproof.library\", 1) = ");
    krnP4PutHex32((uint32_t)(IPTR)SDProofBase);
    krnP4PutStr("\n");
    if (!SDProofBase)
    {
        krnP4PutStr("[a5]     library did NOT open\n");
        passed = 0;
    }
    else
    {
        ULONG marker = SDProofQuery(SDProofBase, SDPROOF_Q_MARKER);
        IPTR id_addr = (IPTR)SDProofQuery(SDProofBase, SDPROOF_Q_ID_ADDR);

        krnP4ReportAddress("[a5]     library base at ", (IPTR)SDProofBase);
        krnP4ReportAddress("[a5]     library id string at ", id_addr);

        krnP4PutStr("[a5]     SDProofQuery marker ");
        krnP4PutHex32(marker);
        krnP4PutStr(" expected ");
        krnP4PutHex32(SDPROOF_EXPECTED);
        krnP4PutStr(marker == SDPROOF_EXPECTED ? "  match\n" : "  MISMATCH\n");
        if (marker != SDPROOF_EXPECTED)
            passed = 0;
        if (!krnP4OutsideResident((IPTR)SDProofBase)
            || !krnP4OutsideResident(id_addr))
            passed = 0;

        CloseLibrary(SDProofBase);
    }


    /*
     * Both commands are loaded, and only one of them is run here.
     *
     * A normal AROS command cannot be RunCommand()ed from this context, and
     * that is not a defect of this port.  The first entry in the
     * PROGRAM_ENTRIES chain is __startup_fromwb(), which decides it was
     * started from Workbench when the calling process has no CLI structure
     * and then does WaitPort() for a WBStartup message.  RTF_AFTERDOS runs
     * inside dos.library's boot process, which has no CLI, so that wait never
     * ends - and because it is the boot process, the whole boot stops with
     * it, Shell included.  Measured: the last line on the console was
     * "Entering __startup_fromwb()".
     *
     * So the non-interactive route runs sdload-test, which is built without
     * the C startup and therefore has no such chain, and sdboot-test is
     * loaded and address-checked here but left to be run from the Shell,
     * where a CLI exists.  Both halves of A5 are covered, by the route each
     * one actually fits.
     */
    {
        static const struct
        {
            const char *path;
            int run;
        } commands[] =
        {
            { "SYS:C/sdload-test", 1 },
            { "SYS:C/sdboot-test", 0 }
        };
        unsigned int c;

        for (c = 0; c < sizeof(commands) / sizeof(commands[0]); ++c)
        {
            seg = LoadSeg((CONST_STRPTR)commands[c].path);
            krnP4PutStr("[a5]     LoadSeg(\"");
            krnP4PutStr(commands[c].path);
            krnP4PutStr("\") = ");
            krnP4PutHex32((uint32_t)(IPTR)seg);
            krnP4PutStr("\n");
            if (!seg)
            {
                krnP4PutStr("[a5]     did NOT load, IoErr ");
                krnP4PutDecS((int32_t)IoErr());
                krnP4PutStr("\n");
                passed = 0;
                continue;
            }

            krnP4ReportAddress("[a5]     segment at ", (IPTR)BADDR(seg));

            if (!commands[c].run)
            {
                krnP4PutStr("[a5]     not run here: it carries the C startup,"
                            " whose first chain entry waits for a Workbench\n"
                            "[a5]     message when the caller has no CLI."
                            "  Run it from the Shell instead.\n");
                UnLoadSeg(seg);
                continue;
            }

            {
                LONG rc;
                BPTR in, out, oldin, oldout;

                in = Open((CONST_STRPTR)"NIL:", MODE_OLDFILE);
                out = Open((CONST_STRPTR)"NIL:", MODE_NEWFILE);
                oldin = SelectInput(in);
                oldout = SelectOutput(out);

                /* The empty argument string still needs its terminating
                   newline; ReadArgs() would otherwise run off the end. */
                rc = RunCommand(seg, AROS_STACKSIZE, (STRPTR)"\n", 1);

                SelectInput(oldin);
                SelectOutput(oldout);
                if (in)
                    Close(in);
                if (out)
                    Close(out);

                krnP4PutStr("[a5]     RunCommand returned ");
                krnP4PutDecS((int32_t)rc);
                if (rc == RETURN_OK)
                    krnP4PutStr(", the command reports success\n");
                else
                {
                    krnP4PutStr(", NOT RETURN_OK\n");
                    passed = 0;
                }
            }

            UnLoadSeg(seg);
        }
    }

    krnP4PutStr("[a5]     load proof ");
    krnP4PutStr(passed ? "passed\n" : "FAILED\n");

    if (!krnP4A5RejectTest())
        passed = 0;

    return passed;
}
#endif /* P4_A5_PROBE */

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
#ifdef P4_AFTERDOS_PROBE
        (void)krnP4AfterDosProbe();
#endif
#ifdef P4_FLASHDISK_TEST
        (void)krnP4FlashDiskDeviceTest();
        (void)krnP4FlashVolumeDosTest();
#endif
#ifdef P4_A5_PROBE
        (void)krnP4A5Probe();
#endif
#ifdef P4_B5_CONCURRENT_STRESS
        krnP4PanelProbe();
#endif
        CloseLibrary((struct Library *)DOSBase);
        DOSBase = NULL;
    }

    return NULL;

    AROS_USERFUNC_EXIT
}

/*
 * RTF_AFTERDOS, so cliinit.c starts it once SYS: and the boot assigns exist.
 *
 * Priority -126, which is below lddemon, shell and shellcommands at -123 and
 * is the correction to a first attempt at 0.  At 0 this ran before lddemon was
 * initialised, and the A5 probe's OpenLibrary() of a disk-based library then
 * did not fail cleanly - it hung, taking the boot with it, with no trap and no
 * further output.  A library that is not resident has to be fetched by
 * lddemon, so asking for one before lddemon exists is asking the wrong
 * question.  Still ahead of the Shell reaching a prompt, since that happens
 * in __dos_Boot() after this pass returns.
 */
const struct Resident krnP4AfterDosResident =
{
    RTC_MATCHWORD,
    (struct Resident *)&krnP4AfterDosResident,
    (APTR)((const char *)&krnP4AfterDosResident + sizeof(struct Resident)),
    RTF_AFTERDOS,
    1,
    NT_TASK,
    -126,
#ifdef P4_B5_CONCURRENT_STRESS
    "esp32p4 B5 stress",
    "esp32p4 B5 stress 1.0",
#else
    "esp32p4 sysfs probe",
    "esp32p4 sysfs probe 1.0",
#endif
    &krnP4AfterDosInit
};

#endif /* P4_AFTERDOS_PROBE || P4_B5_CONCURRENT_STRESS */

#if defined(P4_SDCARD_DEVICE_TEST) || defined(P4_B5_CONCURRENT_STRESS)
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

#ifdef P4_B5_CONCURRENT_STRESS

/*
 * B5's sustained gate uses the already hardware-verified sdcard.device, not
 * another controller implementation.  Each iteration asks that device for a
 * 128-sector CMD18 range into PSRAM while the display GDMA continuously reads
 * its separately reserved framebuffer.  A second PSRAM allocation is then
 * filled, written through the cache and verified from memory.
 *
 * Both SD ranges are card-referenced file data beyond the 32-bit byte-offset
 * boundary.  They therefore exercise READ64 and detect repeated/misdirected
 * sectors instead of merely proving that some zero-filled area was read.
 */
#define P4B5_SD_SECTORS        128UL
#define P4B5_SD_BYTES          (P4B5_SD_SECTORS * 512UL)
#define P4B5_SD_ALLOC_BYTES    (1024UL * 1024UL)
#define P4B5_PSRAM_BYTES       (1024UL * 1024UL)

struct p4b5_sd_range
{
    uint32_t lba;
    uint32_t hash;
};

static const struct p4b5_sd_range p4b5_sd_ranges[] =
{
    {  8388608UL, 0x894be777UL },
    { 10000000UL, 0xa3d171beUL }
};

struct p4b5_stress_state
{
    struct MsgPort *port;
    struct IOStdReq *io;
    unsigned char *sd_raw;
    unsigned char *sd_data;
    unsigned char *psram_raw;
    volatile uint32_t *psram_data;
    unsigned long sd_reads;
    unsigned long psram_passes;
    unsigned long failures;
};

static struct p4b5_stress_state p4b5_stress;

static int krnP4B5StressPSRAMAddress(const void *address,
                                     unsigned long bytes)
{
    IPTR first = (IPTR)address;
    IPTR last;

    if (first > ~(IPTR)0 - bytes)
        return 0;
    last = first + bytes;
    return first >= P4_PSRAM_WINDOW_BASE && last <= P4_FB_BASE;
}

static int krnP4B5StressBegin(void)
{
    struct p4b5_stress_state *s = &p4b5_stress;
    LONG open_error;

    s->port = CreateMsgPort();
    if (s->port)
        s->io = (struct IOStdReq *)CreateIORequest(s->port, sizeof(*s->io));
    if (!s->port || !s->io)
    {
        krnP4PutStr("[b5stress] IO request unavailable\n");
        return 0;
    }

    open_error = OpenDevice("sdcard.device", 0,
                            (struct IORequest *)s->io, 0);
    if (open_error != 0)
    {
        krnP4PutStr("[b5stress] sdcard.device unavailable, error ");
        krnP4PutDec((uint32_t)open_error);
        krnP4PutStr("\n");
        return 0;
    }

    /* One megabyte cannot fit in either internal heap, which makes this a
       PSRAM allocation without inventing a nonstandard memory attribute. */
    s->sd_raw = AllocMem(P4B5_SD_ALLOC_BYTES + 64,
                         MEMF_PUBLIC | MEMF_CLEAR);
    s->psram_raw = AllocMem(P4B5_PSRAM_BYTES + 64,
                            MEMF_PUBLIC | MEMF_CLEAR);
    if (!s->sd_raw || !s->psram_raw)
    {
        krnP4PutStr("[b5stress] PSRAM buffers unavailable\n");
        return 0;
    }

    s->sd_data = (unsigned char *)(((IPTR)s->sd_raw + 63) & ~(IPTR)63);
    s->psram_data = (volatile uint32_t *)
        (((IPTR)s->psram_raw + 63) & ~(IPTR)63);
    if (!krnP4B5StressPSRAMAddress(s->sd_data, P4B5_SD_BYTES) ||
        !krnP4B5StressPSRAMAddress((const void *)s->psram_data,
                                   P4B5_PSRAM_BYTES))
    {
        krnP4PutStr("[b5stress] buffers are not in reserved-safe PSRAM\n");
        return 0;
    }

    krnP4PutStr("[b5stress] started: framebuffer reserved at ");
    krnP4PutHex32(P4_FB_BASE);
    krnP4PutStr(", SD 128-sector READ64 plus 1 MB PSRAM pass per second\n");
    return 1;
}

static int krnP4B5StressStep(unsigned long second)
{
    struct p4b5_stress_state *s = &p4b5_stress;
    const struct p4b5_sd_range *range =
        &p4b5_sd_ranges[second %
                        (sizeof(p4b5_sd_ranges) / sizeof(p4b5_sd_ranges[0]))];
    unsigned long words = P4B5_PSRAM_BYTES / sizeof(uint32_t);
    uint32_t seed = (uint32_t)second * 0x7F4A7C15UL ^ 0xA55A3CC3UL;
    uint32_t hash;
    unsigned long i;

    if (!krnP4SDCardRead(s->io, range->lba, P4B5_SD_SECTORS, 1,
                         s->sd_data))
    {
        krnP4PutStr("[b5stress] SD read FAILED at second ");
        krnP4PutDec((uint32_t)second);
        krnP4PutStr(", error ");
        krnP4PutDec((uint32_t)(unsigned char)s->io->io_Error);
        krnP4PutStr(", actual ");
        krnP4PutDec(s->io->io_Actual);
        krnP4PutStr("\n");
        s->failures++;
        return 0;
    }
    hash = krnP4SDCardHash(s->sd_data, P4B5_SD_BYTES);
    if (hash != range->hash)
    {
        krnP4PutStr("[b5stress] SD hash FAILED at LBA ");
        krnP4PutDec(range->lba);
        krnP4PutStr(", expected ");
        krnP4PutHex32(range->hash);
        krnP4PutStr(", got ");
        krnP4PutHex32(hash);
        krnP4PutStr("\n");
        s->failures++;
        return 0;
    }
    s->sd_reads++;

    for (i = 0; i < words; ++i)
        s->psram_data[i] = ((uint32_t)i * 0x9E3779B9UL) ^ seed;
    krnP4CacheSyncData((void *)s->psram_data, P4B5_PSRAM_BYTES);
    for (i = 0; i < words; ++i)
    {
        uint32_t expected = ((uint32_t)i * 0x9E3779B9UL) ^ seed;
        uint32_t got = s->psram_data[i];

        if (got != expected)
        {
            krnP4PutStr("[b5stress] PSRAM FAILED at second ");
            krnP4PutDec((uint32_t)second);
            krnP4PutStr(", offset ");
            krnP4PutHex32((uint32_t)(i * sizeof(uint32_t)));
            krnP4PutStr(", expected ");
            krnP4PutHex32(expected);
            krnP4PutStr(", got ");
            krnP4PutHex32(got);
            krnP4PutStr("\n");
            s->failures++;
            return 0;
        }
    }
    s->psram_passes++;

    if ((second % 60) == 59)
    {
        krnP4PutStr("[b5stress] progress ");
        krnP4PutDec((uint32_t)(second + 1));
        krnP4PutStr(" s, SD ");
        krnP4PutDec((uint32_t)(s->sd_reads * P4B5_SD_BYTES /
                               (1024UL * 1024UL)));
        krnP4PutStr(" MB verified, PSRAM passes ");
        krnP4PutDec((uint32_t)s->psram_passes);
        krnP4PutStr("\n");
    }
    return 1;
}

static void krnP4B5StressEnd(unsigned long completed, int passed)
{
    struct p4b5_stress_state *s = &p4b5_stress;

    krnP4PutStr("[b5stress] ");
#ifdef P4_B5_STRESS_SMOKE
    krnP4PutStr("SMOKE ");
#endif
    krnP4PutStr(passed && !s->failures ? "PASSED " : "FAILED ");
    krnP4PutDec((uint32_t)completed);
    krnP4PutStr(" s, SD reads ");
    krnP4PutDec((uint32_t)s->sd_reads);
    krnP4PutStr(" x 128 sectors, PSRAM 1 MB passes ");
    krnP4PutDec((uint32_t)s->psram_passes);
    krnP4PutStr(", failures ");
    krnP4PutDec((uint32_t)s->failures);
    krnP4PutStr("\n");

    if (s->io && s->io->io_Device)
        CloseDevice((struct IORequest *)s->io);
    if (s->sd_raw)
        FreeMem(s->sd_raw, P4B5_SD_ALLOC_BYTES + 64);
    if (s->psram_raw)
        FreeMem(s->psram_raw, P4B5_PSRAM_BYTES + 64);
    if (s->io)
        DeleteIORequest((struct IORequest *)s->io);
    if (s->port)
        DeleteMsgPort(s->port);
}

#endif /* P4_B5_CONCURRENT_STRESS */

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
#endif /* P4_SDCARD_DEVICE_TEST || P4_B5_CONCURRENT_STRESS */

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

/*
 * What the calibration measured, not just what it chose.
 *
 * The chosen setting alone is not evidence: a boot that picks index 17 tells
 * you nothing about whether 16 and 18 also worked.  The pass masks are the
 * measurement, so they are printed as they are, one character per candidate,
 * and the window width is the number that says whether the rate is safe or
 * merely lucky today.
 */
static void krnP4ReportPSRAMTuning(const struct P4PSRAMInfo *psram)
{
    static const char *phase_name[4] =
        { "67.5", "78.75", "90", "101.25" };
    const struct P4PSRAMTuning *t = &psram->tuning;
    unsigned int i;

    krnP4PutStr("[psram]  tuning phases  ");
    for (i = 0; i < P4_PSRAM_PHASE_COUNT; ++i)
        krnP4PutStr((t->phase_pass & (1u << i)) ? "1" : "0");
    krnP4PutStr("  window ");
    krnP4PutDec(t->phase_window);
    if (t->phase_window)
    {
        krnP4PutStr(", chose ");
        krnP4PutStr(phase_name[t->phase & 3]);
        krnP4PutStr(" degrees");
    }
    krnP4PutStr("\n");

    krnP4PutStr("[psram]  tuning delays  ");
    for (i = 0; i < P4_PSRAM_DELAY_COUNT; ++i)
        krnP4PutStr((t->delay_pass & (1UL << i)) ? "1" : "0");
    krnP4PutStr("\n[psram]                 ");
    /* A marker under the chosen index, so the middle is visible rather than
       asserted. */
    for (i = 0; i < P4_PSRAM_DELAY_COUNT; ++i)
        krnP4PutStr((t->tuned && i == t->delay_index) ? "^" : " ");
    krnP4PutStr("\n[psram]  window ");
    krnP4PutDec(t->delay_window);
    krnP4PutStr(" of ");
    krnP4PutDec(P4_PSRAM_DELAY_COUNT);
    krnP4PutStr(", index ");
    krnP4PutDec(t->delay_index);
    krnP4PutStr(" means data delay ");
    krnP4PutDec(t->data_delay);
    krnP4PutStr(", strobe delay ");
    krnP4PutDec(t->dqs_delay);
    krnP4PutStr("\n");

    if (t->tuned && !psram->fell_back)
    {
        krnP4PutStr("[psram]  calibrated, running at ");
        krnP4PutDec((uint32_t)(psram->clock_hz / 1000000));
        krnP4PutStr(" MHz\n");
    }
    else
    {
        krnP4PutStr("[psram]  NOT calibrated, fell back to ");
        krnP4PutDec((uint32_t)(psram->clock_hz / 1000000));
        krnP4PutStr(" MHz");
        if (!t->phase_window)
            krnP4PutStr(" - no phase reproduced the reference\n");
        else if (t->delay_window < 2)
            krnP4PutStr(" - no delay window wider than one step\n");
        else
            krnP4PutStr(" - the chosen setting did not hold\n");
    }
}

#ifdef P4_PSRAM_STRESS
/*
 * The whole window, every word, and how fast it goes.
 *
 * krnPSRAMVerify() checks one word per megabyte, which is the right test for
 * "is the mapping there at all" and says nothing about a bus running eight
 * times faster than it did.  A calibration window is a statement about 128
 * bytes at one address; this is the statement about 8,388,608 words at every
 * address, which is what a filesystem cache and a framebuffer will actually
 * ask for.
 *
 * Runs before the memory header exists, so the whole window is scratch.  It
 * is also the last moment at which it is: everything after this point has an
 * owner.
 */

/* Time a pass, in bytes per second, from the 16 MHz counter.  Returned as a
   whole number of MB/s because the measurement is not more precise than that
   and printing more digits would suggest otherwise. */
static uint32_t krnP4RateMB(uint64_t ticks, unsigned long bytes)
{
    uint64_t hz;

    if (ticks == 0)
        return 0;
    hz = ((uint64_t)bytes * P4_SYSTIMER_HZ) / ticks;
    return (uint32_t)(hz / (1024 * 1024));
}

static void krnP4ReportMismatch(const char *what, unsigned long offset,
                                uint32_t expected, uint32_t got)
{
    krnP4PutStr("[stress] ");
    krnP4PutStr(what);
    krnP4PutStr(" FAILED at offset ");
    krnP4PutHex32((uint32_t)offset);
    krnP4PutStr(", expected ");
    krnP4PutHex32(expected);
    krnP4PutStr(", got ");
    krnP4PutHex32(got);
    krnP4PutStr("\n");
}

/*
 * Every word carries a value derived from its own address, so a read that
 * returns the right value for the wrong address fails.  That is the failure
 * an address-line fault or a wrong page mapping produces, and a constant
 * pattern cannot see it: aliased memory holds the constant just fine.
 */
static int krnP4StressAddresses(unsigned long size)
{
    volatile uint32_t *base = (volatile uint32_t *)P4_PSRAM_WINDOW_BASE;
    unsigned long words = size / 4;
    unsigned long i;
    uint64_t t0, t1, t2;

    t0 = krnTimerCount();
    for (i = 0; i < words; ++i)
        base[i] = (uint32_t)(i * 4) ^ 0x5A5A5A5AUL;
    t1 = krnTimerCount();

    for (i = 0; i < words; ++i)
    {
        uint32_t want = (uint32_t)(i * 4) ^ 0x5A5A5A5AUL;
        uint32_t got = base[i];

        if (got != want)
        {
            krnP4ReportMismatch("address uniqueness", i * 4, want, got);
            return 0;
        }
    }
    t2 = krnTimerCount();

    krnP4PutStr("[stress] address uniqueness over ");
    krnP4PutDec((uint32_t)(size / (1024 * 1024)));
    krnP4PutStr(" MB passed, write ");
    krnP4PutDec(krnP4RateMB(t1 - t0, size));
    krnP4PutStr(" MB/s, read ");
    krnP4PutDec(krnP4RateMB(t2 - t1, size));
    krnP4PutStr(" MB/s\n");
    return 1;
}

/*
 * The four patterns that matter for a bus, over every word.
 *
 * All-zeros and all-ones find a line stuck at the other value.  The two
 * alternating patterns put a transition on every line in both directions,
 * which is what a sampling point too close to an edge fails on; they are the
 * patterns a marginal calibration survives an address test but not this one.
 */
static int krnP4StressPatterns(unsigned long size)
{
    static const uint32_t patterns[4] =
        { 0x00000000UL, 0xFFFFFFFFUL, 0xAAAAAAAAUL, 0x55555555UL };
    volatile uint32_t *base = (volatile uint32_t *)P4_PSRAM_WINDOW_BASE;
    unsigned long words = size / 4;
    unsigned int p;

    for (p = 0; p < 4; ++p)
    {
        unsigned long i;

        for (i = 0; i < words; ++i)
            base[i] = patterns[p];
        for (i = 0; i < words; ++i)
            if (base[i] != patterns[p])
            {
                krnP4PutStr("[stress] pattern ");
                krnP4PutHex32(patterns[p]);
                krnP4ReportMismatch("", i * 4, patterns[p], base[i]);
                return 0;
            }
    }

    krnP4PutStr("[stress] four bus patterns over ");
    krnP4PutDec((uint32_t)(size / (1024 * 1024)));
    krnP4PutStr(" MB passed\n");
    return 1;
}

/*
 * Sequential read bandwidth, which is the number B5 depends on.
 *
 * One linear pass over the whole window against a 128 KB cache misses almost
 * everywhere, so this measures the PSRAM path and not the cache.  The words
 * are summed rather than discarded because a loop whose result is unused is a
 * loop the compiler may delete, and the sum is printed so the reader can see
 * it was not.
 *
 * The threshold is not arbitrary.  An 800x1280 RGB565 frame at the panel's
 * 33.82 Hz is 69.3 MB/s of scanout, and a rotation pass reads and writes
 * another frame each, so the display alone can ask for three times that.  100
 * MB/s is the floor at which scanout is possible at all; it is not comfort.
 */
static uint32_t krnP4StressBandwidth(unsigned long size)
{
    volatile uint32_t *base = (volatile uint32_t *)P4_PSRAM_WINDOW_BASE;
    unsigned long words = size / 4;
    unsigned long i;
    uint32_t sum = 0;
    uint32_t rate;
    uint64_t t0, t1;

    t0 = krnTimerCount();
    for (i = 0; i < words; i += 8)
    {
        sum += base[i + 0];
        sum += base[i + 1];
        sum += base[i + 2];
        sum += base[i + 3];
        sum += base[i + 4];
        sum += base[i + 5];
        sum += base[i + 6];
        sum += base[i + 7];
    }
    t1 = krnTimerCount();

    rate = krnP4RateMB(t1 - t0, size);
    krnP4PutStr("[stress] sequential read ");
    krnP4PutDec(rate);
    krnP4PutStr(" MB/s, checksum ");
    krnP4PutHex32(sum);
    krnP4PutStr(rate >= 100 ? "  at or above the 100 MB/s floor\n"
                            : "  BELOW the 100 MB/s floor\n");
    return rate;
}

/*
 * How fast is the CPU, actually?
 *
 * This port configures no CPU clock at all: it inherits whatever the
 * second-stage bootloader left, and that bootloader was built for a different
 * application's configuration.  A bandwidth number is meaningless without
 * this one, because a loop of loads is bounded by the core before it is
 * bounded by the bus.  mcycle counts core cycles, the system timer counts a
 * fixed 16 MHz, so the ratio is the answer and nothing has to be assumed.
 */
static uint32_t krnP4MeasureCPUMHz(void)
{
    uint64_t c0, c1, t0, t1;
    unsigned long spin;

    c0 = (uint64_t)csr_read(mcycle);
    t0 = krnTimerCount();
    /* Long enough that the counter's 62.5 ns resolution is noise. */
    for (spin = 0; spin < 200000; ++spin)
        asm volatile ("" ::: "memory");
    t1 = krnTimerCount();
    c1 = (uint64_t)csr_read(mcycle);

    if (t1 <= t0)
        return 0;
    return (uint32_t)(((c1 - c0) * P4_SYSTIMER_HZ) / ((t1 - t0) * 1000000UL));
}

/*
 * The same pass without volatile, and the same pass over internal SRAM.
 *
 * Two questions the volatile PSRAM number cannot answer on its own.  Volatile
 * loads may not be reordered against each other, so the core cannot issue the
 * next one while the last is still in flight; on a machine that stalls on
 * load-use that is the measurement rather than the memory.  And a pass over
 * SRAM has no external bus in it at all, so whatever it reaches is the ceiling
 * this loop can reach for any memory.
 *
 * The sum is returned and printed for the same reason as before: a loop whose
 * result nobody uses is a loop the compiler is entitled to delete.
 */
static uint32_t krnP4ReadRate(const uint32_t *base, unsigned long bytes,
                              unsigned long repeats, uint32_t *out_sum)
{
    unsigned long words = bytes / 4;
    unsigned long r, i;
    uint32_t sum = 0;
    uint64_t t0, t1;

    t0 = krnTimerCount();
    for (r = 0; r < repeats; ++r)
        for (i = 0; i < words; i += 8)
        {
            sum += base[i + 0];
            sum += base[i + 1];
            sum += base[i + 2];
            sum += base[i + 3];
            sum += base[i + 4];
            sum += base[i + 5];
            sum += base[i + 6];
            sum += base[i + 7];
        }
    t1 = krnTimerCount();

    if (out_sum)
        *out_sum = sum;
    return krnP4RateMB(t1 - t0, bytes * repeats);
}

/*
 * The same loop, but fetched from SRAM instead of flash.
 *
 * This image executes out of flash, so every instruction is a flash access
 * through the cache.  A loop of loads therefore has two costs, the data and
 * the instructions, and the numbers above cannot tell them apart: a loop that
 * is fetch-bound produces the same figure whatever the data bus does, which
 * is exactly the pattern the 20 MHz and 200 MHz runs showed.  Moving only the
 * loop into SRAM changes the fetch cost and nothing else, so the difference is
 * attributable.
 */
P4_SRAMCODE static uint32_t krnP4ReadRateSRAM(const uint32_t *base,
                                              unsigned long words,
                                              unsigned long repeats,
                                              uint32_t *out_sum)
{
    unsigned long r, i;
    uint32_t sum = 0;

    for (r = 0; r < repeats; ++r)
        for (i = 0; i < words; i += 8)
        {
            sum += base[i + 0];
            sum += base[i + 1];
            sum += base[i + 2];
            sum += base[i + 3];
            sum += base[i + 4];
            sum += base[i + 5];
            sum += base[i + 6];
            sum += base[i + 7];
        }

    if (out_sum)
        *out_sum = sum;
    return 0;
}

static uint32_t krnP4TimedSRAMRead(const uint32_t *base, unsigned long bytes,
                                   unsigned long repeats, uint32_t *out_sum)
{
    uint64_t t0, t1;

    t0 = krnTimerCount();
    (void)krnP4ReadRateSRAM(base, bytes / 4, repeats, out_sum);
    t1 = krnTimerCount();

    return krnP4RateMB(t1 - t0, bytes * repeats);
}

static void krnP4StressCompare(unsigned long size)
{
    uint32_t sum = 0, rate;

    krnP4PutStr("[stress] cpu ");
    krnP4PutDec(krnP4MeasureCPUMHz());
    krnP4PutStr(" MHz measured from mcycle against the 16 MHz timer\n");

    rate = krnP4ReadRate((const uint32_t *)P4_PSRAM_WINDOW_BASE, size, 1, &sum);
    krnP4PutStr("[stress] psram read, not volatile  ");
    krnP4PutDec(rate);
    krnP4PutStr(" MB/s, checksum ");
    krnP4PutHex32(sum);
    krnP4PutStr("\n");

    /*
     * SRAM, over the data-only heap region.  Read-only, so exec's ownership
     * of it does not matter, and 256 KB repeated sixty-four times is the same
     * sixteen megabytes of traffic without an external bus in the path.
     */
    rate = krnP4ReadRate((const uint32_t *)P4_HEAP_HIGH_BASE, 256 * 1024, 64,
                         &sum);
    krnP4PutStr("[stress] sram read, same loop      ");
    krnP4PutDec(rate);
    krnP4PutStr(" MB/s, checksum ");
    krnP4PutHex32(sum);
    krnP4PutStr("\n");

    /* And both again with the loop itself fetched from SRAM. */
    rate = krnP4TimedSRAMRead((const uint32_t *)P4_PSRAM_WINDOW_BASE, size, 1,
                              &sum);
    krnP4PutStr("[stress] psram read, loop in sram  ");
    krnP4PutDec(rate);
    krnP4PutStr(" MB/s, checksum ");
    krnP4PutHex32(sum);
    krnP4PutStr("\n");

    rate = krnP4TimedSRAMRead((const uint32_t *)P4_HEAP_HIGH_BASE, 256 * 1024,
                              64, &sum);
    krnP4PutStr("[stress] sram read, loop in sram   ");
    krnP4PutDec(rate);
    krnP4PutStr(" MB/s, checksum ");
    krnP4PutHex32(sum);
    krnP4PutStr("\n");
}

static void krnP4PSRAMStress(unsigned long size)
{
    if (size == 0)
        return;

    krnP4PutStr("[stress] the whole window, while it is still nobody's\n");

    if (!krnP4StressAddresses(size))
        return;
    if (!krnP4StressPatterns(size))
        return;
    (void)krnP4StressBandwidth(size);
    krnP4StressCompare(size);
}
#endif /* P4_PSRAM_STRESS */

#ifdef P4_PANEL_PROBE
/*
 * B2: the panel's power, reset and backlight, and nothing else.
 *
 * The data path is not touched here - no LDO, no DSI, no command to the panel -
 * so the worst this can do is leave a rail off.  It is run twice on purpose:
 * the gate asks that a repeated sequence neither powers the board off nor
 * enables unrelated hardware, and the only way to show that is to repeat it
 * and compare the expander's registers to what they were.
 */
static const char *krnP4I2CName(int r)
{
    return r == P4_I2C_OK       ? "ok"
         : r == P4_I2C_NACK     ? "no answer at the address"
         : r == P4_I2C_TIMEOUT  ? "the bus timed out"
         : r == P4_I2C_ARBLOST  ? "arbitration lost"
         : r == P4_I2C_STUCK    ? "the controller never finished"
         : r == P4_I2C_BUSY     ? "the bus stayed busy"
         : r == P4_I2C_TOOLONG  ? "the transfer is longer than the fifo"
         : r == P4_I2C_NOTREADY ? "the controller was not initialised"
         : r == P4_I2C_MISMATCH ? "the device kept something else"
                                : "unknown";
}

static void krnP4ReportPanel(const char *what, const struct P4PanelState *st)
{
    krnP4PutStr("[panel]  ");
    krnP4PutStr(what);
    krnP4PutStr(": config ");
    krnP4PutHex32(st->config);
    krnP4PutStr(", output ");
    krnP4PutHex32(st->output);
    krnP4PutStr(st->claimed ? ", claimed" : ", NOT claimed");
    krnP4PutStr(st->powered ? ", powered" : ", supply off");
    krnP4PutStr(st->reset_released ? ", reset released\n" : ", in reset\n");
}

static int krnP4PanelPass(int pass)
{
    struct P4PanelState st;
    int r;

    krnP4PutStr("[panel]  pass ");
    krnP4PutDec((uint32_t)pass);
    krnP4PutStr("\n");

    r = krnP4PanelClaim(&st);
    if (r != P4_I2C_OK)
    {
        krnP4PutStr("[panel]  claim failed: ");
        krnP4PutStr(krnP4I2CName(r));
        krnP4PutStr("\n");
        (void)krnP4PanelSafe();
        return 0;
    }
    krnP4PutStr("[panel]  as found: pins ");
    krnP4PutHex32(st.input);
    krnP4PutStr("\n");
    krnP4ReportPanel("claimed", &st);

    /*
     * The check that "preserve unrelated bits" was obeyed rather than merely
     * intended.  Not "every other bit is an input": this board's expander
     * keeps its registers across a CPU reset and is found with all sixteen
     * pins already outputs, so the only meaningful statement is that our four
     * are outputs and that nothing outside them moved.
     */
    {
        UWORD ours = (UWORD)(P4_EXP_LCD_PWR_EN | P4_EXP_LCD_RST
                             | P4_EXP_LCD_BL_EN | P4_EXP_PWR_HOLD);
        UWORD stray;

        krnP4PutStr("[panel]  as found: config ");
        krnP4PutHex32(st.found_config);
        krnP4PutStr(", output ");
        krnP4PutHex32(st.found_output);
        krnP4PutStr("\n");

        if ((st.config & ours) != 0)
            krnP4PutStr("[panel]  OUR PINS ARE NOT OUTPUTS\n");
        else if ((UWORD)(st.config & ~ours) != (UWORD)(st.found_config & ~ours))
            krnP4PutStr("[panel]  A DIRECTION OUTSIDE OUR FOUR CHANGED\n");
        else
            krnP4PutStr("[panel]  our four are outputs, every other"
                        " direction unchanged\n");

        stray = krnP4PanelStrayBits();
        if (stray)
        {
            krnP4PutStr("[panel]  LEVELS CHANGED OUTSIDE OUR FOUR: ");
            krnP4PutHex32(stray);
            krnP4PutStr("\n");
            (void)krnP4PanelSafe();
            return 0;
        }
        krnP4PutStr("[panel]  no level outside our four moved\n");

        if (st.output & P4_EXP_LCD_BL_EN)
        {
            krnP4PutStr("[panel]  THE BACKLIGHT ENABLE IS SET, aborting\n");
            (void)krnP4PanelSafe();
            return 0;
        }
        if (st.output & P4_EXP_LCD_RST)
        {
            krnP4PutStr("[panel]  RESET IS NOT ASSERTED, aborting\n");
            (void)krnP4PanelSafe();
            return 0;
        }
    }

    r = krnP4PanelPowerUp(&st);
    if (r != P4_I2C_OK)
    {
        krnP4PutStr("[panel]  power-up failed: ");
        krnP4PutStr(krnP4I2CName(r));
        krnP4PutStr("\n");
        (void)krnP4PanelSafe();
        return 0;
    }
    krnP4ReportPanel("after the reset pulse", &st);

    if (st.output & P4_EXP_LCD_BL_EN)
    {
        krnP4PutStr("[panel]  THE BACKLIGHT CAME ON, aborting\n");
        (void)krnP4PanelSafe();
        return 0;
    }
    if (!(st.output & P4_EXP_PWR_HOLD))
    {
        krnP4PutStr("[panel]  PWR_HOLD WAS LOST, aborting\n");
        (void)krnP4PanelSafe();
        return 0;
    }

    /* Back to safe, so the next pass starts where this one did. */
    r = krnP4PanelSafe();
    if (r != P4_I2C_OK)
    {
        krnP4PutStr("[panel]  could not return to safe: ");
        krnP4PutStr(krnP4I2CName(r));
        krnP4PutStr("\n");
        return 0;
    }
    krnP4PutStr("[panel]  returned to safe, reset asserted and dark\n");
    return 1;
}

#ifdef P4_BACKLIGHT_ONLY
/*
 * The backlight and nothing else.
 *
 * B4's pattern ran with no host error and the panel stayed unlit, and every
 * register in the backlight path read back asserted: the expander's enable bit
 * confirmed at the pins, GPIO14 driven high and reading high.  So either the
 * backlight does not come on for a reason outside those registers, or something
 * in the DSI sequence undoes it.
 *
 * This separates the two.  It powers the panel, releases reset, turns the
 * backlight on and stops - no PHY, no commands, no pattern.  If the panel
 * lights, the backlight path is sound and the DSI sequence is doing something
 * to it.  If it stays dark, the backlight path is wrong on its own and that is
 * a far smaller thing to chase than a display pipeline.
 */
static void krnP4BacklightOnly(void)
{
    struct P4PanelState st;
    struct P4BacklightState bl;

    krnP4PutStr("[blonly] the backlight alone, no dsi at all\n");

    if (!krnP4I2CInit(1, P4_D1001_I2C1_SDA_GPIO, P4_D1001_I2C1_SCL_GPIO,
                      100000UL))
    {
        krnP4PutStr("[blonly] I2C1 did not configure\n");
        return;
    }
    if (krnP4PanelClaim(&st) != P4_I2C_OK)
    {
        krnP4PutStr("[blonly] the expander would not be claimed\n");
        return;
    }
    if (krnP4PanelPowerUp(&st) != P4_I2C_OK)
    {
        krnP4PutStr("[blonly] the panel would not power up\n");
        (void)krnP4PanelSafe();
        return;
    }
    if (krnP4PanelBacklightOn() != P4_I2C_OK)
    {
        krnP4PutStr("[blonly] the backlight would not switch on\n");
        (void)krnP4PanelSafe();
        return;
    }

    krnP4PanelBacklightState(&bl);
    krnP4PutStr("[blonly] latch ");
    krnP4PutHex32(bl.latch);
    krnP4PutStr(", pins ");
    krnP4PutHex32(bl.expander_pins);
    krnP4PutStr(", gpio14 driven ");
    krnP4PutDec((uint32_t)bl.out_level);
    krnP4PutStr(" reads ");
    krnP4PutDec((uint32_t)bl.pin_level);
    krnP4PutStr(", of ");
    krnP4PutDec((uint32_t)bl.samples);
    krnP4PutStr(" samples ");
    krnP4PutDec((uint32_t)bl.samples_high);
    krnP4PutStr(" high, ");
    krnP4PutDec((uint32_t)(bl.samples_high * 100 / bl.samples));
    krnP4PutStr(" percent\n[blonly] left on.  Is the panel lit, even"
                " uniformly grey or white?\n");
}
#endif

#ifdef P4_POWER_OFF
/*
 * Power the board off, so the next boot is genuinely cold.
 *
 * There is no other way on this board.  The port expander and the PSRAM chip
 * both keep their configuration across a CPU reset, and the battery means
 * removing USB is not a power cycle.  Releasing PWR_HOLD is the board's own
 * shutdown, and it only takes effect once VBUS is gone - so this announces
 * itself, waits long enough to unplug, and then releases the pin.
 */
static void krnP4PowerOff(void)
{
    struct P4PanelState st;
    unsigned int left;

    if (!krnP4I2CInit(1, P4_D1001_I2C1_SDA_GPIO, P4_D1001_I2C1_SCL_GPIO,
                      100000UL)
        || krnP4PanelClaim(&st) != P4_I2C_OK)
    {
        krnP4PutStr("[off]    the expander would not be claimed;"
                    " nothing done\n");
        return;
    }

    /*
     * Sixty seconds, and counted down out loud.
     *
     * The first attempt used twenty and lost the race: esptool resets the board
     * itself after writing, so the countdown had already run to the end before
     * anyone could unplug, and releasing PWR_HOLD with VBUS present does
     * nothing.  The expander's direction register still read all-outputs
     * afterwards, which is how that was established rather than assumed - a
     * cold one reads all-inputs.
     */
    krnP4PutStr("[off]    UNPLUG USB NOW.  PWR_HOLD is released in 60"
                " seconds, which powers the board off once VBUS is gone.\n"
                "[off]    Plug USB back in afterwards and the next boot is"
                " cold.\n");

    for (left = 60; left > 0; --left)
    {
        if (left % 5 == 0 || left <= 5)
        {
            krnP4PutStr("[off]    ");
            krnP4PutDec(left);
            krnP4PutStr("\n");
        }
        krnTimerWait(P4_TICK_HZ);
    }

    krnP4PutStr("[off]    releasing PWR_HOLD\n");
    (void)krnP4PanelPowerOff();
    krnP4PutStr("[off]    released.  If this line is still followed by a"
                " heartbeat, USB was still attached.\n");
}
#endif

static void krnP4PanelProbe(void)
{
    int r, passes = 0;

#ifdef P4_POWER_OFF
    krnP4PowerOff();
    return;
#endif

#ifdef P4_BACKLIGHT_ONLY
    krnP4BacklightOnly();
    return;
#endif

    krnP4PutStr("[panel]  B2: expander, panel supply and reset."
                " No data path.\n");

    if (!krnP4I2CInit(1, P4_D1001_I2C1_SDA_GPIO, P4_D1001_I2C1_SCL_GPIO,
                      100000UL))
    {
        krnP4PutStr("[panel]  I2C1 did not configure\n");
        return;
    }

    /*
     * Who is on which bus.
     *
     * The first run scanned only I2C1 at 100 kHz and found exactly one device
     * where four are documented, so the question became whether the expander
     * is on the bus this port thinks it is.  Both controllers are scanned at
     * two speeds; 400 kHz was tried and dropped, because at that rate nearly
     * every odd address answered, which is a timing fault in this driver and
     * not fifty devices.
     */
    {
        static const struct { unsigned int port, sda, scl; const char *name; }
        buses[2] =
        {
            { 0, P4_D1001_I2C0_SDA_GPIO, P4_D1001_I2C0_SCL_GPIO, "i2c0" },
            { 1, P4_D1001_I2C1_SDA_GPIO, P4_D1001_I2C1_SCL_GPIO, "i2c1" },
        };
        static const unsigned long speeds[2] = { 10000UL, 100000UL };
        unsigned int b, i, a, found;

        for (b = 0; b < 2; ++b)
            for (i = 0; i < 2; ++i)
            {
                if (!krnP4I2CInit(buses[b].port, buses[b].sda, buses[b].scl,
                                  speeds[i]))
                {
                    krnP4PutStr("[panel]  could not configure ");
                    krnP4PutStr(buses[b].name);
                    krnP4PutStr("\n");
                    continue;
                }

                found = 0;
                krnP4PutStr("[panel]  ");
                krnP4PutStr(buses[b].name);
                krnP4PutStr(" sda ");
                krnP4PutDec(buses[b].sda);
                krnP4PutStr(" scl ");
                krnP4PutDec(buses[b].scl);
                krnP4PutStr(" at ");
                krnP4PutDec((uint32_t)(speeds[i] / 1000));
                krnP4PutStr(" kHz:");
                for (a = 0x08; a < 0x78; ++a)
                    if (krnP4I2CProbe(a) == P4_I2C_OK)
                    {
                        krnP4PutStr(" ");
                        krnP4PutHex32(a);
                        ++found;
                    }
                if (!found)
                    krnP4PutStr(" nothing");
                krnP4PutStr("\n");
            }

        /* Back to the bus and rate the sequence runs at. */
        if (!krnP4I2CInit(1, P4_D1001_I2C1_SDA_GPIO, P4_D1001_I2C1_SCL_GPIO,
                          100000UL))
        {
            krnP4PutStr("[panel]  I2C1 did not reconfigure\n");
            return;
        }
    }

    /*
     * The expander, asked three ways.
     *
     * An address-only probe, a one-byte register write, and a real register
     * read.  If the device answers a read but not the probe, the probe is
     * wrong and the bus is fine; if it answers none of them while another
     * device on the same bus answers all of them, the device is not there.
     * The raw interrupt and status words separate a clean NACK from anything
     * else.
     */
    {
        static const unsigned char reg0 = P4_PCA9535_INPUT;
        unsigned char in[2] = { 0, 0 };
        unsigned long raw = 0, sr = 0;
        int rp, rw, rr;

        rp = krnP4I2CProbe(P4_PCA9535_ADDR);
        krnP4I2CLastStatus(&raw, &sr);
        krnP4PutStr("[panel]  0x20 address probe: ");
        krnP4PutStr(krnP4I2CName(rp));
        krnP4PutStr(", raw ");
        krnP4PutHex32((uint32_t)raw);
        krnP4PutStr(", sr ");
        krnP4PutHex32((uint32_t)sr);
        krnP4PutStr("\n");

        rw = krnP4I2CTransfer(P4_PCA9535_ADDR, &reg0, 1, NULL, 0);
        krnP4PutStr("[panel]  0x20 write one byte: ");
        krnP4PutStr(krnP4I2CName(rw));
        krnP4PutStr("\n");

        rr = krnP4I2CTransfer(P4_PCA9535_ADDR, &reg0, 1, in, 2);
        krnP4PutStr("[panel]  0x20 read input port: ");
        krnP4PutStr(krnP4I2CName(rr));
        if (rr == P4_I2C_OK)
        {
            krnP4PutStr(", pins ");
            krnP4PutHex32((uint32_t)(in[0] | ((uint32_t)in[1] << 8)));
        }
        krnP4PutStr("\n");

        /* The same three against the device that does answer, so the
           comparison is against a known-good target on the same bus. */
        rp = krnP4I2CProbe(0x6A);
        krnP4I2CLastStatus(&raw, &sr);
        krnP4PutStr("[panel]  0x6a address probe: ");
        krnP4PutStr(krnP4I2CName(rp));
        krnP4PutStr(", raw ");
        krnP4PutHex32((uint32_t)raw);
        krnP4PutStr(", sr ");
        krnP4PutHex32((uint32_t)sr);
        krnP4PutStr("\n");

        if (rr != P4_I2C_OK)
        {
            krnP4PutStr("[panel]  the expander is not answering; stopping"
                        " before anything is changed\n");
            return;
        }
    }

    /*
     * Seed an unrelated bit with a one, so preserving it means something.
     *
     * The expander survives a CPU reset, and on this board it cannot be
     * cold-started without powering the whole thing down: the battery keeps it
     * alive when USB is removed, and the only way to drop the rails is to
     * release PWR_HOLD, which is a deliberate power-off.  So the first runs
     * found every unrelated bit already at zero, and a read-modify-write that
     * preserves zero proves nothing about read-modify-write.
     *
     * BAT_READ_EN is chosen because the reference driver sets it high in normal
     * operation, so a one there is a value the board is meant to hold rather
     * than an experiment.  It is written directly rather than through the panel
     * driver on purpose: the driver refuses to touch bits outside its four,
     * which is the property being tested.
     */
    {
        static const unsigned char reg = P4_PCA9535_OUTPUT;
        unsigned char rd = P4_PCA9535_OUTPUT, wr[3];
        unsigned char in[2] = { 0, 0 };
        UWORD before, after;

        (void)reg;
        if (krnP4I2CTransfer(P4_PCA9535_ADDR, &rd, 1, in, 2) != P4_I2C_OK)
        {
            krnP4PutStr("[panel]  could not read the output register\n");
            return;
        }
        before = (UWORD)(in[0] | ((UWORD)in[1] << 8));
        before |= P4_EXP_BAT_READ_EN;

        wr[0] = P4_PCA9535_OUTPUT;
        wr[1] = (unsigned char)(before & 0xFF);
        wr[2] = (unsigned char)(before >> 8);
        if (krnP4I2CTransfer(P4_PCA9535_ADDR, wr, 3, NULL, 0) != P4_I2C_OK)
        {
            krnP4PutStr("[panel]  could not seed the test bit\n");
            return;
        }
        krnP4PutStr("[panel]  seeded bit 6 high, output now ");
        krnP4PutHex32(before);
        krnP4PutStr("\n");

        passes += krnP4PanelPass(1);
        passes += krnP4PanelPass(2);

        if (krnP4I2CTransfer(P4_PCA9535_ADDR, &rd, 1, in, 2) != P4_I2C_OK)
        {
            krnP4PutStr("[panel]  could not re-read the output register\n");
            return;
        }
        after = (UWORD)(in[0] | ((UWORD)in[1] << 8));
        krnP4PutStr("[panel]  after both passes, output ");
        krnP4PutHex32(after);
        if (after & P4_EXP_BAT_READ_EN)
            krnP4PutStr(", the seeded one survived\n");
        else
        {
            krnP4PutStr(", THE SEEDED ONE WAS LOST\n");
            passes = 0;
        }
    }

    krnP4PutStr("[panel]  B2 ");
    krnP4PutStr(passes == 2 ? "passed, twice\n" : "FAILED\n");

#ifdef P4_DSI_PROBE
    /*
     * B3 stage one, only if B2 got that far.
     *
     * The panel has to be powered and out of reset before the PHY is asked to
     * lock, and it has to stay that way afterwards, so this claims the pins
     * again rather than running after the safe return above.  The backlight
     * stays dark: this stage produces no image and lighting the panel would
     * show whatever the controller happens to hold.
     */
    if (passes == 2)
    {
        struct P4PanelState st;
        struct P4DsiState dsi;
        int r;

        krnP4PutStr("[dsi]    B3 stage one: supply, clocks and the phy pll\n");

        /*
         * Supply first, reset still asserted, and the reset pulse only after
         * the PHY is locked with its lanes in stop state.
         *
         * This order is the working reference's and the earlier one was not.
         * The D-PHY specification wants the lanes in LP-11 before a peripheral
         * leaves reset; releasing the panel against unpowered floating lanes is
         * what a dark panel and five silent reads look like.
         */
        if (krnP4PanelClaim(&st) != P4_I2C_OK
            || krnP4PanelSupplyOn(&st) != P4_I2C_OK)
        {
            krnP4PutStr("[dsi]    the panel supply would not come up\n");
            (void)krnP4PanelSafe();
            return;
        }

        r = krnP4DsiPhyUp(&dsi);

        krnP4PutStr("[dsi]    ldo ");
        krnP4PutHex32((uint32_t)dsi.ldo_reg);
        krnP4PutStr(" ana ");
        krnP4PutHex32((uint32_t)dsi.ldo_ana);
        krnP4PutStr(", dref ");
        krnP4PutDec((uint32_t)((dsi.ldo_ana & P4_LDO_DREF_MASK)
                               >> P4_LDO_DREF_SHIFT));
        krnP4PutStr(", mul ");
        krnP4PutDec((uint32_t)((dsi.ldo_ana & P4_LDO_MUL_MASK)
                               >> P4_LDO_MUL_SHIFT));
        krnP4PutStr((dsi.ldo_reg & P4_LDO_XPD) ? ", enabled\n"
                                               : ", NOT ENABLED\n");

        krnP4PutStr("[dsi]    pll n ");
        krnP4PutDec(dsi.pll_n);
        krnP4PutStr(" m ");
        krnP4PutDec(dsi.pll_m);
        krnP4PutStr(", range ");
        krnP4PutHex32(dsi.hs_freq_sel);
        krnP4PutStr(", ");
        krnP4PutDec((uint32_t)(40 * dsi.pll_m / dsi.pll_n));
        krnP4PutStr(" Mbit/s per lane from a 40 MHz reference\n");

        krnP4PutStr("[dsi]    phy status ");
        krnP4PutHex32((uint32_t)dsi.status);
        krnP4PutStr(dsi.locked ? "  locked" : "  NOT LOCKED");
        krnP4PutStr(dsi.lanes_stopped ? ", lanes in stop state\n"
                                      : ", lanes NOT in stop state\n");

        if (r == P4_DSI_OK)
        {
            unsigned char id[3] = { 0, 0, 0 };
            int id_result = 0, init;
#ifdef P4_SCANOUT_TEST
            struct P4DsiPattern pat;
#endif

            krnP4PutStr("[dsi]    B3 stage one passed\n");
#ifdef P4_B5_EXACT_PHY_CREATE
            krnP4PutStr("[b5]     exact v6.0 PHY creation, pre-bus phy_if ");
            krnP4PutHex32(p4_r32(P4_DSI_HOST_BASE + P4_DSI_PHY_IF_CFG));
            krnP4PutStr("\n");
#endif

            /* Stage two: command mode, then the panel out of reset, then
               its own sequence.  The reset comes here and not earlier. */
            (void)krnP4DsiCmdModeUp();
#ifdef P4_B5_EXACT_BUS_CREATE
            krnP4PutStr("[b5]     exact v6.0 bus create tmr/clk ");
            krnP4PutHex32(p4_r32(P4_DSI_HOST_BASE + P4_DSI_PHY_TMR_CFG));
            krnP4PutStr("/");
            krnP4PutHex32(p4_r32(P4_DSI_HOST_BASE + P4_DSI_PHY_TMR_LPCLK_CFG));
            krnP4PutStr("/");
            krnP4PutHex32(p4_r32(P4_DSI_HOST_BASE + P4_DSI_CLKMGR_CFG));
            krnP4PutStr("\n");
#endif
#ifdef P4_B5_EXACT_DBI_CREATE
            krnP4PutStr("[b5]     exact v6.0 DBI create pck/cmd ");
            krnP4PutHex32(p4_r32(P4_DSI_HOST_BASE + P4_DSI_PCKHDL_CFG));
            krnP4PutStr("/");
            krnP4PutHex32(p4_r32(P4_DSI_HOST_BASE + P4_DSI_CMD_MODE_CFG));
            krnP4PutStr("\n");
#endif
            krnP4PutStr("[dsi]    command mode, clock lane "
#ifdef P4_B5_IDF_AUTO_CMD_CLOCK
                        "auto (v6.0.1 comparison),"
#else
                        "low power,"
#endif
                        " commands in low power\n");

#ifdef P4_B5_EARLY_DPI_CREATE
            /*
             * Match esp_lcd_new_panel_dpi(), which the Waveshare wrapper
             * calls before the board pulses the JD9365 reset line.  Creation
             * enables the DPI pixel clock, stages host/bridge timings and
             * globally enables the bridge, but deliberately leaves DMA,
             * host video mode and DPI_EN off until the wrapper's underlying
             * DPI init runs after the complete vendor command table.
             *
             * The ordinary AROS path did all of this after that table.  Equal
             * final registers therefore did not prove equal bridge/host FSM
             * history.  Keep the early state live without rewriting it later
             * so this is a lifecycle discriminator rather than another start
             * delay.
             */
            (void)krnP4DsiPatternOn(&pat);
#ifdef P4_B5_EARLY_GDMA_CREATE
            krnP4ScanoutDmaCreate();
#endif
            krnP4ScanoutBridgeUp();
            krnP4PutStr("[b5]     DPI path created before panel reset"
#ifdef P4_B5_EARLY_GDMA_CREATE
                        ", including idle GDMA channel"
#endif
                        "\n");
#endif

            if (krnP4PanelResetPulse(&st) != P4_I2C_OK)
            {
                krnP4PutStr("[dsi]    the reset pulse failed\n");
                (void)krnP4PanelSafe();
                return;
            }
            krnP4PutStr("[dsi]    panel out of reset, with the lanes already"
                        " in stop state\n");

            init = krnP4DsiPanelInit(id, &id_result);

            krnP4PutStr("[dsi]    dcs 0x04 identity read: ");
            if (id_result > 0)
            {
                int k;

                krnP4PutDec((uint32_t)id_result);
                krnP4PutStr(" bytes,");
                for (k = 0; k < id_result && k < 3; ++k)
                {
                    krnP4PutStr(" ");
                    krnP4PutHex32(id[k]);
                }
                /* No expected value exists in any source, so this is recorded
                   as a board fact rather than checked. */
                krnP4PutStr("  recorded, not checked\n");
            }
            else
                krnP4PutStr("no reply, which no source says is wrong\n");

            /*
             * Four more reads, because the silence above answers less than it
             * seems to.
             *
             * A DSI write is not acknowledged: the host transmits and moves
             * on, so a completed command sequence says the host sent it and
             * nothing at all about whether the panel listened.  The identity
             * read was the one chance of panel-side evidence and it produced
             * none - but 0x04 is a vendor-defined read that this panel may
             * simply not implement, so its silence does not separate "the
             * panel is not answering" from "this driver's read path is
             * broken".
             *
             * These four are standard DCS reads that most controllers do
             * implement.  Any one of them answering settles both questions at
             * once; all four silent leaves the read path itself under
             * suspicion, and that is worth knowing before B4 depends on it.
             */
            /*
             * Not while the scanout runs.
             *
             * A read turns the link around so the panel may answer, and a read
             * that gets no answer leaves the host in receive - PHY_DIRECTION
             * set - where it will not transmit video.  So these probes were
             * stalling the very scanout they exist to observe.  The identity
             * read inside krnP4DsiPanelInit is unaffected: it answers, so it
             * completes and the link turns back.
             */
#if !defined(P4_SCANOUT_TEST) || defined(P4_DSI_PANEL_QUERY)
            {
                static const struct { unsigned char cmd; const char *what; }
                probes[4] =
                {
                    { 0x0A, "power mode" },
                    { 0x0B, "address mode" },
                    { 0x0C, "pixel format" },
                    { 0x45, "scanline" },
                };
                unsigned char v[2];
                unsigned int n;
                int any = 0;

                for (n = 0; n < 4; ++n)
                {
                    int got;

                    /* A read that produced nothing leaves the host waiting,
                       so every later one stacks on a busy controller and its
                       result means nothing.  Stop at the first silence. */
                    if (n && !any)
                    {
                        krnP4PutStr("[dsi]    stopping: the host is still"
                                    " waiting on the previous read\n");
                        break;
                    }

                    v[0] = v[1] = 0;
                    got = krnP4DsiDcsRead(probes[n].cmd, v, 1);
                    krnP4PutStr("[dsi]    dcs ");
                    krnP4PutHex32(probes[n].cmd);
                    krnP4PutStr(" ");
                    krnP4PutStr(probes[n].what);
                    krnP4PutStr(": ");
                    if (got > 0)
                    {
                        krnP4PutHex32(v[0]);
                        /*
                         * 0x0A is the one answer that says whether the panel
                         * is in a state to show anything at all: bit 4 is
                         * sleep-out, bit 2 display-on, bit 3 normal mode.  A
                         * panel reporting sleep or display-off is dark no
                         * matter what the host transmits, and no register on
                         * the transmitting side can distinguish that from a
                         * panel that is awake and being sent the wrong pixels.
                         */
                        if (probes[n].cmd == 0x0A)
                        {
                            krnP4PutStr((v[0] & (1U << 4)) ? "  awake"
                                                           : "  SLEEPING");
                            krnP4PutStr((v[0] & (1U << 2)) ? ", display on"
                                                           : ", DISPLAY OFF");
                            krnP4PutStr((v[0] & (1U << 3)) ? ", normal mode"
                                                           : ", partial/idle");
                            krnP4PutStr((v[0] & (1U << 7)) ? ", booster on"
                                                           : ", BOOSTER OFF");
                        }
                        krnP4PutStr("\n");
                        any = 1;
                    }
                    else
                    {
                        unsigned long pkt = 0, i0 = 0, i1 = 0;

                        krnP4DsiCmdStatus(&pkt, &i0, &i1);
                        krnP4PutStr("no reply, pkt ");
                        krnP4PutHex32((uint32_t)pkt);
                        krnP4PutStr(" int0 ");
                        krnP4PutHex32((uint32_t)i0);
                        krnP4PutStr(" int1 ");
                        krnP4PutHex32((uint32_t)i1);
                        krnP4PutStr("\n");
                    }
                }

                krnP4PutStr(any
                    ? "[dsi]    the panel answered, so it is listening and"
                      " the read path works\n"
                    : "[dsi]    nothing answered; the panel may be silent by"
                      " design or the read path may be wrong, and B4's"
                      " pattern is the first thing that can tell them apart\n");
            }
#endif

            if (init == P4_DSI_OK)
                krnP4PutStr("[dsi]    the jd9365 sequence completed,"
                            " display on, backlight still dark\n");
            else
            {
                krnP4PutStr(init == P4_DSI_CMD_BUSY
                            ? "[dsi]    B3 stage two FAILED: a command fifo"
                              " never drained\n"
                            : "[dsi]    B3 stage two FAILED: no reply where"
                              " one was needed\n");
                krnP4DsiPhyDown();
            }

            krnP4PutStr("[dsi]    B3 stage two ");
            krnP4PutStr(init == P4_DSI_OK ? "passed\n" : "FAILED\n");

#ifdef P4_SCANOUT_TEST
            /*
             * B5.  A frame from PSRAM, the bridge asking for it, the DMA
             * feeding it, and the backlight last.
             *
             * The order is B4's safety property kept: the link carries a
             * defined image before anything is lit, so a failure above leaves
             * a dark panel rather than a bright one showing whatever the
             * controller held.
             *
             * A single colour on purpose.  A wrong pixel format, a wrong
             * stride or a half-running DMA cannot produce a flat field of the
             * colour that was asked for, so what appears is evidence and not
             * just light.
             */
            if (init == P4_DSI_OK && __esp32p4_psram_size)
            {
                struct P4HostState hs;
                struct P4ScanoutState sc;
                unsigned long sar_dma, sar_video, sar_feed;
                unsigned long stage_pkt[5], stage_i0[5], stage_i1[5];
                unsigned long stage_phy[5];

                /* One channel at full, the other two at zero.  A byte swap
                   or a wrong channel order changes which colour appears and
                   is therefore visible rather than silent; a wrong stride or
                   pixel format cannot produce a flat field at all. */
#if defined(P4_C1_FRAMEBUFFER_HIDD)
                krnP4ScanoutC1Clear();
                krnP4PutStr("[c1]     two black B6 surfaces prepared;"
                            " fbgfx owns logical 1280x800 updates\n");
#elif defined(P4_B6_HANDOFF_GATE)
                krnP4ScanoutB6Frames();
#ifdef P4_B6_DIRTY_GATE
                krnP4PutStr("[b6dirty] two rotated landscape sources;"
                            " bounded inactive updates submit at frame-done\n");
#else
                krnP4PutStr("[b6]     two immutable rotated landscape frames;"
                            " GDMA swaps only at frame-done\n");
#endif
#elif defined(P4_B5_STATIC_PRELOAD)
                /* Write the complete asymmetric image before the pixel path
                   starts, then leave it unchanged for the whole run. */
#ifdef P4_B5_PHASE_CALIBRATION
                krnP4ScanoutPhaseCalibration();
#else
                krnP4ScanoutCoordinatePattern();
#endif
#elif defined(P4_B5_LIVE_UPDATE_GATE)
                /* Begin from the already observed static coordinate frame.
                   The scheduled live writes then distinguish a dirty-region
                   handoff from a full-frame bandwidth interruption. */
                krnP4ScanoutCoordinatePattern();
#elif defined(P4_SCANOUT_COHERENCY)
                krnP4ScanoutFill(0x000000);
#elif defined(P4_SCANOUT_BANDS)
                krnP4ScanoutBands();
#elif defined(P4_SCANOUT_HALVES)
                krnP4ScanoutHalves();
#elif defined(P4_SCANOUT_LINES)
                krnP4ScanoutThreeLines();
#elif defined(P4_SCANOUT_GRID)
                krnP4ScanoutGrid();
#elif defined(P4_SCANOUT_CROSS)
                krnP4ScanoutCross();
#elif defined(P4_SCANOUT_TESTCARD)
                krnP4ScanoutTestCard();
#else
                /*
                 * A flat field in one channel, which is the instrument for the
                 * byte offset and not just a first-light test.
                 *
                 * At three bytes per pixel a one-byte displacement rotates the
                 * channel assignment, so a single-channel field arrives striped
                 * when the offset walks from row to row: red, green, blue, red
                 * is one byte per row, and another period is another
                 * displacement.  A flat field is also the only pattern whose
                 * stripes cannot be mistaken for its own content, which is
                 * exactly what made the grid unreadable for four rounds.
                 */
                krnP4ScanoutFill(0x0000FF);
#endif
                krnP4CacheWriteback();

                /*
                 * The reference's order, which is not the obvious one: stage
                 * the host's video timing but stay in command mode, configure
                 * the bridge, arm the DMA, and only then hand the host over to
                 * video mode and let the bridge pull.  A host in video mode
                 * with nothing behind its DPI input latches a payload error it
                 * does not recover from.
                 */
#ifndef P4_B5_EARLY_DPI_CREATE
                (void)krnP4DsiPatternOn(&pat);
#endif
#ifdef P4_B5_FULL_ATOMIC_START
                /* Stronger than P4_B5_ATOMIC_START: from the end of the
                   vendor command table through this complete producer and
                   consumer start sequence, perform writes only.  Even the
                   otherwise harmless staged/bridge/DMA snapshots are delayed
                   until after feed commit, matching the reference call path
                   rather than observing each intermediate state. */
#ifndef P4_B5_EARLY_DPI_CREATE
                krnP4ScanoutBridgeUp();
#endif
                krnP4ScanoutDmaUp();
#ifdef P4_B5_CLOCK_GATED_START
                /* Freeze the bridge timing generator while both ends of the
                   DPI path enter their live state.  Releasing this one gate
                   below gives host and bridge the same first pixel edge. */
                p4_w32(P4_CLKRST_PERI_CLK_CTRL03,
                       p4_r32(P4_CLKRST_PERI_CLK_CTRL03)
                       & ~P4_DSI_DPICLK_EN);
#endif
#ifdef P4_B5_FEED_FIRST
#ifndef P4_DSI_VPG
                krnP4ScanoutFeedOn();
#endif
                krnP4DsiVideoOn();
#else
                krnP4DsiVideoOn();
#ifdef P4_B5_REFERENCE_TRANSITION_TRACE
                krnP4DsiReferenceTransitionTrace("after-video-auto");
#endif
#ifndef P4_DSI_VPG
                krnP4ScanoutFeedOn();
#endif
#ifdef P4_B5_REFERENCE_TRANSITION_TRACE
                krnP4DsiReferenceTransitionTrace("after-feed");
#endif
#endif
#ifdef P4_B5_CLOCK_GATED_START
                p4_w32(P4_CLKRST_PERI_CLK_CTRL03,
                       p4_r32(P4_CLKRST_PERI_CLK_CTRL03)
                       | P4_DSI_DPICLK_EN);
#endif
                krnP4ScanoutState(&sc);
                sar_dma = sar_video = sar_feed = sc.ch_sar;
                krnP4DsiCmdStatus(&stage_pkt[4], &stage_i0[4], &stage_i1[4]);
                krnP4HostState(&hs);
                stage_phy[4] = hs.phy_status;
                {
                    unsigned int n;
                    for (n = 0; n < 4; n++)
                    {
                        stage_pkt[n] = stage_pkt[4];
                        stage_i0[n] = stage_i0[4];
                        stage_i1[n] = stage_i1[4];
                        stage_phy[n] = stage_phy[4];
                    }
                }
#else
                krnP4DsiCmdStatus(&stage_pkt[0], &stage_i0[0], &stage_i1[0]);
                krnP4HostState(&hs);
                stage_phy[0] = hs.phy_status;

#ifndef P4_B5_EARLY_DPI_CREATE
                krnP4ScanoutBridgeUp();
#endif
                krnP4DsiCmdStatus(&stage_pkt[1], &stage_i0[1], &stage_i1[1]);
                krnP4HostState(&hs);
                stage_phy[1] = hs.phy_status;
                krnP4ScanoutDmaUp();
                krnP4ScanoutState(&sc);
                sar_dma = sc.ch_sar;
                krnP4DsiCmdStatus(&stage_pkt[2], &stage_i0[2], &stage_i1[2]);
                krnP4HostState(&hs);
                stage_phy[2] = hs.phy_status;
#ifdef P4_B5_ATOMIC_START
                /* These three writes are one start transaction in ESP-IDF:
                   video mode, automatic HS clock, bridge DPI output.  Reading
                   status between them lets a 40-MHz host run without input
                   timing and can itself create the payload overflow being
                   diagnosed.  Observe only after the feed commit. */
                krnP4DsiVideoOn();
#ifndef P4_DSI_VPG
                krnP4ScanoutFeedOn();
#endif
                krnP4ScanoutState(&sc);
                sar_video = sar_feed = sc.ch_sar;
                krnP4DsiCmdStatus(&stage_pkt[4], &stage_i0[4], &stage_i1[4]);
                krnP4HostState(&hs);
                stage_phy[4] = hs.phy_status;
                stage_pkt[3] = stage_pkt[4];
                stage_i0[3] = stage_i0[4];
                stage_i1[3] = stage_i1[4];
                stage_phy[3] = stage_phy[4];
#else
                krnP4DsiVideoOn();
                krnP4ScanoutState(&sc);
                sar_video = sc.ch_sar;
                krnP4DsiCmdStatus(&stage_pkt[3], &stage_i0[3], &stage_i1[3]);
                krnP4HostState(&hs);
                stage_phy[3] = hs.phy_status;
#ifndef P4_DSI_VPG
                krnP4ScanoutFeedOn();
#endif
                krnP4ScanoutState(&sc);
                sar_feed = sc.ch_sar;
                krnP4DsiCmdStatus(&stage_pkt[4], &stage_i0[4], &stage_i1[4]);
                krnP4HostState(&hs);
                stage_phy[4] = hs.phy_status;
#endif
#endif

                {
                    static const char *const names[5] = {
                        "staged", "bridge", "dma",
#if defined(P4_B5_ATOMIC_START) || defined(P4_B5_FULL_ATOMIC_START)
                        "started", "started"
#else
                        "video", "feed"
#endif
                    };
                    unsigned int n;

                    for (n = 0; n < 5; n++)
                    {
                        krnP4PutStr("[b5]     edge ");
                        krnP4PutStr(names[n]);
                        krnP4PutStr(" pkt/i0/i1/phy ");
                        krnP4PutHex32((uint32_t)stage_pkt[n]);
                        krnP4PutStr("/");
                        krnP4PutHex32((uint32_t)stage_i0[n]);
                        krnP4PutStr("/");
                        krnP4PutHex32((uint32_t)stage_i1[n]);
                        krnP4PutStr("/");
                        krnP4PutHex32((uint32_t)stage_phy[n]);
                        krnP4PutStr("\n");
                    }
                }

#ifdef P4_B5_SKIP_FINAL_PANEL_ON
                /* With the live 0x29 removed, catch the first ordinary video
                   transaction that raises the host payload-write condition.
                   DMA completion interrupts remain enabled while this tight
                   foreground poll runs, so dma_frames distinguishes the
                   first frame from a rearm boundary. */
                {
                    uint64_t first_start = krnTimerCount();
                    unsigned long first_pkt, first_i0, first_i1, first_vid;
                    unsigned long first_phy, phy_and = ~0UL, phy_or = 0;
                    unsigned long vid_and = ~0UL, vid_or = 0;
                    unsigned long phy_prev = ~0UL, phy_changes = 0;
                    unsigned long lane_active = 0, rx_direction = 0;

                    do
                    {
                        krnP4DsiCmdStatus(&first_pkt, &first_i0, &first_i1);
                        first_vid = krnP4DsiVideoStatus();
                        first_phy = p4_r32(P4_DSI_HOST_BASE
                                           + P4_DSI_PHY_STATUS);
                        phy_and &= first_phy;
                        phy_or |= first_phy;
                        vid_and &= first_vid;
                        vid_or |= first_vid;
                        if (first_phy != phy_prev)
                        {
                            phy_prev = first_phy;
                            phy_changes++;
                        }
                        if ((first_phy & (P4_DSI_STOPSTATE_L0
                                         | P4_DSI_STOPSTATE_L1))
                            != (P4_DSI_STOPSTATE_L0
                                | P4_DSI_STOPSTATE_L1))
                            lane_active++;
                        if (first_phy & P4_DSI_PHY_DIRECTION)
                            rx_direction++;
                        krnP4ScanoutState(&sc);
                    } while (!first_i1
                             && krnTimerCount() - first_start
                                < P4_SYSTIMER_HZ / 10);

                    krnP4PutStr("[b5]     first int1 after ticks/frames/sar/depth ");
                    krnP4PutHex32((uint32_t)(krnTimerCount() - first_start));
                    krnP4PutStr("/");
                    krnP4PutHex32((uint32_t)sc.dma_frames);
                    krnP4PutStr("/");
                    krnP4PutHex32((uint32_t)sc.ch_sar);
                    krnP4PutStr("/");
                    krnP4PutHex32((uint32_t)sc.brg_depth);
                    krnP4PutStr(" = ");
                    krnP4PutHex32((uint32_t)first_i1);
                    krnP4PutStr(" vid ");
                    krnP4PutHex32((uint32_t)first_vid);
                    krnP4PutStr("\n");
                    krnP4PutStr("[b5]     first phy and/or/changes/active/rx ");
                    krnP4PutHex32((uint32_t)phy_and);
                    krnP4PutStr("/");
                    krnP4PutHex32((uint32_t)phy_or);
                    krnP4PutStr("/");
                    krnP4PutHex32((uint32_t)phy_changes);
                    krnP4PutStr("/");
                    krnP4PutHex32((uint32_t)lane_active);
                    krnP4PutStr("/");
                    krnP4PutHex32((uint32_t)rx_direction);
                    krnP4PutStr(" vid and/or ");
                    krnP4PutHex32((uint32_t)vid_and);
                    krnP4PutStr("/");
                    krnP4PutHex32((uint32_t)vid_or);
                    krnP4PutStr("\n");
                }
#endif

                /* Match the working JD9365 wrapper: its DPI-panel init starts
                   DMA, host video and bridge output before disp_on_off sends
                   the valid parameterless DCS 0x29. */
#ifdef P4_B5_SKIP_FINAL_PANEL_ON
                krnP4PutStr("[dsi]    final display-on skipped for live-command isolation\n");
#else
                if (krnP4DsiPanelOn() != P4_DSI_OK)
                {
                    krnP4PutStr("[dsi]    display-on after video failed\n");
                    krnP4ScanoutQuiesce();
                    (void)krnP4PanelSafe();
                    return;
                }
                krnP4PutStr("[dsi]    display on after video start\n");
#endif

                krnTimerWait(10);               /* 100 ms of frames */

                /*
                 * What the panel says once it has been sent video.
                 *
                 * Every earlier reading of DCS 0x0A was taken before the
                 * handover, where a panel that has seen no pixels reports its
                 * output disabled for the obvious reason.  That made "display
                 * off" look like a finding through several rounds when it was
                 * an artefact of when it was asked.
                 *
                 * Asking afterwards costs the stream: the read needs command
                 * mode, so video stops, the answer is taken, and video is
                 * started again.  Bit 2 set means the panel has enabled its
                 * output, which is the difference between a panel refusing the
                 * stream and a panel showing something invisible.
                 */
#ifdef P4_DSI_POST_VIDEO_QUERY
                {
                    unsigned char pm = 0;
                    int got;

                    krnP4DsiPatternOff();
                    got = krnP4DsiDcsRead(0x0A, &pm, 1);

                    krnP4PutStr("[b5]     after video, power mode ");
                    if (got > 0)
                    {
                        krnP4PutHex32(pm);
                        krnP4PutStr((pm & (1U << 2)) ? "  DISPLAY ON"
                                                     : "  display off");
                        krnP4PutStr((pm & (1U << 4)) ? ", awake\n"
                                                     : ", sleeping\n");
                    }
                    else
                        krnP4PutStr("no reply\n");

                    krnP4DsiVideoOn();
                    krnTimerWait(10);
                }
#endif

                krnP4ScanoutState(&sc);
                krnP4PutStr("[b5]     frame at ");
                krnP4PutHex32((uint32_t)sc.fb_base);
                krnP4PutStr(", ");
                krnP4PutDec((uint32_t)sc.words64);
                krnP4PutStr(" x 64-bit, chen ");
                krnP4PutHex32((uint32_t)sc.chen);
                krnP4PutStr("\n");

                krnP4PutStr("[b5]     start sar dma/video/feed ");
                krnP4PutHex32((uint32_t)sar_dma);
                krnP4PutStr("/");
                krnP4PutHex32((uint32_t)sar_video);
                krnP4PutStr("/");
                krnP4PutHex32((uint32_t)sar_feed);
                krnP4PutStr("\n");

                krnP4PutStr("[b5]     brg  v ");
                krnP4PutHex32((uint32_t)sc.brg_v_cfg0);
                krnP4PutStr("/");
                krnP4PutHex32((uint32_t)sc.brg_v_cfg1);
                krnP4PutStr(" h ");
                krnP4PutHex32((uint32_t)sc.brg_h_cfg0);
                krnP4PutStr("/");
                krnP4PutHex32((uint32_t)sc.brg_h_cfg1);
                krnP4PutStr(" en ");
                krnP4PutHex32((uint32_t)sc.brg_en);
                krnP4PutStr(" pix ");
                krnP4PutHex32((uint32_t)sc.brg_pixel);
                krnP4PutStr("\n");

                krnP4PutStr("[b5]     brg  flow ");
                krnP4PutHex32((uint32_t)sc.brg_flow);
                krnP4PutStr(" rawnum ");
                krnP4PutHex32((uint32_t)sc.brg_raw_num);
                krnP4PutStr(" misc ");
                krnP4PutHex32((uint32_t)sc.brg_misc);
                krnP4PutStr(" int ");
                krnP4PutHex32((uint32_t)sc.brg_int);
                krnP4PutStr("\n");

                /*
                 * The host's error status, read three times.
                 *
                 * INT_ST0 and INT_ST1 are cleared by reading them, so a single
                 * reading says only that something happened since whatever read
                 * them last - and this probe reads them in B3 as well.  Read,
                 * read again, wait, read once more: the second says whether the
                 * first cleared it, and the third whether the fault is still
                 * being produced or was produced once during the handover.
                 */
                {
                    unsigned long pkt = 0, i0 = 0, i1 = 0;
                    unsigned long i1b = 0, i1c = 0, d = 0;

                    krnP4DsiCmdStatus(&pkt, &i0, &i1);
                    krnP4DsiCmdStatus(&d, &d, &i1b);
                    krnTimerWait(5);                    /* 50 ms of frames */
                    krnP4DsiCmdStatus(&d, &d, &i1c);

                    krnP4PutStr("[b5]     host pkt ");
                    krnP4PutHex32((uint32_t)pkt);
                    krnP4PutStr(" int0 ");
                    krnP4PutHex32((uint32_t)i0);
                    krnP4PutStr(" int1 ");
                    krnP4PutHex32((uint32_t)i1);
                    krnP4PutStr(" then ");
                    krnP4PutHex32((uint32_t)i1b);
                    krnP4PutStr(" then ");
                    krnP4PutHex32((uint32_t)i1c);
                    krnP4PutStr((i1c & (1UL << 7))
                                ? "  still failing\n"
                                : ((i1 & (1UL << 7)) ? "  failed once\n"
                                                     : "  no payload error\n"));
                }

                /*
                 * Why the channel stopped, in its own words.
                 *
                 * The source address says only that nothing moved.  The
                 * channel's interrupt status names the cause: a descriptor
                 * that would not read, one the engine rejected as invalid, a
                 * decode error on either side, or a channel that disabled or
                 * aborted itself.  This port had never read it.
                 */
                {
                    static const struct { unsigned long bit; const char *name; }
                    dma_faults[] = {
                        { P4_DMAC_IS_SRC_DEC_ERR,    " src_dec"    },
                        { P4_DMAC_IS_DST_DEC_ERR,    " dst_dec"    },
                        { P4_DMAC_IS_SRC_SLV_ERR,    " src_slv"    },
                        { P4_DMAC_IS_DST_SLV_ERR,    " dst_slv"    },
                        { P4_DMAC_IS_LLI_RD_DEC_ERR, " lli_rd_dec" },
                        { P4_DMAC_IS_LLI_WR_DEC_ERR, " lli_wr_dec" },
                        { P4_DMAC_IS_LLI_RD_SLV_ERR, " lli_rd_slv" },
                        { P4_DMAC_IS_LLI_WR_SLV_ERR, " lli_wr_slv" },
                        { P4_DMAC_IS_LLI_INVALID,    " lli_invalid"},
                        { P4_DMAC_IS_MULTIBLK_ERR,   " multiblk"   },
                        { P4_DMAC_IS_SLVIF_DEC_ERR,  " slvif_dec"  },
                        { P4_DMAC_IS_WRONCHEN_ERR,   " wr_on_chen" },
                        { P4_DMAC_IS_SUSPENDED,      " suspended"  },
                        { P4_DMAC_IS_DISABLED,       " disabled"   },
                        { P4_DMAC_IS_ABORTED,        " aborted"    },
                    };
                    unsigned int k;
                    int named = 0;

                    krnP4ScanoutState(&sc);
                    krnP4PutStr("[b5]     dma  int0 ");
                    krnP4PutHex32((uint32_t)sc.ch_int0);
                    krnP4PutStr(" int1 ");
                    krnP4PutHex32((uint32_t)sc.ch_int1);
                    krnP4PutStr(" frames ");
                    krnP4PutDec((uint32_t)sc.dma_frames);
                    krnP4PutStr(" faults ");
                    krnP4PutHex32((uint32_t)sc.dma_faults);
                    krnP4PutStr("  ");
                    if (sc.ch_int0 & P4_DMAC_IS_BLOCK_DONE)
                    {
                        krnP4PutStr("block_done");
                        named = 1;
                    }
                    if (sc.ch_int0 & P4_DMAC_IS_DMA_DONE)
                    {
                        krnP4PutStr(named ? " dma_done" : "dma_done");
                        named = 1;
                    }
                    for (k = 0; k < sizeof(dma_faults) / sizeof(dma_faults[0]);
                         ++k)
                        if (sc.ch_int0 & dma_faults[k].bit)
                        {
                            krnP4PutStr(named ? dma_faults[k].name
                                              : dma_faults[k].name + 1);
                            named = 1;
                        }
                    krnP4PutStr(named ? "\n" : "nothing reported\n");
                }

                /*
                 * The bridge sampled rather than read once.
                 *
                 * A fifo that fills and drains is a bridge doing its job; one
                 * pinned at a value is a bridge that stopped; one at zero with
                 * no underrun raised is a bridge that never started a frame.
                 * The last of those explains a DMA that never moves and a host
                 * that receives pixels anyway, because on underflow the bridge
                 * substitutes RSV_DPI_DATA rather than stopping.
                 */
                {
                    unsigned long d[6], ir[6];
                    unsigned int k;

                    for (k = 0; k < 6; ++k)
                    {
                        krnP4ScanoutSample(&d[k], &ir[k]);
                        if (k < 5)
                            krnTimerWait(1);        /* 10 ms apart */
                    }

                    krnP4PutStr("[b5]     brg  depth");
                    for (k = 0; k < 6; ++k)
                    {
                        krnP4PutStr(" ");
                        krnP4PutDec((uint32_t)d[k]);
                    }
                    krnP4PutStr("\n[b5]     brg  raw  ");
                    for (k = 0; k < 6; ++k)
                    {
                        krnP4PutStr(" ");
                        krnP4PutHex32((uint32_t)ir[k]);
                    }
                    krnP4PutStr("\n");
                }

                /* The bridge registers this port never writes.  Their stable
                   values are already recorded; the ceiling-bound start-trace
                   build uses this report's space for the three edge SARs. */
#ifndef P4_B5_START_TRACE
                {
                    struct P4BridgeRest br;

                    krnP4ScanoutBridgeRest(&br);
                    krnP4PutStr("[b5]     brg  credit ");
                    krnP4PutHex32((uint32_t)br.credit_ctl);
                    krnP4PutStr(" blkint ");
                    krnP4PutHex32((uint32_t)br.block_intvl);
                    krnP4PutStr(" reqint ");
                    krnP4PutHex32((uint32_t)br.req_intvl);
                    krnP4PutStr(" lcdctl ");
                    krnP4PutHex32((uint32_t)br.lcd_ctl);
                    krnP4PutStr("\n");

                    krnP4PutStr("[b5]     brg  rsvdata ");
                    krnP4PutHex32((uint32_t)br.rsv_dpi_data);
                    krnP4PutStr(" intena ");
                    krnP4PutHex32((uint32_t)br.int_ena);
                    krnP4PutStr(" blkraw ");
                    krnP4PutHex32((uint32_t)br.blk_raw_num);
                    krnP4PutStr(" hostctl ");
                    krnP4PutHex32((uint32_t)br.host_ctrl);
                    krnP4PutStr(" memclk ");
                    krnP4PutHex32((uint32_t)br.mem_clk_ctrl);
                    krnP4PutStr(" dmareq ");
                    krnP4PutHex32((uint32_t)br.dma_req_cfg);
                    krnP4PutStr("\n");
                }
#endif

                /*
                 * How many frames per second the DMA is actually delivering.
                 *
                 * The panel shows the picture twice down its height, which is
                 * a factor of exactly two, and the cleanest thing a factor of
                 * two can be is a rate.  The bridge's timing gives one frame
                 * every htotal * vtotal pixel clocks; if the DMA walks the
                 * framebuffer twice in that time, the panel receives each
                 * frame twice and the arithmetic says so directly.
                 *
                 * Measured over a short window so the source address wraps at
                 * most once, and the wrap is handled by taking the difference
                 * modulo the frame size.
                 */
                {
                    unsigned long a, b, delta;
                    uint64_t t0, t1, ticks;

                    krnP4ScanoutState(&sc);
                    a = sc.ch_sar;
                    t0 = krnTimerCount();
                    /*
                     * Long enough to average over several frames.
                     *
                     * This was 5 ms, which is shorter than a frame: such a
                     * window falls almost entirely inside the active lines and
                     * therefore measures the active data rate, not the mean
                     * over the frame.  It read 218 MB/s where the active rate
                     * is 218.2 and the mean with vertical blanking is 210.6,
                     * and that was taken as evidence the bridge does not pause
                     * for blanking.  It is not evidence of anything except the
                     * window being too short.  100 ms covers about seven
                     * frames at this timing.
                     */
                    while (krnTimerCount() - t0 < P4_SYSTIMER_HZ / 10)
                        ;                       /* 100 ms, spun not slept */
                    krnP4ScanoutState(&sc);
                    b = sc.ch_sar;
                    t1 = krnTimerCount();

                    /*
                     * Over 100 ms the source address wraps several times, so
                     * the difference modulo the frame size is not the distance
                     * travelled.  Count the wraps from the elapsed time and
                     * the expected rate instead: the residue pins down the
                     * fractional part and the whole part comes from the frame
                     * count, which is what makes a long window usable at all.
                     */
                    {
                        uint64_t expect = (uint64_t)P4_PANEL_DPI_MHZ * 1000000UL
                                        * P4_PANEL_H_RES * P4_PANEL_V_RES
                                        * P4_FB_BYTES_PER_PIXEL
                                        / ((unsigned long)(P4_PANEL_H_RES
                                             + P4_PANEL_HSYNC + P4_PANEL_HBP
                                             + P4_PANEL_HFP)
                                           * (P4_PANEL_V_RES + P4_PANEL_VSYNC
                                              + P4_PANEL_VBP + P4_PANEL_VFP));
                        uint64_t rough = expect * (t1 - t0) / P4_SYSTIMER_HZ;
                        unsigned long resid = (b - a) % P4_FB_BYTES;
                        uint64_t wraps = (rough > resid)
                                       ? (rough - resid + P4_FB_BYTES / 2)
                                         / P4_FB_BYTES
                                       : 0;

                        delta = 0;
                        krnP4PutStr("[b5]     expect ");
                        krnP4PutDec((uint32_t)(expect / 1000000UL));
                        krnP4PutStr(" MB/s, residue ");
                        krnP4PutDec((uint32_t)resid);
                        krnP4PutStr(", wraps ");
                        krnP4PutDec((uint32_t)wraps);
                        krnP4PutStr("\n");
                        /* bytes actually moved = wraps * frame + residue */
                        {
                            uint64_t moved = wraps * (uint64_t)P4_FB_BYTES
                                           + resid;
                            uint64_t bps = moved * P4_SYSTIMER_HZ / (t1 - t0);

                            krnP4PutStr("[b5]     measured ");
                            krnP4PutDec((uint32_t)(bps / 1000000UL));
                            krnP4PutStr(" MB/s over ");
                            krnP4PutDec((uint32_t)((t1 - t0) * 1000
                                                   / P4_SYSTIMER_HZ));
                            krnP4PutStr(" ms\n");
                        }
                    }
                    ticks = t1 - t0;

                    krnP4PutStr("[b5]     rate ");
                    krnP4PutDec((uint32_t)delta);
                    krnP4PutStr(" bytes in ");
                    krnP4PutDec((uint32_t)ticks);
                    krnP4PutStr(" ticks = ");
                    /* bytes/s = delta * SYSTIMER_HZ / ticks; report frames per
                       second times ten so a fraction is visible */
                    if (ticks)
                    {
                        uint64_t bps = (uint64_t)delta * P4_SYSTIMER_HZ / ticks;
                        uint32_t fps10 = (uint32_t)(bps * 10 / P4_FB_BYTES);

                        krnP4PutDec(fps10 / 10);
                        krnP4PutStr(".");
                        krnP4PutDec(fps10 % 10);
                        krnP4PutStr(" frames/s from memory, against ");
                        krnP4PutDec((uint32_t)((unsigned long)P4_PANEL_DPI_MHZ
                                    * 1000000UL
                                    / ((P4_PANEL_H_RES + P4_PANEL_HSYNC
                                        + P4_PANEL_HBP + P4_PANEL_HFP)
                                       * (P4_PANEL_V_RES + P4_PANEL_VSYNC
                                          + P4_PANEL_VBP + P4_PANEL_VFP))));
                        krnP4PutStr(" the timing asks for\n");
                    }
                    else
                        krnP4PutStr("no elapsed time\n");
                }

                /*
                 * The source address is the measurement that says the DMA is
                 * actually moving: it walks the frame while a transfer runs.
                 * Read twice, because one reading proves nothing.
                 */
                {
                    unsigned long a, b;

                    krnP4ScanoutState(&sc);
                    a = sc.ch_sar;
                    krnTimerWait(2);
                    krnP4ScanoutState(&sc);
                    b = sc.ch_sar;

                    krnP4PutStr("[b5]     sar ");
                    krnP4PutHex32((uint32_t)a);
                    krnP4PutStr(" then ");
                    krnP4PutHex32((uint32_t)b);
                    krnP4PutStr(a != b ? "  moving\n" : "  stalled\n");
                }

                /*
                 * What the host holds, before the backlight is touched.  The
                 * one register that decides whether any of the rest can work
                 * is MODE_CFG: bit 0 set means still command mode, and a host
                 * in command mode has no video path for the bridge to feed.
                 */
                {
                    struct P4HostState h;

                    krnP4HostState(&h);
                    krnP4PutStr("[b5]     host mode ");
                    krnP4PutHex32((uint32_t)h.mode_cfg);
                    krnP4PutStr((h.mode_cfg & P4_DSI_CMD_VIDEO_MODE)
                                ? "  COMMAND MODE" : "  video mode");
                    krnP4PutStr(", pwr_up ");
                    krnP4PutHex32((uint32_t)h.pwr_up);
                    krnP4PutStr(", lpclk ");
                    krnP4PutHex32((uint32_t)h.lpclk);
                    krnP4PutStr((h.lpclk & P4_DSI_TXREQUESTCLKHS)
                                ? " hs\n" : " NOT hs\n");

                    krnP4PutStr("[b5]     host vid_mode ");
                    krnP4PutHex32((uint32_t)h.vid_mode);
                    krnP4PutStr(" phy ");
                    krnP4PutHex32((uint32_t)h.phy_status);
                    krnP4PutStr(" colour ");
                    krnP4PutHex32((uint32_t)h.colour);
                    krnP4PutStr(" dpiclk ");
                    krnP4PutHex32((uint32_t)h.dpi_clk);
                    krnP4PutStr("\n");

                    krnP4PutStr("[b5]     host pkt ");
                    krnP4PutDec((uint32_t)h.pkt_size);
                    krnP4PutStr(" hsa ");
                    krnP4PutDec((uint32_t)h.hsa);
                    krnP4PutStr(" hbp ");
                    krnP4PutDec((uint32_t)h.hbp);
                    krnP4PutStr(" hline ");
                    krnP4PutDec((uint32_t)h.hline);
                    krnP4PutStr(" vact ");
                    krnP4PutDec((uint32_t)h.vactive);
                    krnP4PutStr("\n");
                }

                /*
                 * Where the link turned around.
                 *
                 * phy_direction reads set at the end of this phase, which
                 * stops the host transmitting and overflows the payload fifo.
                 * These five readings say which step did it: command mode with
                 * nothing sent, the identity read, the four framing commands,
                 * the jd9365 sequence, the handover to video.  Bit 1 is the
                 * direction, bit 2 the clock lane's stop state, bits 4 and 7
                 * the two data lanes'.
                 */
                {
                    unsigned int t;

                    krnP4PutStr("[b5]     phy trace");
                    for (t = 0; t < krnP4DsiPhyTraceCount; ++t)
                    {
                        krnP4PutStr(" ");
                        krnP4PutHex32((uint32_t)krnP4DsiPhyTrace[t]);
                    }
                    krnP4PutStr("\n");

                    krnP4PutStr("[b5]     turned around after step ");
                    for (t = 0; t < krnP4DsiPhyTraceCount; ++t)
                        if (krnP4DsiPhyTrace[t] & 2UL)
                            break;
                    if (t < krnP4DsiPhyTraceCount)
                        krnP4PutDec((uint32_t)t);
                    else
                        krnP4PutStr("none, it is not turned around");
                    krnP4PutStr("\n");
                }

                krnP4PanelBacklightOn();

                /*
                 * The backlight state, read back rather than assumed, because
                 * the enable is a latch in an I2C expander and a write to it
                 * can fail silently.
                 */
                {
                    struct P4BacklightState bl;

                    krnP4PanelBacklightState(&bl);
                    krnP4PutStr("[b5]     backlight latch ");
                    krnP4PutHex32((uint32_t)bl.latch);
                    krnP4PutStr(" pin ");
                    krnP4PutDec((uint32_t)bl.pin_level);
                    krnP4PutStr(" duty ");
                    krnP4PutDec((uint32_t)bl.samples_high);
                    krnP4PutStr("/");
                    krnP4PutDec((uint32_t)bl.samples);
                    krnP4PutStr("\n");
                }

                /*
                 * Left running deliberately, and the return is what makes that
                 * true.  Without it this falls through to krnP4PanelSafe()
                 * below, which darkens the backlight again within a few
                 * hundred microseconds of switching it on: the panel lit for
                 * well under a second and went out, and the same run then
                 * reported "backlight never on".  B4 carries the same return
                 * for the same reason.
                 */
                /*
                 * Left running, but not forever.
                 *
                 * A scanout that outlives the boot that started it is what
                 * made this board unbootable twice: the GDMA survives a CPU
                 * reset and keeps reading PSRAM over AXI while the next boot's
                 * bring-up reconfigures the controller, and recovering from
                 * that took flashing the vendor firmware - a power cycle was
                 * not enough, plausibly because the LP domain holding the PMU
                 * registers is battery-backed here.
                 *
                 * So it runs long enough to look at and photograph, then stops
                 * itself.  P4_SCANOUT_SECS=0 restores the old behaviour for a
                 * session where that is wanted, with the consequence stated.
                 */
#if defined(P4_B5_CONCURRENT_STRESS) && \
    !defined(P4_B5_STRESS_SMOKE) && P4_SCANOUT_SECS < 1800
#error P4_B5_CONCURRENT_STRESS requires P4_SCANOUT_SECS of at least 1800
#endif
#if P4_SCANOUT_SECS == 0
#ifdef P4_C1_FRAMEBUFFER_HIDD
                krnP4PutStr("[c1]     managed scanout retained for the"
                            " graphics HIDD; early reset quiesces GDMA\n");
#else
                krnP4PutStr("[b5]     left running indefinitely;"
                            " a reset from here needs the vendor firmware"
                            " to recover\n");
#endif
#else
                krnP4PutStr("[b5]     running for ");
                krnP4PutDec(P4_SCANOUT_SECS);
                krnP4PutStr(" s, then stopping so a reset is safe\n");

                /*
                 * Sampled while it runs, because the panel goes dark on its
                 * own after about ten seconds and nothing in the log said
                 * anything about it.  The wait used to be silent, so the one
                 * event worth observing happened inside it.
                 *
                 * One line per second: the DMA's source address, the bridge's
                 * fifo depth and raw interrupt, and the host's error status and
                 * PHY state.  Whatever changes at the moment the picture goes
                 * is the thing to chase; if nothing changes, the panel is
                 * deciding on its own and the transmit side is innocent.
                 */
                {
                    unsigned long secs;
#ifdef P4_B6_DIRTY_GATE
                    unsigned long dirty_status_failures = 0;
#endif
#ifdef P4_B5_CONCURRENT_STRESS
                    unsigned long stress_completed = 0;
                    int stress_ready = krnP4B5StressBegin();
#endif

                    for (secs = 0; secs < (unsigned long)P4_SCANOUT_SECS;
                         secs++)
                    {
                        unsigned long depth = 0, raw = 0;
                        unsigned long pkt = 0, i0 = 0, i1 = 0;
                        struct P4HostState h;

#ifdef P4_SCANOUT_COHERENCY
                        {
                            unsigned long phase =
                                krnP4ScanoutCoherencyStep(secs);

                            if (phase)
                            {
                                krnP4PutStr("[b5]     coherency phase ");
                                krnP4PutDec((uint32_t)phase);
                                krnP4PutStr(" written back\n");
                            }
                        }
#endif

#ifdef P4_B5_CONCURRENT_STRESS
                        if (!stress_ready || !krnP4B5StressStep(secs))
                        {
                            stress_ready = 0;
                            break;
                        }
                        stress_completed = secs + 1;
#endif

#ifdef P4_B6_DIRTY_GATE
                        {
                            unsigned long target =
                                krnP4ScanoutB6DirtyStep(secs);

                            krnP4PutStr("[b6dirty] t");
                            krnP4PutDec((uint32_t)secs);
                            if (target)
                            {
                                krnP4PutStr(" queued ");
                                krnP4PutHex32((uint32_t)target);
                            }
                            else
                                krnP4PutStr(" REJECTED");
                            krnP4PutStr("\n");
                        }
#endif

                        krnP4ScanoutState(&sc);
                        krnP4ScanoutSample(&depth, &raw);
                        krnP4DsiCmdStatus(&pkt, &i0, &i1);
                        krnP4HostState(&h);

#ifdef P4_B6_HANDOFF_GATE
                        krnP4PutStr("[b6]     t");
#else
                        krnP4PutStr("[b5]     t");
#endif
                        krnP4PutDec((uint32_t)secs);
                        krnP4PutStr(" sar ");
                        krnP4PutHex32((uint32_t)sc.ch_sar);
                        krnP4PutStr(" depth ");
                        krnP4PutDec((uint32_t)depth);
                        krnP4PutStr(" braw ");
                        krnP4PutHex32((uint32_t)raw);
                        krnP4PutStr(" int1 ");
                        krnP4PutHex32((uint32_t)i1);
                        krnP4PutStr(" phy ");
                        krnP4PutHex32((uint32_t)h.phy_status);
                        krnP4PutStr(" chen ");
                        krnP4PutHex32((uint32_t)sc.chen);
                        krnP4PutStr(" frames ");
                        krnP4PutDec((uint32_t)sc.dma_frames);
                        krnP4PutStr(" faults ");
                        krnP4PutHex32((uint32_t)sc.dma_faults);
#ifdef P4_B6_HANDOFF_GATE
                        krnP4PutStr(" active ");
                        krnP4PutHex32((uint32_t)sc.active_fb);
                        krnP4PutStr(" swaps ");
                        krnP4PutDec((uint32_t)sc.dma_swaps);
#ifdef P4_B6_DIRTY_GATE
                        krnP4PutStr(" pending ");
                        krnP4PutHex32((uint32_t)sc.pending_fb);
                        krnP4PutStr(" submits ");
                        krnP4PutDec((uint32_t)sc.dirty_submits);
                        krnP4PutStr(" rejects ");
                        krnP4PutDec((uint32_t)sc.dirty_rejects);
                        if (raw || i1 || sc.dma_faults)
                            dirty_status_failures++;
#endif
#endif
                        krnP4PutStr("\n");

                        krnTimerWait(P4_TICK_HZ);
                    }
#ifdef P4_B6_DIRTY_GATE
                    krnP4ScanoutState(&sc);
                    krnP4PutStr("[b6dirty] submitted ");
                    krnP4PutDec((uint32_t)sc.dirty_submits);
                    krnP4PutStr(" swapped ");
                    krnP4PutDec((uint32_t)sc.dma_swaps);
                    krnP4PutStr(" pending ");
                    krnP4PutHex32((uint32_t)sc.pending_fb);
                    krnP4PutStr(" rejects ");
                    krnP4PutDec((uint32_t)sc.dirty_rejects);
                    krnP4PutStr(" status failures ");
                    krnP4PutDec((uint32_t)dirty_status_failures);
                    if (sc.dirty_submits == (unsigned long)P4_SCANOUT_SECS
                        && sc.dma_swaps == sc.dirty_submits
                        && sc.pending_fb == 0 && sc.dirty_rejects == 0
                        && dirty_status_failures == 0 && sc.dma_faults == 0)
                        krnP4PutStr(" PASSED\n");
                    else
                        krnP4PutStr(" FAILED\n");
#endif
#ifdef P4_B5_CONCURRENT_STRESS
                    krnP4B5StressEnd(stress_completed,
                        stress_ready &&
                        stress_completed == (unsigned long)P4_SCANOUT_SECS);
#endif
                }

                krnP4ScanoutQuiesce();
                krnP4DsiPatternOff();
                (void)krnP4PanelSafe();
#ifdef P4_B6_HANDOFF_GATE
                krnP4PutStr("[b6]     handoff gate stopped, panel safe;"
                            " reset to run it again\n");
#else
                krnP4PutStr("[b5]     scanout stopped, panel safe;"
                            " reset to run it again\n");
#endif
#endif
                return;
            }
            else if (init == P4_DSI_OK)
                krnP4PutStr("[b5]     no PSRAM, so no frame to scan out\n");
#endif

#ifdef P4_PATTERN_TEST
            /*
             * B4.  The host's own pattern generator, then the backlight.
             *
             * This order is the whole safety property of the phase: the link
             * carries a defined image before anything is lit, so a failure
             * anywhere above leaves a dark panel rather than a bright one
             * showing whatever the controller held.
             */
            if (init == P4_DSI_OK)
            {
                struct P4DsiPattern pat;

                (void)krnP4DsiPatternOn(&pat);

                krnP4PutStr("[b4]     host timing: hsa ");
                krnP4PutDec((uint32_t)pat.hsa);
                krnP4PutStr(", hbp ");
                krnP4PutDec((uint32_t)pat.hbp);
                krnP4PutStr(", hact ");
                krnP4PutDec((uint32_t)pat.hact);
                krnP4PutStr(", hfp ");
                krnP4PutDec((uint32_t)pat.hfp);
                krnP4PutStr(", hline ");
                krnP4PutDec((uint32_t)pat.hline);
                krnP4PutStr("\n");

                /*
                 * The frame rate, derived and not measured, and the difference
                 * matters.  Revision 1.x has no VSYNC interrupt in the bridge -
                 * only underrun - and the panel's scanline register is behind
                 * the read path that does not work, so there is no event this
                 * port can count.  What follows is arithmetic from the
                 * programmed totals and an exactly divided clock.
                 */
                krnP4PutStr("[b4]     ");
                krnP4PutDec((uint32_t)pat.htotal_px);
                krnP4PutStr(" x ");
                krnP4PutDec((uint32_t)pat.vtotal_px);
                krnP4PutStr(" at ");
                krnP4PutDec((uint32_t)pat.frame_mhz);
                krnP4PutStr(" MHz is ");
                {
                    unsigned long total = pat.htotal_px * pat.vtotal_px;
                    unsigned long centihz = pat.frame_mhz * 100000000UL / total;

                    krnP4PutDec((uint32_t)(centihz / 100));
                    krnP4PutStr(".");
                    krnP4PutDec((uint32_t)(centihz % 100));
                    krnP4PutStr(" Hz, derived and not measured\n");
                }

                {
                    unsigned long pkt = 0, i0 = 0, i1 = 0;
                    unsigned long brg;

                    krnTimerWait(10);           /* 100 ms of frames */
                    krnP4DsiCmdStatus(&pkt, &i0, &i1);
                    brg = p4_r32(P4_DSI_BRG_BASE + 0x58);   /* INT_RAW */
                    krnP4PutStr("[b4]     after 100 ms: pkt ");
                    krnP4PutHex32((uint32_t)pkt);
                    krnP4PutStr(" int0 ");
                    krnP4PutHex32((uint32_t)i0);
                    krnP4PutStr(" int1 ");
                    krnP4PutHex32((uint32_t)i1);
                    krnP4PutStr(" brg ");
                    krnP4PutHex32((uint32_t)brg);
                    krnP4PutStr((brg & 1) ? "  UNDERRUN\n" : "  no underrun\n");
                }

                if (krnP4PanelBacklightOn() == P4_I2C_OK)
                {
                    struct P4BacklightState bl;

                    krnP4PanelBacklightState(&bl);
                    krnP4PutStr("[b4]     backlight: latch ");
                    krnP4PutHex32(bl.latch);
                    krnP4PutStr(", expander pins ");
                    krnP4PutHex32(bl.expander_pins);
                    krnP4PutStr(", gpio14 driven ");
                    krnP4PutDec((uint32_t)bl.out_level);
                    krnP4PutStr(" reads ");
                    krnP4PutDec((uint32_t)bl.pin_level);
                    krnP4PutStr(", out_sel ");
                    krnP4PutHex32((uint32_t)bl.out_sel);
                    krnP4PutStr(", iomux ");
                    krnP4PutHex32((uint32_t)bl.iomux);
                    krnP4PutStr("\n");
                }
                else
                    krnP4PutStr("[b4]     the backlight would not switch on\n");

                /*
                 * Left running deliberately.  Everything else in this probe
                 * returns the panel to safe, and here that would blank the one
                 * thing there is to look at.  The next reset ends it.
                 */
                krnP4PutStr("[b4]     left running; reset to end it\n");
                return;
            }
#endif
        }
        else
        {
            krnP4PutStr(r == P4_DSI_NO_LOCK
                        ? "[dsi]    B3 stage one FAILED: the pll never locked."
                          "  The fixed phy reference is the first suspect\n"
                        : "[dsi]    B3 stage one FAILED: a lane never reached"
                          " stop state\n");
            krnP4DsiPhyDown();
        }

        (void)krnP4PanelSafe();
        krnP4PutStr("[dsi]    panel returned to safe, backlight never on\n");
    }
#endif
}

#ifdef P4_C4_TOUCH_PROBE
/*
 * C4's first hardware question, deliberately short of controller init.
 *
 * The D1001 manufacturer tree names a GSL3670 at 0x40, with an interrupt on
 * GPIO16 and reset on PCA9535 output 12.  That is source evidence, not a
 * measurement of this board.  This probe establishes the hardware half with
 * address-only ACKs and the Silead-family ID/status registers.  It does not
 * touch the expander reset bit and does not copy or execute the vendor's RAM
 * firmware.  GPIO16 is observed exactly as inherited: no mux, direction,
 * pull or interrupt register is changed.
 *
 * I2C0 currently answers reliably only with the existing driver's 10-kHz
 * timing.  The 100-kHz failure is retained as a separate transport defect;
 * pretending the target is a 10-kHz device would turn that defect into a
 * hardware contract.
 */
static void krnP4C4TouchProbe(void)
{
    static const unsigned char id_reg = 0xFC;
    static const unsigned char status_reg = 0xB0;
    unsigned char id[4] = { 0, 0, 0, 0 };
    unsigned char status[4] = { 0, 0, 0, 0 };
    unsigned long iomux, gpio;
    int camera, touch, rid, rstatus;

    iomux = p4_r32(P4_IOMUX_BASE + P4_IOMUX_PIN(P4_D1001_TOUCH_IRQ_GPIO));
    gpio = (p4_r32(P4_GPIO_BASE + P4_GPIO_IN)
            >> P4_D1001_TOUCH_IRQ_GPIO) & 1UL;

    krnP4PutStr("[touch]  C4 passive identification; no reset, no firmware\n");
    krnP4PutStr("[touch]  GPIO16 inherited level ");
    krnP4PutDec((uint32_t)gpio);
    krnP4PutStr(", iomux ");
    krnP4PutHex32((uint32_t)iomux);
    krnP4PutStr("\n");

    if (!krnP4I2CInit(0, P4_D1001_I2C0_SDA_GPIO,
                      P4_D1001_I2C0_SCL_GPIO, 10000UL))
    {
        krnP4PutStr("[touch]  I2C0 did not configure\n");
        goto restore_i2c1;
    }

    /* 0x36 is the independent control target observed on this same bus. */
    camera = krnP4I2CProbe(0x36);
    touch = krnP4I2CProbe(P4_D1001_TOUCH_ADDR);
    rid = krnP4I2CTransfer(P4_D1001_TOUCH_ADDR, &id_reg, 1, id, 4);
    rstatus = krnP4I2CTransfer(P4_D1001_TOUCH_ADDR, &status_reg, 1,
                              status, 4);

    krnP4PutStr("[touch]  0x36 control ACK: ");
    krnP4PutStr(krnP4I2CName(camera));
    krnP4PutStr("\n[touch]  0x40 address ACK: ");
    krnP4PutStr(krnP4I2CName(touch));
    krnP4PutStr("\n[touch]  0xfc ID read: ");
    krnP4PutStr(krnP4I2CName(rid));
    if (rid == P4_I2C_OK)
    {
        krnP4PutStr(", little-endian ");
        krnP4PutHex32((uint32_t)id[0] | ((uint32_t)id[1] << 8)
                      | ((uint32_t)id[2] << 16) | ((uint32_t)id[3] << 24));
    }
    krnP4PutStr("\n[touch]  0xb0 RAM/status read: ");
    krnP4PutStr(krnP4I2CName(rstatus));
    if (rstatus == P4_I2C_OK)
    {
        krnP4PutStr(", little-endian ");
        krnP4PutHex32((uint32_t)status[0] | ((uint32_t)status[1] << 8)
                      | ((uint32_t)status[2] << 16)
                      | ((uint32_t)status[3] << 24));
    }
    krnP4PutStr("\n");

restore_i2c1:
    /* Panel/DSI startup below owns I2C1.  Leave the shared transport there. */
    if (!krnP4I2CInit(1, P4_D1001_I2C1_SDA_GPIO,
                      P4_D1001_I2C1_SCL_GPIO, 100000UL))
        krnP4PutStr("[touch]  WARNING: I2C1 restore failed\n");
    else
        krnP4PutStr("[touch]  I2C1 restored for the normal C3 display path\n");
}

#ifdef P4_C4_TOUCH_LOAD
static void krnP4C4TouchLoad(void)
{
    uint32_t status = 0;
    unsigned int failed_record = 0;
    int r;

    krnP4PutStr("[touch]  C4 bounded reset/load/start/status diagnostic\n");
    r = krnP4GSLLoadDiagnostic(&status, &failed_record);
    krnP4PutStr("[touch]  firmware load result: ");
    krnP4PutStr(krnP4I2CName(r));
    if (r != P4_I2C_OK)
    {
        krnP4PutStr(", record/stage ");
        krnP4PutDec(failed_record);
    }
    krnP4PutStr(", 0xb0 ");
    krnP4PutHex32(status);
    krnP4PutStr(r == P4_I2C_OK
                ? " -- volatile controller program alive\n"
                : " -- no touch device published; normal boot continues\n");
#ifdef P4_C4_TOUCH_SAMPLE
    if (r == P4_I2C_OK)
    {
        r = krnP4GSLSampleDiagnostic(20);
        krnP4PutStr("[touch]  raw sampling result: ");
        krnP4PutStr(krnP4I2CName(r));
        krnP4PutStr("; no input events were generated\n");
    }
#endif
}
#endif
#endif
#endif /* P4_PANEL_PROBE */

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

#if defined(P4_B5_CONCURRENT_STRESS) || defined(P4_B6_DOUBLE_BUFFER)
    /* B5 stress keeps scanning after Exec starts; B6 establishes the same
       permanent ownership contract for both handoff surfaces.  Keep every
       reserved framebuffer outside every allocation and package placement. */
    if (end > P4_FB_BASE)
        end = P4_FB_BASE;
#endif

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

#if defined(P4_B5_CONCURRENT_STRESS) || defined(P4_B6_DOUBLE_BUFFER)
    if (psram_end > P4_FB_BASE)
        psram_end = P4_FB_BASE;
#endif

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

#ifdef P4_FLASHDISK_PROBE
/*
 * Can the flash development volume be read, and is it the volume that was
 * built?
 *
 * The first measured step towards ending the card handoffs.  Nothing here
 * needs exec, DOS or a block device: it maps the region through the same
 * krnP4FlashMap() the package loader uses and reads the structures the
 * generator wrote.  So if this fails, it fails before anything else is at
 * stake, and if it passes, the device that comes next has a proven read path
 * underneath it.
 *
 * What is checked is structural rather than a hash of the whole thing.  The
 * numbers below are the ones image/mmakefile.src puts in the .layout file
 * next to the image, so they can be compared without deriving anything: an
 * MBR with one FAT16 LBA entry, a boot sector whose geometry matches, and
 * the label and serial the build was told to use.  Two sector hashes are
 * printed as well, for comparison against the host.
 */
static uint32_t krnP4FlashHash(const unsigned char *p, unsigned long len)
{
    uint32_t h = 2166136261UL;

    while (len--)
    {
        h ^= *p++;
        h *= 16777619UL;
    }
    return h;
}

static uint16_t krnP4Rd16LE(const unsigned char *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t krnP4Rd32LE(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int krnP4FlashDiskProbe(unsigned long pkg_off, unsigned long pkg_size)
{
    unsigned long base = pkg_off + P4_FLASHDISK_PART_OFFSET;
    const unsigned char *sec;
    int passed = 1;

    krnP4PutStr("[fdisk]  flash volume probe starting\n");
    krnP4PutStr("[fdisk]  partition at ");
    krnP4PutHex32((uint32_t)pkg_off);
    krnP4PutStr(" size ");
    krnP4PutHex32((uint32_t)pkg_size);
    krnP4PutStr(", volume at ");
    krnP4PutHex32((uint32_t)base);
    krnP4PutStr(" size ");
    krnP4PutHex32((uint32_t)P4_FLASHDISK_SIZE);
    krnP4PutStr("\n");

    if (P4_FLASHDISK_PART_OFFSET + P4_FLASHDISK_SIZE > pkg_size)
    {
        krnP4PutStr("[fdisk]  the volume does not fit in the partition\n");
        return 0;
    }

    /* Sector 0: the MBR the generator wrote. */
    sec = krnP4FlashMap(base, 512);
    if (!sec)
    {
        krnP4PutStr("[fdisk]  could not map sector 0\n");
        return 0;
    }

    krnP4PutStr("[fdisk]  MBR signature ");
    krnP4PutHex32((uint32_t)krnP4Rd16LE(sec + 510));
    krnP4PutStr(", hash ");
    krnP4PutHex32(krnP4FlashHash(sec, 512));
    krnP4PutStr("\n");
    if (krnP4Rd16LE(sec + 510) != 0xAA55)
    {
        krnP4PutStr("[fdisk]  no MBR signature; nothing was written here\n");
        return 0;
    }

    {
        unsigned char type = sec[446 + 4];
        uint32_t first = krnP4Rd32LE(sec + 446 + 8);
        uint32_t count = krnP4Rd32LE(sec + 446 + 12);

        krnP4PutStr("[fdisk]  partition entry: type ");
        krnP4PutHex32((uint32_t)type);
        krnP4PutStr(", first sector ");
        krnP4PutDec(first);
        krnP4PutStr(", sectors ");
        krnP4PutDec(count);
        /* 0x0e is "W95 16-bit LBA FAT", which
           rom/partition/partition_types.c maps to FAT\1, the DosType the FAT
           handler registers for FAT16. */
        krnP4PutStr(type == 0x0E ? ", FAT16 LBA\n" : ", NOT FAT16 LBA\n");
        if (type != 0x0E)
            passed = 0;

        /* The boot sector of that partition. */
        sec = krnP4FlashMap(base + (unsigned long)first * 512, 512);
        if (!sec)
        {
            krnP4PutStr("[fdisk]  could not map the boot sector\n");
            return 0;
        }
    }

    krnP4PutStr("[fdisk]  boot sector hash ");
    krnP4PutHex32(krnP4FlashHash(sec, 512));
    krnP4PutStr(", signature ");
    krnP4PutHex32((uint32_t)krnP4Rd16LE(sec + 510));
    krnP4PutStr("\n[fdisk]    bytes/sector ");
    krnP4PutDec((uint32_t)krnP4Rd16LE(sec + 11));
    krnP4PutStr(", sectors/cluster ");
    krnP4PutDec((uint32_t)sec[13]);
    krnP4PutStr(", reserved ");
    krnP4PutDec((uint32_t)krnP4Rd16LE(sec + 14));
    krnP4PutStr(", fats ");
    krnP4PutDec((uint32_t)sec[16]);
    krnP4PutStr("\n[fdisk]    root entries ");
    krnP4PutDec((uint32_t)krnP4Rd16LE(sec + 17));
    krnP4PutStr(", fat sectors ");
    krnP4PutDec((uint32_t)krnP4Rd16LE(sec + 22));
    krnP4PutStr(", total sectors ");
    krnP4PutDec(krnP4Rd32LE(sec + 32));
    krnP4PutStr("\n[fdisk]    serial ");
    krnP4PutHex32(krnP4Rd32LE(sec + 39));
    krnP4PutStr(", label '");
    {
        unsigned int i;

        for (i = 0; i < 11; ++i)
            krnP4PutC((char)sec[43 + i]);
    }
    krnP4PutStr("', type '");
    {
        unsigned int i;

        for (i = 0; i < 8; ++i)
            krnP4PutC((char)sec[54 + i]);
    }
    krnP4PutStr("'\n");

    if (krnP4Rd16LE(sec + 510) != 0xAA55)
        passed = 0;
    if (krnP4Rd16LE(sec + 11) != 512 || sec[16] != 2)
        passed = 0;
    /* FAT16 is the whole point: a zero here would mean the generator wrote a
       FAT32 boot sector, which cannot work in a volume this size. */
    if (krnP4Rd16LE(sec + 22) == 0 || krnP4Rd16LE(sec + 17) == 0)
    {
        krnP4PutStr("[fdisk]  boot sector is not FAT16\n");
        passed = 0;
    }
    if (sec[54] != 'F' || sec[55] != 'A' || sec[56] != 'T'
        || sec[57] != '1' || sec[58] != '6')
    {
        krnP4PutStr("[fdisk]  filesystem type string is not FAT16\n");
        passed = 0;
    }

    krnP4PutStr("[fdisk]  flash volume probe ");
    krnP4PutStr(passed ? "passed\n" : "FAILED\n");
    return passed;
}
#endif /* P4_FLASHDISK_PROBE */

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

    /*
     * The digital supply, before the clock that runs on it.
     *
     * This is the fix for the failure that cost 23 August, and its position
     * here is the second half of that fix.  Reset leaves the setting at 20;
     * this chip's PSRAM needs 26 and the vendor's own firmware sets it, while
     * ESP-IDF does not touch the register on the P4 at all.  The PMU survives
     * a CPU reset, so a boot after that firmware inherited a working supply
     * and a boot from cold did not - the same binary, PSRAM present and
     * silent, which is exactly what happened.
     *
     * It is not in the PSRAM bring-up because it is not a PSRAM setting.  Put
     * there, with the clock raised first, the board hung so hard it stopped
     * answering USB.  20 does not carry 360 MHz.
     */
    {
        unsigned char supply = krnP4SupplyUp();

        krnP4PutStr("[clock]  supply  found ");
        krnP4PutDec((uint32_t)supply);
        krnP4PutStr(", now ");
        krnP4PutDec((uint32_t)krnP4SupplyLevel());
        krnP4PutStr("\n");
    }

    /*
     * The CPU clock, before PSRAM.
     *
     * Deliberately in this order: the PSRAM read sampling is calibrated a few
     * lines below, and a calibration made at one CPU and memory clock and used
     * at another is a calibration for conditions that no longer hold.  The
     * AXI path between the core and the controller runs on MEM_CLK, which
     * doubles here.
     */
    {
        struct P4CPUClock clk;

        krnP4CPUClockRead(&clk);
        krnP4PutStr("[clock]  as found  cpu /");
        krnP4PutDec(clk.cpu_div);
        krnP4PutStr("  mem /");
        krnP4PutDec(clk.mem_div);
        krnP4PutStr("  sys /");
        krnP4PutDec(clk.sys_div);
        krnP4PutStr("  apb /");
        krnP4PutDec(clk.apb_div);
        krnP4PutStr(clk.source == P4_HP_ROOT_SRC_CPLL ? "  root cpll\n"
                  : clk.source == P4_HP_ROOT_SRC_XTAL ? "  root xtal\n"
                                                      : "  root rc\n");

#ifdef P4_CPU_MHZ
        if (krnP4CPUClockSet(P4_CPU_MHZ))
        {
            krnP4CPUClockRead(&clk);
            krnP4PutStr("[clock]  set to    cpu /");
            krnP4PutDec(clk.cpu_div);
            krnP4PutStr("  mem /");
            krnP4PutDec(clk.mem_div);
            krnP4PutStr("  sys /");
            krnP4PutDec(clk.sys_div);
            krnP4PutStr("  apb /");
            krnP4PutDec(clk.apb_div);
            krnP4PutStr(", asked for ");
            krnP4PutDec(P4_CPU_MHZ);
            krnP4PutStr(" MHz\n");
        }
        else
            krnP4PutStr("[clock]  the divider change was refused,"
                        " unchanged\n");
#endif
    }

    /*
     * Whatever the last boot left running, before the PSRAM is touched.
     *
     * B5 leaves the scanout running on purpose and a CPU reset does not reset
     * the GDMA, so the channel is still reading the framebuffer over AXI while
     * the bring-up below reconfigures the MSPI controller it reads through.
     * Measured: three resets in a row hung at exactly this point, and only a
     * power cycle recovered the board.
     */
#ifndef P4_NO_QUIESCE
    krnP4ScanoutQuiesce();
#endif

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
        up = krnPSRAMBringUp(&psram, P4_PSRAM_TARGET_HZ);
        csr_set(mstatus, MSTATUS_MIE);
        p4_psram_probe_latency_seen = psram.probe_latency;
        p4_psram_calib_entry = psram.ana_trace[0];
        p4_psram_bias_seen = psram.bias_set;

        if (up)
        {
            krnP4PutStr("[psram]  chip   ");
            krnP4PutDec((uint32_t)(psram.size / (1024 * 1024)));
            krnP4PutStr(" MB at ");
            krnP4PutDec((uint32_t)(psram.clock_hz / 1000000));
            krnP4PutStr(" MHz, vendor ");
            krnP4PutHex32((uint32_t)psram.vendor);
            krnP4PutStr(", a word written and read back after ");
            krnP4PutDec((uint32_t)psram.identify_attempts);
            krnP4PutStr(psram.identify_attempts == 1 ? " attempt\n"
                                                     : " attempts\n");

            /*
             * Which read latency the chip was found in, as opposed to the one
             * it was then set to.  A value other than this port's own means
             * the part arrived configured by other firmware and the sweep is
             * what got in - the case that used to report an absent chip.
             */
            /*
             * Whether this boot calibrated the PLL or inherited a calibration.
             *
             * ana_trace[0] is ANA_PLL_CTRL0 as found, before this port clears
             * MSPI_CAL_STOP.  Bit 8 set there means some earlier firmware left
             * the calibration done and this run proves nothing about being able
             * to start it; bit 8 clear there and PSRAM up means this port did
             * the calibration itself.  Printed because the difference is the
             * whole question and it is not visible any other way.
             */
            krnP4PutStr("[psram]  calib  entry ");
            krnP4PutHex32((uint32_t)psram.ana_trace[0]);
            krnP4PutStr((psram.ana_trace[0] & (1UL << 8))
                        ? " inherited, " : " done here, ");
            krnP4PutDec((uint32_t)psram.mpll_attempts);
            krnP4PutStr(psram.mpll_attempts == 1 ? " attempt\n" : " attempts\n");

            krnP4PutStr("[psram]  supply inherited ");
            krnP4PutDec((uint32_t)psram.bias_found);
            krnP4PutStr(", set ");
            krnP4PutDec((uint32_t)psram.bias_set);
            krnP4PutStr("\n[psram]  found  read latency ");
            if (psram.probe_latency >= 0)
                krnP4PutDec((uint32_t)psram.probe_latency);
            else
                krnP4PutStr("none of eight");
            krnP4PutStr(", set to ");
            krnP4PutDec((uint32_t)(psram.clock_hz > 80000000UL
                                   ? P4_PSRAM_RD_LATENCY_FAST
                                   : P4_PSRAM_RD_LATENCY_SLOW));
            krnP4PutStr("\n[psram]  command timeouts ");
            krnP4PutDec((uint32_t)psram.cmd_timeouts);
            krnP4PutStr(", FSM recoveries ");
            krnP4PutDec((uint32_t)psram.fsm_recoveries);
            krnP4PutStr("\n");

            if (psram.fast_requested)
                krnP4ReportPSRAMTuning(&psram);

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
#ifdef P4_PSRAM_STRESS
                    krnP4PSRAMStress(psram.size);
#endif
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
        {
            krnP4PutStr(psram.mpll_reason == P4_MPLL_NO_BUS
                ? "[psram]  chip   the analogue configuration bus did not"
                  " answer"
                : "[psram]  chip   the mpll calibration never finished");
            krnP4PutStr(", state ");
            krnP4PutHex32((uint32_t)psram.mpll_state);
            krnP4PutStr(", ana_pll_ctrl0 ");
            krnP4PutHex32((uint32_t)psram.ana_pll_ctrl0);
            krnP4PutStr("\n[psram]  trace  ");
            {
                unsigned int t;

                for (t = 0; t < 4; ++t)
                {
                    krnP4PutHex32((uint32_t)psram.ana_trace[t]);
                    krnP4PutStr(" ");
                }
            }
            krnP4PutStr("spins ");
            krnP4PutDec((uint32_t)psram.ana_spins);
            krnP4PutStr("\n");
        }
        else if (!psram.clock_hz)
            krnP4PutStr("[psram]  chip   the controller kept no clock\n");
        else
        {
            /*
             * The PLL's own registers, read back off the configuration bus.
             *
             * A write there reports nothing, so a bring-up that returns success
             * has only shown that the bus answered and the calibration ended -
             * not that the divider took.  This is the one measurement that was
             * missing while PSRAM went from working to silent with the binary
             * unchanged.
             */
            krnP4PutStr("[psram]  mpll   state ");
            krnP4PutHex32((uint32_t)psram.mpll_state);
            krnP4PutStr(", ana_pll_ctrl0 ");
            krnP4PutHex32((uint32_t)psram.ana_pll_ctrl0);
            krnP4PutStr(", bus ");
            krnP4PutDec((uint32_t)(psram.clock_hz / 1000000));
            krnP4PutStr(" MHz, identify tried ");
            krnP4PutDec((uint32_t)psram.identify_attempts);
            krnP4PutStr("\n");

            krnP4PutStr("[psram]  chip   no answer - vendor ");
            krnP4PutHex32((uint32_t)psram.vendor);
            krnP4PutStr(" mr2 ");
            krnP4PutHex32((uint32_t)psram.density);
            krnP4PutStr(", data ");
            krnP4PutStr(psram.connected ? "carried\n" : "lost\n");

            krnP4PutStr("[psram]  found  no answer in any of eight read"
                        " latencies, supply inherited ");
            krnP4PutDec((uint32_t)psram.bias_found);
            krnP4PutStr(", set ");
            krnP4PutDec((uint32_t)psram.bias_set);
            krnP4PutStr("\n[psram]  command timeouts ");
            krnP4PutDec((uint32_t)psram.cmd_timeouts);
            krnP4PutStr(", FSM recoveries ");
            krnP4PutDec((uint32_t)psram.fsm_recoveries);
            krnP4PutStr("\n");

            /*
             * The handover state: the registers this port inherits rather
             * than sets, read before it writes any of them.
             *
             * Every constant and register the bring-up writes has now been
             * read against ESP-IDF's own sequence and matches it, so a
             * failure that survives a real power cycle is not a wrong value
             * here.  What is left is what arrives set: MSPI2 serves flash
             * and PSRAM from one block, and the second-stage bootloader
             * configures the flash half of it.
             *
             * In order: soc_clk_ctrl0, peri_clk_ctrl00, hp_rst_en0,
             * mspi2 sram_clk, mspi3 clock, timing_cali, smem_timing_cali,
             * smem_ac, psram_dqs_0.
             */
            {
                const unsigned long *e = (const unsigned long *)&psram.entry;
                unsigned int k;

                krnP4PutStr("[psram]  entry");
                for (k = 0; k < sizeof(psram.entry) / sizeof(*e); ++k)
                {
                    krnP4PutStr(" ");
                    krnP4PutHex32((uint32_t)e[k]);
                }
                krnP4PutStr("\n");
            }
        }

        __esp32p4_psram_size = psram.size;
    }
#ifdef P4_PSRAM_PROBE
    psram_probe();
#endif
#ifdef P4_PANEL_PROBE
#ifndef P4_B5_CONCURRENT_STRESS
    krnP4PanelProbe();
#endif
#endif
#ifdef P4_C4_TOUCH_PROBE
    krnP4C4TouchProbe();
#endif
#ifdef P4_C4_TOUCH_LOAD
    krnP4C4TouchLoad();
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

            /*
             * Publish where the volume is, so flashdisk.device can serve it.
             * Only if it fits: a partition too small for the split would
             * otherwise have the device read package bytes as sectors.
             */
            if (P4_FLASHDISK_PART_OFFSET + P4_FLASHDISK_SIZE <= pkg_size)
                __esp32p4_flashdisk_base =
                    pkg_off + P4_FLASHDISK_PART_OFFSET;

#ifdef P4_FLASHDISK_PROBE
            /*
             * Before the package is copied, because krnP4FlashMap() has one
             * scratch window and the loader is about to use it.  Reading the
             * volume first leaves the loader the state it expects.
             */
            (void)krnP4FlashDiskProbe(pkg_off, pkg_size);
#endif

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
