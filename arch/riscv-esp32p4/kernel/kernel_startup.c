/*
    Copyright (c) 2026, The AROS Development Team. All rights reserved.

    Desc: Machine mode bring-up for the esp32p4-riscv target.

    Entered from startup.S with the hart number in a0 and, for now,
    nothing in a1; the second argument is where a built-in device tree
    would arrive if this platform grows one.

    The order it works in: silence the watchdogs the ROM armed, bring up
    the CLIC and the tick, the memory, the clocks and the PSRAM, load the
    BSP package from flash, and hand the machine to exec.
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




/* Not static: the exec arch layer's CacheClearU() has no range of its own
   and needs to know how much of the external window is real. */
unsigned long __esp32p4_psram_size;

/* Where the flash development volume starts, for flashdisk.device.  Zero
   until the arosbsp partition has been found, and left at zero if it never
   is, which is what makes the device refuse to open rather than serve
   whatever is at offset zero. */
unsigned long __esp32p4_flashdisk_base;
static struct MemHeader *__esp32p4_mh_psram;
static UWORD *__esp32p4_modules_low;
static UWORD *__esp32p4_modules_high;

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

    /*
     * MEMF_FAST means ordinary, non-chip RAM, as on every other port; it
     * says nothing about speed.  Without it AvailMem(MEMF_FAST) left out
     * the PSRAM and an explicit MEMF_FAST request could only get internal
     * SRAM.  The priority below the code-capable SRAM still keeps untyped
     * allocations there first.
     */
    __esp32p4_mh_psram = (struct MemHeader *)first_free;
    krnCreateMemHeader("External Memory", -20, (APTR)first_free,
                       end - first_free,
                       MEMF_FAST | MEMF_PUBLIC | MEMF_KICK | MEMF_LOCAL);
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

    if (!krnPrepareExecBase(ranges, mh, krnPrepareBootTags()))
    {
        krnP4PutStr("[exec]   krnPrepareExecBase FAILED\n");
        return;
    }

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
    }

    {
        struct MemHeader *m;

        ForeachNode(&SysBase->MemList, m)
        {
            krnP4PutStr("[exec]   memory ");
            krnP4PutHex32((uint32_t)(IPTR)m->mh_Lower);
            krnP4PutStr(" - ");
            krnP4PutHex32((uint32_t)(IPTR)m->mh_Upper);
            krnP4PutStr(", ");
            krnP4PutDec((uint32_t)m->mh_Free);
            krnP4PutStr(" bytes free\n");
        }
    }

    /* From here on tasks print, and the console must not block them */
    krnP4ConsoleRuntime();

    InitCode(RTF_SINGLETASK, 0);
    InitCode(RTF_COLDSTART, 0);
}

void kernel_cstart(unsigned long hartid, void *fdt)
{
    (void)fdt;

    /*
     * platform_init() comes after a line of output, not before, so that a
     * hang inside it is still attributable to it.
     */
    krnP4PutStr("\n\n[kernel] entered on hart ");
    krnP4PutDec((uint32_t)hartid);
    krnP4PutStr("\n");
    krnWdtReportReset();
    platform_init();

    krnCLICInit();
    /* riscv-native dispatches the tick through the hook table, so the
       table has to be filled before the tick starts */
    krnP4PlatformHooks();
    krnTimerInit();
    csr_set(mstatus, MSTATUS_MIE);
    krnWdtArm();

    krnRAMInit();

    /*
     * The digital supply, before the clock that runs on it. Reset leaves
     * the setting at 20; this chip's PSRAM needs 26, and the PMU survives a
     * CPU reset, so a boot after the vendor firmware would inherit a working
     * supply and a boot from cold would not. It is not part of the PSRAM
     * bring-up because it is not a PSRAM setting, and 20 does not carry the
     * clock raised below.
     */
    krnP4SupplyUp();

#ifdef P4_CPU_MHZ
    /*
     * The CPU clock, before PSRAM: the PSRAM read sampling is calibrated
     * below, and the AXI path to the controller runs on MEM_CLK, which this
     * changes.
     */
    if (!krnP4CPUClockSet(P4_CPU_MHZ))
        krnP4PutStr("[clock]  the divider change was refused, unchanged\n");
#endif

    /*
     * Whatever the last boot left running, before the PSRAM is touched. A
     * CPU reset does not reset the GDMA, so a scanout left running by
     * earlier firmware is still reading over AXI while the bring-up below
     * reconfigures the MSPI controller it reads through; that has hung the
     * bring-up until a power cycle.
     */
    krnP4ScanoutQuiesce();

    {
        struct P4PSRAMInfo psram;
        unsigned long bad = 0;
        int up;

        /*
         * With interrupts off: the tick is already running, and a bring-up
         * that can be interrupted between a controller write and the
         * transaction that depends on it is not a sequence.
         */
        csr_clear(mstatus, MSTATUS_MIE);
        up = krnPSRAMBringUp(&psram, P4_PSRAM_TARGET_HZ);
        csr_set(mstatus, MSTATUS_MIE);

        if (up)
        {
            krnPSRAMMap(psram.size);
            if (!krnPSRAMVerify(psram.size, &bad))
            {
                krnP4PutStr("[psram]  mapped, but ");
                krnP4PutHex32((uint32_t)bad);
                krnP4PutStr(" did not read back\n");
                psram.size = 0;
            }
        }
        else
            psram.size = 0;

        krnP4PutStr("[psram]  ");
        if (psram.size)
        {
            krnP4PutDec((uint32_t)(psram.size / (1024 * 1024)));
            krnP4PutStr(" MB at ");
            krnP4PutDec((uint32_t)(psram.clock_hz / 1000000));
            krnP4PutStr(" MHz\n");
        }
        else
            krnP4PutStr("not available\n");

        __esp32p4_psram_size = psram.size;
    }

    /*
     * Parse the partition table. A BSP package partition is validated,
     * copied and relocated into the low end of PSRAM before exec is allowed
     * to publish the remaining memory.
     */
    krnP4FlashSurvey();
    {
        unsigned long pkg_off = 0, pkg_size = 0;
        int found = krnP4PartitionScan(P4_BSP_PART_TYPE, P4_BSP_PART_LABEL,
                                       &pkg_off, &pkg_size, 1);
        int package_claimed_psram = 0;

        if (found > 0)
        {
            /*
             * Publish where the volume is, so flashdisk.device can serve
             * it. Only if it fits: a partition too small for the split
             * would otherwise have the device read package bytes as
             * sectors.
             */
            if (P4_FLASHDISK_PART_OFFSET + P4_FLASHDISK_SIZE <= pkg_size)
                __esp32p4_flashdisk_base = pkg_off + P4_FLASHDISK_PART_OFFSET;

            /* Nonzero means PSRAM bytes were already copied or placed:
               withhold that window rather than describe it as free. */
            if (__esp32p4_psram_size)
                package_claimed_psram =
                    krnP4LoadBSPPackage(pkg_off, pkg_size) != 0;
        }

        if (__esp32p4_psram_size && !__esp32p4_mh_psram &&
            !package_claimed_psram)
            (void)krnP4PublishPSRAM(P4_PSRAM_WINDOW_BASE);
    }

    krnStartExec();

    /* Not reached in a complete boot: exec's cold start does not return */
    for (;;)
        krnTimerWait(P4_TICK_HZ);
}
