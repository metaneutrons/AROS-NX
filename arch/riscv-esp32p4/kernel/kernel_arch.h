/* P4 kernel definitions; do not import the native-RISC-V CPU header. */
#ifndef KERNEL_ARCH_ESP32P4_H
#define KERNEL_ARCH_ESP32P4_H

/*
 * P4 builds keep the generic definitions the core has always been built
 * with: IRQ_COUNT 1 and no interrupt controller hooks. IRQ_COUNT sizes
 * KernelBase, so this is ABI.
 */
#include "../../../rom/kernel/kernel_arch.h"

#endif
