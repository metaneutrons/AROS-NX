/*
 * ESP32-P4's public CPU-mask ABI is one 32-bit word.  This header is private
 * to the port: the public LVO declarations remain in rom/kernel/kernel.conf.
 */
#ifndef P4_CPU_MASKS_H
#define P4_CPU_MASKS_H

#include <stdint.h>

void *krnP4AllocCPUMask(void);
void krnP4FreeCPUMask(void *mask);
void krnP4ClearCPUMask(void *mask);
void krnP4GetCPUMask(uint32_t number, void *mask);
int krnP4CPUInMask(uint32_t number, const void *mask);

unsigned int krnP4GetCPUCount(void);

/*
 * The launch path may call this on hart 0 after it has completed its
 * secondary-hart handshake.  handshake_complete is the launch path's
 * assertion; this routine checks the caller hart and one-way state transition
 * but does not inspect the other hart's hardware state.
 */
int krnP4PublishSecondaryOnlineAfterHandshake(int handshake_complete);

#if defined(P4_CPU_MASKS_TEST)
uint32_t p4CpuMasksTestGetOnlineMask(void);
#endif

#endif /* P4_CPU_MASKS_H */
