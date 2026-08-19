/*
    Copyright (c) 2026, The AROS Development Team. All rights reserved.
    $Id$

    Desc: esp32p4-riscv kernel internals.
*/

#ifndef KERNEL_INTERN_H_
#define KERNEL_INTERN_H_

#include <aros/libcall.h>
#include <inttypes.h>
#include <exec/lists.h>
#include <exec/execbase.h>
#include <exec/memory.h>
#include <utility/tagitem.h>
#include <stdio.h>
#include <stdarg.h>

#undef KernelBase
struct KernelBase;

#define __STR(x) #x
#define STR(x) __STR(x)

/* The hart this image was entered on, read from mhartid in startup.S */
extern unsigned long __boot_hartid;

/*
 * Depth of nested trap handling. KrnIsSuper() reports from it: on a
 * machine that never leaves machine mode the privilege level cannot tell
 * task context from kernel context, and what callers such as the exec
 * semaphores need to know is whether sleeping is possible at all. The
 * trap handler maintains it.
 */
extern int __esp32p4_trap_depth;

/*
 * Which hart this is. The shared rv32 layer's getcpunumber.c calls this
 * and expects the platform to supply it. mhartid is readable here, so
 * this answers for whichever hart asks rather than for the boot one -
 * the supervisor mode ports have to cache the boot hart id instead,
 * because mhartid is out of their reach.
 */
static inline int GetCPUNumber(void)
{
    int hartid;

    __asm__ volatile("csrr %0, mhartid" : "=r"(hartid));
    return hartid;
}

/* The core local interrupt controller (kernel_clic.c) */
void krnCLICInit(void);
void krnCLICEnable(unsigned int line, int edge);
void krnCLICDisable(unsigned int line);
void krnCLICPend(unsigned int line);
void krnCLICClear(unsigned int line);
int  krnCLICPending(unsigned int line);

/* What the trap handler has seen (kernel_traps.c) */
extern volatile unsigned long __esp32p4_irq_count;
extern volatile unsigned long __esp32p4_irq_last;

/* Machine setup that has to happen before anything else (platform_init.c) */
void platform_init(void);
int  platform_wdt_quiet(void);

/* Early UART0 debug console (kernel_console.c) */
void krnP4PutC(char c);
void krnP4PutStr(const char *s);
void krnP4PutHex32(uint32_t val);
void krnP4PutDec(uint32_t val);

#endif /* KERNEL_INTERN_H_ */
