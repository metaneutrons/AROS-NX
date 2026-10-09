/*
    Copyright (C) 1995-2018, The AROS Development Team. All rights reserved.

    Desc: GetUpTime() - Get time since machine was powered on.
*/

#include <proto/exec.h>

#include "timer_intern.h"

/*****************************************************************************

    NAME */
#include <devices/timer.h>
#include <proto/timer.h>

        AROS_LH1(void, GetUpTime,

/*  SYNOPSIS */
        AROS_LHA(struct timeval *, dest, A0),

/*  LOCATION */
        struct Device *, TimerBase, 12, Timer)

/*  FUNCTION
        GetUpTime() will fill in the supplied timeval with the current
        uptime.

    INPUTS
        dest    -   A pointer to the timeval you want the time stored in.

    RESULT
        The timeval "dest" will be filled with the current uptime. This timer
        cannot be changed by the software and thus can be considered to be a
        monotonic clock..

    NOTES
        This function is safe to call from interrupts.

    EXAMPLE

    BUGS

    SEE ALSO
        TR_GETSYSTIME, TR_SETSYSTIME, GetSysTime()

    INTERNALS

    HISTORY
        05-08-2018  schulz   Implemented.

*****************************************************************************/
{
    AROS_LIBFUNC_INIT

    struct timeval up;

    Disable();

    /* Query the hardware, under the time lock on SMP */
    timer_GetTimes(GetTimerBase(TimerBase), NULL, &up);

    Enable();

    dest->tv_secs  = up.tv_secs;
    dest->tv_micro = up.tv_micro;

    AROS_LIBFUNC_EXIT
} /* GetUpTime */
