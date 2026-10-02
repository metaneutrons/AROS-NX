/* P4 kernel definitions; do not import the native-RISC-V CPU header. */
#ifndef KERNEL_ARCH_ESP32P4_H
#define KERNEL_ARCH_ESP32P4_H
#include "kernel_cpu.h"
#include <kernel_irqtypes.h>

/* Preserve the existing native-layer logical IRQ capacity. Actual CLIC
 * accesses remain bounded by P4_CLIC_LINES (48), not this capacity. */
#define IRQ_COUNT 72
struct KernelBase;
extern void ictl_enable_irq(irqid_t irq, struct KernelBase *KernelBase);
extern void ictl_disable_irq(irqid_t irq, struct KernelBase *KernelBase);
#endif
