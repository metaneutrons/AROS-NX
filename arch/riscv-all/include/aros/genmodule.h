/*
    Copyright (C) 2023-2026, The AROS Development Team. All rights reserved.

    Desc: genmodule.h include file for risc-v (RV32 and RV64) systems.

    Library-call stubs jump through the library's downward-growing vector
    table: the function for LVO n lives at (libbase - n*LIB_VECTSIZE), and
    each JumpVec slot holds a register-width function pointer. The stubs
    hand the libbase to the library-side thunk in t6 - a temporary register
    that is never used to pass arguments, so all eight integer (a0-a7) and
    eight FP (fa0-fa7) argument registers stay untouched.
*/

#ifndef AROS_RISCV_GENMODULE_H
#define AROS_RISCV_GENMODULE_H

#include <exec/execbase.h>
#include <aros/riscv/xlen.h>

/* Register-width load and store, and the frame the stubs below save the
 * argument registers in: fa0-fa7 at 0-56, then a0-a7 and ra in register-width
 * slots from 64, rounded up to 16 bytes (144 on RV64, 112 on RV32). */
#define __AROS_GM_RV_LX     __AROS_RV_LX_STR
#define __AROS_GM_RV_SX     __AROS_RV_SX_STR
#define __AROS_GM_RV_SZREG  __AROS_RV_SZREG_STR
#define __AROS_GM_RV_FRAME  "((64+9*" __AROS_GM_RV_SZREG "+15)&~15)"
/* Integer slot k (a0 = 0 ... a7 = 7, ra = 8) */
#define __AROS_GM_RV_SLOT(k) "64+" #k "*" __AROS_GM_RV_SZREG "(sp)"
/* Save and restore of fa<n> in its 8-byte slot, with the opcode the float
 * ABI needs (see aros/riscv/xlen.h); nothing under a soft float ABI, where
 * no FP argument registers exist. The frame layout is the same either way. */
#if defined(__AROS_RV_FSX_STR)
#define __AROS_GM_RV_FS(n)  "\t" __AROS_RV_FSX_STR "  fa" #n ", " #n "*8(sp)\n"
#define __AROS_GM_RV_FL(n)  "\t" __AROS_RV_FLX_STR "  fa" #n ", " #n "*8(sp)\n"
#else
#define __AROS_GM_RV_FS(n)  ""
#define __AROS_GM_RV_FL(n)  ""
#endif

/* Macros for generating library stub functions and aliases for stack libcalls. */

/******************* Linklib Side Thunks ******************/

/* Macro: AROS_GM_LIBFUNCSTUB(functionname, libbasename, lvo)
   Generates a stub for 'functionname' of the library whose base pointer is
   the global 'libbasename'. It loads the base, indexes the vector table at
   -lvo and tail-jumps to the function. lvo must be a compile-time constant.
*/
#define __AROS_GM_LIBFUNCSTUB(fname, libbasename, lvo)                     \
    void __ ## fname ## _ ## libbasename ## _wrapper(void)                 \
    {                                                                      \
        asm volatile(                                                      \
            ".weak " #fname "\n"                                           \
            ".type " #fname ", %%function\n"                               \
            #fname " :\n"                                                  \
            "\tla   t6, " #libbasename "\n" /* t6 = &libbasename        */ \
            "\t" __AROS_GM_RV_LX "   t6, 0(t6)\n" /* t6 = libbase       */ \
            "\tli   t0, %0\n"               /* t0 = lvo*LIB_VECTSIZE    */ \
            "\tsub  t0, t6, t0\n"           /* t0 = &JumpVec[-lvo]      */ \
            "\t" __AROS_GM_RV_LX "   t0, 0(t0)\n" /* t0 = function pointer */ \
            "\tjr   t0\n"                                                  \
            : : "i" ((lvo)*LIB_VECTSIZE)                                   \
        );                                                                 \
    }
#define AROS_GM_LIBFUNCSTUB(fname, libbasename, lvo) \
    __AROS_GM_LIBFUNCSTUB(fname, libbasename, lvo)

/* Macro: AROS_GM_RELLIBFUNCSTUB(functionname, libbasename, lvo)
   Same as AROS_GM_LIBFUNCSTUB but resolves the libbase through the per-task
   offset table (__aros_getoffsettable + __aros_rellib_offset_<libbasename>).
*/
#define __AROS_GM_RELLIBFUNCSTUB(fname, libbasename, lvo)                  \
    void __ ## fname ## _ ## libbasename ## _relwrapper(IPTR args)         \
    {                                                                      \
        asm volatile(                                                      \
            ".weak " #fname "\n"                                           \
            ".type " #fname ", %%function\n"                               \
            #fname " :\n"                                                  \
            /* Preserve every argument-carrying register across the        \
             * helper call: a0-a7 (integer args), fa0-fa7 (FP args) and    \
             * ra. Every RISC-V calling convention lets the callee clobber \
             * all of them. */                                             \
            "\taddi sp, sp, -" __AROS_GM_RV_FRAME "\n"                     \
            __AROS_GM_RV_FS(0)                                             \
            __AROS_GM_RV_FS(1)                                             \
            __AROS_GM_RV_FS(2)                                             \
            __AROS_GM_RV_FS(3)                                             \
            __AROS_GM_RV_FS(4)                                             \
            __AROS_GM_RV_FS(5)                                             \
            __AROS_GM_RV_FS(6)                                             \
            __AROS_GM_RV_FS(7)                                             \
            "\t" __AROS_GM_RV_SX "   a0, " __AROS_GM_RV_SLOT(0) "\n"       \
            "\t" __AROS_GM_RV_SX "   a1, " __AROS_GM_RV_SLOT(1) "\n"       \
            "\t" __AROS_GM_RV_SX "   a2, " __AROS_GM_RV_SLOT(2) "\n"       \
            "\t" __AROS_GM_RV_SX "   a3, " __AROS_GM_RV_SLOT(3) "\n"       \
            "\t" __AROS_GM_RV_SX "   a4, " __AROS_GM_RV_SLOT(4) "\n"       \
            "\t" __AROS_GM_RV_SX "   a5, " __AROS_GM_RV_SLOT(5) "\n"       \
            "\t" __AROS_GM_RV_SX "   a6, " __AROS_GM_RV_SLOT(6) "\n"       \
            "\t" __AROS_GM_RV_SX "   a7, " __AROS_GM_RV_SLOT(7) "\n"       \
            "\t" __AROS_GM_RV_SX "   ra, " __AROS_GM_RV_SLOT(8) "\n"       \
            "\tcall __aros_getoffsettable\n" /* a0 = offset table       */ \
            "\tli   t6, 0\n"                                               \
            "\tbeqz a0, 1f\n"                /* no table -> guard below */ \
            "\tla   t0, __aros_rellib_offset_" #libbasename "\n"           \
            "\t" __AROS_GM_RV_LX "   t0, 0(t0)\n" /* t0 = rellib offset */ \
            "\tadd  t0, a0, t0\n"                                          \
            "\t" __AROS_GM_RV_LX "   t6, 0(t0)\n" /* t6 = libbase       */ \
            "1:\t" __AROS_GM_RV_LX "   ra, " __AROS_GM_RV_SLOT(8) "\n"     \
            "\t" __AROS_GM_RV_LX "   a7, " __AROS_GM_RV_SLOT(7) "\n"       \
            "\t" __AROS_GM_RV_LX "   a6, " __AROS_GM_RV_SLOT(6) "\n"       \
            "\t" __AROS_GM_RV_LX "   a5, " __AROS_GM_RV_SLOT(5) "\n"       \
            "\t" __AROS_GM_RV_LX "   a4, " __AROS_GM_RV_SLOT(4) "\n"       \
            "\t" __AROS_GM_RV_LX "   a3, " __AROS_GM_RV_SLOT(3) "\n"       \
            "\t" __AROS_GM_RV_LX "   a2, " __AROS_GM_RV_SLOT(2) "\n"       \
            "\t" __AROS_GM_RV_LX "   a1, " __AROS_GM_RV_SLOT(1) "\n"       \
            "\t" __AROS_GM_RV_LX "   a0, " __AROS_GM_RV_SLOT(0) "\n"       \
            __AROS_GM_RV_FL(7)                                             \
            __AROS_GM_RV_FL(6)                                             \
            __AROS_GM_RV_FL(5)                                             \
            __AROS_GM_RV_FL(4)                                             \
            __AROS_GM_RV_FL(3)                                             \
            __AROS_GM_RV_FL(2)                                             \
            __AROS_GM_RV_FL(1)                                             \
            __AROS_GM_RV_FL(0)                                             \
            "\taddi sp, sp, " __AROS_GM_RV_FRAME "\n"                      \
            "\tbeqz t6, 2f\n"                /* base unresolved -> trap */ \
            "\tli   t0, %0\n"                /* t0 = lvo*LIB_VECTSIZE   */ \
            "\tsub  t0, t6, t0\n"            /* t0 = &JumpVec[-lvo]     */ \
            "\t" __AROS_GM_RV_LX "   t0, 0(t0)\n" /* t0 = function pointer */ \
            "\tjr   t0\n"                                                  \
            /* Deliberate illegal-instruction trap: the rellib base for  \
             * this stub was never resolved (offset table absent or the \
             * library not opened).  sepc/mepc names the *_relwrapper;  \
             * the restored a0-a7/fa0-fa7 are the original arguments.   */ \
            "2:\tunimp\n"                                                  \
            : : "i" ((lvo)*LIB_VECTSIZE)                                   \
        );                                                                 \
    }
#define AROS_GM_RELLIBFUNCSTUB(fname, libbasename, lvo) \
    __AROS_GM_RELLIBFUNCSTUB(fname, libbasename, lvo)

/* Macro: AROS_GM_LIBFUNCALIAS(functionname, alias)
   Generates a weak alias 'alias' for 'functionname' (CPU-independent).
*/
#define __AROS_GM_LIBFUNCALIAS(fname, alias) \
    asm(".weak " #alias "\n" \
        "\t.set " #alias "," #fname \
    );
#define AROS_GM_LIBFUNCALIAS(fname, alias) \
    __AROS_GM_LIBFUNCALIAS(fname, alias)

/******************* Library Side Thunks ******************/

/* Relies upon the caller (a LIBFUNCSTUB above) having left the libbase in
 * t6. Records it via __aros_setoffsettable then tail-jumps to the real
 * function.
 */
#define __GM_STRINGIZE(x) #x
#define __AROS_GM_STACKCALL(fname, libbasename, libfuncname)               \
    void libfuncname(void);                                                \
    void __ ## fname ## _stackcall(void)                                   \
    {                                                                      \
        asm volatile(                                                      \
            "\t" __GM_STRINGIZE(libfuncname) " :\n"                        \
            /* Preserve a0-a7, fa0-fa7 and ra - every RISC-V calling      \
             * convention lets the callee clobber all of them. */          \
            "\taddi sp, sp, -" __AROS_GM_RV_FRAME "\n"                     \
            __AROS_GM_RV_FS(0)                                             \
            __AROS_GM_RV_FS(1)                                             \
            __AROS_GM_RV_FS(2)                                             \
            __AROS_GM_RV_FS(3)                                             \
            __AROS_GM_RV_FS(4)                                             \
            __AROS_GM_RV_FS(5)                                             \
            __AROS_GM_RV_FS(6)                                             \
            __AROS_GM_RV_FS(7)                                             \
            "\t" __AROS_GM_RV_SX "   a0, " __AROS_GM_RV_SLOT(0) "\n"       \
            "\t" __AROS_GM_RV_SX "   a1, " __AROS_GM_RV_SLOT(1) "\n"       \
            "\t" __AROS_GM_RV_SX "   a2, " __AROS_GM_RV_SLOT(2) "\n"       \
            "\t" __AROS_GM_RV_SX "   a3, " __AROS_GM_RV_SLOT(3) "\n"       \
            "\t" __AROS_GM_RV_SX "   a4, " __AROS_GM_RV_SLOT(4) "\n"       \
            "\t" __AROS_GM_RV_SX "   a5, " __AROS_GM_RV_SLOT(5) "\n"       \
            "\t" __AROS_GM_RV_SX "   a6, " __AROS_GM_RV_SLOT(6) "\n"       \
            "\t" __AROS_GM_RV_SX "   a7, " __AROS_GM_RV_SLOT(7) "\n"       \
            "\t" __AROS_GM_RV_SX "   ra, " __AROS_GM_RV_SLOT(8) "\n"       \
            "\tmv   a0, t6\n"                 /* arg0 = libbase         */ \
            "\tcall __aros_setoffsettable\n"                               \
            "\t" __AROS_GM_RV_LX "   ra, " __AROS_GM_RV_SLOT(8) "\n"       \
            "\t" __AROS_GM_RV_LX "   a7, " __AROS_GM_RV_SLOT(7) "\n"       \
            "\t" __AROS_GM_RV_LX "   a6, " __AROS_GM_RV_SLOT(6) "\n"       \
            "\t" __AROS_GM_RV_LX "   a5, " __AROS_GM_RV_SLOT(5) "\n"       \
            "\t" __AROS_GM_RV_LX "   a4, " __AROS_GM_RV_SLOT(4) "\n"       \
            "\t" __AROS_GM_RV_LX "   a3, " __AROS_GM_RV_SLOT(3) "\n"       \
            "\t" __AROS_GM_RV_LX "   a2, " __AROS_GM_RV_SLOT(2) "\n"       \
            "\t" __AROS_GM_RV_LX "   a1, " __AROS_GM_RV_SLOT(1) "\n"       \
            "\t" __AROS_GM_RV_LX "   a0, " __AROS_GM_RV_SLOT(0) "\n"       \
            __AROS_GM_RV_FL(7)                                             \
            __AROS_GM_RV_FL(6)                                             \
            __AROS_GM_RV_FL(5)                                             \
            __AROS_GM_RV_FL(4)                                             \
            __AROS_GM_RV_FL(3)                                             \
            __AROS_GM_RV_FL(2)                                             \
            __AROS_GM_RV_FL(1)                                             \
            __AROS_GM_RV_FL(0)                                             \
            "\taddi sp, sp, " __AROS_GM_RV_FRAME "\n"                      \
            "\ttail " #fname "\n"                                          \
        );                                                                 \
    }

#define AROS_GM_STACKCALL(fname, libbasename, lvo) \
     __AROS_GM_STACKCALL(fname, libbasename, AROS_SLIB_ENTRY(fname, libbasename, lvo))

/* Macro: AROS_GM_STACKALIAS(functionname, libbasename, lvo)
   Generates a weak alias for the library-side entry of 'functionname'.
*/
#define __AROS_GM_STACKALIAS(fname, alias) \
    void alias(void); \
    asm(".weak " __GM_STRINGIZE(alias) "\n" \
        "\t.set " __GM_STRINGIZE(alias) "," #fname \
    );
#define AROS_GM_STACKALIAS(fname, libbasename, lvo) \
    __AROS_GM_STACKALIAS(fname, AROS_SLIB_ENTRY(fname, libbasename, lvo))

#endif /* AROS_RISCV_GENMODULE_H */
