/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: SANA-II Command Processing for ESP32-C6 (Wi-Fi 6, 6LoWPAN, Zigbee).
*/

#include <exec/types.h>
#include <exec/errors.h>
#include <devices/sana2.h>
#include <devices/sana2specialstats.h>
#include <proto/exec.h>

#include "esp32c6.h"

void process_sana2_cmd(struct esp32c6_unit *u, struct IOSana2Req *ios2)
{
    UWORD cmd = ios2->ios2_Req.io_Command;
    BOOL complete_now = TRUE;

    switch (cmd) {
    case S2_GETSTATIONADDRESS:
        for (int i = 0; i < 6; i++) {
            ios2->ios2_SrcAddr[i] = u->mac_addr[i];
            ios2->ios2_DstAddr[i] = u->mac_addr[i];
        }
        break;

    case S2_CONFIGINTERFACE:
        for (int i = 0; i < 6; i++) {
            u->mac_addr[i] = ios2->ios2_SrcAddr[i];
        }
        break;

    case S2_ONLINE:
        ObtainSemaphore(&u->lock);
        u->online = TRUE;
        ReleaseSemaphore(&u->lock);
        break;

    case S2_OFFLINE:
        ObtainSemaphore(&u->lock);
        u->online = FALSE;
        ReleaseSemaphore(&u->lock);
        break;

    case CMD_READ:
        ObtainSemaphore(&u->lock);
        if (!u->online) {
            ios2->ios2_Req.io_Error = S2ERR_OUTOFSERVICE;
        } else {
            AddTail((struct List *)&u->read_queue, &ios2->ios2_Req.io_Message.mn_Node);
            complete_now = FALSE;
        }
        ReleaseSemaphore(&u->lock);
        break;

    case CMD_WRITE:
    case S2_BROADCAST:
        ObtainSemaphore(&u->lock);
        if (!u->online) {
            ios2->ios2_Req.io_Error = S2ERR_OUTOFSERVICE;
        } else {
            /* Transmit frame */
            ios2->ios2_Req.io_Error = 0;
        }
        ReleaseSemaphore(&u->lock);
        break;

    case S2_ONEVENT:
        complete_now = FALSE;
        break;

    case S2_GETSPECIALSTATS:
        {
            struct Sana2SpecialStatHeader *stats = (struct Sana2SpecialStatHeader *)ios2->ios2_StatData;
            if (stats) {
                stats->RecordCount = 0;
            }
        }
        break;

    default:
        ios2->ios2_Req.io_Error = IOERR_NOCMD;
        break;
    }

    if (complete_now) {
        if (!(ios2->ios2_Req.io_Flags & SANA2IOF_QUICK)) {
            ReplyMsg(&ios2->ios2_Req.io_Message);
        }
    }
}
