/*
    Copyright (c) 2026, The AROS Development Team. All rights reserved.
    $Id$

    Desc: Machine mode trap entry for the esp32p4-riscv target.

    Saves the interrupted state as a struct ExceptionContext (see
    aros/riscv/cpucontext.h) on the current stack and calls
    krnTrapHandler(ctx, mcause, mtval). On return the context, which the
    handler may have changed, is restored and execution resumes via mret.

    Frame layout (16-byte aligned, 144 bytes):
      0-112:  x[29]  ra,sp,t0,t1,t2,fp,s1,a0-a7,s2-s11,t3-t6
      116:    pc     (mepc)
      120:    sr     (mstatus - carries the per-task FS dirty state and
                      MPIE, the pre-trap interrupt enable that mret
                      restores into MIE)
      124:    Flags
      128:    fpuContext
      132:    vecContext

    gp and tp are kernel-owned and are deliberately not part of the
    context, which is what RISCV_REGSAVE_CNT counting 29 of 32 reflects.

    The f registers are not saved here. mstatus.FS travels in the context
    and the scheduler saves the FPU only when it reads back as DIRTY; see
    kernel_cpu.c.

    Nothing here touches the trap depth counter KrnIsSuper() reports from;
    krnTrapHandler owns it, so that it covers exactly the C handler.
*/

/*
 * 64-byte aligned, and not by preference. This CLIC does not take mtvec
 * as the entry address: it jumps to mtvec[31:6] << 6, so the low six bits
 * are not part of the address at all and an entry point that is merely
 * 4-byte aligned gets rounded down to somewhere else entirely. The
 * symptom is an interrupt that pends and is never seen.
 */
#include <aros/config.h>

	.text
	.balign	0x40
	.globl	__trap_entry
	.type	__trap_entry,%function
__trap_entry:
#if defined(__AROSEXEC_SMP__)
	/*
	 * Each hart has its own interrupt stack in internal SRAM, whose top
	 * mscratch holds while no trap is active on that hart. A trap frame
	 * on a task's stack would still be in use while another hart already
	 * runs the task, and while hart 1 waits out a cache-off window PSRAM
	 * cannot be reached at all. mscratch is 0 inside a trap, so a nested
	 * one stays on the stack it interrupted.
	 */
	csrrw	sp, mscratch, sp
	bnez	sp, 1f
	csrrw	sp, mscratch, sp	/* nested: back, mscratch 0 again */
	addi	sp, sp, -144
	sw	t0, 8(sp)
	addi	t0, sp, 144
	sw	t0, 4(sp)		/* interrupted sp */
	sw	zero, 136(sp)		/* nothing to put back on exit */
	j	2f
1:	addi	sp, sp, -144		/* sp: this hart's interrupt stack */
	sw	t0, 8(sp)
	addi	t0, sp, 144
	sw	t0, 136(sp)		/* its top, to put back on exit */
	csrrw	t0, mscratch, zero	/* interrupted sp; 0 while inside */
	sw	t0, 4(sp)
2:	sw	ra, 0(sp)
#else
	addi	sp, sp, -144
	sw	ra, 0(sp)
	sw	t0, 8(sp)
#endif
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
#if !defined(__AROSEXEC_SMP__)
	addi	t0, sp, 144
	sw	t0, 4(sp)
#endif
	csrr	t0, mepc
	sw	t0, 116(sp)
	csrr	t0, mstatus
	sw	t0, 120(sp)
	/* Flags / context pointers */
	sw	zero, 124(sp)
	sw	zero, 128(sp)
	sw	zero, 132(sp)

	mv	a0, sp			/* ctx    */
	csrr	a1, mcause
	csrr	a2, mtval
	call	krnTrapHandler

	/* restore (the handler may have modified the context) */
#if defined(__AROSEXEC_SMP__)
	lw	t0, 136(sp)
	csrw	mscratch, t0		/* the stack top again, or 0 when nested */
#endif
	lw	t0, 116(sp)
	csrw	mepc, t0
	lw	t0, 120(sp)
#if defined(__AROSEXEC_SMP__)
	/* Still on the interrupt stack with mscratch armed: an interrupt now
	   would start a fresh frame over this one. MIE comes back with mret,
	   from MPIE. */
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
