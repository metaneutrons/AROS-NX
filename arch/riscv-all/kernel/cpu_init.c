/*
    Copyright (c) 2023-2026, The AROS Development Team. All rights reserved.

    Desc: CPU probing and context sizing, RISC-V version.
*/

#include <aros/symbolsets.h>
#include <exec/types.h>

#include <aros/riscv/cpucontext.h>
#include <asm/cpu.h>

#include "kernel_base.h"
#include "kernel_cpu.h"

/*
 * Bytes per vector register (the vlenb CSR), or 0 when the vector
 * extension is not implemented. Probed once at boot; KrnCreateContext()
 * uses it to lay out the per-task vector save area.
 */
unsigned long __riscv_vlenb;

/*
 * Which status CSR the probe below may touch. A platform with supervisor
 * mode reaches the FS and VS fields through sstatus; one that never
 * leaves machine mode has no sstatus at all, and touching it raises an
 * illegal instruction rather than reading zero. The platform's
 * kernel_cpu.h says which of the two it is.
 *
 * Given by number rather than by name because the accessors stringify
 * what they are handed, and a macro name would reach the assembler
 * unexpanded. The vector CSR below is written the same way.
 */
#ifndef RISCV_XSTATUS
#define RISCV_XSTATUS               0x100   /* sstatus */
#define RISCV_XSTATUS_VS            SSTATUS_VS
#define RISCV_XSTATUS_VS_INITIAL    SSTATUS_VS_INITIAL
#endif

/*
 * One level of indirection, because # does not expand its argument: the
 * accessors stringify the name they are handed, so RISCV_XSTATUS has to
 * be substituted through a macro that does not stringify before it
 * reaches them.
 */
#define xcsr_set(c, m)      csr_set(c, m)
#define xcsr_read(c)        csr_read(c)
#define xcsr_clear(c, m)    csr_clear(c, m)

/*
 * The job of this function is to probe the CPU and set up kb_ContextFlags
 * and kb_ContextSize.
 * kb_ContextFlags is whatever needs to be passed to KrnCreateContext() in
 * order to create a right thing. kb_ContextSize is total length of our
 * context area (including FPU data, vector data and private data). It is
 * needed for complete context save/restore during Exec exceptions
 * processing.
 */

static int cpu_Init(struct KernelBase *KernelBase)
{
    /*
     * Runtime-detect the vector extension: the VS field reads as a
     * hardwired zero when V is not implemented, so enable it and see if
     * the write sticks. The vector CSRs may only be accessed while VS is
     * enabled, so vlenb is read before switching VS back off.
     */
    xcsr_set(RISCV_XSTATUS, RISCV_XSTATUS_VS_INITIAL);
    if (xcsr_read(RISCV_XSTATUS) & RISCV_XSTATUS_VS)
    {
        __riscv_vlenb = csr_read(0xC22 /* CSR_VLENB */);
        xcsr_clear(RISCV_XSTATUS, RISCV_XSTATUS_VS);
    }

    /*
     * The ExceptionContext, the FPU block and (when present) the vector
     * block are allocated as one chunk; KrnCreateContext() points
     * fpuContext/vecContext at the 16-byte aligned tails.
     */
    KernelBase->kb_ContextSize = sizeof(struct ExceptionContext) + 15
                               + sizeof(struct FpuContext);
    if (__riscv_vlenb)
    {
        KernelBase->kb_ContextSize += 15 + sizeof(struct VectorContext)
                                    + (32 * __riscv_vlenb);
    }

    return TRUE;
}

ADD2INITLIB(cpu_Init, 5);
