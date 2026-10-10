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
