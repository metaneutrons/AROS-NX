/*
    Copyright (C) 1995-2026, The AROS Development Team. All rights reserved.

    Desc: Permit() for the ESP32-P4 transitional Giant build (P4_GIANT=1).
*/

#include <exec/types.h>
#include <exec/execbase.h>
#include <aros/libcall.h>
#include <proto/exec.h>

#include "exec_intern.h"
#include "../kernel/p4_giant.h"

#undef Exec
#ifdef UseExecstubs
#    define Exec _Exec
#endif

        AROS_LH0(void, Permit,
        struct ExecBase *, SysBase, 23, Exec)
{
#undef Exec
    AROS_LIBFUNC_INIT

    p4_giant_permit();

    /* As the generic Permit(): take a switch that became due meanwhile. */
    if (KernelBase && !p4_giant_is_super())
    {
        if ((TDNESTCOUNT_GET < 0) && (IDNESTCOUNT_GET < 0) &&
            FLAG_SCHEDSWITCH_ISSET)
            KrnSchedule();
    }

    AROS_LIBFUNC_EXIT
} /* Permit() */
