/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#ifndef P4_WIFI_RUNTIME_PROBE_H
#define P4_WIFI_RUNTIME_PROBE_H
#include <exec/libraries.h>
#include <exec/semaphores.h>
#include "device_bridge.h"
struct P4WifiBase
{
    struct Library library;
    struct ExecBase *sysbase;
    ULONG bridge_abi, bridge_size;
    BOOL (*attach)(struct P4WifiBase *, APTR,
                   const struct p4_wifi_device_callbacks *);
    void (*detach)(struct P4WifiBase *, APTR);
    BOOL (*send_frame)(struct P4WifiBase *, const UBYTE *, ULONG);
    BOOL (*scan)(struct P4WifiBase *, struct p4_wifi_scan_result *);
    BOOL (*associate)(struct P4WifiBase *,
        const struct p4_wifi_association_target *,
        const struct p4_wifi_association_cancel *);
    BOOL association_enabled;
    /* Protected by client_lock, independent of administrative S2_ONLINE.
     * ABI5 associate leaves this FALSE even on TRUE return. Only its owner
     * service caller may commit it after the unit's selection/lifecycle fence;
     * native RX/TX must also require the unit's committed link. */
    BOOL associated;
    struct p4_wifi_link_info confirmed_ap;
    /* Registration and callbacks own this lock. No SDMMC mutable field is
     * protected by it: only the actual transport owner can touch runtime. */
    struct SignalSemaphore client_lock;
    APTR client;
    const struct p4_wifi_device_callbacks *callbacks;
    BOOL ready;
    UBYTE mac[6];
    struct Task *owner_task;
    APTR runtime;
};
#endif
