#!/usr/bin/env python3
"""Compile the actual kernel_wdt.c against mock headers and registers and
check when its tick feeds the watchdog and when it does not.

Everything above krnP4Halt() is compiled: arming, the heartbeats and the
feeding decision. krnP4Halt() and what follows it (the reset-cause helpers)
need RISC-V assembly or the ROM and are not part of the decision.
"""
import pathlib
import subprocess
import tempfile

kernel = pathlib.Path(__file__).resolve().parents[1]
source = (kernel / "kernel_wdt.c").read_text()
cut = source.index("/*\n * Stop this hart for good.")
wdt = source[:cut]

headers = {
    "hardware.h": r'''
#include <stdint.h>
extern uint32_t mock_regs[64];
#define P4_TIMG0_BASE           ((uintptr_t)mock_regs)
#define P4_TIMG_WDTCONFIG0      0x0048
#define  P4_TIMG_WDT_EN         (1U << 31)
#define  P4_TIMG_WDT_FLASHBOOT  (1U << 14)
#define P4_TIMG_WDTWPROTECT     0x0064
#define  P4_TIMG_WDT_CONF_UPDATE (1U << 22)
#define  P4_TIMG_WDT_STG0(a)    ((uint32_t)(a) << 29)
#define  P4_TIMG_WDT_SYS_RST_LEN(n) ((uint32_t)(n) << 15)
#define  P4_TIMG_WDT_CPU_RST_LEN(n) ((uint32_t)(n) << 18)
#define  P4_TIMG_WDT_ACT_RESET_SYSTEM 3
#define P4_TIMG_WDTCONFIG1      0x004C
#define P4_TIMG_WDTCONFIG2      0x0050
#define P4_TIMG_WDTFEED         0x0060
#define P4_WDT_WKEY             0x50D83AA1UL
#define P4_SYSTIMER_HZ          16000000UL
''',
    "tls.h": r'''
#include <stdint.h>
typedef uint32_t ULONG;
#define P4_TLS_HARTS 2
extern ULONG __p4_harts_online;
''',
    "kernel_intern.h": r'''
#include <stdint.h>
uint64_t krnTimerCount(void);
void krnP4PutStr(const char *s);
void krnP4PutDec(uint32_t v);
''',
}

fixture = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

uint32_t mock_regs[64];
uint32_t __p4_harts_online = 1;
static uint64_t now;
static unsigned warnings;
static char last_msg[256];

uint64_t krnTimerCount(void) { return now; }
void krnP4PutStr(const char *s)
{
    if (strstr(s, "[wdt]")) { warnings++; }
    strncpy(last_msg, s, sizeof(last_msg) - 1);
}
void krnP4PutDec(uint32_t v) { (void)v; }

#include "kernel_wdt_under_test.c"

static void reset_state(void)
{
    memset(mock_regs, 0, sizeof(mock_regs));
    memset((void *)beat, 0, sizeof(beat));
    memset((void *)canary_beat, 0, sizeof(canary_beat));
    memset((void *)canary_live, 0, sizeof(canary_live));
    memset(seen, 0, sizeof(seen));
    memset(silent, 0, sizeof(silent));
    memset(canary_seen, 0, sizeof(canary_seen));
    memset(canary_silent, 0, sizeof(canary_silent));
    armed = 0;
    stuck = 0;
    feeds = 0;
    max_gap = 0;
    warnings = 0;
    __p4_harts_online = 1;
    now = 0;
}

/* One 10 ms tick of hart 0, and hart 1's if it takes its own */
static void tick(int hart1_beats)
{
    now += 160000;
    if (hart1_beats)
        krnWdtBeat(1);
    krnWdtTick();
}

static void ticks(unsigned n, int hart1_beats)
{
    while (n--)
        tick(hart1_beats);
}

int main(void)
{
    unsigned before;

    /* Not armed: a tick does nothing */
    reset_state();
    ticks(100, 1);
    assert(feeds == 0);

    /* Arming programs stage 0 as reset-system and enables the watchdog */
    reset_state();
    krnWdtArm();
    assert(armed);
    assert(mock_regs[0x4C / 4] == (uint32_t)P4_WDT_PRESCALE << 16);
    assert(mock_regs[0x50 / 4] == P4_WDT_TIMEOUT_MS);
    assert(mock_regs[0x48 / 4] & P4_TIMG_WDT_EN);
    assert(((mock_regs[0x48 / 4] >> 29) & 3) == P4_TIMG_WDT_ACT_RESET_SYSTEM);
    assert(!(mock_regs[0x48 / 4] & P4_TIMG_WDT_FLASHBOOT));
    assert(mock_regs[0x64 / 4] == 0);       /* locked again */

    /* Hart 0 alone feeds every P4_WDT_CHECK_TICKS ticks */
    reset_state();
    krnWdtArm();
    ticks(100, 0);
    assert(feeds == 100 / P4_WDT_CHECK_TICKS);
    assert(warnings == 0);

    /* Hart 1 online and counting: the same */
    reset_state();
    krnWdtArm();
    __p4_harts_online = 3;
    ticks(100, 1);
    assert(feeds == 100 / P4_WDT_CHECK_TICKS);
    assert(warnings == 0);

    /* Hart 1 online but silent: no feed, one warning after a second */
    reset_state();
    krnWdtArm();
    __p4_harts_online = 3;
    ticks(10, 1);
    before = feeds;
    ticks(300, 0);
    assert(feeds == before);
    assert(warnings == 1);

    /* ... and it feeds again as soon as hart 1 counts again */
    ticks(10, 1);
    assert(feeds == before + 1);

    /* A hart that is not online is not waited for */
    reset_state();
    krnWdtArm();
    __p4_harts_online = 1;
    ticks(50, 0);
    assert(feeds == 5);

    /* A canary that never woke is ignored; one that woke and then stops
       stops the feeding after P4_WDT_CANARY_CHECKS checks, not before */
    reset_state();
    krnWdtArm();
    ticks(100, 0);
    assert(feeds == 10 && warnings == 0);
    krnWdtCanaryBeat(0);
    ticks(10, 0);                                   /* sees the first beat */
    assert(feeds == 11);
    before = feeds;
    ticks((P4_WDT_CANARY_CHECKS - 1) * P4_WDT_CHECK_TICKS, 0);
    assert(feeds == before + P4_WDT_CANARY_CHECKS - 1);
    assert(warnings == 0);
    ticks(5 * P4_WDT_CHECK_TICKS, 0);
    assert(feeds == before + P4_WDT_CANARY_CHECKS - 1);     /* starved */
    assert(warnings == 1);
    before = feeds;
    krnWdtCanaryBeat(0);
    ticks(10, 0);
    assert(feeds == before + 1);                    /* and fed again */

    /* The canary of hart 1 counts only while hart 1 is online */
    reset_state();
    krnWdtArm();
    krnWdtCanaryBeat(1);
    ticks(30 * P4_WDT_CHECK_TICKS + 20, 0);
    assert(feeds == (30 * P4_WDT_CHECK_TICKS + 20) / P4_WDT_CHECK_TICKS);

    /* Something stuck stops the feeding for good, with one warning */
    reset_state();
    krnWdtArm();
    ticks(20, 0);
    before = feeds;
    krnWdtStuck("a test lock");
    krnWdtStuck("a test lock");
    ticks(100, 0);
    assert(feeds == before);
    assert(warnings == 1);

    /* The longest gap between ticks is kept */
    reset_state();
    krnWdtArm();
    ticks(5, 0);
    now += 16000000UL / 2;                          /* half a second */
    krnWdtTick();
    assert(max_gap >= 8000000UL);

    puts("wdt feed test passed");
    return 0;
}
'''

with tempfile.TemporaryDirectory() as tmp:
    tmp = pathlib.Path(tmp)
    for name, text in headers.items():
        (tmp / name).write_text(text)
    (tmp / "kernel_wdt_under_test.c").write_text(wdt)
    (tmp / "fixture.c").write_text(fixture)
    exe = tmp / "wdt_feed_test"
    subprocess.run(
        ["cc", "-std=gnu11", "-Wall", "-Werror", "-Wno-unused-function",
         "-I", str(tmp), "-o", str(exe), str(tmp / "fixture.c")],
        check=True)
    subprocess.run([str(exe)], check=True)
