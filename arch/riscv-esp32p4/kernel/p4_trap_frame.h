/*
    Copyright (c) 2026, The AROS Development Team. All rights reserved.

    Desc: ESP32-P4 transient trap-frame extensions for the opt-in FPU save path.
*/

#ifndef P4_TRAP_FRAME_H
#define P4_TRAP_FRAME_H

/*
 * Keep the public RISC-V ExceptionContext unchanged. With P4_E3_FPU the
 * assembly entry keeps that 136-byte context at the start of a 288-byte
 * temporary stack frame and puts the 132-byte FLEN=32 image at byte 144.
 */
#define P4_TRAP_CONTEXT_BYTES             136
#define P4_TRAP_BASE_FRAME_BYTES          144
/* Outside the public 136-byte context, before the FPU spill. */
#define P4_TRAP_ISR_TOP_OFFSET           136
#define P4_TRAP_FPU_SPILL_OFFSET          144
#define P4_TRAP_FPU_REGISTER_COUNT        32
#define P4_TRAP_FPU_REGISTER_BYTES        4
#define P4_TRAP_FPU_CSR_BYTES             4
#define P4_TRAP_FPU_SPILL_BYTES           \
    ((P4_TRAP_FPU_REGISTER_COUNT * P4_TRAP_FPU_REGISTER_BYTES) + \
     P4_TRAP_FPU_CSR_BYTES)
#define P4_TRAP_FPU_FRAME_BYTES           288

#define P4_TRAP_MSTATUS_FS_MASK           0x00006000
#define P4_TRAP_MSTATUS_FS_DIRTY          0x00006000

#ifndef __ASSEMBLER__

#include <stddef.h>

/* C mirror of the stack storage; ExceptionContext itself remains unchanged. */
struct P4TrapFrameLayout
{
    unsigned char context_and_alignment[P4_TRAP_FPU_SPILL_OFFSET];
    unsigned char fpu_spill[P4_TRAP_FPU_SPILL_BYTES];
    unsigned char padding[P4_TRAP_FPU_FRAME_BYTES -
                          P4_TRAP_FPU_SPILL_OFFSET -
                          P4_TRAP_FPU_SPILL_BYTES];
};

_Static_assert((P4_TRAP_BASE_FRAME_BYTES % 16) == 0 &&
               (P4_TRAP_FPU_FRAME_BYTES % 16) == 0,
               "both P4 trap frame sizes must preserve ABI stack alignment");
_Static_assert(offsetof(struct P4TrapFrameLayout, fpu_spill) ==
               P4_TRAP_FPU_SPILL_OFFSET,
               "P4 trap FPU spill offset must match traps.S");
_Static_assert(sizeof(struct P4TrapFrameLayout) == P4_TRAP_FPU_FRAME_BYTES,
               "P4 FPU trap frame size must match traps.S");

/* Only a dirty task image is new; INITIAL/CLEAN and OFF keep prior metadata. */
static inline int p4_trap_fpu_state_is_dirty(unsigned long saved_mstatus)
{
    return (saved_mstatus & P4_TRAP_MSTATUS_FS_MASK) ==
           P4_TRAP_MSTATUS_FS_DIRTY;
}

/*
 * Save the image captured by trap entry, never the live F registers after
 * the C trap/scheduler path may have used them. Volatile byte accesses keep
 * this copy representation-preserving and avoid type-punning FpuContext.
 */
static inline void p4_trap_fpu_copy_entry_spill(void *task_image,
                                                 const void *entry_spill)
{
    volatile unsigned char *dst = (volatile unsigned char *)task_image;
    const volatile unsigned char *src =
        (const volatile unsigned char *)entry_spill;
    size_t i;

    for (i = 0; i < P4_TRAP_FPU_SPILL_BYTES; i++)
        dst[i] = src[i];
}

#endif /* !__ASSEMBLER__ */

#endif /* P4_TRAP_FRAME_H */
