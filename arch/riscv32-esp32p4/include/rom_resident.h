/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: Standard AmigaOS / AROS struct Resident & ROM Kickstart Tag Header
          for ESP32-P4 on AROS.
*/

#ifndef RISCV32_ROM_RESIDENT_H
#define RISCV32_ROM_RESIDENT_H

#include "types.h"

#define RTC_MATCHWORD           0x4AFC

#define RTF_AUTOINIT            (1 << 7)
#define RTF_AFTERDOS            (1 << 2)
#define RTF_SINGLETASK          (1 << 1)
#define RTF_COLDSTART           (1 << 0)

#define NT_UNKNOWN              0
#define NT_TASK                 1
#define NT_INTERRUPT            2
#define NT_DEVICE               3
#define NT_MSGPORT              4
#define NT_MESSAGE              5
#define NT_FREEMSG              6
#define NT_REPLYMSG             7
#define NT_RESOURCE             8
#define NT_LIBRARY              9
#define NT_MEMORY               10

struct Resident
{
    UWORD       rt_MatchWord;
    struct Resident *rt_MatchTag;
    APTR        rt_EndSkip;
    UBYTE       rt_Flags;
    UBYTE       rt_Version;
    UBYTE       rt_Type;
    BYTE        rt_Pri;
    CONST_STRPTR rt_Name;
    CONST_STRPTR rt_IdString;
    APTR        rt_Init;
};

/* Module Initialization Function Type */
typedef int (*resident_init_func_t)(void);

/* Functions */
void exec_init_coldstart_residents(void);

#endif /* RISCV32_ROM_RESIDENT_H */
