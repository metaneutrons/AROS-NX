/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#ifndef P4_WIFI_DEVICE_BRIDGE_H
#define P4_WIFI_DEVICE_BRIDGE_H

#include <exec/types.h>

#define P4_WIFI_DEVICE_BRIDGE_ABI 1u
#define P4_WIFI_DEVICE_BRIDGE_NAME "esp32p4wifi.resource"

struct P4WifiBase;
/* Private port contract, not a public SANA-II ABI. The client and callback
 * table remain live until detach returns. service/frame/stopped run in owner task context,
 * outside the SDMMC semaphore, serialized by the client's registration lock.
 * Never attach/detach from a callback. Device close must not hold its own
 * semaphore while calling detach. No callback may retain frame pointers.
 * service may call send_frame; frame/ready/stopped may not access transport.
 * No callback may wait for another client callback or invoke user code which
 * closes the device. Permanent noexpunge modules protect callback code. The
 * device registers its permanent unit once and keeps it across last Close;
 * detach is not a CloseDevice hook and may wait outside Forbid. ready can also
 * run synchronously in attach's caller: metadata only, nonblocking. */
struct p4_wifi_device_callbacks {
    void (*service)(APTR client, struct P4WifiBase *radio);
    /* TRUE means consumed or deliberately dropped (no reader/filter/full).
     * FALSE is a fatal client invariant failure and retires the session;
     * normal packet loss must update device statistics and return TRUE. */
    BOOL (*frame)(APTR client, const UBYTE *frame, ULONG length);
    void (*ready)(APTR client, const UBYTE *mac);
    void (*stopped)(APTR client);
};

/* Attachment does not start/reset a radio or establish association. It is
 * refused until the opt-in owner has initialized and published its MAC.
 * send_frame is owner-task-only and returns after bounded transmission, not
 * after IP delivery. Request ownership and cancellation stay in the device.
 * Link/association API will be added with typed event and privacy gates;
 * neither ready nor successful transmission means CONNECT. */

#endif
