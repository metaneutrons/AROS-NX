/*
    Copyright (C) 1995-2026, The AROS Development Team. All rights reserved.

    Desc: Forbid() for the ESP32-P4 transitional Giant build (P4_GIANT=1).

    On this build Forbid() keeps its Amiga meaning on both harts: while
    any task holds it, no other task runs anywhere, except tasks that are
    already running on the other hart and do not themselves need Forbid()
    or Disable(). See kernel/p4_giant.h.
*/

#include <exec/types.h>
#include <exec/execbase.h>
#include <aros/libcall.h>

#include "exec_intern.h"
#include "../kernel/p4_giant.h"

#undef Exec
#ifdef UseExecstubs
#    define Exec _Exec
#endif

#include <proto/exec.h>

        AROS_LH0(void, Forbid,
        struct ExecBase *, SysBase, 22, Exec)
{
#undef Exec
    AROS_LIBFUNC_INIT

    p4_giant_forbid();

    AROS_LIBFUNC_EXIT
} /* Forbid() */
