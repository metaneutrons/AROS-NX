/*
    Copyright (c) 2026, The AROS Development Team. All rights reserved.
    $Id$

    Desc: esp32p4-riscv kernel CPU definitions.
*/

#ifndef KERNEL_CPU_ESP32P4_H_
#define KERNEL_CPU_ESP32P4_H_

#include <inttypes.h>

#define EXCEPTIONS_COUNT	16

/* The trap frame is the context format, so nothing is converted */
#define regs_t struct ExceptionContext
/* There are no private add-ons */
#define AROSCPUContext ExceptionContext

/*
 * Context switch protocol for the FPU (see asm/cpu.h for the mstatus FS
 * field): the kernel runs with FS enabled, because ilp32f code may touch
 * the f registers anywhere. On dispatch the scheduler sets FS=CLEAN; on
 * switch-out it saves f0-f31 and fcsr into ctx->fpuContext, marking
 * ECF_FPU, only when FS reads back DIRTY.
 *
 * FLEN is 32 here, not 64: misa reads back without D, so the registers
 * are single precision and move with fsw/flw. struct FpuContext sizes
 * itself from __riscv_flen for exactly this reason.
 *
 * There is no vector unit to track - misa has no V - so the VS half of
 * the protocol the 64bit port carries is left out rather than written
 * blind.
 */

/*
 * There is no supervisor mode here - misa has no S - so the shared code
 * reaches for mstatus through the RV_CSR_* names, which configure selects
 * with __AROSPLATFORM_RISCV_MMODE__. Touching sstatus on this machine is
 * an illegal instruction, not a read of zero.
 */

/* Synchronous exception codes in mcause (privileged spec, table 14) */
#define CAUSE_MISALIGNED_FETCH      0
#define CAUSE_FETCH_ACCESS          1
#define CAUSE_ILLEGAL_INSTRUCTION   2
#define CAUSE_BREAKPOINT            3
#define CAUSE_MISALIGNED_LOAD       4
#define CAUSE_LOAD_ACCESS           5
#define CAUSE_MISALIGNED_STORE      6
#define CAUSE_STORE_ACCESS          7
#define CAUSE_USER_ECALL            8
#define CAUSE_MACHINE_ECALL         11
#define CAUSE_FETCH_PAGE_FAULT      12

#define ADDTIME(dest, src)			        \
    (dest)->tv_micro += (src)->tv_micro;	\
    (dest)->tv_secs  += (src)->tv_secs;		\
    while((dest)->tv_micro > 999999)		\
    {						                \
        (dest)->tv_secs++;			        \
        (dest)->tv_micro -= 1000000;		\
    }

#define goSuper() 0
#define goUser()

/*
 * Kernel syscall entry. ecall, not ebreak: there is no firmware below
 * this image for ecall to belong to, and ebreak is what a debugger uses
 * for its breakpoints - which matters on a board whose USB socket is a
 * JTAG probe. The function code travels in a7, and the trap handler
 * steps over the instruction, which is always four bytes since ecall has
 * no compressed form.
 */
#undef krnSysCall
#define krnSysCall(n) \
    asm volatile ( \
    "\taddi a7, zero, %[swi_no]\n" \
    "\tecall\n" \
    : : [swi_no] "I" (n) : "a7", "memory");

#endif /* KERNEL_CPU_ESP32P4_H_ */
