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

/* sie/sip interrupt-enable/pending bits */
#define SIE_SSIE            0x00000002UL /* Supervisor software (IPI) */
#define SIE_STIE            0x00000020UL /* Supervisor timer          */
#define SIE_SEIE            0x00000200UL /* Supervisor external       */

/* scause interrupt codes (with the top bit set) */
#define SCAUSE_IRQ_SSI      1
#define SCAUSE_IRQ_STI      5
#define SCAUSE_IRQ_SEI      9

/*
 * mstatus fields, for platforms that never leave machine mode. The
 * layout is the one sstatus exposes a window onto, so FS, VS, SUM and
 * MXR sit at the same bit positions and the lazy FPU and vector
 * handling above reads the same way from either CSR. What machine mode
 * adds is its own interrupt enable and its own previous-privilege
 * field.
 */
#define MSTATUS_MIE         0x00000008UL
#define MSTATUS_MPIE        0x00000080UL
#define MSTATUS_VS          0x00000600UL /* Vector unit state (V ext)   */
#define MSTATUS_VS_OFF      0x00000000UL
#define MSTATUS_VS_INITIAL  0x00000200UL
#define MSTATUS_VS_CLEAN    0x00000400UL
#define MSTATUS_VS_DIRTY    0x00000600UL
#define MSTATUS_MPP         0x00001800UL /* Privilege level trapped from */
#define MSTATUS_MPP_U       0x00000000UL
#define MSTATUS_MPP_S       0x00000800UL
#define MSTATUS_MPP_M       0x00001800UL
#define MSTATUS_FS          0x00006000UL /* FPU state                   */
#define MSTATUS_FS_OFF      0x00000000UL
#define MSTATUS_FS_INITIAL  0x00002000UL
#define MSTATUS_FS_CLEAN    0x00004000UL
#define MSTATUS_FS_DIRTY    0x00006000UL
#define MSTATUS_MPRV        0x00020000UL
#define MSTATUS_SUM         0x00040000UL
#define MSTATUS_MXR         0x00080000UL

/* mie/mip interrupt-enable/pending bits */
#define MIE_MSIE            0x00000008UL /* Machine software (IPI)    */
#define MIE_MTIE            0x00000080UL /* Machine timer             */
#define MIE_MEIE            0x00000800UL /* Machine external          */

/* mcause interrupt codes (with the top bit set) */
#define MCAUSE_IRQ_MSI      3
#define MCAUSE_IRQ_MTI      7
#define MCAUSE_IRQ_MEI      11

/*
 * Synchronous trap causes. These are numbered the same whichever mode
 * takes the trap, so they are named without a CSR prefix and read out of
 * either mcause or scause.
 */
#define CAUSE_MISALIGNED_FETCH      0
#define CAUSE_FETCH_ACCESS          1
#define CAUSE_ILLEGAL_INSTRUCTION   2
#define CAUSE_BREAKPOINT            3
#define CAUSE_MISALIGNED_LOAD       4
#define CAUSE_LOAD_ACCESS           5
#define CAUSE_MISALIGNED_STORE      6
#define CAUSE_STORE_ACCESS          7
#define CAUSE_USER_ECALL            8
#define CAUSE_SUPERVISOR_ECALL      9
#define CAUSE_MACHINE_ECALL         11
#define CAUSE_FETCH_PAGE_FAULT      12
#define CAUSE_LOAD_PAGE_FAULT       13
#define CAUSE_STORE_PAGE_FAULT      15

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
