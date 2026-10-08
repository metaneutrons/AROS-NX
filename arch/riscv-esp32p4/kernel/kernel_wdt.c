/*
    Copyright (c) 2026, The AROS Development Team. All rights reserved.

    Desc: The hardware watchdog of the esp32p4-riscv target, and the reason
          the last reset happened.

    Timer group 0's main watchdog is armed once the tick runs and fed from
    that tick, but only while every hart that is online shows signs of
    life. Hart 0's own tick is its sign. Hart 1 has no timer of its own:
    it counts the tick that hart 0 forwards to it as an inter-hart
    interrupt, so a hart 1 that has hung with interrupts masked, or has
    stopped after a fatal trap, stops counting, hart 0 stops feeding, and
    the watchdog resets the board a timeout later. A hart 0 that hangs
    stops feeding by itself.

    A hart that still takes interrupts while its tasks make no progress, a
    task stuck in Forbid() or something above the scheduler's reach, is
    caught by a second sign: a canary task per hart (kernel_wdtcanary.c)
    waits on timer.device and reports each time it wakes. The tick cannot
    tell that state from work; the canary can, because it runs above
    every ordinary task and cannot run at all under Forbid().

    The stage action is "reset system": the ROM runs again and prints the
    cause in its banner, 0x07, HP_SYS_HP_WDT_RESET. Every place the kernel
    stops on purpose, the first alert and a fatal trap, masks interrupts
    first (krnP4Halt()), so that stopping ends in a reset instead of a
    machine that stays mute.
*/

#include <inttypes.h>

#include "hardware.h"
#include "kernel_intern.h"
#include "tls.h"

/*
 * The timeout, in milliseconds, assuming the counter ticks once per
 * millisecond: the prescaler divides the 40 MHz crystal by 40000. The
 * register header documents a 12.5 ns source period instead, which would
 * halve it; the measured value is in ROADMAP.md.
 */
#ifndef P4_WDT_TIMEOUT_MS
#define P4_WDT_TIMEOUT_MS       6000
#endif
#define P4_WDT_PRESCALE         40000

/* Feed decisions are taken every this many ticks: 100 ms */
#define P4_WDT_CHECK_TICKS      10
/* A silent hart is reported after this many checks: 1 s */
#define P4_WDT_SILENT_WARN      10
/* A canary that has not woken for this many checks stops the feeding: 3 s.
   It wakes every 500 ms, so six of its periods have to be missed. */
#define P4_WDT_CANARY_CHECKS    30

#ifndef P4_NO_WATCHDOG

static inline void wdt_wr(uint32_t off, uint32_t val)
{
    *(volatile uint32_t *)(P4_TIMG0_BASE + off) = val;
}

/* Each hart's count of its own ticks, written by that hart only */
static volatile uint32_t beat[P4_TLS_HARTS];
/* What hart 0 saw of them at the last check, and for how many checks no change */
static uint32_t seen[P4_TLS_HARTS];
static uint32_t silent[P4_TLS_HARTS];

/* The canary tasks' counts, written by the task, and the same bookkeeping.
   A canary counts only once it has woken for the first time. */
static volatile uint32_t canary_beat[P4_TLS_HARTS];
static volatile uint32_t canary_live[P4_TLS_HARTS];
static uint32_t canary_seen[P4_TLS_HARTS];
static uint32_t canary_silent[P4_TLS_HARTS];

static uint32_t armed;
static uint32_t until_check;
static uint64_t last_tick;
static uint64_t max_gap;        /* SYSTIMER ticks between two of hart 0's ticks */
static uint32_t feeds;

void krnWdtArm(void)
{
    uint32_t cfg = P4_TIMG_WDT_STG0(P4_TIMG_WDT_ACT_RESET_SYSTEM) |
                   P4_TIMG_WDT_SYS_RST_LEN(7) | P4_TIMG_WDT_CPU_RST_LEN(7);

    /* The same order ESP-IDF's wdt_hal_init() and wdt_hal_enable() use */
    wdt_wr(P4_TIMG_WDTWPROTECT, P4_WDT_WKEY);
    wdt_wr(P4_TIMG_WDTCONFIG0, 0);
    wdt_wr(P4_TIMG_WDTCONFIG1, (uint32_t)P4_WDT_PRESCALE << 16);
    wdt_wr(P4_TIMG_WDTCONFIG2, P4_WDT_TIMEOUT_MS);
    wdt_wr(P4_TIMG_WDTCONFIG0, cfg | P4_TIMG_WDT_CONF_UPDATE);
    wdt_wr(P4_TIMG_WDTFEED, 1);
    wdt_wr(P4_TIMG_WDTCONFIG0, cfg | P4_TIMG_WDT_CONF_UPDATE | P4_TIMG_WDT_EN);
    wdt_wr(P4_TIMG_WDTWPROTECT, 0);

    last_tick = krnTimerCount();
    until_check = P4_WDT_CHECK_TICKS;
    armed = 1;

    krnP4PutStr("[kernel] wdt    main watchdog armed, timer group 0, ");
    krnP4PutDec(P4_WDT_TIMEOUT_MS);
    krnP4PutStr(" ms\n");
}

static void wdt_feed(void)
{
    wdt_wr(P4_TIMG_WDTWPROTECT, P4_WDT_WKEY);
    wdt_wr(P4_TIMG_WDTFEED, 1);
    wdt_wr(P4_TIMG_WDTWPROTECT, 0);
    feeds++;
}

/* Hart 1's sign of life, from its handling of the forwarded tick */
void krnWdtBeat(unsigned int hart)
{
    beat[hart]++;
}

/* A canary task's sign of life: it woke from its timer request */
void krnWdtCanaryBeat(unsigned int hart)
{
    canary_beat[hart]++;
    canary_live[hart] = 1;
}

/*
 * Hart 0's tick, with interrupts masked. Counts its own beat, and every
 * P4_WDT_CHECK_TICKS decides whether the others have shown theirs.
 */
void krnWdtTick(void)
{
    ULONG online;
    unsigned int hart;
    int alive = 1;
    uint64_t now;

    if (!armed)
        return;

    beat[0]++;

    now = krnTimerCount();
    if (now - last_tick > max_gap)
        max_gap = now - last_tick;
    last_tick = now;

    if (--until_check)
        return;
    until_check = P4_WDT_CHECK_TICKS;

    online = __atomic_load_n(&__p4_harts_online, __ATOMIC_ACQUIRE);
    for (hart = 0; hart < P4_TLS_HARTS; hart++)
    {
        uint32_t b;

        if (!(online & (1UL << hart)))
            continue;

        /* Hart 0's own tick is this very call; the others count the
           forwarded one */
        if (hart != 0)
        {
            b = beat[hart];
            if (b != seen[hart])
            {
                seen[hart] = b;
                silent[hart] = 0;
            }
            else
            {
                alive = 0;
                if (++silent[hart] == P4_WDT_SILENT_WARN)
                {
                    krnP4PutStr("[wdt] hart ");
                    krnP4PutDec(hart);
                    krnP4PutStr(" has taken no tick for 1 s; "
                                "the watchdog resets the board\n");
                }
            }
        }

        if (canary_live[hart])
        {
            b = canary_beat[hart];
            if (b != canary_seen[hart])
            {
                canary_seen[hart] = b;
                canary_silent[hart] = 0;
            }
            else if (++canary_silent[hart] >= P4_WDT_CANARY_CHECKS)
            {
                alive = 0;
                if (canary_silent[hart] == P4_WDT_CANARY_CHECKS)
                {
                    krnP4PutStr("[wdt] hart ");
                    krnP4PutDec(hart);
                    krnP4PutStr(" runs no task for 3 s; "
                                "the watchdog resets the board\n");
                }
            }
        }
    }

    if (alive)
        wdt_feed();
}

int krnWdtArmed(void)
{
    return armed != 0;
}

/* For the bring-up report */
void krnWdtReport(void)
{
    krnP4PutStr(armed ? "main watchdog armed, " : "main watchdog NOT armed, ");
    krnP4PutDec(P4_WDT_TIMEOUT_MS);
    krnP4PutStr(" ms, fed ");
    krnP4PutDec(feeds);
    krnP4PutStr(" times, longest gap between ticks ");
    krnP4PutDec((uint32_t)(max_gap / (P4_SYSTIMER_HZ / 10000)));
    krnP4PutStr(" x 0.1 ms\n");
}

#else /* P4_NO_WATCHDOG */

void krnWdtArm(void) {}
void krnWdtBeat(unsigned int hart) { (void)hart; }
void krnWdtCanaryBeat(unsigned int hart) { (void)hart; }
void krnWdtTick(void) {}
int krnWdtArmed(void) { return 0; }
void krnWdtReport(void)
{
    krnP4PutStr("main watchdog not built (P4_NO_WATCHDOG)\n");
}

#endif /* P4_NO_WATCHDOG */

/*
 * Stop this hart for good. Interrupts go first, so that from here on the
 * hart takes no tick and feeds nothing, which is what turns a stop into a
 * reset. wfi alone would not: an interrupt that arrives wakes it, and a
 * hart that keeps taking its tick keeps the watchdog fed.
 */
void krnP4Halt(void)
{
    asm volatile("csrci mstatus, 8" ::: "memory");
    if (krnWdtArmed())
        krnP4PutStr("[wdt] this hart is stopped and feeds nothing; "
                    "the board resets when the watchdog runs out\n");
    for (;;)
        asm volatile("wfi");
}

/*
 * Why the board last reset, as the ROM reports it (rtc_get_reset_reason(),
 * the number in its banner). The names are ESP-IDF's, from the ROM's
 * rtc.h and soc/reset_reasons.h.
 */
uint32_t krnResetCause(void)
{
    return ((uint32_t (*)(uint32_t))P4_ROM_GET_RESET_REASON)(0) & 0x3f;
}

const char *krnResetCauseName(uint32_t cause)
{
    switch (cause)
    {
        case 0x01: return "POWERON_RESET";
        case 0x03: return "SW_SYS_RESET";
        case 0x05: return "PMU_SYS_PWR_DOWN_RESET";
        case 0x07: return "HP_SYS_HP_WDT_RESET";
        case 0x09: return "HP_SYS_LP_WDT_RESET";
        case 0x0B: return "HP_CORE_HP_WDT_RESET";
        case 0x0C: return "SW_CPU_RESET";
        case 0x0D: return "HP_CORE_LP_WDT_RESET";
        case 0x0F: return "BROWN_OUT_RESET";
        case 0x10: return "CHIP_LP_WDT_RESET";
        case 0x12: return "SUPER_WDT_RESET";
        case 0x13: return "GLITCH_RTC_RESET";
        case 0x14: return "EFUSE_CRC_ERR_RESET";
        case 0x16: return "CHIP_USB_JTAG_RESET";
        case 0x17: return "CHIP_USB_UART_RESET";
        case 0x18: return "JTAG_RESET";
        case 0x1A: return "CPU_LOCKUP_RESET";
        default:   return "unknown";
    }
}

void krnWdtReportReset(void)
{
    uint32_t cause = krnResetCause();

    krnP4PutStr("[kernel] reset  last reset 0x");
    krnP4PutC("0123456789abcdef"[(cause >> 4) & 15]);
    krnP4PutC("0123456789abcdef"[cause & 15]);
    krnP4PutStr(" ");
    krnP4PutStr(krnResetCauseName(cause));
    krnP4PutStr("\n");
}
