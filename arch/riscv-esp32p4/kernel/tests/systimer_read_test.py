#!/usr/bin/env python3
"""Check the SYSTIMER snapshot read of the package modules against an
interfering second reader.

The snapshot registers are one set for both harts: a request by another
hart between this one's request and its reads replaces both halves. The
modules (i2c/p4i2c_time.c, sdcard/sdcard_esp32p4_time.c) read the high
half a second time and ask again if the two differ. This compiles the
actual p4i2c_now() against mock registers, lets "another hart" take a
snapshot after each of the register accesses in turn, with the counter
carrying out of its low 32 bits in between, and checks that the result is
never a pair of different moments.
"""
import pathlib
import re
import subprocess
import tempfile

port = pathlib.Path(__file__).resolve().parents[2]
source = (port / "i2c" / "p4i2c_time.c").read_text()
start = source.index("uint64_t p4i2c_now(void)")
func = source[start:source.index("\n}\n", start) + 3]
# The fence is RISC-V only; nothing here depends on it on the host
func = func.replace('__asm__ volatile("fence iorw, iorw" ::: "memory");', "")

fixture = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

typedef uint32_t ULONG;
#define P4_SYSTIMER_BASE 0x1000UL
#define P4_ST_UNIT0_OP 0x00
#define P4_ST_UNIT0_UPDATE (1U << 30)
#define P4_ST_UNIT0_VALID (1U << 29)
#define P4_ST_UNIT0_VALUE_HI 0x04
#define P4_ST_UNIT0_VALUE_LO 0x08

static uint64_t counter;            /* the free running counter */
static uint32_t snap_hi, snap_lo;   /* the one shared snapshot */
static int accesses;                /* register accesses so far */
static int interfere_at;            /* after this access the other hart snaps */
static uint64_t first_update, last_access_counter;
static uint64_t latched[8];         /* every pair a snapshot produced */
static int nlatched;
static int updates_by_us;

static void other_hart_snapshot(void)
{
    /* Time passes, across a 32-bit boundary, then the other hart latches */
    counter = (counter | 0xffffffffULL) + 5;
    snap_hi = (uint32_t)(counter >> 32);
    snap_lo = (uint32_t)counter;
    latched[nlatched++] = counter;
}

static void after_access(void)
{
    accesses++;
    counter += 3;
    if (accesses == interfere_at)
        other_hart_snapshot();
    last_access_counter = counter;
}

static void p4_w32(unsigned long addr, uint32_t value)
{
    assert(addr == P4_SYSTIMER_BASE + P4_ST_UNIT0_OP);
    assert(value == P4_ST_UNIT0_UPDATE);
    if (!updates_by_us++)
        first_update = counter;
    snap_hi = (uint32_t)(counter >> 32);
    snap_lo = (uint32_t)counter;
    latched[nlatched++] = counter;
    after_access();
}

static uint32_t p4_r32(unsigned long addr)
{
    uint32_t v = 0;

    if (addr == P4_SYSTIMER_BASE + P4_ST_UNIT0_OP)
        v = P4_ST_UNIT0_VALID;
    else if (addr == P4_SYSTIMER_BASE + P4_ST_UNIT0_VALUE_HI)
        v = snap_hi;
    else if (addr == P4_SYSTIMER_BASE + P4_ST_UNIT0_VALUE_LO)
        v = snap_lo;
    else
        assert(0);
    after_access();
    return v;
}

''' + func + r'''

static void run(int at, uint64_t start)
{
    uint64_t got;
    int i, found = 0;

    counter = start;
    accesses = 0;
    updates_by_us = 0;
    nlatched = 0;
    interfere_at = at;
    got = p4i2c_now();

    /* Never earlier than our first snapshot, never later than the last
       moment the counter was seen, and one of the values a snapshot
       latched: a pair from two different moments is none of them. */
    assert(got >= first_update);
    assert(got <= last_access_counter);
    for (i = 0; i < nlatched; i++)
        if (latched[i] == got)
            found = 1;
    assert(found);
}

int main(void)
{
    int at;
    uint64_t base[] = { 0x0000000000001000ULL,
                        0x00000000fffffff0ULL,
                        0x0000000100000000ULL - 4,
                        0x00000002ffffffffULL - 7 };
    unsigned i;

    for (i = 0; i < sizeof(base) / sizeof(base[0]); i++)
        for (at = 0; at <= 12; at++)
            run(at, base[i]);

    puts("systimer read test passed");
    return 0;
}
'''

# The fixture's mocks use the names the module's code calls
func_check = re.search(r"p4_w32|p4_r32", func)
assert func_check

with tempfile.TemporaryDirectory() as tmp:
    tmp = pathlib.Path(tmp)
    (tmp / "fixture.c").write_text(fixture)
    exe = tmp / "systimer_read_test"
    subprocess.run(
        ["cc", "-std=gnu11", "-Wall", "-Werror", "-Wno-unused-function",
         "-o", str(exe), str(tmp / "fixture.c")], check=True)
    subprocess.run([str(exe)], check=True)
