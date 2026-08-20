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

/* What memory there is (kernel_ram.c) */
void krnRAMInit(void);
void krnRAMReport(void);
extern struct MemHeader *__esp32p4_mh_low;
extern struct MemHeader *__esp32p4_mh_high;

/* The periodic tick and the only clock there is (kernel_timer.c) */
void krnTimerInit(void);
void krnTimerAck(void);
uint64_t krnTimerCount(void);
unsigned long krnTimerTicks(void);
int  krnTimerWait(unsigned long ticks);
extern volatile unsigned long __esp32p4_ticks;

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

/*
 * Code and data that must be in SRAM whatever the link script does.
 *
 * With ldscript-xip.lds the image's .text and .rodata are mapped from
 * flash through the cache, which is fine until something has to touch the
 * controller that serves that cache - reconfiguring MSPI, bringing PSRAM
 * up, changing flash timing. Such code has to be fetched from somewhere
 * else while the cache is off, and so does every byte it reads and every
 * function it calls. ESP-IDF spells this IRAM_ATTR; here it is these two.
 *
 * The discipline they cannot enforce: a P4_SRAMCODE function may only call
 * other P4_SRAMCODE functions and touch P4_SRAMDATA while the cache is
 * disabled. noinline keeps the compiler from copying the body into a
 * caller that lives in flash, which would defeat the point silently.
 */
#define P4_SRAMCODE __attribute__((section(".sramtext"), noinline, used))
#define P4_SRAMDATA __attribute__((section(".sramdata"), used))

#endif /* KERNEL_INTERN_H_ */
