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

#include <aros/config.h>
#include "secondary_hw.h"

_start:
	/* Mask machine interrupts until there is a dispatcher for them */
	csrw	mie, zero
	csrci	mstatus, 8		/* MSTATUS_MIE               */

#if defined(P4_SECONDARY_PROBE) || defined(__AROSEXEC_SMP__)
	/* A retained diagnostic hart must stop BEFORE shared BSS is cleared.
	   Preserve core 0 and every unrelated clock/reset bit. No stack/C call. */
	li t0, P4_SECONDARY_RESET_REG
	lw t1, 0(t0)
	mv s0, t1                 /* inherited control state, before BSS */
	ori t1, t1, P4_SECONDARY_RESET_BIT
	sw t1, 0(t0)
	fence iorw, iorw
	li t0, P4_SECONDARY_CLOCK_REG
	lw t1, 0(t0)
	mv s1, t1
	andi t1, t1, -17
	sw t1, 0(t0)
	li t0, P4_SECONDARY_BOOT_REG
	sw zero, 0(t0)
	fence iorw, iorw
	lw s4, 0(t0)
	bnez s4, __startup_hang
	li t0, P4_SECONDARY_RESET_REG
	lw s2, 0(t0)
	andi t1, s2, P4_SECONDARY_RESET_BIT
	beqz t1, __startup_hang
	li t0, P4_SECONDARY_CLOCK_REG
	lw s3, 0(t0)
	andi t1, s3, P4_SECONDARY_CLOCK_BIT
	bnez t1, __startup_hang
#endif

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
#ifdef P4_SECONDARY_PROBE
	/* These registers survive our BSS loop; snapshot only AFTER clearing it.
	   No C has run and the secondary was isolated before any BSS store. */
	la t0, __p4_secondary_boot_snapshot
	sw s0, 0(t0)
	sw s1, 4(t0)
	sw s2, 8(t0)
	sw s3, 12(t0)
	sw s4, 16(t0)
#endif
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

#ifdef P4_SECONDARY_PROBE
	.balign 16
	.globl __p4_secondary_boot_snapshot
__p4_secondary_boot_snapshot:
	.space 32
#endif
