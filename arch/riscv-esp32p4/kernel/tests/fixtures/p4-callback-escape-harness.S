    .section .text.start,"ax"
    .global _start
_start:
    la sp, stack_top
    .option push
    .option norelax
    la gp, __global_pointer$
    .option pop
    li t0, 0x6000
    csrs mstatus, t0
    call fixture_main
    li t0, 0x100000
    li t1, 0x13333
    sw t1, 0(t0)
1:  wfi
    j 1b

    .section .text,"ax"
    .global register_probe
/* Preserve our caller, then seed registers across actual nonlocal return. */
register_probe:
    addi sp, sp, -32
    sw ra, 28(sp)
    sw s1, 24(sp)
    sw s11, 20(sp)
    fsw fs0, 16(sp)
    fsw fs11, 12(sp)
    li s1, 0x13572468
    li s11, 0x24681357
    li t0, 0x3f800001
    fmv.w.x fs0, t0
    li t0, 0x40000001
    fmv.w.x fs11, t0
    call krnP4RunTaskCallback
    bnez a0, fail
    li t0, 0x13572468
    bne s1, t0, fail
    li t0, 0x24681357
    bne s11, t0, fail
    fmv.x.w t0, fs0
    li t1, 0x3f800001
    bne t0, t1, fail
    fmv.x.w t0, fs11
    li t1, 0x40000001
    bne t0, t1, fail
    flw fs11, 12(sp)
    flw fs0, 16(sp)
    lw s11, 20(sp)
    lw s1, 24(sp)
    lw ra, 28(sp)
    addi sp, sp, 32
    ret
fail:
    li t0, 0x100000
    li t1, 0x13333
    sw t1, 0(t0)
2:  wfi
    j 2b
    .section .bss,"aw",@nobits
    .balign 16
    .space 16384
stack_top:
