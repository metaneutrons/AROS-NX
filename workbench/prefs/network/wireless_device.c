/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */

#include <exec/io.h>
#include <devices/newstyle.h>
#include <devices/sana2.h>
#include <devices/sana2wireless.h>
#include <devices/timer.h>
#include <utility/tagitem.h>
#include <proto/exec.h>
#include <stddef.h>
#include <string.h>

#include "wireless_device.h"

#define NETPREFS_COMMAND_LIMIT 256

/* Device-owned command storage is trusted to be readable while open, as in
 * NSD itself. A cap limits malformed lists; it cannot validate arbitrary
 * pointers returned by a broken driver in this shared address space. */
static enum NetPrefsWirelessKind query_kind(
    const struct NSDeviceQueryResult *query, ULONG actual)
{
    ULONG required = offsetof(struct NSDeviceQueryResult, SupportedCommands)
        + sizeof(query->SupportedCommands);
    ULONG i;
    BOOL wireless = FALSE;

    if (actual < required || actual > sizeof(*query) ||
        query->SizeAvailable < required || query->SizeAvailable > actual ||
        query->DevQueryFormat != 0 || query->DeviceType != NSDEVTYPE_SANA2 ||
        query->SupportedCommands == NULL)
        return NETPREFS_WIRELESS_UNKNOWN;

    for (i = 0; i < NETPREFS_COMMAND_LIMIT; ++i)
    {
        UWORD command = query->SupportedCommands[i];
        if (command == 0)
            return wireless ? NETPREFS_WIRELESS_YES : NETPREFS_WIRELESS_NO;
        if (command == S2_GETNETWORKS)
            wireless = TRUE;
    }
    return NETPREFS_WIRELESS_UNKNOWN;
}

/* Required SANA-II opener hooks. This query never transfers frames; refuse
 * any unexpected invocation rather than copying unvalidated arguments. */
static BOOL query_copy(APTR target, APTR source, ULONG length)
{
    (void)target;
    (void)source;
    (void)length;
    return FALSE;
}

enum NetPrefsWirelessKind NetPrefs_QueryWireless(CONST_STRPTR device, LONG unit)
{
    struct MsgPort *port = NULL, *timer_port = NULL;
    struct IOSana2Req *request = NULL;
    struct timerequest *timer = NULL;
    struct NSDeviceQueryResult query;
    struct TagItem tags[] = {
        { S2_CopyToBuff, (IPTR)query_copy },
        { S2_CopyFromBuff, (IPTR)query_copy },
        { TAG_DONE, 0 }
    };
    BOOL opened = FALSE, timer_opened = FALSE;
    enum NetPrefsWirelessKind result = NETPREFS_WIRELESS_UNKNOWN;

    if (device == NULL || *device == '\0' || unit < 0)
        return result;

    port = CreateMsgPort();
    timer_port = CreateMsgPort();
    if (port == NULL || timer_port == NULL)
        goto out;
    request = (APTR)CreateIORequest(port, sizeof(*request));
    timer = (APTR)CreateIORequest(timer_port, sizeof(*timer));
    if (request == NULL || timer == NULL)
        goto out;
    if (OpenDevice((CONST_STRPTR)TIMERNAME, UNIT_MICROHZ, (APTR)timer, 0) != 0)
        goto out;
    timer_opened = TRUE;
    request->ios2_BufferManagement = tags;
    if (OpenDevice(device, unit, (APTR)request, 0) != 0)
        goto out;
    opened = TRUE;

    memset(&query, 0, sizeof(query));
    ((struct IOStdReq *)request)->io_Command = NSCMD_DEVICEQUERY;
    ((struct IOStdReq *)request)->io_Data = &query;
    ((struct IOStdReq *)request)->io_Length = sizeof(query);
    timer->tr_node.io_Command = TR_ADDREQUEST;
    timer->tr_time.tv_secs = 2;
    timer->tr_time.tv_micro = 0;
    SendIO((APTR)timer);
    SendIO((APTR)request);
    while (!CheckIO((APTR)request) && !CheckIO((APTR)timer))
        Wait((1UL << port->mp_SigBit) | (1UL << timer_port->mp_SigBit));

    if (!CheckIO((APTR)request))
        AbortIO((APTR)request);
    else if (request->ios2_Req.io_Error == 0)
        result = query_kind(&query, ((struct IOStdReq *)request)->io_Actual);
    /* AbortIO is not completion. Keep all borrowed storage until WaitIO,
     * including on timeout; a non-conforming driver cannot be freed safely. */
    WaitIO((APTR)request);
    AbortIO((APTR)timer);
    WaitIO((APTR)timer);

out:
    if (opened) CloseDevice((APTR)request);
    if (timer_opened) CloseDevice((APTR)timer);
    if (request) DeleteIORequest((APTR)request);
    if (timer) DeleteIORequest((APTR)timer);
    if (port) DeleteMsgPort(port);
    if (timer_port) DeleteMsgPort(timer_port);
    return result;
}
