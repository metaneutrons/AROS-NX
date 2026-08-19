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

    /* Copy initialized .data section from Flash LMA to SRAM VMA */
    la      t0, __data_load
    la      t1, __data_start
    la      t2, __data_end
1:
    bgeu    t1, t2, 2f
    lw      t3, 0(t0)
    sw      t3, 0(t1)
    addi    t0, t0, 4
    addi    t1, t1, 4
    j       1b

2:
    /* Clear BSS section */
    la      t0, __bss_start
    la      t1, __bss_end
3:
    bgeu    t0, t1, 4f
    sw      zero, 0(t0)
    addi    t0, t0, 4
    j       3b

4:
    /* Jump into C kernel entry */
    call    c_boot

    /* Infinite halt loop if c_boot returns */
5:
    wfi
    j       5b

.size _start, . - _start
