/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#ifndef P4_WIFI_DEVICE_BRIDGE_H
#define P4_WIFI_DEVICE_BRIDGE_H

#include <exec/types.h>
#include "hosted_scan.h"

#define P4_WIFI_DEVICE_BRIDGE_ABI 2u
#define P4_WIFI_DEVICE_BRIDGE_NAME "esp32p4wifi.resource"

struct P4WifiBase;
/* Owned default-config scan result. At most eight APs are retained locally;
 * total_count is the peer's earlier count, not an atomic snapshot guarantee.
 * Security fields are summaries, not captured Beacon/RSN/WPA IEs. */
struct p4_wifi_scan_result {
    struct p4_hosted_scan_records aps;
    ULONG total_count;
    BOOL truncated;
    BOOL empty_records_workaround;
};
/* Private port contract, not a public SANA-II ABI. The client and callback
 * table remain live until detach returns. service/frame/stopped run in owner task context,
 * outside the SDMMC semaphore, serialized by the client's registration lock.
 * Never attach/detach from a callback. Device close must not hold its own
 * semaphore while calling detach. No callback may retain frame pointers.
 * service may call send_frame or scan; frame/ready/stopped may not access
 * transport. scan suppresses transport re-entry for its full duration.
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
 * scan is also owner-service-only, bounded by one aggregate eight-second
 * deadline. It runs without a client unit lock, returns owned results only on
 * success, and leaves output unchanged on failure. The output must not alias
 * resource/transport state. No scan filters or credentials are supplied.
 * Link/association API will be added with typed event and privacy gates;
 * neither ready nor successful transmission means CONNECT. */

#endif
