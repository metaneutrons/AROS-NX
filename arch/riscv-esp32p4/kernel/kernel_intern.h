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
/* The kernel's single I2C controller (i2c_hw.c over ../i2c/p4i2c_hw.c);
   results are P4_I2C_*. */
#include "../i2c/p4i2c_hw.h"

int krnP4I2CInit(unsigned int port, unsigned int sda_gpio,
                 unsigned int scl_gpio, unsigned long bus_hz);
int krnP4I2CTransfer(unsigned int address,
                     const unsigned char *wbuf, unsigned int wlen,
                     unsigned char *rbuf, unsigned int rlen);
int krnP4I2CProbe(unsigned int address);
void krnP4I2CLastStatus(unsigned long *raw, unsigned long *sr);
#define P4_I2C_MISMATCH     (-8)    /* the device kept something else */

struct KrnBacklightOps;
struct KrnBacklightOps *krnP4BacklightOps(void);

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
int krnP4PanelSupplyOn(struct P4PanelState *out);
int krnP4PanelResetPulse(struct P4PanelState *out);
int krnP4PanelPowerUp(struct P4PanelState *out);
int krnP4PanelSafe(void);
int krnP4PanelBacklightOn(void);
int krnP4PanelPowerOff(void);

struct P4BacklightState
{
    UWORD latch;            /* the expander latch this port believes in */
    UWORD expander_pins;    /* the expander's input port, i.e. the pins */
    unsigned long pin_level;/* GPIO14 as the input register reports it */
    unsigned long out_level;/* what the output register is driving */
    unsigned long samples;      /* how many times the pin was sampled */
    unsigned long samples_high; /* how many of those were high */
    unsigned long out_sel;  /* the matrix entry for GPIO14 */
    unsigned long iomux;    /* the pad's own configuration */
};

void krnP4PanelBacklightState(struct P4BacklightState *out);

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

/*
 * One entry of the panel's initialisation sequence.  A single parameter byte
 * covers every command the JD9365 sequence uses; a command needing more would
 * need this widened, and the table would say so.
 */
struct P4JD9365Cmd
{
    unsigned char cmd;
    unsigned char param;
    unsigned char param_bytes;
    unsigned short delay_ms;
};

extern const struct P4JD9365Cmd krnP4JD9365Init[];
extern const unsigned int krnP4JD9365InitCount;

/*
 * The JD9165 sequences (jd9165_init.c) send up to 14 parameter bytes in one
 * command, so their entry carries an array; otherwise the same encoding.
 */
#define P4_JD9165_MAX_PARAM     14

struct P4JD9165Cmd
{
    unsigned char cmd;
    unsigned char param[P4_JD9165_MAX_PARAM];
    unsigned char param_bytes;
    unsigned short delay_ms;
};

extern const struct P4JD9165Cmd krnP4JD9165InitOld[];
extern const unsigned int krnP4JD9165InitOldCount;
extern const struct P4JD9165Cmd krnP4JD9165InitNew[];
extern const unsigned int krnP4JD9165InitNewCount;

/* The JC1060P470C ships two panel batches; the board ID picks the table. */
#ifdef P4_JC1060_PANEL_V1
#define P4_JD9165_INIT          krnP4JD9165InitOld
#define P4_JD9165_INIT_COUNT    krnP4JD9165InitOldCount
#else
#define P4_JD9165_INIT          krnP4JD9165InitNew
#define P4_JD9165_INIT_COUNT    krnP4JD9165InitNewCount
#endif

/* DSI command-path results. */
#define P4_DSI_CMD_BUSY         (-3)
#define P4_DSI_CMD_NO_REPLY     (-4)

void krnP4DsiLdoUp(void);
int  krnP4DsiCmdModeUp(void);
int  krnP4DsiDcsWrite(unsigned char cmd, const unsigned char *param,
                      unsigned int param_bytes);
int  krnP4DsiDcsRead(unsigned char cmd, unsigned char *out,
                     unsigned int want);
int  krnP4DsiPanelInit(unsigned char *id, int *id_result);
int  krnP4DsiPanelOn(void);
/* What the pattern setup programmed, so the arithmetic can be checked. */
/*
 * What the scanout path is doing, for the report.  Every value read back from
 * hardware rather than remembered, because a configuration that did not take
 * is the failure this phase is most likely to hit.
 */
struct P4ScanoutState
{
    unsigned long chen;         /* channel enable, as the DMA reports it */
    unsigned long ch_cfg1;
    unsigned long ch_llp;
    unsigned long ch_sar;       /* moves while a transfer runs */
    unsigned long brg_flow;
    unsigned long brg_raw_num;
    unsigned long brg_misc;
    unsigned long brg_int;
    unsigned long words64;
    unsigned long fb_base;
    unsigned long brg_v_cfg0;
    unsigned long brg_v_cfg1;
    unsigned long brg_h_cfg0;
    unsigned long brg_h_cfg1;
    unsigned long brg_en;
    unsigned long brg_pixel;
    unsigned long ch_int0;      /* why the channel stopped, in its own words */
    unsigned long ch_int1;
    unsigned long brg_depth;    /* the bridge's fifo occupancy */
    unsigned long dma_frames;   /* complete frames re-armed by the ISR */
    unsigned long dma_faults;   /* non-completion status latched by the ISR */
    unsigned long active_fb;    /* descriptor source selected for next frame */
    unsigned long dma_swaps;    /* frame-boundary source changes */
    unsigned long pending_fb;   /* prepared source waiting for frame-done */
    unsigned long dirty_submits;/* bounded producer submissions */
    unsigned long dirty_rejects;/* producer ownership/bounds failures */
};

/*
 * The bridge's configuration this port has never written, read back once.
 *
 * Every one of these has a reset value that is not obviously right, and the
 * pixel format was exactly such a register: printed as a diagnostic for three
 * sessions while its zero meant RGB888.
 */
struct P4BridgeRest
{
    unsigned long credit_ctl;
    unsigned long block_intvl;
    unsigned long req_intvl;
    unsigned long lcd_ctl;
    unsigned long rsv_dpi_data;
    unsigned long int_ena;
    unsigned long blk_raw_num;
    unsigned long host_ctrl;
    unsigned long mem_clk_ctrl;
    unsigned long dma_req_cfg;
};
void krnP4ScanoutBridgeRest(struct P4BridgeRest *out);

/*
 * The bridge's fifo occupancy and raw interrupt, sampled rather than read
 * once.  A depth that changes says the bridge is working; a depth that stays
 * at zero with no underrun says it never started a frame.
 */
void krnP4ScanoutSample(unsigned long *depth, unsigned long *int_raw);

struct P4HostState
{
    unsigned long pwr_up;
    unsigned long mode_cfg;     /* bit 0 set means still command mode */
    unsigned long vid_mode;
    unsigned long lpclk;        /* bit 0 is TXREQUESTCLKHS */
    unsigned long phy_status;
    unsigned long pkt_size;
    unsigned long hsa;
    unsigned long hbp;
    unsigned long hline;
    unsigned long vactive;
    unsigned long colour;
    unsigned long dpi_clk;      /* PERI_CLK_CTRL03: source, divider and gate */
};

void krnP4HostState(struct P4HostState *out);
void krnP4ScanoutFill(unsigned long rgb);
void krnP4ScanoutCoordinatePattern(void);
#ifdef P4_B6_DOUBLE_BUFFER
void krnP4ScanoutB6Frames(void);
#ifdef P4_C1_FRAMEBUFFER_HIDD
struct KrnFrameBufferOps;
struct KrnFrameBufferOps *krnP4FrameBufferOps(void);
void krnP4ScanoutC1Clear(void);
#endif
#ifdef P4_B6_DIRTY_GATE
unsigned long krnP4ScanoutB6DirtyStep(unsigned long second);
#endif
#endif
void krnP4ScanoutPhaseCalibration(void);
void krnP4ScanoutTestCard(void);
void krnP4ScanoutCross(void);
void krnP4ScanoutGrid(void);
void krnP4ScanoutThreeLines(void);
void krnP4ScanoutHalves(void);
void krnP4ScanoutQuiesce(void);
void krnP4ScanoutBands(void);
unsigned long krnP4ScanoutCoherencyStep(unsigned long second);
void krnP4ScanoutBridgeUp(void);
void krnP4ScanoutDmaCreate(void);
void krnP4ScanoutDmaUp(void);
void krnP4ScanoutFeedOn(void);
void krnP4ScanoutState(struct P4ScanoutState *out);
void krnP4ScanoutDmaInterrupt(void);

struct P4DsiPattern
{
    unsigned long hsa, hbp, hfp, hact, hline;
    unsigned long htotal_px, vtotal_px;
    unsigned long frame_mhz;
    unsigned long brg_en;
};

int  krnP4DsiPatternOn(struct P4DsiPattern *out);
void krnP4DsiVideoOn(void);
void krnP4DsiPatternOff(void);

void krnP4DsiCmdStatus(unsigned long *pkt, unsigned long *int0,
                       unsigned long *int1);
unsigned long krnP4DsiVideoStatus(void);
int  krnP4DsiPhyUp(struct P4DsiState *out);

/*
 * PHY_STATUS wherever the link's direction can change.
 *
 * Bit 1 is phy_direction and it has been read set where a scanout was meant
 * to be transmitting, which stalls the video path: the host cannot send while
 * it is receiving, so the payload fifo overflows and DPI_PLD_WR_ERR stands.
 * One reading taken at the end cannot say which step turned the link around,
 * so each candidate step records one and the caller prints them together.
 */
#define P4_DSI_TRACE_MAX    8

extern unsigned long krnP4DsiPhyTrace[P4_DSI_TRACE_MAX];
extern unsigned int  krnP4DsiPhyTraceCount;
void krnP4DsiReferenceTransitionTrace(const char *stage);

void krnP4DsiPhyDown(void);
UWORD krnP4PanelStrayBits(void);
const char *krnP4I2CResultName(int result);

/* The SoC clock-tree root dividers, as read back from the registers. */
unsigned char krnP4SupplyLevel(void);
unsigned char krnP4SupplyUp(void);

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
#ifdef P4_BOOT_TIMING
enum P4BootPhase
{
    P4_BOOT_EARLY_OUTPUT, P4_BOOT_PSRAM, P4_BOOT_PANEL,
    P4_BOOT_PACKAGE, P4_BOOT_EXEC, P4_BOOT_UPDATE1,
    P4_BOOT_UPDATE128, P4_BOOT_UPDATE256, P4_BOOT_PHASES
};
void krnP4BootTimingStart(void);
void krnP4BootTimingMark(enum P4BootPhase phase);
void krnP4BootTimingUpdate(void);
void krnP4BootTimingReport(void);
#endif
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
void krnP4CacheWriteback(void);
void krnP4CacheWritebackData(void *addr, unsigned long len);
void krnP4CacheInvalidateData(void *addr, unsigned long len);
extern const struct KrnCacheOps __p4_cache_ops;
void krnP4CacheSyncData(void *addr, unsigned long len);
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
#include "../board/board.h"
#define P4_BSP_PART_TYPE    P4_BOARD_BSP_PART_TYPE
#define P4_BSP_PART_LABEL   P4_BOARD_BSP_PART_LABEL

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
#define P4_FLASHDISK_PART_OFFSET    P4_BOARD_FLASHDISK_PART_OFFSET
#define P4_FLASHDISK_SIZE           P4_BOARD_FLASHDISK_SIZE
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
int krnP4FlashCopy(unsigned long paddr, void *dest, unsigned long len);
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

/* The main watchdog and the reset cause (kernel_wdt.c) */
void krnWdtArm(void);
void krnWdtBeat(unsigned int hart);
void krnWdtCanaryBeat(unsigned int hart);
void krnWdtStuck(const char *what);
void krnWdtTick(void);
int  krnWdtArmed(void);
void krnWdtReport(void);
void krnWdtReportReset(void);
uint32_t krnResetCause(void);
const char *krnResetCauseName(uint32_t cause);
/* Stop this hart with interrupts masked, so that it feeds nothing */
void krnP4Halt(void) __attribute__((noreturn));

/* Early UART0 debug console (kernel_console.c) */
/* Switch to nonblocking, lossy output before runtime tasks can print. */
void krnP4ConsoleRuntime(void);
void krnP4ConsoleBlocking(void);

/*
 * The second hart (SMP.md, S3). Hart n raises its peer with
 * CPU_INTR_FROM_CPU_n, interrupt matrix source 79 + n; each hart's map
 * routes the source aimed at it to CLIC line 22.
 */
#define P4_SMP_IPI_LINE         22
#define P4_SMP_IPI_FROM(h)      (0x500e5010UL + (h) * 4)
#define P4_SMP_IPI_SOURCE(h)    (79 + (h))
#define P4_SMP_INTMTX_ROUTE(c, s) (0x500d6000UL + (c) * 0x800UL + (s) * 4)
#define P4_SMP_INTMTX_SOURCES   128

#define P4_IPI_PARK             (1UL << 0)  /* wait in SRAM: cache window */
#define P4_IPI_TICK             (1UL << 1)  /* hart 0's tick, forwarded */
#define P4_IPI_SCHEDULE         (1UL << 2)  /* look for something to run */
#define P4_IPI_CALL_HOOK        (1UL << 3)  /* run the queued hook calls */
#define P4_IPI_SOFTINT          (1UL << 4)  /* hart 0: soft interrupts are pending */

void krnP4SMPInitPrimary(void);
void krnP4IPIInterrupt(void);
void krnP4IPISend(unsigned int hart, ULONG work);
void krnP4TickOthers(void);
int krnP4ParkOthers(void);
void krnP4UnparkOthers(void);
void krnP4ParkServe(void);
unsigned long krnP4LockTake(volatile ULONG *lock);
void krnP4LockRelease(volatile ULONG *lock, unsigned long state);
extern volatile ULONG __p4_parks;
void krnP4SpinReport(const char *what, void *lock, void *caller);
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
