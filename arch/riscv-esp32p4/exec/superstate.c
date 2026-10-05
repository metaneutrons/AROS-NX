/*
    Copyright (c) 2026, The AROS Development Team. All rights reserved.

    Desc: SuperState() - Switch the processor into a higher plane.
*/

#include <proto/exec.h>

/* See rom/exec/superstate.c for documentation */

AROS_LH0(APTR, SuperState,
    struct ExecBase *, SysBase, 25, Exec)
{
    AROS_LIBFUNC_INIT

    /*
     * There is no higher plane here. The ESP32-P4 has no supervisor mode
     * and nothing runs below this image, so machine mode is where
     * everything already is. Once user mode task separation exists this
     * will trap into the kernel the way the other native ports do.
     *
     * Overriding the generic version also keeps cpu_SuperState() out of
     * the picture: rom/exec/superstate.c calls it as an architecture
     * specific assembly helper, and this platform has no use for one.
     */
    return NULL;

    AROS_LIBFUNC_EXIT
} /* SuperState() */
