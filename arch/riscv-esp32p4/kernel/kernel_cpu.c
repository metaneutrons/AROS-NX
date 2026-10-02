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
#if defined(__AROSEXEC_SMP__)
#include <exec_platform.h>
#include "p4_dispatch.h"
#include "p4_switch.h"
#include "p4_discard.h"
#include "p4_dispatch_callbacks.h"
#elif defined(P4_GIANT)
#include <exec_platform.h>
#include "p4_giant.h"
#endif

#if defined(P4_E3_FPU) && (P4_E3_FPU == 1)
#include "p4_trap_frame.h"

_Static_assert(sizeof(struct ExceptionContext) == P4_TRAP_CONTEXT_BYTES,
               "the public RISC-V context must keep its existing size");
_Static_assert(offsetof(struct ExceptionContext, fpuContext) == 128,
               "traps.S requires fpuContext at byte 128");
_Static_assert(sizeof(struct FpuContext) == P4_TRAP_FPU_SPILL_BYTES,
               "P4 trap spill and FLEN=32 task image must have the same size");
_Static_assert(offsetof(struct FpuContext, fcsr) == 128,
               "P4 FLEN=32 fcsr must follow the 32 single-width registers");
#endif

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

#if !defined(P4_E3_FPU) || (P4_E3_FPU != 1)
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
#endif

void cpu_Switch(regs_t *regs)
{
#if defined(__AROSEXEC_SMP__)
    krnP4Switch(regs, SysBase);
#else
    struct Task *task =
#if defined(P4_GIANT)
        GET_THIS_TASK;
#else
        SysBase->ThisTask;
#endif
    struct ExceptionContext *ctx = task->tc_UnionETask.tc_ETask->et_RegFrame;

    copyContext(ctx, regs);
    task->tc_SPReg = (APTR)regs->sp;

#if defined(P4_E3_FPU) && (P4_E3_FPU == 1)
    /*
     * The trap entry has already captured every F register and fcsr before
     * calling C. Copy that immutable per-entry image, not the live hardware
     * state, which the handler and scheduler may have used. FS=INITIAL/CLEAN
     * deliberately leaves the saved task image untouched; this also handles
     * a never-used FPU context whose allocated block is not yet valid.
     */
    if (p4_trap_fpu_state_is_dirty(ctx->sr))
    {
        p4_trap_fpu_copy_entry_spill(ctx->fpuContext, regs->fpuContext);
        ctx->Flags |= ECF_FPU;
        ctx->sr = (ctx->sr & ~MSTATUS_FS) | MSTATUS_FS_CLEAN;
    }
#else
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
#endif

    /*
     * No CPU time accounting yet. The 64bit port charges the run segment
     * that just ended to the task here, reading the time CSR; the clock
     * available on this machine is the SYSTIMER counter, which costs a
     * snapshot request and a poll rather than one instruction, so it is
     * left out until it is worth the switch path.
     */

    core_Switch();
#endif
}

void cpu_Dispatch(regs_t *regs)
{
    struct Task *task;
    struct ExceptionContext *ctx;

#if defined(__AROSEXEC_SMP__)
    /* SC_DISPATCH bypasses cpu_Switch; only an already removed/detached Task
     * may be discarded. Ordinary switches already cleared current_task. */
    if (GET_THIS_TASK != NULL)
        krnP4DiscardCurrent(SysBase);
#endif

#if defined(__AROSEXEC_SMP__)
dispatch_again:
    /* No current Task/queue lock remains after switch/discard. A producer's
     * nested IPI can be consumed here before choosing a ready Task. */
    IDNESTCOUNT_SET(-1);
    Exec_P4DeliverCleanupWake(SysBase);
    csr_clear(mstatus, MSTATUS_MIE);
#endif
    while (!(task =
#if defined(__AROSEXEC_SMP__)
             krnP4Dispatch(SysBase)
#else
             core_Dispatch()
#endif
            ))
    {
        /*
         * The outgoing task's nesting state was saved by core_Switch().
         * Nothing is executing on its behalf while the dispatcher idles,
         * so do not inherit its Disable() level here.  In particular the
         * SYSTIMER IRQ only causes INTB_VERTB while IDNest is negative;
         * retaining zero here suppresses the timer reply that would make
         * a waiting task runnable and deadlocks the empty-ready-list path.
         * core_Dispatch() restores the selected task's saved nesting state
         * before returning it.
         */
        IDNESTCOUNT_SET(-1);
#if defined(P4_GIANT)
        /* Give up F and D while asleep: the other hart must be able to
           Disable(), and its interrupts, while this one waits for work. */
        p4_giant_idle_enter();
#endif
        csr_set(mstatus, MSTATUS_MIE);
        asm volatile("wfi");
        csr_clear(mstatus, MSTATUS_MIE);
#if defined(P4_GIANT)
        p4_giant_idle_leave();
#endif

#if defined(__AROSEXEC_SMP__)
        Exec_P4DeliverCleanupWake(SysBase);
        csr_clear(mstatus, MSTATUS_MIE);
        /* Dispatcher callbacks return with MIE enabled. Re-mask before
         * admission, and repoll work whose doorbell ran as a nested IPI. */
        while (EXEC_PLATFORM_SOFTINT_PENDING())
        {
            core_Cause(INTB_SOFTINT, 1L << INTB_SOFTINT);
            csr_clear(mstatus, MSTATUS_MIE);
        }
#elif defined(P4_GIANT)
        /* Soft interrupts run on hart 0 only. */
        if (EXEC_PLATFORM_SOFTINT_PENDING())
            core_Cause(INTB_SOFTINT, 1L << INTB_SOFTINT);
#else
        if (SysBase->SysFlags & SFF_SoftInt)
            core_Cause(INTB_SOFTINT, 1L << INTB_SOFTINT);
#endif
    }

#if defined(P4_GIANT)
    p4_giant_dispatched();
#endif
    ctx = task->tc_UnionETask.tc_ETask->et_RegFrame;
    copyContext(regs, ctx);

#if !defined(P4_E3_FPU) || (P4_E3_FPU != 1)
    /* Bring in the task's FPU state, or the clean state if it has none */
    if (ctx->Flags & ECF_FPU)
        krnRestoreFPU(ctx->fpuContext);
    else
        krnInitFPU();
#endif

#if defined(__AROSEXEC_SMP__)
    if (!krnP4DispatchCallbacks(task, SysBase))
    {
        /* RUN kept the old context alive through callbacks. After discard,
         * neither task nor ctx may be touched; replace the whole return frame. */
        krnP4DiscardCurrent(SysBase);
        goto dispatch_again;
    }
#else
    if (task->tc_Flags & TF_EXCEPT)
        Exception();

    if (task->tc_Flags & TF_LAUNCH)
    {
        AROS_UFC1(void, task->tc_Launch,
                  AROS_UFCA(struct ExecBase *, SysBase, A6));
    }
#endif
#if defined(P4_E3_FPU) && (P4_E3_FPU == 1)
    /*
     * Leave the entry spill installed throughout every C call above. A
     * nested trap gets its own stack-local image; only now select the task
     * image that traps.S restores after this final C return.
     */
    regs->fpuContext = (ctx->Flags & ECF_FPU) ? ctx->fpuContext : (APTR)0;
#endif
    /* Leave the trap and resume the new task */
}
