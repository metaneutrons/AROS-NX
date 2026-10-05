/*
 * Copyright (c) 2026, The AROS Development Team. All rights reserved.
 *
 * Isolated HP-hart entry foundation, not an Exec SMP entry.
 * No BSS clearing, shared boot stack, peripheral access or console.
 * E2_MAILBOX alone calls the private bounded SRAM worker, never shared C boot.
 * The release controller must initialize/report this private mailbox before
 * launch and prove its memory visibility before interpreting these stores.
 * P4_SECONDARY_PROBE alone enables the bounded diagnostic launcher.
 */
#include "secondary_hw.h"

    .section .sramtext.secondary,"ax"
    .balign 64
    .globl __p4_secondary_entry
    .type __p4_secondary_entry,@function
__p4_secondary_entry:
    csrw mie, zero
    csrci mstatus, 8
    .option push
    .option norelax
    la gp, __global_pointer$
    .option pop
    la sp, __p4_secondary_stack_end
    li tp, 0
#ifdef P4_E2_PRIMITIVES
    la t0, __p4_e2_irq
#else
    la t0, __p4_secondary_trap
#endif
    csrw mtvec, t0
    fence.i
    csrr t0, mhartid
    la t1, __p4_secondary_report
    li t2, P4_SECONDARY_UNCACHED_OFFSET
    add t1, t1, t2
    sw t0, 0(t1)
    lw t0, 16(t1)             /* launch generation */
    sw t0, 20(t1)
    sw sp, 24(t1)
    fence rw, rw
    li t0, 1                   /* entered; not scheduled or qualified */
    sw t0, 4(t1)
    fence rw, rw
#ifdef P4_E2_MAILBOX
    call krnP4SecondaryMailboxWorker
#endif
1:  wfi
    j 1b
    .size __p4_secondary_entry, . - __p4_secondary_entry

    .balign 64
    .globl __p4_secondary_trap
    .type __p4_secondary_trap,@function
__p4_secondary_trap:
    csrw mie, zero
    csrci mstatus, 8
    la t1, __p4_secondary_report
    li t2, P4_SECONDARY_UNCACHED_OFFSET
    add t1, t1, t2
    csrr t0, mcause
    sw t0, 8(t1)
    csrr t0, mepc
    sw t0, 12(t1)
    fence rw, rw
    li t0, 2                   /* fault; do not enter the shared trap path */
    sw t0, 4(t1)
    fence rw, rw
2:  wfi
    j 2b
    .size __p4_secondary_trap, . - __p4_secondary_trap
    .globl __p4_secondary_entry_end
__p4_secondary_entry_end:

    .section .bss.secondary,"aw",@nobits
    .balign 64
    .globl __p4_secondary_report
__p4_secondary_report:
    .space 64                 /* hart/state/fault/nonce/echo/SP; own line */
    .balign 64
    .globl __p4_secondary_guard_low
__p4_secondary_guard_low:
    .space 64
    .globl __p4_secondary_stack
__p4_secondary_stack:
    .space 4096
    .globl __p4_secondary_stack_end
__p4_secondary_stack_end:
    .globl __p4_secondary_guard_high
__p4_secondary_guard_high:
    .space 64
