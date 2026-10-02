/* P4 kernel definitions; do not import the native-RISC-V CPU header. */
#ifndef KERNEL_ARCH_ESP32P4_H
#define KERNEL_ARCH_ESP32P4_H

#if defined(__AROSEXEC_SMP__)
#include "kernel_cpu.h"
#include <kernel_irqtypes.h>

/* Preserve the existing native-layer logical IRQ capacity. Actual CLIC
 * accesses remain bounded by P4_CLIC_LINES (48), not this capacity. */
#define IRQ_COUNT 72
struct KernelBase;
extern void ictl_enable_irq(irqid_t irq, struct KernelBase *KernelBase);
extern void ictl_disable_irq(irqid_t irq, struct KernelBase *KernelBase);
#else
/*
 * Every other P4 build, including P4_GIANT, keeps the generic definitions
 * the normal core has always been built with: IRQ_COUNT 1 and no interrupt
 * controller hooks. IRQ_COUNT sizes KernelBase, so this is ABI.
 */
#include "../../../rom/kernel/kernel_arch.h"
#endif

#endif
