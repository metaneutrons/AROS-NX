/*
    Copyright (c) 2026, The AROS Development Team. All rights reserved.
    $Id$

    Desc: esp32p4-riscv kernel internals.
*/

#ifndef KERNEL_INTERN_H_
#define KERNEL_INTERN_H_

#include <aros/libcall.h>
#include <inttypes.h>
#include <exec/lists.h>
#include <exec/execbase.h>
#include <exec/memory.h>
#include <utility/tagitem.h>
#include <stdio.h>
#include <stdarg.h>

#undef KernelBase
struct KernelBase;

#define __STR(x) #x
#define STR(x) __STR(x)

/* The hart this image was entered on, read from mhartid in startup.S */
extern unsigned long __boot_hartid;

/*
 * Depth of nested trap handling. KrnIsSuper() reports from it: on a
 * machine that never leaves machine mode the privilege level cannot tell
 * task context from kernel context, and what callers such as the exec
 * semaphores need to know is whether sleeping is possible at all. The
 * trap handler maintains it.
 */
extern int __esp32p4_trap_depth;

/*
 * Which hart this is. The shared rv32 layer's getcpunumber.c calls this
 * and expects the platform to supply it. mhartid is readable here, so
 * this answers for whichever hart asks rather than for the boot one -
 * the supervisor mode ports have to cache the boot hart id instead,
 * because mhartid is out of their reach.
 */
static inline int GetCPUNumber(void)
{
    int hartid;

    __asm__ volatile("csrr %0, mhartid" : "=r"(hartid));
    return hartid;
}

/* The core local interrupt controller (kernel_clic.c) */
void krnCLICInit(void);
void krnCLICEnable(unsigned int line, int edge);
void krnCLICDisable(unsigned int line);
void krnCLICPend(unsigned int line);
void krnCLICClear(unsigned int line);
int  krnCLICPending(unsigned int line);

/* What memory there is (kernel_ram.c) */
void krnRAMInit(void);
void krnRAMReport(void);
extern struct MemHeader *__esp32p4_mh_low;
extern struct MemHeader *__esp32p4_mh_high;

/* The periodic tick and the only clock there is (kernel_timer.c) */
/*
 * I2C transport results.  A NACK and a timeout are different answers and a
 * caller has to be able to tell them apart: nothing at the address, against
 * something holding the line.
 */
#define P4_I2C_OK           0
#define P4_I2C_NACK         (-1)
#define P4_I2C_TIMEOUT      (-2)
#define P4_I2C_ARBLOST      (-3)
#define P4_I2C_STUCK        (-4)
#define P4_I2C_BUSY         (-5)
#define P4_I2C_TOOLONG      (-6)
#define P4_I2C_NOTREADY     (-7)

int krnP4I2CInit(unsigned int port, unsigned int sda_gpio,
                 unsigned int scl_gpio, unsigned long bus_hz);
int krnP4I2CTransfer(unsigned int address,
                     const unsigned char *wbuf, unsigned int wlen,
                     unsigned char *rbuf, unsigned int rlen);
int krnP4I2CProbe(unsigned int address);
void krnP4I2CLastStatus(unsigned long *raw, unsigned long *sr);
#define P4_I2C_MISMATCH     (-8)    /* the device kept something else */

/* What the panel bring-up claimed and where it got to. */
struct P4PanelState
{
    unsigned char claimed;          /* the four pins are ours */
    unsigned char powered;          /* LCD_PWR_EN asserted */
    unsigned char reset_released;   /* the pulse completed */
    UWORD config;                   /* the direction register, read back */
    UWORD output;                   /* the output latch, read back */
    UWORD input;                    /* the pins as found, before anything */
    UWORD found_output;             /* the output latch as found */
    UWORD found_config;             /* the direction register as found */
};

int krnP4PanelClaim(struct P4PanelState *out);
int krnP4PanelPowerUp(struct P4PanelState *out);
int krnP4PanelSafe(void);

/* DSI PHY bring-up results. */
#define P4_DSI_OK               0
#define P4_DSI_NO_LOCK          (-1)
#define P4_DSI_NO_STOPSTATE     (-2)

struct P4DsiState
{
    unsigned long ldo_reg;
    unsigned long ldo_ana;
    unsigned long status;
    unsigned char locked;
    unsigned char lanes_stopped;
    unsigned char pll_n;
    unsigned char hs_freq_sel;
    unsigned short pll_m;
};

void krnP4DsiLdoUp(void);
int  krnP4DsiPhyUp(struct P4DsiState *out);
void krnP4DsiPhyDown(void);
UWORD krnP4PanelStrayBits(void);
const char *krnP4I2CResultName(int result);

/* The SoC clock-tree root dividers, as read back from the registers. */
struct P4CPUClock
{
    unsigned char source;           /* 0 XTAL, 1 CPLL, 2 fast RC */
    unsigned char cpu_div;
    unsigned char cpu_numerator;
    unsigned char cpu_denominator;
    unsigned char mem_div;
    unsigned char sys_div;
    unsigned char apb_div;
};

void krnP4CPUClockRead(struct P4CPUClock *out);
int krnP4CPUClockSet(unsigned int mhz);

void krnTimerInit(void);
void krnTimerAck(void);
uint64_t krnTimerCount(void);
unsigned long krnTimerTicks(void);
int  krnTimerWait(unsigned long ticks);
extern volatile unsigned long __esp32p4_ticks;

/* What the trap handler has seen (kernel_traps.c) */
extern volatile unsigned long __esp32p4_irq_count;
extern volatile unsigned long __esp32p4_irq_last;

/*
 * A fault the caller is deliberately causing (kernel_probe.c). While
 * expect holds a cause code and addr the matching mtval, a trap with both
 * steps over the faulting instruction and bumps caught instead of halting
 * the hart. Nothing outside bring-up probes should ever set them.
 */
extern volatile unsigned long __esp32p4_trap_expect;
extern volatile unsigned long __esp32p4_trap_addr;
extern volatile unsigned long __esp32p4_trap_caught;
void krnP4Probe(void *psram_scratch);

/* Read-only native-SD bring-up probe (kernel_sdmmc.c). */
void krnP4SDMMCProbe(void);

/* Cache maintenance and the cache-off window (kernel_cache.c) */
void krnP4SyncCode(void *addr, unsigned long len);
/* How much of the external window the PSRAM probe found, zero if none */
extern unsigned long __esp32p4_psram_size;
unsigned long krnP4CacheOff(void);
void krnP4CacheOn(unsigned long token);

/*
 * Where the BSP package lives on flash.
 *
 * Its own partition, not a payload hidden inside the app partition. Type
 * 0x40 is the bottom of the range ESP-IDF documents for custom partition
 * types (docs/en/api-guides/partition-tables.rst), with any subtype free;
 * the second stage bootloader falls through to `default: break;` for a
 * type it does not know, so it neither boots it nor complains about it.
 *
 * On the board this is brought up on, the partition replaces ota_1 and
 * runs 0x820000 to 0x1000000. It stops exactly at the 16 MB line rather
 * than inheriting ota_1's 8 MB, because cache mapped flash ends there.
 */
#define P4_BSP_PART_TYPE    0x40
#define P4_BSP_PART_LABEL   "arosbsp"

/*
 * The development volume, at the end of that same partition.
 *
 * The partition is a custom type this port defines, so how it is divided is
 * this port's decision, and dividing it costs nothing: the package uses
 * 1.16 MB of 8.25 MB.  The volume takes the last four megabytes and the
 * package keeps everything below, which the build asserts, so growing the
 * package past the split is a build error rather than a corrupted
 * filesystem.
 *
 * Expressed as an offset inside the partition rather than as a flash
 * address, so it follows the partition if the table ever moves.  On this
 * board that lands it at 0x820000 + 0x3e0000 = 0xc00000, ending exactly at
 * the 16 MB line where cache-mapped flash stops.  That matters: the
 * `storage` partition the ESP-IDF table declares for filesystems begins at
 * 0x1020000, past the limit, so krnP4FlashMap() cannot reach it at all.
 *
 * image/mmakefile.src writes the volume at FLASHDISK_OFFSET with
 * FLASHDISK_SIZE_MB, and those two have to agree with these.  The probe
 * prints both sides so a disagreement shows up as a wrong signature rather
 * than as a mystery.
 */
#define P4_FLASHDISK_PART_OFFSET    0x003E0000UL
#define P4_FLASHDISK_SIZE           0x00400000UL
#define P4_FLASHDISK_PKG_LIMIT      P4_FLASHDISK_PART_OFFSET

/*
 * The absolute flash offset of the volume, filled in once the partition has
 * been located, and zero if it never was.  flashdisk.device reads it; a
 * device that guessed would serve whatever happens to sit at offset zero.
 */
extern unsigned long __esp32p4_flashdisk_base;

/* Reading flash from code that executes out of it (kernel_flash.c) */
void krnP4FlashSurvey(void);
void *krnP4FlashMap(unsigned long paddr, unsigned long len);
int krnP4PartitionScan(unsigned char want_type, const char *want_label,
                       unsigned long *out_off, unsigned long *out_size,
                       int report);

/* Relocating boot-time ELF32 module loader (kernel_elf.c) */
int krnLoadPackage(void *pkg, IPTR pkgsize, IPTR memlow, IPTR memhigh,
                   IPTR *lo, IPTR *hi, IPTR *memused);
extern void *__ks_debuginfo;

/* Machine setup that has to happen before anything else (platform_init.c) */
void platform_init(void);
int  platform_wdt_quiet(void);

/* Early UART0 debug console (kernel_console.c) */
void krnP4PutC(char c);
void krnP4PutStr(const char *s);
void krnP4PutHex32(uint32_t val);
void krnP4PutDec(uint32_t val);
/* One character from the console, or -1 if none is waiting.  Never blocks. */
int krnP4GetC(void);
void krnP4PutDecS(int32_t val);

/*
 * Code and data that must be in SRAM whatever the link script does.
 *
 * With ldscript-xip.lds the image's .text and .rodata are mapped from
 * flash through the cache, which is fine until something has to touch the
 * controller that serves that cache - reconfiguring MSPI, bringing PSRAM
 * up, changing flash timing. Such code has to be fetched from somewhere
 * else while the cache is off, and so does every byte it reads and every
 * function it calls. ESP-IDF spells this IRAM_ATTR; here it is these two.
 *
 * The discipline they cannot enforce: a P4_SRAMCODE function may only call
 * other P4_SRAMCODE functions and touch P4_SRAMDATA while the cache is
 * disabled. noinline keeps the compiler from copying the body into a
 * caller that lives in flash, which would defeat the point silently.
 */
#define P4_SRAMCODE __attribute__((section(".sramtext"), noinline, used))
#define P4_SRAMDATA __attribute__((section(".sramdata"), used))
/*
 * Constant data in SRAM.  A named section may not hold both const and
 * non-const objects - gcc derives the section's flags from what it puts there
 * first and then refuses the other kind - and the linker script globs
 * .sramdata*, so the two names land in the same output section regardless.
 */
#define P4_SRAMRODATA __attribute__((section(".sramdata.ro"), used))

#endif /* KERNEL_INTERN_H_ */
