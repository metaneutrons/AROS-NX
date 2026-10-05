/*
    Copyright (c) 2023-2026, The AROS Development Team. All rights reserved.
    $Id$

    Desc: StackSwap() - swap the task's stack, RISC-V version.

    Translated from the aarch64-all version. Uses the build-generated
    symbolic offsets from <aros/riscv/asm.h> (FindTask, Disable, Enable,
    tc_SPLower, tc_SPUpper, stk_Lower, stk_Upper, stk_Pointer) - no magic
    numbers. The task comes from FindTask(NULL), not SysBase->ThisTask: an
    SMP build keeps the running task per hart and has no ThisTask field.
*/

	#include <aros/riscv/asm.h>

	.text
	.balign 4
	.globl	AROS_SLIB_ENTRY(StackSwap,Exec,122)

AROS_SLIB_ENTRY(StackSwap,Exec,122):

	// a0 = newStack (struct StackSwapStruct *)
	// a1 = SysBase
	// ra = return address
	//
	// Keep live values on the (old) stack across the Disable() call and
	// in registers across the SP swap itself; never hold a stack frame
	// that spans the swap (it would move with the stack).

	mv	t0, a0			// t0 = newStack

	// FindTask(NULL) and Disable() are ordinary calls: keep a1, t0, the
	// task and ra in a frame across both.
	addi	sp, sp, -16
	sw	a1, 0(sp)
	sw	t0, 4(sp)
	sw	ra, 12(sp)

	// The current task - arg0 = NULL, arg1 = SysBase.
	li	a0, 0
	lw	t2, FindTask(a1)
	jalr	t2
	sw	a0, 8(sp)

	// Disable() - arg0 = SysBase.
	lw	a1, 0(sp)
	mv	a0, a1
	lw	t2, Disable(a1)
	jalr	t2
	lw	a1, 0(sp)
	lw	t0, 4(sp)
	lw	t1, 8(sp)		// t1 = current task
	lw	ra, 12(sp)
	addi	sp, sp, 16

	// Swap lower stack boundary: task->tc_SPLower <-> newStack->stk_Lower
	lw	t2, tc_SPLower(t1)
	lw	t3, stk_Lower(t0)
	sw	t3, tc_SPLower(t1)
	sw	t2, stk_Lower(t0)

	// Swap upper stack boundary
	lw	t2, tc_SPUpper(t1)
	lw	t3, stk_Upper(t0)
	sw	t3, tc_SPUpper(t1)
	sw	t2, stk_Upper(t0)

	// Swap the stack pointer itself (nothing live on the stack here)
	mv	t2, sp
	lw	t3, stk_Pointer(t0)
	mv	sp, t3
	sw	t2, stk_Pointer(t0)

	// Enable() - running on the new stack now. Preserve a1 and ra.
	addi	sp, sp, -16
	sw	a1, 0(sp)
	sw	ra, 4(sp)
	mv	a0, a1
	lw	t2, Enable(a1)
	jalr	t2
	lw	a1, 0(sp)
	lw	ra, 4(sp)
	addi	sp, sp, 16

	ret
