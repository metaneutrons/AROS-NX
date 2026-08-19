/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: RISC-V 32-bit (RV32IMAFDC) Exception & Interrupt Trap Handler
          for ESP32-P4 on AROS.
*/

    .section .text
    .align 4
    .global cpu_trap_entry
    .type cpu_trap_entry, @function

cpu_trap_entry:
    /* Allocate 128-byte trap frame on current stack */
    addi    sp, sp, -128

    /* Save caller-saved and general purpose registers */
    sw      ra,  0(sp)
    sw      t0,  4(sp)
    sw      t1,  8(sp)
    sw      t2,  12(sp)
    sw      a0,  16(sp)
    sw      a1,  20(sp)
    sw      a2,  24(sp)
    sw      a3,  28(sp)
    sw      a4,  32(sp)
    sw      a5,  36(sp)
    sw      a6,  40(sp)
    sw      a7,  44(sp)
    sw      t3,  48(sp)
    sw      t4,  52(sp)
    sw      t5,  56(sp)
    sw      t6,  60(sp)

    /* Read CSRs */
    csrr    t0, mepc
    csrr    t1, mstatus
    csrr    t2, mcause
    csrr    t3, mtval

    sw      t0, 64(sp)
    sw      t1, 68(sp)
    sw      t2, 72(sp)
    sw      t3, 76(sp)

    /* Pass Trap Frame pointer in a0 */
    mv      a0, sp
    call    c_trap_handler

    /* Restore CSRs */
    lw      t0, 64(sp)
    lw      t1, 68(sp)
    csrw    mepc, t0
    csrw    mstatus, t1

    /* Restore Registers */
    lw      ra,  0(sp)
    lw      t0,  4(sp)
    lw      t1,  8(sp)
    lw      t2,  12(sp)
    lw      a0,  16(sp)
    lw      a1,  20(sp)
    lw      a2,  24(sp)
    lw      a3,  28(sp)
    lw      a4,  32(sp)
    lw      a5,  36(sp)
    lw      a6,  40(sp)
    lw      a7,  44(sp)
    lw      t3,  48(sp)
    lw      t4,  52(sp)
    lw      t5,  56(sp)
    lw      t6,  60(sp)

    addi    sp, sp, 128
    mret
    .size cpu_trap_entry, .-cpu_trap_entry
