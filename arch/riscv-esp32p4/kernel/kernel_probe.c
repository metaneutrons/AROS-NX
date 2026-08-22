/*
    Copyright (c) 2026, The AROS Development Team. All rights reserved.

    Desc: Two questions about this silicon that the ELF loader's shape
          depends on, answered by measurement rather than by reading.

    THE FIRST: does the core execute a misaligned 32-bit access, or trap?

    It decides whether the module loader needs alignment handling at all.
    AROS module objects are relocatable ELF, and the assembler, when it
    emits `.align` in a section the linker may relax, writes the maximum
    padding it might need and leaves an R_RISCV_ALIGN relocation saying how
    much of it may be deleted. The final link relaxes and the padding goes
    away; a loader that only honours sh_addralign does not, so everything
    after the first such point sits two bytes off. In a real module built
    for this target - utility.library, measured - .text has addralign 4,
    there are two R_RISCV_ALIGN relocations with addend 2, and
    Utility_ROMTag lands at offset 0x1BF6, which is 2 mod 4. The romtag's
    rt_MatchTag field is four bytes into a struct at that address, so both
    the relocator writing it and the resident scanner reading it would be
    doing a misaligned 32-bit access.

    Nothing in ESP-IDF states whether the HP core handles that in hardware.
    The indirect evidence leans yes: the ROM ships a linker script named
    esp32p4.rom.libc-suboptimal_for_misaligned_mem.ld, which describes ROM
    string routines as badly optimised for misaligned memory rather than
    incapable of it. But that is an inference about memcpy, not about the
    core's own lw and sw, and the toolchain is no help either: this port's
    gcc has -mstrict-align enabled by default, so it byte-assembles every
    packed access and never emits a misaligned load on its own. The only
    misaligned accesses in the system would be the deliberate ones.

    So: do one of each, in internal SRAM and in PSRAM, with the trap
    handler told to step over a misaligned-access fault instead of halting.

    THE SECOND: can code be executed out of the PSRAM window this port
    mapped itself?

    ESP-IDF says the silicon can: SOC_SPIRAM_XIP_SUPPORTED is 1,
    SOC_IRAM_PSRAM_ADDRESS_LOW is 0x48000000, CONFIG_SPIRAM_FETCH_INSTRUCTIONS
    links a whole application's .text at 0x48000020, and the bootloader's
    locked PMA entry 13 grants RWX over the whole 0x40000000 span. What that
    does not cover is this port's own MMU programming and cache state, which
    are not IDF's. Writing eight bytes of instruction through the data path,
    making them coherent, and calling them settles it.

    Both probes are behind -DP4_PROBE and off by default. They belong to
    bring-up, not to booting.
*/

#include <inttypes.h>

#include <asm/cpu.h>

#include "hardware.h"
#include "kernel_intern.h"

#ifdef P4_PROBE

/*
 * Three instruction words, verified against the assembler:
 *
 *      a5a5f537    lui  a0, 0xa5a5f
 *      00d50513    addi a0, a0, 13
 *      00008067    ret
 *
 * Written as words rather than compiled, because copying a compiled
 * function means guessing its length and hoping it has no relative branch
 * or literal of its own. A leaf returning a constant is short enough to
 * state outright.
 */
#define P4_PROBE_STUB_MAGIC     0xA5A5F00DUL

static const uint32_t probe_stub[3] =
{
    0xA5A5F537UL, 0x00D50513UL, 0x00008067UL
};

typedef unsigned long (*probe_fn_t)(void);

/*
 * One misaligned store and one misaligned load at the same address.
 *
 * The buffer is eight bytes so that base+2 and its four bytes are inside
 * it whatever the alignment of the buffer itself. Interrupts are the
 * caller's business; the trap tolerance is set and cleared here so a
 * genuine fault from anywhere else still halts.
 */
static int probe_misaligned(void *buf, const char *where)
{
    volatile unsigned char *p = (volatile unsigned char *)buf;
    volatile uint32_t *w = (volatile uint32_t *)(p + 2);
    unsigned long before;
    uint32_t got;
    int ok;

    p[0] = p[1] = p[6] = p[7] = 0;

    before = __esp32p4_trap_caught;

    __esp32p4_trap_addr = (unsigned long)w;
    __esp32p4_trap_expect = CAUSE_MISALIGNED_STORE;
    *w = 0x11223344UL;
    __esp32p4_trap_expect = 0;
    __esp32p4_trap_addr = 0;

    if (__esp32p4_trap_caught != before)
    {
        krnP4PutStr("[probe] align  ");
        krnP4PutStr(where);
        krnP4PutStr(" store trapped\n");
        return 0;
    }

    __esp32p4_trap_addr = (unsigned long)w;
    __esp32p4_trap_expect = CAUSE_MISALIGNED_LOAD;
    got = *w;
    __esp32p4_trap_expect = 0;
    __esp32p4_trap_addr = 0;

    if (__esp32p4_trap_caught != before)
    {
        krnP4PutStr("[probe] align  ");
        krnP4PutStr(where);
        krnP4PutStr(" load trapped\n");
        return 0;
    }

    ok = (got == 0x11223344UL);

    krnP4PutStr("[probe] align  ");
    krnP4PutStr(where);
    krnP4PutStr(ok ? " 32-bit access at 2 mod 4 round-tripped\n"
                   : " 32-bit access at 2 mod 4 returned the wrong value\n");

    /*
     * A wrong value without a trap would be the worst of the three
     * outcomes, so say what came back rather than only that it was wrong.
     */
    if (!ok)
    {
        krnP4PutStr("[probe]        wrote 0x11223344, read ");
        krnP4PutHex32(got);
        krnP4PutStr("\n");
    }

    return ok;
}

/*
 * Write the stub into PSRAM through the data path, make it coherent, call
 * it. A trap here is not tolerated: an instruction fetch fault at an
 * address this port mapped itself is the answer, and it should be reported
 * with a backtrace rather than skipped.
 */
static int probe_psram_exec(void *at)
{
    volatile uint32_t *dst = (volatile uint32_t *)at;
    probe_fn_t fn = (probe_fn_t)at;
    unsigned long got;
    int i;

    for (i = 0; i < 3; i++)
        dst[i] = probe_stub[i];

    krnP4SyncCode(at, sizeof(probe_stub));

    got = fn();

    krnP4PutStr("[probe] exec   psram stub at ");
    krnP4PutHex32((uint32_t)(unsigned long)at);
    krnP4PutStr(got == P4_PROBE_STUB_MAGIC ? " returned its magic\n"
                                           : " returned ");
    if (got != P4_PROBE_STUB_MAGIC)
    {
        krnP4PutHex32((uint32_t)got);
        krnP4PutStr(", not its magic\n");
    }

    return got == P4_PROBE_STUB_MAGIC;
}

/*
 * scratch must be twelve bytes of writable PSRAM the caller does not need
 * afterwards, or NULL to skip everything PSRAM.
 */
void krnP4Probe(void *psram_scratch)
{
    uint32_t sram_buf[2];
    unsigned long status = csr_read(mstatus);

    /*
     * Keep the expected synchronous fault tied to the instruction being
     * measured. A timer interrupt in the few instructions between setting
     * the expectation and clearing it would add no information and would
     * make a global recovery latch needlessly ambiguous.
     */
    csr_clear(mstatus, MSTATUS_MIE);

    probe_misaligned(sram_buf, "sram ");

    if (!psram_scratch)
    {
        krnP4PutStr("[probe] psram  not up, skipped\n");
    }
    else
    {
        probe_misaligned(psram_scratch, "psram");
        probe_psram_exec(psram_scratch);
    }

    if (status & MSTATUS_MIE)
        csr_set(mstatus, MSTATUS_MIE);
}

#endif /* P4_PROBE */
