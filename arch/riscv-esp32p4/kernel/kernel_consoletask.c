/* Copyright (c) 2026, The AROS Development Team. All rights reserved.
 * Scheduler-backed sender for the ESP32-P4 debug console. No DOS dependency.
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
#include "kernel_intern.h"

#if !defined(P4_CONSOLE_WAIT)
static void console_task(struct ExecBase *SysBase)
{
    struct MsgPort *port = CreateMsgPort();
    struct timerequest *tr = port ?
        (struct timerequest *)CreateIORequest(port, sizeof(*tr)) : NULL;

    if (!tr || OpenDevice("timer.device", UNIT_VBLANK,
                          (struct IORequest *)tr, 0))
    {
        if (tr)
            DeleteIORequest((struct IORequest *)tr);
        if (port)
            DeleteMsgPort(port);
        krnP4PutStr("[console] sender unavailable; bounded direct output\n");
        return;
    }
    krnP4PutStr("[console] queued sender ready; records=64 bytes=128 period=20ms\n");
    krnP4ConsoleSenderReady();
    for (;;)
    {
        (void)krnP4ConsoleDrain(256);
        tr->tr_node.io_Command = TR_ADDREQUEST;
        tr->tr_time.tv_secs = 0;
        tr->tr_time.tv_micro = 20000;
        if (DoIO((struct IORequest *)tr))
        {
            krnP4ConsoleSenderFailed();
            CloseDevice((struct IORequest *)tr);
            DeleteIORequest((struct IORequest *)tr);
            DeleteMsgPort(port);
            krnP4PutStr("[console] sender timer failed; undelivered backlog lost; bounded direct output\n");
            return;
        }
    }
}

AROS_UFH3(static APTR, console_init,
          AROS_UFPA(void *, dummy, D0),
          AROS_UFPA(BPTR, segList, A0),
          AROS_UFPA(struct ExecBase *, SysBase, A6))
{
    AROS_USERFUNC_INIT
    if (!NewCreateTask(TASKTAG_NAME, (IPTR)"P4 console TX",
                       TASKTAG_PRI, 0,
                       TASKTAG_STACKSIZE, 8192,
                       TASKTAG_PC, (IPTR)console_task,
                       TASKTAG_ARG1, (IPTR)SysBase, TAG_DONE))
        krnP4PutStr("[console] sender not created; bounded direct output\n");
    return NULL;
    AROS_USERFUNC_EXIT
}

extern const struct Resident krnP4ConsoleResident;
/* timer.device is initialized at priority 50. Run before DOS starts. */
const struct Resident krnP4ConsoleResident =
{
    RTC_MATCHWORD,
    (struct Resident *)&krnP4ConsoleResident,
    (APTR)((const char *)&krnP4ConsoleResident + sizeof(struct Resident)),
    RTF_COLDSTART, 1, NT_TASK, 40,
    "esp32p4 console sender", "esp32p4 console sender 1.0", &console_init
};
#endif
