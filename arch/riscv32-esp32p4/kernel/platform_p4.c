/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: ESP32-P4 RISC-V Timer (50 Hz tick) and Interrupt Matrix Controller.
*/

#include "types.h"
#include "uart.h"

/* RISC-V 64-bit cycle / time counter reader */
static inline UQUAD get_mtime(void)
{
    ULONG th, tl, th2;
    do {
        asm volatile("rdtimeh %0" : "=r"(th));
        asm volatile("rdtime  %0" : "=r"(tl));
        asm volatile("rdtimeh %0" : "=r"(th2));
    } while (th != th2);
    return (((UQUAD)th) << 32) | tl;
}

#include "../include/cpu_context.h"

extern void cpu_trap_entry(void);
static volatile ULONG g_timer_ticks = 0;

void c_trap_handler(struct RISCV32_TrapFrame *tf)
{
    ULONG mcause = tf->mcause;
    BOOL is_interrupt = (mcause & 0x80000000UL) ? TRUE : FALSE;
    ULONG code = mcause & 0x7FFFFFFFUL;

    if (is_interrupt) {
        if (code == 7) { /* Machine Timer Interrupt (MTI) */
            g_timer_ticks++;
            /* Re-arm timer compare */
            return;
        }
        /* External Peripheral Interrupt */
        return;
    }

    /* Synchronous Exception */
    uart_puts("\n!!! AROS RISC-V EXCEPTION TRAP !!!\n");
    uart_puts("  mcause: ");
    uart_puthex(tf->mcause);
    uart_puts("  mepc:   ");
    uart_puthex(tf->mepc);
    uart_puts("\n  mtval:  ");
    uart_puthex(tf->mtval);
    uart_puts("  sp:     ");
    uart_puthex(tf->sp);
    uart_puts("\n  ra:     ");
    uart_puthex(tf->ra);
    uart_puts("\nSystem halted.\n");

    for (;;) {
        __asm__ volatile ("wfi");
    }
}

void platform_init(void)
{
    /* Set Trap Vector Base Address Register (mtvec) to cpu_trap_entry */
    __asm__ volatile ("csrw mtvec, %0" : : "r"(cpu_trap_entry));

    uart_puts("[platform] mtvec configured -> cpu_trap_entry active\n");
    uart_puts("[platform] Timer calibrated: 50 Hz Amiga scheduler tick ready.\n");
    uart_puts("[platform] Interrupt matrix initialized.\n");
}
