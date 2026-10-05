/*
    Copyright (c) 2026, The AROS Development Team. All rights reserved.

    Desc: What this machine needs to make written bytes executable.

    RISC-V keeps instruction fetch and the data side apart on purpose, so
    a program just written stays data until something says otherwise.  What
    can say it differs per machine, which is why the shared riscv layer's
    CacheClearE() is a bare `fence rw, rw` that orders accesses and clears
    nothing, and why each platform is expected to replace it with whatever
    it actually has.  This port had not, and the omission was invisible
    until the first thing outside the ELF loader wrote code.

    That first thing was dos.library.  CreateSegList() builds a three
    instruction trampoline - auipc, lw, jr - through the data path and then
    jumps to it, calling CacheClearE() in between exactly as it should.
    With the generic version behind that call, the jump reached an address
    the instruction side had never re-fetched and the hart took an illegal
    instruction trap at the first word of a perfectly valid trampoline.

    On this SoC reconciling the two sides is three steps, not one, and
    kernel.resource already does all three in krnP4SyncCode(): write back
    the data caches so the bytes reach memory, invalidate both L1
    instruction caches and the L2 so the fetch goes out again, and fence.i
    so the hart's own fetch pipeline is flushed.  exec and kernel.resource
    are linked into the same kickstart image, so this calls it directly.
*/

#include <aros/debug.h>

#include <exec/types.h>
#include <exec/execbase.h>
#include <aros/libcall.h>
#include <aros/symbolsets.h>
#include <proto/exec.h>

#include <defines/exec_LVO.h>

#include "hardware.h"
#include "psram.h"
#include "kernel_intern.h"

AROS_LH3(void, CacheClearE_P4,
    AROS_LHA(APTR,  address, A0),
    AROS_LHA(IPTR,  length,  D0),
    AROS_LHA(ULONG, caches,  D1),
    struct ExecBase *, SysBase, 107, Exec)
{
    AROS_LIBFUNC_INIT

    __asm__ __volatile__ ("fence rw, rw" ::: "memory");

    /*
     * A caller asking only for a data invalidate is asking about a DMA
     * buffer, not about code, and krnP4SyncCode() would write the buffer
     * back over whatever a device had just put there.  So only the
     * clear/flush cases reach it.
     */
    if ((caches & (CACRF_ClearI | CACRF_ClearD)) && length)
        krnP4SyncCode(address, (unsigned long)length);

    AROS_LIBFUNC_EXIT
}

AROS_LH0(void, CacheClearU_P4,
    struct ExecBase *, SysBase, 106, Exec)
{
    AROS_LIBFUNC_INIT

    /*
     * CacheClearU() names no range, and the ROM helpers krnP4SyncCode()
     * uses take one.  Rather than invent a range, the whole of the memory
     * this port can execute from is covered: the internal SRAM the
     * kickstart and the general pool live in, and the external window the
     * package modules are relocated into.  Both are stated by the link
     * script and the PSRAM probe, so neither is a guess.
     */
    krnP4SyncCode((void *)P4_SRAM_BASE, P4_SRAM_END - P4_SRAM_BASE);
    if (__esp32p4_psram_size)
        krnP4SyncCode((void *)P4_PSRAM_WINDOW_BASE,
                      (unsigned long)__esp32p4_psram_size);

    AROS_LIBFUNC_EXIT
}

static int cpu_Init(struct ExecBase *SysBase)
{
    /*
     * Unconditional, unlike the SBI and Zicbom cases other RISC-V ports
     * have to probe for: this is one known SoC, its ROM cache routines are
     * at fixed addresses in both the base and the ECO5 ROM link scripts,
     * and the build already requires zifencei in -march.  There is nothing
     * to detect.
     */
    SetFunction(&SysBase->LibNode, -LVOCacheClearE * LIB_VECTSIZE,
                AROS_SLIB_ENTRY(CacheClearE_P4, Exec, LVOCacheClearE));
    SetFunction(&SysBase->LibNode, -LVOCacheClearU * LIB_VECTSIZE,
                AROS_SLIB_ENTRY(CacheClearU_P4, Exec, LVOCacheClearU));

    D(bug("[Exec] esp32p4: instruction fetch reconciled through the ROM"
          " cache routines\n"));

    return TRUE;
}

ADD2INITLIB(cpu_Init, 0);
