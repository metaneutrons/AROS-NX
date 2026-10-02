/*
    Copyright (c) 2026, The AROS Development Team. All rights reserved.
    $Id$

    Desc: Machine mode trap entry for the esp32p4-riscv target.

    Saves the interrupted state as a struct ExceptionContext (see
    aros/riscv/cpucontext.h) on the current stack and calls
    krnTrapHandler(ctx, mcause, mtval). On return the context, which the
    handler may have changed, is restored and execution resumes via mret.

    Frame layout (16-byte aligned): 288 bytes with P4_E3_FPU, otherwise 144.
      0-112:  x[29]  ra,sp,t0,t1,t2,fp,s1,a0-a7,s2-s11,t3-t6
      116:    pc     (mepc)
      120:    sr     (mstatus - carries the per-task FS dirty state and
                      MPIE, the pre-trap interrupt enable that mret
                      restores into MIE)
      124:    Flags
      128:    fpuContext
      132:    vecContext
      144:    P4_E3_FPU only: entry-time f0-f31 and fcsr spill (132 bytes)

    gp and tp are kernel-owned and are deliberately not part of the
    context, which is what RISCV_REGSAVE_CNT counting 29 of 32 reflects.

    Without P4_E3_FPU the original lazy save path remains unchanged. With
    P4_E3_FPU every entry captures FPU state before entering C; fpuContext
    points to this per-entry image until dispatch chooses a saved task image.

    With __AROSEXEC_SMP__ or P4_GIANT, mscratch holds this hart's private
    ISR stack top while a task runs and zero inside a trap, so every trap
    from task context leaves the task's stack at once. Two harts must not
    share one task stack, and the dispatcher idles inside the trap.

    Nothing here touches the trap depth counter KrnIsSuper() reports from;
    krnTrapHandler owns it, so that it covers exactly the C handler.
*/

#include "p4_trap_frame.h"

#if defined(__AROSEXEC_SMP__) && (!defined(P4_E3_FPU) || P4_E3_FPU != 1)
#error "P4 SMP traps require the matched 288-byte FPU frame"
#endif

#if defined(P4_E3_FPU) && (P4_E3_FPU == 1)
#define P4_TRAP_FRAME_SIZE P4_TRAP_FPU_FRAME_BYTES
#else
#define P4_TRAP_FRAME_SIZE P4_TRAP_BASE_FRAME_BYTES
#endif

/*
 * 64-byte aligned, and not by preference. This CLIC does not take mtvec
 * as the entry address: it jumps to mtvec[31:6] << 6, so the low six bits
 * are not part of the address at all and an entry point that is merely
 * 4-byte aligned gets rounded down to somewhere else entirely. The
 * symptom is an interrupt that pends and is never seen.
 */
	.text
	.balign	0x40
	.globl	__trap_entry
	.type	__trap_entry,%function
__trap_entry:
#if defined(__AROSEXEC_SMP__) || defined(P4_GIANT)
	/* Outside traps mscratch holds this hart's private ISR top. Inside
	 * kernel C it is zero, so nested traps retain the existing ISR stack.
	 * No general register is clobbered before its frame slot is saved. */
	csrrw	sp, mscratch, sp
	bnez	sp, .Lp4_isr_stack_selected
	csrrw	sp, mscratch, sp
.Lp4_isr_stack_selected:
#endif
	addi	sp, sp, -P4_TRAP_FRAME_SIZE
	sw	ra, 0(sp)
	sw	t0, 8(sp)
	sw	t1, 12(sp)
	sw	t2, 16(sp)
	sw	s0, 20(sp)
	sw	s1, 24(sp)
	sw	a0, 28(sp)
	sw	a1, 32(sp)
	sw	a2, 36(sp)
	sw	a3, 40(sp)
	sw	a4, 44(sp)
	sw	a5, 48(sp)
	sw	a6, 52(sp)
	sw	a7, 56(sp)
	sw	s2, 60(sp)
	sw	s3, 64(sp)
	sw	s4, 68(sp)
	sw	s5, 72(sp)
	sw	s6, 76(sp)
	sw	s7, 80(sp)
	sw	s8, 84(sp)
	sw	s9, 88(sp)
	sw	s10, 92(sp)
	sw	s11, 96(sp)
	sw	t3, 100(sp)
	sw	t4, 104(sp)
	sw	t5, 108(sp)
	sw	t6, 112(sp)
	/* interrupted sp, pc and mstatus */
#if defined(__AROSEXEC_SMP__) || defined(P4_GIANT)
	csrr	t0, mscratch
	beqz	t0, .Lp4_isr_nested_frame
	sw	t0, 4(sp)
	addi	t0, sp, P4_TRAP_FRAME_SIZE
	sw	t0, P4_TRAP_ISR_TOP_OFFSET(sp)
	csrw	mscratch, zero
	j	.Lp4_isr_frame_ready
.Lp4_isr_nested_frame:
	addi	t0, sp, P4_TRAP_FRAME_SIZE
	sw	t0, 4(sp)
	sw	zero, P4_TRAP_ISR_TOP_OFFSET(sp)
.Lp4_isr_frame_ready:
#else
	addi	t0, sp, P4_TRAP_FRAME_SIZE
	sw	t0, 4(sp)
#endif
	csrr	t0, mepc
	sw	t0, 116(sp)
	csrr	t0, mstatus
	sw	t0, 120(sp)
	/* Flags / context pointers */
	sw	zero, 124(sp)

#if defined(P4_E3_FPU) && (P4_E3_FPU == 1)
	sw	zero, 132(sp)
	/*
	 * Save the entry status first, then make F instructions legal for both
	 * the snapshot and the C handler. The image pointer is always valid,
	 * including when the interrupted frame had FS=OFF. In that case there
	 * is no live task FPU state to preserve, so start C from a zero image.
	 */
	li	t1, P4_TRAP_MSTATUS_FS_MASK
	csrs	mstatus, t1
	addi	t0, sp, P4_TRAP_FPU_SPILL_OFFSET
	sw	t0, 128(sp)
	lw	t1, 120(sp)
	li	t2, P4_TRAP_MSTATUS_FS_MASK
	and	t1, t1, t2
	bnez	t1, .Lp4_trap_fpu_save
	fmv.w.x f0, zero
	fmv.w.x f1, zero
	fmv.w.x f2, zero
	fmv.w.x f3, zero
	fmv.w.x f4, zero
	fmv.w.x f5, zero
	fmv.w.x f6, zero
	fmv.w.x f7, zero
	fmv.w.x f8, zero
	fmv.w.x f9, zero
	fmv.w.x f10, zero
	fmv.w.x f11, zero
	fmv.w.x f12, zero
	fmv.w.x f13, zero
	fmv.w.x f14, zero
	fmv.w.x f15, zero
	fmv.w.x f16, zero
	fmv.w.x f17, zero
	fmv.w.x f18, zero
	fmv.w.x f19, zero
	fmv.w.x f20, zero
	fmv.w.x f21, zero
	fmv.w.x f22, zero
	fmv.w.x f23, zero
	fmv.w.x f24, zero
	fmv.w.x f25, zero
	fmv.w.x f26, zero
	fmv.w.x f27, zero
	fmv.w.x f28, zero
	fmv.w.x f29, zero
	fmv.w.x f30, zero
	fmv.w.x f31, zero
	csrw	fcsr, zero
.Lp4_trap_fpu_save:
	fsw	f0, 0(t0)
	fsw	f1, 4(t0)
	fsw	f2, 8(t0)
	fsw	f3, 12(t0)
	fsw	f4, 16(t0)
	fsw	f5, 20(t0)
	fsw	f6, 24(t0)
	fsw	f7, 28(t0)
	fsw	f8, 32(t0)
	fsw	f9, 36(t0)
	fsw	f10, 40(t0)
	fsw	f11, 44(t0)
	fsw	f12, 48(t0)
	fsw	f13, 52(t0)
	fsw	f14, 56(t0)
	fsw	f15, 60(t0)
	fsw	f16, 64(t0)
	fsw	f17, 68(t0)
	fsw	f18, 72(t0)
	fsw	f19, 76(t0)
	fsw	f20, 80(t0)
	fsw	f21, 84(t0)
	fsw	f22, 88(t0)
	fsw	f23, 92(t0)
	fsw	f24, 96(t0)
	fsw	f25, 100(t0)
	fsw	f26, 104(t0)
	fsw	f27, 108(t0)
	fsw	f28, 112(t0)
	fsw	f29, 116(t0)
	fsw	f30, 120(t0)
	fsw	f31, 124(t0)
	csrr	t1, fcsr
	sw	t1, 128(t0)
	/* Kernel C starts with deterministic RNE and clear accrued flags. */
	csrw	fcsr, zero
#else
	sw	zero, 128(sp)
	sw	zero, 132(sp)
#endif

	mv	a0, sp			/* ctx    */
	csrr	a1, mcause
	csrr	a2, mtval
	call	krnTrapHandler

	/* Restore only after the handler and any dispatch-side C calls finish. */
#if defined(P4_E3_FPU) && (P4_E3_FPU == 1)
	/* C is allowed to use FPU; re-enable it before the final restore. */
	li	t1, P4_TRAP_MSTATUS_FS_MASK
	csrs	mstatus, t1
	lw	t0, 128(sp)
	beqz	t0, .Lp4_trap_fpu_zero_exit
	lw	t1, 128(t0)
	csrw	fcsr, t1
	flw	f0, 0(t0)
	flw	f1, 4(t0)
	flw	f2, 8(t0)
	flw	f3, 12(t0)
	flw	f4, 16(t0)
	flw	f5, 20(t0)
	flw	f6, 24(t0)
	flw	f7, 28(t0)
	flw	f8, 32(t0)
	flw	f9, 36(t0)
	flw	f10, 40(t0)
	flw	f11, 44(t0)
	flw	f12, 48(t0)
	flw	f13, 52(t0)
	flw	f14, 56(t0)
	flw	f15, 60(t0)
	flw	f16, 64(t0)
	flw	f17, 68(t0)
	flw	f18, 72(t0)
	flw	f19, 76(t0)
	flw	f20, 80(t0)
	flw	f21, 84(t0)
	flw	f22, 88(t0)
	flw	f23, 92(t0)
	flw	f24, 96(t0)
	flw	f25, 100(t0)
	flw	f26, 104(t0)
	flw	f27, 108(t0)
	flw	f28, 112(t0)
	flw	f29, 116(t0)
	flw	f30, 120(t0)
	flw	f31, 124(t0)
	j	.Lp4_trap_fpu_exit_done
.Lp4_trap_fpu_zero_exit:
	csrw	fcsr, zero
	fmv.w.x f0, zero
	fmv.w.x f1, zero
	fmv.w.x f2, zero
	fmv.w.x f3, zero
	fmv.w.x f4, zero
	fmv.w.x f5, zero
	fmv.w.x f6, zero
	fmv.w.x f7, zero
	fmv.w.x f8, zero
	fmv.w.x f9, zero
	fmv.w.x f10, zero
	fmv.w.x f11, zero
	fmv.w.x f12, zero
	fmv.w.x f13, zero
	fmv.w.x f14, zero
	fmv.w.x f15, zero
	fmv.w.x f16, zero
	fmv.w.x f17, zero
	fmv.w.x f18, zero
	fmv.w.x f19, zero
	fmv.w.x f20, zero
	fmv.w.x f21, zero
	fmv.w.x f22, zero
	fmv.w.x f23, zero
	fmv.w.x f24, zero
	fmv.w.x f25, zero
	fmv.w.x f26, zero
	fmv.w.x f27, zero
	fmv.w.x f28, zero
	fmv.w.x f29, zero
	fmv.w.x f30, zero
	fmv.w.x f31, zero
.Lp4_trap_fpu_exit_done:
#endif
	/* restore (the handler may have modified the context) */
#if defined(__AROSEXEC_SMP__) || defined(P4_GIANT)
	/* Marker is not part of copyContext: dispatch cannot overwrite it.
	 * Restore before register/SP handoff, while MIE is still clear. */
	lw	t0, P4_TRAP_ISR_TOP_OFFSET(sp)
	csrw	mscratch, t0
#endif
	lw	t0, 116(sp)
	csrw	mepc, t0
	lw	t0, 120(sp)
#if defined(__AROSEXEC_SMP__) || defined(P4_GIANT)
	/* mret, not a premature CSR write, restores interrupt delivery. */
	andi	t0, t0, -9
#endif
	csrw	mstatus, t0
	lw	ra, 0(sp)
	lw	t1, 12(sp)
	lw	t2, 16(sp)
	lw	s0, 20(sp)
	lw	s1, 24(sp)
	lw	a0, 28(sp)
	lw	a1, 32(sp)
	lw	a2, 36(sp)
	lw	a3, 40(sp)
	lw	a4, 44(sp)
	lw	a5, 48(sp)
	lw	a6, 52(sp)
	lw	a7, 56(sp)
	lw	s2, 60(sp)
	lw	s3, 64(sp)
	lw	s4, 68(sp)
	lw	s5, 72(sp)
	lw	s6, 76(sp)
	lw	s7, 80(sp)
	lw	s8, 84(sp)
	lw	s9, 88(sp)
	lw	s10, 92(sp)
	lw	s11, 96(sp)
	lw	t3, 100(sp)
	lw	t4, 104(sp)
	lw	t5, 108(sp)
	lw	t6, 112(sp)
	lw	t0, 8(sp)
	/* Load sp last, directly from the context - the dispatcher may
	   have switched the frame to a different task */
	lw	sp, 4(sp)
	mret

	.size	__trap_entry, . - __trap_entry
