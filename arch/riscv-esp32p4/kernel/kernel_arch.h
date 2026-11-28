/*
    Copyright (c) 2026, The AROS Development Team. All rights reserved.
    $Id$

    Desc: Machine-specific kernel definitions for the esp32p4-riscv target.

    Replaces rom/kernel/kernel_arch.h, whose defaults turn the interrupt
    controller calls into no-ops.
*/

#ifndef KERNEL_ARCH_H
#define KERNEL_ARCH_H

#include <kernel_cpu.h>        /* riscv-native's */
#include <kernel_irqtypes.h>   /* irqid_t - this header may be reached without kernel_base.h */

/*
 * Interrupt numbers are CLIC lines. A device's interrupt matrix source is
 * routed to a line by its driver; the tick (20) and the inter-hart
 * interrupt (22) are the kernel's own.
 */
#define IRQ_COUNT       48
#define HW_IRQ_COUNT    IRQ_COUNT

/* Interrupt controller, over the CLIC (kernel_clic.c) */
extern void ictl_enable_irq(irqid_t irq, struct KernelBase *KernelBase);
extern void ictl_disable_irq(irqid_t irq, struct KernelBase *KernelBase);

#endif /* KERNEL_ARCH_H */
