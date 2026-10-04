/*
    Copyright (C) 2011-2026, The AROS Development Team. All rights reserved.

    Desc: High-level scheduler calling code, ESP32-P4 Giant version.
*/

/*
 * Built only with P4_GIANT; every other P4 build uses rom/kernel/kernel_intr.c
 * unchanged. The difference is the soft interrupt check on the way out:
 * soft interrupts run on hart 0 only (EXEC_PLATFORM_SOFTINT_PENDING), and
 * this drains them before the scheduler runs.
 */

#if !defined(P4_CORE_EXIT_TEST)
#include <exec/execbase.h>
#include <hardware/intbits.h>
#include <proto/exec.h>

#include <kernel_base.h>
#include <kernel_debug.h>
#include <kernel_intr.h>
#include <kernel_scheduler.h>
#include <kernel_syscall.h>
#include <proto/kernel.h>
#endif

#define D(x)

/*
 * Leave the interrupt. This function receives the interrupt register frame
 * and runs task scheduler if needed.
 *
 * You should call it only if you're returning to user mode (i.e. not from
 * within nested interrupt).
 *
 * It relies on CPU-specific cpu_Switch() and cpu_Dispatch() implementations
 * which save and restore CPU context. cpu_Dispatch() is allowed just to
 * jump to the saved context and not return here.
 */
void core_ExitInterrupt(regs_t *regs)
{
    /*
     * Soft interrupts requested? Exec's SoftIntDispatch runs the handlers
     * with interrupts enabled (KrnSti()) and returns that way, and a handler
     * may cause another soft interrupt. Mask again before every check, so
     * that the scheduler below always runs with interrupts off, and drain
     * until nothing is pending.
     */
    while (EXEC_PLATFORM_SOFTINT_PENDING())
    {
        core_Cause(INTB_SOFTINT, 1L << INTB_SOFTINT);
        KrnCli();
    }

    /* If task switching is disabled, do nothing */
    if (TDNESTCOUNT_GET < 0)
    {
        /*
         * Do not disturb task if it's not necessary.
         * Reschedule only if switch pending flag is set. Exit otherwise.
         */
        if (FLAG_SCHEDSWITCH_ISSET)
        {
            D(bug("[Kernel] %s: Triggering Schedule\n", __func__);)
            /* Run task scheduling sequence */
            if (core_Schedule())
            {
                cpu_Switch(regs);
                cpu_Dispatch(regs);
            }
        }
    }
}

/*
 * This routine dispatches scheduler's SysCall, when some task wants to give up
 * the CPU time explicitly.
 * Similar to above, it should be called only when you're returning to user mode.
 */
void core_SysCall(int sc, regs_t *regs)
{
    switch (sc)
    {
    case SC_CAUSE:
        core_ExitInterrupt(regs);
        break;

    case SC_SCHEDULE:
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
