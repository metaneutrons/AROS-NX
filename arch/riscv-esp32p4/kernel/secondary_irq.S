#include "secondary_hw.h"
    .section .sramtext.secondary_mailbox,"ax"
    .balign 64
    .globl __p4_e2_irq
    .type __p4_e2_irq,@function
__p4_e2_irq:
    /* No nesting or FP use. Preserve every caller-clobbered integer register
       and trap CSRs; C preserves s0-s11. gp/tp are never modified here. */
    addi sp, sp, -80
    sw ra, 0(sp)
    sw t0, 4(sp)
    sw t1, 8(sp)
    sw t2, 12(sp)
    sw t3, 16(sp)
    sw t4, 20(sp)
    sw t5, 24(sp)
    sw t6, 28(sp)
    sw a0, 32(sp)
    sw a1, 36(sp)
    sw a2, 40(sp)
    sw a3, 44(sp)
    sw a4, 48(sp)
    sw a5, 52(sp)
    sw a6, 56(sp)
    sw a7, 60(sp)
    csrr t0, mepc
    sw t0, 64(sp)
    csrr t0, mstatus
    sw t0, 68(sp)
    csrr t0, mcause
    sw t0, 72(sp)
    call krnP4E2IRQ
    beqz a0, 1f
    lw t0, 64(sp)
    csrw mepc, t0
    lw t0, 68(sp)
    csrw mstatus, t0
    lw t0, 72(sp)
    csrw mcause, t0
    lw ra, 0(sp)
    lw t0, 4(sp)
    lw t1, 8(sp)
    lw t2, 12(sp)
    lw t3, 16(sp)
    lw t4, 20(sp)
    lw t5, 24(sp)
    lw t6, 28(sp)
    lw a0, 32(sp)
    lw a1, 36(sp)
    lw a2, 40(sp)
    lw a3, 44(sp)
    lw a4, 48(sp)
    lw a5, 52(sp)
    lw a6, 56(sp)
    lw a7, 60(sp)
    addi sp, sp, 80
    mret
1:  csrci mstatus, 8
    wfi
    j 1b
    .size __p4_e2_irq, . - __p4_e2_irq
