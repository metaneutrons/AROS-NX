/*
 * Copyright (c) 2026, The AROS Development Team. All rights reserved.
 *
 * Per-task all-register FPU isolation workload.  Kept in assembly so there
 * are no C calls or compiler-managed FP values between initialization and
 * verification.  This tests hart-0 task context switches, not SMP execution.
 */

    .option push
    .option norvc
    .text
    .align 2

    .macro LOAD_FPR reg, index
        addi t0, s3, \index
        fmv.w.x \reg, t0
    .endm

    .macro CHECK_FPR reg, index
        addi t0, s3, \index
        fmv.x.w t1, \reg
        beq t0, t1, .Lok\@
        li a2, \index
        j .Lfailed
.Lok\@:
    .endm

    .globl krnP4FpuProbeTask
    .type krnP4FpuProbeTask, @function
krnP4FpuProbeTask:
    /* Args: task index, ExecBase->DispCount, progress[2], timer ticks. */
    mv s1, a0
    mv s0, a1
    mv s11, a3
    lw s2, 0(s0)
    lw a3, 0(s11)

    beqz s1, .Ltask0_pattern
    li s3, 0xbf000200
    li s4, 0x40                  /* FCSR frm=2, fflags=0. */
    addi s8, a2, 4
    mv s9, a2
    j .Lpattern_ready
.Ltask0_pattern:
    li s3, 0x3f000100
    li s4, 0x20                  /* FCSR frm=1, fflags=0. */
    mv s8, a2
    addi s9, a2, 4
.Lpattern_ready:
    li s10, 0
    li s5, 720000000             /* Counter budget; wall-time is uncalibrated. */
    li s6, 20000000              /* Bound a stalled/broken cycle source. */

    LOAD_FPR f0, 0
    LOAD_FPR f1, 1
    LOAD_FPR f2, 2
    LOAD_FPR f3, 3
    LOAD_FPR f4, 4
    LOAD_FPR f5, 5
    LOAD_FPR f6, 6
    LOAD_FPR f7, 7
    LOAD_FPR f8, 8
    LOAD_FPR f9, 9
    LOAD_FPR f10, 10
    LOAD_FPR f11, 11
    LOAD_FPR f12, 12
    LOAD_FPR f13, 13
    LOAD_FPR f14, 14
    LOAD_FPR f15, 15
    LOAD_FPR f16, 16
    LOAD_FPR f17, 17
    LOAD_FPR f18, 18
    LOAD_FPR f19, 19
    LOAD_FPR f20, 20
    LOAD_FPR f21, 21
    LOAD_FPR f22, 22
    LOAD_FPR f23, 23
    LOAD_FPR f24, 24
    LOAD_FPR f25, 25
    LOAD_FPR f26, 26
    LOAD_FPR f27, 27
    LOAD_FPR f28, 28
    LOAD_FPR f29, 29
    LOAD_FPR f30, 30
    LOAD_FPR f31, 31
    csrw fcsr, s4
    csrr s7, mcycle
    li t4, 0

.Lcheck_loop:
    CHECK_FPR f0, 0
    CHECK_FPR f1, 1
    CHECK_FPR f2, 2
    CHECK_FPR f3, 3
    CHECK_FPR f4, 4
    CHECK_FPR f5, 5
    CHECK_FPR f6, 6
    CHECK_FPR f7, 7
    CHECK_FPR f8, 8
    CHECK_FPR f9, 9
    CHECK_FPR f10, 10
    CHECK_FPR f11, 11
    CHECK_FPR f12, 12
    CHECK_FPR f13, 13
    CHECK_FPR f14, 14
    CHECK_FPR f15, 15
    CHECK_FPR f16, 16
    CHECK_FPR f17, 17
    CHECK_FPR f18, 18
    CHECK_FPR f19, 19
    CHECK_FPR f20, 20
    CHECK_FPR f21, 21
    CHECK_FPR f22, 22
    CHECK_FPR f23, 23
    CHECK_FPR f24, 24
    CHECK_FPR f25, 25
    CHECK_FPR f26, 26
    CHECK_FPR f27, 27
    CHECK_FPR f28, 28
    CHECK_FPR f29, 29
    CHECK_FPR f30, 30
    CHECK_FPR f31, 31

    csrr t0, fcsr
    beq t0, s4, .Lfcsr_ok
    li a2, 32
    j .Lfailed
.Lfcsr_ok:
    addi t4, t4, 1
    sw t4, 0(s8)

    /* A nonzero, not-yet-finished peer value proves actual task interleave. */
    lw t0, 0(s9)
    bltz t0, .Lpeer_not_running
    beqz t0, .Lpeer_not_running
    li s10, 1
.Lpeer_not_running:
    csrr t0, mcycle
    sub t0, t0, s7
    bgeu t0, s5, .Ltime_done
    bgeu t4, s6, .Literation_limit
    j .Lcheck_loop

.Ltime_done:
    li a1, 1
    li a2, 0
    j .Lreport
.Literation_limit:
    li a1, 0
    li a2, 34
    j .Lreport
.Lfailed:
    li a1, 0
    j .Lreport

.Lreport:
    /* Additional pass conditions make serial runs and dead timers fail. */
    lw a4, 0(s0)
    sub a4, a4, s2
    lw t0, 0(s11)
    sub a6, t0, a3
    beqz a1, .Lmark_finished
    bnez a4, .Lcheck_ticks
    li a1, 0
    li a2, 33
    j .Lmark_finished
.Lcheck_ticks:
    bnez a6, .Lcheck_interleave
    li a1, 0
    li a2, 35
    j .Lmark_finished
.Lcheck_interleave:
    bnez s10, .Lmark_finished
    li a1, 0
    li a2, 36

.Lmark_finished:
    li t0, 0x80000000
    or t0, t0, t4
    sw t0, 0(s8)
    mv a0, s1
    mv a3, t4
    mv a5, s10
    tail krnP4FpuProbeFinish

    .size krnP4FpuProbeTask, .-krnP4FpuProbeTask
    .option pop
