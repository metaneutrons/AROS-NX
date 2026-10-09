/*
    Copyright (C) 1995-2011, The AROS Development Team. All rights reserved.

    Desc: GetSysTime() - Find out what time it is.
*/

#include <proto/exec.h>

#include "timer_intern.h"

/*****************************************************************************

    NAME */
#include <devices/timer.h>
#include <proto/timer.h>

        AROS_LH1(void, GetSysTime,

/*  SYNOPSIS */
        AROS_LHA(struct timeval *, dest, A0),

/*  LOCATION */
        struct Device *, TimerBase, 11, Timer)

/*  FUNCTION
        GetSysTime() will fill in the supplied timeval with the current
        system time.

    INPUTS
        dest    -   A pointer to the timeval you want the time stored in.

    RESULT
        The timeval "dest" will be filled with the current system time.

    NOTES
        This function is safe to call from interrupts.

    EXAMPLE

    BUGS

    SEE ALSO
        TR_GETSYSTIME, TR_SETSYSTIME

    INTERNALS

    HISTORY
        18-02-1997  iaint   Implemented.

*****************************************************************************/
{
    AROS_LIBFUNC_INIT

    struct timeval now;

    Disable();

    /* Query the hardware, under the time lock on SMP */
    timer_GetTimes(GetTimerBase(TimerBase), &now, NULL);

    Enable();

    dest->tv_secs  = now.tv_secs;
    dest->tv_micro = now.tv_micro;

    AROS_LIBFUNC_EXIT
} /* GetSysTime */
