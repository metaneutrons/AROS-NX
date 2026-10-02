/* Dedicated production Exec entry: no shared BSS clear or primary boot stack.
 * ROM supplies no ABI arguments. Read only the primary's uncached command.
 * The private early fault vector remains installed until local setup succeeds.
 */
#include "secondary_hw.h"
#include "p4_secondary_exec.h"
#if !defined(__AROSEXEC_SMP__)
#error "Production secondary entry requires the matched Exec SMP variant"
#endif
    .section .sramtext.p4_secondary_exec,"ax"
    .balign 64
    .globl __p4_secondary_exec_entry
    .type __p4_secondary_exec_entry,@function
__p4_secondary_exec_entry:
    csrw mie, zero
    csrci mstatus, 8
    csrw mscratch, zero
    .option push
    .option norelax
    la gp, __global_pointer$
    .option pop
    li tp, 0
    la sp, __p4_secondary_exec_stack_end
    la t0, .Lsecondary_exec_fault
    csrw mtvec, t0
    li t0, 0x2000
    csrs mstatus, t0
    fence.i
    csrr t0, mhartid
    li t1, 1
    bne t0, t1, .Lsecondary_exec_park
    la s0, __p4_secondary_exec_exchange
    li t0, P4_SECONDARY_UNCACHED_OFFSET
    add s0, s0, t0
    lw t0, 0(s0)
    li t1, P4_SECONDARY_EXEC_PREPARED
    bne t0, t1, .Lsecondary_exec_park
    fence rw, rw
    lw s1, 4(s0)
    beqz s1, .Lsecondary_exec_park
    lw s2, 8(s0)
    lw a2, 12(s0)
    mv a0, s1
    mv a1, s2
    call krnP4SecondaryExecBootstrap
    beqz a0, .Lsecondary_exec_fault
    la t0, __trap_entry
    csrw mtvec, t0
    fence.i
    sw s1, 68(s0)
    li t0, 1
    sw t0, 72(s0)
    sw zero, 76(s0)
    sw zero, 80(s0)
    csrr t0, mscratch
    sw t0, 84(s0)
    sw tp, 88(s0)
    fence rw, rw
    li t0, P4_SECONDARY_EXEC_READY
    sw t0, 64(s0)
    fence rw, rw
.Lsecondary_exec_wait_go:
    lw t0, 0(s0)
    li t1, P4_SECONDARY_EXEC_GO
    beq t0, t1, .Lsecondary_exec_go
    /* READY may not expire while hart0 publishes online. The primary owns
     * the bounded ACK timeout/reset before GO; this hart never revokes its
     * successful ACK merely because the primary has not scheduled yet. */
    j .Lsecondary_exec_wait_go
.Lsecondary_exec_go:
    fence rw, rw
    lw t0, 4(s0)
    bne t0, s1, .Lsecondary_exec_fault
    mv a0, s2
    call krnP4SecondaryExecDispatch
    j .Lsecondary_exec_fault

    .balign 64
.Lsecondary_exec_fault:
    csrw mie, zero
    csrci mstatus, 8
    la t1, __p4_secondary_exec_exchange
    li t2, P4_SECONDARY_UNCACHED_OFFSET
    add t1, t1, t2
    csrr t0, mcause
    sw t0, 76(t1)
    csrr t0, mepc
    sw t0, 80(t1)
    fence rw, rw
    li t0, P4_SECONDARY_EXEC_FAILED
    sw t0, 64(t1)
    fence rw, rw
.Lsecondary_exec_park:
    wfi
    j .Lsecondary_exec_park
    .size __p4_secondary_exec_entry, . - __p4_secondary_exec_entry
    .globl __p4_secondary_exec_entry_end
__p4_secondary_exec_entry_end:

    .section .bss.p4_secondary_exec_stack,"aw",@nobits
    .balign 64
    .globl __p4_secondary_exec_stack
__p4_secondary_exec_stack:
    .space P4_SECONDARY_EXEC_STACK_BYTES
    .globl __p4_secondary_exec_stack_end
__p4_secondary_exec_stack_end:
