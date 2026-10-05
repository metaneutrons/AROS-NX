/*
    Copyright (c) 2023-2026, The AROS Development Team. All rights reserved.
    $Id$

    Desc: POSIX.1-2008 function sigsetjmp()
    Lang: english

    ILP32 layout (see aros/stdc/setjmp.h, _JMPLEN 37):
      0:      ra (retaddr)
      4-48:   s0-s11
      52:     sp
      56-:    fs0-fs11, JB_FPSZ bytes each; ends at 152 under a double
              precision ABI, at 104 under a single precision one, and is
              not written at all under a soft float ABI
*/

#include "aros/riscv/asm.h"

/*
 * The floating point half of the buffer depends on the ABI, not on the
 * hardware: fs0-fs11 are callee-saved wherever the f registers exist, but
 * they are 8 bytes wide under a double precision ABI, 4 under a single
 * precision one, and absent under a soft float ABI.  The slot base stays
 * at 56 in every case; only the stride and the opcode change.  This is the
 * same three-way split aros/riscv/genmodule.h makes for fa0-fa7.
 */
#if defined(__riscv_float_abi_double)
#define JB_FPST         fsd
#define JB_FPLD         fld
#define JB_FPSZ         8
#elif defined(__riscv_float_abi_single)
#define JB_FPST         fsw
#define JB_FPLD         flw
#define JB_FPSZ         4
#endif

	.text
	.align	2
	.global AROS_CDEFNAME(sigsetjmp)
	.type	AROS_CDEFNAME(sigsetjmp),%function

AROS_CDEFNAME(sigsetjmp):
	/* a0 = pointer to jmp_buf, a1 = savesigs (unused in AROS) */
	sw	ra, 0(a0)					/* store return address explicitly */
	sw	s0, 1*4(a0)
	sw	s1, 2*4(a0)
	sw	s2, 3*4(a0)
	sw	s3, 4*4(a0)
	sw	s4, 5*4(a0)
	sw	s5, 6*4(a0)
	sw	s6, 7*4(a0)
	sw	s7, 8*4(a0)
	sw	s8, 9*4(a0)
	sw	s9, 10*4(a0)
	sw	s10, 11*4(a0)
	sw	s11, 12*4(a0)
	sw	sp, 13*4(a0)
#ifdef JB_FPSZ
	JB_FPST	fs0, (56 + 0 * JB_FPSZ)(a0)
	JB_FPST	fs1, (56 + 1 * JB_FPSZ)(a0)
	JB_FPST	fs2, (56 + 2 * JB_FPSZ)(a0)
	JB_FPST	fs3, (56 + 3 * JB_FPSZ)(a0)
	JB_FPST	fs4, (56 + 4 * JB_FPSZ)(a0)
	JB_FPST	fs5, (56 + 5 * JB_FPSZ)(a0)
	JB_FPST	fs6, (56 + 6 * JB_FPSZ)(a0)
	JB_FPST	fs7, (56 + 7 * JB_FPSZ)(a0)
	JB_FPST	fs8, (56 + 8 * JB_FPSZ)(a0)
	JB_FPST	fs9, (56 + 9 * JB_FPSZ)(a0)
	JB_FPST	fs10, (56 + 10 * JB_FPSZ)(a0)
	JB_FPST	fs11, (56 + 11 * JB_FPSZ)(a0)
#endif
	li	a0, 0						/* return zero */
	ret
