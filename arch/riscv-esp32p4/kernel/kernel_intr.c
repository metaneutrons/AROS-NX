/*
    Copyright (c) 2011-2026, The AROS Development Team. All rights reserved.

    Desc: leaving an interrupt or a scheduler syscall, for the smp variant
          of the esp32p4-riscv target.

    The generic version (rom/kernel/kernel_intr.c) with one difference:
    soft interrupts run on hart 0 only. Their handlers - timer.device and
    the drivers that Cause() - are written for one CPU, and a hart that
    happened to leave a trap while one was pending would run it alongside
    hart 0. A soft interrupt raised on hart 1 waits for hart 0's next trap
    exit, at most one tick.
*/

#include <exec/execbase.h>
#include <hardware/intbits.h>
#include <proto/exec.h>

#include <kernel_base.h>
#include <kernel_debug.h>
#include <kernel_intr.h>
#include <kernel_scheduler.h>
#include <kernel_syscall.h>

#include <exec_platform.h>

#include "kernel_intern.h"

void core_ExitInterrupt(regs_t *regs)
{
    if ((SysBase->SysFlags & SFF_SoftInt) && GetCPUNumber() == 0)
        core_Cause(INTB_SOFTINT, 1L << INTB_SOFTINT);

    /* Task switching disabled, or the task holds a spinlock (tls.h,
       p4_spin_taken()): leave the task alone. The switch stays pending. */
    if (TDNESTCOUNT_GET < 0 && !TLS_GET(SpinHeld))
    {
        /* Only when a switch is pending */
        if (FLAG_SCHEDSWITCH_ISSET)
        {
            if (core_Schedule())
            {
                cpu_Switch(regs);
                cpu_Dispatch(regs);
            }
        }
    }
}

void core_SysCall(int sc, regs_t *regs)
{
    switch (sc)
    {
    case SC_CAUSE:
        core_ExitInterrupt(regs);
        break;

    case SC_SCHEDULE:
        /* Not while the task holds a spinlock: its last unlock asks again */
        if (TLS_GET(SpinHeld))
        {
            FLAG_SCHEDSWITCH_SET;
            break;
        }
        if (!core_Schedule())
            break;
        /* Fallthrough */

    case SC_SWITCH:
        cpu_Switch(regs);
        /* Fallthrough */

    case SC_DISPATCH:
        cpu_Dispatch(regs);
        break;
    }
}
