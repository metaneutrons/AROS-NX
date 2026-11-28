/*
    Copyright (c) 2026, The AROS Development Team. All rights reserved.

    Desc: The watchdog's canary tasks of the esp32p4-riscv target.

    One task per hart, bound to it, above every ordinary task, that waits
    half a second on timer.device and then reports that it woke
    (krnWdtCanaryBeat(), kernel_wdt.c). The watchdog feeder stops feeding
    when a canary has not woken for three seconds. The wake takes the whole
    path of a task becoming ready: the timer interrupt, exec's VBlank
    server, the reply, the signal and, for hart 1, the inter-hart interrupt,
    and then the dispatcher has to pick the task. A hart that takes
    interrupts but runs no task fails that, and so does a task that holds
    Forbid() for good.

    Cold-started after dos.library, like the other late services: before
    that the tick alone is the sign of life.
*/

#define __NOLIBBASE__

#include <aros/asmcall.h>
#include <devices/timer.h>
#include <exec/io.h>
#include <exec/ports.h>
#include <exec/resident.h>
#include <exec/tasks.h>
#include <proto/exec.h>
#include <utility/tagitem.h>

#include <proto/kernel.h>

#include "hardware.h"
#include "kernel_intern.h"
#include "tls.h"

#ifndef P4_NO_WATCHDOG

#define CANARY_PRIORITY     100
#define CANARY_STACK        8192
#define CANARY_PERIOD_US    500000

static struct Library *KernelBase;

static void canary_task(struct ExecBase *SysBase)
{
    unsigned long hart;
    struct MsgPort *port;
    struct timerequest *tr;

    asm volatile("csrr %0, mhartid" : "=r"(hart));

    port = CreateMsgPort();
    tr = port ? (struct timerequest *)CreateIORequest(port, sizeof(*tr)) : NULL;
    if (!tr || OpenDevice((CONST_STRPTR)"timer.device", UNIT_VBLANK,
                          (struct IORequest *)tr, 0))
    {
        krnP4PutStr("[wdt] canary of hart ");
        krnP4PutDec((uint32_t)hart);
        krnP4PutStr(" could not open timer.device; this hart has the tick only\n");
        return;
    }

    for (;;)
    {
        tr->tr_node.io_Command = TR_ADDREQUEST;
        tr->tr_time.tv_secs = 0;
        tr->tr_time.tv_micro = CANARY_PERIOD_US;
        DoIO((struct IORequest *)tr);
        krnWdtCanaryBeat((unsigned int)hart);
    }
}

static BOOL start_canary(struct ExecBase *SysBase, unsigned int hart)
{
    void *aff = KrnAllocCPUMask();
    char *name = hart ? "WDT canary 1" : "WDT canary 0";

    if (!aff)
        return FALSE;
    KrnGetCPUMask(hart, aff);
    return NewCreateTask(TASKTAG_NAME, (IPTR)name,
                         TASKTAG_AFFINITY, (IPTR)aff,
                         TASKTAG_PRI, CANARY_PRIORITY,
                         TASKTAG_STACKSIZE, CANARY_STACK,
                         TASKTAG_PC, (IPTR)canary_task,
                         TASKTAG_ARG1, (IPTR)SysBase,
                         TAG_DONE) != NULL;
}

extern const struct Resident krnP4WdtCanaryResident;

AROS_UFH3(static APTR, canary_init,
          AROS_UFPA(void *, dummy, D0),
          AROS_UFPA(BPTR, segList, A0),
          AROS_UFPA(struct ExecBase *, SysBase, A6))
{
    AROS_USERFUNC_INIT

    unsigned int hart;
    ULONG online;

    if (!krnWdtArmed())
        return NULL;

    KernelBase = OpenResource("kernel.resource");
    if (!KernelBase)
        return NULL;

    online = __atomic_load_n(&__p4_harts_online, __ATOMIC_ACQUIRE);
    for (hart = 0; hart < 2; hart++)
    {
        if (!(online & (1UL << hart)))
            continue;
        if (!start_canary(SysBase, hart))
        {
            krnP4PutStr("[wdt] canary of hart ");
            krnP4PutDec(hart);
            krnP4PutStr(" not created\n");
        }
    }
    return NULL;

    AROS_USERFUNC_EXIT
}

/* After dos.library has booted, ahead of the test residents at -125 and -126 */
const struct Resident krnP4WdtCanaryResident =
{
    RTC_MATCHWORD,
    (struct Resident *)&krnP4WdtCanaryResident,
    (APTR)((const char *)&krnP4WdtCanaryResident + sizeof(struct Resident)),
    RTF_AFTERDOS,
    1,
    NT_TASK,
    -124,
    "esp32p4 watchdog canary",
    "esp32p4 watchdog canary 1.0",
    &canary_init
};

#endif /* !P4_NO_WATCHDOG */
