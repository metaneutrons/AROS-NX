#!/usr/bin/env python3
"""Compile the actual C1 producer functions against a host DMA/IRQ fixture.

No model copy of the producer: extract it mechanically from dsi_scanout.c.
The fixture injects a frame IRQ at every producer fence and cache publication.
"""
import pathlib
import subprocess
import tempfile

kernel = pathlib.Path(__file__).resolve().parents[1]
source = (kernel / "dsi_scanout.c").read_text()
dirty = source[source.index("#ifdef P4_C1_COALESCE\nstatic unsigned long c1_dirty_surface;"):]
dirty = dirty[:dirty.index("#ifdef P4_C1_FULL_DIAGNOSTICS")]
producer = source[source.index("static void c1_copy_rect("):source.index("static VOID c1_get_stats(")]
producer = producer.replace('asm volatile("fence rw, rw" ::: "memory");', "test_barrier();")
producer = producer.replace('asm volatile("nop");', "test_irq();")
fixture = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "framebuffer_rotate.h"
typedef int BOOL;
typedef unsigned long ULONG;
typedef long LONG;
typedef const void *CONST_APTR;
#define VOID void
#define TRUE 1
#define FALSE 0
#define P4_C1_COALESCE 1
#define P4_PANEL_V_RES 64UL
#define P4_PANEL_H_RES 40UL
#define P4_SCANOUT_ROW_PHASE_WORKAROUND 25UL
#define P4_FB_BYTES (64UL * 40 * 2)
static uint16_t front[64 * 40], back[64 * 40], wanted[64 * 40];
static unsigned char logical[64 * 40 * 2];
#define P4_FB_BASE ((unsigned long)front)
#define P4_FB_BACK_BASE ((unsigned long)back)
static volatile unsigned long scanout_active_fb, scanout_pending_fb;
static volatile unsigned long scanout_dirty_submits, scanout_dirty_rejects;
static volatile unsigned long scanout_copy_busy;
static unsigned long skipped, swapped;
static void test_irq(void)
{
    if (scanout_copy_busy) { skipped++; return; }
    if (scanout_pending_fb)
    {
        assert(scanout_pending_fb != scanout_active_fb);
        scanout_active_fb = scanout_pending_fb;
        scanout_pending_fb = 0;
        swapped++;
        assert(memcmp((void *)scanout_active_fb, wanted, sizeof(wanted)) == 0);
    }
}
static void test_barrier(void) { test_irq(); }
static int b6_dirty_writeback(unsigned long fb, unsigned long x,
    unsigned long y, unsigned long w, unsigned long h)
{
    assert(fb != scanout_active_fb);
    assert(x + w <= 64 && y + h <= 40);
    test_irq();
    return 1;
}
static void krnP4CacheWritebackData(void *fb, unsigned long n)
{
    assert((unsigned long)fb != scanout_active_fb && n == P4_FB_BYTES);
    test_irq();
}
'''
main = r'''
int main(void)
{
    unsigned long i, j;
    scanout_active_fb = P4_FB_BASE;
    /* Multiple submissions before a frame boundary, then stale-box sync.
     * Include full replacement, overlapping/separated boxes and phase wrap. */
    for (i = 0; i < 400; i++)
    {
        unsigned long x = (i * 17) % 64, y = (i * 13) % 40;
        unsigned long w = 1 + (i * 7) % (64 - x);
        unsigned long h = 1 + (i * 11) % (40 - y);
        uint16_t active_snapshot[64 * 40];
        if ((i % 37) == 0) { x = y = 0; w = 64; h = 40; }
        memcpy(active_snapshot, (void *)scanout_active_fb, sizeof(active_snapshot));
        for (j = 0; j < w * h; j++)
        {
            unsigned long p = (y + j / w) * 64 + x + j % w;
            logical[p * 2] = (unsigned char)(i + 1);
            logical[p * 2 + 1] = (unsigned char)(i / 3);
        }
        p4_rotate_rect(wanted, logical, 128, 64, 40, 25, x, y, w, h);
        assert(c1_update_rect(logical, 128, x, y, w, h));
        assert(scanout_copy_busy == 0 && scanout_dirty_rejects == 0);
        assert(memcmp(active_snapshot, (void *)scanout_active_fb, sizeof(active_snapshot)) == 0);
        if ((i % 5) == 4) test_irq();
    }
    test_irq();
    assert(c1_flush());
    assert(!c1_update_rect(logical, 128, -1, 0, 1, 1));
    assert(!c1_update_rect(logical, 128, 63, 39, 2, 2));
    assert(!scanout_copy_busy);
    for (i = 0; i < 64 * 40; i++) wanted[i] = 0x3344;
    assert(c1_clear(0x3344));
    assert(memcmp(front, wanted, sizeof(wanted)) == 0);
    assert(memcmp(back, wanted, sizeof(wanted)) == 0);
    assert(!scanout_pending_fb && !c1_dirty_surface);
    printf("400 actual producer submissions passed; %lu swaps, %lu guarded IRQs; bounds rejected\n", swapped, skipped);
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="p4-coalesce-test-") as temporary:
    binary = str(pathlib.Path(temporary) / "test")
    subprocess.run(["clang", "-std=gnu99", "-O2", "-Wall", "-Wextra",
                    "-Wno-unused-function", "-fsanitize=address,undefined",
                    "-I", str(kernel), "-x", "c", "-", "-o", binary],
                   input=fixture + dirty + producer + main, text=True, check=True)
    subprocess.run([binary], check=True)
