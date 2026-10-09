/*
    Copyright (C) 1995-2007, The AROS Development Team. All rights reserved.

    Desc: Checks for signals in a mask.
*/
#include <exec/tasks.h>
#include <proto/exec.h>
#include "dos_intern.h"

/*****************************************************************************

    NAME */
#include <proto/dos.h>

        AROS_LH1(LONG, CheckSignal,

/*  SYNOPSIS */
        AROS_LHA(LONG, mask, D1),

/*  LOCATION */
        struct DosLibrary *, DOSBase, 132, Dos)

/*  FUNCTION
        Checks the current task to see if any of the signals specified in
        the mask have been set. The mask of all signals which were set is
        returned. The signals specified in the mask will be cleared.

    INPUTS
        mask - The signal mask to check.

    RESULT
        The mask of all signals which were set.

    NOTES

    EXAMPLE

    BUGS

    SEE ALSO

    INTERNALS

*****************************************************************************/
{
    AROS_LIBFUNC_INIT

    /*
     * Get the active signals in mask and clear them, in one step through
     * exec. Doing it here under Disable() protected the signal mask only
     * against this core: on an SMP system Signal() from another core
     * changes tc_SigRecvd under the task's spinlock, which SetSignal()
     * takes and Disable() does not, so a signal arriving in between could
     * be lost.
     */
    return (LONG)(SetSignal(0, mask) & mask);

    AROS_LIBFUNC_EXIT
} /* CheckSignal */
