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
