/*
 * Copyright (c) 2023-2026, The AROS Development Team. All rights reserved.
 * $Id$
 *
 * Layout (see aros/stdc/setjmp.h); SZREG is 4 on RV32 (_JMPLEN 37 for ILP32D),
 * 8 on RV64 (LP64D, _JMPLEN 25):
 *   0:                ra
 *   1*SZREG-12*SZREG: s0-s11
 *   13*SZREG:         sp
 *   14*SZREG + JB_FPSZ*k: fs0-fs11 (JB_FPSZ is 8 under a double
                        precision ABI, 4 under a single precision one;
                        none are saved under a soft float ABI)
 */

#include "aros/riscv/xlen.h"

/*
 * The floating point half of the buffer depends on the ABI, not on the
 * hardware: fs0-fs11 are callee-saved wherever the f registers exist, but
 * they are 8 bytes wide under a double precision ABI, 4 under a single
 * precision one, and absent under a soft float ABI (JB_FPSZ is then not
 * defined and the block is skipped). The slot base stays at 14*SZREG in
 * every case; only the stride and the opcode change. This is the same
 * three-way split aros/riscv/genmodule.h makes for fa0-fa7.
 */
#if defined(__riscv_float_abi_double)
#define JB_FPST		fsd
#define JB_FPLD		fld
#define JB_FPSZ		8
#elif defined(__riscv_float_abi_single)
#define JB_FPST		fsw
#define JB_FPLD		flw
#define JB_FPSZ		4
#endif

	.text
	.align	2
	.global	AROS_CDEFNAME(longjmp)
	.type	AROS_CDEFNAME(longjmp),%function

AROS_CDEFNAME(longjmp):
	REG_L	ra, 0(a0)
	REG_L	s0, 1*SZREG(a0)
	REG_L	s1, 2*SZREG(a0)
	REG_L	s2, 3*SZREG(a0)
	REG_L	s3, 4*SZREG(a0)
	REG_L	s4, 5*SZREG(a0)
	REG_L	s5, 6*SZREG(a0)
	REG_L	s6, 7*SZREG(a0)
	REG_L	s7, 8*SZREG(a0)
	REG_L	s8, 9*SZREG(a0)
	REG_L	s9, 10*SZREG(a0)
	REG_L	s10, 11*SZREG(a0)
	REG_L	s11, 12*SZREG(a0)
	REG_L	sp, 13*SZREG(a0)
#ifdef JB_FPSZ
	JB_FPLD	fs0, (14*SZREG + 0 * JB_FPSZ)(a0)
	JB_FPLD	fs1, (14*SZREG + 1 * JB_FPSZ)(a0)
	JB_FPLD	fs2, (14*SZREG + 2 * JB_FPSZ)(a0)
	JB_FPLD	fs3, (14*SZREG + 3 * JB_FPSZ)(a0)
	JB_FPLD	fs4, (14*SZREG + 4 * JB_FPSZ)(a0)
	JB_FPLD	fs5, (14*SZREG + 5 * JB_FPSZ)(a0)
	JB_FPLD	fs6, (14*SZREG + 6 * JB_FPSZ)(a0)
	JB_FPLD	fs7, (14*SZREG + 7 * JB_FPSZ)(a0)
	JB_FPLD	fs8, (14*SZREG + 8 * JB_FPSZ)(a0)
	JB_FPLD	fs9, (14*SZREG + 9 * JB_FPSZ)(a0)
	JB_FPLD	fs10, (14*SZREG + 10 * JB_FPSZ)(a0)
	JB_FPLD	fs11, (14*SZREG + 11 * JB_FPSZ)(a0)
#endif

	/* return val, or 1 if val was 0 */
	bne	a1, zero, 1f
	li	a1, 1
1:
	mv	a0, a1
	ret
