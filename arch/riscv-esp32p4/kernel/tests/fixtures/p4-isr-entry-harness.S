/* M-mode execution fixture linked with the preprocessed production traps.S. */
	.option rvc
	.section .text.start,"ax",@progbits
	.balign 16
	.globl _start
	.type _start,@function
_start:
	csrci mstatus, 8
	csrw mie, zero
	.option push
	.option norelax
	la gp, __global_pointer$
	.option pop
	la sp, task0_stack_top
	la t0, __trap_entry
	csrw mtvec, t0
	la t0, irq_stack_top
	csrw mscratch, t0

	/* Fill both task stacks; trap entry and its C handler must not touch them. */
	la t0, task0_stack
	la t1, task0_stack_top
	li t2, 0x5aa55aa5
.Lfill_task0:
	sw t2, 0(t0)
	addi t0, t0, 4
	bltu t0, t1, .Lfill_task0
	la t0, task1_stack
	la t1, task1_stack_top
.Lfill_task1:
	sw t2, 0(t0)
	addi t0, t0, 4
	bltu t0, t1, .Lfill_task1

	la tp, regs_out
	li t0, 0x11000002
	csrw mie, zero
	li t1, 8
	csrw mie, t1
	li t1, 0x6000
	csrs mstatus, t1
	li ra, 0x11000001
	li t0, 0x11000002
	li t1, 0x11000003
	li t2, 0x11000004
	li s0, 0x11000005
	li s1, 0x11000006
	li a0, 0x11000007
	li a1, 0x11000008
	li a2, 0x11000009
	li a3, 0x1100000a
	li a4, 0x1100000b
	li a5, 0x1100000c
	li a6, 0x1100000d
	li a7, 0x1100000e
	li s2, 0x1100000f
	li s3, 0x11000010
	li s4, 0x11000011
	li s5, 0x11000012
	li s6, 0x11000013
	li s7, 0x11000014
	li s8, 0x11000015
	li s9, 0x11000016
	li s10, 0x11000017
	li s11, 0x11000018
	li t3, 0x11000019
	li t4, 0x1100001a
	li t5, 0x1100001b
	li t6, 0x1100001c
	csrsi mstatus, 8
	.globl task_ecall
task_ecall:
	ecall
	.globl task_after_ecall
task_after_ecall:

	/* Store every contexted integer register before using a scratch register. */
	sw ra, 0(tp)
	sw sp, 4(tp)
	sw t0, 8(tp)
	sw t1, 12(tp)
	sw t2, 16(tp)
	sw s0, 20(tp)
	sw s1, 24(tp)
	sw a0, 28(tp)
	sw a1, 32(tp)
	sw a2, 36(tp)
	sw a3, 40(tp)
	sw a4, 44(tp)
	sw a5, 48(tp)
	sw a6, 52(tp)
	sw a7, 56(tp)
	sw s2, 60(tp)
	sw s3, 64(tp)
	sw s4, 68(tp)
	sw s5, 72(tp)
	sw s6, 76(tp)
	sw s7, 80(tp)
	sw s8, 84(tp)
	sw s9, 88(tp)
	sw s10, 92(tp)
	sw s11, 96(tp)
	sw t3, 100(tp)
	sw t4, 104(tp)
	sw t5, 108(tp)
	sw t6, 112(tp)

	/* The outer mret must have selected Task1's stack and restored mscratch. */
	la t0, task1_stack_top
	bne sp, t0, .Lfail
	csrr t1, mscratch
	la t0, irq_stack_top
	bne t0, t1, .Lfail

	/* Callback observations, including the saved pre-migration Task SP. */
	la t0, test_failure
	lw t1, 0(t0)
	bnez t1, .Lfail
	la t0, outer_saved_sp
	lw t1, 0(t0)
	la t2, task0_stack_top
	bne t1, t2, .Lfail
	la t0, outer_marker
	lw t1, 0(t0)
	la t2, irq_stack_top
	bne t1, t2, .Lfail
	la t0, outer_scratch
	lw t1, 0(t0)
	bnez t1, .Lfail
	la t0, outer_mie
	lw t1, 0(t0)
	bnez t1, .Lfail
	la t0, nested_marker
	lw t1, 0(t0)
	bnez t1, .Lfail
	la t0, nested_scratch
	lw t1, 0(t0)
	bnez t1, .Lfail
	la t0, nested_mie
	lw t1, 0(t0)
	bnez t1, .Lfail
	la t0, nested_saved_sp
	lw t1, 0(t0)
	la t2, outer_active_sp
	lw t2, 0(t2)
	bne t1, t2, .Lfail
	la t0, post_marker
	lw t1, 0(t0)
	la t2, irq_stack_top
	bne t1, t2, .Lfail
	la t0, post_saved_sp
	lw t1, 0(t0)
	la t2, task1_stack_top
	bne t1, t2, .Lfail
	la t0, post_mie
	lw t1, 0(t0)
	bnez t1, .Lfail
	la t0, post_scratch
	lw t1, 0(t0)
	bnez t1, .Lfail
	la t0, post_pc
	lw t1, 0(t0)
	la t2, task_after_ecall
	bne t1, t2, .Lfail
	la t0, outer_after_nested_scratch
	lw t1, 0(t0)
	bnez t1, .Lfail

	/* Nested frame must end at the active handler SP, below both outer areas. */
	la t0, outer_frame
	lw t1, 0(t0)
	la t0, outer_active_sp
	lw t2, 0(t0)
	addi t0, t1, -128
	bne t0, t2, .Lfail
	la t0, nested_frame
	lw t3, 0(t0)
	addi t0, t3, 288
	bne t0, t2, .Lfail
	bgeu t3, t2, .Lfail
	bltu t1, t3, .Lfail

	/* The mask in the epilogue keeps a pending IRQ out until mret. */
	la t0, post_frame
	lw t1, 0(t0)
	la t2, irq_stack_top
	addi t2, t2, -288
	bne t1, t2, .Lfail

	/* Every task-stack word stays unchanged across entry, C, dispatch, and mret. */
	la t0, task0_stack
	la t1, task0_stack_top
	li t2, 0x5aa55aa5
.Lcheck_task0:
	lw t3, 0(t0)
	bne t3, t2, .Lfail
	addi t0, t0, 4
	bltu t0, t1, .Lcheck_task0
	la t0, task1_stack
	la t1, task1_stack_top
.Lcheck_task1:
	lw t3, 0(t0)
	bne t3, t2, .Lfail
	addi t0, t0, 4
	bltu t0, t1, .Lcheck_task1
	la t0, irq_guard_low
	li t1, 16
.Lcheck_guard_low:
	lw t2, 0(t0)
	bnez t2, .Lfail
	addi t0, t0, 4
	addi t1, t1, -1
	bnez t1, .Lcheck_guard_low
	la t0, irq_guard_high
	li t1, 16
.Lcheck_guard_high:
	lw t2, 0(t0)
	bnez t2, .Lfail
	addi t0, t0, 4
	addi t1, t1, -1
	bnez t1, .Lcheck_guard_high

	/* Verify every saved integer register except the deliberately switched SP. */
	li t0, 0x11000001
	lw t1, 0(tp)
	bne t0, t1, .Lfail
	li t0, 0x11000002
	lw t1, 8(tp)
	bne t0, t1, .Lfail
	li t0, 0x11000003
	lw t1, 12(tp)
	bne t0, t1, .Lfail
	li t0, 0x11000004
	lw t1, 16(tp)
	bne t0, t1, .Lfail
	li t0, 0x11000005
	lw t1, 20(tp)
	bne t0, t1, .Lfail
	li t0, 0x11000006
	lw t1, 24(tp)
	bne t0, t1, .Lfail
	li t0, 0x11000007
	lw t1, 28(tp)
	bne t0, t1, .Lfail
	li t0, 0x11000008
	lw t1, 32(tp)
	bne t0, t1, .Lfail
	li t0, 0x11000009
	lw t1, 36(tp)
	bne t0, t1, .Lfail
	li t0, 0x1100000a
	lw t1, 40(tp)
	bne t0, t1, .Lfail
	li t0, 0x1100000b
	lw t1, 44(tp)
	bne t0, t1, .Lfail
	li t0, 0x1100000c
	lw t1, 48(tp)
	bne t0, t1, .Lfail
	li t0, 0x1100000d
	lw t1, 52(tp)
	bne t0, t1, .Lfail
	li t0, 0x1100000e
	lw t1, 56(tp)
	bne t0, t1, .Lfail
	li t0, 0x1100000f
	lw t1, 60(tp)
	bne t0, t1, .Lfail
	li t0, 0x11000010
	lw t1, 64(tp)
	bne t0, t1, .Lfail
	li t0, 0x11000011
	lw t1, 68(tp)
	bne t0, t1, .Lfail
	li t0, 0x11000012
	lw t1, 72(tp)
	bne t0, t1, .Lfail
	li t0, 0x11000013
	lw t1, 76(tp)
	bne t0, t1, .Lfail
	li t0, 0x11000014
	lw t1, 80(tp)
	bne t0, t1, .Lfail
	li t0, 0x11000015
	lw t1, 84(tp)
	bne t0, t1, .Lfail
	li t0, 0x11000016
	lw t1, 88(tp)
	bne t0, t1, .Lfail
	li t0, 0x11000017
	lw t1, 92(tp)
	bne t0, t1, .Lfail
	li t0, 0x11000018
	lw t1, 96(tp)
	bne t0, t1, .Lfail
	li t0, 0x11000019
	lw t1, 100(tp)
	bne t0, t1, .Lfail
	li t0, 0x1100001a
	lw t1, 104(tp)
	bne t0, t1, .Lfail
	li t0, 0x1100001b
	lw t1, 108(tp)
	bne t0, t1, .Lfail
	li t0, 0x1100001c
	lw t1, 112(tp)
	bne t0, t1, .Lfail

	li t0, 0x00100000
	li t1, 0x5555
	sw t1, 0(t0)
.Lstop:
	j .Lstop
.Lfail:
	li t0, 0x00100000
	li t1, 0x3333
	sw t1, 0(t0)
	j .Lstop
	.size _start, . - _start

	/* Mock kernel C callback, kept in assembly so no runtime is linked. */
	.section .text,"ax",@progbits
	.balign 4
	.globl krnTrapHandler
	.type krnTrapHandler,@function
krnTrapHandler:
	la t0, callback_depth
	lw t1, 0(t0)
	addi t1, t1, 1
	sw t1, 0(t0)
	li t2, 11
	beq a1, t2, .Louter_ecall
	li t2, 0x80000003
	bne a1, t2, .Lhandler_bad
	li t2, 2
	beq t1, t2, .Lnested_irq
	li t2, 1
	beq t1, t2, .Lpost_mret_irq
	j .Lhandler_bad

.Louter_ecall:
	li t2, 1
	bne t1, t2, .Lhandler_bad
	la t0, outer_frame
	sw a0, 0(t0)
	la t0, outer_handler_sp
	sw sp, 0(t0)
	lw t0, 4(a0)
	la t1, outer_saved_sp
	sw t0, 0(t1)
	lw t0, 136(a0)
	la t1, outer_marker
	sw t0, 0(t1)
	csrr t0, mscratch
	la t1, outer_scratch
	sw t0, 0(t1)
	csrr t0, mstatus
	andi t0, t0, 8
	la t1, outer_mie
	sw t0, 0(t1)
	beqz t0, .Louter_mie_ok
	j .Lhandler_bad
.Louter_mie_ok:
	addi sp, sp, -128
	li t0, 0x6a11c0de
	sw t0, 0(sp)
	sw t0, 124(sp)
	la t0, outer_active_sp
	sw sp, 0(t0)
	li t0, 0x02000000
	li t1, 1
	sw t1, 0(t0)
	fence iorw, iorw
	csrsi mstatus, 8
.Lafter_nested_irq:
	csrci mstatus, 8
	csrr t0, mscratch
	la t1, outer_after_nested_scratch
	sw t0, 0(t1)
	bnez t0, .Lhandler_bad
	lw t0, 0(sp)
	li t1, 0x6a11c0de
	bne t0, t1, .Lhandler_bad
	lw t0, 124(sp)
	bne t0, t1, .Lhandler_bad
	addi sp, sp, 128
	la t0, task1_stack_top
	sw t0, 4(a0)
	lw t0, 116(a0)
	addi t0, t0, 4
	sw t0, 116(a0)
	/* Pending IRQ must wait until mret even if saved sr has MIE set. */
	lw t0, 120(a0)
	ori t0, t0, 8
	sw t0, 120(a0)
	li t0, 0x02000000
	li t1, 1
	sw t1, 0(t0)
	fence iorw, iorw
	la t0, callback_depth
	sw zero, 0(t0)
	ret

.Lnested_irq:
	la t0, nested_frame
	sw a0, 0(t0)
	lw t0, 4(a0)
	la t1, nested_saved_sp
	sw t0, 0(t1)
	lw t0, 136(a0)
	la t1, nested_marker
	sw t0, 0(t1)
	csrr t0, mscratch
	la t1, nested_scratch
	sw t0, 0(t1)
	csrr t0, mstatus
	andi t0, t0, 8
	la t1, nested_mie
	sw t0, 0(t1)
	bnez t0, .Lhandler_bad
	/* The nested frame ends exactly at the interrupted outer C SP. */
	addi t0, a0, 288
	la t1, outer_active_sp
	lw t1, 0(t1)
	bne t0, t1, .Lhandler_bad
	la t0, outer_frame
	lw t0, 0(t0)
	bgeu a0, t0, .Lhandler_bad
	li t0, 0x02000000
	sw zero, 0(t0)
	fence iorw, iorw
	la t0, callback_depth
	li t1, 1
	sw t1, 0(t0)
	ret

.Lpost_mret_irq:
	la t0, post_frame
	sw a0, 0(t0)
	lw t0, 4(a0)
	la t1, post_saved_sp
	sw t0, 0(t1)
	lw t0, 136(a0)
	la t1, post_marker
	sw t0, 0(t1)
	lw t0, 116(a0)
	la t1, post_pc
	sw t0, 0(t1)
	csrr t0, mscratch
	la t1, post_scratch
	sw t0, 0(t1)
	csrr t0, mstatus
	andi t0, t0, 8
	la t1, post_mie
	sw t0, 0(t1)
	bnez t0, .Lhandler_bad
	li t0, 0x02000000
	sw zero, 0(t0)
	fence iorw, iorw
	la t0, callback_depth
	sw zero, 0(t0)
	ret

.Lhandler_bad:
	la t0, test_failure
	li t1, 1
	sw t1, 0(t0)
	/* Clear software IRQ so one failed assertion cannot livelock QEMU. */
	li t0, 0x02000000
	sw zero, 0(t0)
	fence iorw, iorw
	li t0, 0x00100000
	li t1, 0x3333
	sw t1, 0(t0)
.Lhandler_stop:
	wfi
	j .Lhandler_stop
	.size krnTrapHandler, . - krnTrapHandler

	.section .bss,"aw",@nobits
	.balign 16
task0_stack:
	.space 1024
task0_stack_top:
	.balign 16
task1_stack:
	.space 1024
task1_stack_top:
	.balign 64
irq_guard_low:
	.space 64
irq_stack:
	.space 16384
irq_stack_top:
irq_guard_high:
	.space 64
	.balign 4
regs_out:
	.space 116
test_failure:
	.word 0
callback_depth:
	.word 0
outer_frame:
	.word 0
outer_handler_sp:
	.word 0
outer_active_sp:
	.word 0
outer_saved_sp:
	.word 0
outer_marker:
	.word 0
outer_scratch:
	.word 0
outer_mie:
	.word 0
outer_after_nested_scratch:
	.word 0
nested_frame:
	.word 0
nested_saved_sp:
	.word 0
nested_marker:
	.word 0
nested_scratch:
	.word 0
nested_mie:
	.word 0
post_frame:
	.word 0
post_saved_sp:
	.word 0
post_marker:
	.word 0
post_scratch:
	.word 0
post_mie:
	.word 0
post_pc:
	.word 0
