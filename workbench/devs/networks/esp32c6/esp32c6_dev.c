/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: AROS SANA-II Network Device Implementation for ESP32-C6.
*/

#include <exec/types.h>
#include <exec/libraries.h>
#include <exec/errors.h>
#include <exec/memory.h>
#include <devices/sana2.h>
#include <proto/exec.h>
#include <aros/debug.h>

#include "esp32c6.h"

LIBFUNC_P2(struct esp32c6_base *, init_device, D0, struct esp32c6_base *, base, a6, struct ExecBase *, sys_base)
{
    base->sys_base = sys_base;

    for (int i = 0; i < ESP32C6_MAX_UNITS; i++) {
        struct esp32c6_unit *u = &base->units[i];
        u->unit_num = i;
        u->online = FALSE;
        NEWLIST((struct List *)&u->read_queue);
        NEWLIST((struct List *)&u->write_queue);
        InitSemaphore(&u->lock);

        if (i == ESP32C6_UNIT_WIFI) {
            u->mtu = 1500;
            u->wire_type = S2WireType_Ethernet;
            u->mac_addr[0] = 0x24; u->mac_addr[1] = 0xEC;
            u->mac_addr[2] = 0x4A; u->mac_addr[3] = 0x01;
            u->mac_addr[4] = 0x10; u->mac_addr[5] = 0x01;
        } else if (i == ESP32C6_UNIT_THREAD) {
            u->mtu = 1280; /* IPv6 standard minimum MTU over 6LoWPAN */
            u->wire_type = S2WireType_IEEE8023; /* Mapped for bsdsocket IPv6 */
            u->mac_addr[0] = 0x24; u->mac_addr[1] = 0xEC;
            u->mac_addr[2] = 0x4A; u->mac_addr[3] = 0x01;
            u->mac_addr[4] = 0x10; u->mac_addr[5] = 0x01;
            u->mac_addr[6] = 0x00; u->mac_addr[7] = 0x01;
        } else {
            u->mtu = 127; /* Raw IEEE 802.15.4 / Zigbee PSDU */
            u->wire_type = S2WireType_Ethernet;
        }
    }

    return base;
}
LIBFUNC_END

LIBFUNC_P3(struct esp32c6_base *, open_device, D0, ULONG, unit_num, A1, struct IOSana2Req *, ios2, a6, struct esp32c6_base *, base)
{
    if (unit_num >= ESP32C6_MAX_UNITS) {
        ios2->ios2_Req.io_Error = IOERR_OPENFAIL;
        return NULL;
    }

    struct esp32c6_unit *u = &base->units[unit_num];
    ios2->ios2_Req.io_Unit = (struct Unit *)u;
    ios2->ios2_Req.io_Error = 0;
    base->lib.lib_OpenCnt++;

    return base;
}
LIBFUNC_END

LIBFUNC_P2(BPTR, close_device, A1, struct IOSana2Req *, ios2, a6, struct esp32c6_base *, base)
{
    ios2->ios2_Req.io_Unit = NULL;
    base->lib.lib_OpenCnt--;
    return 0;
}
LIBFUNC_END

LIBFUNC_P2(BPTR, expunge_device, D0, struct esp32c6_base *, base, a6, struct ExecBase *, sys_base)
{
    if (base->lib.lib_OpenCnt > 0) {
        base->lib.lib_Flags |= LIBF_DELEXP;
        return 0;
    }

    BPTR seg = base->seg_list;
    Remove(&base->lib.lib_Node);
    FreeMem((APTR)((IPTR)base - base->lib.lib_NegSize),
            base->lib.lib_NegSize + base->lib.lib_PosSize);
    return seg;
}
LIBFUNC_END

void begin_io(struct esp32c6_base *base, struct IOSana2Req *ios2)
{
    struct esp32c6_unit *u = (struct esp32c6_unit *)ios2->ios2_Req.io_Unit;
    if (!u) {
        ios2->ios2_Req.io_Error = IOERR_OPENFAIL;
        TermIO((struct IORequest *)ios2);
        return;
    }

    ios2->ios2_Req.io_Error = 0;
    ios2->ios2_Req.io_Message.mn_Node.ln_Type = NT_MESSAGE;

    process_sana2_cmd(u, ios2);
}

void abort_io(struct esp32c6_base *base, struct IOSana2Req *ios2)
{
    struct esp32c6_unit *u = (struct esp32c6_unit *)ios2->ios2_Req.io_Unit;
    if (!u) return;

    ObtainSemaphore(&u->lock);
    Remove(&ios2->ios2_Req.io_Message.mn_Node);
    ReleaseSemaphore(&u->lock);

    ios2->ios2_Req.io_Error = IOERR_ABORTED;
    TermIO((struct IORequest *)ios2);
}
