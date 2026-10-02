/*
    Copyright (C) 1995-2026, The AROS Development Team. All rights reserved.

    Desc: Enable() for the ESP32-P4 transitional Giant build (P4_GIANT=1).
*/

#include <exec/execbase.h>
#include <aros/libcall.h>

#include "exec_intern.h"
#include "../kernel/p4_giant.h"

#undef  Exec
#ifdef UseExecstubs
#    define Exec _Exec
#endif

#include <proto/exec.h>

        AROS_LH0(void, Enable,
        struct ExecBase *, SysBase, 21, Exec)
{
#undef Exec
    AROS_LIBFUNC_INIT

    /* Nonzero once interrupts are on again on this hart; then do what
       the generic Enable() does after KrnSti(). */
    if (p4_giant_enable() && KernelBase)
    {
        if (EXEC_PLATFORM_SOFTINT_PENDING())
            KrnCause();
        if ((TDNESTCOUNT_GET < 0) && FLAG_SCHEDSWITCH_ISSET)
            KrnSchedule();
    }

    AROS_LIBFUNC_EXIT
} /* Enable() */
