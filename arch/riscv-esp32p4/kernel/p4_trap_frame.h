/*
    Copyright (c) 2026, The AROS Development Team. All rights reserved.

    Desc: ESP32-P4 trap stack frame layout, shared with traps.S.
*/

#ifndef P4_TRAP_FRAME_H
#define P4_TRAP_FRAME_H

/*
 * traps.S keeps the public 136-byte RISC-V ExceptionContext unchanged at
 * the start of a 16-byte aligned stack frame. With P4_GIANT the word after
 * the context records the ISR stack top to restore into mscratch on exit,
 * or zero for a nested trap.
 */
#define P4_TRAP_BASE_FRAME_BYTES          144
#define P4_TRAP_ISR_TOP_OFFSET            136

#endif /* P4_TRAP_FRAME_H */
