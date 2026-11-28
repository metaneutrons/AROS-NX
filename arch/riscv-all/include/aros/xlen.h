#ifndef AROS_RISCV_XLEN_H
#define AROS_RISCV_XLEN_H

/*
    Copyright (c) 2026, The AROS Development Team. All rights reserved.
    $Id$

    Desc: register width selection for RISC-V (RV32 and RV64).
    Lang: english

    C code (genmodule.h's inline asm stubs) gets the load, store and
    register size as strings. Assembler files also get the generated
    asm.h for their CPU, and SZREG, REG_S and REG_L for their loads and
    stores; those are defined only under __ASSEMBLER__, as C sources may
    use the same names for themselves.
*/

#if (__riscv_xlen == 64)
#define __AROS_RV_LX_STR    "ld"
#define __AROS_RV_SX_STR    "sd"
#define __AROS_RV_SZREG_STR "8"
#ifdef __ASSEMBLER__
#include "aros/riscv64/asm.h"
#define SZREG   8
#define REG_S   sd
#define REG_L   ld
#endif
#elif (__riscv_xlen == 32)
#define __AROS_RV_LX_STR    "lw"
#define __AROS_RV_SX_STR    "sw"
#define __AROS_RV_SZREG_STR "4"
#ifdef __ASSEMBLER__
#include "aros/riscv/asm.h"
#define SZREG   4
#define REG_S   sw
#define REG_L   lw
#endif
#else
#error "aros/riscv/xlen.h: __riscv_xlen must be 32 or 64"
#endif

/*
    The callee-saved and argument FP registers (fs0-fs11, fa0-fa7) exist
    under every hard float ABI, but their saved width follows the ABI, not
    the hardware: 8 bytes under ILP32D/LP64D, 4 under ILP32F/LP64F. Code
    that saves them keeps 8-byte slots in every case and only picks the
    opcode, so no layout differs between the ABIs. Under a soft float ABI
    there is nothing to save and the names stay undefined.
*/
#if defined(__riscv_float_abi_double)
#define __AROS_RV_FLX_STR   "fld"
#define __AROS_RV_FSX_STR   "fsd"
#ifdef __ASSEMBLER__
#define FREG_S  fsd
#define FREG_L  fld
#endif
#elif defined(__riscv_float_abi_single)
#define __AROS_RV_FLX_STR   "flw"
#define __AROS_RV_FSX_STR   "fsw"
#ifdef __ASSEMBLER__
#define FREG_S  fsw
#define FREG_L  flw
#endif
#endif

#endif /* AROS_RISCV_XLEN_H */
