/*
    Copyright (c) 2026, The AROS Development Team. All rights reserved.
    $Id$

    Desc: Machine mode entry point for the esp32p4-riscv target.

    The ESP32-P4's first stage ROM loader reads the image header, copies
    each segment to the address the header names and jumps to the entry
    point. Nothing is handed over in registers: there is no SBI below us
    and no device tree pointer, so the hart number is read from mhartid
    and passed on in its place.

    Only one hart arrives here. The second high performance core is held
    in reset out of power-on and stays there until something releases it.

    The mstatus bit values are written as literals, as they are in the
    other RISC-V ports: the names in asm/cpu.h carry a UL suffix for C
    and do not assemble.
*/

	.section .aros.init,"ax"
	.globl	_start
	.type	_start,%function

_start:
	/* Mask machine interrupts until there is a dispatcher for them */
	csrw	mie, zero
	csrci	mstatus, 8		/* MSTATUS_MIE               */

	/* Global pointer. Linker relaxation has to be off while gp itself
	   is being loaded, or the load relaxes against the gp it is
	   setting up. */
.option push
.option norelax
	la	gp, __global_pointer$
.option pop

	/* Boot stack */
	la	sp, __boot_stack_end

	/* Trap vector, in direct mode: every trap lands on one entry
	   point. The CLIC's vectored table in mtvt is set up later, once
	   there is an interrupt controller to point it at. */
	la	t0, __trap_entry
	csrw	mtvec, t0

	/*
	 * Turn the FPU on. ilp32f code may touch the f registers at any
	 * point, including in the startup path below, so the state field
	 * has to leave OFF before the first C call rather than when the
	 * scheduler gets around to it. INITIAL, not CLEAN: nothing has
	 * been written yet, and the first write moves it to DIRTY, which
	 * is what the lazy save keys on.
	 */
	li	t0, 0x2000		/* MSTATUS_FS_INITIAL        */
	csrs	mstatus, t0

	/* Clear .bss. The ldscript 4-aligns both ends. */
	la	t0, __bss_start
	la	t1, __bss_end
1:	bgeu	t0, t1, 2f
	sw	zero, 0(t0)
	addi	t0, t0, 4
	j	1b
2:
	/* Record and pass on the hart we came up on */
	csrr	a0, mhartid
	la	t0, __boot_hartid
	sw	a0, 0(t0)

	/* kernel_cstart(hartid, fdt). There is no firmware supplied
	   device tree; the argument is kept so that a built-in one can be
	   handed over here later without changing the entry contract. */
	li	a1, 0
	call	kernel_cstart

	/* Not reached. If it is, stop somewhere identifiable. */
	.balign	4
__startup_hang:
3:	wfi
	j	3b

	.size	_start, . - _start

	.section .bss

	.globl	__boot_hartid
	.balign	4
__boot_hartid:
	.space	4

	/* Declared after the hart id so that the stack's end and the next
	   object do not share an address; the stack grows down from
	   __boot_stack_end, so sharing would be harmless but confusing. */
	.balign	16
__boot_stack:
	.space	16384
__boot_stack_end:
