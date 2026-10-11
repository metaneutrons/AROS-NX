/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#ifndef P4_WIFI_RUNTIME_PROBE_H
#define P4_WIFI_RUNTIME_PROBE_H
#include <exec/libraries.h>
#include <exec/semaphores.h>
#include "device_bridge.h"
#include "owner_ota_queue.h"
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
    BOOL (*disconnect)(struct P4WifiBase *);
    BOOL association_enabled;
    /* Protected by client_lock, independent of administrative S2_ONLINE.
     * ABI5 associate leaves this FALSE even on TRUE return. Only its owner
     * service caller may commit it after the unit's selection/lifecycle fence;
     * native RX/TX must also require the unit's committed link. */
    BOOL associated;
    struct p4_wifi_link_info confirmed_ap;
    ULONG link_state;
    struct p4_wifi_link_info observed_ap;
    /* Registration and callbacks own this lock. No SDMMC mutable field is
     * protected by it: only the actual transport owner can perform runtime
     * transport. Maintenance callers inspect metadata and queue/copy discovery
     * under this lock, never invoking SDIO. */
    struct SignalSemaphore client_lock;
    APTR client;
    const struct p4_wifi_device_callbacks *callbacks;
    BOOL ready;
    UBYTE mac[6];
    struct Task *owner_task;
    APTR runtime;
    ULONG (*maintenance_request)(struct P4WifiBase *);
    ULONG (*maintenance_poll)(struct P4WifiBase *, ULONG);
    BOOL (*maintenance_release)(struct P4WifiBase *, ULONG);
    BOOL (*maintenance_discover)(struct P4WifiBase *, ULONG);
    ULONG (*maintenance_discovery_poll)(struct P4WifiBase *, ULONG,
                                        struct p4_wifi_discovery_result *);
    struct Task *maintenance_task;
    ULONG maintenance_token, maintenance_state, maintenance_started;
    BOOL (*maintenance_fence)(struct P4WifiBase *, ULONG, ULONG);
    /* Sticky after accepted terminal handoff; never clear on expiry/retirement. */
    ULONG maintenance_fence_mode;
    /* ABI10 optional dispatcher hooks. Check exact bridge_abi and bridge_size
     * before reading these fields. NULL is unavailable, never permission to
     * fall back to raw owner symbols. They retain task-bound copied jobs;
     * poll/cancel/discard never acquire client_lock or perform transport. */
    uint32_t (*ota_submit)(struct P4WifiBase *, const struct p4_wifi_ota_request *);
    enum p4_wifi_ota_job_state (*ota_poll)(struct P4WifiBase *, uint32_t,
                                        struct p4_wifi_ota_observation *);
    int (*ota_cancel)(struct P4WifiBase *, uint32_t);
    int (*ota_discard)(struct P4WifiBase *, uint32_t);
};
#endif
