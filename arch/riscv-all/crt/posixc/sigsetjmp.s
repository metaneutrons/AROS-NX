/*
    Copyright (c) 2023-2026, The AROS Development Team. All rights reserved.
    $Id$

    Desc: POSIX.1-2008 function sigsetjmp()
    Lang: english

    Layout (see aros/stdc/setjmp.h); SZREG is 4 on RV32 (_JMPLEN 37),
    8 on RV64 (_JMPLEN 25):
      0:                ra (retaddr)
      1*SZREG-12*SZREG: s0-s11
      13*SZREG:         sp
      14*SZREG + 8*k:   fs0-fs11 (fs<k>, 8-byte slots, 8-aligned;
                        FREG_S/FREG_L store 8 or 4 bytes as
                        the float ABI says, nothing under soft float)
*/

#include "aros/riscv/xlen.h"

	.text
	.align	2
	.global AROS_CDEFNAME(sigsetjmp)
	.type	AROS_CDEFNAME(sigsetjmp),%function

AROS_CDEFNAME(sigsetjmp):
	/* a0 = pointer to jmp_buf, a1 = savesigs (unused in AROS) */
	REG_S	ra, 0(a0)					/* store return address explicitly */
	REG_S	s0, 1*SZREG(a0)
	REG_S	s1, 2*SZREG(a0)
	REG_S	s2, 3*SZREG(a0)
	REG_S	s3, 4*SZREG(a0)
	REG_S	s4, 5*SZREG(a0)
	REG_S	s5, 6*SZREG(a0)
	REG_S	s6, 7*SZREG(a0)
	REG_S	s7, 8*SZREG(a0)
	REG_S	s8, 9*SZREG(a0)
	REG_S	s9, 10*SZREG(a0)
	REG_S	s10, 11*SZREG(a0)
	REG_S	s11, 12*SZREG(a0)
	REG_S	sp, 13*SZREG(a0)
#ifdef FREG_S
	FREG_S	fs0, 14*SZREG+0*8(a0)
	FREG_S	fs1, 14*SZREG+1*8(a0)
	FREG_S	fs2, 14*SZREG+2*8(a0)
	FREG_S	fs3, 14*SZREG+3*8(a0)
	FREG_S	fs4, 14*SZREG+4*8(a0)
	FREG_S	fs5, 14*SZREG+5*8(a0)
	FREG_S	fs6, 14*SZREG+6*8(a0)
	FREG_S	fs7, 14*SZREG+7*8(a0)
	FREG_S	fs8, 14*SZREG+8*8(a0)
	FREG_S	fs9, 14*SZREG+9*8(a0)
	FREG_S	fs10, 14*SZREG+10*8(a0)
	FREG_S	fs11, 14*SZREG+11*8(a0)
#endif
	li	a0, 0						/* return zero */
	ret
