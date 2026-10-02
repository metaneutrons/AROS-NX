/* P4 matched-SMP software interrupt producer and owner0 dispatcher. */
#include <aros/asmcall.h>
#include <aros/libcall.h>
#include <exec/interrupts.h>
#include <proto/exec.h>
#include <proto/kernel.h>
#include "exec_intern.h"
#include "p4_softint.h"
#include "p4_signal_protocol.h"
#include "../kernel/p4_ipi.h"
#if !defined(__AROSEXEC_SMP__) || !defined(__AROSPLATFORM_SMP__)
#error "P4 Cause requires matched Exec SMP"
#endif

AROS_LH1(void, Cause, AROS_LHA(struct Interrupt *, softint, A1),
         struct ExecBase *, SysBase, 30, Exec)
{
    AROS_LIBFUNC_INIT
    struct p4_signal_effect wake = {0};
    Disable();
    krnP4SoftIntEnqueue(softint, SysBase);
    /* No descriptor access after publishing; primary wake precedes Enable. */
    wake.status = P4_SIGNAL_STATUS_OK;
    wake.request_reschedule = 1;
    wake.reschedule_hart = 0;
    krnP4SignalPoke(&wake, SysBase);
    Enable();
    AROS_LIBFUNC_EXIT
}

AROS_INTH0(SoftIntDispatch)
{
    AROS_INTFUNC_INIT
    (void)__ufi_intmask;
    (void)__ufi_custom;
    (void)__ufi_code;
    KrnCli();
    if (krnP4SoftIntBegin(SysBase))
    {
        for (;;)
        {
            struct Interrupt *interrupt = krnP4SoftIntTake(SysBase);
            if (interrupt == NULL)
                break;
            /* Callback may requeue itself. Never retain/access its descriptor
             * after it returns, and never invoke it under the shared gate. */
            KrnSti();
            AROS_INTC1(interrupt->is_Code, interrupt->is_Data);
            KrnCli();
        }
    }
    KrnSti();
    return FALSE;
    AROS_INTFUNC_EXIT
}
