/*
    Copyright (c) 2026, The AROS Development Team. All rights reserved.

    Desc: the per-hart records of the smp variant (see tls.h).
*/

#include <exec/types.h>

#include <exec_platform.h>

#include "tls.h"

/*
 * Nesting counts start at zero; exec_init() loads them from the boot task
 * and its Permit()/Enable() take them to -1. Exec does not seed the
 * quantum in an SMP build, so each hart's starts here.
 */
tls_t __p4_tls[P4_TLS_HARTS] =
{
    { .Quantum = SCHEDQUANTUM_VALUE, .CPUNumber = 0 },
    { .Quantum = SCHEDQUANTUM_VALUE, .CPUNumber = 1 },
};

ULONG __p4_harts_online = 1;
