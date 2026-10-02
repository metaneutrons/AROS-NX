/* P4 SMP per-hart interrupt stacks; not a hart-launch implementation. */
#ifndef P4_ISR_STACK_H
#define P4_ISR_STACK_H
#include <stdint.h>
#define P4_ISR_STACK_HARTS 2u
#define P4_ISR_STACK_BYTES 16384u
#define P4_ISR_STACK_GUARD_BYTES 64u
#define P4_ISR_STACK_C_MARGIN_BYTES 2048u
#define P4_ISR_STACK_MAX_DEPTH 8u
/* Owner hart only, once, MIE clear, mscratch zero, before its first trap.
 * Hart0 installs after SRAM/BSS setup; hart1 after the release handshake.
 * Caller must not enable interrupts or expose tasks after an install error. */
int krnP4ISRStackInstall(void);
/* Owner-only range/guard check before trap handling. No Task dereference.
 * Margin is a conservative fixed reserve, not proven C-stack usage. */
int krnP4ISRStackFrameValid(const void *frame, uint32_t frame_bytes);
#endif
