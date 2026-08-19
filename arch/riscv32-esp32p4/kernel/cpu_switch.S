/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: RISC-V 32-bit (RV32IMAFDC) Exec Context Switch Engine
          for ESP32-P4 on AROS.
*/

    .section .text
    .align 4
    .global cpu_switch_task
    .type cpu_switch_task, @function

/*
 * void cpu_switch_task(struct RISCV32_CalContext *old_ctx, struct RISCV32_CalContext *new_ctx)
 *   a0 = old_ctx (pointer to store current task state)
 *   a1 = new_ctx (pointer to load next task state)
 */
cpu_switch_task:
    /* Check if saving old context is requested */
    beqz    a0, .L_load_new_context

    /* Save General Purpose Callee-Saved Registers */
    sw      ra,  0(a0)
    sw      sp,  4(a0)
    sw      s0,  8(a0)
    sw      s1,  12(a0)
    sw      s2,  16(a0)
    sw      s3,  20(a0)
    sw      s4,  24(a0)
    sw      s5,  28(a0)
    sw      s6,  32(a0)
    sw      s7,  36(a0)
    sw      s8,  40(a0)
    sw      s9,  44(a0)
    sw      s10, 48(a0)
    sw      s11, 52(a0)

    /* Save Callee-Saved FPU Registers (D-Extension 64-bit) */
    fsd     fs0,  56(a0)
    fsd     fs1,  64(a0)
    fsd     fs2,  72(a0)
    fsd     fs3,  80(a0)
    fsd     fs4,  88(a0)
    fsd     fs5,  96(a0)
    fsd     fs6,  104(a0)
    fsd     fs7,  112(a0)
    fsd     fs8,  120(a0)
    fsd     fs9,  128(a0)
    fsd     fs10, 136(a0)
    fsd     fs11, 144(a0)

.L_load_new_context:
    /* Load General Purpose Callee-Saved Registers */
    lw      ra,  0(a1)
    lw      sp,  4(a1)
    lw      s0,  8(a1)
    lw      s1,  12(a1)
    lw      s2,  16(a1)
    lw      s3,  20(a1)
    lw      s4,  24(a1)
    lw      s5,  28(a1)
    lw      s6,  32(a1)
    lw      s7,  36(a1)
    lw      s8,  40(a1)
    lw      s9,  44(a1)
    lw      s10, 48(a1)
    lw      s11, 52(a1)

    /* Load Callee-Saved FPU Registers */
    fld     fs0,  56(a1)
    fld     fs1,  64(a1)
    fld     fs2,  72(a1)
    fld     fs3,  80(a1)
    fld     fs4,  88(a1)
    fld     fs5,  96(a1)
    fld     fs6,  104(a1)
    fld     fs7,  112(a1)
    fld     fs8,  120(a1)
    fld     fs9,  128(a1)
    fld     fs10, 136(a1)
    fld     fs11, 144(a1)

    ret
    .size cpu_switch_task, .-cpu_switch_task
