#ifndef ASM_RISCV_CPU_H
#define ASM_RISCV_CPU_H

/*
    Copyright (c) 2023-2026, The AROS Development Team. All rights reserved.
    $Id$

    Desc: assembler-level specific definitions for riscv CPUs,
          usable from both RV32 and RV64 code.
    Lang: english
*/

#ifdef __cplusplus
extern "C" {
#endif

/* Memory barriers and hints */
static inline void fence(void)    { asm volatile("fence"      : : : "memory"); }
static inline void fence_r(void)  { asm volatile("fence r,rw" : : : "memory"); }
static inline void fence_w(void)  { asm volatile("fence w,rw" : : : "memory"); }
/* Zifencei is optional in the RVA22 profile - name it explicitly */
static inline void fence_i(void)
{
    asm volatile(".option push\n"
                 ".option arch, +zifencei\n"
                 "fence.i\n"
                 ".option pop" : : : "memory");
}
static inline void wfi(void)      { asm volatile("wfi"); }

/*
 * sstatus fields. The FS and VS dirty-tracking fields drive lazy FPU and
 * vector context switching: the scheduler only saves the unit's state
 * when the field reads back as DIRTY, and traps on first use when it is
 * set to OFF. VS reads as a hardwired zero when the vector extension is
 * not implemented, which is also how its presence is detected at runtime.
 */
#define SSTATUS_SIE         0x00000002UL
#define SSTATUS_SPIE        0x00000020UL
#define SSTATUS_UBE         0x00000040UL
#define SSTATUS_SPP         0x00000100UL
#define SSTATUS_VS          0x00000600UL /* Vector unit state (V ext)   */
#define SSTATUS_VS_OFF      0x00000000UL
#define SSTATUS_VS_INITIAL  0x00000200UL
#define SSTATUS_VS_CLEAN    0x00000400UL
#define SSTATUS_VS_DIRTY    0x00000600UL
#define SSTATUS_FS          0x00006000UL /* FPU state                   */
#define SSTATUS_FS_OFF      0x00000000UL
#define SSTATUS_FS_INITIAL  0x00002000UL
#define SSTATUS_FS_CLEAN    0x00004000UL
#define SSTATUS_FS_DIRTY    0x00006000UL
#define SSTATUS_SUM         0x00040000UL
#define SSTATUS_MXR         0x00080000UL

/*
 * mstatus fields a context needs to name. mstatus is a superset of
 * sstatus, but the privilege to return to does not live in the same
 * place: mret reads MPP, sret reads SPP.
 */
#define MSTATUS_MPIE        0x00000080UL /* Interrupt enable to restore */
#define MSTATUS_MPP         0x00001800UL /* Privilege level trapped from */
#define MSTATUS_MPP_U       0x00000000UL
#define MSTATUS_MPP_S       0x00000800UL
#define MSTATUS_MPP_M       0x00001800UL

/*
 * The status a freshly prepared task context is entered with.
 *
 * Because mret and sret take the privilege to return to from different
 * fields, a value that names only one of them is wrong on half the
 * machines. An SSTATUS_SPP-only value on a core that implements M-mode
 * alone leaves MPP at zero, mret returns to User, and the first
 * instruction fetch of the task faults before a single one of its
 * instructions has run - which reads as a task that cannot be entered
 * rather than as a status value that is missing a field.
 *
 * Naming both fields costs nothing, because each platform reads only the
 * half it owns: mstatus.SPP and mstatus.SPIE are read-only zero on a
 * machine without S-mode, and sstatus does not expose MPP or MPIE at
 * all, so the hardware drops the half that does not apply.
 */
#define RISCV_CTX_STATUS_INITIAL    (SSTATUS_SPP | SSTATUS_SPIE | \
                                     MSTATUS_MPP_M | MSTATUS_MPIE | \
                                     SSTATUS_FS_INITIAL)

/* sie/sip interrupt-enable/pending bits */
#define SIE_SSIE            0x00000002UL /* Supervisor software (IPI) */
#define SIE_STIE            0x00000020UL /* Supervisor timer          */
#define SIE_SEIE            0x00000200UL /* Supervisor external       */

/* scause interrupt codes (with the top bit set) */
#define SCAUSE_IRQ_SSI      1
#define SCAUSE_IRQ_STI      5
#define SCAUSE_IRQ_SEI      9

/*
 * Vector extension CSR numbers. Numeric so they assemble without V in
 * the build's -march; accessing them traps unless sstatus.VS is enabled.
 */
#define CSR_VSTART          0x008
#define CSR_VCSR            0x00F
#define CSR_VL              0xC20
#define CSR_VTYPE           0xC21
#define CSR_VLENB           0xC22

/* CSR accessors. 'csr' must be a compile time CSR name or number. */
#define csr_read(csr)                                           \
({                                                              \
    unsigned long __v;                                          \
    asm volatile("csrr %0, " #csr : "=r"(__v) : : "memory");    \
    __v;                                                        \
})

#define csr_write(csr, val)                                     \
do {                                                            \
    unsigned long __v = (unsigned long)(val);                   \
    asm volatile("csrw " #csr ", %0" : : "rK"(__v) : "memory"); \
} while (0)

#define csr_swap(csr, val)                                      \
({                                                              \
    unsigned long __v = (unsigned long)(val);                   \
    asm volatile("csrrw %0, " #csr ", %1"                       \
                 : "=r"(__v) : "rK"(__v) : "memory");           \
    __v;                                                        \
})

#define csr_set(csr, mask)                                      \
do {                                                            \
    unsigned long __v = (unsigned long)(mask);                  \
    asm volatile("csrs " #csr ", %0" : : "rK"(__v) : "memory"); \
} while (0)

#define csr_clear(csr, mask)                                    \
do {                                                            \
    unsigned long __v = (unsigned long)(mask);                  \
    asm volatile("csrc " #csr ", %0" : : "rK"(__v) : "memory"); \
} while (0)

#ifdef __cplusplus
}
#endif

#endif /* ASM_RISCV_CPU_H */
