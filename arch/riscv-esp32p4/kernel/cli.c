/*
    Copyright (c) 2026, The AROS Development Team. All rights reserved.

    Desc: Disable interrupts, ESP32-P4 version.
*/

#include <aros/kernel.h>
#include <aros/libcall.h>

#include <asm/cpu.h>

#include <kernel_base.h>

#include <proto/kernel.h>

/*
 * See rom/kernel/cli.c for documentation.
 *
 * Machine mode is the only mode this platform runs in, so the enable bit
 * to clear is mstatus.MIE rather than the sstatus.SIE the supervisor mode
 * platforms use.
 */

AROS_LH0I(void, KrnCli,
          struct KernelBase *, KernelBase, 9, Kernel)
{
    AROS_LIBFUNC_INIT

    csr_clear(mstatus, MSTATUS_MIE);

    AROS_LIBFUNC_EXIT
}
