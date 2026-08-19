/*
    Copyright (c) 2026, The AROS Development Team. All rights reserved.

    Desc: CPU-level task switching for the esp32p4-riscv target.

    cpu_Switch() saves the interrupted context - the trap frame traps.S
    built - into the current task's register frame; cpu_Dispatch() picks
    the next task and loads its frame back into the trap frame, and the
    trap exit path resumes it with mret.
*/

#include <exec/execbase.h>
#include <exec/tasks.h>
#include <proto/exec.h>

#include <aros/riscv/cpucontext.h>
#include <asm/cpu.h>

#include <hardware/intbits.h>

#include <kernel_base.h>
#include <kernel_debug.h>
#include <kernel_intr.h>
#include <kernel_scheduler.h>

#include "etask.h"

#include "hardware.h"
#include "kernel_intern.h"
#include "kernel_cpu.h"

/*
 * The volatile part of a context: the x registers, pc and sr. Flags and
 * the fpuContext pointer stay as they are - they are metadata belonging
 * to the saved context, not values being carried across.
 */
static void copyContext(struct ExceptionContext *dst,
                        struct ExceptionContext *src)
{
    int i;

    for (i = 0; i < RISCV_REGSAVE_CNT; i++)
        dst->x[i] = src->x[i];
    dst->pc = src->pc;
    dst->sr = src->sr;
}

/* FLEN is 32, so these are fsw/flw at four byte spacing, not fsd/fld */
static void krnSaveFPU(struct FpuContext *fpu)
{
    unsigned long fcsr;

    asm volatile(
        "fsw f0, 0(%1) \n fsw f1, 4(%1) \n fsw f2, 8(%1) \n"
        "fsw f3, 12(%1) \n fsw f4, 16(%1) \n fsw f5, 20(%1) \n"
        "fsw f6, 24(%1) \n fsw f7, 28(%1) \n fsw f8, 32(%1) \n"
        "fsw f9, 36(%1) \n fsw f10, 40(%1) \n fsw f11, 44(%1) \n"
        "fsw f12, 48(%1) \n fsw f13, 52(%1) \n fsw f14, 56(%1) \n"
        "fsw f15, 60(%1) \n fsw f16, 64(%1) \n fsw f17, 68(%1) \n"
        "fsw f18, 72(%1) \n fsw f19, 76(%1) \n fsw f20, 80(%1) \n"
        "fsw f21, 84(%1) \n fsw f22, 88(%1) \n fsw f23, 92(%1) \n"
        "fsw f24, 96(%1) \n fsw f25, 100(%1) \n fsw f26, 104(%1) \n"
        "fsw f27, 108(%1) \n fsw f28, 112(%1) \n fsw f29, 116(%1) \n"
        "fsw f30, 120(%1) \n fsw f31, 124(%1) \n"
        "csrr %0, fcsr\n"
        : "=r"(fcsr) : "r"(fpu->f) : "memory");

    fpu->fcsr = fcsr;
}

static void krnRestoreFPU(struct FpuContext *fpu)
{
    asm volatile(
        "csrw fcsr, %1\n"
        "flw f0, 0(%0) \n flw f1, 4(%0) \n flw f2, 8(%0) \n"
        "flw f3, 12(%0) \n flw f4, 16(%0) \n flw f5, 20(%0) \n"
        "flw f6, 24(%0) \n flw f7, 28(%0) \n flw f8, 32(%0) \n"
        "flw f9, 36(%0) \n flw f10, 40(%0) \n flw f11, 44(%0) \n"
        "flw f12, 48(%0) \n flw f13, 52(%0) \n flw f14, 56(%0) \n"
        "flw f15, 60(%0) \n flw f16, 64(%0) \n flw f17, 68(%0) \n"
        "flw f18, 72(%0) \n flw f19, 76(%0) \n flw f20, 80(%0) \n"
        "flw f21, 84(%0) \n flw f22, 88(%0) \n flw f23, 92(%0) \n"
        "flw f24, 96(%0) \n flw f25, 100(%0) \n flw f26, 104(%0) \n"
        "flw f27, 108(%0) \n flw f28, 112(%0) \n flw f29, 116(%0) \n"
        "flw f30, 120(%0) \n flw f31, 124(%0) \n"
        : : "r"(fpu->f), "r"((unsigned long)fpu->fcsr) : "memory");
}

/*
 * The canonical clean state, for a task that has never touched the FPU.
 * Without it the previous owner's registers would show through.
 */
static void krnInitFPU(void)
{
    asm volatile(
        "csrw fcsr, zero\n"
        "fmv.w.x f0, zero \n fmv.w.x f1, zero \n fmv.w.x f2, zero \n"
        "fmv.w.x f3, zero \n fmv.w.x f4, zero \n fmv.w.x f5, zero \n"
        "fmv.w.x f6, zero \n fmv.w.x f7, zero \n fmv.w.x f8, zero \n"
        "fmv.w.x f9, zero \n fmv.w.x f10, zero \n fmv.w.x f11, zero \n"
        "fmv.w.x f12, zero \n fmv.w.x f13, zero \n fmv.w.x f14, zero \n"
        "fmv.w.x f15, zero \n fmv.w.x f16, zero \n fmv.w.x f17, zero \n"
        "fmv.w.x f18, zero \n fmv.w.x f19, zero \n fmv.w.x f20, zero \n"
        "fmv.w.x f21, zero \n fmv.w.x f22, zero \n fmv.w.x f23, zero \n"
        "fmv.w.x f24, zero \n fmv.w.x f25, zero \n fmv.w.x f26, zero \n"
        "fmv.w.x f27, zero \n fmv.w.x f28, zero \n fmv.w.x f29, zero \n"
        "fmv.w.x f30, zero \n fmv.w.x f31, zero \n"
        );
}

void cpu_Switch(regs_t *regs)
{
    struct Task *task = SysBase->ThisTask;
    struct ExceptionContext *ctx = task->tc_UnionETask.tc_ETask->et_RegFrame;

    copyContext(ctx, regs);
    task->tc_SPReg = (APTR)regs->sp;

    /*
     * Lazy FPU: the task's FS state arrived in sr at trap entry. Only a
     * dirty FPU is worth saving, and it resumes Clean so the next write
     * is tracked again.
     *
     * This trusts that nothing between the trap entry and here uses the
     * f registers. traps.S does not, and the C path leading here should
     * not either; building the kernel objects without floating point
     * would make that a guarantee rather than a convention.
     */
    if ((ctx->sr & MSTATUS_FS) == MSTATUS_FS_DIRTY)
    {
        krnSaveFPU(ctx->fpuContext);
        ctx->Flags |= ECF_FPU;
        ctx->sr = (ctx->sr & ~MSTATUS_FS) | MSTATUS_FS_CLEAN;
    }

    /*
     * No CPU time accounting yet. The 64bit port charges the run segment
     * that just ended to the task here, reading the time CSR; the clock
     * available on this machine is the SYSTIMER counter, which costs a
     * snapshot request and a poll rather than one instruction, so it is
     * left out until it is worth the switch path.
     */

    core_Switch();
}

void cpu_Dispatch(regs_t *regs)
{
    struct Task *task;
    struct ExceptionContext *ctx;

    while (!(task = core_Dispatch()))
    {
        /*
         * Nothing runnable at all. The idle task normally prevents this,
         * so this is the window before it exists. Wait with interrupts
         * open for one to arrive.
         */
        csr_set(mstatus, MSTATUS_MIE);
        asm volatile("wfi");
        csr_clear(mstatus, MSTATUS_MIE);

        if (SysBase->SysFlags & SFF_SoftInt)
            core_Cause(INTB_SOFTINT, 1L << INTB_SOFTINT);
    }

    ctx = task->tc_UnionETask.tc_ETask->et_RegFrame;
    copyContext(regs, ctx);

    /* Bring in the task's FPU state, or the clean state if it has none */
    if (ctx->Flags & ECF_FPU)
        krnRestoreFPU(ctx->fpuContext);
    else
        krnInitFPU();

    if (task->tc_Flags & TF_EXCEPT)
        Exception();

    if (task->tc_Flags & TF_LAUNCH)
    {
        AROS_UFC1(void, task->tc_Launch,
                  AROS_UFCA(struct ExecBase *, SysBase, A6));
    }
    /* Leave the trap and resume the new task */
}
