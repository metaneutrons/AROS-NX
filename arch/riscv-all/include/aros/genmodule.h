/*
    Copyright (C) 2023-2026, The AROS Development Team. All rights reserved.

    Desc: genmodule.h include file for 32bit risc-v systems.

    Library-call stubs jump through the library's downward-growing vector
    table: the function for LVO n lives at (libbase - n*LIB_VECTSIZE), and
    each JumpVec slot holds a function pointer. The stubs hand the libbase
    to the library-side thunk in t6 - a temporary register that is never
    used to pass arguments, so all eight integer (a0-a7) and eight FP
    (fa0-fa7) argument registers stay untouched.
*/

#ifndef AROS_RISCV_GENMODULE_H
#define AROS_RISCV_GENMODULE_H

#include <exec/execbase.h>

/*
 * The argument registers a stub has to preserve across its helper call,
 * and the frame that holds them. What varies is the floating point half:
 * fa0-fa7 are 8 bytes wide under a double precision ABI and 4 under a
 * single precision one, and a soft float ABI passes nothing in them at
 * all. The frame is rounded up to keep sp 16-byte aligned.
 */
#if defined(__riscv_float_abi_double)

#define __GM_FRAME      "112"
#define __GM_SAVE_FP                                                       \
            "\tfsd  fa0, 0(sp)\n"                                          \
            "\tfsd  fa1, 8(sp)\n"                                          \
            "\tfsd  fa2, 16(sp)\n"                                         \
            "\tfsd  fa3, 24(sp)\n"                                         \
            "\tfsd  fa4, 32(sp)\n"                                         \
            "\tfsd  fa5, 40(sp)\n"                                         \
            "\tfsd  fa6, 48(sp)\n"                                         \
            "\tfsd  fa7, 56(sp)\n"
#define __GM_LOAD_FP                                                       \
            "\tfld  fa7, 56(sp)\n"                                         \
            "\tfld  fa6, 48(sp)\n"                                         \
            "\tfld  fa5, 40(sp)\n"                                         \
            "\tfld  fa4, 32(sp)\n"                                         \
            "\tfld  fa3, 24(sp)\n"                                         \
            "\tfld  fa2, 16(sp)\n"                                         \
            "\tfld  fa1, 8(sp)\n"                                          \
            "\tfld  fa0, 0(sp)\n"
#define __GM_SAVE_INT                                                      \
            "\tsw   a0, 64(sp)\n"                                          \
            "\tsw   a1, 68(sp)\n"                                          \
            "\tsw   a2, 72(sp)\n"                                          \
            "\tsw   a3, 76(sp)\n"                                          \
            "\tsw   a4, 80(sp)\n"                                          \
            "\tsw   a5, 84(sp)\n"                                          \
            "\tsw   a6, 88(sp)\n"                                          \
            "\tsw   a7, 92(sp)\n"                                          \
            "\tsw   ra, 96(sp)\n"
#define __GM_LOAD_INT                                                      \
            "\tlw   ra, 96(sp)\n"                                          \
            "\tlw   a7, 92(sp)\n"                                          \
            "\tlw   a6, 88(sp)\n"                                          \
            "\tlw   a5, 84(sp)\n"                                          \
            "\tlw   a4, 80(sp)\n"                                          \
            "\tlw   a3, 76(sp)\n"                                          \
            "\tlw   a2, 72(sp)\n"                                          \
            "\tlw   a1, 68(sp)\n"                                          \
            "\tlw   a0, 64(sp)\n"

#elif defined(__riscv_float_abi_single)

#define __GM_FRAME      "80"
#define __GM_SAVE_FP                                                       \
            "\tfsw  fa0, 0(sp)\n"                                          \
            "\tfsw  fa1, 4(sp)\n"                                          \
            "\tfsw  fa2, 8(sp)\n"                                          \
            "\tfsw  fa3, 12(sp)\n"                                         \
            "\tfsw  fa4, 16(sp)\n"                                         \
            "\tfsw  fa5, 20(sp)\n"                                         \
            "\tfsw  fa6, 24(sp)\n"                                         \
            "\tfsw  fa7, 28(sp)\n"
#define __GM_LOAD_FP                                                       \
            "\tflw  fa7, 28(sp)\n"                                         \
            "\tflw  fa6, 24(sp)\n"                                         \
            "\tflw  fa5, 20(sp)\n"                                         \
            "\tflw  fa4, 16(sp)\n"                                         \
            "\tflw  fa3, 12(sp)\n"                                         \
            "\tflw  fa2, 8(sp)\n"                                          \
            "\tflw  fa1, 4(sp)\n"                                          \
            "\tflw  fa0, 0(sp)\n"
#define __GM_SAVE_INT                                                      \
            "\tsw   a0, 32(sp)\n"                                          \
            "\tsw   a1, 36(sp)\n"                                          \
            "\tsw   a2, 40(sp)\n"                                          \
            "\tsw   a3, 44(sp)\n"                                          \
            "\tsw   a4, 48(sp)\n"                                          \
            "\tsw   a5, 52(sp)\n"                                          \
            "\tsw   a6, 56(sp)\n"                                          \
            "\tsw   a7, 60(sp)\n"                                          \
            "\tsw   ra, 64(sp)\n"
#define __GM_LOAD_INT                                                      \
            "\tlw   ra, 64(sp)\n"                                          \
            "\tlw   a7, 60(sp)\n"                                          \
            "\tlw   a6, 56(sp)\n"                                          \
            "\tlw   a5, 52(sp)\n"                                          \
            "\tlw   a4, 48(sp)\n"                                          \
            "\tlw   a3, 44(sp)\n"                                          \
            "\tlw   a2, 40(sp)\n"                                          \
            "\tlw   a1, 36(sp)\n"                                          \
            "\tlw   a0, 32(sp)\n"

#else /* soft float: nothing is passed in the f registers */

#define __GM_FRAME      "48"
#define __GM_SAVE_FP    ""
#define __GM_LOAD_FP    ""
#define __GM_SAVE_INT                                                      \
            "\tsw   a0, 0(sp)\n"                                           \
            "\tsw   a1, 4(sp)\n"                                           \
            "\tsw   a2, 8(sp)\n"                                           \
            "\tsw   a3, 12(sp)\n"                                          \
            "\tsw   a4, 16(sp)\n"                                          \
            "\tsw   a5, 20(sp)\n"                                          \
            "\tsw   a6, 24(sp)\n"                                          \
            "\tsw   a7, 28(sp)\n"                                          \
            "\tsw   ra, 32(sp)\n"
#define __GM_LOAD_INT                                                      \
            "\tlw   ra, 32(sp)\n"                                          \
            "\tlw   a7, 28(sp)\n"                                          \
            "\tlw   a6, 24(sp)\n"                                          \
            "\tlw   a5, 20(sp)\n"                                          \
            "\tlw   a4, 16(sp)\n"                                          \
            "\tlw   a3, 12(sp)\n"                                          \
            "\tlw   a2, 8(sp)\n"                                           \
            "\tlw   a1, 4(sp)\n"                                           \
            "\tlw   a0, 0(sp)\n"

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
            "\tlw   t6, 0(t6)\n"            /* t6 = libbase             */ \
            "\tli   t0, %0\n"               /* t0 = lvo*LIB_VECTSIZE    */ \
            "\tsub  t0, t6, t0\n"           /* t0 = &JumpVec[-lvo]      */ \
            "\tlw   t0, 0(t0)\n"            /* t0 = function pointer    */ \
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
             * ra. The ABI lets the callee clobber all of them. */         \
            "\taddi sp, sp, -" __GM_FRAME "\n"                             \
            __GM_SAVE_FP                                                   \
            __GM_SAVE_INT                                                  \
            "\tcall __aros_getoffsettable\n" /* a0 = offset table       */ \
            "\tli   t6, 0\n"                                               \
            "\tbeqz a0, 1f\n"                /* no table -> guard below */ \
            "\tla   t0, __aros_rellib_offset_" #libbasename "\n"           \
            "\tlw   t0, 0(t0)\n"             /* t0 = rellib offset      */ \
            "\tadd  t0, a0, t0\n"                                          \
            "\tlw   t6, 0(t0)\n"             /* t6 = libbase            */ \
            "1:\n"                                                         \
            __GM_LOAD_INT                                                  \
            __GM_LOAD_FP                                                   \
            "\taddi sp, sp, " __GM_FRAME "\n"                              \
            "\tbeqz t6, 2f\n"                /* base unresolved -> trap */ \
            "\tli   t0, %0\n"                /* t0 = lvo*LIB_VECTSIZE   */ \
            "\tsub  t0, t6, t0\n"            /* t0 = &JumpVec[-lvo]     */ \
            "\tlw   t0, 0(t0)\n"             /* t0 = function pointer   */ \
            "\tjr   t0\n"                                                  \
            /* Deliberate illegal-instruction trap: the rellib base for  \
             * this stub was never resolved (offset table absent or the \
             * library not opened).  sepc names the *_relwrapper; the   \
             * restored a0-a7/fa0-fa7 are the original arguments.       */ \
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
            /* Preserve a0-a7, fa0-fa7 and ra - the ABI lets the callee    \
             * clobber all of them. */                                     \
            "\taddi sp, sp, -" __GM_FRAME "\n"                             \
            __GM_SAVE_FP                                                   \
            __GM_SAVE_INT                                                  \
            "\tmv   a0, t6\n"                 /* arg0 = libbase         */ \
            "\tcall __aros_setoffsettable\n"                               \
            __GM_LOAD_INT                                                  \
            __GM_LOAD_FP                                                   \
            "\taddi sp, sp, " __GM_FRAME "\n"                              \
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
