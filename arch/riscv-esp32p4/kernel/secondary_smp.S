/*
    Copyright (c) 2026, The AROS Development Team. All rights reserved.
    $Id$

    Desc: Hart 1's entry for the esp32p4-riscv target.

    In internal SRAM, where the boot register points. What it needs comes
    from __p4_smp_boot, written by hart 0 before the release
    (kernel_native.c): hart 0's mtvec, so both harts take traps through
    __trap_entry; the TLS riscv-native's krnRVStartCPUs() built for the
    hart, whose StackTop is its start-up and trap stack; and the C entry,
    riscv-native's cpu_Register(), which does not return.
*/

#include <aros/config.h>
#include "tls.h"

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
	lw	t1, 0(t0)
	csrw	mtvec, t1
	lw	tp, 4(t0)
	lw	sp, TLS_STACKTOP_OFFSET(tp)
	/* FPU on, state Initial, as on hart 0 before its first C call */
	li	t1, 0x2000
	csrs	mstatus, t1
	fence.i
	/* cpu_Register, which is in flash on an XIP build: reached through
	   the boot block, so that nothing in this section refers to flash */
	lw	t1, 8(t0)
	jalr	t1
1:	wfi
	j	1b
	.globl	__p4_smp_entry_end
__p4_smp_entry_end:
