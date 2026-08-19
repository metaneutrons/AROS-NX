/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: Standard AROS/Amiga scalar types for bare-metal RISC-V 32-bit.
*/

#ifndef RISCV32_TYPES_H
#define RISCV32_TYPES_H

typedef unsigned int        ULONG;
typedef signed int          LONG;
typedef unsigned short      UWORD;
typedef signed short        WORD;
typedef unsigned char       UBYTE;
typedef signed char         BYTE;
typedef unsigned long long  UQUAD;
typedef signed long long    QUAD;
typedef void *              APTR;
typedef char *              STRPTR;
typedef const char *        CONST_STRPTR;
typedef int                 BOOL;

#ifndef TRUE
#define TRUE                1
#endif
#ifndef FALSE
#define FALSE               0
#endif
#ifndef NULL
#define NULL                ((void *)0)
#endif

#endif /* RISCV32_TYPES_H */
