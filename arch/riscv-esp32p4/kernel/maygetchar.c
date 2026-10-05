/*
    Copyright (c) 2026, The AROS Development Team. All rights reserved.

    Desc: KrnMayGetChar() - console input for the esp32p4-riscv target.

    The generic implementation in rom/kernel/maygetchar.c returns -1 and
    says the real one is architecture-specific.  Leaving it at that made
    the emergency console output-only: econsole's Raw_Read() polls
    RawMayGetChar(), which reaches here, so a shell prompt could be printed
    and never answered.  That also starves the machine, because econsole
    reschedules between attempts at the handler priority DOS gave it, and a
    poll that can never succeed is a spin that never ends.

    Input still cannot be made to block - there is no interrupt wired to
    either console channel yet - so this only makes the poll capable of
    succeeding.  The spin remains, and anything below the handler's
    priority still has to account for it.
*/

#include <aros/kernel.h>
#include <aros/libcall.h>

#include <kernel_base.h>

#include "kernel_intern.h"

#include <proto/kernel.h>

/* See rom/kernel/maygetchar.c for documentation */

AROS_LH0(int, KrnMayGetChar,
         struct KernelBase *, KernelBase, 26, Kernel)
{
    AROS_LIBFUNC_INIT

    return krnP4GetC();

    AROS_LIBFUNC_EXIT
}
