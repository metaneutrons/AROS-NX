/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: RISC-V 32-bit entry point and vector setup for ESP32-P4.
*/

.section .text.entry, "ax"
.global _start
.type _start, @function

_start:
    /* Disable interrupts */
    csrci   mstatus, 8

    /* Set up stack pointer in high internal SRAM (768KB at 0x4FF00000) */
    lui     sp, 0x4FFC0
    addi    sp, sp, -16

    /* Clear BSS section */
    la      t0, __bss_start
    la      t1, __bss_end
1:
    bgeu    t0, t1, 2f
    sw      zero, 0(t0)
    addi    t0, t0, 4
    j       1b

2:
    /* Jump into C kernel entry */
    call    c_boot

    /* Infinite halt loop if c_boot returns */
3:
    wfi
    j       3b

.size _start, . - _start
