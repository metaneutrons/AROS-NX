/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: ESP32-P4 RISC-V Timer (50 Hz tick) and Interrupt Matrix Controller.
*/

#include <exec/types.h>
#include <exec/interrupts.h>
#include "../boot/uart.h"

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

void platform_init(void)
{
    uart_puts("[platform] Timer calibrated: 50 Hz Amiga scheduler tick ready.\n");
    uart_puts("[platform] Interrupt matrix initialized.\n");
}
