/*
    Copyright (C) 1995-2026, The AROS Development Team. All rights reserved.

    Desc: Disable() for the ESP32-P4 transitional Giant build (P4_GIANT=1).

    Interrupts are masked on this hart and the Giant D lock keeps every
    other Disable() section and every trap handler on the other hart out.
    Disable() implies Forbid(): F is taken first. See kernel/p4_giant.h.
*/

#include <exec/types.h>
#include <exec/execbase.h>
#include <aros/libcall.h>

#include "exec_intern.h"
#include "../kernel/p4_giant.h"

#undef  Exec
#ifdef UseExecstubs
#    define Exec _Exec
#endif

#include <proto/exec.h>

        AROS_LH0(void, Disable,
        struct ExecBase *, SysBase, 20, Exec)
{
#undef Exec
    AROS_LIBFUNC_INIT

    p4_giant_disable();

    AROS_LIBFUNC_EXIT
} /* Disable() */
