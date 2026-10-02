/*
 * Copyright (c) 2026, The AROS Development Team. All rights reserved.
 *
 * Hart 1 entry for the transitional Giant Exec SMP (P4_GIANT=1).
 *
 * The ROM releases core 1 here, in internal SRAM. This is deliberately not
 * P4_SRAMCODE (.sramtext): it is a one-shot trampoline whose purpose is to
 * hand hart 1 over to ordinary flash code. It runs only while hart 0 holds
 * Forbid() inside p4_giant_start_secondary(), so no cache-off window of
 * the flash MMU code can overlap it. The flash targets come from hart 0 in
 * p4_giant_boot[] rather than being linked in.
 *
 *   p4_giant_boot[0]  mtvec (hart 0's, i.e. __trap_entry in direct mode)
 *   p4_giant_boot[1]  top of hart 1's private ISR stack, for mscratch
 *   p4_giant_boot[2]  p4_giant_secondary_main
 *   p4_giant_boot[3]  progress word for hart 0's failure report
 */

    .section .sramentry,"ax"
    .balign 64
    .globl __p4_giant_entry
    .type __p4_giant_entry,@function
__p4_giant_entry:
    csrw    mie, zero
    csrci   mstatus, 8
.option push
.option norelax
    la      gp, __global_pointer$
.option pop
    li      tp, 0
    la      sp, p4_giant_boot_stack1 + 2048
    la      t1, p4_giant_boot
    li      t0, 1
    sw      t0, 12(t1)
    fence   rw, rw
    lw      t0, 0(t1)
    csrw    mtvec, t0
    lw      t0, 4(t1)
    csrw    mscratch, t0
    /* FPU on, INITIAL, as startup.S does on hart 0 */
    li      t0, 0x2000
    csrs    mstatus, t0
    fence.i
    lw      t0, 8(t1)
    jalr    t0
1:  wfi
    j       1b
    .size __p4_giant_entry, . - __p4_giant_entry
