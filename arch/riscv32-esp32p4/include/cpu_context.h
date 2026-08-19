/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: RISC-V 32-bit (RV32IMAFDC) Exec Task Context & Trap Frame
          for ESP32-P4 on AROS.
*/

#ifndef RISCV32_CPU_CONTEXT_H
#define RISCV32_CPU_CONTEXT_H

#include "types.h"

/*
 * Callee-saved context saved during voluntary task switch (Switch / Dispatch)
 */
struct RISCV32_CalContext
{
    /* General Purpose Callee-Saved Registers */
    ULONG   ra;     /* x1  Return Address */
    ULONG   sp;     /* x2  Stack Pointer */
    ULONG   s0;     /* x8  Saved Register 0 / Frame Pointer */
    ULONG   s1;     /* x9  Saved Register 1 */
    ULONG   s2;     /* x18 Saved Register 2 */
    ULONG   s3;     /* x19 Saved Register 3 */
    ULONG   s4;     /* x20 Saved Register 4 */
    ULONG   s5;     /* x21 Saved Register 5 */
    ULONG   s6;     /* x22 Saved Register 6 */
    ULONG   s7;     /* x23 Saved Register 7 */
    ULONG   s8;     /* x24 Saved Register 8 */
    ULONG   s9;     /* x25 Saved Register 9 */
    ULONG   s10;    /* x26 Saved Register 10 */
    ULONG   s11;    /* x27 Saved Register 11 */

    /* FPU Callee-Saved Double Registers (FS0-FS11, d-extension) */
    UQUAD   fs0;
    UQUAD   fs1;
    UQUAD   fs2;
    UQUAD   fs3;
    UQUAD   fs4;
    UQUAD   fs5;
    UQUAD   fs6;
    UQUAD   fs7;
    UQUAD   fs8;
    UQUAD   fs9;
    UQUAD   fs10;
    UQUAD   fs11;
};

/*
 * Full Exception / Interrupt Trap Frame (stacked on mtvec entry)
 */
struct RISCV32_TrapFrame
{
    ULONG   mepc;
    ULONG   mstatus;
    ULONG   mcause;
    ULONG   mtval;

    /* All 32 Integer Registers */
    ULONG   ra, sp, gp, tp;
    ULONG   t0, t1, t2;
    ULONG   s0, s1;
    ULONG   a0, a1, a2, a3, a4, a5, a6, a7;
    ULONG   s2, s3, s4, s5, s6, s7, s8, s9, s10, s11;
    ULONG   t3, t4, t5, t6;
};

/* Assembly Functions */
extern void cpu_switch_task(struct RISCV32_CalContext *old_ctx, struct RISCV32_CalContext *new_ctx);
extern void cpu_trap_entry(void);

#endif /* RISCV32_CPU_CONTEXT_H */
