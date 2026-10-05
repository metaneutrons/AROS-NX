/*
    Copyright (c) 2026, The AROS Development Team. All rights reserved.

    Desc: hart 1's entry in the smp variant of the esp32p4-riscv target.

    In internal SRAM, where the boot register points. Everything it needs
    comes from __p4_smp_boot, written by hart 0 before the release:
    hart 0's mtvec, so both harts take traps through __trap_entry; the top
    of hart 1's interrupt stack for mscratch (see traps.S); the C entry,
    krnP4SecondaryMain, which is in flash and is reached through that word
    rather than by name, so that nothing in this section refers to flash;
    and the top of its boot stack.
*/

	.section .sramtext.secondary_smp, "ax"
	.balign	16
	.globl	__p4_smp_entry
__p4_smp_entry:
	csrw	mie, zero
	csrci	mstatus, 8
.option push
.option norelax
	la	gp, __global_pointer$
.option pop
	la	t0, __p4_smp_boot
	lw	sp, 12(t0)
	li	tp, 0
	lw	t1, 0(t0)
	csrw	mtvec, t1
	lw	t1, 4(t0)
	csrw	mscratch, t1
	/* FPU on, state Initial, as on hart 0 before its first C call */
	li	t1, 0x2000
	csrs	mstatus, t1
	fence.i
	lw	t1, 8(t0)
	jalr	t1
1:	wfi
	j	1b
	.globl	__p4_smp_entry_end
__p4_smp_entry_end:
